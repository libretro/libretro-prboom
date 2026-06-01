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
 *  Something to do with weapon sprite frames. Don't ask me.
 *
 *-----------------------------------------------------------------------------
 */

// We are referring to sprite numbers.
#include "doomtype.h"
#include "info.h"

#include "d_items.h"


//
// PSPRITE ACTIONS for waepons.
// This struct controls the weapon animations.
//
// Each entry is:
//  ammo/amunition type
//  upstate
//  downstate
//  readystate
//  atkstate, i.e. attack/fire/hit frame
//  flashstate, muzzle flash
//
weaponinfo_t    weaponinfo[NUMWEAPONS] =
{
  {
    // fist
    AM_NOAMMO,
    S_PUNCHUP,
    S_PUNCHDOWN,
    S_PUNCH,
    S_PUNCH1,
    S_NULL,
    WPF_FLEEMELEE|WPF_AUTOSWITCHFROM|WPF_NOAUTOSWITCHTO, // MBF21 flags
    -1                                                   // ammopershot (vanilla)
  },
  {
    // pistol
    AM_CLIP,
    S_PISTOLUP,
    S_PISTOLDOWN,
    S_PISTOL,
    S_PISTOL1,
    S_PISTOLFLASH,
    WPF_AUTOSWITCHFROM, // MBF21 flags
    -1                  // ammopershot (vanilla)
  },
  {
    // shotgun
    AM_SHELL,
    S_SGUNUP,
    S_SGUNDOWN,
    S_SGUN,
    S_SGUN1,
    S_SGUNFLASH1,
    0,  // MBF21 flags
    -1  // ammopershot (vanilla)
  },
  {
    // chaingun
    AM_CLIP,
    S_CHAINUP,
    S_CHAINDOWN,
    S_CHAIN,
    S_CHAIN1,
    S_CHAINFLASH1,
    0,  // MBF21 flags
    -1  // ammopershot (vanilla)
  },
  {
    // missile launcher
    AM_MISL,
    S_MISSILEUP,
    S_MISSILEDOWN,
    S_MISSILE,
    S_MISSILE1,
    S_MISSILEFLASH1,
    WPF_NOAUTOFIRE, // MBF21 flags
    -1              // ammopershot (vanilla)
  },
  {
    // plasma rifle
    AM_CELL,
    S_PLASMAUP,
    S_PLASMADOWN,
    S_PLASMA,
    S_PLASMA1,
    S_PLASMAFLASH1,
    0,  // MBF21 flags
    -1  // ammopershot (vanilla)
  },
  {
    // bfg 9000
    AM_CELL,
    S_BFGUP,
    S_BFGDOWN,
    S_BFG,
    S_BFG1,
    S_BFGFLASH1,
    WPF_NOAUTOFIRE, // MBF21 flags
    -1              // ammopershot (vanilla; BFG cells/shot handled separately)
  },
  {
    // chainsaw
    AM_NOAMMO,
    S_SAWUP,
    S_SAWDOWN,
    S_SAW,
    S_SAW1,
    S_NULL,
    WPF_NOTHRUST|WPF_FLEEMELEE|WPF_NOAUTOSWITCHTO, // MBF21 flags (chainsaw)
    -1                                             // ammopershot (vanilla)
  },
  {
    // super shotgun
    AM_SHELL,
    S_DSGUNUP,
    S_DSGUNDOWN,
    S_DSGUN,
    S_DSGUN1,
    S_DSGUNFLASH1,
    0,  // MBF21 flags
    -1  // ammopershot (vanilla)
  },
};
