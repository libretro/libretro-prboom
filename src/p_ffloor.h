/* p_ffloor.h: ZDoom 3D floors (Sector_Set3DFloor, special 160).
 *
 * A 3D floor is a horizontal slab inside a target sector whose
 * geometry lives in a control sector: the control sector's ceiling is
 * the slab's top, its floor the slab's bottom.  Heights are read live
 * through the control sector, so movers animate slabs for free.
 *
 * This commit covers the data model, level-load attachment, and
 * movement physics (solid slabs clip things, becoming floor or
 * ceiling depending on which side of the slab a thing's midpoint
 * is).  Rendering and hitscan/sight interaction are separate work.
 * MyHouse's census: 1064 slab lines, 92% opaque solid. */

#ifndef __P_FFLOOR__
#define __P_FFLOOR__

#include "doomtype.h"
#include "m_fixed.h"
#include "r_defs.h"

/* low two bits of Sector_Set3DFloor arg1 */
#define FFLOOR_SOLID      1
#define FFLOOR_SWIMMABLE  2
#define FFLOOR_RENDERONLY 3

typedef struct ffloor_s
{
  sector_t *model;       /* control sector: ceiling = top, floor = bottom */
  struct line_s *controlline; /* the special-160 line; supplies face texture */
  int       type;        /* FFLOOR_* (arg1 & 3) */
  int       alpha;       /* 0..255 (arg3); opaque rendering may clamp */
  struct ffloor_s *next;
} ffloor_t;

/* Walk all lines for special 160 and attach slabs to tagged sectors.
 * Call once per level after lines, sectors and tags are loaded; it
 * clears any previous level's attachments first.  No-op on maps
 * without ZDoom specials. */
void P_AttachFFloors(void);

/* Clip a floor/ceiling height against a sector's solid slabs for a
 * thing of `height` whose feet are at `z`.  A slab whose midpoint is
 * below the thing's midpoint raises the floor to the slab top;
 * otherwise it lowers the ceiling to the slab bottom. */
fixed_t P_FFloorAdjustFloorZ(const sector_t *s, fixed_t x, fixed_t y,
                             fixed_t z, fixed_t height, fixed_t floorz);
fixed_t P_FFloorAdjustCeilingZ(const sector_t *s, fixed_t x, fixed_t y,
                               fixed_t z, fixed_t height, fixed_t ceilingz);

#endif
