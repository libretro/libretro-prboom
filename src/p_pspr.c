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
#include "p_inter.h"
#include "p_pspr.h"
#include "p_enemy.h"
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
      if (state->action)
      {
         state->action(player, psp);
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
boolean P_CheckMana(player_t *player)
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

  if (weapon == WP_BFG) // Minimal amount for one shot varies.
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

boolean P_CheckAmmo(player_t *player)
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
  P_SetMobjState(player->mo, S_PLAY_ATK1);
  newstate = weaponinfo[player->readyweapon].atkstate;
#endif

  P_SetPsprite(player, ps_weapon, newstate);
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
   if (player->mo->state == &states[S_PLAY_ATK1]
         || player->mo->state == &states[S_PLAY_ATK2] )
      P_SetMobjState(player->mo, S_PLAY);
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
      if (!player->attackdown || (player->readyweapon != WP_MISSILE &&
               player->readyweapon != WP_BFG))
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
    if (angle - player->mo->angle < -ANG90/20)
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
}

//
// A_FireMissile
//

void A_FireMissile(player_t *player, pspdef_t *psp)
{
  player->ammo[weaponinfo[player->readyweapon].ammo]--;
  P_SpawnPlayerMissile(player->mo, MT_ROCKET);
}

//
// A_FireBFG
//

void A_FireBFG(player_t *player, pspdef_t *psp)
{
  player->ammo[weaponinfo[player->readyweapon].ammo] -= BFGCELLS;
  P_SpawnPlayerMissile(player->mo, MT_BFG);
}

//
// A_FirePlasma
//

void A_FirePlasma(player_t *player, pspdef_t *psp)
{
  player->ammo[weaponinfo[player->readyweapon].ammo]--;

  A_FireSomething(player,P_Random(pr_plasma)&1);              // phares
  P_SpawnPlayerMissile(player->mo, MT_PLASMA);
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

static void P_GunShot(mobj_t *mo, boolean accurate)
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
  player->ammo[weaponinfo[player->readyweapon].ammo]--;

  A_FireSomething(player,0);                                      // phares
  P_BulletSlope(player->mo);
  P_GunShot(player->mo, !player->refire);
}

//
// A_FireShotgun
//

void A_FireShotgun(player_t *player, pspdef_t *psp)
{
  int i;

  S_StartSound(player->mo, sfx_shotgn);
  P_SetMobjState(player->mo, S_PLAY_ATK2);

  player->ammo[weaponinfo[player->readyweapon].ammo]--;

  A_FireSomething(player,0);                                      // phares

  P_BulletSlope(player->mo);

  for (i=0; i<7; i++)
    P_GunShot(player->mo, false);
}

//
// A_FireShotgun2
//

void A_FireShotgun2(player_t *player, pspdef_t *psp)
{
  int i;

  S_StartSound(player->mo, sfx_dshtgn);
  P_SetMobjState(player->mo, S_PLAY_ATK2);
  player->ammo[weaponinfo[player->readyweapon].ammo] -= 2;

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
  player->ammo[weaponinfo[player->readyweapon].ammo]--;

  A_FireSomething(player,psp->state - &states[S_CHAIN1]);           // phares

  P_BulletSlope(player->mo);

  P_GunShot(player->mo, !player->refire);
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

void A_BFGSpray(mobj_t *mo)
{
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
