/* Emacs style mode select   -*- C -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 * DESCRIPTION:
 *   Hexen scripted line specials.
 *
 *-----------------------------------------------------------------------------*/

#ifndef __P_SPEC_HEXEN_H__
#define __P_SPEC_HEXEN_H__

#include "doomtype.h"
#include "r_defs.h"
#include "p_mobj.h"
#include "p_spec.h"

/* Run a Hexen line special.  Returns true if it did something (so use/switch
 * activation can flip the switch texture). */
dbool P_ExecuteHexenLineSpecial(int special, byte *args, line_t *line,
                                int side, mobj_t *mo);

/* Door handlers + thinker. */
void  T_HexenVerticalDoor(vldoor_t *door);
int   Hexen_EV_DoDoor(line_t *line, byte *args, vldoor_e type);
dbool Hexen_EV_VerticalDoor(line_t *line, mobj_t *thing);
int   Hexen_EV_DoFloor(line_t *line, byte *args, floor_e floortype);
void  T_HexenMoveCeiling(ceiling_t *ceiling);
int   Hexen_EV_DoCeiling(line_t *line, byte *args, ceiling_e type);
int   Hexen_EV_CeilingCrushStop(line_t *line, byte *args);
void  T_HexenBuildPillar(pillar_t *pillar);
int   EV_BuildPillar(line_t *line, byte *args, int crush);
int   EV_OpenPillar(line_t *line, byte *args);
void  T_HexenLight(light_t *light);
int   EV_SpawnLight(line_t *line, byte *args, lighttype_t type);
dbool P_HexenTeleport(mobj_t *thing, fixed_t x, fixed_t y, angle_t angle,
                      dbool useFog);
dbool EV_HexenTeleport(int tid, mobj_t *thing, dbool fog);
int   EV_DoHexenPlat(line_t *line, byte *args, plattype_e type, int amount);
void  Hexen_EV_StopPlat(line_t *line, byte *args);

/* Line activation entry points (wired through map_format). */
void  P_CrossHexenSpecialLine(line_t *line, int side, mobj_t *thing);
void  P_ShootHexenSpecialLine(mobj_t *thing, line_t *line);
dbool P_UseHexenSpecialLine(mobj_t *thing, line_t *line, int side);

#endif
