/* p_vslope.c: thing-based vertex slopes (ZDoom things 1504/1505).
 *
 * See p_vslope.h.  The plane fit is the same three-point construction
 * p_slope.c uses for Plane_Align: build two edge vectors, take their
 * cross product as the normal, normalise, force c > 0, reject a plane
 * with no vertical component (c == 0 at 16.16, which P_PlaneZatPoint
 * would divide by), then store a*x + b*y + c*z + d = 0 in 16.16.
 *
 * Setup-only work runs in doubles; the resulting secplane_t is queried
 * at runtime in fixed point by the shared P_PlaneZatPoint.
 */

#include <math.h>

#include "doomstat.h"
#include "r_state.h"
#include "r_defs.h"
#include "z_zone.h"
#include "lprintf.h"
#include "p_slope.h"
#include "p_vslope.h"

typedef struct
{
  fixed_t x, y, z;
  int     which;                /* 0 floor, 1 ceiling */
} slopevert_t;

static slopevert_t *svlist  = NULL;
static int          svcount = 0;
static int          svalloc = 0;

void P_ClearSlopeVertices(void)
{
  /* svlist is a plain malloc grow-array (not PU_LEVEL), so it survives
   * the zone purge between levels and must be freed explicitly. */
  free(svlist);
  svlist  = NULL;
  svcount = 0;
  svalloc = 0;
}

void P_AddSlopeVertex(fixed_t x, fixed_t y, fixed_t z, int which)
{
  if (svcount == svalloc)
  {
    int newalloc = svalloc ? svalloc * 2 : 32;
    slopevert_t *grown =
      (slopevert_t *)realloc(svlist, (size_t)newalloc * sizeof(*svlist));
    if (!grown)
      return;                   /* out of memory: drop this vertex */
    svlist  = grown;
    svalloc = newalloc;
  }
  svlist[svcount].x     = x;
  svlist[svcount].y     = y;
  svlist[svcount].z     = z;
  svlist[svcount].which = which;
  svcount++;
}

/* z of the recorded slope vertex sitting exactly on (x,y) for the given
 * plane, or a miss.  Coordinates match exactly: vertex things are placed
 * on integer/parsed vertex coordinates by the editor, and both the
 * vertices and the things come through the same udmf_to_fixed/SHORT path,
 * so a coincident thing lands on the identical fixed_t. */
static dbool slopevert_z(fixed_t x, fixed_t y, int which, fixed_t *out)
{
  int i;
  for (i = 0; i < svcount; i++)
    if (svlist[i].which == which && svlist[i].x == x && svlist[i].y == y)
    {
      *out = svlist[i].z;
      return TRUE;
    }
  return FALSE;
}

/* fit a plane through three (x,y,z) points and store it on sec for the
 * given plane.  Mirrors P_AlignPlane's construction. */
static void P_FitVertexPlane(sector_t *sec, int which,
                             fixed_t x1, fixed_t y1, fixed_t z1,
                             fixed_t x2, fixed_t y2, fixed_t z2,
                             fixed_t x3, fixed_t y3, fixed_t z3)
{
  double p1x = x1 / 65536.0, p1y = y1 / 65536.0, p1z = z1 / 65536.0;
  double p2x = x2 / 65536.0, p2y = y2 / 65536.0, p2z = z2 / 65536.0;
  double p3x = x3 / 65536.0, p3y = y3 / 65536.0, p3z = z3 / 65536.0;
  double ux = p2x - p1x, uy = p2y - p1y, uz = p2z - p1z;
  double vx = p3x - p1x, vy = p3y - p1y, vz = p3z - p1z;
  double nx = uy * vz - uz * vy;
  double ny = uz * vx - ux * vz;
  double nz = ux * vy - uy * vx;
  double len = sqrt(nx * nx + ny * ny + nz * nz);
  double d;
  secplane_t *pl;

  if (len < 1e-6)
    return;                     /* collinear / degenerate triangle */
  nx /= len; ny /= len; nz /= len;
  if (nz < 0)                   /* keep c positive */
  {
    nx = -nx; ny = -ny; nz = -nz;
  }
  if ((fixed_t)(nz * FRACUNIT) <= 0)
    return;                     /* no vertical component: not a plane */
  d = -(nx * p1x + ny * p1y + nz * p1z);

  pl = (secplane_t *)Z_Malloc(sizeof(*pl), PU_LEVEL, NULL);
  pl->a = (fixed_t)(nx * FRACUNIT);
  pl->b = (fixed_t)(ny * FRACUNIT);
  pl->c = (fixed_t)(nz * FRACUNIT);
  pl->d = (fixed_t)(d  * FRACUNIT);
  if (which)
    sec->ceiling_slope = pl;
  else
    sec->floor_slope = pl;
}

/* collect the distinct vertices of a sector that carry a slope vertex
 * for the given plane, fit a plane if exactly three were found.  More
 * than three is ambiguous (ZDoom only slopes triangular sectors this
 * way), so leave those flat. */
static void P_VertexSlopeSector(sector_t *sec, int which)
{
  fixed_t vx[3], vy[3], vz[3];
  int n = 0;
  int i;

  for (i = 0; i < sec->linecount; i++)
  {
    const line_t *l = sec->lines[i];
    int e;
    for (e = 0; e < 2; e++)
    {
      fixed_t px = e ? l->v2->x : l->v1->x;
      fixed_t py = e ? l->v2->y : l->v1->y;
      fixed_t pz;
      int j;
      dbool dup = FALSE;

      if (!slopevert_z(px, py, which, &pz))
        continue;
      for (j = 0; j < n; j++)   /* skip a vertex shared by two lines */
        if (vx[j] == px && vy[j] == py)
        {
          dup = TRUE;
          break;
        }
      if (dup)
        continue;
      if (n < 3)
      {
        vx[n] = px; vy[n] = py; vz[n] = pz;
      }
      n++;
      if (n > 3)
        return;                 /* not a clean triangle: leave flat */
    }
  }

  if (n == 3)
    P_FitVertexPlane(sec, which,
                     vx[0], vy[0], vz[0],
                     vx[1], vy[1], vz[1],
                     vx[2], vy[2], vz[2]);
}

void P_SpawnVertexSlopes(void)
{
  int i, f = 0, c = 0;

  if (!svcount)
    return;

  for (i = 0; i < numsectors; i++)
  {
    if (!sectors[i].floor_slope)
    {
      P_VertexSlopeSector(&sectors[i], 0);
      if (sectors[i].floor_slope)
        f++;
    }
    if (!sectors[i].ceiling_slope)
    {
      P_VertexSlopeSector(&sectors[i], 1);
      if (sectors[i].ceiling_slope)
        c++;
    }
  }

  if (f || c)
    lprintf(LO_INFO,
            "P_SpawnVertexSlopes: %d floor, %d ceiling vertex slopes\n",
            f, c);
}
