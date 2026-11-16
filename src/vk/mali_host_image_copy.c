/* SPDX-License-Identifier: MIT */
#include "mali_vk.h"
#include "mali_entrypoints.h"
#include "mali_image.h"
#include "mali_memory.h"
#include "unified_host_image_copy_plan.h"
#include "util/format/u_format.h"
#include "util/u_debug.h"
#include "vk_util.h"

VkImageLayout mali_host_copy_layouts[MALI_HOST_COPY_LAYOUT_COUNT] = {
   VK_IMAGE_LAYOUT_GENERAL,
   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
};

bool
mali_host_copy_enabled(uint32_t arch)
{
   return arch == 11 && debug_get_bool_option("UNIFIED_HOST_IMAGE_COPY", false);
}

static bool
host_layout(VkImageLayout layout)
{
   for (unsigned i = 0; i < ARRAY_SIZE(mali_host_copy_layouts); ++i)
      if (layout == mali_host_copy_layouts[i])
         return true;
   return false;
}

static bool
usable(const struct mali_image *img)
{
   return (img->vk.usage & VK_IMAGE_USAGE_HOST_TRANSFER_BIT) &&
          img->vk.samples == VK_SAMPLE_COUNT_1_BIT &&
          ((img->plane_count == 1 && img->vk.aspects == VK_IMAGE_ASPECT_COLOR_BIT) ||
           mali_format_is_ycbcr(img->vk.format)) &&
          (img->vk.drm_format_mod == DRM_FORMAT_MOD_LINEAR ||
           img->vk.drm_format_mod == DRM_FORMAT_MOD_ARM_16X16_BLOCK_U_INTERLEAVED) &&
          img->mem && img->mem->dmabuf_fd < 0 && img->mem->bo.cpu &&
          img->mem_offset <= img->mem->vk.size &&
          img->size <= img->mem->vk.size - img->mem_offset;
}

static VkResult
sync_image(struct mali_device *dev, struct mali_image *img, bool flush)
{
   struct mali_device_memory *mem = img->mem;
   if (mem->props & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
      return VK_SUCCESS;

   const enum mali_kbase_result result = flush ?
      mali_kbase_bo_flush(dev->kbase, &mem->bo, img->mem_offset, img->size) :
      mali_kbase_bo_invalidate(dev->kbase, &mem->bo, img->mem_offset, img->size);

   return result == MALI_KBASE_SUCCESS ? VK_SUCCESS :
      vk_error(dev, VK_ERROR_MEMORY_MAP_FAILED);
}

static bool
plan_region(const struct mali_image *img, const VkImageSubresourceLayers *sub,
            VkOffset3D offset, VkExtent3D extent, unsigned row, unsigned height,
            struct unified_host_copy_plan *plan)
{
   const unsigned mip = sub->mipLevel;
   if (!(img->vk.aspects & sub->aspectMask) || !util_is_power_of_two_nonzero(sub->aspectMask) ||
       mip >= img->vk.mip_levels)
      return false;
   const unsigned plane = mali_image_aspect_plane(img, sub->aspectMask);
   const VkExtent3D level = mali_image_plane_extent(img, plane, mip);
   const struct mali_image_plane *p = &img->planes[plane];
   const struct mali_image_slice *s = &p->layout.slices[mip];
   const struct unified_host_level info = {
      .offset = s->offset,
      .row_stride = s->row_stride,
      .surface_stride = s->surface_stride,
      .array_stride = p->layout.array_stride,
      .width = level.width, .height = level.height, .depth = level.depth,
      .layers = img->vk.array_layers,
      .block_width = util_format_get_blockwidth(p->format),
      .block_height = util_format_get_blockheight(p->format),
      .block_bytes = util_format_get_blocksize(p->format),
      .tile_side = img->vk.drm_format_mod == DRM_FORMAT_MOD_LINEAR ? 0 :
                   util_format_is_compressed(p->format) ? 4 : 16,
      .image_3d = img->vk.image_type == VK_IMAGE_TYPE_3D,
   };
   if (sub->baseArrayLayer >= img->vk.array_layers)
      return false;
   const struct unified_host_texel_region region = {
      .x = offset.x, .y = offset.y, .z = offset.z,
      .width = extent.width, .height = extent.height, .depth = extent.depth,
      .base_layer = sub->baseArrayLayer,
      .layer_count = vk_image_subresource_layer_count(&img->vk, sub),
      .memory_row_length = row, .memory_image_height = height,
   };
   return unified_host_image_plan(&info, &region, img->mem->bo.cpu,
                                  img->mem->vk.size, img->mem_offset, plan);
}

static bool
raw_region(const struct mali_image *img, const VkImageSubresourceLayers *sub,
           VkOffset3D offset, VkExtent3D extent, struct unified_host_copy_plan *plan)
{
   if (!plan_region(img, sub, offset, extent, 0, 0, plan))
      return false;
   const unsigned plane = mali_image_aspect_plane(img, sub->aspectMask);
   const VkExtent3D mip = mali_image_plane_extent(img, plane, sub->mipLevel);
   if (offset.x || offset.y || offset.z || extent.width != mip.width ||
       extent.height != mip.height || extent.depth != mip.depth)
      return false;
   const struct mali_image_slice *s = &img->planes[plane].layout.slices[sub->mipLevel];
   const unsigned layers = vk_image_subresource_layer_count(&img->vk, sub);
   const uint64_t base = plan->plane.offset + sub->baseArrayLayer * plan->plane.surface_stride;
   const uint64_t end = base + (layers - 1) * plan->plane.surface_stride + s->size;
   if (end > plan->plane.available || s->size > SIZE_MAX / layers)
      return false;
   plan->blocks.depth = layers;
   plan->host_surface_stride = s->size;
   plan->host_span = s->size * layers;
   plan->plane.offset = base;
   return true;
}

static unsigned
spread4(unsigned x)
{
   x = (x | (x << 2)) & 0x33;
   return (x | (x << 1)) & 0x55;
}

static uint8_t *
block_at(const struct unified_host_copy_plan *p, unsigned x, unsigned y, unsigned z)
{
   const struct unified_host_plane *plane = &p->plane;
   x += p->blocks.x;
   y += p->blocks.y;
   z += p->blocks.z;
   uint64_t offset = plane->offset + z * plane->surface_stride;
   const unsigned tile = plane->tile_side;
   if (!tile)
      offset += y * plane->row_stride + (uint64_t)x * plane->block_bytes;
   else {
      const unsigned mask = tile - 1;
      const unsigned index = (spread4(y & mask) * 3) ^ spread4(x & mask);
      offset += (y / tile) * plane->row_stride +
                ((uint64_t)(x / tile) * tile * tile + index) * plane->block_bytes;
   }
   return plane->mapping + offset;
}

static void
copy_host(const struct unified_host_copy_plan *p, void *host, bool raw, bool upload)
{
   for (unsigned z = 0; z < p->blocks.depth; ++z) {
      uint8_t *host_slice = (uint8_t *)host + z * p->host_surface_stride;
      if (raw) {
         uint8_t *image_slice = p->plane.mapping + p->plane.offset + z * p->plane.surface_stride;
         if (upload)
            memcpy(image_slice, host_slice, p->host_surface_stride);
         else
            memcpy(host_slice, image_slice, p->host_surface_stride);
         continue;
      }
      for (unsigned y = 0; y < p->blocks.height; ++y) {
         uint8_t *host_row = host_slice + y * p->host_row_stride;
         const unsigned step = p->plane.tile_side ? 1 : p->blocks.width;
         const size_t bytes = step * p->plane.block_bytes;
         for (unsigned x = 0; x < p->blocks.width; x += step) {
            uint8_t *block = block_at(p, x, y, z);
            if (upload)
               memcpy(block, host_row + x * p->plane.block_bytes, bytes);
            else
               memcpy(host_row + x * p->plane.block_bytes, block, bytes);
         }
      }
   }
}

struct host_region {
   VkImageSubresourceLayers sub;
   VkOffset3D offset;
   VkExtent3D extent;
   void *ptr;
   unsigned row, height;
};

static struct host_region
host_region_info(const void *regions, unsigned i, bool upload)
{
   if (upload) {
      const VkMemoryToImageCopy *r = (const VkMemoryToImageCopy *)regions + i;
      return (struct host_region){r->imageSubresource, r->imageOffset, r->imageExtent,
         (void *)r->pHostPointer, r->memoryRowLength, r->memoryImageHeight};
   }
   const VkImageToMemoryCopy *r = (const VkImageToMemoryCopy *)regions + i;
   return (struct host_region){r->imageSubresource, r->imageOffset, r->imageExtent,
      r->pHostPointer, r->memoryRowLength, r->memoryImageHeight};
}

static VkResult
host_image_memory(struct mali_device *dev, struct mali_image *img, VkImageLayout layout,
                  VkHostImageCopyFlags flags, unsigned count, const void *regions, bool upload)
{
   if (!usable(img) || !host_layout(layout))
      return vk_error(dev, VK_ERROR_FORMAT_NOT_SUPPORTED);
   const bool raw = flags & VK_HOST_IMAGE_COPY_MEMCPY_BIT;
   struct unified_host_copy_plan *plans = calloc(count, sizeof(*plans));
   if (!plans && count)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   VkResult result = VK_SUCCESS;
   for (unsigned i = 0; i < count; ++i) {
      struct host_region r = host_region_info(regions, i, upload);
      const bool valid = raw ? raw_region(img, &r.sub, r.offset, r.extent, &plans[i]) :
                              plan_region(img, &r.sub, r.offset, r.extent, r.row, r.height, &plans[i]);
      if (!valid || !r.ptr || (uintptr_t)r.ptr > UINTPTR_MAX - plans[i].host_span) {
         result = vk_error(dev, VK_ERROR_FORMAT_NOT_SUPPORTED);
         goto finish;
      }
   }
   result = sync_image(dev, img, false);
   if (result != VK_SUCCESS)
      goto finish;
   for (unsigned i = 0; i < count; ++i)
      copy_host(&plans[i], host_region_info(regions, i, upload).ptr, raw, upload);
   if (upload) {
      img->fresh = false;
      result = sync_image(dev, img, true);
   }
finish:
   free(plans);
   return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_CopyMemoryToImage(VkDevice device, const VkCopyMemoryToImageInfo *info)
{
   VK_FROM_HANDLE(mali_device, dev, device);
   VK_FROM_HANDLE(mali_image, img, info->dstImage);
   return host_image_memory(dev, img, info->dstImageLayout, info->flags,
                            info->regionCount, info->pRegions, true);
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_CopyImageToMemory(VkDevice device, const VkCopyImageToMemoryInfo *info)
{
   VK_FROM_HANDLE(mali_device, dev, device);
   VK_FROM_HANDLE(mali_image, img, info->srcImage);
   return host_image_memory(dev, img, info->srcImageLayout, info->flags,
                            info->regionCount, info->pRegions, false);
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_CopyImageToImage(VkDevice device, const VkCopyImageToImageInfo *info)
{
   VK_FROM_HANDLE(mali_device, dev, device);
   VK_FROM_HANDLE(mali_image, src, info->srcImage);
   VK_FROM_HANDLE(mali_image, dst, info->dstImage);
   if (!usable(src) || !usable(dst) || !host_layout(info->srcImageLayout) ||
       !host_layout(info->dstImageLayout))
      return vk_error(dev, VK_ERROR_FORMAT_NOT_SUPPORTED);
   struct plan_pair { struct unified_host_copy_plan src, dst; };
   const unsigned max_planes = mali_format_is_ycbcr(src->vk.format) ? src->plane_count : 1;
   struct plan_pair *plans = calloc(info->regionCount, max_planes * sizeof(*plans));
   if (!plans && info->regionCount)
      return vk_error(dev, VK_ERROR_OUT_OF_HOST_MEMORY);
   VkResult result = VK_SUCCESS;
   const bool raw = info->flags & VK_HOST_IMAGE_COPY_MEMCPY_BIT;
   size_t plan_count = 0;
   for (unsigned i = 0; i < info->regionCount; ++i) {
      const VkImageCopy2 *region = &info->pRegions[i];
      const unsigned count = mali_image_copy_plane_count(src, region);
      if (count > 1 && src->vk.format != dst->vk.format) {
         result = vk_error(dev, VK_ERROR_FORMAT_NOT_SUPPORTED);
         goto finish;
      }
      for (unsigned plane = 0; plane < count; ++plane) {
         VkImageCopy2 r_plane;
         const VkImageCopy2 *r = region;
         if (count > 1) {
            r_plane = mali_image_copy_plane(region, plane);
            r = &r_plane;
         }
         struct plan_pair *p = &plans[plan_count++];
         const bool valid = raw ?
            raw_region(src, &r->srcSubresource, r->srcOffset, r->extent, &p->src) &&
            raw_region(dst, &r->dstSubresource, r->dstOffset, r->extent, &p->dst) :
            plan_region(src, &r->srcSubresource, r->srcOffset, r->extent, 0, 0, &p->src) &&
            plan_region(dst, &r->dstSubresource, r->dstOffset, r->extent, 0, 0, &p->dst);
         if (!valid || p->src.blocks.depth != p->dst.blocks.depth ||
             p->src.blocks.width != p->dst.blocks.width ||
             p->src.blocks.height != p->dst.blocks.height ||
             p->src.plane.block_bytes != p->dst.plane.block_bytes ||
             (raw && (p->src.host_surface_stride != p->dst.host_surface_stride ||
                      p->src.plane.row_stride != p->dst.plane.row_stride ||
                      p->src.plane.tile_side != p->dst.plane.tile_side))) {
            result = vk_error(dev, VK_ERROR_FORMAT_NOT_SUPPORTED);
            goto finish;
         }
      }
   }
   result = sync_image(dev, src, false);
   if (result == VK_SUCCESS && dst != src)
      result = sync_image(dev, dst, false);
   if (result != VK_SUCCESS)
      goto finish;
   for (size_t i = 0; i < plan_count; ++i) {
      const struct plan_pair *p = &plans[i];
      for (unsigned z = 0; z < p->src.blocks.depth; ++z) {
         if (raw) {
            memcpy(p->dst.plane.mapping + p->dst.plane.offset + z * p->dst.plane.surface_stride,
                   p->src.plane.mapping + p->src.plane.offset + z * p->src.plane.surface_stride,
                   p->src.host_surface_stride);
            continue;
         }
         for (unsigned y = 0; y < p->src.blocks.height; ++y)
            for (unsigned x = 0; x < p->src.blocks.width; ++x)
               memcpy(block_at(&p->dst, x, y, z), block_at(&p->src, x, y, z),
                      p->src.plane.block_bytes);
      }
   }
   dst->fresh = false;
   result = sync_image(dev, dst, true);
finish:
   free(plans);
   return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_TransitionImageLayout(VkDevice device, uint32_t count,
                           const VkHostImageLayoutTransitionInfo *transitions)
{
   VK_FROM_HANDLE(mali_device, dev, device);
   for (unsigned i = 0; i < count; ++i) {
      const VkHostImageLayoutTransitionInfo *t = &transitions[i];
      VK_FROM_HANDLE(mali_image, img, t->image);
      if (!usable(img) || !host_layout(t->newLayout) ||
          !(host_layout(t->oldLayout) || t->oldLayout == VK_IMAGE_LAYOUT_UNDEFINED ||
            t->oldLayout == VK_IMAGE_LAYOUT_PREINITIALIZED))
         return vk_error(dev, VK_ERROR_FORMAT_NOT_SUPPORTED);
   }
   /* All admitted layouts share a single storage representation. */
   return VK_SUCCESS;
}
