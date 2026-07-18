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
 *      Truecolor (XRGB8888 / XRGB2101010) drawer core.
 *
 *      This is the deliberate twin of the RGB565 renderer in r_draw.c.
 *      The column/span/wall-run drawers, the composed-LUT machinery and
 *      the dispatch tables are the same code with a 16->32-bit surface
 *      retype: identical index arithmetic, identical batching, identical
 *      filter taps.  Only the read-modify-write blend kernels differ per
 *      format, and those live in r_drawtcfmt.inl, included once for each
 *      32-bit layout.
 *
 *      KEEPING THE TWO IN SYNC: a change to the drawer bodies, dispatch
 *      tables, composed-LUT machinery or the wall-run kernel in r_draw.c
 *      almost always needs the same change here.  The 16-bit path is the
 *      reference and must stay byte-identical; verify that first, then
 *      check this file renders the same scene.
 *
 *      The two 32-bit formats SHARE one drawer instantiation: an opaque
 *      drawer writes lut[texel] and is correct for any format given a
 *      format-correct LUT (V_PaletteTC).  Only the read-modify-write
 *      blend kernels carry per-format channel constants, so only those
 *      are compiled twice; the dispatch thunks below pick the right pair
 *      at runtime from vid_mode.
 *
 *-----------------------------------------------------------------------------*/

#include <stdlib.h>
#include <string.h>

#include "doomstat.h"
#include "w_wad.h"
#include "r_main.h"
#include "r_things.h"
#include "r_draw.h"
#include "r_drawtc.h"
#include "r_plane.h"
#include "v_video.h"
#include "st_stuff.h"
#include "g_game.h"
#include "am_map.h"
#include "lprintf.h"
#include "r_filter.h"
#include "r_data.h"
#include "vid_mode.h"

/* Same SIMD gating as r_draw.c (WALL_RUN_SSE2 / WALL_RUN_NEON). */
#if defined(__SSE2__)
#define WALL_RUN_SSE2 1
#include <emmintrin.h>
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
#define WALL_RUN_NEON 1
#include <arm_neon.h>
#endif

/* V_PaletteTC / VID_PALTC come from v_video.h (built in v_video.c). */

/* Shared globals owned by r_draw.c. */
extern uint8_t *translationtables;

#ifdef PRBOOM_RENDER_PROFILE
/* Wall-fill accumulator, defined in r_draw.c: both renderers report into the
 * same counter so a profile build reads the same way whichever is active. */
extern double prof_wallfill_usec;
#endif

/* ---- filtered-tap macros (TC edition) -----------------------------------
 * Identical structure to filter_getFilteredForColumn16 / ...ForSpan16 in
 * r_filter.h, but summing VID_PALTC (32-bit premultiplied) terms.  The
 * premultiply-by-weight trick still holds: the four weights sum to
 * 0xffff (one full unit), and each field of a VID_PALTC entry already
 * carries channel*weight/max with enough headroom (8/10-bit channel in a
 * 32-bit lane) that the four-term add cannot overflow a channel into its
 * neighbour -- the same property the 565 macro relies on within 16 bits. */
#define filter_getFilteredForColumnTC(depthmap, texV, nextRowTexV) ( \
  VID_PALTC( depthmap(nextsource[(nextRowTexV)>>FRACBITS]),   (filter_fracu*((texV)&0xffff))>>(32-VID_COLORWEIGHTBITS) ) + \
  VID_PALTC( depthmap(source[(nextRowTexV)>>FRACBITS]),       ((0xffff-filter_fracu)*((texV)&0xffff))>>(32-VID_COLORWEIGHTBITS) ) + \
  VID_PALTC( depthmap(source[(texV)>>FRACBITS]),              ((0xffff-filter_fracu)*(0xffff-((texV)&0xffff)))>>(32-VID_COLORWEIGHTBITS) ) + \
  VID_PALTC( depthmap(nextsource[(texV)>>FRACBITS]),          (filter_fracu*(0xffff-((texV)&0xffff)))>>(32-VID_COLORWEIGHTBITS) ))

#define filter_getFilteredForSpanTC(depthmap, texU, texV) ( \
  VID_PALTC( depthmap(source[ ((((texU)+FRACUNIT)>>16)&0x3f) | ((((texV)+FRACUNIT)>>10)&0xfc0)]),  (unsigned int)(((texU)&0xffff)*((texV)&0xffff))>>(32-VID_COLORWEIGHTBITS)) + \
  VID_PALTC( depthmap(source[ (((texU)>>16)&0x3f) | ((((texV)+FRACUNIT)>>10)&0xfc0)]),             (unsigned int)((0xffff-((texU)&0xffff))*((texV)&0xffff))>>(32-VID_COLORWEIGHTBITS)) + \
  VID_PALTC( depthmap(source[ (((texU)>>16)&0x3f) | (((texV)>>10)&0xfc0)]),                        (unsigned int)((0xffff-((texU)&0xffff))*(0xffff-((texV)&0xffff)))>>(32-VID_COLORWEIGHTBITS)) + \
  VID_PALTC( depthmap(source[ ((((texU)+FRACUNIT)>>16)&0x3f) | (((texV)>>10)&0xfc0)]),             (unsigned int)(((texU)&0xffff)*(0xffff-((texV)&0xffff)))>>(32-VID_COLORWEIGHTBITS)))

/* ---- column-type tags (mirror r_draw.c's columntype_e) ------------------- */
/* Defined here (ahead of the .inl and the body, both of which reference the
 * COL_* tags); the body's own copy is stripped by the assembler. */
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

/* ---- batch/blit state (referenced by the .inl and the body) -------------- */
static int      tc_temp_x = 0;
static int      tc_tempyl[4], tc_tempyh[4];
static uint32_t tc_tempbuf[MAX_SCREENHEIGHT * 4];
static int      tc_startx = 0;
static int      tc_temptype = 0;            /* COL_NONE */
static int      tc_commontop, tc_commonbot;
static const uint8_t *tc_tempfuzzmap;
static int      tc_fuzzoffset[50];          /* TC_FUZZTABLE */
static int      tc_fuzzpos = 0;

/* Raven translucent-sprite state.  The setters below vid_mode-dispatch the
 * flush-pointer trio to the correct per-format kernels. */
static int  tc_tl_alpha    = 16;
static int  tc_tl_temptype = COL_OPAQUE;    /* re-seeded by the setters below */

/* Forward decls of the per-format flushers the .inl defines (name-mangled). */
static void R_FlushWholeTL8888(void);   static void R_FlushHTTL8888(void);   static void R_FlushQuadTL8888(void);
static void R_FlushWholeADD8888(void);  static void R_FlushHTADD8888(void);  static void R_FlushQuadADD8888(void);
static void R_FlushWholeLERP8888(void); static void R_FlushHTLERP8888(void); static void R_FlushQuadLERP8888(void);
static void R_FlushWholeFuzz8888(void); static void R_FlushHTFuzz8888(void); static void R_FlushQuadFuzz8888(void);
static void R_FlushWholeTLA2(void);     static void R_FlushHTTLA2(void);     static void R_FlushQuadTLA2(void);
static void R_FlushWholeADDA2(void);    static void R_FlushHTADDA2(void);    static void R_FlushQuadADDA2(void);
static void R_FlushWholeLERPA2(void);   static void R_FlushHTLERPA2(void);   static void R_FlushQuadLERPA2(void);
static void R_FlushWholeFuzzA2(void);   static void R_FlushHTFuzzA2(void);   static void R_FlushQuadFuzzA2(void);

/* Opaque flushers are format-independent (plain copy of packed pixels);
 * they are defined in the body (region 563-724) under these names. */
static void R_FlushWholeTC(void);
static void R_FlushHTTC(void);
static void R_FlushQuadTC(void);

/* The flush-pointer trio the batching drawers install (points at whichever
 * translucency kernels the current sprite/line selected). */
static void (*tc_tl_flush_whole)(void) = R_FlushWholeTC;
static void (*tc_tl_flush_ht)(void)    = R_FlushHTTC;
static void (*tc_tl_flush_quad)(void)  = R_FlushQuadTC;

/* ---- fuzz table dimensions (mirror r_draw.c) ----------------------------- */
#define TC_FUZZTABLE 50
#define TC_FUZZOFF   1

/* ---- water volume LUTs (built lazily; shared by both formats) ------------ */
#define TC_MAXWATERDEPTH 4096
static unsigned char  tc_water_keep_lut[TC_MAXWATERDEPTH];
/* Additive blue lift, in the ACTIVE format's channel units (up to 10 bits,
 * so this is wider than the 16-bit renderer's byte table). */
static unsigned short tc_water_bl_lut[TC_MAXWATERDEPTH];
static int           tc_water_lut_ready = 0;
static const signed char tc_water_ripple[8] = { 0, 2, 3, 2, 0, -2, -3, -2 };

static void R_BuildWaterLUTTC(void)
{
   /* Same curve as the 16-bit build, but evaluated against the output's own
    * blue maximum instead of 31.  Scaling a 5-bit curve up afterwards would
    * leave the surface falloff stepping in 1/31 increments (multiples of 8
    * at 8bpc, 33 at 10bpc) -- visible banding on a large, smooth water
    * volume, which is exactly what truecolor is here to avoid.  `keep` is a
    * multiplicative /32 factor, so it needs no rescale: the product carries
    * the destination's full precision already. */
   const int mx = (vid_mode == VID_MODE2101010) ? 1023 : 255;
   const int lo = (2 * mx) / 31;                 /* deep: faint constant blue */
   int depth;
   for (depth = 0; depth < TC_MAXWATERDEPTH; depth++)
   {
      int keep = 26 - (depth / 3);
      int bl;
      if (keep < 11) keep = 11;
      if (depth < 6)        bl = (16 * mx) / 31;                 /* lit surface line */
      else if (depth < 56)  bl = ((16 * mx) / 31)
                                 - (((depth - 6) * mx) / (4 * 31));
      else                  bl = lo;
      if (bl < lo) bl = lo;
      tc_water_keep_lut[depth] = (unsigned char)keep;
      tc_water_bl_lut[depth]   = (unsigned short)bl;
   }
   tc_water_lut_ready = 1;
}

/* Forward decls of composed-LUT accessors (defined in the body) so the .inl
 * water/span kernels can call them. */
static INLINE const uint32_t *R_GetComposedColormapTC(const lighttable_t *colormap);
static INLINE const uint32_t *R_GetComposedPaletteTC(void);

/* R_TintLUTTC is exported from the postamble but the wall-run kernel in the
 * body calls it; declare it up front. */
void R_TintLUTTC(uint32_t *dst, const uint32_t *src, int ar, int ag, int ab);
/* Shared 565-side wall-tint recorder (r_draw.c); the wall-run kernel records
 * overflow tints into the same buffer, replayed by R_WallTintReplay. */
void R_WallTintRecord(int x, int yl, int yh, int ar, int ag, int ab);

/* =========================================================================
 *  Per-format blend kernels: include the template twice.
 * ========================================================================= */

#define RDF(name)   name##8888
#define RDF_RSHIFT  16
#define RDF_GSHIFT  8
#define RDF_CMAX    255
#define RDF_M5050   0x00FEFEFEu
#include "r_drawtcfmt.inl"
#undef RDF
#undef RDF_RSHIFT
#undef RDF_GSHIFT
#undef RDF_CMAX
#undef RDF_M5050

#define RDF(name)   name##A2
#define RDF_RSHIFT  20
#define RDF_GSHIFT  10
#define RDF_CMAX    1023
#define RDF_M5050   0x3FEFFBFEu
#include "r_drawtcfmt.inl"
#undef RDF
#undef RDF_RSHIFT
#undef RDF_GSHIFT
#undef RDF_CMAX
#undef RDF_M5050

/* =========================================================================
 *  Dispatch thunks: pick the per-format kernel from vid_mode.
 * ========================================================================= */

#define TC_THUNK(name) \
  static void name(void) { \
    if (vid_mode == VID_MODE2101010) name##A2(); else name##8888(); }

TC_THUNK(R_FlushWholeTL)   TC_THUNK(R_FlushHTTL)   TC_THUNK(R_FlushQuadTL)
TC_THUNK(R_FlushWholeADD)  TC_THUNK(R_FlushHTADD)  TC_THUNK(R_FlushQuadADD)
TC_THUNK(R_FlushWholeLERP) TC_THUNK(R_FlushHTLERP) TC_THUNK(R_FlushQuadLERP)
TC_THUNK(R_FlushWholeFuzz) TC_THUNK(R_FlushHTFuzz) TC_THUNK(R_FlushQuadFuzz)
#undef TC_THUNK

/* Translucent 3D-floor span: R_DrawSpanTL8888 / ...A2 come from the .inl.
 * The body's R_DrawSpanTC references this name. */
static void R_DrawSpanTC_TL(draw_span_vars_t *dsvars)
{
  if (vid_mode == VID_MODE2101010) R_DrawSpanTLA2(dsvars);
  else                             R_DrawSpanTL8888(dsvars);
}

/* Bare names the transformed body references for the fuzz flush pointers
 * (the fuzz column drawers install these directly, matching r_draw.c). */
#define R_FlushWholeFuzzTC R_FlushWholeFuzz
#define R_FlushHTFuzzTC    R_FlushHTFuzz
#define R_FlushQuadFuzzTC  R_FlushQuadFuzz

/* ---- Raven translucency setters (public, vid_mode-aware) ----------------- */

void R_SetTransAlphaTC(int a32)
{
  tc_tl_alpha = a32 < 0 ? 0 : (a32 > 32 ? 32 : a32);
}

void R_SetSpriteTranslucencyTC(int mode)
{
  if (mode == 4)
  {
    tc_tl_temptype = COL_FLEXTRANS;
    tc_tl_flush_whole = R_FlushWholeLERP;
    tc_tl_flush_ht    = R_FlushHTLERP;
    tc_tl_flush_quad  = R_FlushQuadLERP;
  }
  else if (mode == 3)
  {
    tc_tl_temptype = COL_FLEXADD;
    tc_tl_flush_whole = R_FlushWholeADD;
    tc_tl_flush_ht    = R_FlushHTADD;
    tc_tl_flush_quad  = R_FlushQuadADD;
  }
  else if (mode)
  {
    tc_tl_temptype = (mode == 2) ? COL_ALTTRANS : COL_TRANS;
    tc_tl_flush_whole = R_FlushWholeTL;
    tc_tl_flush_ht    = R_FlushHTTL;
    tc_tl_flush_quad  = R_FlushQuadTL;
  }
  else
  {
    tc_tl_temptype = COL_OPAQUE;
    tc_tl_flush_whole = R_FlushWholeTC;
    tc_tl_flush_ht    = R_FlushHTTC;
    tc_tl_flush_quad  = R_FlushQuadTC;
  }
}

/* Seed the flush-pointer trio to the opaque state. */
static void R_InitTransStateTC(void)
{
  tc_tl_temptype = COL_OPAQUE;
  tc_tl_flush_whole = R_FlushWholeTC;
  tc_tl_flush_ht    = R_FlushHTTC;
  tc_tl_flush_quad  = R_FlushQuadTC;
}


/* ===== BEGIN GENERATED BODY (transformed from r_draw.c) ===== */

/* Shared composed colormap+palette table.
 *
 * Point-sampled drawing turns an 8-bit texel into a 16-bit pixel via two
 * dependent lookups plus index arithmetic: V_PaletteTC[colormap[texel]*64
 * + 63] (the colormap applies the per-distance light level, V_PaletteTC
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
 * Keying also on V_PaletteTC rebuilds on a palette/gamma/video-mode
 * change.  Restricted to point sampling with a single colormap: the
 * filtered (Linear/Rounded UV) and dithered (LinearZ) paths blend several
 * palette weights / two colormaps per pixel and cannot use one table. */
static const lighttable_t *tc_composed_cm  = NULL;
static const uint32_t     *tc_composed_pal = NULL;
static uint32_t            tc_composed_lut[256];

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
 * V_PaletteTC already stores every palette colour expanded across
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
 * V_PaletteTC[ texel*64 + weight ].  Distance darkening now scales luma
 * continuously, like CRY, instead of snapping the palette index.  The
 * texel's own colormap remap (translations, invuln inversion baked into
 * colormap[i]) is preserved; only the brightness selection changes. */

/* Fine (sub-band) light weight side channel for Smooth mode.
 *
 * The band-level Smooth path recovers light from the colormap pointer,
 * which the renderer has already snapped to one of NUMCOLORMAPS (32)
 * bands -- so it cannot represent light variation finer than 32 steps even
 * though V_PaletteTC carries 64 luma weights.  R_ColourMap (walls/sprites)
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
static int tc_composed_weight = -2; /* cache key companion; -2 = never built */

/* Private fine luma ramp for Smooth shading: [colour][SMOOTH_WEIGHTS].
 * Rebuilt lazily when the active palette (V_PaletteTC) changes -- gamma swap,
 * invuln flash, palette blend -- so it always matches the rest of the
 * renderer's colours.  Each colour's full-bright RGB565 is taken from
 * V_PaletteTC at its top weight, decomposed to channels, and rescaled across
 * SMOOTH_WEIGHTS steps the same way V_PaletteTC scales across its 64 -- just
 * finer.  Only the composed-LUT build reads this; the shared V_PaletteTC and
 * every per-pixel path that strides it are left exactly as they were. */
static uint32_t       tc_smooth_ramp[256 * SMOOTH_WEIGHTS];
static const uint32_t *tc_smooth_ramp_pal = NULL;

static void R_BuildSmoothRampTC(void)
{
   int i, w;
   /* V_PaletteTC field layout depends on the active format; the ramp is
    * shared code compiled once, so branch at runtime (built rarely). */
   const int rs   = (vid_mode == VID_MODE2101010) ? 20  : 16;
   const int gs   = (vid_mode == VID_MODE2101010) ? 10  : 8;
   const int cmax = (vid_mode == VID_MODE2101010) ? 1023 : 255;
   for (i = 0; i < 256; i++)
   {
      /* Full-bright pixel of this palette entry (weight 63 in V_PaletteTC),
       * decomposed by the active truecolor field layout. */
      uint32_t full = V_PaletteTC[ i*VID_NUMCOLORWEIGHTS + (VID_NUMCOLORWEIGHTS-1) ];
      int r = (int)((full >> rs) & cmax);
      int g = (int)((full >> gs) & cmax);
      int b = (int)( full        & cmax);
      uint32_t *row = &tc_smooth_ramp[i * SMOOTH_WEIGHTS];
      for (w = 0; w < SMOOTH_WEIGHTS; w++)
      {
         /* (channel * t + 0.5) rounded to nearest, in integer math. */
         int nr = (r * w + (SMOOTH_WEIGHTS-1)/2) / (SMOOTH_WEIGHTS-1);
         int ng = (g * w + (SMOOTH_WEIGHTS-1)/2) / (SMOOTH_WEIGHTS-1);
         int nb = (b * w + (SMOOTH_WEIGHTS-1)/2) / (SMOOTH_WEIGHTS-1);
         row[w] = ((uint32_t)nr << rs) | ((uint32_t)ng << gs) | (uint32_t)nb;
      }
   }
   tc_smooth_ramp_pal = V_PaletteTC;
}

static INLINE const uint32_t *R_GetComposedColormapTC(const lighttable_t *colormap)
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

   if (colormap != tc_composed_cm || V_PaletteTC != tc_composed_pal
       || want_weight != tc_composed_weight)
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

         if (tc_smooth_ramp_pal != V_PaletteTC)
            R_BuildSmoothRampTC();

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
          * shared V_PaletteTC at full weight (the point-path output).  For a
          * distance band, index the private fine ramp by the undimmed texel. */
         if (is_band)
         {
            for (i = 0; i < 256; i++)
               tc_composed_lut[i] = tc_smooth_ramp[ fullcolormap[i]*SMOOTH_WEIGHTS + weight ];
         }
         else
         {
            for (i = 0; i < 256; i++)
               tc_composed_lut[i] = V_PaletteTC[ colormap[i]*VID_NUMCOLORWEIGHTS + (VID_NUMCOLORWEIGHTS-1) ];
         }

         tc_composed_weight = want_weight;
      }
      else
      {
         for (i = 0; i < 256; i++)
            tc_composed_lut[i] = V_PaletteTC[ colormap[i]*64 + (64-1) ];
         tc_composed_weight = -2;
      }
      tc_composed_cm  = colormap;
      tc_composed_pal = V_PaletteTC;
   }
   return tc_composed_lut;
}

/* No-colormap variant for the patch/HUD/UI point column
 * (R_DrawColumnTC_PointUV): that path has no colormap (full-bright 2D
 * blitting), so the per-pixel expression is just V_PaletteTC[texel*64+63].
 * That is still a fixed 8bpp->16bpp function of the texel, composable into
 * one table keyed solely on V_PaletteTC (rebuilt on a palette/gamma/video-
 * mode change).  Speeds HUD/status-bar/menu blits and the title/inter-
 * mission background-cache builds, which all run through this column. */
static const uint32_t *tc_composed_nolight_pal = NULL;
static uint32_t        tc_composed_nolight_lut[256];

static INLINE const uint32_t *R_GetComposedPaletteTC(void)
{
   if (V_PaletteTC != tc_composed_nolight_pal)
   {
      int i;
      for (i = 0; i < 256; i++)
         tc_composed_nolight_lut[i] = V_PaletteTC[ i*64 + (64-1) ];
      tc_composed_nolight_pal = V_PaletteTC;
   }
   return tc_composed_nolight_lut;
}



// SoM 7-28-04: Fix the fuzz problem.

//
// Spectre/Invisibility.
//

#define TC_FUZZTABLE 50
#define TC_FUZZOFF 1




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

//
// Error functions that will abort if R_FlushColumnsTC tries to flush
// columns without a column type.
//

static void R_FlushWholeErrorTC(void)
{
   I_Error("R_FlushWholeColumnsTC called without being initialized.\n");
}

static void R_FlushHTErrorTC(void)
{
   I_Error("R_FlushHTColumnsTC called without being initialized.\n");
}

static void R_QuadFlushErrorTC(void)
{
   I_Error("R_FlushQuadColumnTC called without being initialized.\n");
}

static void (*R_FlushWholeColumnsTC)(void) = R_FlushWholeErrorTC;
static void (*R_FlushHTColumnsTC)(void)    = R_FlushHTErrorTC;
static void (*R_FlushQuadColumnTC)(void) = R_QuadFlushErrorTC;

static void R_FlushColumnsTC(void)
{
#ifdef PRBOOM_RENDER_PROFILE
   double _t0 = I_RenderProfileUsec();
#endif
   if(tc_temp_x != 4 || tc_commontop >= tc_commonbot)
      R_FlushWholeColumnsTC();
   else
   {
      R_FlushHTColumnsTC();
      R_FlushQuadColumnTC();
   }
   tc_temp_x = 0;
#ifdef PRBOOM_RENDER_PROFILE
   prof_wallfill_usec += (I_RenderProfileUsec() - _t0);
#endif
}

//
// R_ResetColumnBufferTC
//
// haleyjd 09/13/04: new function to call from main rendering loop
// which gets rid of the unnecessary reset of various variables during
// column drawing.
//
void R_ResetColumnBufferTC(void)
{
   // haleyjd 10/06/05: this must not be done if tc_temp_x == 0!
   if(tc_temp_x)
      R_FlushColumnsTC();

   tc_temptype            = COL_NONE;
   R_FlushWholeColumnsTC = R_FlushWholeErrorTC;
   R_FlushHTColumnsTC    = R_FlushHTErrorTC;
   R_FlushQuadColumnTC   = R_QuadFlushErrorTC;
}

/*
 * R_FlushWholeTC
 *
 * Flushes the entire columns in the buffer, one at a time.
 * This is used when a quad flush isn't possible.
 * Opaque version -- no remapping whatsoever.
*/
static void R_FlushWholeTC(void)
{
   while(--tc_temp_x >= 0)
   {
      int yl           = tc_tempyl[tc_temp_x];
      uint32_t *source = &tc_tempbuf[tc_temp_x + (yl << 2)];
      uint32_t *dest   = ((uint32_t *)drawvars.int_topleft) + yl * SURFACE_SHORT_PITCH + tc_startx + tc_temp_x;
      int   count      = tc_tempyh[tc_temp_x] - yl + 1;

      while(--count >= 0)
      {
         *dest   = *source;
         source += 4;
         dest   += SURFACE_SHORT_PITCH;
      }
   }
}

//
// R_FlushHTTC
//
// Flushes the head and tail of columns in the buffer in
// preparation for a quad flush.
// Opaque version -- no remapping whatsoever.
//
static void R_FlushHTTC(void)
{
   uint32_t *source;
   uint32_t *dest;
   int count, colnum = 0;
   int yl, yh;

   while(colnum < 4)
   {
      yl = tc_tempyl[colnum];
      yh = tc_tempyh[colnum];

      // flush column head
      if(yl < tc_commontop)
      {
         source = &tc_tempbuf[colnum + (yl << 2)];
         dest   = ((uint32_t *)drawvars.int_topleft) + yl * SURFACE_SHORT_PITCH + tc_startx + colnum;
         count  = tc_commontop - yl;

         while(--count >= 0)
         {
            *dest = *source;
            source += 4;
            dest += SURFACE_SHORT_PITCH;
         }
      }

      // flush column tail
      if(yh > tc_commonbot)
      {
         source = &tc_tempbuf[colnum + ((tc_commonbot + 1) << 2)];
         dest   = ((uint32_t *)drawvars.int_topleft) + (tc_commonbot + 1) * SURFACE_SHORT_PITCH + tc_startx + colnum;
         count  = yh - tc_commonbot;

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

static void R_FlushQuadTC(void)
{
   uint32_t *source = &tc_tempbuf[tc_commontop << 2];
   uint32_t *dest   = ((uint32_t *)drawvars.int_topleft) + tc_commontop * SURFACE_SHORT_PITCH + tc_startx;
   int        count = tc_commonbot - tc_commontop + 1;

   /* Each row copies the 4 transposed columns, which are 4 contiguous
    * uint32_t in the source (the transpose buffer is 4-wide) to 4 adjacent
    * pixels in the destination -- i.e. exactly 8 contiguous bytes from a
    * contiguous source.  The original wrote them as four separate 16-bit
    * stores.  Collapse to a single 8-byte move: memcpy(,,8) lowers to one
    * unaligned movq/str on every target the core builds for, makes no
    * alignment assumption (dest = topleft + tc_startx is only 2-byte aligned),
    * and is endian-agnostic (a byte copy, so bit-identical on LE and BE).
    * The whole quad-column path is the common wall-fill case, so this is the
    * pixel-write hot loop. */
   while(--count >= 0)
   {
      memcpy(dest, source, 4 * sizeof(uint32_t));
      source += 4;
      dest += SURFACE_SHORT_PITCH;
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


// no color mapping
static void R_DrawColumnTC_PointUV(draw_column_vars_t *dcvars)
{
   int count;

   uint32_t *dest;

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

   if(tc_temp_x == 4 ||
         (tc_temp_x && (tc_temptype != tc_tl_temptype || tc_temp_x + tc_startx != dcvars->x)))
      R_FlushColumnsTC();

   if(!tc_temp_x)
   {
      tc_startx = dcvars->x;
      tc_tempyl[0] = tc_commontop = dcvars->yl;
      tc_tempyh[0] = tc_commonbot = dcvars->yh;
      tc_temptype = tc_tl_temptype;





      R_FlushWholeColumnsTC = tc_tl_flush_whole;
      R_FlushHTColumnsTC = tc_tl_flush_ht;
      R_FlushQuadColumnTC = tc_tl_flush_quad;

      dest = &tc_tempbuf[dcvars->yl << 2];

   }
   else
   {
      tc_tempyl[tc_temp_x] = dcvars->yl;
      tc_tempyh[tc_temp_x] = dcvars->yh;

      if(dcvars->yl > tc_commontop)
         tc_commontop = dcvars->yl;
      if(dcvars->yh < tc_commonbot)
         tc_commonbot = dcvars->yh;


      dest = &tc_tempbuf[(dcvars->yl << 2) + tc_temp_x];

   }
   tc_temp_x += 1;

   {
      const uint8_t *source = dcvars->source;
      /* No colormap on this path: composed palette table collapses
       * V_PaletteTC[texel*64+63] to lut[texel] (see R_GetComposedPaletteTC). */
      const uint32_t *lut = R_GetComposedPaletteTC();
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
static void R_DrawColumnTC_PointUV_PointZ(draw_column_vars_t *dcvars)
{
   uint32_t *dest;

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

   if(tc_temp_x == 4 ||
         (tc_temp_x && (tc_temptype != tc_tl_temptype || tc_temp_x + tc_startx != dcvars->x)))
      R_FlushColumnsTC();

   if(!tc_temp_x)
   {
      tc_startx = dcvars->x;
      tc_tempyl[0] = tc_commontop = dcvars->yl;
      tc_tempyh[0] = tc_commonbot = dcvars->yh;
      tc_temptype = tc_tl_temptype;





      R_FlushWholeColumnsTC = tc_tl_flush_whole;
      R_FlushHTColumnsTC = tc_tl_flush_ht;
      R_FlushQuadColumnTC = tc_tl_flush_quad;

      dest = &tc_tempbuf[dcvars->yl << 2];

   }
   else
   {
      tc_tempyl[tc_temp_x] = dcvars->yl;
      tc_tempyh[tc_temp_x] = dcvars->yh;

      if(dcvars->yl > tc_commontop)
         tc_commontop = dcvars->yl;
      if(dcvars->yh < tc_commonbot)
         tc_commonbot = dcvars->yh;


      dest = &tc_tempbuf[(dcvars->yl << 2) + tc_temp_x];

   }
   tc_temp_x += 1;



   {
      const uint8_t *source = dcvars->source;
      /* Shared composed colormap+palette table: collapses
       * V_PaletteTC[colormap[texel]*64+63] to lut[texel].  Reuses the
       * table the spans (and other lit columns) already built for this
       * colormap pointer, so there is no per-column rebuild cost. */
      const uint32_t *lut = R_GetComposedColormapTC(dcvars->colormap);
      count++;

      /* Brightmap path: where the per-texel mask is set the texel ignores
       * the distance light and is drawn through the undimmed base map
       * (fullcolormap, band 0).  fullcolormap's composed table is snapshot
       * into a column-local array first, because the shared tc_composed_lut
       * cache holds a single entry -- fetching the distance `lut` would
       * otherwise evict it.  Kept as one general loop (handles every
       * texheight); the SIMD select lands in a later step. */
      if (dcvars->brightmask)
      {
         const uint8_t *mask = dcvars->brightmask;
         uint32_t lut_bright[256];
         const uint32_t *bsrc = R_GetComposedColormapTC(fullcolormap
                                                      ? fullcolormap
                                                      : dcvars->colormap);
         unsigned heightmask = dcvars->texheight ? dcvars->texheight - 1 : 0;
         int npot = (dcvars->texheight &&
                     (dcvars->texheight & heightmask)) ? 1 : 0;
         memcpy(lut_bright, bsrc, sizeof(lut_bright));
         /* re-fetch the distance table: the snapshot above may have
          * replaced it in the shared cache */
         lut = R_GetComposedColormapTC(dcvars->colormap);

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
static void R_DrawColumnTC_PointUV_LinearZ(draw_column_vars_t *dcvars)
{
   uint32_t *dest;

   fixed_t frac;
   const fixed_t fracstep = dcvars->iscale;
   const fixed_t slope_texu = dcvars->texu;
   int count = dcvars->yh - dcvars->yl;

   /* Brightmapped columns take the point/point drawer, the only one
    * carrying the fullbright select.  The filtered/dithered variants blend
    * several colormap lookups per pixel with no single table to redirect,
    * so a masked surface trades that smoothing for correct fullbright --
    * the same trade R_DrawColumnTC_LinearUV already makes when minified. */
   if (dcvars->brightmask)
   {
      R_DrawColumnTC_PointUV_PointZ(dcvars);
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

      if(tc_temp_x == 4 ||
            (tc_temp_x && (tc_temptype != tc_tl_temptype || tc_temp_x + tc_startx != dcvars->x)))
         R_FlushColumnsTC();

      if(!tc_temp_x)
      {
         tc_startx = dcvars->x;
         tc_tempyl[0] = tc_commontop = dcvars->yl;
         tc_tempyh[0] = tc_commonbot = dcvars->yh;
         tc_temptype = tc_tl_temptype;





         R_FlushWholeColumnsTC = tc_tl_flush_whole;
         R_FlushHTColumnsTC = tc_tl_flush_ht;
         R_FlushQuadColumnTC = tc_tl_flush_quad;

         dest = &tc_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tc_tempyl[tc_temp_x] = dcvars->yl;
         tc_tempyh[tc_temp_x] = dcvars->yh;

         if(dcvars->yl > tc_commontop)
            tc_commontop = dcvars->yl;
         if(dcvars->yh < tc_commonbot)
            tc_commonbot = dcvars->yh;


         dest = &tc_tempbuf[(dcvars->yl << 2) + tc_temp_x];

      }
      tc_temp_x += 1;

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
            *dest = (V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(frac & ((127<<16)|0xffff))>>16])]))*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(frac)>>16])]))*64 + ((64 -1)) ]);
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
               *dest = (V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(frac & fixedt_heightmask)>>16])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(frac & fixedt_heightmask)>>16])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(frac & fixedt_heightmask)>>16])]))*64 + ((64 -1)) ]);
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





               *dest = (V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(frac)>>16])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;



            }
         }
      }
   }

}

// bilinear with no color mapping
static void R_DrawColumnTC_LinearUV(draw_column_vars_t *dcvars)
{
   int count;

   uint32_t *dest;

   fixed_t frac;
   const fixed_t fracstep = dcvars->iscale;

   const fixed_t slope_texu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;

   if (dcvars->brightmask)
   {
      R_DrawColumnTC_PointUV_PointZ(dcvars);
      return;
   }

   if (dcvars->iscale > drawvars.mag_threshold)
   {
      R_GetDrawColumnFuncTC(RDC_PIPELINE_STANDARD,
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


   if(tc_temp_x == 4 ||
         (tc_temp_x && (tc_temptype != tc_tl_temptype || tc_temp_x + tc_startx != dcvars->x)))
      R_FlushColumnsTC();

   if(!tc_temp_x)
   {
      tc_startx = dcvars->x;
      tc_tempyl[0] = tc_commontop = dcvars->yl;
      tc_tempyh[0] = tc_commonbot = dcvars->yh;
      tc_temptype = tc_tl_temptype;





      R_FlushWholeColumnsTC = tc_tl_flush_whole;
      R_FlushHTColumnsTC = tc_tl_flush_ht;
      R_FlushQuadColumnTC = tc_tl_flush_quad;

      dest = &tc_tempbuf[dcvars->yl << 2];

   }
   else
   {
      tc_tempyl[tc_temp_x] = dcvars->yl;
      tc_tempyh[tc_temp_x] = dcvars->yh;

      if(dcvars->yl > tc_commontop)
         tc_commontop = dcvars->yl;
      if(dcvars->yh < tc_commonbot)
         tc_commonbot = dcvars->yh;


      dest = &tc_tempbuf[(dcvars->yl << 2) + tc_temp_x];

   }
   tc_temp_x += 1;



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
            *dest = (( V_PaletteTC[ ((nextsource[((frac+(1<<16)) & ((127<<16)|0xffff))>>16]))*64 + ((filter_fracu*((frac & ((127<<16)|0xffff))&0xffff))>>(32-6)) ] + V_PaletteTC[ ((source[((frac+(1<<16)) & ((127<<16)|0xffff))>>16]))*64 + (((0xffff-filter_fracu)*((frac & ((127<<16)|0xffff))&0xffff))>>(32-6)) ] + V_PaletteTC[ ((source[(frac & ((127<<16)|0xffff))>>16]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & ((127<<16)|0xffff))&0xffff)))>>(32-6)) ] + V_PaletteTC[ ((nextsource[(frac & ((127<<16)|0xffff))>>16]))*64 + ((filter_fracu*(0xffff-((frac & ((127<<16)|0xffff))&0xffff)))>>(32-6)) ]));
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (( V_PaletteTC[ ((nextsource[((frac+(1<<16)))>>16]))*64 + ((filter_fracu*((frac)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((source[((frac+(1<<16)))>>16]))*64 + (((0xffff-filter_fracu)*((frac)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((source[(frac)>>16]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac)&0xffff)))>>(32-6)) ] + V_PaletteTC[ ((nextsource[(frac)>>16]))*64 + ((filter_fracu*(0xffff-((frac)&0xffff)))>>(32-6)) ]));
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
               *dest = (( V_PaletteTC[ ((nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16]))*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((source[((frac+(1<<16)) & fixedt_heightmask)>>16]))*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((source[(frac & fixedt_heightmask)>>16]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_PaletteTC[ ((nextsource[(frac & fixedt_heightmask)>>16]))*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (( V_PaletteTC[ ((nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16]))*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((source[((frac+(1<<16)) & fixedt_heightmask)>>16]))*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((source[(frac & fixedt_heightmask)>>16]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_PaletteTC[ ((nextsource[(frac & fixedt_heightmask)>>16]))*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (( V_PaletteTC[ ((nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16]))*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((source[((frac+(1<<16)) & fixedt_heightmask)>>16]))*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((source[(frac & fixedt_heightmask)>>16]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_PaletteTC[ ((nextsource[(frac & fixedt_heightmask)>>16]))*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
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





               *dest = (( V_PaletteTC[ ((nextsource[(nextfrac)>>16]))*64 + ((filter_fracu*((frac)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((source[(nextfrac)>>16]))*64 + (((0xffff-filter_fracu)*((frac)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((source[(frac)>>16]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac)&0xffff)))>>(32-6)) ] + V_PaletteTC[ ((nextsource[(frac)>>16]))*64 + ((filter_fracu*(0xffff-((frac)&0xffff)))>>(32-6)) ]));
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
static void R_DrawColumnTC_LinearUV_PointZ(draw_column_vars_t *dcvars)
{
   int count;

   uint32_t *dest;

   fixed_t frac;
   const fixed_t fracstep = dcvars->iscale;

   const fixed_t slope_texu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;

   if (dcvars->brightmask)
   {
      R_DrawColumnTC_PointUV_PointZ(dcvars);
      return;
   }

   if (dcvars->iscale > drawvars.mag_threshold)
   {
      R_GetDrawColumnFuncTC(RDC_PIPELINE_STANDARD,
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

   if(tc_temp_x == 4 ||
         (tc_temp_x && (tc_temptype != tc_tl_temptype || tc_temp_x + tc_startx != dcvars->x)))
      R_FlushColumnsTC();

   if(!tc_temp_x)
   {
      tc_startx = dcvars->x;
      tc_tempyl[0] = tc_commontop = dcvars->yl;
      tc_tempyh[0] = tc_commonbot = dcvars->yh;
      tc_temptype = tc_tl_temptype;





      R_FlushWholeColumnsTC = tc_tl_flush_whole;
      R_FlushHTColumnsTC = tc_tl_flush_ht;
      R_FlushQuadColumnTC = tc_tl_flush_quad;

      dest = &tc_tempbuf[dcvars->yl << 2];

   }
   else
   {
      tc_tempyl[tc_temp_x] = dcvars->yl;
      tc_tempyh[tc_temp_x] = dcvars->yh;

      if(dcvars->yl > tc_commontop)
         tc_commontop = dcvars->yl;
      if(dcvars->yh < tc_commonbot)
         tc_commonbot = dcvars->yh;


      dest = &tc_tempbuf[(dcvars->yl << 2) + tc_temp_x];

   }
   tc_temp_x += 1;



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
            *dest = (( V_PaletteTC[ (colormap[(nextsource[((frac+(1<<16)) & ((127<<16)|0xffff))>>16])])*64 + ((filter_fracu*((frac & ((127<<16)|0xffff))&0xffff))>>(32-6)) ] + V_PaletteTC[ (colormap[(source[((frac+(1<<16)) & ((127<<16)|0xffff))>>16])])*64 + (((0xffff-filter_fracu)*((frac & ((127<<16)|0xffff))&0xffff))>>(32-6)) ] + V_PaletteTC[ (colormap[(source[(frac & ((127<<16)|0xffff))>>16])])*64 + (((0xffff-filter_fracu)*(0xffff-((frac & ((127<<16)|0xffff))&0xffff)))>>(32-6)) ] + V_PaletteTC[ (colormap[(nextsource[(frac & ((127<<16)|0xffff))>>16])])*64 + ((filter_fracu*(0xffff-((frac & ((127<<16)|0xffff))&0xffff)))>>(32-6)) ]));
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (( V_PaletteTC[ (colormap[(nextsource[((frac+(1<<16)))>>16])])*64 + ((filter_fracu*((frac)&0xffff))>>(32-6)) ] + V_PaletteTC[ (colormap[(source[((frac+(1<<16)))>>16])])*64 + (((0xffff-filter_fracu)*((frac)&0xffff))>>(32-6)) ] + V_PaletteTC[ (colormap[(source[(frac)>>16])])*64 + (((0xffff-filter_fracu)*(0xffff-((frac)&0xffff)))>>(32-6)) ] + V_PaletteTC[ (colormap[(nextsource[(frac)>>16])])*64 + ((filter_fracu*(0xffff-((frac)&0xffff)))>>(32-6)) ]));
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
               *dest = (( V_PaletteTC[ (colormap[(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])])*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_PaletteTC[ (colormap[(source[((frac+(1<<16)) & fixedt_heightmask)>>16])])*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_PaletteTC[ (colormap[(source[(frac & fixedt_heightmask)>>16])])*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_PaletteTC[ (colormap[(nextsource[(frac & fixedt_heightmask)>>16])])*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (( V_PaletteTC[ (colormap[(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])])*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_PaletteTC[ (colormap[(source[((frac+(1<<16)) & fixedt_heightmask)>>16])])*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_PaletteTC[ (colormap[(source[(frac & fixedt_heightmask)>>16])])*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_PaletteTC[ (colormap[(nextsource[(frac & fixedt_heightmask)>>16])])*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (( V_PaletteTC[ (colormap[(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])])*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_PaletteTC[ (colormap[(source[((frac+(1<<16)) & fixedt_heightmask)>>16])])*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_PaletteTC[ (colormap[(source[(frac & fixedt_heightmask)>>16])])*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_PaletteTC[ (colormap[(nextsource[(frac & fixedt_heightmask)>>16])])*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
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
               *dest = (( V_PaletteTC[ (colormap[(nextsource[(nextfrac)>>16])])*64 + ((filter_fracu*((frac)&0xffff))>>(32-6)) ] + V_PaletteTC[ (colormap[(source[(nextfrac)>>16])])*64 + (((0xffff-filter_fracu)*((frac)&0xffff))>>(32-6)) ] + V_PaletteTC[ (colormap[(source[(frac)>>16])])*64 + (((0xffff-filter_fracu)*(0xffff-((frac)&0xffff)))>>(32-6)) ] + V_PaletteTC[ (colormap[(nextsource[(frac)>>16])])*64 + ((filter_fracu*(0xffff-((frac)&0xffff)))>>(32-6)) ]));
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
static void R_DrawColumnTC_LinearUV_LinearZ(draw_column_vars_t *dcvars)
{
   int count;

   uint32_t *dest;

   fixed_t frac;
   const fixed_t fracstep = dcvars->iscale;

   const fixed_t slope_texu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;

   if (dcvars->brightmask)
   {
      R_DrawColumnTC_PointUV_PointZ(dcvars);
      return;
   }

   if (dcvars->iscale > drawvars.mag_threshold)
   {
      R_GetDrawColumnFuncTC(RDC_PIPELINE_STANDARD,
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

   if(tc_temp_x == 4 ||
         (tc_temp_x && (tc_temptype != tc_tl_temptype || tc_temp_x + tc_startx != dcvars->x)))
      R_FlushColumnsTC();

   if(!tc_temp_x)
   {
      tc_startx = dcvars->x;
      tc_tempyl[0] = tc_commontop = dcvars->yl;
      tc_tempyh[0] = tc_commonbot = dcvars->yh;
      tc_temptype = tc_tl_temptype;





      R_FlushWholeColumnsTC = tc_tl_flush_whole;
      R_FlushHTColumnsTC = tc_tl_flush_ht;
      R_FlushQuadColumnTC = tc_tl_flush_quad;

      dest = &tc_tempbuf[dcvars->yl << 2];

   }
   else
   {
      tc_tempyl[tc_temp_x] = dcvars->yl;
      tc_tempyh[tc_temp_x] = dcvars->yh;

      if(dcvars->yl > tc_commontop)
         tc_commontop = dcvars->yl;
      if(dcvars->yh < tc_commonbot)
         tc_commonbot = dcvars->yh;


      dest = &tc_tempbuf[(dcvars->yl << 2) + tc_temp_x];

   }
   tc_temp_x += 1;



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
            *dest = (( V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(nextsource[((frac+(1<<16)) & ((127<<16)|0xffff))>>16])]))*64 + ((filter_fracu*((frac & ((127<<16)|0xffff))&0xffff))>>(32-6)) ] + V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[((frac+(1<<16)) & ((127<<16)|0xffff))>>16])]))*64 + (((0xffff-filter_fracu)*((frac & ((127<<16)|0xffff))&0xffff))>>(32-6)) ] + V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(frac & ((127<<16)|0xffff))>>16])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & ((127<<16)|0xffff))&0xffff)))>>(32-6)) ] + V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(nextsource[(frac & ((127<<16)|0xffff))>>16])]))*64 + ((filter_fracu*(0xffff-((frac & ((127<<16)|0xffff))&0xffff)))>>(32-6)) ]));
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (( V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(nextsource[((frac+(1<<16)))>>16])]))*64 + ((filter_fracu*((frac)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[((frac+(1<<16)))>>16])]))*64 + (((0xffff-filter_fracu)*((frac)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(frac)>>16])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac)&0xffff)))>>(32-6)) ] + V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(nextsource[(frac)>>16])]))*64 + ((filter_fracu*(0xffff-((frac)&0xffff)))>>(32-6)) ]));
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
               *dest = (( V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])]))*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[((frac+(1<<16)) & fixedt_heightmask)>>16])]))*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(frac & fixedt_heightmask)>>16])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(nextsource[(frac & fixedt_heightmask)>>16])]))*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (( V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])]))*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[((frac+(1<<16)) & fixedt_heightmask)>>16])]))*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(frac & fixedt_heightmask)>>16])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(nextsource[(frac & fixedt_heightmask)>>16])]))*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (( V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])]))*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[((frac+(1<<16)) & fixedt_heightmask)>>16])]))*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(frac & fixedt_heightmask)>>16])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(nextsource[(frac & fixedt_heightmask)>>16])]))*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
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





               *dest = (( V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(nextsource[(nextfrac)>>16])]))*64 + ((filter_fracu*((frac)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(nextfrac)>>16])]))*64 + (((0xffff-filter_fracu)*((frac)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(frac)>>16])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac)&0xffff)))>>(32-6)) ] + V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(nextsource[(frac)>>16])]))*64 + ((filter_fracu*(0xffff-((frac)&0xffff)))>>(32-6)) ]));
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
static void R_DrawColumnTC_RoundedUV(draw_column_vars_t *dcvars)
{
  int count;

  uint32_t *dest;

  fixed_t frac;
  const fixed_t fracstep = dcvars->iscale;
  const fixed_t slope_texu = dcvars->texu;

  if (dcvars->brightmask)
  {
     R_DrawColumnTC_PointUV_PointZ(dcvars);
     return;
  }

  if (dcvars->iscale > drawvars.mag_threshold)
  {
    R_GetDrawColumnFuncTC(RDC_PIPELINE_STANDARD,
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

      if(tc_temp_x == 4 ||
         (tc_temp_x && (tc_temptype != tc_tl_temptype || tc_temp_x + tc_startx != dcvars->x)))
         R_FlushColumnsTC();

      if(!tc_temp_x)
      {
         tc_startx = dcvars->x;
         tc_tempyl[0] = tc_commontop = dcvars->yl;
         tc_tempyh[0] = tc_commonbot = dcvars->yh;
         tc_temptype = tc_tl_temptype;





         R_FlushWholeColumnsTC = tc_tl_flush_whole;
         R_FlushHTColumnsTC = tc_tl_flush_ht;
         R_FlushQuadColumnTC = tc_tl_flush_quad;

         dest = &tc_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tc_tempyl[tc_temp_x] = dcvars->yl;
         tc_tempyh[tc_temp_x] = dcvars->yh;

         if(dcvars->yl > tc_commontop)
            tc_commontop = dcvars->yl;
         if(dcvars->yh < tc_commonbot)
            tc_commonbot = dcvars->yh;


         dest = &tc_tempbuf[(dcvars->yl << 2) + tc_temp_x];

      }
      tc_temp_x += 1;
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
            *dest = (V_PaletteTC[ ((filter_getScale2xQuadColors( source[ ((frac & ((127<<16)|0xffff))>>16) ], source[ (((0)>(((frac & ((127<<16)|0xffff))>>16)-1)?(0):(((frac & ((127<<16)|0xffff))>>16)-1))) ], nextsource[ ((frac & ((127<<16)|0xffff))>>16) ], source[ (((frac+(1<<16)) & ((127<<16)|0xffff))>>16) ], prevsource[ ((frac & ((127<<16)|0xffff))>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & ((127<<16)|0xffff))>>8) & 0xff)>>(8-6)) ] ]))*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (V_PaletteTC[ ((filter_getScale2xQuadColors( source[ ((frac)>>16) ], source[ (((0)>(((frac)>>16)-1)?(0):(((frac)>>16)-1))) ], nextsource[ ((frac)>>16) ], source[ (((frac+(1<<16)))>>16) ], prevsource[ ((frac)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac)>>8) & 0xff)>>(8-6)) ] ]))*64 + ((64 -1)) ]);
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
               *dest = (V_PaletteTC[ ((filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (V_PaletteTC[ ((filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (V_PaletteTC[ ((filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ]))*64 + ((64 -1)) ]);
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





               *dest = (V_PaletteTC[ ((filter_getScale2xQuadColors( source[ ((frac)>>16) ], source[ (((0)>(((frac)>>16)-1)?(0):(((frac)>>16)-1))) ], nextsource[ ((frac)>>16) ], source[ ((nextfrac)>>16) ], prevsource[ ((frac)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac)>>8) & 0xff)>>(8-6)) ] ]))*64 + ((64 -1)) ]);
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
static void R_DrawColumnTC_RoundedUV_PointZ(draw_column_vars_t *dcvars)
{
   int count;

   uint32_t *dest;

   fixed_t frac;
   const fixed_t fracstep = dcvars->iscale;
   const fixed_t slope_texu = dcvars->texu;

   if (dcvars->brightmask)
   {
      R_DrawColumnTC_PointUV_PointZ(dcvars);
      return;
   }

   if (dcvars->iscale > drawvars.mag_threshold)
   {
      R_GetDrawColumnFuncTC(RDC_PIPELINE_STANDARD,
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

      if(tc_temp_x == 4 ||
            (tc_temp_x && (tc_temptype != tc_tl_temptype || tc_temp_x + tc_startx != dcvars->x)))
         R_FlushColumnsTC();

      if(!tc_temp_x)
      {
         tc_startx = dcvars->x;
         tc_tempyl[0] = tc_commontop = dcvars->yl;
         tc_tempyh[0] = tc_commonbot = dcvars->yh;
         tc_temptype = tc_tl_temptype;





         R_FlushWholeColumnsTC = tc_tl_flush_whole;
         R_FlushHTColumnsTC = tc_tl_flush_ht;
         R_FlushQuadColumnTC = tc_tl_flush_quad;

         dest = &tc_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tc_tempyl[tc_temp_x] = dcvars->yl;
         tc_tempyh[tc_temp_x] = dcvars->yh;

         if(dcvars->yl > tc_commontop)
            tc_commontop = dcvars->yl;
         if(dcvars->yh < tc_commonbot)
            tc_commonbot = dcvars->yh;


         dest = &tc_tempbuf[(dcvars->yl << 2) + tc_temp_x];

      }
      tc_temp_x += 1;
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
            *dest = (V_PaletteTC[ (colormap[(filter_getScale2xQuadColors( source[ ((frac & ((127<<16)|0xffff))>>16) ], source[ (((0)>(((frac & ((127<<16)|0xffff))>>16)-1)?(0):(((frac & ((127<<16)|0xffff))>>16)-1))) ], nextsource[ ((frac & ((127<<16)|0xffff))>>16) ], source[ (((frac+(1<<16)) & ((127<<16)|0xffff))>>16) ], prevsource[ ((frac & ((127<<16)|0xffff))>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & ((127<<16)|0xffff))>>8) & 0xff)>>(8-6)) ] ])])*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (V_PaletteTC[ (colormap[(filter_getScale2xQuadColors( source[ ((frac)>>16) ], source[ (((0)>(((frac)>>16)-1)?(0):(((frac)>>16)-1))) ], nextsource[ ((frac)>>16) ], source[ (((frac+(1<<16)))>>16) ], prevsource[ ((frac)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac)>>8) & 0xff)>>(8-6)) ] ])])*64 + ((64 -1)) ]);
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
               *dest = (V_PaletteTC[ (colormap[(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])])*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (V_PaletteTC[ (colormap[(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])])*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (V_PaletteTC[ (colormap[(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])])*64 + ((64 -1)) ]);
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





               *dest = (V_PaletteTC[ (colormap[(filter_getScale2xQuadColors( source[ ((frac)>>16) ], source[ (((0)>(((frac)>>16)-1)?(0):(((frac)>>16)-1))) ], nextsource[ ((frac)>>16) ], source[ ((nextfrac)>>16) ], prevsource[ ((frac)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac)>>8) & 0xff)>>(8-6)) ] ])])*64 + ((64 -1)) ]);
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
static void R_DrawColumnTC_RoundedUV_LinearZ(draw_column_vars_t *dcvars)
{
   int count;

   uint32_t *dest;

   fixed_t frac;
   const fixed_t fracstep = dcvars->iscale;
   const fixed_t slope_texu = dcvars->texu;

   if (dcvars->brightmask)
   {
      R_DrawColumnTC_PointUV_PointZ(dcvars);
      return;
   }

   if (dcvars->iscale > drawvars.mag_threshold)
   {
      R_GetDrawColumnFuncTC(RDC_PIPELINE_STANDARD,
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

      if(tc_temp_x == 4 ||
            (tc_temp_x && (tc_temptype != tc_tl_temptype || tc_temp_x + tc_startx != dcvars->x)))
         R_FlushColumnsTC();

      if(!tc_temp_x)
      {
         tc_startx = dcvars->x;
         tc_tempyl[0] = tc_commontop = dcvars->yl;
         tc_tempyh[0] = tc_commonbot = dcvars->yh;
         tc_temptype = tc_tl_temptype;





         R_FlushWholeColumnsTC = tc_tl_flush_whole;
         R_FlushHTColumnsTC = tc_tl_flush_ht;
         R_FlushQuadColumnTC = tc_tl_flush_quad;

         dest = &tc_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tc_tempyl[tc_temp_x] = dcvars->yl;
         tc_tempyh[tc_temp_x] = dcvars->yh;

         if(dcvars->yl > tc_commontop)
            tc_commontop = dcvars->yl;
         if(dcvars->yh < tc_commonbot)
            tc_commonbot = dcvars->yh;


         dest = &tc_tempbuf[(dcvars->yl << 2) + tc_temp_x];

      }
      tc_temp_x += 1;
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
            *dest = (V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(filter_getScale2xQuadColors( source[ ((frac & ((127<<16)|0xffff))>>16) ], source[ (((0)>(((frac & ((127<<16)|0xffff))>>16)-1)?(0):(((frac & ((127<<16)|0xffff))>>16)-1))) ], nextsource[ ((frac & ((127<<16)|0xffff))>>16) ], source[ (((frac+(1<<16)) & ((127<<16)|0xffff))>>16) ], prevsource[ ((frac & ((127<<16)|0xffff))>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & ((127<<16)|0xffff))>>8) & 0xff)>>(8-6)) ] ])]))*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(filter_getScale2xQuadColors( source[ ((frac)>>16) ], source[ (((0)>(((frac)>>16)-1)?(0):(((frac)>>16)-1))) ], nextsource[ ((frac)>>16) ], source[ (((frac+(1<<16)))>>16) ], prevsource[ ((frac)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac)>>8) & 0xff)>>(8-6)) ] ])]))*64 + ((64 -1)) ]);
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
               *dest = (V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])]))*64 + ((64 -1)) ]);
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





               *dest = (V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(filter_getScale2xQuadColors( source[ ((frac)>>16) ], source[ (((0)>(((frac)>>16)-1)?(0):(((frac)>>16)-1))) ], nextsource[ ((frac)>>16) ], source[ ((nextfrac)>>16) ], prevsource[ ((frac)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac)>>8) & 0xff)>>(8-6)) ] ])]))*64 + ((64 -1)) ]);
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

static void R_DrawTranslatedColumnTC_PointUV(draw_column_vars_t *dcvars)
{
   int count;

   uint32_t *dest;

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

   if(tc_temp_x == 4 ||
         (tc_temp_x && (tc_temptype != tc_tl_temptype || tc_temp_x + tc_startx != dcvars->x)))
      R_FlushColumnsTC();

   if(!tc_temp_x)
   {
      tc_startx = dcvars->x;
      tc_tempyl[0] = tc_commontop = dcvars->yl;
      tc_tempyh[0] = tc_commonbot = dcvars->yh;
      tc_temptype = tc_tl_temptype;





      R_FlushWholeColumnsTC = tc_tl_flush_whole;
      R_FlushHTColumnsTC = tc_tl_flush_ht;
      R_FlushQuadColumnTC = tc_tl_flush_quad;

      dest = &tc_tempbuf[dcvars->yl << 2];

   }
   else
   {
      tc_tempyl[tc_temp_x] = dcvars->yl;
      tc_tempyh[tc_temp_x] = dcvars->yh;

      if(dcvars->yl > tc_commontop)
         tc_commontop = dcvars->yl;
      if(dcvars->yh < tc_commonbot)
         tc_commonbot = dcvars->yh;


      dest = &tc_tempbuf[(dcvars->yl << 2) + tc_temp_x];

   }
   tc_temp_x += 1;



   {
      const uint8_t *source = dcvars->source;
      const uint8_t *translation = dcvars->translation;
      count++;







      if (dcvars->texheight == 128)
      {

         while(count--)
         {
            *dest = (V_PaletteTC[ ((translation[(source[(frac & ((127<<16)|0xffff))>>16])]))*64 + ((64 -1)) ]);
            ;
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (V_PaletteTC[ ((translation[(source[(frac)>>16])]))*64 + ((64 -1)) ]);
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
               *dest = (V_PaletteTC[ ((translation[(source[(frac & fixedt_heightmask)>>16])]))*64 + ((64 -1)) ]);
               ;
               dest += 4;
               frac += fracstep;
               *dest = (V_PaletteTC[ ((translation[(source[(frac & fixedt_heightmask)>>16])]))*64 + ((64 -1)) ]);
               ;
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (V_PaletteTC[ ((translation[(source[(frac & fixedt_heightmask)>>16])]))*64 + ((64 -1)) ]);
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





               *dest = (V_PaletteTC[ ((translation[(source[(frac)>>16])]))*64 + ((64 -1)) ]);
               ;
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;



            }
         }
      }
   }

}

static void R_DrawTranslatedColumnTC_PointUV_PointZ(draw_column_vars_t *dcvars)
{
   int count;

   uint32_t *dest;

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

   if(tc_temp_x == 4 ||
         (tc_temp_x && (tc_temptype != tc_tl_temptype || tc_temp_x + tc_startx != dcvars->x)))
      R_FlushColumnsTC();

   if(!tc_temp_x)
   {
      tc_startx = dcvars->x;
      tc_tempyl[0] = tc_commontop = dcvars->yl;
      tc_tempyh[0] = tc_commonbot = dcvars->yh;
      tc_temptype = tc_tl_temptype;





      R_FlushWholeColumnsTC = tc_tl_flush_whole;
      R_FlushHTColumnsTC = tc_tl_flush_ht;
      R_FlushQuadColumnTC = tc_tl_flush_quad;

      dest = &tc_tempbuf[dcvars->yl << 2];

   }
   else
   {
      tc_tempyl[tc_temp_x] = dcvars->yl;
      tc_tempyh[tc_temp_x] = dcvars->yh;

      if(dcvars->yl > tc_commontop)
         tc_commontop = dcvars->yl;
      if(dcvars->yh < tc_commonbot)
         tc_commonbot = dcvars->yh;


      dest = &tc_tempbuf[(dcvars->yl << 2) + tc_temp_x];

   }
   tc_temp_x += 1;



   {
      const uint8_t *source = dcvars->source;
      const lighttable_t *colormap = dcvars->colormap;
      const uint8_t *translation = dcvars->translation;
      count++;







      if (dcvars->texheight == 128)
      {

         while(count--)
         {
            *dest = (V_PaletteTC[ (colormap[(translation[(source[(frac & ((127<<16)|0xffff))>>16])])])*64 + ((64 -1)) ]);
            ;
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (V_PaletteTC[ (colormap[(translation[(source[(frac)>>16])])])*64 + ((64 -1)) ]);
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
               *dest = (V_PaletteTC[ (colormap[(translation[(source[(frac & fixedt_heightmask)>>16])])])*64 + ((64 -1)) ]);
               ;
               dest += 4;
               frac += fracstep;
               *dest = (V_PaletteTC[ (colormap[(translation[(source[(frac & fixedt_heightmask)>>16])])])*64 + ((64 -1)) ]);
               ;
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (V_PaletteTC[ (colormap[(translation[(source[(frac & fixedt_heightmask)>>16])])])*64 + ((64 -1)) ]);
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
               *dest = (V_PaletteTC[ (colormap[(translation[(source[(frac)>>16])])])*64 + ((64 -1)) ]);
               ;
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;
            }
         }
      }
   }

}


static void R_DrawTranslatedColumnTC_PointUV_LinearZ(draw_column_vars_t *dcvars)
{
  int count;

  uint32_t *dest;

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

      if(tc_temp_x == 4 ||
         (tc_temp_x && (tc_temptype != tc_tl_temptype || tc_temp_x + tc_startx != dcvars->x)))
         R_FlushColumnsTC();

      if(!tc_temp_x)
      {
         tc_startx = dcvars->x;
         tc_tempyl[0] = tc_commontop = dcvars->yl;
         tc_tempyh[0] = tc_commonbot = dcvars->yh;
         tc_temptype = tc_tl_temptype;





         R_FlushWholeColumnsTC = tc_tl_flush_whole;
         R_FlushHTColumnsTC = tc_tl_flush_ht;
         R_FlushQuadColumnTC = tc_tl_flush_quad;

         dest = &tc_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tc_tempyl[tc_temp_x] = dcvars->yl;
         tc_tempyh[tc_temp_x] = dcvars->yh;

         if(dcvars->yl > tc_commontop)
            tc_commontop = dcvars->yl;
         if(dcvars->yh < tc_commonbot)
            tc_commonbot = dcvars->yh;


         dest = &tc_tempbuf[(dcvars->yl << 2) + tc_temp_x];

      }
      tc_temp_x += 1;
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
            *dest = (V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(frac & ((127<<16)|0xffff))>>16])])]))*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(frac)>>16])])]))*64 + ((64 -1)) ]);
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
               *dest = (V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(frac & fixedt_heightmask)>>16])])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(frac & fixedt_heightmask)>>16])])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(frac & fixedt_heightmask)>>16])])]))*64 + ((64 -1)) ]);
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





               *dest = (V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(frac)>>16])])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;



            }
         }
      }
   }

}

static void R_DrawTranslatedColumnTC_LinearUV(draw_column_vars_t *dcvars)
{
  int count;

  uint32_t *dest;

  fixed_t frac;
  const fixed_t fracstep = dcvars->iscale;

  const fixed_t slope_texu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;






  if (dcvars->iscale > drawvars.mag_threshold) {
    R_GetDrawColumnFuncTC(RDC_PIPELINE_TRANSLATED,
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

      if(tc_temp_x == 4 ||
         (tc_temp_x && (tc_temptype != tc_tl_temptype || tc_temp_x + tc_startx != dcvars->x)))
         R_FlushColumnsTC();

      if(!tc_temp_x)
      {
         tc_startx = dcvars->x;
         tc_tempyl[0] = tc_commontop = dcvars->yl;
         tc_tempyh[0] = tc_commonbot = dcvars->yh;
         tc_temptype = tc_tl_temptype;





         R_FlushWholeColumnsTC = tc_tl_flush_whole;
         R_FlushHTColumnsTC = tc_tl_flush_ht;
         R_FlushQuadColumnTC = tc_tl_flush_quad;

         dest = &tc_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tc_tempyl[tc_temp_x] = dcvars->yl;
         tc_tempyh[tc_temp_x] = dcvars->yh;

         if(dcvars->yl > tc_commontop)
            tc_commontop = dcvars->yl;
         if(dcvars->yh < tc_commonbot)
            tc_commonbot = dcvars->yh;


         dest = &tc_tempbuf[(dcvars->yl << 2) + tc_temp_x];

      }
      tc_temp_x += 1;
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
            *dest = (( V_PaletteTC[ ((translation[(nextsource[((frac+(1<<16)) & ((127<<16)|0xffff))>>16])]))*64 + ((filter_fracu*((frac & ((127<<16)|0xffff))&0xffff))>>(32-6)) ] + V_PaletteTC[ ((translation[(source[((frac+(1<<16)) & ((127<<16)|0xffff))>>16])]))*64 + (((0xffff-filter_fracu)*((frac & ((127<<16)|0xffff))&0xffff))>>(32-6)) ] + V_PaletteTC[ ((translation[(source[(frac & ((127<<16)|0xffff))>>16])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & ((127<<16)|0xffff))&0xffff)))>>(32-6)) ] + V_PaletteTC[ ((translation[(nextsource[(frac & ((127<<16)|0xffff))>>16])]))*64 + ((filter_fracu*(0xffff-((frac & ((127<<16)|0xffff))&0xffff)))>>(32-6)) ]));
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (( V_PaletteTC[ ((translation[(nextsource[((frac+(1<<16)))>>16])]))*64 + ((filter_fracu*((frac)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((translation[(source[((frac+(1<<16)))>>16])]))*64 + (((0xffff-filter_fracu)*((frac)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((translation[(source[(frac)>>16])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac)&0xffff)))>>(32-6)) ] + V_PaletteTC[ ((translation[(nextsource[(frac)>>16])]))*64 + ((filter_fracu*(0xffff-((frac)&0xffff)))>>(32-6)) ]));
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
               *dest = (( V_PaletteTC[ ((translation[(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])]))*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((translation[(source[((frac+(1<<16)) & fixedt_heightmask)>>16])]))*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((translation[(source[(frac & fixedt_heightmask)>>16])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_PaletteTC[ ((translation[(nextsource[(frac & fixedt_heightmask)>>16])]))*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (( V_PaletteTC[ ((translation[(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])]))*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((translation[(source[((frac+(1<<16)) & fixedt_heightmask)>>16])]))*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((translation[(source[(frac & fixedt_heightmask)>>16])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_PaletteTC[ ((translation[(nextsource[(frac & fixedt_heightmask)>>16])]))*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (( V_PaletteTC[ ((translation[(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])]))*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((translation[(source[((frac+(1<<16)) & fixedt_heightmask)>>16])]))*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((translation[(source[(frac & fixedt_heightmask)>>16])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_PaletteTC[ ((translation[(nextsource[(frac & fixedt_heightmask)>>16])]))*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
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





               *dest = (( V_PaletteTC[ ((translation[(nextsource[(nextfrac)>>16])]))*64 + ((filter_fracu*((frac)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((translation[(source[(nextfrac)>>16])]))*64 + (((0xffff-filter_fracu)*((frac)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((translation[(source[(frac)>>16])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac)&0xffff)))>>(32-6)) ] + V_PaletteTC[ ((translation[(nextsource[(frac)>>16])]))*64 + ((filter_fracu*(0xffff-((frac)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;

               if ((nextfrac += fracstep) >= (int)heightmask) nextfrac -= heightmask;;

            }
         }
      }
   }

}

static void R_DrawTranslatedColumnTC_LinearUV_PointZ(draw_column_vars_t *dcvars)
{
  int count;

  uint32_t *dest;

  fixed_t frac;
  const fixed_t fracstep = dcvars->iscale;

  const fixed_t slope_texu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;

  if (dcvars->iscale > drawvars.mag_threshold) {
    R_GetDrawColumnFuncTC(RDC_PIPELINE_TRANSLATED,
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

      if(tc_temp_x == 4 ||
         (tc_temp_x && (tc_temptype != tc_tl_temptype || tc_temp_x + tc_startx != dcvars->x)))
         R_FlushColumnsTC();

      if(!tc_temp_x)
      {
         tc_startx = dcvars->x;
         tc_tempyl[0] = tc_commontop = dcvars->yl;
         tc_tempyh[0] = tc_commonbot = dcvars->yh;
         tc_temptype = tc_tl_temptype;





         R_FlushWholeColumnsTC = tc_tl_flush_whole;
         R_FlushHTColumnsTC = tc_tl_flush_ht;
         R_FlushQuadColumnTC = tc_tl_flush_quad;

         dest = &tc_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tc_tempyl[tc_temp_x] = dcvars->yl;
         tc_tempyh[tc_temp_x] = dcvars->yh;

         if(dcvars->yl > tc_commontop)
            tc_commontop = dcvars->yl;
         if(dcvars->yh < tc_commonbot)
            tc_commonbot = dcvars->yh;


         dest = &tc_tempbuf[(dcvars->yl << 2) + tc_temp_x];

      }
      tc_temp_x += 1;
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
            *dest = (( V_PaletteTC[ (colormap[(translation[(nextsource[((frac+(1<<16)) & ((127<<16)|0xffff))>>16])])])*64 + ((filter_fracu*((frac & ((127<<16)|0xffff))&0xffff))>>(32-6)) ] + V_PaletteTC[ (colormap[(translation[(source[((frac+(1<<16)) & ((127<<16)|0xffff))>>16])])])*64 + (((0xffff-filter_fracu)*((frac & ((127<<16)|0xffff))&0xffff))>>(32-6)) ] + V_PaletteTC[ (colormap[(translation[(source[(frac & ((127<<16)|0xffff))>>16])])])*64 + (((0xffff-filter_fracu)*(0xffff-((frac & ((127<<16)|0xffff))&0xffff)))>>(32-6)) ] + V_PaletteTC[ (colormap[(translation[(nextsource[(frac & ((127<<16)|0xffff))>>16])])])*64 + ((filter_fracu*(0xffff-((frac & ((127<<16)|0xffff))&0xffff)))>>(32-6)) ]));
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (( V_PaletteTC[ (colormap[(translation[(nextsource[((frac+(1<<16)))>>16])])])*64 + ((filter_fracu*((frac)&0xffff))>>(32-6)) ] + V_PaletteTC[ (colormap[(translation[(source[((frac+(1<<16)))>>16])])])*64 + (((0xffff-filter_fracu)*((frac)&0xffff))>>(32-6)) ] + V_PaletteTC[ (colormap[(translation[(source[(frac)>>16])])])*64 + (((0xffff-filter_fracu)*(0xffff-((frac)&0xffff)))>>(32-6)) ] + V_PaletteTC[ (colormap[(translation[(nextsource[(frac)>>16])])])*64 + ((filter_fracu*(0xffff-((frac)&0xffff)))>>(32-6)) ]));
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
               *dest = (( V_PaletteTC[ (colormap[(translation[(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])])])*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_PaletteTC[ (colormap[(translation[(source[((frac+(1<<16)) & fixedt_heightmask)>>16])])])*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_PaletteTC[ (colormap[(translation[(source[(frac & fixedt_heightmask)>>16])])])*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_PaletteTC[ (colormap[(translation[(nextsource[(frac & fixedt_heightmask)>>16])])])*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (( V_PaletteTC[ (colormap[(translation[(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])])])*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_PaletteTC[ (colormap[(translation[(source[((frac+(1<<16)) & fixedt_heightmask)>>16])])])*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_PaletteTC[ (colormap[(translation[(source[(frac & fixedt_heightmask)>>16])])])*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_PaletteTC[ (colormap[(translation[(nextsource[(frac & fixedt_heightmask)>>16])])])*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (( V_PaletteTC[ (colormap[(translation[(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])])])*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_PaletteTC[ (colormap[(translation[(source[((frac+(1<<16)) & fixedt_heightmask)>>16])])])*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_PaletteTC[ (colormap[(translation[(source[(frac & fixedt_heightmask)>>16])])])*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_PaletteTC[ (colormap[(translation[(nextsource[(frac & fixedt_heightmask)>>16])])])*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
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





               *dest = (( V_PaletteTC[ (colormap[(translation[(nextsource[(nextfrac)>>16])])])*64 + ((filter_fracu*((frac)&0xffff))>>(32-6)) ] + V_PaletteTC[ (colormap[(translation[(source[(nextfrac)>>16])])])*64 + (((0xffff-filter_fracu)*((frac)&0xffff))>>(32-6)) ] + V_PaletteTC[ (colormap[(translation[(source[(frac)>>16])])])*64 + (((0xffff-filter_fracu)*(0xffff-((frac)&0xffff)))>>(32-6)) ] + V_PaletteTC[ (colormap[(translation[(nextsource[(frac)>>16])])])*64 + ((filter_fracu*(0xffff-((frac)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;

               if ((nextfrac += fracstep) >= (int)heightmask) nextfrac -= heightmask;;

            }
         }
      }
   }

}

static void R_DrawTranslatedColumnTC_LinearUV_LinearZ(draw_column_vars_t *dcvars)
{
  int count;

  uint32_t *dest;

  fixed_t frac;
  const fixed_t fracstep = dcvars->iscale;

  const fixed_t slope_texu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;

  if (dcvars->iscale > drawvars.mag_threshold)
  {
     R_GetDrawColumnFuncTC(RDC_PIPELINE_TRANSLATED,
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

      if(tc_temp_x == 4 ||
         (tc_temp_x && (tc_temptype != tc_tl_temptype || tc_temp_x + tc_startx != dcvars->x)))
         R_FlushColumnsTC();

      if(!tc_temp_x)
      {
         tc_startx = dcvars->x;
         tc_tempyl[0] = tc_commontop = dcvars->yl;
         tc_tempyh[0] = tc_commonbot = dcvars->yh;
         tc_temptype = tc_tl_temptype;





         R_FlushWholeColumnsTC = tc_tl_flush_whole;
         R_FlushHTColumnsTC = tc_tl_flush_ht;
         R_FlushQuadColumnTC = tc_tl_flush_quad;

         dest = &tc_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tc_tempyl[tc_temp_x] = dcvars->yl;
         tc_tempyh[tc_temp_x] = dcvars->yh;

         if(dcvars->yl > tc_commontop)
            tc_commontop = dcvars->yl;
         if(dcvars->yh < tc_commonbot)
            tc_commonbot = dcvars->yh;


         dest = &tc_tempbuf[(dcvars->yl << 2) + tc_temp_x];

      }
      tc_temp_x += 1;
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
            *dest = (( V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(nextsource[((frac+(1<<16)) & ((127<<16)|0xffff))>>16])])]))*64 + ((filter_fracu*((frac & ((127<<16)|0xffff))&0xffff))>>(32-6)) ] + V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[((frac+(1<<16)) & ((127<<16)|0xffff))>>16])])]))*64 + (((0xffff-filter_fracu)*((frac & ((127<<16)|0xffff))&0xffff))>>(32-6)) ] + V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(frac & ((127<<16)|0xffff))>>16])])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & ((127<<16)|0xffff))&0xffff)))>>(32-6)) ] + V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(nextsource[(frac & ((127<<16)|0xffff))>>16])])]))*64 + ((filter_fracu*(0xffff-((frac & ((127<<16)|0xffff))&0xffff)))>>(32-6)) ]));
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (( V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(nextsource[((frac+(1<<16)))>>16])])]))*64 + ((filter_fracu*((frac)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[((frac+(1<<16)))>>16])])]))*64 + (((0xffff-filter_fracu)*((frac)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(frac)>>16])])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac)&0xffff)))>>(32-6)) ] + V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(nextsource[(frac)>>16])])]))*64 + ((filter_fracu*(0xffff-((frac)&0xffff)))>>(32-6)) ]));
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
               *dest = (( V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])])]))*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[((frac+(1<<16)) & fixedt_heightmask)>>16])])]))*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(frac & fixedt_heightmask)>>16])])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(nextsource[(frac & fixedt_heightmask)>>16])])]))*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (( V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])])]))*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[((frac+(1<<16)) & fixedt_heightmask)>>16])])]))*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(frac & fixedt_heightmask)>>16])])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(nextsource[(frac & fixedt_heightmask)>>16])])]))*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (( V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])])]))*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[((frac+(1<<16)) & fixedt_heightmask)>>16])])]))*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(frac & fixedt_heightmask)>>16])])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(nextsource[(frac & fixedt_heightmask)>>16])])]))*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
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





               *dest = (( V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(nextsource[(nextfrac)>>16])])]))*64 + ((filter_fracu*((frac)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(nextfrac)>>16])])]))*64 + (((0xffff-filter_fracu)*((frac)&0xffff))>>(32-6)) ] + V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(frac)>>16])])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac)&0xffff)))>>(32-6)) ] + V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(nextsource[(frac)>>16])])]))*64 + ((filter_fracu*(0xffff-((frac)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;

               if ((nextfrac += fracstep) >= (int)heightmask) nextfrac -= heightmask;;

            }
         }
      }
   }

}

static void R_DrawTranslatedColumnTC_RoundedUV(draw_column_vars_t *dcvars)
{
  int count;
  uint32_t *dest;
  fixed_t frac;
  const fixed_t fracstep = dcvars->iscale;
  const fixed_t slope_texu = dcvars->texu;

  if (dcvars->iscale > drawvars.mag_threshold)
  {
    R_GetDrawColumnFuncTC(RDC_PIPELINE_TRANSLATED,
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

      if(tc_temp_x == 4 ||
         (tc_temp_x && (tc_temptype != tc_tl_temptype || tc_temp_x + tc_startx != dcvars->x)))
         R_FlushColumnsTC();

      if(!tc_temp_x)
      {
         tc_startx = dcvars->x;
         tc_tempyl[0] = tc_commontop = dcvars->yl;
         tc_tempyh[0] = tc_commonbot = dcvars->yh;
         tc_temptype = tc_tl_temptype;





         R_FlushWholeColumnsTC = tc_tl_flush_whole;
         R_FlushHTColumnsTC = tc_tl_flush_ht;
         R_FlushQuadColumnTC = tc_tl_flush_quad;

         dest = &tc_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tc_tempyl[tc_temp_x] = dcvars->yl;
         tc_tempyh[tc_temp_x] = dcvars->yh;

         if(dcvars->yl > tc_commontop)
            tc_commontop = dcvars->yl;
         if(dcvars->yh < tc_commonbot)
            tc_commonbot = dcvars->yh;


         dest = &tc_tempbuf[(dcvars->yl << 2) + tc_temp_x];

      }
      tc_temp_x += 1;
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
            *dest = (V_PaletteTC[ ((translation[(filter_getScale2xQuadColors( source[ ((frac & ((127<<16)|0xffff))>>16) ], source[ (((0)>(((frac & ((127<<16)|0xffff))>>16)-1)?(0):(((frac & ((127<<16)|0xffff))>>16)-1))) ], nextsource[ ((frac & ((127<<16)|0xffff))>>16) ], source[ (((frac+(1<<16)) & ((127<<16)|0xffff))>>16) ], prevsource[ ((frac & ((127<<16)|0xffff))>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & ((127<<16)|0xffff))>>8) & 0xff)>>(8-6)) ] ])]))*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (V_PaletteTC[ ((translation[(filter_getScale2xQuadColors( source[ ((frac)>>16) ], source[ (((0)>(((frac)>>16)-1)?(0):(((frac)>>16)-1))) ], nextsource[ ((frac)>>16) ], source[ (((frac+(1<<16)))>>16) ], prevsource[ ((frac)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac)>>8) & 0xff)>>(8-6)) ] ])]))*64 + ((64 -1)) ]);
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
               *dest = (V_PaletteTC[ ((translation[(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (V_PaletteTC[ ((translation[(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (V_PaletteTC[ ((translation[(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])]))*64 + ((64 -1)) ]);
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





               *dest = (V_PaletteTC[ ((translation[(filter_getScale2xQuadColors( source[ ((frac)>>16) ], source[ (((0)>(((frac)>>16)-1)?(0):(((frac)>>16)-1))) ], nextsource[ ((frac)>>16) ], source[ ((nextfrac)>>16) ], prevsource[ ((frac)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac)>>8) & 0xff)>>(8-6)) ] ])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;

               if ((nextfrac += fracstep) >= (int)heightmask) nextfrac -= heightmask;;

            }
         }
      }
   }

}

static void R_DrawTranslatedColumnTC_RoundedUV_PointZ(draw_column_vars_t *dcvars)
{
  int count;

  uint32_t *dest;

  fixed_t frac;
  const fixed_t fracstep = dcvars->iscale;
  const fixed_t slope_texu = dcvars->texu;

  if (dcvars->iscale > drawvars.mag_threshold) {
    R_GetDrawColumnFuncTC(RDC_PIPELINE_TRANSLATED,
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

      if(tc_temp_x == 4 ||
         (tc_temp_x && (tc_temptype != tc_tl_temptype || tc_temp_x + tc_startx != dcvars->x)))
         R_FlushColumnsTC();

      if(!tc_temp_x)
      {
         tc_startx = dcvars->x;
         tc_tempyl[0] = tc_commontop = dcvars->yl;
         tc_tempyh[0] = tc_commonbot = dcvars->yh;
         tc_temptype = tc_tl_temptype;





         R_FlushWholeColumnsTC = tc_tl_flush_whole;
         R_FlushHTColumnsTC = tc_tl_flush_ht;
         R_FlushQuadColumnTC = tc_tl_flush_quad;

         dest = &tc_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tc_tempyl[tc_temp_x] = dcvars->yl;
         tc_tempyh[tc_temp_x] = dcvars->yh;

         if(dcvars->yl > tc_commontop)
            tc_commontop = dcvars->yl;
         if(dcvars->yh < tc_commonbot)
            tc_commonbot = dcvars->yh;


         dest = &tc_tempbuf[(dcvars->yl << 2) + tc_temp_x];

      }
      tc_temp_x += 1;
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
            *dest = (V_PaletteTC[ (colormap[(translation[(filter_getScale2xQuadColors( source[ ((frac & ((127<<16)|0xffff))>>16) ], source[ (((0)>(((frac & ((127<<16)|0xffff))>>16)-1)?(0):(((frac & ((127<<16)|0xffff))>>16)-1))) ], nextsource[ ((frac & ((127<<16)|0xffff))>>16) ], source[ (((frac+(1<<16)) & ((127<<16)|0xffff))>>16) ], prevsource[ ((frac & ((127<<16)|0xffff))>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & ((127<<16)|0xffff))>>8) & 0xff)>>(8-6)) ] ])])])*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (V_PaletteTC[ (colormap[(translation[(filter_getScale2xQuadColors( source[ ((frac)>>16) ], source[ (((0)>(((frac)>>16)-1)?(0):(((frac)>>16)-1))) ], nextsource[ ((frac)>>16) ], source[ (((frac+(1<<16)))>>16) ], prevsource[ ((frac)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac)>>8) & 0xff)>>(8-6)) ] ])])])*64 + ((64 -1)) ]);
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
               *dest = (V_PaletteTC[ (colormap[(translation[(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])])])*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (V_PaletteTC[ (colormap[(translation[(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])])])*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (V_PaletteTC[ (colormap[(translation[(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])])])*64 + ((64 -1)) ]);
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





               *dest = (V_PaletteTC[ (colormap[(translation[(filter_getScale2xQuadColors( source[ ((frac)>>16) ], source[ (((0)>(((frac)>>16)-1)?(0):(((frac)>>16)-1))) ], nextsource[ ((frac)>>16) ], source[ ((nextfrac)>>16) ], prevsource[ ((frac)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac)>>8) & 0xff)>>(8-6)) ] ])])])*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;

               if ((nextfrac += fracstep) >= (int)heightmask) nextfrac -= heightmask;;

            }
         }
      }
   }

}

static void R_DrawTranslatedColumnTC_RoundedUV_LinearZ(draw_column_vars_t *dcvars)
{
   int count;
   uint32_t *dest;
   fixed_t frac;
   const fixed_t fracstep = dcvars->iscale;
   const fixed_t slope_texu = dcvars->texu;

   if (dcvars->iscale > drawvars.mag_threshold)
   {
      R_GetDrawColumnFuncTC(RDC_PIPELINE_TRANSLATED,
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

      if(tc_temp_x == 4 ||
            (tc_temp_x && (tc_temptype != tc_tl_temptype || tc_temp_x + tc_startx != dcvars->x)))
         R_FlushColumnsTC();

      if(!tc_temp_x)
      {
         tc_startx = dcvars->x;
         tc_tempyl[0] = tc_commontop = dcvars->yl;
         tc_tempyh[0] = tc_commonbot = dcvars->yh;
         tc_temptype = tc_tl_temptype;





         R_FlushWholeColumnsTC = tc_tl_flush_whole;
         R_FlushHTColumnsTC = tc_tl_flush_ht;
         R_FlushQuadColumnTC = tc_tl_flush_quad;

         dest = &tc_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tc_tempyl[tc_temp_x] = dcvars->yl;
         tc_tempyh[tc_temp_x] = dcvars->yh;

         if(dcvars->yl > tc_commontop)
            tc_commontop = dcvars->yl;
         if(dcvars->yh < tc_commonbot)
            tc_commonbot = dcvars->yh;


         dest = &tc_tempbuf[(dcvars->yl << 2) + tc_temp_x];

      }
      tc_temp_x += 1;
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
            *dest = (V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(filter_getScale2xQuadColors( source[ ((frac & ((127<<16)|0xffff))>>16) ], source[ (((0)>(((frac & ((127<<16)|0xffff))>>16)-1)?(0):(((frac & ((127<<16)|0xffff))>>16)-1))) ], nextsource[ ((frac & ((127<<16)|0xffff))>>16) ], source[ (((frac+(1<<16)) & ((127<<16)|0xffff))>>16) ], prevsource[ ((frac & ((127<<16)|0xffff))>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & ((127<<16)|0xffff))>>8) & 0xff)>>(8-6)) ] ])])]))*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(filter_getScale2xQuadColors( source[ ((frac)>>16) ], source[ (((0)>(((frac)>>16)-1)?(0):(((frac)>>16)-1))) ], nextsource[ ((frac)>>16) ], source[ (((frac+(1<<16)))>>16) ], prevsource[ ((frac)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac)>>8) & 0xff)>>(8-6)) ] ])])]))*64 + ((64 -1)) ]);
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
               *dest = (V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])])]))*64 + ((64 -1)) ]);
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





               *dest = (V_PaletteTC[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(filter_getScale2xQuadColors( source[ ((frac)>>16) ], source[ (((0)>(((frac)>>16)-1)?(0):(((frac)>>16)-1))) ], nextsource[ ((frac)>>16) ], source[ ((nextfrac)>>16) ], prevsource[ ((frac)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac)>>8) & 0xff)>>(8-6)) ] ])])]))*64 + ((64 -1)) ]);
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

static void R_DrawFuzzColumnTC_PointUV(draw_column_vars_t *dcvars)
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

   if(tc_temp_x == 4 ||
         (tc_temp_x && (tc_temptype != (COL_FUZZ) || tc_temp_x + tc_startx != dcvars->x)))
      R_FlushColumnsTC();

   if(!tc_temp_x)
   {
      tc_startx = dcvars->x;
      tc_tempyl[0] = tc_commontop = dcvars->yl;
      tc_tempyh[0] = tc_commonbot = dcvars->yh;
      tc_temptype = (COL_FUZZ);

      tc_tempfuzzmap = fullcolormap;

      R_FlushWholeColumnsTC = R_FlushWholeFuzzTC;
      R_FlushHTColumnsTC = R_FlushHTFuzzTC;
      R_FlushQuadColumnTC = R_FlushQuadFuzzTC;
   }
   else
   {
      tc_tempyl[tc_temp_x] = dcvars->yl;
      tc_tempyh[tc_temp_x] = dcvars->yh;

      if(dcvars->yl > tc_commontop)
         tc_commontop = dcvars->yl;
      if(dcvars->yh < tc_commonbot)
         tc_commonbot = dcvars->yh;
   }
   tc_temp_x += 1;
}

static void R_DrawFuzzColumnTC_PointUV_PointZ(draw_column_vars_t *dcvars)
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

  if(tc_temp_x == 4 ||
        (tc_temp_x && (tc_temptype != (COL_FUZZ) || tc_temp_x + tc_startx != dcvars->x)))
     R_FlushColumnsTC();

  if(!tc_temp_x)
  {
     tc_startx = dcvars->x;
     tc_tempyl[0] = tc_commontop = dcvars->yl;
     tc_tempyh[0] = tc_commonbot = dcvars->yh;
     tc_temptype = (COL_FUZZ);

     tc_tempfuzzmap = fullcolormap;

     R_FlushWholeColumnsTC = R_FlushWholeFuzzTC;
     R_FlushHTColumnsTC = R_FlushHTFuzzTC;
     R_FlushQuadColumnTC = R_FlushQuadFuzzTC;
  }
  else
  {
     tc_tempyl[tc_temp_x] = dcvars->yl;
     tc_tempyh[tc_temp_x] = dcvars->yh;

     if(dcvars->yl > tc_commontop)
        tc_commontop = dcvars->yl;
     if(dcvars->yh < tc_commonbot)
        tc_commonbot = dcvars->yh;
  }
  tc_temp_x += 1;
}

static void R_DrawFuzzColumnTC_PointUV_LinearZ(draw_column_vars_t *dcvars)
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

  if(tc_temp_x == 4 ||
        (tc_temp_x && (tc_temptype != (COL_FUZZ) || tc_temp_x + tc_startx != dcvars->x)))
     R_FlushColumnsTC();

  if(!tc_temp_x)
  {
     tc_startx = dcvars->x;
     tc_tempyl[0] = tc_commontop = dcvars->yl;
     tc_tempyh[0] = tc_commonbot = dcvars->yh;
     tc_temptype = (COL_FUZZ);



     tc_tempfuzzmap = fullcolormap;

     R_FlushWholeColumnsTC = R_FlushWholeFuzzTC;
     R_FlushHTColumnsTC = R_FlushHTFuzzTC;
     R_FlushQuadColumnTC = R_FlushQuadFuzzTC;



  }
  else
  {
     tc_tempyl[tc_temp_x] = dcvars->yl;
     tc_tempyh[tc_temp_x] = dcvars->yh;

     if(dcvars->yl > tc_commontop)
        tc_commontop = dcvars->yl;
     if(dcvars->yh < tc_commonbot)
        tc_commonbot = dcvars->yh;




  }
  tc_temp_x += 1;
}

static void R_DrawFuzzColumnTC_LinearUV(draw_column_vars_t *dcvars)
{
  int count;
  fixed_t frac;
  const fixed_t fracstep = dcvars->iscale;
  const fixed_t slope_texu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;

  if (dcvars->iscale > drawvars.mag_threshold)
  {
    R_GetDrawColumnFuncTC(RDC_PIPELINE_FUZZ,
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

  if(tc_temp_x == 4 ||
        (tc_temp_x && (tc_temptype != (COL_FUZZ) || tc_temp_x + tc_startx != dcvars->x)))
     R_FlushColumnsTC();

  if(!tc_temp_x)
  {
     tc_startx = dcvars->x;
     tc_tempyl[0] = tc_commontop = dcvars->yl;
     tc_tempyh[0] = tc_commonbot = dcvars->yh;
     tc_temptype = (COL_FUZZ);



     tc_tempfuzzmap = fullcolormap;

     R_FlushWholeColumnsTC = R_FlushWholeFuzzTC;
     R_FlushHTColumnsTC = R_FlushHTFuzzTC;
     R_FlushQuadColumnTC = R_FlushQuadFuzzTC;



  }
  else
  {
     tc_tempyl[tc_temp_x] = dcvars->yl;
     tc_tempyh[tc_temp_x] = dcvars->yh;

     if(dcvars->yl > tc_commontop)
        tc_commontop = dcvars->yl;
     if(dcvars->yh < tc_commonbot)
        tc_commonbot = dcvars->yh;




  }
  tc_temp_x += 1;
}

static void R_DrawFuzzColumnTC_LinearUV_PointZ(draw_column_vars_t *dcvars)
{
   int count;
   fixed_t frac;
   const fixed_t fracstep = dcvars->iscale;
   const fixed_t slope_texu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;

   if (dcvars->iscale > drawvars.mag_threshold)
   {
      R_GetDrawColumnFuncTC(RDC_PIPELINE_FUZZ,
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

   if(tc_temp_x == 4 ||
         (tc_temp_x && (tc_temptype != (COL_FUZZ) || tc_temp_x + tc_startx != dcvars->x)))
      R_FlushColumnsTC();

   if(!tc_temp_x)
   {
      tc_startx = dcvars->x;
      tc_tempyl[0] = tc_commontop = dcvars->yl;
      tc_tempyh[0] = tc_commonbot = dcvars->yh;
      tc_temptype = (COL_FUZZ);



      tc_tempfuzzmap = fullcolormap;

      R_FlushWholeColumnsTC = R_FlushWholeFuzzTC;
      R_FlushHTColumnsTC = R_FlushHTFuzzTC;
      R_FlushQuadColumnTC = R_FlushQuadFuzzTC;



   }
   else
   {
      tc_tempyl[tc_temp_x] = dcvars->yl;
      tc_tempyh[tc_temp_x] = dcvars->yh;

      if(dcvars->yl > tc_commontop)
         tc_commontop = dcvars->yl;
      if(dcvars->yh < tc_commonbot)
         tc_commonbot = dcvars->yh;




   }
   tc_temp_x += 1;
}

static void R_DrawFuzzColumnTC_LinearUV_LinearZ(draw_column_vars_t *dcvars)
{
  int count;



  fixed_t frac;
  const fixed_t fracstep = dcvars->iscale;

  const fixed_t slope_texu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;






  if (dcvars->iscale > drawvars.mag_threshold) {
    R_GetDrawColumnFuncTC(RDC_PIPELINE_FUZZ,
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

      if(tc_temp_x == 4 ||
         (tc_temp_x && (tc_temptype != (COL_FUZZ) || tc_temp_x + tc_startx != dcvars->x)))
         R_FlushColumnsTC();

      if(!tc_temp_x)
      {
         tc_startx = dcvars->x;
         tc_tempyl[0] = tc_commontop = dcvars->yl;
         tc_tempyh[0] = tc_commonbot = dcvars->yh;
         tc_temptype = (COL_FUZZ);



         tc_tempfuzzmap = fullcolormap;

         R_FlushWholeColumnsTC = R_FlushWholeFuzzTC;
         R_FlushHTColumnsTC = R_FlushHTFuzzTC;
         R_FlushQuadColumnTC = R_FlushQuadFuzzTC;



      }
      else
      {
         tc_tempyl[tc_temp_x] = dcvars->yl;
         tc_tempyh[tc_temp_x] = dcvars->yh;

         if(dcvars->yl > tc_commontop)
            tc_commontop = dcvars->yl;
         if(dcvars->yh < tc_commonbot)
            tc_commonbot = dcvars->yh;




      }
      tc_temp_x += 1;
   }
}

static void R_DrawFuzzColumnTC_RoundedUV(draw_column_vars_t *dcvars)
{
   int count;
   fixed_t frac;
   const fixed_t fracstep = dcvars->iscale;
   const fixed_t slope_texu = dcvars->texu;

   if (dcvars->iscale > drawvars.mag_threshold)
   {
      R_GetDrawColumnFuncTC(RDC_PIPELINE_FUZZ,
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

   if(tc_temp_x == 4 ||
         (tc_temp_x && (tc_temptype != (COL_FUZZ) || tc_temp_x + tc_startx != dcvars->x)))
      R_FlushColumnsTC();

   if(!tc_temp_x)
   {
      tc_startx = dcvars->x;
      tc_tempyl[0] = tc_commontop = dcvars->yl;
      tc_tempyh[0] = tc_commonbot = dcvars->yh;
      tc_temptype = (COL_FUZZ);



      tc_tempfuzzmap = fullcolormap;

      R_FlushWholeColumnsTC = R_FlushWholeFuzzTC;
      R_FlushHTColumnsTC = R_FlushHTFuzzTC;
      R_FlushQuadColumnTC = R_FlushQuadFuzzTC;
   }
   else
   {
      tc_tempyl[tc_temp_x] = dcvars->yl;
      tc_tempyh[tc_temp_x] = dcvars->yh;

      if(dcvars->yl > tc_commontop)
         tc_commontop = dcvars->yl;
      if(dcvars->yh < tc_commonbot)
         tc_commonbot = dcvars->yh;
   }
   tc_temp_x += 1;
}

static void R_DrawFuzzColumnTC_RoundedUV_PointZ(draw_column_vars_t *dcvars)
{
   int count;
   fixed_t frac;
   const fixed_t fracstep = dcvars->iscale;
   const fixed_t slope_texu = dcvars->texu;

   if (dcvars->iscale > drawvars.mag_threshold)
   {
      R_GetDrawColumnFuncTC(RDC_PIPELINE_FUZZ,
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

   if(tc_temp_x == 4 ||
         (tc_temp_x && (tc_temptype != (COL_FUZZ) || tc_temp_x + tc_startx != dcvars->x)))
      R_FlushColumnsTC();

   if(!tc_temp_x)
   {
      tc_startx = dcvars->x;
      tc_tempyl[0] = tc_commontop = dcvars->yl;
      tc_tempyh[0] = tc_commonbot = dcvars->yh;
      tc_temptype = (COL_FUZZ);



      tc_tempfuzzmap = fullcolormap;

      R_FlushWholeColumnsTC = R_FlushWholeFuzzTC;
      R_FlushHTColumnsTC = R_FlushHTFuzzTC;
      R_FlushQuadColumnTC = R_FlushQuadFuzzTC;



   }
   else
   {
      tc_tempyl[tc_temp_x] = dcvars->yl;
      tc_tempyh[tc_temp_x] = dcvars->yh;

      if(dcvars->yl > tc_commontop)
         tc_commontop = dcvars->yl;
      if(dcvars->yh < tc_commonbot)
         tc_commonbot = dcvars->yh;




   }
   tc_temp_x += 1;
}

static void R_DrawFuzzColumnTC_RoundedUV_LinearZ(draw_column_vars_t *dcvars)
{
   int count;
   fixed_t frac;
   const fixed_t fracstep = dcvars->iscale;
   const fixed_t slope_texu = dcvars->texu;

   if (dcvars->iscale > drawvars.mag_threshold)
   {
      R_GetDrawColumnFuncTC(RDC_PIPELINE_FUZZ,
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

   if(tc_temp_x == 4 ||
         (tc_temp_x && (tc_temptype != (COL_FUZZ) || tc_temp_x + tc_startx != dcvars->x)))
      R_FlushColumnsTC();

   if(!tc_temp_x)
   {
      tc_startx = dcvars->x;
      tc_tempyl[0] = tc_commontop = dcvars->yl;
      tc_tempyh[0] = tc_commonbot = dcvars->yh;
      tc_temptype = (COL_FUZZ);

      tc_tempfuzzmap = fullcolormap;

      R_FlushWholeColumnsTC = R_FlushWholeFuzzTC;
      R_FlushHTColumnsTC = R_FlushHTFuzzTC;
      R_FlushQuadColumnTC = R_FlushQuadFuzzTC;
   }
   else
   {
      tc_tempyl[tc_temp_x] = dcvars->yl;
      tc_tempyh[tc_temp_x] = dcvars->yh;

      if(dcvars->yl > tc_commontop)
         tc_commontop = dcvars->yl;
      if(dcvars->yh < tc_commonbot)
         tc_commonbot = dcvars->yh;
   }
   tc_temp_x += 1;
}


static R_DrawColumn_f tc_drawcolumnfuncs[RDRAW_FILTER_MAXFILTERS][RDRAW_FILTER_MAXFILTERS][RDC_PIPELINE_MAXPIPELINES] = {
    {
      {NULL, NULL, NULL},
      {R_DrawColumnTC_PointUV,
       R_DrawTranslatedColumnTC_PointUV,
       R_DrawFuzzColumnTC_PointUV,},
      {R_DrawColumnTC_LinearUV,
       R_DrawTranslatedColumnTC_LinearUV,
       R_DrawFuzzColumnTC_LinearUV,},
      {R_DrawColumnTC_RoundedUV,
       R_DrawTranslatedColumnTC_RoundedUV,
       R_DrawFuzzColumnTC_RoundedUV,},
    },
    {
      {NULL, NULL, NULL},
      {R_DrawColumnTC_PointUV_PointZ,
       R_DrawTranslatedColumnTC_PointUV_PointZ,
       R_DrawFuzzColumnTC_PointUV_PointZ,},
      {R_DrawColumnTC_LinearUV_PointZ,
       R_DrawTranslatedColumnTC_LinearUV_PointZ,
       R_DrawFuzzColumnTC_LinearUV_PointZ,},
      {R_DrawColumnTC_RoundedUV_PointZ,
       R_DrawTranslatedColumnTC_RoundedUV_PointZ,
       R_DrawFuzzColumnTC_RoundedUV_PointZ,},
    },
    {
      {NULL, NULL, NULL},
      {R_DrawColumnTC_PointUV_LinearZ,
       R_DrawTranslatedColumnTC_PointUV_LinearZ,
       R_DrawFuzzColumnTC_PointUV_LinearZ,},
      {R_DrawColumnTC_LinearUV_LinearZ,
       R_DrawTranslatedColumnTC_LinearUV_LinearZ,
       R_DrawFuzzColumnTC_LinearUV_LinearZ,},
      {R_DrawColumnTC_RoundedUV_LinearZ,
       R_DrawTranslatedColumnTC_RoundedUV_LinearZ,
       R_DrawFuzzColumnTC_RoundedUV_LinearZ,},
    },
};

R_DrawColumn_f R_GetDrawColumnFuncTC(enum column_pipeline_e type,
                                   enum draw_filter_type_e filter,
                                   enum draw_filter_type_e filterz) {
  R_DrawColumn_f result = tc_drawcolumnfuncs[filterz][filter][type];
  if (result == NULL)
    I_Error("R_GetDrawColumnFuncTC: undefined function (%d, %d, %d)",
            type, filter, filterz);
  return result;
}

/* Classify a column drawer for the wall-run kernel below: 1 for the
 * unlit point drawer (composed palette), 2 for the lit point drawer
 * (composed colormap), 0 for everything else.  Only these two are
 * reproduced by R_DrawWallColumnRunTC; records using any other drawer
 * replay individually. */
int R_WallColumnKernelClassTC(R_DrawColumn_f fn)
{
  if (fn == R_DrawColumnTC_PointUV)
    return 1;
  if (fn == R_DrawColumnTC_PointUV_PointZ)
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

void R_DrawWallColumnRunTC(const draw_column_vars_t *const *cols, int n, int pointz)
{
  const uint8_t     *src[WALL_RUN_MAX];
  const lighttable_t *cmap[WALL_RUN_MAX];
  fixed_t            frac[WALL_RUN_MAX];
  fixed_t            step[WALL_RUN_MAX];
  unsigned int       mask[WALL_RUN_MAX];
  int                cyl[WALL_RUN_MAX];
  int                cyh[WALL_RUN_MAX];
  const uint32_t    *lut = NULL;
  const uint32_t    *lanelut[WALL_RUN_MAX];
  int                lane_mode = 0;
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
   * V_PaletteTC[colormap[texel]*64 + 63], the same values either way. */
  if (!pointz)
    lut = R_GetComposedPaletteTC();
  else
  {
    for (j = 1; j < n; j++)
      if (cmap[j] != cmap[0])
        break;
    if (j == n)
      lut = R_GetComposedColormapTC(cmap[0]);
  }

  /* Dynamic-light colour tint (dcvars.tint, packed r:g:b channel adds).
   * The kernel writes exactly lut[texel] per pixel, so folding the tint into
   * the table -- clamp(lut[i] + tint) per channel -- yields bit-identical
   * pixels to the old post-pass framebuffer RMW while touching 256 entries
   * instead of every drawn pixel.  The uniform case (one colormap, one tint
   * across the run: adjacent wall columns in the same light pool) tints the
   * shared table once and falls through to the unchanged fast paths below.
   * Mixed runs resolve a small per-run pool of tinted tables with lane
   * sharing; if the pool ever overflows, the excess lanes draw untinted and
   * record the old RMW tint instead (the replay pass runs after this), so the
   * output cannot change, only the route. */
  {
    unsigned tint0 = cols[0]->tint;
    unsigned anytint = 0;
    int      same = 1;
    for (j = 0; j < n; j++)
    {
      anytint |= cols[j]->tint;
      if (cols[j]->tint != tint0)
        same = 0;
    }
    if (anytint)
    {
      static uint32_t tintbuf[256];
      if (lut && same)
      {
        R_TintLUTTC(tintbuf, lut,
                  (int)(tint0 >> (2*VID_TINT_BITS)) & VID_TINT_MASK,
                  (int)(tint0 >> VID_TINT_BITS) & VID_TINT_MASK,
                  (int)tint0 & VID_TINT_MASK);
        lut = tintbuf;
      }
      else
      {
#define WALL_TINT_POOL 8
        static uint32_t           pool[WALL_TINT_POOL][256];
        const lighttable_t       *pool_cm[WALL_TINT_POOL];
        unsigned                  pool_tint[WALL_TINT_POOL];
        int pooln = 0, k;

        for (j = 0; j < n; j++)
        {
          unsigned t = cols[j]->tint;
          if (!t)
          {
            lanelut[j] = NULL;          /* per-pixel defining expression */
            continue;
          }
          for (k = 0; k < pooln; k++)
            if (pool_cm[k] == cmap[j] && pool_tint[k] == t)
              break;
          if (k < pooln)
            lanelut[j] = pool[k];
          else if (pooln < WALL_TINT_POOL)
          {
            R_TintLUTTC(pool[pooln], R_GetComposedColormapTC(cmap[j]),
                      (int)(t >> (2*VID_TINT_BITS)) & VID_TINT_MASK,
                      (int)(t >> VID_TINT_BITS) & VID_TINT_MASK,
                      (int)t & VID_TINT_MASK);
            pool_cm[pooln] = cmap[j];
            pool_tint[pooln] = t;
            lanelut[j] = pool[pooln++];
          }
          else
          {
            /* pool exhausted: draw untinted, tint via the RMW replay pass */
            lanelut[j] = NULL;
            R_WallTintRecord(cols[j]->x, cyl[j], cyh[j],
                             (int)(t >> (2*VID_TINT_BITS)) & VID_TINT_MASK,
                             (int)(t >> VID_TINT_BITS) & VID_TINT_MASK,
                             (int)t & VID_TINT_MASK);
          }
        }
        lane_mode = 1;
      }
    }
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
    uint32_t *row = ((uint32_t *)drawvars.int_topleft)                 \
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
    uint32_t *row = ((uint32_t *)drawvars.int_topleft)                 \
                  + y * SURFACE_SHORT_PITCH + x0;          \
    for (j = 0; j < n; j++)                                \
    {                                                      \
      int texel = src[j][(int)(frac[j] & mask[j]) >> 16];   \
      row[j] = EXPR;                                       \
      frac[j] += step[j];                                  \
    }                                                      \
  }

  if (lane_mode)
  {
    /* Mixed colormaps/tints: every lane writes through its own resolved
     * table (or the defining expression when untinted); coverage-tested
     * everywhere, which is exact for the dense band too. */
    for (y = ymin; y <= ymax; y++)
      WALL_RUN_RAGGED_ROW(lanelut[j]
                          ? lanelut[j][texel]
                          : (pointz ? V_PaletteTC[ cmap[j][texel] * 64 + 63 ]
                                    : R_GetComposedPaletteTC()[texel]))
    return;
  }

  if (dtop > dbot)
  {
    /* No row covers every lane; the whole run is ragged. */
    if (lut)
      for (y = ymin; y <= ymax; y++)
        WALL_RUN_RAGGED_ROW(lut[texel])
    else
      for (y = ymin; y <= ymax; y++)
        WALL_RUN_RAGGED_ROW(V_PaletteTC[ cmap[j][texel] * 64 + 63 ])
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
      uint32_t *row = ((uint32_t *)drawvars.int_topleft)
                    + y * SURFACE_SHORT_PITCH + x0;
      int j4 = n & ~3;
      for (j = 0; j < j4; j += 4)
      {
        int32_t      idx4[4];
        uint32_t     out4[4];
        WALL_RUN_VEC4(j)
        out4[0] = lut[src[j + 0][idx4[0]]];
        out4[1] = lut[src[j + 1][idx4[1]]];
        out4[2] = lut[src[j + 2][idx4[2]]];
        out4[3] = lut[src[j + 3][idx4[3]]];
        memcpy(&row[j], out4, 4 * sizeof(uint32_t));
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
      WALL_RUN_RAGGED_ROW(V_PaletteTC[ cmap[j][texel] * 64 + 63 ])
    for (y = dtop; y <= dbot; y++)
      WALL_RUN_DENSE_ROW(V_PaletteTC[ cmap[j][texel] * 64 + 63 ])
    for (y = dbot + 1; y <= ymax; y++)
      WALL_RUN_RAGGED_ROW(V_PaletteTC[ cmap[j][texel] * 64 + 63 ])
  }

#undef WALL_RUN_RAGGED_ROW
#undef WALL_RUN_DENSE_ROW
}

//  the texture at an angle in all but a few cases.
// In consequence, flats are not stored by column (like walls),
//  and the inner loop has to step in texture space u and v.
//

static void R_DrawSpanTC_PointUV_PointZ(draw_span_vars_t *dsvars)
{
   unsigned count = dsvars->x2 - dsvars->x1 + 1;
   fixed_t xfrac = dsvars->xfrac;
   fixed_t yfrac = dsvars->yfrac;
   const fixed_t xstep = dsvars->xstep;
   const fixed_t ystep = dsvars->ystep;
   const uint8_t *source = dsvars->source;

   uint32_t *dest = ((uint32_t *)drawvars.int_topleft) + dsvars->y* SCREENWIDTH + dsvars->x1;

   /* Shared composed colormap+palette table (see R_GetComposedColormapTC):
    * collapses V_PaletteTC[colormap[texel]*64+63] to one lookup, rebuilt
    * only when the colormap pointer or V_PaletteTC changes. */
   const uint32_t *lut = R_GetComposedColormapTC(dsvars->colormap);

   /* Brightmap path: where the 64x64 row-major mask is set, the texel is
    * drawn through the undimmed base map (fullcolormap) instead of the
    * distance-lit table.  fullcolormap's composed table is snapshot into
    * a local first (the shared tc_composed_lut cache is single-entry, so
    * fetching the distance `lut` would evict it).  Kept scalar; the SIMD
    * select lands in a later step.  NULL mask -> the vectorised path
    * below runs unchanged. */
   if (dsvars->brightmask)
   {
      const uint8_t  *mask = dsvars->brightmask;
      uint32_t        lut_bright[256];
      const uint32_t *bsrc = R_GetComposedColormapTC(fullcolormap
                                                   ? fullcolormap
                                                   : dsvars->colormap);
      memcpy(lut_bright, bsrc, sizeof(lut_bright));
      lut = R_GetComposedColormapTC(dsvars->colormap);

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

static void R_DrawSpanTC_PointUV_LinearZ(draw_span_vars_t *dsvars)
{
   /* Brightmapped flats take the point/point span drawer, the only
    * one with the fullbright select; filtered spans blend several
    * lookups per pixel with no single table to redirect, so a masked
    * flat trades that smoothing for correct fullbright. */
   if (dsvars->brightmask)
   {
      R_DrawSpanTC_PointUV_PointZ(dsvars);
      return;
   }
   {
   unsigned count = dsvars->x2 - dsvars->x1 + 1;
   fixed_t xfrac = dsvars->xfrac;
   fixed_t yfrac = dsvars->yfrac;
   const fixed_t xstep = dsvars->xstep;
   const fixed_t ystep = dsvars->ystep;
   const uint8_t *source = dsvars->source;




   uint32_t *dest = ((uint32_t *)drawvars.int_topleft) + dsvars->y* SCREENWIDTH + dsvars->x1;

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
      *dest++ = V_PaletteTC[ (dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x1)&(4 -1)] < (fracz)) ? 1 : 0)][(source[spot])])*64 + ((64 -1)) ];
      count--;

      x1--;


   }
   }
}

static void R_DrawSpanTC_LinearUV_PointZ(draw_span_vars_t *dsvars)
{
   /* Brightmapped flats take the point/point span drawer, the only
    * one with the fullbright select; filtered spans blend several
    * lookups per pixel with no single table to redirect, so a masked
    * flat trades that smoothing for correct fullbright. */
   if (dsvars->brightmask)
   {
      R_DrawSpanTC_PointUV_PointZ(dsvars);
      return;
   }
   if ((D_abs(dsvars->xstep) > drawvars.mag_threshold)
         || (D_abs(dsvars->ystep) > drawvars.mag_threshold))
   {
      R_GetDrawSpanFuncTC(RDRAW_FILTER_POINT,
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

      uint32_t *dest = ((uint32_t *)drawvars.int_topleft) + dsvars->y* SCREENWIDTH + dsvars->x1;

      while (count) {


         *dest++ = ( V_PaletteTC[ (colormap[(source[ ((((xfrac)+(1<<16))>>16)&0x3f) | ((((yfrac)+(1<<16))>>10)&0xfc0)])])*64 + ((unsigned int)(((xfrac)&0xffff)*((yfrac)&0xffff))>>(32-6)) ] + V_PaletteTC[ (colormap[(source[ (((xfrac)>>16)&0x3f) | ((((yfrac)+(1<<16))>>10)&0xfc0)])])*64 + ((unsigned int)((0xffff-((xfrac)&0xffff))*((yfrac)&0xffff))>>(32-6)) ] + V_PaletteTC[ (colormap[(source[ (((xfrac)>>16)&0x3f) | (((yfrac)>>10)&0xfc0)])])*64 + ((unsigned int)((0xffff-((xfrac)&0xffff))*(0xffff-((yfrac)&0xffff)))>>(32-6)) ] + V_PaletteTC[ (colormap[(source[ ((((xfrac)+(1<<16))>>16)&0x3f) | (((yfrac)>>10)&0xfc0)])])*64 + ((unsigned int)(((xfrac)&0xffff)*(0xffff-((yfrac)&0xffff)))>>(32-6)) ]);
         xfrac += xstep;
         yfrac += ystep;
         count--;
      }
   }
}

static void R_DrawSpanTC_LinearUV_LinearZ(draw_span_vars_t *dsvars)
{
   /* Brightmapped flats take the point/point span drawer, the only
    * one with the fullbright select; filtered spans blend several
    * lookups per pixel with no single table to redirect, so a masked
    * flat trades that smoothing for correct fullbright. */
   if (dsvars->brightmask)
   {
      R_DrawSpanTC_PointUV_PointZ(dsvars);
      return;
   }
   if ((D_abs(dsvars->xstep) > drawvars.mag_threshold)
         || (D_abs(dsvars->ystep) > drawvars.mag_threshold))
   {
      R_GetDrawSpanFuncTC(RDRAW_FILTER_POINT,
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




      uint32_t *dest = ((uint32_t *)drawvars.int_topleft) + dsvars->y* SCREENWIDTH + dsvars->x1;

      const int y = dsvars->y;
      int x1 = dsvars->x1;


      const int fracz = (dsvars->z >> 12) & 255;
      const uint8_t *dither_colormaps[2] = { dsvars->colormap, dsvars->nextcolormap };


      while (count) {


         *dest++ = ( V_PaletteTC[ (dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x1)&(4 -1)] < (fracz)) ? 1 : 0)][(source[ ((((xfrac)+(1<<16))>>16)&0x3f) | ((((yfrac)+(1<<16))>>10)&0xfc0)])])*64 + ((unsigned int)(((xfrac)&0xffff)*((yfrac)&0xffff))>>(32-6)) ] + V_PaletteTC[ (dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x1)&(4 -1)] < (fracz)) ? 1 : 0)][(source[ (((xfrac)>>16)&0x3f) | ((((yfrac)+(1<<16))>>10)&0xfc0)])])*64 + ((unsigned int)((0xffff-((xfrac)&0xffff))*((yfrac)&0xffff))>>(32-6)) ] + V_PaletteTC[ (dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x1)&(4 -1)] < (fracz)) ? 1 : 0)][(source[ (((xfrac)>>16)&0x3f) | (((yfrac)>>10)&0xfc0)])])*64 + ((unsigned int)((0xffff-((xfrac)&0xffff))*(0xffff-((yfrac)&0xffff)))>>(32-6)) ] + V_PaletteTC[ (dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x1)&(4 -1)] < (fracz)) ? 1 : 0)][(source[ ((((xfrac)+(1<<16))>>16)&0x3f) | (((yfrac)>>10)&0xfc0)])])*64 + ((unsigned int)(((xfrac)&0xffff)*(0xffff-((yfrac)&0xffff)))>>(32-6)) ]);
         xfrac += xstep;
         yfrac += ystep;
         count--;

         x1--;
      }
   }
}

static void R_DrawSpanTC_RoundedUV_PointZ(draw_span_vars_t *dsvars)
{
   /* Brightmapped flats take the point/point span drawer, the only
    * one with the fullbright select; filtered spans blend several
    * lookups per pixel with no single table to redirect, so a masked
    * flat trades that smoothing for correct fullbright. */
   if (dsvars->brightmask)
   {
      R_DrawSpanTC_PointUV_PointZ(dsvars);
      return;
   }
   if ((D_abs(dsvars->xstep) > drawvars.mag_threshold)
         || (D_abs(dsvars->ystep) > drawvars.mag_threshold))
   {
      R_GetDrawSpanFuncTC(RDRAW_FILTER_POINT,
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

      uint32_t *dest = ((uint32_t *)drawvars.int_topleft) + dsvars->y* SCREENWIDTH + dsvars->x1;
      while (count) {
         *dest++ = V_PaletteTC[ (colormap[(filter_getScale2xQuadColors( source[ (((xfrac)>>16)&0x3f) | (((yfrac)>>10)&0xfc0) ], source[ (((xfrac)>>16)&0x3f) | ((((yfrac)-(1<<16))>>10)&0xfc0) ], source[ ((((xfrac)+(1<<16))>>16)&0x3f) | (((yfrac)>>10)&0xfc0) ], source[ (((xfrac)>>16)&0x3f) | ((((yfrac)+(1<<16))>>10)&0xfc0) ], source[ ((((xfrac)-(1<<16))>>16)&0x3f) | (((yfrac)>>10)&0xfc0) ] ) [ filter_roundedUVMap[ (((((xfrac)>>8) & 0xff)>>(8-6))<<6) + ((((yfrac)>>8) & 0xff)>>(8-6)) ] ])])*64 + ((64 -1)) ];
         xfrac += xstep;
         yfrac += ystep;
         count--;
      }
   }
}

static void R_DrawSpanTC_RoundedUV_LinearZ(draw_span_vars_t *dsvars)
{
   /* Brightmapped flats take the point/point span drawer, the only
    * one with the fullbright select; filtered spans blend several
    * lookups per pixel with no single table to redirect, so a masked
    * flat trades that smoothing for correct fullbright. */
   if (dsvars->brightmask)
   {
      R_DrawSpanTC_PointUV_PointZ(dsvars);
      return;
   }
   if ((D_abs(dsvars->xstep) > drawvars.mag_threshold)
         || (D_abs(dsvars->ystep) > drawvars.mag_threshold))
   {
      R_GetDrawSpanFuncTC(RDRAW_FILTER_POINT,
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




      uint32_t *dest = ((uint32_t *)drawvars.int_topleft) + dsvars->y* SCREENWIDTH + dsvars->x1;

      const int y = dsvars->y;
      int x1 = dsvars->x1;


      const int fracz = (dsvars->z >> 12) & 255;
      const uint8_t *dither_colormaps[2] = { dsvars->colormap, dsvars->nextcolormap };


      while (count) {
         *dest++ = V_PaletteTC[ (dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x1)&(4 -1)] < (fracz)) ? 1 : 0)][(filter_getScale2xQuadColors( source[ (((xfrac)>>16)&0x3f) | (((yfrac)>>10)&0xfc0) ], source[ (((xfrac)>>16)&0x3f) | ((((yfrac)-(1<<16))>>10)&0xfc0) ], source[ ((((xfrac)+(1<<16))>>16)&0x3f) | (((yfrac)>>10)&0xfc0) ], source[ (((xfrac)>>16)&0x3f) | ((((yfrac)+(1<<16))>>10)&0xfc0) ], source[ ((((xfrac)-(1<<16))>>16)&0x3f) | (((yfrac)>>10)&0xfc0) ] ) [ filter_roundedUVMap[ (((((xfrac)>>8) & 0xff)>>(8-6))<<6) + ((((yfrac)>>8) & 0xff)>>(8-6)) ] ])])*64 + ((64 -1)) ];
         xfrac += xstep;
         yfrac += ystep;
         count--;

         x1--;
      }
   }
}


/* r_span_translucent is defined in r_draw.c (shared render state). */
extern int r_span_translucent;

static R_DrawSpan_f tc_drawspanfuncs[RDRAW_FILTER_MAXFILTERS][RDRAW_FILTER_MAXFILTERS] = {
    {
      NULL,
      NULL,
      NULL,
      NULL,
    },
    {
      NULL,
      R_DrawSpanTC_PointUV_PointZ,
      R_DrawSpanTC_LinearUV_PointZ,
      R_DrawSpanTC_RoundedUV_PointZ,
    },
    {
      NULL,
      R_DrawSpanTC_PointUV_LinearZ,
      R_DrawSpanTC_LinearUV_LinearZ,
      R_DrawSpanTC_RoundedUV_LinearZ,
    },
    {
      NULL,
      NULL,
      NULL,
      NULL,
    },
};

R_DrawSpan_f R_GetDrawSpanFuncTC(enum draw_filter_type_e filter,
                               enum draw_filter_type_e filterz) {
  R_DrawSpan_f result = tc_drawspanfuncs[filterz][filter];
  if (result == NULL)
    I_Error("R_GetDrawSpanFuncTC: undefined function (%d, %d)",
            filter, filterz);
  return result;
}

void R_DrawSpanTC(draw_span_vars_t *dsvars) {
  if (r_span_translucent) {
    R_DrawSpanTC_TL(dsvars);
    return;
  }
  R_GetDrawSpanFuncTC(drawvars.filterfloor, drawvars.filterz)(dsvars);
}

/* ===== END GENERATED BODY ===== */

/* =========================================================================
 *  Public entry points (called from r_draw.c when VID_TRUECOLOR).
 *
 *  These wrap the per-format kernels / shared body functions with the
 *  vid_mode dispatch, and mirror the r_draw.c public API one-for-one so
 *  the branch in r_draw.c is a straight "if truecolor call the TC twin".
 * ========================================================================= */

/* --- composed-LUT public linkage (sprite/voxel direct paths) -------------- */
const uint32_t *R_ComposedColormapTC(const lighttable_t *colormap)
{
  return R_GetComposedColormapTC(colormap);
}

const uint32_t *R_ComposedPaletteTC(void)
{
  return R_GetComposedPaletteTC();
}

/* --- underwater tint + flat average --------------------------------------- */
uint32_t R_FlatAverageColorTC(int picnum)
{
  return (vid_mode == VID_MODE2101010) ? R_FlatAverageColorA2(picnum)
                                       : R_FlatAverageColor8888(picnum);
}

void R_TintViewTC(uint32_t color)
{
  if (vid_mode == VID_MODE2101010) R_TintViewA2(color);
  else                             R_TintView8888(color);
}

/* --- dynamic-light tints -------------------------------------------------- */
void R_TintLUTTC(uint32_t *dst, const uint32_t *src, int ar, int ag, int ab)
{
  if (vid_mode == VID_MODE2101010) R_TintLUTA2(dst, src, ar, ag, ab);
  else                             R_TintLUT8888(dst, src, ar, ag, ab);
}

void R_WallTintRunTC(int x, int yl, int yh, int ar, int ag, int ab)
{
  if (vid_mode == VID_MODE2101010) R_WallTintRunA2(x, yl, yh, ar, ag, ab);
  else                             R_WallTintRun8888(x, yl, yh, ar, ag, ab);
}

void R_TintSpanTC(int y, int x1, int x2, int ar, int ag, int ab)
{
  if (vid_mode == VID_MODE2101010) R_TintSpanA2(y, x1, x2, ar, ag, ab);
  else                             R_TintSpan8888(y, x1, x2, ar, ag, ab);
}

/* --- water volume shading ------------------------------------------------- */
void R_WaterDarkenSpanTC(int y, int x1, int x2, int surf_y)
{
  if (vid_mode == VID_MODE2101010) R_WaterDarkenSpanA2(y, x1, x2, surf_y);
  else                             R_WaterDarkenSpan8888(y, x1, x2, surf_y);
}

void R_WaterSurfaceBandTC(int x, int yl, int yh, int surf_line, int band_h)
{
  if (vid_mode == VID_MODE2101010) R_WaterSurfaceBandA2(x, yl, yh, surf_line, band_h);
  else                             R_WaterSurfaceBand8888(x, yl, yh, surf_line, band_h);
}

void R_WaterSurfaceLiftTC(int x, int y0, int y1, int bandtop)
{
  if (vid_mode == VID_MODE2101010) R_WaterSurfaceLiftA2(x, y0, y1, bandtop);
  else                             R_WaterSurfaceLift8888(x, y0, y1, bandtop);
}

void R_WaterDarkenColumnTC(int x, int yl, int yh, int surf_y)
{
  if (vid_mode == VID_MODE2101010) R_WaterDarkenColumnA2(x, yl, yh, surf_y);
  else                             R_WaterDarkenColumn8888(x, yl, yh, surf_y);
}

/* --- buffer init (fuzz offset table, keyed to the current pitch) ---------- */
/* Mirror of r_draw.c's R_InitBuffer for the fuzz-offset scaling; the
 * short/int topleft latch is done once in r_draw.c's R_InitBuffer (it sets
 * both pointer views from screens[0].data), so we only refresh the fuzz
 * offsets against SURFACE_SHORT_PITCH. */
static const int tc_fuzzoffset_seed[TC_FUZZTABLE] = {
  TC_FUZZOFF,-TC_FUZZOFF,TC_FUZZOFF,-TC_FUZZOFF,TC_FUZZOFF,TC_FUZZOFF,-TC_FUZZOFF,
  TC_FUZZOFF,TC_FUZZOFF,-TC_FUZZOFF,TC_FUZZOFF,TC_FUZZOFF,TC_FUZZOFF,-TC_FUZZOFF,
  TC_FUZZOFF,TC_FUZZOFF,TC_FUZZOFF,-TC_FUZZOFF,-TC_FUZZOFF,-TC_FUZZOFF,-TC_FUZZOFF,
  TC_FUZZOFF,-TC_FUZZOFF,-TC_FUZZOFF,TC_FUZZOFF,TC_FUZZOFF,TC_FUZZOFF,TC_FUZZOFF,-TC_FUZZOFF,
  TC_FUZZOFF,-TC_FUZZOFF,TC_FUZZOFF,TC_FUZZOFF,-TC_FUZZOFF,-TC_FUZZOFF,TC_FUZZOFF,
  TC_FUZZOFF,-TC_FUZZOFF,-TC_FUZZOFF,-TC_FUZZOFF,-TC_FUZZOFF,TC_FUZZOFF,TC_FUZZOFF,
  TC_FUZZOFF,TC_FUZZOFF,-TC_FUZZOFF,TC_FUZZOFF,TC_FUZZOFF,-TC_FUZZOFF,TC_FUZZOFF
};

void R_InitBufferTC(void)
{
  int i;
  for (i = 0; i < TC_FUZZTABLE; i++)
    tc_fuzzoffset[i] = tc_fuzzoffset_seed[i] * SURFACE_SHORT_PITCH;
  R_InitTransStateTC();
}
