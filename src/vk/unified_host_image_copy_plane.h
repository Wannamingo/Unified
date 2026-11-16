// SPDX-License-Identifier: MIT
// Internal prototype only. Block coordinates, distinct host/image mappings.
// No Vulkan entrypoint, extension exposure, cache maintenance or GPU accesses.
#ifndef UNIFIED_HOST_IMAGE_COPY_PLANE_H
#define UNIFIED_HOST_IMAGE_COPY_PLANE_H
#include <stdbool.h>
#include <stdint.h>
struct unified_host_plane {
 uint8_t *mapping;
 uint64_t available,offset,row_stride,surface_stride;
 uint32_t width_blocks,height_blocks,depth_slices,block_bytes;
 uint32_t tile_side; // 0 linear; 4 compressed blocks;16 uncompressed texels.
};
struct unified_host_region {uint32_t x,y,z,width,height,depth;};
// Zero host strides mean tightly packed. Invalid ranges are rejected before
// any write. Every block is copied with memcpy, including unaligned pointers.
bool unified_host_plane_copy(const struct unified_host_plane *plane,
 const struct unified_host_region *region,void *host,uint64_t host_available,
 uint64_t host_row_stride,uint64_t host_surface_stride,bool to_image);
#endif
