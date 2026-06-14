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
 *      The actual span/column drawing functions.
 *      Here find the main potential for optimization,
 *       e.g. inline assembly, different algorithms.
 *
 *-----------------------------------------------------------------------------*/

#include "doomstat.h"
#include "w_wad.h"
#include "r_main.h"
#include "i_system.h"
#include "r_draw.h"
#include "r_filter.h"
#include "v_video.h"
#include "st_stuff.h"
#include "g_game.h"
#include "am_map.h"
#include "lprintf.h"

/* Wall-run kernel vector paths (see R_DrawWallColumnRun): vectorize
 * the per-lane frac mask/shift/step arithmetic of the dense band and
 * pair the texel/table lookups, which stay scalar on both ISAs (no
 * byte gather below AVX2 and none on NEON).  SSE2 is baseline on
 * x86-64; NEON is baseline on AArch64 and opt-in on ARMv7
 * (-mfpu=neon).  Everything else keeps the portable band loop.  The
 * frac adds run in unsigned lanes on both paths, the same bits as the
 * signed C arithmetic mod 2^32. */
#if defined(__SSE2__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2) || defined(_M_X64)
#define WALL_RUN_SSE2 1
#include <emmintrin.h>
#elif defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(_M_ARM64)
#define WALL_RUN_NEON 1
#include <arm_neon.h>
#endif


#if defined(__SSE2__)
#include <emmintrin.h>
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#endif
#include <string.h>

//
// All drawing to the view buffer is accomplished in this file.
// The other refresh files only know about ccordinates,
//  not the architecture of the frame buffer.
// Conveniently, the frame buffer is a linear one,
//  and we need only the base address,
//  and the total size == width*height*depth/8.,
//

#ifdef PRBOOM_RENDER_PROFILE
double prof_wallfill_usec = 0.0;  /* us spent writing wall/sprite columns to the framebuffer (R_FlushColumns) */
double prof_storewall_usec = 0.0; /* us spent in R_StoreWallRange (wall seg setup + RenderSegLoop) */
double prof_findplane_usec = 0.0; /* us spent in R_FindPlane (visplane hash + alloc) */
double prof_sprproj_usec = 0.0;   /* us spent in R_ProjectSprite (sprite projection/selection, inside bsp+walls) */
#endif

uint8_t *viewimage;
int  viewwidth;
int  scaledviewwidth;
int  viewheight;

/* Shared composed colormap+palette table.
 *
 * Point-sampled drawing turns an 8-bit texel into a 16-bit pixel via two
 * dependent lookups plus index arithmetic: V_Palette16[colormap[texel]*64
 * + 63] (the colormap applies the per-distance light level, V_Palette16
 * converts the lit index to RGB565 at full/point weight 63).  Because the
 * colormap is constant across a span or column, that whole expression is
 * a fixed function of the texel, so it can be pre-composed into a single
 * 256-entry 8bpp->16bpp table and the inner loop reduced to one lookup.
 *
 * The table is shared by the floor/ceiling spans AND the wall/sprite
 * point columns: both draw their colormaps from the same set of light-
 * level colormaps (R_ColourMap / planezlight / fixedcolormap), so a span
 * and a column at the same light level use the identical colormap pointer
 * and reuse the same composed table.  Keying on the colormap pointer means
 * the 256-entry build happens once per distinct light level per frame and
 * amortises across every span and column at that level -- which is why a
 * column, too short to amortise a private rebuild, still benefits here.
 * Keying also on V_Palette16 rebuilds on a palette/gamma/video-mode
 * change.  Restricted to point sampling with a single colormap: the
 * filtered (Linear/Rounded UV) and dithered (LinearZ) paths blend several
 * palette weights / two colormaps per pixel and cannot use one table. */
static const lighttable_t *composed_cm  = NULL;
static const uint16_t     *composed_pal = NULL;
static uint16_t            composed_lut[256];

/* Smooth (CRY-style) distance shading.
 *
 * The Atari Jaguar port of Doom applied distance light entirely as a
 * continuous luma (Y) bias in the blitter (the B_IINC register), leaving
 * each texel's hue/saturation untouched -- one fresh 0..255 light value
 * per wall column, interpolated linearly with depth, with no quantisation
 * to a fixed set of light maps.  The DOS/PC engine instead snaps distance
 * light to one of NUMCOLORMAPS (32) discrete colormaps, each of which also
 * re-points the texel to the nearest palette index; that double snap is
 * what produces the characteristic light banding on walls and floors.
 *
 * V_Palette16 already stores every palette colour expanded across
 * VID_NUMCOLORWEIGHTS (64) continuous luma weights, computed in RGB space
 * and packed to 565 -- structurally identical to the Jaguar's per-column
 * Y value, only precomputed into a table rather than applied by hardware.
 * The point path normally ignores that ramp: it reads weight 63 (full
 * bright) and takes all its distance light from colormap[i]'s index snap.
 *
 * Smooth mode keeps the SAME single-lookup inner loop (so the per-pixel
 * cost is identical to the banded point path) but builds the 256-entry
 * table differently: it recovers the colormap's distance-light level from
 * its offset against the active base (fullcolormap; sector and fixed
 * colormaps are all expressed as fullcolormap + level*256), maps that
 * 0..(NUMCOLORMAPS-1) level onto the 64-weight luma axis, and reads
 * V_Palette16[ texel*64 + weight ].  Distance darkening now scales luma
 * continuously, like CRY, instead of snapping the palette index.  The
 * texel's own colormap remap (translations, invuln inversion baked into
 * colormap[i]) is preserved; only the brightness selection changes. */
int r_smooth_shading = 0; /* set by R_ApplyDiminishedLighting (diminished_lighting==1, Smooth) */

/* Fine (sub-band) light weight side channel for Smooth mode.
 *
 * The band-level Smooth path recovers light from the colormap pointer,
 * which the renderer has already snapped to one of NUMCOLORMAPS (32)
 * bands -- so it cannot represent light variation finer than 32 steps even
 * though V_Palette16 carries 64 luma weights.  R_ColourMap (walls/sprites)
 * and the plane light selection (floors/ceilings) additionally publish the
 * light level here at the full 0..(VID_NUMCOLORWEIGHTS-1) resolution,
 * computed from the same formulas at 2x granularity (63 = brightest, 0 =
 * darkest), so band boundaries still agree with Default.  When >= 0 the
 * Smooth LUT uses it directly; -1 means "no fine value published, fall
 * back to band recovery" (e.g. the patch/HUD path, or a fixedcolormap).
 *
 * This is the "keep all the precision until the final step" refinement the
 * R_ColourMap comment anticipates.  Capping at 64 matches what the weight
 * ramp can actually express, which also bounds the composed-LUT rebuild
 * rate: at most VID_NUMCOLORWEIGHTS distinct tables per frame rather than
 * the unbounded per-column rebuilds a finer-than-ramp value would force. */
int r_fine_lightweight = -1;
const lighttable_t *r_fine_colormap = NULL; /* pointer r_fine_lightweight was computed for */
static int composed_weight = -2; /* cache key companion; -2 = never built */

/* Private fine luma ramp for Smooth shading: [colour][SMOOTH_WEIGHTS].
 * Rebuilt lazily when the active palette (V_Palette16) changes -- gamma swap,
 * invuln flash, palette blend -- so it always matches the rest of the
 * renderer's colours.  Each colour's full-bright RGB565 is taken from
 * V_Palette16 at its top weight, decomposed to channels, and rescaled across
 * SMOOTH_WEIGHTS steps the same way V_Palette16 scales across its 64 -- just
 * finer.  Only the composed-LUT build reads this; the shared V_Palette16 and
 * every per-pixel path that strides it are left exactly as they were. */
static uint16_t       smooth_ramp[256 * SMOOTH_WEIGHTS];
static const uint16_t *smooth_ramp_pal = NULL;

static void R_BuildSmoothRamp(void)
{
   int i, w;
   for (i = 0; i < 256; i++)
   {
      /* Full-bright 565 of this palette entry (weight 63 in V_Palette16). */
      uint16_t full = V_Palette16[ i*VID_NUMCOLORWEIGHTS + (VID_NUMCOLORWEIGHTS-1) ];
#if defined(ABGR1555)
      int r = (full      ) & 0x1f;
      int g = (full >>  5) & 0x1f;
      int b = (full >> 10) & 0x1f;
#else
      int r = (full >> 11) & 0x1f;
      int g = (full >>  5) & 0x3f;
      int b = (full      ) & 0x1f;
#endif
      uint16_t *row = &smooth_ramp[i * SMOOTH_WEIGHTS];
      for (w = 0; w < SMOOTH_WEIGHTS; w++)
      {
         /* t in [0,1]; rounded scale of each channel, matching the engine's
          * (channel * t + 0.5) intent but in fixed point (no float in the
          * hot build).  +(SMOOTH_WEIGHTS-1)/2 rounds to nearest. */
         int nr = (r * w + (SMOOTH_WEIGHTS-1)/2) / (SMOOTH_WEIGHTS-1);
         int ng = (g * w + (SMOOTH_WEIGHTS-1)/2) / (SMOOTH_WEIGHTS-1);
         int nb = (b * w + (SMOOTH_WEIGHTS-1)/2) / (SMOOTH_WEIGHTS-1);
#if defined(ABGR1555)
         row[w] = (uint16_t)((nb<<10) | (ng<<5) | nr);
#else
         row[w] = (uint16_t)((nr<<11) | (ng<<5) | nb);
#endif
      }
   }
   smooth_ramp_pal = V_Palette16;
}

static INLINE const uint16_t *R_GetComposedColormap(const lighttable_t *colormap)
{
   /* The fine weight is only valid for the exact colormap pointer it was
    * published against (R_ColourMap / the plane span setup).  Sprite paths
    * that assign vis->colormap directly -- spectre NULL, fixedcolormap,
    * FF_FULLBRIGHT -> fullcolormap -- never publish a fine weight, so the
    * pointer check rejects a stale value and falls back to band recovery. */
   int fine = (r_smooth_shading && colormap == r_fine_colormap)
              ? r_fine_lightweight : -1;
   /* In Smooth mode the fine weight participates in the cache key, since two
    * columns sharing a colormap band can still differ in sub-band light. */
   int want_weight = (r_smooth_shading) ? fine : -2;

   if (colormap != composed_cm || V_Palette16 != composed_pal
       || want_weight != composed_weight)
   {
      int i;

      if (r_smooth_shading && fullcolormap)
      {
         int  weight;
         long off  = (long)(colormap - fullcolormap);
         int  band = (int)(off >> 8);
         /* A distance-light band is colormap == fullcolormap + level*256 for
          * level in [0, NUMCOLORMAPS).  Anything else -- the invulnerability /
          * light-amp INVERSECOLORMAP (level == NUMCOLORMAPS), a sector map we
          * can't express as a band offset, or a misaligned pointer -- is NOT a
          * distance fade and must be drawn through its own map at full bright,
          * exactly as the banded point path does. */
         int is_band = (off >= 0) && ((off & 255) == 0)
                       && band >= 0 && band < NUMCOLORMAPS;

         if (smooth_ramp_pal != V_Palette16)
            R_BuildSmoothRamp();

         if (is_band && fine >= 0)
         {
            /* Sub-band precision published by the light-selection chokepoint
             * (already at SMOOTH_WEIGHTS resolution). */
            weight = fine;
         }
         else if (is_band)
         {
            /* Sprite/patch at a distance band, no fine value: map the band
             * level 0..31 onto the 0..(SMOOTH_WEIGHTS-1) ramp. */
            weight = (NUMCOLORMAPS-1 - band) * (SMOOTH_WEIGHTS-1)
                     / (NUMCOLORMAPS-1);
         }
         else
         {
            /* Not a distance band (invuln inverse map, etc.): full bright. */
            weight = SMOOTH_WEIGHTS-1;
         }

         if (weight < 0)                       weight = 0;
         else if (weight > SMOOTH_WEIGHTS-1)    weight = SMOOTH_WEIGHTS-1;

         /* Distance darkening is applied ENTIRELY through the luma weight (the
          * CRY way), so for a distance band the texel is looked up through the
          * UNDIMMED base map (band 0 = fullcolormap), NOT the dimmed band the
          * renderer selected.  Indexing the dimmed band AND applying the weight
          * darkens twice -- an over-dark, blotchy image.  For a non-band map
          * (invuln) the special remap must be preserved, so fall back to the
          * shared V_Palette16 at full weight (the point-path output).  For a
          * distance band, index the private fine ramp by the undimmed texel. */
         if (is_band)
         {
            for (i = 0; i < 256; i++)
               composed_lut[i] = smooth_ramp[ fullcolormap[i]*SMOOTH_WEIGHTS + weight ];
         }
         else
         {
            for (i = 0; i < 256; i++)
               composed_lut[i] = V_Palette16[ colormap[i]*VID_NUMCOLORWEIGHTS + (VID_NUMCOLORWEIGHTS-1) ];
         }

         composed_weight = want_weight;
      }
      else
      {
         for (i = 0; i < 256; i++)
            composed_lut[i] = V_Palette16[ colormap[i]*64 + (64-1) ];
         composed_weight = -2;
      }
      composed_cm  = colormap;
      composed_pal = V_Palette16;
   }
   return composed_lut;
}

/* No-colormap variant for the patch/HUD/UI point column
 * (R_DrawColumn16_PointUV): that path has no colormap (full-bright 2D
 * blitting), so the per-pixel expression is just V_Palette16[texel*64+63].
 * That is still a fixed 8bpp->16bpp function of the texel, composable into
 * one table keyed solely on V_Palette16 (rebuilt on a palette/gamma/video-
 * mode change).  Speeds HUD/status-bar/menu blits and the title/inter-
 * mission background-cache builds, which all run through this column. */
static const uint16_t *composed_nolight_pal = NULL;
static uint16_t        composed_nolight_lut[256];

static INLINE const uint16_t *R_GetComposedPalette(void)
{
   if (V_Palette16 != composed_nolight_pal)
   {
      int i;
      for (i = 0; i < 256; i++)
         composed_nolight_lut[i] = V_Palette16[ i*64 + (64-1) ];
      composed_nolight_pal = V_Palette16;
   }
   return composed_nolight_lut;
}

// Color tables for different players,
//  translate a limited part to another
//  (color ramps used for  suit colors).
//

//
// R_DrawColumn
// Source is the top of the column to scale.
//

// SoM: OPTIMIZE for ANYRES
typedef enum
{
   COL_NONE,
   COL_OPAQUE,
   COL_TRANS,
   COL_ALTTRANS,
   COL_FLEXTRANS,
   COL_FUZZ,
   COL_FLEXADD
} columntype_e;

static int    temp_x = 0;
static int    tempyl[4], tempyh[4];
static uint16_t short_tempbuf[MAX_SCREENHEIGHT * 4];
static int    startx = 0;

/* Raven translucent sprites (Hexen MF_SHADOW/MF_ALTSHADOW, Heretic ghosts).
 * The batching column drawers consult these instead of hardcoding the opaque
 * type/flushers, so a translucent sprite gets its own batch type (breaking
 * any run shared with neighbouring opaque columns) and blending flushers.
 * R_SetSpriteTranslucency selects the mode around a sprite's draw; the
 * default is the opaque set.  The blend works directly on the RGB565
 * framebuffer: 50/50 for SHADOW, 25/75 for ALTSHADOW. */
static void R_FlushWhole16(void);
static void R_FlushHT16(void);
static void R_FlushQuad16(void);
static void R_FlushWholeTL16(void);
static void R_FlushHTTL16(void);
static void R_FlushQuadTL16(void);
static void R_FlushWholeADD16(void);
static void R_FlushHTADD16(void);
static void R_FlushQuadADD16(void);
static void R_FlushWholeLERP16(void);
static void R_FlushHTLERP16(void);
static void R_FlushQuadLERP16(void);

/* Per-line/-surface blend weight, alpha*32 in 0..32, consumed by the LERP and
 * ADD flushers.  Set via R_SetTransAlpha before the masked draw; the fixed
 * TL/ALTTRANS paths ignore it. */
static int  tl_alpha = 16;
static int  tl_temptype = COL_OPAQUE;
static void (*tl_flush_whole)(void) = R_FlushWhole16;
static void (*tl_flush_ht)(void)    = R_FlushHT16;
static void (*tl_flush_quad)(void)  = R_FlushQuad16;

/* Non-inline linkage for the composed lookup tables, for the direct
 * sprite column path in r_things.c. */
const uint16_t *R_ComposedColormap(const lighttable_t *colormap)
{
  return R_GetComposedColormap(colormap);
}

const uint16_t *R_ComposedPalette(void)
{
  return R_GetComposedPalette();
}

void R_SetTransAlpha(int a32)
{
  tl_alpha = a32 < 0 ? 0 : (a32 > 32 ? 32 : a32);
}

void R_SetSpriteTranslucency(int mode)
{
  if (mode == 4)
  {
    /* per-alpha lerp: dst + (src-dst)*tl_alpha/32 (ZDoom alpha glass) */
    tl_temptype    = COL_FLEXTRANS;
    tl_flush_whole = R_FlushWholeLERP16;
    tl_flush_ht    = R_FlushHTLERP16;
    tl_flush_quad  = R_FlushQuadLERP16;
  }
  else if (mode == 3)
  {
    /* per-alpha additive: dst += src*tl_alpha/32, saturating (light beam) */
    tl_temptype    = COL_FLEXADD;
    tl_flush_whole = R_FlushWholeADD16;
    tl_flush_ht    = R_FlushHTADD16;
    tl_flush_quad  = R_FlushQuadADD16;
  }
  else if (mode)
  {
    tl_temptype    = (mode == 2) ? COL_ALTTRANS : COL_TRANS;
    tl_flush_whole = R_FlushWholeTL16;
    tl_flush_ht    = R_FlushHTTL16;
    tl_flush_quad  = R_FlushQuadTL16;
  }
  else
  {
    tl_temptype    = COL_OPAQUE;
    tl_flush_whole = R_FlushWhole16;
    tl_flush_ht    = R_FlushHT16;
    tl_flush_quad  = R_FlushQuad16;
  }
}

/* 50/50 RGB565 blend: mask off each channel's low bit, halve, add. */
#define TL_BLEND565(s, d) \
  ((uint16_t)((((s) & 0xF7DEu) >> 1) + (((d) & 0xF7DEu) >> 1)))

/* Per-channel RGB565 blends parameterised by a 0..32 weight (= alpha*32).
 * Channels are kept separate so each value is <= 0x3F and value*a fits in the
 * 16-bit lane the SSE2/NEON kernels use, making these scalar references and
 * those vector kernels bit-identical.  LERP565A interpolates dst->src (glass);
 * ADD565A adds a fraction of src to dst, clamped (additive light beam). */
static INLINE uint16_t LERP565A(uint16_t s, uint16_t d, int a)
{
  int r = ((d >> 11) & 0x1F) + ((((int)((s >> 11) & 0x1F) - (int)((d >> 11) & 0x1F)) * a) >> 5);
  int g = ((d >>  5) & 0x3F) + ((((int)((s >>  5) & 0x3F) - (int)((d >>  5) & 0x3F)) * a) >> 5);
  int b =  ((d)      & 0x1F) + ((((int)((s)       & 0x1F) - (int)((d)       & 0x1F)) * a) >> 5);
  return (uint16_t)((r << 11) | (g << 5) | b);
}

static INLINE uint16_t ADD565A(uint16_t s, uint16_t d, int a)
{
  int r = ((d >> 11) & 0x1F) + (((int)((s >> 11) & 0x1F) * a) >> 5);
  int g = ((d >>  5) & 0x3F) + (((int)((s >>  5) & 0x3F) * a) >> 5);
  int b =  ((d)      & 0x1F) + (((int)((s)       & 0x1F) * a) >> 5);
  if (r > 0x1F) r = 0x1F;
  if (g > 0x3F) g = 0x3F;
  if (b > 0x1F) b = 0x1F;
  return (uint16_t)((r << 11) | (g << 5) | b);
}

static int    temptype = COL_NONE;
static int    commontop, commonbot;
// SoM 7-28-04: Fix the fuzz problem.
static const uint8_t   *tempfuzzmap;

//
// Spectre/Invisibility.
//

#define FUZZTABLE 50
#define FUZZOFF 1

static const int fuzzoffset_org[FUZZTABLE] = {
  FUZZOFF,-FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,
  FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,
  FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,
  FUZZOFF,-FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,
  FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,-FUZZOFF,FUZZOFF,
  FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,
  FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF
};

static int fuzzoffset[FUZZTABLE];

static int fuzzpos = 0;

// render pipelines
#define RDC_STANDARD      1
#define RDC_TRANSLATED    4
#define RDC_FUZZ          8
// no color mapping
#define RDC_NOCOLMAP     16
// filter modes
#define RDC_DITHERZ      32
#define RDC_BILINEAR     64
#define RDC_ROUNDED     128

draw_vars_t drawvars = { 
  NULL, // short_topleft
  NULL, // int_topleft
  RDRAW_FILTER_POINT, // filterwall
  RDRAW_FILTER_POINT, // filterfloor
  RDRAW_FILTER_POINT, // filtersprite
  RDRAW_FILTER_POINT, // filterz
  RDRAW_FILTER_POINT, // filterpatch

  RDRAW_MASKEDCOLUMNEDGE_SQUARE, // sprite_edges
  RDRAW_MASKEDCOLUMNEDGE_SQUARE, // patch_edges

  // 49152 = FRACUNIT * 0.75
  // 81920 = FRACUNIT * 1.25
  49152 // mag_threshold
};

/* "Diminished Lighting" setting (General > Video menu).
 *   0 = Default  : point-sampled light selection -- the standard 32
 *                  discrete distance/light colormap bands.
 *   1 = Smooth   : CRY-style continuous luma shading (see r_smooth_shading
 *                  and R_GetComposedColormap).  Keeps the point path's
 *                  single-lookup inner loop -- same per-pixel cost as
 *                  Default -- but selects the texel's brightness from
 *                  V_Palette16's 64-step continuous luma ramp by recovered
 *                  light level instead of snapping through the 32 colormap
 *                  bands, removing the band edges.
 *
 * A third mode (Dithered: filterz = LINEAR, ordered dither between adjacent
 * light colormaps) was removed: it was visually near-indistinguishable from
 * Default yet roughly halved the framerate (e.g. 368 -> 179 fps), and Smooth
 * supersedes it -- smoother result at Default's per-pixel cost.  Configs that
 * still hold the old value 2 (Smooth) or 1 (Dithered) are migrated to the new
 * Smooth (1) at apply time; see R_ApplyDiminishedLighting.
 * R_ApplyDiminishedLighting() pushes the value into drawvars.filterz; it
 * is called once at R_Init (after the config is loaded) and again from
 * the menu change callback.  Render-only: never touches playsim, so it is
 * savegame- and netgame-neutral. */
int diminished_lighting = 0;

void R_ApplyDiminishedLighting(void)
{
   /* Migrate old 3-state configs (0 Default / 1 Dithered / 2 Smooth) to the
    * 2-state scheme (0 Default / 1 Smooth): any nonzero value -> Smooth.  The
    * removed Dithered mode mapped to LINEAR filterz; with it gone, both modes
    * keep point-sampled Z and use the single-lookup composed LUT, so filterz
    * is always POINT here. */
   if (diminished_lighting > 1)
      diminished_lighting = 1;

   drawvars.filterz = RDRAW_FILTER_POINT;
   r_smooth_shading  = (diminished_lighting == 1);

   /* The composed LUT is cached on (colormap ptr, V_Palette16); neither
    * changes when only the shading mode flips, so force a rebuild on the
    * next lookup, otherwise a stale banded/smooth table would persist until
    * the active colormap happens to change. */
   composed_cm  = NULL;
   composed_pal = NULL;
   composed_weight = -2;
}

//
// Error functions that will abort if R_FlushColumns tries to flush 
// columns without a column type.
//

static void R_FlushWholeError(void)
{
   I_Error("R_FlushWholeColumns called without being initialized.\n");
}

static void R_FlushHTError(void)
{
   I_Error("R_FlushHTColumns called without being initialized.\n");
}

static void R_QuadFlushError(void)
{
   I_Error("R_FlushQuadColumn called without being initialized.\n");
}

static void (*R_FlushWholeColumns)(void) = R_FlushWholeError;
static void (*R_FlushHTColumns)(void)    = R_FlushHTError;
static void (*R_FlushQuadColumn)(void) = R_QuadFlushError;

static void R_FlushColumns(void)
{
#ifdef PRBOOM_RENDER_PROFILE
   double _t0 = I_RenderProfileUsec();
#endif
   if(temp_x != 4 || commontop >= commonbot)
      R_FlushWholeColumns();
   else
   {
      R_FlushHTColumns();
      R_FlushQuadColumn();
   }
   temp_x = 0;
#ifdef PRBOOM_RENDER_PROFILE
   prof_wallfill_usec += (I_RenderProfileUsec() - _t0);
#endif
}

//
// R_ResetColumnBuffer
//
// haleyjd 09/13/04: new function to call from main rendering loop
// which gets rid of the unnecessary reset of various variables during
// column drawing.
//
void R_ResetColumnBuffer(void)
{
   // haleyjd 10/06/05: this must not be done if temp_x == 0!
   if(temp_x)
      R_FlushColumns();

   temptype            = COL_NONE;
   R_FlushWholeColumns = R_FlushWholeError;
   R_FlushHTColumns    = R_FlushHTError;
   R_FlushQuadColumn   = R_QuadFlushError;
}

/*
 * R_FlushWhole16
 *
 * Flushes the entire columns in the buffer, one at a time.
 * This is used when a quad flush isn't possible.
 * Opaque version -- no remapping whatsoever.
*/
static void R_FlushWhole16(void)
{
   while(--temp_x >= 0)
   {
      int yl           = tempyl[temp_x];
      uint16_t *source = &short_tempbuf[temp_x + (yl << 2)];
      uint16_t *dest   = drawvars.short_topleft + yl * SURFACE_SHORT_PITCH + startx + temp_x;
      int   count      = tempyh[temp_x] - yl + 1;
      
      while(--count >= 0)
      {
         *dest   = *source;
         source += 4;
         dest   += SURFACE_SHORT_PITCH;
      }
   }
}

//
// R_FlushHT16
//
// Flushes the head and tail of columns in the buffer in
// preparation for a quad flush.
// Opaque version -- no remapping whatsoever.
//
static void R_FlushHT16(void)
{
   uint16_t *source;
   uint16_t *dest;
   int count, colnum = 0;
   int yl, yh;

   while(colnum < 4)
   {
      yl = tempyl[colnum];
      yh = tempyh[colnum];
      
      // flush column head
      if(yl < commontop)
      {
         source = &short_tempbuf[colnum + (yl << 2)];
         dest   = drawvars.short_topleft + yl * SURFACE_SHORT_PITCH + startx + colnum;
         count  = commontop - yl;
         
         while(--count >= 0)
         {
            *dest = *source;
            source += 4;
            dest += SURFACE_SHORT_PITCH;
         }
      }
      
      // flush column tail
      if(yh > commonbot)
      {
         source = &short_tempbuf[colnum + ((commonbot + 1) << 2)];
         dest   = drawvars.short_topleft + (commonbot + 1) * SURFACE_SHORT_PITCH + startx + colnum;
         count  = yh - commonbot;
         
         while(--count >= 0)
         {
            *dest = *source;

            source += 4;
            dest += SURFACE_SHORT_PITCH;
         }
      }         
      ++colnum;
   }
}

static void R_FlushQuad16(void)
{
   uint16_t *source = &short_tempbuf[commontop << 2];
   uint16_t *dest   = drawvars.short_topleft + commontop * SURFACE_SHORT_PITCH + startx;
   int        count = commonbot - commontop + 1;

   /* Each row copies the 4 transposed columns, which are 4 contiguous
    * uint16_t in the source (the transpose buffer is 4-wide) to 4 adjacent
    * pixels in the destination -- i.e. exactly 8 contiguous bytes from a
    * contiguous source.  The original wrote them as four separate 16-bit
    * stores.  Collapse to a single 8-byte move: memcpy(,,8) lowers to one
    * unaligned movq/str on every target the core builds for, makes no
    * alignment assumption (dest = topleft + startx is only 2-byte aligned),
    * and is endian-agnostic (a byte copy, so bit-identical on LE and BE).
    * The whole quad-column path is the common wall-fill case, so this is the
    * pixel-write hot loop. */
   while(--count >= 0)
   {
      memcpy(dest, source, 4 * sizeof(uint16_t));
      source += 4;
      dest += SURFACE_SHORT_PITCH;
   }
}

/* Translucent flushers: as the opaque trio, but each pixel store blends the
 * column sample against the destination.  COL_ALTTRANS blends twice toward
 * the destination (~25%% sprite), matching Hexen's fainter alt-shadow. */
static void R_FlushWholeTL16(void)
{
   int alt = (temptype == COL_ALTTRANS);

   while(--temp_x >= 0)
   {
      int yl           = tempyl[temp_x];
      uint16_t *source = &short_tempbuf[temp_x + (yl << 2)];
      uint16_t *dest   = drawvars.short_topleft + yl * SURFACE_SHORT_PITCH + startx + temp_x;
      int   count      = tempyh[temp_x] - yl + 1;

      while(--count >= 0)
      {
         uint16_t px = TL_BLEND565(*source, *dest);
         if (alt)
            px = TL_BLEND565(px, *dest);
         *dest   = px;
         source += 4;
         dest   += SURFACE_SHORT_PITCH;
      }
   }
}

static void R_FlushHTTL16(void)
{
   uint16_t *source;
   uint16_t *dest;
   int count, colnum = 0;
   int yl, yh;
   int alt = (temptype == COL_ALTTRANS);

   while(colnum < 4)
   {
      yl = tempyl[colnum];
      yh = tempyh[colnum];

      if(yl < commontop)
      {
         source = &short_tempbuf[colnum + (yl << 2)];
         dest   = drawvars.short_topleft + yl * SURFACE_SHORT_PITCH + startx + colnum;
         count  = commontop - yl;

         while(--count >= 0)
         {
            uint16_t px = TL_BLEND565(*source, *dest);
            if (alt)
               px = TL_BLEND565(px, *dest);
            *dest   = px;
            source += 4;
            dest   += SURFACE_SHORT_PITCH;
         }
      }

      if(yh > commonbot)
      {
         source = &short_tempbuf[colnum + ((commonbot + 1) << 2)];
         dest   = drawvars.short_topleft + (commonbot + 1) * SURFACE_SHORT_PITCH + startx + colnum;
         count  = yh - commonbot;

         while(--count >= 0)
         {
            uint16_t px = TL_BLEND565(*source, *dest);
            if (alt)
               px = TL_BLEND565(px, *dest);
            *dest   = px;
            source += 4;
            dest   += SURFACE_SHORT_PITCH;
         }
      }
      ++colnum;
   }
}

static void R_FlushQuadTL16(void)
{
   uint16_t *source = &short_tempbuf[commontop << 2];
   uint16_t *dest   = drawvars.short_topleft + commontop * SURFACE_SHORT_PITCH + startx;
   int        count = commonbot - commontop + 1;
   int        alt   = (temptype == COL_ALTTRANS);

   /* Two rows per step: the transpose buffer is contiguous (8 lanes in one
    * load), the framebuffer rows are a pitch apart (two 64-bit halves).
    * The vector arithmetic is the TL_BLEND565 mask/shift/add per 16-bit
    * lane, so the output is bit-identical to the scalar rows below, which
    * also finish any odd row. */
#if defined(WALL_RUN_SSE2)
   {
      const __m128i mask = _mm_set1_epi16((short)0xF7DEu);
      while (count >= 2)
      {
         __m128i s  = _mm_loadu_si128((const __m128i *)source);
         __m128i d0 = _mm_loadl_epi64((const __m128i *)dest);
         __m128i d1 = _mm_loadl_epi64((const __m128i *)(dest + SURFACE_SHORT_PITCH));
         __m128i d  = _mm_unpacklo_epi64(d0, d1);
         __m128i dh = _mm_srli_epi16(_mm_and_si128(d, mask), 1);
         __m128i px = _mm_add_epi16(_mm_srli_epi16(_mm_and_si128(s, mask), 1), dh);
         if (alt)
            px = _mm_add_epi16(_mm_srli_epi16(_mm_and_si128(px, mask), 1), dh);
         _mm_storel_epi64((__m128i *)dest, px);
         _mm_storel_epi64((__m128i *)(dest + SURFACE_SHORT_PITCH),
                          _mm_unpackhi_epi64(px, px));
         source += 8;
         dest   += 2 * SURFACE_SHORT_PITCH;
         count  -= 2;
      }
   }
#elif defined(WALL_RUN_NEON)
   {
      const uint16x8_t mask = vdupq_n_u16(0xF7DEu);
      while (count >= 2)
      {
         uint16x8_t s  = vld1q_u16(source);
         uint16x8_t d  = vcombine_u16(vld1_u16(dest),
                                      vld1_u16(dest + SURFACE_SHORT_PITCH));
         uint16x8_t dh = vshrq_n_u16(vandq_u16(d, mask), 1);
         uint16x8_t px = vaddq_u16(vshrq_n_u16(vandq_u16(s, mask), 1), dh);
         if (alt)
            px = vaddq_u16(vshrq_n_u16(vandq_u16(px, mask), 1), dh);
         vst1_u16(dest, vget_low_u16(px));
         vst1_u16(dest + SURFACE_SHORT_PITCH, vget_high_u16(px));
         source += 8;
         dest   += 2 * SURFACE_SHORT_PITCH;
         count  -= 2;
      }
   }
#endif

   while(--count >= 0)
   {
      int i;
      for (i = 0; i < 4; i++)
      {
         uint16_t px = TL_BLEND565(source[i], dest[i]);
         if (alt)
            px = TL_BLEND565(px, dest[i]);
         dest[i] = px;
      }
      source += 4;
      dest += SURFACE_SHORT_PITCH;
   }
}

/* Channel split/pack helpers for the vector blend kernels: R/G/B in separate
 * 16-bit lanes so channel*alpha (<= 0x3F*32) stays inside the lane and the
 * result is bit-identical to the LERP565A / ADD565A scalar references. */
#if defined(WALL_RUN_SSE2)
#define SR(v) _mm_and_si128(_mm_srli_epi16(v,11),_mm_set1_epi16(0x1F))
#define SG(v) _mm_and_si128(_mm_srli_epi16(v, 5),_mm_set1_epi16(0x3F))
#define SB(v) _mm_and_si128(v,                   _mm_set1_epi16(0x1F))
#define SPACK(r,g,b) _mm_or_si128(_mm_or_si128(_mm_slli_epi16(r,11),_mm_slli_epi16(g,5)),b)
#elif defined(WALL_RUN_NEON)
#define NR(v) vandq_u16(vshrq_n_u16(v,11),vdupq_n_u16(0x1F))
#define NG(v) vandq_u16(vshrq_n_u16(v, 5),vdupq_n_u16(0x3F))
#define NB(v) vandq_u16(v,                vdupq_n_u16(0x1F))
#define NPACK(r,g,b) vorrq_u16(vorrq_u16(vshlq_n_u16(r,11),vshlq_n_u16(g,5)),b)
#endif

/* Additive flushers: dst += src*tl_alpha/32 per channel, saturating -- the map's
 * "Add" renderstyle light beams.  Whole/HT scalar; the quad path vectorises the
 * common run. */
static void R_FlushWholeADD16(void)
{
   while(--temp_x >= 0)
   {
      int yl           = tempyl[temp_x];
      uint16_t *source = &short_tempbuf[temp_x + (yl << 2)];
      uint16_t *dest   = drawvars.short_topleft + yl * SURFACE_SHORT_PITCH + startx + temp_x;
      int   count      = tempyh[temp_x] - yl + 1;

      while(--count >= 0)
      {
         *dest   = ADD565A(*source, *dest, tl_alpha);
         source += 4;
         dest   += SURFACE_SHORT_PITCH;
      }
   }
}

static void R_FlushHTADD16(void)
{
   uint16_t *source;
   uint16_t *dest;
   int count, colnum = 0;
   int yl, yh;

   while(colnum < 4)
   {
      yl = tempyl[colnum];
      yh = tempyh[colnum];

      if(yl < commontop)
      {
         source = &short_tempbuf[colnum + (yl << 2)];
         dest   = drawvars.short_topleft + yl * SURFACE_SHORT_PITCH + startx + colnum;
         count  = commontop - yl;

         while(--count >= 0)
         {
            *dest   = ADD565A(*source, *dest, tl_alpha);
            source += 4;
            dest   += SURFACE_SHORT_PITCH;
         }
      }

      if(yh > commonbot)
      {
         source = &short_tempbuf[colnum + ((commonbot + 1) << 2)];
         dest   = drawvars.short_topleft + (commonbot + 1) * SURFACE_SHORT_PITCH + startx + colnum;
         count  = yh - commonbot;

         while(--count >= 0)
         {
            *dest   = ADD565A(*source, *dest, tl_alpha);
            source += 4;
            dest   += SURFACE_SHORT_PITCH;
         }
      }
      ++colnum;
   }
}

static void R_FlushQuadADD16(void)
{
   uint16_t *source = &short_tempbuf[commontop << 2];
   uint16_t *dest   = drawvars.short_topleft + commontop * SURFACE_SHORT_PITCH + startx;
   int        count = commonbot - commontop + 1;

#if defined(WALL_RUN_SSE2)
   {
      __m128i va = _mm_set1_epi16((short)tl_alpha);
      __m128i rm = _mm_set1_epi16(0x1F), gm = _mm_set1_epi16(0x3F);
      while (count >= 2)
      {
         __m128i s  = _mm_loadu_si128((const __m128i *)source);
         __m128i d0 = _mm_loadl_epi64((const __m128i *)dest);
         __m128i d1 = _mm_loadl_epi64((const __m128i *)(dest + SURFACE_SHORT_PITCH));
         __m128i d  = _mm_unpacklo_epi64(d0, d1);
         __m128i r  = _mm_min_epi16(_mm_add_epi16(SR(d), _mm_srli_epi16(_mm_mullo_epi16(SR(s), va), 5)), rm);
         __m128i g  = _mm_min_epi16(_mm_add_epi16(SG(d), _mm_srli_epi16(_mm_mullo_epi16(SG(s), va), 5)), gm);
         __m128i b  = _mm_min_epi16(_mm_add_epi16(SB(d), _mm_srli_epi16(_mm_mullo_epi16(SB(s), va), 5)), rm);
         __m128i px = SPACK(r, g, b);
         _mm_storel_epi64((__m128i *)dest, px);
         _mm_storel_epi64((__m128i *)(dest + SURFACE_SHORT_PITCH), _mm_unpackhi_epi64(px, px));
         source += 8;
         dest   += 2 * SURFACE_SHORT_PITCH;
         count  -= 2;
      }
   }
#elif defined(WALL_RUN_NEON)
   {
      uint16x8_t va = vdupq_n_u16((uint16_t)tl_alpha);
      uint16x8_t rm = vdupq_n_u16(0x1F), gm = vdupq_n_u16(0x3F);
      while (count >= 2)
      {
         uint16x8_t s = vld1q_u16(source);
         uint16x8_t d = vcombine_u16(vld1_u16(dest), vld1_u16(dest + SURFACE_SHORT_PITCH));
         uint16x8_t r = vminq_u16(vaddq_u16(NR(d), vshrq_n_u16(vmulq_u16(NR(s), va), 5)), rm);
         uint16x8_t g = vminq_u16(vaddq_u16(NG(d), vshrq_n_u16(vmulq_u16(NG(s), va), 5)), gm);
         uint16x8_t b = vminq_u16(vaddq_u16(NB(d), vshrq_n_u16(vmulq_u16(NB(s), va), 5)), rm);
         uint16x8_t px = NPACK(r, g, b);
         vst1_u16(dest, vget_low_u16(px));
         vst1_u16(dest + SURFACE_SHORT_PITCH, vget_high_u16(px));
         source += 8;
         dest   += 2 * SURFACE_SHORT_PITCH;
         count  -= 2;
      }
   }
#endif

   while(--count >= 0)
   {
      int i;
      for (i = 0; i < 4; i++)
         dest[i] = ADD565A(source[i], dest[i], tl_alpha);
      source += 4;
      dest += SURFACE_SHORT_PITCH;
   }
}

/* Per-alpha lerp flushers: dst + (src-dst)*tl_alpha/32 per channel -- ZDoom
 * alpha glass at its true alpha rather than the bucketed 50/50.  Same shape as
 * the additive trio; the diff is signed so the quad uses an arithmetic shift. */
/* Submerged 3D-floor water (matching GZDoom's hardware look): rather than
 * alpha-blending the water surface texture over the scene, the swimmable-water
 * midtexture darkens the geometry behind it toward a dark blue-grey, fading
 * with depth below the surface line.  Deep water lands near-black with a faint
 * blue; near the surface it stays lighter and bluer.  This reproduces the
 * reference profile (surface ~RGB 56,69,85 -> deep ~15,15,15) and composites
 * correctly because it darkens whatever is behind, sidestepping the draw-order
 * problem of overlaying a translucent plane that later geometry paints over. */
static INLINE uint16_t R_WaterDarken1(uint16_t d, int keep, int bluelift)
{
   int dr = (d >> 11) & 0x1F, dg = (d >> 5) & 0x3F, db = d & 0x1F;
   int nr = (dr * keep) >> 5;
   int ng = (dg * keep) >> 5;
   int nb = ((db * keep) >> 5) + bluelift;
   if (nb > 31) nb = 31;
   return (uint16_t)((nr << 11) | (ng << 5) | nb);
}

/* Darken one screen column [yl..yh] with depth below surf_y.
 * SIMD CANDIDATE: this per-pixel RGB565 darken over a vertical run is a prime
 * SSE2/NEON target -- unpack 8 px to channels, multiply by keep, shift, add
 * blue, repack; keep/bluelift vary slowly so they can be recomputed per short
 * block.  Mirrors the existing quad-column LERP/ADD flushers. */
void R_WaterDarkenColumn(int x, int yl, int yh, int surf_y)
{
   uint16_t *dest = drawvars.short_topleft + yl * SURFACE_SHORT_PITCH + x;
   int y;
   for (y = yl; y <= yh; y++, dest += SURFACE_SHORT_PITCH)
   {
      int depth = y - surf_y; if (depth < 0) depth = 0;
      {
         int keep = 28 - depth;
         int bl;
         if (keep < 9) keep = 9;             /* deep floor, not pure black */
         bl = (depth < 32) ? (8 - depth/4) : 0;
         bl += 2;                            /* constant deep blue */
         if (bl < 2) bl = 2;
         *dest = R_WaterDarken1(*dest, keep, bl);
      }
   }
}

static void R_FlushWholeLERP16(void)
{
   while(--temp_x >= 0)
   {
      int yl           = tempyl[temp_x];
      uint16_t *source = &short_tempbuf[temp_x + (yl << 2)];
      uint16_t *dest   = drawvars.short_topleft + yl * SURFACE_SHORT_PITCH + startx + temp_x;
      int   count      = tempyh[temp_x] - yl + 1;

      while(--count >= 0)
      {
         *dest   = LERP565A(*source, *dest, tl_alpha);
         source += 4;
         dest   += SURFACE_SHORT_PITCH;
      }
   }
}

static void R_FlushHTLERP16(void)
{
   uint16_t *source;
   uint16_t *dest;
   int count, colnum = 0;
   int yl, yh;

   while(colnum < 4)
   {
      yl = tempyl[colnum];
      yh = tempyh[colnum];

      if(yl < commontop)
      {
         source = &short_tempbuf[colnum + (yl << 2)];
         dest   = drawvars.short_topleft + yl * SURFACE_SHORT_PITCH + startx + colnum;
         count  = commontop - yl;

         while(--count >= 0)
         {
            *dest   = LERP565A(*source, *dest, tl_alpha);
            source += 4;
            dest   += SURFACE_SHORT_PITCH;
         }
      }

      if(yh > commonbot)
      {
         source = &short_tempbuf[colnum + ((commonbot + 1) << 2)];
         dest   = drawvars.short_topleft + (commonbot + 1) * SURFACE_SHORT_PITCH + startx + colnum;
         count  = yh - commonbot;

         while(--count >= 0)
         {
            *dest   = LERP565A(*source, *dest, tl_alpha);
            source += 4;
            dest   += SURFACE_SHORT_PITCH;
         }
      }
      ++colnum;
   }
}

static void R_FlushQuadLERP16(void)
{
   uint16_t *source = &short_tempbuf[commontop << 2];
   uint16_t *dest   = drawvars.short_topleft + commontop * SURFACE_SHORT_PITCH + startx;
   int        count = commonbot - commontop + 1;

#if defined(WALL_RUN_SSE2)
   {
      __m128i va = _mm_set1_epi16((short)tl_alpha);
      while (count >= 2)
      {
         __m128i s  = _mm_loadu_si128((const __m128i *)source);
         __m128i d0 = _mm_loadl_epi64((const __m128i *)dest);
         __m128i d1 = _mm_loadl_epi64((const __m128i *)(dest + SURFACE_SHORT_PITCH));
         __m128i d  = _mm_unpacklo_epi64(d0, d1);
         __m128i r  = _mm_add_epi16(SR(d), _mm_srai_epi16(_mm_mullo_epi16(_mm_sub_epi16(SR(s), SR(d)), va), 5));
         __m128i g  = _mm_add_epi16(SG(d), _mm_srai_epi16(_mm_mullo_epi16(_mm_sub_epi16(SG(s), SG(d)), va), 5));
         __m128i b  = _mm_add_epi16(SB(d), _mm_srai_epi16(_mm_mullo_epi16(_mm_sub_epi16(SB(s), SB(d)), va), 5));
         __m128i px = SPACK(r, g, b);
         _mm_storel_epi64((__m128i *)dest, px);
         _mm_storel_epi64((__m128i *)(dest + SURFACE_SHORT_PITCH), _mm_unpackhi_epi64(px, px));
         source += 8;
         dest   += 2 * SURFACE_SHORT_PITCH;
         count  -= 2;
      }
   }
#elif defined(WALL_RUN_NEON)
   {
      int16x8_t va = vdupq_n_s16((int16_t)tl_alpha);
      while (count >= 2)
      {
         uint16x8_t s = vld1q_u16(source);
         uint16x8_t d = vcombine_u16(vld1_u16(dest), vld1_u16(dest + SURFACE_SHORT_PITCH));
         int16x8_t  dr = vreinterpretq_s16_u16(NR(d)), dg = vreinterpretq_s16_u16(NG(d)), db = vreinterpretq_s16_u16(NB(d));
         int16x8_t  pr = vshrq_n_s16(vmulq_s16(vsubq_s16(vreinterpretq_s16_u16(NR(s)), dr), va), 5);
         int16x8_t  pg = vshrq_n_s16(vmulq_s16(vsubq_s16(vreinterpretq_s16_u16(NG(s)), dg), va), 5);
         int16x8_t  pb = vshrq_n_s16(vmulq_s16(vsubq_s16(vreinterpretq_s16_u16(NB(s)), db), va), 5);
         uint16x8_t r = vreinterpretq_u16_s16(vaddq_s16(dr, pr));
         uint16x8_t g = vreinterpretq_u16_s16(vaddq_s16(dg, pg));
         uint16x8_t b = vreinterpretq_u16_s16(vaddq_s16(db, pb));
         uint16x8_t px = NPACK(r, g, b);
         vst1_u16(dest, vget_low_u16(px));
         vst1_u16(dest + SURFACE_SHORT_PITCH, vget_high_u16(px));
         source += 8;
         dest   += 2 * SURFACE_SHORT_PITCH;
         count  -= 2;
      }
   }
#endif

   while(--count >= 0)
   {
      int i;
      for (i = 0; i < 4; i++)
         dest[i] = LERP565A(source[i], dest[i], tl_alpha);
      source += 4;
      dest += SURFACE_SHORT_PITCH;
   }
}

/*
 * R_FlushWholeFuzz16
 *
 * Flushes the entire columns in the buffer, one at a time.
 * This is used when a quad flush isn't possible.
 * Opaque version -- no remapping whatsoever.
*/
static void R_FlushWholeFuzz16(void)
{
   uint16_t *source;
   uint16_t *dest;
   int  count, yl;

   while(--temp_x >= 0)
   {
      yl     = tempyl[temp_x];
      source = &short_tempbuf[temp_x + (yl << 2)];
      dest   = drawvars.short_topleft + yl * SURFACE_SHORT_PITCH + startx + temp_x;
      count  = tempyh[temp_x] - yl + 1;
      
      while(--count >= 0)
      {
         // SoM 7-28-04: Fix the fuzz problem.
         *dest = GETBLENDED16_9406(dest[fuzzoffset[fuzzpos]], 0);
         
         // Clamp table lookup index.
         if(++fuzzpos == FUZZTABLE) 
            fuzzpos = 0;

         source += 4;
         dest += SURFACE_SHORT_PITCH;
      }
   }
}

//
// R_FlushHTFuzz16
//
// Flushes the head and tail of columns in the buffer in
// preparation for a quad flush.
// Opaque version -- no remapping whatsoever.
//
static void R_FlushHTFuzz16(void)
{
   uint16_t *source;
   uint16_t *dest;
   int count, colnum = 0;
   int yl, yh;

   while(colnum < 4)
   {
      yl = tempyl[colnum];
      yh = tempyh[colnum];
      
      // flush column head
      if(yl < commontop)
      {
         source = &short_tempbuf[colnum + (yl << 2)];
         dest   = drawvars.short_topleft + yl * SURFACE_SHORT_PITCH + startx + colnum;
         count  = commontop - yl;
         
         while(--count >= 0)
         {
            // SoM 7-28-04: Fix the fuzz problem.
            *dest = GETBLENDED16_9406(dest[fuzzoffset[fuzzpos]], 0);
            
            // Clamp table lookup index.
            if(++fuzzpos == FUZZTABLE) 
               fuzzpos = 0;

            source += 4;
            dest += SURFACE_SHORT_PITCH;
         }
      }
      
      // flush column tail
      if(yh > commonbot)
      {
         source = &short_tempbuf[colnum + ((commonbot + 1) << 2)];
         dest   = drawvars.short_topleft + (commonbot + 1) * SURFACE_SHORT_PITCH + startx + colnum;
         count  = yh - commonbot;
         
         while(--count >= 0)
         {
            // SoM 7-28-04: Fix the fuzz problem.
            *dest = GETBLENDED16_9406(dest[fuzzoffset[fuzzpos]], 0);
            
            // Clamp table lookup index.
            if(++fuzzpos == FUZZTABLE) 
               fuzzpos = 0;

            source += 4;
            dest += SURFACE_SHORT_PITCH;
         }
      }         
      ++colnum;
   }
}

static void R_FlushQuadFuzz16(void)
{
   uint16_t *source = &short_tempbuf[commontop << 2];
   uint16_t *dest   = drawvars.short_topleft + commontop * SURFACE_SHORT_PITCH + startx;
   int fuzz1        = fuzzpos;
   int fuzz2        = (fuzz1 + tempyl[1]) % FUZZTABLE;
   int fuzz3        = (fuzz2 + tempyl[2]) % FUZZTABLE;
   int fuzz4        = (fuzz3 + tempyl[3]) % FUZZTABLE;
   int count        = commonbot - commontop + 1;

   while(--count >= 0)
   {
      dest[0] = GETBLENDED16_9406(dest[0 + fuzzoffset[fuzz1]], 0);
      dest[1] = GETBLENDED16_9406(dest[1 + fuzzoffset[fuzz2]], 0);
      dest[2] = GETBLENDED16_9406(dest[2 + fuzzoffset[fuzz3]], 0);
      dest[3] = GETBLENDED16_9406(dest[3 + fuzzoffset[fuzz4]], 0);
      fuzz1 = (fuzz1 + 1) % FUZZTABLE;
      fuzz2 = (fuzz2 + 1) % FUZZTABLE;
      fuzz3 = (fuzz3 + 1) % FUZZTABLE;
      fuzz4 = (fuzz4 + 1) % FUZZTABLE;
      source += 4 * sizeof(uint8_t);
      dest += SURFACE_SHORT_PITCH * sizeof(uint8_t);
   }
}

//
// R_DrawColumn
//

//
// A column is a vertical slice/span from a wall texture that,
//  given the DOOM style restrictions on the view orientation,
//  will always have constant z depth.
// Thus a special case loop for very fast rendering can
//  be used. It has also been used with Wolfenstein 3D.
//

uint8_t *translationtables;

// no color mapping
static void R_DrawColumn16_PointUV(draw_column_vars_t *dcvars)
{
   int count;

   uint16_t *dest;

   fixed_t frac;
   const fixed_t fracstep = dcvars->iscale;
   const fixed_t slope_texu = dcvars->texu;
   count = dcvars->yh - dcvars->yl;

   if (count < 0)
      return;

   frac = dcvars->texturemid + (dcvars->yl-centery)*fracstep;

   if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED)
   {
      if (dcvars->yl != 0) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += 0xffff-(slope_texu & 0xffff);
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += slope_texu & 0xffff;
         }
      }
      if (dcvars->yh != viewheight-1) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
      }
      if (count <= 0) return;
   }

   if(temp_x == 4 ||
         (temp_x && (temptype != tl_temptype || temp_x + startx != dcvars->x)))
      R_FlushColumns();

   if(!temp_x)
   {
      startx = dcvars->x;
      tempyl[0] = commontop = dcvars->yl;
      tempyh[0] = commonbot = dcvars->yh;
      temptype = tl_temptype;





      R_FlushWholeColumns = tl_flush_whole;
      R_FlushHTColumns = tl_flush_ht;
      R_FlushQuadColumn = tl_flush_quad;

      dest = &short_tempbuf[dcvars->yl << 2];

   }
   else
   {
      tempyl[temp_x] = dcvars->yl;
      tempyh[temp_x] = dcvars->yh;

      if(dcvars->yl > commontop)
         commontop = dcvars->yl;
      if(dcvars->yh < commonbot)
         commonbot = dcvars->yh;


      dest = &short_tempbuf[(dcvars->yl << 2) + temp_x];

   }
   temp_x += 1;

   {
      const uint8_t *source = dcvars->source;
      /* No colormap on this path: composed palette table collapses
       * V_Palette16[texel*64+63] to lut[texel] (see R_GetComposedPalette). */
      const uint16_t *lut = R_GetComposedPalette();
      count++;

      if (dcvars->texheight == 128)
      {

         while(count--)
         {
            *dest = lut[ source[(frac & ((127<<16)|0xffff))>>16] ];
            ;
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = lut[ source[(frac)>>16] ];
            ;
            dest += 4;
            frac += fracstep;
         }
      }
      else
      {
         unsigned heightmask = dcvars->texheight-1;
         if (! (dcvars->texheight & heightmask))
         {
            fixed_t fixedt_heightmask = (heightmask<<16)|0xffff;
            while ((count-=2)>=0)
            {
               *dest = lut[ source[(frac & fixedt_heightmask)>>16] ];
               ;
               dest += 4;
               frac += fracstep;
               *dest = lut[ source[(frac & fixedt_heightmask)>>16] ];
               ;
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = lut[ source[(frac & fixedt_heightmask)>>16] ];
            ;
         }
         else
         {
            heightmask++;
            heightmask <<= 16;

            if (frac < 0)
               while ((frac += heightmask) < 0);
            else
               while (frac >= (int)heightmask)
                  frac -= heightmask;
            while (count--)
            {





               *dest = lut[ source[(frac)>>16] ];
               ;
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;



            }
         }
      }
   }
}

// simple depth color mapping
static void R_DrawColumn16_PointUV_PointZ(draw_column_vars_t *dcvars)
{
   uint16_t *dest;

   fixed_t frac;
   const fixed_t   fracstep = dcvars->iscale;
   const fixed_t slope_texu = dcvars->texu;
   int                count = dcvars->yh - dcvars->yl;

   if (count < 0)
      return;

   frac = dcvars->texturemid + (dcvars->yl-centery)*fracstep;

   if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED)
   {
      if (dcvars->yl != 0) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += 0xffff-(slope_texu & 0xffff);
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += slope_texu & 0xffff;
         }
      }
      if (dcvars->yh != viewheight-1) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
      }
      if (count <= 0) return;
   }

   if(temp_x == 4 ||
         (temp_x && (temptype != tl_temptype || temp_x + startx != dcvars->x)))
      R_FlushColumns();

   if(!temp_x)
   {
      startx = dcvars->x;
      tempyl[0] = commontop = dcvars->yl;
      tempyh[0] = commonbot = dcvars->yh;
      temptype = tl_temptype;





      R_FlushWholeColumns = tl_flush_whole;
      R_FlushHTColumns = tl_flush_ht;
      R_FlushQuadColumn = tl_flush_quad;

      dest = &short_tempbuf[dcvars->yl << 2];

   }
   else
   {
      tempyl[temp_x] = dcvars->yl;
      tempyh[temp_x] = dcvars->yh;

      if(dcvars->yl > commontop)
         commontop = dcvars->yl;
      if(dcvars->yh < commonbot)
         commonbot = dcvars->yh;


      dest = &short_tempbuf[(dcvars->yl << 2) + temp_x];

   }
   temp_x += 1;



   {
      const uint8_t *source = dcvars->source;
      /* Shared composed colormap+palette table: collapses
       * V_Palette16[colormap[texel]*64+63] to lut[texel].  Reuses the
       * table the spans (and other lit columns) already built for this
       * colormap pointer, so there is no per-column rebuild cost. */
      const uint16_t *lut = R_GetComposedColormap(dcvars->colormap);
      count++;

      /* Brightmap path: where the per-texel mask is set the texel ignores
       * the distance light and is drawn through the undimmed base map
       * (fullcolormap, band 0).  fullcolormap's composed table is snapshot
       * into a column-local array first, because the shared composed_lut
       * cache holds a single entry -- fetching the distance `lut` would
       * otherwise evict it.  Kept as one general loop (handles every
       * texheight); the SIMD select lands in a later step. */
      if (dcvars->brightmask)
      {
         const uint8_t *mask = dcvars->brightmask;
         uint16_t lut_bright[256];
         const uint16_t *bsrc = R_GetComposedColormap(fullcolormap
                                                      ? fullcolormap
                                                      : dcvars->colormap);
         unsigned heightmask = dcvars->texheight ? dcvars->texheight - 1 : 0;
         int npot = (dcvars->texheight &&
                     (dcvars->texheight & heightmask)) ? 1 : 0;
         memcpy(lut_bright, bsrc, sizeof(lut_bright));
         /* re-fetch the distance table: the snapshot above may have
          * replaced it in the shared cache */
         lut = R_GetComposedColormap(dcvars->colormap);

         if (npot)
         {
            unsigned h = dcvars->texheight;
            unsigned hs = h << 16;
            if (frac < 0)
               while ((frac += hs) < 0);
            else
               while (frac >= (int)hs)
                  frac -= hs;
            while (count--)
            {
               unsigned t = (frac >> 16);
               *dest = (mask[t] ? lut_bright : lut)[ source[t] ];
               dest += 4;
               if ((frac += fracstep) >= (int)hs) frac -= hs;
            }
         }
         else
         {
            fixed_t fmask = dcvars->texheight
                            ? ((heightmask << 16) | 0xffff)
                            : 0xffffffffu;
#if defined(__SSE2__)
            /* Vectorise the per-pixel frac->texel index for four pixels at
             * once (packed add, mask, logical shift), matching the scalar
             * (frac & fmask) >> 16 exactly.  The gather and the stride-4
             * transpose-buffer stores stay scalar -- there is no SSE2
             * gather and dest is column-interleaved (dest[0], dest[4], ...)
             * -- with a per-lane select on the mask bit.  Tail is scalar. */
            if (count >= 4)
            {
               unsigned blocks = (unsigned)count >> 2;
               __m128i vf  = _mm_set_epi32(frac + 3*fracstep, frac + 2*fracstep,
                                           frac + fracstep,   frac);
               const __m128i vfs = _mm_set1_epi32(fracstep << 2);
               const __m128i vm  = _mm_set1_epi32((int)fmask);
               unsigned consumed = blocks << 2;
               while (blocks--)
               {
                  uint32_t t[4];
                  __m128i vt = _mm_srli_epi32(_mm_and_si128(vf, vm), 16);
                  _mm_storeu_si128((__m128i *)t, vt);
                  dest[0]  = (mask[t[0]] ? lut_bright : lut)[ source[t[0]] ];
                  dest[4]  = (mask[t[1]] ? lut_bright : lut)[ source[t[1]] ];
                  dest[8]  = (mask[t[2]] ? lut_bright : lut)[ source[t[2]] ];
                  dest[12] = (mask[t[3]] ? lut_bright : lut)[ source[t[3]] ];
                  dest += 16;
                  vf = _mm_add_epi32(vf, vfs);
               }
               frac += (fixed_t)consumed * fracstep;
               count -= consumed;
            }
#elif defined(__ARM_NEON)
            if (count >= 4)
            {
               unsigned blocks = (unsigned)count >> 2;
               const int32_t fb4[4] = { frac, frac + fracstep,
                                        frac + 2*fracstep, frac + 3*fracstep };
               int32x4_t vf = vld1q_s32(fb4);
               const int32x4_t vfs = vdupq_n_s32(fracstep << 2);
               const int32x4_t vm  = vdupq_n_s32((int)fmask);
               unsigned consumed = blocks << 2;
               while (blocks--)
               {
                  uint32_t t[4];
                  uint32x4_t vt = vshrq_n_u32(
                     vreinterpretq_u32_s32(vandq_s32(vf, vm)), 16);
                  vst1q_u32(t, vt);
                  dest[0]  = (mask[t[0]] ? lut_bright : lut)[ source[t[0]] ];
                  dest[4]  = (mask[t[1]] ? lut_bright : lut)[ source[t[1]] ];
                  dest[8]  = (mask[t[2]] ? lut_bright : lut)[ source[t[2]] ];
                  dest[12] = (mask[t[3]] ? lut_bright : lut)[ source[t[3]] ];
                  dest += 16;
                  vf = vaddq_s32(vf, vfs);
               }
               frac += (fixed_t)consumed * fracstep;
               count -= consumed;
            }
#endif
            while (count--)
            {
               unsigned t = (frac & fmask) >> 16;
               *dest = (mask[t] ? lut_bright : lut)[ source[t] ];
               dest += 4;
               frac += fracstep;
            }
         }
         return;
      }

      if (dcvars->texheight == 128)
      {

         while(count--)
         {
            *dest = lut[ source[(frac & ((127<<16)|0xffff))>>16] ];
            ;
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = lut[ source[(frac)>>16] ];
            ;
            dest += 4;
            frac += fracstep;
         }
      }
      else
      {
         unsigned heightmask = dcvars->texheight-1;
         if (! (dcvars->texheight & heightmask))
         {
            fixed_t fixedt_heightmask = (heightmask<<16)|0xffff;
            while ((count-=2)>=0)
            {
               *dest = lut[ source[(frac & fixedt_heightmask)>>16] ];
               ;
               dest += 4;
               frac += fracstep;
               *dest = lut[ source[(frac & fixedt_heightmask)>>16] ];
               ;
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = lut[ source[(frac & fixedt_heightmask)>>16] ];
            ;
         }
         else
         {
            heightmask++;
            heightmask <<= 16;

            if (frac < 0)
               while ((frac += heightmask) < 0);
            else
               while (frac >= (int)heightmask)
                  frac -= heightmask;
            while (count--)
            {





               *dest = lut[ source[(frac)>>16] ];
               ;
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;



            }
         }
      }
   }

}

// z-dither
static void R_DrawColumn16_PointUV_LinearZ(draw_column_vars_t *dcvars)
{
   uint16_t *dest;

   fixed_t frac;
   const fixed_t fracstep = dcvars->iscale;
   const fixed_t slope_texu = dcvars->texu;
   int count = dcvars->yh - dcvars->yl;

   /* Brightmapped columns take the point/point drawer, the only one
    * carrying the fullbright select.  The filtered/dithered variants blend
    * several colormap lookups per pixel with no single table to redirect,
    * so a masked surface trades that smoothing for correct fullbright --
    * the same trade R_DrawColumn16_LinearUV already makes when minified. */
   if (dcvars->brightmask)
   {
      R_DrawColumn16_PointUV_PointZ(dcvars);
      return;
   }

   if (count < 0)
      return;

   frac = dcvars->texturemid + (dcvars->yl-centery)*fracstep;

   if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED)
   {
      if (dcvars->yl != 0) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += 0xffff-(slope_texu & 0xffff);
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += slope_texu & 0xffff;
         }
      }
      if (dcvars->yh != viewheight-1) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
      }
      if (count <= 0) return;
   }

      if(temp_x == 4 ||
            (temp_x && (temptype != tl_temptype || temp_x + startx != dcvars->x)))
         R_FlushColumns();

      if(!temp_x)
      {
         startx = dcvars->x;
         tempyl[0] = commontop = dcvars->yl;
         tempyh[0] = commonbot = dcvars->yh;
         temptype = tl_temptype;





         R_FlushWholeColumns = tl_flush_whole;
         R_FlushHTColumns = tl_flush_ht;
         R_FlushQuadColumn = tl_flush_quad;

         dest = &short_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tempyl[temp_x] = dcvars->yl;
         tempyh[temp_x] = dcvars->yh;

         if(dcvars->yl > commontop)
            commontop = dcvars->yl;
         if(dcvars->yh < commonbot)
            commonbot = dcvars->yh;


         dest = &short_tempbuf[(dcvars->yl << 2) + temp_x];

      }
      temp_x += 1;

   {
      const uint8_t *source = dcvars->source;

      int y = dcvars->yl;
      const int x = dcvars->x;


      const int fracz = (dcvars->z >> 6) & 255;
      const uint8_t *dither_colormaps[2] = { dcvars->colormap, dcvars->nextcolormap };
      count++;







      if (dcvars->texheight == 128)
      {

         while(count--)
         {
            *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(frac & ((127<<16)|0xffff))>>16])]))*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(frac)>>16])]))*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else
      {
         unsigned heightmask = dcvars->texheight-1;
         if (! (dcvars->texheight & heightmask))
         {
            fixed_t fixedt_heightmask = (heightmask<<16)|0xffff;
            while ((count-=2)>=0)
            {
               *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(frac & fixedt_heightmask)>>16])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(frac & fixedt_heightmask)>>16])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(frac & fixedt_heightmask)>>16])]))*64 + ((64 -1)) ]);
            (y++);
         }
         else
         {
            heightmask++;
            heightmask <<= 16;

            if (frac < 0)
               while ((frac += heightmask) < 0);
            else
               while (frac >= (int)heightmask)
                  frac -= heightmask;
            while (count--)
            {





               *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(frac)>>16])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;



            }
         }
      }
   }

}

// bilinear with no color mapping
static void R_DrawColumn16_LinearUV(draw_column_vars_t *dcvars)
{
   int count;

   uint16_t *dest;

   fixed_t frac;
   const fixed_t fracstep = dcvars->iscale;

   const fixed_t slope_texu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;

   if (dcvars->brightmask)
   {
      R_DrawColumn16_PointUV_PointZ(dcvars);
      return;
   }

   if (dcvars->iscale > drawvars.mag_threshold)
   {
      R_GetDrawColumnFunc(RDC_PIPELINE_STANDARD,
            RDRAW_FILTER_POINT,
            drawvars.filterz)(dcvars);
      return;
   }

   count = dcvars->yh - dcvars->yl;

   if (count < 0)
      return;

   frac = dcvars->texturemid - ((1<<16)>>1) + (dcvars->yl-centery)*fracstep;

   if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED)
   {
      if (dcvars->yl != 0) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += 0xffff-(slope_texu & 0xffff);
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += slope_texu & 0xffff;
         }
      }
      if (dcvars->yh != viewheight-1) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
      }
      if (count <= 0) return;
   }


   if(temp_x == 4 ||
         (temp_x && (temptype != tl_temptype || temp_x + startx != dcvars->x)))
      R_FlushColumns();

   if(!temp_x)
   {
      startx = dcvars->x;
      tempyl[0] = commontop = dcvars->yl;
      tempyh[0] = commonbot = dcvars->yh;
      temptype = tl_temptype;





      R_FlushWholeColumns = tl_flush_whole;
      R_FlushHTColumns = tl_flush_ht;
      R_FlushQuadColumn = tl_flush_quad;

      dest = &short_tempbuf[dcvars->yl << 2];

   }
   else
   {
      tempyl[temp_x] = dcvars->yl;
      tempyh[temp_x] = dcvars->yh;

      if(dcvars->yl > commontop)
         commontop = dcvars->yl;
      if(dcvars->yh < commonbot)
         commonbot = dcvars->yh;


      dest = &short_tempbuf[(dcvars->yl << 2) + temp_x];

   }
   temp_x += 1;



   {
      const uint8_t *source = dcvars->source;

      int y = dcvars->yl;

      const uint8_t *nextsource = dcvars->nextsource;
      const unsigned int filter_fracu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;







      count++;







      if (dcvars->texheight == 128)
      {

         while(count--)
         {
            *dest = (( V_Palette16[ ((nextsource[((frac+(1<<16)) & ((127<<16)|0xffff))>>16]))*64 + ((filter_fracu*((frac & ((127<<16)|0xffff))&0xffff))>>(32-6)) ] + V_Palette16[ ((source[((frac+(1<<16)) & ((127<<16)|0xffff))>>16]))*64 + (((0xffff-filter_fracu)*((frac & ((127<<16)|0xffff))&0xffff))>>(32-6)) ] + V_Palette16[ ((source[(frac & ((127<<16)|0xffff))>>16]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & ((127<<16)|0xffff))&0xffff)))>>(32-6)) ] + V_Palette16[ ((nextsource[(frac & ((127<<16)|0xffff))>>16]))*64 + ((filter_fracu*(0xffff-((frac & ((127<<16)|0xffff))&0xffff)))>>(32-6)) ]));
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (( V_Palette16[ ((nextsource[((frac+(1<<16)))>>16]))*64 + ((filter_fracu*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ ((source[((frac+(1<<16)))>>16]))*64 + (((0xffff-filter_fracu)*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ ((source[(frac)>>16]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac)&0xffff)))>>(32-6)) ] + V_Palette16[ ((nextsource[(frac)>>16]))*64 + ((filter_fracu*(0xffff-((frac)&0xffff)))>>(32-6)) ]));
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else
      {
         unsigned heightmask = dcvars->texheight-1;
         if (! (dcvars->texheight & heightmask))
         {
            fixed_t fixedt_heightmask = (heightmask<<16)|0xffff;
            while ((count-=2)>=0)
            {
               *dest = (( V_Palette16[ ((nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16]))*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((source[((frac+(1<<16)) & fixedt_heightmask)>>16]))*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((source[(frac & fixedt_heightmask)>>16]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_Palette16[ ((nextsource[(frac & fixedt_heightmask)>>16]))*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (( V_Palette16[ ((nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16]))*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((source[((frac+(1<<16)) & fixedt_heightmask)>>16]))*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((source[(frac & fixedt_heightmask)>>16]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_Palette16[ ((nextsource[(frac & fixedt_heightmask)>>16]))*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (( V_Palette16[ ((nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16]))*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((source[((frac+(1<<16)) & fixedt_heightmask)>>16]))*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((source[(frac & fixedt_heightmask)>>16]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_Palette16[ ((nextsource[(frac & fixedt_heightmask)>>16]))*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
            (y++);
         }
         else
         {
            fixed_t nextfrac = 0;

            heightmask++;
            heightmask <<= 16;

            if (frac < 0)
               while ((frac += heightmask) < 0);
            else
               while (frac >= (int)heightmask)
                  frac -= heightmask;


            nextfrac = frac + (1<<16);
            while (nextfrac >= (int)heightmask)
               nextfrac -= heightmask;




            while (count--)
            {





               *dest = (( V_Palette16[ ((nextsource[(nextfrac)>>16]))*64 + ((filter_fracu*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ ((source[(nextfrac)>>16]))*64 + (((0xffff-filter_fracu)*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ ((source[(frac)>>16]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac)&0xffff)))>>(32-6)) ] + V_Palette16[ ((nextsource[(frac)>>16]))*64 + ((filter_fracu*(0xffff-((frac)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;

               if ((nextfrac += fracstep) >= (int)heightmask) nextfrac -= heightmask;;

            }
         }
      }
   }

}

// bilinear with simple depth color mapping
static void R_DrawColumn16_LinearUV_PointZ(draw_column_vars_t *dcvars)
{
   int count;

   uint16_t *dest;

   fixed_t frac;
   const fixed_t fracstep = dcvars->iscale;

   const fixed_t slope_texu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;

   if (dcvars->brightmask)
   {
      R_DrawColumn16_PointUV_PointZ(dcvars);
      return;
   }

   if (dcvars->iscale > drawvars.mag_threshold)
   {
      R_GetDrawColumnFunc(RDC_PIPELINE_STANDARD,
            RDRAW_FILTER_POINT,
            drawvars.filterz)(dcvars);
      return;
   }

   count = dcvars->yh - dcvars->yl;

   if (count < 0)
      return;

   frac = dcvars->texturemid - ((1<<16)>>1) + (dcvars->yl-centery)*fracstep;

   if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED)
   {
      if (dcvars->yl != 0) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += 0xffff-(slope_texu & 0xffff);
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += slope_texu & 0xffff;
         }
      }
      if (dcvars->yh != viewheight-1) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
      }
      if (count <= 0) return;
   }

   if(temp_x == 4 ||
         (temp_x && (temptype != tl_temptype || temp_x + startx != dcvars->x)))
      R_FlushColumns();

   if(!temp_x)
   {
      startx = dcvars->x;
      tempyl[0] = commontop = dcvars->yl;
      tempyh[0] = commonbot = dcvars->yh;
      temptype = tl_temptype;





      R_FlushWholeColumns = tl_flush_whole;
      R_FlushHTColumns = tl_flush_ht;
      R_FlushQuadColumn = tl_flush_quad;

      dest = &short_tempbuf[dcvars->yl << 2];

   }
   else
   {
      tempyl[temp_x] = dcvars->yl;
      tempyh[temp_x] = dcvars->yh;

      if(dcvars->yl > commontop)
         commontop = dcvars->yl;
      if(dcvars->yh < commonbot)
         commonbot = dcvars->yh;


      dest = &short_tempbuf[(dcvars->yl << 2) + temp_x];

   }
   temp_x += 1;



   {
      const uint8_t *source = dcvars->source;
      const lighttable_t *colormap = dcvars->colormap;

      int y = dcvars->yl;

      const uint8_t *nextsource = dcvars->nextsource;
      const unsigned int filter_fracu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;

      count++;

      if (dcvars->texheight == 128)
      {

         while(count--)
         {
            *dest = (( V_Palette16[ (colormap[(nextsource[((frac+(1<<16)) & ((127<<16)|0xffff))>>16])])*64 + ((filter_fracu*((frac & ((127<<16)|0xffff))&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(source[((frac+(1<<16)) & ((127<<16)|0xffff))>>16])])*64 + (((0xffff-filter_fracu)*((frac & ((127<<16)|0xffff))&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(source[(frac & ((127<<16)|0xffff))>>16])])*64 + (((0xffff-filter_fracu)*(0xffff-((frac & ((127<<16)|0xffff))&0xffff)))>>(32-6)) ] + V_Palette16[ (colormap[(nextsource[(frac & ((127<<16)|0xffff))>>16])])*64 + ((filter_fracu*(0xffff-((frac & ((127<<16)|0xffff))&0xffff)))>>(32-6)) ]));
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (( V_Palette16[ (colormap[(nextsource[((frac+(1<<16)))>>16])])*64 + ((filter_fracu*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(source[((frac+(1<<16)))>>16])])*64 + (((0xffff-filter_fracu)*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(source[(frac)>>16])])*64 + (((0xffff-filter_fracu)*(0xffff-((frac)&0xffff)))>>(32-6)) ] + V_Palette16[ (colormap[(nextsource[(frac)>>16])])*64 + ((filter_fracu*(0xffff-((frac)&0xffff)))>>(32-6)) ]));
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else
      {
         unsigned heightmask = dcvars->texheight-1;
         if (! (dcvars->texheight & heightmask))
         {
            fixed_t fixedt_heightmask = (heightmask<<16)|0xffff;
            while ((count-=2)>=0)
            {
               *dest = (( V_Palette16[ (colormap[(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])])*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(source[((frac+(1<<16)) & fixedt_heightmask)>>16])])*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(source[(frac & fixedt_heightmask)>>16])])*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_Palette16[ (colormap[(nextsource[(frac & fixedt_heightmask)>>16])])*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (( V_Palette16[ (colormap[(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])])*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(source[((frac+(1<<16)) & fixedt_heightmask)>>16])])*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(source[(frac & fixedt_heightmask)>>16])])*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_Palette16[ (colormap[(nextsource[(frac & fixedt_heightmask)>>16])])*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (( V_Palette16[ (colormap[(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])])*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(source[((frac+(1<<16)) & fixedt_heightmask)>>16])])*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(source[(frac & fixedt_heightmask)>>16])])*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_Palette16[ (colormap[(nextsource[(frac & fixedt_heightmask)>>16])])*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
            (y++);
         }
         else
         {
            fixed_t nextfrac = 0;

            heightmask++;
            heightmask <<= 16;

            if (frac < 0)
               while ((frac += heightmask) < 0);
            else
               while (frac >= (int)heightmask)
                  frac -= heightmask;


            nextfrac = frac + (1<<16);
            while (nextfrac >= (int)heightmask)
               nextfrac -= heightmask;

            while (count--)
            {
               *dest = (( V_Palette16[ (colormap[(nextsource[(nextfrac)>>16])])*64 + ((filter_fracu*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(source[(nextfrac)>>16])])*64 + (((0xffff-filter_fracu)*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(source[(frac)>>16])])*64 + (((0xffff-filter_fracu)*(0xffff-((frac)&0xffff)))>>(32-6)) ] + V_Palette16[ (colormap[(nextsource[(frac)>>16])])*64 + ((filter_fracu*(0xffff-((frac)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;

               if ((nextfrac += fracstep) >= (int)heightmask) nextfrac -= heightmask;;

            }
         }
      }
   }
}

// bilinear + z-dither
static void R_DrawColumn16_LinearUV_LinearZ(draw_column_vars_t *dcvars)
{
   int count;

   uint16_t *dest;

   fixed_t frac;
   const fixed_t fracstep = dcvars->iscale;

   const fixed_t slope_texu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;

   if (dcvars->brightmask)
   {
      R_DrawColumn16_PointUV_PointZ(dcvars);
      return;
   }

   if (dcvars->iscale > drawvars.mag_threshold)
   {
      R_GetDrawColumnFunc(RDC_PIPELINE_STANDARD,
            RDRAW_FILTER_POINT,
            drawvars.filterz)(dcvars);
      return;
   }

   count = dcvars->yh - dcvars->yl;

   if (count < 0)
      return;

   frac = dcvars->texturemid - ((1<<16)>>1) + (dcvars->yl-centery)*fracstep;

   if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED)
   {
      if (dcvars->yl != 0) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += 0xffff-(slope_texu & 0xffff);
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += slope_texu & 0xffff;
         }
      }
      if (dcvars->yh != viewheight-1) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
      }
      if (count <= 0) return;
   }

   if(temp_x == 4 ||
         (temp_x && (temptype != tl_temptype || temp_x + startx != dcvars->x)))
      R_FlushColumns();

   if(!temp_x)
   {
      startx = dcvars->x;
      tempyl[0] = commontop = dcvars->yl;
      tempyh[0] = commonbot = dcvars->yh;
      temptype = tl_temptype;





      R_FlushWholeColumns = tl_flush_whole;
      R_FlushHTColumns = tl_flush_ht;
      R_FlushQuadColumn = tl_flush_quad;

      dest = &short_tempbuf[dcvars->yl << 2];

   }
   else
   {
      tempyl[temp_x] = dcvars->yl;
      tempyh[temp_x] = dcvars->yh;

      if(dcvars->yl > commontop)
         commontop = dcvars->yl;
      if(dcvars->yh < commonbot)
         commonbot = dcvars->yh;


      dest = &short_tempbuf[(dcvars->yl << 2) + temp_x];

   }
   temp_x += 1;



   {
      const uint8_t *source = dcvars->source;

      int y = dcvars->yl;
      const int x = dcvars->x;


      const int fracz = (dcvars->z >> 6) & 255;
      const uint8_t *dither_colormaps[2] = { dcvars->colormap, dcvars->nextcolormap };


      const uint8_t *nextsource = dcvars->nextsource;
      const unsigned int filter_fracu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;

      count++;

      if (dcvars->texheight == 128)
      {

         while(count--)
         {
            *dest = (( V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(nextsource[((frac+(1<<16)) & ((127<<16)|0xffff))>>16])]))*64 + ((filter_fracu*((frac & ((127<<16)|0xffff))&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[((frac+(1<<16)) & ((127<<16)|0xffff))>>16])]))*64 + (((0xffff-filter_fracu)*((frac & ((127<<16)|0xffff))&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(frac & ((127<<16)|0xffff))>>16])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & ((127<<16)|0xffff))&0xffff)))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(nextsource[(frac & ((127<<16)|0xffff))>>16])]))*64 + ((filter_fracu*(0xffff-((frac & ((127<<16)|0xffff))&0xffff)))>>(32-6)) ]));
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (( V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(nextsource[((frac+(1<<16)))>>16])]))*64 + ((filter_fracu*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[((frac+(1<<16)))>>16])]))*64 + (((0xffff-filter_fracu)*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(frac)>>16])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac)&0xffff)))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(nextsource[(frac)>>16])]))*64 + ((filter_fracu*(0xffff-((frac)&0xffff)))>>(32-6)) ]));
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else
      {
         unsigned heightmask = dcvars->texheight-1;
         if (! (dcvars->texheight & heightmask))
         {
            fixed_t fixedt_heightmask = (heightmask<<16)|0xffff;
            while ((count-=2)>=0)
            {
               *dest = (( V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])]))*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[((frac+(1<<16)) & fixedt_heightmask)>>16])]))*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(frac & fixedt_heightmask)>>16])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(nextsource[(frac & fixedt_heightmask)>>16])]))*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (( V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])]))*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[((frac+(1<<16)) & fixedt_heightmask)>>16])]))*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(frac & fixedt_heightmask)>>16])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(nextsource[(frac & fixedt_heightmask)>>16])]))*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (( V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])]))*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[((frac+(1<<16)) & fixedt_heightmask)>>16])]))*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(frac & fixedt_heightmask)>>16])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(nextsource[(frac & fixedt_heightmask)>>16])]))*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
            (y++);
         }
         else
         {
            fixed_t nextfrac = 0;

            heightmask++;
            heightmask <<= 16;

            if (frac < 0)
               while ((frac += heightmask) < 0);
            else
               while (frac >= (int)heightmask)
                  frac -= heightmask;


            nextfrac = frac + (1<<16);
            while (nextfrac >= (int)heightmask)
               nextfrac -= heightmask;




            while (count--)
            {





               *dest = (( V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(nextsource[(nextfrac)>>16])]))*64 + ((filter_fracu*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(nextfrac)>>16])]))*64 + (((0xffff-filter_fracu)*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(frac)>>16])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac)&0xffff)))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(nextsource[(frac)>>16])]))*64 + ((filter_fracu*(0xffff-((frac)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;

               if ((nextfrac += fracstep) >= (int)heightmask) nextfrac -= heightmask;;

            }
         }
      }
   }

}

/* rounded with no color mapping */
static void R_DrawColumn16_RoundedUV(draw_column_vars_t *dcvars)
{
  int count;

  uint16_t *dest;

  fixed_t frac;
  const fixed_t fracstep = dcvars->iscale;
  const fixed_t slope_texu = dcvars->texu;

  if (dcvars->brightmask)
  {
     R_DrawColumn16_PointUV_PointZ(dcvars);
     return;
  }

  if (dcvars->iscale > drawvars.mag_threshold)
  {
    R_GetDrawColumnFunc(RDC_PIPELINE_STANDARD,
                        RDRAW_FILTER_POINT,
                        drawvars.filterz)(dcvars);
    return;
  }
  count = dcvars->yh - dcvars->yl;

  if (count < 0)
    return;

  frac = dcvars->texturemid + (dcvars->yl-centery)*fracstep;

  if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED)
  {
     if (dcvars->yl != 0) {
        if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

           int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
           dcvars->yl += shift;
           count -= shift;
           frac += 0xffff-(slope_texu & 0xffff);
        }
        else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

           int shift = ((slope_texu & 0xffff)/dcvars->iscale);
           dcvars->yl += shift;
           count -= shift;
           frac += slope_texu & 0xffff;
        }
     }
     if (dcvars->yh != viewheight-1) {
        if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

           int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
           dcvars->yh -= shift;
           count -= shift;
        }
        else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

           int shift = ((slope_texu & 0xffff)/dcvars->iscale);
           dcvars->yh -= shift;
           count -= shift;
        }
     }
     if (count <= 0) return;
  }



   {

      if(temp_x == 4 ||
         (temp_x && (temptype != tl_temptype || temp_x + startx != dcvars->x)))
         R_FlushColumns();

      if(!temp_x)
      {
         startx = dcvars->x;
         tempyl[0] = commontop = dcvars->yl;
         tempyh[0] = commonbot = dcvars->yh;
         temptype = tl_temptype;





         R_FlushWholeColumns = tl_flush_whole;
         R_FlushHTColumns = tl_flush_ht;
         R_FlushQuadColumn = tl_flush_quad;

         dest = &short_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tempyl[temp_x] = dcvars->yl;
         tempyh[temp_x] = dcvars->yh;

         if(dcvars->yl > commontop)
            commontop = dcvars->yl;
         if(dcvars->yh < commonbot)
            commonbot = dcvars->yh;


         dest = &short_tempbuf[(dcvars->yl << 2) + temp_x];

      }
      temp_x += 1;
   }



   {
      const uint8_t *source = dcvars->source;

      int y = dcvars->yl;
      const uint8_t *prevsource = dcvars->prevsource;
      const uint8_t *nextsource = dcvars->nextsource;
      const unsigned int filter_fracu = (dcvars->source == dcvars->nextsource) ? 0 : (dcvars->texu>>8) & 0xff;


      count++;







      if (dcvars->texheight == 128)
      {

         while(count--)
         {
            *dest = (V_Palette16[ ((filter_getScale2xQuadColors( source[ ((frac & ((127<<16)|0xffff))>>16) ], source[ (((0)>(((frac & ((127<<16)|0xffff))>>16)-1)?(0):(((frac & ((127<<16)|0xffff))>>16)-1))) ], nextsource[ ((frac & ((127<<16)|0xffff))>>16) ], source[ (((frac+(1<<16)) & ((127<<16)|0xffff))>>16) ], prevsource[ ((frac & ((127<<16)|0xffff))>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & ((127<<16)|0xffff))>>8) & 0xff)>>(8-6)) ] ]))*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (V_Palette16[ ((filter_getScale2xQuadColors( source[ ((frac)>>16) ], source[ (((0)>(((frac)>>16)-1)?(0):(((frac)>>16)-1))) ], nextsource[ ((frac)>>16) ], source[ (((frac+(1<<16)))>>16) ], prevsource[ ((frac)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac)>>8) & 0xff)>>(8-6)) ] ]))*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else
      {
         unsigned heightmask = dcvars->texheight-1;
         if (! (dcvars->texheight & heightmask))
         {
            fixed_t fixedt_heightmask = (heightmask<<16)|0xffff;
            while ((count-=2)>=0)
            {
               *dest = (V_Palette16[ ((filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (V_Palette16[ ((filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (V_Palette16[ ((filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ]))*64 + ((64 -1)) ]);
            (y++);
         }
         else
         {
            fixed_t nextfrac = 0;

            heightmask++;
            heightmask <<= 16;

            if (frac < 0)
               while ((frac += heightmask) < 0);
            else
               while (frac >= (int)heightmask)
                  frac -= heightmask;


            nextfrac = frac + (1<<16);
            while (nextfrac >= (int)heightmask)
               nextfrac -= heightmask;




            while (count--)
            {





               *dest = (V_Palette16[ ((filter_getScale2xQuadColors( source[ ((frac)>>16) ], source[ (((0)>(((frac)>>16)-1)?(0):(((frac)>>16)-1))) ], nextsource[ ((frac)>>16) ], source[ ((nextfrac)>>16) ], prevsource[ ((frac)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac)>>8) & 0xff)>>(8-6)) ] ]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;

               if ((nextfrac += fracstep) >= (int)heightmask) nextfrac -= heightmask;;

            }
         }
      }
   }

}

/* rounded with simple depth color mapping */
static void R_DrawColumn16_RoundedUV_PointZ(draw_column_vars_t *dcvars)
{
   int count;

   uint16_t *dest;

   fixed_t frac;
   const fixed_t fracstep = dcvars->iscale;
   const fixed_t slope_texu = dcvars->texu;

   if (dcvars->brightmask)
   {
      R_DrawColumn16_PointUV_PointZ(dcvars);
      return;
   }

   if (dcvars->iscale > drawvars.mag_threshold)
   {
      R_GetDrawColumnFunc(RDC_PIPELINE_STANDARD,
            RDRAW_FILTER_POINT,
            drawvars.filterz)(dcvars);
      return;
   }
   count = dcvars->yh - dcvars->yl;

   if (count < 0)
      return;

   frac = dcvars->texturemid + (dcvars->yl-centery)*fracstep;


   if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED) {



      if (dcvars->yl != 0) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += 0xffff-(slope_texu & 0xffff);
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += slope_texu & 0xffff;
         }
      }
      if (dcvars->yh != viewheight-1) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
      }
      if (count <= 0) return;
   }



   {

      if(temp_x == 4 ||
            (temp_x && (temptype != tl_temptype || temp_x + startx != dcvars->x)))
         R_FlushColumns();

      if(!temp_x)
      {
         startx = dcvars->x;
         tempyl[0] = commontop = dcvars->yl;
         tempyh[0] = commonbot = dcvars->yh;
         temptype = tl_temptype;





         R_FlushWholeColumns = tl_flush_whole;
         R_FlushHTColumns = tl_flush_ht;
         R_FlushQuadColumn = tl_flush_quad;

         dest = &short_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tempyl[temp_x] = dcvars->yl;
         tempyh[temp_x] = dcvars->yh;

         if(dcvars->yl > commontop)
            commontop = dcvars->yl;
         if(dcvars->yh < commonbot)
            commonbot = dcvars->yh;


         dest = &short_tempbuf[(dcvars->yl << 2) + temp_x];

      }
      temp_x += 1;
   }



   {
      const uint8_t *source = dcvars->source;
      const lighttable_t *colormap = dcvars->colormap;

      int y = dcvars->yl;
      const uint8_t *prevsource = dcvars->prevsource;
      const uint8_t *nextsource = dcvars->nextsource;
      const unsigned int filter_fracu = (dcvars->source == dcvars->nextsource) ? 0 : (dcvars->texu>>8) & 0xff;


      count++;







      if (dcvars->texheight == 128)
      {

         while(count--)
         {
            *dest = (V_Palette16[ (colormap[(filter_getScale2xQuadColors( source[ ((frac & ((127<<16)|0xffff))>>16) ], source[ (((0)>(((frac & ((127<<16)|0xffff))>>16)-1)?(0):(((frac & ((127<<16)|0xffff))>>16)-1))) ], nextsource[ ((frac & ((127<<16)|0xffff))>>16) ], source[ (((frac+(1<<16)) & ((127<<16)|0xffff))>>16) ], prevsource[ ((frac & ((127<<16)|0xffff))>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & ((127<<16)|0xffff))>>8) & 0xff)>>(8-6)) ] ])])*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (V_Palette16[ (colormap[(filter_getScale2xQuadColors( source[ ((frac)>>16) ], source[ (((0)>(((frac)>>16)-1)?(0):(((frac)>>16)-1))) ], nextsource[ ((frac)>>16) ], source[ (((frac+(1<<16)))>>16) ], prevsource[ ((frac)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac)>>8) & 0xff)>>(8-6)) ] ])])*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else
      {
         unsigned heightmask = dcvars->texheight-1;
         if (! (dcvars->texheight & heightmask))
         {
            fixed_t fixedt_heightmask = (heightmask<<16)|0xffff;
            while ((count-=2)>=0)
            {
               *dest = (V_Palette16[ (colormap[(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])])*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (V_Palette16[ (colormap[(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])])*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (V_Palette16[ (colormap[(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])])*64 + ((64 -1)) ]);
            (y++);
         }
         else
         {
            fixed_t nextfrac = 0;

            heightmask++;
            heightmask <<= 16;

            if (frac < 0)
               while ((frac += heightmask) < 0);
            else
               while (frac >= (int)heightmask)
                  frac -= heightmask;


            nextfrac = frac + (1<<16);
            while (nextfrac >= (int)heightmask)
               nextfrac -= heightmask;




            while (count--)
            {





               *dest = (V_Palette16[ (colormap[(filter_getScale2xQuadColors( source[ ((frac)>>16) ], source[ (((0)>(((frac)>>16)-1)?(0):(((frac)>>16)-1))) ], nextsource[ ((frac)>>16) ], source[ ((nextfrac)>>16) ], prevsource[ ((frac)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac)>>8) & 0xff)>>(8-6)) ] ])])*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;

               if ((nextfrac += fracstep) >= (int)heightmask) nextfrac -= heightmask;;

            }
         }
      }
   }

}

/* rounded + z-dither */
static void R_DrawColumn16_RoundedUV_LinearZ(draw_column_vars_t *dcvars)
{
   int count;

   uint16_t *dest;

   fixed_t frac;
   const fixed_t fracstep = dcvars->iscale;
   const fixed_t slope_texu = dcvars->texu;

   if (dcvars->brightmask)
   {
      R_DrawColumn16_PointUV_PointZ(dcvars);
      return;
   }

   if (dcvars->iscale > drawvars.mag_threshold)
   {
      R_GetDrawColumnFunc(RDC_PIPELINE_STANDARD,
            RDRAW_FILTER_POINT,
            drawvars.filterz)(dcvars);
      return;
   }

   count = dcvars->yh - dcvars->yl;

   if (count < 0)
      return;

   frac = dcvars->texturemid + (dcvars->yl-centery)*fracstep;


   if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED) {



      if (dcvars->yl != 0) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += 0xffff-(slope_texu & 0xffff);
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += slope_texu & 0xffff;
         }
      }
      if (dcvars->yh != viewheight-1) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
      }
      if (count <= 0) return;
   }



   {

      if(temp_x == 4 ||
            (temp_x && (temptype != tl_temptype || temp_x + startx != dcvars->x)))
         R_FlushColumns();

      if(!temp_x)
      {
         startx = dcvars->x;
         tempyl[0] = commontop = dcvars->yl;
         tempyh[0] = commonbot = dcvars->yh;
         temptype = tl_temptype;





         R_FlushWholeColumns = tl_flush_whole;
         R_FlushHTColumns = tl_flush_ht;
         R_FlushQuadColumn = tl_flush_quad;

         dest = &short_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tempyl[temp_x] = dcvars->yl;
         tempyh[temp_x] = dcvars->yh;

         if(dcvars->yl > commontop)
            commontop = dcvars->yl;
         if(dcvars->yh < commonbot)
            commonbot = dcvars->yh;


         dest = &short_tempbuf[(dcvars->yl << 2) + temp_x];

      }
      temp_x += 1;
   }



   {
      const uint8_t *source = dcvars->source;

      int y = dcvars->yl;
      const int x = dcvars->x;


      const int fracz = (dcvars->z >> 6) & 255;
      const uint8_t *dither_colormaps[2] = { dcvars->colormap, dcvars->nextcolormap };






      const uint8_t *prevsource = dcvars->prevsource;
      const uint8_t *nextsource = dcvars->nextsource;
      const unsigned int filter_fracu = (dcvars->source == dcvars->nextsource) ? 0 : (dcvars->texu>>8) & 0xff;


      count++;







      if (dcvars->texheight == 128)
      {

         while(count--)
         {
            *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(filter_getScale2xQuadColors( source[ ((frac & ((127<<16)|0xffff))>>16) ], source[ (((0)>(((frac & ((127<<16)|0xffff))>>16)-1)?(0):(((frac & ((127<<16)|0xffff))>>16)-1))) ], nextsource[ ((frac & ((127<<16)|0xffff))>>16) ], source[ (((frac+(1<<16)) & ((127<<16)|0xffff))>>16) ], prevsource[ ((frac & ((127<<16)|0xffff))>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & ((127<<16)|0xffff))>>8) & 0xff)>>(8-6)) ] ])]))*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(filter_getScale2xQuadColors( source[ ((frac)>>16) ], source[ (((0)>(((frac)>>16)-1)?(0):(((frac)>>16)-1))) ], nextsource[ ((frac)>>16) ], source[ (((frac+(1<<16)))>>16) ], prevsource[ ((frac)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac)>>8) & 0xff)>>(8-6)) ] ])]))*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else
      {
         unsigned heightmask = dcvars->texheight-1;
         if (! (dcvars->texheight & heightmask))
         {
            fixed_t fixedt_heightmask = (heightmask<<16)|0xffff;
            while ((count-=2)>=0)
            {
               *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])]))*64 + ((64 -1)) ]);
            (y++);
         }
         else
         {
            fixed_t nextfrac = 0;

            heightmask++;
            heightmask <<= 16;

            if (frac < 0)
               while ((frac += heightmask) < 0);
            else
               while (frac >= (int)heightmask)
                  frac -= heightmask;


            nextfrac = frac + (1<<16);
            while (nextfrac >= (int)heightmask)
               nextfrac -= heightmask;




            while (count--)
            {





               *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(filter_getScale2xQuadColors( source[ ((frac)>>16) ], source[ (((0)>(((frac)>>16)-1)?(0):(((frac)>>16)-1))) ], nextsource[ ((frac)>>16) ], source[ ((nextfrac)>>16) ], prevsource[ ((frac)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac)>>8) & 0xff)>>(8-6)) ] ])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;

               if ((nextfrac += fracstep) >= (int)heightmask) nextfrac -= heightmask;;

            }
         }
      }
   }
}

//
// R_DrawTranslatedColumn
// Used to draw player sprites
//  with the green colorramp mapped to others.
// Could be used with different translation
//  tables, e.g. the lighter colored version
//  of the BaronOfHell, the HellKnight, uses
//  identical sprites, kinda brightened up.
//

static void R_DrawTranslatedColumn16_PointUV(draw_column_vars_t *dcvars)
{
   int count;

   uint16_t *dest;

   fixed_t frac;
   const fixed_t fracstep = dcvars->iscale;
   const fixed_t slope_texu = dcvars->texu;
   count = dcvars->yh - dcvars->yl;

   if (count < 0)
      return;

   frac = dcvars->texturemid + (dcvars->yl-centery)*fracstep;

   if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED)
   {
      if (dcvars->yl != 0) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += 0xffff-(slope_texu & 0xffff);
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += slope_texu & 0xffff;
         }
      }
      if (dcvars->yh != viewheight-1) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
      }
      if (count <= 0) return;
   }

   if(temp_x == 4 ||
         (temp_x && (temptype != tl_temptype || temp_x + startx != dcvars->x)))
      R_FlushColumns();

   if(!temp_x)
   {
      startx = dcvars->x;
      tempyl[0] = commontop = dcvars->yl;
      tempyh[0] = commonbot = dcvars->yh;
      temptype = tl_temptype;





      R_FlushWholeColumns = tl_flush_whole;
      R_FlushHTColumns = tl_flush_ht;
      R_FlushQuadColumn = tl_flush_quad;

      dest = &short_tempbuf[dcvars->yl << 2];

   }
   else
   {
      tempyl[temp_x] = dcvars->yl;
      tempyh[temp_x] = dcvars->yh;

      if(dcvars->yl > commontop)
         commontop = dcvars->yl;
      if(dcvars->yh < commonbot)
         commonbot = dcvars->yh;


      dest = &short_tempbuf[(dcvars->yl << 2) + temp_x];

   }
   temp_x += 1;



   {
      const uint8_t *source = dcvars->source;
      const uint8_t *translation = dcvars->translation;
      count++;







      if (dcvars->texheight == 128)
      {

         while(count--)
         {
            *dest = (V_Palette16[ ((translation[(source[(frac & ((127<<16)|0xffff))>>16])]))*64 + ((64 -1)) ]);
            ;
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (V_Palette16[ ((translation[(source[(frac)>>16])]))*64 + ((64 -1)) ]);
            ;
            dest += 4;
            frac += fracstep;
         }
      }
      else
      {
         unsigned heightmask = dcvars->texheight-1;
         if (! (dcvars->texheight & heightmask))
         {
            fixed_t fixedt_heightmask = (heightmask<<16)|0xffff;
            while ((count-=2)>=0)
            {
               *dest = (V_Palette16[ ((translation[(source[(frac & fixedt_heightmask)>>16])]))*64 + ((64 -1)) ]);
               ;
               dest += 4;
               frac += fracstep;
               *dest = (V_Palette16[ ((translation[(source[(frac & fixedt_heightmask)>>16])]))*64 + ((64 -1)) ]);
               ;
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (V_Palette16[ ((translation[(source[(frac & fixedt_heightmask)>>16])]))*64 + ((64 -1)) ]);
            ;
         }
         else
         {
            heightmask++;
            heightmask <<= 16;

            if (frac < 0)
               while ((frac += heightmask) < 0);
            else
               while (frac >= (int)heightmask)
                  frac -= heightmask;
            while (count--)
            {





               *dest = (V_Palette16[ ((translation[(source[(frac)>>16])]))*64 + ((64 -1)) ]);
               ;
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;



            }
         }
      }
   }

}

static void R_DrawTranslatedColumn16_PointUV_PointZ(draw_column_vars_t *dcvars)
{
   int count;

   uint16_t *dest;

   fixed_t frac;
   const fixed_t fracstep = dcvars->iscale;
   const fixed_t slope_texu = dcvars->texu;
   count = dcvars->yh - dcvars->yl;

   if (count < 0)
      return;

   frac = dcvars->texturemid + (dcvars->yl-centery)*fracstep;

   if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED)
   {
      if (dcvars->yl != 0) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += 0xffff-(slope_texu & 0xffff);
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += slope_texu & 0xffff;
         }
      }
      if (dcvars->yh != viewheight-1) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
      }
      if (count <= 0) return;
   }

   if(temp_x == 4 ||
         (temp_x && (temptype != tl_temptype || temp_x + startx != dcvars->x)))
      R_FlushColumns();

   if(!temp_x)
   {
      startx = dcvars->x;
      tempyl[0] = commontop = dcvars->yl;
      tempyh[0] = commonbot = dcvars->yh;
      temptype = tl_temptype;





      R_FlushWholeColumns = tl_flush_whole;
      R_FlushHTColumns = tl_flush_ht;
      R_FlushQuadColumn = tl_flush_quad;

      dest = &short_tempbuf[dcvars->yl << 2];

   }
   else
   {
      tempyl[temp_x] = dcvars->yl;
      tempyh[temp_x] = dcvars->yh;

      if(dcvars->yl > commontop)
         commontop = dcvars->yl;
      if(dcvars->yh < commonbot)
         commonbot = dcvars->yh;


      dest = &short_tempbuf[(dcvars->yl << 2) + temp_x];

   }
   temp_x += 1;



   {
      const uint8_t *source = dcvars->source;
      const lighttable_t *colormap = dcvars->colormap;
      const uint8_t *translation = dcvars->translation;
      count++;







      if (dcvars->texheight == 128)
      {

         while(count--)
         {
            *dest = (V_Palette16[ (colormap[(translation[(source[(frac & ((127<<16)|0xffff))>>16])])])*64 + ((64 -1)) ]);
            ;
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (V_Palette16[ (colormap[(translation[(source[(frac)>>16])])])*64 + ((64 -1)) ]);
            ;
            dest += 4;
            frac += fracstep;
         }
      }
      else
      {
         unsigned heightmask = dcvars->texheight-1;
         if (! (dcvars->texheight & heightmask))
         {
            fixed_t fixedt_heightmask = (heightmask<<16)|0xffff;
            while ((count-=2)>=0)
            {
               *dest = (V_Palette16[ (colormap[(translation[(source[(frac & fixedt_heightmask)>>16])])])*64 + ((64 -1)) ]);
               ;
               dest += 4;
               frac += fracstep;
               *dest = (V_Palette16[ (colormap[(translation[(source[(frac & fixedt_heightmask)>>16])])])*64 + ((64 -1)) ]);
               ;
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (V_Palette16[ (colormap[(translation[(source[(frac & fixedt_heightmask)>>16])])])*64 + ((64 -1)) ]);
            ;
         }
         else
         {
            heightmask++;
            heightmask <<= 16;

            if (frac < 0)
               while ((frac += heightmask) < 0);
            else
               while (frac >= (int)heightmask)
                  frac -= heightmask;
            while (count--)
            {
               *dest = (V_Palette16[ (colormap[(translation[(source[(frac)>>16])])])*64 + ((64 -1)) ]);
               ;
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;
            }
         }
      }
   }

}


static void R_DrawTranslatedColumn16_PointUV_LinearZ(draw_column_vars_t *dcvars)
{
  int count;

  uint16_t *dest;

  fixed_t frac;
  const fixed_t fracstep = dcvars->iscale;
  const fixed_t slope_texu = dcvars->texu;
  count = dcvars->yh - dcvars->yl;

  if (count < 0)
    return;

  frac = dcvars->texturemid + (dcvars->yl-centery)*fracstep;

  if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED)
  {
     if (dcvars->yl != 0) {
        if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

           int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
           dcvars->yl += shift;
           count -= shift;
           frac += 0xffff-(slope_texu & 0xffff);
        }
        else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

           int shift = ((slope_texu & 0xffff)/dcvars->iscale);
           dcvars->yl += shift;
           count -= shift;
           frac += slope_texu & 0xffff;
        }
     }
     if (dcvars->yh != viewheight-1) {
        if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

           int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
           dcvars->yh -= shift;
           count -= shift;
        }
        else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

           int shift = ((slope_texu & 0xffff)/dcvars->iscale);
           dcvars->yh -= shift;
           count -= shift;
        }
     }
     if (count <= 0) return;
  }



   {

      if(temp_x == 4 ||
         (temp_x && (temptype != tl_temptype || temp_x + startx != dcvars->x)))
         R_FlushColumns();

      if(!temp_x)
      {
         startx = dcvars->x;
         tempyl[0] = commontop = dcvars->yl;
         tempyh[0] = commonbot = dcvars->yh;
         temptype = tl_temptype;





         R_FlushWholeColumns = tl_flush_whole;
         R_FlushHTColumns = tl_flush_ht;
         R_FlushQuadColumn = tl_flush_quad;

         dest = &short_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tempyl[temp_x] = dcvars->yl;
         tempyh[temp_x] = dcvars->yh;

         if(dcvars->yl > commontop)
            commontop = dcvars->yl;
         if(dcvars->yh < commonbot)
            commonbot = dcvars->yh;


         dest = &short_tempbuf[(dcvars->yl << 2) + temp_x];

      }
      temp_x += 1;
   }



   {
      const uint8_t *source = dcvars->source;
      const uint8_t *translation = dcvars->translation;

      int y = dcvars->yl;
      const int x = dcvars->x;


      const int fracz = (dcvars->z >> 6) & 255;
      const uint8_t *dither_colormaps[2] = { dcvars->colormap, dcvars->nextcolormap };
      count++;







      if (dcvars->texheight == 128)
      {

         while(count--)
         {
            *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(frac & ((127<<16)|0xffff))>>16])])]))*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(frac)>>16])])]))*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else
      {
         unsigned heightmask = dcvars->texheight-1;
         if (! (dcvars->texheight & heightmask))
         {
            fixed_t fixedt_heightmask = (heightmask<<16)|0xffff;
            while ((count-=2)>=0)
            {
               *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(frac & fixedt_heightmask)>>16])])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(frac & fixedt_heightmask)>>16])])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(frac & fixedt_heightmask)>>16])])]))*64 + ((64 -1)) ]);
            (y++);
         }
         else
         {
            heightmask++;
            heightmask <<= 16;

            if (frac < 0)
               while ((frac += heightmask) < 0);
            else
               while (frac >= (int)heightmask)
                  frac -= heightmask;
            while (count--)
            {





               *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(frac)>>16])])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;



            }
         }
      }
   }

}

static void R_DrawTranslatedColumn16_LinearUV(draw_column_vars_t *dcvars)
{
  int count;

  uint16_t *dest;

  fixed_t frac;
  const fixed_t fracstep = dcvars->iscale;

  const fixed_t slope_texu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;






  if (dcvars->iscale > drawvars.mag_threshold) {
    R_GetDrawColumnFunc(RDC_PIPELINE_TRANSLATED,
                        RDRAW_FILTER_POINT,
                        drawvars.filterz)(dcvars);
    return;
  }
  count = dcvars->yh - dcvars->yl;







  if (count < 0)
    return;

  frac = dcvars->texturemid - ((1<<16)>>1) + (dcvars->yl-centery)*fracstep;

  if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED) {



    if (dcvars->yl != 0) {
      if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

        int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
        dcvars->yl += shift;
        count -= shift;
        frac += 0xffff-(slope_texu & 0xffff);
      }
      else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

        int shift = ((slope_texu & 0xffff)/dcvars->iscale);
        dcvars->yl += shift;
        count -= shift;
        frac += slope_texu & 0xffff;
      }
    }
    if (dcvars->yh != viewheight-1) {
      if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

        int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
        dcvars->yh -= shift;
        count -= shift;
      }
      else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

        int shift = ((slope_texu & 0xffff)/dcvars->iscale);
        dcvars->yh -= shift;
        count -= shift;
      }
    }
    if (count <= 0) return;
  }



   {

      if(temp_x == 4 ||
         (temp_x && (temptype != tl_temptype || temp_x + startx != dcvars->x)))
         R_FlushColumns();

      if(!temp_x)
      {
         startx = dcvars->x;
         tempyl[0] = commontop = dcvars->yl;
         tempyh[0] = commonbot = dcvars->yh;
         temptype = tl_temptype;





         R_FlushWholeColumns = tl_flush_whole;
         R_FlushHTColumns = tl_flush_ht;
         R_FlushQuadColumn = tl_flush_quad;

         dest = &short_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tempyl[temp_x] = dcvars->yl;
         tempyh[temp_x] = dcvars->yh;

         if(dcvars->yl > commontop)
            commontop = dcvars->yl;
         if(dcvars->yh < commonbot)
            commonbot = dcvars->yh;


         dest = &short_tempbuf[(dcvars->yl << 2) + temp_x];

      }
      temp_x += 1;
   }



   {
      const uint8_t *source = dcvars->source;
      const uint8_t *translation = dcvars->translation;

      int y = dcvars->yl;

      const uint8_t *nextsource = dcvars->nextsource;
      const unsigned int filter_fracu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;







      count++;







      if (dcvars->texheight == 128)
      {

         while(count--)
         {
            *dest = (( V_Palette16[ ((translation[(nextsource[((frac+(1<<16)) & ((127<<16)|0xffff))>>16])]))*64 + ((filter_fracu*((frac & ((127<<16)|0xffff))&0xffff))>>(32-6)) ] + V_Palette16[ ((translation[(source[((frac+(1<<16)) & ((127<<16)|0xffff))>>16])]))*64 + (((0xffff-filter_fracu)*((frac & ((127<<16)|0xffff))&0xffff))>>(32-6)) ] + V_Palette16[ ((translation[(source[(frac & ((127<<16)|0xffff))>>16])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & ((127<<16)|0xffff))&0xffff)))>>(32-6)) ] + V_Palette16[ ((translation[(nextsource[(frac & ((127<<16)|0xffff))>>16])]))*64 + ((filter_fracu*(0xffff-((frac & ((127<<16)|0xffff))&0xffff)))>>(32-6)) ]));
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (( V_Palette16[ ((translation[(nextsource[((frac+(1<<16)))>>16])]))*64 + ((filter_fracu*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ ((translation[(source[((frac+(1<<16)))>>16])]))*64 + (((0xffff-filter_fracu)*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ ((translation[(source[(frac)>>16])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac)&0xffff)))>>(32-6)) ] + V_Palette16[ ((translation[(nextsource[(frac)>>16])]))*64 + ((filter_fracu*(0xffff-((frac)&0xffff)))>>(32-6)) ]));
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else
      {
         unsigned heightmask = dcvars->texheight-1;
         if (! (dcvars->texheight & heightmask))
         {
            fixed_t fixedt_heightmask = (heightmask<<16)|0xffff;
            while ((count-=2)>=0)
            {
               *dest = (( V_Palette16[ ((translation[(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])]))*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((translation[(source[((frac+(1<<16)) & fixedt_heightmask)>>16])]))*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((translation[(source[(frac & fixedt_heightmask)>>16])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_Palette16[ ((translation[(nextsource[(frac & fixedt_heightmask)>>16])]))*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (( V_Palette16[ ((translation[(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])]))*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((translation[(source[((frac+(1<<16)) & fixedt_heightmask)>>16])]))*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((translation[(source[(frac & fixedt_heightmask)>>16])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_Palette16[ ((translation[(nextsource[(frac & fixedt_heightmask)>>16])]))*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (( V_Palette16[ ((translation[(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])]))*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((translation[(source[((frac+(1<<16)) & fixedt_heightmask)>>16])]))*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((translation[(source[(frac & fixedt_heightmask)>>16])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_Palette16[ ((translation[(nextsource[(frac & fixedt_heightmask)>>16])]))*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
            (y++);
         }
         else
         {
            fixed_t nextfrac = 0;

            heightmask++;
            heightmask <<= 16;

            if (frac < 0)
               while ((frac += heightmask) < 0);
            else
               while (frac >= (int)heightmask)
                  frac -= heightmask;


            nextfrac = frac + (1<<16);
            while (nextfrac >= (int)heightmask)
               nextfrac -= heightmask;




            while (count--)
            {





               *dest = (( V_Palette16[ ((translation[(nextsource[(nextfrac)>>16])]))*64 + ((filter_fracu*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ ((translation[(source[(nextfrac)>>16])]))*64 + (((0xffff-filter_fracu)*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ ((translation[(source[(frac)>>16])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac)&0xffff)))>>(32-6)) ] + V_Palette16[ ((translation[(nextsource[(frac)>>16])]))*64 + ((filter_fracu*(0xffff-((frac)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;

               if ((nextfrac += fracstep) >= (int)heightmask) nextfrac -= heightmask;;

            }
         }
      }
   }

}

static void R_DrawTranslatedColumn16_LinearUV_PointZ(draw_column_vars_t *dcvars)
{
  int count;

  uint16_t *dest;

  fixed_t frac;
  const fixed_t fracstep = dcvars->iscale;

  const fixed_t slope_texu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;

  if (dcvars->iscale > drawvars.mag_threshold) {
    R_GetDrawColumnFunc(RDC_PIPELINE_TRANSLATED,
                        RDRAW_FILTER_POINT,
                        drawvars.filterz)(dcvars);
    return;
  }
  count = dcvars->yh - dcvars->yl;

  if (count < 0)
    return;

  frac = dcvars->texturemid - ((1<<16)>>1) + (dcvars->yl-centery)*fracstep;

  if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED)
  {
     if (dcvars->yl != 0) {
        if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

           int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
           dcvars->yl += shift;
           count -= shift;
           frac += 0xffff-(slope_texu & 0xffff);
        }
        else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

           int shift = ((slope_texu & 0xffff)/dcvars->iscale);
           dcvars->yl += shift;
           count -= shift;
           frac += slope_texu & 0xffff;
        }
     }
     if (dcvars->yh != viewheight-1) {
        if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

           int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
           dcvars->yh -= shift;
           count -= shift;
        }
        else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

           int shift = ((slope_texu & 0xffff)/dcvars->iscale);
           dcvars->yh -= shift;
           count -= shift;
        }
     }
     if (count <= 0) return;
  }



   {

      if(temp_x == 4 ||
         (temp_x && (temptype != tl_temptype || temp_x + startx != dcvars->x)))
         R_FlushColumns();

      if(!temp_x)
      {
         startx = dcvars->x;
         tempyl[0] = commontop = dcvars->yl;
         tempyh[0] = commonbot = dcvars->yh;
         temptype = tl_temptype;





         R_FlushWholeColumns = tl_flush_whole;
         R_FlushHTColumns = tl_flush_ht;
         R_FlushQuadColumn = tl_flush_quad;

         dest = &short_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tempyl[temp_x] = dcvars->yl;
         tempyh[temp_x] = dcvars->yh;

         if(dcvars->yl > commontop)
            commontop = dcvars->yl;
         if(dcvars->yh < commonbot)
            commonbot = dcvars->yh;


         dest = &short_tempbuf[(dcvars->yl << 2) + temp_x];

      }
      temp_x += 1;
   }



   {
      const uint8_t *source = dcvars->source;
      const lighttable_t *colormap = dcvars->colormap;
      const uint8_t *translation = dcvars->translation;

      int y = dcvars->yl;

      const uint8_t *nextsource = dcvars->nextsource;
      const unsigned int filter_fracu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;







      count++;







      if (dcvars->texheight == 128)
      {

         while(count--)
         {
            *dest = (( V_Palette16[ (colormap[(translation[(nextsource[((frac+(1<<16)) & ((127<<16)|0xffff))>>16])])])*64 + ((filter_fracu*((frac & ((127<<16)|0xffff))&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(translation[(source[((frac+(1<<16)) & ((127<<16)|0xffff))>>16])])])*64 + (((0xffff-filter_fracu)*((frac & ((127<<16)|0xffff))&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(translation[(source[(frac & ((127<<16)|0xffff))>>16])])])*64 + (((0xffff-filter_fracu)*(0xffff-((frac & ((127<<16)|0xffff))&0xffff)))>>(32-6)) ] + V_Palette16[ (colormap[(translation[(nextsource[(frac & ((127<<16)|0xffff))>>16])])])*64 + ((filter_fracu*(0xffff-((frac & ((127<<16)|0xffff))&0xffff)))>>(32-6)) ]));
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (( V_Palette16[ (colormap[(translation[(nextsource[((frac+(1<<16)))>>16])])])*64 + ((filter_fracu*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(translation[(source[((frac+(1<<16)))>>16])])])*64 + (((0xffff-filter_fracu)*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(translation[(source[(frac)>>16])])])*64 + (((0xffff-filter_fracu)*(0xffff-((frac)&0xffff)))>>(32-6)) ] + V_Palette16[ (colormap[(translation[(nextsource[(frac)>>16])])])*64 + ((filter_fracu*(0xffff-((frac)&0xffff)))>>(32-6)) ]));
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else
      {
         unsigned heightmask = dcvars->texheight-1;
         if (! (dcvars->texheight & heightmask))
         {
            fixed_t fixedt_heightmask = (heightmask<<16)|0xffff;
            while ((count-=2)>=0)
            {
               *dest = (( V_Palette16[ (colormap[(translation[(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])])])*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(translation[(source[((frac+(1<<16)) & fixedt_heightmask)>>16])])])*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(translation[(source[(frac & fixedt_heightmask)>>16])])])*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_Palette16[ (colormap[(translation[(nextsource[(frac & fixedt_heightmask)>>16])])])*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (( V_Palette16[ (colormap[(translation[(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])])])*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(translation[(source[((frac+(1<<16)) & fixedt_heightmask)>>16])])])*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(translation[(source[(frac & fixedt_heightmask)>>16])])])*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_Palette16[ (colormap[(translation[(nextsource[(frac & fixedt_heightmask)>>16])])])*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (( V_Palette16[ (colormap[(translation[(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])])])*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(translation[(source[((frac+(1<<16)) & fixedt_heightmask)>>16])])])*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(translation[(source[(frac & fixedt_heightmask)>>16])])])*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_Palette16[ (colormap[(translation[(nextsource[(frac & fixedt_heightmask)>>16])])])*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
            (y++);
         }
         else
         {
            fixed_t nextfrac = 0;

            heightmask++;
            heightmask <<= 16;

            if (frac < 0)
               while ((frac += heightmask) < 0);
            else
               while (frac >= (int)heightmask)
                  frac -= heightmask;


            nextfrac = frac + (1<<16);
            while (nextfrac >= (int)heightmask)
               nextfrac -= heightmask;




            while (count--)
            {





               *dest = (( V_Palette16[ (colormap[(translation[(nextsource[(nextfrac)>>16])])])*64 + ((filter_fracu*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(translation[(source[(nextfrac)>>16])])])*64 + (((0xffff-filter_fracu)*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(translation[(source[(frac)>>16])])])*64 + (((0xffff-filter_fracu)*(0xffff-((frac)&0xffff)))>>(32-6)) ] + V_Palette16[ (colormap[(translation[(nextsource[(frac)>>16])])])*64 + ((filter_fracu*(0xffff-((frac)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;

               if ((nextfrac += fracstep) >= (int)heightmask) nextfrac -= heightmask;;

            }
         }
      }
   }

}

static void R_DrawTranslatedColumn16_LinearUV_LinearZ(draw_column_vars_t *dcvars)
{
  int count;

  uint16_t *dest;

  fixed_t frac;
  const fixed_t fracstep = dcvars->iscale;

  const fixed_t slope_texu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;

  if (dcvars->iscale > drawvars.mag_threshold)
  {
     R_GetDrawColumnFunc(RDC_PIPELINE_TRANSLATED,
           RDRAW_FILTER_POINT,
           drawvars.filterz)(dcvars);
     return;
  }

  count = dcvars->yh - dcvars->yl;

  if (count < 0)
    return;

  frac = dcvars->texturemid - ((1<<16)>>1) + (dcvars->yl-centery)*fracstep;

  if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED) {



    if (dcvars->yl != 0) {
      if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

        int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
        dcvars->yl += shift;
        count -= shift;
        frac += 0xffff-(slope_texu & 0xffff);
      }
      else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

        int shift = ((slope_texu & 0xffff)/dcvars->iscale);
        dcvars->yl += shift;
        count -= shift;
        frac += slope_texu & 0xffff;
      }
    }
    if (dcvars->yh != viewheight-1) {
      if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

        int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
        dcvars->yh -= shift;
        count -= shift;
      }
      else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

        int shift = ((slope_texu & 0xffff)/dcvars->iscale);
        dcvars->yh -= shift;
        count -= shift;
      }
    }
    if (count <= 0) return;
  }



   {

      if(temp_x == 4 ||
         (temp_x && (temptype != tl_temptype || temp_x + startx != dcvars->x)))
         R_FlushColumns();

      if(!temp_x)
      {
         startx = dcvars->x;
         tempyl[0] = commontop = dcvars->yl;
         tempyh[0] = commonbot = dcvars->yh;
         temptype = tl_temptype;





         R_FlushWholeColumns = tl_flush_whole;
         R_FlushHTColumns = tl_flush_ht;
         R_FlushQuadColumn = tl_flush_quad;

         dest = &short_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tempyl[temp_x] = dcvars->yl;
         tempyh[temp_x] = dcvars->yh;

         if(dcvars->yl > commontop)
            commontop = dcvars->yl;
         if(dcvars->yh < commonbot)
            commonbot = dcvars->yh;


         dest = &short_tempbuf[(dcvars->yl << 2) + temp_x];

      }
      temp_x += 1;
   }



   {
      const uint8_t *source = dcvars->source;
      const uint8_t *translation = dcvars->translation;

      int y = dcvars->yl;
      const int x = dcvars->x;


      const int fracz = (dcvars->z >> 6) & 255;
      const uint8_t *dither_colormaps[2] = { dcvars->colormap, dcvars->nextcolormap };


      const uint8_t *nextsource = dcvars->nextsource;
      const unsigned int filter_fracu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;







      count++;







      if (dcvars->texheight == 128)
      {

         while(count--)
         {
            *dest = (( V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(nextsource[((frac+(1<<16)) & ((127<<16)|0xffff))>>16])])]))*64 + ((filter_fracu*((frac & ((127<<16)|0xffff))&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[((frac+(1<<16)) & ((127<<16)|0xffff))>>16])])]))*64 + (((0xffff-filter_fracu)*((frac & ((127<<16)|0xffff))&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(frac & ((127<<16)|0xffff))>>16])])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & ((127<<16)|0xffff))&0xffff)))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(nextsource[(frac & ((127<<16)|0xffff))>>16])])]))*64 + ((filter_fracu*(0xffff-((frac & ((127<<16)|0xffff))&0xffff)))>>(32-6)) ]));
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (( V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(nextsource[((frac+(1<<16)))>>16])])]))*64 + ((filter_fracu*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[((frac+(1<<16)))>>16])])]))*64 + (((0xffff-filter_fracu)*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(frac)>>16])])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac)&0xffff)))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(nextsource[(frac)>>16])])]))*64 + ((filter_fracu*(0xffff-((frac)&0xffff)))>>(32-6)) ]));
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else
      {
         unsigned heightmask = dcvars->texheight-1;
         if (! (dcvars->texheight & heightmask))
         {
            fixed_t fixedt_heightmask = (heightmask<<16)|0xffff;
            while ((count-=2)>=0)
            {
               *dest = (( V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])])]))*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[((frac+(1<<16)) & fixedt_heightmask)>>16])])]))*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(frac & fixedt_heightmask)>>16])])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(nextsource[(frac & fixedt_heightmask)>>16])])]))*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (( V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])])]))*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[((frac+(1<<16)) & fixedt_heightmask)>>16])])]))*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(frac & fixedt_heightmask)>>16])])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(nextsource[(frac & fixedt_heightmask)>>16])])]))*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (( V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])])]))*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[((frac+(1<<16)) & fixedt_heightmask)>>16])])]))*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(frac & fixedt_heightmask)>>16])])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(nextsource[(frac & fixedt_heightmask)>>16])])]))*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
            (y++);
         }
         else
         {
            fixed_t nextfrac = 0;

            heightmask++;
            heightmask <<= 16;

            if (frac < 0)
               while ((frac += heightmask) < 0);
            else
               while (frac >= (int)heightmask)
                  frac -= heightmask;


            nextfrac = frac + (1<<16);
            while (nextfrac >= (int)heightmask)
               nextfrac -= heightmask;




            while (count--)
            {





               *dest = (( V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(nextsource[(nextfrac)>>16])])]))*64 + ((filter_fracu*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(nextfrac)>>16])])]))*64 + (((0xffff-filter_fracu)*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(frac)>>16])])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac)&0xffff)))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(nextsource[(frac)>>16])])]))*64 + ((filter_fracu*(0xffff-((frac)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;

               if ((nextfrac += fracstep) >= (int)heightmask) nextfrac -= heightmask;;

            }
         }
      }
   }

}

static void R_DrawTranslatedColumn16_RoundedUV(draw_column_vars_t *dcvars)
{
  int count;
  uint16_t *dest;
  fixed_t frac;
  const fixed_t fracstep = dcvars->iscale;
  const fixed_t slope_texu = dcvars->texu;

  if (dcvars->iscale > drawvars.mag_threshold)
  {
    R_GetDrawColumnFunc(RDC_PIPELINE_TRANSLATED,
                        RDRAW_FILTER_POINT,
                        drawvars.filterz)(dcvars);
    return;
  }
  count = dcvars->yh - dcvars->yl;

  if (count < 0)
    return;

  frac = dcvars->texturemid + (dcvars->yl-centery)*fracstep;

  if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED)
  {
     if (dcvars->yl != 0) {
        if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

           int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
           dcvars->yl += shift;
           count -= shift;
           frac += 0xffff-(slope_texu & 0xffff);
        }
        else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

           int shift = ((slope_texu & 0xffff)/dcvars->iscale);
           dcvars->yl += shift;
           count -= shift;
           frac += slope_texu & 0xffff;
        }
     }
     if (dcvars->yh != viewheight-1) {
        if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

           int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
           dcvars->yh -= shift;
           count -= shift;
        }
        else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

           int shift = ((slope_texu & 0xffff)/dcvars->iscale);
           dcvars->yh -= shift;
           count -= shift;
        }
     }
     if (count <= 0) return;
  }

   {

      if(temp_x == 4 ||
         (temp_x && (temptype != tl_temptype || temp_x + startx != dcvars->x)))
         R_FlushColumns();

      if(!temp_x)
      {
         startx = dcvars->x;
         tempyl[0] = commontop = dcvars->yl;
         tempyh[0] = commonbot = dcvars->yh;
         temptype = tl_temptype;





         R_FlushWholeColumns = tl_flush_whole;
         R_FlushHTColumns = tl_flush_ht;
         R_FlushQuadColumn = tl_flush_quad;

         dest = &short_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tempyl[temp_x] = dcvars->yl;
         tempyh[temp_x] = dcvars->yh;

         if(dcvars->yl > commontop)
            commontop = dcvars->yl;
         if(dcvars->yh < commonbot)
            commonbot = dcvars->yh;


         dest = &short_tempbuf[(dcvars->yl << 2) + temp_x];

      }
      temp_x += 1;
   }



   {
      const uint8_t *source = dcvars->source;
      const uint8_t *translation = dcvars->translation;

      int y = dcvars->yl;
      const uint8_t *prevsource = dcvars->prevsource;
      const uint8_t *nextsource = dcvars->nextsource;
      const unsigned int filter_fracu = (dcvars->source == dcvars->nextsource) ? 0 : (dcvars->texu>>8) & 0xff;


      count++;







      if (dcvars->texheight == 128)
      {

         while(count--)
         {
            *dest = (V_Palette16[ ((translation[(filter_getScale2xQuadColors( source[ ((frac & ((127<<16)|0xffff))>>16) ], source[ (((0)>(((frac & ((127<<16)|0xffff))>>16)-1)?(0):(((frac & ((127<<16)|0xffff))>>16)-1))) ], nextsource[ ((frac & ((127<<16)|0xffff))>>16) ], source[ (((frac+(1<<16)) & ((127<<16)|0xffff))>>16) ], prevsource[ ((frac & ((127<<16)|0xffff))>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & ((127<<16)|0xffff))>>8) & 0xff)>>(8-6)) ] ])]))*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (V_Palette16[ ((translation[(filter_getScale2xQuadColors( source[ ((frac)>>16) ], source[ (((0)>(((frac)>>16)-1)?(0):(((frac)>>16)-1))) ], nextsource[ ((frac)>>16) ], source[ (((frac+(1<<16)))>>16) ], prevsource[ ((frac)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac)>>8) & 0xff)>>(8-6)) ] ])]))*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else
      {
         unsigned heightmask = dcvars->texheight-1;
         if (! (dcvars->texheight & heightmask))
         {
            fixed_t fixedt_heightmask = (heightmask<<16)|0xffff;
            while ((count-=2)>=0)
            {
               *dest = (V_Palette16[ ((translation[(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (V_Palette16[ ((translation[(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (V_Palette16[ ((translation[(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])]))*64 + ((64 -1)) ]);
            (y++);
         }
         else
         {
            fixed_t nextfrac = 0;

            heightmask++;
            heightmask <<= 16;

            if (frac < 0)
               while ((frac += heightmask) < 0);
            else
               while (frac >= (int)heightmask)
                  frac -= heightmask;


            nextfrac = frac + (1<<16);
            while (nextfrac >= (int)heightmask)
               nextfrac -= heightmask;




            while (count--)
            {





               *dest = (V_Palette16[ ((translation[(filter_getScale2xQuadColors( source[ ((frac)>>16) ], source[ (((0)>(((frac)>>16)-1)?(0):(((frac)>>16)-1))) ], nextsource[ ((frac)>>16) ], source[ ((nextfrac)>>16) ], prevsource[ ((frac)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac)>>8) & 0xff)>>(8-6)) ] ])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;

               if ((nextfrac += fracstep) >= (int)heightmask) nextfrac -= heightmask;;

            }
         }
      }
   }

}

static void R_DrawTranslatedColumn16_RoundedUV_PointZ(draw_column_vars_t *dcvars)
{
  int count;

  uint16_t *dest;

  fixed_t frac;
  const fixed_t fracstep = dcvars->iscale;
  const fixed_t slope_texu = dcvars->texu;

  if (dcvars->iscale > drawvars.mag_threshold) {
    R_GetDrawColumnFunc(RDC_PIPELINE_TRANSLATED,
                        RDRAW_FILTER_POINT,
                        drawvars.filterz)(dcvars);
    return;
  }
  count = dcvars->yh - dcvars->yl;







  if (count < 0)
    return;

  frac = dcvars->texturemid + (dcvars->yl-centery)*fracstep;

  if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED)
  {
     if (dcvars->yl != 0) {
        if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

           int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
           dcvars->yl += shift;
           count -= shift;
           frac += 0xffff-(slope_texu & 0xffff);
        }
        else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

           int shift = ((slope_texu & 0xffff)/dcvars->iscale);
           dcvars->yl += shift;
           count -= shift;
           frac += slope_texu & 0xffff;
        }
     }
     if (dcvars->yh != viewheight-1) {
        if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

           int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
           dcvars->yh -= shift;
           count -= shift;
        }
        else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

           int shift = ((slope_texu & 0xffff)/dcvars->iscale);
           dcvars->yh -= shift;
           count -= shift;
        }
     }
     if (count <= 0) return;
  }



   {

      if(temp_x == 4 ||
         (temp_x && (temptype != tl_temptype || temp_x + startx != dcvars->x)))
         R_FlushColumns();

      if(!temp_x)
      {
         startx = dcvars->x;
         tempyl[0] = commontop = dcvars->yl;
         tempyh[0] = commonbot = dcvars->yh;
         temptype = tl_temptype;





         R_FlushWholeColumns = tl_flush_whole;
         R_FlushHTColumns = tl_flush_ht;
         R_FlushQuadColumn = tl_flush_quad;

         dest = &short_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tempyl[temp_x] = dcvars->yl;
         tempyh[temp_x] = dcvars->yh;

         if(dcvars->yl > commontop)
            commontop = dcvars->yl;
         if(dcvars->yh < commonbot)
            commonbot = dcvars->yh;


         dest = &short_tempbuf[(dcvars->yl << 2) + temp_x];

      }
      temp_x += 1;
   }



   {
      const uint8_t *source = dcvars->source;
      const lighttable_t *colormap = dcvars->colormap;
      const uint8_t *translation = dcvars->translation;

      int y = dcvars->yl;
      const uint8_t *prevsource = dcvars->prevsource;
      const uint8_t *nextsource = dcvars->nextsource;
      const unsigned int filter_fracu = (dcvars->source == dcvars->nextsource) ? 0 : (dcvars->texu>>8) & 0xff;


      count++;







      if (dcvars->texheight == 128)
      {

         while(count--)
         {
            *dest = (V_Palette16[ (colormap[(translation[(filter_getScale2xQuadColors( source[ ((frac & ((127<<16)|0xffff))>>16) ], source[ (((0)>(((frac & ((127<<16)|0xffff))>>16)-1)?(0):(((frac & ((127<<16)|0xffff))>>16)-1))) ], nextsource[ ((frac & ((127<<16)|0xffff))>>16) ], source[ (((frac+(1<<16)) & ((127<<16)|0xffff))>>16) ], prevsource[ ((frac & ((127<<16)|0xffff))>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & ((127<<16)|0xffff))>>8) & 0xff)>>(8-6)) ] ])])])*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (V_Palette16[ (colormap[(translation[(filter_getScale2xQuadColors( source[ ((frac)>>16) ], source[ (((0)>(((frac)>>16)-1)?(0):(((frac)>>16)-1))) ], nextsource[ ((frac)>>16) ], source[ (((frac+(1<<16)))>>16) ], prevsource[ ((frac)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac)>>8) & 0xff)>>(8-6)) ] ])])])*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else
      {
         unsigned heightmask = dcvars->texheight-1;
         if (! (dcvars->texheight & heightmask))
         {
            fixed_t fixedt_heightmask = (heightmask<<16)|0xffff;
            while ((count-=2)>=0)
            {
               *dest = (V_Palette16[ (colormap[(translation[(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])])])*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (V_Palette16[ (colormap[(translation[(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])])])*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (V_Palette16[ (colormap[(translation[(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])])])*64 + ((64 -1)) ]);
            (y++);
         }
         else
         {
            fixed_t nextfrac = 0;

            heightmask++;
            heightmask <<= 16;

            if (frac < 0)
               while ((frac += heightmask) < 0);
            else
               while (frac >= (int)heightmask)
                  frac -= heightmask;


            nextfrac = frac + (1<<16);
            while (nextfrac >= (int)heightmask)
               nextfrac -= heightmask;




            while (count--)
            {





               *dest = (V_Palette16[ (colormap[(translation[(filter_getScale2xQuadColors( source[ ((frac)>>16) ], source[ (((0)>(((frac)>>16)-1)?(0):(((frac)>>16)-1))) ], nextsource[ ((frac)>>16) ], source[ ((nextfrac)>>16) ], prevsource[ ((frac)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac)>>8) & 0xff)>>(8-6)) ] ])])])*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;

               if ((nextfrac += fracstep) >= (int)heightmask) nextfrac -= heightmask;;

            }
         }
      }
   }

}

static void R_DrawTranslatedColumn16_RoundedUV_LinearZ(draw_column_vars_t *dcvars)
{
   int count;
   uint16_t *dest;
   fixed_t frac;
   const fixed_t fracstep = dcvars->iscale;
   const fixed_t slope_texu = dcvars->texu;

   if (dcvars->iscale > drawvars.mag_threshold)
   {
      R_GetDrawColumnFunc(RDC_PIPELINE_TRANSLATED,
            RDRAW_FILTER_POINT,
            drawvars.filterz)(dcvars);
      return;
   }

   count = dcvars->yh - dcvars->yl;

   if (count < 0)
      return;

   frac = dcvars->texturemid + (dcvars->yl-centery)*fracstep;


   if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED) {



      if (dcvars->yl != 0) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += 0xffff-(slope_texu & 0xffff);
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += slope_texu & 0xffff;
         }
      }
      if (dcvars->yh != viewheight-1) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
      }
      if (count <= 0) return;
   }



   {

      if(temp_x == 4 ||
            (temp_x && (temptype != tl_temptype || temp_x + startx != dcvars->x)))
         R_FlushColumns();

      if(!temp_x)
      {
         startx = dcvars->x;
         tempyl[0] = commontop = dcvars->yl;
         tempyh[0] = commonbot = dcvars->yh;
         temptype = tl_temptype;





         R_FlushWholeColumns = tl_flush_whole;
         R_FlushHTColumns = tl_flush_ht;
         R_FlushQuadColumn = tl_flush_quad;

         dest = &short_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tempyl[temp_x] = dcvars->yl;
         tempyh[temp_x] = dcvars->yh;

         if(dcvars->yl > commontop)
            commontop = dcvars->yl;
         if(dcvars->yh < commonbot)
            commonbot = dcvars->yh;


         dest = &short_tempbuf[(dcvars->yl << 2) + temp_x];

      }
      temp_x += 1;
   }

   {
      const uint8_t *source = dcvars->source;
      const uint8_t *translation = dcvars->translation;

      int y = dcvars->yl;
      const int x = dcvars->x;


      const int fracz = (dcvars->z >> 6) & 255;
      const uint8_t *dither_colormaps[2] = { dcvars->colormap, dcvars->nextcolormap };






      const uint8_t *prevsource = dcvars->prevsource;
      const uint8_t *nextsource = dcvars->nextsource;
      const unsigned int filter_fracu = (dcvars->source == dcvars->nextsource) ? 0 : (dcvars->texu>>8) & 0xff;


      count++;







      if (dcvars->texheight == 128)
      {

         while(count--)
         {
            *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(filter_getScale2xQuadColors( source[ ((frac & ((127<<16)|0xffff))>>16) ], source[ (((0)>(((frac & ((127<<16)|0xffff))>>16)-1)?(0):(((frac & ((127<<16)|0xffff))>>16)-1))) ], nextsource[ ((frac & ((127<<16)|0xffff))>>16) ], source[ (((frac+(1<<16)) & ((127<<16)|0xffff))>>16) ], prevsource[ ((frac & ((127<<16)|0xffff))>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & ((127<<16)|0xffff))>>8) & 0xff)>>(8-6)) ] ])])]))*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(filter_getScale2xQuadColors( source[ ((frac)>>16) ], source[ (((0)>(((frac)>>16)-1)?(0):(((frac)>>16)-1))) ], nextsource[ ((frac)>>16) ], source[ (((frac+(1<<16)))>>16) ], prevsource[ ((frac)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac)>>8) & 0xff)>>(8-6)) ] ])])]))*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else
      {
         unsigned heightmask = dcvars->texheight-1;
         if (! (dcvars->texheight & heightmask))
         {
            fixed_t fixedt_heightmask = (heightmask<<16)|0xffff;
            while ((count-=2)>=0)
            {
               *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])])]))*64 + ((64 -1)) ]);
            (y++);
         }
         else
         {
            fixed_t nextfrac = 0;

            heightmask++;
            heightmask <<= 16;

            if (frac < 0)
               while ((frac += heightmask) < 0);
            else
               while (frac >= (int)heightmask)
                  frac -= heightmask;


            nextfrac = frac + (1<<16);
            while (nextfrac >= (int)heightmask)
               nextfrac -= heightmask;




            while (count--)
            {





               *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(filter_getScale2xQuadColors( source[ ((frac)>>16) ], source[ (((0)>(((frac)>>16)-1)?(0):(((frac)>>16)-1))) ], nextsource[ ((frac)>>16) ], source[ ((nextfrac)>>16) ], prevsource[ ((frac)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac)>>8) & 0xff)>>(8-6)) ] ])])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;

               if ((nextfrac += fracstep) >= (int)heightmask) nextfrac -= heightmask;;

            }
         }
      }
   }
}

//
// Framebuffer postprocessing.
// Creates a fuzzy image by copying pixels
//  from adjacent ones to left and right.
// Used with an all black colormap, this
//  could create the SHADOW effect,
//  i.e. spectres and invisible players.
//

static void R_DrawFuzzColumn16_PointUV(draw_column_vars_t *dcvars)
{
   int count;
   fixed_t frac;
   const fixed_t fracstep = dcvars->iscale;
   const fixed_t slope_texu = dcvars->texu;

   if (!dcvars->yl)
      dcvars->yl = 1;

   if (dcvars->yh == viewheight-1)
      dcvars->yh = viewheight - 2;

   count = dcvars->yh - dcvars->yl;

   if (count < 0)
      return;

   frac = dcvars->texturemid + (dcvars->yl-centery)*fracstep;

   if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED)
   {
      if (dcvars->yl != 0) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += 0xffff-(slope_texu & 0xffff);
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += slope_texu & 0xffff;
         }
      }
      if (dcvars->yh != viewheight-1) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
      }
      if (count <= 0) return;
   }

   if(temp_x == 4 ||
         (temp_x && (temptype != (COL_FUZZ) || temp_x + startx != dcvars->x)))
      R_FlushColumns();

   if(!temp_x)
   {
      startx = dcvars->x;
      tempyl[0] = commontop = dcvars->yl;
      tempyh[0] = commonbot = dcvars->yh;
      temptype = (COL_FUZZ);

      tempfuzzmap = fullcolormap;

      R_FlushWholeColumns = R_FlushWholeFuzz16;
      R_FlushHTColumns = R_FlushHTFuzz16;
      R_FlushQuadColumn = R_FlushQuadFuzz16;
   }
   else
   {
      tempyl[temp_x] = dcvars->yl;
      tempyh[temp_x] = dcvars->yh;

      if(dcvars->yl > commontop)
         commontop = dcvars->yl;
      if(dcvars->yh < commonbot)
         commonbot = dcvars->yh;
   }
   temp_x += 1;
}

static void R_DrawFuzzColumn16_PointUV_PointZ(draw_column_vars_t *dcvars)
{
  int count;
  fixed_t frac;
  const fixed_t fracstep = dcvars->iscale;
  const fixed_t slope_texu = dcvars->texu;

  if (!dcvars->yl)
    dcvars->yl = 1;

  if (dcvars->yh == viewheight-1)
    dcvars->yh = viewheight - 2;

  count = dcvars->yh - dcvars->yl;

  if (count < 0)
    return;

  frac = dcvars->texturemid + (dcvars->yl-centery)*fracstep;

  if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED)
  {

     if (dcvars->yl != 0) {
        if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

           int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
           dcvars->yl += shift;
           count -= shift;
           frac += 0xffff-(slope_texu & 0xffff);
        }
        else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

           int shift = ((slope_texu & 0xffff)/dcvars->iscale);
           dcvars->yl += shift;
           count -= shift;
           frac += slope_texu & 0xffff;
        }
     }
     if (dcvars->yh != viewheight-1) {
        if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

           int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
           dcvars->yh -= shift;
           count -= shift;
        }
        else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

           int shift = ((slope_texu & 0xffff)/dcvars->iscale);
           dcvars->yh -= shift;
           count -= shift;
        }
     }
     if (count <= 0) return;
  }

  if(temp_x == 4 ||
        (temp_x && (temptype != (COL_FUZZ) || temp_x + startx != dcvars->x)))
     R_FlushColumns();

  if(!temp_x)
  {
     startx = dcvars->x;
     tempyl[0] = commontop = dcvars->yl;
     tempyh[0] = commonbot = dcvars->yh;
     temptype = (COL_FUZZ);

     tempfuzzmap = fullcolormap;

     R_FlushWholeColumns = R_FlushWholeFuzz16;
     R_FlushHTColumns = R_FlushHTFuzz16;
     R_FlushQuadColumn = R_FlushQuadFuzz16;
  }
  else
  {
     tempyl[temp_x] = dcvars->yl;
     tempyh[temp_x] = dcvars->yh;

     if(dcvars->yl > commontop)
        commontop = dcvars->yl;
     if(dcvars->yh < commonbot)
        commonbot = dcvars->yh;
  }
  temp_x += 1;
}

static void R_DrawFuzzColumn16_PointUV_LinearZ(draw_column_vars_t *dcvars)
{
  int count;
  fixed_t frac;
  const fixed_t fracstep = dcvars->iscale;
  const fixed_t slope_texu = dcvars->texu;

  if (!dcvars->yl)
    dcvars->yl = 1;

  if (dcvars->yh == viewheight-1)
    dcvars->yh = viewheight - 2;

  count = dcvars->yh - dcvars->yl;

  if (count < 0)
    return;

  frac = dcvars->texturemid + (dcvars->yl-centery)*fracstep;

  if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED)
  {
     if (dcvars->yl != 0) {
        if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

           int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
           dcvars->yl += shift;
           count -= shift;
           frac += 0xffff-(slope_texu & 0xffff);
        }
        else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

           int shift = ((slope_texu & 0xffff)/dcvars->iscale);
           dcvars->yl += shift;
           count -= shift;
           frac += slope_texu & 0xffff;
        }
     }
     if (dcvars->yh != viewheight-1) {
        if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

           int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
           dcvars->yh -= shift;
           count -= shift;
        }
        else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

           int shift = ((slope_texu & 0xffff)/dcvars->iscale);
           dcvars->yh -= shift;
           count -= shift;
        }
     }
     if (count <= 0) return;
  }

  if(temp_x == 4 ||
        (temp_x && (temptype != (COL_FUZZ) || temp_x + startx != dcvars->x)))
     R_FlushColumns();

  if(!temp_x)
  {
     startx = dcvars->x;
     tempyl[0] = commontop = dcvars->yl;
     tempyh[0] = commonbot = dcvars->yh;
     temptype = (COL_FUZZ);



     tempfuzzmap = fullcolormap;

     R_FlushWholeColumns = R_FlushWholeFuzz16;
     R_FlushHTColumns = R_FlushHTFuzz16;
     R_FlushQuadColumn = R_FlushQuadFuzz16;



  }
  else
  {
     tempyl[temp_x] = dcvars->yl;
     tempyh[temp_x] = dcvars->yh;

     if(dcvars->yl > commontop)
        commontop = dcvars->yl;
     if(dcvars->yh < commonbot)
        commonbot = dcvars->yh;




  }
  temp_x += 1;
}

static void R_DrawFuzzColumn16_LinearUV(draw_column_vars_t *dcvars)
{
  int count;
  fixed_t frac;
  const fixed_t fracstep = dcvars->iscale;
  const fixed_t slope_texu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;

  if (dcvars->iscale > drawvars.mag_threshold)
  {
    R_GetDrawColumnFunc(RDC_PIPELINE_FUZZ,
                        RDRAW_FILTER_POINT,
                        drawvars.filterz)(dcvars);
    return;
  }

  if (!dcvars->yl)
    dcvars->yl = 1;


  if (dcvars->yh == viewheight-1)
    dcvars->yh = viewheight - 2;

  count = dcvars->yh - dcvars->yl;

  if (count < 0)
    return;

  frac = dcvars->texturemid - ((1<<16)>>1) + (dcvars->yl-centery)*fracstep;

  if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED)
  {
     if (dcvars->yl != 0) {
        if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

           int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
           dcvars->yl += shift;
           count -= shift;
           frac += 0xffff-(slope_texu & 0xffff);
        }
        else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

           int shift = ((slope_texu & 0xffff)/dcvars->iscale);
           dcvars->yl += shift;
           count -= shift;
           frac += slope_texu & 0xffff;
        }
     }
     if (dcvars->yh != viewheight-1) {
        if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

           int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
           dcvars->yh -= shift;
           count -= shift;
        }
        else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

           int shift = ((slope_texu & 0xffff)/dcvars->iscale);
           dcvars->yh -= shift;
           count -= shift;
        }
     }
     if (count <= 0) return;
  }

  if(temp_x == 4 ||
        (temp_x && (temptype != (COL_FUZZ) || temp_x + startx != dcvars->x)))
     R_FlushColumns();

  if(!temp_x)
  {
     startx = dcvars->x;
     tempyl[0] = commontop = dcvars->yl;
     tempyh[0] = commonbot = dcvars->yh;
     temptype = (COL_FUZZ);



     tempfuzzmap = fullcolormap;

     R_FlushWholeColumns = R_FlushWholeFuzz16;
     R_FlushHTColumns = R_FlushHTFuzz16;
     R_FlushQuadColumn = R_FlushQuadFuzz16;



  }
  else
  {
     tempyl[temp_x] = dcvars->yl;
     tempyh[temp_x] = dcvars->yh;

     if(dcvars->yl > commontop)
        commontop = dcvars->yl;
     if(dcvars->yh < commonbot)
        commonbot = dcvars->yh;




  }
  temp_x += 1;
}

static void R_DrawFuzzColumn16_LinearUV_PointZ(draw_column_vars_t *dcvars)
{
   int count;
   fixed_t frac;
   const fixed_t fracstep = dcvars->iscale;
   const fixed_t slope_texu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;

   if (dcvars->iscale > drawvars.mag_threshold)
   {
      R_GetDrawColumnFunc(RDC_PIPELINE_FUZZ,
            RDRAW_FILTER_POINT,
            drawvars.filterz)(dcvars);
      return;
   }




   if (!dcvars->yl)
      dcvars->yl = 1;


   if (dcvars->yh == viewheight-1)
      dcvars->yh = viewheight - 2;

   count = dcvars->yh - dcvars->yl;
   if (count < 0)
      return;

   frac = dcvars->texturemid - ((1<<16)>>1) + (dcvars->yl-centery)*fracstep;

   if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED)
   {
      if (dcvars->yl != 0) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += 0xffff-(slope_texu & 0xffff);
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += slope_texu & 0xffff;
         }
      }
      if (dcvars->yh != viewheight-1) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
      }
      if (count <= 0) return;
   }

   if(temp_x == 4 ||
         (temp_x && (temptype != (COL_FUZZ) || temp_x + startx != dcvars->x)))
      R_FlushColumns();

   if(!temp_x)
   {
      startx = dcvars->x;
      tempyl[0] = commontop = dcvars->yl;
      tempyh[0] = commonbot = dcvars->yh;
      temptype = (COL_FUZZ);



      tempfuzzmap = fullcolormap;

      R_FlushWholeColumns = R_FlushWholeFuzz16;
      R_FlushHTColumns = R_FlushHTFuzz16;
      R_FlushQuadColumn = R_FlushQuadFuzz16;



   }
   else
   {
      tempyl[temp_x] = dcvars->yl;
      tempyh[temp_x] = dcvars->yh;

      if(dcvars->yl > commontop)
         commontop = dcvars->yl;
      if(dcvars->yh < commonbot)
         commonbot = dcvars->yh;




   }
   temp_x += 1;
}

static void R_DrawFuzzColumn16_LinearUV_LinearZ(draw_column_vars_t *dcvars)
{
  int count;



  fixed_t frac;
  const fixed_t fracstep = dcvars->iscale;

  const fixed_t slope_texu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;






  if (dcvars->iscale > drawvars.mag_threshold) {
    R_GetDrawColumnFunc(RDC_PIPELINE_FUZZ,
                        RDRAW_FILTER_POINT,
                        drawvars.filterz)(dcvars);
    return;
  }




  if (!dcvars->yl)
    dcvars->yl = 1;


  if (dcvars->yh == viewheight-1)
    dcvars->yh = viewheight - 2;

  count = dcvars->yh - dcvars->yl;

  if (count < 0)
    return;

  frac = dcvars->texturemid - ((1<<16)>>1) + (dcvars->yl-centery)*fracstep;

  if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED) {



    if (dcvars->yl != 0) {
      if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

        int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
        dcvars->yl += shift;
        count -= shift;
        frac += 0xffff-(slope_texu & 0xffff);
      }
      else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

        int shift = ((slope_texu & 0xffff)/dcvars->iscale);
        dcvars->yl += shift;
        count -= shift;
        frac += slope_texu & 0xffff;
      }
    }
    if (dcvars->yh != viewheight-1) {
      if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

        int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
        dcvars->yh -= shift;
        count -= shift;
      }
      else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

        int shift = ((slope_texu & 0xffff)/dcvars->iscale);
        dcvars->yh -= shift;
        count -= shift;
      }
    }
    if (count <= 0) return;
  }



   {

      if(temp_x == 4 ||
         (temp_x && (temptype != (COL_FUZZ) || temp_x + startx != dcvars->x)))
         R_FlushColumns();

      if(!temp_x)
      {
         startx = dcvars->x;
         tempyl[0] = commontop = dcvars->yl;
         tempyh[0] = commonbot = dcvars->yh;
         temptype = (COL_FUZZ);



         tempfuzzmap = fullcolormap;

         R_FlushWholeColumns = R_FlushWholeFuzz16;
         R_FlushHTColumns = R_FlushHTFuzz16;
         R_FlushQuadColumn = R_FlushQuadFuzz16;



      }
      else
      {
         tempyl[temp_x] = dcvars->yl;
         tempyh[temp_x] = dcvars->yh;

         if(dcvars->yl > commontop)
            commontop = dcvars->yl;
         if(dcvars->yh < commonbot)
            commonbot = dcvars->yh;




      }
      temp_x += 1;
   }
}

static void R_DrawFuzzColumn16_RoundedUV(draw_column_vars_t *dcvars)
{
   int count;
   fixed_t frac;
   const fixed_t fracstep = dcvars->iscale;
   const fixed_t slope_texu = dcvars->texu;

   if (dcvars->iscale > drawvars.mag_threshold)
   {
      R_GetDrawColumnFunc(RDC_PIPELINE_FUZZ,
            RDRAW_FILTER_POINT,
            drawvars.filterz)(dcvars);
      return;
   }

   if (!dcvars->yl)
      dcvars->yl = 1;

   if (dcvars->yh == viewheight-1)
      dcvars->yh = viewheight - 2;

   count = dcvars->yh - dcvars->yl;

   if (count < 0)
      return;

   frac = dcvars->texturemid + (dcvars->yl-centery)*fracstep;

   if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED)
   {
      if (dcvars->yl != 0) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += 0xffff-(slope_texu & 0xffff);
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += slope_texu & 0xffff;
         }
      }
      if (dcvars->yh != viewheight-1) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
      }
      if (count <= 0) return;
   }

   if(temp_x == 4 ||
         (temp_x && (temptype != (COL_FUZZ) || temp_x + startx != dcvars->x)))
      R_FlushColumns();

   if(!temp_x)
   {
      startx = dcvars->x;
      tempyl[0] = commontop = dcvars->yl;
      tempyh[0] = commonbot = dcvars->yh;
      temptype = (COL_FUZZ);



      tempfuzzmap = fullcolormap;

      R_FlushWholeColumns = R_FlushWholeFuzz16;
      R_FlushHTColumns = R_FlushHTFuzz16;
      R_FlushQuadColumn = R_FlushQuadFuzz16;
   }
   else
   {
      tempyl[temp_x] = dcvars->yl;
      tempyh[temp_x] = dcvars->yh;

      if(dcvars->yl > commontop)
         commontop = dcvars->yl;
      if(dcvars->yh < commonbot)
         commonbot = dcvars->yh;
   }
   temp_x += 1;
}

static void R_DrawFuzzColumn16_RoundedUV_PointZ(draw_column_vars_t *dcvars)
{
   int count;
   fixed_t frac;
   const fixed_t fracstep = dcvars->iscale;
   const fixed_t slope_texu = dcvars->texu;

   if (dcvars->iscale > drawvars.mag_threshold)
   {
      R_GetDrawColumnFunc(RDC_PIPELINE_FUZZ,
            RDRAW_FILTER_POINT,
            drawvars.filterz)(dcvars);
      return;
   }

   if (!dcvars->yl)
      dcvars->yl = 1;

   if (dcvars->yh == viewheight-1)
      dcvars->yh = viewheight - 2;

   count = dcvars->yh - dcvars->yl;

   if (count < 0)
      return;

   frac = dcvars->texturemid + (dcvars->yl-centery)*fracstep;

   if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED)
   {
      if (dcvars->yl != 0)
      {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP)
         {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += 0xffff-(slope_texu & 0xffff);
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += slope_texu & 0xffff;
         }
      }
      if (dcvars->yh != viewheight-1) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
      }
      if (count <= 0)
         return;
   }

   if(temp_x == 4 ||
         (temp_x && (temptype != (COL_FUZZ) || temp_x + startx != dcvars->x)))
      R_FlushColumns();

   if(!temp_x)
   {
      startx = dcvars->x;
      tempyl[0] = commontop = dcvars->yl;
      tempyh[0] = commonbot = dcvars->yh;
      temptype = (COL_FUZZ);



      tempfuzzmap = fullcolormap;

      R_FlushWholeColumns = R_FlushWholeFuzz16;
      R_FlushHTColumns = R_FlushHTFuzz16;
      R_FlushQuadColumn = R_FlushQuadFuzz16;



   }
   else
   {
      tempyl[temp_x] = dcvars->yl;
      tempyh[temp_x] = dcvars->yh;

      if(dcvars->yl > commontop)
         commontop = dcvars->yl;
      if(dcvars->yh < commonbot)
         commonbot = dcvars->yh;




   }
   temp_x += 1;
}

static void R_DrawFuzzColumn16_RoundedUV_LinearZ(draw_column_vars_t *dcvars)
{
   int count;
   fixed_t frac;
   const fixed_t fracstep = dcvars->iscale;
   const fixed_t slope_texu = dcvars->texu;

   if (dcvars->iscale > drawvars.mag_threshold)
   {
      R_GetDrawColumnFunc(RDC_PIPELINE_FUZZ,
            RDRAW_FILTER_POINT,
            drawvars.filterz)(dcvars);
      return;
   }

   if (!dcvars->yl)
      dcvars->yl = 1;

   if (dcvars->yh == viewheight-1)
      dcvars->yh = viewheight - 2;

   count = dcvars->yh - dcvars->yl;

   if (count < 0)
      return;

   frac = dcvars->texturemid + (dcvars->yl-centery)*fracstep;

   if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED)
   {
      if (dcvars->yl != 0)
      {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += 0xffff-(slope_texu & 0xffff);
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += slope_texu & 0xffff;
         }
      }
      if (dcvars->yh != viewheight-1) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
      }
      if (count <= 0) return;
   }

   if(temp_x == 4 ||
         (temp_x && (temptype != (COL_FUZZ) || temp_x + startx != dcvars->x)))
      R_FlushColumns();

   if(!temp_x)
   {
      startx = dcvars->x;
      tempyl[0] = commontop = dcvars->yl;
      tempyh[0] = commonbot = dcvars->yh;
      temptype = (COL_FUZZ);

      tempfuzzmap = fullcolormap;

      R_FlushWholeColumns = R_FlushWholeFuzz16;
      R_FlushHTColumns = R_FlushHTFuzz16;
      R_FlushQuadColumn = R_FlushQuadFuzz16;
   }
   else
   {
      tempyl[temp_x] = dcvars->yl;
      tempyh[temp_x] = dcvars->yh;

      if(dcvars->yl > commontop)
         commontop = dcvars->yl;
      if(dcvars->yh < commonbot)
         commonbot = dcvars->yh;
   }
   temp_x += 1;
}

static R_DrawColumn_f drawcolumnfuncs[RDRAW_FILTER_MAXFILTERS][RDRAW_FILTER_MAXFILTERS][RDC_PIPELINE_MAXPIPELINES] = {
    {
      {NULL, NULL, NULL},
      {R_DrawColumn16_PointUV,
       R_DrawTranslatedColumn16_PointUV,
       R_DrawFuzzColumn16_PointUV,},
      {R_DrawColumn16_LinearUV,
       R_DrawTranslatedColumn16_LinearUV,
       R_DrawFuzzColumn16_LinearUV,},
      {R_DrawColumn16_RoundedUV,
       R_DrawTranslatedColumn16_RoundedUV,
       R_DrawFuzzColumn16_RoundedUV,},
    },
    {
      {NULL, NULL, NULL},
      {R_DrawColumn16_PointUV_PointZ,
       R_DrawTranslatedColumn16_PointUV_PointZ,
       R_DrawFuzzColumn16_PointUV_PointZ,},
      {R_DrawColumn16_LinearUV_PointZ,
       R_DrawTranslatedColumn16_LinearUV_PointZ,
       R_DrawFuzzColumn16_LinearUV_PointZ,},
      {R_DrawColumn16_RoundedUV_PointZ,
       R_DrawTranslatedColumn16_RoundedUV_PointZ,
       R_DrawFuzzColumn16_RoundedUV_PointZ,},
    },
    {
      {NULL, NULL, NULL},
      {R_DrawColumn16_PointUV_LinearZ,
       R_DrawTranslatedColumn16_PointUV_LinearZ,
       R_DrawFuzzColumn16_PointUV_LinearZ,},
      {R_DrawColumn16_LinearUV_LinearZ,
       R_DrawTranslatedColumn16_LinearUV_LinearZ,
       R_DrawFuzzColumn16_LinearUV_LinearZ,},
      {R_DrawColumn16_RoundedUV_LinearZ,
       R_DrawTranslatedColumn16_RoundedUV_LinearZ,
       R_DrawFuzzColumn16_RoundedUV_LinearZ,},
    },
};

R_DrawColumn_f R_GetDrawColumnFunc(enum column_pipeline_e type,
                                   enum draw_filter_type_e filter,
                                   enum draw_filter_type_e filterz) {
  R_DrawColumn_f result = drawcolumnfuncs[filterz][filter][type];
  if (result == NULL)
    I_Error("R_GetDrawColumnFunc: undefined function (%d, %d, %d)",
            type, filter, filterz);
  return result;
}

/* Classify a column drawer for the wall-run kernel below: 1 for the
 * unlit point drawer (composed palette), 2 for the lit point drawer
 * (composed colormap), 0 for everything else.  Only these two are
 * reproduced by R_DrawWallColumnRun; records using any other drawer
 * replay individually. */
int R_WallColumnKernelClass(R_DrawColumn_f fn)
{
  if (fn == R_DrawColumn16_PointUV)
    return 1;
  if (fn == R_DrawColumn16_PointUV_PointZ)
    return 2;
  return 0;
}

/* Row-major rasterization of a run of x-adjacent wall columns,
 * writing the framebuffer directly instead of going through the
 * 4-column temp buffer and its transposing flush.  The caller (the
 * draw-record replay) guarantees: x ascends by one across the run,
 * drawingmasked is clear, and every column's texheight is zero or a
 * power of two, so the texel fetch is a mask in every lane.
 *
 * Per-pixel arithmetic is exactly the point drawers':
 *   frac(y) = texturemid + (y - centery) * iscale
 *   texel   = source[(int)(frac & mask) >> 16]
 *
 * The shift is arithmetic on purpose: the drawers index
 * source[frac>>16] on the signed frac, so for texheight 0 (mask all
 * ones) a negative frac on the first rows must read the column
 * padding at index -1, exactly as they do, rather than a huge
 * unsigned index.  For the power-of-two masks the & clears the sign
 * bit and the arithmetic shift is identical to the logical one.
 *   pixel   = lut[texel]
 * The drawers compute frac iteratively from yl; starting the iteration
 * at the run's first row instead is the same integer sequence (adds
 * are associative mod 2^32), so every written pixel is bit-identical
 * to individual replay -- only the visit order changes, and solid
 * wall pixels are disjoint.  frac advances every row whether or not
 * the column covers it, which keeps the row loop uniform (and is the
 * shape a vector version of this kernel wants). */
#define WALL_RUN_MAX 64

void R_DrawWallColumnRun(const draw_column_vars_t *const *cols, int n, int pointz)
{
  const uint8_t     *src[WALL_RUN_MAX];
  const lighttable_t *cmap[WALL_RUN_MAX];
  fixed_t            frac[WALL_RUN_MAX];
  fixed_t            step[WALL_RUN_MAX];
  unsigned int       mask[WALL_RUN_MAX];
  int                cyl[WALL_RUN_MAX];
  int                cyh[WALL_RUN_MAX];
  const uint16_t    *lut = NULL;
  int                ymin, ymax, dtop, dbot, y, j;
  int                x0 = cols[0]->x;

  ymin = cols[0]->yl;
  ymax = cols[0]->yh;
  for (j = 0; j < n; j++)
  {
    const draw_column_vars_t *c = cols[j];
    cyl[j]  = c->yl;
    cyh[j]  = c->yh;
    if (c->yl < ymin) ymin = c->yl;
    if (c->yh > ymax) ymax = c->yh;
    src[j]  = c->source;
    step[j] = c->iscale;
    cmap[j] = c->colormap;
    mask[j] = ((unsigned int)(c->texheight - 1) << 16) | 0xffffu;
  }
  for (j = 0; j < n; j++)
    frac[j] = cols[j]->texturemid + (ymin - centery) * step[j];

  /* Resolve the per-texel table up front where one table covers the
   * whole run.  The unlit drawer's palette table is shared by nature.
   * The lit drawer's composed table is keyed on a single colormap, so
   * it applies whenever every lane carries the same colormap -- which
   * adjacent wall columns usually do (same light band): measured ~77%
   * of runs on freedoom E1M1.  A run with mixed colormaps falls back
   * to the table's defining expression per pixel,
   * V_Palette16[colormap[texel]*64 + 63], the same values either way. */
  if (!pointz)
    lut = R_GetComposedPalette();
  else
  {
    for (j = 1; j < n; j++)
      if (cmap[j] != cmap[0])
        break;
    if (j == n)
      lut = R_GetComposedColormap(cmap[0]);
  }

  /* Every lane covers one contiguous row interval, so the rows where
   * ALL lanes are covered form exactly [max yl, min yh] -- the dense
   * band, ~91% of run pixels here.  The ragged head and tail above and
   * below it keep the per-pixel coverage test; the dense band drops it.
   * frac advances every row in every lane throughout, as before, so
   * the per-pixel arithmetic is unchanged -- this only removes tests
   * and table indirections that cannot change the written values. */
  /* Seed from cols[0] directly (cyl[0]/cyh[0] hold the same values):
   * the function requires n >= 1 -- cols[0] is dereferenced above
   * unconditionally -- but the compiler cannot see that across the
   * call boundary and warns that the arrays may be uninitialized. */
  dtop = cols[0]->yl;
  dbot = cols[0]->yh;
  for (j = 1; j < n; j++)
  {
    if (cyl[j] > dtop) dtop = cyl[j];
    if (cyh[j] < dbot) dbot = cyh[j];
  }

#define WALL_RUN_RAGGED_ROW(EXPR)                          \
  {                                                        \
    uint16_t *row = drawvars.short_topleft                 \
                  + y * SURFACE_SHORT_PITCH + x0;          \
    for (j = 0; j < n; j++)                                \
    {                                                      \
      if (y >= cyl[j] && y <= cyh[j])                      \
      {                                                    \
        int texel = src[j][(int)(frac[j] & mask[j]) >> 16]; \
        row[j] = EXPR;                                     \
      }                                                    \
      frac[j] += step[j];                                  \
    }                                                      \
  }

#define WALL_RUN_DENSE_ROW(EXPR)                           \
  {                                                        \
    uint16_t *row = drawvars.short_topleft                 \
                  + y * SURFACE_SHORT_PITCH + x0;          \
    for (j = 0; j < n; j++)                                \
    {                                                      \
      int texel = src[j][(int)(frac[j] & mask[j]) >> 16];   \
      row[j] = EXPR;                                       \
      frac[j] += step[j];                                  \
    }                                                      \
  }

  if (dtop > dbot)
  {
    /* No row covers every lane; the whole run is ragged. */
    if (lut)
      for (y = ymin; y <= ymax; y++)
        WALL_RUN_RAGGED_ROW(lut[texel])
    else
      for (y = ymin; y <= ymax; y++)
        WALL_RUN_RAGGED_ROW(V_Palette16[ cmap[j][texel] * 64 + 63 ])
    return;
  }

  if (lut)
  {
    for (y = ymin; y < dtop; y++)
      WALL_RUN_RAGGED_ROW(lut[texel])
#if defined(WALL_RUN_SSE2)
#define WALL_RUN_VEC4(J)                                              \
      {                                                               \
        __m128i f = _mm_loadu_si128((const __m128i *)&frac[J]);       \
        __m128i m = _mm_loadu_si128((const __m128i *)&mask[J]);       \
        __m128i s = _mm_loadu_si128((const __m128i *)&step[J]);       \
        _mm_storeu_si128((__m128i *)idx4,                             \
                         _mm_srai_epi32(_mm_and_si128(f, m), 16));    \
        _mm_storeu_si128((__m128i *)&frac[J], _mm_add_epi32(f, s));   \
      }
#elif defined(WALL_RUN_NEON)
#define WALL_RUN_VEC4(J)                                              \
      {                                                               \
        uint32x4_t f = vld1q_u32((const uint32_t *)&frac[J]);         \
        uint32x4_t m = vld1q_u32(&mask[J]);                           \
        uint32x4_t s = vld1q_u32((const uint32_t *)&step[J]);         \
        vst1q_s32(idx4,                                               \
                  vshrq_n_s32(vreinterpretq_s32_u32(vandq_u32(f, m)),  \
                              16));                                   \
        vst1q_u32((uint32_t *)&frac[J], vaddq_u32(f, s));             \
      }
#endif

#ifdef WALL_RUN_VEC4
    for (y = dtop; y <= dbot; y++)
    {
      uint16_t *row = drawvars.short_topleft
                    + y * SURFACE_SHORT_PITCH + x0;
      int j4 = n & ~3;
      for (j = 0; j < j4; j += 4)
      {
        int32_t      idx4[4];
        uint16_t     out4[4];
        WALL_RUN_VEC4(j)
        out4[0] = lut[src[j + 0][idx4[0]]];
        out4[1] = lut[src[j + 1][idx4[1]]];
        out4[2] = lut[src[j + 2][idx4[2]]];
        out4[3] = lut[src[j + 3][idx4[3]]];
        memcpy(&row[j], out4, 4 * sizeof(uint16_t));
      }
      for (; j < n; j++)
      {
        row[j] = lut[ src[j][(int)(frac[j] & mask[j]) >> 16] ];
        frac[j] += step[j];
      }
    }
#undef WALL_RUN_VEC4
#else
    for (y = dtop; y <= dbot; y++)
      WALL_RUN_DENSE_ROW(lut[texel])
#endif
    for (y = dbot + 1; y <= ymax; y++)
      WALL_RUN_RAGGED_ROW(lut[texel])
  }
  else
  {
    for (y = ymin; y < dtop; y++)
      WALL_RUN_RAGGED_ROW(V_Palette16[ cmap[j][texel] * 64 + 63 ])
    for (y = dtop; y <= dbot; y++)
      WALL_RUN_DENSE_ROW(V_Palette16[ cmap[j][texel] * 64 + 63 ])
    for (y = dbot + 1; y <= ymax; y++)
      WALL_RUN_RAGGED_ROW(V_Palette16[ cmap[j][texel] * 64 + 63 ])
  }

#undef WALL_RUN_RAGGED_ROW
#undef WALL_RUN_DENSE_ROW
}

void R_SetDefaultDrawColumnVars(draw_column_vars_t *dcvars)
{
   dcvars->x             = 0;
   dcvars->yl            = 0;
   dcvars->yh            = 0;
   dcvars->z             = 0;
   dcvars->iscale        = 0;
   dcvars->texturemid    = 0;
   dcvars->texheight     = 0;
   dcvars->texu          = 0;
   dcvars->source        = NULL;
   dcvars->prevsource    = NULL;
   dcvars->nextsource    = NULL;
   dcvars->brightmask    = NULL;
   dcvars->colormap      = colormaps[0];
   dcvars->nextcolormap  = colormaps[0];
   dcvars->translation   = NULL;
   dcvars->edgeslope     = 0;
   dcvars->drawingmasked = 0;
   dcvars->edgetype      = drawvars.sprite_edges;
}

//
// R_InitTranslationTables
// Creates the translation tables to map
//  the green color ramp to gray, brown, red.
// Assumes a given structure of the PLAYPAL.
// Could be read from a lump instead.
//

uint8_t playernumtotrans[MAXPLAYERS];
extern lighttable_t *(*c_zlight)[LIGHTLEVELS][MAXLIGHTZ];

void R_InitTranslationTables (void)
{
   int i, j;
#define MAXTRANS 3
   uint8_t transtocolour[MAXTRANS];

   // killough 5/2/98:
   // Remove dependency of colormaps aligned on 256-byte boundary

   if (!translationtables) // CPhipps - allow multiple calls
      translationtables = Z_Malloc(256*MAXTRANS, PU_STATIC, 0);

   for (i=0; i<MAXTRANS; i++)
      transtocolour[i] = 255;

   for (i=0; i<MAXPLAYERS; i++)
   {
      uint8_t wantcolour = mapcolor_plyr[i];
      playernumtotrans[i] = 0;
      if (wantcolour != 0x70) // Not green, would like translation
         for (j=0; j<MAXTRANS; j++)
         {
            if (transtocolour[j] == 255)
            {
               transtocolour[j] = wantcolour;
               playernumtotrans[i] = j+1;
               break;
            }
         }
   }

   // translate just the 16 green colors
   for (i=0; i<256; i++)
   {
      if (i >= 0x70 && i<= 0x7f)
      {
         // CPhipps - configurable player colours
         translationtables[i]     = colormaps[0][((i&0xf)<<9) + transtocolour[0]];
         translationtables[i+256] = colormaps[0][((i&0xf)<<9) + transtocolour[1]];
         translationtables[i+512] = colormaps[0][((i&0xf)<<9) + transtocolour[2]];
      }
      else  // Keep all other colors as is.
      {
         translationtables[i]     = i;
         translationtables[i+256] = i;
         translationtables[i+512] = i;
      }
   }
}

//
// R_DrawSpan
// With DOOM style restrictions on view orientation,
//  the floors and ceilings consist of horizontal slices
//  or spans with constant z depth.
// However, rotation around the world z axis is possible,
//  thus this mapping, while simpler and faster than
//  perspective correct texture mapping, has to traverse
//  the texture at an angle in all but a few cases.
// In consequence, flats are not stored by column (like walls),
//  and the inner loop has to step in texture space u and v.
//

static void R_DrawSpan16_PointUV_PointZ(draw_span_vars_t *dsvars)
{
   unsigned count = dsvars->x2 - dsvars->x1 + 1;
   fixed_t xfrac = dsvars->xfrac;
   fixed_t yfrac = dsvars->yfrac;
   const fixed_t xstep = dsvars->xstep;
   const fixed_t ystep = dsvars->ystep;
   const uint8_t *source = dsvars->source;

   uint16_t *dest = drawvars.short_topleft + dsvars->y* SCREENWIDTH + dsvars->x1;

   /* Shared composed colormap+palette table (see R_GetComposedColormap):
    * collapses V_Palette16[colormap[texel]*64+63] to one lookup, rebuilt
    * only when the colormap pointer or V_Palette16 changes. */
   const uint16_t *lut = R_GetComposedColormap(dsvars->colormap);

   /* Brightmap path: where the 64x64 row-major mask is set, the texel is
    * drawn through the undimmed base map (fullcolormap) instead of the
    * distance-lit table.  fullcolormap's composed table is snapshot into
    * a local first (the shared composed_lut cache is single-entry, so
    * fetching the distance `lut` would evict it).  Kept scalar; the SIMD
    * select lands in a later step.  NULL mask -> the vectorised path
    * below runs unchanged. */
   if (dsvars->brightmask)
   {
      const uint8_t  *mask = dsvars->brightmask;
      uint16_t        lut_bright[256];
      const uint16_t *bsrc = R_GetComposedColormap(fullcolormap
                                                   ? fullcolormap
                                                   : dsvars->colormap);
      memcpy(lut_bright, bsrc, sizeof(lut_bright));
      lut = R_GetComposedColormap(dsvars->colormap);

#if defined(__SSE2__)
      /* Same spot-index vectorisation as the non-brightmap path: four
       * (xtemp|ytemp) indices per iteration with packed arithmetic
       * shifts/masks, then a scalar gather -- here a per-lane select
       * between the distance and fullbright tables on the mask bit.  The
       * spot math is identical to the scalar loop below (srai matches the
       * signed >> on fixed_t), so the output is bit-identical. */
      if (count >= 8)
      {
         unsigned blocks = count >> 2;
         __m128i vx  = _mm_set_epi32(xfrac + 3*xstep, xfrac + 2*xstep,
                                     xfrac + xstep,   xfrac);
         __m128i vy  = _mm_set_epi32(yfrac + 3*ystep, yfrac + 2*ystep,
                                     yfrac + ystep,   yfrac);
         const __m128i vxs   = _mm_set1_epi32(xstep << 2);
         const __m128i vys   = _mm_set1_epi32(ystep << 2);
         const __m128i m63   = _mm_set1_epi32(63);
         const __m128i m4032 = _mm_set1_epi32(4032);
         unsigned consumed   = blocks << 2;

         while (blocks--)
         {
            uint32_t idx[4];
            __m128i xt   = _mm_and_si128(_mm_srai_epi32(vx, 16), m63);
            __m128i yt   = _mm_and_si128(_mm_srai_epi32(vy, 10), m4032);
            __m128i spot = _mm_or_si128(xt, yt);

            _mm_storeu_si128((__m128i *)idx, spot);

            dest[0] = (mask[idx[0]] ? lut_bright : lut)[ source[idx[0]] ];
            dest[1] = (mask[idx[1]] ? lut_bright : lut)[ source[idx[1]] ];
            dest[2] = (mask[idx[2]] ? lut_bright : lut)[ source[idx[2]] ];
            dest[3] = (mask[idx[3]] ? lut_bright : lut)[ source[idx[3]] ];
            dest += 4;

            vx = _mm_add_epi32(vx, vxs);
            vy = _mm_add_epi32(vy, vys);
         }

         xfrac += (fixed_t)consumed * xstep;
         yfrac += (fixed_t)consumed * ystep;
         count -= consumed;
      }
#elif defined(__ARM_NEON)
      if (count >= 8)
      {
         unsigned blocks = count >> 2;
         const int32_t xbase[4] = { xfrac, xfrac + xstep,
                                    xfrac + 2*xstep, xfrac + 3*xstep };
         const int32_t ybase[4] = { yfrac, yfrac + ystep,
                                    yfrac + 2*ystep, yfrac + 3*ystep };
         int32x4_t vx = vld1q_s32(xbase);
         int32x4_t vy = vld1q_s32(ybase);
         const int32x4_t vxs   = vdupq_n_s32(xstep << 2);
         const int32x4_t vys   = vdupq_n_s32(ystep << 2);
         const int32x4_t m63   = vdupq_n_s32(63);
         const int32x4_t m4032 = vdupq_n_s32(4032);
         unsigned consumed     = blocks << 2;

         while (blocks--)
         {
            uint32_t idx[4];
            int32x4_t xt   = vandq_s32(vshrq_n_s32(vx, 16), m63);
            int32x4_t yt   = vandq_s32(vshrq_n_s32(vy, 10), m4032);
            int32x4_t spot = vorrq_s32(xt, yt);

            vst1q_u32(idx, vreinterpretq_u32_s32(spot));

            dest[0] = (mask[idx[0]] ? lut_bright : lut)[ source[idx[0]] ];
            dest[1] = (mask[idx[1]] ? lut_bright : lut)[ source[idx[1]] ];
            dest[2] = (mask[idx[2]] ? lut_bright : lut)[ source[idx[2]] ];
            dest[3] = (mask[idx[3]] ? lut_bright : lut)[ source[idx[3]] ];
            dest += 4;

            vx = vaddq_s32(vx, vxs);
            vy = vaddq_s32(vy, vys);
         }

         xfrac += (fixed_t)consumed * xstep;
         yfrac += (fixed_t)consumed * ystep;
         count -= consumed;
      }
#endif

      while (count)
      {
         const fixed_t xtemp = (xfrac >> 16) & 63;
         const fixed_t ytemp = (yfrac >> 10) & 4032;
         const fixed_t spot  = xtemp | ytemp;
         xfrac += xstep;
         yfrac += ystep;
         *dest++ = (mask[spot] ? lut_bright : lut)[ source[spot] ];
         count--;
      }
      return;
   }

#if defined(__SSE2__)
   /* The per-pixel index math (two arithmetic shifts, two masks, an OR,
    * and the two fixed-point accumulator adds) is the bulk of this loop;
    * the texel/LUT gather is cheap because the 64x64 source tile and the
    * 256-entry LUT stay resident in L1.  SSE2 has no gather, so the
    * gather + store stay scalar, but computing four spot indices at once
    * removes most of the loop's arithmetic.  Output is bit-identical to
    * the scalar path: srai matches the signed >> on fixed_t, and the
    * mask/or and accumulator progression are the same per lane.
    *
    * Spans shorter than 8 px (screen edges, and any span too narrow to
    * amortise the vector setup over more than one iteration) fall through
    * to the scalar loop below; the vector block handles the count & ~3
    * prefix and the scalar loop finishes the remainder with the same
    * xfrac/yfrac. */
   if (count >= 8)
   {
      unsigned blocks = count >> 2;
      __m128i vx  = _mm_set_epi32(xfrac + 3*xstep, xfrac + 2*xstep,
                                  xfrac + xstep,   xfrac);
      __m128i vy  = _mm_set_epi32(yfrac + 3*ystep, yfrac + 2*ystep,
                                  yfrac + ystep,   yfrac);
      const __m128i vxs   = _mm_set1_epi32(xstep << 2);
      const __m128i vys   = _mm_set1_epi32(ystep << 2);
      const __m128i m63   = _mm_set1_epi32(63);
      const __m128i m4032 = _mm_set1_epi32(4032);
      unsigned consumed   = blocks << 2;

      while (blocks--)
      {
         uint32_t idx[4];
         __m128i xt   = _mm_and_si128(_mm_srai_epi32(vx, 16), m63);
         __m128i yt   = _mm_and_si128(_mm_srai_epi32(vy, 10), m4032);
         __m128i spot = _mm_or_si128(xt, yt);

         _mm_storeu_si128((__m128i *)idx, spot);

         dest[0] = lut[ source[idx[0]] ];
         dest[1] = lut[ source[idx[1]] ];
         dest[2] = lut[ source[idx[2]] ];
         dest[3] = lut[ source[idx[3]] ];
         dest += 4;

         vx = _mm_add_epi32(vx, vxs);
         vy = _mm_add_epi32(vy, vys);
      }

      /* Advance scalar accumulators past the vectorised prefix and
       * leave only count & 3 iterations for the scalar tail. */
      xfrac += (fixed_t)consumed * xstep;
      yfrac += (fixed_t)consumed * ystep;
      count -= consumed;
   }
#elif defined(__ARM_NEON)
   /* NEON counterpart of the SSE2 path above: compute four spot indices
    * per iteration with packed arithmetic shifts/masks, gather and store
    * scalar (NEON has no gather, and the L1-resident tile/LUT make scalar
    * loads cheap).  vshrq_n_s32 is an arithmetic (sign-propagating) shift,
    * matching the signed >> on fixed_t, so output is bit-identical to the
    * scalar path -- verified over 300k spans across widths, full 32-bit
    * signed fracs and both-sign steps.  Same count >= 8 entry threshold
    * and scalar tail as the SSE2 path. */
   if (count >= 8)
   {
      unsigned blocks = count >> 2;
      const int32_t xbase[4] = { xfrac, xfrac + xstep,
                                 xfrac + 2*xstep, xfrac + 3*xstep };
      const int32_t ybase[4] = { yfrac, yfrac + ystep,
                                 yfrac + 2*ystep, yfrac + 3*ystep };
      int32x4_t vx = vld1q_s32(xbase);
      int32x4_t vy = vld1q_s32(ybase);
      const int32x4_t vxs   = vdupq_n_s32(xstep << 2);
      const int32x4_t vys   = vdupq_n_s32(ystep << 2);
      const int32x4_t m63   = vdupq_n_s32(63);
      const int32x4_t m4032 = vdupq_n_s32(4032);
      unsigned consumed     = blocks << 2;

      while (blocks--)
      {
         uint32_t idx[4];
         int32x4_t xt   = vandq_s32(vshrq_n_s32(vx, 16), m63);
         int32x4_t yt   = vandq_s32(vshrq_n_s32(vy, 10), m4032);
         int32x4_t spot = vorrq_s32(xt, yt);

         vst1q_u32(idx, vreinterpretq_u32_s32(spot));

         dest[0] = lut[ source[idx[0]] ];
         dest[1] = lut[ source[idx[1]] ];
         dest[2] = lut[ source[idx[2]] ];
         dest[3] = lut[ source[idx[3]] ];
         dest += 4;

         vx = vaddq_s32(vx, vxs);
         vy = vaddq_s32(vy, vys);
      }

      xfrac += (fixed_t)consumed * xstep;
      yfrac += (fixed_t)consumed * ystep;
      count -= consumed;
   }
#endif

   while (count)
   {
      const fixed_t xtemp = (xfrac >> 16) & 63;
      const fixed_t ytemp = (yfrac >> 10) & 4032;

      const fixed_t spot = xtemp | ytemp;
      xfrac += xstep;
      yfrac += ystep;
      *dest++ = lut[ source[spot] ];
      count--;
   }
}

static void R_DrawSpan16_PointUV_LinearZ(draw_span_vars_t *dsvars)
{
   /* Brightmapped flats take the point/point span drawer, the only
    * one with the fullbright select; filtered spans blend several
    * lookups per pixel with no single table to redirect, so a masked
    * flat trades that smoothing for correct fullbright. */
   if (dsvars->brightmask)
   {
      R_DrawSpan16_PointUV_PointZ(dsvars);
      return;
   }
   {
   unsigned count = dsvars->x2 - dsvars->x1 + 1;
   fixed_t xfrac = dsvars->xfrac;
   fixed_t yfrac = dsvars->yfrac;
   const fixed_t xstep = dsvars->xstep;
   const fixed_t ystep = dsvars->ystep;
   const uint8_t *source = dsvars->source;




   uint16_t *dest = drawvars.short_topleft + dsvars->y* SCREENWIDTH + dsvars->x1;

   const int y = dsvars->y;
   int x1 = dsvars->x1;


   const int fracz = (dsvars->z >> 12) & 255;
   const uint8_t *dither_colormaps[2] = { dsvars->colormap, dsvars->nextcolormap };


   while (count) {
      const fixed_t xtemp = (xfrac >> 16) & 63;
      const fixed_t ytemp = (yfrac >> 10) & 4032;

      const fixed_t spot = xtemp | ytemp;
      xfrac += xstep;
      yfrac += ystep;
      *dest++ = V_Palette16[ (dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x1)&(4 -1)] < (fracz)) ? 1 : 0)][(source[spot])])*64 + ((64 -1)) ];
      count--;

      x1--;


   }
   }
}

static void R_DrawSpan16_LinearUV_PointZ(draw_span_vars_t *dsvars)
{
   /* Brightmapped flats take the point/point span drawer, the only
    * one with the fullbright select; filtered spans blend several
    * lookups per pixel with no single table to redirect, so a masked
    * flat trades that smoothing for correct fullbright. */
   if (dsvars->brightmask)
   {
      R_DrawSpan16_PointUV_PointZ(dsvars);
      return;
   }
   if ((D_abs(dsvars->xstep) > drawvars.mag_threshold)
         || (D_abs(dsvars->ystep) > drawvars.mag_threshold))
   {
      R_GetDrawSpanFunc(RDRAW_FILTER_POINT,
            drawvars.filterz)(dsvars);
      return;
   }

   {
      unsigned count = dsvars->x2 - dsvars->x1 + 1;
      fixed_t xfrac = dsvars->xfrac;
      fixed_t yfrac = dsvars->yfrac;
      const fixed_t xstep = dsvars->xstep;
      const fixed_t ystep = dsvars->ystep;
      const uint8_t *source = dsvars->source;


      const uint8_t *colormap = dsvars->colormap;

      uint16_t *dest = drawvars.short_topleft + dsvars->y* SCREENWIDTH + dsvars->x1;

      while (count) {


         *dest++ = ( V_Palette16[ (colormap[(source[ ((((xfrac)+(1<<16))>>16)&0x3f) | ((((yfrac)+(1<<16))>>10)&0xfc0)])])*64 + ((unsigned int)(((xfrac)&0xffff)*((yfrac)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(source[ (((xfrac)>>16)&0x3f) | ((((yfrac)+(1<<16))>>10)&0xfc0)])])*64 + ((unsigned int)((0xffff-((xfrac)&0xffff))*((yfrac)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(source[ (((xfrac)>>16)&0x3f) | (((yfrac)>>10)&0xfc0)])])*64 + ((unsigned int)((0xffff-((xfrac)&0xffff))*(0xffff-((yfrac)&0xffff)))>>(32-6)) ] + V_Palette16[ (colormap[(source[ ((((xfrac)+(1<<16))>>16)&0x3f) | (((yfrac)>>10)&0xfc0)])])*64 + ((unsigned int)(((xfrac)&0xffff)*(0xffff-((yfrac)&0xffff)))>>(32-6)) ]);
         xfrac += xstep;
         yfrac += ystep;
         count--;
      }
   }
}

static void R_DrawSpan16_LinearUV_LinearZ(draw_span_vars_t *dsvars)
{
   /* Brightmapped flats take the point/point span drawer, the only
    * one with the fullbright select; filtered spans blend several
    * lookups per pixel with no single table to redirect, so a masked
    * flat trades that smoothing for correct fullbright. */
   if (dsvars->brightmask)
   {
      R_DrawSpan16_PointUV_PointZ(dsvars);
      return;
   }
   if ((D_abs(dsvars->xstep) > drawvars.mag_threshold)
         || (D_abs(dsvars->ystep) > drawvars.mag_threshold))
   {
      R_GetDrawSpanFunc(RDRAW_FILTER_POINT,
            drawvars.filterz)(dsvars);
      return;
   }

   {
      unsigned count = dsvars->x2 - dsvars->x1 + 1;
      fixed_t xfrac = dsvars->xfrac;
      fixed_t yfrac = dsvars->yfrac;
      const fixed_t xstep = dsvars->xstep;
      const fixed_t ystep = dsvars->ystep;
      const uint8_t *source = dsvars->source;




      uint16_t *dest = drawvars.short_topleft + dsvars->y* SCREENWIDTH + dsvars->x1;

      const int y = dsvars->y;
      int x1 = dsvars->x1;


      const int fracz = (dsvars->z >> 12) & 255;
      const uint8_t *dither_colormaps[2] = { dsvars->colormap, dsvars->nextcolormap };


      while (count) {


         *dest++ = ( V_Palette16[ (dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x1)&(4 -1)] < (fracz)) ? 1 : 0)][(source[ ((((xfrac)+(1<<16))>>16)&0x3f) | ((((yfrac)+(1<<16))>>10)&0xfc0)])])*64 + ((unsigned int)(((xfrac)&0xffff)*((yfrac)&0xffff))>>(32-6)) ] + V_Palette16[ (dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x1)&(4 -1)] < (fracz)) ? 1 : 0)][(source[ (((xfrac)>>16)&0x3f) | ((((yfrac)+(1<<16))>>10)&0xfc0)])])*64 + ((unsigned int)((0xffff-((xfrac)&0xffff))*((yfrac)&0xffff))>>(32-6)) ] + V_Palette16[ (dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x1)&(4 -1)] < (fracz)) ? 1 : 0)][(source[ (((xfrac)>>16)&0x3f) | (((yfrac)>>10)&0xfc0)])])*64 + ((unsigned int)((0xffff-((xfrac)&0xffff))*(0xffff-((yfrac)&0xffff)))>>(32-6)) ] + V_Palette16[ (dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x1)&(4 -1)] < (fracz)) ? 1 : 0)][(source[ ((((xfrac)+(1<<16))>>16)&0x3f) | (((yfrac)>>10)&0xfc0)])])*64 + ((unsigned int)(((xfrac)&0xffff)*(0xffff-((yfrac)&0xffff)))>>(32-6)) ]);
         xfrac += xstep;
         yfrac += ystep;
         count--;

         x1--;
      }
   }
}

static void R_DrawSpan16_RoundedUV_PointZ(draw_span_vars_t *dsvars)
{
   /* Brightmapped flats take the point/point span drawer, the only
    * one with the fullbright select; filtered spans blend several
    * lookups per pixel with no single table to redirect, so a masked
    * flat trades that smoothing for correct fullbright. */
   if (dsvars->brightmask)
   {
      R_DrawSpan16_PointUV_PointZ(dsvars);
      return;
   }
   if ((D_abs(dsvars->xstep) > drawvars.mag_threshold)
         || (D_abs(dsvars->ystep) > drawvars.mag_threshold))
   {
      R_GetDrawSpanFunc(RDRAW_FILTER_POINT,
            drawvars.filterz)(dsvars);
      return;
   }

   {
      unsigned count = dsvars->x2 - dsvars->x1 + 1;
      fixed_t xfrac = dsvars->xfrac;
      fixed_t yfrac = dsvars->yfrac;
      const fixed_t xstep = dsvars->xstep;
      const fixed_t ystep = dsvars->ystep;
      const uint8_t *source = dsvars->source;


      const uint8_t *colormap = dsvars->colormap;

      uint16_t *dest = drawvars.short_topleft + dsvars->y* SCREENWIDTH + dsvars->x1;
      while (count) {
         *dest++ = V_Palette16[ (colormap[(filter_getScale2xQuadColors( source[ (((xfrac)>>16)&0x3f) | (((yfrac)>>10)&0xfc0) ], source[ (((xfrac)>>16)&0x3f) | ((((yfrac)-(1<<16))>>10)&0xfc0) ], source[ ((((xfrac)+(1<<16))>>16)&0x3f) | (((yfrac)>>10)&0xfc0) ], source[ (((xfrac)>>16)&0x3f) | ((((yfrac)+(1<<16))>>10)&0xfc0) ], source[ ((((xfrac)-(1<<16))>>16)&0x3f) | (((yfrac)>>10)&0xfc0) ] ) [ filter_roundedUVMap[ (((((xfrac)>>8) & 0xff)>>(8-6))<<6) + ((((yfrac)>>8) & 0xff)>>(8-6)) ] ])])*64 + ((64 -1)) ];
         xfrac += xstep;
         yfrac += ystep;
         count--;
      }
   }
}

static void R_DrawSpan16_RoundedUV_LinearZ(draw_span_vars_t *dsvars)
{
   /* Brightmapped flats take the point/point span drawer, the only
    * one with the fullbright select; filtered spans blend several
    * lookups per pixel with no single table to redirect, so a masked
    * flat trades that smoothing for correct fullbright. */
   if (dsvars->brightmask)
   {
      R_DrawSpan16_PointUV_PointZ(dsvars);
      return;
   }
   if ((D_abs(dsvars->xstep) > drawvars.mag_threshold)
         || (D_abs(dsvars->ystep) > drawvars.mag_threshold))
   {
      R_GetDrawSpanFunc(RDRAW_FILTER_POINT,
            drawvars.filterz)(dsvars);
      return;
   }

   {
      unsigned count = dsvars->x2 - dsvars->x1 + 1;
      fixed_t xfrac = dsvars->xfrac;
      fixed_t yfrac = dsvars->yfrac;
      const fixed_t xstep = dsvars->xstep;
      const fixed_t ystep = dsvars->ystep;
      const uint8_t *source = dsvars->source;




      uint16_t *dest = drawvars.short_topleft + dsvars->y* SCREENWIDTH + dsvars->x1;

      const int y = dsvars->y;
      int x1 = dsvars->x1;


      const int fracz = (dsvars->z >> 12) & 255;
      const uint8_t *dither_colormaps[2] = { dsvars->colormap, dsvars->nextcolormap };


      while (count) {
         *dest++ = V_Palette16[ (dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x1)&(4 -1)] < (fracz)) ? 1 : 0)][(filter_getScale2xQuadColors( source[ (((xfrac)>>16)&0x3f) | (((yfrac)>>10)&0xfc0) ], source[ (((xfrac)>>16)&0x3f) | ((((yfrac)-(1<<16))>>10)&0xfc0) ], source[ ((((xfrac)+(1<<16))>>16)&0x3f) | (((yfrac)>>10)&0xfc0) ], source[ (((xfrac)>>16)&0x3f) | ((((yfrac)+(1<<16))>>10)&0xfc0) ], source[ ((((xfrac)-(1<<16))>>16)&0x3f) | (((yfrac)>>10)&0xfc0) ] ) [ filter_roundedUVMap[ (((((xfrac)>>8) & 0xff)>>(8-6))<<6) + ((((yfrac)>>8) & 0xff)>>(8-6)) ] ])])*64 + ((64 -1)) ];
         xfrac += xstep;
         yfrac += ystep;
         count--;

         x1--;
      }
   }
}

/* Translucent point-sampled span: the opaque PointUV_PointZ kernel with a
 * 50/50 RGB565 blend against the framebuffer on store, used to lay a
 * see-through 3D-floor (swimmable) surface over whatever was drawn beneath
 * it.  Point sampling only -- a translucent water surface does not need the
 * filtered/dithered variants.  The SSE2/NEON blocks mirror the opaque
 * kernel's index math exactly (so the texel gather is bit-identical) and add
 * only the load-blend-store of the destination; the blend folds the masked
 * half-sum of source and dest, the same arithmetic as TL_BLEND565. */
static void R_DrawSpan16_TL(draw_span_vars_t *dsvars)
{
   unsigned count = dsvars->x2 - dsvars->x1 + 1;
   fixed_t xfrac = dsvars->xfrac;
   fixed_t yfrac = dsvars->yfrac;
   const fixed_t xstep = dsvars->xstep;
   const fixed_t ystep = dsvars->ystep;
   const uint8_t *source = dsvars->source;
   uint16_t *dest = drawvars.short_topleft + dsvars->y * SCREENWIDTH + dsvars->x1;
   const uint16_t *lut = R_GetComposedColormap(dsvars->colormap);

#if defined(__SSE2__)
   if (count >= 8)
   {
      unsigned blocks = count >> 2;
      __m128i vx  = _mm_set_epi32(xfrac + 3*xstep, xfrac + 2*xstep,
                                  xfrac + xstep,   xfrac);
      __m128i vy  = _mm_set_epi32(yfrac + 3*ystep, yfrac + 2*ystep,
                                  yfrac + ystep,   yfrac);
      const __m128i vxs   = _mm_set1_epi32(xstep << 2);
      const __m128i vys   = _mm_set1_epi32(ystep << 2);
      const __m128i m63   = _mm_set1_epi32(63);
      const __m128i m4032 = _mm_set1_epi32(4032);
      /* 0xF7DE clears each channel's low bit so the >>1 cannot bleed across
       * the R/G/B field boundaries; halving both operands and adding is the
       * 50/50 blend, vectorised four pixels at a time. */
      const __m128i mlow  = _mm_set1_epi16((short)0xF7DE);
      unsigned consumed   = blocks << 2;

      while (blocks--)
      {
         uint32_t idx[4];
         __m128i xt   = _mm_and_si128(_mm_srai_epi32(vx, 16), m63);
         __m128i yt   = _mm_and_si128(_mm_srai_epi32(vy, 10), m4032);
         __m128i spot = _mm_or_si128(xt, yt);
         __m128i src4, dst4, blended;

         _mm_storeu_si128((__m128i *)idx, spot);

         /* gather the four source texels through the LUT into the low four
          * lanes of a 16-bit vector, load the four destination pixels, blend */
         src4 = _mm_set_epi16(0, 0, 0, 0,
                              (short)lut[source[idx[3]]],
                              (short)lut[source[idx[2]]],
                              (short)lut[source[idx[1]]],
                              (short)lut[source[idx[0]]]);
         dst4 = _mm_loadl_epi64((const __m128i *)dest);
         blended = _mm_add_epi16(
                      _mm_srli_epi16(_mm_and_si128(src4, mlow), 1),
                      _mm_srli_epi16(_mm_and_si128(dst4, mlow), 1));
         _mm_storel_epi64((__m128i *)dest, blended);
         dest += 4;

         vx = _mm_add_epi32(vx, vxs);
         vy = _mm_add_epi32(vy, vys);
      }

      xfrac += (fixed_t)consumed * xstep;
      yfrac += (fixed_t)consumed * ystep;
      count -= consumed;
   }
#elif defined(__ARM_NEON)
   if (count >= 8)
   {
      unsigned blocks = count >> 2;
      const int32_t xbase[4] = { xfrac, xfrac + xstep,
                                 xfrac + 2*xstep, xfrac + 3*xstep };
      const int32_t ybase[4] = { yfrac, yfrac + ystep,
                                 yfrac + 2*ystep, yfrac + 3*ystep };
      int32x4_t vx = vld1q_s32(xbase);
      int32x4_t vy = vld1q_s32(ybase);
      const int32x4_t vxs   = vdupq_n_s32(xstep << 2);
      const int32x4_t vys   = vdupq_n_s32(ystep << 2);
      const int32x4_t m63   = vdupq_n_s32(63);
      const int32x4_t m4032 = vdupq_n_s32(4032);
      const uint16x4_t mlow = vdup_n_u16(0xF7DE);
      unsigned consumed     = blocks << 2;

      while (blocks--)
      {
         uint32_t idx[4];
         uint16_t s4[4];
         uint16x4_t src4, dst4, blended;
         int32x4_t xt   = vandq_s32(vshrq_n_s32(vx, 16), m63);
         int32x4_t yt   = vandq_s32(vshrq_n_s32(vy, 10), m4032);
         int32x4_t spot = vorrq_s32(xt, yt);

         vst1q_u32(idx, vreinterpretq_u32_s32(spot));

         s4[0] = lut[source[idx[0]]];
         s4[1] = lut[source[idx[1]]];
         s4[2] = lut[source[idx[2]]];
         s4[3] = lut[source[idx[3]]];
         src4 = vld1_u16(s4);
         dst4 = vld1_u16(dest);
         blended = vadd_u16(vshr_n_u16(vand_u16(src4, mlow), 1),
                            vshr_n_u16(vand_u16(dst4, mlow), 1));
         vst1_u16(dest, blended);
         dest += 4;

         vx = vaddq_s32(vx, vxs);
         vy = vaddq_s32(vy, vys);
      }

      xfrac += (fixed_t)consumed * xstep;
      yfrac += (fixed_t)consumed * ystep;
      count -= consumed;
   }
#endif

   while (count)
   {
      const fixed_t xtemp = (xfrac >> 16) & 63;
      const fixed_t ytemp = (yfrac >> 10) & 4032;
      const fixed_t spot = xtemp | ytemp;
      uint16_t s = lut[source[spot]];
      xfrac += xstep;
      yfrac += ystep;
      *dest = TL_BLEND565(s, *dest);
      dest++;
      count--;
   }
}

/* Set by the plane drawer around a translucent 3D-floor surface pass. */
int r_span_translucent = 0;

static R_DrawSpan_f drawspanfuncs[RDRAW_FILTER_MAXFILTERS][RDRAW_FILTER_MAXFILTERS] = {
    {
      NULL,
      NULL,
      NULL,
      NULL,
    },
    {
      NULL,
      R_DrawSpan16_PointUV_PointZ,
      R_DrawSpan16_LinearUV_PointZ,
      R_DrawSpan16_RoundedUV_PointZ,
    },
    {
      NULL,
      R_DrawSpan16_PointUV_LinearZ,
      R_DrawSpan16_LinearUV_LinearZ,
      R_DrawSpan16_RoundedUV_LinearZ,
    },
    {
      NULL,
      NULL,
      NULL,
      NULL,
    },
};

R_DrawSpan_f R_GetDrawSpanFunc(enum draw_filter_type_e filter,
                               enum draw_filter_type_e filterz) {
  R_DrawSpan_f result = drawspanfuncs[filterz][filter];
  if (result == NULL)
    I_Error("R_GetDrawSpanFunc: undefined function (%d, %d)",
            filter, filterz);
  return result;
}

void R_DrawSpan(draw_span_vars_t *dsvars) {
  if (r_span_translucent) {
    R_DrawSpan16_TL(dsvars);
    return;
  }
  R_GetDrawSpanFunc(drawvars.filterfloor, drawvars.filterz)(dsvars);
}

/*
 * R_FlatAverageColor565 -- the mean RGB565 colour of a flat, used to tint
 * the view when the camera is submerged in a 3D-floor water volume so the
 * tint matches the water surface the player sees.  Strided sample (the flat
 * is flat-shaded enough that every 7th texel is representative) averaged per
 * channel; cached on (picnum, palette) since it only changes on a palette
 * swap.
 */
uint16_t R_FlatAverageColor565(int picnum)
{
   static int       lastpic = -1;
   static uint16_t *lastpal = NULL;
   static uint16_t  lastcol = 0;
   const uint8_t   *flat;
   unsigned long    sr = 0, sg = 0, sb = 0, n = 0;
   int              i, synthetic;
   int              lump = firstflat + flattranslation[picnum];

   if (picnum == lastpic && V_Palette16 == lastpal)
      return lastcol;

   synthetic = R_IsSyntheticFlat(picnum);
   flat = synthetic ? R_GetSyntheticFlat(picnum)
                    : (const uint8_t *)W_CacheLumpNum(lump);

   for (i = 0; i < 4096; i += 7)
   {
      uint16_t c = VID_PAL16(flat[i], VID_NUMCOLORWEIGHTS - 1);
      sr += (c >> 11) & 0x1F;
      sg += (c >> 5)  & 0x3F;
      sb +=  c        & 0x1F;
      n++;
   }

   if (!synthetic)
      W_UnlockLumpNum(lump);

   if (!n)
      n = 1;
   lastcol = (uint16_t)(((sr / n) << 11) | ((sg / n) << 5) | (sb / n));
   lastpic = picnum;
   lastpal = V_Palette16;
   return lastcol;
}

/*
 * R_TintView -- blend the 3D view 50/50 toward a constant colour, the
 * underwater tint applied after the scene is drawn.  Scalar, SSE2 and NEON;
 * the SSE2/NEON paths fold eight RGB565 pixels per step with the same masked
 * half-sum as TL_BLEND565 and fall back to the scalar tail for the < 8
 * remainder of each row.
 */
void R_TintView(uint16_t color)
{
   uint16_t *base = drawvars.short_topleft;
   int y;
#if defined(__SSE2__)
   const __m128i vcol = _mm_set1_epi16((short)color);
   const __m128i mlow = _mm_set1_epi16((short)0xF7DE);
   const __m128i vch  = _mm_srli_epi16(_mm_and_si128(vcol, mlow), 1);
#elif defined(__ARM_NEON)
   const uint16x8_t vcol = vdupq_n_u16(color);
   const uint16x8_t mlow = vdupq_n_u16(0xF7DE);
   const uint16x8_t vch  = vshrq_n_u16(vandq_u16(vcol, mlow), 1);
#endif

   for (y = 0; y < viewheight; y++)
   {
      uint16_t *row = base + y * SCREENWIDTH;
      int x = 0;
#if defined(__SSE2__)
      for (; x + 8 <= viewwidth; x += 8)
      {
         __m128i px = _mm_loadu_si128((const __m128i *)(row + x));
         __m128i bl = _mm_add_epi16(vch, _mm_srli_epi16(_mm_and_si128(px, mlow), 1));
         _mm_storeu_si128((__m128i *)(row + x), bl);
      }
#elif defined(__ARM_NEON)
      for (; x + 8 <= viewwidth; x += 8)
      {
         uint16x8_t px = vld1q_u16(row + x);
         uint16x8_t bl = vaddq_u16(vch, vshrq_n_u16(vandq_u16(px, mlow), 1));
         vst1q_u16(row + x, bl);
      }
#endif
      for (; x < viewwidth; x++)
         row[x] = TL_BLEND565(color, row[x]);
   }
}

//
// R_InitBuffer
// Creats lookup tables that avoid
//  multiplies and other hazzles
//  for getting the framebuffer address
//  of a pixel to draw.
//

void R_InitBuffer(int width, int height)
{
  int i=0;
  // Handle resize,
  //  e.g. smaller view windows
  //  with border and/or status bar.

  // Same with base row offset.

  drawvars.short_topleft = (unsigned short *)(screens[0].data);
  drawvars.int_topleft = (unsigned int *)(screens[0].data);

  for (i=0; i<FUZZTABLE; i++)
	  fuzzoffset[i] = fuzzoffset_org[i] * SURFACE_SHORT_PITCH;
}
