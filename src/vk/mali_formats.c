/*
 * SPDX-FileCopyrightText: 2026 ARMSX2 and bmdhacks
 * SPDX-License-Identifier: MIT
 */

/*
 * Format support: vkGetPhysicalDeviceFormatProperties(2) and
 * vkGetPhysicalDeviceImageFormatProperties(2).
 *
 * The answers come from Mesa's format table for the device's architecture
 * (pan_format.c, vendored; the per-arch half is mali_format_table.c) and
 * the GPU's compressed-format bits (TEXTURE_FEATURES_0), derived the way
 * panvk does (panvk_physical_device.c), because the descriptors that use
 * the formats are packed from the same table. Where the blob's answer
 * differs and the blob's is the safer one we follow it: integer formats
 * get no blend or linear-filter bit, and combined depth/stencil formats
 * have no linear-tiling features. YCbCr formats, sparse images and protected
 * images are not supported. External memory: on Android only the Android
 * loader's question about swapchain buffers (the hardware-buffer handle
 * type) is answered; elsewhere dma-bufs can be imported, as linear images
 * (DRM_FORMAT_MOD_LINEAR, the one modifier offered) or as buffers.
 */

#include "mali_vk.h"
#include "mali_image.h"
#include "mali_format_features2.h"
#include "mali_memory.h"
#include "mali_android_buffer.h"
#include "mali_wsi.h"

#include "util/format/u_format.h"
#include "util/log.h"
#include "util/u_debug.h"
#include "vk_format.h"
#include "vk_util.h"
#include "vulkan/vulkan_android.h"

bool
mali_ycbcr_enabled(uint32_t arch)
{
   const bool supported = arch == 11 ||
      (UNIFIED_API13_LAB && UNIFIED_VALIDATION_LAB && (arch == 7 || arch == 9));
   return supported && debug_get_bool_option("UNIFIED_YUV", false);
}

unsigned
mali_format_planes(VkFormat format, enum pipe_format planes[MALI_IMAGE_MAX_PLANES])
{
   switch (format) {
   case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM:
      planes[0] = PIPE_FORMAT_R8_UNORM;
      planes[1] = PIPE_FORMAT_R8G8_UNORM;
      return 2;
   case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM:
      planes[0] = planes[1] = planes[2] = PIPE_FORMAT_R8_UNORM;
      return 3;
   /* Combined depth/stencil in two planes, as panvk does on arch 9 and
    * later (Z32_S8X24 does not exist there) and as the blob keeps stencil
    * in a second slice array. */
   case VK_FORMAT_D32_SFLOAT_S8_UINT:
      planes[0] = PIPE_FORMAT_Z32_FLOAT;
      planes[1] = PIPE_FORMAT_S8_UINT;
      return 2;
   case VK_FORMAT_D24_UNORM_S8_UINT:
      planes[0] = PIPE_FORMAT_Z24X8_UNORM;
      planes[1] = PIPE_FORMAT_S8_UINT;
      return 2;
   default:
      break;
   }

   if (vk_format_get_plane_count(format) != 1)
      return 0;
   planes[0] = vk_format_to_pipe_format(format);
   return planes[0] == PIPE_FORMAT_NONE ? 0 : 1;
}

VkFormatFeatureFlags
mali_format_image_features(const struct mali_physical_device *pdev, VkFormat format)
{
   /* This four-component format requires a YCbCr conversion without
    * VK_EXT_rgba10x6_formats. Our conversion only supports the 8-bit
    * multi-plane formats described by mali_format_planes(). */
   if (format == VK_FORMAT_R10X6G10X6B10X6A10X6_UNORM_4PACK16)
      return 0;
   if (mali_format_is_ycbcr(format)) {
      if (!mali_ycbcr_enabled(pdev->arch))
         return 0;
      return VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
             VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
             VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT | VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT |
             VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT;
   }
   return mali_arch_dispatch(pdev->arch, format_image_features, pdev, format);
}

VkFormatFeatureFlags
mali_format_buffer_features(const struct mali_physical_device *pdev, VkFormat format)
{
   if (format == VK_FORMAT_R10X6G10X6B10X6A10X6_UNORM_4PACK16)
      return 0;
   return mali_arch_dispatch(pdev->arch, format_buffer_features, pdev, format);
}

/* Linear images of a combined depth/stencil format are not offered (the
 * blob reports optimal tiling only for them). */
static VkFormatFeatureFlags
linear_features(const struct mali_physical_device *pdev, VkFormat format)
{
   enum pipe_format planes[MALI_IMAGE_MAX_PLANES];
   if (mali_format_planes(format, planes) > 1 && !mali_format_is_ycbcr(format))
      return 0;
   return mali_format_image_features(pdev, format);
}

static bool
host_copy_format(const struct mali_physical_device *pdev, VkFormat format,
                 VkFormatFeatureFlags features)
{
   return mali_host_copy_enabled(pdev->arch) &&
          (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) &&
          !vk_format_is_depth_or_stencil(format) &&
          (vk_format_get_plane_count(format) == 1 || mali_format_is_ycbcr(format));
}

bool
mali_fd_image_format_supported(VkFormat format)
{
   if (vk_format_is_depth_or_stencil(format) || vk_format_is_compressed(format) ||
       vk_format_get_plane_count(format) != 1)
      return false;
   enum pipe_format pf = vk_format_to_pipe_format(format);
   if (pf == PIPE_FORMAT_NONE) return false;
   unsigned bytes = util_format_get_blocksize(pf);
   return bytes == 1 || bytes == 2 || bytes == 4 || bytes == 8 || bytes == 16;
}

VKAPI_ATTR void VKAPI_CALL
mali_GetPhysicalDeviceFormatProperties2(VkPhysicalDevice physicalDevice, VkFormat format,
                                        VkFormatProperties2 *pFormatProperties)
{
   VK_FROM_HANDLE(mali_physical_device, pdev, physicalDevice);

   const VkFormatFeatureFlags linear = linear_features(pdev, format);
   pFormatProperties->formatProperties = (VkFormatProperties) {
      .linearTilingFeatures = linear,
      .optimalTilingFeatures = mali_format_image_features(pdev, format),
      .bufferFeatures = mali_format_buffer_features(pdev, format),
   };

   const VkFormatFeatureFlags2 linear2 = mali_format_features2(format, linear, false);

   /* DRM format modifiers: DRM_FORMAT_MOD_LINEAR alone, with the linear
    * tiling features, for every format linear images can have. The list's
    * second form only differs in the width of the feature flags. */
   vk_foreach_struct(stype, ext, pFormatProperties->pNext) {
      switch (stype) {
      case VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3: {
         VkFormatProperties3 *p = ext;
         p->linearTilingFeatures = linear2;
         p->optimalTilingFeatures = mali_format_features2(
            format, pFormatProperties->formatProperties.optimalTilingFeatures, false);
         if (host_copy_format(pdev, format, linear))
            p->linearTilingFeatures |= VK_FORMAT_FEATURE_2_HOST_IMAGE_TRANSFER_BIT;
         if (host_copy_format(pdev, format, pFormatProperties->formatProperties.optimalTilingFeatures))
            p->optimalTilingFeatures |= VK_FORMAT_FEATURE_2_HOST_IMAGE_TRANSFER_BIT;
         p->bufferFeatures = mali_format_features2(
            format, pFormatProperties->formatProperties.bufferFeatures, true);
         break;
      }
      case VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT: {
         VkDrmFormatModifierPropertiesListEXT *list = ext;
         VK_OUTARRAY_MAKE_TYPED(VkDrmFormatModifierPropertiesEXT, out,
                                list->pDrmFormatModifierProperties,
                                &list->drmFormatModifierCount);
         if (linear && mali_fd_image_format_supported(format)) {
            vk_outarray_append_typed(VkDrmFormatModifierPropertiesEXT, &out, p) {
               *p = (VkDrmFormatModifierPropertiesEXT){
                  .drmFormatModifier = DRM_FORMAT_MOD_LINEAR,
                  .drmFormatModifierPlaneCount = 1,
                  .drmFormatModifierTilingFeatures = linear,
               };
            }
         }
         break;
      }
      case VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_2_EXT: {
         VkDrmFormatModifierPropertiesList2EXT *list = ext;
         VK_OUTARRAY_MAKE_TYPED(VkDrmFormatModifierProperties2EXT, out,
                                list->pDrmFormatModifierProperties,
                                &list->drmFormatModifierCount);
         if (linear && mali_fd_image_format_supported(format)) {
            vk_outarray_append_typed(VkDrmFormatModifierProperties2EXT, &out, p) {
               *p = (VkDrmFormatModifierProperties2EXT){
                  .drmFormatModifier = DRM_FORMAT_MOD_LINEAR,
                  .drmFormatModifierPlaneCount = 1,
                  .drmFormatModifierTilingFeatures = linear2,
               };
            }
         }
         break;
      }
      default:
         vk_debug_ignored_stype(stype);
         break;
      }
   }
}

static VkResult
image_format_properties(const struct mali_physical_device *pdev,
                        const VkPhysicalDeviceImageFormatInfo2 *info,
                        VkImageFormatProperties *props)
{
   const VkImageStencilUsageCreateInfo *stencil_usage_info =
      vk_find_struct_const(info->pNext, IMAGE_STENCIL_USAGE_CREATE_INFO);
   const VkImageUsageFlags usage =
      info->usage | (stencil_usage_info ? stencil_usage_info->stencilUsage : 0);
   const bool ycbcr = mali_format_is_ycbcr(info->format);
   if (ycbcr && (info->type != VK_IMAGE_TYPE_2D ||
                 (usage & ~MALI_YCBCR_IMAGE_USAGES) ||
                 (info->flags & ~MALI_YCBCR_IMAGE_FLAGS)))
      return VK_ERROR_FORMAT_NOT_SUPPORTED;

   if (info->flags & (VK_IMAGE_CREATE_SPARSE_BINDING_BIT |
                      VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT |
                      VK_IMAGE_CREATE_SPARSE_ALIASED_BIT |
                      VK_IMAGE_CREATE_PROTECTED_BIT |
                      VK_IMAGE_CREATE_DISJOINT_BIT))
      return VK_ERROR_FORMAT_NOT_SUPPORTED;

   VkFormatFeatureFlags feat;
   switch (info->tiling) {
   case VK_IMAGE_TILING_OPTIMAL:
      feat = mali_format_image_features(pdev, info->format);
      break;
   case VK_IMAGE_TILING_LINEAR:
      feat = linear_features(pdev, info->format);
      break;
   case VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT: {
      /* DRM_FORMAT_MOD_LINEAR is linear tiling under another name. */
      const VkPhysicalDeviceImageDrmFormatModifierInfoEXT *mod_info =
         vk_find_struct_const(info->pNext, PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT);
      if (!mod_info || mod_info->drmFormatModifier != DRM_FORMAT_MOD_LINEAR ||
          !mali_fd_image_format_supported(info->format) || info->type != VK_IMAGE_TYPE_2D ||
          (info->flags & ~VK_IMAGE_CREATE_ALIAS_BIT) ||
          (usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) ||
          mod_info->sharingMode != VK_SHARING_MODE_EXCLUSIVE)
         return VK_ERROR_FORMAT_NOT_SUPPORTED;
      feat = linear_features(pdev, info->format);
      break;
   }
   default:
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   }
   if (!feat)
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   if ((usage & VK_IMAGE_USAGE_HOST_TRANSFER_BIT) &&
       (!host_copy_format(pdev, info->format, feat) ||
        info->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT ||
        usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT))
      return VK_ERROR_FORMAT_NOT_SUPPORTED;

   const struct vk_properties *lim = &pdev->vk.properties;
   VkExtent3D extent;
   uint32_t layers;
   switch (info->type) {
   case VK_IMAGE_TYPE_1D:
      extent = (VkExtent3D){lim->maxImageDimension1D, 1, 1};
      layers = lim->maxImageArrayLayers;
      break;
   case VK_IMAGE_TYPE_2D:
      extent = (VkExtent3D){lim->maxImageDimension2D, lim->maxImageDimension2D, 1};
      layers = lim->maxImageArrayLayers;
      break;
   case VK_IMAGE_TYPE_3D:
      extent = (VkExtent3D){lim->maxImageDimension3D, lim->maxImageDimension3D,
                            lim->maxImageDimension3D};
      layers = 1;
      break;
   default:
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   }

   /* Unless EXTENDED_USAGE lets views use other formats, every usage needs
    * its format feature (as panvk). */
   if (!(info->flags & VK_IMAGE_CREATE_EXTENDED_USAGE_BIT)) {
      const bool ds = vk_format_is_depth_or_stencil(info->format);
      if ((usage & VK_IMAGE_USAGE_SAMPLED_BIT) && !(feat & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
         return VK_ERROR_FORMAT_NOT_SUPPORTED;
      if ((usage & VK_IMAGE_USAGE_STORAGE_BIT) && !(feat & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
         return VK_ERROR_FORMAT_NOT_SUPPORTED;
      if ((usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) &&
          !(feat & VK_FORMAT_FEATURE_TRANSFER_SRC_BIT))
         return VK_ERROR_FORMAT_NOT_SUPPORTED;
      if ((usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) &&
          !(feat & VK_FORMAT_FEATURE_TRANSFER_DST_BIT))
         return VK_ERROR_FORMAT_NOT_SUPPORTED;
      if (((usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ||
           ((usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) && !ds)) &&
          !(feat & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
         return VK_ERROR_FORMAT_NOT_SUPPORTED;
      if (((usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) ||
           ((usage & VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) && ds)) &&
          !(feat & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
         return VK_ERROR_FORMAT_NOT_SUPPORTED;
   }

   /* Exercise D24 independently before enabling it for normal clients.
    * Imported and linear images keep their existing single-sample path. */
   const bool d24 = info->format == VK_FORMAT_X8_D24_UNORM_PACK32 ||
                    info->format == VK_FORMAT_D24_UNORM_S8_UINT;
   const bool test_d24 = d24 && UNIFIED_VALIDATION_LAB &&
                         debug_get_bool_option("UNIFIED_D24_MSAA", false);
   const bool ds_msaa_candidate = info->format == VK_FORMAT_D16_UNORM ||
      info->format == VK_FORMAT_D32_SFLOAT || info->format == VK_FORMAT_S8_UINT ||
      info->format == VK_FORMAT_D32_SFLOAT_S8_UINT || test_d24;
   /* Multisampling: optimal 2D attachments, not cube, not storage. */
   VkSampleCountFlags samples = VK_SAMPLE_COUNT_1_BIT;
   if (info->tiling == VK_IMAGE_TILING_OPTIMAL && info->type == VK_IMAGE_TYPE_2D &&
       (feat & (VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) &&
       !(info->flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) &&
       !(usage & (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_HOST_TRANSFER_BIT)) &&
       (!vk_format_is_depth_or_stencil(info->format) || ds_msaa_candidate)) {
      enum pipe_format planes[MALI_IMAGE_MAX_PLANES];
      mali_format_planes(info->format, planes);
      samples = mali_physical_device_sample_counts(pdev, util_format_get_blocksize(planes[0]));
      if (test_d24)
         samples &= VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_4_BIT;
   }

   *props = (VkImageFormatProperties) {
      .maxExtent = extent,
      .maxMipLevels = ycbcr ? 1 : MALI_IMAGE_MAX_LEVELS,
      .maxArrayLayers = ycbcr ? 1 : layers,
      .sampleCounts = samples,
      /* An image has to fit in one allocation. */
      .maxResourceSize = pdev->vk.properties.maxMemoryAllocationSize,
   };
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
mali_GetPhysicalDeviceSparseImageFormatProperties2(
   UNUSED VkPhysicalDevice physicalDevice,
   UNUSED const VkPhysicalDeviceSparseImageFormatInfo2 *pFormatInfo,
   uint32_t *pPropertyCount,
   UNUSED VkSparseImageFormatProperties2 *pProperties)
{
   /* The device has no sparse binding. Both count and array queries
    * return zero, without modifying the caller's array. */
   *pPropertyCount = 0;
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_GetPhysicalDeviceImageFormatProperties2(VkPhysicalDevice physicalDevice,
                                             const VkPhysicalDeviceImageFormatInfo2 *pInfo,
                                             VkImageFormatProperties2 *pProps)
{
   VK_FROM_HANDLE(mali_physical_device, pdev, physicalDevice);

   /*
    * Two external handle types:
    *
    * - Android's hardware buffer, only for what the Android loader asks
    *   before it allocates swapchain buffers: their gralloc usage
    *   (mali_wsi_ahb_usage). The image itself is created with a
    *   VkNativeBufferANDROID, not by importing the hardware buffer, so
    *   nothing is importable or exportable.
    * - dma-buf (not on Android), importable into a 2D linear image (linear
    *   tiling or DRM_FORMAT_MOD_LINEAR), not exportable: we cannot allocate
    *   memory another process can open.
    *
    * Either image has one level, one layer and one sample.
    */
   const VkPhysicalDeviceExternalImageFormatInfo *ext_info =
      vk_find_struct_const(pInfo->pNext, PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO);
   const VkExternalMemoryHandleTypeFlagBits handle_type = ext_info ? ext_info->handleType : 0;
   if (handle_type && ((pInfo->usage & VK_IMAGE_USAGE_HOST_TRANSFER_BIT) ||
                       mali_format_is_ycbcr(pInfo->format))) {
      pProps->imageFormatProperties = (VkImageFormatProperties){0};
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   }
   const bool ahb =
      handle_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
   const bool dmabuf = handle_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
#ifdef VK_USE_PLATFORM_ANDROID_KHR
   const bool fd = dmabuf || handle_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
   const bool fd_exportable = mali_android_buffer_available();
   const VkExternalMemoryHandleTypeFlags compatible_fd = pInfo->tiling == VK_IMAGE_TILING_OPTIMAL ?
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT : MALI_EXTERNAL_FD_TYPES;
#else
   const bool fd = dmabuf, fd_exportable = false;
   const VkExternalMemoryHandleTypeFlags compatible_fd = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
#endif
   /* Preserve the loader's usage-only queries, including mutable formats.
    * A format which lacks typed AHB import still has a valid native usage
    * query; report no public import/export features rather than breaking it. */
   const bool ahb_importable = ahb && fd_exportable && mali_android_hal_format(pInfo->format);
   uint64_t ahb_usage = 0;
   VkResult result;
   if (ahb) {
      result = mali_wsi_ahb_usage(pdev, pInfo, &ahb_usage);
      if (result == VK_SUCCESS)
         result = image_format_properties(pdev, pInfo, &pProps->imageFormatProperties);
   } else if (fd) {
      if (!pdev->vk.supported_extensions.KHR_external_memory_fd ||
          !mali_fd_image_format_supported(pInfo->format) || pInfo->type != VK_IMAGE_TYPE_2D ||
          (pInfo->flags & ~VK_IMAGE_CREATE_ALIAS_BIT) ||
          (pInfo->usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) ||
          (dmabuf && pInfo->tiling == VK_IMAGE_TILING_OPTIMAL))
         result = VK_ERROR_FORMAT_NOT_SUPPORTED;
      else result = image_format_properties(pdev, pInfo, &pProps->imageFormatProperties);
   } else if (handle_type) {
      result = VK_ERROR_FORMAT_NOT_SUPPORTED;
   } else {
      result = image_format_properties(pdev, pInfo, &pProps->imageFormatProperties);
   }
   if (result != VK_SUCCESS) {
      pProps->imageFormatProperties = (VkImageFormatProperties){0};
      return result;
   }
   if (ahb || fd || pInfo->tiling == VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT) {
      VkImageFormatProperties *p = &pProps->imageFormatProperties;
      p->maxMipLevels = 1;
      p->maxArrayLayers = 1;
      p->sampleCounts = VK_SAMPLE_COUNT_1_BIT;
   }

   vk_foreach_struct(stype, ext, pProps->pNext) {
      switch (stype) {
      case VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES: {
         VkExternalImageFormatProperties *e = ext;
         e->externalMemoryProperties = (VkExternalMemoryProperties){
            .externalMemoryFeatures = ahb_importable ? VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT |
                 VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT :
                 (fd ? VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT |
                     (fd_exportable ? VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT : 0) : 0),
            .exportFromImportedHandleTypes = (fd && fd_exportable) ? compatible_fd : (ahb_importable ? handle_type : 0),
            .compatibleHandleTypes = fd ? compatible_fd : handle_type,
         };
         break;
      }
      case VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_USAGE_ANDROID: {
         VkAndroidHardwareBufferUsageANDROID *u = ext;
         u->androidHardwareBufferUsage = ahb_usage;
         break;
      }
      case VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_IMAGE_FORMAT_PROPERTIES: {
         VkSamplerYcbcrConversionImageFormatProperties *y = ext;
         y->combinedImageSamplerDescriptorCount = vk_format_get_plane_count(pInfo->format);
         break;
      }
      case VK_STRUCTURE_TYPE_HOST_IMAGE_COPY_DEVICE_PERFORMANCE_QUERY: {
         VkHostImageCopyDevicePerformanceQuery *q = ext;
         /* Host transfer disables AFBC. Block-compressed textures and
          * planar YCbCr never use AFBC, and linear tiling is unchanged. */
         const bool identical = pInfo->tiling == VK_IMAGE_TILING_LINEAR ||
                                vk_format_is_compressed(pInfo->format) ||
                                mali_format_is_ycbcr(pInfo->format);
         q->optimalDeviceAccess = identical;
         q->identicalMemoryLayout = identical;
         break;
      }
      default:
         vk_debug_ignored_stype(stype);
         break;
      }
   }
   return VK_SUCCESS;
}

/* Buffers: a dma-buf (not on Android) or a BLOB AHardwareBuffer (Android)
 * can be imported as a buffer's memory (mali_device_memory.c); nothing is
 * exportable. */
VKAPI_ATTR void VKAPI_CALL
mali_GetPhysicalDeviceExternalBufferProperties(VkPhysicalDevice physicalDevice,
   const VkPhysicalDeviceExternalBufferInfo *info, VkExternalBufferProperties *out)
{
   bool ahb = false, fd = false;
#ifdef VK_USE_PLATFORM_ANDROID_KHR
   const bool available = mali_android_buffer_available();
   ahb = !info->flags && info->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
   fd = available && !info->flags && (info->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT ||
                                     info->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT);
#else
   const bool available = false;
   fd = !info->flags && info->handleType == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
   out->externalMemoryProperties = (VkExternalMemoryProperties){
      .compatibleHandleTypes = info->handleType,
      .externalMemoryFeatures = fd ? VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT : 0,
   };
   return;
#endif
   out->externalMemoryProperties = (VkExternalMemoryProperties){
      .compatibleHandleTypes = fd ? MALI_EXTERNAL_FD_TYPES : info->handleType,
      .externalMemoryFeatures = (ahb || fd ? VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT : 0) |
          ((fd || (ahb && available)) ? VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT : 0),
      .exportFromImportedHandleTypes = fd ? MALI_EXTERNAL_FD_TYPES : (ahb && available ? info->handleType : 0),
   };
}
