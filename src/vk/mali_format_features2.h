/* SPDX-License-Identifier: MIT */
#ifndef MALI_FORMAT_FEATURES2_H
#define MALI_FORMAT_FEATURES2_H

#include <stdbool.h>
#include <vulkan/vulkan_core.h>

/* Existing per-arch tables describe the actual descriptor conversions.
 * Formatless storage access uses that descriptor's format, as in PanVK.
 * R64 image access is the exception: it needs typed 64-bit operations.
 * The legacy response is deliberately kept separate from these extra bits.
 */
static inline VkFormatFeatureFlags2
mali_format_features2(VkFormat format, VkFormatFeatureFlags legacy, bool buffer)
{
   VkFormatFeatureFlags2 features = legacy;
   bool int64 = format >= VK_FORMAT_R64_UINT &&
                format <= VK_FORMAT_R64G64B64A64_SFLOAT;
   VkFormatFeatureFlags storage = buffer ? VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT :
                                          VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
   if ((legacy & storage) && !int64)
      features |= VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT |
                  VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;

   if (!buffer && (legacy & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
      switch (format) {
      case VK_FORMAT_D16_UNORM:
      case VK_FORMAT_X8_D24_UNORM_PACK32:
      case VK_FORMAT_D32_SFLOAT:
      case VK_FORMAT_D16_UNORM_S8_UINT:
      case VK_FORMAT_D24_UNORM_S8_UINT:
      case VK_FORMAT_D32_SFLOAT_S8_UINT:
         features |= VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_DEPTH_COMPARISON_BIT;
         break;
      default:
         break;
      }
   }
   return features;
}

#endif
