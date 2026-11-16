/* Copyright 2026 Unified contributors
 * SPDX-License-Identifier: MIT
 * Portable Mesa libpoly lowering, isolated from kbase/WSI. */
#include "mali_poly.h"
#include "nir_builder.h"
#include "gallium/include/pipe/p_defines.h"
#include "poly/cl/libpoly.h"
#include "poly/tessellator.h"
#include "panfrost/compiler/pan_nir.h"
#include <stddef.h>
#include <string.h>
#include "util/set.h"

static unsigned
type_size(const struct glsl_type *t, bool bindless)
{
   return glsl_count_attribute_slots(t, false);
}

static void
prepare_io(nir_shader *nir)
{
   NIR_PASS(_, nir, nir_lower_system_values);
   /* Compact built-ins (tess factors, clip/cull arrays) index scalar
    * components, not whole locations. nir_lower_io requires their dynamic
    * dereferences to have been lowered first. Keep per-vertex arrays direct. */
   struct set *compact = _mesa_pointer_set_create(NULL);
   nir_foreach_variable_with_modes(v, nir, nir_var_shader_in | nir_var_shader_out)
      if (v->data.compact) _mesa_set_add(compact, v);
   if (compact->entries)
      NIR_PASS(_, nir, nir_lower_indirect_var_derefs_to_if_else_trees, compact);
   _mesa_set_destroy(compact, NULL);
   NIR_PASS(_, nir, nir_lower_io_vars_to_temporaries,
            nir_shader_get_entrypoint(nir), nir_var_shader_out);
   NIR_PASS(_, nir, nir_lower_global_vars_to_local);
   NIR_PASS(_, nir, nir_split_var_copies);
   NIR_PASS(_, nir, nir_lower_var_copies);
   NIR_PASS(_, nir, nir_lower_vars_to_ssa);
   NIR_PASS(_, nir, nir_lower_io, nir_var_shader_in | nir_var_shader_out, type_size, 0);
   NIR_PASS(_, nir, nir_opt_constant_folding);
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
}

static void
as_compute(nir_shader *nir, unsigned size)
{
   nir->info.stage = MESA_SHADER_COMPUTE;
   memset(&nir->info.cs, 0, sizeof(nir->info.cs));
   nir->info.workgroup_size[0] = size;
   nir->info.workgroup_size[1] = nir->info.workgroup_size[2] = 1;
   nir->info.workgroup_size_variable = false;
}

static bool
generated_output_slots(nir_builder *b, nir_intrinsic_instr *i, void *unused)
{
   if (i->intrinsic != nir_intrinsic_store_output) return false;
   nir_io_semantics sem = nir_intrinsic_io_semantics(i);
   if (sem.num_slots) return false;
   sem.num_slots = 1;
   nir_intrinsic_set_io_semantics(i, sem);
   return true;
}

bool
mali_poly_lower_geometry(nir_shader *gs, struct mali_poly_geometry *out)
{
   memset(out, 0, sizeof(*out));
   if (!gs || gs->info.stage != MESA_SHADER_GEOMETRY) return false;
   prepare_io(gs);
   out->main = gs;
   poly_nir_lower_gs(gs, &out->count, &out->raster, &out->pre, &out->info);
   /* libpoly emits location-based stores with default base=0. Mali's
    * varying layout is indexed by base, so position and generic outputs
    * must have distinct driver slots before collecting that layout. */
   if (out->raster) {
      nir_shader_intrinsics_pass(out->raster, generated_output_slots, nir_metadata_all, NULL);
      NIR_PASS(_, out->raster, nir_recompute_io_bases, nir_var_shader_out);
   }
   as_compute(out->main, 1);
   if (out->count) as_compute(out->count, 1);
   if (out->pre) as_compute(out->pre, 1);
   return out->raster != NULL;
}

void
mali_poly_geometry_finish(struct mali_poly_geometry *p)
{
   ralloc_free(p->main);ralloc_free(p->count);
   ralloc_free(p->pre);ralloc_free(p->raster);
   memset(p, 0, sizeof(*p));
}

/* libpoly may narrow a pure shader-output barrier to a subgroup. A
 * Vulkan patch can span multiple Mali subgroups: map its output memory
 * to global *before* that lowering, preserving workgroup execution. */
static bool
preserve_tcs_barrier(nir_builder *b, nir_intrinsic_instr *i, void *data)
{
   if (i->intrinsic != nir_intrinsic_barrier) return false;
   nir_variable_mode modes = nir_intrinsic_memory_modes(i);
   if (!(modes & nir_var_shader_out)) return false;
   nir_intrinsic_set_memory_modes(i, (modes & ~nir_var_shader_out) | nir_var_mem_global);
   return true;
}

bool
mali_poly_lower_tcs(nir_shader *nir)
{
   if (!nir || nir->info.stage != MESA_SHADER_TESS_CTRL) return false;
   unsigned size = nir->info.tess.tcs_vertices_out;
   if (!size || size > 32) return false;
   prepare_io(nir);
   nir_shader_intrinsics_pass(nir, preserve_tcs_barrier, nir_metadata_control_flow, NULL);
   poly_nir_lower_tcs(nir, false);
   as_compute(nir, size);
   return true;
}

bool
mali_poly_lower_tes_raster(nir_shader *nir)
{
   if (!nir || nir->info.stage != MESA_SHADER_TESS_EVAL) return false;
   prepare_io(nir);
   poly_nir_lower_tes(nir, true);
   nir_shader_intrinsics_pass(nir, generated_output_slots, nir_metadata_all, NULL);
   NIR_PASS(_, nir, nir_recompute_io_bases, nir_var_shader_out);
   return true;
}

bool
mali_poly_lower_tes_compute(nir_shader *nir, uint64_t *outputs)
{
   if (!nir || nir->info.stage != MESA_SHADER_TESS_EVAL)
      return false;
   prepare_io(nir);
   nir->info.next_stage = MESA_SHADER_GEOMETRY;
   poly_nir_lower_tes(nir, false);
   *outputs = nir->info.outputs_written;
   poly_nir_lower_vs_before_gs(nir);
   as_compute(nir, 1);
   return true;
}

static bool
software_vertex_id(nir_builder *b, nir_intrinsic_instr *i, void *unused)
{
   if (i->intrinsic != nir_intrinsic_load_raw_vertex_id &&
       i->intrinsic != nir_intrinsic_load_vertex_id &&
       i->intrinsic != nir_intrinsic_load_vertex_id_zero_base)
      return false;
   b->cursor = nir_before_instr(&i->instr);
   nir_def *id = nir_channel(b, nir_load_global_invocation_id(b, 32), 0);
   nir_def_replace(&i->def, poly_nir_load_vertex_id(b, id));
   return true;
}

bool
mali_poly_lower_vertex(nir_shader *nir)
{
   if (!nir || nir->info.stage != MESA_SHADER_VERTEX) return false;
   nir_foreach_shader_in_variable(v, nir)
      v->data.driver_location = v->data.location - VERT_ATTRIB_GENERIC0;
   prepare_io(nir);
   pan_nir_lower_vs_inputs(nir);
   /* Attribute pulls use the original indexed/base vertex ID. Stores
    * inserted below use the draw-order ID, without applying indexing twice. */
   nir_shader_intrinsics_pass(nir, software_vertex_id, nir_metadata_control_flow, NULL);
   poly_nir_lower_vs_before_gs(nir);
   poly_nir_lower_sw_vs(nir);
   as_compute(nir, 1);
   return true;
}

static bool
lower_param(nir_builder *b, nir_intrinsic_instr *i, void *data)
{
   b->cursor = nir_before_instr(&i->instr);
   unsigned base = *(const unsigned *)data, off, bits = i->def.bit_size;
   switch (i->intrinsic) {
   case nir_intrinsic_load_push_constant:
      if (nir_intrinsic_base(i) != MALI_POLY_PUSH_CONST_BASE ||
          !nir_src_is_const(i->src[0])) return false;
      off = nir_src_as_uint(i->src[0]);
      assert(off + i->def.num_components * bits / 8 <= sizeof(struct mali_poly_params));
      break;
   case nir_intrinsic_load_vertex_param_buffer_poly: off = offsetof(struct mali_poly_params, vertex); break;
   case nir_intrinsic_load_geometry_param_buffer_poly: off = offsetof(struct mali_poly_params, geometry); break;
   case nir_intrinsic_load_tess_param_buffer_poly: off = offsetof(struct mali_poly_params, tess); break;
   case nir_intrinsic_load_ro_sink_address_poly: off = offsetof(struct mali_poly_params, ro_sink); break;
   case nir_intrinsic_load_first_vertex: off = offsetof(struct mali_poly_params, first_vertex); break;
   case nir_intrinsic_load_base_instance: off = offsetof(struct mali_poly_params, base_instance); break;
   case nir_intrinsic_load_draw_id: off = offsetof(struct mali_poly_params, draw_id); break;
   case nir_intrinsic_load_provoking_last: off = offsetof(struct mali_poly_params, provoking_last); break;
   case nir_intrinsic_load_printf_buffer_address: off = offsetof(struct mali_poly_params, printf_buffer); break;
   case nir_intrinsic_load_printf_buffer_size:
      nir_def_replace(&i->def, nir_imm_int(b, 1024));return true;
   case nir_intrinsic_load_stat_query_address_poly:
      switch (nir_intrinsic_base(i)) {
      case PIPE_STAT_QUERY_GS_INVOCATIONS: off = 0; break;
      case PIPE_STAT_QUERY_GS_PRIMITIVES: off = 1; break;
      case PIPE_STAT_QUERY_C_PRIMITIVES: off = 2; break;
      case PIPE_STAT_QUERY_C_INVOCATIONS: off = 3; break;
      default: return false;
      }
      off = offsetof(struct mali_poly_params, stat_query) + off * 8;
      break;
   case nir_intrinsic_ro_to_rw_poly:
      nir_def_replace(&i->def, i->src[0].ssa);
      return true;
   case nir_intrinsic_load_vertex_id_zero_base:
   case nir_intrinsic_load_vertex_id:
   case nir_intrinsic_load_raw_vertex_id:
      if (b->shader->info.stage != MESA_SHADER_COMPUTE) return false;
      b->cursor = nir_before_instr(&i->instr);
      nir_def_replace(&i->def, nir_channel(b, nir_load_global_invocation_id(b, 32), 0));
      return true;
   case nir_intrinsic_load_instance_id:
      if (b->shader->info.stage != MESA_SHADER_COMPUTE) return false;
      b->cursor = nir_before_instr(&i->instr);
      nir_def_replace(&i->def, nir_channel(b, nir_load_global_invocation_id(b, 32), 1));
      return true;
   default: return false;
   }
   b->cursor = nir_before_instr(&i->instr);
   nir_def *value;
   if (base & MALI_POLY_INDIRECT_ROOT) {
      nir_def *root = nir_load_push_constant(b, 1, 64, nir_imm_int(b, 0),
         .base = base & ~MALI_POLY_INDIRECT_ROOT, .range = 8);
      value = nir_load_global(b, i->def.num_components, bits,
         nir_iadd_imm(b, root, off), .align_mul = MIN2(bits / 8, 8));
   } else {
      value = nir_load_push_constant(b, i->def.num_components, bits,
         nir_imm_int(b, off), .base = base, .range = sizeof(struct mali_poly_params));
   }
   nir_def_replace(&i->def, value);
   return true;
}

bool
mali_poly_lower_params(nir_shader *nir, unsigned base)
{
   poly_nir_lower_sysvals(nir);
   bool progress = nir_shader_intrinsics_pass(nir, lower_param, nir_metadata_control_flow, &base);
   NIR_PASS(_, nir, nir_opt_dce);
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
   return progress;
}

static nir_def *
private_load(nir_builder *b, unsigned bits, unsigned offset)
{
   return nir_load_push_constant(b, 1, bits, nir_imm_int(b, offset),
      .base = MALI_POLY_PUSH_CONST_BASE, .range = sizeof(struct mali_poly_params));
}

static bool
lower_tess_scratch_base(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   if (intr->intrinsic != nir_intrinsic_load_scratch_base_ptr)
      return false;

   b->cursor = nir_before_instr(&intr->instr);
   const uint32_t stride = *(const uint32_t *)data;
   nir_def *scratch = private_load(b, 64, offsetof(struct mali_poly_params, tess_scratch));
   nir_def *patch = nir_channel(b, nir_load_global_invocation_id(b, 32), 0);
   nir_def *offset = nir_imul_imm(b, patch, stride);
   nir_def_replace(&intr->def, nir_iadd(b, scratch, nir_u2u64(b, offset)));
   return true;
}

static nir_def *
load_u32(nir_builder *b, nir_def *base, unsigned offset)
{
   return nir_load_global(b, 1, 32, nir_iadd_imm(b, base, offset), .align_mul = 4);
}

static void
store_u32(nir_builder *b, nir_def *base, unsigned offset, nir_def *value)
{
   nir_store_global(b, value, nir_iadd_imm(b, base, offset), .align_mul = 4);
}

static void
store_grid(nir_builder *b, nir_def *base, unsigned offset, nir_def *x, nir_def *y)
{
   store_u32(b, base, offset, x);
   store_u32(b, base, offset + 4, y);
   store_u32(b, base, offset + 8, nir_imm_int(b, 1));
}

nir_shader *
mali_poly_tess_indirect(const nir_shader_compiler_options *options)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, options,
                                                  "Unified indirect tessellation setup");
   as_compute(b.shader, 1);
   const unsigned root_grids = offsetof(struct mali_poly_params, grids);
   nir_def *root = private_load(&b, 64, offsetof(struct mali_poly_params, self));
   nir_def *vp = nir_load_vertex_param_buffer_poly(&b);
   nir_def *tp = nir_load_tess_param_buffer_poly(&b);
   nir_def *cmd = private_load(&b, 64, offsetof(struct mali_poly_params, indirect_params));
   nir_def *count = private_load(&b, 64, offsetof(struct mali_poly_params, indirect_count));
   nir_def *draw_id = private_load(&b, 32, offsetof(struct mali_poly_params, draw_id));
   nir_def *max_patches = private_load(&b, 32, offsetof(struct mali_poly_params, max_patches));
   nir_def *index_size = load_u32(&b, vp, offsetof(struct poly_vertex_params, index_size_B));
   nir_def *points = load_u32(&b, tp, offsetof(struct poly_tess_params, input_patch_size));
   nir_def *zero = nir_imm_int(&b, 0), *one = nir_imm_int(&b, 1);
   nir_def *enabled = nir_imm_true(&b);
   nir_push_if(&b, nir_ine_imm(&b, count, 0));
   nir_def *in_range = nir_ult(&b, draw_id, load_u32(&b, count, 0));
   nir_pop_if(&b, NULL);
   enabled = nir_if_phi(&b, in_range, enabled);

   nir_push_if(&b, enabled);
   nir_def *patches = nir_udiv(&b, load_u32(&b, cmd, 0), points);
   nir_def *instances = load_u32(&b, cmd, 4);
   nir_def *valid = nir_uge(&b, nir_udiv(&b, max_patches, nir_umax(&b, instances, one)), patches);
   patches = nir_bcsel(&b, valid, patches, zero);
   nir_def *first = load_u32(&b, cmd, 8), *base_vertex = load_u32(&b, cmd, 12);
   nir_push_if(&b, nir_ine_imm(&b, index_size, 0));
   nir_def *base_instance = load_u32(&b, cmd, 16);
   /* The original range and address live in the immutable command record,
    * even when a recorded command buffer executes again. */
   nir_def *bound = nir_load_global(&b, 1, 64,
      nir_iadd_imm(&b, root, offsetof(struct mali_poly_params, tess_index_base)), .align_mul = 8);
   nir_def *size = load_u32(&b, root, offsetof(struct mali_poly_params, tess_index_elements));
   nir_def *start = nir_umin(&b, first, size);
   nir_def *index_base = nir_iadd(&b, bound, nir_umul_2x32_64(&b, start, index_size));
   nir_store_global(&b, index_base,
      nir_iadd_imm(&b, vp, offsetof(struct poly_vertex_params, index_buffer)), .align_mul = 8);
   store_u32(&b, vp, offsetof(struct poly_vertex_params, index_buffer_range_el),
             nir_isub(&b, size, start));
   nir_pop_if(&b, NULL);
   nir_def *instance_offset = nir_if_phi(&b, base_instance, base_vertex);
   nir_def *vertex_offset = nir_bcsel(&b, nir_ine_imm(&b, index_size, 0), base_vertex, first);
   store_u32(&b, root, offsetof(struct mali_poly_params, first_vertex), vertex_offset);
   store_u32(&b, root, offsetof(struct mali_poly_params, base_instance), instance_offset);
   nir_def *attribs = private_load(&b, 64, offsetof(struct mali_poly_params, attribs));
   for (unsigned i = 0; i < ARRAY_SIZE(((struct mali_poly_params *)0)->attrib_stride); i++) {
      nir_def *stride = load_u32(&b, root, offsetof(struct mali_poly_params, attrib_stride) + 4 * i);
      nir_push_if(&b, nir_ine_imm(&b, stride, 0));
      nir_def *offset = load_u32(&b, root, offsetof(struct mali_poly_params, attrib_offset) + 4 * i);
      store_u32(&b, attribs, 32 * i + 8, nir_iadd(&b, offset, nir_imul(&b, stride, instance_offset)));
      nir_pop_if(&b, NULL);
   }
   nir_pop_if(&b, NULL);
   patches = nir_if_phi(&b, patches, zero);
   instances = nir_if_phi(&b, instances, zero);
   nir_def *total = nir_imul(&b, patches, instances);
   nir_def *verts = nir_imul(&b, patches, points);
   store_u32(&b, vp, offsetof(struct poly_vertex_params, verts_per_instance), verts);
   store_u32(&b, tp, offsetof(struct poly_tess_params, patches_per_instance), patches);
   store_u32(&b, tp, offsetof(struct poly_tess_params, nr_patches), total);
   store_u32(&b, root, offsetof(struct mali_poly_params, patch_count), total);
   store_grid(&b, root, root_grids, verts, instances);
   store_grid(&b, root, root_grids + 12, patches, instances);
   store_grid(&b, root, root_grids + 24, total, one);
   return b.shader;
}

nir_shader *
mali_poly_tess_gs_setup(const nir_shader_compiler_options *options,
                       const struct poly_gs_info *gs, unsigned arity)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, options,
                                                  "Unified tessellation GS setup");
   as_compute(b.shader, 1);
   nir_def *gp = nir_load_geometry_param_buffer_poly(&b);
   nir_def *vp = nir_load_vertex_param_buffer_poly(&b);
   nir_def *count = private_load(&b, 64, offsetof(struct mali_poly_params, draw_count));
   nir_def *verts = load_u32(&b, count, 0), *one = nir_imm_int(&b, 1);
   nir_def *prims = nir_udiv_imm(&b, verts, arity);
   store_grid(&b, gp, offsetof(struct poly_geometry_params, grid), prims, one);
   store_grid(&b, gp, offsetof(struct poly_geometry_params, grid) + 12, verts, one);
   store_u32(&b, vp, offsetof(struct poly_vertex_params, verts_per_instance), verts);
   store_u32(&b, gp, offsetof(struct poly_geometry_params, input_primitives), prims);
   store_u32(&b, gp, offsetof(struct poly_geometry_params, primitives_log2),
             nir_isub(&b, nir_imm_int(&b, 32), nir_uclz(&b, nir_isub(&b, nir_umax(&b, prims, one), one))));
   bool per_prim = gs->shape == POLY_GS_SHAPE_STATIC_INDEXED ||
                   gs->shape == POLY_GS_SHAPE_STATIC_PER_PRIM;
   nir_def *max_indices = nir_imm_int(&b, gs->max_indices);
   nir_def *out = per_prim ? max_indices : nir_imul(&b, max_indices, prims);
   store_u32(&b, gp, offsetof(struct poly_geometry_params, draw.index_count), out);
   store_u32(&b, gp, offsetof(struct poly_geometry_params, draw.instance_count), per_prim ? prims : one);
   return b.shader;
}

nir_shader *
mali_poly_tessellator(const nir_shader_compiler_options *options,
                      enum tess_primitive_mode domain, unsigned mode,
                      uint32_t *scratch_stride)
{
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, options,
                                                  "Unified reference tessellator");
   b.shader->info.workgroup_size[0] = b.shader->info.workgroup_size[1] =
      b.shader->info.workgroup_size[2] = 1;
   nir_def *p = nir_load_tess_param_buffer_poly(&b);
   nir_def *patch = nir_channel(&b, nir_load_global_invocation_id(&b, 32), 0);
   nir_def *m = nir_imm_int(&b, mode);
   nir_def *nr = nir_load_global(&b, 1, 32,
      nir_iadd_imm(&b, p, offsetof(struct poly_tess_params, nr_patches)), .align_mul = 4);
   if (mode == POLY_TESS_MODE_COUNT) {
      nir_def *original = nir_load_push_constant(&b, 1, 32,
         nir_imm_int(&b, offsetof(struct mali_poly_params, patch_count)),
         .base = MALI_POLY_PUSH_CONST_BASE, .range = sizeof(struct mali_poly_params));
      nr = nir_bcsel(&b, nir_ine_imm(&b, original, 0), original, nr);
   }
   nir_push_if(&b, nir_ult(&b, patch, nr));
   switch (domain) {
   case TESS_PRIMITIVE_TRIANGLES: mali_poly_tess_triangle(&b, p, patch, m);break;
   case TESS_PRIMITIVE_QUADS: mali_poly_tess_quad(&b, p, patch, m);break;
   case TESS_PRIMITIVE_ISOLINES: mali_poly_tess_isoline(&b, p, patch, m);break;
   default: ralloc_free(b.shader);return NULL;
   }
   nir_pop_if(&b, NULL);
   /* The reference kernels have large private arrays. Give each patch its
    * own storage so shader spills retain the backend's small TLS offsets. */
   if (b.shader->scratch_size) {
      uint32_t stride = ALIGN_POT(b.shader->scratch_size, 16);
      *scratch_stride = MAX2(*scratch_stride, stride);
      NIR_PASS(_, b.shader, pan_nir_lower_mem_to_global);
      nir_shader_intrinsics_pass(b.shader, lower_tess_scratch_base,
                                 nir_metadata_control_flow, &stride);
      b.shader->scratch_size = 0;
   }
   return b.shader;
}

nir_shader *
mali_poly_tess_prefix(const nir_shader_compiler_options *options)
{
   /* Serial, exact reference setup; replace with a subgroup-independent
    * parallel scan after the complete draw path has a correctness oracle. */
   nir_builder b = nir_builder_init_simple_shader(MESA_SHADER_COMPUTE, options,
                                                   "Unified tess prefix and bounded setup");
   b.shader->info.workgroup_size[0] = b.shader->info.workgroup_size[1] =
      b.shader->info.workgroup_size[2] = 1;
   nir_def *tp = nir_load_tess_param_buffer_poly(&b);
   nir_def *counts = nir_load_global(&b, 1, 64,
      nir_iadd_imm(&b, tp, offsetof(struct poly_tess_params, counts)), .align_mul = 8);
   nir_def *nr = private_load(&b, 32, offsetof(struct mali_poly_params, patch_count));
   nir_def *limit = private_load(&b, 32, offsetof(struct mali_poly_params, tess_index_limit));
   nir_variable *idx = nir_local_variable_create(b.impl, glsl_uint_type(), "patch");
   nir_variable *sum = nir_local_variable_create(b.impl, glsl_uint_type(), "sum");
   nir_variable *failed = nir_local_variable_create(b.impl, glsl_bool_type(), "failed");
   nir_store_var(&b, idx, nir_imm_int(&b, 0), 1);
   nir_store_var(&b, sum, nir_imm_int(&b, 0), 1);
   nir_store_var(&b, failed, nir_imm_false(&b), 1);
   nir_push_loop(&b);
   nir_def *i = nir_load_var(&b, idx), *s = nir_load_var(&b, sum);
   nir_push_if(&b, nir_uge(&b, i, nr));nir_jump(&b, nir_jump_break);nir_pop_if(&b, NULL);
   nir_def *a = nir_iadd(&b, counts, nir_u2u64(&b, nir_imul_imm(&b, i, 4)));
   nir_def *c = nir_load_global(&b, 1, 32, a, .align_mul = 4);
   nir_push_if(&b, nir_ult(&b, nir_isub(&b, limit, s), c));
   nir_store_var(&b, failed, nir_imm_true(&b), 1);nir_jump(&b, nir_jump_break);nir_pop_if(&b, NULL);
   nir_def *next = nir_iadd(&b, s, c);
   nir_store_global(&b, next, a, .align_mul = 4);
   nir_store_var(&b, sum, next, 1);nir_store_var(&b, idx, nir_iadd_imm(&b, i, 1), 1);
   nir_pop_loop(&b, NULL);
   nir_def *abort = nir_load_var(&b, failed);
   nir_def *total = nir_bcsel(&b, abort, nir_imm_int(&b, 0), nir_load_var(&b, sum));
   nir_def *draw_count = private_load(&b, 64, offsetof(struct mali_poly_params, draw_count));
   nir_store_global(&b, total, draw_count, .align_mul = 4);
   nir_store_global(&b, nir_bcsel(&b, abort, nir_imm_int(&b, 0), nr),
      nir_iadd_imm(&b, tp, offsetof(struct poly_tess_params, nr_patches)), .align_mul = 4);
   nir_def *heap = nir_load_global(&b, 1, 64,
      nir_iadd_imm(&b, tp, offsetof(struct poly_tess_params, heap)), .align_mul = 8);
   nir_store_global(&b, nir_imm_int(&b, 0),
      nir_iadd_imm(&b, heap, offsetof(struct poly_heap, bottom)), .align_mul = 4);
   nir_def *status = nir_load_printf_buffer_address(&b, 64);
   nir_store_global(&b, nir_b2i32(&b, abort), nir_iadd_imm(&b, status, 4), .align_mul = 4);
   return b.shader;
}
