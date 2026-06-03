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
 *   Hexen MAPINFO lump parser and per-map information store.
 *
 *-----------------------------------------------------------------------------*/

#ifndef __P_MAPINFO_H__
#define __P_MAPINFO_H__

#include "doomtype.h"
#include "m_fixed.h"

/* Hexen sector specials that drive the lightning storm. */
#define LIGHTNING_SPECIAL  198
#define LIGHTNING_SPECIAL2 199

#define HEXEN_MAPINFO_SONG_LEN 10
#define HEXEN_MAPINFO_NAME_LEN 32

typedef struct hexen_mapinfo_s
{
  short   cluster;
  short   warpTrans;
  short   nextMap;
  short   sky1Texture;
  short   sky2Texture;
  fixed_t sky1ScrollDelta;
  fixed_t sky2ScrollDelta;
  dbool   doubleSky;
  dbool   lightning;
  int     fadetable;
  char    name[HEXEN_MAPINFO_NAME_LEN];
  char    songLump[HEXEN_MAPINFO_SONG_LEN];
} hexen_mapinfo_t;

/* Parse the MAPINFO lump (Hexen only).  Safe to call once at startup. */
void P_LoadMapInfo(void);

/* Select the MapInfo record for the given map number (1-based). */
void P_SetCurrentMapInfo(int map);

/* Accessors for the currently selected map. */
const hexen_mapinfo_t *P_CurrentMapInfo(void);
int   P_MapInfoCount(void);

/* Translate a warp number (the value the player warps to) into a real map
 * number, and vice-versa. */
int P_TranslateMapWarp(int warp);
int P_GetMapWarpTrans(int map);

/* Convenience accessors used by the engine. */
short   P_GetMapSky1Texture(int map);
short   P_GetMapSky2Texture(int map);
fixed_t P_GetMapSky1ScrollDelta(int map);
fixed_t P_GetMapSky2ScrollDelta(int map);
dbool   P_GetMapDoubleSky(int map);
dbool   P_GetMapLightning(int map);
int     P_GetMapCluster(int map);
int     P_GetMapNextMap(int map);
int     P_GetMapFadeTable(int map);
const char *P_GetMapName(int map);
const char *P_GetMapSongLump(int map);

#endif
