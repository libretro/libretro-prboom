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
 *  Items: key cards, artifacts, weapon, ammunition.
 *
 *-----------------------------------------------------------------------------*/


#ifndef __D_ITEMS__
#define __D_ITEMS__

#include "doomdef.h"

/* MBF21 weapon flags ("MBF21 Bits" in a Weapon definition).  Values are
 * fixed by the MBF21 spec. */
#define WPF_NOTHRUST       0x00000001 /* doesn't thrust things */
#define WPF_SILENT         0x00000002 /* weapon is silent */
#define WPF_NOAUTOFIRE     0x00000004 /* won't autofire when swapped to */
#define WPF_FLEEMELEE      0x00000008 /* monsters consider it a melee weapon */
#define WPF_AUTOSWITCHFROM 0x00000010 /* can be switched away from on ammo pickup */
#define WPF_NOAUTOSWITCHTO 0x00000020 /* cannot be switched to on ammo pickup */

/* Weapon info: sprite frames, ammunition use. */
typedef struct
{
  ammotype_t  ammo;
  int         upstate;
  int         downstate;
  int         readystate;
  int         atkstate;
  int         flashstate;

  /* MBF21 (inert unless mbf21_features): */
  int         flags;       /* WPF_* */
  int         ammopershot;  /* ammo consumed per shot; -1 = use vanilla */
} weaponinfo_t;

extern  weaponinfo_t    weaponinfo[NUMWEAPONS];

#endif
