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


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "doomstat.h"
#include "d_net.h"
#include "w_wad.h"
#include "r_main.h"
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

int viewangleoffset;
int validcount = 1;         // increment every time a check is made
const lighttable_t *fixedcolormap;
int      centerx, centery;
fixed_t  centerxfrac, centeryfrac;
fixed_t  viewheightfrac; //e6y: for correct clipping of things
fixed_t  projection;
// proff 11/06/98: Added for high-res
fixed_t  projectiony;
fixed_t  viewx, viewy, viewz;
angle_t  viewangle;
fixed_t  viewcos, viewsin;
player_t *viewplayer;
extern lighttable_t **walllights;

#ifndef __LIBRETRO__
static mobj_t *oviewer;
#endif

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
    return x <= node->x ? node->dy > 0 : node->dy < 0;

  if (!node->dy)
    return y <= node->y ? node->dx < 0 : node->dx > 0;

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
  register int i,x;
  fixed_t focallength;

  // Use tangent table to generate viewangletox:
  //  viewangletox will give the next greatest x
  //  after the view angle.
  //
  // Calc focallength
  //  so FIELDOFVIEW angles covers SCREENWIDTH.

  focallength = FixedDiv(centerxfrac, finetangent[FINEANGLES/4+FIELDOFVIEW/2]);

  for (i=0 ; i<FINEANGLES/2 ; i++)
    {
      int t;
      if (finetangent[i] > FRACUNIT*2)
        t = -1;
      else
        if (finetangent[i] < -FRACUNIT*2)
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

boolean setsizeneeded;
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

  // psprite scales
// proff 08/17/98: Changed for high-res
  pspritescale = FRACUNIT*viewwidth/320;
  pspriteiscale = FRACUNIT*320/viewwidth;
// proff 11/06/98: Added for high-res
  pspriteyscale = (((SCREENHEIGHT*viewwidth)/SCREENWIDTH) << FRACBITS) / 200;

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
  lprintf(LO_INFO, "\nR_LoadTrigTables: ");
  R_LoadTrigTables();
  lprintf(LO_INFO, "\nR_InitData: ");
  R_InitData();
  R_SetViewSize(screenblocks);
  lprintf(LO_INFO, "\nR_Init: R_InitPlanes ");
  R_InitPlanes();
  lprintf(LO_INFO, "R_InitLightTables ");
  R_InitLightTables();
  lprintf(LO_INFO, "R_InitSkyMap ");
  R_InitSkyMap();
  lprintf(LO_INFO, "R_InitTranslationsTables ");
  R_InitTranslationTables();
  lprintf(LO_INFO, "R_InitPatches ");
  R_InitPatches();
}

//
// R_PointInSubsector
//
// killough 5/2/98: reformatted, cleaned up

subsector_t *R_PointInSubsector(fixed_t x, fixed_t y)
{
  int nodenum = numnodes-1;

  // special case for trivial maps (single subsector, no nodes)
  if (numnodes == 0)
    return subsectors;

  while (!(nodenum & NF_SUBSECTOR))
    nodenum = nodes[nodenum].children[R_PointOnSide(x, y, nodes+nodenum)];
  return &subsectors[nodenum & ~NF_SUBSECTOR];
}

//
// R_SetupFrame
//

static void R_SetupFrame (player_t *player)
{
  int cm;
#ifndef __LIBRETRO__
  boolean NoInterpolate = paused || (menuactive && !demoplayback);
#endif

  viewplayer = player;

#ifndef __LIBRETRO__
  if (player->mo != oviewer || NoInterpolate)
  {
    R_ResetViewInterpolation ();
    oviewer = player->mo;
  }
  tic_vars.frac = I_GetTimeFrac ();
  if (NoInterpolate)
    tic_vars.frac = FRACUNIT;
  R_InterpolateView (player, tic_vars.frac);
#else
  R_InterpolateView (player);
#endif

  extralight = player->extralight;

  viewsin = finesine[viewangle>>ANGLETOFINESHIFT];
  viewcos = finecosine[viewangle>>ANGLETOFINESHIFT];

#ifndef __LIBRETRO__
  R_DoInterpolations(tic_vars.frac);
#endif

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
// R_ShowStats
//
int rendered_visplanes, rendered_segs, rendered_vissprites;
boolean rendering_stats;

#ifndef __LIBRETRO__
static void R_ShowStats(void)
{
//e6y
#define KEEPTIMES 10
  static int keeptime[KEEPTIMES], showtime;
  int now = I_GetTime();

  if (now - showtime > 35) {
    doom_printf("Frame rate %d fps\nSegs %d, Visplanes %d, Sprites %d",
    (35*KEEPTIMES)/(now - keeptime[0]), rendered_segs,
    rendered_visplanes, rendered_vissprites);
    showtime = now;
  }
  memmove(keeptime, keeptime+1, sizeof(keeptime[0]) * (KEEPTIMES-1));
  keeptime[KEEPTIMES-1] = now;
}
#endif

//
// R_RenderView
//
void R_RenderPlayerView (player_t* player)
{
  R_SetupFrame (player);

  // Clear buffers.
  R_ClearClipSegs ();
  R_ClearDrawSegs ();
  R_ClearPlanes ();
  R_ClearSprites ();

  rendered_segs = rendered_visplanes = 0;
    if (autodetect_hom)
    { // killough 2/10/98: add flashing red HOM indicators
      unsigned char color=(gametic % 20) < 9 ? 0xb0 : 0;
      V_FillRect(0, 0, viewwidth, viewheight, color);
    }

  // check for new console commands.
#ifdef HAVE_NET
  NetUpdate ();
#endif

  // The head node is the last node output.
  R_RenderBSPNode (numnodes-1);
  R_ResetColumnBuffer();

  // Check for new console commands.
#ifdef HAVE_NET
  NetUpdate ();
#endif

    R_DrawPlanes ();

  // Check for new console commands.
#ifdef HAVE_NET
  NetUpdate ();
#endif

    R_DrawMasked ();
    R_ResetColumnBuffer();

  // Check for new console commands.
#ifdef HAVE_NET
  NetUpdate ();
#endif

#ifndef __LIBRETRO__
  if (rendering_stats) R_ShowStats();

  R_RestoreInterpolations();
#endif
}
