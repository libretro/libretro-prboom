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
#include "r_drawcmd.h"
#include "r_data.h"
#include "r_state.h"
#include "r_things.h"
#include "r_plane.h"
#include "r_bsp.h"
#include "r_draw.h"
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

  x -= node->x;
  y -= node->y;

  // Try to quickly decide by looking at sign bits.
  if ((node->dy ^ node->dx ^ x ^ y) < 0)
    return (node->dy ^ x) < 0;  // (left is negative)
  return FixedMul(y, node->dx>>FRACBITS) >= FixedMul(node->dy>>FRACBITS, x);
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
  R_ApplyDiminishedLighting(); /* sync General>Video setting into filterz */
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
static void R_SetupFrame (player_t *player)
{
  int cm;

  R_InterpolateView (player);

  /* Low-latency turning: fold the mouse counts the next tic will
   * consume into this frame's view angle (g_game.c). */
  viewangle += G_PendingTurn();

  extralight = player->extralight;

  viewsin = finesine[viewangle>>ANGLETOFINESHIFT];
  viewcos = finecosine[viewangle>>ANGLETOFINESHIFT];

  R_SetupFreelook();

  R_DoInterpolations(tic_vars.frac);

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

  validcount++;
}

int autodetect_hom = 0;       // killough 2/7/98: HOM autodetection flag

//
// R_RenderView
//
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
                acc_setup = 0.0, acc_wallfill = 0.0,
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
  R_RenderBSPNode (numnodes-1);

  /* Rasterize the wall columns the walk recorded (see r_drawcmd.h). */
  R_DrawCmdReplay();
  R_ResetColumnBuffer();

#ifdef PRBOOM_RENDER_PROFILE
  t2 = I_RenderProfileUsec();
#endif

    R_DrawPlanes ();

#ifdef PRBOOM_RENDER_PROFILE
  t3 = I_RenderProfileUsec();
#endif

    R_DrawMasked ();
    R_ResetColumnBuffer();

#ifdef PRBOOM_RENDER_PROFILE
  t4 = I_RenderProfileUsec();
  acc_setup  += (t1 - t0);
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
      "setup %.1f | bsp+walls %.1f [traverse %.1f, storewall %.1f (fill %.1f), "
      "findplane %.1f, sprproj %.1f] | planes %.1f | masked %.1f | total %.1f\n",
      prof_frames,
      acc_setup / n,
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
    acc_storewall = acc_findplane = acc_sprproj = 0.0;
    prof_frames = 0;
  }
# undef PROF_REPORT
#endif

  R_RestoreInterpolations();
}
