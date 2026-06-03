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

#include "doomstat.h"
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
weaponinfo_t    doom_weaponinfo[NUMWEAPONS] =
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

/*
 * Heretic weapon table (level-1 / no-tome forms).
 *
 * Heretic reuses the player weapon slots (wp_fist..wp_supershotgun) for its
 * own arsenal: staff, gold wand, crossbow, blaster, skull rod, phoenix rod,
 * mace, gauntlets and the chicken beak. The frame numbers are Heretic state
 * indices, so this table must be selected when running Heretic -- otherwise
 * the player psprite is driven by Doom weapon states, which index into the
 * Heretic state table as garbage and crash on the first tic.
 *
 * Heretic has its own ammo types that this build's ammotype_t does not yet
 * enumerate; the per-weapon ammo here is mapped onto the existing slots as a
 * placeholder (AM_NOAMMO for the no-ammo weapons). Ammo accounting is not the
 * subject of this table -- correct weapon frames are -- and the Tome-of-Power
 * level-2 forms are a later addition.
 */
weaponinfo_t    heretic_weaponinfo[NUMWEAPONS] =
{
  { /* staff      */ AM_NOAMMO, HERETIC_S_STAFFUP,    HERETIC_S_STAFFDOWN,    HERETIC_S_STAFFREADY,    HERETIC_S_STAFFATK1_1,    S_NULL, 0, -1 },
  { /* gold wand  */ AM_CLIP,   HERETIC_S_GOLDWANDUP, HERETIC_S_GOLDWANDDOWN, HERETIC_S_GOLDWANDREADY, HERETIC_S_GOLDWANDATK1_1, S_NULL, 0, -1 },
  { /* crossbow   */ AM_CLIP,   HERETIC_S_CRBOWUP,    HERETIC_S_CRBOWDOWN,    HERETIC_S_CRBOW1,        HERETIC_S_CRBOWATK1_1,    S_NULL, 0, -1 },
  { /* blaster    */ AM_CLIP,   HERETIC_S_BLASTERUP,  HERETIC_S_BLASTERDOWN,  HERETIC_S_BLASTERREADY,  HERETIC_S_BLASTERATK1_1,  S_NULL, 0, -1 },
  { /* skull rod  */ AM_CLIP,   HERETIC_S_HORNRODUP,  HERETIC_S_HORNRODDOWN,  HERETIC_S_HORNRODREADY,  HERETIC_S_HORNRODATK1_1,  S_NULL, 0, -1 },
  { /* phoenix    */ AM_CLIP,   HERETIC_S_PHOENIXUP,  HERETIC_S_PHOENIXDOWN,  HERETIC_S_PHOENIXREADY,  HERETIC_S_PHOENIXATK1_1,  S_NULL, WPF_NOAUTOFIRE, -1 },
  { /* mace       */ AM_CLIP,   HERETIC_S_MACEUP,     HERETIC_S_MACEDOWN,     HERETIC_S_MACEREADY,     HERETIC_S_MACEATK1_1,     S_NULL, 0, -1 },
  { /* gauntlets  */ AM_NOAMMO, HERETIC_S_GAUNTLETUP, HERETIC_S_GAUNTLETDOWN, HERETIC_S_GAUNTLETREADY, HERETIC_S_GAUNTLETATK1_1, S_NULL, 0, -1 },
  { /* beak       */ AM_NOAMMO, HERETIC_S_BEAKUP,     HERETIC_S_BEAKDOWN,     HERETIC_S_BEAKREADY,     HERETIC_S_BEAKATK1_1,     S_NULL, 0, -1 }
};

/* Active weapon table. Points at the Doom table by default; swapped to the
 * Heretic table by D_InitWeaponInfo once the game type is known. All weapon
 * code indexes through this pointer. */
weaponinfo_t   *weaponinfo = doom_weaponinfo;

/* Hexen weapons.  Only the Fighter column is populated; the Cleric and Mage
 * columns are inert placeholders (all HEXEN_S_NULL / MANA_NONE) to be filled
 * when those classes are wired.  Indexed [slot][class]; PCLASS_NULL is the
 * unused Doom/Heretic slot. */
hexen_weaponinfo_t WeaponInfo[NUMWEAPONS][NUMCLASSES] =
{
  /* WP_FIRST */
  {
    { MANA_NONE, HEXEN_S_NULL,     HEXEN_S_NULL,       HEXEN_S_NULL,        HEXEN_S_NULL,        HEXEN_S_NULL        }, /* PCLASS_NULL */
    { MANA_NONE, HEXEN_S_PUNCHUP,  HEXEN_S_PUNCHDOWN,  HEXEN_S_PUNCHREADY,  HEXEN_S_PUNCHATK1_1, HEXEN_S_PUNCHATK2_1 }, /* PCLASS_FIGHTER */
    { MANA_NONE, HEXEN_S_CMACEUP,  HEXEN_S_CMACEDOWN,  HEXEN_S_CMACEREADY,  HEXEN_S_CMACEATK_1,  HEXEN_S_CMACEATK_1  }, /* PCLASS_CLERIC: mace */
    { MANA_NONE, HEXEN_S_MWANDUP,  HEXEN_S_MWANDDOWN,  HEXEN_S_MWANDREADY,  HEXEN_S_MWANDATK_1,  HEXEN_S_MWANDATK_1  }, /* PCLASS_MAGE: wand */
    { MANA_NONE, HEXEN_S_NULL,     HEXEN_S_NULL,       HEXEN_S_NULL,        HEXEN_S_NULL,        HEXEN_S_NULL        }  /* PCLASS_PIG */
  },
  /* WP_SECOND */
  {
    { MANA_NONE, HEXEN_S_NULL,     HEXEN_S_NULL,       HEXEN_S_NULL,        HEXEN_S_NULL,      HEXEN_S_NULL      },
    { MANA_1,    HEXEN_S_FAXEUP,   HEXEN_S_FAXEDOWN,   HEXEN_S_FAXEREADY,   HEXEN_S_FAXEATK_1, HEXEN_S_FAXEATK_1 }, /* PCLASS_FIGHTER: axe */
    { MANA_1,    HEXEN_S_CSTAFFUP, HEXEN_S_CSTAFFDOWN, HEXEN_S_CSTAFFREADY, HEXEN_S_CSTAFFATK_1, HEXEN_S_CSTAFFATK_1 }, /* PCLASS_CLERIC: serpent staff */
    { MANA_1,    HEXEN_S_CONEUP,   HEXEN_S_CONEDOWN,   HEXEN_S_CONEREADY,   HEXEN_S_CONEATK1_1, HEXEN_S_CONEATK1_3 }, /* PCLASS_MAGE: cone of shards */
    { MANA_NONE, HEXEN_S_NULL,     HEXEN_S_NULL,       HEXEN_S_NULL,        HEXEN_S_NULL,      HEXEN_S_NULL      }
  },
  /* WP_THIRD */
  {
    { MANA_NONE, HEXEN_S_NULL,        HEXEN_S_NULL,         HEXEN_S_NULL,          HEXEN_S_NULL,         HEXEN_S_NULL         },
    { MANA_2,    HEXEN_S_FHAMMERUP,   HEXEN_S_FHAMMERDOWN,  HEXEN_S_FHAMMERREADY,  HEXEN_S_FHAMMERATK_1, HEXEN_S_FHAMMERATK_1 }, /* PCLASS_FIGHTER: hammer */
    { MANA_2,    HEXEN_S_CFLAMEUP,    HEXEN_S_CFLAMEDOWN,   HEXEN_S_CFLAMEREADY1,  HEXEN_S_CFLAMEATK_1,  HEXEN_S_CFLAMEATK_1  }, /* PCLASS_CLERIC: flame strike */
    { MANA_2,    HEXEN_S_MLIGHTNINGUP, HEXEN_S_MLIGHTNINGDOWN, HEXEN_S_MLIGHTNINGREADY, HEXEN_S_MLIGHTNINGATK_1, HEXEN_S_MLIGHTNINGATK_1 }, /* PCLASS_MAGE: arc of death */
    { MANA_NONE, HEXEN_S_NULL,        HEXEN_S_NULL,         HEXEN_S_NULL,          HEXEN_S_NULL,         HEXEN_S_NULL         }
  },
  /* WP_FOURTH */
  {
    { MANA_NONE, HEXEN_S_NULL,        HEXEN_S_NULL,         HEXEN_S_NULL,          HEXEN_S_NULL,         HEXEN_S_NULL         },
    { MANA_BOTH, HEXEN_S_FSWORDUP,    HEXEN_S_FSWORDDOWN,   HEXEN_S_FSWORDREADY,   HEXEN_S_FSWORDATK_1,  HEXEN_S_FSWORDATK_1  }, /* PCLASS_FIGHTER: sword */
    { MANA_BOTH, HEXEN_S_CHOLYUP,     HEXEN_S_CHOLYDOWN,    HEXEN_S_CHOLYREADY,    HEXEN_S_CHOLYATK_1,   HEXEN_S_CHOLYATK_1   }, /* PCLASS_CLERIC: wraithverge */
    { MANA_BOTH, HEXEN_S_MSTAFFUP,    HEXEN_S_MSTAFFDOWN,   HEXEN_S_MSTAFFREADY,   HEXEN_S_MSTAFFATK_1,  HEXEN_S_MSTAFFATK_1  }, /* PCLASS_MAGE: bloodscourge */
    { MANA_NONE, HEXEN_S_NULL,        HEXEN_S_NULL,         HEXEN_S_NULL,          HEXEN_S_NULL,         HEXEN_S_NULL         }
  }
  /* slots 4..NUMWEAPONS-1 are zero-initialised (unused by Hexen) */
};

/* Per-class mana cost per weapon slot.  Fighter: fists free, axe 2 MANA_1,
 * hammer 3 MANA_2, sword (Quietus) 14 of both.  Other classes filled later. */
int WeaponManaUse[NUMCLASSES][NUMWEAPONS] =
{
  {  0,  0,  0,  0 }, /* PCLASS_NULL */
  {  0,  2,  3, 14 }, /* PCLASS_FIGHTER */
  {  0,  1,  4, 18 }, /* PCLASS_CLERIC: mace free, staff 1, flame 4, wraithverge 18 */
  {  0,  3,  5, 15 }, /* PCLASS_MAGE: wand free, cone 3, arc 5, bloodscourge 15 */
  {  0,  0,  0,  0 }  /* PCLASS_PIG */
};

/* Doom-shaped weapon table for the shared raise/lower path (P_BringUpWeapon
 * reads weaponinfo[pendingweapon].upstate).  Only the upstate is meaningful
 * for Hexen; the rest of the per-class behaviour comes from WeaponInfo[][].
 * Seeded for the Fighter; selected at runtime once class support is fully
 * wired. */
weaponinfo_t   hexen_weaponinfo[NUMWEAPONS] =
{
  { AM_NOAMMO, HEXEN_S_PUNCHUP,   HEXEN_S_PUNCHDOWN,   HEXEN_S_PUNCHREADY,   HEXEN_S_PUNCHATK1_1,  S_NULL, 0, -1 },
  { AM_NOAMMO, HEXEN_S_FAXEUP,    HEXEN_S_FAXEDOWN,    HEXEN_S_FAXEREADY,    HEXEN_S_FAXEATK_1,    S_NULL, 0, -1 },
  { AM_NOAMMO, HEXEN_S_FHAMMERUP, HEXEN_S_FHAMMERDOWN, HEXEN_S_FHAMMERREADY, HEXEN_S_FHAMMERATK_1, S_NULL, 0, -1 },
  { AM_NOAMMO, HEXEN_S_FSWORDUP,  HEXEN_S_FSWORDDOWN,  HEXEN_S_FSWORDREADY,  HEXEN_S_FSWORDATK_1,  S_NULL, 0, -1 }
};

void D_InitWeaponInfo(void)
{
  if (hexen)
    weaponinfo = hexen_weaponinfo;
  else
    weaponinfo = heretic ? heretic_weaponinfo : doom_weaponinfo;
}
