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

    if (ln->special != 156 || ln->args[2] != 0)
      continue;                          /* not a visual line portal */

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
    lineportals[i].ax = ln->v1->x;
    lineportals[i].ay = ln->v1->y;
    lineportals[i].bx = tg->v2->x;
    lineportals[i].by = tg->v2->y;
    pairs++;
  }

  line_portals_active = pairs > 0;
  if (pairs)
    lprintf(LO_INFO, "P_SpawnLinePortals: %d line portal(s)\n", pairs);
}
