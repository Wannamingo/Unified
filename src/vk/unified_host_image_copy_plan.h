// SPDX-License-Identifier: MIT
// Internal HOST prototype, not a Vulkan entrypoint or feature declaration.
#ifndef UNIFIED_HOST_IMAGE_COPY_PLAN_H
#define UNIFIED_HOST_IMAGE_COPY_PLAN_H
#include "unified_host_image_copy_plane.h"
struct unified_host_level {
 uint64_t offset,row_stride,surface_stride,array_stride;
 uint32_t width,height,depth,layers;
 uint32_t block_width,block_height,block_bytes,tile_side;
 bool image_3d;
};
struct unified_host_texel_region {
 int32_t x,y,z;
 uint32_t width,height,depth,base_layer,layer_count;
 uint32_t memory_row_length,memory_image_height;
};
struct unified_host_copy_plan {
 struct unified_host_plane plane;
 struct unified_host_region blocks;
 uint64_t host_row_stride,host_surface_stride,host_span;
};
// The level comes from the driver's already-selected format/plane/mip layout.
// image_offset includes the VkDeviceMemory binding offset, not level.offset.
// No AFBC/MSAA, MEMCPY layout mode, combinedDS packing or YUV conversion here.
// Pure planning: no mapped-memory access or writes on either success/failure.
bool unified_host_image_plan(const struct unified_host_level *level,
 const struct unified_host_texel_region *region,uint8_t *mapping,
 uint64_t available,uint64_t image_offset,struct unified_host_copy_plan *out);
#endif
