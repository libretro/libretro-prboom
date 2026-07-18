/* p_lineportal.h: visual line portals (Line_SetPortal, special 156).
 *
 * A portal line shows the view from its partner line instead of its own
 * wall, as though the two lines were the same opening seen from two places.
 * The pair is resolved at level load into a per-line transform: the viewer's
 * offset from this line's anchor, rotated by the angle between the lines and
 * planted at the partner's anchor.
 *
 * Only the visual type is rendered -- actors do not move through these.
 */
#ifndef P_LINEPORTAL_H
#define P_LINEPORTAL_H

#include "m_fixed.h"
#include "tables.h"

typedef struct
{
  int     active;
  int     target;       /* line index of the partner ("exit") line */
  angle_t angle;        /* yaw delta from this line to the partner */
  fixed_t ax, ay;       /* this line's anchor vertex */
  fixed_t bx, by;       /* the partner's anchor vertex */
} lineportal_t;

/* indexed by line number; NULL until a level with portals loads */
extern lineportal_t *lineportals;
extern int           line_portals_active;

void P_SpawnLinePortals(void);
void P_ClearLinePortals(void);

#endif
