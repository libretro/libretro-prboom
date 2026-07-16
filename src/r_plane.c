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
 *      Here is a core component: drawing the floors and ceilings,
 *       while maintaining a per column clipping list only.
 *      Moreover, the sky areas have to be determined.
 *
 * MAXVISPLANES is no longer a limit on the number of visplanes,
 * but a limit on the number of hash slots; larger numbers mean
 * better performance usually but after a point they are wasted,
 * and memory and time overheads creep in.
 *
 * For more information on visplanes, see:
 *
 * http://classicgaming.com/doom/editing/
 *
 * Lee Killough
 *
 *-----------------------------------------------------------------------------*/

#include "config.h"

#include "z_zone.h"  /* memory allocation wrappers -- killough */

#include "doomstat.h"
#include "w_wad.h"
#include "r_main.h"
#include "r_draw.h"
#include "u_brightmap.h"
#include "r_things.h"
#include "r_dynlight.h"
#include "r_sky.h"
#include "r_plane.h"
#include "u_zanimdefs.h"
#include "v_video.h"
#include "lprintf.h"

/* DISTMAP mirrors the (file-local) constant in r_main.c's R_InitLightTables;
 * the Smooth floor fine-weight recompute must use the identical value so its
 * 64-resolution darkness agrees with the 32-band zlight table at band centres. */
#ifndef DISTMAP
#define DISTMAP 2
#endif
#include "i_system.h"

#define MAXVISPLANES 128    /* must be a power of 2 */

static visplane_t *visplanes[MAXVISPLANES];   // killough
static visplane_t *freetail;                  // killough
static visplane_t **freehead = &freetail;     // killough
visplane_t *floorplane, *ceilingplane;
/* Per-subsector translucent 3D-floor (swimmable) surface, set in
 * R_Subsector, span-filled in R_RenderSegLoop, drawn after the opaque
 * planes in R_DrawPlanes.  NULL except in a swimmable sector viewed from
 * above its water surface. */
visplane_t *waterplane;
visplane_t *morewater[MAXMOREWATER];
int nmorewater;

// killough -- hash function for visplanes
// Empirically verified to be fairly uniform:

#define visplane_hash(picnum,lightlevel,height) \
  ((unsigned)((picnum)*3+(lightlevel)+(height)*7) & (MAXVISPLANES-1))

size_t maxopenings;
int *openings,*lastopening; // dropoff overflow

// Clip values are the solid pixel bounding the range.
//  floorclip starts out SCREENHEIGHT
//  ceilingclip starts out -1

int floorclip[MAX_SCREENWIDTH], ceilingclip[MAX_SCREENWIDTH]; // dropoff overflow

// spanstart holds the start of a plane span; initialized to 0 at start

static int spanstart[MAX_SCREENHEIGHT];                // killough 2/8/98

//
// texture mapping
//

static const lighttable_t **planezlight;
static int                  planelightlevel; /* LIGHTLEVELS index of current plane, for Smooth fine weight */
static int                  planerawlight;   /* raw 0..255 sector light + extralight, for Smooth sub-band base */
static fixed_t planeheight;
static int     plane_dynlit;    /* any GLDEFS point light can reach this plane */

// killough 2/8/98: make variables static

static fixed_t basexscale, baseyscale;
static fixed_t cachedheight[MAX_SCREENHEIGHT];
static fixed_t cacheddistance[MAX_SCREENHEIGHT];
static fixed_t cachedxstep[MAX_SCREENHEIGHT];
static fixed_t cachedystep[MAX_SCREENHEIGHT];
static fixed_t xoffs,yoffs;    // killough 2/28/98: flat offsets

fixed_t yslope[MAX_SCREENHEIGHT], distscale[MAX_SCREENWIDTH];

/* forward declarations */
extern dbool   r_wiggle_fix;

//
// R_InitPlanes
// Only at game startup.
//
void R_InitPlanes (void)
{
}

//
/* ---- tilted (sloped) visplanes ----------------------------------------
 *
 * A sloped visplane cannot use the constant-z row walk: depth varies
 * along each row.  Instead each span is subdivided into short chunks;
 * the exact world hit point of the view ray is computed at the chunk
 * endpoints by intersecting the ray with the plane (doubles), and the
 * existing affine span drawer fills between them.  Chunks are 8 pixels,
 * narrow enough that the residual perspective error on the shallow
 * Plane_Align ramps this supports is under a texel.
 *
 * Ray construction: forward = (cos,sin,0), right = (sin,-cos,0),
 * up = (0,0,1); the ray through pixel (sx, sy) is
 *   dir = forward + right * (sx-centerx)/fx + up * (centery-sy)/fy
 * with fx/fy the horizontal/vertical focal lengths (anisotropic under
 * hor+ widescreen).  dir's forward component is 1, so the plane-hit
 * parameter t is also the light/shading distance. */

static const secplane_t *tilt_plane;
static double tilt_nx, tilt_ny, tilt_nz, tilt_num;  /* normal, -dist(view) */
static double tilt_vx, tilt_vy;                      /* view origin */
static double tilt_sin, tilt_cos;                    /* view angle */
static double tilt_ifx, tilt_ify;                    /* 1/focal lengths */

static void R_TiltedPlaneSetup(const visplane_t *pl)
{
   tilt_plane = pl->slope;
   tilt_nx = tilt_plane->a / 65536.0;
   tilt_ny = tilt_plane->b / 65536.0;
   tilt_nz = tilt_plane->c / 65536.0;
   tilt_vx = viewx / 65536.0;
   tilt_vy = viewy / 65536.0;
   tilt_num = -(tilt_nx * tilt_vx + tilt_ny * tilt_vy +
                tilt_nz * (viewz / 65536.0) + tilt_plane->d / 65536.0);
   tilt_sin = finesine[viewangle >> ANGLETOFINESHIFT] / 65536.0;
   tilt_cos = finecosine[viewangle >> ANGLETOFINESHIFT] / 65536.0;
   tilt_ifx = 65536.0 / projectionx;
   tilt_ify = 65536.0 / projectiony;
}

/* world-space hit of the ray through pixel (x, y); returns depth t and
 * writes the hit's world coordinates */
static double R_TiltedHit(int x, int y, double *wx, double *wy)
{
   double sx = (x - centerx + 0.5) * tilt_ifx;
   double sy = (centery - y - 0.5) * tilt_ify;
   double dx = tilt_cos + sx * tilt_sin;
   double dy = tilt_sin - sx * tilt_cos;
   double den = tilt_nx * dx + tilt_ny * dy + tilt_nz * sy;
   double tt = den ? tilt_num / den : 0;
   *wx = tilt_vx + tt * dx;
   *wy = tilt_vy + tt * dy;
   return tt;
}

#define TILT_CHUNK 8

static void R_MapTiltedPlane(int y, int x1, int x2, draw_span_vars_t *dsvars)
{
   /* The flat-plane mapper (R_MapPlane) caches per-row xstep/ystep keyed on
    * planeheight via cachedheight[y].  A tilted plane writes spans at row y
    * with its own per-span steps but never updates cachedheight[y], so a later
    * flat plane that happens to share that planeheight takes the cache hit and
    * reuses steps left over from before the tilted draw -- the flat floor then
    * shears into diagonal stripes (visible on large floors behind/around
    * sloped sectors).  Invalidate this row's cache so the next flat span at
    * row y recomputes its steps. */
   double wx, wy, wx2, wy2, tt;
   int cx1 = x1;

   cachedheight[y] = 0;

   while (cx1 <= x2)
   {
      int cx2 = cx1 + TILT_CHUNK - 1;
      int len;
      if (cx2 > x2)
         cx2 = x2;
      len = cx2 - cx1 + 1;

      tt = R_TiltedHit(cx1, y, &wx, &wy);
      R_TiltedHit(cx2 + 1, y, &wx2, &wy2);   /* one past, exact end step */
      /* The span's screen rows were already bounded to this plane's visible
       * extent by the visplane top[]/bottom[] walk, so the chunk is genuinely
       * on-surface.  R_TiltedHit's t is the signed ray parameter: it comes out
       * negative when the eye is on the plane's back face relative to the
       * up-facing normal p_slope enforces (a sloped ceiling viewed from below,
       * or a reverse-facing slab), even though the intersection the visplane
       * selected is real and visible.  Rejecting t <= 0 dropped those chunks
       * and left the framebuffer uncovered (a torn residue band over sloped
       * ceilings and reverse-facing slabs).  Use |t| for depth and only skip
       * the genuinely degenerate parallel ray. */
      if (tt == 0.0)
      {
         cx1 = cx2 + 1;
         continue;                            /* ray parallel to plane */
      }
      if (tt < 0.0)
         tt = -tt;

      dsvars->xfrac = xoffs + (fixed_t)(wx * 65536.0);
      dsvars->yfrac = yoffs - (fixed_t)(wy * 65536.0);
      dsvars->xstep = (fixed_t)((wx2 - wx) * 65536.0) / len;
      dsvars->ystep = -((fixed_t)((wy2 - wy) * 65536.0) / len);
      if (drawvars.filterfloor == RDRAW_FILTER_LINEAR)
      {
         dsvars->xfrac -= (FRACUNIT >> 1);
         dsvars->yfrac -= (FRACUNIT >> 1);
      }

      if (!(dsvars->colormap = fixedcolormap))
      {
         fixed_t distance = (fixed_t)(tt * 65536.0);
         unsigned index = distance >> LIGHTZSHIFT;
         if (index >= MAXLIGHTZ)
            index = MAXLIGHTZ - 1;
         dsvars->z = distance;
         dsvars->colormap = planezlight[index];
         if (r_smooth_shading && fullcolormap)
         {
            /* Continuous base darkness from the raw 0..255 sector light
             * (planerawlight) instead of the 16-band planelightlevel.  This
             * agrees with the band formula at band centres but interpolates
             * between them, so adjacent sectors with close light levels no
             * longer snap to different bands (the floor light banding). */
            int startmap = ((255 - planerawlight) * 2 * (LIGHTLEVELS-1)
                            * SMOOTH_WEIGHTS) / (255 * LIGHTLEVELS);
            int scale    = FixedDiv((320/2*FRACUNIT),((int)index+1)<<LIGHTZSHIFT);
            int fine2    = startmap
                         - (scale >> (LIGHTSCALESHIFT-SMOOTH_WEIGHTS_SHIFT))/DISTMAP;
            if (fine2 < 0)                       fine2 = 0;
            else if (fine2 > SMOOTH_WEIGHTS-1)   fine2 = SMOOTH_WEIGHTS-1;
            r_fine_lightweight = (SMOOTH_WEIGHTS-1) - fine2;
            r_fine_colormap    = dsvars->colormap;
         }
         else
            r_fine_lightweight = -1;
      }
      else
      {
         dsvars->z = 0;
         r_fine_lightweight = -1;
      }

      dsvars->y = y;
      dsvars->x1 = cx1;
      dsvars->x2 = cx2;
      R_DrawSpan(dsvars);
      cx1 = cx2 + 1;
   }
}

// R_MapPlane
//
// Uses global vars:
//  planeheight
//  dsvars.source
//  basexscale
//  baseyscale
//  viewx
//  viewy
//  xoffs
//  yoffs
//
// BASIC PRIMITIVE
//

#define DL_FLAT_CHUNK 8   /* span chunk width (px) for dynamic-light shading */

/* World (x,y) of the plane point under screen column `col` at this row's
 * `distance` (the view ray through the column intersected with the plane). */
static void R_PlaneColumnWorld(int col, fixed_t distance, int *wx, int *wy)
{
   unsigned oidx = (unsigned)xtoviewangle[col] >> ANGLETOFINESHIFT;
   fixed_t  oc   = finecosine[oidx];
   fixed_t  hd   = (oc > 4096) ? FixedDiv(distance, oc) : distance;
   unsigned ridx = (unsigned)(viewangle + xtoviewangle[col]) >> ANGLETOFINESHIFT;
   *wx = (viewx + FixedMul(hd, finecosine[ridx])) >> FRACBITS;
   *wy = (viewy + FixedMul(hd, finesine[ridx]))   >> FRACBITS;
}

static void R_MapPlane(int y, int x1, int x2, draw_span_vars_t *dsvars)
{
   fixed_t distance;
   int dx, dy;
   unsigned index;

   if (tilt_plane)
   {
      R_MapTiltedPlane(y, x1, x2, dsvars);
      return;
   }

   if ((dy = abs(centery - y)) == 0)
      return; // skip early if there's no change

   if (planeheight != cachedheight[y])
   {
      cachedheight[y] = planeheight;
      distance = cacheddistance[y] = FixedMul (planeheight, yslope[y]);
      dsvars->xstep = cachedxstep[y] = FixedMul (viewsin, planeheight) / dy;
      dsvars->ystep = cachedystep[y] = FixedMul (viewcos, planeheight) / dy;

      /* hor+ widescreen uses anisotropic focal lengths: the horizontal
       * focal is projectionx (== focallength) while the vertical focal
       * is projectiony.  The plane walk xstep/ystep is built through the
       * vertical dy/yslope path, so it carries the vertical focal; the
       * horizontal world rate must therefore be scaled by
       * projectiony/projectionx (the focal-length anisotropy), per the
       * pinhole floor projection d(worldX)/d(screenX) = worldZ/fx.  At
       * 4:3 projectionx == projectiony and this is a no-op.
       *
       * NB: the reference is projectiony, NOT centerxfrac -- they happen
       * to coincide at 4:3 but diverge once the buffer width is clamped
       * (16:9 at the MAX_SCREENWIDTH ceiling needs ~unity here even
       * though centerxfrac/projectionx would be 1.33). */
      {
         if (projectionx && projectionx != projectiony)
         {
            dsvars->xstep = cachedxstep[y] =
               (fixed_t)(((int64_t)dsvars->xstep * projectiony) / projectionx);
            dsvars->ystep = cachedystep[y] =
               (fixed_t)(((int64_t)dsvars->ystep * projectiony) / projectionx);
         }
      }
   }
   else
   {
      distance = cacheddistance[y];
      dsvars->xstep = cachedxstep[y];
      dsvars->ystep = cachedystep[y];
   }

   /* dx*xstep must be computed in 64 bits: at wide aspects dx reaches
    * +/-(viewwidth/2) (~1280 at 2560) and xstep is large for near rows,
    * so the 32-bit product overflows and wraps -- which manifests as the
    * floor texture warping/jumping as the view pans (panning changes
    * which spans are near and flips the overflow on and off).  Vanilla
    * narrowly avoided this at 320-wide; widescreen does not. */
   dx = x1 - centerx;
   dsvars->xfrac = xoffs + viewx + FixedMul(viewcos, distance)
                 + (fixed_t)((int64_t)dx * dsvars->xstep);
   dsvars->yfrac = yoffs - viewy - FixedMul(viewsin, distance)
                 + (fixed_t)((int64_t)dx * dsvars->ystep);

   if (drawvars.filterfloor == RDRAW_FILTER_LINEAR) {
      dsvars->xfrac -= (FRACUNIT>>1);
      dsvars->yfrac -= (FRACUNIT>>1);
   }

   if (!(dsvars->colormap = fixedcolormap))
   {
      dsvars->z = distance;
      index = distance >> LIGHTZSHIFT;
      if (index >= MAXLIGHTZ )
         index = MAXLIGHTZ-1;
      dsvars->colormap = planezlight[index];

      /* Smooth shading: publish the distance darkness at 64-step resolution.
       * R_InitLightTables baked planezlight at NUMCOLORMAPS (32) granularity
       * via:  level = startmap - (scale>>LIGHTSCALESHIFT)/DISTMAP, with
       *       startmap = ((LIGHTLEVELS-1-light)*2)*NUMCOLORMAPS/LIGHTLEVELS
       * and scale = FixedDiv(320/2*FRACUNIT,(index+1)<<LIGHTZSHIFT).
       * Recompute the same value with NUMCOLORMAPS replaced by SMOOTH_WEIGHTS
       * so the darkness index carries sub-band precision while agreeing with
       * Default at the band centres.  Mapped to a 0..(SMOOTH_WEIGHTS-1)
       * weight (max = brightest). */
      if (r_smooth_shading && fullcolormap)
      {
         /* Continuous base darkness from the raw 0..255 sector light
          * (planerawlight) instead of the 16-band planelightlevel, so
          * adjacent sectors with close light levels no longer snap to
          * different bands (the floor light banding).  Agrees with the band
          * formula at band centres. */
         int startmap = ((255 - planerawlight) * 2 * (LIGHTLEVELS-1)
                         * SMOOTH_WEIGHTS) / (255 * LIGHTLEVELS);
         int scale    = FixedDiv((320/2*FRACUNIT),(index+1)<<LIGHTZSHIFT);
         /* Both halves scaled to SMOOTH_WEIGHTS resolution: startmap via the
          * *SMOOTH_WEIGHTS factor above, and the distance term via the shift
          * reduced by log2(SMOOTH_WEIGHTS/NUMCOLORMAPS)=SMOOTH_WEIGHTS_SHIFT,
          * so fine agrees with the 32-band zlight at the band centres. */
         int fine2    = startmap - (scale >> (LIGHTSCALESHIFT-SMOOTH_WEIGHTS_SHIFT))/DISTMAP;
         if (fine2 < 0)                       fine2 = 0;
         else if (fine2 > SMOOTH_WEIGHTS-1)   fine2 = SMOOTH_WEIGHTS-1;
         r_fine_lightweight = (SMOOTH_WEIGHTS-1) - fine2;
         r_fine_colormap    = dsvars->colormap;
      }
      else
         r_fine_lightweight = -1;

      /* nextcolormap is only read by the *_LinearZ span drawers, which are
       * selected by filterz (NOT filterfloor: PointUV_LinearZ reads it too,
       * and filterz goes LINEAR whenever diminished_lighting is on).  Skip
       * the extra table index only when filterz is not LINEAR. */
      if (drawvars.filterz == RDRAW_FILTER_LINEAR)
         dsvars->nextcolormap = planezlight[index+1 >= MAXLIGHTZ ? MAXLIGHTZ-1 : index+1];
   }
   else
   {
      dsvars->z = 0;
   }

   dsvars->y = y;

   /* Dynamic point lights on the floor/ceiling.  Split the span into short
    * chunks, intersect each chunk's view ray with the plane to get its world
    * point, and re-pick the colourmap for the light-boosted band.  Only when
    * plane_dynlit; otherwise the single span below runs unchanged. */
   if (plane_dynlit && !fixedcolormap)
   {
      const fixed_t bxf = dsvars->xfrac, byf = dsvars->yfrac;
      int wx_a, wy_a, wx_b, wy_b, cx;

      /* Span-level cull: if no light on this plane reaches the span, draw it
       * as one span with the base colourmap (already set) -- this keeps the
       * common far-from-light span off the chunk path entirely. */
      R_PlaneColumnWorld(x1, distance, &wx_a, &wy_a);
      R_PlaneColumnWorld(x2, distance, &wx_b, &wy_b);
      if (!R_PlaneSpanLit(wx_a, wy_a, wx_b, wy_b))
      {
         dsvars->x1 = x1;
         dsvars->x2 = x2;
         R_DrawSpan(dsvars);
         return;
      }

      for (cx = x1; cx <= x2; cx += DL_FLAT_CHUNK)
      {
         int ex = cx + DL_FLAT_CHUNK - 1;
         int mid, wx, wy, boost, band;

         if (ex > x2) ex = x2;
         mid = (cx + ex) >> 1;
         R_PlaneColumnWorld(mid, distance, &wx, &wy);

         boost = R_PlaneBoost(wx, wy);
         band  = planelightlevel + (boost >> LIGHTSEGSHIFT);
         if (band > LIGHTLEVELS-1) band = LIGHTLEVELS-1;
         dsvars->colormap = zlight[band][index];
         if (drawvars.filterz == RDRAW_FILTER_LINEAR)
            dsvars->nextcolormap =
               zlight[band][index+1 >= MAXLIGHTZ ? MAXLIGHTZ-1 : index+1];
         if (r_smooth_shading && fullcolormap)
         {
            int raw = planerawlight + boost;
            int startmap, scale, fine2;
            if (raw > 255) raw = 255;
            startmap = ((255 - raw) * 2 * (LIGHTLEVELS-1) * SMOOTH_WEIGHTS)
                       / (255 * LIGHTLEVELS);
            scale    = FixedDiv((320/2*FRACUNIT),(index+1)<<LIGHTZSHIFT);
            fine2    = startmap
                       - (scale >> (LIGHTSCALESHIFT-SMOOTH_WEIGHTS_SHIFT))/DISTMAP;
            if (fine2 < 0)                     fine2 = 0;
            else if (fine2 > SMOOTH_WEIGHTS-1) fine2 = SMOOTH_WEIGHTS-1;
            r_fine_lightweight = (SMOOTH_WEIGHTS-1) - fine2;
            r_fine_colormap    = dsvars->colormap;
         }
         else
            r_fine_lightweight = -1;

         dsvars->xfrac = bxf + (fixed_t)((int64_t)(cx - x1) * dsvars->xstep);
         dsvars->yfrac = byf + (fixed_t)((int64_t)(cx - x1) * dsvars->ystep);
         dsvars->x1 = cx;
         dsvars->x2 = ex;
         R_DrawSpan(dsvars);
      }
      return;
   }

   dsvars->x1 = x1;
   dsvars->x2 = x2;

   R_DrawSpan(dsvars);
}

//
// R_ClearPlanes
// At begining of frame.
//

void R_ClearPlanes(void)
{
   int i;

   // opening / clipping determination
   for (i=0 ; i<viewwidth ; i++)
   {
      floorclip[i]   = viewheight;
      ceilingclip[i] = -1;
   }

   for (i=0;i<MAXVISPLANES;i++)    // new code -- killough
      for (*freehead = visplanes[i], visplanes[i] = NULL; *freehead; )
         freehead = &(*freehead)->next;

   lastopening = openings;

   // texture calculation
   memset (cachedheight, 0, sizeof(cachedheight));

   // scale will be unit scale at SCREENWIDTH/2 distance
   basexscale = FixedDiv (viewsin,projection);
   baseyscale = FixedDiv (viewcos,projection);
}

// New function, by Lee Killough

static visplane_t *new_visplane(unsigned hash)
{
  visplane_t *check = freetail;
  if (!check)
    check = calloc(1, sizeof *check);
  else
    if (!(freetail = freetail->next))
      freehead = &freetail;
  check->next = visplanes[hash];
  visplanes[hash] = check;
  return check;
}

/*
 * R_DupPlane
 *
 * cph 2003/04/18 - create duplicate of existing visplane and set initial range
 */
visplane_t *R_DupPlane(const visplane_t *pl, int start, int stop)
{
      unsigned hash = visplane_hash(pl->picnum, pl->lightlevel, pl->height);
      visplane_t *new_pl = new_visplane(hash);

      new_pl->height = pl->height;
      new_pl->picnum = pl->picnum;
      new_pl->lightlevel = pl->lightlevel;
      new_pl->xoffs = pl->xoffs;           // killough 2/28/98
      new_pl->yoffs = pl->yoffs;
      new_pl->slope = pl->slope;
      new_pl->skybox = pl->skybox;
      new_pl->minx = start;
      new_pl->maxx = stop;
      new_pl->modified = 0;
      new_pl->translucent = pl->translucent;
      memset(new_pl->top, 0xff, sizeof new_pl->top);
      return new_pl;
}
//
// R_FindPlane
//
// killough 2/28/98: Add offsets

#ifdef PRBOOM_RENDER_PROFILE
static visplane_t *R_FindPlane_impl(fixed_t height, int picnum, int lightlevel,
                        fixed_t xoffs, fixed_t yoffs, const secplane_t *slope,
                        int skybox);
visplane_t *R_FindPlane(fixed_t height, int picnum, int lightlevel,
                        fixed_t xoffs, fixed_t yoffs, const secplane_t *slope,
                        int skybox)
{
   extern double prof_findplane_usec;
   visplane_t *r;
   double _t0 = I_RenderProfileUsec();
   r = R_FindPlane_impl(height, picnum, lightlevel, xoffs, yoffs, slope, skybox);
   prof_findplane_usec += (I_RenderProfileUsec() - _t0);
   return r;
}
static visplane_t *R_FindPlane_impl(fixed_t height, int picnum, int lightlevel,
                        fixed_t xoffs, fixed_t yoffs, const secplane_t *slope,
                        int skybox)
#else
visplane_t *R_FindPlane(fixed_t height, int picnum, int lightlevel,
                        fixed_t xoffs, fixed_t yoffs, const secplane_t *slope,
                        int skybox)
#endif
{
   visplane_t *check;
   unsigned hash;                      // killough

   if (picnum == skyflatnum || picnum & PL_SKYFLAT)
      height = lightlevel = 0;         // killough 7/19/98: most skies map together

   // New visplane algorithm uses hash table -- killough
   hash = visplane_hash(picnum,lightlevel,height);

   for (check=visplanes[hash]; check; check=check->next)  // killough
      if (height == check->height &&
            picnum == check->picnum &&
            lightlevel == check->lightlevel &&
            xoffs == check->xoffs &&      // killough 2/28/98: Add offset checks
            yoffs == check->yoffs &&
            !check->translucent &&        /* water planes never merge with opaque */
            slope == check->slope &&      /* tilted planes never merge with flat */
            skybox == check->skybox)      /* different skyboxes never merge */
         return check;

   check = new_visplane(hash);         // killough

   check->height = height;
   check->picnum = picnum;
   check->lightlevel = lightlevel;
   check->minx = viewwidth; // Was SCREENWIDTH -- killough 11/98
   check->maxx = -1;
   check->xoffs = xoffs;               // killough 2/28/98: Save offsets
   check->yoffs = yoffs;
   check->slope = slope;
   check->modified = 0;
   check->translucent = 0;
   check->skybox = skybox;

   memset (check->top, 0xff, sizeof check->top);

   return check;
}

/*
 * R_FindWaterPlane -- allocate a dedicated translucent visplane for a
 * 3D-floor (swimmable) water surface.  Unlike R_FindPlane it never shares
 * with an opaque plane (its translucent flag would wrongly blend an ordinary
 * floor), so it is allocated fresh; a swimmable sector has at most one per
 * subsector, span-extended across that subsector's segs by R_CheckPlane.
 */
visplane_t *R_FindWaterPlane(fixed_t height, int picnum, int lightlevel)
{
   visplane_t *check = new_visplane(visplane_hash(picnum, lightlevel, height));

   check->height = height;
   check->picnum = picnum;
   check->lightlevel = lightlevel;
   check->minx = viewwidth;
   check->maxx = -1;
   check->xoffs = 0;
   check->yoffs = 0;
   check->slope = NULL;
   check->modified = 0;
   check->translucent = 1;
   check->skybox = -1;

   memset (check->top, 0xff, sizeof check->top);

   return check;
}

//
// R_CheckPlane
//
visplane_t *R_CheckPlane(visplane_t *pl, int start, int stop)
{
   int intrl, intrh, unionl, unionh, x;

   if (start < pl->minx)
   {
      intrl   = pl->minx;
      unionl  = start;
   }
   else
   {
      unionl  = pl->minx;
      intrl   = start;
   }

   if (stop  > pl->maxx)
   {
      intrh   = pl->maxx;
      unionh  = stop;
   }
   else
   {
      unionh  = pl->maxx;
      intrh   = stop;
   }

   for(x = intrl; x <= intrh; x++)
   {
      if(pl->top[x] != 0xff)
         break;
   }

   if (x > intrh)
   { /* Can use existing plane; extend range */
      pl->minx = unionl; pl->maxx = unionh;
      return pl;
   }

   /* Cannot use existing plane; create a new one */
   return R_DupPlane(pl,start,stop);
}

//
// R_MakeSpans
//

static void R_MakeSpans(int x, unsigned int t1, unsigned int b1,
                        unsigned int t2, unsigned int b2,
                        draw_span_vars_t *dsvars)
{
   for (; t1 < t2 && t1 <= b1; t1++)
      R_MapPlane(t1, spanstart[t1], x-1, dsvars);
   for (; b1 > b2 && b1 >= t1; b1--)
      R_MapPlane(b1, spanstart[b1] ,x-1, dsvars);
   while (t2 < t1 && t2 <= b2)
      spanstart[t2++] = x;
   while (b2 > b1 && b2 >= t2)
      spanstart[b2--] = x;
}

// New function, by Lee Killough

/* Heretic sky hack (from dsda-doom): Heretic sky textures are declared 128
 * tall but their single patch is actually 200 pixels.  Drawing them through
 * the normal composite path tiles/mirrors the 128-tall texture, which only
 * showed once free-look let the view pitch up far enough to expose the area
 * above the horizon.  When the texture is Heretic and is a single 200-tall
 * patch, return that raw patch so the sky can be drawn from the full 200-row
 * data with a 200-anchored texturemid (see R_DoDrawPlane). */
static const rpatch_t *R_HackedSkyPatch(int texturenum)
{
   if (heretic && textures[texturenum]->patchcount == 1)
   {
      int patchnum = textures[texturenum]->patches[0].patch;
      const rpatch_t *patch = R_CachePatchNum(patchnum);
      if (patch->height == 200)
         return patch;          /* caller releases the lock after drawing */
      R_UnlockPatchNum(patchnum);
   }
   return NULL;
}

static void R_DoDrawPlane(visplane_t *pl)
{
   int x;
   draw_column_vars_t dcvars;
   R_DrawColumn_f colfunc = R_GetDrawColumnFunc(RDC_PIPELINE_STANDARD, drawvars.filterwall, drawvars.filterz);

   R_SetDefaultDrawColumnVars(&dcvars);

   if (!pl->modified)
      return;

   if (pl->minx <= pl->maxx)
   {
      if (pl->picnum == skyflatnum || pl->picnum & PL_SKYFLAT)
      { // sky flat
         int texture;
         const rpatch_t *tex_patch;
         angle_t an, flip;

         /* 3D skybox active and this is the main (non-skybox) pass: leave the
          * sky pixels showing the skybox scene already rendered underneath
          * rather than overwriting them with the flat sky texture. */
         if (skyview.active && !r_in_skybox)
            return;

         // killough 10/98: allow skies to come from sidedefs.
         // Allows scrolling and/or animated skies, as well as
         // arbitrary multiple skies per level without having
         // to use info lumps.

         an = viewangle;

         if (pl->picnum & PL_SKYFLAT)
         {
            // Sky Linedef
            const line_t *l = &lines[pl->picnum & ~PL_SKYFLAT];

            // Sky transferred from first sidedef
            const side_t *s = *l->sidenum + sides;

            // Texture comes from upper texture of reference sidedef
            texture = texturetranslation[s->toptexture];

            // Horizontal offset is turned into an angle offset,
            // to allow sky rotation as well as careful positioning.
            // However, the offset is scaled very small, so that it
            // allows a long-period of sky rotation.

            an += s->textureoffset;

            // Vertical offset allows careful sky positioning.

            dcvars.texturemid = s->rowoffset - 28*FRACUNIT;

            // We sometimes flip the picture horizontally.
            //
            // Doom always flipped the picture, so we make it optional,
            // to make it easier to use the new feature, while to still
            // allow old sky textures to be used.

            flip = l->special==272 ? 0u : ~0u;

            if (skystretch)
            {
              int skyheight = textureheight[texture]>>FRACBITS;
              dcvars.texturemid = (int64_t)dcvars.texturemid * skyheight / SKYSTRETCH_HEIGHT;
            }
         }
         else
         {    // Normal Doom sky, only one allowed per level
            dcvars.texturemid = skytexturemid;    // Default y-offset
            /* ZDoom ANIMDEFS can animate the sky texture, which is drawn
             * outside the texturetranslation path walls use; read through
             * the translation so an animated sky cycles.  Without
             * ANIMDEFS the translation is the identity. */
            if (U_ZAnimPresent)
               texture = texturetranslation[skytexture];
            else
               texture = skytexture;          // Default texture
            flip = 0;                         // Doom flips it

            /* Hexen scrolls the sky horizontally.  Sky1ColumnOffset
             * accumulates Sky1ScrollDelta each tic (see P_UpdateSpecials);
             * fold it into the view angle so the sky drifts. */
            if (hexen)
               an += Sky1ColumnOffset;
         }

         /* Sky is always drawn full bright, i.e. colormaps[0] is used.
          * Because of this hack, sky is not affected by INVUL inverse mapping.
          * Until Boom fixed this. Compat option added in MBF. */

         if (comp[comp_skymap] || !(dcvars.colormap = fixedcolormap))
            dcvars.colormap = fullcolormap;          // killough 3/20/98

         dcvars.nextcolormap = dcvars.colormap; // for filtering -- POPE

         //dcvars.texturemid = skytexturemid;
         dcvars.texheight = textureheight[texture]>>FRACBITS; // killough
         dcvars.iscale = skyiscale;

         {
            /* Heretic 200-tall single-patch sky: draw straight from the raw
             * patch (full 200 rows) instead of the 128-tall composite, so the
             * sky does not mirror/tile.  Anchor it the way dsda-doom does
             * (row 200 at the horizon, scaled across the screen height) and,
             * crucially, clamp each column's top so the texture coordinate
             * never runs above row 0 of the patch.  Without the clamp, looking
             * far enough up pushes centery past the patch and the column
             * drawer -- which wraps non-power-of-two textures -- tiles the sky,
             * producing a hard horizontal seam.  Clamping makes the sky scale
             * to fill and hold its top edge instead of wrapping. */
            const rpatch_t *hacked = R_HackedSkyPatch(texture);
            if (hacked)
            {
               int xm1;
               int skypatchnum = textures[texture]->patches[0].patch;
               /* Smallest screen row whose texture coordinate is still inside
                * the patch.  The column drawer's linear-filter path samples at
                * frac - FRACUNIT/2, so require frac >= FRACUNIT/2 at the top
                * row to avoid reading row -1 (garbage) right at the clamp:
                *   texturemid + (y-centery)*iscale >= FRACUNIT/2
                *   y >= centery - (texturemid - FRACUNIT/2)/iscale
                * Computed from the actual texturemid/iscale rather than the
                * algebraic SCREENHEIGHT so integer truncation in iscale cannot
                * leave the boundary row a fraction below zero. */
               int sky_top_clamp;
               fixed_t sky_texturemid = 200 << FRACBITS;
               fixed_t sky_iscale     = (200 << FRACBITS) / SCREENHEIGHT;

               dcvars.texheight = hacked->height;
               dcvars.texturemid = sky_texturemid;
               dcvars.iscale = sky_iscale;
               sky_top_clamp = centery -
                  (int)((sky_texturemid - (FRACUNIT >> 1)) / sky_iscale);

               for (x = pl->minx; (dcvars.x = x) <= pl->maxx; x++)
               {
                  int yl = pl->top[x];
                  if (yl != -1 && yl <= (dcvars.yh = pl->bottom[x])) // dropoff overflow
                  {
                     /* Raise the column top to where the sky still has texture,
                      * so the coordinate clamps at row 0 instead of wrapping. */
                     if (yl < sky_top_clamp)
                        yl = sky_top_clamp;
                     if (yl > dcvars.yh)
                        continue;
                     dcvars.yl = yl;
                     xm1 = x > 0 ? x - 1 : 0;
                     dcvars.source = R_GetPatchColumn(hacked, ((an + xtoviewangle[x])^flip) >> ANGLETOSKYSHIFT)->pixels;
                     dcvars.prevsource = R_GetPatchColumn(hacked, ((an + xtoviewangle[xm1])^flip) >> ANGLETOSKYSHIFT)->pixels;
                     dcvars.nextsource = R_GetPatchColumn(hacked, ((an + xtoviewangle[x+1])^flip) >> ANGLETOSKYSHIFT)->pixels;
                     colfunc(&dcvars);
                  }
               }

               /* R_HackedSkyPatch took a lock via R_CachePatchNum; release it
                * so the patch does not stay pinned and leak a lock per frame. */
               R_UnlockPatchNum(skypatchnum);
               return;
            }
         }

         tex_patch = R_CacheTextureCompositePatchNum(texture);

         // killough 10/98: Use sky scrolling offset, and possibly flip picture
         for (x = pl->minx; (dcvars.x = x) <= pl->maxx; x++)
            if ((dcvars.yl = pl->top[x]) != -1 && dcvars.yl <= (dcvars.yh = pl->bottom[x])) // dropoff overflow
            {
               /* xtoviewangle is sized MAX_SCREENWIDTH+1 (the +1 slot
                * covers the x+1 lookahead at x == SCREENWIDTH-1).  The
                * x-1 lookback has no such slot, so when a sky visplane
                * starts at column 0 (any outdoor area facing the sky --
                * Chex Quest E1M1's starting position is the motivating
                * case) the prevsource fetch reads xtoviewangle[-1].
                * Clamp to xtoviewangle[0]: at the screen edge there is
                * no previous column to filter against, so reusing the
                * current column's angle is the natural fallback. */
               int xm1 = x > 0 ? x - 1 : 0;
               dcvars.source = R_GetTextureColumn(tex_patch, ((an + xtoviewangle[x])^flip) >> ANGLETOSKYSHIFT);
               dcvars.prevsource = R_GetTextureColumn(tex_patch, ((an + xtoviewangle[xm1])^flip) >> ANGLETOSKYSHIFT);
               dcvars.nextsource = R_GetTextureColumn(tex_patch, ((an + xtoviewangle[x+1])^flip) >> ANGLETOSKYSHIFT);
               colfunc(&dcvars);
            }

         R_UnlockTextureCompositePatchNum(texture);

      }
      else
      {     // regular flat
         int stop, light;
         draw_span_vars_t dsvars;

         /* Synthetic flats (textures on floors) live outside the lump
          * range and the animation translation table. */
         if (R_IsSyntheticFlat(pl->picnum))
         {
            dsvars.source = R_GetSyntheticFlat(pl->picnum);
            dsvars.brightmask = NULL;     /* synthetic flats: no brightmap */
         }
         else
         {
            int fnum = flattranslation[pl->picnum];
            dsvars.source = W_CacheLumpNum(firstflat + fnum);
            dsvars.brightmask = U_BrightmaskForFlat(fnum);
         }

         xoffs = pl->xoffs;  // killough 2/28/98: Add offsets
         yoffs = pl->yoffs;
         planeheight = D_abs(pl->height-viewz);
         light = (pl->lightlevel >> LIGHTSEGSHIFT) + extralight;

         if (light >= LIGHTLEVELS)
            light = LIGHTLEVELS-1;

         if (light < 0)
            light = 0;

         stop = pl->maxx + 1;
         planezlight = zlight[light];
         planelightlevel = light;
         /* Dynamic point lights: build the per-plane light sublist (lights
          * that reach this plane's world z) so R_MapPlane can brighten the
          * floor/ceiling near lights.  Gated here to skip the common case of
          * a plane no light reaches. */
         plane_dynlit = R_DynLightsActive() &&
                        R_PlanePrepareLights(pl->height >> FRACBITS) > 0;
         /* Smooth mode bases its sub-band darkness on planelightlevel, which
          * is the sector light quantised to LIGHTLEVELS(16) bands.  On maps
          * with many adjacent sectors at slightly different light levels that
          * quantisation makes the floor/ceiling break into hard light bands.
          * Keep the raw 0..255 sector light (plus extralight, in the same
          * LIGHTSEGSHIFT units the band uses) so the Smooth path can place the
          * base darkness continuously between bands instead. */
         planerawlight = pl->lightlevel + (extralight << LIGHTSEGSHIFT);
         if (planerawlight < 0)        planerawlight = 0;
         else if (planerawlight > 255) planerawlight = 255;
         pl->top[pl->minx-1] = pl->top[stop] = 0xffffffffu; // dropoff overflow

         if (pl->slope)
            R_TiltedPlaneSetup(pl);
         else
            tilt_plane = NULL;

         if (pl->translucent)
            r_span_translucent = 1;

         for (x = pl->minx ; x <= stop ; x++)
            R_MakeSpans(x,pl->top[x-1],pl->bottom[x-1],
                  pl->top[x],pl->bottom[x], &dsvars);

         r_span_translucent = 0;

         /* The translucent water flat blended 50/50 over the dark volume reads
          * too dim; lift the water-surface span toward a blue-grey caustic so
          * the water level is clearly visible -- brightest at the surface line,
          * easing to a gentle blue floor in the depths.  Uses the plane's own
          * per-column span, so the lit surface is consistent across the whole
          * opening (no per-wall-seg approximation). */
         if (pl->translucent)
         {
            extern void R_WaterSurfaceLift(int x, int y0, int y1, int bandtop);
            int xx;
            for (xx = pl->minx; xx <= stop; xx++)
            {
               unsigned t = pl->top[xx], b = pl->bottom[xx];
               if (t > b || b == 0xffffffffu) continue;
               R_WaterSurfaceLift(xx, (int)t, (int)b, (int)t);
            }
         }

         tilt_plane = NULL;

         if (!R_IsSyntheticFlat(pl->picnum))
            W_UnlockLumpNum(firstflat + flattranslation[pl->picnum]);
      }
   }
}

//
// RDrawPlanes
// At the end of each frame.
//

int R_CollectSkyboxSpan(int sbidx, short *out_top, short *out_bot)
{
  int x, any = 0, i;
  visplane_t *pl;
  for (x = 0; x < viewwidth; x++) { out_top[x] = 32767; out_bot[x] = -1; }
  for (i = 0; i < MAXVISPLANES; i++)
    for (pl = visplanes[i]; pl; pl = pl->next)
    {
      if (!pl->modified || pl->skybox != sbidx)
        continue;
      if (!(pl->picnum == skyflatnum || (pl->picnum & PL_SKYFLAT)))
        continue;
      for (x = pl->minx; x <= pl->maxx; x++)
      {
        int t = (int)pl->top[x];
        int b = (int)pl->bottom[x];
        /* top[x] == -1 (0xffffffff fill) is the "no span" sentinel, exactly
         * as the sky column draw tests it; skip those columns. */
        if (t == -1 || t > b)
          continue;
        if (t < out_top[x]) out_top[x] = (short)t;
        if (b > out_bot[x]) out_bot[x] = (short)b;
        any = 1;
      }
    }
  return any;
}

void R_DrawPlanes (void)
{
  int i;
  visplane_t *pl;

  /* Opaque planes first, then translucent 3D-floor water surfaces, so the
   * water blends over the floor (and submerged walls) already in the
   * framebuffer rather than over stale pixels. */
  for (i=0;i<MAXVISPLANES;i++)
     for (pl=visplanes[i]; pl; pl=pl->next)
        if (!pl->translucent)
           R_DoDrawPlane(pl);

  for (i=0;i<MAXVISPLANES;i++)
     for (pl=visplanes[i]; pl; pl=pl->next)
        if (pl->translucent)
           R_DoDrawPlane(pl);
}
