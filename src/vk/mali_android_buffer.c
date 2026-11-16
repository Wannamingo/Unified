/* SPDX-License-Identifier: MIT */
/* Android 13/14 gralloc4 bridge. The public libnativewindow dependency
 * supplies the system mapper. No handle offsets or vendor fd indices are used.
 * Supports linear RGB, BLOB and opt-in linear YUV AHardwareBuffers.
 *
 * The mapper's C++ entrypoints are resolved by their exact ABI names. Its AIDL
 * PlaneLayout is components + eight int64_t fields (AOSP graphics/common AIDL).
 * We use the platform vector/string ABI here, and its own string destructor and
 * operator delete: never pass NDK libc++ objects into the platform libc++ ABI.
 * Missing symbols, another SDK or unknown layouts fail closed.
 */
#include "mali_android_buffer.h"
#include "mali_vk.h"
#include "mali_memory.h"
#include "mali_image.h"
#include "mali_wsi.h"
#include "drm-uapi/drm_fourcc.h"
#include "vk_util.h"
#include "vk_common_entrypoints.h"
#include "util/u_debug.h"
#include "util/u_call_once.h"
#include "util/format/u_format.h"
#include "util/log.h"
#include "vk_format.h"
#include <android/hardware_buffer.h>
#include <cutils/native_handle.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>

/* AOSP Android 13/14 NDK AIDL data layout; no Parcelable base/vtable.
 * Android 13 GraphicBufferMapper already uses ui::PlaneLayout, an alias
 * of this same AIDL type. Keep exact symbol resolution and all metadata
 * checks: supporting SDK33 must not fall back to guessing vendor handles. */
struct platform_vector { void *begin, *end, *capacity; };
struct platform_component { uint64_t name[3]; int64_t type, offset, size; };
struct platform_plane {
   struct platform_vector components;
   int64_t offset, increment, stride, width, height, size, hsub, vsub;
};
static_assert(sizeof(struct platform_plane) == 88, "AIDL plane ABI");
static_assert(sizeof(struct platform_component) == 48, "AIDL component ABI");

static struct {
   void *lib, *mapper;
   int (*planes)(void *, const native_handle_t *, struct platform_vector *);
   int (*size)(void *, const native_handle_t *, uint64_t *);
   int (*modifier)(void *, const native_handle_t *, uint64_t *);
   int (*usage)(void *, const native_handle_t *, uint64_t *);
   int (*width)(void *, const native_handle_t *, uint64_t *);
   int (*height)(void *, const native_handle_t *, uint64_t *);
   int (*layers)(void *, const native_handle_t *, uint64_t *);
   int (*format)(void *, const native_handle_t *, uint32_t *);
   int (*string_compare)(const void *, const char *);
   void (*string_destroy)(void *);
   void (*deallocate)(void *);
   const native_handle_t *(*handle)(const AHardwareBuffer *);
   void (*describe)(const AHardwareBuffer *, AHardwareBuffer_Desc *);
   int (*allocate)(const AHardwareBuffer_Desc *, AHardwareBuffer **);
   void (*acquire)(AHardwareBuffer *);
   void (*release)(AHardwareBuffer *);
   bool ready;
   bool native_only; /* SDK31 private ANB pilot; public AHB admission unchanged. */
} android;
static util_once_flag android_once = UTIL_ONCE_FLAG_INIT;

static void
android_init(void)
{
   char sdk[PROP_VALUE_MAX];
   if (__system_property_get("ro.build.version.sdk", sdk) <= 0)
      return;
   const int version = atoi(sdk);
   if (version != 31 && version != 33 && version != 34)
      return;
   /* SDK31 can export the same AIDL mapper ABI; exact symbols below remain
    * mandatory. Public AHB/FD features stay closed on this new SDK. */
   android.native_only = version == 31;
   android.lib = dlopen("libnativewindow.so", RTLD_NOW | RTLD_LOCAL);
   if (!android.lib) return;
#define LOAD(field, name) do { *(void **)(&android.field) = dlsym(android.lib, name); if (!android.field) {mesa_logw("Unified: Android metadata missing %s", name); return;} } while (0)
   LOAD(planes, "_ZN7android19GraphicBufferMapper15getPlaneLayoutsEPK13native_handlePNSt3__16vectorIN4aidl7android8hardware8graphics6common11PlaneLayoutENS4_9allocatorISB_EEEE");
   LOAD(size, "_ZN7android19GraphicBufferMapper17getAllocationSizeEPK13native_handlePm");
   LOAD(modifier, "_ZN7android19GraphicBufferMapper22getPixelFormatModifierEPK13native_handlePm");
   LOAD(usage, "_ZN7android19GraphicBufferMapper8getUsageEPK13native_handlePm");
   LOAD(width, "_ZN7android19GraphicBufferMapper8getWidthEPK13native_handlePm");
   LOAD(height, "_ZN7android19GraphicBufferMapper9getHeightEPK13native_handlePm");
   LOAD(layers, "_ZN7android19GraphicBufferMapper13getLayerCountEPK13native_handlePm");
   LOAD(format, "_ZN7android19GraphicBufferMapper23getPixelFormatRequestedEPK13native_handlePNS_8hardware8graphics6common4V1_211PixelFormatE");
   LOAD(string_destroy, "_ZNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEED1Ev");
   *(void **)(&android.string_compare) = dlsym(android.lib,
      "_ZNKSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEE7compareEPKc");
   LOAD(deallocate, "_ZdlPv");
   LOAD(handle, "AHardwareBuffer_getNativeHandle");
   LOAD(describe, "AHardwareBuffer_describe");
   LOAD(allocate, "AHardwareBuffer_allocate");
   LOAD(acquire, "AHardwareBuffer_acquire");
   LOAD(release, "AHardwareBuffer_release");
#undef LOAD
   void (*preload)(void) = (void (*)(void))dlsym(android.lib, "_ZN7android19GraphicBufferMapper10preloadHalEv");
   void **instance = dlsym(android.lib, "_ZN7android9SingletonINS_19GraphicBufferMapperEE9sInstanceE");
   if (!preload || !instance) return;
   preload();
   if (!*instance) {
      AHardwareBuffer_Desc desc = {.width=64,.height=1,.layers=1,.format=AHARDWAREBUFFER_FORMAT_BLOB,
         .usage=AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER};
      AHardwareBuffer *bootstrap = NULL;
      if (!android.allocate(&desc, &bootstrap)) android.release(bootstrap);
   }
   android.mapper = *instance;
   android.ready = android.mapper != NULL;
}

static bool android_metadata_available(void)
{
   util_call_once(&android_once, android_init);
   return android.ready;
}

bool mali_android_buffer_available(void)
{
   return android_metadata_available() && !android.native_only;
}

VkFormat mali_android_vk_format(uint32_t f)
{
   switch (f) {
   case 1: case 2: return VK_FORMAT_R8G8B8A8_UNORM;
   case 4: return VK_FORMAT_R5G6B5_UNORM_PACK16;
   case 5: return VK_FORMAT_B8G8R8A8_UNORM;
   case 0x16: return VK_FORMAT_R16G16B16A16_SFLOAT;
   case 0x2b: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
   case 0x38: return VK_FORMAT_R8_UNORM;
   default: return VK_FORMAT_UNDEFINED;
   }
}
uint32_t mali_android_hal_format(VkFormat f)
{
   switch (f) {
   case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: return 1;
   case VK_FORMAT_R5G6B5_UNORM_PACK16: return 4;
   case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: return 5;
   case VK_FORMAT_R16G16B16A16_SFLOAT: return 0x16;
   case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return 0x2b;
   case VK_FORMAT_R8_UNORM: return 0x38;
   default: return 0;
   }
}

static void destroy_planes(struct platform_vector *v)
{
   for (struct platform_plane *p = v->begin; p < (struct platform_plane *)v->end; p++) {
      for (struct platform_component *c = p->components.begin;
           c < (struct platform_component *)p->components.end; c++)
         android.string_destroy(c->name);
      android.deallocate(p->components.begin);
   }
   android.deallocate(v->begin);
}

static bool
android_yuv_metadata(const struct platform_vector *vector,
                     struct mali_android_buffer *info)
{
   if (!debug_get_bool_option("UNIFIED_YUV", false) ||
       android.native_only || !android.string_compare ||
       (info->format != 0x23 && info->format != 0x11 && info->format != 0x32315659))
      return false;
   const uintptr_t begin = (uintptr_t)vector->begin, end = (uintptr_t)vector->end;
   if (end < begin || (end - begin) % sizeof(struct platform_plane)) return false;
   const unsigned count = (end - begin) / sizeof(struct platform_plane);
   if (count != 2 && count != 3) return false;
   struct mali_android_yuv_plane planes[3] = {0};
   for (unsigned p = 0; p < count; p++) {
      const struct platform_plane *src = (const struct platform_plane *)vector->begin + p;
      const uintptr_t cb = (uintptr_t)src->components.begin, ce = (uintptr_t)src->components.end;
      if (ce < cb || (ce - cb) % sizeof(struct platform_component)) return false;
      unsigned components = (ce - cb) / sizeof(struct platform_component);
      if (components < 1 || components > 2 || src->offset < 0 || src->stride <= 0 ||
          src->size <= 0 || src->width <= 0 || src->height <= 0 || src->hsub <= 0 ||
          src->vsub <= 0 || src->increment <= 0 || src->width > UINT32_MAX ||
          src->height > UINT32_MAX || src->hsub > UINT32_MAX || src->vsub > UINT32_MAX ||
          src->increment > UINT32_MAX)
         return false;
      planes[p] = (struct mali_android_yuv_plane){
         .offset = src->offset, .row_stride = src->stride, .size = src->size,
         .width = src->width, .height = src->height, .hsub = src->hsub, .vsub = src->vsub,
         .increment_bits = src->increment, .component_count = components,
      };
      for (unsigned c = 0; c < components; c++) {
         const struct platform_component *comp = (const struct platform_component *)src->components.begin + c;
         if (android.string_compare(comp->name,
                "android.hardware.graphics.common.PlaneLayoutComponentType") ||
             (comp->type != 1 && comp->type != 2 && comp->type != 4) ||
             comp->offset < 0 || comp->offset > UINT32_MAX ||
             comp->size < 0 || comp->size > UINT32_MAX)
            return false;
         planes[p].components[c] = (struct mali_android_yuv_component){
            comp->type, comp->offset, comp->size,
         };
      }
   }
   return mali_android_yuv_layout_parse(info->width, info->height, info->allocation_size,
                                        count, planes, &info->yuv);
}

VkResult mali_android_buffer_info(const native_handle_t *handle,
                                  struct mali_android_buffer *out)
{
   if (!android_metadata_available() || !handle || handle->version != sizeof(native_handle_t) ||
       handle->numInts < 0 || handle->numInts > 256 || handle->numFds < 1 || handle->numFds > 4)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   struct mali_android_buffer info = {.fd = -1};
   uint64_t width, height, layers;
   if (android.size(android.mapper, handle, &info.allocation_size) ||
       android.usage(android.mapper, handle, &info.usage) ||
       android.width(android.mapper, handle, &width) ||
       android.height(android.mapper, handle, &height) ||
       android.layers(android.mapper, handle, &layers) ||
       android.format(android.mapper, handle, &info.format) ||
       !width || !height || layers != 1 || width > UINT32_MAX || height > UINT32_MAX ||
       !info.allocation_size || (info.usage & AHARDWAREBUFFER_USAGE_PROTECTED_CONTENT))
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   info.width = width; info.height = height; info.layers = layers;
   /* Metadata and gralloc-extra FDs are not GPU memory. No import-by-trial. */
   for (int i = 0; i < handle->numFds; i++) {
      char path[64], target[128] = {0};
      snprintf(path, sizeof(path), "/proc/self/fd/%d", handle->data[i]);
      ssize_t len = readlink(path, target, sizeof(target)-1);
      /* readlink may return exactly /dmabuf: when no name is set. */
      if (len >= 8 && !strncmp(target, "/dmabuf:", 8)) {
         if (info.fd >= 0) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
         info.fd = handle->data[i];
      }
   }
   struct stat statbuf;
   if (info.fd < 0 || fstat(info.fd, &statbuf) || statbuf.st_size < 0 ||
       (uint64_t)statbuf.st_size < info.allocation_size)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   if (info.format == AHARDWAREBUFFER_FORMAT_BLOB) {
      if (height != 1 || !(info.usage & AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER))
         return VK_ERROR_INVALID_EXTERNAL_HANDLE;
      *out = info; return VK_SUCCESS;
   }
   const bool yuv = info.format == AHARDWAREBUFFER_FORMAT_Y8Cb8Cr8_420 ||
                    info.format == 0x11 || info.format == 0x32315659;
   if ((!yuv && mali_android_vk_format(info.format) == VK_FORMAT_UNDEFINED) ||
       android.modifier(android.mapper, handle, &info.modifier) || info.modifier != DRM_FORMAT_MOD_LINEAR)
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   struct platform_vector planes = {0};
   int err = android.planes(android.mapper, handle, &planes);
   VkResult result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
   if (!err && yuv && android_yuv_metadata(&planes, &info)) {
      *out = info;
      result = VK_SUCCESS;
   } else if (!err && !yuv && (char *)planes.end - (char *)planes.begin == sizeof(struct platform_plane)) {
      const struct platform_plane *p = planes.begin;
      if (p->offset >= 0 && p->stride > 0 && p->size > 0 && p->increment > 0 && !(p->increment % 8) &&
          p->width > 0 && p->height > 0 &&
          (uint64_t)p->width >= width && (uint64_t)p->height >= height &&
          p->hsub == 1 && p->vsub == 1 &&
          (uint64_t)p->offset <= info.allocation_size &&
          (uint64_t)p->size <= info.allocation_size - p->offset &&
          p->increment == 8 * util_format_get_blocksize(vk_format_to_pipe_format(mali_android_vk_format(info.format))) &&
          (uint64_t)p->stride <= UINT32_MAX && !(p->stride % 64) &&
          (uint64_t)p->stride * height <= (uint64_t)p->size) {
         info.offset = p->offset; info.row_stride = p->stride; info.layer_stride = p->size;
         info.pixel_stride = p->stride / (p->increment / 8);
         *out = info; result = VK_SUCCESS;
      }
   }
   destroy_planes(&planes);
   return result;
}

VkResult mali_android_ahb_info(const AHardwareBuffer *buffer, struct mali_android_buffer *out)
{
   if (!buffer || !android_metadata_available()) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   AHardwareBuffer_Desc desc;
   android.describe(buffer, &desc);
   VkResult r = mali_android_buffer_info(android.handle(buffer), out);
   if (r != VK_SUCCESS) return r;
   return desc.width == out->width && desc.height == out->height && desc.layers == out->layers &&
      desc.format == out->format && (desc.usage & (AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
      AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT | AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER)) ? VK_SUCCESS : VK_ERROR_INVALID_EXTERNAL_HANDLE;
}

VkResult mali_android_image_layout(struct mali_image *image, const struct mali_android_buffer *info)
{
   if (info->yuv.plane_count) {
      const struct vk_image *vi = &image->vk;
      if (!debug_get_bool_option("UNIFIED_YUV", false) ||
          vi->format != info->yuv.format || image->plane_count != info->yuv.plane_count ||
          vi->image_type != VK_IMAGE_TYPE_2D || vi->mip_levels != 1 || vi->array_layers != 1 ||
          vi->samples != 1 || vi->extent.width != info->width || vi->extent.height != info->height ||
          (image->android_external_format ?
             image->android_external_format != mali_android_yuv_token(&info->yuv) :
             info->yuv.swap_chroma))
         return VK_ERROR_INVALID_EXTERNAL_HANDLE;
      struct mali_image_plane_layout layouts[MALI_IMAGE_MAX_PLANES];
      for (unsigned p = 0; p < image->plane_count; p++) {
         const struct mali_android_yuv_plane *plane = &info->yuv.planes[p];
         VkExtent3D extent = mali_image_plane_extent(image, p, 0);
         const struct mali_image_layout_info li = {
            .format = image->planes[p].format, .modifier = DRM_FORMAT_MOD_LINEAR,
            .width = extent.width, .height = extent.height, .depth = 1,
            .samples = 1, .levels = 1, .layers = 1,
            .explicit_row_stride = plane->row_stride,
         };
         if (!mali_image_plane_layout_init(&li, 0, &layouts[p]))
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
         layouts[p].slices[0].offset = plane->offset;
         layouts[p].slices[0].row_stride = plane->row_stride;
         layouts[p].slices[0].surface_stride = layouts[p].slices[0].size = plane->size;
         layouts[p].array_stride = plane->size; layouts[p].data_size = plane->size;
      }
      for (unsigned p = 0; p < image->plane_count; p++) image->planes[p].layout = layouts[p];
      image->size = info->allocation_size;
      image->vk.drm_format_mod = DRM_FORMAT_MOD_LINEAR;
      image->crc_header = 0;
      return VK_SUCCESS;
   }
   const VkFormat fmt = mali_android_vk_format(info->format);
   const struct vk_image *vi = &image->vk;
   if (fmt == VK_FORMAT_UNDEFINED || image->plane_count != 1 || vi->image_type != VK_IMAGE_TYPE_2D ||
       vi->mip_levels != 1 || vi->array_layers != 1 || vi->samples != 1 ||
       vi->extent.width != info->width || vi->extent.height != info->height ||
       util_format_linear(vk_format_to_pipe_format(vi->format)) != util_format_linear(vk_format_to_pipe_format(fmt)))
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   enum pipe_format pf = image->planes[0].format;
   if (info->pixel_stride < vi->extent.width ||
       info->row_stride != (uint64_t)info->pixel_stride * util_format_get_blocksize(pf))
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   struct mali_image_plane_layout layout;
   const struct mali_image_layout_info li = {.format=pf,.modifier=DRM_FORMAT_MOD_LINEAR,
      .width=info->width,.height=info->height,.depth=1,.samples=1,.levels=1,.layers=1};
   if (!mali_image_plane_layout_init(&li, 0, &layout)) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   layout.slices[0].offset = info->offset;
   layout.slices[0].row_stride = info->row_stride;
   layout.slices[0].surface_stride = layout.slices[0].size = info->layer_stride;
   layout.array_stride = info->layer_stride; layout.data_size = info->allocation_size;
   image->planes[0].layout = layout; image->size = info->allocation_size;
   image->vk.drm_format_mod = DRM_FORMAT_MOD_LINEAR;
   image->crc_header = 0;
   return VK_SUCCESS;
}

/* Mesa's platform-neutral runtime intentionally has no Android external-format
 * decoder. Normalize supported tokens at the driver boundary, preserving the
 * AHB handle type and dedicated allocation while omitting the consumed token. */
VkResult
mali_android_external_image_info(struct mali_device *dev,
   const VkImageCreateInfo *in, VkImageCreateInfo *out,
   VkExternalMemoryImageCreateInfo *memory, uint64_t *external_format)
{
   *out = *in; *external_format = 0;
   const VkExternalFormatANDROID *ext = vk_find_struct_const(in->pNext, EXTERNAL_FORMAT_ANDROID);
   if (!ext || !ext->externalFormat) return VK_SUCCESS;
   const VkExternalMemoryImageCreateInfo *mem =
      vk_find_struct_const(in->pNext, EXTERNAL_MEMORY_IMAGE_CREATE_INFO);
   VkFormat format = mali_android_rgb_token_format(ext->externalFormat, NULL);
   if (format == VK_FORMAT_UNDEFINED && android.string_compare &&
       mali_ycbcr_enabled(mali_device_physical(dev)->arch))
      format = mali_android_yuv_token_format(ext->externalFormat, NULL);
   if (!mali_android_buffer_available() || !mem ||
       mem->handleTypes != VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID ||
       format == VK_FORMAT_UNDEFINED || in->format != VK_FORMAT_UNDEFINED ||
       in->imageType != VK_IMAGE_TYPE_2D || in->extent.depth != 1 || in->mipLevels != 1 ||
       in->arrayLayers != 1 || in->samples != 1 || in->flags ||
       in->tiling != VK_IMAGE_TILING_OPTIMAL || in->usage != VK_IMAGE_USAGE_SAMPLED_BIT ||
       in->sharingMode != VK_SHARING_MODE_EXCLUSIVE ||
       !(mali_format_image_features(mali_device_physical(dev), format) & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   vk_foreach_struct_const(stype, item, in->pNext)
      if (stype != VK_STRUCTURE_TYPE_EXTERNAL_FORMAT_ANDROID &&
          stype != VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO)
         return VK_ERROR_FORMAT_NOT_SUPPORTED;
   *memory = *mem; memory->pNext = NULL;
   out->format = format; out->pNext = memory;
   *external_format = ext->externalFormat;
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_CreateSamplerYcbcrConversion(VkDevice device,
   const VkSamplerYcbcrConversionCreateInfo *info, const VkAllocationCallbacks *alloc,
   VkSamplerYcbcrConversion *out)
{
   VK_FROM_HANDLE(mali_device, dev, device);
   *out = VK_NULL_HANDLE;
   const VkExternalFormatANDROID *external = vk_find_struct_const(info->pNext, EXTERNAL_FORMAT_ANDROID);
   if (!external || !external->externalFormat)
      return vk_common_CreateSamplerYcbcrConversion(device, info, alloc, out);
   bool swap = false;
   VkFormat format = mali_android_rgb_token_format(external->externalFormat, &swap);
   bool yuv = false;
   if (format == VK_FORMAT_UNDEFINED && android.string_compare &&
       mali_ycbcr_enabled(mali_device_physical(dev)->arch)) {
      format = mali_android_yuv_token_format(external->externalFormat, &swap);
      yuv = mali_format_is_ycbcr(format);
   }
   if ((!yuv && info->ycbcrModel != VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY) ||
       info->forceExplicitReconstruction)
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   if (!mali_android_buffer_available() ||
       info->format != VK_FORMAT_UNDEFINED || format == VK_FORMAT_UNDEFINED ||
       !(mali_format_image_features(mali_device_physical(dev), format) & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   VkSamplerYcbcrConversionCreateInfo normalized = *info;
   normalized.format = format; normalized.pNext = NULL;
   normalized.components = swap ?
      (VkComponentMapping){VK_COMPONENT_SWIZZLE_B,VK_COMPONENT_SWIZZLE_G,VK_COMPONENT_SWIZZLE_R,VK_COMPONENT_SWIZZLE_A} :
      (VkComponentMapping){VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY,VK_COMPONENT_SWIZZLE_IDENTITY};
   return vk_common_CreateSamplerYcbcrConversion(device, &normalized, alloc, out);
}

/* Allocate exportable raw bytes through Android's existing gralloc backend.
 * The duplicated dma-buf retains the payload after dropping the AHB wrapper.
 * No vendor handle index, kernel allocator ABI or image layout is assumed. */
VkResult mali_android_create_blob_fd(uint64_t size, int *out_fd)
{
   *out_fd = -1;
   if (!mali_android_buffer_available()) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   if (!size || size > UINT32_MAX) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   AHardwareBuffer_Desc desc = {.width = size, .height = 1, .layers = 1,
      .format = AHARDWAREBUFFER_FORMAT_BLOB,
      .usage = AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER |
               AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN};
   AHardwareBuffer *buffer = NULL;
   if (android.allocate(&desc, &buffer)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   struct mali_android_buffer metadata;
   VkResult result = mali_android_ahb_info(buffer, &metadata);
   if (result == VK_SUCCESS && (metadata.format != AHARDWAREBUFFER_FORMAT_BLOB ||
                               metadata.allocation_size < size))
      result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
   if (result == VK_SUCCESS) {
      *out_fd = fcntl(metadata.fd, F_DUPFD_CLOEXEC, 0);
      if (*out_fd < 0)
         result = (errno == EMFILE || errno == ENFILE) ? VK_ERROR_TOO_MANY_OBJECTS : VK_ERROR_OUT_OF_HOST_MEMORY;
   }
   android.release(buffer);
   return result;
}

static void (*public_ahb_acquire)(AHardwareBuffer *);
static void (*public_ahb_release)(AHardwareBuffer *);
static util_once_flag public_ahb_once = UTIL_ONCE_FLAG_INIT;
static void public_ahb_init(void)
{
   void *lib = dlopen("libnativewindow.so", RTLD_NOW | RTLD_LOCAL);
   if (!lib) return;
   *(void **)(&public_ahb_acquire) = dlsym(lib, "AHardwareBuffer_acquire");
   *(void **)(&public_ahb_release) = dlsym(lib, "AHardwareBuffer_release");
}
bool mali_android_buffer_acquire(AHardwareBuffer *buffer)
{
   util_call_once(&public_ahb_once, public_ahb_init);
   if (!buffer || !public_ahb_acquire || !public_ahb_release) return false;
   public_ahb_acquire(buffer);
   return true;
}

void mali_android_buffer_release(AHardwareBuffer *buffer)
{
   if (!buffer) return;
   util_call_once(&public_ahb_once, public_ahb_init);
   if (public_ahb_release) public_ahb_release(buffer);
}

VkResult mali_android_allocate_memory(struct mali_device *dev, const VkMemoryAllocateInfo *info,
                                      const VkAllocationCallbacks *alloc, VkDeviceMemory *out)
{
   *out = VK_NULL_HANDLE;
   if (!mali_android_buffer_available()) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   const VkImportAndroidHardwareBufferInfoANDROID *import = vk_find_struct_const(info->pNext, IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID);
   const VkExportMemoryAllocateInfo *export = vk_find_struct_const(info->pNext, EXPORT_MEMORY_ALLOCATE_INFO);
   if (export && (export->handleTypes & ~VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID))
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   const VkMemoryDedicatedAllocateInfo *dedicated = vk_find_struct_const(info->pNext, MEMORY_DEDICATED_ALLOCATE_INFO);
   VK_FROM_HANDLE(mali_image, image, dedicated ? dedicated->image : VK_NULL_HANDLE);
   VK_FROM_HANDLE(mali_buffer, buffer, dedicated ? dedicated->buffer : VK_NULL_HANDLE);
   AHardwareBuffer *ahb = import ? import->buffer : NULL;
   bool created = false;
   if (!ahb) {
      if (!export || export->handleTypes != VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID)
         return VK_ERROR_INVALID_EXTERNAL_HANDLE;
      AHardwareBuffer_Desc desc = {.layers=1};
      if (image) {
         desc.width=image->vk.extent.width; desc.height=image->vk.extent.height;
         desc.format=mali_android_hal_format(image->vk.format);
         desc.usage=AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                    AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
                    mali_wsi_swapchain_no_afbc(mali_device_physical(dev)->arch);
         if (!desc.format) return VK_ERROR_FORMAT_NOT_SUPPORTED;
      } else {
         /* BLOB width is the Vulkan allocation size, not the size of a
          * dedicated buffer which may be smaller than its allocation. */
         uint64_t size = info->allocationSize;
         if (buffer && (!buffer->ahb || buffer->vk.size > size))
            return VK_ERROR_INVALID_EXTERNAL_HANDLE;
         if (!size || size > UINT32_MAX) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
         desc.width=size;desc.height=1;desc.format=AHARDWAREBUFFER_FORMAT_BLOB;
         desc.usage=AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER | AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
      }
      if (android.allocate(&desc, &ahb)) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
      created = true;
   }
   struct mali_android_buffer metadata;
   VkResult result = mali_android_ahb_info(ahb, &metadata);
   if (result != VK_SUCCESS) goto done;
   if (image) {
      if (!image->ahb) {result=VK_ERROR_INVALID_EXTERNAL_HANDLE;goto done;}
      struct mali_image validation = *image;
      result = mali_android_image_layout(&validation, &metadata);
      if (result != VK_SUCCESS) goto done;
   } else if (metadata.format != AHARDWAREBUFFER_FORMAT_BLOB ||
              (buffer && buffer->vk.size > metadata.allocation_size)) {
      result=VK_ERROR_INVALID_EXTERNAL_HANDLE;goto done;
   }
   if ((!(created && image) && info->memoryTypeIndex != MALI_MEMORY_TYPE_EXTERNAL_CACHED) ||
       (!created && info->allocationSize != metadata.allocation_size) ||
       (created && image && info->allocationSize != 0) ||
       (created && !image && info->allocationSize > metadata.allocation_size)) {
      result=VK_ERROR_INVALID_EXTERNAL_HANDLE;goto done;
   }
   int fd = fcntl(metadata.fd, F_DUPFD_CLOEXEC, 0);
   if (fd < 0) { result=errno==EMFILE?VK_ERROR_TOO_MANY_OBJECTS:VK_ERROR_OUT_OF_HOST_MEMORY; goto done; }
   /* Preserve address/device-mask metadata through the internalFD import.
    * Do not forward the AHB import itself and recursively re-enter this path. */
   const VkMemoryAllocateFlagsInfo *requested_flags =
      vk_find_struct_const(info->pNext, MEMORY_ALLOCATE_FLAGS_INFO);
   VkMemoryAllocateFlagsInfo flags;
   if (requested_flags) {
      flags = *requested_flags;
      flags.pNext = NULL;
   }
   const VkImportMemoryFdInfoKHR fi = {.sType=VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .pNext=requested_flags ? &flags : NULL,
      .handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,.fd=fd};
   const VkMemoryAllocateInfo ai = {.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.pNext=&fi,
      .allocationSize=metadata.allocation_size,.memoryTypeIndex=MALI_MEMORY_TYPE_EXTERNAL_CACHED};
   result=mali_AllocateMemory(mali_device_to_handle(dev),&ai,alloc,out);
   if (result != VK_SUCCESS) {close(fd);goto done;}
   VK_FROM_HANDLE(mali_device_memory, mem, *out);
   if (!created) android.acquire(ahb);
   mem->android_buffer=ahb;mem->android_layout=metadata;
   mem->android_dedicated_image = image ? dedicated->image : VK_NULL_HANDLE;
   mem->android_dedicated_buffer = buffer ? dedicated->buffer : VK_NULL_HANDLE;
   if (created && !image) mem->vk.size = info->allocationSize;
   mem->vk.import_handle_type=import&&import->buffer?VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID:0;
   mem->vk.export_handle_types=export?export->handleTypes:0;
   created=false;
done:
   if (created) android.release(ahb);
   return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_GetAndroidHardwareBufferPropertiesANDROID(VkDevice device, const AHardwareBuffer *buffer,
                                                VkAndroidHardwareBufferPropertiesANDROID *props)
{
   VK_FROM_HANDLE(mali_device, dev, device);
   if (!mali_android_buffer_available())
      return mali_android_blob_properties(device, buffer, props);
   struct mali_android_buffer info;
   VkResult r = mali_android_ahb_info(buffer, &info);
   if (r != VK_SUCCESS) return r;
   const bool yuv = info.yuv.plane_count != 0;
   if (yuv && !mali_ycbcr_enabled(mali_device_physical(dev)->arch))
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   props->allocationSize=info.allocation_size;
   props->memoryTypeBits=1u << MALI_MEMORY_TYPE_EXTERNAL_CACHED;
   vk_foreach_struct(stype,ext,props->pNext) {
      VkFormat format = yuv ? info.yuv.format : mali_android_vk_format(info.format);
      VkFormatFeatureFlags features=format==VK_FORMAT_UNDEFINED?0:mali_format_image_features(mali_device_physical(dev),format);
      const uint64_t external_format = yuv ? mali_android_yuv_token(&info.yuv) : mali_android_rgb_token(format);
      VkFormatFeatureFlags external_features = 0;
      if (external_format) {
         external_features = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
            VK_FORMAT_FEATURE_MIDPOINT_CHROMA_SAMPLES_BIT |
            VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT;
         if (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
            external_features |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
               VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT;
      }
      if (yuv) external_features = features;
      if (stype==VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_2_ANDROID) {
         VkAndroidHardwareBufferFormatProperties2ANDROID *p=ext;
         p->format=yuv ? VK_FORMAT_UNDEFINED : format;p->externalFormat=external_format;p->formatFeatures=external_features;
         p->samplerYcbcrConversionComponents=(VkComponentMapping){0,0,0,0};
         p->suggestedYcbcrModel=yuv ? VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601 : VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY;
         p->suggestedYcbcrRange=yuv ? VK_SAMPLER_YCBCR_RANGE_ITU_NARROW : VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
         p->suggestedXChromaOffset=p->suggestedYChromaOffset=VK_CHROMA_LOCATION_COSITED_EVEN;
      }
      if (stype==VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID) {
         VkAndroidHardwareBufferFormatPropertiesANDROID *p=ext;
         p->format=yuv ? VK_FORMAT_UNDEFINED : format;p->externalFormat=external_format;p->formatFeatures=external_features;
         p->samplerYcbcrConversionComponents=(VkComponentMapping){0,0,0,0};
         p->suggestedYcbcrModel=yuv ? VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601 : VK_SAMPLER_YCBCR_MODEL_CONVERSION_RGB_IDENTITY;
         p->suggestedYcbcrRange=yuv ? VK_SAMPLER_YCBCR_RANGE_ITU_NARROW : VK_SAMPLER_YCBCR_RANGE_ITU_FULL;
         p->suggestedXChromaOffset=p->suggestedYChromaOffset=VK_CHROMA_LOCATION_COSITED_EVEN;
      }
   }
   return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL
mali_GetMemoryAndroidHardwareBufferANDROID(VkDevice device,
   const VkMemoryGetAndroidHardwareBufferInfoANDROID *info, AHardwareBuffer **out)
{
   VK_FROM_HANDLE(mali_device_memory, mem, info->memory);
   *out=NULL;
   if (!mem->android_buffer || !(mem->vk.export_handle_types & VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID))
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   android.acquire(mem->android_buffer);*out=mem->android_buffer;return VK_SUCCESS;
}
