/* SPDX-License-Identifier: MIT */
/* Pure validation of gralloc's standard Y/Cb/Cr metadata. No handle offsets,
 * mapping, CPU locks or GPU submission. Unknown/vendor layouts fail closed. */
#ifndef MALI_ANDROID_YUV_H
#define MALI_ANDROID_YUV_H
#include <stdbool.h>
#include <stdint.h>
#include "vulkan/vulkan.h"

struct mali_android_yuv_component { uint32_t type, offset_bits, size_bits; };
struct mali_android_yuv_plane {
   uint64_t offset, row_stride, size;
   uint32_t width, height, hsub, vsub, increment_bits, component_count;
   struct mali_android_yuv_component components[2];
};
struct mali_android_yuv_layout {
   VkFormat format;
   uint32_t plane_count;
   bool swap_chroma;
   struct mali_android_yuv_plane planes[3];
};

/* Private external-format identity describes the sampling layout, never an
 * address or an allocation lifetime. Bit 32 selects interleaved Cr,Cb. */
#define MALI_ANDROID_YUV_TOKEN_TAG UINT64_C(0x554e590000000000)
#define MALI_ANDROID_YUV_SWAP_BIT UINT64_C(0x0000000100000000)
static inline uint64_t
mali_android_yuv_token(const struct mali_android_yuv_layout *layout)
{
   return MALI_ANDROID_YUV_TOKEN_TAG | (uint32_t)layout->format |
      (layout->swap_chroma ? MALI_ANDROID_YUV_SWAP_BIT : 0);
}

static inline VkFormat
mali_android_yuv_token_format(uint64_t token, bool *swap)
{
   if ((token & ~UINT64_C(0x00000001ffffffff)) != MALI_ANDROID_YUV_TOKEN_TAG)
      return VK_FORMAT_UNDEFINED;
   VkFormat format = (VkFormat)(uint32_t)token;
   bool reversed = (token & MALI_ANDROID_YUV_SWAP_BIT) != 0;
   switch (format) {
   case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
   case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM:
      break;
   default: return VK_FORMAT_UNDEFINED;
   }
   if (reversed && format != VK_FORMAT_G8_B8R8_2PLANE_420_UNORM)
      return VK_FORMAT_UNDEFINED;
   if (swap) *swap = reversed;
   return format;
}

static inline bool
mali_android_yuv_layout_parse(uint32_t width, uint32_t height, uint64_t allocation_size,
                              unsigned count, const struct mali_android_yuv_plane *planes,
                              struct mali_android_yuv_layout *out)
{
   if (!planes || !out || !width || !height || (width & 1) || (height & 1) ||
       !allocation_size || (count != 2 && count != 3))
      return false;
   int y = -1, cb = -1, cr = -1;
   unsigned cb_offset = 0, cr_offset = 0;
   for (unsigned p = 0; p < count; p++) {
      const struct mali_android_yuv_plane *in = &planes[p];
      if (in->component_count < 1 || in->component_count > 2 ||
          (in->increment_bits != 8 && in->increment_bits != 16) ||
          in->offset % 64 || !in->row_stride || in->row_stride % 64 ||
          in->row_stride > UINT32_MAX || !in->size ||
          in->offset > allocation_size || in->size > allocation_size - in->offset)
         return false;
      bool luma = in->components[0].type == 1;
      unsigned sx = luma ? 1 : 2, sy = luma ? 1 : 2;
      uint64_t w = width / sx, h = height / sy;
      unsigned bytes = in->increment_bits / 8;
      if (in->hsub != sx || in->vsub != sy || in->width < w || in->height < h ||
          w * bytes > in->row_stride || h > in->size / in->row_stride)
         return false;
      unsigned mask = 0;
      for (unsigned c = 0; c < in->component_count; c++) {
         const struct mali_android_yuv_component *comp = &in->components[c];
         if (comp->size_bits != 8 || comp->offset_bits % 8 ||
             comp->offset_bits > in->increment_bits - 8 ||
             (mask & (1u << (comp->offset_bits / 8))))
            return false;
         mask |= 1u << (comp->offset_bits / 8);
         if (comp->type == 1 && y < 0 && in->component_count == 1 &&
             comp->offset_bits == 0 && in->increment_bits == 8)
            y = p;
         else if (comp->type == 2 && cb < 0 && !luma) {
            cb = p; cb_offset = comp->offset_bits;
         } else if (comp->type == 4 && cr < 0 && !luma) {
            cr = p; cr_offset = comp->offset_bits;
         } else return false;
      }
      /* Physical planes must not alias. NV12/NV21's two components occupy
       * one plane and are checked together above. */
      for (unsigned q = 0; q < p; q++)
         if (in->offset < planes[q].offset + planes[q].size &&
             planes[q].offset < in->offset + in->size)
            return false;
   }
   if (y < 0 || cb < 0 || cr < 0) return false;
   struct mali_android_yuv_layout result = {0};
   result.planes[0] = planes[y];
   if (count == 2) {
      if (cb != cr || cb == y || planes[cb].component_count != 2 ||
          planes[cb].increment_bits != 16 || cb_offset == cr_offset)
         return false;
      result.format = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
      result.plane_count = 2; result.swap_chroma = cb_offset != 0;
      result.planes[1] = planes[cb];
   } else {
      if (cb == cr || cb == y || cr == y || cb_offset || cr_offset ||
          planes[cb].component_count != 1 || planes[cr].component_count != 1 ||
          planes[cb].increment_bits != 8 || planes[cr].increment_bits != 8)
         return false;
      result.format = VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM;
      result.plane_count = 3;
      result.planes[1] = planes[cb]; result.planes[2] = planes[cr];
   }
   *out = result;
   return true;
}
#endif
