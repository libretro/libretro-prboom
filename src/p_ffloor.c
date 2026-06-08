/* p_ffloor.c: ZDoom 3D floors.  See p_ffloor.h. */

#include "doomstat.h"
#include "doomtype.h"
#include "lprintf.h"
#include "m_fixed.h"
#include "map_format.h"
#include "p_spec.h"
#include "p_slope.h"
#include "r_defs.h"
#include "r_main.h"
#include "r_state.h"
#include "z_zone.h"
#include "p_ffloor.h"

/* exported by the hexen special code; works from sector tags */
int P_FindSectorFromTag(short tag, int start);

void P_AttachFFloors(void)
{
  int i, attached = 0;

  /* sectors are reallocated per level; make every list explicitly
   * empty rather than trusting loader zeroing */
  for (i = 0; i < numsectors; i++)
    sectors[i].ffloors = NULL;

  if (!map_format.zdoom)
    return;

  for (i = 0; i < numlines; i++)
  {
    line_t *l = &lines[i];
    int s, type, alpha;

    if (l->special != 160 || !l->frontsector)
      continue;

    type = l->args[1] & 3;
    if (type == 0)
      continue;
    alpha = l->args[3];
    if (alpha < 0)
      alpha = 0;
    if (alpha > 255)
      alpha = 255;

    for (s = P_FindSectorFromTag((short)l->args[0], -1); s >= 0;
         s = P_FindSectorFromTag((short)l->args[0], s))
    {
      ffloor_t *ff = Z_Malloc(sizeof(*ff), PU_LEVEL, 0);
      ff->model = l->frontsector;
      ff->controlline = l;
      ff->type = type;
      ff->alpha = alpha;
      ff->next = sectors[s].ffloors;
      sectors[s].ffloors = ff;
      attached++;
    }
  }
  if (attached)
    lprintf(LO_INFO, "P_AttachFFloors: %d 3D floor slabs attached\n",
            attached);
}

fixed_t P_FFloorAdjustFloorZ(const sector_t *s, fixed_t x, fixed_t y,
                             fixed_t z, fixed_t height, fixed_t floorz)
{
  const ffloor_t *ff;
  fixed_t thingmid = z + (height >> 1);

  for (ff = s->ffloors; ff; ff = ff->next)
  {
    fixed_t top, bottom, slabmid;
    if (ff->type != FFLOOR_SOLID)
      continue;
    /* slope-aware: the slab top is the control sector's ceiling and its
     * bottom is the control sector's floor, each evaluated at (x,y) so a
     * tilted 3D floor is stood on at the right height (these reduce to the
     * flat sector heights when the model is not sloped) */
    top = P_CeilingZAtPoint(ff->model, x, y);
    bottom = P_FloorZAtPoint(ff->model, x, y);
    if (bottom > top)
      continue;
    slabmid = bottom + ((top - bottom) >> 1);
    if (thingmid >= slabmid && top > floorz)
      floorz = top;
  }
  return floorz;
}

fixed_t P_FFloorAdjustCeilingZ(const sector_t *s, fixed_t x, fixed_t y,
                               fixed_t z, fixed_t height, fixed_t ceilingz)
{
  const ffloor_t *ff;
  fixed_t thingmid = z + (height >> 1);

  for (ff = s->ffloors; ff; ff = ff->next)
  {
    fixed_t top, bottom, slabmid;
    if (ff->type != FFLOOR_SOLID)
      continue;
    top = P_CeilingZAtPoint(ff->model, x, y);
    bottom = P_FloorZAtPoint(ff->model, x, y);
    if (bottom > top)
      continue;
    slabmid = bottom + ((top - bottom) >> 1);
    if (thingmid < slabmid && bottom < ceilingz)
      ceilingz = bottom;
  }
  return ceilingz;
}

