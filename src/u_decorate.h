/* u_decorate.h: DECORATE doomednum aliasing (see u_decorate.c). */

#ifndef U_DECORATE_H
#define U_DECORATE_H

#include "doomtype.h"

/* If `doomednum` belongs to a DECORATE actor whose inheritance/replaces
 * chain roots in a class with a known editor number, return that number;
 * otherwise -1. */
int U_DecorateAliasDoomedNum(int doomednum);

/* ZDoom editor-only map things (particle fountains, interpolation points,
 * camera/view stacks, editor cameras): skip without a warning. */
dbool U_IsInertZDoomThing(int doomednum);

#endif
