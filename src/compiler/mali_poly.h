/* SPDX-License-Identifier: MIT */
#ifndef MALI_POLY_H
#define MALI_POLY_H
#include "nir.h"
#include "poly/nir/poly_nir.h"
#include "mali_poly_winding.h"

/* Internal ABI, independent of application descriptors/push constants.
 * The caller supplies a private address block and bounds every dispatch
 * to the exact number of valid primitives/patches. This is not an
 * advertised geometry/tessellation feature until draw integration passes. */
struct mali_poly_params {
   uint64_t vertex, geometry, tess, ro_sink;
   uint64_t stat_query[4];
   uint64_t raster_output;
   uint64_t printf_buffer;
   int32_t first_vertex;
   uint32_t base_instance, provoking_last;
   uint64_t draw_count;
   uint32_t patch_count, tess_index_limit;
   uint64_t tess_scratch;
   uint64_t self, indirect_params, indirect_count, attribs, tess_index_base;
   uint32_t draw_id, max_patches, tess_index_elements, attrib_stride[16], attrib_offset[16];
   uint32_t grids[3][3];
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
bool mali_poly_lower_tcs(nir_shader *tcs);
bool mali_poly_lower_tes_raster(nir_shader *tes);
bool mali_poly_lower_tes_compute(nir_shader *tes, uint64_t *outputs);
bool mali_poly_lower_params(nir_shader *nir, unsigned push_base);
nir_shader *mali_poly_tessellator(const nir_shader_compiler_options *options,
                                enum tess_primitive_mode domain, unsigned mode,
                                uint32_t *scratch_stride);
nir_shader *mali_poly_tess_prefix(const nir_shader_compiler_options *options);
nir_shader *mali_poly_tess_indirect(const nir_shader_compiler_options *options);
nir_shader *mali_poly_tess_gs_setup(const nir_shader_compiler_options *options,
                                  const struct poly_gs_info *gs, unsigned arity);
#endif
