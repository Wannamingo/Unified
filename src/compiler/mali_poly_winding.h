/* SPDX-License-Identifier: MIT */
#ifndef MALI_POLY_WINDING_H
#define MALI_POLY_WINDING_H
#include <stdbool.h>

/* libpoly receives the domain-origin-adjusted tessellator winding. The
 * hardware front-face convention must apply the same flip when TES feeds
 * rasterization directly. GS output defines its own winding instead.
 * See pinned Mesa asahi/vulkan/hk_cmd_draw.c (tess args and CULL packing). */
static inline bool
mali_poly_tess_winding(bool surface, bool shader_ccw, bool lower_left)
{
   return surface && (shader_ccw != lower_left);
}
#endif
