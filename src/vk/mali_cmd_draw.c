/*
 * SPDX-FileCopyrightText: 2026 ARMSX2 and bmdhacks
 * SPDX-License-Identifier: MIT
 */

/*
 * Draws on the command-stream frontend: vkCmdDraw / vkCmdDrawIndexed as
 * RUN_IDVS on the vertex/tiler subqueue, full-screen draws (RUN_FULLSCREEN)
 * for vkCmdClearAttachments (mali_cmd_meta_gfx.c) and in-pass barriers. The
 * binds, the dirty bits and the descriptors a draw points at are shared
 * with the job-manager back half (mali_cmd_state.[ch]); this file turns
 * them into register moves.
 *
 * The encoding follows panvk v11 (csf/panvk_vX_cmd_draw.c: prepare_draw,
 * launch_draw, cmd_run_fullscreen, cmd_fb_barrier), "which source wins"
 * for command-stream encoding; the per-pipeline words are the ones our
 * pipeline compile baked in (mali_pipeline_state.c). Differences:
 *
 *  - RUN_IDVS sets bit 55, which v11.xml does not define, as every blob
 *    RUN_IDVS does (read as "Tiler Flags2 Enable", the RUN_FULLSCREEN
 *    field at the same bit). TILER_FLAGS2 is written as 0 before every
 *    draw either way.
 *  - The FAU blocks carry our sysval layout (mali_shader.h), built only
 *    when their inputs changed.
 *
 * Register use on the vertex/tiler subqueue is the v11 IDVS staging set;
 * state registers persist between draws and are rewritten from dirty
 * bits (MALI_GFX_DIRTY_*). What a draw emits before its RUN_IDVS is in
 * mali_cmd_draw.h, shared with the indirect draws
 * (mali_cmd_draw_indirect.c).
 */

#include "mali_cmd_state.h"
#include "mali_measure.h"

#include "vk_log.h"
#include "vk_util.h"

#include "mali_arch.h"
#include "mali_queue.h"
#include "poly/tessellator.h"

#if PAN_ARCH != 11
#error "mali_cmd_draw.c is written for arch v11"
#endif

#include "mali_cmd_draw.h"

/* Bit 55 of RUN_IDVS; the on-device smoke test clears it once to check
 * what it does. */
bool mali_idvs_bit55 = true;

static void
launch_raster(struct mali_cmd_buffer *cmd, struct cs_builder *b,
              struct idvs_batch *batch, uint32_t flags)
{
   const struct mali_render_state *r = &cmd->gfx.render;
   if (r->td_count > 1) {
      batch_flush(b, batch);
      struct cs_index ctx = cs_reg64(b, IDVS(TILER_CTX));
      for (uint32_t i = 0; i < r->td_count; i++) {
         if (i)
            cs_add_imm64(b, ctx, ctx, pan_size(TILER_CONTEXT));
         emit_run_idvs(b, flags);
      }
      cs_move64_to(b, ctx, r->tiler);
   } else {
      b->req_resource_mask |= CS_IDVS_RES;
      assert(batch->n < ARRAY_SIZE(batch->ins));
      batch->ins[batch->n++] = run_idvs_ins(flags);
      batch_flush(b, batch);
   }
}

static void
draw_raster(struct mali_cmd_buffer *cmd, const struct mali_draw_info *di)
{
   struct mali_gfx_draw_state *d = &cmd->gfx.draw;
   struct mali_render_state *r = &cmd->gfx.render;

   if (!di->count || !di->instance_count)
      return;
   if (!r->active || !d->pipeline || !d->pipeline->vs) {
      vk_logw(VK_LOG_OBJS(cmd), "Unified: draw outside a render pass or without a pipeline");
      return;
   }
   if (vk_command_buffer_has_error(&cmd->vk))
      return;
   if (mali_gfx_discards(cmd))
      return;

   struct idvs_batch e;
   e.d = d;
   e.n = 0;
   if (!prepare_draw(cmd, di, &e)) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      return;
   }

   struct cs_builder *b = mali_cmd_cs(cmd, MALI_SUBQUEUE_VERTEX_TILER);
   batch_move32(&e, IDVS(INDEX_COUNT), di->count);
   batch_move32(&e, IDVS(INSTANCE_COUNT), di->instance_count);
   batch_move32(&e, IDVS(INDEX_OFFSET), di->indexed ? di->first_index : 0);
   batch_move32(&e, IDVS(VERTEX_OFFSET), (uint32_t)di->vertex_offset);
   /* Zero-based instance IDs; firstInstance reaches the shader as a
    * system value (panvk). */
   batch_move32(&e, IDVS(INSTANCE_OFFSET), 0);

   struct mali_primitive_flags_packed ovr;
   pan_pack_nodefaults(&ovr, PRIMITIVE_FLAGS, cfg) {
      cfg.index_type = di->indexed ? mali_gfx_index_type(d->ib.index_size) : MALI_INDEX_TYPE_NONE;
   }

   /* Per-draw timing ("draws" mode). */
   uint32_t mh = 0;
   if (unlikely(cmd->measure) && cmd->measure->draws) {
      batch_flush(b, &e);
      mh = mali_measure_begin(cmd, MALI_MEASURE_DRAW, MALI_SUBQUEUE_VERTEX_TILER,
                              cmd->draws);
      mali_measure_info(cmd, mh, di->count, di->instance_count, di->indexed,
                        cmd->passes - 1);
      uint64_t key = 0;
      if (d->pipeline->fs)
         memcpy(&key, d->pipeline->fs->key, sizeof(key));
      mali_measure_shader(cmd, mh, key);
      mali_measure_extra(cmd, mh, d->blend_shader_mask);
   }

   /* One RUN_IDVS per tiler context: a layered pass has one per 8
    * layers, and the draw's primitives may go to any of them. */
   if (di->tess_count_va) {
      batch_flush(b, &e);
      struct cs_index addr = mali_cs_scratch_reg64(b, 0);
      struct cs_index count = cs_reg32(b, IDVS(INDEX_COUNT));
      cs_move64_to(b, addr, di->tess_count_va);
      struct cs_index active = count;
      if (di->tess_indirect_instances) {
         cs_load_to(b, cs_reg_tuple(b, IDVS(INDEX_COUNT), 2), addr, 3, 0);
         active = mali_cs_scratch_reg32(b, 2);
         cs_umin32(b, active, count, cs_reg32(b, IDVS(INSTANCE_COUNT)));
         d->regs_valid &= ~BITFIELD64_BIT(IDVS(INSTANCE_COUNT));
      } else {
         cs_load32_to(b, count, addr, 0);
      }
      cs_if(b, MALI_CS_CONDITION_GREATER, active) {
         launch_raster(cmd, b, &e, ovr.opaque[0]);
      }
      d->regs_valid &= ~BITFIELD64_BIT(IDVS(INDEX_COUNT));
   } else {
      launch_raster(cmd, b, &e, ovr.opaque[0]);
   }
   r->draws++;
   cmd->draws++;
   if (unlikely(mh))
      mali_measure_end(cmd, mh);
}

/* Internal shader-output handoff. Do not use ALL_COMMANDS here: the
 * fragment pass may still be open, and waiting for it would form a cycle. */
static void
poly_barrier(struct mali_cmd_buffer *cmd, VkPipelineStageFlags2 dst)
{
   const VkMemoryBarrier2 mem = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
      .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
      .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
      .dstStageMask = dst,
      .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT,
   };
   const VkDependencyInfo info = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .memoryBarrierCount = 1, .pMemoryBarriers = &mem,
   };
   struct mali_cs_deps deps = {0};
   mali_cmd_add_deps(cmd, &info, &deps);
   mali_cmd_emit_barrier(cmd, &deps);
}

static bool
poly_geometry_prepare(struct mali_cmd_buffer *cmd, const struct mali_draw_info *di,
                      struct mali_draw_info *rast)
{
   struct mali_gfx_draw_state *d = &cmd->gfx.draw;
   const struct mali_graphics_pipeline *p = d->pipeline;
   const struct poly_gs_info *gs = &p->geometry.gs;
   const struct vk_dynamic_graphics_state *dyn = &cmd->vk.dynamic_graphics_state;
   const enum mesa_prim mode = BITSET_TEST(p->dynamic, MESA_VK_DYNAMIC_IA_PRIMITIVE_TOPOLOGY) ?
      vk_topology_to_mesa(dyn->ia.primitive_topology) : vk_topology_to_mesa(p->state.ia.primitive_topology);
   const bool restart = BITSET_TEST(p->dynamic, MESA_VK_DYNAMIC_IA_PRIMITIVE_RESTART_ENABLE) ?
      dyn->ia.primitive_restart_enable : p->state.ia.primitive_restart_enable;
   /* Primitive restart is an index-buffer operation. The non-indexed
    * input has no restart tokens, regardless of the pipeline state. */
   if ((di->indexed && restart) || mode >= MESA_PRIM_PATCHES) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_FEATURE_NOT_PRESENT);
      return false;
   }
   if (di->count > INT32_MAX) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      return false;
   }
   const uint32_t prims = u_decomposed_prims_for_vertices(mode, di->count);
   if (!prims)
      return false;
   const uint64_t vertex_count = (uint64_t)di->count * di->instance_count;
   const uint64_t total_prims = (uint64_t)prims * di->instance_count;
   const uint32_t vertex_stride =
      MAX2(util_bitcount64(p->geometry.vertex_outputs) * 16, 1);
   const uint32_t count_stride = MAX2(gs->count_words, 1) * 4;
   if (vertex_count > UINT32_MAX / vertex_stride ||
       total_prims > UINT32_MAX / 4 / MAX2(gs->max_indices, 1) ||
       total_prims > UINT32_MAX / count_stride) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      return false;
   }
   const uint64_t vertices_size = vertex_count * vertex_stride;
   const uint64_t index_count = gs->max_indices * total_prims;
   const uint64_t counts_size = total_prims * count_stride;
   struct mali_ptr root_mem = mali_cmd_alloc(cmd, sizeof(struct mali_poly_params), 16);
   struct mali_ptr vp_mem = mali_cmd_alloc(cmd, sizeof(struct poly_vertex_params), 16);
   struct mali_ptr gp_mem = mali_cmd_alloc(cmd, sizeof(struct poly_geometry_params), 16);
   struct mali_ptr vertices = mali_cmd_alloc(cmd, MAX2(vertices_size, 16), 16);
   struct mali_ptr indices = mali_cmd_alloc(cmd, MAX2(index_count * 4, 64), 16);
   struct mali_ptr counts = mali_cmd_alloc(cmd, MAX2(counts_size, 16), 16);
   struct mali_ptr scratch = mali_cmd_alloc(cmd, 1152, 16);
   if (!root_mem.cpu || !vp_mem.cpu || !gp_mem.cpu || !vertices.cpu ||
       !indices.cpu || !counts.cpu || !scratch.cpu) return false;
   memset(scratch.cpu, 0, 1152);
   struct mali_poly_params root = {
      .vertex = vp_mem.gpu, .geometry = gp_mem.gpu, .ro_sink = scratch.gpu,
      .printf_buffer = scratch.gpu + 128, .first_vertex = di->vertex_offset,
      .base_instance = di->first_instance,
      .provoking_last = p->state.rs.provoking_vertex == VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT,
   };
   for (unsigned i = 0; i < 4; i++) root.stat_query[i] = scratch.gpu + 16 + i * 8;
   memcpy(root_mem.cpu, &root, sizeof(root));
   struct poly_vertex_params vp;
   const uint32_t wg[] = {1, 1, 1};
   poly_vertex_params_init(&vp, p->geometry.vertex_outputs, wg);
   poly_vertex_params_set_draw(&vp, di->count, di->instance_count);
   vp.output_buffer = vertices.gpu;
   if (di->indexed) {
      const uint64_t off = (uint64_t)di->first_index * d->ib.index_size;
      vp.index_size_B = d->ib.index_size;
      vp.index_buffer = d->ib.addr + MIN2(off, d->ib.size);
      vp.index_buffer_range_el = d->ib.index_size && off < d->ib.size ?
         MIN2((d->ib.size - off) / d->ib.index_size, UINT32_MAX) : 0;
   }
   memcpy(vp_mem.cpu, &vp, sizeof(vp));
   struct poly_geometry_params gp;
   poly_geometry_params_init(&gp, mode, wg);
   poly_geometry_params_set_draw(&gp, mode, gs->shape, gs->max_indices,
                                  di->count, di->instance_count);
   gp.output_index_buffer = indices.gpu;
   gp.count_buffer = counts.gpu;
   gp.count_buffer_stride = gs->count_words * 4;
   for (unsigned i = 0; i < 4; i++)
      gp.prims_generated_counter[i] = scratch.gpu + 64 + i * 4;
   memcpy(gp_mem.cpu, &gp, sizeof(gp));
   cmd->poly_params = root_mem.gpu;
   d->base_instance = di->first_instance;
   const uint32_t base[] = {0, 0, 0};
   uint32_t groups[] = {di->count, di->instance_count, 1};
#define POLY_DISPATCH(shader_) MALI_PER_ARCH(cmd_dispatch_shader)(cmd, shader_, \
   &cmd->gfx.desc, cmd->push_constants, sizeof(cmd->push_constants), base, groups)
   POLY_DISPATCH(p->geometry.vertex);
   poly_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
   groups[0] = prims;
   if (p->geometry.count) {
      POLY_DISPATCH(p->geometry.count);
      poly_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
   }
   if (p->geometry.pre) {
      groups[0] = groups[1] = 1;
      POLY_DISPATCH(p->geometry.pre);
      poly_barrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
   }
   groups[0] = prims;groups[1] = di->instance_count;
   POLY_DISPATCH(p->geometry.main);
#undef POLY_DISPATCH
   poly_barrier(cmd, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT);
   *rast = (struct mali_draw_info){
      .count = gp.draw.index_count, .instance_count = gp.draw.instance_count,
      .indexed = poly_gs_indexed(gs->shape),
   };
   if (rast->indexed) {
      d->ib.addr = indices.gpu;
      d->ib.index_size = poly_gs_index_size(gs->shape);
      d->ib.size = gs->shape == POLY_GS_SHAPE_STATIC_INDEXED ? gs->max_indices : index_count * 4;
      if (gs->shape == POLY_GS_SHAPE_STATIC_INDEXED)
         memcpy(indices.cpu, gs->topology, MIN2(gs->max_indices, sizeof(gs->topology)));
   }
   d->dirty |= MALI_GFX_DIRTY_VS_FAU | MALI_GFX_DIRTY_VS_SRT | MALI_GFX_DIRTY_INDEX;
   return !vk_command_buffer_has_error(&cmd->vk);
}

static void
dispatch_tess_stage(struct mali_cmd_buffer *cmd, const struct mali_shader *shader,
                    uint32_t x, uint32_t y, VkPipelineStageFlags2 next)
{
   const uint32_t base[] = {0, 0, 0}, groups[] = {x, y, 1};
   MALI_PER_ARCH(cmd_dispatch_shader)(cmd, shader, &cmd->gfx.desc,
      cmd->push_constants, sizeof(cmd->push_constants), base, groups);
   poly_barrier(cmd, next);
}

struct tess_indirect_draw {
   uint64_t params, count_va;
   uint32_t draw_id, max_patches;
};

static void
dispatch_poly_grid(struct mali_cmd_buffer *cmd, const struct mali_shader *shader,
                   uint64_t grid, uint64_t vertex_srt, VkPipelineStageFlags2 next)
{
   mali_v11_cmd_dispatch_poly_indirect(cmd, shader, grid, vertex_srt);
   poly_barrier(cmd, next);
}

static bool
tess_geometry_prepare(struct mali_cmd_buffer *cmd, const struct mali_poly_params *root,
                      uint32_t max_vertices, struct mali_draw_info *raster)
{
   struct mali_gfx_draw_state *d = &cmd->gfx.draw;
   const struct mali_graphics_pipeline *p = d->pipeline;
   const struct poly_gs_info *gs = &p->geometry.gs;
   const enum mesa_prim mode = p->tess.raster_mode;
   const uint32_t max_prims = u_decomposed_prims_for_vertices(mode, max_vertices);
   const uint64_t vertices_size = (uint64_t)max_vertices *
      MAX2(util_bitcount64(p->tess.eval_outputs) * 16, 1);
   const uint64_t index_size = (uint64_t)max_prims * gs->max_indices * 4;
   const uint64_t count_size = (uint64_t)max_prims * gs->count_words * 4;
   if (vertices_size > UINT32_MAX || index_size > UINT32_MAX || count_size > UINT32_MAX) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      return false;
   }

   const struct mali_ptr new_root = mali_cmd_alloc(cmd, sizeof(*root), 16);
   const struct mali_ptr vp_mem = mali_cmd_alloc(cmd, sizeof(struct poly_vertex_params), 16);
   const struct mali_ptr gp_mem = mali_cmd_alloc(cmd, sizeof(struct poly_geometry_params), 16);
   const struct mali_ptr vertices = mali_cmd_alloc(cmd, MAX2(vertices_size, 16), 16);
   const struct mali_ptr counts = mali_cmd_alloc(cmd, MAX2(count_size, 16), 16);
   const bool indexed = poly_gs_indexed(gs->shape);
   const bool static_indices = gs->shape == POLY_GS_SHAPE_STATIC_INDEXED;
   const uint32_t index_bytes = static_indices ? gs->max_indices : index_size;
   const struct mali_ptr indices = indexed ?
      mali_cmd_alloc(cmd, MAX2(index_bytes, 16), 16) : (struct mali_ptr){0};
   if (!new_root.cpu || !vp_mem.cpu || !gp_mem.cpu || !vertices.cpu || !counts.cpu ||
       (indexed && !indices.cpu))
      return false;

   struct mali_poly_params geometry = *root;
   geometry.vertex = vp_mem.gpu;
   geometry.geometry = gp_mem.gpu;
   geometry.provoking_last = p->state.rs.provoking_vertex == VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
   for (unsigned i = 0; i < 4; i++)
      geometry.stat_query[i] = root->ro_sink + 16 + i * 8;
   memcpy(new_root.cpu, &geometry, sizeof(geometry));

   const uint32_t wg[] = {1, 1, 1};
   struct poly_vertex_params vp;
   poly_vertex_params_init(&vp, p->tess.eval_outputs, wg);
   vp.output_buffer = vertices.gpu;
   memcpy(vp_mem.cpu, &vp, sizeof(vp));
   struct poly_geometry_params gp;
   poly_geometry_params_init(&gp, mode, wg);
   gp.output_index_buffer = indices.gpu;
   gp.count_buffer = counts.gpu;
   gp.count_buffer_stride = gs->count_words * 4;
   for (unsigned i = 0; i < 4; i++)
      gp.prims_generated_counter[i] = root->ro_sink + 64 + (i + 1) * 4;
   memcpy(gp_mem.cpu, &gp, sizeof(gp));
   if (static_indices)
      memcpy(indices.cpu, gs->topology, MIN2(index_bytes, sizeof(gs->topology)));

   cmd->poly_params = new_root.gpu;
   const VkPipelineStageFlags2 compute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
   const uint64_t grid = gp_mem.gpu + offsetof(struct poly_geometry_params, grid);
   dispatch_tess_stage(cmd, p->tess.gs_setup, 1, 1, compute);
   dispatch_poly_grid(cmd, p->tess.eval, grid + 12, 0, compute);
   if (p->geometry.count)
      dispatch_poly_grid(cmd, p->geometry.count, grid, 0, compute);
   if (p->geometry.pre)
      dispatch_tess_stage(cmd, p->geometry.pre, 1, 1, compute);
   dispatch_poly_grid(cmd, p->geometry.main, grid, 0,
                     VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT);

   *raster = (struct mali_draw_info){
      .count = 1, .instance_count = 1, .indexed = indexed,
      .tess_count_va = gp_mem.gpu + offsetof(struct poly_geometry_params, draw),
      .tess_indirect_instances = true,
   };
   if (indexed) {
      d->ib.addr = indices.gpu;
      d->ib.index_size = poly_gs_index_size(gs->shape);
      d->ib.size = index_bytes;
   }
   return !vk_command_buffer_has_error(&cmd->vk);
}

static bool
tess_prepare(struct mali_cmd_buffer *cmd, const struct mali_draw_info *di,
             struct mali_draw_info *raster, const struct tess_indirect_draw *indirect)
{
   struct mali_gfx_draw_state *d = &cmd->gfx.draw;
   const struct mali_graphics_pipeline *p = d->pipeline;
   const struct mali_tess_pipeline *tess = &p->tess;
   const struct vk_dynamic_graphics_state *dyn = &cmd->vk.dynamic_graphics_state;
   const bool restart = BITSET_TEST(p->dynamic, MESA_VK_DYNAMIC_IA_PRIMITIVE_RESTART_ENABLE) ?
      dyn->ia.primitive_restart_enable : p->state.ia.primitive_restart_enable;
   if (di->indexed && restart) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_FEATURE_NOT_PRESENT);
      return false;
   }
   const uint32_t patches_per_instance = di->count / tess->input_points;
   if (!indirect && !patches_per_instance)
      return false;
   const uint32_t vertices_per_instance = patches_per_instance * tess->input_points;
   const uint64_t patches = indirect ? indirect->max_patches :
      (uint64_t)patches_per_instance * di->instance_count;
   const uint64_t v_stride = MAX2(util_bitcount64(tess->vertex_outputs) * 16, 1);
   const uint64_t tcs_stride = (uint64_t)tess->stride_el * 4;
   const uint64_t coords_per_patch = POLY_TES_PATCH_ID_STRIDE * sizeof(struct poly_tess_point) + 16;
   const uint64_t indices_per_patch = 65536;

   /* Leave space for each patch's full coordinate encoding and for the
    * reference tessellator's worst-case index list. Bound products before
    * truncation to the shader's 32-bit sizes. */
   if (patches > UINT32_MAX / (4 * indices_per_patch) ||
       patches > UINT32_MAX / coords_per_patch ||
       patches > UINT32_MAX / tcs_stride ||
       (tess->scratch_stride && patches > UINT32_MAX / tess->scratch_stride) ||
       patches > UINT32_MAX / v_stride / tess->input_points) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      return false;
   }
   const uint32_t coord_size = patches * coords_per_patch;
   const uint32_t index_count = patches * indices_per_patch;
   const struct mali_ptr root_mem = mali_cmd_alloc(cmd, sizeof(struct mali_poly_params), 16);
   const struct mali_ptr vp_mem = mali_cmd_alloc(cmd, sizeof(struct poly_vertex_params), 16);
   const struct mali_ptr tp_mem = mali_cmd_alloc(cmd, sizeof(struct poly_tess_params), 16);
   const struct mali_ptr heap_mem = mali_cmd_alloc(cmd, sizeof(struct poly_heap), 16);
   const struct mali_ptr vertices = mali_cmd_alloc(cmd, MAX2(patches * tess->input_points * v_stride, 16), 16);
   const struct mali_ptr control = mali_cmd_alloc(cmd, patches * tcs_stride, 16);
   const struct mali_ptr coords = mali_cmd_alloc(cmd, coord_size, 16);
   const struct mali_ptr indices = mali_cmd_alloc(cmd, (uint64_t)index_count * 4, 16);
   const struct mali_ptr counts = mali_cmd_alloc(cmd, patches * 4, 16);
   const struct mali_ptr offsets = mali_cmd_alloc(cmd, patches * 4, 16);
   const struct mali_ptr scratch = mali_cmd_alloc(cmd, 1152, 16);
   const struct mali_ptr shader_scratch = tess->scratch_stride ?
      mali_cmd_alloc(cmd, patches * tess->scratch_stride, 16) : (struct mali_ptr){0};
   if (!root_mem.cpu || !vp_mem.cpu || !tp_mem.cpu || !heap_mem.cpu || !vertices.cpu ||
       !control.cpu || !coords.cpu || !indices.cpu || !counts.cpu || !offsets.cpu || !scratch.cpu ||
       (tess->scratch_stride && !shader_scratch.cpu))
      return false;

   memset(scratch.cpu, 0, 1152);
   const struct mali_poly_params root = {
      .vertex = vp_mem.gpu, .tess = tp_mem.gpu, .ro_sink = scratch.gpu,
      .first_vertex = di->vertex_offset, .base_instance = di->first_instance,
      .printf_buffer = scratch.gpu + 128, .draw_count = scratch.gpu + 64,
      .patch_count = patches, .tess_index_limit = index_count,
      .tess_scratch = shader_scratch.gpu,
      .self = root_mem.gpu,
   };
   struct mali_poly_params *params = root_mem.cpu;
   *params = root;

   const uint32_t wg[] = {1, 1, 1};
   struct poly_vertex_params vp;
   poly_vertex_params_init(&vp, tess->vertex_outputs, wg);
   poly_vertex_params_set_draw(&vp, vertices_per_instance, di->instance_count);
   vp.output_buffer = vertices.gpu;
   if (di->indexed) {
      const uint64_t off = (uint64_t)di->first_index * d->ib.index_size;
      vp.index_size_B = d->ib.index_size;
      vp.index_buffer = d->ib.addr + MIN2(off, d->ib.size);
      vp.index_buffer_range_el = d->ib.index_size && off < d->ib.size ?
         MIN2((d->ib.size - off) / d->ib.index_size, UINT32_MAX) : 0;
   }
   memcpy(vp_mem.cpu, &vp, sizeof(vp));

   const struct poly_tess_params tp = {
      .heap = heap_mem.gpu, .patch_coord_buffer = coords.gpu,
      .coord_allocs = offsets.gpu, .tcs_buffer = control.gpu,
      .counts = counts.gpu, .index_buffer = indices.gpu,
      .tcs_per_vertex_outputs = tess->control_outputs,
      .input_patch_size = tess->input_points, .output_patch_size = tess->output_points,
      .tcs_patch_constants = tess->patch_constants, .patches_per_instance = patches_per_instance,
      .tcs_stride_el = tess->stride_el, .nr_patches = patches, .partitioning = tess->partitioning,
      .points_mode = tess->point_mode, .isolines = tess->domain == TESS_PRIMITIVE_ISOLINES,
      .ccw = mali_poly_tess_winding(tess->raster_mode == MESA_PRIM_TRIANGLES,
                                    tess->ccw, tess->lower_left),
   };
   memcpy(tp_mem.cpu, &tp, sizeof(tp));
   const struct poly_heap heap = {.base = coords.gpu, .size = coord_size};
   memcpy(heap_mem.cpu, &heap, sizeof(heap));
   cmd->poly_params = root_mem.gpu;
   d->base_instance = di->first_instance;
   const VkPipelineStageFlags2 compute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
   const VkPipelineStageFlags2 raster_stages = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                                               VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT;
   const uint64_t grid = root_mem.gpu + offsetof(struct mali_poly_params, grids);
   if (indirect) {
      params->indirect_params = indirect->params;
      params->indirect_count = indirect->count_va;
      params->draw_id = indirect->draw_id;
      params->max_patches = indirect->max_patches;
      params->tess_index_base = d->ib.addr;
      params->tess_index_elements = vp.index_buffer_range_el;
      uint64_t vertex_srt = 0;
      static_assert(MALI_MAX_VS_ATTRIBS == 16 && MALI_DESCRIPTOR_SIZE == 32,
                    "tessellation setup shader attribute descriptor ABI");
      u_foreach_bit(i, d->vi->attributes_valid & BITFIELD_MASK(MALI_MAX_VS_ATTRIBS)) {
         const struct vk_vertex_attribute_state *a = &d->vi->attributes[i];
         const struct vk_vertex_binding_state *binding = &d->vi->bindings[a->binding];
         if (binding->input_rate != VK_VERTEX_INPUT_RATE_INSTANCE || !d->vi_strides[a->binding])
            continue;
         if (!vertex_srt) {
            vertex_srt = mali_gfx_shader_vs_srt_drv(cmd, tess->vertex, &params->attribs);
            if (!vertex_srt)
               return false;
         }
         params->attrib_stride[i] = d->vi_strides[a->binding];
         params->attrib_offset[i] = a->offset;
      }
      dispatch_tess_stage(cmd, tess->indirect, 1, 1, compute);
      dispatch_poly_grid(cmd, tess->vertex, grid, vertex_srt, compute);
      dispatch_poly_grid(cmd, tess->control, grid + 12, 0, compute);
      dispatch_poly_grid(cmd, tess->count, grid + 24, 0, compute);
   } else {
      dispatch_tess_stage(cmd, tess->vertex, vertices_per_instance, di->instance_count, compute);
      dispatch_tess_stage(cmd, tess->control, patches_per_instance, di->instance_count, compute);
      dispatch_tess_stage(cmd, tess->count, patches, 1, compute);
   }
   dispatch_tess_stage(cmd, tess->prefix, 1, 1, compute);
   const VkPipelineStageFlags2 next = tess->eval ? compute : raster_stages;
   if (indirect)
      dispatch_poly_grid(cmd, tess->generate, grid + 24, 0, next);
   else
      dispatch_tess_stage(cmd, tess->generate, patches, 1, next);

   if (tess->eval) {
      d->dirty |= MALI_GFX_DIRTY_VS_FAU | MALI_GFX_DIRTY_VS_SRT | MALI_GFX_DIRTY_INDEX;
      return tess_geometry_prepare(cmd, &root, index_count, raster);
   }

   *raster = (struct mali_draw_info){
      .count = index_count, .instance_count = 1, .indexed = true,
      .tess_count_va = root.draw_count,
   };
   d->ib.addr = indices.gpu;
   d->ib.index_size = 4;
   d->ib.size = (uint64_t)index_count * 4;
   d->dirty |= MALI_GFX_DIRTY_VS_FAU | MALI_GFX_DIRTY_VS_SRT | MALI_GFX_DIRTY_INDEX;
   return !vk_command_buffer_has_error(&cmd->vk);
}

static void
draw(struct mali_cmd_buffer *cmd, const struct mali_draw_info *di,
     const struct tess_indirect_draw *indirect)
{
   struct mali_gfx_draw_state *d = &cmd->gfx.draw;
   if (!d->pipeline || (!d->pipeline->geometry.vertex && !d->pipeline->tess.control) ||
       !di->count || !di->instance_count) {
      draw_raster(cmd, di);
      return;
   }
   if (!cmd->gfx.render.active || vk_command_buffer_has_error(&cmd->vk)) return;
   if (mali_gfx_discards(cmd))
      return;
   mali_gfx_collect_dynamic_dirty(cmd);
   const uint64_t saved_root = cmd->poly_params;
   const __typeof__(d->ib) saved_ib = d->ib;
   struct mali_draw_info rast;
   const bool prepared = d->pipeline->tess.control ?
      tess_prepare(cmd, di, &rast, indirect) : poly_geometry_prepare(cmd, di, &rast);
   if (prepared) draw_raster(cmd, &rast);
   cmd->poly_params = saved_root;
   d->ib = saved_ib;
   d->dirty |= MALI_GFX_DIRTY_VS_FAU | MALI_GFX_DIRTY_VS_SRT | MALI_GFX_DIRTY_INDEX;
}

void
mali_v11_tess_draw_indirect(struct mali_cmd_buffer *cmd, bool indexed, uint64_t params,
                           uint32_t stride, uint64_t count_va, uint32_t max_draws)
{
   /* A test build with bounded per-draw storage. Zero and out-of-range GPU
    * counts emit no geometry, and a large call cannot reserve unbounded BOs. */
   if (max_draws > 16) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_FEATURE_NOT_PRESENT);
      return;
   }
   const struct mali_draw_info di = {.count = 1, .instance_count = 1, .indexed = indexed};
   for (uint32_t i = 0; i < max_draws && !vk_command_buffer_has_error(&cmd->vk); i++) {
      const struct tess_indirect_draw indirect = {
         .params = params + (uint64_t)i * stride, .count_va = count_va,
         .draw_id = i, .max_patches = 8,
      };
      draw(cmd, &di, &indirect);
   }
}

VKAPI_ATTR void VKAPI_CALL
MALI_PER_ARCH(CmdDraw)(VkCommandBuffer commandBuffer, uint32_t vertexCount, uint32_t instanceCount,
                       uint32_t firstVertex, uint32_t firstInstance)
{
   VK_FROM_HANDLE(mali_cmd_buffer, cmd, commandBuffer);
   const struct mali_draw_info di = {
      .count = vertexCount,
      .instance_count = instanceCount,
      .vertex_offset = (int32_t)firstVertex,
      .first_instance = firstInstance,
   };
   draw(cmd, &di, NULL);
}

VKAPI_ATTR void VKAPI_CALL
MALI_PER_ARCH(CmdDrawIndexed)(VkCommandBuffer commandBuffer, uint32_t indexCount,
                              uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset,
                              uint32_t firstInstance)
{
   VK_FROM_HANDLE(mali_cmd_buffer, cmd, commandBuffer);
   const struct mali_draw_info di = {
      .count = indexCount,
      .instance_count = instanceCount,
      .first_index = firstIndex,
      .vertex_offset = vertexOffset,
      .first_instance = firstInstance,
      .indexed = true,
   };
   draw(cmd, &di, NULL);
}

/* ---------------------------------------------------------------------- */
/* Full-screen draws                                                       */

void
MALI_PER_ARCH(cmd_run_fullscreen)(struct mali_cmd_buffer *cmd, uint64_t dcd, const VkRect2D *rect,
                                  uint32_t base_layer, uint32_t layer_count)
{
   struct mali_render_state *r = &cmd->gfx.render;
   struct cs_builder *b = mali_cmd_cs(cmd, MALI_SUBQUEUE_VERTEX_TILER);

   if (!MALI_PER_ARCH(cmd_render_tiler)(cmd)) {
      vk_command_buffer_set_error(&cmd->vk, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      return;
   }

   int minx = MAX2(rect->offset.x, 0), miny = MAX2(rect->offset.y, 0);
   int maxx = MIN2(rect->offset.x + (int)rect->extent.width, (int)r->desc.width) - 1;
   int maxy = MIN2(rect->offset.y + (int)rect->extent.height, (int)r->desc.height) - 1;
   if (maxx < minx || maxy < miny)
      return;
   struct mali_scissor_packed sc;
   pan_pack(&sc, SCISSOR, cfg) {
      cfg.scissor_minimum_x = minx;
      cfg.scissor_minimum_y = miny;
      cfg.scissor_maximum_x = maxx;
      cfg.scissor_maximum_y = maxy;
   }
   cs_move64_to(b, cs_reg64(b, IDVS(SCISSOR_BOX)), sc.opaque[0] | (uint64_t)sc.opaque[1] << 32);
   cs_move32_to(b, cs_reg32(b, IDVS(TILER_FLAGS2)), 0);

   struct cs_index dcd_reg = mali_cs_scratch_reg64(b, 0);
   cs_move64_to(b, dcd_reg, dcd);

   /* RUN_FULLSCREEN draws into the layers its tiler flags' view mask
    * selects inside the current tiler context (panvk cmd_run_fullscreen). */
   const uint32_t first_td = base_layer / MALI_LAYERS_PER_TILER_CTX;
   const uint32_t last_td = (base_layer + layer_count - 1) / MALI_LAYERS_PER_TILER_CTX;
   for (uint32_t td = first_td; td <= last_td && td < r->td_count; td++) {
      const uint32_t td_base = td * MALI_LAYERS_PER_TILER_CTX;
      const uint32_t first = MAX2(base_layer, td_base);
      const uint32_t last = MIN2(base_layer + layer_count, td_base + MALI_LAYERS_PER_TILER_CTX);
      uint32_t view_mask = BITFIELD_RANGE(first - td_base, last - first);
      if (r->desc.view_mask) {
         if (td_base >= 32)
            continue;
         view_mask &= r->desc.view_mask >> td_base;
      }
      if (!view_mask)
         continue;
      struct mali_primitive_flags_packed tf;
      pan_pack(&tf, PRIMITIVE_FLAGS, cfg) {
         cfg.low_depth_cull = false;
         cfg.high_depth_cull = false;
         cfg.view_mask = view_mask;
      }
      cs_move64_to(b, cs_reg64(b, IDVS(TILER_CTX)), r->tiler + td * pan_size(TILER_CONTEXT));
      cs_move32_to(b, cs_reg32(b, IDVS(TILER_FLAGS)), tf.opaque[0]);
      cs_run_fullscreen(b, 0, dcd_reg);
   }

   r->draws++;
   cmd->gfx.draw.dirty |= MALI_GFX_DIRTY_PIPELINE | MALI_GFX_DIRTY_VIEWPORT |
                          MALI_GFX_DIRTY_PASS;
   /* The scissor, tiler flags and tiler context registers changed behind
    * the draws' record of them. */
   cmd->gfx.draw.regs_valid = 0;
}

/* A primitive barrier over the whole framebuffer: later fragments of the
 * pass wait for earlier ones in each tile (panvk cmd_fb_barrier). */
void
MALI_PER_ARCH(cmd_fb_barrier)(struct mali_cmd_buffer *cmd)
{
   struct mali_render_state *r = &cmd->gfx.render;
   struct mali_ptr zsd = mali_cmd_alloc(cmd, pan_size(DEPTH_STENCIL), 32);
   struct mali_ptr dcd = mali_cmd_alloc(cmd, pan_size(DRAW), 64);
   if (!zsd.cpu || !dcd.cpu)
      return;

   pan_cast_and_pack(zsd.cpu, DEPTH_STENCIL, cfg) {
      cfg.stencil_test_enable = false;
      cfg.depth_write_enable = false;
      cfg.depth_function = MALI_FUNC_ALWAYS;
   }
   pan_cast_and_pack(dcd.cpu, DRAW, cfg) {
      cfg.flags_0.allow_forward_pixel_to_kill = false;
      cfg.flags_0.allow_forward_pixel_to_be_killed = false;
      cfg.flags_0.primitive_barrier = true;
      cfg.flags_0.occlusion_query = MALI_OCCLUSION_MODE_DISABLED;
      cfg.flags_2.read_mask = 0;
      cfg.flags_2.write_mask = 0;
      cfg.flags_2.no_shader_depth_read = true;
      cfg.flags_2.no_shader_stencil_read = true;
      cfg.depth_stencil = zsd.gpu;
   }
   const VkRect2D all = {{0, 0}, {r->desc.width, r->desc.height}};
   MALI_PER_ARCH(cmd_run_fullscreen)(cmd, dcd.gpu, &all, 0, MAX2(r->desc.layer_count, 1));
}
