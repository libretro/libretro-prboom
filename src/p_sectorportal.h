/* p_sectorportal.h: stacked-sector "look only" portals (rooms over rooms).
 *
 * Legacy ZDoom stacked sectors are set up with a matching tid pair:
 *   - UpperStackLookOnly (9077) sits in the LOWER room; that room's ceiling
 *     becomes a window looking UP into the upper room.
 *   - LowerStackLookOnly (9078) sits in the UPPER room; that room's floor
 *     becomes a window looking DOWN into the lower room.
 * The two are linked by a shared tid.  Looking through one plane shows the
 * other room, translated by the offset between the two anchor things; the
 * portal is view-only (no movement through it).
 *
 * Recorded at thing-load time and resolved once afterwards (the partner may
 * load later), filling per-sector floor/ceiling portal descriptors.
 */

#ifndef P_SECTORPORTAL_H
#define P_SECTORPORTAL_H

#include "m_fixed.h"

/* a resolved sector portal: viewing this sector's plane shows another
 * region of the level, offset by (dx,dy,dz) from the viewer. */
typedef struct {
  int     active;
  fixed_t dx, dy, dz;   /* added to the viewer position to get the camera */
  int     alpha;        /* 0..255; 255 = opaque */
} secportal_t;

/* per-sector floor/ceiling portals; indexed by sector number.  Allocated by
 * P_SpawnSectorPortals, freed by P_ClearSectorPortals. */
extern secportal_t *floorportals;
extern secportal_t *ceilingportals;
/* any resolved pairings on this level (renderer gate) */
extern int sector_portals_active;

/* record an UpperStackLookOnly (9077) / LowerStackLookOnly (9078) at load */
void P_AddStackPoint(int upper, int tid, fixed_t x, fixed_t y, int alpha);

/* drop the previous level's tables */
void P_ClearSectorPortals(void);

/* pair the recorded stack points by tid and fill the per-sector portals */
void P_SpawnSectorPortals(void);

#endif
