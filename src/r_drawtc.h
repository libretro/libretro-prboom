/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 * DESCRIPTION:
 *      Public interface of the truecolor (XRGB8888 / XRGB2101010)
 *      drawer core.  r_draw.c's public entry points forward here when
 *      VID_TRUECOLOR; the signatures mirror the 16bpp API with 32-bit
 *      pixels.
 *
 *-----------------------------------------------------------------------------*/

#ifndef __R_DRAWTC__
#define __R_DRAWTC__

#include "r_defs.h"
#include "r_draw.h"

/* Drawer dispatch (shared instantiation; format handled inside the LUTs). */
R_DrawColumn_f R_GetDrawColumnFuncTC(enum column_pipeline_e type,
                                     enum draw_filter_type_e filter,
                                     enum draw_filter_type_e filterz);
R_DrawSpan_f   R_GetDrawSpanFuncTC(enum draw_filter_type_e filter,
                                   enum draw_filter_type_e filterz);
void R_DrawSpanTC(draw_span_vars_t *dsvars);

/* Wall-run kernel + classification (draw-record replay). */
int  R_WallColumnKernelClassTC(R_DrawColumn_f fn);
void R_DrawWallColumnRunTC(const draw_column_vars_t *const *cols, int n, int pointz);

/* Composed colour tables (direct sprite/voxel paths). */
const uint32_t *R_ComposedColormapTC(const lighttable_t *colormap);
const uint32_t *R_ComposedPaletteTC(void);

/* Batch buffer lifecycle. */
void R_ResetColumnBufferTC(void);
void R_InitBufferTC(void);

/* Raven translucency selection. */
void R_SetSpriteTranslucencyTC(int mode);
void R_SetTransAlphaTC(int a32);

/* Underwater tint + flat mean colour. */
uint32_t R_FlatAverageColorTC(int picnum);
void     R_TintViewTC(uint32_t color);

/* Dynamic-light colour tints. */
void R_TintLUTTC(uint32_t *dst, const uint32_t *src, int ar, int ag, int ab);
void R_WallTintRunTC(int x, int yl, int yh, int ar, int ag, int ab);
void R_TintSpanTC(int y, int x1, int x2, int ar, int ag, int ab);

/* Submerged-water volume shading. */
void R_WaterDarkenSpanTC(int y, int x1, int x2, int surf_y);
void R_WaterSurfaceBandTC(int x, int yl, int yh, int surf_line, int band_h);
void R_WaterSurfaceLiftTC(int x, int y0, int y1, int bandtop);
void R_WaterDarkenColumnTC(int x, int yl, int yh, int surf_y);

#endif /* __R_DRAWTC__ */
