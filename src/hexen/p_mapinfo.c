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
 *   Hexen describes each level with a MAPINFO lump: a space-delimited text
 *   script with one "map <number> <name>" block per level, followed by any
 *   number of optional keywords (sky textures and scroll speeds, the double
 *   sky flag, the lightning-storm flag, cluster/warp/next links, the fade
 *   table, and CD-audio track numbers).  This module parses the whole lump
 *   and exposes per-map accessors the rest of the engine reads.
 *
 *-----------------------------------------------------------------------------*/

#include <string.h>

#include "doomstat.h"
#include "doomdef.h"
#include "w_wad.h"
#include "r_data.h"
#include "lprintf.h"
#include "u_scanner.h"
#include "hexen/p_mapinfo.h"

#define MAPINFO_SCRIPT_NAME "MAPINFO"

#define DEFAULT_SKY_NAME    "SKY1"
#define DEFAULT_FADE_TABLE  "COLORMAP"
#define UNKNOWN_MAP_NAME    "DEVELOPMENT MAP"

#define MCMD_SKY1          1
#define MCMD_SKY2          2
#define MCMD_DOUBLESKY     3
#define MCMD_LIGHTNING     4
#define MCMD_FADETABLE     5
#define MCMD_CLUSTER       6
#define MCMD_WARPTRANS     7
#define MCMD_NEXT          8
#define MCMD_CDTRACK       9
#define MCMD_CD_STARTTRACK 10
#define MCMD_CD_END1TRACK  11
#define MCMD_CD_END2TRACK  12
#define MCMD_CD_END3TRACK  13
#define MCMD_CD_INTERTRACK 14
#define MCMD_CD_TITLETRACK 15

#define MAPINFO_MAX_MAPS 99

static const char *MapCmdNames[] =
{
  "SKY1",
  "SKY2",
  "DOUBLESKY",
  "LIGHTNING",
  "FADETABLE",
  "CLUSTER",
  "WARPTRANS",
  "NEXT",
  "CDTRACK",
  "CD_START_TRACK",
  "CD_END1_TRACK",
  "CD_END2_TRACK",
  "CD_END3_TRACK",
  "CD_INTERMISSION_TRACK",
  "CD_TITLE_TRACK",
  NULL
};

static const int MapCmdIDs[] =
{
  MCMD_SKY1,
  MCMD_SKY2,
  MCMD_DOUBLESKY,
  MCMD_LIGHTNING,
  MCMD_FADETABLE,
  MCMD_CLUSTER,
  MCMD_WARPTRANS,
  MCMD_NEXT,
  MCMD_CDTRACK,
  MCMD_CD_STARTTRACK,
  MCMD_CD_END1TRACK,
  MCMD_CD_END2TRACK,
  MCMD_CD_END3TRACK,
  MCMD_CD_INTERTRACK,
  MCMD_CD_TITLETRACK
};

#define NUM_MAP_CMDS ((int)(sizeof(MapCmdIDs) / sizeof(MapCmdIDs[0])))

static hexen_mapinfo_t MapInfo[MAPINFO_MAX_MAPS + 1];
static int             MapCount;
static dbool           MapInfoLoaded;

/* Clamp a map number into the valid range; out-of-range falls back to the
 * defaults stored in slot 0. */
static int QualifyMap(int map)
{
  return (map < 1 || map > MapCount) ? 0 : map;
}

/* Look up a MAPINFO keyword, returning its MCMD_* id or -1. */
static int FindMapCmd(const char *name)
{
  int i;
  for (i = 0; i < NUM_MAP_CMDS; i++)
    if (!strcasecmp(name, MapCmdNames[i]))
      return MapCmdIDs[i];
  return -1;
}

static int SafeTextureNumForName(const char *name)
{
  int tex = R_CheckTextureNumForName(name);
  if (tex == -1)
    tex = 0;
  return tex;
}

void P_LoadMapInfo(void)
{
  int              lump;
  int              length;
  const char      *data;
  u_scanner_t      s;
  hexen_mapinfo_t *info;
  const char      *default_sky_name = DEFAULT_SKY_NAME;
  int              mapMax = 1;

  if (!hexen || MapInfoLoaded)
    return;
  MapInfoLoaded = true;

  memset(MapInfo, 0, sizeof(MapInfo));
  MapCount = 1;

  if (gamemode == shareware)
    default_sky_name = "SKY2";

  /* Slot 0 holds the defaults that every map block starts from. */
  info = &MapInfo[0];
  info->cluster         = 0;
  info->warpTrans       = 0;
  info->nextMap         = 1;          /* fall through to map 1 if unset */
  info->sky1Texture     = SafeTextureNumForName(default_sky_name);
  info->sky2Texture     = info->sky1Texture;
  info->sky1ScrollDelta = 0;
  info->sky2ScrollDelta = 0;
  info->doubleSky       = false;
  info->lightning       = false;
  info->fadetable       = W_CheckNumForName(DEFAULT_FADE_TABLE);
  strncpy(info->name, UNKNOWN_MAP_NAME, sizeof(info->name) - 1);

  lump = W_CheckNumForName(MAPINFO_SCRIPT_NAME);
  if (lump < 0)
    return;

  length = W_LumpLength(lump);
  data   = (const char *) W_CacheLumpNum(lump);

  /* The u_scanner understands C-style comments but not Hexen MAPINFO's
   * ';' line comments, so strip those (outside quoted strings) into a scratch
   * copy before scanning. */
  {
    char *scratch = (char *) malloc(length + 1);
    int   in, out = 0;
    dbool inQuote = false;

    for (in = 0; in < length; in++)
    {
      char c = data[in];
      if (c == '"')
        inQuote = !inQuote;
      if (c == ';' && !inQuote)
      {                         /* drop to end of line */
        while (in < length && data[in] != '\n' && data[in] != '\r')
          in++;
        if (in < length)
          scratch[out++] = data[in];   /* keep the newline */
        continue;
      }
      scratch[out++] = c;
    }
    scratch[out] = '\0';

    s = U_ScanOpen(scratch, out, MAPINFO_SCRIPT_NAME);
    free(scratch);
  }

  /* Parse with straight consuming reads.  The optional-keyword loop also
   * detects the next "map" keyword; when it does, that token has already been
   * consumed, so carry a flag to reuse it as the start of the next block
   * instead of reading again. */
  {
    dbool haveMap = U_GetNextToken(&s, true);

    while (haveMap)
    {
      int   map;
      dbool sawNextMap = false;

      if (s.token != TK_Identifier || strcasecmp(s.string, "MAP"))
      {
        U_Error(&s, "Expected 'map' at start of definition");
        break;
      }

      if (!U_MustGetInteger(&s))
        break;
      map = s.number;
      if (map < 1 || map > MAPINFO_MAX_MAPS)
      {
        U_Error(&s, "Bad map number %d", map);
        break;
      }

      info = &MapInfo[map];

      /* Start from the defaults. */
      memcpy(info, &MapInfo[0], sizeof(*info));
      info->warpTrans = map;          /* warp defaults to the map number */

      /* The map name follows the number. */
      if (!U_GetNextToken(&s, true))
        break;
      strncpy(info->name, s.string, sizeof(info->name) - 1);
      info->name[sizeof(info->name) - 1] = '\0';

      /* Optional keywords until the next MAP block or EOF. */
      while (U_GetNextToken(&s, true))
      {
        int cmd;

        if (s.token != TK_Identifier)
        {
          U_Error(&s, "Expected map keyword");
          break;
        }

        if (!strcasecmp(s.string, "MAP"))
        {
          sawNextMap = true;          /* reuse this token as the next block */
          break;
        }

        cmd = FindMapCmd(s.string);
        switch (cmd)
        {
          case MCMD_CLUSTER:
            U_MustGetInteger(&s);
            info->cluster = (short) s.number;
            break;
          case MCMD_WARPTRANS:
            U_MustGetInteger(&s);
            info->warpTrans = (short) s.number;
            break;
          case MCMD_NEXT:
            U_MustGetInteger(&s);
            info->nextMap = (short) s.number;
            break;
          case MCMD_SKY1:
            U_GetNextToken(&s, true);
            info->sky1Texture = (short) SafeTextureNumForName(s.string);
            U_MustGetInteger(&s);
            info->sky1ScrollDelta = s.number << 8;
            break;
          case MCMD_SKY2:
            U_GetNextToken(&s, true);
            info->sky2Texture = (short) SafeTextureNumForName(s.string);
            U_MustGetInteger(&s);
            info->sky2ScrollDelta = s.number << 8;
            break;
          case MCMD_DOUBLESKY:
            info->doubleSky = true;
            break;
          case MCMD_LIGHTNING:
            info->lightning = true;
            break;
          case MCMD_FADETABLE:
            U_GetNextToken(&s, true);
            info->fadetable = W_CheckNumForName(s.string);
            break;
          case MCMD_CDTRACK:
          case MCMD_CD_STARTTRACK:
          case MCMD_CD_END1TRACK:
          case MCMD_CD_END2TRACK:
          case MCMD_CD_END3TRACK:
          case MCMD_CD_INTERTRACK:
          case MCMD_CD_TITLETRACK:
            U_MustGetInteger(&s);     /* CD audio: parsed, not used */
            break;
          default:
            U_Error(&s, "Unknown map keyword '%s'", s.string);
            break;
        }
      }

      if (map > mapMax)
        mapMax = map;

      /* If the keyword loop stopped on a "map" token, it is already in hand;
       * otherwise the lump is exhausted. */
      haveMap = sawNextMap;
    }
  }

  U_ScanClose(&s);
  MapCount = mapMax;
}

void P_SetCurrentMapInfo(int map)
{
  (void) map;   /* current map is resolved on demand via gamemap */
}

const hexen_mapinfo_t *P_CurrentMapInfo(void)
{
  return &MapInfo[QualifyMap(gamemap)];
}

int P_MapInfoCount(void)
{
  return MapCount;
}

int P_TranslateMapWarp(int warp)
{
  int i;
  for (i = 1; i <= MapCount; i++)
    if (MapInfo[i].warpTrans == warp)
      return i;
  return warp;
}

int P_GetMapWarpTrans(int map)
{
  return MapInfo[QualifyMap(map)].warpTrans;
}

short P_GetMapSky1Texture(int map)
{
  return MapInfo[QualifyMap(map)].sky1Texture;
}

short P_GetMapSky2Texture(int map)
{
  return MapInfo[QualifyMap(map)].sky2Texture;
}

fixed_t P_GetMapSky1ScrollDelta(int map)
{
  return MapInfo[QualifyMap(map)].sky1ScrollDelta;
}

fixed_t P_GetMapSky2ScrollDelta(int map)
{
  return MapInfo[QualifyMap(map)].sky2ScrollDelta;
}

dbool P_GetMapDoubleSky(int map)
{
  return MapInfo[QualifyMap(map)].doubleSky;
}

dbool P_GetMapLightning(int map)
{
  return MapInfo[QualifyMap(map)].lightning;
}

int P_GetMapCluster(int map)
{
  return MapInfo[QualifyMap(map)].cluster;
}

int P_GetMapNextMap(int map)
{
  return MapInfo[QualifyMap(map)].nextMap;
}

int P_GetMapFadeTable(int map)
{
  return MapInfo[QualifyMap(map)].fadetable;
}

const char *P_GetMapName(int map)
{
  return MapInfo[QualifyMap(map)].name;
}

const char *P_GetMapSongLump(int map)
{
  return MapInfo[QualifyMap(map)].songLump;
}
