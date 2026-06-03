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
 *   Hexen outdoor lightning storm.
 *
 *-----------------------------------------------------------------------------*/

#ifndef __P_LIGHTNING_H__
#define __P_LIGHTNING_H__

#include "doomtype.h"

extern dbool LevelHasLightning;
extern int   NextLightningFlash;
extern int   LightningFlash;

/* Set up the storm for the current map (no-op unless MAPINFO flags it). */
void P_InitLightning(void);

/* Advance the storm one tic. */
void P_UpdateLightning(void);

/* Trigger an immediate flash (used by the ACS thunder command). */
void P_ForceLightning(void);

#endif
