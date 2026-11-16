/* SPDX-License-Identifier: MIT */
#ifndef MALI_ANDROID_BUFFER_H
#define MALI_ANDROID_BUFFER_H
#include <stdbool.h>
#include <stdint.h>
#include "vulkan/vulkan.h"
#include "mali_android_rgb_external.h"
#include "mali_android_yuv.h"
struct mali_device;
struct mali_device_memory;
struct mali_image;
struct AHardwareBuffer;
struct native_handle;
struct mali_android_buffer {
   struct mali_android_yuv_layout yuv;
   int fd;                       /* borrowed from the registered gralloc handle */
   uint32_t format, width, height, layers, pixel_stride;
   uint64_t usage, allocation_size, modifier, offset, row_stride, layer_stride;
};
#ifdef VK_USE_PLATFORM_ANDROID_KHR
bool mali_android_buffer_available(void);
bool mali_android_buffer_acquire(struct AHardwareBuffer *buffer);
VkResult mali_android_external_image_info(struct mali_device *dev,
   const VkImageCreateInfo *in, VkImageCreateInfo *out,
   VkExternalMemoryImageCreateInfo *memory, uint64_t *external_format);
VkResult mali_android_blob_properties(VkDevice device, const struct AHardwareBuffer *buffer,
   VkAndroidHardwareBufferPropertiesANDROID *props);
VkFormat mali_android_vk_format(uint32_t format);
uint32_t mali_android_hal_format(VkFormat format);
#else
static inline bool mali_android_buffer_available(void) { return false; }
static inline uint32_t mali_android_hal_format(VkFormat format) { (void)format; return 0; }
#endif
VkResult mali_android_buffer_info(const struct native_handle *handle,
                                  struct mali_android_buffer *out);
VkResult mali_android_ahb_info(const struct AHardwareBuffer *buffer,
                               struct mali_android_buffer *out);
VkResult mali_android_image_layout(struct mali_image *image,
                                   const struct mali_android_buffer *info);
VkResult mali_android_allocate_memory(struct mali_device *dev,
                                      const VkMemoryAllocateInfo *info,
                                      const VkAllocationCallbacks *alloc,
                                      VkDeviceMemory *out);
VkResult mali_android_create_blob_fd(uint64_t size, int *out_fd);
void mali_android_buffer_release(struct AHardwareBuffer *buffer);
#endif
