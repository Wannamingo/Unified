/*
 * Copyright 2024 Collabora Ltd.
 * Copyright 2024 Arm Ltd.
 * Copyright 2026 Unified contributors
 * SPDX-License-Identifier: MIT
 *
 * CSF incremental rendering, adapted from Mesa's
 * csf/panvk_vX_exception_handler.c. kbase supplies the exception on the
 * fragment CS of the *same group* as the tiler. No CPU retry of a failed
 * submit: that could duplicate storage/atomic shader side effects.
 */
#include "mali_cmd_state.h"
#include "mali_queue.h"
#include "mali_image.h"
#include "vk_log.h"

#define IR_HANDLER_BYTES 4096
#define IR_HANDLER_COUNT 16
#define IR_DUMP_OFFSET (IR_HANDLER_BYTES * IR_HANDLER_COUNT)
#define SPILL_OFF(f) offsetof(struct mali_cs_subqueue_ctx, spill.f)

/* Descriptors are multiples of 32 bytes. Registers 24..31 hold one cache
 * line half; src/dst are disposable copies. Flush the store before either
 * the registers or the scratch FBD are reused. */
static void
copy_fbd(struct cs_builder *b, struct cs_index src, struct cs_index dst,
         unsigned size)
{
   for (unsigned off = 0; off < size; off += 32) {
      cs_load_to(b, cs_reg_tuple(b, 24, 8), src, 0xff, off);
      cs_store(b, cs_reg_tuple(b, 24, 8), dst, 0xff, off);
      cs_flush_stores(b);
   }
}

static enum cs_reg_perm
handler_perm(struct cs_builder *b, unsigned reg)
{
   if (reg == MALI_FRAGMENT_SR_BBOX_MIN || reg == MALI_FRAGMENT_SR_BBOX_MAX ||
       reg == MALI_CS_REG_SUBQUEUE_CTX || reg == MALI_CS_REG_SUBQUEUE_CTX + 1)
      return CS_REG_RD;
   return CS_REG_RW;
}

static bool
build_handler(struct mali_device *dev, struct cs_buffer mem, bool zs, unsigned rts)
{
   const unsigned size = pan_size(FRAMEBUFFER) +
      (zs ? pan_size(ZS_CRC_EXTENSION) : 0) + rts * pan_size(RENDER_TARGET);
   struct mali_framebuffer_pointer_packed tag;
   pan_pack(&tag, FRAMEBUFFER_POINTER, cfg) {
      cfg.zs_crc_extension_present = zs;
      cfg.render_target_count = rts;
   }
   struct cs_builder b;
   const struct cs_builder_conf conf = {
      .nr_registers = dev->csf->nr_registers,
      .nr_kernel_registers = MALI_CS_KERNEL_REGS,
      .ls_sb_slot = MALI_SB_LS,
      .reg_perm = handler_perm,
   };
   cs_builder_init(&b, &conf, mem);
   struct cs_function fn;
   const struct cs_function_ctx fctx = {
      .ctx_reg = mali_cs_subqueue_ctx_reg(&b), .dump_addr_offset = offsetof(struct mali_cs_subqueue_ctx, ir_dump),
   };
   cs_function_def(&b, &fn, fctx) {
      struct cs_index ctx = mali_cs_subqueue_ctx_reg(&b);
      struct cs_index src = cs_reg64(&b, 4);
      struct cs_index scratch = cs_reg64(&b, 6), layers = cs_reg32(&b, 8);
      struct cs_index count = cs_reg32(&b, 9), mask = cs_reg32(&b, 10);
      struct cs_index active = cs_reg32(&b, 11), one = cs_reg32(&b, 12);
      struct cs_index all = cs_reg32(&b, 13);
      struct cs_index zero = cs_reg64(&b, 14), tiler = cs_reg64(&b, 16);
      struct cs_index tdcount = cs_reg32(&b, 18), total = cs_reg32(&b, 19);
      struct cs_index chunks = cs_reg_tuple(&b, 20, 4);

      cs_load64_to(&b, scratch, ctx, SPILL_OFF(scratch));
      cs_load32_to(&b, layers, ctx, SPILL_OFF(layers));
      cs_load32_to(&b, count, ctx, SPILL_OFF(counter));
      cs_load32_to(&b, mask, ctx, SPILL_OFF(view_mask));
      cs_move32_to(&b, all, 0);
      cs_if(&b, MALI_CS_CONDITION_EQUAL, mask) {
         cs_move32_to(&b, mask, UINT32_MAX);
         cs_move32_to(&b, all, 1);
      }
      cs_if(&b, MALI_CS_CONDITION_EQUAL, count) {
         cs_load64_to(&b, src, ctx, SPILL_OFF(first));
      } cs_else(&b) {
         cs_load64_to(&b, src, ctx, SPILL_OFF(middle));
      }
      cs_move32_to(&b, one, 0);
      cs_while(&b, MALI_CS_CONDITION_GREATER, layers) {
         cs_if(&b, MALI_CS_CONDITION_NEQUAL, all) {
            cs_move32_to(&b, active, 1);
         } cs_else(&b) {
            cs_move32_to(&b, active, 0);
            cs_bit_set32(&b, active, active, one);
            cs_and32(&b, active, mask, active);
         }
         cs_if(&b, MALI_CS_CONDITION_NEQUAL, active) {
            copy_fbd(&b, src, scratch, size);
            cs_add_imm64(&b, cs_sr_reg64(&b, FRAGMENT, FBD_POINTER),
                         scratch, tag.opaque[0]);
            cs_run_fragment(&b, false, MALI_TILE_RENDER_ORDER_Z_ORDER);
            cs_wait_slots(&b, dev->csf->sb.all_iters_mask);
         }
         cs_add_imm64(&b, src, src, size);
         cs_add_imm32(&b, one, one, 1);
         cs_add_imm32(&b, layers, layers, -1);
      }

      /* Give back only the chunks completed so far. Do not increment the
       * pass's fragment-completed counter: the final ordinary pass does. */
      cs_load64_to(&b, tiler, ctx, SPILL_OFF(tiler));
      cs_load32_to(&b, tdcount, ctx, SPILL_OFF(td_count));
      cs_move64_to(&b, zero, 0);
      cs_while(&b, MALI_CS_CONDITION_GREATER, tdcount) {
         cs_load_to(&b, chunks, tiler, 0xf, 40);
         cs_finish_fragment(&b, false, cs_reg64(&b, 20), cs_reg64(&b, 22), cs_now());
         cs_store64(&b, zero, tiler, 0);
         cs_store64(&b, zero, tiler, 40);
         cs_store64(&b, zero, tiler, 48);
         cs_flush_stores(&b);
         cs_add_imm64(&b, tiler, tiler, pan_size(TILER_CONTEXT));
         cs_add_imm32(&b, tdcount, tdcount, -1);
      }

      cs_add_imm32(&b, count, count, 1);
      cs_store32(&b, count, ctx, SPILL_OFF(counter));
      cs_load32_to(&b, total, ctx, offsetof(struct mali_cs_subqueue_ctx, ir_calls));
      cs_add_imm32(&b, total, total, 1);
      cs_store32(&b, total, ctx, offsetof(struct mali_cs_subqueue_ctx, ir_calls));
      cs_flush_stores(&b);
      cs_move32_to(&b, cs_reg32(&b, 0), 0);
      cs_flush_caches(&b, MALI_CS_FLUSH_MODE_NONE, MALI_CS_FLUSH_MODE_NONE,
                      MALI_CS_OTHER_FLUSH_MODE_INVALIDATE, cs_reg32(&b, 0),
                      cs_defer(0, MALI_SB_IMM_FLUSH));
      cs_wait_slot(&b, MALI_SB_IMM_FLUSH);
   }
   const bool valid = cs_is_valid(&b) && fn.length * 8 <= IR_HANDLER_BYTES &&
                      fn.dump_size <= 1024;
   cs_end(&b);
   cs_builder_fini(&b);
   return valid;
}

VkResult
mali_tiler_oom_init(struct mali_device *dev, struct mali_csf_queue *q)
{
   const struct mali_kbase_alloc_info ai = {
      .size = IR_DUMP_OFFSET + 4096,
      .flags = mali_kbase_mem_same_va_policy(MALI_KBASE_FLAGS_DEVICE_UNCACHED),
      .mem_class = MALI_KBASE_MEM_CLASS_INTERNAL,
   };
   if (mali_kbase_alloc(dev->kbase, &ai, &q->oom_handler) != MALI_KBASE_SUCCESS)
      return vk_error(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY);
   memset(q->oom_handler.cpu, 0, ai.size);
   for (unsigned zs = 0; zs < 2; zs++) {
      for (unsigned rt = 1; rt <= 8; rt++) {
         unsigned off = (zs * 8 + rt - 1) * IR_HANDLER_BYTES;
         struct cs_buffer mem = {
            .cpu = (void *)((uint8_t *)q->oom_handler.cpu + off),
            .gpu = q->oom_handler.gpu_va + off,
            .capacity = IR_HANDLER_BYTES / 8,
         };
         if (!build_handler(dev, mem, zs, rt))
            return vk_errorf(dev, VK_ERROR_INITIALIZATION_FAILED, "invalid tiler OOM handler");
      }
   }
   struct mali_cs_subqueue_ctx *ctx = (void *)((uint8_t *)q->sync_mem.cpu +
      MALI_QUEUE_CTX_OFFSET(MALI_SUBQUEUE_FRAGMENT));
   ctx->ir_dump = q->oom_handler.gpu_va + IR_DUMP_OFFSET;
   return VK_SUCCESS;
}

/* The template lives with the command buffer. Recording fills the spill
 * descriptors at EndRendering, when coverage and attachment state are
 * final. Execution sees complete values, even on repeated submits. */
bool
mali_tiler_ir_arm(struct mali_cmd_buffer *cmd, struct mali_render_state *r)
{
   const struct mali_ptr mem = mali_cmd_alloc(cmd, sizeof(*r->ir_arm), 16);
   if (!mem.cpu)
      return false;
   r->ir_arm = mem.cpu;
   memset(r->ir_arm, 0, sizeof(*r->ir_arm));

   struct cs_builder *frag = mali_cmd_cs(cmd, MALI_SUBQUEUE_FRAGMENT);
   cs_wait_slots(frag, cmd->dev->csf->sb.all_iters_mask);
   mali_cmd_frag_flush_pending(cmd);
   cs_move32_to(frag, cs_sr_reg32(frag, FRAGMENT, BBOX_MIN), r->miny << 16 | r->minx);
   cs_move32_to(frag, cs_sr_reg32(frag, FRAGMENT, BBOX_MAX), r->maxy << 16 | r->maxx);

   struct cs_index src = mali_cs_scratch_reg64(frag, 0);
   struct cs_index data = mali_cs_scratch_reg_tuple(frag, 2, 8);
   struct cs_index ctx = mali_cs_subqueue_ctx_reg(frag);
   cs_move64_to(frag, src, mem.gpu);
   STATIC_ASSERT(sizeof(struct mali_tiler_spill) == 48);
   cs_load_to(frag, data, src, BITFIELD_MASK(8), 0);
   cs_store(frag, data, ctx, BITFIELD_MASK(8), SPILL_OFF(first));
   cs_load_to(frag, data, src, BITFIELD_MASK(4), 32);
   cs_store(frag, data, ctx, BITFIELD_MASK(4), SPILL_OFF(layers));
   cs_load_to(frag, data, src, BITFIELD_MASK(3), offsetof(struct mali_tiler_arm, handler));
   cs_flush_stores(frag);
   cs_set_exception_handler(frag, MALI_CS_EXCEPTION_TYPE_TILER_OOM,
                            cs_extract64(frag, data, 0), cs_extract32(frag, data, 2));

   /* Every tiled pass participates, even when its attachment state leaves
    * handler=0. A later pass must never consume the previous pass's FBD. */
   /* The available firmware rejects SHARED_SB_INC in immediate form.
    * LS has drained before SET_EXCEPTION_HANDLER. Use the same deferred
    * form as the tiler-to-fragment signal. */
   mali_cs_shared_sb_inc(frag, MALI_SHARED_SB_IR_READY, cs_defer_indirect());
   mali_cs_shared_sb_dec(mali_cmd_cs(cmd, MALI_SUBQUEUE_VERTEX_TILER), MALI_SHARED_SB_IR_READY);
   return true;
}

static bool
spillable(const struct mali_fb_target *target)
{
   return !target->image ||
      (target->store && target->image->mem &&
       !(target->image->vk.usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT));
}

bool
mali_tiler_ir_eligible(const struct mali_render_state *r)
{
   if (r->desc.blit || !spillable(&r->desc.z) || !spillable(&r->desc.s))
      return false;
   for (unsigned i = 0; i < r->desc.rt_count; ++i)
      if (!spillable(&r->desc.rt[i]))
         return false;
   return true;
}

static void
spill_reload(struct mali_fb_target *target)
{
   if (!target->image)
      return;
   target->load = MALI_ATT_LOAD_LOAD;
   target->preload = true;
}

bool
mali_tiler_ir_prepare(struct mali_cmd_buffer *cmd, struct mali_render_state *r,
                      uint32_t stride)
{
   /* The first spill keeps the original load/clear. Both subsequent
    * spills and the final pass reload the saved tiles. Eligibility
    * guarantees every present attachment already has store enabled. */
   uint32_t first_size, reload_size;
   struct mali_render_state first = *r, reload = *r;
   const uint64_t first_fbd = MALI_PER_ARCH(fb_build)(cmd, &first, &first_size);
   for (unsigned i = 0; i < reload.desc.rt_count; ++i)
      spill_reload(&reload.desc.rt[i]);
   spill_reload(&reload.desc.z);
   spill_reload(&reload.desc.s);
   const uint64_t reload_fbd = MALI_PER_ARCH(fb_build)(cmd, &reload, &reload_size);
   const struct mali_ptr scratch = mali_cmd_alloc(cmd, stride, 64);
   if (!first_fbd || !reload_fbd || !scratch.cpu || first_size != stride ||
       reload_size != stride)
      return false;

   const unsigned variant = ((r->desc.z.image || r->desc.s.image) ? 8 : 0) + r->rt_count - 1;
   *r->ir_arm = (struct mali_tiler_arm){
      .spill = {
         .first = first_fbd & ~UINT64_C(63),
         .middle = reload_fbd & ~UINT64_C(63),
         .scratch = scratch.gpu,
         .tiler = r->tiler,
         .layers = MAX2(r->desc.layer_count, 1),
         .view_mask = r->desc.view_mask,
         .td_count = r->td_count,
      },
      .handler = cmd->dev->csf->queue->oom_handler.gpu_va + variant * IR_HANDLER_BYTES,
      .handler_length = IR_HANDLER_BYTES,
   };
   r->ir_last = reload_fbd;
   return true;
}

void
mali_tiler_ir_finish(struct mali_cmd_buffer *cmd, const struct mali_render_state *r,
                     uint64_t regular)
{
   struct cs_builder *b = mali_cmd_cs(cmd, MALI_SUBQUEUE_FRAGMENT);
   struct cs_index addr = mali_cs_scratch_reg64(b, 0);
   struct cs_index len = mali_cs_scratch_reg32(b, 2);
   cs_move64_to(b, addr, 0);
   cs_move32_to(b, len, 0);
   cs_set_exception_handler(b, MALI_CS_EXCEPTION_TYPE_TILER_OOM, addr, len);
   if (r->ir_descriptors && cmd->dev->csf->tiler_ir_force) {
      cs_move64_to(b, addr, r->ir_arm->handler);
      cs_move32_to(b, len, r->ir_arm->handler_length);
      cs_call(b, addr, len);
   }

   const struct cs_index fbd = cs_sr_reg64(b, FRAGMENT, FBD_POINTER);
   cs_move64_to(b, fbd, regular);
   if (r->ir_descriptors) {
      struct cs_index counter = mali_cs_scratch_reg32(b, 0);
      cs_load32_to(b, counter, mali_cs_subqueue_ctx_reg(b), SPILL_OFF(counter));
      cs_if(b, MALI_CS_CONDITION_NEQUAL, counter) {
         cs_move64_to(b, fbd, r->ir_last);
      }
   }
}

