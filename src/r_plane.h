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
 *      Refresh, visplane stuff (floor, ceilings).
 *
 *-----------------------------------------------------------------------------*/

#ifndef __R_PLANE__
#define __R_PLANE__

#include "r_data.h"

/* killough 10/98: special mask indicates sky flat comes from sidedef */
#define PL_SKYFLAT (0x80000000)

/* Visplane related. */
extern int *lastopening; // dropoff overflow

extern int floorclip[], ceilingclip[]; // dropoff overflow
extern fixed_t yslope[], distscale[];

void R_InitPlanes(void);
void R_ClearPlanes(void);
void R_DrawPlanes (void);

visplane_t *R_FindPlane(
  fixed_t height,
  int picnum,
  int lightlevel,
  fixed_t xoffs,                /* killough 2/28/98: add x-y offsets */
  fixed_t yoffs,
  const secplane_t *slope,      /* tilted plane or NULL */
  int skybox, int portal);                  /* per-sector 3D skybox index, or -1 */

visplane_t *R_CheckPlane(visplane_t *pl, int start, int stop);
visplane_t *R_DupPlane(const visplane_t *pl, int start, int stop);
visplane_t *R_FindWaterPlane(fixed_t height, int picnum, int lightlevel);

/* collect the union per-column span [top,bottom] of all modified sky
 * visplanes using skybox index sbidx, into caller arrays sized viewwidth.
 * Returns 1 if any column is covered. */
int R_CollectSkyboxSpan(int sbidx, short *out_top, short *out_bot);
int R_CollectPortalIds(int *out_ids, int maxids);
int R_CollectPortalSpan(int portal, short *out_top, short *out_bot);

/* Default-skybox reveal mask (see r_plane.c): per-pixel ground truth of
 * which sky pixels the main pass left showing the skybox scene. */
extern int sky_reveal_active;
extern int sky_row_min, sky_row_max;
void R_SkyRevealBuild(void);
void R_SkyRevealCoverCol(int x, int y1, int y2);
int  R_SkyRevealExtents(short *out_top, short *out_bot);
int  R_SkyRevealTest(int x, int y);

/* visual line portal claims (see r_plane.c) */
extern int lp_any;
void R_LinePortalClearClaims(void);
void R_LinePortalClaim(int x, int y1, int y2, int portal);
void R_LinePortalReveal(void);
int  R_LinePortalSpan(int portal, short *out_top, short *out_bot);
int  R_LinePortalIds(int *out_ids, int maxids);

#endif
