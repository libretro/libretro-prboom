/* u_decorate.h: DECORATE doomednum aliasing (see u_decorate.c). */

#ifndef U_DECORATE_H
#define U_DECORATE_H

#include "doomtype.h"

/* If `doomednum` belongs to a DECORATE actor whose inheritance/replaces
 * chain roots in a class with a known editor number, return that number;
 * otherwise -1. */
int U_DecorateAliasDoomedNum(int doomednum);

/* true if the wad's DECORATE lump redefines the named sprite's state
 * sequence (r_things only unifies art for such sprites) */
dbool U_DecorateMentionsSprite(const char *name);

/* Register static single-frame DECORATE decorations as thing types via the
 * DSDHacked tables.  Call once at startup, after dehacked processing and
 * before R_Init; Doom game only. */
void U_RegisterDecorateThings(void);

/* ZDoom editor-only map things (particle fountains, interpolation points,
 * camera/view stacks, editor cameras): skip without a warning. */
dbool U_IsInertZDoomThing(int doomednum);

/* Register spawnable ZDoom utility markers (MapSpot, TeleportDest2/3)
 * as dynamic mobj types; call once after dsda_InitTables. */
void U_RegisterZDoomUtilityThings(void);

#endif
