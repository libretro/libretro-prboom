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
 *      Rendering main loop and setup functions,
 *       utility functions (BSP, geometry, trigonometry).
 *      See tables.c, too.
 *
 *-----------------------------------------------------------------------------*/


#include "config.h"
#include "doomstat.h"
#include "d_net.h"
#include "w_wad.h"
#include "r_main.h"
#include <stdlib.h>
#include "r_drawcmd.h"
#include "r_data.h"
#include "u_brightmap.h"
#include "r_state.h"
#include "r_things.h"
#include "r_dynlight.h"
#include "r_plane.h"
#include "r_bsp.h"
#include "r_draw.h"
#include "r_drawtc.h"
#include "vid_mode.h"
#include "p_skybox.h"
#include "p_sectorportal.h"
#include "p_lineportal.h"
#include "p_ffloor.h"
#include "m_bbox.h"
#include "r_sky.h"
#include "v_video.h"
#include "lprintf.h"
#include "st_stuff.h"
#include "i_main.h"
#include "i_system.h"
#include "g_game.h"
#include "r_demo.h"
#include "r_fps.h"

// Fineangles in the SCREENWIDTH wide window.
#define FIELDOFVIEW 2048

// killough: viewangleoffset is a legacy from the pre-v1.2 days, when Doom
// had Left/Mid/Right viewing. +/-ANG90 offsets were placed here on each
// node, by d_net.c, to set up a L/M/R session.

int viewangleoffset, viewpitchoffset;
int validcount = 1;         // increment every time a check is made
const lighttable_t *fixedcolormap;
int      centerx, centery;
fixed_t  centerxfrac, centeryfrac;
fixed_t  viewheightfrac; //e6y: for correct clipping of things
fixed_t  projection;
/* Horizontal projection anchor for sprite x-placement.  Equals
 * focallength, so sprites use the same horizontal scale as walls.  At
 * 4:3 this equals centerxfrac (== projection); under hor+ widescreen
 * focallength is smaller, and using it here keeps sprites aligned with
 * the widened wall projection instead of drifting toward the edge. */
fixed_t  projectionx;
// proff 11/06/98: Added for high-res
fixed_t  projectiony;
fixed_t  skyiscale;
fixed_t  viewx, viewy, viewz;
angle_t  viewangle;
angle_t  viewpitch;
fixed_t  viewcos, viewsin;
player_t *viewplayer;
fixed_t  focallength;
int      fieldofview;
/* Display aspect ratio selector (in-game General/Video menu).
 * 0 = 4:3, 1 = 16:9, 2 = 16:10, 3 = 32:9, 4 = 21:9.  The libretro layer reads
 * this to size the render buffer width; the renderer itself derives
 * FOV from the resulting buffer dimensions and needs no other input. */
int      render_aspect;
int      r_in_skybox;   /* set while rendering the 3D-skybox pass */
extern lighttable_t **walllights;

//
// precalculated math tables
//

angle_t clipangle;

// The viewangletox[viewangle + FINEANGLES/4] lookup
// maps the visible view angles to screen X coordinates,
// flattening the arc to a flat projection plane.
// There will be many angles mapped to the same X.

int viewangletox[FINEANGLES/2];

// The xtoviewangleangle[] table maps a screen pixel
// to the lowest viewangle that maps back to x ranges
// from clipangle to -clipangle.

angle_t xtoviewangle[MAX_SCREENWIDTH+1];   // killough 2/8/98

// killough 3/20/98: Support dynamic colormaps, e.g. deep water
// killough 4/4/98: support dynamic number of them as well

int numcolormaps;
const lighttable_t *(*c_zlight)[LIGHTLEVELS][MAXLIGHTZ];
const lighttable_t *(*zlight)[MAXLIGHTZ];
const lighttable_t *fullcolormap;
const lighttable_t **colormaps;

// killough 3/20/98, 4/4/98: end dynamic colormaps

int extralight;                           // bumped light from gun blasts

/*
===============================================================================
=
= R_PointOnSide
=  Traverse BSP (sub) tree,
=  check point against partition plane.
=
= Returns side 0 (front) or 1 (back)
===============================================================================
*/

int R_PointOnSide(fixed_t x, fixed_t y, const node_t *node)
{
  if (!node->dx)
  {
     if (x <= node->x)
        return node->dy > 0;
     return node->dy < 0;
  }

  if (!node->dy)
  {
     if (y <= node->y)
        return node->dx < 0;
     return node->dx > 0;
  }

  /* Offset the point from the partition in 64-bit.  ZDoom XGL3 extended GL
   * nodes store the partition line in 16.16 fixed point, so on a large map
   * node->x/y reach ~10^9 and the vanilla 32-bit "x -= node->x" overflows
   * when the point and partition straddle the origin -- the wrong child is
   * taken and the traversal ends in the wrong subsector. */
  {
    int64_t dx64 = (int64_t)x - node->x;
    int64_t dy64 = (int64_t)y - node->y;

    /* Classic nodes (and any point near the partition) stay within 32 bits:
     * run the exact vanilla computation so demo-compatible maps are
     * bit-for-bit unchanged. */
    if (dx64 == (fixed_t)dx64 && dy64 == (fixed_t)dy64)
    {
      fixed_t dx = (fixed_t)dx64;
      fixed_t dy = (fixed_t)dy64;

      // Try to quickly decide by looking at sign bits.
      if ((node->dy ^ node->dx ^ dx ^ dy) < 0)
        return (node->dy ^ dx) < 0;  // (left is negative)
      return FixedMul(dy, node->dx>>FRACBITS) >= FixedMul(node->dy>>FRACBITS, dx);
    }

    /* Large coordinates: evaluate the same half-plane test in 64-bit, with
     * the partition delta reduced to whole units exactly as the vanilla path
     * does via >>FRACBITS, so the cross-product sign is preserved without
     * overflow. */
    {
      int64_t left  = (dy64 >> FRACBITS) * (int64_t)(node->dx >> FRACBITS);
      int64_t right = (int64_t)(node->dy >> FRACBITS) * (dx64 >> FRACBITS);
      return left >= right ? 1 : 0;
    }
  }
}

// killough 5/2/98: reformatted

int R_PointOnSegSide(fixed_t x, fixed_t y, const seg_t *line)
{
  fixed_t lx = line->v1->x;
  fixed_t ly = line->v1->y;
  fixed_t ldx = line->v2->x - lx;
  fixed_t ldy = line->v2->y - ly;

  if (!ldx)
    return x <= lx ? ldy > 0 : ldy < 0;

  if (!ldy)
    return y <= ly ? ldx < 0 : ldx > 0;

  x -= lx;
  y -= ly;

  // Try to quickly decide by looking at sign bits.
  if ((ldy ^ ldx ^ x ^ y) < 0)
    return (ldy ^ x) < 0;          // (left is negative)
  return FixedMul(y, ldx>>FRACBITS) >= FixedMul(ldy>>FRACBITS, x);
}

extern angle_t tantoangle[2049];

int SlopeDiv(unsigned num, unsigned den)
{
  unsigned ans;

  if (den < 512)
    return SLOPERANGE;
  ans = (num<<3)/(den>>8);
  return ans <= SLOPERANGE ? ans : SLOPERANGE;
}

//
// R_PointToAngle
// To get a global angle from cartesian coordinates,
//  the coordinates are flipped until they are in
//  the first octant of the coordinate system, then
//  the y (<=x) is scaled and divided by x to get a
//  tangent (slope) value which is looked up in the
//  tantoangle[] table. The +1 size of tantoangle[]
//  is to handle the case when x==y without additional
//  checking.
//
// killough 5/2/98: reformatted, cleaned up

angle_t R_PointToAngle (fixed_t x, fixed_t y)
{
   x -= viewx;
   y -= viewy;

   if ( (!x) && (!y) )
      return 0;

   if (x >= 0)
   {
      if (y>= 0)
      {
         /* y>= 0 */
         if (x>y)
            return tantoangle[ SlopeDiv(y,x)];     /* octant 0 */
         return ANG90-1-tantoangle[ SlopeDiv(x,y)];  /* octant 1 */
      }

      /* y<0 */
      y = -y;
      if (x>y)
         return -tantoangle[SlopeDiv(y,x)];  /* octant 8 */
      return ANG270+tantoangle[ SlopeDiv(x,y)];  /* octant 7 */
   }

   /* x < 0 */
   x = -x;

   if (y>= 0)
   {
      if (x>y)
         return ANG180-1-tantoangle[ SlopeDiv(y,x)]; /* octant 3 */
      return ANG90+ tantoangle[ SlopeDiv(x,y)];  /* octant 2 */
   }

   /* y < 0 */
   y = -y;
   if (x>y)
      return ANG180+tantoangle[ SlopeDiv(y,x)]; /* octant 4 */
   return ANG270-1-tantoangle[ SlopeDiv(x,y)];  /* octant 5 */
}

angle_t R_PointToAngle2(fixed_t x1, fixed_t y1, fixed_t x2, fixed_t y2)
{
   int x = x2 - x1;
   int y = y2 - y1;

   if ( (!x) && (!y))
      return 0;

   if (x >= 0)
   { /* x >= 0 */
      if (y >= 0)
      {
         /* y >= 0 */
         if (x > y)
            return tantoangle[SlopeDiv(y,x)];                      /* octant 0 */
         return ANG90-1-tantoangle[SlopeDiv(x,y)];                 /* octant 1 */
      }
      else
      { /* y < 0 */
         y = -y;
         if (x > y)
            return -tantoangle[SlopeDiv(y,x)];                     /* octant 8 */
         return ANG270+tantoangle[SlopeDiv(x,y)];                  /* octant 7 */
      }
   }
   else
   {
      /* x < 0 */
      x = -x;
      if (y >= 0)
      {
         /* y >= 0 */
         if (x > y)
            return ANG180-1-tantoangle[SlopeDiv(y,x)];            /* octant 3 */
         return ANG90 + tantoangle[SlopeDiv(x,y)];                /* octant 2 */
      }
      else
      {
         /* y < 0 */
         y = -y;
         if (x > y)
            return ANG180+tantoangle[ SlopeDiv(y,x)];             /* octant 4 */
         return ANG270-1-tantoangle[SlopeDiv(x,y)];               /* octant 5 */
      }
   }
   return 0;
}

//
// R_InitTextureMapping
//
// killough 5/2/98: reformatted

static void R_InitTextureMapping (void)
{
  int i,x;
  fieldofview = FIELDOFVIEW;

  // Use tangent table to generate viewangletox:
  //  viewangletox will give the next greatest x
  //  after the view angle.
  //
  // Calc focallength
  //  so FIELDOFVIEW angles covers SCREENWIDTH.
  //
  // For widescreen (buffer wider than 4:3 for the current height) we
  // derive focallength from the 4:3-equivalent half-width instead of
  // the real half-width.  This keeps the vertical FOV fixed at the
  // vanilla value and widens the horizontal FOV to match the buffer
  // aspect (hor+), rather than stretching the 90 degree image.

  {
    /* hor+ widescreen: widen the horizontal FOV for wider-than-4:3
     * buffers while keeping the vertical FOV at the vanilla value.
     *
     * The buffer width was produced by scaling a 4:3 reference width
     * by render_aspect's ratio (num/den vs 4:3).  Recover the 4:3
     * reference half-width by dividing centerx back down by that same
     * ratio, then anchor focallength to it.  At 4:3 the ratio is 1:1
     * so cx43frac == centerxfrac and the result is bit-identical to
     * vanilla. */
    static const int num[5] = { 4, 16, 16, 32, 64 };
    static const int den[5] = { 3,  9, 10,  9, 27 };
    int a = render_aspect;
    fixed_t cx43frac;

    if (a < 0 || a > 4)
      a = 0;

    /* centerx_4:3 = centerx * (4:3) / (num/den)
     *            = centerx * 4 * den / (3 * num) */
    cx43frac = (fixed_t)(((int64_t)centerx * 4 * den[a]) / (3 * num[a])) << FRACBITS;
    if (cx43frac <= 0 || cx43frac > centerxfrac)
      cx43frac = centerxfrac;
    focallength = FixedDiv(cx43frac, finetangent[FINEANGLES/4 + fieldofview/2]);

    /* hor+ widens focallength so the real screen edge (centerx) sits
     * at a larger angle than half of the vanilla fieldofview.  The
     * viewangletox clamp below rejects any finetangent[] beyond
     * finetangent[FINEANGLES/4 + fieldofview/2] as off-screen, so with
     * the unwidened fieldofview the outermost columns get no angle
     * mapping and are never drawn (HOM at the left/right edges).
     * Widen fieldofview to the angle actually subtended by the full
     * half-width: find the fine angle whose tangent reaches
     * centerx/focallength and set fieldofview to twice its offset from
     * the centre.  At 4:3 this reproduces the vanilla fieldofview. */
    if (cx43frac < centerxfrac)
    {
      fixed_t edgetan = FixedDiv(centerxfrac, focallength);
      int hi = FINEANGLES/4;
      while (hi < FINEANGLES/2 - 1 && finetangent[hi] < edgetan)
        hi++;
      fieldofview = 2 * (hi - FINEANGLES/4);
      if (fieldofview >= FINEANGLES/2)
        fieldofview = FINEANGLES/2 - 1;
    }
  }

  for (i=0 ; i<FINEANGLES/2 ; i++)
    {
      int t;
      int limit = finetangent[FINEANGLES/4 + fieldofview/2];
      if (finetangent[i] > limit)
        t = -1;
      else
        if (finetangent[i] < -limit)
          t = viewwidth+1;
      else
        {
          t = FixedMul(finetangent[i], focallength);
          t = (centerxfrac - t + FRACUNIT-1) >> FRACBITS;
          if (t < -1)
            t = -1;
          else
            if (t > viewwidth+1)
              t = viewwidth+1;
        }
      viewangletox[i] = t;
    }

  // Scan viewangletox[] to generate xtoviewangle[]:
  //  xtoviewangle will give the smallest view angle
  //  that maps to x.

  for (x=0; x<=viewwidth; x++)
    {
      for (i=0; viewangletox[i] > x; i++)
        ;
      xtoviewangle[x] = (i<<ANGLETOFINESHIFT)-ANG90;
    }

  // Take out the fencepost cases from viewangletox.
  for (i=0; i<FINEANGLES/2; i++)
    if (viewangletox[i] == -1)
      viewangletox[i] = 0;
    else
      if (viewangletox[i] == viewwidth+1)
        viewangletox[i] = viewwidth;

  clipangle = xtoviewangle[0];
}

//
// R_InitLightTables
//

#define DISTMAP 2

static void R_InitLightTables (void)
{
  int i;

  // killough 4/4/98: dynamic colormaps
  c_zlight = malloc(sizeof(*c_zlight) * numcolormaps);

  // Calculate the light levels to use
  //  for each level / distance combination.
  for (i=0; i< LIGHTLEVELS; i++)
    {
      int j, startmap = ((LIGHTLEVELS-1-i)*2)*NUMCOLORMAPS/LIGHTLEVELS;
      for (j=0; j<MAXLIGHTZ; j++)
        {
    // CPhipps - use 320 here instead of SCREENWIDTH, otherwise hires is
    //           brighter than normal res
          int scale = FixedDiv ((320/2*FRACUNIT), (j+1)<<LIGHTZSHIFT);
          int t, level = startmap - (scale >>= LIGHTSCALESHIFT)/DISTMAP;

          if (level < 0)
            level = 0;
          else
            if (level >= NUMCOLORMAPS)
              level = NUMCOLORMAPS-1;

          // killough 3/20/98: Initialize multiple colormaps
          level *= 256;
          for (t=0; t<numcolormaps; t++)         // killough 4/4/98
            c_zlight[t][i][j] = colormaps[t] + level;
        }
    }
}

//
// R_SetViewSize
// Do not really change anything here,
//  because it might be in the middle of a refresh.
// The change will take effect next refresh.
//

dbool   setsizeneeded;
int     setblocks;

void R_SetViewSize(int blocks)
{
  setsizeneeded = TRUE;
  setblocks = blocks;
}

//
// R_ExecuteSetViewSize
//

void R_ExecuteSetViewSize (void)
{
  int i;

  setsizeneeded = FALSE;

  if (!setblocks)
  {
     scaledviewwidth = SCREENWIDTH;
     viewheight = SCREENHEIGHT - ST_SCALED_HEIGHT;
  }
  else
  {
     scaledviewwidth = SCREENWIDTH;
     viewheight = SCREENHEIGHT;
  }

  viewwidth = scaledviewwidth;

  viewheightfrac = viewheight<<FRACBITS;//e6y

  centery = viewheight/2;
  centerx = viewwidth/2;
  centerxfrac = centerx<<FRACBITS;
  centeryfrac = centery<<FRACBITS;
  projection = centerxfrac;
// proff 11/06/98: Added for high-res
  projectiony = ((SCREENHEIGHT * centerx * 320) / 200) / SCREENWIDTH * FRACUNIT;

  R_InitBuffer (scaledviewwidth, viewheight);

  R_InitTextureMapping();

  /* focallength is now valid; sprites project horizontally against it
   * so they line up with the (hor+ widened) wall projection. */
  projectionx = focallength;

  // psprite scales
// proff 08/17/98: Changed for high-res
  /* The player weapon is a screen-space HUD sprite, not world geometry,
   * so it must keep its vanilla 4:3 proportions and stay centred rather
   * than stretch with the hor+ widened viewwidth.  focallength is the
   * 4:3-equivalent half-width (== centerxfrac at 4:3), so the 4:3
   * reference viewwidth is 2*focallength.  Scaling the psprite against
   * that keeps the weapon the same on-screen size and centred at any
   * aspect; at 4:3 it reduces to the vanilla FRACUNIT*viewwidth/320. */
  {
    int psp_w = 2 * (focallength >> FRACBITS);
    if (psp_w <= 0)
      psp_w = viewwidth;
    pspritescale  = FRACUNIT * psp_w / 320;
    pspriteiscale = FRACUNIT * 320 / psp_w;
  }
// proff 11/06/98: Added for high-res
  pspriteyscale = (((SCREENHEIGHT*viewwidth)/SCREENWIDTH) << FRACBITS) / 200;

  // sky scaling
  skyiscale = (fixed_t)(((uint64_t)FRACUNIT * SCREENWIDTH * 200) / (viewwidth * SCREENHEIGHT));

  // [RH] Sky height fix for screens not 200 (or 240) pixels tall
  R_InitSkyMap();

  // thing clipping
  for (i=0 ; i<viewwidth ; i++)
    screenheightarray[i] = viewheight;

  // planes
  for (i=0 ; i<viewheight ; i++)
    {   // killough 5/2/98: reformatted
      fixed_t dy = D_abs(((i-viewheight/2)<<FRACBITS)+FRACUNIT/2);
// proff 08/17/98: Changed for high-res
      yslope[i] = FixedDiv(projectiony, dy);
    }

  for (i=0 ; i<viewwidth ; i++)
    {
      fixed_t cosadj = D_abs(finecosine[xtoviewangle[i]>>ANGLETOFINESHIFT]);
      distscale[i] = FixedDiv(FRACUNIT,cosadj);
    }

}

//
// R_Init
//

extern int screenblocks;

void R_Init (void)
{
  // CPhipps - R_DrawColumn isn't constant anymore, so must
  //  initialise in code
  // current column draw function
  lprintf(LO_INFO, "\nR_LoadTrigTables:\n");
  R_LoadTrigTables();
  lprintf(LO_INFO, "\nR_InitData:\n");
  R_InitData();
  R_SetViewSize(screenblocks);
  lprintf(LO_INFO, "\nR_Init: R_InitPlanes\n");
  R_InitPlanes();
  lprintf(LO_INFO, "R_InitLightTables\n");
  R_InitLightTables();
  lprintf(LO_INFO, "R_InitSkyMap\n");
  R_InitSkyMap();
  lprintf(LO_INFO, "R_InitTranslationsTables\n");
  R_InitTranslationTables();
  lprintf(LO_INFO, "R_InitPatches\n");
  R_InitPatches();
  /* Patch cache is live now, so the brightmap masks (which read both the
   * mask patches and the composite texture dimensions) can be baked. */
  U_BuildBrightmasks();
  /* Distance light is always point-sampled: the ordered-dither ("Dithered")
   * mode that used a LINEAR filterz was removed, so a config still holding
   * that value must not resurrect a drawer set nothing else selects. */
  drawvars.filterz = RDRAW_FILTER_POINT;
}

/* R_Deinit
 *
 * Releases the per-session allocations made by R_Init / R_InitData /
 * R_InitLightTables / R_InitSpriteDefs.  Z_Close at retro_deinit
 * would eventually reclaim these, but on libretro Z_Close only fires
 * at libretro deinit, not between content loads -- without an
 * explicit deinit, every retro_load_game leaks textures / texture
 * patches / textureheight / texturetranslation / flattranslation /
 * colormaps array / c_zlight / sprites / sprite frames.
 *
 * Total reclaim is wad-dependent, but for stock DOOM2 this runs
 * into low MB territory once you account for per-texture allocations.
 *
 * R_FlushAllPatches must run BEFORE this -- it iterates `numtextures`
 * to free its texture_composites parallel array.
 *
 * We intentionally do NOT W_UnlockLumpNum the COLORMAP / C_START..C_END
 * lumps cached into colormaps[i].  The lock-count creep is +1 per
 * session per colormap (so usually +1 for stock COLORMAP), tiny next
 * to the rest of the leak; reclaiming them properly would require
 * tracking the lump numbers across the R_InitColormaps "no markers"
 * fallback and isn't worth the complexity.
 */
void R_Deinit(void)
{
   int i;

   /* sprites: spritedef_t array holding per-sprite spriteframes.
    * R_InitSpriteDefs Z_Mallocs both, but only assigns spriteframes
    * for sprites that actually have lumps in the WAD (the
    * `if (j >= 0)` and `if (++maxframe)` guards in R_InitSpriteDefs).
    * The Z_Malloc is now memset to zero up front, so unallocated
    * entries have spriteframes==NULL and free(NULL) is a no-op --
    * but check numframes too as belt-and-braces against future
    * regressions in the init path. */
   if (sprites)
   {
      for (i = 0; i < numsprites; i++)
         if (sprites[i].numframes > 0 && sprites[i].spriteframes)
            Z_Free(sprites[i].spriteframes);
      Z_Free(sprites);
      sprites = NULL;
   }
   numsprites = 0;

   /* c_zlight: malloc'd (= Z_Malloc) by R_InitLightTables; entries
    * are pointers INTO colormaps[t], so freeing colormaps below
    * doesn't leave c_zlight itself in a worse state. */
   if (c_zlight)
   {
      free(c_zlight);
      c_zlight = NULL;
   }

   /* colormaps: array of pointers Z_Malloc'd by R_InitColormaps.
    * The pointed-at lumps are still cached/locked; they get reclaimed
    * by Z_Close at retro_deinit (see comment above). */
   if (colormaps)
   {
      Z_Free((void *)colormaps);
      colormaps = NULL;
   }
   numcolormaps = 0;

   /* flattranslation: small Z_Malloc'd array. */
   if (flattranslation)
   {
      Z_Free(flattranslation);
      flattranslation = NULL;
   }

   /* texturetranslation: small Z_Malloc'd array. */
   if (texturetranslation)
   {
      Z_Free(texturetranslation);
      texturetranslation = NULL;
   }

   /* textureheight: per-texture Z_Malloc'd array. */
   if (textureheight)
   {
      Z_Free(textureheight);
      textureheight = NULL;
   }

   /* textures: per-texture Z_Malloc'd structs PLUS the array itself.
    * Free elements first, then the container. */
   if (textures)
   {
      for (i = 0; i < numtextures; i++)
         Z_Free(textures[i]);
      Z_Free(textures);
      textures = NULL;
   }
   numtextures = 0;
}

/*
==============
=
= R_PointInSubsector
=
==============
*/

subsector_t *R_PointInSubsector(fixed_t x, fixed_t y)
{
   int nodenum;

   /* special case for trivial maps (single subsector, no nodes) */
   if (!numnodes)
      return subsectors;

   nodenum = numnodes-1;

   while (!(nodenum & NF_SUBSECTOR))
   {
      node_t *node   = &nodes[nodenum];
      int    side    = R_PointOnSide(x, y, node);
      nodenum        = node->children[side];
   }

   return &subsectors[nodenum & ~NF_SUBSECTOR];
}

//
// R_SetupFreelook
//
void R_SetupFreelook(void)
{
  static int old_centery = 0;
  fixed_t dy;
  int i;

  dy = FixedMul(focallength, finetangent[(ANG90-viewpitch)>>ANGLETOFINESHIFT]);

  if (movement_mouselook || raven){
    centeryfrac = (viewheight << (FRACBITS-1)) + dy;
    centery = centeryfrac >> FRACBITS;
  }
  else
  {
    centery = viewheight / 2;
    centeryfrac = centery<<FRACBITS;
  }

  if (centery != old_centery)
  {
    old_centery = centery;
    for (i=0 ; i<viewheight ; i++)
    {   // killough 5/2/98: reformatted
      fixed_t dy = D_abs(((i-centery)<<FRACBITS)+FRACUNIT/2);
      // proff 08/17/98: Changed for high-res
      yslope[i] = FixedDiv(projectiony, dy);
    }
  }
}

//
// R_SetupFrame
//
static int      view_underwater;
/* Underwater tint colour, in the ACTIVE surface format.  In truecolor the
 * flat's mean is accumulated over V_PaletteTC's native channels, so the
 * tint is not first collapsed to 565 and re-expanded. */
static uint32_t view_underwater_color;

static void R_SetupFrame (player_t *player)
{
  int cm;

  R_InterpolateView (player);

  /* rebuild the GLDEFS point-light list from current mobj positions */
  R_CollectDynLights();

  /* Low-latency turning: the view answers staged turn input every
   * frame instead of waiting for the tic.  The base must be the
   * LATEST tic's angle, not the interpolated one: the staged turn is
   * shown in full as it accumulates, and once a tic consumes it the
   * angle lerp would replay the same delta gradually while the new
   * pending restarts at zero -- a backward snap and re-advance every
   * tic.  Player rotation only ever comes from ticcmds, so anchoring
   * to the latest angle plus the staged remainder loses nothing and
   * advances monotonically. */
  if (G_PendingTurnActive() && !zacs_view_camera)
  {
    viewangle = R_SmoothPlaying_Get(player->mo->angle) + viewangleoffset
              + G_PendingTurn();
    /* and the freelook analogue: anchor the pitch to the latest tic and
     * add the unconsumed look backlog, instead of lerping a stale pair.
     * Only when mouselook drives the pitch -- the keyboard/gamepad
     * lookdir path has no backlog, and anchoring it would just strip
     * the interpolation and step the view at 35Hz. */
    if (G_PendingPitchActive(player->mo))
      viewpitch = G_PendingPitch(player->mo) + viewpitchoffset;
  }

  extralight = player->extralight;

  viewsin = finesine[viewangle>>ANGLETOFINESHIFT];
  viewcos = finecosine[viewangle>>ANGLETOFINESHIFT];

  R_SetupFreelook();

  /* Use the same frozen-view test as R_InterpolateView: when a menu or
   * pause holds the world still (single-player, non-demo), the camera is
   * pinned to FRACUNIT, so the world interpolations must be too -- otherwise
   * scrolling skies/textures and mid-motion sectors keep being lerped by the
   * free-running tic fraction and the "still" frame micro-jitters every
   * display frame (and is not cacheable). */
  {
    const fixed_t interp_frac =
      (paused || (menuactive && !demoplayback)) ? FRACUNIT : tic_vars.frac;
    R_DoInterpolations(interp_frac);
  }

  // killough 3/20/98, 4/4/98: select colormap based on player status

  if (player->mo->subsector->sector->heightsec != -1)
    {
      const sector_t *s = player->mo->subsector->sector->heightsec + sectors;
      cm = viewz < s->floorheight ? s->bottommap : viewz > s->ceilingheight ?
        s->topmap : s->midmap;
      if (cm < 0 || cm > numcolormaps)
        cm = 0;
    }
  else
    cm = 0;

  fullcolormap = colormaps[cm];
  zlight = c_zlight[cm];

  if (player->fixedcolormap)
    {
      fixedcolormap = fullcolormap   // killough 3/20/98: use fullcolormap
        + player->fixedcolormap*256*sizeof(lighttable_t);
    }
  else
    fixedcolormap = 0;

  /* Underwater: if the eye is inside a swimmable 3D-floor volume in the view
   * sector, tint the whole view toward the water's colour after the scene is
   * drawn (R_RenderPlayerView).  The tint is taken from the water surface
   * flat so it matches the surface seen from above. */
  view_underwater = 0;
  {
    const sector_t *vs = player->mo->subsector->sector;
    const ffloor_t *ff;
    for (ff = vs->ffloors; ff; ff = ff->next)
    {
      if (ff->type != FFLOOR_SWIMMABLE)
        continue;
      if (viewz >= ff->model->floorheight && viewz <= ff->model->ceilingheight)
      {
        view_underwater = 1;
        view_underwater_color = VID_TRUECOLOR
                                ? R_FlatAverageColorTC(ff->model->floorpic)
                                : (uint32_t)R_FlatAverageColor565(ff->model->floorpic);
        break;
      }
    }
  }

  validcount++;
}

int autodetect_hom = 0;       // killough 2/7/98: HOM autodetection flag

//
// R_RenderView
//
/* Render the 3D skybox (ZDoom SkyViewpoint) into the view buffer.  Called
 * before the main scene when a default SkyViewpoint exists; the main pass then
 * leaves its sky pixels untouched so this scene shows through them.
 *
 * Camera (matching GZDoom's HWSkyboxPortal::Setup): positioned at the
 * SkyViewpoint's location, fixed (no parallax for a plain SkyViewpoint), with
 * the view yaw set to the player's yaw plus the viewpoint's own yaw, same
 * pitch.  The skybox's own sky areas draw the flat sky texture as usual. */
/* Per-sector tagged skyboxes (SkyPicker).  After the main scene draws, the
 * sky pixels of sectors using a tagged skybox still show the default
 * sky/skyview.  For each tagged skybox visible this frame we re-render its
 * scene into a scratch buffer and copy back only the columns/rows owned by
 * that skybox's sky visplanes.
 *
 * The per-column window [top,bottom] for each tagged skybox is captured
 * from the sky visplanes BEFORE this runs, because the per-skybox render
 * clears the planes. */
extern int floorclip[], ceilingclip[];   /* r_plane.c */
static short  sb_top[MAX_SCREENWIDTH];
static short  sb_bot[MAX_SCREENWIDTH];
/* Skybox render scratch, one full surface.  Allocated on first use at the
 * active pixel width rather than kept as a fixed 16-bit static: truecolor
 * needs 4 bytes per pixel, and sizing it lazily also keeps the 16-bit build
 * from carrying the buffer at all until a tagged skybox is actually used. */
static void *sb_scratch = NULL;

static void *R_SkyboxScratch(void)
{
  static size_t sb_scratch_bytes = 0;
  size_t want = (size_t)MAX_SCREENWIDTH * MAX_SCREENHEIGHT * SURFACE_PIXEL_DEPTH;
  if (!sb_scratch || sb_scratch_bytes != want)
  {
    free(sb_scratch);
    sb_scratch = malloc(want);
    sb_scratch_bytes = sb_scratch ? want : 0;
  }
  return sb_scratch;
}

/* When set, the composite copies only pixels the reveal mask marks (the
 * default skybox); tagged skyboxes keep their span-based copy. */
static int sb_use_reveal;
static int sb_flat_alpha;   /* stacked-portal flat opacity 0..254 at composite
                             * time: 0 = replace (fast path), else blend the
                             * scene under the already-drawn flat */

/* stacked-sector portal snapshots (collected pre-composite, rendered post) */
/* Distinct displacements rendered per frame (each costs one scene render). */
#define PORTAL_CAP_MAX 16
/* Portal-plane ids collected per frame.  This is NOT the same budget: one
 * logical window contributes an id per sector it spans (zdcmp2's largest
 * carries 26), and they all collapse into a single group, so the id list
 * must be able to hold every visible piece of every window or the grouping
 * loses spans and the window renders with holes. */
#define PORTAL_ID_MAX 96
static int     portal_cap_hor[PORTAL_CAP_MAX];
static int     portal_cap_hfix[PORTAL_CAP_MAX];
static int     portal_cap_hsec[PORTAL_CAP_MAX];
static int     portal_cap_abs[PORTAL_CAP_MAX];
static angle_t portal_cap_ang[PORTAL_CAP_MAX];
static fixed_t portal_cap_dx[PORTAL_CAP_MAX];
static fixed_t portal_cap_dy[PORTAL_CAP_MAX];
static fixed_t portal_cap_dz[PORTAL_CAP_MAX];
static int     portal_cap_alpha[PORTAL_CAP_MAX];
static short portal_cap_top[PORTAL_CAP_MAX][MAX_SCREENWIDTH];
static short portal_cap_bot[PORTAL_CAP_MAX][MAX_SCREENWIDTH];
static int   n_portal_caps;

/* render skybox camera `sb` into scratch, then copy its owned sky pixels. */
/* Render the level from (camx,camy,camz, view angle + angdelta) into the
 * scratch buffer, sealed to the sb_top/sb_bot column spans, and composite
 * those spans back into the frame.  The shared core of tagged 3D skyboxes
 * and stacked-sector portals; r_in_skybox suppresses nested skyboxes and
 * portal windows inside the scene, so recursion depth is one. */
static void R_CompositeScratchSpans(const void *scratch,
                                    unsigned short *real_tl,
                                    unsigned int *real_tl_i);

static void R_RenderCompositeView(fixed_t camx, fixed_t camy, fixed_t camz,
                                  angle_t angdelta)
{
  fixed_t savex = viewx, savey = viewy, savez = viewz;
  angle_t saveang = viewangle;
  fixed_t savecos = viewcos, savesin = viewsin;
  unsigned short *real_tl   = drawvars.short_topleft;
  unsigned int   *real_tl_i = drawvars.int_topleft;
  void           *scratch   = R_SkyboxScratch();

  if (!scratch)
    return;

  /* render the scene into the scratch buffer */
  drawvars.short_topleft = (unsigned short *)scratch;
  drawvars.int_topleft   = (unsigned int   *)scratch;

  r_in_skybox = 1;
  viewx = camx;
  viewy = camy;
  viewz = camz;
  viewangle = saveang + angdelta;
  viewsin = finesine[viewangle>>ANGLETOFINESHIFT];
  viewcos = finecosine[viewangle>>ANGLETOFINESHIFT];
  viewplayer = &players[displayplayer];

  R_ClearClipSegs();
  R_ClearDrawSegs();
  R_ClearPlanes();
  R_ClearSprites();

  /* The composite below copies only the sb_top..sb_bot span of each column
   * out of the scratch buffer, so columns with no sky span for this skybox
   * can never contribute a pixel: seal those columns shut before the walk
   * and the scene renders only the columns the composite can use.  Sealing
   * is whole-column only -- the clip arrays double as the renderer's
   * occlusion state, and sealing a PART of a column corrupts portal
   * clipping for near geometry straddling the window (verified: far
   * skybox-room walls showed through an occluder).  All-or-nothing per
   * column has no partial-height interactions, so occlusion inside open
   * columns is untouched and the output is identical by construction. */
  {
    int cx;
    for (cx = 0; cx < viewwidth; cx++)
      if (sb_bot[cx] < sb_top[cx])
      {
        ceilingclip[cx] = viewheight;
        floorclip[cx]   = -1;
      }
  }

  R_WallTintClear();
  R_RenderBSPNode(numnodes-1);
  R_DrawCmdReplay();
  R_WallTintReplay();
  R_ResetColumnBuffer();
  R_DrawPlanes();
  R_DrawMasked();
  R_ResetColumnBuffer();
  r_in_skybox = 0;

  viewx = savex; viewy = savey; viewz = savez;
  viewangle = saveang; viewcos = savecos; viewsin = savesin;
  drawvars.short_topleft = real_tl;
  drawvars.int_topleft   = real_tl_i;

  R_CompositeScratchSpans(scratch, real_tl, real_tl_i);
}

/* Copy the sb_top..sb_bot span of every column out of the scratch buffer,
 * honouring the reveal mask and the stacked-portal flat blend.  Shared by
 * the scene composite above and the horizon composite below. */

/* Sector_SetPortal type 4: horizon portal.
 *
 * "Renders the linedef's frontsector's planes into infinity at the planes'
 * heights."  There is no scene to walk: the window shows one sector's floor
 * and ceiling as unbounded planes, which the standard flat mapper already
 * draws correctly -- its per-row distance grows without bound as a row
 * approaches the horizon, so a visplane spanning the window's rows converges
 * on its own.  The floor takes the rows below the horizon and the ceiling
 * those above; a plane on the wrong side of the viewer (a floor above the
 * eye, say) simply contributes nothing. */
static void R_RenderHorizonView(int secnum, int fixedplane)
{
  unsigned short *real_tl   = drawvars.short_topleft;
  unsigned int   *real_tl_i = drawvars.int_topleft;
  void           *scratch   = R_SkyboxScratch();
  const sector_t *sec;
  visplane_t     *plf, *plc;
  fixed_t         fh, ch, xo, yo;
  int             x, horizon;

  if (!scratch || (unsigned)secnum >= (unsigned)numsectors)
    return;
  sec     = &sectors[secnum];
  horizon = centery;

  drawvars.short_topleft = (unsigned short *)scratch;
  drawvars.int_topleft   = (unsigned int   *)scratch;

  r_in_skybox = 1;
  R_ClearPlanes();
  R_ClearSprites();

  /* type 3 measures the source heights from the camera and pins the flat's
   * texture to it (xfrac adds viewx, yfrac subtracts viewy -- see
   * R_MapPlane), so the surface never shifts as the viewer moves; type 4
   * uses the heights as they stand in the level. */
  fh = fixedplane ? viewz + sec->floorheight   : sec->floorheight;
  ch = fixedplane ? viewz + sec->ceilingheight : sec->ceilingheight;
  xo = fixedplane ? -viewx : 0;
  yo = fixedplane ?  viewy : 0;

  plf = fh < viewz
      ? R_FindPlane(fh, sec->floorpic, sec->lightlevel, xo, yo, NULL, -1, 0)
      : NULL;
  plc = ch > viewz
      ? R_FindPlane(ch, sec->ceilingpic, sec->lightlevel, xo, yo, NULL, -1, 0)
      : NULL;

  for (x = 0; x < viewwidth; x++)
  {
    int t = sb_top[x], b = sb_bot[x];
    if (b < t)
      continue;
    if (t < 0) t = 0;
    if (b >= viewheight) b = viewheight - 1;

    if (plc && t < horizon)
    {
      int cb = b < horizon - 1 ? b : horizon - 1;
      plc->top[x]    = (unsigned short)t;
      plc->bottom[x] = (unsigned short)cb;
      plc->modified  = 1;
      if (x < plc->minx) plc->minx = x;
      if (x > plc->maxx) plc->maxx = x;
    }
    if (plf && b > horizon)
    {
      int ft = t > horizon + 1 ? t : horizon + 1;
      plf->top[x]    = (unsigned short)ft;
      plf->bottom[x] = (unsigned short)b;
      plf->modified  = 1;
      if (x < plf->minx) plf->minx = x;
      if (x > plf->maxx) plf->maxx = x;
    }
  }

  R_ResetColumnBuffer();
  R_DrawPlanes();
  R_ResetColumnBuffer();
  r_in_skybox = 0;

  drawvars.short_topleft = real_tl;
  drawvars.int_topleft   = real_tl_i;

  R_CompositeScratchSpans(scratch, real_tl, real_tl_i);
}

static void R_CompositeScratchSpans(const void *scratch,
                                    unsigned short *real_tl,
                                    unsigned int *real_tl_i)
{
  int x, y;
  for (x = 0; x < viewwidth; x++)
  {
    int t = sb_top[x], b = sb_bot[x];
    if (b < t)
      continue;
    if (t < 0) t = 0;                       /* clamp to the surface */
    if (b >= SCREENHEIGHT) b = SCREENHEIGHT - 1;
    for (y = t; y <= b; y++)
    {
      if (sb_use_reveal && !R_SkyRevealTest(x, y))
        continue;
      if (VID_TRUECOLOR)
      {
        uint32_t *d = &((uint32_t *)real_tl_i)[y * SURFACE_SHORT_PITCH + x];
        uint32_t  sc = ((const uint32_t *)scratch)[y * SURFACE_SHORT_PITCH + x];
        if (!sb_flat_alpha)
          *d = sc;
        else
        {
          /* dst holds the flat drawn at full strength; keep it at alpha and
           * let the scene show through at (255 - alpha), per channel */
          uint32_t dv = *d;
          int a = sb_flat_alpha, w = 255 - a;
          uint32_t r = (((dv >> 16) & 0xff) * a + ((sc >> 16) & 0xff) * w) / 255;
          uint32_t g = (((dv >>  8) & 0xff) * a + ((sc >>  8) & 0xff) * w) / 255;
          uint32_t b = ((dv & 0xff) * a + (sc & 0xff) * w) / 255;
          *d = (dv & 0xff000000u) | (r << 16) | (g << 8) | b;
        }
      }
      else
      {
        uint16_t *d = &real_tl[y * SURFACE_SHORT_PITCH + x];
        uint16_t  sc = ((const uint16_t *)scratch)[y * SURFACE_SHORT_PITCH + x];
        if (!sb_flat_alpha)
          *d = sc;
        else
        {
          uint16_t dv = *d;
          int a = sb_flat_alpha, w = 255 - a;
          uint32_t r = (((dv >> 11) & 0x1f) * a + ((sc >> 11) & 0x1f) * w) / 255;
          uint32_t g = (((dv >>  5) & 0x3f) * a + ((sc >>  5) & 0x3f) * w) / 255;
          uint32_t b = ((dv & 0x1f) * a + (sc & 0x1f) * w) / 255;
          *d = (uint16_t)((r << 11) | (g << 5) | b);
        }
      }
    }
  }
}

static void R_RenderTaggedSkybox(const skybox_t *sb)
{
  R_RenderCompositeView(sb->x, sb->y, sb->z, sb->angle);
}

#ifdef PRBOOM_RENDER_PROFILE
static double rprof_skybox;
#endif

void R_RenderPlayerView (player_t* player)
{
#ifdef PRBOOM_RENDER_PROFILE
  /* Optional per-phase render profiler.  Compile with
   * -DPRBOOM_RENDER_PROFILE to build it in; it is entirely absent
   * otherwise (zero cost, zero risk to normal builds).  It times the
   * three main render phases using the libretro perf interface's
   * high-resolution microsecond clock (I_RenderProfileUsec) and logs a
   * rolling average every PROF_REPORT frames so the relative cost of
   * BSP+walls vs floors/ceilings vs sprites is visible on real hardware
   * -- replacing guesswork about where the frame goes. */
# define PROF_REPORT 120
  static double acc_bsp = 0.0, acc_planes = 0.0, acc_masked = 0.0,
                acc_setup = 0.0, acc_wallfill = 0.0, acc_skybox = 0.0,
                acc_storewall = 0.0, acc_findplane = 0.0, acc_sprproj = 0.0;
  static unsigned prof_frames = 0;
  double t0, t1, t2, t3, t4;
  extern double prof_wallfill_usec, prof_storewall_usec, prof_findplane_usec;
  extern double prof_sprproj_usec;
  prof_wallfill_usec = prof_storewall_usec = prof_findplane_usec = 0.0;
  prof_sprproj_usec = 0.0;
  t0 = I_RenderProfileUsec();
#endif

  R_SetupFrame (player);

  /* Default 3D skybox: the main pass records which sky pixels it leaves
   * showing the skybox scene (see the reveal mask in r_plane.c); the scene
   * itself renders after the planes, clipped and composited to exactly
   * those pixels. */
  sky_reveal_active = (skyview.active || sector_portals_active ||
                       line_portals_active) ? 1 : 0;
  if (line_portals_active)
    R_LinePortalClearClaims();


  // Clear buffers.
  R_ClearClipSegs ();
  R_ClearDrawSegs ();
  R_ClearPlanes ();
  R_ClearSprites ();

    if (autodetect_hom)
    { // killough 2/10/98: add flashing red HOM indicators
      unsigned char color=(gametic % 20) < 9 ? 0xb0 : 0;
      V_FillRect(0, 0, viewwidth, viewheight, color);
    }
    else if (render_aspect != 0)
    {
      /* hor+ widescreen: projection rounding at the extreme view edges
       * can leave a 1-2px column with no wall or plane covering it,
       * which then shows stale framebuffer content (the direct-render
       * buffer is never otherwise cleared).  Paint the two edge columns
       * black up front so any such gap is defined rather than garbage.
       * This is cheap (a few columns) and only runs in widened aspects;
       * 4:3 always achieves full coverage and keeps the zero-clear
       * path.  A wider margin than 1px guards against rounding landing
       * on either of the outermost columns. */
      int m = 4;
      if (m > viewwidth) m = viewwidth;
      V_FillRect(0, 0, m, viewheight, 0);
      V_FillRect(viewwidth - m, 0, m, viewheight, 0);
    }

#ifdef PRBOOM_RENDER_PROFILE
  t1 = I_RenderProfileUsec();
#endif

  // The head node is the last node output.
  R_WallTintClear();
  R_RenderBSPNode (numnodes-1);

  /* Rasterize the wall columns the walk recorded (see r_drawcmd.h). */
  R_DrawCmdReplay();
  R_WallTintReplay();
  R_ResetColumnBuffer();

#ifdef PRBOOM_RENDER_PROFILE
  t2 = I_RenderProfileUsec();
#endif

    R_DrawPlanes ();

#ifdef PRBOOM_RENDER_PROFILE
  t3 = I_RenderProfileUsec();
#endif

    /* Default 3D skybox: derive the reveal mask from the plane pass's
     * surviving visplanes (see r_plane.c) before masked draws clear their
     * pixels from it. */
    if (sky_reveal_active)
    {
      R_SkyRevealBuild();
      R_LinePortalReveal();
    }

    R_DrawMasked ();
    R_ResetColumnBuffer();

    /* Stacked-sector portals, collection phase: the composite renders below
     * (default skybox, tagged skyboxes) each call R_ClearPlanes for their
     * scene walk, destroying the main scene's visplanes -- so the portal
     * windows' ids and spans must be snapshotted NOW, exactly as the tagged
     * skybox block snapshots its spans.  The renders happen after the
     * skybox composites. */
    n_portal_caps = 0;
    if (sector_portals_active && (floorportals || ceilingportals))
    {
      int pids[PORTAL_ID_MAX];
      int npids = R_CollectPortalIds(pids, PORTAL_ID_MAX);
      int k;
      for (k = 0; k < npids; k++)
      {
        int id = pids[k];
        int secnum = (id > 0 ? id : -id) - 1;
        const secportal_t *sp = id > 0 ? &ceilingportals[secnum]
                                       : &floorportals[secnum];
        int g, x;
        if (!sp->active)
          continue;
        if (!R_CollectPortalSpan(id, sb_top, sb_bot))
          continue;
        /* A window spanning many sectors yields one id per sector, but they
         * all show the same scene from the same translated camera -- so
         * group by displacement (and blend weight) and union the spans.
         * Without this a 26-sector window would drive 26 full scene
         * renders per frame instead of one. */
        for (g = 0; g < n_portal_caps; g++)
          if (portal_cap_dx[g] == sp->dx && portal_cap_dy[g] == sp->dy &&
              portal_cap_dz[g] == sp->dz && portal_cap_alpha[g] == sp->alpha &&
              portal_cap_abs[g] == sp->absolute &&
              portal_cap_ang[g] == sp->angle &&
              portal_cap_hor[g] == sp->horizon &&
              portal_cap_hfix[g] == sp->hfixed &&
              portal_cap_hsec[g] == sp->hsec)
            break;
        if (g == n_portal_caps)
        {
          if (n_portal_caps == PORTAL_CAP_MAX)
            continue;
          portal_cap_hor[g]   = sp->horizon;
          portal_cap_hfix[g]  = sp->hfixed;
          portal_cap_hsec[g]  = sp->hsec;
          portal_cap_abs[g]   = sp->absolute;
          portal_cap_ang[g]   = sp->angle;
          portal_cap_dx[g]    = sp->dx;
          portal_cap_dy[g]    = sp->dy;
          portal_cap_dz[g]    = sp->dz;
          portal_cap_alpha[g] = sp->alpha;
          memcpy(portal_cap_top[g], sb_top, sizeof(short) * viewwidth);
          memcpy(portal_cap_bot[g], sb_bot, sizeof(short) * viewwidth);
          n_portal_caps++;
        }
        else
        {
          for (x = 0; x < viewwidth; x++)
          {
            if (sb_top[x] < portal_cap_top[g][x])
              portal_cap_top[g][x] = sb_top[x];
            if (sb_bot[x] > portal_cap_bot[g][x])
              portal_cap_bot[g][x] = sb_bot[x];
          }
        }
      }
    }

    /* Default 3D skybox: the reveal mask now holds exactly the pixels the
     * plane pass left showing the skybox scene (sky-plane skips minus any
     * later plane draw over them).  Render the scene sealed to those
     * columns and composite the masked pixels.  Runs after R_DrawMasked --
     * masked draws (sprites, mid textures) cover the mask, so the composite
     * cannot paint over them and the skybox scene's own clears cannot
     * destroy the main scene's masked state. */
    if (skyview.active)
    {
#ifdef PRBOOM_RENDER_PROFILE
      unsigned long long skm0 = I_RenderProfileUsec();
#endif
      sky_reveal_active = 0;   /* no cover hooks inside the skybox scene */
      if (R_SkyRevealExtents(sb_top, sb_bot))
      {
        skybox_t gsb;
        gsb.x = skyview.x;
        gsb.y = skyview.y;
        gsb.z = skyview.z;
        gsb.angle = skyview.angle;
        sb_use_reveal = 1;
        R_RenderTaggedSkybox(&gsb);
        sb_use_reveal = 0;
      }
#ifdef PRBOOM_RENDER_PROFILE
      rprof_skybox += I_RenderProfileUsec() - skm0;
#endif
    }


    /* Skyboxes (default SkyViewpoint and per-sector SkyPicker): after the
     * main scene draws, its sky pixels are still untouched (see the skip in
     * R_DrawPlanes).  For each skybox visible this frame -- the default one
     * included -- capture its sky-region span from the sky visplanes, then
     * render that skybox's scene clipped to those pixels and composite them.
     * Rendering after the main pass, against this frame's spans, means a
     * skybox only ever draws the pixels it will actually show: a view with
     * little sky pays little, one with none pays nothing.  (The default
     * skybox used to render the whole viewport up front and be almost
     * entirely overdrawn.)  Spans for all skyboxes are captured before any
     * render because each render clears the planes (destroying the main
     * scene's sky visplanes). */
    if (numskyboxes > 0)
    {
      static short cap_top[16][MAX_SCREENWIDTH];
      static short cap_bot[16][MAX_SCREENWIDTH];
      int used[16], nused = 0, k, x;
      int nb = numskyboxes < 16 ? numskyboxes : 16;
      /* which tagged skyboxes appear, and capture their spans */
      for (k = 0; k < nb; k++)
      {
        if (R_CollectSkyboxSpan(k, sb_top, sb_bot))
        {
          memcpy(cap_top[nused], sb_top, sizeof(short) * viewwidth);
          memcpy(cap_bot[nused], sb_bot, sizeof(short) * viewwidth);
          used[nused++] = k;
        }
      }
      for (k = 0; k < nused; k++)
      {
        for (x = 0; x < viewwidth; x++)
        {
          sb_top[x] = cap_top[k][x];
          sb_bot[x] = cap_bot[k][x];
        }
        R_RenderTaggedSkybox(&skyboxes[used[k]]);
      }
    }

    /* Stacked-sector portals, render phase: for each snapshotted window,
     * render the level from the viewer translated by the portal offset,
     * sealed to the cached spans, and composite into the window's surviving
     * (reveal-tested) pixels.  Same machinery as tagged skyboxes; single
     * depth (portal windows inside the scene draw their flats).  Zero cost
     * without portal planes this frame. */
    {
      int k;
      for (k = 0; k < n_portal_caps; k++)
      {
        memcpy(sb_top, portal_cap_top[k], sizeof(short) * viewwidth);
        memcpy(sb_bot, portal_cap_bot[k], sizeof(short) * viewwidth);
        sb_use_reveal = 1;
        sb_flat_alpha = portal_cap_alpha[k] > 0 && portal_cap_alpha[k] < 255
                      ? portal_cap_alpha[k] : 0;
        /* absolute cameras (skybox portals) sit at a fixed spot and turn
         * with the viewer; displacement portals ride the viewer */
        if (portal_cap_hor[k])
          R_RenderHorizonView(portal_cap_hsec[k], portal_cap_hfix[k]);
        else if (portal_cap_abs[k])
          R_RenderCompositeView(portal_cap_dx[k], portal_cap_dy[k],
                                portal_cap_dz[k], portal_cap_ang[k]);
        else
          R_RenderCompositeView(viewx + portal_cap_dx[k],
                                viewy + portal_cap_dy[k],
                                viewz + portal_cap_dz[k], 0);
        sb_flat_alpha = 0;
        sb_use_reveal = 0;
      }
    }

    /* Visual line portals: each claimed portal line renders the level from
     * the viewer transformed into its partner line's frame -- the offset
     * from this line's anchor, rotated by the angle between the lines and
     * planted at the partner's anchor -- sealed to the wall columns the seg
     * pass claimed and composited into the pixels that survived. */
    if (line_portals_active && lineportals && lp_any)
    {
      int lids[PORTAL_CAP_MAX];
      int nl = R_LinePortalIds(lids, PORTAL_CAP_MAX);
      int k;
      for (k = 0; k < nl; k++)
      {
        const lineportal_t *lp = &lineportals[lids[k]];
        fixed_t rx, ry, c, s2, camx, camy;
        if (!lp->active || !R_LinePortalSpan(lids[k], sb_top, sb_bot))
          continue;
        c  = finecosine[lp->angle >> ANGLETOFINESHIFT];
        s2 = finesine[lp->angle >> ANGLETOFINESHIFT];
        rx = viewx - lp->ax;
        ry = viewy - lp->ay;
        camx = lp->bx + FixedMul(rx, c) - FixedMul(ry, s2);
        camy = lp->by + FixedMul(rx, s2) + FixedMul(ry, c);
        sb_use_reveal = 1;
        R_RenderCompositeView(camx, camy, viewz, lp->angle);
        sb_use_reveal = 0;
      }
    }

    /* Underwater: blend the finished view toward the water colour. */
    if (view_underwater)
    {
      if (VID_TRUECOLOR)
        R_TintViewTC(view_underwater_color);
      else
        R_TintView((uint16_t)view_underwater_color);
    }

#ifdef PRBOOM_RENDER_PROFILE
  t4 = I_RenderProfileUsec();
  acc_setup  += (t1 - t0);
  acc_skybox += rprof_skybox; rprof_skybox = 0.0;
  acc_bsp    += (t2 - t1);   /* BSP traversal + wall/clip + visplane build */
  acc_planes += (t3 - t2);   /* floor/ceiling span fill */
  acc_masked += (t4 - t3);   /* sprites + masked midtextures */
  acc_wallfill += prof_wallfill_usec; /* subset: wall pixel writes */
  acc_storewall += prof_storewall_usec; /* subset of bsp+walls: seg setup+fill */
  acc_findplane += prof_findplane_usec; /* subset of bsp+walls: visplane hash */
  acc_sprproj += prof_sprproj_usec;     /* subset of bsp+walls: sprite projection */
  if (++prof_frames >= PROF_REPORT)
  {
    double n = (double)prof_frames;
    double bsp = acc_bsp / n;
    double sw  = acc_storewall / n;
    double fp  = acc_findplane / n;
    double sp  = acc_sprproj / n;
    lprintf(LO_INFO,
      "R_RenderProfile (avg over %u frames, us): "
      "setup %.1f (skybox %.1f) | bsp+walls %.1f [traverse %.1f, storewall %.1f (fill %.1f), "
      "findplane %.1f, sprproj %.1f] | planes %.1f | masked %.1f | total %.1f\n",
      prof_frames,
      acc_setup / n,
      acc_skybox / n,
      bsp,
      bsp - sw - fp - sp,       /* pure traversal/clip remainder */
      sw,
      acc_wallfill / n,
      fp,
      sp,
      acc_planes / n,
      acc_masked / n,
      (acc_setup + acc_bsp + acc_planes + acc_masked) / n);
    acc_setup = acc_bsp = acc_planes = acc_masked = acc_wallfill = 0.0;
    acc_skybox = 0.0;
    acc_storewall = acc_findplane = acc_sprproj = 0.0;
    prof_frames = 0;
  }
# undef PROF_REPORT
#endif

  R_RestoreInterpolations();
}
