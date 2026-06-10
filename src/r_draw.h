/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2000 by
 *  Jess Haas, Nicolas Kalkhof, Colin Phipps, Florian Schulze
 *  Copyright 2005, 2006 by
 *  Florian Schulze, Colin Phipps, Neil Stevens, Andrey Budko
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
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 *  02111-1307, USA.
 *
 * DESCRIPTION:
 *      System specific interface stuff.
 *
 *-----------------------------------------------------------------------------*/

#ifndef __R_DRAW__
#define __R_DRAW__

#include "r_defs.h"

enum column_pipeline_e {
  RDC_PIPELINE_STANDARD,
  RDC_PIPELINE_TRANSLATED,
  RDC_PIPELINE_FUZZ,
  RDC_PIPELINE_MAXPIPELINES,
};

// Used to specify what kind of filering you want
enum draw_filter_type_e {
  RDRAW_FILTER_NONE,
  RDRAW_FILTER_POINT,
  RDRAW_FILTER_LINEAR,
  RDRAW_FILTER_ROUNDED,
  RDRAW_FILTER_MAXFILTERS
};

// Used to specify what kind of column edge rendering to use on masked 
// columns. SQUARE = standard, SLOPED = slope the column edge up or down
// based on neighboring columns
enum sloped_edge_type_e {
  RDRAW_MASKEDCOLUMNEDGE_SQUARE,
  RDRAW_MASKEDCOLUMNEDGE_SLOPED
};

// Packaged into a struct - POPE
typedef struct {
  int                 x;
  int                 yl;
  int                 yh;
  fixed_t             z; // the current column z coord
  fixed_t             iscale;
  fixed_t             texturemid;
  int                 texheight;    // killough
  fixed_t             texu; // the current column u coord
  const uint8_t       *source; // first pixel in a column
  const uint8_t       *prevsource; // first pixel in previous column
  const uint8_t       *nextsource; // first pixel in next column
  /* Per-texel fullbright mask aligned to `source` (same column height and
   * row index): where mask[texel] != 0 the texel ignores the distance
   * light and draws at full brightness.  NULL when the texture has no
   * brightmap. */
  const uint8_t       *brightmask;
  const lighttable_t  *colormap;
  const lighttable_t  *nextcolormap;
  const uint8_t       *translation;
  int                 edgeslope; // OR'ed RDRAW_EDGESLOPE_*
  // 1 if R_DrawColumn* is currently drawing a masked column, otherwise 0
  int                 drawingmasked;
  enum sloped_edge_type_e edgetype;
} draw_column_vars_t;

void R_SetDefaultDrawColumnVars(draw_column_vars_t *dcvars);

typedef struct {
  int                 y;
  int                 x1;
  int                 x2;
  fixed_t             z; // the current span z coord
  fixed_t             xfrac;
  fixed_t             yfrac;
  fixed_t             xstep;
  fixed_t             ystep;
  const uint8_t       *source; // start of a 64*64 tile image
  const lighttable_t  *colormap;
  const lighttable_t  *nextcolormap;
} draw_span_vars_t;

typedef struct {
  unsigned short *short_topleft;
  unsigned int   *int_topleft;

  enum draw_filter_type_e filterwall;
  enum draw_filter_type_e filterfloor;
  enum draw_filter_type_e filtersprite;
  enum draw_filter_type_e filterz;
  enum draw_filter_type_e filterpatch;

  enum sloped_edge_type_e sprite_edges;
  enum sloped_edge_type_e patch_edges;

  // Used to specify an early-out magnification threshold for filtering.
  // If a texture is being minified (dcvars.iscale > rdraw_magThresh), then it
  // drops back to point filtering.
  fixed_t mag_threshold;
} draw_vars_t;

extern draw_vars_t drawvars;

extern int diminished_lighting;       /* General > Video menu setting; applied via R_ApplyDiminishedLighting (r_main.h) */
/* Smooth shading uses a PRIVATE luma ramp finer than VID_NUMCOLORWEIGHTS (64),
 * so distance gradients band far less at high output resolutions, WITHOUT
 * enlarging the shared V_Palette16 (which the filtered/translucency per-pixel
 * paths stride through -- growing it would risk their cache behaviour).  The
 * ramp is built lazily, keyed on the active V_Palette16, and read only by the
 * composed-LUT build.  256 is the natural ceiling: source intensity is 8-bit,
 * so finer than 256 luma steps cannot represent anything new.  Must stay a
 * power of two (the band->weight scaling uses a shift). */
#define SMOOTH_WEIGHTS 256
/* log2(SMOOTH_WEIGHTS / NUMCOLORMAPS); NUMCOLORMAPS is 32, so 256/32 = 8 = 2^3.
 * Used to reduce the distance-term shift when scaling the band light formula
 * up to SMOOTH_WEIGHTS resolution.  Keep in sync if either constant changes. */
#define SMOOTH_WEIGHTS_SHIFT 3

extern int r_smooth_shading;          /* set by R_ApplyDiminishedLighting when diminished_lighting==1 (Smooth) */
extern int r_fine_lightweight;        /* Smooth mode: sub-band light weight 0..SMOOTH_WEIGHTS-1 (max=bright), -1 if none */
extern const lighttable_t *r_fine_colormap; /* colormap ptr r_fine_lightweight was computed for (self-validation) */

extern uint8_t playernumtotrans[MAXPLAYERS]; // CPhipps - what translation table for what player
extern uint8_t       *translationtables;

typedef void (*R_DrawColumn_f)(draw_column_vars_t *dcvars);

/* Wall-run kernel (r_draw.c): classification of drawers it reproduces,
 * and row-major rasterization of an x-adjacent record run.  Used by the
 * draw-record replay in r_drawcmd.c. */
int R_WallColumnKernelClass(R_DrawColumn_f fn);
const uint16_t *R_ComposedColormap(const lighttable_t *colormap);
const uint16_t *R_ComposedPalette(void);
void R_DrawWallColumnRun(const draw_column_vars_t *const *cols, int n, int pointz);
R_DrawColumn_f R_GetDrawColumnFunc(enum column_pipeline_e type,
                                   enum draw_filter_type_e filter,
                                   enum draw_filter_type_e filterz);

// Span blitting for rows, floor/ceiling. No Spectre effect needed.
typedef void (*R_DrawSpan_f)(draw_span_vars_t *dsvars);
R_DrawSpan_f R_GetDrawSpanFunc(enum draw_filter_type_e filter,
                               enum draw_filter_type_e filterz);
void R_DrawSpan(draw_span_vars_t *dsvars);

/* When nonzero, R_DrawSpan blends its output 50/50 against the framebuffer
 * instead of overwriting it; set around a translucent 3D-floor surface. */
extern int r_span_translucent;

/* Underwater tint: mean colour of a flat, and a 50/50 blend of the whole 3D
 * view toward a colour (applied when the camera is inside a 3D-floor water
 * volume). */
uint16_t R_FlatAverageColor565(int picnum);
void R_TintView(uint16_t color);

void R_InitBuffer(int width, int height);

// Initialize color translation tables, for player rendering etc.
void R_InitTranslationTables(void);

// haleyjd 09/13/04: new function to call from main rendering loop
// which gets rid of the unnecessary reset of various variables during
// column drawing.
void R_ResetColumnBuffer(void);

/* Raven translucent sprite mode: 0 opaque, 1 MF_SHADOW (50%), 2 MF_ALTSHADOW (25%) */
void R_SetSpriteTranslucency(int mode);
void R_SetTransAlpha(int a32);   /* blend weight alpha*32 (0..32) for modes 3/4 */

#endif
