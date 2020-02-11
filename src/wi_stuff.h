/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
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
 * DESCRIPTION:
 *  Intermission screens.
 *
 *-----------------------------------------------------------------------------*/

#ifndef __WI_STUFF__
#define __WI_STUFF__

//#include "v_video.h"

#include "doomdef.h"

// States for the intermission

typedef enum
{
  NoState = -1,
  StatCount,
  ShowNextLoc

} stateenum_t;

struct wi_state {
  uint32_t state;
  uint32_t cnt;
  uint32_t bcnt;
  uint32_t cnt_time;
  uint32_t cnt_total_time;
  uint32_t cnt_par;
  uint32_t cnt_pause;
  uint32_t sp_state;
  uint32_t cnt_kills;
  uint32_t cnt_items;
  uint32_t cnt_secret;
  uint32_t cnt_frags;
  uint32_t dofrags;
  uint32_t ng_state;
  wbstartstruct_t wbs;
  wbplayerstruct_t plrs[MAXPLAYERS];
};

// Called by main loop, animate the intermission.
void WI_Ticker (void);

// Called by main loop,
// draws the intermission directly into the screen buffer.
void WI_Drawer (void);

// Setup for an intermission screen.
void WI_Start(wbstartstruct_t*   wbstartstruct);

// Release intermission screen memory
void WI_End(void);

void
WI_Save(struct wi_state *save);

void
WI_Load(const struct wi_state *save);

#endif
