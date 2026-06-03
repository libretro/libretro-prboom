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
 *   On maps flagged "lightning" in MAPINFO, the sky periodically flashes:
 *   every sky sector (and sectors with the lightning specials) brightens for
 *   a few tics, the sky texture swaps to the brighter Sky2, and a thunder
 *   crash plays.  The flash then decays back to the stored light levels.
 *
 *-----------------------------------------------------------------------------*/

#include <string.h>

#include "doomstat.h"
#include "m_random.h"
#include "r_state.h"
#include "r_sky.h"
#include "s_sound.h"
#include "sounds.h"
#include "z_zone.h"
#include "hexen/p_mapinfo.h"
#include "hexen/p_lightning.h"

dbool LevelHasLightning;
int   NextLightningFlash;
int   LightningFlash;

static int *LightningLightLevels;

static void P_LightningFlash(void)
{
  int        i;
  sector_t  *tempSec;
  int       *tempLight;
  dbool      foundSec;
  int        flashLight;

  if (LightningFlash)
  {
    LightningFlash--;
    if (LightningFlash)
    {
      tempLight = LightningLightLevels;
      tempSec   = sectors;
      for (i = 0; i < numsectors; i++, tempSec++)
      {
        if (tempSec->ceilingpic == skyflatnum
            || tempSec->special == LIGHTNING_SPECIAL
            || tempSec->special == LIGHTNING_SPECIAL2)
        {
          if (*tempLight < tempSec->lightlevel - 4)
            tempSec->lightlevel -= 4;
          tempLight++;
        }
      }
    }
    else
    {                           /* remove the alternate flash sky */
      tempLight = LightningLightLevels;
      tempSec   = sectors;
      for (i = 0; i < numsectors; i++, tempSec++)
      {
        if (tempSec->ceilingpic == skyflatnum
            || tempSec->special == LIGHTNING_SPECIAL
            || tempSec->special == LIGHTNING_SPECIAL2)
        {
          tempSec->lightlevel = (short) *tempLight;
          tempLight++;
        }
      }
      skytexture = Sky1Texture;     /* restore the regular sky */
    }
    return;
  }

  LightningFlash = (P_Random(pr_heretic) & 7) + 8;
  flashLight = 200 + (P_Random(pr_heretic) & 31);
  tempSec   = sectors;
  tempLight = LightningLightLevels;
  foundSec  = false;
  for (i = 0; i < numsectors; i++, tempSec++)
  {
    if (tempSec->ceilingpic == skyflatnum
        || tempSec->special == LIGHTNING_SPECIAL
        || tempSec->special == LIGHTNING_SPECIAL2)
    {
      *tempLight = tempSec->lightlevel;
      if (tempSec->special == LIGHTNING_SPECIAL)
      {
        tempSec->lightlevel += 64;
        if (tempSec->lightlevel > flashLight)
          tempSec->lightlevel = (short) flashLight;
      }
      else if (tempSec->special == LIGHTNING_SPECIAL2)
      {
        tempSec->lightlevel += 32;
        if (tempSec->lightlevel > flashLight)
          tempSec->lightlevel = (short) flashLight;
      }
      else
      {
        tempSec->lightlevel = (short) flashLight;
      }
      if (tempSec->lightlevel < *tempLight)
        tempSec->lightlevel = (short) *tempLight;
      tempLight++;
      foundSec = true;
    }
  }
  if (foundSec)
  {
    skytexture = Sky2Texture;       /* swap to the brighter flash sky */
    S_StartSound(NULL, hexen_sfx_thunder_crash);
  }

  /* schedule the next flash */
  if (!NextLightningFlash)
  {
    if (P_Random(pr_heretic) < 50)
      NextLightningFlash = (P_Random(pr_heretic) & 15) + 16;   /* quick double */
    else
    {
      if (P_Random(pr_heretic) < 128 && !(leveltime & 32))
        NextLightningFlash = ((P_Random(pr_heretic) & 7) + 2) * 35;
      else
        NextLightningFlash = ((P_Random(pr_heretic) & 15) + 5) * 35;
    }
  }
}

void P_ForceLightning(void)
{
  NextLightningFlash = 0;
}

void P_InitLightning(void)
{
  int i;
  int secCount;

  if (!hexen || !P_GetMapLightning(gamemap))
  {
    LevelHasLightning = false;
    LightningFlash    = 0;
    return;
  }

  LightningFlash = 0;
  secCount = 0;
  for (i = 0; i < numsectors; i++)
  {
    if (sectors[i].ceilingpic == skyflatnum
        || sectors[i].special == LIGHTNING_SPECIAL
        || sectors[i].special == LIGHTNING_SPECIAL2)
      secCount++;
  }

  if (secCount)
    LevelHasLightning = true;
  else
  {
    LevelHasLightning = false;
    return;
  }

  LightningLightLevels = (int *) Z_Malloc(secCount * sizeof(int), PU_LEVEL, 0);
  NextLightningFlash = ((P_Random(pr_heretic) & 15) + 5) * 35; /* no flash at start */
}

void P_UpdateLightning(void)
{
  if (!LevelHasLightning)
    return;

  if (!NextLightningFlash || LightningFlash)
    P_LightningFlash();
  else
    NextLightningFlash--;
}
