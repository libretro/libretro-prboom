//-----------------------------------------------------------------------------
//
// Copyright 2017 Christoph Oelckers
// Copyright 2019 Fernando Carmona Varo
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
//-----------------------------------------------------------------------------

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>

#include "doomdef.h"
#include "m_misc.h"
#include "g_game.h"
#include "u_scanner.h"

#include "u_mapinfo.h"


//==========================================================================
//
// The Doom actors in their original order
// Names are the same as in ZDoom.
//
//==========================================================================

static const int actornum = 144;
static const char * const ActorNames[] =
{
  "DoomPlayer",
  "ZombieMan",
  "ShotgunGuy",
  "Archvile",
  "ArchvileFire",
  "Revenant",
  "RevenantTracer",
  "RevenantTracerSmoke",
  "Fatso",
  "FatShot",
  "ChaingunGuy",
  "DoomImp",
  "Demon",
  "Spectre",
  "Cacodemon",
  "BaronOfHell",
  "BaronBall",
  "HellKnight",
  "LostSoul",
  "SpiderMastermind",
  "Arachnotron",
  "Cyberdemon",
  "PainElemental",
  "WolfensteinSS",
  "CommanderKeen",
  "BossBrain",
  "BossEye",
  "BossTarget",
  "SpawnShot",
  "SpawnFire",
  "ExplosiveBarrel",
  "DoomImpBall",
  "CacodemonBall",
  "Rocket",
  "PlasmaBall",
  "BFGBall",
  "ArachnotronPlasma",
  "BulletPuff",
  "Blood",
  "TeleportFog",
  "ItemFog",
  "TeleportDest",
  "BFGExtra",
  "GreenArmor",
  "BlueArmor",
  "HealthBonus",
  "ArmorBonus",
  "BlueCard",
  "RedCard",
  "YellowCard",
  "YellowSkull",
  "RedSkull",
  "BlueSkull",
  "Stimpack",
  "Medikit",
  "Soulsphere",
  "InvulnerabilitySphere",
  "Berserk",
  "BlurSphere",
  "RadSuit",
  "Allmap",
  "Infrared",
  "Megasphere",
  "Clip",
  "ClipBox",
  "RocketAmmo",
  "RocketBox",
  "Cell",
  "CellPack",
  "Shell",
  "ShellBox",
  "Backpack",
  "BFG9000",
  "Chaingun",
  "Chainsaw",
  "RocketLauncher",
  "PlasmaRifle",
  "Shotgun",
  "SuperShotgun",
  "TechLamp",
  "TechLamp2",
  "Column",
  "TallGreenColumn",
  "ShortGreenColumn",
  "TallRedColumn",
  "ShortRedColumn",
  "SkullColumn",
  "HeartColumn",
  "EvilEye",
  "FloatingSkull",
  "TorchTree",
  "BlueTorch",
  "GreenTorch",
  "RedTorch",
  "ShortBlueTorch",
  "ShortGreenTorch",
  "ShortRedTorch",
  "Slalagtite",
  "TechPillar",
  "CandleStick",
  "Candelabra",
  "BloodyTwitch",
  "Meat2",
  "Meat3",
  "Meat4",
  "Meat5",
  "NonsolidMeat2",
  "NonsolidMeat4",
  "NonsolidMeat3",
  "NonsolidMeat5",
  "NonsolidTwitch",
  "DeadCacodemon",
  "DeadMarine",
  "DeadZombieMan",
  "DeadDemon",
  "DeadLostSoul",
  "DeadDoomImp",
  "DeadShotgunGuy",
  "GibbedMarine",
  "GibbedMarineExtra",
  "HeadsOnAStick",
  "Gibs",
  "HeadOnAStick",
  "HeadCandles",
  "DeadStick",
  "LiveStick",
  "BigTree",
  "BurningBarrel",
  "HangNoGuts",
  "HangBNoBrain",
  "HangTLookingDown",
  "HangTSkull",
  "HangTLookingUp",
  "HangTNoBrain",
  "ColonGibs",
  "SmallBloodPool",
  "BrainStem",
  //Boom/MBF additions
  "PointPusher",
  "PointPuller",
  "MBFHelperDog",
  "PlasmaBall1",
  "PlasmaBall2",
  "EvilSceptre",
  "UnholyBible",
  NULL
};


void M_AddEpisode(const char *map, char *def);

umapinfo_t U_mapinfo;

// -----------------------------------------------
//
//
// -----------------------------------------------

static void FreeMap(mapentry_t *mape)
{
  if (mape->mapname) free(mape->mapname);
  if (mape->levelname) free(mape->levelname);
  if (mape->intertext) free(mape->intertext);
  if (mape->intertextsecret) free(mape->intertextsecret);
  mape->mapname = NULL;
}

static void ReplaceString(char **pptr, const char *newstring)
{
  if (*pptr != NULL) free(*pptr);
  *pptr = strdup(newstring);
}


// -----------------------------------------------
// Parses a set of string and concatenates them
// Returns a pointer to the string (must be freed)
// -----------------------------------------------
static char *ParseMultiString(u_scanner_t* s, int error)
{
  char *build = NULL;

  if (U_CheckToken(s, TK_Identifier))
  {
    if (!strcasecmp(s->string, "clear"))
    {
      return strdup("-"); // this was explicitly deleted to override the default.
    }
    else
    {
      U_Error(s, "Either 'clear' or string constant expected");
    }
  }

  do
  {
    U_MustGetToken(s, TK_StringConst);
    if (build == NULL) build = strdup(s->string);
    else
    {
      size_t oldlen = strlen(build);
      size_t newlen = oldlen + strlen(s->string) + 2;

      build = (char*)realloc(build, newlen);
      build[oldlen] = '\n';
      strcpy(build + oldlen + 1, s->string);
      build[newlen] = 0;
    }
  } while (U_CheckToken(s, ','));
  return build;
}


// -----------------------------------------------
// Parses a lump name. The buffer must be at least 9 characters.
// -----------------------------------------------
static int ParseLumpName(u_scanner_t* s, char *buffer)
{
  U_MustGetToken(s, TK_StringConst);
  if (strlen(s->string) > 8)
  {
    U_Error(s, "String too long. Maximum size is 8 characters.");
    return 0;
  }
  strncpy(buffer, s->string, 8);
  buffer[8] = 0;
  M_Strupr(buffer);
  return 1;
}


// -----------------------------------------------
// Parses a standard property that is already known
// These do not get stored in the property list
// but in dedicated struct member variables.
// -----------------------------------------------
static int ParseStandardProperty(u_scanner_t* s, mapentry_t *mape)
{
  U_MustGetToken(s, TK_Identifier);
  char *pname = strdup(s->string);
  U_MustGetToken(s, '=');

  if (!strcasecmp(pname, "levelname"))
  {
    U_MustGetToken(s, TK_StringConst);
    ReplaceString(&mape->levelname, s->string);
  }
  else if (!strcasecmp(pname, "episode"))
  {
    char *lname = ParseMultiString(s, 1);
    if (!lname) return 0;
    M_AddEpisode(mape->mapname, lname);
  }
  else if (!strcasecmp(pname, "next"))
  {
    ParseLumpName(s, mape->nextmap);
    if (!G_ValidateMapName(mape->nextmap, NULL, NULL))
    {
      U_Error(s, "Invalid map name %s.", mape->nextmap);
      return 0;
    }
  }
  else if (!strcasecmp(pname, "nextsecret"))
  {
    ParseLumpName(s, mape->nextsecret);
    if (!G_ValidateMapName(mape->nextsecret, NULL, NULL))
    {
      U_Error(s, "Invalid map name %s", mape->nextmap);
      return 0;
    }
  }
  else if (!strcasecmp(pname, "levelpic"))
  {
    ParseLumpName(s, mape->levelpic);
  }
  else if (!strcasecmp(pname, "skytexture"))
  {
    ParseLumpName(s, mape->skytexture);
  }
  else if (!strcasecmp(pname, "music"))
  {
    ParseLumpName(s, mape->music);
  }
  else if (!strcasecmp(pname, "endpic"))
  {
    ParseLumpName(s, mape->endpic);
  }
  else if (!strcasecmp(pname, "endcast"))
  {
    U_MustGetToken(s, TK_BoolConst);
    if (s->boolean) strcpy(mape->endpic, "$CAST");
    else strcpy(mape->endpic, "-");
  }
  else if (!strcasecmp(pname, "endbunny"))
  {
    U_MustGetToken(s, TK_BoolConst);
    if (s->boolean) strcpy(mape->endpic, "$BUNNY");
    else strcpy(mape->endpic, "-");
  }
  else if (!strcasecmp(pname, "endgame"))
  {
    U_MustGetToken(s, TK_BoolConst);
    if (s->boolean) strcpy(mape->endpic, "!");
    else strcpy(mape->endpic, "-");
  }
  else if (!strcasecmp(pname, "exitpic"))
  {
    ParseLumpName(s, mape->exitpic);
  }
  else if (!strcasecmp(pname, "enterpic"))
  {
    ParseLumpName(s, mape->enterpic);
  }
  else if (!strcasecmp(pname, "nointermission"))
  {
    U_MustGetToken(s, TK_BoolConst);
    mape->nointermission = s->boolean;
  }
  else if (!strcasecmp(pname, "partime"))
  {
    U_MustGetInteger(s);
    mape->partime = TICRATE * s->number;
  }
  else if (!strcasecmp(pname, "intertext"))
  {
    char *lname = ParseMultiString(s, 1);
    if (!lname) return 0;
    if (mape->intertext != NULL) free(mape->intertext);
    mape->intertext = lname;
  }
  else if (!strcasecmp(pname, "intertextsecret"))
  {
    char *lname = ParseMultiString(s, 1);
    if (!lname) return 0;
    if (mape->intertextsecret != NULL) free(mape->intertextsecret);
    mape->intertextsecret = lname;
  }
  else if (!strcasecmp(pname, "interbackdrop"))
  {
    ParseLumpName(s, mape->interbackdrop);
  }
  else if (!strcasecmp(pname, "intermusic"))
  {
    ParseLumpName(s, mape->intermusic);
  }
  else if (!strcasecmp(pname, "bossaction"))
  {
    U_MustGetToken(s, TK_Identifier);
    int special, tag;
    if (!strcasecmp(s->string, "clear"))
    {
      // mark level free of boss actions
      special = tag = -1;
      if (mape->bossactions) free(mape->bossactions);
      mape->bossactions = NULL;
      mape->numbossactions = -1;
    }
    else
    {
      int i;
      for (i = 0; i < actornum; i++)
      {
        if (!strcasecmp(s->string, ActorNames[i])) break;
      }
      if (ActorNames[i] == NULL)
      {
        U_Error(s, "Unknown thing type %s", s->string);
        return 0;
      }

      U_MustGetToken(s, ',');
      U_MustGetInteger(s);
      special = s->number;
      U_MustGetToken(s, ',');
      U_MustGetInteger(s);
      tag = s->number;
      // allow no 0-tag specials here, unless a level exit.
      if (tag != 0 || special == 11 || special == 51 || special == 52 || special == 124)
      {
        if (mape->numbossactions == -1) mape->numbossactions = 1;
        else mape->numbossactions++;
        mape->bossactions = (bossaction_t *)realloc(mape->bossactions, sizeof(bossaction_t) * mape->numbossactions);
        mape->bossactions[mape->numbossactions - 1].type = i;
        mape->bossactions[mape->numbossactions - 1].special = special;
        mape->bossactions[mape->numbossactions - 1].tag = tag;
      }
    }
  }
  else do
  {
    if (!U_CheckFloat(s)) U_GetNextToken(s, TRUE);
    if (s->token > TK_BoolConst)
    {
      U_ErrorToken(s, TK_Identifier);
    }
  } while (U_CheckToken(s, ','));

  free(pname);
  return 1;
}

// -----------------------------------------------
//
// Parses a complete map entry
//
// -----------------------------------------------

static int ParseMapEntry(u_scanner_t *s, mapentry_t *val)
{
  val->mapname = NULL;

  U_MustGetIdentifier(s, "map");
  U_MustGetToken(s, TK_Identifier);

  if (!G_ValidateMapName(s->string, NULL, NULL))
  {
    U_Error(s, "Invalid map name %s", s->string);
    return 0;
  }

  ReplaceString(&val->mapname, s->string);
  U_MustGetToken(s, '{');
  while(!U_CheckToken(s, '}'))
  {
    ParseStandardProperty(s, val);
  }
  return 1;
}


// -----------------------------------------------
//
// Parses a complete UMAPINFO lump
//
// -----------------------------------------------

int U_ParseMapInfo(const char *buffer, size_t length)
{
  unsigned int i;
  u_scanner_t scanner = U_ScanOpen(buffer, length);

  while (U_HasTokensLeft(&scanner))
  {
    mapentry_t parsed = { 0 };
    if (!ParseMapEntry(&scanner, &parsed))
    {
      U_Error(&scanner, "Skipping entry: %s", scanner.string);
    }

    // Does this property already exist? If yes, replace it.
    for(i = 0; i < U_mapinfo.mapcount; i++)
    {
      if (!strcmp(parsed.mapname, U_mapinfo.maps[i].mapname))
      {
        FreeMap(&U_mapinfo.maps[i]);
        U_mapinfo.maps[i] = parsed;
        break;
      }
    }
    // Not found so create a new one.
    if (i == U_mapinfo.mapcount)
    {
      U_mapinfo.mapcount++;
      U_mapinfo.maps = (mapentry_t*)realloc(U_mapinfo.maps, sizeof(mapentry_t)*U_mapinfo.mapcount);
      U_mapinfo.maps[U_mapinfo.mapcount-1] = parsed;
    }
  }
  U_ScanClose(&scanner);
  return 1;
}



void U_FreeMapInfo()
{
  unsigned i;

  for(i = 0; i < U_mapinfo.mapcount; i++)
  {
    FreeMap(&U_mapinfo.maps[i]);
  }
  free(U_mapinfo.maps);
  U_mapinfo.maps = NULL;
  U_mapinfo.mapcount = 0;
}
