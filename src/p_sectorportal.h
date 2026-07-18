/* p_sectorportal.h: stacked-sector "look only" portals (rooms over rooms).
 *
 * Legacy ZDoom stacked sectors are set up with a matching tid pair:
 *   - UpperStackLookOnly (9077) sits in the UPPER room; that room's floor
 *     becomes a window looking DOWN into the lower room.
 *   - LowerStackLookOnly (9078) sits in the LOWER room; that room's
 *     ceiling becomes a window looking UP into the upper room.
 * (Verified against real content: in every zdcmp2 pairing the 9077's room
 * floor sits above the 9078's.)  The two are linked by a shared tid.
 * Looking through one plane shows the other room, translated by the offset
 * between the two anchor things; the portal is view-only (no movement
 * through it).
 *
 * Recorded at thing-load time and resolved once afterwards (the partner may
 * load later), filling per-sector floor/ceiling portal descriptors.
 */

#ifndef P_SECTORPORTAL_H
#define P_SECTORPORTAL_H

#include "m_fixed.h"
#include "tables.h"   /* angle_t */

/* a resolved sector portal: viewing this sector's plane shows another
 * region of the level, offset by (dx,dy,dz) from the viewer. */
typedef struct {
  int     active;
  int     hfixed;       /* with horizon: 1 = Sector_SetPortal type 3, the
                         * plane sits at the source height measured from the
                         * CAMERA and its texture is anchored to the camera,
                         * so the surface never shifts as the viewer moves */
  int     horizon;      /* 1: Sector_SetPortal type 4 (or type 3 with
                         * hfixed) -- the window shows
                         * sector `hsec`'s planes extended to infinity, and
                         * there is no camera at all */
  int     hsec;
  int     absolute;     /* 1: dx/dy/dz are an absolute camera position and
                         * `angle` a yaw delta -- Sector_SetPortal type 2
                         * (skybox portals).  0: they are a displacement. */
  angle_t angle;        /* yaw delta, absolute cameras only */
  fixed_t dx, dy, dz;   /* added to the viewer position to get the camera */
  int     alpha;        /* the FLAT's opacity (ZDoom 9077 arg0): 0 = the
                         * flat is invisible (pure window), 255 = fully
                         * opaque (no view through; not activated) */
} secportal_t;

/* per-sector floor/ceiling portals; indexed by sector number.  Allocated by
 * P_SpawnSectorPortals, freed by P_ClearSectorPortals. */
extern secportal_t *floorportals;
extern secportal_t *ceilingportals;
/* any resolved pairings on this level (renderer gate) */
extern int sector_portals_active;

/* SkyCamCompat cameras (Sector_SetPortal type 2 names them by sector). */
void P_AddSkyCam(fixed_t x, fixed_t y, fixed_t z, angle_t angle, int secnum);

/* record an UpperStackLookOnly (9077) / LowerStackLookOnly (9078) at load */
void P_AddStackPoint(int upper, int tid, fixed_t x, fixed_t y, int alpha);

/* drop the previous level's tables */
void P_ClearSectorPortals(void);

/* pair the recorded stack points by tid and fill the per-sector portals */
void P_SpawnSectorPortals(void);

#endif
