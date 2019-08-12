/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2000 by
 *  Jess Haas, Nicolas Kalkhof, Colin Phipps, Florian Schulze
 *  Copyright 2005, 2006 by
 *  Florian Schulze, Colin Phipps, Neil Stevens, Andrey Budko
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
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 *  02111-1307, USA.
 *
 * DESCRIPTION:  MUSINFO Support
 *
 *-----------------------------------------------------------------------------*/

// killough 3/7/98: modified to allow arbitrary listeners in spy mode
// killough 5/2/98: reindented, removed useless code, beautified
// ferk 10/1/19: cleanup/refactor, reuse u_scanner, support id:0 default music like ZDoom

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "doomstat.h"
#include "doomtype.h"
#include "d_main.h"
#include "p_mobj.h"
#include "m_misc.h"
#include "sounds.h"
#include "s_sound.h"
#include "i_sound.h"
#include "r_defs.h"
#include "w_wad.h"
#include "lprintf.h"
#include "u_scanner.h"

#include "u_musinfo.h"

#define MAX_MUS_ENTRIES 64

//
//MUSINFO lump
//

typedef struct musinfo_s
{
  mobj_t *mapthing;
  mobj_t *lastmapthing;
  int tics;
  int items[MAX_MUS_ENTRIES];
} musinfo_t;

musinfo_t musinfo;

//
// U_ParseMusInfo
// Parses MUSINFO lump.
//
void U_ParseMusInfo(const char *mapid)
{
  memset(&musinfo, 0, sizeof(musinfo));

  S_music[NUMMUSIC].lumpnum = -1;

  int musinfolump = W_GetNumForName("MUSINFO");
  if (musinfolump != -1)
  {
    const char *data = W_CacheLumpNum(musinfolump);
    int datalength = W_LumpLength(musinfolump);
    int lumpnum;
    int inMap = false;

    u_scanner_t s = U_ScanOpen(data, datalength);
    while (U_HasTokensLeft(&s))
    {
      if (inMap || U_CheckToken(&s, TK_Identifier))
      {
        if (!inMap)
          inMap = true;

        // If there's a new map as identifier, stop search
        if (U_CheckToken(&s, TK_Identifier) &&
             (s.string[0] == 'E' || s.string[0] == 'e' ||
               s.string[0] == 'M' || s.string[0] == 'm'))
        {
          break;
        }

        U_MustGetInteger(&s);

        // Check number in range
        if (s.number > 0 && s.number < MAX_MUS_ENTRIES)
        {
          if (U_MustGetToken(&s, TK_Identifier))
          {
            lumpnum = W_CheckNumForName(s.string);

            if (lumpnum >= 0)
              musinfo.items[s.number] = lumpnum;
            else
              U_Error(&s, "U_ParseMusInfo: Unknown MUS lump %s", s.string);
          }
        }
        else
          U_Error(&s, "U_ParseMusInfo: Number not in range 1 to %d", MAX_MUS_ENTRIES);
      }
      else
         U_GetNextToken(&s, TRUE);
    }

    U_ScanClose(&s);
  }
}

// Thinker for Music Changer mapthing
// It'll configure the music to play when player is in the same sector
void P_MusInfoMobjThinker(mobj_t *thing)
{
  if (musinfo.mapthing != thing &&
      thing->subsector->sector == players[displayplayer].mo->subsector->sector)
  {
    musinfo.lastmapthing = musinfo.mapthing;
    musinfo.mapthing = thing;
    musinfo.tics = 30;
  }
}

void P_MapMusicThinker(void)
{
  if (musinfo.tics < 0 || !musinfo.mapthing)
    return;

  if (musinfo.tics > 0)
    musinfo.tics--;
  else if (musinfo.lastmapthing != musinfo.mapthing)
  {
    int musitem = musinfo.mapthing->iden_num;

    if (musitem == 0) // play default level music
    {
      S_Start();
    }
    else if (musitem > 0 && musitem < MAX_MUS_ENTRIES)
    {
      char* musicname = W_GetNameForNum(musinfo.items[musitem]);
      if (musicname)
        S_ChangeMusicByName(musicname, true);
    }
    musinfo.tics = -1;
  }
}
