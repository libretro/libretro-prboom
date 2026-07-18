/* p_lineportal.h: visual line portals (Line_SetPortal, special 156).
 *
 * A portal line shows the view from its partner line instead of its own
 * wall, as though the two lines were the same opening seen from two places.
 * The pair is resolved at level load into a per-line transform: the viewer's
 * offset from this line's anchor, rotated by the angle between the lines and
 * planted at the partner's anchor.
 *
 * Line_SetPortal types 0-3 all render this window; they differ only in what
 * actors may do with it, and the movement half is not implemented.
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
  fixed_t dz;           /* height shift from the planeanchor argument: 0 for
                         * no alignment, else what lifts the partner's floor
                         * or ceiling to meet this side's */
  int     horizon;      /* 1: Line_Horizon -- the wall shows its own front
                         * sector's planes run to infinity, and there is no
                         * partner line or camera at all */
  int     hsec;         /* source sector for the horizon case */
} lineportal_t;

/* indexed by line number; NULL until a level with portals loads */
extern lineportal_t *lineportals;
extern int           line_portals_active;

void P_SpawnLinePortals(void);
void P_ClearLinePortals(void);

#endif
