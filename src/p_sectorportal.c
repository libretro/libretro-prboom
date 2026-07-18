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


/* ---------------------------------------------------------------------------
 * Sector_SetPortal (line special 57), type 0: normal view portal
 *
 * The non-deprecated way to author the same look-only windows.  Two lines
 * carry the special with matching (tag, type, plane) and differing misc:
 * misc=0 marks the line in the sector where the portal is SEEN, misc=1 the
 * line in the sector that is VIEWED.  Every sector tagged with `tag` shows
 * the view sector's contents on the selected plane, translated by the offset
 * between the two lines' first vertices -- the same displacement the stack
 * things express with their anchor positions.
 *
 * Only the view-portal type is handled; copied (1), skybox (2), plane (3),
 * horizon (4) and line (5..) portals are left inert as before, and only the
 * static, map-load form is resolved (no ACS activation).
 * ------------------------------------------------------------------------ */

static void P_SpawnLinePortals(int *pairs)
{
  int i;
  for (i = 0; i < numlines; i++)
  {
    const line_t *seen = &lines[i];
    const line_t *view = NULL;
    fixed_t dx, dy, dz;
    int plane, alpha, tag, j, s;

    if (seen->special != 57 || seen->args[1] != 0 || seen->args[3] != 0)
      continue;                          /* not a "seen" view-portal line */

    /* its partner: same tag/type/plane, misc = 1 */
    for (j = 0; j < numlines; j++)
    {
      const line_t *ln = &lines[j];
      if (j == i || ln->special != 57)
        continue;
      if (ln->args[0] == seen->args[0] && ln->args[1] == 0 &&
          ln->args[2] == seen->args[2] && ln->args[3] == 1)
      {
        view = ln;
        break;
      }
    }
    if (!view || !view->frontsector)
      continue;

    tag   = seen->args[0];
    plane = seen->args[2];
    alpha = seen->args[4];
    if (alpha >= 255)
      continue;                          /* fully opaque flat: no window */

    /* displacement between the two lines' reference vertices, plus the
     * height step between the two sectors' matching planes */
    dx = view->v1->x - seen->v1->x;
    dy = view->v1->y - seen->v1->y;

    /* Sector search by the special's own tag argument (Hexen lines keep the
     * editor id separate from args[0]), scanned directly: the tag hash
     * lists are not built until P_SpawnSpecials, which runs after this. */
    for (s = 0; s < numsectors; s++)
    {
      sector_t *sec = &sectors[s];
      if (tag ? sec->tag != tag : sec != seen->frontsector)
        continue;
      if (plane == 0 || plane == 2)      /* floor window: looking down, the
                                          * seen floor meets the viewed
                                          * sector's ceiling */
      {
        dz = view->frontsector->ceilingheight - sec->floorheight;
        floorportals[s].active = 1;
        floorportals[s].dx = dx;
        floorportals[s].dy = dy;
        floorportals[s].dz = dz;
        floorportals[s].alpha = alpha;
        (*pairs)++;
      }
      if (plane == 1 || plane == 2)      /* ceiling window: looking up, the
                                          * seen ceiling meets the viewed
                                          * sector's floor */
      {
        dz = view->frontsector->floorheight - sec->ceilingheight;
        ceilingportals[s].active = 1;
        ceilingportals[s].dx = dx;
        ceilingportals[s].dy = dy;
        ceilingportals[s].dz = dz;
        ceilingportals[s].alpha = alpha;
        (*pairs)++;
      }
    }
  }
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

  /* line-special portals need no stack things, so they resolve regardless */
  if (!numpoints)
  {
    P_SpawnLinePortals(&pairs);
    sector_portals_active = pairs > 0;
    if (pairs)
      lprintf(LO_INFO, "P_SpawnSectorPortals: %d sector portal window(s)\n",
              pairs);
    return;
  }

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
        /* 9077: the physically UPPER room; its floor is the window,
         * looking down into the partner room (offset toward the 9078) */
        if (points[j].alpha >= 255)
          continue;                      /* fully opaque flat: no window */
        floorportals[sec - sectors].active = 1;
        floorportals[sec - sectors].dx = dx;
        floorportals[sec - sectors].dy = dy;
        floorportals[sec - sectors].dz = dz;
        floorportals[sec - sectors].alpha = points[j].alpha;
      }
      else
      {
        /* 9078: the LOWER room; its ceiling looks up into the 9077's room */
        if (points[j].alpha >= 255)
          continue;                      /* fully opaque flat: no window */
        ceilingportals[sec - sectors].active = 1;
        ceilingportals[sec - sectors].dx = -dx;
        ceilingportals[sec - sectors].dy = -dy;
        ceilingportals[sec - sectors].dz = -dz;
        ceilingportals[sec - sectors].alpha = points[j].alpha;
      }
    }
    pairs++;
  }

  P_SpawnLinePortals(&pairs);

  sector_portals_active = pairs > 0;
  if (pairs)
    lprintf(LO_INFO, "P_SpawnSectorPortals: %d sector portal window(s)\n",
            pairs);
}
