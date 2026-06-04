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
 *      Weapon sprite animation, weapon objects.
 *      Action functions for weapons.
 *
 *-----------------------------------------------------------------------------*/

#include "doomstat.h"
#include "r_main.h"
#include "p_map.h"
#include "p_maputl.h"
#include "p_inter.h"
#include "p_pspr.h"
extern fixed_t FloatBobOffsets[64];
dbool P_SeekerMissile(mobj_t *actor, mobj_t **seekTarget, angle_t thresh, angle_t turnMax, dbool seekcenter);
void A_UnHideThing(mobj_t *thing);
#include "dsda_hacked.h"
#include "p_enemy.h"
#include "p_tick.h"
#include "m_random.h"
#include "s_sound.h"
#include "sounds.h"
#include "d_event.h"
#include "r_demo.h"

#define LOWERSPEED   (FRACUNIT*6)
#define RAISESPEED   (FRACUNIT*6)
#define WEAPONBOTTOM (FRACUNIT*128)
#define WEAPONTOP    (FRACUNIT*32)

#define BFGCELLS bfgcells        /* Ty 03/09/98 externalized in p_inter.c */

extern void P_Thrust(player_t *, angle_t, fixed_t);
extern mobjtype_t PuffType;   /* puff actor the next hitscan/melee spawns */
extern mobj_t *PuffSpawned;   /* last puff spawned by P_SpawnPuff */

extern void retro_set_rumble_damage(int damage, float duration);

// The following array holds the recoil values         // phares

static const int recoil_values[] = {    // phares
  10, // WP_FIST
  10, // WP_PISTOL
  30, // WP_SHOTGUN
  10, // WP_CHAINGUN
  100,// WP_MISSILE
  20, // WP_PLASMA
  100,// WP_BFG
  0,  // WP_CHAINSAW
  80  // WP_SUPERSHOTGUN
};

//
// P_SetPsprite
//

static void P_SetPsprite(player_t *player, int position, statenum_t stnum)
{
   pspdef_t *psp = &player->psprites[position];

   do
   {
      state_t *state;

      if (!stnum)
      {
         /* object removed itself */
         psp->state = NULL;
         break;
      }

      /* DSDHacked: stnum comes from editable weapon-frame data
       * (nextstate, or a codepointer's target state) and may fall outside
       * the grown state table.  Indexing states[] with it would be out of
       * bounds; treat it like state 0 (psprite removed) rather than crash. */
      if ((unsigned)stnum >= (unsigned)num_states)
      {
         psp->state = NULL;
         break;
      }

      state      = &states[stnum];
      psp->state = state;
      psp->tics  = state->tics;        // could be 0

      if (state->misc1)
      {
         /* coordinate set */
         psp->sx = state->misc1 << FRACBITS;
         psp->sy = state->misc2 << FRACBITS;
      }

#ifdef HEXEN
      if(state->misc2)
      {
         psp->sy = state->misc2<<FRACBITS;
      }
#endif

      // Call action routine.
      // Modified handling.
      if (state->action.arg2)
      {
         state->action.arg2(player, psp);
         if (!psp->state)
            break;
      }
      stnum = psp->state->nextstate;
   }while (!psp->tics);     /* an initial state of 0 could cycle through */
}

/*
 * P_BringUpWeapon
 * Starts bringing the pending weapon up
 * from the bottom of the screen.
 * Uses player
*/

static void P_BringUpWeapon(player_t *player)
{
   statenum_t newstate;

   if (player->pendingweapon == WP_NOCHANGE)
      player->pendingweapon = player->readyweapon;

#ifdef HEXEN
   if(player->class == PCLASS_FIGHTER && player->pendingweapon == WP_SECOND
         && player->mana[MANA_1])
      newstate = S_FAXEUP_G;
   else
      newstate = weaponinfo[player->pendingweapon].upstate;
#else
   if (player->pendingweapon == WP_CHAINSAW)
      S_StartSound (player->mo, sfx_sawup);
   newstate = weaponinfo[player->pendingweapon].upstate;
#endif

   player->pendingweapon = WP_NOCHANGE;

   player->psprites[ps_weapon].sy = WEAPONBOTTOM;

#ifndef HEXEN
   // killough 12/98: prevent pistol from starting visibly at bottom of screen:
   if (mbf_features)
      player->psprites[ps_weapon].sy = WEAPONBOTTOM+FRACUNIT*2;
#endif

   P_SetPsprite(player, ps_weapon, newstate);
}

//---------------------------------------------------------------------------
//
// FUNC P_CheckMana
//
// Returns true if there is enough mana to shoot.  If not, selects the
// next weapon to use.
//
//---------------------------------------------------------------------------

#ifdef HEXEN
dbool P_CheckMana(player_t *player)
{
   manatype_t mana;
   int count;

   mana = WeaponInfo[player->readyweapon][player->class].mana;
   count = WeaponManaUse[player->class][player->readyweapon];
   if(mana == MANA_BOTH)
   {
      if(player->mana[MANA_1] >= count && player->mana[MANA_2] >= count)
      {
         return true;
      }
   }
   else if(mana == MANA_NONE || player->mana[mana] >= count)
   {
      return(true);
   }
   // out of mana, pick a weapon to change to
   do
   {
      if(player->weaponowned[WP_THIRD]
            && player->mana[MANA_2] >= WeaponManaUse[player->class][WP_THIRD])
      {
         player->pendingweapon = WP_THIRD;
      }
      else if(player->weaponowned[WP_SECOND]
            && player->mana[MANA_1] >= WeaponManaUse[player->class][WP_SECOND])
      {
         player->pendingweapon = WP_SECOND;
      }
      else if(player->weaponowned[WP_FOURTH]
            && player->mana[MANA_1] >= WeaponManaUse[player->class][WP_FOURTH]
            && player->mana[MANA_2] >= WeaponManaUse[player->class][WP_FOURTH])
      {
         player->pendingweapon = WP_FOURTH;
      }
      else
      {
         player->pendingweapon = WP_FIRST;
      }
   } while(player->pendingweapon == WP_NOCHANGE);
   P_SetPsprite(player, ps_weapon,
         WeaponInfo[player->readyweapon][player->class].downstate);
   return(false);
}
#endif

// The first set is where the weapon preferences from             // killough,
// default.cfg are stored. These values represent the keys used   // phares
// in DOOM2 to bring up the weapon, i.e. 6 = plasma gun. These    //    |
// are NOT the wp_* constants.                                    //    V

int weapon_preferences[2][NUMWEAPONS+1] = {
  {6, 9, 4, 3, 2, 8, 5, 7, 1, 0},  // !compatibility preferences
  {6, 9, 4, 3, 2, 8, 5, 7, 1, 0},  //  compatibility preferences
};

// P_SwitchWeapon checks current ammo levels and gives you the
// most preferred weapon with ammo. It will not pick the currently
// raised weapon. When called from P_CheckAmmo this won't matter,
// because the raised weapon has no ammo anyway. When called from
// G_BuildTiccmd you want to toggle to a different weapon regardless.

int P_SwitchWeapon(player_t *player)
{
  int *prefer = weapon_preferences[demo_compatibility!=0]; // killough 3/22/98
  int currentweapon = player->readyweapon;
  int newweapon = currentweapon;
  int i = NUMWEAPONS+1;   // killough 5/2/98

  // killough 2/8/98: follow preferences and fix BFG/SSG bugs

  do
    switch (*prefer++)
      {
      case 1:
        if (!player->powers[pw_strength])      // allow chainsaw override
          break;
        // fall through
      case 0:
        newweapon = WP_FIST;
        break;
      case 2:
        if (player->ammo[AM_CLIP])
          newweapon = WP_PISTOL;
        break;
      case 3:
        if (player->weaponowned[WP_SHOTGUN] && player->ammo[AM_SHELL])
          newweapon = WP_SHOTGUN;
        break;
      case 4:
        if (player->weaponowned[WP_CHAINGUN] && player->ammo[AM_CLIP])
          newweapon = WP_CHAINGUN;
        break;
      case 5:
        if (player->weaponowned[WP_MISSILE] && player->ammo[AM_MISL])
          newweapon = WP_MISSILE;
        break;
      case 6:
        if (player->weaponowned[WP_PLASMA] && player->ammo[AM_CELL] &&
            gamemode != shareware)
          newweapon = WP_PLASMA;
        break;
      case 7:
        if (player->weaponowned[WP_BFG] && gamemode != shareware &&
            player->ammo[AM_CELL] >= (demo_compatibility ? 41 : 40))
          newweapon = WP_BFG;
        break;
      case 8:
        if (player->weaponowned[WP_CHAINSAW])
          newweapon = WP_CHAINSAW;
        break;
      case 9:
        if (player->weaponowned[WP_SUPERSHOTGUN] && gamemode == commercial &&
            player->ammo[AM_SHELL] >= (demo_compatibility ? 3 : 2))
          newweapon = WP_SUPERSHOTGUN;
        break;
      }
  while (newweapon==currentweapon && --i);          // killough 5/2/98
  return newweapon;
}

// killough 5/2/98: whether consoleplayer prefers weapon w1 over weapon w2.
int P_WeaponPreferred(int w1, int w2)
{
  return
    (weapon_preferences[0][0] != ++w2 && (weapon_preferences[0][0] == ++w1 ||
    (weapon_preferences[0][1] !=   w2 && (weapon_preferences[0][1] ==   w1 ||
    (weapon_preferences[0][2] !=   w2 && (weapon_preferences[0][2] ==   w1 ||
    (weapon_preferences[0][3] !=   w2 && (weapon_preferences[0][3] ==   w1 ||
    (weapon_preferences[0][4] !=   w2 && (weapon_preferences[0][4] ==   w1 ||
    (weapon_preferences[0][5] !=   w2 && (weapon_preferences[0][5] ==   w1 ||
    (weapon_preferences[0][6] !=   w2 && (weapon_preferences[0][6] ==   w1 ||
    (weapon_preferences[0][7] !=   w2 && (weapon_preferences[0][7] ==   w1
   ))))))))))))))));
}

// P_GetAmmoLevel
// split out of P_CheckAmmo
// given a player and a weapon type, returns a number [0,100]
//  describing how fully loaded the weapon is.
// special cased to return 0 for weapons that cannot be fired.
// also used by the status bar and HUD for number colouring.

int P_GetAmmoLevel(player_t *player, weapontype_t weapon)
{
  ammotype_t ammo = weaponinfo[weapon].ammo;
  int current, min, max, result;

  /* MBF21: a weapon with an explicit Ammo per shot uses it as the minimum
   * needed to fire.  ammopershot defaults to -1 (vanilla) for every weapon,
   * so this branch is inert unless a deh patch set the field. */
  if (mbf21_features && weaponinfo[weapon].ammopershot >= 0)
    min = weaponinfo[weapon].ammopershot;
  else if (weapon == WP_BFG) // Minimal amount for one shot varies.
    min = BFGCELLS;
  else if (weapon == WP_SUPERSHOTGUN) // Double barrel.
    min = 2;
  else
    min = 1; // Regular

  current = player->ammo[ammo];
  max = player->maxammo[ammo];

  if (ammo == AM_NOAMMO // no ammunition => always full
      || current >= max // weapon is full
      || max == 0) // avoid div-by-zero
    result = 100;
  else if (current < min) // weapon is empty
    result = 0;
  else
  {
    // this division may still give 0 for sufficiently large
    // values of max. make sure the weapon can still be fired.
    result = (100 * current) / max;
    if (result < 1)
      result = 1;
  }

  return result;
}

//
// P_CheckAmmo
// Returns true if there is enough ammo to shoot.
// If not, selects the next weapon to use.
// (only in demo_compatibility mode -- killough 3/22/98)
//

dbool P_CheckAmmo(player_t *player)
{
  if (P_GetAmmoLevel(player, player->readyweapon) > 0) // has enough ammo
    return true;

  // Out of ammo, pick a weapon to change to.
  //
  // killough 3/22/98: for old demos we do the switch here and now;
  // for Boom games we cannot do this, and have different player
  // preferences across demos or networks, so we have to use the
  // G_BuildTiccmd() interface instead of making the switch here.

  if (demo_compatibility)
    {
      player->pendingweapon = P_SwitchWeapon(player);      // phares
      // Now set appropriate weapon overlay.
      P_SetPsprite(player,ps_weapon,weaponinfo[player->readyweapon].downstate);
    }

  return false;
}

/*
 * P_FireWeapon.
*/

static void P_FireWeapon(player_t *player)
{
  statenum_t newstate;

#ifdef HEXEN
  if (!P_CheckMana(player))
#else
  if (!P_CheckAmmo(player))
#endif
    return;

#ifdef HEXEN
  P_SetMobjState(player->mo, PStateAttack[player->class]); // S_PLAY_ATK1);
  if(player->class == PCLASS_FIGHTER && player->readyweapon == WP_SECOND
        && player->mana[MANA_1] > 0)
     attackState = S_FAXEATK_G1; /* Glowing axe */
  else
     attackState = player->refire ? 
        WeaponInfo[player->readyweapon][player->class].holdatkstate
        : WeaponInfo[player->readyweapon][player->class].atkstate;
#else
  P_SetMobjState(player->mo, g_s_play_atk1);
  newstate = weaponinfo[player->readyweapon].atkstate;
#endif

  /* Hexen: pick the attack state from the per-class weapon table rather
   * than the Doom-shaped weaponinfo[], and put the player model into its
   * class attack pose.  The Fighter's axe (WP_SECOND) additionally has a
   * powered, glowing attack animation when blue mana is available. */
  if (hexen)
  {
    P_SetMobjState(player->mo, PStateAttack[player->class]);
    if (player->class == PCLASS_FIGHTER && player->readyweapon == WP_SECOND
        && player->mana[MANA_1] > 0)
      newstate = HEXEN_S_FAXEATK_G1;
    else
      newstate = player->refire
        ? WeaponInfo[player->readyweapon][player->class].holdatkstate
        : WeaponInfo[player->readyweapon][player->class].atkstate;
  }

  P_SetPsprite(player, ps_weapon, newstate);
  /* MBF21: a SILENT weapon does not alert monsters when fired. */
  if (!(mbf21_features &&
        (weaponinfo[player->readyweapon].flags & WPF_SILENT)))
    P_NoiseAlert(player->mo, player->mo);
}

/*
 * P_DropWeapon
 * Player died, so put the weapon away.
*/

void P_DropWeapon(player_t *player)
{
  P_SetPsprite(player, ps_weapon, weaponinfo[player->readyweapon].downstate);
}

/*
 * A_WeaponReady
 * The player can fire the weapon
 * or change to another weapon at this time.
 * Follows after getting weapon up,
 * or after previous attack/fire sequence.
*/

void A_WeaponReady(player_t *player, pspdef_t *psp)
{
#ifdef HEXEN
   // Change player from attack state
	if(player->mo->state >= &states[PStateAttack[player->class]]
		&& player->mo->state <= &states[PStateAttackEnd[player->class]])
      P_SetMobjState(player->mo, PStateNormal[player->class]);
#else
   /* get out of attack state */
   if (player->mo->state == &states[g_s_play_atk1]
         || player->mo->state == &states[g_s_play_atk2] )
      P_SetMobjState(player->mo, g_s_play);
#endif

   if (player->readyweapon == WP_CHAINSAW && psp->state == &states[S_SAW])
      S_StartSound(player->mo, sfx_sawidl);

   // check for change
   //  if player is dead, put the weapon away

   if (player->pendingweapon != WP_NOCHANGE || !player->health)
   {
      // change weapon (pending weapon should already be validated)
      statenum_t newstate = weaponinfo[player->readyweapon].downstate;
      P_SetPsprite(player, ps_weapon, newstate);
      return;
   }

   // check for fire
   //  the missile launcher and bfg do not auto fire

   if (player->cmd.buttons & BT_ATTACK)
   {
#ifndef HEXEN
      /* MBF21: a weapon flagged WPF_NOAUTOFIRE won't refire while the
       * button is held.  Below complevel 21, keep the hardcoded
       * missile-launcher / BFG behaviour exactly. */
      dbool noautofire = mbf21_features
        ? (weaponinfo[player->readyweapon].flags & WPF_NOAUTOFIRE) != 0
        : (player->readyweapon == WP_MISSILE || player->readyweapon == WP_BFG);
      if (!player->attackdown || !noautofire)
#endif
      {
         player->attackdown = true;
         P_FireWeapon(player);
         return;
      }
   }
   else
      player->attackdown = false;

#ifdef HEXEN
   if(!player->morphTics)
#endif
   {
      /* bob the weapon based on movement speed */
      int angle = (128*leveltime) & FINEMASK;
      psp->sx = FRACUNIT + FixedMul(player->bob, finecosine[angle]);
      angle &= FINEANGLES/2-1;
      psp->sy = WEAPONTOP + FixedMul(player->bob, finesine[angle]);
   }
}

/*
 * A_ReFire
 * The player can re-fire the weapon
 * without lowering it entirely.
*/

void A_ReFire(player_t *player, pspdef_t *psp)
{
  /* check for fire
   *  (if a weaponchange is pending, let it go through instead) */

  if ( (player->cmd.buttons & BT_ATTACK)
       && player->pendingweapon == WP_NOCHANGE && player->health)
    {
      player->refire++;
      P_FireWeapon(player);
    }
  else
    {
      player->refire = 0;
#ifdef HEXEN
      P_CheckMana(player);
#else
      P_CheckAmmo(player);
#endif
    }
}

void A_CheckReload(player_t *player, pspdef_t *psp)
{
  if (!P_CheckAmmo(player) && compatibility_level >= prboom_4_compatibility) {
    /* cph 2002/08/08 - In old Doom, P_CheckAmmo would start the weapon lowering
     * immediately. This was lost in Boom when the weapon switching logic was
     * rewritten. But we must tell Doom that we don't need to complete the
     * reload frames for the weapon here. G_BuildTiccmd will set ->pendingweapon
     * for us later on. */
    P_SetPsprite(player,ps_weapon,weaponinfo[player->readyweapon].downstate);
  }
}

/*
 * A_Lower
 * Lowers current weapon,
 *  and changes weapon at bottom.
*/

void A_Lower(player_t *player, pspdef_t *psp)
{
#ifdef HEXEN
   if(player->morphTics)
      psp->sy = WEAPONBOTTOM;
   else
#endif
      psp->sy += LOWERSPEED;

   // Is already down.
   if (psp->sy < WEAPONBOTTOM)
      return;

   /* Player is dead. */
   if (player->playerstate == PST_DEAD)
   {
      /* Player is dead, so don't bring up a pending weapon. */
      psp->sy = WEAPONBOTTOM;
      return;
   }

   /* The old weapon has been lowered off the screen,
    * so change the weapon and start raising it */

   if (!player->health)
   {
      /* Player is dead, so keep the weapon off screen. */
      P_SetPsprite(player,  ps_weapon, S_NULL);
      return;
   }

   player->readyweapon = player->pendingweapon;

   P_BringUpWeapon(player);
}

/*
 * A_Raise
*/

void A_Raise(player_t *player, pspdef_t *psp)
{
  statenum_t newstate;

  psp->sy -= RAISESPEED;

  if (psp->sy > WEAPONTOP) /* Not raised all the way yet */
    return;

  psp->sy = WEAPONTOP;

#ifdef HEXEN
  if(player->class == PCLASS_FIGHTER && player->readyweapon == WP_SECOND
        && player->mana[MANA_1])
     P_SetPsprite(player, ps_weapon, S_FAXEREADY_G);
  else
     P_SetPsprite(player, ps_weapon,
           WeaponInfo[player->readyweapon][player->class].readystate);
#else
  /* The weapon has been raised all the way,
   *  so change to the ready state. */

  newstate = weaponinfo[player->readyweapon].readystate;

  P_SetPsprite(player, ps_weapon, newstate);
#endif

}


// Weapons now recoil, amount depending on the weapon.              // phares
//                                                                  //   |
// The P_SetPsprite call in each of the weapon firing routines      //   V
// was moved here so the recoil could be synched with the
// muzzle flash, rather than the pressing of the trigger.
// The BFG delay caused this to be necessary.

static void A_FireSomething(player_t* player,int adder)
{
  P_SetPsprite(player, ps_flash,
               weaponinfo[player->readyweapon].flashstate+adder);

  // killough 3/27/98: prevent recoil in no-clipping mode
  if (!(player->mo->flags & MF_NOCLIP))
    if (!compatibility && weapon_recoil)
      P_Thrust(player,
               ANG180+player->mo->angle,                          //   ^
               2048*recoil_values[player->readyweapon]);          //   |
}                                                                 // phares

//
// A_GunFlash
//

void A_GunFlash(player_t *player, pspdef_t *psp)
{
  P_SetMobjState(player->mo, S_PLAY_ATK2);

  A_FireSomething(player,0);                                      // phares
}

//
// WEAPON ATTACKS
//

//
// A_Punch
//

void A_Punch(player_t *player, pspdef_t *psp)
{
  angle_t angle;
  int t, slope, damage = (P_Random(pr_punch)%10+1)<<1;

  if (player->powers[pw_strength])
    damage *= 10;

  angle = player->mo->angle;

  // killough 5/5/98: remove dependence on order of evaluation:
  t = P_Random(pr_punchangle);
  angle += (t - P_Random(pr_punchangle))<<18;

  /* killough 8/2/98: make autoaiming prefer enemies */
  if (!mbf_features ||
      (slope = P_AimLineAttack(player->mo, angle, MELEERANGE, MF_FRIEND),
       !linetarget))
    slope = P_AimLineAttack(player->mo, angle, MELEERANGE, 0);

  P_LineAttack(player->mo, angle, MELEERANGE, slope, damage);

  if (!linetarget)
    return;

  S_StartSound(player->mo, sfx_punch);

  // turn to face target

  player->mo->angle = R_PointToAngle2(player->mo->x, player->mo->y,
                                      linetarget->x, linetarget->y);
  R_SmoothPlaying_Reset(player); // e6y

  retro_set_rumble_damage(30, 120.0f);
}

//
// A_Saw
//

void A_Saw(player_t *player, pspdef_t *psp)
{
  int slope, damage = 2*(P_Random(pr_saw)%10+1);
  angle_t angle = player->mo->angle;
  // killough 5/5/98: remove dependence on order of evaluation:
  int t = P_Random(pr_saw);
  angle += (t - P_Random(pr_saw))<<18;

  /* Use meleerange + 1 so that the puff doesn't skip the flash
   * killough 8/2/98: make autoaiming prefer enemies */
  if (!mbf_features ||
      (slope = P_AimLineAttack(player->mo, angle, MELEERANGE+1, MF_FRIEND),
       !linetarget))
    slope = P_AimLineAttack(player->mo, angle, MELEERANGE+1, 0);

  P_LineAttack(player->mo, angle, MELEERANGE+1, slope, damage);

  if (!linetarget)
    {
      S_StartSound(player->mo, sfx_sawful);
      return;
    }

  S_StartSound(player->mo, sfx_sawhit);

  // turn to face target
  angle = R_PointToAngle2(player->mo->x, player->mo->y,
                          linetarget->x, linetarget->y);

  if (angle - player->mo->angle > ANG180) {
    if (angle - player->mo->angle < ((angle_t) -ANG90/20))
      player->mo->angle = angle + ANG90/21;
    else
      player->mo->angle -= ANG90/20;
  } else {
    if (angle - player->mo->angle > ANG90/20)
      player->mo->angle = angle - ANG90/21;
    else
      player->mo->angle += ANG90/20;
  }

  player->mo->flags |= MF_JUSTATTACKED;
  R_SmoothPlaying_Reset(player); // e6y

  retro_set_rumble_damage(40, 120.0f);
}

//
// A_FireMissile
//

/* MBF21: subtract ammo for a native weapon attack.  When the ready weapon
 * has an explicit Ammo per shot (>= 0), that amount is used; otherwise the
 * vanilla amount passed by the caller.  ammopershot is -1 for every weapon
 * unless a deh patch set it, so vanilla consumption is unchanged. */
static void P_SubtractAmmo(player_t *player, int vanilla_amount)
{
  ammotype_t type = weaponinfo[player->readyweapon].ammo;
  int amount = vanilla_amount;
  if (mbf21_features && weaponinfo[player->readyweapon].ammopershot >= 0)
    amount = weaponinfo[player->readyweapon].ammopershot;
  if (type != AM_NOAMMO)
    player->ammo[type] -= amount;
}

void A_FireMissile(player_t *player, pspdef_t *psp)
{
  P_SubtractAmmo(player, 1);
  P_SpawnPlayerMissile(player->mo, MT_ROCKET);
  retro_set_rumble_damage(50, 120.0f);
}

//
// A_FireBFG
//

void A_FireBFG(player_t *player, pspdef_t *psp)
{
  P_SubtractAmmo(player, BFGCELLS);
  P_SpawnPlayerMissile(player->mo, MT_BFG);
  retro_set_rumble_damage(50, 120.0f);
}

//
// A_FireOldBFG
//
// This function emulates Doom's Pre-Beta BFG
// By Lee Killough 6/6/98, 7/11/98, 7/19/98, 8/20/98
//
// This code may not be used in other mods without appropriate credit given.
// Code leeches will be telefragged.

int autoaim = 0;  // killough 7/19/98: autoaiming was not in original beta
void A_FireOldBFG(player_t *player, pspdef_t *psp)
{
  int type = MT_PLASMA1;

  if (compatibility_level < mbf_compatibility)
    return;

  if (weapon_recoil && !(player->mo->flags & MF_NOCLIP))
    P_Thrust(player, ANG180 + player->mo->angle,
    512*recoil_values[WP_PLASMA]);

  P_SubtractAmmo(player, 1);

  player->extralight = 2;

  do
  {
    mobj_t *th, *mo = player->mo;
    angle_t an = mo->angle;
    angle_t an1 = ((P_Random(pr_bfg)&127) - 64) * (ANG90/768) + an;
    angle_t an2 = ((P_Random(pr_bfg)&127) - 64) * (ANG90/640) + ANG90;
    extern int autoaim;

    if (autoaim/* || !beta_emulation*/)
    {
      // killough 8/2/98: make autoaiming prefer enemies
      uint64_t mask = mbf_features ? MF_FRIEND : 0;
      fixed_t slope;
      do
      {
        slope = P_AimLineAttack(mo, an, 16*64*FRACUNIT, mask);
        if (!linetarget)
          slope = P_AimLineAttack(mo, an += 1<<26, 16*64*FRACUNIT, mask);
        if (!linetarget)
          slope = P_AimLineAttack(mo, an -= 2<<26, 16*64*FRACUNIT, mask);
        if (!linetarget)
          slope = 0, an = mo->angle;
      }
      while (mask && (mask=0, !linetarget));     // killough 8/2/98
      an1 += an - mo->angle;
      an2 += tantoangle[slope >> DBITS];
    }

    th = P_SpawnMobj(mo->x, mo->y, mo->z + 62*FRACUNIT - player->psprites[ps_weapon].sy, type);
    P_SetTarget(&th->target, mo);
    th->angle = an1;
    th->momx = finecosine[an1>>ANGLETOFINESHIFT] * 25;
    th->momy = finesine[an1>>ANGLETOFINESHIFT] * 25;
    th->momz = finetangent[an2>>ANGLETOFINESHIFT] * 25;
    P_CheckMissileSpawn(th);
  }
  while ((type != MT_PLASMA2) && (type = MT_PLASMA2)); //killough: obfuscated! // lgtm[cpp/assign-where-compare-meant]
  retro_set_rumble_damage(50, 120.0f);
}

//
// A_FirePlasma
//

void A_FirePlasma(player_t *player, pspdef_t *psp)
{
  P_SubtractAmmo(player, 1);

  A_FireSomething(player,P_Random(pr_plasma)&1);              // phares
  P_SpawnPlayerMissile(player->mo, MT_PLASMA);
  retro_set_rumble_damage(50, 120.0f);
}

//
// P_BulletSlope
// Sets a slope so a near miss is at aproximately
// the height of the intended target
//

static fixed_t bulletslope;

static void P_BulletSlope(mobj_t *mo)
{
   angle_t an = mo->angle;    // see which target is to be aimed at

   /* killough 8/2/98: make autoaiming prefer enemies */
   uint64_t mask = mbf_features ? MF_FRIEND : 0;

   do
   {
      bulletslope = P_AimLineAttack(mo, an, 16*64*FRACUNIT, mask);
      if (!linetarget)
         bulletslope = P_AimLineAttack(mo, an += 1<<26, 16*64*FRACUNIT, mask);
      if (!linetarget)
         bulletslope = P_AimLineAttack(mo, an -= 2<<26, 16*64*FRACUNIT, mask);
   }
   while (mask && (mask=0, !linetarget));  /* killough 8/2/98 */
}

//
// P_GunShot
//

static void P_GunShot(mobj_t *mo, dbool accurate)
{
  int damage = 5*(P_Random(pr_gunshot)%3+1);
  angle_t angle = mo->angle;

  if (!accurate)
    {  // killough 5/5/98: remove dependence on order of evaluation:
      int t = P_Random(pr_misfire);
      angle += (t - P_Random(pr_misfire))<<18;
    }

  P_LineAttack(mo, angle, MISSILERANGE, bulletslope, damage);
}

//
// A_FirePistol
//

void A_FirePistol(player_t *player, pspdef_t *psp)
{
  S_StartSound(player->mo, sfx_pistol);

  P_SetMobjState(player->mo, S_PLAY_ATK2);
  P_SubtractAmmo(player, 1);

  A_FireSomething(player,0);                                      // phares
  P_BulletSlope(player->mo);
  P_GunShot(player->mo, !player->refire);

  retro_set_rumble_damage(30, 120.0f);
}

//
// A_FireShotgun
//

void A_FireShotgun(player_t *player, pspdef_t *psp)
{
  int i;

  S_StartSound(player->mo, sfx_shotgn);
  P_SetMobjState(player->mo, S_PLAY_ATK2);

  P_SubtractAmmo(player, 1);

  A_FireSomething(player,0);                                      // phares

  P_BulletSlope(player->mo);

  for (i=0; i<7; i++)
    P_GunShot(player->mo, false);

  retro_set_rumble_damage(40, 120.0f);
}

//
// A_FireShotgun2
//

void A_FireShotgun2(player_t *player, pspdef_t *psp)
{
  int i;

  S_StartSound(player->mo, sfx_dshtgn);
  P_SetMobjState(player->mo, S_PLAY_ATK2);
  P_SubtractAmmo(player, 2);

  A_FireSomething(player,0);                                      // phares

  P_BulletSlope(player->mo);

  for (i=0; i<20; i++)
    {
      int damage = 5*(P_Random(pr_shotgun)%3+1);
      angle_t angle = player->mo->angle;
      // killough 5/5/98: remove dependence on order of evaluation:
      int t = P_Random(pr_shotgun);
      angle += (t - P_Random(pr_shotgun))<<19;
      t = P_Random(pr_shotgun);
      P_LineAttack(player->mo, angle, MISSILERANGE, bulletslope +
                   ((t - P_Random(pr_shotgun))<<5), damage);
    }

  retro_set_rumble_damage(40, 120.0f);
}

//
// A_FireCGun
//

void A_FireCGun(player_t *player, pspdef_t *psp)
{
  if (player->ammo[weaponinfo[player->readyweapon].ammo] || comp[comp_sound])
    S_StartSound(player->mo, sfx_pistol);

  if (!player->ammo[weaponinfo[player->readyweapon].ammo])
    return;

  P_SetMobjState(player->mo, S_PLAY_ATK2);
  P_SubtractAmmo(player, 1);

  A_FireSomething(player,psp->state - &states[S_CHAIN1]);           // phares

  P_BulletSlope(player->mo);

  P_GunShot(player->mo, !player->refire);

  retro_set_rumble_damage(10, 120.0f);
}

/* ------------------------------------------------------------------------
 * Hexen Fighter fists (active port).
 *
 * Ported from the dormant vanilla-Hexen weapon block below and adapted to
 * this core's API: P_Random takes a pr_class, P_AimLineAttack takes a mask
 * argument, mobj special1 is the specialval_t union (.i), and the custom
 * MT_PUNCHPUFF/MT_HAMMERPUFF puff types are dropped (P_LineAttack here spawns
 * the engine's fixed puff -- cosmetic only; the hit/damage is unaffected).
 * This is the fists-first slice of the Fighter weapon set; the axe/hammer/
 * sword come later.
 * --------------------------------------------------------------------- */

#define HX_MAX_ANGLE_ADJUST (5*ANG1)

/* P_ThrustMobj is defined in p_mobj.c; its only header prototype lives in
 * heretic/p_action.h, which p_pspr.c includes far below this point.  Declare
 * it locally so the fists code above that include sees the correct type. */
void P_ThrustMobj(mobj_t *mo, angle_t angle, fixed_t move);

static void AdjustPlayerAngle(mobj_t *pmo)
{
  angle_t angle;
  int     difference;

  angle = R_PointToAngle2(pmo->x, pmo->y, linetarget->x, linetarget->y);
  difference = (int)angle - (int)pmo->angle;
  if (abs(difference) > HX_MAX_ANGLE_ADJUST)
    pmo->angle += difference > 0 ? HX_MAX_ANGLE_ADJUST : -HX_MAX_ANGLE_ADJUST;
  else
    pmo->angle = angle;
}

void A_FPunchAttack(player_t *player, pspdef_t *psp)
{
  angle_t angle;
  int     damage;
  fixed_t slope;
  mobj_t *pmo = player->mo;
  fixed_t power;
  int     i;

  (void)psp;
  damage = 40 + (P_Random(pr_punch) & 15);
  power  = 2 * FRACUNIT;
  PuffType = HEXEN_MT_PUNCHPUFF;
  for (i = 0; i < 16; i++)
  {
    angle = pmo->angle + i * (ANG45 / 16);
    slope = P_AimLineAttack(pmo, angle, 2 * MELEERANGE, 0);
    if (linetarget)
    {
      pmo->special1.i++;
      if (pmo->special1.i == 3)
      {
        damage <<= 1;
        power = 6 * FRACUNIT;
        PuffType = HEXEN_MT_HAMMERPUFF;
      }
      P_LineAttack(pmo, angle, 2 * MELEERANGE, slope, damage);
      if (linetarget->flags & MF_COUNTKILL || linetarget->player)
        P_ThrustMobj(linetarget, angle, power);
      AdjustPlayerAngle(pmo);
      goto punchdone;
    }
    angle = pmo->angle - i * (ANG45 / 16);
    slope = P_AimLineAttack(pmo, angle, 2 * MELEERANGE, 0);
    if (linetarget)
    {
      pmo->special1.i++;
      if (pmo->special1.i == 3)
      {
        damage <<= 1;
        power = 6 * FRACUNIT;
        PuffType = HEXEN_MT_HAMMERPUFF;
      }
      P_LineAttack(pmo, angle, 2 * MELEERANGE, slope, damage);
      if (linetarget->flags & MF_COUNTKILL || linetarget->player)
        P_ThrustMobj(linetarget, angle, power);
      AdjustPlayerAngle(pmo);
      goto punchdone;
    }
  }
  /* didn't find any creatures, so try to strike any walls */
  pmo->special1.i = 0;

  angle = pmo->angle;
  slope = P_AimLineAttack(pmo, angle, MELEERANGE, 0);
  P_LineAttack(pmo, angle, MELEERANGE, slope, damage);

punchdone:
  if (pmo->special1.i == 3)
  {
    pmo->special1.i = 0;
    P_SetPsprite(player, ps_weapon, HEXEN_S_PUNCHATK2_1);
    S_StartSound(pmo, hexen_sfx_fighter_grunt);
  }
}

#define HX_AXERANGE (9*MELEERANGE/4)   /* 2.25 * MELEERANGE */

void A_FAxeAttack(player_t *player, pspdef_t *psp)
{
  angle_t angle;
  mobj_t *pmo = player->mo;
  fixed_t power;
  int     damage;
  fixed_t slope;
  int     i;
  int     useMana;

  (void)psp;
  damage = 40 + (P_Random(pr_saw) & 15) + (P_Random(pr_saw) & 7);
  power = 0;
  if (player->mana[MANA_1] > 0)
  {
    /* powered (glowing) axe: double damage, knockback, spends mana */
    damage <<= 1;
    power = 6 * FRACUNIT;
    PuffType = HEXEN_MT_AXEPUFF_GLOW;
    useMana = 1;
  }
  else
  {
    PuffType = HEXEN_MT_AXEPUFF;
    useMana = 0;
  }

  for (i = 0; i < 16; i++)
  {
    angle = pmo->angle + i * (ANG45 / 16);
    slope = P_AimLineAttack(pmo, angle, HX_AXERANGE, 0);
    if (linetarget)
    {
      P_LineAttack(pmo, angle, HX_AXERANGE, slope, damage);
      if (linetarget->flags & MF_COUNTKILL || linetarget->player)
        P_ThrustMobj(linetarget, angle, power);
      AdjustPlayerAngle(pmo);
      useMana++;
      goto axedone;
    }
    angle = pmo->angle - i * (ANG45 / 16);
    slope = P_AimLineAttack(pmo, angle, HX_AXERANGE, 0);
    if (linetarget)
    {
      P_LineAttack(pmo, angle, HX_AXERANGE, slope, damage);
      if (linetarget->flags & MF_COUNTKILL)
        P_ThrustMobj(linetarget, angle, power);
      AdjustPlayerAngle(pmo);
      useMana++;
      goto axedone;
    }
  }
  /* didn't find any creatures, so try to strike any walls */
  pmo->special1.i = 0;

  angle = pmo->angle;
  slope = P_AimLineAttack(pmo, angle, MELEERANGE, 0);
  P_LineAttack(pmo, angle, MELEERANGE, slope, damage);

axedone:
  if (useMana == 2)
  {
    player->mana[MANA_1] -= WeaponManaUse[player->class][player->readyweapon];
    if (player->mana[MANA_1] <= 0)
      P_SetPsprite(player, ps_weapon, HEXEN_S_FAXEATK_5);
  }
}

#define HX_HAMMER_RANGE (MELEERANGE+MELEERANGE/2)

/* Class-boss weapon secondaries: the Fighter/Cleric/Mage bosses fire the
 * fourth-weapon attacks as monster (mobj-level) actions. */

void A_FSwordAttack2(mobj_t *actor)
{
  angle_t angle = actor->angle;

  P_SpawnMissileAngle(actor, HEXEN_MT_FSWORD_MISSILE, angle + ANG45 / 4, 0);
  P_SpawnMissileAngle(actor, HEXEN_MT_FSWORD_MISSILE, angle + ANG45 / 8, 0);
  P_SpawnMissileAngle(actor, HEXEN_MT_FSWORD_MISSILE, angle, 0);
  P_SpawnMissileAngle(actor, HEXEN_MT_FSWORD_MISSILE, angle - ANG45 / 8, 0);
  P_SpawnMissileAngle(actor, HEXEN_MT_FSWORD_MISSILE, angle - ANG45 / 4, 0);
  S_StartSound(actor, hexen_sfx_fighter_sword_fire);
}

static void MStaffSpawn2(mobj_t *actor, angle_t angle)
{
  mobj_t *mo;

  mo = P_SpawnMissileAngle(actor, HEXEN_MT_MSTAFF_FX2, angle, 0);
  if (mo)
  {
    P_SetTarget(&mo->target, actor);
    P_SetTarget(&mo->special1.m, P_RoughTargetSearch(mo, 0, 10));
  }
}

void A_MStaffAttack2(mobj_t *actor)
{
  angle_t angle;

  angle = actor->angle;
  MStaffSpawn2(actor, angle);
  MStaffSpawn2(actor, angle - ANG1 * 5);
  MStaffSpawn2(actor, angle + ANG1 * 5);
  S_StartSound(actor, hexen_sfx_mage_staff_fire);
}

void A_CHolyAttack3(mobj_t *actor)
{
  P_SpawnMissile(actor, actor->target, HEXEN_MT_HOLY_MISSILE);
  S_StartSound(actor, hexen_sfx_choly_fire);
}

void A_SnoutAttack(player_t *player, pspdef_t *psp)
{
  angle_t angle;
  int damage;
  int slope;

  damage = 3 + (P_Random(pr_heretic) & 3);
  angle = player->mo->angle;
  slope = P_AimLineAttack(player->mo, angle, MELEERANGE, 0);
  PuffType = HEXEN_MT_SNOUTPUFF;
  PuffSpawned = NULL;
  P_LineAttack(player->mo, angle, MELEERANGE, slope, damage);
  S_StartSound(player->mo,
               hexen_sfx_pig_active1 + (P_Random(pr_heretic) & 1));
  if (linetarget)
  {
    AdjustPlayerAngle(player->mo);
    if (PuffSpawned)
    {                                  /* bit something */
      S_StartSound(player->mo, hexen_sfx_pig_attack);
    }
  }
}

void A_FHammerAttack(player_t *player, pspdef_t *psp)
{
  angle_t angle;
  mobj_t *pmo = player->mo;
  int     damage;
  fixed_t power;
  fixed_t slope;
  int     i;

  (void)psp;
  damage = 60 + (P_Random(pr_saw) & 63);
  power = 10 * FRACUNIT;
  PuffType = HEXEN_MT_HAMMERPUFF;
  for (i = 0; i < 16; i++)
  {
    angle = pmo->angle + i * (ANG45 / 32);
    slope = P_AimLineAttack(pmo, angle, HX_HAMMER_RANGE, 0);
    if (linetarget)
    {
      P_LineAttack(pmo, angle, HX_HAMMER_RANGE, slope, damage);
      AdjustPlayerAngle(pmo);
      if (linetarget->flags & MF_COUNTKILL || linetarget->player)
        P_ThrustMobj(linetarget, angle, power);
      pmo->special1.i = false; /* don't throw a hammer */
      goto hammerdone;
    }
    angle = pmo->angle - i * (ANG45 / 32);
    slope = P_AimLineAttack(pmo, angle, HX_HAMMER_RANGE, 0);
    if (linetarget)
    {
      P_LineAttack(pmo, angle, HX_HAMMER_RANGE, slope, damage);
      AdjustPlayerAngle(pmo);
      if (linetarget->flags & MF_COUNTKILL || linetarget->player)
        P_ThrustMobj(linetarget, angle, power);
      pmo->special1.i = false; /* don't throw a hammer */
      goto hammerdone;
    }
  }
  /* didn't find any targets in melee range: still swing, then throw a hammer.
   * (Vanilla suppressed the throw when the swing struck a nearby wall, using
   * the spawned puff as the signal; this core's P_LineAttack does not expose
   * that, so we always throw when no creature was hit -- a minor difference
   * only in the point-blank-against-a-wall case.) */
  angle = pmo->angle;
  slope = P_AimLineAttack(pmo, angle, HX_HAMMER_RANGE, 0);
  P_LineAttack(pmo, angle, HX_HAMMER_RANGE, slope, damage);
  pmo->special1.i = true;

hammerdone:
  /* don't spawn a hammer if the player can't afford the mana */
  if (player->mana[MANA_2] < WeaponManaUse[player->class][player->readyweapon])
    pmo->special1.i = false;
}

void A_FHammerThrow(player_t *player, pspdef_t *psp)
{
  mobj_t *mo;

  (void)psp;
  if (!player->mo->special1.i)
    return;
  player->mana[MANA_2] -= WeaponManaUse[player->class][player->readyweapon];
  mo = P_SpawnPlayerMissile(player->mo, HEXEN_MT_HAMMER_MISSILE);
  if (mo)
    mo->special1.i = 0;
}

void A_FSwordAttack(player_t *player, pspdef_t *psp)
{
  mobj_t *pmo;

  (void)psp;
  player->mana[MANA_1] -= WeaponManaUse[player->class][player->readyweapon];
  player->mana[MANA_2] -= WeaponManaUse[player->class][player->readyweapon];
  pmo = player->mo;
  P_SPMAngleXYZ(pmo, pmo->x, pmo->y, pmo->z - 10*FRACUNIT,
                HEXEN_MT_FSWORD_MISSILE, pmo->angle + ANG45/4);
  P_SPMAngleXYZ(pmo, pmo->x, pmo->y, pmo->z - 5*FRACUNIT,
                HEXEN_MT_FSWORD_MISSILE, pmo->angle + ANG45/8);
  P_SPMAngleXYZ(pmo, pmo->x, pmo->y, pmo->z,
                HEXEN_MT_FSWORD_MISSILE, pmo->angle);
  P_SPMAngleXYZ(pmo, pmo->x, pmo->y, pmo->z + 5*FRACUNIT,
                HEXEN_MT_FSWORD_MISSILE, pmo->angle - ANG45/8);
  P_SPMAngleXYZ(pmo, pmo->x, pmo->y, pmo->z + 10*FRACUNIT,
                HEXEN_MT_FSWORD_MISSILE, pmo->angle - ANG45/4);
  S_StartSound(pmo, hexen_sfx_fighter_sword_fire);
}

/* Actor codepointer (runs on the FSwordFlame missile, not a psprite). */
void A_FSwordFlames(mobj_t *actor)
{
  int i;

  for (i = 1 + (P_Random(pr_saw) & 3); i; i--)
    P_SpawnMobj(actor->x + ((P_Random(pr_saw) - 128) << 12),
                actor->y + ((P_Random(pr_saw) - 128) << 12),
                actor->z + ((P_Random(pr_saw) - 128) << 11),
                HEXEN_MT_FSWORD_FLAME);
}

/* --------------------------------------------------------------------------
 * Cleric weapons
 * ------------------------------------------------------------------------ */

/* The Mace (Cleric WP_FIRST): a wide melee sweep.  Fans out +/- across a
 * 45-degree arc looking for a target, striking the first found; otherwise
 * a straight melee swing at any wall. */
void A_CMaceAttack(player_t *player, pspdef_t *psp)
{
  angle_t angle;
  int     damage;
  fixed_t slope;
  mobj_t *pmo = player->mo;
  int     i;

  (void)psp;
  damage = 25 + (P_Random(pr_punch) & 15);
  PuffType = HEXEN_MT_HAMMERPUFF;
  for (i = 0; i < 16; i++)
  {
    angle = pmo->angle + i * (ANG45 / 16);
    slope = P_AimLineAttack(pmo, angle, 2 * MELEERANGE, 0);
    if (linetarget)
    {
      P_LineAttack(pmo, angle, 2 * MELEERANGE, slope, damage);
      AdjustPlayerAngle(pmo);
      return;
    }
    angle = pmo->angle - i * (ANG45 / 16);
    slope = P_AimLineAttack(pmo, angle, 2 * MELEERANGE, 0);
    if (linetarget)
    {
      P_LineAttack(pmo, angle, 2 * MELEERANGE, slope, damage);
      AdjustPlayerAngle(pmo);
      return;
    }
  }
  /* no creatures found; strike whatever wall is ahead */
  pmo->special1.i = 0;
  angle = pmo->angle;
  slope = P_AimLineAttack(pmo, angle, MELEERANGE, 0);
  P_LineAttack(pmo, angle, MELEERANGE, slope, damage);
}

/* The Serpent Staff (Cleric WP_SECOND): a life-draining melee check.  On a
 * hit it heals the Cleric a little and switches to the drain animation;
 * A_CStaffAttack then fires the twin slithering missiles.  The ready state
 * periodically blinks the staff's eyes (Init/CheckBlink). */
void A_CStaffCheck(player_t *player, pspdef_t *psp)
{
  mobj_t *pmo;
  int     damage;
  int     newLife;
  angle_t angle;
  fixed_t slope;
  int     i;

  (void)psp;
  pmo = player->mo;
  damage = 20 + (P_Random(pr_punch) & 15);
  PuffType = HEXEN_MT_CSTAFFPUFF;
  for (i = 0; i < 3; i++)
  {
    angle = pmo->angle + i * (ANG45 / 16);
    slope = P_AimLineAttack(pmo, angle, (3 * MELEERANGE) / 2, 0);
    if (linetarget)
    {
      P_LineAttack(pmo, angle, (3 * MELEERANGE) / 2, slope, damage);
      pmo->angle = R_PointToAngle2(pmo->x, pmo->y,
                                   linetarget->x, linetarget->y);
      if ((linetarget->player || (linetarget->flags & MF_COUNTKILL))
          && !(linetarget->flags2 & (MF2_DORMANT | MF2_INVULNERABLE)))
      {
        newLife = player->health + (damage >> 3);
        newLife = newLife > 100 ? 100 : newLife;
        pmo->health = player->health = newLife;
        P_SetPsprite(player, ps_weapon, HEXEN_S_CSTAFFATK2_1);
      }
      player->mana[MANA_1] -= WeaponManaUse[player->class][player->readyweapon];
      break;
    }
    angle = pmo->angle - i * (ANG45 / 16);
    slope = P_AimLineAttack(pmo, angle, (3 * MELEERANGE) / 2, 0);
    if (linetarget)
    {
      P_LineAttack(pmo, angle, (3 * MELEERANGE) / 2, slope, damage);
      pmo->angle = R_PointToAngle2(pmo->x, pmo->y,
                                   linetarget->x, linetarget->y);
      if (linetarget->player || (linetarget->flags & MF_COUNTKILL))
      {
        newLife = player->health + (damage >> 4);
        newLife = newLife > 100 ? 100 : newLife;
        pmo->health = player->health = newLife;
        P_SetPsprite(player, ps_weapon, HEXEN_S_CSTAFFATK2_1);
      }
      player->mana[MANA_1] -= WeaponManaUse[player->class][player->readyweapon];
      break;
    }
  }
  R_SmoothPlaying_Reset(player);
}

void A_CStaffAttack(player_t *player, pspdef_t *psp)
{
  mobj_t *mo;
  mobj_t *pmo;

  (void)psp;
  player->mana[MANA_1] -= WeaponManaUse[player->class][player->readyweapon];
  pmo = player->mo;
  mo = P_SPMAngle(pmo, HEXEN_MT_CSTAFF_MISSILE, pmo->angle - (ANG45 / 15));
  if (mo)
    mo->special2.i = 32;
  mo = P_SPMAngle(pmo, HEXEN_MT_CSTAFF_MISSILE, pmo->angle + (ANG45 / 15));
  if (mo)
    mo->special2.i = 0;
  S_StartSound(player->mo, hexen_sfx_cleric_cstaff_fire);
}

void A_CStaffMissileSlither(mobj_t *actor)
{
  fixed_t newX, newY;
  int     weaveXY;
  int     angle;

  weaveXY = actor->special2.i;
  angle = (actor->angle + ANG90) >> ANGLETOFINESHIFT;
  newX = actor->x - FixedMul(finecosine[angle], FloatBobOffsets[weaveXY]);
  newY = actor->y - FixedMul(finesine[angle], FloatBobOffsets[weaveXY]);
  weaveXY = (weaveXY + 3) & 63;
  newX += FixedMul(finecosine[angle], FloatBobOffsets[weaveXY]);
  newY += FixedMul(finesine[angle], FloatBobOffsets[weaveXY]);
  P_TryMove(actor, newX, newY, 0);
  actor->special2.i = weaveXY;
}

void A_CStaffInitBlink(player_t *player, pspdef_t *psp)
{
  (void)psp;
  player->mo->special1.i = (P_Random(pr_punch) >> 1) + 20;
}

void A_CStaffCheckBlink(player_t *player, pspdef_t *psp)
{
  (void)psp;
  if (!--player->mo->special1.i)
  {
    P_SetPsprite(player, ps_weapon, HEXEN_S_CSTAFFBLINK1);
    player->mo->special1.i = (P_Random(pr_punch) + 50) >> 2;
  }
}

/* Flame Strike (Cleric WP_THIRD): launches a flame missile that, on hitting
 * a creature, rings it with a circle of rotating flames. */
#define FLAMESPEED    29491        /* 0.45 * FRACUNIT */
#define FLAMEROTSPEED (2 * FRACUNIT)

void A_CFlameAttack(player_t *player, pspdef_t *psp)
{
  mobj_t *mo;

  (void)psp;
  mo = P_SpawnPlayerMissile(player->mo, HEXEN_MT_CFLAME_MISSILE);
  if (mo)
  {
    mo->thinker.function.arg1 = (arg1_t)P_BlasterMobjThinker;
    mo->special1.i = 2;
  }
  player->mana[MANA_2] -= WeaponManaUse[player->class][player->readyweapon];
  S_StartSound(player->mo, hexen_sfx_cleric_flame_fire);
}

void A_CFlameMissile(mobj_t *actor)
{
  int     i;
  int     an;
  fixed_t dist;
  mobj_t *mo;

  A_UnHideThing(actor);
  S_StartSound(actor, hexen_sfx_cleric_flame_explode);
  if (BlockingMobj && (BlockingMobj->flags & MF_SHOOTABLE))
  {
    /* hit a creature: ring it with rotating flames */
    dist = BlockingMobj->radius + 18 * FRACUNIT;
    for (i = 0; i < 4; i++)
    {
      an = (i * ANG45) >> ANGLETOFINESHIFT;
      mo = P_SpawnMobj(BlockingMobj->x + FixedMul(dist, finecosine[an]),
                       BlockingMobj->y + FixedMul(dist, finesine[an]),
                       BlockingMobj->z + 5 * FRACUNIT, HEXEN_MT_CIRCLEFLAME);
      if (mo)
      {
        mo->angle = an << ANGLETOFINESHIFT;
        P_SetTarget(&mo->target, actor->target);
        mo->momx = mo->special1.i = FixedMul(FLAMESPEED, finecosine[an]);
        mo->momy = mo->special2.i = FixedMul(FLAMESPEED, finesine[an]);
        mo->tics -= P_Random(pr_heretic) & 3;
      }
      mo = P_SpawnMobj(BlockingMobj->x - FixedMul(dist, finecosine[an]),
                       BlockingMobj->y - FixedMul(dist, finesine[an]),
                       BlockingMobj->z + 5 * FRACUNIT, HEXEN_MT_CIRCLEFLAME);
      if (mo)
      {
        mo->angle = ANG180 + (an << ANGLETOFINESHIFT);
        P_SetTarget(&mo->target, actor->target);
        mo->momx = mo->special1.i = FixedMul(-FLAMESPEED, finecosine[an]);
        mo->momy = mo->special2.i = FixedMul(-FLAMESPEED, finesine[an]);
        mo->tics -= P_Random(pr_heretic) & 3;
      }
    }
    P_SetMobjState(actor, HEXEN_S_FLAMEPUFF2_1);
  }
}

void A_CFlamePuff(mobj_t *actor)
{
  A_UnHideThing(actor);
  actor->momx = 0;
  actor->momy = 0;
  actor->momz = 0;
  S_StartSound(actor, hexen_sfx_cleric_flame_explode);
}

void A_CFlameRotate(mobj_t *actor)
{
  int an;

  an = (actor->angle + ANG90) >> ANGLETOFINESHIFT;
  actor->momx = actor->special1.i + FixedMul(FLAMEROTSPEED, finecosine[an]);
  actor->momy = actor->special2.i + FixedMul(FLAMEROTSPEED, finesine[an]);
  actor->angle += ANG90 / 15;
}

/* --------------------------------------------------------------------------
 * Wraithverge (Cleric WP_FOURTH): fires Holy spirits that seek out enemies,
 * weave through the air trailing a tail of ghost segments, and home in.
 * ------------------------------------------------------------------------ */

static void CHolyFindTarget(mobj_t *actor)
{
  mobj_t *target = P_RoughTargetSearch(actor, 0, 6);
  if (target)
  {
    P_SetTarget(&actor->special1.m, target);
    actor->flags |= MF_NOCLIP | MF_SKULLFLY;
    actor->flags &= ~MF_MISSILE;
  }
}

static void CHolySeekerMissile(mobj_t *actor, angle_t thresh, angle_t turnMax)
{
  int     dir;
  int     dist;
  angle_t delta;
  angle_t angle;
  mobj_t *target;
  fixed_t newZ;
  fixed_t deltaZ;

  target = actor->special1.m;
  if (!target)
    return;
  if (!(target->flags & MF_SHOOTABLE)
      || (!(target->flags & MF_COUNTKILL) && !target->player))
  {                       /* target died / isn't a creature */
    P_SetTarget(&actor->special1.m, NULL);
    actor->flags &= ~(MF_NOCLIP | MF_SKULLFLY);
    actor->flags |= MF_MISSILE;
    CHolyFindTarget(actor);
    return;
  }
  dir = P_FaceMobj(actor, target, &delta);
  if (delta > thresh)
  {
    delta >>= 1;
    if (delta > turnMax)
      delta = turnMax;
  }
  if (dir)
    actor->angle += delta;     /* clockwise */
  else
    actor->angle -= delta;     /* counter-clockwise */
  angle = actor->angle >> ANGLETOFINESHIFT;
  actor->momx = FixedMul(actor->info->speed, finecosine[angle]);
  actor->momy = FixedMul(actor->info->speed, finesine[angle]);
  if (!(leveltime & 15)
      || actor->z > target->z + target->height
      || actor->z + actor->height < target->z)
  {
    newZ = target->z + ((P_Random(pr_heretic) * target->height) >> 8);
    deltaZ = newZ - actor->z;
    if (abs(deltaZ) > 15 * FRACUNIT)
      deltaZ = (deltaZ > 0) ? 15 * FRACUNIT : -15 * FRACUNIT;
    dist = P_AproxDistance(target->x - actor->x, target->y - actor->y);
    dist = dist / actor->info->speed;
    if (dist < 1)
      dist = 1;
    actor->momz = deltaZ / dist;
  }
}

static void CHolyWeave(mobj_t *actor)
{
  fixed_t newX, newY;
  int     weaveXY, weaveZ;
  int     angle;

  weaveXY = actor->special2.i >> 16;
  weaveZ = actor->special2.i & 0xFFFF;
  angle = (actor->angle + ANG90) >> ANGLETOFINESHIFT;
  newX = actor->x - FixedMul(finecosine[angle], FloatBobOffsets[weaveXY] << 2);
  newY = actor->y - FixedMul(finesine[angle], FloatBobOffsets[weaveXY] << 2);
  weaveXY = (weaveXY + (P_Random(pr_heretic) % 5)) & 63;
  newX += FixedMul(finecosine[angle], FloatBobOffsets[weaveXY] << 2);
  newY += FixedMul(finesine[angle], FloatBobOffsets[weaveXY] << 2);
  P_TryMove(actor, newX, newY, 0);
  actor->z -= FloatBobOffsets[weaveZ] << 1;
  weaveZ = (weaveZ + (P_Random(pr_heretic) % 5)) & 63;
  actor->z += FloatBobOffsets[weaveZ] << 1;
  actor->special2.i = weaveZ + (weaveXY << 16);
}

void A_CHolySeek(mobj_t *actor)
{
  actor->health--;
  if (actor->health <= 0)
  {
    actor->momx >>= 2;
    actor->momy >>= 2;
    actor->momz = 0;
    P_SetMobjState(actor, actor->info->deathstate);
    actor->tics -= P_Random(pr_heretic) & 3;
    return;
  }
  if (actor->special1.m)
  {
    CHolySeekerMissile(actor, actor->special_args[0] * ANG1,
                       actor->special_args[0] * ANG1 * 2);
    if (!((leveltime + 7) & 15))
      actor->special_args[0] = 5 + (P_Random(pr_heretic) / 20);
  }
  CHolyWeave(actor);
}

static void CHolyTailFollow(mobj_t *actor, fixed_t dist)
{
  mobj_t *child;
  int     an;
  fixed_t oldDistance, newDistance;

  child = actor->special1.m;
  if (child)
  {
    an = R_PointToAngle2(actor->x, actor->y, child->x, child->y)
         >> ANGLETOFINESHIFT;
    oldDistance = P_AproxDistance(child->x - actor->x, child->y - actor->y);
    if (P_TryMove(child,
                  actor->x + FixedMul(dist, finecosine[an]),
                  actor->y + FixedMul(dist, finesine[an]), 0))
    {
      newDistance = P_AproxDistance(child->x - actor->x,
                                    child->y - actor->y) - FRACUNIT;
      if (oldDistance < FRACUNIT)
        child->z = (child->z < actor->z) ? actor->z - dist : actor->z + dist;
      else
        child->z = actor->z + FixedMul(FixedDiv(newDistance, oldDistance),
                                       child->z - actor->z);
    }
    CHolyTailFollow(child, dist - FRACUNIT);
  }
}

static void CHolyTailRemove(mobj_t *actor)
{
  mobj_t *child = actor->special1.m;
  if (child)
    CHolyTailRemove(child);
  P_RemoveMobj(actor);
}

void A_CHolyTail(mobj_t *actor)
{
  mobj_t *parent = actor->special2.m;

  if (parent)
  {
    if (parent->state >= &states[parent->info->deathstate])
    {                   /* ghost gone: remove all tail parts */
      CHolyTailRemove(actor);
      return;
    }
    else if (P_TryMove(actor,
               parent->x - FixedMul(14 * FRACUNIT,
                 finecosine[parent->angle >> ANGLETOFINESHIFT]),
               parent->y - FixedMul(14 * FRACUNIT,
                 finesine[parent->angle >> ANGLETOFINESHIFT]), 0))
    {
      actor->z = parent->z - 5 * FRACUNIT;
    }
    CHolyTailFollow(actor, 10 * FRACUNIT);
  }
}

void A_CHolyCheckScream(mobj_t *actor)
{
  A_CHolySeek(actor);
  if (P_Random(pr_heretic) < 20)
    S_StartSound(actor, hexen_sfx_spirit_active);
  if (!actor->special1.m)
    CHolyFindTarget(actor);
}

void A_CHolySpawnPuff(mobj_t *actor)
{
  P_SpawnMobj(actor->x, actor->y, actor->z, HEXEN_MT_HOLY_MISSILE_PUFF);
}

void A_CHolyAttack2(mobj_t *actor)
{
  int j;
  int i;
  int r;
  mobj_t *mo;
  mobj_t *tail, *next;

  for (j = 0; j < 4; j++)
  {
    mo = P_SpawnMobj(actor->x, actor->y, actor->z, HEXEN_MT_HOLY_FX);
    if (!mo)
      continue;
    switch (j)
    {                           /* float bob index */
      case 0:
        mo->special2.i = P_Random(pr_heretic) & 7;              /* upper-left */
        break;
      case 1:
        mo->special2.i = 32 + (P_Random(pr_heretic) & 7);       /* upper-right */
        break;
      case 2:
        mo->special2.i = (32 + (P_Random(pr_heretic) & 7)) << 16;  /* lower-left */
        break;
      default:
        r = P_Random(pr_heretic);
        mo->special2.i = ((32 + (r & 7)) << 16) + 32 +
                         (P_Random(pr_heretic) & 7);
        break;
    }
    mo->z = actor->z;
    mo->angle = actor->angle + (ANG45 + ANG45 / 2) - ANG45 * j;
    P_ThrustMobj(mo, mo->angle, mo->info->speed);
    P_SetTarget(&mo->target, actor->target);
    mo->special_args[0] = 10;   /* initial turn value */
    mo->special_args[1] = 0;    /* initial look angle */
    if (deathmatch)
    {                           /* ghosts last slightly shorter in deathmatch */
      mo->health = 85;
    }
    if (linetarget)
    {
      P_SetTarget(&mo->special1.m, linetarget);
      mo->flags |= MF_NOCLIP | MF_SKULLFLY;
      mo->flags &= ~MF_MISSILE;
    }
    tail = P_SpawnMobj(mo->x, mo->y, mo->z, HEXEN_MT_HOLY_TAIL);
    P_SetTarget(&tail->special2.m, mo);   /* parent */
    for (i = 1; i < 3; i++)
    {
      next = P_SpawnMobj(mo->x, mo->y, mo->z, HEXEN_MT_HOLY_TAIL);
      P_SetMobjState(next, next->info->spawnstate + 1);
      P_SetTarget(&tail->special1.m, next);
      tail = next;
    }
    P_SetTarget(&tail->special1.m, NULL); /* last tail bit */
  }
}

void A_CHolyAttack(player_t *player, pspdef_t *psp)
{
  (void)psp;
  player->mana[MANA_1] -= WeaponManaUse[player->class][player->readyweapon];
  player->mana[MANA_2] -= WeaponManaUse[player->class][player->readyweapon];
  P_SpawnPlayerMissile(player->mo, HEXEN_MT_HOLY_MISSILE);
  S_StartSound(player->mo, hexen_sfx_choly_fire);
}

/* The screen-flash palette stages of the Wraithverge fire animation are
 * cosmetic and depend on status-bar refresh hooks this core does not have;
 * the state advances normally without them. */
void A_CHolyPalette(player_t *player, pspdef_t *psp)
{
  (void)player;
  (void)psp;
}

/* --------------------------------------------------------------------------
 * Mage weapons
 * ------------------------------------------------------------------------ */

/* The Sapphire Wand (Mage WP_FIRST): a free, fast blaster bolt. */
void A_MWandAttack(player_t *player, pspdef_t *psp)
{
  mobj_t *mo;

  (void)psp;
  mo = P_SpawnPlayerMissile(player->mo, HEXEN_MT_MWAND_MISSILE);
  if (mo)
    mo->thinker.function.arg1 = (arg1_t)P_BlasterMobjThinker;
  S_StartSound(player->mo, hexen_sfx_mage_wand_fire);
}

/* The Cone of Shards (Mage WP_SECOND): a frost burst.  At point-blank it
 * deals heavy ice damage to the first creature in a 45-degree arc; with no
 * target in range it instead fires a self-reproducing spray of ice shards
 * that fan out in fixed directions (A_ShedShard). */
#define SHARDSPAWN_LEFT  1
#define SHARDSPAWN_RIGHT 2
#define SHARDSPAWN_UP    4
#define SHARDSPAWN_DOWN  8

void A_FireConePL1(player_t *player, pspdef_t *psp)
{
  angle_t angle;
  int     damage;
  int     i;
  mobj_t *pmo, *mo;
  int     conedone = false;

  (void)psp;
  pmo = player->mo;
  player->mana[MANA_1] -= WeaponManaUse[player->class][player->readyweapon];
  S_StartSound(pmo, hexen_sfx_mage_shards_fire);

  damage = 90 + (P_Random(pr_punch) & 15);
  for (i = 0; i < 16; i++)
  {
    angle = pmo->angle + i * (ANG45 / 16);
    P_AimLineAttack(pmo, angle, MELEERANGE, 0);
    if (linetarget)
    {
      pmo->flags2 |= MF2_ICEDAMAGE;
      P_DamageMobj(linetarget, pmo, pmo, damage);
      pmo->flags2 &= ~MF2_ICEDAMAGE;
      conedone = true;
      break;
    }
  }

  /* no creatures in range: fire the reproducing shard spray */
  if (!conedone)
  {
    mo = P_SpawnPlayerMissile(pmo, HEXEN_MT_SHARDFX1);
    if (mo)
    {
      mo->special1.i = SHARDSPAWN_LEFT | SHARDSPAWN_DOWN | SHARDSPAWN_UP
                     | SHARDSPAWN_RIGHT;
      mo->special2.i = 3;          /* reproduction levels */
      P_SetTarget(&mo->target, pmo);
      mo->special_args[0] = 3;     /* initial shard does super damage */
    }
  }
}

void A_ShedShard(mobj_t *actor)
{
  mobj_t *mo;
  int     spawndir = actor->special1.i;
  int     spermcount = actor->special2.i;

  if (spermcount <= 0)
    return;                        /* exhausted */
  actor->special2.i = 0;
  spermcount--;

  if (spawndir & SHARDSPAWN_LEFT)
  {
    mo = P_SpawnMissileAngleSpeed(actor, HEXEN_MT_SHARDFX1,
                                  actor->angle + (ANG45 / 9), 0,
                                  (20 + 2 * spermcount) << FRACBITS);
    if (mo)
    {
      mo->special1.i = SHARDSPAWN_LEFT;
      mo->special2.i = spermcount;
      mo->momz = actor->momz;
      P_SetTarget(&mo->target, actor->target);
      mo->special_args[0] = (spermcount == 3) ? 2 : 0;
    }
  }
  if (spawndir & SHARDSPAWN_RIGHT)
  {
    mo = P_SpawnMissileAngleSpeed(actor, HEXEN_MT_SHARDFX1,
                                  actor->angle - (ANG45 / 9), 0,
                                  (20 + 2 * spermcount) << FRACBITS);
    if (mo)
    {
      mo->special1.i = SHARDSPAWN_RIGHT;
      mo->special2.i = spermcount;
      mo->momz = actor->momz;
      P_SetTarget(&mo->target, actor->target);
      mo->special_args[0] = (spermcount == 3) ? 2 : 0;
    }
  }
  if (spawndir & SHARDSPAWN_UP)
  {
    mo = P_SpawnMissileAngleSpeed(actor, HEXEN_MT_SHARDFX1, actor->angle,
                                  0, (15 + 2 * spermcount) << FRACBITS);
    if (mo)
    {
      mo->momz = actor->momz;
      mo->z += 8 * FRACUNIT;
      if (spermcount & 1)
        mo->special1.i = SHARDSPAWN_UP | SHARDSPAWN_LEFT | SHARDSPAWN_RIGHT;
      else
        mo->special1.i = SHARDSPAWN_UP;
      mo->special2.i = spermcount;
      P_SetTarget(&mo->target, actor->target);
      mo->special_args[0] = (spermcount == 3) ? 2 : 0;
    }
  }
  if (spawndir & SHARDSPAWN_DOWN)
  {
    mo = P_SpawnMissileAngleSpeed(actor, HEXEN_MT_SHARDFX1, actor->angle,
                                  0, (15 + 2 * spermcount) << FRACBITS);
    if (mo)
    {
      mo->momz = actor->momz;
      mo->z -= 4 * FRACUNIT;
      if (spermcount & 1)
        mo->special1.i = SHARDSPAWN_DOWN | SHARDSPAWN_LEFT | SHARDSPAWN_RIGHT;
      else
        mo->special1.i = SHARDSPAWN_DOWN;
      mo->special2.i = spermcount;
      P_SetTarget(&mo->target, actor->target);
      mo->special_args[0] = (spermcount == 3) ? 2 : 0;
    }
  }
}

/* --------------------------------------------------------------------------
 * Arc of Death / Lightning (Mage WP_THIRD): fires a pair of lightning bolts
 * that crawl along the floor and ceiling, zig-zagging and homing, each
 * mirroring the other.  Several interacting actor codepointers drive the
 * floor bolt, the ceiling bolt and their shared "zap" segments.
 * ------------------------------------------------------------------------ */
#define ZAGSPEED FRACUNIT

void A_LightningClip(mobj_t *actor)
{
  mobj_t *cMo;
  mobj_t *target = NULL;
  int     zigZag;

  if (actor->type == HEXEN_MT_LIGHTNING_FLOOR)
  {
    actor->z = actor->floorz;
    if (actor->special2.m)
      target = actor->special2.m->special1.m;
  }
  else if (actor->type == HEXEN_MT_LIGHTNING_CEILING)
  {
    actor->z = actor->ceilingz - actor->height;
    target = actor->special1.m;
  }

  if (actor->type == HEXEN_MT_LIGHTNING_FLOOR)
  {                       /* floor bolt zig-zags and drags the ceiling bolt */
    cMo = actor->special2.m;
    zigZag = P_Random(pr_heretic);
    if ((zigZag > 128 && actor->special1.i < 2) || actor->special1.i < -2)
    {
      P_ThrustMobj(actor, actor->angle + ANG90, ZAGSPEED);
      if (cMo)
        P_ThrustMobj(cMo, actor->angle + ANG90, ZAGSPEED);
      actor->special1.i++;
    }
    else
    {
      P_ThrustMobj(actor, actor->angle - ANG90, ZAGSPEED);
      if (cMo)
        P_ThrustMobj(cMo, cMo->angle - ANG90, ZAGSPEED);
      actor->special1.i--;
    }
  }

  if (target)
  {
    if (target->health <= 0)
    {
      P_ExplodeMissile(actor);
    }
    else
    {
      actor->angle = R_PointToAngle2(actor->x, actor->y,
                                     target->x, target->y);
      actor->momx = 0;
      actor->momy = 0;
      P_ThrustMobj(actor, actor->angle, actor->info->speed >> 1);
    }
  }
}

void A_LightningZap(mobj_t *actor)
{
  mobj_t *mo;
  fixed_t deltaZ;
  int     r1, r2;

  A_LightningClip(actor);

  actor->health -= 8;
  if (actor->health <= 0)
  {
    P_SetMobjState(actor, actor->info->deathstate);
    return;
  }
  deltaZ = (actor->type == HEXEN_MT_LIGHTNING_FLOOR)
         ? 10 * FRACUNIT : -10 * FRACUNIT;
  r1 = P_Random(pr_heretic);
  r2 = P_Random(pr_heretic);
  mo = P_SpawnMobj(actor->x + ((r2 - 128) * actor->radius / 256),
                   actor->y + ((r1 - 128) * actor->radius / 256),
                   actor->z + deltaZ, HEXEN_MT_LIGHTNING_ZAP);
  if (mo)
  {
    P_SetTarget(&mo->special2.m, actor);
    mo->momx = actor->momx;
    mo->momy = actor->momy;
    P_SetTarget(&mo->target, actor->target);
    mo->momz = (actor->type == HEXEN_MT_LIGHTNING_FLOOR)
             ? 20 * FRACUNIT : -20 * FRACUNIT;
  }

  if (actor->type == HEXEN_MT_LIGHTNING_FLOOR && P_Random(pr_heretic) < 160)
    S_StartSound(actor, hexen_sfx_mage_lightning_continuous);
}

void A_MLightningAttack2(mobj_t *actor)
{
  mobj_t *fmo, *cmo;

  fmo = P_SpawnPlayerMissile(actor, HEXEN_MT_LIGHTNING_FLOOR);
  cmo = P_SpawnPlayerMissile(actor, HEXEN_MT_LIGHTNING_CEILING);
  if (fmo)
  {
    P_SetTarget(&fmo->special1.m, NULL);
    P_SetTarget(&fmo->special2.m, cmo);
    A_LightningZap(fmo);
  }
  if (cmo)
  {
    P_SetTarget(&cmo->special1.m, NULL);
    P_SetTarget(&cmo->special2.m, fmo);
    A_LightningZap(cmo);
  }
  S_StartSound(actor, hexen_sfx_mage_lightning_fire);
}

void A_MLightningAttack(player_t *player, pspdef_t *psp)
{
  (void)psp;
  A_MLightningAttack2(player->mo);
  player->mana[MANA_2] -= WeaponManaUse[player->class][player->readyweapon];
}

void A_ZapMimic(mobj_t *actor)
{
  mobj_t *mo = actor->special2.m;

  if (mo)
  {
    if (mo->state >= &states[mo->info->deathstate]
        || mo->state == &states[HEXEN_S_FREETARGMOBJ])
      P_ExplodeMissile(actor);
    else
    {
      actor->momx = mo->momx;
      actor->momy = mo->momy;
    }
  }
}

void A_LastZap(mobj_t *actor)
{
  mobj_t *mo;

  mo = P_SpawnMobj(actor->x, actor->y, actor->z, HEXEN_MT_LIGHTNING_ZAP);
  if (mo)
  {
    P_SetMobjState(mo, HEXEN_S_LIGHTNING_ZAP_X1);
    mo->momz = 40 * FRACUNIT;
  }
}

void A_LightningRemove(mobj_t *actor)
{
  mobj_t *mo = actor->special2.m;

  if (mo)
  {
    P_SetTarget(&mo->special2.m, NULL);
    P_ExplodeMissile(mo);
  }
}

void A_LightningReady(player_t *player, pspdef_t *psp)
{
  A_WeaponReady(player, psp);
  if (P_Random(pr_punch) < 160)
    S_StartSound(player->mo, hexen_sfx_mage_lightning_ready);
}

/* --------------------------------------------------------------------------
 * Bloodscourge (Mage WP_FOURTH): fires three homing staff missiles that
 * acquire targets, weave through the air and seek their prey.
 * ------------------------------------------------------------------------ */
static void MStaffSpawn(mobj_t *pmo, angle_t angle)
{
  mobj_t *mo;

  mo = P_SPMAngle(pmo, HEXEN_MT_MSTAFF_FX2, angle);
  if (mo)
  {
    P_SetTarget(&mo->target, pmo);
    P_SetTarget(&mo->special1.m, P_RoughTargetSearch(mo, 0, 10));
  }
}

void A_MStaffAttack(player_t *player, pspdef_t *psp)
{
  angle_t angle;
  mobj_t *pmo;

  (void)psp;
  player->mana[MANA_1] -= WeaponManaUse[player->class][player->readyweapon];
  player->mana[MANA_2] -= WeaponManaUse[player->class][player->readyweapon];
  pmo = player->mo;
  angle = pmo->angle;

  MStaffSpawn(pmo, angle);
  MStaffSpawn(pmo, angle - ANG1 * 5);
  MStaffSpawn(pmo, angle + ANG1 * 5);
  S_StartSound(player->mo, hexen_sfx_mage_staff_fire);
}

/* The fire-animation screen flash is cosmetic and needs status-bar palette
 * hooks this core lacks; the state advances normally without it. */
void A_MStaffPalette(player_t *player, pspdef_t *psp)
{
  (void)player;
  (void)psp;
}

void A_MStaffWeave(mobj_t *actor)
{
  fixed_t newX, newY;
  int     weaveXY, weaveZ;
  int     angle;

  weaveXY = actor->special2.i >> 16;
  weaveZ = actor->special2.i & 0xFFFF;
  angle = (actor->angle + ANG90) >> ANGLETOFINESHIFT;
  newX = actor->x - FixedMul(finecosine[angle], FloatBobOffsets[weaveXY] << 2);
  newY = actor->y - FixedMul(finesine[angle], FloatBobOffsets[weaveXY] << 2);
  weaveXY = (weaveXY + 6) & 63;
  newX += FixedMul(finecosine[angle], FloatBobOffsets[weaveXY] << 2);
  newY += FixedMul(finesine[angle], FloatBobOffsets[weaveXY] << 2);
  P_TryMove(actor, newX, newY, 0);
  actor->z -= FloatBobOffsets[weaveZ] << 1;
  weaveZ = (weaveZ + 3) & 63;
  actor->z += FloatBobOffsets[weaveZ] << 1;
  if (actor->z <= actor->floorz)
    actor->z = actor->floorz + FRACUNIT;
  actor->special2.i = weaveZ + (weaveXY << 16);
}

void A_MStaffTrack(mobj_t *actor)
{
  if (actor->special1.m == NULL && P_Random(pr_heretic) < 50)
    P_SetTarget(&actor->special1.m, P_RoughTargetSearch(actor, 0, 10));
  P_SeekerMissile(actor, &actor->special1.m, ANG1 * 2, ANG1 * 10, false);
}

#ifdef HEXEN
//****************************************************************************
//
// WEAPON ATTACKS
//
//****************************************************************************

//============================================================================
//
//	AdjustPlayerAngle
//
//============================================================================

#define MAX_ANGLE_ADJUST (5*ANGLE_1)

void AdjustPlayerAngle(mobj_t *pmo)
{
	angle_t angle;
	int difference;

	angle = R_PointToAngle2(pmo->x, pmo->y, linetarget->x, linetarget->y);
	difference = (int)angle-(int)pmo->angle;
	if(abs(difference) > MAX_ANGLE_ADJUST)
	{
		pmo->angle += difference > 0 ? MAX_ANGLE_ADJUST : -MAX_ANGLE_ADJUST;
	}
	else
	{
		pmo->angle = angle;
	}
}

//============================================================================
//
// A_SnoutAttack
//
//============================================================================

void A_SnoutAttack(player_t *player, pspdef_t *psp)
{
	angle_t angle;
	int damage;
	int slope;

	damage = 3+(P_Random()&3);
	angle = player->mo->angle;
	slope = P_AimLineAttack(player->mo, angle, MELEERANGE);
	PuffType = MT_SNOUTPUFF;
	PuffSpawned = NULL;
	P_LineAttack(player->mo, angle, MELEERANGE, slope, damage);
	S_StartSound(player->mo, SFX_PIG_ACTIVE1+(P_Random()&1));
	if(linetarget)
	{
		AdjustPlayerAngle(player->mo);
//		player->mo->angle = R_PointToAngle2(player->mo->x,
//			player->mo->y, linetarget->x, linetarget->y);
		if(PuffSpawned)
		{ // Bit something
			S_StartSound(player->mo, SFX_PIG_ATTACK);
		}
	}
}

//============================================================================
//
// A_FHammerAttack
//
//============================================================================

#define HAMMER_RANGE	(MELEERANGE+MELEERANGE/2)

void A_FHammerAttack(player_t *player, pspdef_t *psp)
{
	angle_t angle;
	mobj_t *pmo=player->mo;
	int damage;
	fixed_t power;
	int slope;
	int i;

	damage = 60+(P_Random()&63);
	power = 10*FRACUNIT;
	PuffType = MT_HAMMERPUFF;
	for(i = 0; i < 16; i++)
	{
		angle = pmo->angle+i*(ANG45/32);
		slope = P_AimLineAttack(pmo, angle, HAMMER_RANGE);
		if(linetarget)
		{
			P_LineAttack(pmo, angle, HAMMER_RANGE, slope, damage);
			AdjustPlayerAngle(pmo);
			if (linetarget->flags&MF_COUNTKILL || linetarget->player)
			{
				P_ThrustMobj(linetarget, angle, power);
			}
			pmo->special1 = false; // Don't throw a hammer
			goto hammerdone;
		}
		angle = pmo->angle-i*(ANG45/32);
		slope = P_AimLineAttack(pmo, angle, HAMMER_RANGE);
		if(linetarget)
		{
			P_LineAttack(pmo, angle, HAMMER_RANGE, slope, damage);
			AdjustPlayerAngle(pmo);
			if (linetarget->flags&MF_COUNTKILL || linetarget->player)
			{
				P_ThrustMobj(linetarget, angle, power);
			}
			pmo->special1 = false; // Don't throw a hammer
			goto hammerdone;
		}
	}
	// didn't find any targets in meleerange, so set to throw out a hammer
	PuffSpawned = NULL;
	angle = pmo->angle;
	slope = P_AimLineAttack(pmo, angle, HAMMER_RANGE);
	P_LineAttack(pmo, angle, HAMMER_RANGE, slope, damage);
	if(PuffSpawned)
	{
		pmo->special1 = false;
	}
	else
	{
		pmo->special1 = true;
	}
hammerdone:
	if(player->mana[MANA_2] < 
		WeaponManaUse[player->class][player->readyweapon])
	{ // Don't spawn a hammer if the player doesn't have enough mana
		pmo->special1 = false;
	}
	return;		
}

//============================================================================
//
// A_FHammerThrow
//
//============================================================================

void A_FHammerThrow(player_t *player, pspdef_t *psp)
{
	mobj_t *mo;

	if(!player->mo->special1)
	{
		return;
	}
	player->mana[MANA_2] -= WeaponManaUse[player->class][player->readyweapon];
	mo = P_SpawnPlayerMissile(player->mo, MT_HAMMER_MISSILE); 
	if(mo)
	{
		mo->special1 = 0;
	}	
}

//============================================================================
//
// A_FSwordAttack
//
//============================================================================

void A_FSwordAttack(player_t *player, pspdef_t *psp)
{
	mobj_t *pmo;

	player->mana[MANA_1] -= WeaponManaUse[player->class][player->readyweapon];
	player->mana[MANA_2] -= WeaponManaUse[player->class][player->readyweapon];
	pmo = player->mo;
	P_SPMAngleXYZ(pmo, pmo->x, pmo->y, pmo->z-10*FRACUNIT, MT_FSWORD_MISSILE, 
		pmo->angle+ANG45/4);
	P_SPMAngleXYZ(pmo, pmo->x, pmo->y, pmo->z-5*FRACUNIT, MT_FSWORD_MISSILE, 
		pmo->angle+ANG45/8);
	P_SPMAngleXYZ(pmo, pmo->x, pmo->y, pmo->z, MT_FSWORD_MISSILE, pmo->angle);
	P_SPMAngleXYZ(pmo, pmo->x, pmo->y, pmo->z+5*FRACUNIT, MT_FSWORD_MISSILE, 
		pmo->angle-ANG45/8);
	P_SPMAngleXYZ(pmo, pmo->x, pmo->y, pmo->z+10*FRACUNIT, MT_FSWORD_MISSILE, 
		pmo->angle-ANG45/4);
	S_StartSound(pmo, SFX_FIGHTER_SWORD_FIRE);
}

//============================================================================
//
// A_FSwordAttack2
//
//============================================================================

void A_FSwordAttack2(mobj_t *actor)
{
	angle_t angle = actor->angle;

	P_SpawnMissileAngle(actor, MT_FSWORD_MISSILE,angle+ANG45/4, 0);
	P_SpawnMissileAngle(actor, MT_FSWORD_MISSILE,angle+ANG45/8, 0);
	P_SpawnMissileAngle(actor, MT_FSWORD_MISSILE,angle,         0);
	P_SpawnMissileAngle(actor, MT_FSWORD_MISSILE,angle-ANG45/8, 0);
	P_SpawnMissileAngle(actor, MT_FSWORD_MISSILE,angle-ANG45/4, 0);
	S_StartSound(actor, SFX_FIGHTER_SWORD_FIRE);
}

//============================================================================
//
// A_FSwordFlames
//
//============================================================================

void A_FSwordFlames(mobj_t *actor)
{
	int i;

	for(i = 1+(P_Random()&3); i; i--)
	{
		P_SpawnMobj(actor->x+((P_Random()-128)<<12), actor->y
			+((P_Random()-128)<<12), actor->z+((P_Random()-128)<<11),
			MT_FSWORD_FLAME);
	}
}

//============================================================================
//
// A_MWandAttack
//
//============================================================================

void A_MWandAttack(player_t *player, pspdef_t *psp)
{
	mobj_t *mo;

	mo = P_SpawnPlayerMissile(player->mo, MT_MWAND_MISSILE);
	if(mo)
	{
		mo->thinker.function = P_BlasterMobjThinker;
	}
	S_StartSound(player->mo, SFX_MAGE_WAND_FIRE);
}

// ===== Mage Lightning Weapon =====

//============================================================================
//
// A_LightningReady
//
//============================================================================

void A_LightningReady(player_t *player, pspdef_t *psp)
{
	A_WeaponReady(player, psp);
	if(P_Random() < 160)
	{
		S_StartSound(player->mo, SFX_MAGE_LIGHTNING_READY);
	}
}

//============================================================================
//
// A_LightningClip
//
//============================================================================

#define ZAGSPEED	FRACUNIT

void A_LightningClip(mobj_t *actor)
{
	mobj_t *cMo;
	mobj_t *target;
	int zigZag;

	if(actor->type == MT_LIGHTNING_FLOOR)
	{
		actor->z = actor->floorz;
		target = (mobj_t *)((mobj_t *)actor->special2)->special1;
	}
	else if(actor->type == MT_LIGHTNING_CEILING)
	{
		actor->z = actor->ceilingz-actor->height;
		target = (mobj_t *)actor->special1;
	}
	if(actor->type == MT_LIGHTNING_FLOOR)
	{ // floor lightning zig-zags, and forces the ceiling lightning to mimic
		cMo = (mobj_t *)actor->special2;
		zigZag = P_Random();
		if((zigZag > 128 && actor->special1 < 2) || actor->special1 < -2)
		{
			P_ThrustMobj(actor, actor->angle+ANG90, ZAGSPEED);
			if(cMo)
			{
				P_ThrustMobj(cMo, actor->angle+ANG90, ZAGSPEED);
			}
			actor->special1++;
		}
		else
		{
			P_ThrustMobj(actor, actor->angle-ANG90, ZAGSPEED);
			if(cMo)
			{
				P_ThrustMobj(cMo, cMo->angle-ANG90, ZAGSPEED);
			}
			actor->special1--;
		}
	}
	if(target)
	{
		if(target->health <= 0)
		{
			P_ExplodeMissile(actor);
		}
		else
		{
			actor->angle = R_PointToAngle2(actor->x, actor->y, target->x,
				target->y);
			actor->momx = 0;
			actor->momy = 0;
			P_ThrustMobj(actor, actor->angle, actor->info->speed>>1);
		}
	}
}

//============================================================================
//
// A_LightningZap
//
//============================================================================

void A_LightningZap(mobj_t *actor)
{
	mobj_t *mo;
	fixed_t deltaZ;

	A_LightningClip(actor);

	actor->health -= 8;
	if(actor->health <= 0)
	{
		P_SetMobjState(actor, actor->info->deathstate);
		return;
	}
	if(actor->type == MT_LIGHTNING_FLOOR)
	{
		deltaZ = 10*FRACUNIT;
	}
	else
	{
		deltaZ = -10*FRACUNIT;
	}
	mo = P_SpawnMobj(actor->x+((P_Random()-128)*actor->radius/256), 
		actor->y+((P_Random()-128)*actor->radius/256), 
		actor->z+deltaZ, MT_LIGHTNING_ZAP);
	if(mo)
	{
		mo->special2 = (int)actor;
		mo->momx = actor->momx;
		mo->momy = actor->momy;
		mo->target = actor->target;
		if(actor->type == MT_LIGHTNING_FLOOR)
		{
			mo->momz = 20*FRACUNIT;
		}
		else 
		{
			mo->momz = -20*FRACUNIT;
		}
	}
/*
	mo = P_SpawnMobj(actor->x+((P_Random()-128)*actor->radius/256), 
		actor->y+((P_Random()-128)*actor->radius/256), 
		actor->z+deltaZ, MT_LIGHTNING_ZAP);
	if(mo)
	{
		mo->special2 = (int)actor;
		mo->momx = actor->momx;
		mo->momy = actor->momy;
		mo->target = actor->target;
		if(actor->type == MT_LIGHTNING_FLOOR)
		{
			mo->momz = 16*FRACUNIT;
		}
		else 
		{
			mo->momz = -16*FRACUNIT;
		}
	}
*/
	if(actor->type == MT_LIGHTNING_FLOOR && P_Random() < 160)
	{
		S_StartSound(actor, SFX_MAGE_LIGHTNING_CONTINUOUS);
	}
}

//============================================================================
//
// A_MLightningAttack2
//
//============================================================================

void A_MLightningAttack2(mobj_t *actor)
{
	mobj_t *fmo, *cmo;

	fmo = P_SpawnPlayerMissile(actor, MT_LIGHTNING_FLOOR);
	cmo = P_SpawnPlayerMissile(actor, MT_LIGHTNING_CEILING);
	if(fmo)
	{
		fmo->special1 = 0;
		fmo->special2 = (int)cmo;
		A_LightningZap(fmo);	
	}
	if(cmo)
	{
		cmo->special1 = 0;	// mobj that it will track
		cmo->special2 = (int)fmo;
		A_LightningZap(cmo);	
	}
	S_StartSound(actor, SFX_MAGE_LIGHTNING_FIRE);
}

//============================================================================
//
// A_MLightningAttack
//
//============================================================================

void A_MLightningAttack(player_t *player, pspdef_t *psp)
{
	A_MLightningAttack2(player->mo);
	player->mana[MANA_2] -= WeaponManaUse[player->class][player->readyweapon];
}

//============================================================================
//
// A_ZapMimic
//
//============================================================================

void A_ZapMimic(mobj_t *actor)
{
	mobj_t *mo;

	mo = (mobj_t *)actor->special2;
	if(mo)
	{
		if(mo->state >= &states[mo->info->deathstate]
			|| mo->state == &states[S_FREETARGMOBJ])
		{
			P_ExplodeMissile(actor);
		}
		else
		{
			actor->momx = mo->momx;
			actor->momy = mo->momy;
		}
	}
}

//============================================================================
//
// A_LastZap
//
//============================================================================

void A_LastZap(mobj_t *actor)
{
	mobj_t *mo;

	mo = P_SpawnMobj(actor->x, actor->y, actor->z, MT_LIGHTNING_ZAP);
	if(mo)
	{
		P_SetMobjState(mo, S_LIGHTNING_ZAP_X1);
		mo->momz = 40*FRACUNIT;
	}
}

//============================================================================
//
// A_LightningRemove
//
//============================================================================

void A_LightningRemove(mobj_t *actor)
{
	mobj_t *mo;

	mo = (mobj_t *)actor->special2;
	if(mo)
	{
		mo->special2 = 0;
		P_ExplodeMissile(mo);
	}
}


//============================================================================
//
// MStaffSpawn
//
//============================================================================
void MStaffSpawn(mobj_t *pmo, angle_t angle)
{
	mobj_t *mo;

	mo = P_SPMAngle(pmo, MT_MSTAFF_FX2, angle);
	if (mo)
	{
		mo->target = pmo;
		mo->special1 = (int)P_RoughMonsterSearch(mo, 10);
	}
}

//============================================================================
//
// A_MStaffAttack
//
//============================================================================

void A_MStaffAttack(player_t *player, pspdef_t *psp)
{
	angle_t angle;
	mobj_t *pmo;

	player->mana[MANA_1] -= WeaponManaUse[player->class][player->readyweapon];
	player->mana[MANA_2] -= WeaponManaUse[player->class][player->readyweapon];
	pmo = player->mo;
	angle = pmo->angle;
	
	MStaffSpawn(pmo, angle);
	MStaffSpawn(pmo, angle-ANGLE_1*5);
	MStaffSpawn(pmo, angle+ANGLE_1*5);
	S_StartSound(player->mo, SFX_MAGE_STAFF_FIRE);
	if(player == &players[consoleplayer])
	{
		player->damagecount = 0;
		player->bonuscount = 0;
		I_SetPalette((byte *)W_CacheLumpNum(W_GetNumForName("playpal"),
			PU_CACHE)+STARTSCOURGEPAL*768);
	}
}

//============================================================================
//
// A_MStaffPalette
//
//============================================================================

void A_MStaffPalette(player_t *player, pspdef_t *psp)
{
	int pal;

	if(player == &players[consoleplayer])
	{
		pal = STARTSCOURGEPAL+psp->state-(&states[S_MSTAFFATK_2]);
		if(pal == STARTSCOURGEPAL+3)
		{ // reset back to original playpal
			pal = 0;
		}
		I_SetPalette((byte *)W_CacheLumpNum(W_GetNumForName("playpal"),
			PU_CACHE)+pal*768);
	}
}

//============================================================================
//
// A_MStaffWeave
//
//============================================================================

void A_MStaffWeave(mobj_t *actor)
{
	fixed_t newX, newY;
	int weaveXY, weaveZ;
	int angle;

	weaveXY = actor->special2>>16;
	weaveZ = actor->special2&0xFFFF;
	angle = (actor->angle+ANG90)>>ANGLETOFINESHIFT;
	newX = actor->x-FixedMul(finecosine[angle], 
		FloatBobOffsets[weaveXY]<<2);
	newY = actor->y-FixedMul(finesine[angle],
		FloatBobOffsets[weaveXY]<<2);
	weaveXY = (weaveXY+6)&63;
	newX += FixedMul(finecosine[angle], 
		FloatBobOffsets[weaveXY]<<2);
	newY += FixedMul(finesine[angle], 
		FloatBobOffsets[weaveXY]<<2);
	P_TryMove(actor, newX, newY);
	actor->z -= FloatBobOffsets[weaveZ]<<1;
	weaveZ = (weaveZ+3)&63;
	actor->z += FloatBobOffsets[weaveZ]<<1;
	if(actor->z <= actor->floorz)
	{
		actor->z = actor->floorz+FRACUNIT;
	}
	actor->special2 = weaveZ+(weaveXY<<16);
}


//============================================================================
//
// A_MStaffTrack
//
//============================================================================

void A_MStaffTrack(mobj_t *actor)
{
	if ((actor->special1 == 0) && (P_Random()<50))
	{
		actor->special1 = (int)P_RoughMonsterSearch(actor, 10);
	}
	P_SeekerMissile(actor, ANGLE_1*2, ANGLE_1*10);
}


//============================================================================
//
// MStaffSpawn2 - for use by mage class boss
//
//============================================================================

void MStaffSpawn2(mobj_t *actor, angle_t angle)
{
	mobj_t *mo;

	mo = P_SpawnMissileAngle(actor, MT_MSTAFF_FX2, angle, 0);
	if (mo)
	{
		mo->target = actor;
		mo->special1 = (int)P_RoughMonsterSearch(mo, 10);
	}
}

//============================================================================
//
// A_MStaffAttack2 - for use by mage class boss
//
//============================================================================

void A_MStaffAttack2(mobj_t *actor)
{
	angle_t angle;
	angle = actor->angle;
	MStaffSpawn2(actor, angle);
	MStaffSpawn2(actor, angle-ANGLE_1*5);
	MStaffSpawn2(actor, angle+ANGLE_1*5);
	S_StartSound(actor, SFX_MAGE_STAFF_FIRE);
}

//============================================================================
//
// A_FPunchAttack
//
//============================================================================

void A_FPunchAttack(player_t *player, pspdef_t *psp)
{
	angle_t angle;
	int damage;
	int slope;
	mobj_t *pmo = player->mo;
	fixed_t power;
	int i;

	damage = 40+(P_Random()&15);
	power = 2*FRACUNIT;
	PuffType = MT_PUNCHPUFF;
	for(i = 0; i < 16; i++)
	{
		angle = pmo->angle+i*(ANG45/16);
		slope = P_AimLineAttack(pmo, angle, 2*MELEERANGE);
		if(linetarget)
		{
			player->mo->special1++;
			if(pmo->special1 == 3)
			{
				damage <<= 1;
				power = 6*FRACUNIT;
				PuffType = MT_HAMMERPUFF;
			}
			P_LineAttack(pmo, angle, 2*MELEERANGE, slope, damage);
			if (linetarget->flags&MF_COUNTKILL || linetarget->player)
			{
				P_ThrustMobj(linetarget, angle, power);
			}
			AdjustPlayerAngle(pmo);
			goto punchdone;
		}
		angle = pmo->angle-i*(ANG45/16);
		slope = P_AimLineAttack(pmo, angle, 2*MELEERANGE);
		if(linetarget)
		{
			pmo->special1++;
			if(pmo->special1 == 3)
			{
				damage <<= 1;
				power = 6*FRACUNIT;
				PuffType = MT_HAMMERPUFF;
			}
			P_LineAttack(pmo, angle, 2*MELEERANGE, slope, damage);
			if (linetarget->flags&MF_COUNTKILL || linetarget->player)
			{
				P_ThrustMobj(linetarget, angle, power);
			}
			AdjustPlayerAngle(pmo);
			goto punchdone;
		}
	}
	// didn't find any creatures, so try to strike any walls
	pmo->special1 = 0;

	angle = pmo->angle;
	slope = P_AimLineAttack(pmo, angle, MELEERANGE);
	P_LineAttack(pmo, angle, MELEERANGE, slope, damage);

punchdone:
	if(pmo->special1 == 3)
	{
		pmo->special1 = 0;
		P_SetPsprite(player, ps_weapon, S_PUNCHATK2_1);
		S_StartSound(pmo, SFX_FIGHTER_GRUNT);
	}
	return;		
}

//============================================================================
//
// A_FAxeAttack
//
//============================================================================

#define AXERANGE	2.25*MELEERANGE

void A_FAxeAttack(player_t *player, pspdef_t *psp)
{
	angle_t angle;
	mobj_t *pmo=player->mo;
	fixed_t power;
	int damage;
	int slope;
	int i;
	int useMana;

	damage = 40+(P_Random()&15)+(P_Random()&7);
	power = 0;
	if(player->mana[MANA_1] > 0)
	{
		damage <<= 1;
		power = 6*FRACUNIT;
		PuffType = MT_AXEPUFF_GLOW;
		useMana = 1;
	}
	else
	{
		PuffType = MT_AXEPUFF;
		useMana = 0;
	}
	for(i = 0; i < 16; i++)
	{
		angle = pmo->angle+i*(ANG45/16);
		slope = P_AimLineAttack(pmo, angle, AXERANGE);
		if(linetarget)
		{
			P_LineAttack(pmo, angle, AXERANGE, slope, damage);
			if (linetarget->flags&MF_COUNTKILL || linetarget->player)
			{
				P_ThrustMobj(linetarget, angle, power);
			}
			AdjustPlayerAngle(pmo);
			useMana++; 
			goto axedone;
		}
		angle = pmo->angle-i*(ANG45/16);
		slope = P_AimLineAttack(pmo, angle, AXERANGE);
		if(linetarget)
		{
			P_LineAttack(pmo, angle, AXERANGE, slope, damage);
			if (linetarget->flags&MF_COUNTKILL)
			{
				P_ThrustMobj(linetarget, angle, power);
			}
			AdjustPlayerAngle(pmo);
			useMana++; 
			goto axedone;
		}
	}
	// didn't find any creatures, so try to strike any walls
	pmo->special1 = 0;

	angle = pmo->angle;
	slope = P_AimLineAttack(pmo, angle, MELEERANGE);
	P_LineAttack(pmo, angle, MELEERANGE, slope, damage);

axedone:
	if(useMana == 2)
	{
		player->mana[MANA_1] -= 
			WeaponManaUse[player->class][player->readyweapon];
		if(player->mana[MANA_1] <= 0)
		{
			P_SetPsprite(player, ps_weapon, S_FAXEATK_5);
		}
	}
	return;		
}

//===========================================================================
//
// A_CMaceAttack
//
//===========================================================================

void A_CMaceAttack(player_t *player, pspdef_t *psp)
{
	angle_t angle;
	int damage;
	int slope;
	int i;

	damage = 25+(P_Random()&15);
	PuffType = MT_HAMMERPUFF;
	for(i = 0; i < 16; i++)
	{
		angle = player->mo->angle+i*(ANG45/16);
		slope = P_AimLineAttack(player->mo, angle, 2*MELEERANGE);
		if(linetarget)
		{
			P_LineAttack(player->mo, angle, 2*MELEERANGE, slope, 
				damage);
			AdjustPlayerAngle(player->mo);
//			player->mo->angle = R_PointToAngle2(player->mo->x,
//				player->mo->y, linetarget->x, linetarget->y);
			goto macedone;
		}
		angle = player->mo->angle-i*(ANG45/16);
		slope = P_AimLineAttack(player->mo, angle, 2*MELEERANGE);
		if(linetarget)
		{
			P_LineAttack(player->mo, angle, 2*MELEERANGE, slope, 
				damage);
			AdjustPlayerAngle(player->mo);
//			player->mo->angle = R_PointToAngle2(player->mo->x,
//				player->mo->y, linetarget->x, linetarget->y);
			goto macedone;
		}
	}
	// didn't find any creatures, so try to strike any walls
	player->mo->special1 = 0;

	angle = player->mo->angle;
	slope = P_AimLineAttack(player->mo, angle, MELEERANGE);
	P_LineAttack(player->mo, angle, MELEERANGE, slope, 
		damage);
macedone:
	return;		
}

//============================================================================
//
// A_CStaffCheck
//
//============================================================================

void A_CStaffCheck(player_t *player, pspdef_t *psp)
{
	mobj_t *pmo;
	int damage;
	int newLife;
	angle_t angle;
	int slope;
	int i;

	pmo = player->mo;
	damage = 20+(P_Random()&15);
	PuffType = MT_CSTAFFPUFF;
	for(i = 0; i < 3; i++)
	{
		angle = pmo->angle+i*(ANG45/16);
		slope = P_AimLineAttack(pmo, angle, 1.5*MELEERANGE);
		if(linetarget)
		{
			P_LineAttack(pmo, angle, 1.5*MELEERANGE, slope, damage);
			pmo->angle = R_PointToAngle2(pmo->x, pmo->y, 
				linetarget->x, linetarget->y);
			if((linetarget->player || linetarget->flags&MF_COUNTKILL)
				&& (!(linetarget->flags2&(MF2_DORMANT+MF2_INVULNERABLE))))
			{
				newLife = player->health+(damage>>3);
				newLife = newLife > 100 ? 100 : newLife;
				pmo->health = player->health = newLife;
				P_SetPsprite(player, ps_weapon, S_CSTAFFATK2_1);
			}
			player->mana[MANA_1] -= 
				WeaponManaUse[player->class][player->readyweapon];
			break;
		}
		angle = pmo->angle-i*(ANG45/16);
		slope = P_AimLineAttack(player->mo, angle, 1.5*MELEERANGE);
		if(linetarget)
		{
			P_LineAttack(pmo, angle, 1.5*MELEERANGE, slope, damage);
			pmo->angle = R_PointToAngle2(pmo->x, pmo->y, 
				linetarget->x, linetarget->y);
			if(linetarget->player || linetarget->flags&MF_COUNTKILL)
			{
				newLife = player->health+(damage>>4);
				newLife = newLife > 100 ? 100 : newLife;
				pmo->health = player->health = newLife;
				P_SetPsprite(player, ps_weapon, S_CSTAFFATK2_1);
			}
			player->mana[MANA_1] -= 
				WeaponManaUse[player->class][player->readyweapon];
			break;
		}
	}
}

//============================================================================
//
// A_CStaffAttack
//
//============================================================================

void A_CStaffAttack(player_t *player, pspdef_t *psp)
{
	mobj_t *mo;
	mobj_t *pmo;

	player->mana[MANA_1] -=	WeaponManaUse[player->class][player->readyweapon];
	pmo = player->mo;
	mo = P_SPMAngle(pmo, MT_CSTAFF_MISSILE, pmo->angle-(ANG45/15));
	if(mo)
	{
		mo->special2 = 32;
	}
	mo = P_SPMAngle(pmo, MT_CSTAFF_MISSILE, pmo->angle+(ANG45/15));
	if(mo)
	{
		mo->special2 = 0;
	}
	S_StartSound(player->mo, SFX_CLERIC_CSTAFF_FIRE);
}

//============================================================================
//
// A_CStaffMissileSlither
//
//============================================================================

void A_CStaffMissileSlither(mobj_t *actor)
{
	fixed_t newX, newY;
	int weaveXY;
	int angle;

	weaveXY = actor->special2;
	angle = (actor->angle+ANG90)>>ANGLETOFINESHIFT;
	newX = actor->x-FixedMul(finecosine[angle], 
		FloatBobOffsets[weaveXY]);
	newY = actor->y-FixedMul(finesine[angle],
		FloatBobOffsets[weaveXY]);
	weaveXY = (weaveXY+3)&63;
	newX += FixedMul(finecosine[angle], 
		FloatBobOffsets[weaveXY]);
	newY += FixedMul(finesine[angle], 
		FloatBobOffsets[weaveXY]);
	P_TryMove(actor, newX, newY);
	actor->special2 = weaveXY;	
}

//============================================================================
//
// A_CStaffInitBlink
//
//============================================================================

void A_CStaffInitBlink(player_t *player, pspdef_t *psp)
{
	player->mo->special1 = (P_Random()>>1)+20;
}

//============================================================================
//
// A_CStaffCheckBlink
//
//============================================================================

void A_CStaffCheckBlink(player_t *player, pspdef_t *psp)
{
	if(!--player->mo->special1)
	{
		P_SetPsprite(player, ps_weapon, S_CSTAFFBLINK1);
		player->mo->special1 = (P_Random()+50)>>2;
	}
}

//============================================================================
//
// A_CFlameAttack
//
//============================================================================

#define FLAMESPEED	(0.45*FRACUNIT)
#define CFLAMERANGE	(12*64*FRACUNIT)

void A_CFlameAttack(player_t *player, pspdef_t *psp)
{
	mobj_t *mo;

	mo = P_SpawnPlayerMissile(player->mo, MT_CFLAME_MISSILE);
	if(mo)
	{
		mo->thinker.function = P_BlasterMobjThinker;
		mo->special1 = 2;
	}

	player->mana[MANA_2] -= WeaponManaUse[player->class][player->readyweapon];
	S_StartSound(player->mo, SFX_CLERIC_FLAME_FIRE);
}


//============================================================================
//
// A_CFlamePuff
//
//============================================================================

void A_CFlamePuff(mobj_t *actor)
{
	A_UnHideThing(actor);
	actor->momx = 0;
	actor->momy = 0;
	actor->momz = 0;
	S_StartSound(actor, SFX_CLERIC_FLAME_EXPLODE);
}

//============================================================================
//
// A_CFlameMissile
//
//============================================================================

void A_CFlameMissile(mobj_t *actor)
{
	int i;
	int an, an90;
	fixed_t dist;
	mobj_t *mo;
	
	A_UnHideThing(actor);
	S_StartSound(actor, SFX_CLERIC_FLAME_EXPLODE);
	if(BlockingMobj && BlockingMobj->flags&MF_SHOOTABLE)
	{ // Hit something, so spawn the flame circle around the thing
		dist = BlockingMobj->radius+18*FRACUNIT;
		for(i = 0; i < 4; i++)
		{
			an = (i*ANG45)>>ANGLETOFINESHIFT;
			an90 = (i*ANG45+ANG90)>>ANGLETOFINESHIFT;
			mo = P_SpawnMobj(BlockingMobj->x+FixedMul(dist, finecosine[an]),
				BlockingMobj->y+FixedMul(dist, finesine[an]), 
				BlockingMobj->z+5*FRACUNIT, MT_CIRCLEFLAME);
			if(mo)
			{
				mo->angle = an<<ANGLETOFINESHIFT;
				mo->target = actor->target;
				mo->momx = mo->special1 = FixedMul(FLAMESPEED, finecosine[an]);
				mo->momy = mo->special2 = FixedMul(FLAMESPEED, finesine[an]);
				mo->tics -= P_Random()&3;
			}
			mo = P_SpawnMobj(BlockingMobj->x-FixedMul(dist, finecosine[an]),
				BlockingMobj->y-FixedMul(dist, finesine[an]), 
				BlockingMobj->z+5*FRACUNIT, MT_CIRCLEFLAME);
			if(mo)
			{
				mo->angle = ANG180+(an<<ANGLETOFINESHIFT);
				mo->target = actor->target;
				mo->momx = mo->special1 = FixedMul(-FLAMESPEED, 
					finecosine[an]);
				mo->momy = mo->special2 = FixedMul(-FLAMESPEED, finesine[an]);
				mo->tics -= P_Random()&3;
			}
		}
		P_SetMobjState(actor, S_FLAMEPUFF2_1);
	}
}

//============================================================================
//
// A_CFlameRotate
//
//============================================================================

#define FLAMEROTSPEED	2*FRACUNIT

void A_CFlameRotate(mobj_t *actor)
{
	int an;

	an = (actor->angle+ANG90)>>ANGLETOFINESHIFT;
	actor->momx = actor->special1+FixedMul(FLAMEROTSPEED, finecosine[an]);
	actor->momy = actor->special2+FixedMul(FLAMEROTSPEED, finesine[an]);
	actor->angle += ANG90/15;
}


//============================================================================
//
// A_CHolyAttack3
//
// 	Spawns the spirits
//============================================================================

void A_CHolyAttack3(mobj_t *actor)
{
	P_SpawnMissile(actor, actor->target, MT_HOLY_MISSILE);
	S_StartSound(actor, SFX_CHOLY_FIRE);
}


//============================================================================
//
// A_CHolyAttack2 
//
// 	Spawns the spirits
//============================================================================

void A_CHolyAttack2(mobj_t *actor)
{
	int j;
	int i;
	mobj_t *mo;
	mobj_t *tail, *next;

	for(j = 0; j < 4; j++)
	{
		mo = P_SpawnMobj(actor->x, actor->y, actor->z, MT_HOLY_FX);
		if(!mo)
		{
			continue;
		}
		switch(j)
		{ // float bob index
			case 0:
				mo->special2 = P_Random()&7; // upper-left
				break;
			case 1:
				mo->special2 = 32+(P_Random()&7); // upper-right
				break;
			case 2:
				mo->special2 = (32+(P_Random()&7))<<16; // lower-left
				break;
			case 3:
				mo->special2 = ((32+(P_Random()&7))<<16)+32+(P_Random()&7);
				break;
		}
		mo->z = actor->z;
		mo->angle = actor->angle+(ANGLE_45+ANGLE_45/2)-ANGLE_45*j;
		P_ThrustMobj(mo, mo->angle, mo->info->speed);
		mo->target = actor->target;
		mo->args[0] = 10; // initial turn value
		mo->args[1] = 0; // initial look angle
		if(deathmatch)
		{ // Ghosts last slightly less longer in DeathMatch
			mo->health = 85;
		}
		if(linetarget)
		{
			mo->special1 = (int)linetarget;
			mo->flags |= MF_NOCLIP|MF_SKULLFLY;
			mo->flags &= ~MF_MISSILE;
		}
		tail = P_SpawnMobj(mo->x, mo->y, mo->z, MT_HOLY_TAIL);
		tail->special2 = (int)mo; // parent
		for(i = 1; i < 3; i++)
		{
			next = P_SpawnMobj(mo->x, mo->y, mo->z, MT_HOLY_TAIL);
			P_SetMobjState(next, next->info->spawnstate+1);
			tail->special1 = (int)next;
			tail = next;
		}
		tail->special1 = 0; // last tail bit
	}
}

//============================================================================
//
// A_CHolyAttack
//
//============================================================================

void A_CHolyAttack(player_t *player, pspdef_t *psp)
{
	mobj_t *mo;

	player->mana[MANA_1] -= WeaponManaUse[player->class][player->readyweapon];
	player->mana[MANA_2] -= WeaponManaUse[player->class][player->readyweapon];
	mo = P_SpawnPlayerMissile(player->mo, MT_HOLY_MISSILE);
	if(player == &players[consoleplayer])
	{
		player->damagecount = 0;
		player->bonuscount = 0;
		I_SetPalette((byte *)W_CacheLumpNum(W_GetNumForName("playpal"),
			PU_CACHE)+STARTHOLYPAL*768);
	}
	S_StartSound(player->mo, SFX_CHOLY_FIRE);
}

//============================================================================
//
// A_CHolyPalette
//
//============================================================================

void A_CHolyPalette(player_t *player, pspdef_t *psp)
{
	int pal;

	if(player == &players[consoleplayer])
	{
		pal = STARTHOLYPAL+psp->state-(&states[S_CHOLYATK_6]);
		if(pal == STARTHOLYPAL+3)
		{ // reset back to original playpal
			pal = 0;
		}
		I_SetPalette((byte *)W_CacheLumpNum(W_GetNumForName("playpal"),
			PU_CACHE)+pal*768);
	}
}

//============================================================================
//
// CHolyFindTarget
//
//============================================================================

static void CHolyFindTarget(mobj_t *actor)
{
	mobj_t *target;

	if(target = P_RoughMonsterSearch(actor, 6))
	{
		actor->special1 = (int)target;
		actor->flags |= MF_NOCLIP|MF_SKULLFLY;
		actor->flags &= ~MF_MISSILE;
	}
}

//============================================================================
//
// CHolySeekerMissile
//
// 	 Similar to P_SeekerMissile, but seeks to a random Z on the target
//============================================================================

static void CHolySeekerMissile(mobj_t *actor, angle_t thresh, angle_t turnMax)
{
	int dir;
	int dist;
	angle_t delta;
	angle_t angle;
	mobj_t *target;
	fixed_t newZ;
	fixed_t deltaZ;

	target = (mobj_t *)actor->special1;
	if(target == NULL)
	{
		return;
	}
	if(!(target->flags&MF_SHOOTABLE) 
	|| (!(target->flags&MF_COUNTKILL) && !target->player))
	{ // Target died/target isn't a player or creature
		actor->special1 = 0;
		actor->flags &= ~(MF_NOCLIP|MF_SKULLFLY);
		actor->flags |= MF_MISSILE;
		CHolyFindTarget(actor);
		return;
	}
	dir = P_FaceMobj(actor, target, &delta);
	if(delta > thresh)
	{
		delta >>= 1;
		if(delta > turnMax)
		{
			delta = turnMax;
		}
	}
	if(dir)
	{ // Turn clockwise
		actor->angle += delta;
	}
	else
	{ // Turn counter clockwise
		actor->angle -= delta;
	}
	angle = actor->angle>>ANGLETOFINESHIFT;
	actor->momx = FixedMul(actor->info->speed, finecosine[angle]);
	actor->momy = FixedMul(actor->info->speed, finesine[angle]);
	if(!(leveltime&15) 
		|| actor->z > target->z+(target->height)
		|| actor->z+actor->height < target->z)
	{
		newZ = target->z+((P_Random()*target->height)>>8);
		deltaZ = newZ-actor->z;
		if(abs(deltaZ) > 15*FRACUNIT)
		{
			if(deltaZ > 0)
			{
				deltaZ = 15*FRACUNIT;
			}
			else
			{
				deltaZ = -15*FRACUNIT;
			}
		}
		dist = P_AproxDistance(target->x-actor->x, target->y-actor->y);
		dist = dist/actor->info->speed;
		if(dist < 1)
		{
			dist = 1;
		}
		actor->momz = deltaZ/dist;
	}
	return;
}

//============================================================================
//
// A_CHolyWeave
//
//============================================================================

static void CHolyWeave(mobj_t *actor)
{
	fixed_t newX, newY;
	int weaveXY, weaveZ;
	int angle;

	weaveXY = actor->special2>>16;
	weaveZ = actor->special2&0xFFFF;
	angle = (actor->angle+ANG90)>>ANGLETOFINESHIFT;
	newX = actor->x-FixedMul(finecosine[angle], 
		FloatBobOffsets[weaveXY]<<2);
	newY = actor->y-FixedMul(finesine[angle],
		FloatBobOffsets[weaveXY]<<2);
	weaveXY = (weaveXY+(P_Random()%5))&63;
	newX += FixedMul(finecosine[angle], 
		FloatBobOffsets[weaveXY]<<2);
	newY += FixedMul(finesine[angle], 
		FloatBobOffsets[weaveXY]<<2);
	P_TryMove(actor, newX, newY);
	actor->z -= FloatBobOffsets[weaveZ]<<1;
	weaveZ = (weaveZ+(P_Random()%5))&63;
	actor->z += FloatBobOffsets[weaveZ]<<1;	
	actor->special2 = weaveZ+(weaveXY<<16);
}

//============================================================================
//
// A_CHolySeek
//
//============================================================================

void A_CHolySeek(mobj_t *actor)
{
	actor->health--;
	if(actor->health <= 0)
	{
		actor->momx >>= 2;
		actor->momy >>= 2;
		actor->momz = 0;
		P_SetMobjState(actor, actor->info->deathstate);
		actor->tics -= P_Random()&3;
		return;
	}
	if(actor->special1)
	{
		CHolySeekerMissile(actor, actor->args[0]*ANGLE_1,
			actor->args[0]*ANGLE_1*2);
		if(!((leveltime+7)&15))
		{
			actor->args[0] = 5+(P_Random()/20);
		}
	}
	CHolyWeave(actor);
}

//============================================================================
//
// CHolyTailFollow
//
//============================================================================

static void CHolyTailFollow(mobj_t *actor, fixed_t dist)
{
	mobj_t *child;
	int an;
	fixed_t oldDistance, newDistance;

	child = (mobj_t *)actor->special1;
	if(child)
	{
		an = R_PointToAngle2(actor->x, actor->y, child->x, 
			child->y)>>ANGLETOFINESHIFT;
		oldDistance = P_AproxDistance(child->x-actor->x, child->y-actor->y);
		if(P_TryMove(child, actor->x+FixedMul(dist, finecosine[an]), 
			actor->y+FixedMul(dist, finesine[an])))
		{
			newDistance = P_AproxDistance(child->x-actor->x, 
				child->y-actor->y)-FRACUNIT;
			if(oldDistance < FRACUNIT)
			{
				if(child->z < actor->z)
				{
					child->z = actor->z-dist;
				}
				else
				{
					child->z = actor->z+dist;
				}
			}
			else
			{
				child->z = actor->z+FixedMul(FixedDiv(newDistance, 
					oldDistance), child->z-actor->z);
			}
		}
		CHolyTailFollow(child, dist-FRACUNIT);
	}
}

//============================================================================
//
// CHolyTailRemove
//
//============================================================================

static void CHolyTailRemove(mobj_t *actor)
{
	mobj_t *child;

	child = (mobj_t *)actor->special1;
	if(child)
	{
		CHolyTailRemove(child);
	}
	P_RemoveMobj(actor);
}

//============================================================================
//
// A_CHolyTail
//
//============================================================================

void A_CHolyTail(mobj_t *actor)
{
	mobj_t *parent;

	parent = (mobj_t *)actor->special2;

	if(parent)
	{
		if(parent->state >= &states[parent->info->deathstate])
		{ // Ghost removed, so remove all tail parts
			CHolyTailRemove(actor);
			return;
		}
		else if(P_TryMove(actor, parent->x-FixedMul(14*FRACUNIT,
			finecosine[parent->angle>>ANGLETOFINESHIFT]),
			parent->y-FixedMul(14*FRACUNIT, 
			finesine[parent->angle>>ANGLETOFINESHIFT])))
		{
			actor->z = parent->z-5*FRACUNIT;
		}
		CHolyTailFollow(actor, 10*FRACUNIT);
	}
}

//============================================================================
//
// A_CHolyCheckScream
//
//============================================================================

void A_CHolyCheckScream(mobj_t *actor)
{
	A_CHolySeek(actor);
	if(P_Random() < 20)
	{
		S_StartSound(actor, SFX_SPIRIT_ACTIVE);
	}
	if(!actor->special1)
	{
		CHolyFindTarget(actor);
	}
}

//============================================================================
//
// A_CHolySpawnPuff
//
//============================================================================

void A_CHolySpawnPuff(mobj_t *actor)
{
	P_SpawnMobj(actor->x, actor->y, actor->z, MT_HOLY_MISSILE_PUFF);
}

//----------------------------------------------------------------------------
//
// PROC A_FireConePL1
//
//----------------------------------------------------------------------------

#define SHARDSPAWN_LEFT		1
#define SHARDSPAWN_RIGHT	2
#define SHARDSPAWN_UP		4
#define SHARDSPAWN_DOWN		8

void A_FireConePL1(player_t *player, pspdef_t *psp)
{
	angle_t angle;
	int damage;
	int slope;
	int i;
	mobj_t *pmo,*mo;
	int conedone=false;

	pmo = player->mo;
	player->mana[MANA_1] -= WeaponManaUse[player->class][player->readyweapon];
	S_StartSound(pmo, SFX_MAGE_SHARDS_FIRE);

	damage = 90+(P_Random()&15);
	for(i = 0; i < 16; i++)
	{
		angle = pmo->angle+i*(ANG45/16);
		slope = P_AimLineAttack(pmo, angle, MELEERANGE);
		if(linetarget)
		{
			pmo->flags2 |= MF2_ICEDAMAGE;
			P_DamageMobj(linetarget, pmo, pmo, damage);
			pmo->flags2 &= ~MF2_ICEDAMAGE;
			conedone = true;
			break;
		}
	}

	// didn't find any creatures, so fire projectiles
	if (!conedone)
	{
		mo = P_SpawnPlayerMissile(pmo, MT_SHARDFX1);
		if (mo)
		{
			mo->special1 = SHARDSPAWN_LEFT|SHARDSPAWN_DOWN|SHARDSPAWN_UP
				|SHARDSPAWN_RIGHT;
			mo->special2 = 3; // Set sperm count (levels of reproductivity)
			mo->target = pmo;
			mo->args[0] = 3;		// Mark Initial shard as super damage
		}
	}
}

void A_ShedShard(mobj_t *actor)
{
	mobj_t *mo;
	int spawndir = actor->special1;
	int spermcount = actor->special2;

	if (spermcount <= 0) return;				// No sperm left
	actor->special2 = 0;
	spermcount--;

	// every so many calls, spawn a new missile in it's set directions
	if (spawndir & SHARDSPAWN_LEFT)
	{
		mo=P_SpawnMissileAngleSpeed(actor, MT_SHARDFX1, actor->angle+(ANG45/9),
											 0, (20+2*spermcount)<<FRACBITS);
		if (mo)
		{
			mo->special1 = SHARDSPAWN_LEFT;
			mo->special2 = spermcount;
			mo->momz = actor->momz;
			mo->target = actor->target;
			mo->args[0] = (spermcount==3)?2:0;
		}
	}
	if (spawndir & SHARDSPAWN_RIGHT)
	{
		mo=P_SpawnMissileAngleSpeed(actor, MT_SHARDFX1, actor->angle-(ANG45/9),
											 0, (20+2*spermcount)<<FRACBITS);
		if (mo)
		{
			mo->special1 = SHARDSPAWN_RIGHT;
			mo->special2 = spermcount;
			mo->momz = actor->momz;
			mo->target = actor->target;
			mo->args[0] = (spermcount==3)?2:0;
		}
	}
	if (spawndir & SHARDSPAWN_UP)
	{
		mo=P_SpawnMissileAngleSpeed(actor, MT_SHARDFX1, actor->angle, 
											 0, (15+2*spermcount)<<FRACBITS);
		if (mo)
		{
			mo->momz = actor->momz;
			mo->z += 8*FRACUNIT;
			if (spermcount & 1)			// Every other reproduction
				mo->special1 = SHARDSPAWN_UP | SHARDSPAWN_LEFT | SHARDSPAWN_RIGHT;
			else
				mo->special1 = SHARDSPAWN_UP;
			mo->special2 = spermcount;
			mo->target = actor->target;
			mo->args[0] = (spermcount==3)?2:0;
		}
	}
	if (spawndir & SHARDSPAWN_DOWN)
	{
		mo=P_SpawnMissileAngleSpeed(actor, MT_SHARDFX1, actor->angle, 
											 0, (15+2*spermcount)<<FRACBITS);
		if (mo)
		{
			mo->momz = actor->momz;
			mo->z -= 4*FRACUNIT;
			if (spermcount & 1)			// Every other reproduction
				mo->special1 = SHARDSPAWN_DOWN | SHARDSPAWN_LEFT | SHARDSPAWN_RIGHT;
			else
				mo->special1 = SHARDSPAWN_DOWN;
			mo->special2 = spermcount;
			mo->target = actor->target;
			mo->args[0] = (spermcount==3)?2:0;
		}
	}
}
#endif

void A_Light0(player_t *player, pspdef_t *psp)
{
  player->extralight = 0;
}

void A_Light1 (player_t *player, pspdef_t *psp)
{
  player->extralight = 1;
}

void A_Light2 (player_t *player, pspdef_t *psp)
{
  player->extralight = 2;
}

//
// A_BFGSpray
// Spawn a BFG explosion on every monster in view
//

void A_BFGSpray(void *m)
{
  mobj_t *mo = (mobj_t *)m;
  int i;

  for (i=0 ; i<40 ; i++)  // offset angles from its attack angle
    {
      int j, damage;
      angle_t an = mo->angle - ANG90/2 + ANG90/40*i;

      // mo->target is the originator (player) of the missile

      // killough 8/2/98: make autoaiming prefer enemies
      if (!mbf_features ||
         (P_AimLineAttack(mo->target, an, 16*64*FRACUNIT, MF_FRIEND),
         !linetarget))
        P_AimLineAttack(mo->target, an, 16*64*FRACUNIT, 0);

      if (!linetarget)
        continue;

      P_SpawnMobj(linetarget->x, linetarget->y,
                  linetarget->z + (linetarget->height>>2), MT_EXTRABFG);

      for (damage=j=0; j<15; j++)
        damage += (P_Random(pr_bfg)&7) + 1;

      P_DamageMobj(linetarget, mo->target, mo->target, damage);
    }
}

//
// A_BFGsound
//

void A_BFGsound(player_t *player, pspdef_t *psp)
{
  S_StartSound(player->mo, sfx_bfg);
}

//
// P_SetupPsprites
// Called at start of level for each player.
//

void P_SetupPsprites(player_t *player)
{
  int i;

  /* remove all psprites */
  for (i=0; i<NUMPSPRITES; i++)
    player->psprites[i].state = NULL;

  /* spawn the ready weapon */
  player->pendingweapon = player->readyweapon;
  P_BringUpWeapon(player);
}

/*
 * P_MovePsprites
 * Called every tic by player thinking routine.
*/
void P_MovePsprites(player_t *player)
{
  int i;
  pspdef_t *psp  = player->psprites;

  /* a null state means not active
   * drop tic count and possibly change state
   * a -1 tic count never changes */

  for (i=0; i<NUMPSPRITES; i++, psp++)
  {
     state_t *state = psp->state;

     if(state == 0) /* a null state means not active */
        continue;

     /* drop tic count and possibly change state */
     if (psp->tics != -1)
     {
        --psp->tics;
        if (!psp->tics)
           P_SetPsprite(player, i, psp->state->nextstate);
     }
  }

  player->psprites[ps_flash].sx = player->psprites[ps_weapon].sx;
  player->psprites[ps_flash].sy = player->psprites[ps_weapon].sy;
}

/* ====================================================================
 * MBF21 codepointers (weapon side)
 *
 * Read parameters from the calling psprite state's args[] and are inert
 * unless mbf21_features is active.  Live here (rather than a separate TU)
 * for access to the static P_SetPsprite / bulletslope / P_BulletSlope /
 * P_GunShot helpers.  Mechanics follow the MBF21 spec.
 * ==================================================================== */

void A_WeaponProjectile(player_t *player, pspdef_t *psp)
{
  int type, angle, pitch, spawnofs_xy, spawnofs_z, an;
  mobj_t *mo;

  if (!mbf21_features || !psp->state || !psp->state->args[0])
    return;

  type        = psp->state->args[0] - 1;
  angle       = psp->state->args[1];
  pitch       = psp->state->args[2];
  spawnofs_xy = psp->state->args[3];
  spawnofs_z  = psp->state->args[4];

  mo = P_SpawnPlayerMissile(player->mo, type);
  if (!mo)
    return;

  mo->angle += (unsigned int)(((int64_t)angle << 16) / 360);
  an = mo->angle >> ANGLETOFINESHIFT;
  mo->momx = FixedMul(mo->info->speed, finecosine[an]);
  mo->momy = FixedMul(mo->info->speed, finesine[an]);
  mo->momz += FixedMul(mo->info->speed, DegToSlope(pitch));

  an = (player->mo->angle - ANG90) >> ANGLETOFINESHIFT;
  mo->x += FixedMul(spawnofs_xy, finecosine[an]);
  mo->y += FixedMul(spawnofs_xy, finesine[an]);
  mo->z += spawnofs_z;

  P_SetTarget(&mo->tracer, linetarget);
}

void A_WeaponBulletAttack(player_t *player, pspdef_t *psp)
{
  int hspread, vspread, numbullets, damagebase, damagemod;
  int i, damage, angle, slope;

  if (!mbf21_features || !psp->state)
    return;

  hspread    = psp->state->args[0];
  vspread    = psp->state->args[1];
  numbullets = psp->state->args[2];
  damagebase = psp->state->args[3];
  damagemod  = psp->state->args[4];

  P_BulletSlope(player->mo);

  for (i = 0; i < numbullets; i++)
  {
    /* MBF21 allows damagemod == 0 (deterministic damage); guard the
     * modulo so a WAD-supplied 0 cannot raise SIGFPE.  mod 0 => no spread. */
    int dmgrand = (damagemod > 0) ? (P_Random(pr_mbf21) % damagemod) : 0;
    damage = (dmgrand + 1) * damagebase;
    angle  = (int)player->mo->angle + P_RandomHitscanAngle(pr_mbf21, hspread);
    slope  = bulletslope + P_RandomHitscanSlope(pr_mbf21, vspread);
    P_LineAttack(player->mo, angle, MISSILERANGE, slope, damage);
  }
}

void A_WeaponMeleeAttack(player_t *player, pspdef_t *psp)
{
  int damagebase, damagemod, zerkfactor, hitsound, range;
  angle_t angle;
  int t, slope, damage;

  if (!mbf21_features || !psp->state)
    return;

  damagebase = psp->state->args[0];
  damagemod  = psp->state->args[1];
  zerkfactor = psp->state->args[2];
  hitsound   = psp->state->args[3];
  range      = psp->state->args[4];

  if (range == 0)
    range = player->mo->info->meleerange;

  /* MBF21 allows damagemod == 0 (deterministic damage); guard the modulo. */
  damage = ((damagemod > 0 ? (P_Random(pr_mbf21) % damagemod) : 0) + 1) * damagebase;
  if (player->powers[pw_strength])
    damage = (damage * zerkfactor) >> FRACBITS;

  angle = player->mo->angle;
  t = P_Random(pr_mbf21);
  angle += (t - P_Random(pr_mbf21)) << 18;

  /* prefer enemies for autoaim */
  slope = P_AimLineAttack(player->mo, angle, range, MF_FRIEND);
  if (!linetarget)
    slope = P_AimLineAttack(player->mo, angle, range, 0);

  P_LineAttack(player->mo, angle, range, slope, damage);

  if (!linetarget)
    return;

  S_StartSound(player->mo, hitsound);
  player->mo->angle = R_PointToAngle2(player->mo->x, player->mo->y,
                                      linetarget->x, linetarget->y);
}

void A_WeaponSound(player_t *player, pspdef_t *psp)
{
  if (!mbf21_features || !psp->state)
    return;
  S_StartSound(psp->state->args[1] ? NULL : player->mo, psp->state->args[0]);
}

void A_WeaponAlert(player_t *player, pspdef_t *psp)
{
  if (!mbf21_features)
    return;
  P_NoiseAlert(player->mo, player->mo);
}

void A_ConsumeAmmo(player_t *player, pspdef_t *psp)
{
  int amount;
  ammotype_t type;

  if (!mbf21_features)
    return;

  type = weaponinfo[player->readyweapon].ammo;
  if (!psp->state || type == AM_NOAMMO)
    return;

  if (psp->state->args[0] != 0)
    amount = psp->state->args[0];
  else
    amount = weaponinfo[player->readyweapon].ammopershot;

  if (player->ammo[type] >= amount)
    player->ammo[type] -= amount;
  else
    player->ammo[type] = 0;
}

void A_CheckAmmo(player_t *player, pspdef_t *psp)
{
  int amount;
  ammotype_t type;

  if (!mbf21_features)
    return;

  type = weaponinfo[player->readyweapon].ammo;
  if (!psp->state || type == AM_NOAMMO)
    return;

  if (psp->state->args[1] != 0)
    amount = psp->state->args[1];
  else
    amount = weaponinfo[player->readyweapon].ammopershot;

  if (player->ammo[type] < amount)
    P_SetPsprite(player, ps_weapon, psp->state->args[0]);
}

void A_RefireTo(player_t *player, pspdef_t *psp)
{
  if (!mbf21_features || !psp->state)
    return;

  if ((psp->state->args[1] || P_CheckAmmo(player)) &&
      (player->cmd.buttons & BT_ATTACK) &&
      (player->pendingweapon == WP_NOCHANGE && player->health))
    P_SetPsprite(player, ps_weapon, psp->state->args[0]);
}

void A_GunFlashTo(player_t *player, pspdef_t *psp)
{
  if (!mbf21_features || !psp->state)
    return;

  if (!psp->state->args[1])
    P_SetMobjState(player->mo, S_PLAY_ATK2);

  P_SetPsprite(player, ps_flash, psp->state->args[0]);
}

/* ====================================================================
 * Heretic weapon codepointers (ported from dsda-doom)
 *
 * Live here for access to the static P_SetPsprite / P_BulletSlope /
 * bulletslope / PuffType helpers, following the same convention as the
 * MBF21 weapon side above.  Inert unless a Heretic game is active.
 * ==================================================================== */

#include "heretic/p_action.h"

extern mobjtype_t PuffType;
extern mobj_t *MissileMobj;
extern void P_BlasterMobjThinker(mobj_t *mobj);
void A_FireMacePL1B(player_t *player, pspdef_t *psp);

#define FOOTCLIPSIZE (10*FRACUNIT)
#define FLAME_THROWER_TICS (10*35)
#define HITDICE(a) ((1 + (P_Random(pr_heretic) & 7)) * (a))

/* Heretic has no free-look pitch in this core; mirror the small helpers
 * from heretic/p_action.c (they are file-static there). */
static int p_pspr_PlayerLookDir(player_t *player) { return player->lookdir; }
static fixed_t p_pspr_PlayerSlope(player_t *player) { return (player->lookdir << FRACBITS) / 173; }
#define dsda_PlayerLookDir p_pspr_PlayerLookDir
#define dsda_PlayerSlope   p_pspr_PlayerSlope

/* Heretic ammo types (separate space from this core's Doom AM_* enum). */

/* Heretic per-shot ammo costs. */
#define USE_GWND_AMMO_1 1
#define USE_GWND_AMMO_2 1
#define USE_CBOW_AMMO_1 1
#define USE_CBOW_AMMO_2 1
#define USE_BLSR_AMMO_1 1
#define USE_BLSR_AMMO_2 5
#define USE_SKRD_AMMO_1 1
#define USE_SKRD_AMMO_2 5
#define USE_PHRD_AMMO_1 1
#define USE_PHRD_AMMO_2 1
#define USE_MACE_AMMO_1 1
#define USE_MACE_AMMO_2 5



void A_BeakAttackPL1(player_t * player, pspdef_t * psp)
{
    angle_t angle;
    int damage;
    int slope;

    damage = 1 + (P_Random(pr_heretic) & 3);
    angle = player->mo->angle;
    slope = P_AimLineAttack(player->mo, angle, MELEERANGE, 0);
    PuffType = HERETIC_MT_BEAKPUFF;
    P_LineAttack(player->mo, angle, MELEERANGE, slope, damage);
    if (linetarget)
    {
        player->mo->angle = R_PointToAngle2(player->mo->x,
                                            player->mo->y, linetarget->x,
                                            linetarget->y);
    }
    S_StartMobjSound(player->mo, heretic_sfx_chicpk1 + (P_Random(pr_heretic) % 3));
    player->chickenPeck = 12;
    psp->tics -= P_Random(pr_heretic) & 7;
}


void A_BeakAttackPL2(player_t * player, pspdef_t * psp)
{
    angle_t angle;
    int damage;
    int slope;

    damage = HITDICE(4);
    angle = player->mo->angle;
    slope = P_AimLineAttack(player->mo, angle, MELEERANGE, 0);
    PuffType = HERETIC_MT_BEAKPUFF;
    P_LineAttack(player->mo, angle, MELEERANGE, slope, damage);
    if (linetarget)
    {
        player->mo->angle = R_PointToAngle2(player->mo->x,
                                            player->mo->y, linetarget->x,
                                            linetarget->y);
    }
    S_StartMobjSound(player->mo, heretic_sfx_chicpk1 + (P_Random(pr_heretic) % 3));
    player->chickenPeck = 12;
    psp->tics -= P_Random(pr_heretic) & 3;
}


void A_BeakRaise(player_t * player, pspdef_t * psp)
{
    psp->sy = WEAPONTOP;
    P_SetPsprite(player, ps_weapon,
                 weaponinfo[player->readyweapon].readystate);
}


void A_BeakReady(player_t * player, pspdef_t * psp)
{
    if (player->cmd.buttons & BT_ATTACK)
    {                           // Chicken beak attack
        player->attackdown = true;
        P_SetMobjState(player->mo, HERETIC_S_CHICPLAY_ATK1);
        if (player->powers[pw_weaponlevel2])
        {
            P_SetPsprite(player, ps_weapon, HERETIC_S_BEAKATK2_1);
        }
        else
        {
            P_SetPsprite(player, ps_weapon, HERETIC_S_BEAKATK1_1);
        }
        P_NoiseAlert(player->mo, player->mo);
    }
    else
    {
        if (player->mo->state == &states[HERETIC_S_CHICPLAY_ATK1])
        {                       // Take out of attack state
            P_SetMobjState(player->mo, HERETIC_S_CHICPLAY);
        }
        player->attackdown = false;
    }
}


void A_FireBlasterPL1(player_t * player, pspdef_t * psp)
{
    mobj_t *mo;
    angle_t angle;
    int damage;

    mo = player->mo;
    S_StartMobjSound(mo, heretic_sfx_gldhit);
    player->ammo[am_blaster] -= USE_BLSR_AMMO_1;
    P_BulletSlope(mo);
    damage = HITDICE(4);
    angle = mo->angle;
    if (player->refire)
    {
        angle += P_SubRandom() << 18;
    }
    PuffType = HERETIC_MT_BLASTERPUFF1;
    P_LineAttack(mo, angle, MISSILERANGE, bulletslope, damage);
    S_StartMobjSound(player->mo, heretic_sfx_blssht);
}


void A_FireBlasterPL2(player_t * player, pspdef_t * psp)
{
    mobj_t *mo;

    player->ammo[am_blaster] -=
        deathmatch ? USE_BLSR_AMMO_1 : USE_BLSR_AMMO_2;
    mo = P_SpawnPlayerMissile(player->mo, HERETIC_MT_BLASTERFX1);
    if (mo)
    {
        mo->thinker.function.arg1 = (void (*)(void *))P_BlasterMobjThinker;
    }
    S_StartMobjSound(player->mo, heretic_sfx_blssht);
}


void A_FireCrossbowPL1(player_t * player, pspdef_t * psp)
{
    mobj_t *pmo;

    pmo = player->mo;
    player->ammo[am_crossbow] -= USE_CBOW_AMMO_1;
    P_SpawnPlayerMissile(pmo, HERETIC_MT_CRBOWFX1);
    P_SPMAngle(pmo, HERETIC_MT_CRBOWFX3, pmo->angle - (ANG45 / 10));
    P_SPMAngle(pmo, HERETIC_MT_CRBOWFX3, pmo->angle + (ANG45 / 10));
}


void A_FireCrossbowPL2(player_t * player, pspdef_t * psp)
{
    mobj_t *pmo;

    pmo = player->mo;
    player->ammo[am_crossbow] -=
        deathmatch ? USE_CBOW_AMMO_1 : USE_CBOW_AMMO_2;
    P_SpawnPlayerMissile(pmo, HERETIC_MT_CRBOWFX2);
    P_SPMAngle(pmo, HERETIC_MT_CRBOWFX2, pmo->angle - (ANG45 / 10));
    P_SPMAngle(pmo, HERETIC_MT_CRBOWFX2, pmo->angle + (ANG45 / 10));
    P_SPMAngle(pmo, HERETIC_MT_CRBOWFX3, pmo->angle - (ANG45 / 5));
    P_SPMAngle(pmo, HERETIC_MT_CRBOWFX3, pmo->angle + (ANG45 / 5));
}


void A_FireGoldWandPL1(player_t * player, pspdef_t * psp)
{
    mobj_t *mo;
    angle_t angle;
    int damage;

    mo = player->mo;
    player->ammo[am_goldwand] -= USE_GWND_AMMO_1;
    P_BulletSlope(mo);
    damage = 7 + (P_Random(pr_heretic) & 7);
    angle = mo->angle;
    if (player->refire)
    {
        angle += P_SubRandom() << 18;
    }
    PuffType = HERETIC_MT_GOLDWANDPUFF1;
    P_LineAttack(mo, angle, MISSILERANGE, bulletslope, damage);
    S_StartMobjSound(player->mo, heretic_sfx_gldhit);
}


void A_FireGoldWandPL2(player_t * player, pspdef_t * psp)
{
    int i;
    mobj_t *mo;
    angle_t angle;
    int damage;
    fixed_t momz;

    mo = player->mo;
    player->ammo[am_goldwand] -=
        deathmatch ? USE_GWND_AMMO_1 : USE_GWND_AMMO_2;
    PuffType = HERETIC_MT_GOLDWANDPUFF2;
    P_BulletSlope(mo);
    momz = FixedMul(mobjinfo[HERETIC_MT_GOLDWANDFX2].speed, bulletslope);
    P_SpawnMissileAngle(mo, HERETIC_MT_GOLDWANDFX2, mo->angle - (ANG45 / 8), momz);
    P_SpawnMissileAngle(mo, HERETIC_MT_GOLDWANDFX2, mo->angle + (ANG45 / 8), momz);
    angle = mo->angle - (ANG45 / 8);
    for (i = 0; i < 5; i++)
    {
        damage = 1 + (P_Random(pr_heretic) & 7);
        P_LineAttack(mo, angle, MISSILERANGE, bulletslope, damage);
        angle += ((ANG45 / 8) * 2) / 4;
    }
    S_StartMobjSound(player->mo, heretic_sfx_gldhit);
}


void A_FireMacePL1(player_t * player, pspdef_t * psp)
{
    mobj_t *ball;

    if (P_Random(pr_heretic) < 28)
    {
        A_FireMacePL1B(player, psp);
        return;
    }
    if (player->ammo[am_mace] < USE_MACE_AMMO_1)
    {
        return;
    }
    player->ammo[am_mace] -= USE_MACE_AMMO_1;
    psp->sx = ((P_Random(pr_heretic) & 3) - 2) * FRACUNIT;
    psp->sy = WEAPONTOP + (P_Random(pr_heretic) & 3) * FRACUNIT;
    ball = P_SPMAngle(player->mo, HERETIC_MT_MACEFX1, player->mo->angle
                      + (((P_Random(pr_heretic) & 7) - 4) << 24));
    if (ball)
    {
        ball->special1.i = 16;    // tics till dropoff
    }
}


void A_FireMacePL2(player_t * player, pspdef_t * psp)
{
    mobj_t *mo;

    player->ammo[am_mace] -= deathmatch ? USE_MACE_AMMO_1 : USE_MACE_AMMO_2;
    mo = P_SpawnPlayerMissile(player->mo, HERETIC_MT_MACEFX4);
    if (mo)
    {
        mo->momx += player->mo->momx;
        mo->momy += player->mo->momy;
        mo->momz = 2 * FRACUNIT + (dsda_PlayerLookDir(player) << (FRACBITS - 5));
        if (linetarget)
        {
            P_SetTarget(&mo->special1.m, linetarget);
        }
    }
    S_StartMobjSound(player->mo, heretic_sfx_lobsht);
}


void A_FirePhoenixPL1(player_t * player, pspdef_t * psp)
{
    angle_t angle;

    player->ammo[am_phoenixrod] -= USE_PHRD_AMMO_1;
    P_SpawnPlayerMissile(player->mo, HERETIC_MT_PHOENIXFX1);
    angle = player->mo->angle + ANG180;
    angle >>= ANGLETOFINESHIFT;
    player->mo->momx += FixedMul(4 * FRACUNIT, finecosine[angle]);
    player->mo->momy += FixedMul(4 * FRACUNIT, finesine[angle]);
}


void A_FirePhoenixPL2(player_t * player, pspdef_t * psp)
{
    mobj_t *mo;
    mobj_t *pmo;
    angle_t angle;
    fixed_t x, y, z;
    fixed_t slope;

    if (--player->flamecount == 0)
    {                           // Out of flame
        P_SetPsprite(player, ps_weapon, HERETIC_S_PHOENIXATK2_4);
        player->refire = 0;
        return;
    }
    pmo = player->mo;
    angle = pmo->angle;
    x = pmo->x + (P_SubRandom() << 9);
    y = pmo->y + (P_SubRandom() << 9);
    z = pmo->z + 26 * FRACUNIT + dsda_PlayerSlope(player);
    if (pmo->flags2 & MF2_FEETARECLIPPED)
    {
        z -= FOOTCLIPSIZE;
    }
    slope = dsda_PlayerSlope(player) + (FRACUNIT / 10);
    mo = P_SpawnMobj(x, y, z, HERETIC_MT_PHOENIXFX2);
    P_SetTarget(&mo->target, pmo);
    mo->angle = angle;
    mo->momx = pmo->momx + FixedMul(mo->info->speed,
                                    finecosine[angle >> ANGLETOFINESHIFT]);
    mo->momy = pmo->momy + FixedMul(mo->info->speed,
                                    finesine[angle >> ANGLETOFINESHIFT]);
    mo->momz = FixedMul(mo->info->speed, slope);
    if (!player->refire || !(leveltime % 38))
    {
        S_StartMobjSound(player->mo, heretic_sfx_phopow);
    }
    P_CheckMissileSpawn(mo);
}


void A_FireSkullRodPL1(player_t * player, pspdef_t * psp)
{
    mobj_t *mo;

    if (player->ammo[am_skullrod] < USE_SKRD_AMMO_1)
    {
        return;
    }
    player->ammo[am_skullrod] -= USE_SKRD_AMMO_1;
    mo = P_SpawnPlayerMissile(player->mo, HERETIC_MT_HORNRODFX1);
    // Randomize the first frame
    if (mo && P_Random(pr_heretic) > 128)
    {
        P_SetMobjState(mo, HERETIC_S_HRODFX1_2);
    }
}


void A_FireSkullRodPL2(player_t * player, pspdef_t * psp)
{
    player->ammo[am_skullrod] -=
        deathmatch ? USE_SKRD_AMMO_1 : USE_SKRD_AMMO_2;
    P_SpawnPlayerMissile(player->mo, HERETIC_MT_HORNRODFX2);
    // Use MissileMobj instead of the return value from
    // P_SpawnPlayerMissile because we need to give info to the mobj
    // even if it exploded immediately.
    if (netgame)
    {                           // Multi-player game
        MissileMobj->special2.i = P_GetPlayerNum(player);
    }
    else
    {                           // Always use red missiles in single player games
        MissileMobj->special2.i = 2;
    }
    if (linetarget)
    {
        P_SetTarget(&MissileMobj->special1.m, linetarget);
    }
    S_StartMobjSound(MissileMobj, heretic_sfx_hrnpow);
}


void A_GauntletAttack(player_t * player, pspdef_t * psp)
{
    angle_t angle;
    int damage;
    int slope;
    int randVal;
    fixed_t dist;

    psp->sx = ((P_Random(pr_heretic) & 3) - 2) * FRACUNIT;
    psp->sy = WEAPONTOP + (P_Random(pr_heretic) & 3) * FRACUNIT;
    angle = player->mo->angle;
    if (player->powers[pw_weaponlevel2])
    {
        damage = HITDICE(2);
        dist = 4 * MELEERANGE;
        angle += P_SubRandom() << 17;
        PuffType = HERETIC_MT_GAUNTLETPUFF2;
    }
    else
    {
        damage = HITDICE(2);
        dist = MELEERANGE + 1;
        angle += P_SubRandom() << 18;
        PuffType = HERETIC_MT_GAUNTLETPUFF1;
    }
    slope = P_AimLineAttack(player->mo, angle, dist, 0);
    P_LineAttack(player->mo, angle, dist, slope, damage);
    if (!linetarget)
    {
        if (P_Random(pr_heretic) > 64)
        {
            player->extralight = !player->extralight;
        }
        S_StartMobjSound(player->mo, heretic_sfx_gntful);
        return;
    }
    randVal = P_Random(pr_heretic);
    if (randVal < 64)
    {
        player->extralight = 0;
    }
    else if (randVal < 160)
    {
        player->extralight = 1;
    }
    else
    {
        player->extralight = 2;
    }
    if (player->powers[pw_weaponlevel2])
    {
        P_GiveBody(player, damage >> 1);
        S_StartMobjSound(player->mo, heretic_sfx_gntpow);
    }
    else
    {
        S_StartMobjSound(player->mo, heretic_sfx_gnthit);
    }
    // turn to face target
    angle = R_PointToAngle2(player->mo->x, player->mo->y,
                            linetarget->x, linetarget->y);
    if (angle - player->mo->angle > ANG180)
    {
        if (angle - player->mo->angle < (angle_t)(-ANG90 / 20))
            player->mo->angle = angle + ANG90 / 21;
        else
            player->mo->angle -= ANG90 / 20;
    }
    else
    {
        if (angle - player->mo->angle > ANG90 / 20)
            player->mo->angle = angle - ANG90 / 21;
        else
            player->mo->angle += ANG90 / 20;
    }
    player->mo->flags |= MF_JUSTATTACKED;
    R_SmoothPlaying_Reset(player); // e6y
}


void A_InitPhoenixPL2(player_t * player, pspdef_t * psp)
{
    player->flamecount = FLAME_THROWER_TICS;
}


void A_ShutdownPhoenixPL2(player_t * player, pspdef_t * psp)
{
    player->ammo[am_phoenixrod] -= USE_PHRD_AMMO_2;
}


void A_StaffAttackPL1(player_t * player, pspdef_t * psp)
{
    angle_t angle;
    int damage;
    int slope;

    damage = 5 + (P_Random(pr_heretic) & 15);
    angle = player->mo->angle;
    angle += P_SubRandom() << 18;
    slope = P_AimLineAttack(player->mo, angle, MELEERANGE, 0);
    PuffType = HERETIC_MT_STAFFPUFF;
    P_LineAttack(player->mo, angle, MELEERANGE, slope, damage);
    if (linetarget)
    {
        //S_StartMobjSound(player->mo, sfx_stfhit);
        // turn to face target
        player->mo->angle = R_PointToAngle2(player->mo->x,
                                            player->mo->y, linetarget->x,
                                            linetarget->y);
        R_SmoothPlaying_Reset(player); // e6y
    }
}


void A_StaffAttackPL2(player_t * player, pspdef_t * psp)
{
    angle_t angle;
    int damage;
    int slope;

    // P_inter.c:P_DamageMobj() handles target momentums
    damage = 18 + (P_Random(pr_heretic) & 63);
    angle = player->mo->angle;
    angle += P_SubRandom() << 18;
    slope = P_AimLineAttack(player->mo, angle, MELEERANGE, 0);
    PuffType = HERETIC_MT_STAFFPUFF2;
    P_LineAttack(player->mo, angle, MELEERANGE, slope, damage);
    if (linetarget)
    {
        //S_StartMobjSound(player->mo, sfx_stfpow);
        // turn to face target
        player->mo->angle = R_PointToAngle2(player->mo->x,
                                            player->mo->y, linetarget->x,
                                            linetarget->y);
        R_SmoothPlaying_Reset(player); // e6y
    }
}


/* P_RepositionMace relocates the mace spawn among the map's mace spots;
 * the mace-spot scan is not implemented in this core yet, so this is a
 * no-op (the mace simply stays put). */
void P_RepositionMace(mobj_t *mo)
{
  (void)mo;
}

void A_FireMacePL1B(player_t * player, pspdef_t * psp)
{
    mobj_t *pmo;
    mobj_t *ball;
    angle_t angle;

    if (player->ammo[am_mace] < USE_MACE_AMMO_1)
    {
        return;
    }
    player->ammo[am_mace] -= USE_MACE_AMMO_1;
    pmo = player->mo;

    // Vanilla bug here:
    // Original code here looks like:
    //   (pmo->flags2 & MF2_FEETARECLIPPED != 0)
    // C's operator precedence interprets this as:
    //   (pmo->flags2 & (MF2_FEETARECLIPPED != 0))
    // Which simplifies to:
    //   (pmo->flags2 & 1)
    ball = P_SpawnMobj(pmo->x, pmo->y, pmo->z + 28 * FRACUNIT
                       - FOOTCLIPSIZE * (pmo->flags2 & 1), HERETIC_MT_MACEFX2);

    ball->momz = 2 * FRACUNIT + (dsda_PlayerLookDir(player) << (FRACBITS - 5));
    angle = pmo->angle;
    P_SetTarget(&ball->target, pmo);
    ball->angle = angle;
    ball->z += dsda_PlayerLookDir(player) << (FRACBITS - 4);
    angle >>= ANGLETOFINESHIFT;
    ball->momx = (pmo->momx >> 1)
        + FixedMul(ball->info->speed, finecosine[angle]);
    ball->momy = (pmo->momy >> 1)
        + FixedMul(ball->info->speed, finesine[angle]);
    S_StartMobjSound(ball, heretic_sfx_lobsht);
    P_CheckMissileSpawn(ball);
}