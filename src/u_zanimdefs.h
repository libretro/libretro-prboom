/* u_zanimdefs.h: ZDoom ANIMDEFS texture and flat animations
 * (see u_zanimdefs.c). */

#ifndef U_ZANIMDEFS_H
#define U_ZANIMDEFS_H

#include "doomtype.h"

extern dbool U_ZAnimPresent;

void U_LoadAnimDefs(void);
void U_UpdateZAnims(void);
void U_FreeZAnims(void);

#endif
