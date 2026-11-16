/* Copyright 2026 Unified contributors
 * SPDX-License-Identifier: MIT
 * Portable Mesa libpoly lowering, isolated from kbase/WSI. */
#include "mali_poly.h"
#include "nir_builder.h"
#include "gallium/include/pipe/p_defines.h"
#include "poly/cl/libpoly.h"
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
   case nir_intrinsic_load_ro_sink_address_poly: off = offsetof(struct mali_poly_params, ro_sink); break;
   case nir_intrinsic_load_first_vertex: off = offsetof(struct mali_poly_params, first_vertex); break;
   case nir_intrinsic_load_base_instance: off = offsetof(struct mali_poly_params, base_instance); break;
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
