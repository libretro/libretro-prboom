/* r_voxel.c: software voxel rasteriser.
 *
 * Stage 2 placed and routed voxel vissprites; this stage (3a) draws them.
 * The approach here is deliberately the simplest correct one: splat every
 * surface voxel as a projected point.  Each voxel's model-space cell is
 * mapped to world space (centred on the thing through the model pivot),
 * transformed into view space with the standard camera basis, projected to
 * a screen pixel, depth-tested against a per-pixel z-buffer and plotted
 * through the vissprite's lit colormap.  Model-space yaw rotation and the
 * faster column-run rasterisation come in later sub-stages; isolating the
 * projection first lets the world->screen math be validated on its own.
 *
 * KVX axes: x,y are the horizontal footprint and z is vertical with z=0 at
 * the top of the model (z increases downward), so a cell's height above the
 * thing origin is (zsiz - z). */

#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "tables.h"
#include "r_defs.h"
#include "r_main.h"
#include "r_state.h"
#include "r_draw.h"
#include "v_video.h"
#include "u_voxel.h"
#include "r_voxel.h"

/* exported from r_draw.c: palette index + light colormap -> screen pixel */
extern const uint16_t *R_ComposedColormap(const lighttable_t *colormap);

/* per-pixel depth buffer for the voxel currently being drawn (view-space z,
 * smaller = nearer); lazily sized to the screen. */
static fixed_t  *vox_zbuf;
static int       vox_zbuf_len;
static unsigned  vox_zbuf_stamp;      /* bumped per draw to avoid clearing */
static unsigned *vox_zbuf_seen;

static dbool vox_zbuf_ensure(int npix)
{
  if (vox_zbuf_len >= npix)
    return true;
  free(vox_zbuf);
  free(vox_zbuf_seen);
  vox_zbuf      = malloc((size_t)npix * sizeof(*vox_zbuf));
  vox_zbuf_seen = calloc((size_t)npix, sizeof(*vox_zbuf_seen));
  if (!vox_zbuf || !vox_zbuf_seen)
  {
    free(vox_zbuf); free(vox_zbuf_seen);
    vox_zbuf = NULL; vox_zbuf_seen = NULL; vox_zbuf_len = 0;
    return false;
  }
  vox_zbuf_len = npix;
  return true;
}

void R_DrawVoxel(vissprite_t *vis)
{
  const voxel_model_t *vox = (const voxel_model_t *)vis->voxel;
  const uint16_t      *lut;
  uint16_t            *fb = drawvars.short_topleft;
  int      npix  = SCREENWIDTH * viewheight;
  fixed_t  scale = FRACUNIT;             /* world units per voxel (1.0)     */
  int x, y;

  if (!vox || !fb || !vox->columns || !vox->slabs)
    return;
  if (!vox_zbuf_ensure(npix))
    return;

  lut = R_ComposedColormap(vis->colormap ? vis->colormap : fullcolormap);
  vox_zbuf_stamp++;

  /* model-yaw rotation of the horizontal cells into world space */
  {
  fixed_t rcos = finecosine[vis->voxangle >> ANGLETOFINESHIFT];
  fixed_t rsin = finesine[vis->voxangle >> ANGLETOFINESHIFT];

  for (x = 0; x < vox->xsiz; x++)
  {
    for (y = 0; y < vox->ysiz; y++)
    {
      const voxcolumn_t *c = &vox->columns[x * vox->ysiz + y];
      fixed_t lx = FixedMul(((x << FRACBITS) - vox->xpiv), scale);
      fixed_t ly = FixedMul(((y << FRACBITS) - vox->ypiv), scale);
      /* rotate local (lx,ly) by the actor yaw, then offset by the origin */
      fixed_t ox = FixedMul(lx, rcos) - FixedMul(ly, rsin);
      fixed_t oy = FixedMul(lx, rsin) + FixedMul(ly, rcos);
      fixed_t wx = vis->gx + ox;
      fixed_t wy = vis->gy + oy;
      fixed_t tr_x = wx - viewx;
      fixed_t tr_y = wy - viewy;
      fixed_t tz = FixedMul(tr_x, viewcos) - FixedMul(tr_y, -viewsin);
      fixed_t tx, xscale, sxf, vsize;
      int sxl, sxr, sxc;

      if (tz < (FRACUNIT * 4))
        continue;
      tx = -(FixedMul(tr_y, viewcos) + FixedMul(tr_x, -viewsin));
      xscale = FixedDiv(projectionx, tz);
      sxf = centerxfrac + FixedMul(tx, xscale);
      sxc = sxf >> FRACBITS;
      /* one voxel's projected width in pixels: fill from this column's
       * centre to where the next column projects, so adjacent columns tile
       * with no horizontal gaps (a point/!tiling splat shows the model as a
       * dot cloud).  vsize is a full voxel width here. */
      vsize = FixedMul(scale, xscale);
      sxl = (sxf - (vsize >> 1)) >> FRACBITS;
      sxr = (sxf + (vsize >> 1) + (FRACUNIT >> 1)) >> FRACBITS;
      if (sxl < 0) sxl = 0;
      if (sxr >= SCREENWIDTH) sxr = SCREENWIDTH - 1;
      if (sxr < sxl) sxr = sxl;
      if (sxc < 0 || sxc >= SCREENWIDTH) { if (sxr < sxl) continue; }

      if (c->count <= 0)
        continue;

      /* Draw each slab as a stack of per-voxel coloured bands.  A voxel at
       * model z occupies world z [zsiz-(z+1) .. zsiz-z]; project both edges
       * to screen y and fill that band with the voxel's own colour, so the
       * model shows its real surface detail instead of one silhouette
       * colour.  The per-pixel depth buffer keeps nearer columns in front. */
      {
        int si, yy, xx;
        for (si = 0; si < c->count; si++)
        {
          const voxslab_t *sl = &vox->slabs[c->first + si];
          int k;
          for (k = 0; k < sl->zleng; k++)
          {
            int zc = sl->ztop + k;
            fixed_t wz_hi = vis->gz +
              FixedMul(((vox->zsiz - zc) << FRACBITS), scale);
            fixed_t wz_lo = vis->gz +
              FixedMul(((vox->zsiz - (zc + 1)) << FRACBITS), scale);
            int syt = (centeryfrac - FixedMul(wz_hi - viewz, xscale)) >> FRACBITS;
            int syb = (centeryfrac - FixedMul(wz_lo - viewz, xscale)) >> FRACBITS;
            uint16_t px = lut[vox->pal_remap[sl->col[k]]];

            if (syt > syb) { int t = syt; syt = syb; syb = t; }
            if (syb == syt) syb++;          /* at least one row per voxel */
            if (syt < 0) syt = 0;
            if (syb > viewheight) syb = viewheight;

            for (xx = sxl; xx <= sxr; xx++)
              for (yy = syt; yy < syb; yy++)
              {
                int off = yy * SCREENWIDTH + xx;
                if (vox_zbuf_seen[off] == vox_zbuf_stamp &&
                    vox_zbuf[off] <= tz)
                  continue;
                vox_zbuf_seen[off] = vox_zbuf_stamp;
                vox_zbuf[off]      = tz;
                fb[yy * SURFACE_SHORT_PITCH + xx] = px;
              }
          }
        }
      }
    }
  }
  }
}
