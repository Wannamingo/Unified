/*
 * SPDX-FileCopyrightText: 2026 ARMSX2 and bmdhacks
 * SPDX-License-Identifier: MIT
 */

/*
 * VkBuffer. A buffer is a size and, once bound, a GPU address; binding
 * makes no kernel call. The common buffer-device-address getter uses that
 * address where the architecture's basicBDA feature is admitted. Sparse
 * buffers and address capture/replay are not exposed.
 */

#include "mali_vk.h"
#include "mali_memory.h"

#include "util/log.h"
#include "util/u_atomic.h"
#include "vk_util.h"

VKAPI_ATTR VkResult VKAPI_CALL
mali_CreateBuffer(VkDevice _device, const VkBufferCreateInfo *pCreateInfo,
                  const VkAllocationCallbacks *pAllocator, VkBuffer *pBuffer)
{
   VK_FROM_HANDLE(mali_device, dev, _device);
   struct mali_physical_device *pdev = mali_device_physical(dev);

   /* A buffer has to fit in one allocation to be bound. */
   if (pCreateInfo->size > pdev->vk.properties.maxMemoryAllocationSize)
      return vk_errorf(dev, VK_ERROR_OUT_OF_DEVICE_MEMORY,
                       "buffer of %llu bytes is above maxMemoryAllocationSize",
                       (unsigned long long)pCreateInfo->size);

   struct mali_buffer *buf =
      vk_buffer_create(&dev->vk, pCreateInfo, pAllocator, sizeof(*buf));
   if (!buf)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);

   const VkExternalMemoryBufferCreateInfo *ext =
      vk_find_struct_const(pCreateInfo->pNext, EXTERNAL_MEMORY_BUFFER_CREATE_INFO);
   buf->external_handles = ext ? ext->handleTypes : 0;
   buf->ahb = ext && (ext->handleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID);
   *pBuffer = mali_buffer_to_handle(buf);
   return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL
mali_DestroyBuffer(VkDevice _device, VkBuffer _buffer,
                   const VkAllocationCallbacks *pAllocator)
{
   VK_FROM_HANDLE(mali_device, dev, _device);
   VK_FROM_HANDLE(mali_buffer, buf, _buffer);

   if (!buf)
      return;
   vk_buffer_destroy(&dev->vk, pAllocator, &buf->vk);
}

/* The blob's answer: the size as created, 64-byte alignment, the two
 * host-visible types, no dedicated allocation. */
static void
buffer_requirements(uint64_t size, bool ahb, VkMemoryRequirements2 *reqs)
{
   reqs->memoryRequirements = (VkMemoryRequirements) {
      .size = size,
      .alignment = MALI_BUFFER_ALIGNMENT,
      .memoryTypeBits = ahb ? (1u << MALI_MEMORY_TYPE_EXTERNAL_CACHED) : MALI_MEMORY_TYPES_HOST_VISIBLE,
   };

   vk_foreach_struct(stype, ext, reqs->pNext) {
      switch (stype) {
      case VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS: {
         VkMemoryDedicatedRequirements *d = ext;
         d->prefersDedicatedAllocation = VK_FALSE;
         d->requiresDedicatedAllocation = VK_FALSE;
         break;
      }
      default:
         vk_debug_ignored_stype(stype);
         break;
      }
   }
}

VKAPI_ATTR void VKAPI_CALL
mali_GetBufferMemoryRequirements2(VkDevice _device,
                                  const VkBufferMemoryRequirementsInfo2 *pInfo,
                                  VkMemoryRequirements2 *pMemoryRequirements)
{
   VK_FROM_HANDLE(mali_buffer, buf, pInfo->buffer);
   buffer_requirements(buf->vk.size, (buf->ahb && mali_android_buffer_available()) || (buf->external_handles & MALI_EXTERNAL_FD_TYPES), pMemoryRequirements);
}

VKAPI_ATTR void VKAPI_CALL
mali_GetDeviceBufferMemoryRequirements(VkDevice _device,
   const VkDeviceBufferMemoryRequirements *pInfo, VkMemoryRequirements2 *pRequirements)
{
   const VkExternalMemoryBufferCreateInfo *external =
      vk_find_struct_const(pInfo->pCreateInfo->pNext, EXTERNAL_MEMORY_BUFFER_CREATE_INFO);
   const bool imported = external &&
      ((external->handleTypes & MALI_EXTERNAL_FD_TYPES) ||
       ((external->handleTypes & VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID) &&
        mali_android_buffer_available()));
   buffer_requirements(pInfo->pCreateInfo->size, imported, pRequirements);
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_BindBufferMemory2(VkDevice _device, uint32_t bindInfoCount,
                       const VkBindBufferMemoryInfo *pBindInfos)
{
   VkResult result = VK_SUCCESS;
   for (uint32_t i = 0; i < bindInfoCount; i++) {
      VK_FROM_HANDLE(mali_buffer, buf, pBindInfos[i].buffer);
      VK_FROM_HANDLE(mali_device_memory, mem, pBindInfos[i].memory);
      const VkBindMemoryStatus *status =
         vk_find_struct_const(pBindInfos[i].pNext, BIND_MEMORY_STATUS);

      if ((buf->external_handles & MALI_EXTERNAL_FD_TYPES) &&
          (mem->dmabuf_fd < 0 || mem->vk.memory_type_index != MALI_MEMORY_TYPE_EXTERNAL_CACHED ||
           !(buf->external_handles & (mem->vk.import_handle_type | mem->vk.export_handle_types)) ||
           pBindInfos[i].memoryOffset % MALI_BUFFER_ALIGNMENT ||
           pBindInfos[i].memoryOffset % MALI_BUFFER_ALIGNMENT ||
          pBindInfos[i].memoryOffset > mem->vk.size ||
           buf->vk.size > mem->vk.size - pBindInfos[i].memoryOffset)) {
         if (status) *status->pResult = VK_ERROR_INVALID_EXTERNAL_HANDLE;
         result = VK_ERROR_INVALID_EXTERNAL_HANDLE; continue;
      }
      if (mem->android_buffer && (!buf->ahb || mem->android_layout.format != 0x21 ||
          (mem->android_dedicated_buffer && mem->android_dedicated_buffer != pBindInfos[i].buffer) ||
          pBindInfos[i].memoryOffset % MALI_BUFFER_ALIGNMENT ||
          pBindInfos[i].memoryOffset > mem->vk.size ||
          buf->vk.size > mem->vk.size - pBindInfos[i].memoryOffset)) {
         if (status) *status->pResult = VK_ERROR_INVALID_EXTERNAL_HANDLE;
         result = VK_ERROR_INVALID_EXTERNAL_HANDLE; continue;
      }
      p_atomic_inc(&mem->binds);
      buf->mem = mem;
      buf->mem_offset = pBindInfos[i].memoryOffset;
      buf->vk.device_address = mali_device_memory_gpu_va(mem, pBindInfos[i].memoryOffset);
      if (status)
         *status->pResult = VK_SUCCESS;
   }
   return result;
}
