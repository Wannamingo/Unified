/* SPDX-License-Identifier: MIT */
#ifndef MALI_POLY_H
#define MALI_POLY_H
#include "nir.h"
#include "poly/nir/poly_nir.h"

/* Internal ABI, independent of application descriptors/push constants.
 * The caller supplies a private address block and bounds every dispatch
 * to the exact number of valid primitives/patches. This is not an
 * advertised geometry/tessellation feature until draw integration passes. */
struct mali_poly_params {
   uint64_t vertex, geometry, unused_tess, ro_sink;
   uint64_t stat_query[4];
   uint64_t raster_output;
   uint64_t printf_buffer;
   int32_t first_vertex;
   uint32_t base_instance, provoking_last;
   uint64_t draw_count;
   uint32_t patch_count, tess_index_limit;
};

/* Set this bit when push_base names a private root pointer in the driver
 * sysval block, rather than a block in application push constants. */
#define MALI_POLY_INDIRECT_ROOT (1u << 31)
#define MALI_POLY_PUSH_CONST_BASE 8192u

struct mali_poly_geometry {
   nir_shader *main, *count, *pre, *raster;
   struct poly_gs_info info;
};

bool mali_poly_lower_geometry(nir_shader *gs, struct mali_poly_geometry *out);
void mali_poly_geometry_finish(struct mali_poly_geometry *p);
bool mali_poly_lower_vertex(nir_shader *vs);
bool mali_poly_lower_params(nir_shader *nir, unsigned push_base);
#endif
