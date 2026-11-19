/*
 * SPDX-FileCopyrightText: 2026 ARMSX2 and bmdhacks
 * SPDX-License-Identifier: MIT
 */

/*
 * vkCreateGraphicsPipelines, vkCreateComputePipelines and pipeline
 * destruction.
 *
 * Mesa's runtime has a generic pipeline implementation (vk_pipeline.c,
 * which panvk uses) built on per-shader objects; we use its pieces
 * (SPIR-V to NIR for a stage, stage hashing, robustness, the pipeline
 * cache, graphics state parsing) but create the pipeline objects ourselves,
 * because a pipeline here bakes hardware words at creation, as the blob
 * does, and ARMSX2 needs only vertex + fragment and compute pipelines
 * without libraries.
 *
 * Shaders are cached per stage. Keys:
 *   vertex   = BLAKE3("vs", stage, layout, view mask)
 *   fragment = BLAKE3("fs", stage, layout, vertex stage, the state the
 *              fragment lowering reads)
 *   compute  = BLAKE3("cs", stage, layout)
 * and on v7 and v11, for the fragment shader that only clips
 * (clip_fs_compile),
 *   BLAKE3("mali-v7-clip-fs" or "mali-v11-clip-fs", the vertex shader's
 *          varying layout, the state the fragment lowering reads)
 * "stage" is the runtime's hash of the stage create info (SPIR-V, entry
 * point, specialization, flags, robustness). The fragment shader is
 * compiled against the vertex shader's varying layout, so its key includes
 * the vertex stage. The vertex shader is specialized on which generic
 * varyings the fragment shader reads (strip_unused_outputs), so its key
 * includes that mask; it reads the noperspective mask from a system value, so
 * one compiled vertex shader serves every pipeline whose fragment shader reads
 * the same varyings.
 */

#ifndef PAN_ARCH
#error "mali_pipeline.c is built per arch: PAN_ARCH must be set"
#endif

#include "mali_blend.h"
#include "mali_pipeline.h"
#if PAN_ARCH >= 10
#include "mali_cmd_buffer.h"
#else
#include "mali_jm.h"
#endif
#include "mali_vk.h"

#include "mali_arch.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

#include "vk_alloc.h"
#include "compiler/spirv/nir_spirv.h"
#include "vk_command_buffer.h"
#include "vk_log.h"
#include "vk_shader_module.h"
#include "vk_util.h"

#include "panfrost/compiler/pan_nir.h"
#include "poly/tessellator.h"
#include "util/hash_table.h"
#include "util/os_time.h"
#include "util/simple_mtx.h"

#include "mali_compiler.h"
#include "mali_measure.h"

/* ---------------------------------------------------------------------- */
/* Destruction and bind                                                    */

static void
tess_pipeline_finish(struct mali_device *dev, struct mali_tess_pipeline *tess)
{
   MALI_PER_ARCH(shader_unref)(dev, tess->vertex);
   MALI_PER_ARCH(shader_unref)(dev, tess->control);
   MALI_PER_ARCH(shader_unref)(dev, tess->count);
   MALI_PER_ARCH(shader_unref)(dev, tess->prefix);
   MALI_PER_ARCH(shader_unref)(dev, tess->generate);
   MALI_PER_ARCH(shader_unref)(dev, tess->indirect);
   MALI_PER_ARCH(shader_unref)(dev, tess->eval);
   MALI_PER_ARCH(shader_unref)(dev, tess->gs_setup);
}

static void
graphics_pipeline_destroy(struct vk_device *vk_dev, struct vk_pipeline *vk_pipeline,
                          const VkAllocationCallbacks *alloc)
{
   struct mali_device *dev = container_of(vk_dev, struct mali_device, vk);
   struct mali_graphics_pipeline *p = mali_graphics_pipeline(vk_pipeline);

   MALI_PER_ARCH(shader_unref)(dev, p->vs);
   MALI_PER_ARCH(shader_unref)(dev, p->geometry.vertex);
   MALI_PER_ARCH(shader_unref)(dev, p->geometry.main);
   MALI_PER_ARCH(shader_unref)(dev, p->geometry.count);
   MALI_PER_ARCH(shader_unref)(dev, p->geometry.pre);
   tess_pipeline_finish(dev, &p->tess);
   MALI_PER_ARCH(shader_unref)(dev, p->fs);
   if (p->zsd_mem.size)
      mali_bo_pool_free(&dev->desc_pool, &p->zsd_mem);
   if (p->base.layout)
      vk_pipeline_layout_unref(vk_dev, p->base.layout);
   vk_pipeline_free(vk_dev, alloc, vk_pipeline);
}

static void
compute_pipeline_destroy(struct vk_device *vk_dev, struct vk_pipeline *vk_pipeline,
                         const VkAllocationCallbacks *alloc)
{
   struct mali_device *dev = container_of(vk_dev, struct mali_device, vk);
   struct mali_compute_pipeline *p = mali_compute_pipeline(vk_pipeline);

   MALI_PER_ARCH(shader_unref)(dev, p->cs);
   if (p->base.layout)
      vk_pipeline_layout_unref(vk_dev, p->base.layout);
   vk_pipeline_free(vk_dev, alloc, vk_pipeline);
}

/* The pipeline ops' bind (the runtime's vkCmdBindPipeline would call it;
 * the per-arch CmdBindPipeline entry point replaces that). The
 * dynamic-state part of a graphics bind is mali_cmd_bind_graphics'. */
static void
pipeline_cmd_bind(struct vk_command_buffer *cmd, struct vk_pipeline *pipeline)
{
   MALI_PER_ARCH(cmd_bind_pipeline)(container_of(cmd, struct mali_cmd_buffer, vk), pipeline);
}

static const struct vk_pipeline_ops graphics_pipeline_ops = {
   .destroy = graphics_pipeline_destroy,
   .cmd_bind = pipeline_cmd_bind,
};

static const struct vk_pipeline_ops compute_pipeline_ops = {
   .destroy = compute_pipeline_destroy,
   .cmd_bind = pipeline_cmd_bind,
};

/* ---------------------------------------------------------------------- */
/* Shader lookup and compile                                               */

/* Everything about a layout that the shader lowering reads. */
static void
hash_layout(struct mesa_blake3 *ctx, const struct vk_pipeline_layout *layout)
{
   static const blake3_hash zero;

   uint32_t set_count = layout ? layout->set_count : 0;
   _mesa_blake3_update(ctx, &set_count, sizeof(set_count));
   for (uint32_t i = 0; i < set_count; i++) {
      const struct vk_descriptor_set_layout *sl = layout->set_layouts[i];
      _mesa_blake3_update(ctx, sl ? sl->blake3 : zero, sizeof(blake3_hash));
   }
   /* Push constant ranges do not change the code (offsets are the API's),
    * so they are not part of the key. */
}

struct stage {
   const VkPipelineShaderStageCreateInfo *info;
   struct vk_pipeline_robustness_state rs;
   blake3_hash stage_hash;
   blake3_hash key;
   nir_shader *nir;
   struct mali_shader *shader;
   bool cache_hit;
   bool compact_fxaa;
   bool predecode_push_colors;
   bool local_read_rcp;
   int64_t duration_ns;
   /* Vertex stage: the generic varying slots the fragment stage reads (~0: all, for a
    * pipeline whose fragment stage is unknown). */
   uint64_t keep_outputs;
};

static bool
stage_spirv(const VkPipelineShaderStageCreateInfo *info, const uint32_t **words,
            size_t *size);

#if PAN_ARCH == 11
static bool
shader_option(const char *variable, const char *property_name)
{
   const char *enabled = getenv(variable);
#ifdef __ANDROID__
   char property[PROP_VALUE_MAX];
   if (!enabled && __system_property_get(property_name, property) > 0)
      enabled = property;
#else
   (void)property_name;
#endif
   return enabled && strcmp(enabled, "1") == 0;
}

static bool
matches_fragment(const VkPipelineShaderStageCreateInfo *info, size_t captured_size,
                 const blake3_hash captured)
{
   if (info->stage != VK_SHADER_STAGE_FRAGMENT_BIT || info->pSpecializationInfo ||
       strcmp(info->pName, "main") != 0)
      return false;
   const uint32_t *words;
   size_t size;
   if (!stage_spirv(info, &words, &size) || size != captured_size)
      return false;
   blake3_hash hash;
   _mesa_blake3_compute(words, size, hash);
   return memcmp(hash, captured, sizeof(hash)) == 0;
}

static bool
compact_fxaa_stage(const VkPipelineShaderStageCreateInfo *info)
{
   /* Keep this experiment restricted to the exact captured FXAA module. */
   static const blake3_hash captured = {
      0x8f, 0xb5, 0xc8, 0xa7, 0xa4, 0x6a, 0xc5, 0xc8,
      0x84, 0xb6, 0x65, 0xee, 0x0a, 0x85, 0x8c, 0xbb,
      0x14, 0xce, 0x10, 0x4d, 0x17, 0x7d, 0x17, 0xf1,
      0x51, 0x98, 0x31, 0x7f, 0x83, 0x25, 0xd0, 0x0d,
   };
   return UNIFIED_COMPACT_FXAA_LAB &&
          shader_option("UNIFIED_COMPACT_FXAA", "debug.unified.compact_fxaa") &&
          matches_fragment(info, 14484, captured);
}

static bool
local_read_stage(const VkPipelineShaderStageCreateInfo *info)
{
   static const blake3_hash captured = {
      0x4d, 0x63, 0x17, 0x88, 0xa0, 0x1b, 0x5a, 0x2a,
      0xb5, 0xa3, 0x4d, 0x12, 0x12, 0xc9, 0xe8, 0x9d,
      0x2b, 0x36, 0xfd, 0xde, 0x36, 0x5d, 0xae, 0xe3,
      0x5c, 0xee, 0x7a, 0x00, 0x9c, 0x4d, 0x34, 0x2c,
   };
   return matches_fragment(info, 28912, captured);
}

static void
keep_fxaa_search_loop(nir_shader *nir)
{
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_loop *loop = nir_block_get_following_loop(block);
         if (loop && loop->control == nir_loop_control_none)
            loop->control = nir_loop_control_dont_unroll;
      }
   }
}
#endif

static struct vk_pipeline_cache *
pick_cache(struct mali_device *dev, VkPipelineCache handle)
{
   VK_FROM_HANDLE(vk_pipeline_cache, cache, handle);
   return cache ? cache : dev->vk.mem_cache;
}

static void
stage_init(struct mali_device *dev, VkPipelineCreateFlags2KHR flags,
           const void *pipeline_pNext, const VkPipelineShaderStageCreateInfo *info,
           struct stage *st)
{
   memset(st, 0, sizeof(*st));
   st->keep_outputs = ~0ull;
   st->info = info;
   vk_pipeline_robustness_state_fill(&dev->vk.robustness_state, &st->rs,
                                     pipeline_pNext, info->pNext);
#if PAN_ARCH == 7
   /* A v7 Vertex job shades every vertex of the draw's index range,
    * including ones no index names (mali_jm_cmd_draw_v7.c), which the
    * application never meant to run. A vertex shader that pulls its
    * vertices out of a storage buffer by gl_VertexIndex can compute an
    * address far outside the buffer for one of those: ARMSX2's line
    * expansion reads vertex BaseVertex + n - 1, which wraps to 2^32 - 1
    * when BaseVertex and n are 0, and the unchecked load there faults the
    * GPU context. Vertex shaders get bounds-checked storage buffer loads,
    * as with robustBufferAccess (the loads are clamped, not branched
    * around: mali_nir_lower_bounded_loads.c); the hash below keeps them
    * apart in the caches. */
   if (info->stage == VK_SHADER_STAGE_VERTEX_BIT &&
       st->rs.storage_buffers == VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DISABLED_EXT)
      st->rs.storage_buffers = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_ROBUST_BUFFER_ACCESS_EXT;
#endif
   vk_pipeline_hash_shader_stage(flags, info, &st->rs, st->stage_hash);
#if PAN_ARCH == 11
   st->compact_fxaa = compact_fxaa_stage(info);
   if (UNIFIED_PUSH_COLOR_LAB &&
       shader_option("UNIFIED_PUSH_COLORS", "debug.unified.push_colors") &&
       local_read_stage(info)) {
      st->predecode_push_colors = true;
      st->local_read_rcp = shader_option("UNIFIED_LOCAL_READ_RCP",
                                        "debug.unified.local_read_rcp");
   }
   if (st->compact_fxaa || st->predecode_push_colors) {
      const char *variant = st->compact_fxaa ? "unified-fxaa-loop-v1"
                           : st->local_read_rcp ? "unified-push-color-local-read-rcp-v1"
                                              : "unified-push-color-v1";
      struct mesa_blake3 hash;
      _mesa_blake3_init(&hash);
      _mesa_blake3_update(&hash, st->stage_hash, sizeof(st->stage_hash));
      _mesa_blake3_update(&hash, variant, strlen(variant));
      _mesa_blake3_final(&hash, st->stage_hash);
   }
#endif
}

static bool
stage_lookup(struct mali_device *dev, struct vk_pipeline_cache *cache, struct stage *st)
{
   if (!cache)
      return false;

   struct vk_pipeline_cache_object *obj =
      vk_pipeline_cache_lookup_object(cache, st->key, sizeof(st->key),
                                      &MALI_PER_ARCH(shader_cache_ops), &st->cache_hit);
   if (!obj) {
      st->cache_hit = false;
      return false;
   }
   st->shader = container_of(obj, struct mali_shader, base);
   st->cache_hit = true;
   p_atomic_inc(&dev->pipeline_stats.shader_cache_hits);
   return true;
}

/* The stage's SPIR-V, from its module or an inline VkShaderModuleCreateInfo. */
static bool
stage_spirv(const VkPipelineShaderStageCreateInfo *info, const uint32_t **words,
            size_t *size)
{
   VK_FROM_HANDLE(vk_shader_module, module, info->module);
   if (module) {
      *words = (const uint32_t *)module->data;
      *size = module->size;
      return true;
   }
   const VkShaderModuleCreateInfo *minfo =
      vk_find_struct_const(info->pNext, SHADER_MODULE_CREATE_INFO);
   if (!minfo)
      return false;
   *words = minfo->pCode;
   *size = minfo->codeSize;
   return true;
}

/* SPIR-V -> NIR -> generic lowering. */
static VkResult
stage_to_nir(struct mali_device *dev, VkPipelineCreateFlags2KHR flags, struct stage *st)
{
   if (st->nir)
      return VK_SUCCESS;

   /* A cache-only request must never enter the SPIR-V/NIR front end,
    * including preprocessing to discover a fragment input mask. */
   if (flags & VK_PIPELINE_CREATE_2_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_KHR)
      return VK_PIPELINE_COMPILE_REQUIRED;

   const uint32_t *words;
   size_t size;
   if (!stage_spirv(st->info, &words, &size) || size % 4 != 0 ||
       !mali_spirv_structure_ok(words, size / 4))
      return vk_errorf(dev, VK_ERROR_INVALID_SHADER_NV,
                       "malformed SPIR-V for the %s stage",
                       mesa_shader_stage_name(vk_to_mesa_shader_stage(st->info->stage)));

   const mesa_shader_stage stage = vk_to_mesa_shader_stage(st->info->stage);
   const struct spirv_to_nir_options spirv_options = MALI_PER_ARCH(shader_spirv_options)(&st->rs);
   VkResult result =
      vk_pipeline_shader_stage_to_nir(&dev->vk, flags, st->info, &spirv_options,
                                      MALI_PER_ARCH(shader_nir_options)(dev, stage), NULL,
                                      &st->nir);
   if (result != VK_SUCCESS)
      return vk_errorf(dev, VK_ERROR_INVALID_SHADER_NV,
                       "the %s stage's SPIR-V was rejected by the SPIR-V front end",
                       mesa_shader_stage_name(stage));

#if PAN_ARCH == 11
   if (st->compact_fxaa)
      keep_fxaa_search_loop(st->nir);
#endif

   /* libpoly must see the original stage IO and TCS's output barriers. */
   if (stage != MESA_SHADER_GEOMETRY &&
       stage != MESA_SHADER_TESS_CTRL && stage != MESA_SHADER_TESS_EVAL) {
      const struct nir_shader_compiler_options *original = st->nir->options;
      struct nir_shader_compiler_options options;
      if (st->predecode_push_colors) {
         options = *original;
         options.lower_unpack_unorm_4x8 = false;
         st->nir->options = &options;
      }
      MALI_PER_ARCH(shader_preprocess)(dev, st->nir);
      st->nir->options = original;
   }
   return VK_SUCCESS;
}

/*
 * MALISX2_MEASURE=shaders: every stage compiled (not cache hits) is
 * written as <dir>/shaders/<key>.<stage>.spv and .bin, with a line in
 * index.txt, so the machine code of an application's shaders can be
 * disassembled offline.
 */
static void
stage_dump(const struct stage *st, const struct mali_shader *s)
{
   static simple_mtx_t lock = SIMPLE_MTX_INITIALIZER;
   const char *dir = mali_measure_shader_dir();
   if (!dir)
      return;
   simple_mtx_lock(&lock);

   const char *sname = s->stage == MESA_SHADER_VERTEX     ? "vs"
                       : s->stage == MESA_SHADER_FRAGMENT ? "fs"
                                                          : "cs";
   uint64_t key;
   memcpy(&key, st->key, sizeof(key));
   char path[512];
   const uint32_t *words;
   size_t size;
   if (stage_spirv(st->info, &words, &size)) {
      snprintf(path, sizeof(path), "%s/%016" PRIx64 ".%s.spv", dir, key, sname);
      FILE *f = fopen(path, "wb");
      if (f) {
         fwrite(words, 1, size, f);
         fclose(f);
      }
   }
   snprintf(path, sizeof(path), "%s/%016" PRIx64 ".%s.bin", dir, key, sname);
   FILE *f = fopen(path, "wb");
   if (f) {
      fwrite(s->bin, 1, s->bin_size, f);
      fclose(f);
   }
#if PAN_ARCH == 7
   /* The compiler's static estimate: registers, spills, and cycles per
    * quad in the arithmetic, load/store, varying and texture units. */
   snprintf(path, sizeof(path), "%s/%016" PRIx64 ".%s.stats", dir, key, sname);
   f = fopen(path, "w");
   if (f) {
      pan_bifrost_stats_verbose(f, &s->info.stats.bifrost, s->info.tls_size);
      fclose(f);
   }
#endif
   snprintf(path, sizeof(path), "%s/index.txt", dir);
   f = fopen(path, "a");
   if (f) {
      fprintf(f, "%016" PRIx64 " %s bytes=%u regs=%u fau=%u entry=%s\n", key, sname,
              s->bin_size, s->info.work_reg_count, s->fau.total_count,
              st->info->pName);
      fclose(f);
   }
   simple_mtx_unlock(&lock);
}

/* The varying slots a fragment shader reads, from its input variables and any lowered loads. */
static uint64_t
fs_input_mask(nir_shader *nir)
{
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
   uint64_t m = nir->info.inputs_read;
   nir_foreach_shader_in_variable(var, nir)
      m |= BITFIELD64_RANGE(var->data.location,
                            MAX2(glsl_count_vec4_slots(var->type, true, false), 1));
   nir_foreach_function_impl(impl, nir) {
      nir_foreach_block(block, impl) {
         nir_foreach_instr(instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;
            const nir_intrinsic_instr *i = nir_instr_as_intrinsic(instr);
            if (i->intrinsic == nir_intrinsic_load_input ||
                i->intrinsic == nir_intrinsic_load_interpolated_input ||
                i->intrinsic == nir_intrinsic_load_per_vertex_input) {
               const nir_io_semantics sem = nir_intrinsic_io_semantics(i);
               m |= BITFIELD64_RANGE(sem.location, MAX2(sem.num_slots, 1));
            }
         }
      }
   }
   return m;
}

/*
 * The cross-stage dead-varying elimination panvk lacks: the generic vertex outputs
 * (VARYING_SLOT_VAR0..31) that the fragment shader does not read become temporaries and
 * their stores go, so the vertex shader writes, and the varying buffer holds, only what the
 * fragment shader reads. Arm's driver does the same: hakuX's vertex shader writes nine
 * outputs (144 bytes a vertex) and its fragment shaders read one or two; with all nine in
 * the buffer a 224k-vertex pass took 0.25 ms longer a frame
 * (docs/bringup/hakux-g615-gpu-2026-10-29.md).
 */
static void
strip_unused_outputs(nir_shader *nir, uint64_t keep)
{
   bool progress = false;
   nir_foreach_shader_out_variable_safe(var, nir) {
      const int loc = var->data.location;
      if (loc < VARYING_SLOT_VAR0 || loc > VARYING_SLOT_VAR31)
         continue;
      const unsigned slots = glsl_count_vec4_slots(var->type, false, false);
      if (BITFIELD64_RANGE(loc, MAX2(slots, 1)) & keep)
         continue;
      var->data.mode = nir_var_shader_temp;
      exec_node_remove(&var->node);
      exec_list_push_tail(&nir->variables, &var->node);
      progress = true;
   }
   if (!progress)
      return;
   nir_fixup_deref_modes(nir);
   NIR_PASS(_, nir, nir_lower_global_vars_to_local);
   nir_shader_gather_info(nir, nir_shader_get_entrypoint(nir));
}

static VkResult
stage_compile(struct mali_device *dev, struct vk_pipeline_cache *cache,
              VkPipelineCreateFlags2KHR flags, struct stage *st,
              const struct vk_pipeline_layout *layout,
              const struct vk_graphics_pipeline_state *state,
              const struct pan_varying_layout *vs_varying_layout)
{
   int64_t start = os_time_get_nano();

   VkResult result = stage_to_nir(dev, flags, st);
   if (result != VK_SUCCESS)
      return result;
   if (st->keep_outputs != ~0ull && st->nir->info.stage == MESA_SHADER_VERTEX)
      strip_unused_outputs(st->nir, st->keep_outputs);

   const uint64_t preprocessed_inputs_read =
      st->nir->info.stage == MESA_SHADER_FRAGMENT ? fs_input_mask(st->nir) : 0;

   const struct mali_shader_compile_info info = {
      .predecode_push_colors = st->predecode_push_colors,
      .local_read_rcp = st->local_read_rcp,
      .nir = st->nir,
      .rs = &st->rs,
      .layout = layout,
      .state = state,
      .vs_varying_layout = vs_varying_layout,
   };
   st->nir = NULL; /* consumed */

   struct mali_shader *shader;
   result = MALI_PER_ARCH(shader_compile)(dev, &info, st->key, &shader);
   if (result != VK_SUCCESS)
      return result;
   /* Set this before publishing the shader to the shared application cache.
    * It must be the same preprocessed mask used for the vertex cache key,
    * not the final backend's possibly reduced varying mask. */
   if (shader->stage == MESA_SHADER_FRAGMENT)
      shader->fs.preprocessed_inputs_read = preprocessed_inputs_read;
   if (st->compact_fxaa)
      mesa_logi("Unified FXAA loop variant: %u bytes, %u work registers",
                shader->bin_size, shader->info.work_reg_count);
   if (st->predecode_push_colors)
      mesa_logi("Unified push colors: %u decoded constants, %u bytes",
                shader->fau.predecoded_color_count, shader->bin_size);
   p_atomic_inc(&dev->pipeline_stats.shaders_compiled);
   stage_dump(st, shader);

   if (cache) {
      struct vk_pipeline_cache_object *obj =
         vk_pipeline_cache_add_object(cache, &shader->base);
      shader = container_of(obj, struct mali_shader, base);
   }
   st->shader = shader;
   st->duration_ns = os_time_get_nano() - start;
   return VK_SUCCESS;
}

static void
stage_finish(struct mali_device *dev, struct stage *st)
{
   ralloc_free(st->nir);
   st->nir = NULL;
   MALI_PER_ARCH(shader_unref)(dev, st->shader);
   st->shader = NULL;
}

/* VkPipelineCreationFeedbackCreateInfo, if the application chained one. */
static void
write_feedback(const void *pNext, const struct stage *stages, uint32_t stage_count,
               bool app_cache, int64_t duration_ns)
{
   const VkPipelineCreationFeedbackCreateInfo *fb =
      vk_find_struct_const(pNext, PIPELINE_CREATION_FEEDBACK_CREATE_INFO);
   if (!fb)
      return;

   bool all_hit = stage_count > 0;
   for (uint32_t i = 0; i < stage_count; i++)
      all_hit &= stages[i].cache_hit;

   if (fb->pPipelineCreationFeedback) {
      fb->pPipelineCreationFeedback->flags =
         VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT |
         (all_hit && app_cache ?
             VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT : 0);
      fb->pPipelineCreationFeedback->duration = duration_ns;
   }

   for (uint32_t i = 0; i < fb->pipelineStageCreationFeedbackCount && i < stage_count; i++) {
      fb->pPipelineStageCreationFeedbacks[i].flags =
         VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT |
         (stages[i].cache_hit && app_cache ?
             VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT : 0);
      fb->pPipelineStageCreationFeedbacks[i].duration = stages[i].duration_ns;
   }
}

/* ---------------------------------------------------------------------- */
/* Graphics                                                                */

/*
 * Dynamic states that feed none of the baked words: the draw encoder
 * handles them itself (viewport and scissor registers, blend constants in
 * the blend descriptor and FAU, line width, vertex buffers).
 */
static bool
baked_words_use_dynamic_state(const BITSET_WORD *dynamic)
{
   BITSET_DECLARE(d, MESA_VK_DYNAMIC_GRAPHICS_STATE_ENUM_MAX);
   BITSET_COPY(d, dynamic);

   static const enum mesa_vk_dynamic_graphics_state independent[] = {
      MESA_VK_DYNAMIC_VI,
      MESA_VK_DYNAMIC_VI_BINDINGS_VALID,
      MESA_VK_DYNAMIC_VI_BINDING_STRIDES,
      MESA_VK_DYNAMIC_VP_VIEWPORT_COUNT,
      MESA_VK_DYNAMIC_VP_VIEWPORTS,
      MESA_VK_DYNAMIC_VP_SCISSOR_COUNT,
      MESA_VK_DYNAMIC_VP_SCISSORS,
      MESA_VK_DYNAMIC_RS_LINE_WIDTH,
      MESA_VK_DYNAMIC_CB_BLEND_CONSTANTS,
      /* The runtime marks advanced blending dynamic when the feature is
       * off (always, here); nothing reads it. */
      MESA_VK_DYNAMIC_CB_BLEND_ADVANCED,
   };
   for (unsigned i = 0; i < ARRAY_SIZE(independent); i++)
      BITSET_CLEAR(d, independent[i]);

   return !BITSET_IS_EMPTY(d);
}

/* The graphics state the fragment lowering reads. */
static void
hash_fs_state(struct mesa_blake3 *ctx, const struct mali_device *dev,
              const struct vk_graphics_pipeline_state *state)
{
   bool sample_shading = state->ms && state->ms->sample_shading_enable;
   uint32_t view_mask = state->mv ? state->mv->view_mask : 0;
   /* Whether the fragment shader keeps its ATEST (mali_shader.c). */
   bool a2c = BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_MS_ALPHA_TO_COVERAGE_ENABLE) ||
              (state->ms && state->ms->alpha_to_coverage_enable);
   uint32_t roa = state->rasterization_order_access;
   /* Whether it keeps its second colour output. */
   bool dual_src = MALI_PER_ARCH(shader_fs_keeps_dual_source)(dev, state);
   _mesa_blake3_update(ctx, &sample_shading, sizeof(sample_shading));
   _mesa_blake3_update(ctx, &view_mask, sizeof(view_mask));
   _mesa_blake3_update(ctx, &a2c, sizeof(a2c));
   _mesa_blake3_update(ctx, &roa, sizeof(roa));
   _mesa_blake3_update(ctx, &dual_src, sizeof(dual_src));
   if (state->ial)
      _mesa_blake3_update(ctx, state->ial, sizeof(*state->ial));
   if (state->cal)
      _mesa_blake3_update(ctx, state->cal, sizeof(*state->cal));
#if PAN_ARCH < 9
   /* Which output locations feed an attachment (mali_shader.c
    * location_attachment). */
   if (state->rp)
      _mesa_blake3_update(ctx, &state->rp->color_attachment_count,
                          sizeof(state->rp->color_attachment_count));
#endif
}

/* Descriptor sets a stage's resource table reads: the sets it indexes and
 * the sets its dynamic buffer copies come from. */
static uint16_t
stage_sets(const struct mali_shader *s)
{
   uint32_t m = s->desc.used_set_mask;
   for (uint32_t i = 0; i < s->desc.dyn_bufs.count; i++)
      m |= BITFIELD_BIT(MALI_COPY_DESC_HANDLE_SET(s->desc.dyn_bufs.map[i]));
   return (uint16_t)m;
}

/* Everything mali_gfx_vs_srt (mali_cmd_state.h) reads from the pipeline side,
 * with unused entries zero. */
struct vs_srt_key {
   struct mali_shader_desc_info desc;
   uint32_t dynamic_vi, dynamic_strides;
   uint32_t attributes_valid, bindings_valid;
   struct {
      uint32_t binding, format, offset;
   } attr[MESA_VK_MAX_VERTEX_ATTRIBUTES];
   struct {
      uint32_t input_rate, divisor, stride;
   } binding[MESA_VK_MAX_VERTEX_BINDINGS];
};

/* Everything mali_gfx_fs_srt reads from the pipeline side. */
struct fs_srt_key {
   uint32_t has_fs;
   struct mali_shader_desc_info desc;
   struct pan_varying_layout vs_formats, fs_formats;
};

static void
copy_varying_layout(struct pan_varying_layout *dst, const struct pan_varying_layout *src)
{
   dst->count = src->count;
   dst->known = src->known;
   dst->generic_size_B = src->generic_size_B;
   for (unsigned i = 0; i < src->count && i < ARRAY_SIZE(dst->slots); i++)
      dst->slots[i] = src->slots[i];
}

/* The bind block (struct mali_gfx_bind): register words, the static
 * states the draw reads, table keys, and on v9 and v11 a GPU copy of the
 * Depth/stencil descriptor when nothing dynamic feeds it. */
static VkResult
fill_bind(struct mali_device *dev, struct mali_graphics_pipeline *p)
{
   struct mali_gfx_bind *bd = &p->bind;
   const struct mali_gfx_baked *bk = &p->baked;
   const BITSET_WORD *set = p->state.set;

   bd->spd[0] = bk->vs_pos_spd;
   bd->spd[1] = bk->vs_var_spd;
   bd->spd[2] = bk->fs_spd;
   bd->tiler_flags = bk->tiler_flags;
   bd->dcd0 = bk->dcd0[0];
   bd->dcd0_oq = bk->dcd0[1];
   bd->dcd1 = bk->dcd1;
   bd->dcd2 = bk->dcd2;
   bd->vary_size = p->fs ? p->vs->info.varyings.formats.generic_size_B : 0;
   bd->tls_size = bk->tls_size;
   bd->noperspective = p->fs ? p->fs->info.varyings.noperspective : 0;
   bd->dynamic = bk->uses_dynamic_state;
   bd->discard = !bk->uses_dynamic_state && p->state.rs.rasterizer_discard_enable;
   /* A dynamic topology keeps the point size (mali_cmd_draw.c reads
    * baked.prim only when it is static). */
   bd->lines = bk->prim != MESA_PRIM_COUNT && u_reduced_prim(bk->prim) == MESA_PRIM_LINES;
   bd->has_fs = p->fs != NULL;
   bd->vs_sets = stage_sets(p->vs);
   bd->vs_first_vertex =
      BITSET_TEST(p->vs->fau.used_sysvals,
                  offsetof(struct mali_graphics_sysvals, vs.first_vertex) / MALI_FAU_WORD_SIZE);
   bd->vs_base_instance =
      BITSET_TEST(p->vs->fau.used_sysvals,
                  offsetof(struct mali_graphics_sysvals, vs.base_instance) / MALI_FAU_WORD_SIZE);
   bd->fs_sets = p->fs ? stage_sets(p->fs) : 0;

   bd->static_mask = 0;
   if (BITSET_TEST(set, MESA_VK_DYNAMIC_VI))
      bd->static_mask |= MALI_GFX_STATIC_VI;
   if (BITSET_TEST(set, MESA_VK_DYNAMIC_VI_BINDING_STRIDES))
      bd->static_mask |= MALI_GFX_STATIC_STRIDES;
   if (BITSET_TEST(set, MESA_VK_DYNAMIC_VP_VIEWPORTS))
      bd->static_mask |= MALI_GFX_STATIC_VIEWPORTS;
   if (BITSET_TEST(set, MESA_VK_DYNAMIC_VP_SCISSORS))
      bd->static_mask |= MALI_GFX_STATIC_SCISSORS;
   if (BITSET_TEST(set, MESA_VK_DYNAMIC_CB_BLEND_CONSTANTS))
      bd->static_mask |= MALI_GFX_STATIC_BLEND_CONSTANTS;
   if (BITSET_TEST(set, MESA_VK_DYNAMIC_RS_LINE_WIDTH))
      bd->static_mask |= MALI_GFX_STATIC_LINE_WIDTH;
   if (BITSET_TEST(set, MESA_VK_DYNAMIC_INPUT_ATTACHMENT_MAP))
      bd->static_mask |= MALI_GFX_STATIC_IAL;

   bd->line_width = p->state.rs.line.width;
   bd->ial = p->state.ial;

   struct vs_srt_key vkey;
   memset(&vkey, 0, sizeof(vkey));
   vkey.desc = p->vs->desc;
   vkey.dynamic_vi = !(bd->static_mask & MALI_GFX_STATIC_VI);
   vkey.dynamic_strides = !(bd->static_mask & MALI_GFX_STATIC_STRIDES);
   if (!vkey.dynamic_vi) {
      vkey.attributes_valid = p->vi.attributes_valid;
      vkey.bindings_valid = p->vi.bindings_valid;
      u_foreach_bit(a, p->vi.attributes_valid) {
         vkey.attr[a].binding = p->vi.attributes[a].binding;
         vkey.attr[a].format = p->vi.attributes[a].format;
         vkey.attr[a].offset = p->vi.attributes[a].offset;
      }
      u_foreach_bit(b, p->vi.bindings_valid) {
         vkey.binding[b].input_rate = p->vi.bindings[b].input_rate;
         vkey.binding[b].divisor = p->vi.bindings[b].divisor;
      }
   }
   if (!vkey.dynamic_strides) {
      for (unsigned b = 0; b < MESA_VK_MAX_VERTEX_BINDINGS; b++)
         vkey.binding[b].stride = p->state.vi_binding_strides[b];
   }
   bd->vs_srt_key = mali_device_intern_key(dev, &vkey, sizeof(vkey));

   struct fs_srt_key fkey;
   memset(&fkey, 0, sizeof(fkey));
   if (p->fs) {
      fkey.has_fs = 1;
      fkey.desc = p->fs->desc;
      if (p->fs->desc.needs_varying_descs) {
         copy_varying_layout(&fkey.vs_formats, &p->vs->info.varyings.formats);
         copy_varying_layout(&fkey.fs_formats, &p->fs->info.varyings.formats);
      }
   }
   bd->fs_srt_key = mali_device_intern_key(dev, &fkey, sizeof(fkey));

   bd->zsd = 0;
#if PAN_ARCH >= 9
   if (!bk->uses_dynamic_state) {
      if (mali_bo_pool_alloc(&dev->desc_pool, sizeof(bk->zsd), 32, &p->zsd_mem) !=
          MALI_KBASE_SUCCESS)
         return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
      memcpy(p->zsd_mem.cpu, bk->zsd, sizeof(bk->zsd));
      bd->zsd = p->zsd_mem.gpu_va;
   }
#else
   /* v7 has no Depth/stencil descriptor: the draw copies baked.zsd into
    * its fragment Renderer State Descriptor (mali_jm_cmd_draw_v7.c
    * build_fs_rsd). */
#endif
   return VK_SUCCESS;
}

#if PAN_ARCH != 9
#if PAN_ARCH == 7
#define CLIP_FS_TAG "mali-v7-clip-fs"
#define CLIP_FS_NAME "mali_v7_clip_fs"
#else
#define CLIP_FS_TAG "mali-v11-clip-fs"
#define CLIP_FS_NAME "mali_v11_clip_fs"
#endif

/*
 * v7 and v11 clip in the fragment shader (mali_shader.c, "Clip
 * distances"), so a pipeline whose vertex shader writes clip distances and
 * that has no fragment shader gets one that only clips: an empty fragment
 * shader, to which the fragment compile adds the clip discard. Like no
 * fragment shader, it writes no colour (every colour target stays off,
 * mali_pipeline_state.c pack_blend); its discard makes the depth/stencil
 * update late, so a clipped fragment writes no depth or stencil.
 */
static bool
vs_writes_clip_distances(const struct mali_shader *vs)
{
   const struct pan_varying_layout *vl = &vs->info.varyings.formats;
   return pan_varying_layout_find_slot(vl, VARYING_SLOT_CLIP_DIST0) ||
          pan_varying_layout_find_slot(vl, VARYING_SLOT_CLIP_DIST1);
}

/* Its cache key covers what its compile reads: the vertex shader's
 * varying layout and the fragment state (hash_fs_state). */
static void
clip_fs_key(const struct mali_device *dev, const struct vk_graphics_pipeline_state *state,
            const struct mali_shader *vs, blake3_hash key)
{
   struct pan_varying_layout vl;
   memset(&vl, 0, sizeof(vl));
   copy_varying_layout(&vl, &vs->info.varyings.formats);

   struct mesa_blake3 h;
   _mesa_blake3_init(&h);
   _mesa_blake3_update(&h, CLIP_FS_TAG, sizeof(CLIP_FS_TAG) - 1);
   _mesa_blake3_update(&h, &vl, sizeof(vl));
   hash_fs_state(&h, dev, state);
   _mesa_blake3_final(&h, key);
}

static VkResult
clip_fs_compile(struct mali_device *dev, struct vk_pipeline_cache *cache,
                const struct vk_graphics_pipeline_state *state, const struct mali_shader *vs,
                struct stage *st)
{
   const int64_t start = os_time_get_nano();
   nir_builder b = nir_builder_init_simple_shader(
      MESA_SHADER_FRAGMENT, MALI_PER_ARCH(shader_nir_options)(dev, MESA_SHADER_FRAGMENT),
      CLIP_FS_NAME);
   MALI_PER_ARCH(shader_preprocess)(dev, b.shader);

   /* The shader reads no buffer or image. */
   const struct vk_pipeline_robustness_state rs = {
      .storage_buffers = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DISABLED_EXT,
      .uniform_buffers = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DISABLED_EXT,
      .vertex_inputs = VK_PIPELINE_ROBUSTNESS_BUFFER_BEHAVIOR_DISABLED_EXT,
      .images = VK_PIPELINE_ROBUSTNESS_IMAGE_BEHAVIOR_DISABLED_EXT,
   };
   const struct mali_shader_compile_info info = {
      .nir = b.shader,
      .rs = &rs,
      .state = state,
      .vs_varying_layout = &vs->info.varyings.formats,
   };

   struct mali_shader *shader;
   VkResult result = MALI_PER_ARCH(shader_compile)(dev, &info, st->key, &shader);
   if (result != VK_SUCCESS)
      return result;
   p_atomic_inc(&dev->pipeline_stats.shaders_compiled);

   if (cache) {
      struct vk_pipeline_cache_object *obj =
         vk_pipeline_cache_add_object(cache, &shader->base);
      shader = container_of(obj, struct mali_shader, base);
   }
   st->shader = shader;
   st->duration_ns = os_time_get_nano() - start;
   return VK_SUCCESS;
}
#endif /* PAN_ARCH != 9 */

static VkResult
poly_compile(struct mali_device *dev, nir_shader *nir, struct stage *stage,
             const struct vk_pipeline_layout *layout,
             const struct vk_graphics_pipeline_state *state, bool vertex_inputs,
             const char *label, struct mali_shader **out)
{
   const unsigned root = MALI_SYSVALS_PUSH_CONST_BASE +
      offsetof(struct mali_graphics_sysvals, common.poly_params);
   mali_poly_lower_params(nir, MALI_POLY_INDIRECT_ROOT | root);
   MALI_PER_ARCH(shader_preprocess)(dev, nir);
   struct mesa_blake3 h;
   blake3_hash key;
   _mesa_blake3_init(&h);
   _mesa_blake3_update(&h, label, strlen(label));
   _mesa_blake3_update(&h, stage->stage_hash, sizeof(stage->stage_hash));
   hash_layout(&h, layout);
   _mesa_blake3_final(&h, key);
   const struct mali_shader_compile_info info = {
      .nir = nir, .rs = &stage->rs, .layout = layout, .state = state,
      .poly_vertex_inputs = vertex_inputs,
   };
   return MALI_PER_ARCH(shader_compile)(dev, &info, key, out);
}

static VkResult
poly_compile_stage(struct mali_device *dev, struct stage *stage,
                   struct vk_pipeline_layout *layout, const struct vk_graphics_pipeline_state *state,
                   bool inputs, const char *key, struct mali_shader **shader)
{
   nir_shader *nir = stage->nir;
   stage->nir = NULL;
   return poly_compile(dev, nir, stage, layout, state, inputs, key, shader);
}

static VkResult
compile_tess_pipeline(struct mali_device *dev, VkPipelineCreateFlags2KHR flags,
                      struct vk_pipeline_layout *layout,
                      const struct vk_graphics_pipeline_state *state, struct stage *vs,
                      struct stage *tcs, struct stage *tes, bool with_gs,
                      struct mali_tess_pipeline *tess)
{
   if (!state->ts || !state->ts->patch_control_points ||
       state->ts->patch_control_points > 32 || (state->mv && state->mv->view_mask) ||
       BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_TS_PATCH_CONTROL_POINTS) ||
       BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_TS_DOMAIN_ORIGIN) ||
       BITSET_TEST(state->dynamic, MESA_VK_DYNAMIC_IA_PRIMITIVE_TOPOLOGY) ||
       !state->ia || state->ia->primitive_topology != VK_PRIMITIVE_TOPOLOGY_PATCH_LIST)
      return vk_errorf(dev, VK_ERROR_FEATURE_NOT_PRESENT,
                       "tessellation lab requires a static patch topology and one view");

   struct stage *stages[] = {vs, tcs, tes};
   for (unsigned i = 0; i < ARRAY_SIZE(stages); ++i) {
      VkResult result = stage_to_nir(dev, flags, stages[i]);
      if (result != VK_SUCCESS)
         return result;
      nir_shader_gather_info(stages[i]->nir, nir_shader_get_entrypoint(stages[i]->nir));
   }

   /* Vulkan allows the modes in either stage. The generator reads TES
    * modes and the TCS lowering also needs the domain and output count. */
   struct shader_info *eval = &tes->nir->info, *control = &tcs->nir->info;
   eval->tess.tcs_vertices_out |= control->tess.tcs_vertices_out;
   eval->tess._primitive_mode |= control->tess._primitive_mode;
   eval->tess.spacing |= control->tess.spacing;
   eval->tess.ccw |= control->tess.ccw;
   eval->tess.point_mode |= control->tess.point_mode;
   control->tess.tcs_vertices_out = eval->tess.tcs_vertices_out;
   control->tess._primitive_mode = eval->tess._primitive_mode;
   if (eval->tess._primitive_mode == TESS_PRIMITIVE_UNSPECIFIED ||
       !control->tess.tcs_vertices_out || control->tess.tcs_vertices_out > 32)
      return vk_error(dev, VK_ERROR_INVALID_SHADER_NV);

   *tess = (struct mali_tess_pipeline) {
      .vertex_outputs = vs->nir->info.outputs_written,
      .control_outputs = poly_tcs_per_vertex_outputs(tcs->nir),
      .input_points = state->ts->patch_control_points,
      .output_points = control->tess.tcs_vertices_out,
      .patch_constants = util_last_bit(control->patch_outputs_written),
      .stride_el = poly_tcs_output_stride(tcs->nir) / 4,
      .domain = eval->tess._primitive_mode,
      .partitioning = eval->tess.spacing == TESS_SPACING_FRACTIONAL_ODD ?
         POLY_TESS_PARTITIONING_FRACTIONAL_ODD :
         eval->tess.spacing == TESS_SPACING_FRACTIONAL_EVEN ?
         POLY_TESS_PARTITIONING_FRACTIONAL_EVEN : POLY_TESS_PARTITIONING_INTEGER,
      .point_mode = eval->tess.point_mode,
      .ccw = eval->tess.ccw,
      .lower_left = state->ts->domain_origin == VK_TESSELLATION_DOMAIN_ORIGIN_LOWER_LEFT,
   };
   tess->raster_mode = tess->point_mode ? MESA_PRIM_POINTS :
      tess->domain == TESS_PRIMITIVE_ISOLINES ? MESA_PRIM_LINES : MESA_PRIM_TRIANGLES;
   if (!tess->stride_el || !mali_poly_lower_vertex(vs->nir) ||
       !mali_poly_lower_tcs(tcs->nir) ||
       !(with_gs ? mali_poly_lower_tes_compute(tes->nir, &tess->eval_outputs) :
                   mali_poly_lower_tes_raster(tes->nir)))
      return vk_error(dev, VK_ERROR_INVALID_SHADER_NV);

   VkResult result = poly_compile_stage(dev, vs, layout, NULL, true,
                                        "poly-tess-vertex-v1", &tess->vertex);
   if (result == VK_SUCCESS)
      result = poly_compile_stage(dev, tcs, layout, NULL, false,
                                  "poly-tess-control-v1", &tess->control);
   if (result == VK_SUCCESS) {
      if (with_gs)
         result = poly_compile_stage(dev, tes, layout, NULL, false,
                                     "poly-tess-eval-gs-v1", &tess->eval);
      else
         result = poly_compile_stage(dev, tes, layout, state, false,
                                     "poly-tess-raster-v1", &vs->shader);
   }
   if (result != VK_SUCCESS)
      return result;

   const nir_shader_compiler_options *opts =
      MALI_PER_ARCH(shader_nir_options)(dev, MESA_SHADER_COMPUTE);
   nir_shader *nir = mali_poly_tess_indirect(opts);
   result = poly_compile(dev, nir, tcs, layout, NULL, false, "poly-tess-indirect-v1", &tess->indirect);
   if (result != VK_SUCCESS)
      return result;
   nir = mali_poly_tessellator(opts, tess->domain, POLY_TESS_MODE_COUNT,
                                          &tess->scratch_stride);
   if (!nir)
      return vk_error(dev, VK_ERROR_INVALID_SHADER_NV);
   result = poly_compile(dev, nir, tcs, layout, NULL, false, "poly-tess-count-v1", &tess->count);
   if (result != VK_SUCCESS)
      return result;
   nir = mali_poly_tess_prefix(opts);
   result = poly_compile(dev, nir, tcs, layout, NULL, false, "poly-tess-prefix-v1", &tess->prefix);
   if (result != VK_SUCCESS)
      return result;
   nir = mali_poly_tessellator(opts, tess->domain, POLY_TESS_MODE_WITH_COUNTS,
                              &tess->scratch_stride);
   if (!nir)
      return vk_error(dev, VK_ERROR_INVALID_SHADER_NV);
   return poly_compile(dev, nir, tcs, layout, NULL, false,
                       "poly-tess-generate-v1", &tess->generate);
}

static VkResult
create_graphics_pipeline(struct mali_device *dev, VkPipelineCache cache_handle,
                         const VkGraphicsPipelineCreateInfo *ci,
                         const VkAllocationCallbacks *alloc, VkPipeline *out)
{
   const int64_t start = os_time_get_nano();
   const VkPipelineCreateFlags2KHR flags = vk_graphics_pipeline_create_flags(ci);
   VK_FROM_HANDLE(vk_pipeline_layout, layout, ci->layout);
   struct vk_pipeline_cache *cache = pick_cache(dev, cache_handle);
   struct stage stages[5];
   struct stage *vs = NULL, *fs = NULL, *gs = NULL;
   struct stage *tcs = NULL, *tes = NULL;
   struct mali_tess_pipeline tess = {0};
   struct mali_shader *poly_vertex = NULL, *poly_main = NULL;
   struct mali_shader *poly_count = NULL, *poly_pre = NULL;
   struct poly_gs_info gs_info = {0};
   uint64_t poly_outputs = 0;
#if PAN_ARCH != 9
   /* The fragment shader that only clips (clip_fs_compile), if needed. */
   struct stage clip = {0};
#endif
   uint32_t stage_count = 0;
   VkResult result;

   *out = VK_NULL_HANDLE;

   if (flags & VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR)
      return vk_errorf(dev, VK_ERROR_FEATURE_NOT_PRESENT,
                       "graphics pipeline libraries are not supported");

   for (uint32_t i = 0; i < ci->stageCount; i++) {
      const VkPipelineShaderStageCreateInfo *si = &ci->pStages[i];
      if (vk_pipeline_shader_stage_is_null(si))
         continue;
      switch (si->stage) {
      case VK_SHADER_STAGE_VERTEX_BIT:
         vs = &stages[stage_count++];
         stage_init(dev, flags, ci->pNext, si, vs);
         break;
      case VK_SHADER_STAGE_FRAGMENT_BIT:
         fs = &stages[stage_count++];
         stage_init(dev, flags, ci->pNext, si, fs);
         break;
      case VK_SHADER_STAGE_GEOMETRY_BIT:
         if (PAN_ARCH != 11 || !dev->vk.enabled_features.geometryShader)
            return vk_errorf(dev, VK_ERROR_FEATURE_NOT_PRESENT,
                             "geometry shader feature was not enabled");
         gs = &stages[stage_count++];
         stage_init(dev, flags, ci->pNext, si, gs);
         break;
      case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT:
      case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: {
         if (PAN_ARCH != 11 || !dev->vk.enabled_features.tessellationShader)
            return vk_errorf(dev, VK_ERROR_FEATURE_NOT_PRESENT,
                             "tessellation shader feature was not enabled");
         struct stage **stage = si->stage == VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT ?
                                 &tcs : &tes;
         *stage = &stages[stage_count++];
         stage_init(dev, flags, ci->pNext, si, *stage);
         break;
      }
      default:
         return vk_errorf(dev, VK_ERROR_FEATURE_NOT_PRESENT,
                          "shader stage 0x%x is not supported", si->stage);
      }
   }
   if (!vs)
      return vk_errorf(dev, VK_ERROR_FEATURE_NOT_PRESENT,
                       "graphics pipeline without a vertex shader");
   if (!!tcs != !!tes)
      return vk_errorf(dev, VK_ERROR_FEATURE_NOT_PRESENT,
                       "tessellation lab requires paired TCS and TES");

   struct vk_graphics_pipeline_all_state all;
   struct vk_graphics_pipeline_state state = {0};
   result = vk_graphics_pipeline_state_fill(&dev->vk, &state, ci, NULL, NULL, 0, &all,
                                            NULL, 0, NULL);
   if (result != VK_SUCCESS)
      return result;

   const uint32_t view_mask = state.mv ? state.mv->view_mask : 0;
   if (tes) {
      result = compile_tess_pipeline(dev, flags, layout, &state, vs, tcs, tes, gs != NULL, &tess);
      if (result != VK_SUCCESS)
         goto out;
   } else if (!gs) {
      /* Fragment key does not depend on the pruned vertex output mask.
       * Look it up first: its saved preprocessed mask lets a warm request build
       * the exact same vertex key without translating SPIR-V again. */
      struct mesa_blake3 h;
      if (fs) {
         _mesa_blake3_init(&h);
         _mesa_blake3_update(&h, "fs", 2);
         _mesa_blake3_update(&h, fs->stage_hash, sizeof(fs->stage_hash));
         hash_layout(&h, layout);
         _mesa_blake3_update(&h, vs->stage_hash, sizeof(vs->stage_hash));
         hash_fs_state(&h, dev, &state);
         _mesa_blake3_final(&h, fs->key);
         stage_lookup(dev, cache, fs);

         uint64_t reads;
         if (fs->shader) {
            reads = fs->shader->fs.preprocessed_inputs_read;
         } else {
            if (flags & VK_PIPELINE_CREATE_2_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_KHR) {
               result = VK_PIPELINE_COMPILE_REQUIRED;
               goto out;
            }
            result = stage_to_nir(dev, flags, fs);
            if (result != VK_SUCCESS)
               goto out;
            reads = fs_input_mask(fs->nir);
         }
         vs->keep_outputs = reads ? reads : ~0ull;
      } else {
         vs->keep_outputs = 0;
      }

      _mesa_blake3_init(&h);
      _mesa_blake3_update(&h, "vs", 2);
      _mesa_blake3_update(&h, vs->stage_hash, sizeof(vs->stage_hash));
      hash_layout(&h, layout);
      _mesa_blake3_update(&h, &view_mask, sizeof(view_mask));
      _mesa_blake3_update(&h, &vs->keep_outputs, sizeof(vs->keep_outputs));
      _mesa_blake3_final(&h, vs->key);
      stage_lookup(dev, cache, vs);

      if ((flags & VK_PIPELINE_CREATE_2_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_KHR) &&
          !vs->shader) {
         result = VK_PIPELINE_COMPILE_REQUIRED;
         goto out;
      }
   }
   if (gs) {
      if (flags & VK_PIPELINE_CREATE_2_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_KHR) {
         result = VK_PIPELINE_COMPILE_REQUIRED;
         goto out;
      }
      /* Keep this path out of the persistent stage cache: the raster VS
       * depends on the whole shader chain, not only the original VS key. */
      if (!tes) {
         result = stage_to_nir(dev, flags, vs);
         if (result != VK_SUCCESS) goto out;
         nir_shader_gather_info(vs->nir, nir_shader_get_entrypoint(vs->nir));
         poly_outputs = vs->nir->info.outputs_written;
         if (!mali_poly_lower_vertex(vs->nir)) {
            result = VK_ERROR_INVALID_SHADER_NV;goto out;
         }
         result = poly_compile_stage(dev, vs, layout, NULL, true, "poly-sw-vs-v1", &poly_vertex);
         if (result != VK_SUCCESS) goto out;
      } else {
         poly_outputs = tess.eval_outputs;
      }
      result = stage_to_nir(dev, flags, gs);
      if (result != VK_SUCCESS) goto out;
      if (tes)
         gs->nir->info.prev_stage = MESA_SHADER_TESS_EVAL;
      struct mali_poly_geometry lower;
      nir_shader *input = gs->nir;gs->nir = NULL;
      if (!mali_poly_lower_geometry(input, &lower)) {
         mali_poly_geometry_finish(&lower);
         result = VK_ERROR_INVALID_SHADER_NV;
         goto out;
      }
      gs_info = lower.info;
      if (gs_info.xfb || gs_info.multistream || gs_info.prefix_sum) {
         mali_poly_geometry_finish(&lower);
         result = VK_ERROR_FEATURE_NOT_PRESENT;goto out;
      }
#define POLY_COMPILE(field, target, name, state_) do { \
   if (lower.field) { \
      nir_shader *nir = lower.field;lower.field = NULL; \
      result = poly_compile(dev, nir, gs, layout, state_, false, name, target); \
      if (result != VK_SUCCESS) {mali_poly_geometry_finish(&lower);goto out;} \
   } \
} while (0)
      POLY_COMPILE(main, &poly_main, "poly-gs-main-v1", NULL);
      POLY_COMPILE(count, &poly_count, "poly-gs-count-v1", NULL);
      POLY_COMPILE(pre, &poly_pre, "poly-gs-pre-v1", NULL);
      POLY_COMPILE(raster, &vs->shader, "poly-gs-raster-v1", &state);
#undef POLY_COMPILE
      mali_poly_geometry_finish(&lower);
      if (tes) {
         const nir_shader_compiler_options *opts =
            MALI_PER_ARCH(shader_nir_options)(dev, MESA_SHADER_COMPUTE);
         const unsigned arity = tess.point_mode ? 1 : tess.domain == TESS_PRIMITIVE_ISOLINES ? 2 : 3;
         nir_shader *setup = mali_poly_tess_gs_setup(opts, &gs_info, arity);
         result = poly_compile(dev, setup, gs, layout, NULL, false, "poly-tess-gs-setup-v1", &tess.gs_setup);
         if (result != VK_SUCCESS) goto out;
      }
   }
   if (!vs->shader) {
      result = stage_compile(dev, cache, flags, vs, layout, &state, NULL);
      if (result != VK_SUCCESS)
         goto out;
   }
   if (fs && !fs->shader) {
      result = stage_compile(dev, (gs || tes) ? NULL : cache, flags, fs, layout, &state,
                             &vs->shader->info.varyings.formats);
      if (result != VK_SUCCESS)
         goto out;
   }
#if PAN_ARCH != 9
   if (!fs && vs_writes_clip_distances(vs->shader)) {
      clip_fs_key(dev, &state, vs->shader, clip.key);
      if (!stage_lookup(dev, cache, &clip)) {
         if (flags & VK_PIPELINE_CREATE_2_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_KHR) {
            result = VK_PIPELINE_COMPILE_REQUIRED;
            goto out;
         }
         result = clip_fs_compile(dev, cache, &state, vs->shader, &clip);
         if (result != VK_SUCCESS)
            goto out;
         /* Creation feedback counts it with the vertex stage. */
         vs->cache_hit = false;
         vs->duration_ns += clip.duration_ns;
      }
   }
#endif

   struct mali_graphics_pipeline *p =
      vk_pipeline_zalloc(&dev->vk, &graphics_pipeline_ops,
                         VK_PIPELINE_BIND_POINT_GRAPHICS, flags, alloc, sizeof(*p));
   if (!p) {
      result = vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto out;
   }
   p->base.vk.stages = state.shader_stages;
   p->geometry.vertex = poly_vertex;
   p->geometry.main = poly_main;
   p->geometry.count = poly_count;
   p->geometry.pre = poly_pre;
   p->geometry.gs = gs_info;
   p->geometry.vertex_outputs = poly_outputs;
   poly_vertex = poly_main = poly_count = poly_pre = NULL;
   p->tess = tess;
   memset(&tess, 0, sizeof(tess));
   if (layout)
      p->base.layout = vk_pipeline_layout_ref(layout);
   p->vs = vs->shader;
   vs->shader = NULL;
   if (fs) {
      p->fs = fs->shader;
      fs->shader = NULL;
   }
#if PAN_ARCH != 9
   if (clip.shader) {
      p->fs = clip.shader;
      clip.shader = NULL;
   }
#endif

   /* The static state, and the words baked from it. */
   BITSET_COPY(p->dynamic, state.dynamic);
   vk_dynamic_graphics_state_init(&p->state);
   p->state.vi = &p->vi;
   p->state.ms.sample_locations = &p->sl;
   vk_dynamic_graphics_state_fill(&p->state, &state);
   if (state.rp)
      p->rp = *state.rp;
   p->sampled_feedback_loop = state.feedback_loop_not_input_only;

   p->baked.uses_dynamic_state = baked_words_use_dynamic_state(state.dynamic);
   const struct mali_gfx_pack_input pack = {
      .geometry = gs ? &p->geometry.gs : NULL,
      .tess = tes && !gs ? &p->tess : NULL,
      .dyn = &p->state,
      .rp = &p->rp,
      .vs = p->vs,
      .fs = p->fs,
      .view_mask = view_mask,
      .sampled_feedback_loop = p->sampled_feedback_loop,
   };
   MALI_PER_ARCH(gfx_pack_state)(&pack, &p->baked);
   MALI_PER_ARCH(blend_shaders_prepare)(dev, p);

   result = fill_bind(dev, p);
   if (result != VK_SUCCESS) {
      graphics_pipeline_destroy(&dev->vk, &p->base.vk, alloc);
      goto out;
   }

   *out = vk_pipeline_to_handle(&p->base.vk);
   result = VK_SUCCESS;

out:
   tess_pipeline_finish(dev, &tess);
   MALI_PER_ARCH(shader_unref)(dev, poly_vertex);
   MALI_PER_ARCH(shader_unref)(dev, poly_main);
   MALI_PER_ARCH(shader_unref)(dev, poly_count);
   MALI_PER_ARCH(shader_unref)(dev, poly_pre);
   write_feedback(ci->pNext, stages, stage_count, cache_handle != VK_NULL_HANDLE,
                  os_time_get_nano() - start);
   for (uint32_t i = 0; i < stage_count; i++)
      stage_finish(dev, &stages[i]);
#if PAN_ARCH != 9
   stage_finish(dev, &clip);
#endif
   return result;
}

/* ---------------------------------------------------------------------- */
/* Compute                                                                 */

static VkResult
create_compute_pipeline(struct mali_device *dev, VkPipelineCache cache_handle,
                        const VkComputePipelineCreateInfo *ci,
                        const VkAllocationCallbacks *alloc, VkPipeline *out)
{
   const int64_t start = os_time_get_nano();
   const VkPipelineCreateFlags2KHR flags = vk_compute_pipeline_create_flags(ci);
   VK_FROM_HANDLE(vk_pipeline_layout, layout, ci->layout);
   struct vk_pipeline_cache *cache = pick_cache(dev, cache_handle);
   struct stage cs;
   VkResult result;

   *out = VK_NULL_HANDLE;

   stage_init(dev, flags, ci->pNext, &ci->stage, &cs);

   struct mesa_blake3 h;
   _mesa_blake3_init(&h);
   _mesa_blake3_update(&h, "cs", 2);
   _mesa_blake3_update(&h, cs.stage_hash, sizeof(cs.stage_hash));
   hash_layout(&h, layout);
   _mesa_blake3_final(&h, cs.key);

   stage_lookup(dev, cache, &cs);

   if (!cs.shader) {
      if (flags & VK_PIPELINE_CREATE_2_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_KHR) {
         result = VK_PIPELINE_COMPILE_REQUIRED;
         goto out;
      }
      result = stage_compile(dev, cache, flags, &cs, layout, NULL, NULL);
      if (result != VK_SUCCESS)
         goto out;
   }

   struct mali_compute_pipeline *p =
      vk_pipeline_zalloc(&dev->vk, &compute_pipeline_ops,
                         VK_PIPELINE_BIND_POINT_COMPUTE, flags, alloc, sizeof(*p));
   if (!p) {
      result = vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto out;
   }
   p->base.vk.stages = VK_SHADER_STAGE_COMPUTE_BIT;
   if (layout)
      p->base.layout = vk_pipeline_layout_ref(layout);
   p->cs = cs.shader;
   cs.shader = NULL;

   *out = vk_pipeline_to_handle(&p->base.vk);
   result = VK_SUCCESS;

out:
   write_feedback(ci->pNext, &cs, 1, cache_handle != VK_NULL_HANDLE,
                  os_time_get_nano() - start);
   stage_finish(dev, &cs);
   return result;
}

/* ---------------------------------------------------------------------- */
/* Entry points                                                            */

/*
 * The multi-create rules of the spec: every handle is written, failures
 * are VK_NULL_HANDLE, the first error is returned, and
 * EARLY_RETURN_ON_FAILURE stops at the first failure.
 */
#define CREATE_PIPELINES(create_fn, infos, count, flags_fn)                    \
   VkResult result = VK_SUCCESS;                                               \
   uint32_t i = 0;                                                             \
   for (; i < (count); i++) {                                                  \
      VkResult r = create_fn(dev, pipelineCache, &(infos)[i], pAllocator,      \
                             &pPipelines[i]);                                  \
      if (r == VK_SUCCESS)                                                     \
         continue;                                                             \
      if (result == VK_SUCCESS)                                                \
         result = r;                                                           \
      if (flags_fn(&(infos)[i]) &                                              \
          VK_PIPELINE_CREATE_2_EARLY_RETURN_ON_FAILURE_BIT_KHR) {              \
         i++;                                                                  \
         break;                                                                \
      }                                                                        \
   }                                                                           \
   for (; i < (count); i++)                                                    \
      pPipelines[i] = VK_NULL_HANDLE;                                          \
   return result;

VKAPI_ATTR VkResult VKAPI_CALL
MALI_PER_ARCH(CreateGraphicsPipelines)(VkDevice _device, VkPipelineCache pipelineCache,
                                       uint32_t createInfoCount,
                                       const VkGraphicsPipelineCreateInfo *pCreateInfos,
                                       const VkAllocationCallbacks *pAllocator,
                                       VkPipeline *pPipelines)
{
   VK_FROM_HANDLE(mali_device, dev, _device);
   CREATE_PIPELINES(create_graphics_pipeline, pCreateInfos, createInfoCount,
                    vk_graphics_pipeline_create_flags)
}

VKAPI_ATTR VkResult VKAPI_CALL
MALI_PER_ARCH(CreateComputePipelines)(VkDevice _device, VkPipelineCache pipelineCache,
                                      uint32_t createInfoCount,
                                      const VkComputePipelineCreateInfo *pCreateInfos,
                                      const VkAllocationCallbacks *pAllocator,
                                      VkPipeline *pPipelines)
{
   VK_FROM_HANDLE(mali_device, dev, _device);
   CREATE_PIPELINES(create_compute_pipeline, pCreateInfos, createInfoCount,
                    vk_compute_pipeline_create_flags)
}
