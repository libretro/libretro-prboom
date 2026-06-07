/* p_slope.c: sloped sector planes (ZDoom Plane_Align, special 181).
 *
 * Plane_Align tilts one side's floor and/or ceiling so it meets the
 * other side's plane along the line.  The plane passes through the
 * line at the reference sector's height and through the sloping
 * sector's farthest vertex at that sector's own flat height -- the
 * same construction ZDoom uses, so maps built for it line up.
 *
 * Plane setup runs once at load in doubles; runtime queries are
 * fixed point with 64-bit intermediates.
 */

#include <math.h>

#include "doomstat.h"
#include "p_spec.h"
#include "r_state.h"
#include "z_zone.h"
#include "lprintf.h"
#include "p_slope.h"

fixed_t P_PlaneZatPoint(const secplane_t *p, fixed_t x, fixed_t y)
{
  /* z = (-d - a*x - b*y) / c, all 16.16 */
  int64_t num = -(int64_t)p->d - (((int64_t)p->a * x) >> FRACBITS)
                              - (((int64_t)p->b * y) >> FRACBITS);
  return (fixed_t)((num << FRACBITS) / p->c);
}

fixed_t P_FloorZAtPoint(const sector_t *s, fixed_t x, fixed_t y)
{
  return s->floor_slope ? P_PlaneZatPoint(s->floor_slope, x, y)
                        : s->floorheight;
}

fixed_t P_CeilingZAtPoint(const sector_t *s, fixed_t x, fixed_t y)
{
  return s->ceiling_slope ? P_PlaneZatPoint(s->ceiling_slope, x, y)
                          : s->ceilingheight;
}

/* squared perpendicular distance proxy from point to the line through
 * v1/v2 (doubles; setup only) */
static double slope_line_dist(const line_t *l, fixed_t px, fixed_t py)
{
  double x1 = l->v1->x / 65536.0, y1 = l->v1->y / 65536.0;
  double dx = l->dx / 65536.0,    dy = l->dy / 65536.0;
  double x  = px / 65536.0,       y  = py / 65536.0;
  return fabs((y - y1) * dx - (x - x1) * dy) / sqrt(dx * dx + dy * dy);
}

/* Build the plane for one side of a Plane_Align line.  which: 0 =
 * floor, 1 = ceiling; sec slopes toward refsec's height at the line. */
static void P_AlignPlane(sector_t *sec, const sector_t *refsec,
                         const line_t *line, int which)
{
  double refz1, refz2, secz, dist, fardist;
  double p1x, p1y, p2x, p2y, fx, fy;
  double ux, uy, uz, vx, vy, vz, nx, ny, nz, len, d;
  fixed_t fdx, fdy;
  secplane_t *pl;
  int i;

  /* the reference height is the neighbour's CURRENT plane at each line
   * endpoint -- a neighbour already sloped by an earlier Plane_Align
   * line contributes its tilted heights, which is how ZDoom makes
   * chains of aligned steps meet seamlessly */
  if (which)
  {
    refz1 = P_CeilingZAtPoint(refsec, line->v1->x, line->v1->y) / 65536.0;
    refz2 = P_CeilingZAtPoint(refsec, line->v2->x, line->v2->y) / 65536.0;
    secz  = sec->ceilingheight / 65536.0;
  }
  else
  {
    refz1 = P_FloorZAtPoint(refsec, line->v1->x, line->v1->y) / 65536.0;
    refz2 = P_FloorZAtPoint(refsec, line->v2->x, line->v2->y) / 65536.0;
    secz  = sec->floorheight / 65536.0;
  }

  /* farthest vertex of the sloping sector from the alignment line */
  fardist = 0;
  fx = fy = 0;
  for (i = 0; i < sec->linecount; i++)
  {
    const line_t *l = sec->lines[i];
    int e;
    for (e = 0; e < 2; e++)
    {
      fdx = e ? l->v2->x : l->v1->x;
      fdy = e ? l->v2->y : l->v1->y;
      dist = slope_line_dist(line, fdx, fdy);
      if (dist > fardist)
      {
        fardist = dist;
        fx = fdx / 65536.0;
        fy = fdy / 65536.0;
      }
    }
  }
  if (fardist < 1.0 || (refz1 == secz && refz2 == secz))
    return;                     /* degenerate or already level */

  /* plane through (v1,refz1), (v2,refz2), (far,secz) */
  p1x = line->v1->x / 65536.0; p1y = line->v1->y / 65536.0;
  p2x = line->v2->x / 65536.0; p2y = line->v2->y / 65536.0;
  ux = p2x - p1x; uy = p2y - p1y; uz = refz2 - refz1;
  vx = fx - p1x;  vy = fy - p1y;  vz = secz - refz1;
  nx = uy * vz - uz * vy;
  ny = uz * vx - ux * vz;
  nz = ux * vy - uy * vx;
  len = sqrt(nx * nx + ny * ny + nz * nz);
  if (len < 1e-6)
    return;
  nx /= len; ny /= len; nz /= len;
  if (nz < 0)                   /* keep c positive */
  {
    nx = -nx; ny = -ny; nz = -nz;
  }
  d = -(nx * p1x + ny * p1y + nz * refz1);

  pl = Z_Malloc(sizeof(*pl), PU_LEVEL, NULL);
  pl->a = (fixed_t)(nx * FRACUNIT);
  pl->b = (fixed_t)(ny * FRACUNIT);
  pl->c = (fixed_t)(nz * FRACUNIT);
  pl->d = (fixed_t)(d * FRACUNIT);
  if (which)
    sec->ceiling_slope = pl;
  else
    sec->floor_slope = pl;
}

void P_SpawnZDoomSlopes(void)
{
  int i, n = 0;

  for (i = 0; i < numlines; i++)
  {
    line_t *l = &lines[i];
    int fa, ca;

    if (l->special != 181)
      continue;
    l->special = 0;
    if (!l->frontsector || !l->backsector)
      continue;

    fa = l->args[0] & 3;        /* floor:   1 front, 2 back */
    ca = l->args[1] & 3;        /* ceiling: 1 front, 2 back */
    if (fa == 1)
      P_AlignPlane(l->frontsector, l->backsector, l, 0);
    else if (fa == 2)
      P_AlignPlane(l->backsector, l->frontsector, l, 0);
    if (ca == 1)
      P_AlignPlane(l->frontsector, l->backsector, l, 1);
    else if (ca == 2)
      P_AlignPlane(l->backsector, l->frontsector, l, 1);
    if (fa || ca)
      n++;
  }
  if (n)
    lprintf(LO_INFO, "P_SpawnZDoomSlopes: %d Plane_Align lines\n", n);
}
