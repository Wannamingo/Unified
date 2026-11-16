/* SPDX-License-Identifier: MIT
 * Reuse the unmodified Mesa/Microsoft reference tessellator. */
#include "poly/cl/tessellator.h"
void mali_poly_tess_triangle(constant struct poly_tess_params *p, uint patch, uint mode)
{
   poly_tess_tri_process(p, patch, mode);
}
void mali_poly_tess_quad(constant struct poly_tess_params *p, uint patch, uint mode)
{
   poly_tess_quad_process(p, patch, mode);
}
void mali_poly_tess_isoline(constant struct poly_tess_params *p, uint patch, uint mode)
{
   poly_tess_isoline_process(p, patch, mode);
}
