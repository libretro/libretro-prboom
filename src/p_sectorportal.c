/* p_sectorportal.c: stacked-sector "look only" portals.  See header. */

#include "doomstat.h"
#include "r_state.h"
#include "r_defs.h"
#include "r_main.h"
#include "lprintf.h"
#include "p_sectorportal.h"

secportal_t *floorportals   = NULL;
secportal_t *ceilingportals = NULL;
int sector_portals_active   = 0;

/* pending stack points recorded during thing-load */
typedef struct {
  int     upper;        /* 1 = UpperStackLookOnly (9077), 0 = Lower (9078) */
  int     tid;
  fixed_t x, y, z;
  int     alpha;
} stackpt_t;

static stackpt_t *points    = NULL;
static int        numpoints = 0;
static int        pointalloc = 0;

void P_ClearSectorPortals(void)
{
  sector_portals_active = 0;
  free(floorportals);   floorportals = NULL;
  free(ceilingportals); ceilingportals = NULL;
  free(points);         points = NULL; numpoints = 0; pointalloc = 0;
}

void P_AddStackPoint(int upper, int tid, fixed_t x, fixed_t y, int alpha)
{
  if (numpoints == pointalloc)
  {
    int na = pointalloc ? pointalloc * 2 : 32;
    stackpt_t *g = (stackpt_t *)realloc(points, (size_t)na * sizeof(*g));
    if (!g) return;
    points = g; pointalloc = na;
  }
  points[numpoints].upper = upper;
  points[numpoints].tid   = tid;
  points[numpoints].x     = x;
  points[numpoints].y     = y;
  points[numpoints].z     = 0;          /* resolved from sector at spawn */
  points[numpoints].alpha = alpha;
  numpoints++;
}

void P_SpawnSectorPortals(void)
{
  int i, j, pairs = 0;

  if (numsectors <= 0)
    return;

  floorportals   = (secportal_t *)calloc((size_t)numsectors, sizeof(secportal_t));
  ceilingportals = (secportal_t *)calloc((size_t)numsectors, sizeof(secportal_t));
  if (!floorportals || !ceilingportals)
    return;

  if (!numpoints)
    return;

  /* resolve each point's z from the floor of the sector it sits in */
  for (i = 0; i < numpoints; i++)
  {
    subsector_t *ss = R_PointInSubsector(points[i].x, points[i].y);
    points[i].z = ss && ss->sector ? ss->sector->floorheight : 0;
  }

  /* A shared tid links a whole stacked window that may span many sectors,
   * but the displacement between the two rooms is a single constant for the
   * tid.  Derive that constant once per tid from the lowest-positioned
   * upper/lower of that tid (sorting both the same way makes them
   * correspond), then:
   *   - every Upper-anchor sector gets a ceiling portal (looks up) with
   *     offset (lower - upper);
   *   - every Lower-anchor sector gets a floor portal (looks down) with
   *     offset (upper - lower).
   * Anchors with no partner of the opposite kind are skipped. */
  for (i = 0; i < numpoints; i++)
  {
    fixed_t ux, uy, uz, lx, ly, lz, dx, dy, dz;
    int tid = points[i].tid;
    int have_u = 0, have_l = 0;
    int alpha = 255;

    /* process each tid once (when we hit its first occurrence) */
    for (j = 0; j < i; j++)
      if (points[j].tid == tid)
        break;
    if (j < i)
      continue;                          /* tid already handled */

    /* pick the lowest-x,y upper and lower of this tid as the corresponding
     * reference pair (both lists ordered identically => same window cell) */
    ux = uy = uz = lx = ly = lz = 0;
    for (j = 0; j < numpoints; j++)
    {
      if (points[j].tid != tid)
        continue;
      if (points[j].upper)
      {
        if (!have_u || points[j].x < ux ||
            (points[j].x == ux && points[j].y < uy))
        { ux = points[j].x; uy = points[j].y; uz = points[j].z; }
        have_u = 1;
        alpha = points[j].alpha;
      }
      else
      {
        if (!have_l || points[j].x < lx ||
            (points[j].x == lx && points[j].y < ly))
        { lx = points[j].x; ly = points[j].y; lz = points[j].z; }
        have_l = 1;
      }
    }
    if (!have_u || !have_l)
      continue;                          /* unpaired tid */

    dx = lx - ux; dy = ly - uy; dz = lz - uz;

    /* assign every anchor sector of this tid */
    for (j = 0; j < numpoints; j++)
    {
      subsector_t *ss;
      sector_t *sec;
      if (points[j].tid != tid)
        continue;
      ss = R_PointInSubsector(points[j].x, points[j].y);
      if (!ss || !ss->sector)
        continue;
      sec = ss->sector;
      if (points[j].upper)
      {
        ceilingportals[sec - sectors].active = 1;
        ceilingportals[sec - sectors].dx = dx;
        ceilingportals[sec - sectors].dy = dy;
        ceilingportals[sec - sectors].dz = dz;
        ceilingportals[sec - sectors].alpha = alpha;
      }
      else
      {
        floorportals[sec - sectors].active = 1;
        floorportals[sec - sectors].dx = -dx;
        floorportals[sec - sectors].dy = -dy;
        floorportals[sec - sectors].dz = -dz;
        floorportals[sec - sectors].alpha = alpha;
      }
    }
    pairs++;
  }

  sector_portals_active = pairs > 0;
  if (pairs)
    lprintf(LO_INFO, "P_SpawnSectorPortals: %d stacked-sector pairing(s)\n",
            pairs);
}
