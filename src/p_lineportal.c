/* p_lineportal.c: visual line portals (Line_SetPortal, special 156). */

#include <stdlib.h>
#include "doomstat.h"
#include "r_state.h"
#include "r_defs.h"
#include "r_main.h"
#include "lprintf.h"
#include "map_format.h"
#include "p_lineportal.h"

lineportal_t *lineportals       = NULL;
int           line_portals_active = 0;

void P_ClearLinePortals(void)
{
  free(lineportals);
  lineportals = NULL;
  line_portals_active = 0;
}

/* Line_SetPortal (targetline, thisline, type, planeanchor):
 *   targetline names the line ID this portal exits at
 *   thisline is the line's own ID (Hexen only; UDMF carries a line id)
 *   type 0 is visual only, which is all this renders
 *
 * Special 156 is only Line_SetPortal on Hexen-format and UDMF maps; in Doom
 * format it is vanilla's "start light strobing", so the scan is gated. */
void P_SpawnLinePortals(void)
{
  int i, j, pairs = 0;

  P_ClearLinePortals();

  if (numlines <= 0 || !map_format.hexen)
    return;

  lineportals = (lineportal_t *)calloc((size_t)numlines,
                                       sizeof(lineportal_t));
  if (!lineportals)
    return;

  for (i = 0; i < numlines; i++)
  {
    const line_t *ln = &lines[i];
    const line_t *tg = NULL;

    /* Types 0-3 all show the partner's surroundings; they differ only in
     * what actors may do with the opening.  Type 1 is "visual plus simple
     * teleporter", type 2 adds interaction on top of that, and type 3 is
     * the linked/static portal -- so all four render the same window here,
     * and only the movement half goes unimplemented.  Type 4 is reserved
     * for XLAT with different parameter use, so it is left alone. */
    if (ln->special != 156 || (unsigned)ln->args[2] > 3u)
      continue;

    /* The exit line, by line id.  Where that id lives depends on the map
     * format: UDMF gives every linedef an id, which the loader keeps in
     * tag, while Hexen-format linedefs have no tag field at all -- there a
     * line announces its own id either in this special's `thisline`
     * argument or with Line_SetIdentification (121), whose arg0 is the id.
     * Matching only on tag would pair nothing on a Hexen map.
     *
     * The scan is direct because the tag hash is not built this early (see
     * p_sectorportal.c for the same trap). */
    for (j = 0; j < numlines; j++)
    {
      const line_t *cand = &lines[j];
      if (j == i)
        continue;
      if (cand->tag == ln->args[0] ||
          (cand->special == 156 && cand->args[1] == ln->args[0]) ||
          (cand->special == 121 && cand->args[0] == ln->args[0]))
      {
        tg = cand;
        break;
      }
    }
    if (!tg)
      continue;

    /* The partner is walked from v2 to v1 so the two openings face each
     * other: a viewer approaching this line from the front emerges on the
     * front side of the partner. */
    lineportals[i].active = 1;
    lineportals[i].target = (int)(tg - lines);
    lineportals[i].angle  =
      R_PointToAngle2(tg->v2->x, tg->v2->y, tg->v1->x, tg->v1->y) -
      R_PointToAngle2(ln->v1->x, ln->v1->y, ln->v2->x, ln->v2->y);
    /* planeanchor: "determines how the planes at the other side of the
     * portal are relative to this line".  0 leaves the partner's heights
     * alone (its sectors are expected to match), 1 aligns floors and 2
     * aligns ceilings, which is a shift of the camera's height by the
     * difference between the two sides' planes. */
    lineportals[i].dz = 0;
    if (ln->frontsector && tg->frontsector)
    {
      if (ln->args[3] == 1)
        lineportals[i].dz = tg->frontsector->floorheight -
                            ln->frontsector->floorheight;
      else if (ln->args[3] == 2)
        lineportals[i].dz = tg->frontsector->ceilingheight -
                            ln->frontsector->ceilingheight;
    }
    lineportals[i].ax = ln->v1->x;
    lineportals[i].ay = ln->v1->y;
    lineportals[i].bx = tg->v2->x;
    lineportals[i].by = tg->v2->y;
    pairs++;
  }

  /* Line_Horizon (special 9): the line's own front sector's floor and
   * ceiling are drawn through it to infinity -- the wall becomes an open
   * vista rather than a window onto a partner.  It reuses the portal
   * window machinery: same wall claim, different fill.  Special 9 is
   * Line_Horizon only in the ZDoom line-special namespace; in Doom format
   * it is vanilla's S1 donut, so this shares the format gate above. */
  for (i = 0; i < numlines; i++)
  {
    const line_t *ln = &lines[i];
    if (ln->special != 9 || !ln->frontsector)
      continue;
    lineportals[i].active  = 1;
    lineportals[i].horizon = 1;
    lineportals[i].hsec    = (int)(ln->frontsector - sectors);
    pairs++;
  }

  line_portals_active = pairs > 0;
  if (pairs)
    lprintf(LO_INFO, "P_SpawnLinePortals: %d line portal(s)\n", pairs);
}
