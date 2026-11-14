/* SPDX-License-Identifier: MIT */
#ifndef MALI_ANDROID_RGB_EXTERNAL_H
#define MALI_ANDROID_RGB_EXTERNAL_H
#include <stdbool.h>
#include <stdint.h>
#include "vulkan/vulkan_core.h"
/* Stable implementation-owned token, independent of AHB addresses/lifetime. */
#define MALI_ANDROID_RGB_TOKEN_TAG UINT64_C(0x554e520000000000)
static inline bool
mali_android_rgb_format_supported(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_R8G8B8A8_UNORM:
   case VK_FORMAT_B8G8R8A8_UNORM:
   case VK_FORMAT_R5G6B5_UNORM_PACK16:
   case VK_FORMAT_R16G16B16A16_SFLOAT:
   case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
   case VK_FORMAT_R8_UNORM:
      return true;
   default:
      return false;
   }
}
static inline uint64_t
mali_android_rgb_token(VkFormat format)
{
   return mali_android_rgb_format_supported(format) ?
      MALI_ANDROID_RGB_TOKEN_TAG | (uint32_t)format : 0;
}
static inline VkFormat
mali_android_rgb_token_format(uint64_t token, bool *swap)
{
   if (swap) *swap = false;
   VkFormat format = (VkFormat)(uint32_t)token;
   return (token & UINT64_C(0xffffffff00000000)) == MALI_ANDROID_RGB_TOKEN_TAG &&
      mali_android_rgb_format_supported(format) ? format : VK_FORMAT_UNDEFINED;
}
#endif
