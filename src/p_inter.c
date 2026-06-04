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
 *      Handling interactions (i.e., collisions).
 *
 *-----------------------------------------------------------------------------*/

#include "doomstat.h"
#include "dstrings.h"
#include "m_random.h"
#include "am_map.h"
#include "r_main.h"
#include "s_sound.h"
#include "sounds.h"
#include "d_deh.h"  // Ty 03/22/98 - externalized strings
#include "p_tick.h"
#include "lprintf.h"

#include "p_inter.h"
#include "hexen/p_acs.h"
#include "hexen/p_spec_hexen.h"
#include "p_enemy.h"

#include "p_inter.h"

#define BONUSADD        6

// Ty 03/07/98 - add deh externals
// Maximums and such were hardcoded values.  Need to externalize those for
// dehacked support (and future flexibility).  Most var names came from the key
// strings used in dehacked.

int initial_health = 100;
int initial_bullets = 50;
int maxhealth = 100; // was MAXHEALTH as a #define, used only in this module
int max_armor = 200;
int green_armor_class = 1;  // these are involved with armortype below
int blue_armor_class = 2;
int max_soul = 200;
int soul_health = 100;
int mega_health = 200;
int god_health = 100;   // these are used in cheats (see st_stuff.c)
int idfa_armor = 200;
int idfa_armor_class = 2;
// not actually used due to pairing of cheat_k and cheat_fa
int idkfa_armor = 200;
int idkfa_armor_class = 2;

int bfgcells = 40;      // used in p_pspr.c
int monsters_infight = 0; // e6y: Dehacked support - monsters infight
// Ty 03/07/98 - end deh externals

// a weapon is found with two clip loads,
// a big item has five clip loads
// Sized to NUMAMMO (6, Heretic). Doom uses the first four; the trailing
// two are unused under Doom. Heretic values are selected at player init
// (see G_PlayerReborn) via heretic_maxammo / heretic_clipammo.
int maxammo[NUMAMMO]  = {200, 50, 300, 50, 0, 0};
int clipammo[NUMAMMO] = { 10,  4,  20,  1, 0, 0};

/* Heretic per-type maximum ammo and pickup clip sizes (vanilla Heretic):
 *   goldwand, crossbow, blaster, skullrod, phoenixrod, mace */
int heretic_maxammo[NUMAMMO]  = {100, 50, 200, 200, 20, 150};
int heretic_clipammo[NUMAMMO] = { 10,  5,  10,  20,  1,  10};

//
// GET STUFF
//

//
// P_GiveAmmo
// Num is the number of clip loads,
// not the individual count (0= 1/2 clip).
// Returns FALSE if the ammo can't be picked up at all
//

/* Ammo consumed by one shot of a weapon: the MBF21 Ammo per shot when set,
 * otherwise the vanilla per-shot amount.  Used by the MBF21 ammo-pickup
 * autoswitch logic. */
static int P_WeaponAmmoPerShot(weapontype_t weapon)
{
  if (mbf21_features && weaponinfo[weapon].ammopershot >= 0)
    return weaponinfo[weapon].ammopershot;
  if (weapon == WP_BFG)         return bfgcells;
  if (weapon == WP_SUPERSHOTGUN) return 2;
  return 1;
}

static dbool   P_GiveAmmo(player_t *player, ammotype_t ammo, int num)
{
   int oldammo;

   if (ammo == AM_NOAMMO)
      return FALSE;

   if ( player->ammo[ammo] == player->maxammo[ammo]  )
      return FALSE;

   if (num)
      num *= clipammo[ammo];
   else
      num = clipammo[ammo]/2;

   // give double ammo in trainer mode, you'll need in nightmare
   if (gameskill == sk_baby || gameskill == sk_nightmare)
      num <<= 1;

   oldammo = player->ammo[ammo];
   player->ammo[ammo] += num;

   if (player->ammo[ammo] > player->maxammo[ammo])
      player->ammo[ammo] = player->maxammo[ammo];

   // If non zero ammo, don't change up weapons, player was lower on purpose.
   if (oldammo)
      return TRUE;

   /* MBF21: ammo-pickup autoswitch accounts for ammo-per-shot and the
    * AUTOSWITCHFROM / NOAUTOSWITCHTO weapon flags.  If the current weapon
    * allows being switched away from, pick the highest-index weapon the
    * player owns that is not NOAUTOSWITCHTO, uses the picked-up ammo, and
    * went from "couldn't fire" to "can fire" with this pickup.  Below
    * complevel 21 the hardcoded vanilla preferences below are used. */
   if (mbf21_features)
   {
      if (weaponinfo[player->readyweapon].flags & WPF_AUTOSWITCHFROM)
      {
         int w;
         for (w = NUMWEAPONS - 1; w >= 0; w--)
         {
            if (!player->weaponowned[w])                continue;
            if (weaponinfo[w].flags & WPF_NOAUTOSWITCHTO) continue;
            if (weaponinfo[w].ammo != ammo)             continue;
            if (w == (int)player->readyweapon)          continue;
            /* couldn't fire before the pickup, can fire now */
            if (oldammo >= P_WeaponAmmoPerShot((weapontype_t)w))       continue;
            if (player->ammo[ammo] < P_WeaponAmmoPerShot((weapontype_t)w)) continue;
            player->pendingweapon = (weapontype_t)w;
            break;
         }
      }
      return TRUE;
   }

   // We were down to zero, so select a new weapon.
   // Preferences are not user selectable.

   switch (ammo)
   {
      case AM_CLIP:
         if (player->readyweapon == WP_FIST)
         {
            if (player->weaponowned[WP_CHAINGUN])
               player->pendingweapon = WP_CHAINGUN;
            else
               player->pendingweapon = WP_PISTOL;
         }
         break;

      case AM_SHELL:
         if (player->readyweapon == WP_FIST || player->readyweapon == WP_PISTOL)
            if (player->weaponowned[WP_SHOTGUN])
               player->pendingweapon = WP_SHOTGUN;
         break;

      case AM_CELL:
         if (player->readyweapon == WP_FIST || player->readyweapon == WP_PISTOL)
            if (player->weaponowned[WP_PLASMA])
               player->pendingweapon = WP_PLASMA;
         break;

      case AM_MISL:
         if (player->readyweapon == WP_FIST)
            if (player->weaponowned[WP_MISSILE])
               player->pendingweapon = WP_MISSILE;
      default:
         break;
   }
   return TRUE;
}

//
// P_GiveWeapon
// The weapon name may have a MF_DROPPED flag ored in.
//

static dbool   P_GiveWeapon(player_t *player, weapontype_t weapon, dbool   dropped)
{
  dbool   gaveammo;
  dbool   gaveweapon;

  if (netgame && deathmatch!=2 && !dropped)
    {
      // leave placed weapons forever on net games
      if (player->weaponowned[weapon])
        return FALSE;

      player->bonuscount += BONUSADD;
      player->weaponowned[weapon] = TRUE;

      P_GiveAmmo(player, weaponinfo[weapon].ammo, deathmatch ? 5 : 2);

      player->pendingweapon = weapon;
      /* cph 20028/10 - for old-school DM addicts, allow old behavior
       * where only consoleplayer's pickup sounds are heard */
      // displayplayer, not consoleplayer, for viewing multiplayer demos
      if (!comp[comp_sound] || player == &players[displayplayer])
        S_StartSound (player->mo, sfx_wpnup|PICKUP_SOUND); // killough 4/25/98
      return FALSE;
    }

  if (weaponinfo[weapon].ammo != AM_NOAMMO)
    {
      // give one clip with a dropped weapon,
      // two clips with a found weapon
      gaveammo = P_GiveAmmo (player, weaponinfo[weapon].ammo, dropped ? 1 : 2);
    }
  else
    gaveammo = FALSE;

  if (player->weaponowned[weapon])
    gaveweapon = FALSE;
  else
    {
      gaveweapon = TRUE;
      player->weaponowned[weapon] = TRUE;
      player->pendingweapon = weapon;
    }
  return gaveweapon || gaveammo;
}

//
// P_GiveBody
// Returns FALSE if the body isn't needed at all
//

dbool P_GiveBody(player_t *player, int num)
{
  if (player->health >= maxhealth)
    return FALSE; // Ty 03/09/98 externalized MAXHEALTH to maxhealth
  player->health += num;
  if (player->health > maxhealth)
    player->health = maxhealth;
  player->mo->health = player->health;
  return TRUE;
}

//
// P_GiveArmor
// Returns FALSE if the armor is worse
// than the current armor.
//

static dbool   P_GiveArmor(player_t *player, int armortype)
{
  int hits = armortype*100;
  if (player->armorpoints >= hits)
    return FALSE;   // don't pick up
  player->armortype = armortype;
  player->armorpoints = hits;
  return TRUE;
}

/* Per-class Hexen armor parameters (fixed-point), indexed by pclass_t.
 * armor_increment[] is the value each armor piece is set to by a full
 * pickup of that type and its weight in the damage-absorption sum;
 * auto_armor_save is the class's innate save; armor_max caps the total a
 * Boost-Armor pickup can build to.  Values match the original Hexen. */
static const struct
{
  fixed_t armor_increment[NUMARMOR];
  fixed_t auto_armor_save;
  fixed_t armor_max;
} hexen_class_armor[NUMCLASSES] = {
  /* PCLASS_NULL    */ { { 0, 0, 0, 0 }, 0, 0 },
  /* PCLASS_FIGHTER */ { { 25*FRACUNIT, 20*FRACUNIT, 15*FRACUNIT,  5*FRACUNIT }, 15*FRACUNIT, 100*FRACUNIT },
  /* PCLASS_CLERIC  */ { { 10*FRACUNIT, 25*FRACUNIT,  5*FRACUNIT, 20*FRACUNIT }, 10*FRACUNIT,  90*FRACUNIT },
  /* PCLASS_MAGE    */ { {  5*FRACUNIT, 15*FRACUNIT, 10*FRACUNIT, 25*FRACUNIT },  5*FRACUNIT,  80*FRACUNIT },
  /* PCLASS_PIG     */ { { 0, 0, 0, 0 }, 0, 5*FRACUNIT }
};

/* Hexen armor grant.  amount == -1 sets the given piece to its full
 * class value (the in-world armor pickups); a positive amount adds
 * amount*5 save-percent up to the class total cap (the Boost Armor /
 * Heal Radius artifacts).  Returns FALSE if nothing could be added. */
dbool Hexen_P_GiveArmor(player_t *player, armortype_t armortype, int amount)
{
  int hits;
  int totalArmor;
  int cls = player->class;

  if (amount == -1)
  {
    hits = hexen_class_armor[cls].armor_increment[armortype];
    if (player->hexen_armorpoints[armortype] >= hits)
      return FALSE;
    player->hexen_armorpoints[armortype] = hits;
  }
  else
  {
    hits = amount * 5 * FRACUNIT;
    totalArmor = player->hexen_armorpoints[ARMOR_ARMOR]
               + player->hexen_armorpoints[ARMOR_SHIELD]
               + player->hexen_armorpoints[ARMOR_HELMET]
               + player->hexen_armorpoints[ARMOR_AMULET]
               + hexen_class_armor[cls].auto_armor_save;
    if (totalArmor < hexen_class_armor[cls].armor_max)
      player->hexen_armorpoints[armortype] += hits;
    else
      return FALSE;
  }
  return TRUE;
}

//
// P_GiveCard
//

static void P_GiveCard(player_t *player, card_t card)
{
  if (player->cards[card])
    return;
  player->bonuscount = BONUSADD;
  player->cards[card] = 1;
}

//
// P_GiveArtifact
//
// Heretic: add an artifact to the player's inventory. Returns false if the
// inventory cannot accept it (already holding the per-item limit). This is
// the Heretic path only -- the Hexen puzzle-item handling and the on-screen
// inventory-cursor bookkeeping are omitted (the Hexen inventory UI is not
// built here).
//
#define ARTI_LIMIT 16

dbool P_GiveArtifact(player_t *player, int arti, mobj_t *mo)
{
  int i = 0;

  while (i < player->inventorySlotNum && player->inventory[i].type != arti)
    i++;

  if (i == player->inventorySlotNum)
  {
    player->inventory[i].count = 1;
    player->inventory[i].type  = arti;
    player->inventorySlotNum++;
  }
  else
  {
    if (player->inventory[i].count >= ARTI_LIMIT)
      return FALSE;
    player->inventory[i].count++;
  }

  if (player->artifactCount == 0)
    player->readyArtifact = arti;
  player->artifactCount++;

  if (mo && (mo->flags & MF_COUNTITEM))
    player->itemcount++;

  return TRUE;
}

//
// P_GivePower
//
// Rewritten by Lee Killough
//

dbool   P_GivePower(player_t *player, int power)
{
  static const int tics[NUMPOWERS] = {
    INVULNTICS, 1 /* strength */, INVISTICS,
    IRONTICS, 1 /* allmap */, INFRATICS,
    WPNLEV2TICS, FLIGHTTICS, 1 /* shield */, 1 /* health2 */,
    SPEEDTICS, MAULATORTICS
   };

  /* Raven: re-using an artifact whose power is still well above the blink
   * threshold is refused (you can't stack invuln/etc.). Ironfeet/minotaur
   * and the instantaneous powers (tics==1) are exempt. */
  if (raven
      && tics[power] > 1
      && power != pw_ironfeet && power != pw_minotaur
      && player->powers[power] > BLINKTHRESHOLD)
    return FALSE;

  switch (power)
    {
      case pw_invisibility:
        player->mo->flags |= MF_SHADOW;
        break;
      case pw_allmap:
        if (player->powers[pw_allmap])
          return FALSE;
        break;
      case pw_strength:
        P_GiveBody(player,100);
        break;
      case pw_flight:
        player->mo->flags2 |= MF2_FLY;
        player->mo->flags  |= MF_NOGRAVITY;
        if (player->mo->z <= player->mo->floorz)
          player->flyheight = 10;     /* thrust the player up a bit */
        break;
    }

  // Unless player has infinite duration cheat, set duration (killough)

  if (player->powers[power] >= 0)
    player->powers[power] = tics[power];
  return TRUE;
}

//
// P_TouchSpecialThing
//

extern void retro_set_rumble_touch(unsigned intensity, float duration);

static void Heretic_P_TouchSpecialThing(mobj_t *special, mobj_t *toucher);
static void Hexen_P_TouchSpecialThing(mobj_t *special, mobj_t *toucher);

void P_TouchSpecialThing(mobj_t *special, mobj_t *toucher)
{
  player_t *player;
  int      i;
  int      sound;
  fixed_t  delta = special->z - toucher->z;

  if (heretic)
  {
    Heretic_P_TouchSpecialThing(special, toucher);
    return;
  }

  if (hexen)
  {
    Hexen_P_TouchSpecialThing(special, toucher);
    return;
  }

  if (delta > toucher->height || delta < -8*FRACUNIT)
    return;        // out of reach

  sound = sfx_itemup;
  player = toucher->player;

  // Dead thing touching.
  // Can happen with a sliding player corpse.
  if (toucher->health <= 0)
    return;

    // Identify by sprite.
  switch (special->sprite)
    {
      // armor
    case SPR_ARM1:
      if (!P_GiveArmor (player, green_armor_class))
        return;
      player->message = s_GOTARMOR; // Ty 03/22/98 - externalized
      retro_set_rumble_touch(12, 160.0f);
      break;

    case SPR_ARM2:
      if (!P_GiveArmor (player, blue_armor_class))
        return;
      player->message = s_GOTMEGA; // Ty 03/22/98 - externalized
      retro_set_rumble_touch(14, 160.0f);
      break;

        // bonus items
    case SPR_BON1:
      player->health++;               // can go over 100%
      if (player->health > (maxhealth * 2))
        player->health = (maxhealth * 2);
      player->mo->health = player->health;
      player->message = s_GOTHTHBONUS; // Ty 03/22/98 - externalized
      retro_set_rumble_touch(5, 160.0f);
      break;

    case SPR_BON2:
      player->armorpoints++;          // can go over 100%
      if (player->armorpoints > max_armor)
        player->armorpoints = max_armor;
      if (!player->armortype)
        player->armortype = green_armor_class;
      player->message = s_GOTARMBONUS; // Ty 03/22/98 - externalized
      retro_set_rumble_touch(5, 160.0f);
      break;

    case SPR_SOUL:
      player->health += soul_health;
      if (player->health > max_soul)
        player->health = max_soul;
      player->mo->health = player->health;
      player->message = s_GOTSUPER; // Ty 03/22/98 - externalized
      sound = sfx_getpow;
      retro_set_rumble_touch(14, 160.0f);
      break;

    case SPR_MEGA:
      if (gamemode != commercial)
        return;
      player->health = mega_health;
      player->mo->health = player->health;
      P_GiveArmor (player,blue_armor_class);
      player->message = s_GOTMSPHERE; // Ty 03/22/98 - externalized
      sound = sfx_getpow;
      retro_set_rumble_touch(16, 160.0f);
      break;

        // cards
        // leave cards for everyone
    case SPR_BKEY:
      if (!player->cards[it_bluecard])
        player->message = s_GOTBLUECARD; // Ty 03/22/98 - externalized
      P_GiveCard (player, it_bluecard);
      retro_set_rumble_touch(7, 150.0f);
      if (!netgame)
        break;
      return;

    case SPR_YKEY:
      if (!player->cards[it_yellowcard])
        player->message = s_GOTYELWCARD; // Ty 03/22/98 - externalized
      P_GiveCard (player, it_yellowcard);
      retro_set_rumble_touch(7, 150.0f);
      if (!netgame)
        break;
      return;

    case SPR_RKEY:
      if (!player->cards[it_redcard])
        player->message = s_GOTREDCARD; // Ty 03/22/98 - externalized
      P_GiveCard (player, it_redcard);
      retro_set_rumble_touch(7, 150.0f);
      if (!netgame)
        break;
      return;

    case SPR_BSKU:
      if (!player->cards[it_blueskull])
        player->message = s_GOTBLUESKUL; // Ty 03/22/98 - externalized
      P_GiveCard (player, it_blueskull);
      retro_set_rumble_touch(8, 150.0f);
      if (!netgame)
        break;
      return;

    case SPR_YSKU:
      if (!player->cards[it_yellowskull])
        player->message = s_GOTYELWSKUL; // Ty 03/22/98 - externalized
      P_GiveCard (player, it_yellowskull);
      retro_set_rumble_touch(8, 150.0f);
      if (!netgame)
        break;
      return;

    case SPR_RSKU:
      if (!player->cards[it_redskull])
        player->message = s_GOTREDSKULL; // Ty 03/22/98 - externalized
      P_GiveCard (player, it_redskull);
      retro_set_rumble_touch(8, 150.0f);
      if (!netgame)
        break;
      return;

      // medikits, heals
    case SPR_STIM:
      if (!P_GiveBody (player, 10))
        return;
      player->message = s_GOTSTIM; // Ty 03/22/98 - externalized
      retro_set_rumble_touch(6, 160.0f);
      break;

    case SPR_MEDI:
      if (!P_GiveBody (player, 25))
        return;

      if (player->health < 50) // cph - 25 + the 25 just added, thanks to Quasar for reporting this bug
        player->message = s_GOTMEDINEED; // Ty 03/22/98 - externalized
      else
        player->message = s_GOTMEDIKIT; // Ty 03/22/98 - externalized

      retro_set_rumble_touch(8, 160.0f);
      break;


      // power ups
    case SPR_PINV:
      if (!P_GivePower (player, pw_invulnerability))
        return;
      player->message = s_GOTINVUL; // Ty 03/22/98 - externalized
      sound = sfx_getpow;
      retro_set_rumble_touch(18, 160.0f);
      break;

    case SPR_PSTR:
      if (!P_GivePower (player, pw_strength))
        return;
      player->message = s_GOTBERSERK; // Ty 03/22/98 - externalized
      if (player->readyweapon != WP_FIST)
        player->pendingweapon = WP_FIST;
      sound = sfx_getpow;
      retro_set_rumble_touch(20, 180.0f);
      break;

    case SPR_PINS:
      if (!P_GivePower (player, pw_invisibility))
        return;
      player->message = s_GOTINVIS; // Ty 03/22/98 - externalized
      sound = sfx_getpow;
      retro_set_rumble_touch(18, 160.0f);
      break;

    case SPR_SUIT:
      if (!P_GivePower (player, pw_ironfeet))
        return;
      player->message = s_GOTSUIT; // Ty 03/22/98 - externalized
      sound = sfx_getpow;
      retro_set_rumble_touch(18, 160.0f);
      break;

    case SPR_PMAP:
      if (!P_GivePower (player, pw_allmap))
        return;
      player->message = s_GOTMAP; // Ty 03/22/98 - externalized
      sound = sfx_getpow;
      retro_set_rumble_touch(18, 160.0f);
      break;

    case SPR_PVIS:
      if (!P_GivePower (player, pw_infrared))
        return;
      player->message = s_GOTVISOR; // Ty 03/22/98 - externalized
      sound = sfx_getpow;
      retro_set_rumble_touch(18, 160.0f);
      break;

      // ammo
    case SPR_CLIP:
      if (special->flags & MF_DROPPED)
        {
          if (!P_GiveAmmo (player,AM_CLIP,0))
            return;
        }
      else
        {
          if (!P_GiveAmmo (player,AM_CLIP,1))
            return;
        }
      player->message = s_GOTCLIP; // Ty 03/22/98 - externalized

      retro_set_rumble_touch(6, 140.0f);
      break;

    case SPR_AMMO:
      if (!P_GiveAmmo (player, AM_CLIP,5))
        return;
      player->message = s_GOTCLIPBOX; // Ty 03/22/98 - externalized
      retro_set_rumble_touch(8, 140.0f);
      break;

    case SPR_ROCK:
      if (!P_GiveAmmo (player, AM_MISL,1))
        return;
      player->message = s_GOTROCKET; // Ty 03/22/98 - externalized
      retro_set_rumble_touch(6, 140.0f);
      break;

    case SPR_BROK:
      if (!P_GiveAmmo (player, AM_MISL,5))
        return;
      player->message = s_GOTROCKBOX; // Ty 03/22/98 - externalized
      retro_set_rumble_touch(8, 140.0f);
      break;

    case SPR_CELL:
      if (!P_GiveAmmo (player, AM_CELL,1))
        return;
      player->message = s_GOTCELL; // Ty 03/22/98 - externalized
      retro_set_rumble_touch(6, 140.0f);
      break;

    case SPR_CELP:
      if (!P_GiveAmmo (player, AM_CELL,5))
        return;
      player->message = s_GOTCELLBOX; // Ty 03/22/98 - externalized
      retro_set_rumble_touch(8, 140.0f);
      break;

    case SPR_SHEL:
      if (!P_GiveAmmo (player, AM_SHELL,1))
        return;
      player->message = s_GOTSHELLS; // Ty 03/22/98 - externalized
      retro_set_rumble_touch(6, 140.0f);
      break;

    case SPR_SBOX:
      if (!P_GiveAmmo (player, AM_SHELL,5))
        return;
      player->message = s_GOTSHELLBOX; // Ty 03/22/98 - externalized
      retro_set_rumble_touch(8, 140.0f);
      break;

    case SPR_BPAK:
      if (!player->backpack)
        {
          for (i=0 ; i<NUMAMMO ; i++)
            player->maxammo[i] *= 2;
          player->backpack = TRUE;
        }
      for (i=0 ; i<NUMAMMO ; i++)
        P_GiveAmmo (player, i, 1);
      player->message = s_GOTBACKPACK; // Ty 03/22/98 - externalized
      retro_set_rumble_touch(12, 160.0f);
      break;

        // weapons
    case SPR_BFUG:
      if (!P_GiveWeapon (player, WP_BFG, FALSE) )
        return;
      player->message = s_GOTBFG9000; // Ty 03/22/98 - externalized
      sound = sfx_wpnup;
      retro_set_rumble_touch(20, 180.0f);
      break;

    case SPR_MGUN:
      if (!P_GiveWeapon (player, WP_CHAINGUN, (special->flags&MF_DROPPED)!=0) )
        return;
      player->message = s_GOTCHAINGUN; // Ty 03/22/98 - externalized
      sound = sfx_wpnup;
      retro_set_rumble_touch(15, 180.0f);
      break;

    case SPR_CSAW:
      if (!P_GiveWeapon (player, WP_CHAINSAW, FALSE) )
        return;
      player->message = s_GOTCHAINSAW; // Ty 03/22/98 - externalized
      sound = sfx_wpnup;
      retro_set_rumble_touch(15, 180.0f);
      break;

    case SPR_LAUN:
      if (!P_GiveWeapon (player, WP_MISSILE, FALSE) )
        return;
      player->message = s_GOTLAUNCHER; // Ty 03/22/98 - externalized
      sound = sfx_wpnup;
      retro_set_rumble_touch(18, 180.0f);
      break;

    case SPR_PLAS:
      if (!P_GiveWeapon (player, WP_PLASMA, FALSE) )
        return;
      player->message = s_GOTPLASMA; // Ty 03/22/98 - externalized
      sound = sfx_wpnup;
      retro_set_rumble_touch(17, 180.0f);
      break;

    case SPR_SHOT:
      if (!P_GiveWeapon (player, WP_SHOTGUN, (special->flags&MF_DROPPED)!=0 ) )
        return;
      player->message = s_GOTSHOTGUN; // Ty 03/22/98 - externalized
      sound = sfx_wpnup;
      retro_set_rumble_touch(14, 180.0f);
      break;

    case SPR_SGN2:
      if (!P_GiveWeapon(player, WP_SUPERSHOTGUN, (special->flags&MF_DROPPED)!=0))
        return;
      player->message = s_GOTSHOTGUN2; // Ty 03/22/98 - externalized
      sound = sfx_wpnup;
      retro_set_rumble_touch(16, 180.0f);
      break;

    default:
      I_Error ("P_SpecialThing: Unknown gettable thing");
    }

  if (special->flags & MF_COUNTITEM)
    player->itemcount++;
  P_RemoveMobj (special);
  player->bonuscount += BONUSADD;

  /* cph 20028/10 - for old-school DM addicts, allow old behavior
   * where only consoleplayer's pickup sounds are heard */
  // displayplayer, not consoleplayer, for viewing multiplayer demos
  if (!comp[comp_sound] || player == &players[displayplayer])
    S_StartSound (player->mo, sound | PICKUP_SOUND);   // killough 4/25/98
}

/*
 * Heretic pickups.
 *
 * Heretic identifies pickups by sprite (a distinct sprite table from
 * Doom). This grants the matching item, ammo, key, artifact or weapon.
 * Messages are posted via player->message (the engine's HUD message
 * mechanism) using plain text. Weapon slots map onto the shared
 * weapontype_t enum (staff=WP_FIST .. gauntlets=WP_CHAINSAW); ammo uses
 * the am_* slots, keys use cards[] slots 0/1/2, and artifacts use the
 * Raven inventory via P_GiveArtifact. The pickup amount for ammo items is
 * carried in special->health (the ammo mobj's spawnhealth).
 */
static void Heretic_P_TouchSpecialThing(mobj_t *special, mobj_t *toucher)
{
  player_t *player;
  int       i;
  int       sound;
  fixed_t   delta = special->z - toucher->z;

  if (delta > toucher->height || delta < -32 * FRACUNIT)
    return;                 /* out of reach */
  if (toucher->health <= 0)
    return;                 /* toucher is dead */

  sound  = heretic_sfx_itemup;
  player = toucher->player;

  switch (special->sprite)
  {
    /* Items */
    case HERETIC_SPR_PTN1:        /* healing potion */
      if (!P_GiveBody(player, 10))
        return;
      player->message = "CRYSTAL VIAL";
      break;
    case HERETIC_SPR_SHLD:        /* silver shield */
      if (!P_GiveArmor(player, 1))
        return;
      player->message = "SILVER SHIELD";
      break;
    case HERETIC_SPR_SHD2:        /* enchanted shield */
      if (!P_GiveArmor(player, 2))
        return;
      player->message = "ENCHANTED SHIELD";
      break;
    case HERETIC_SPR_BAGH:        /* bag of holding */
      if (!player->backpack)
      {
        for (i = 0; i < NUMAMMO; i++)
          player->maxammo[i] *= 2;
        player->backpack = TRUE;
      }
      P_GiveAmmo(player, am_goldwand, AMMO_GWND_WIMPY);
      P_GiveAmmo(player, am_blaster, AMMO_BLSR_WIMPY);
      P_GiveAmmo(player, am_crossbow, AMMO_CBOW_WIMPY);
      P_GiveAmmo(player, am_skullrod, AMMO_SKRD_WIMPY);
      P_GiveAmmo(player, am_phoenixrod, AMMO_PHRD_WIMPY);
      player->message = "BAG OF HOLDING";
      break;
    case HERETIC_SPR_SPMP:        /* map scroll */
      if (!P_GivePower(player, pw_allmap))
        return;
      player->message = "MAP SCROLL";
      break;

    /* Keys (blue/yellow/green map to cards[] slots 0/1/2). */
    case HERETIC_SPR_BKYY:        /* blue key */
      P_GiveCard(player, it_bluecard);
      player->message = "BLUE KEY";
      sound = heretic_sfx_keyup;
      if (!netgame)
        break;
      return;
    case HERETIC_SPR_CKYY:        /* yellow key */
      P_GiveCard(player, it_yellowcard);
      player->message = "YELLOW KEY";
      sound = heretic_sfx_keyup;
      if (!netgame)
        break;
      return;
    case HERETIC_SPR_AKYY:        /* green key */
      P_GiveCard(player, it_redcard);
      player->message = "GREEN KEY";
      sound = heretic_sfx_keyup;
      if (!netgame)
        break;
      return;

    /* Artifacts (inventory). These never disappear in netgames. */
    case HERETIC_SPR_PTN2:        /* quartz flask */
      if (P_GiveArtifact(player, arti_health, special))
      {
        player->message = "QUARTZ FLASK";
        if (!netgame)
          break;
      }
      return;
    case HERETIC_SPR_SOAR:        /* wings of wrath */
      if (P_GiveArtifact(player, arti_fly, special))
      {
        player->message = "WINGS OF WRATH";
        if (!netgame)
          break;
      }
      return;
    case HERETIC_SPR_INVU:        /* ring of invincibility */
      if (P_GiveArtifact(player, arti_invulnerability, special))
      {
        player->message = "RING OF INVINCIBILITY";
        if (!netgame)
          break;
      }
      return;
    case HERETIC_SPR_PWBK:        /* tome of power */
      if (P_GiveArtifact(player, arti_tomeofpower, special))
      {
        player->message = "TOME OF POWER";
        if (!netgame)
          break;
      }
      return;
    case HERETIC_SPR_INVS:        /* shadowsphere */
      if (P_GiveArtifact(player, arti_invisibility, special))
      {
        player->message = "SHADOWSPHERE";
        if (!netgame)
          break;
      }
      return;
    case HERETIC_SPR_EGGC:        /* morph ovum */
      if (P_GiveArtifact(player, arti_egg, special))
      {
        player->message = "MORPH OVUM";
        if (!netgame)
          break;
      }
      return;
    case HERETIC_SPR_SPHL:        /* mystic urn */
      if (P_GiveArtifact(player, arti_superhealth, special))
      {
        player->message = "MYSTIC URN";
        if (!netgame)
          break;
      }
      return;
    case HERETIC_SPR_TRCH:        /* torch */
      if (P_GiveArtifact(player, arti_torch, special))
      {
        player->message = "TORCH";
        if (!netgame)
          break;
      }
      return;
    case HERETIC_SPR_FBMB:        /* time bomb of the ancients */
      if (P_GiveArtifact(player, arti_firebomb, special))
      {
        player->message = "TIME BOMB OF THE ANCIENTS";
        if (!netgame)
          break;
      }
      return;
    case HERETIC_SPR_ATLP:        /* chaos device */
      if (P_GiveArtifact(player, arti_teleport, special))
      {
        player->message = "CHAOS DEVICE";
        if (!netgame)
          break;
      }
      return;

    /* Ammo (pickup amount in special->health). */
    case HERETIC_SPR_AMG1:        /* wand crystal */
    case HERETIC_SPR_AMG2:        /* crystal geode */
      if (!P_GiveAmmo(player, am_goldwand, special->health))
        return;
      player->message = "WAND CRYSTAL";
      break;
    case HERETIC_SPR_AMM1:        /* mace spheres */
    case HERETIC_SPR_AMM2:
      if (!P_GiveAmmo(player, am_mace, special->health))
        return;
      player->message = "MACE SPHERES";
      break;
    case HERETIC_SPR_AMC1:        /* ethereal arrows */
    case HERETIC_SPR_AMC2:
      if (!P_GiveAmmo(player, am_crossbow, special->health))
        return;
      player->message = "ETHEREAL ARROWS";
      break;
    case HERETIC_SPR_AMB1:        /* claw orb */
    case HERETIC_SPR_AMB2:
      if (!P_GiveAmmo(player, am_blaster, special->health))
        return;
      player->message = "CLAW ORB";
      break;
    case HERETIC_SPR_AMS1:        /* lesser/greater runes */
    case HERETIC_SPR_AMS2:
      if (!P_GiveAmmo(player, am_skullrod, special->health))
        return;
      player->message = "RUNES";
      break;
    case HERETIC_SPR_AMP1:        /* flame orb */
    case HERETIC_SPR_AMP2:
      if (!P_GiveAmmo(player, am_phoenixrod, special->health))
        return;
      player->message = "FLAME ORB";
      break;

    /* Weapons (Heretic slots overlay the Doom weapontype_t slots). */
    case HERETIC_SPR_WMCE:        /* firemace */
      if (!P_GiveWeapon(player, WP_BFG /* mace */, FALSE))
        return;
      player->message = "FIREMACE";
      sound = heretic_sfx_wpnup;
      break;
    case HERETIC_SPR_WBOW:        /* ethereal crossbow */
      if (!P_GiveWeapon(player, WP_SHOTGUN /* crossbow */, FALSE))
        return;
      player->message = "ETHEREAL CROSSBOW";
      sound = heretic_sfx_wpnup;
      break;
    case HERETIC_SPR_WBLS:        /* dragon claw */
      if (!P_GiveWeapon(player, WP_CHAINGUN /* blaster */, FALSE))
        return;
      player->message = "DRAGON CLAW";
      sound = heretic_sfx_wpnup;
      break;
    case HERETIC_SPR_WSKL:        /* hellstaff */
      if (!P_GiveWeapon(player, WP_MISSILE /* skullrod */, FALSE))
        return;
      player->message = "HELLSTAFF";
      sound = heretic_sfx_wpnup;
      break;
    case HERETIC_SPR_WPHX:        /* phoenix rod */
      if (!P_GiveWeapon(player, WP_PLASMA /* phoenixrod */, FALSE))
        return;
      player->message = "PHOENIX ROD";
      sound = heretic_sfx_wpnup;
      break;
    case HERETIC_SPR_WGNT:        /* gauntlets of the necromancer */
      if (!P_GiveWeapon(player, WP_CHAINSAW /* gauntlets */, FALSE))
        return;
      player->message = "GAUNTLETS OF THE NECROMANCER";
      sound = heretic_sfx_wpnup;
      break;

    default:
      /* Unknown Heretic pickup: leave it in the world rather than
       * silently removing it. */
      return;
  }

  if (special->flags & MF_COUNTITEM)
    player->itemcount++;
  P_RemoveMobj(special);
  player->bonuscount += BONUSADD;

  if (!comp[comp_sound] || player == &players[displayplayer])
    S_StartSound(player->mo, sound | PICKUP_SOUND);
}

/* --------------------------------------------------------------------------
 * Hexen item pickups.
 *
 * Single-player handling for the Hexen mana and (Fighter) weapon pickups.
 * Mana uses the dedicated player->mana[] pool; weapons set weaponowned and
 * auto-switch to a more powerful slot.  Picking up the first blue mana while
 * the axe is ready switches it to its glowing/powered form.  Cleric/Mage
 * weapons and the assembled fourth weapon (weapon pieces) are not handled
 * yet.
 * ------------------------------------------------------------------------ */

dbool P_GiveMana(player_t *player, manatype_t mana, int count)
{
  int prevMana;

  if (mana == MANA_NONE || mana == MANA_BOTH)
    return false;
  if ((unsigned int)mana >= NUMMANA)
    return false;
  if (player->mana[mana] == MAX_MANA)
    return false;

  prevMana = player->mana[mana];
  player->mana[mana] += count;
  if (player->mana[mana] > MAX_MANA)
    player->mana[mana] = MAX_MANA;
  (void)prevMana;

  return true;
}

/* Give a Hexen weapon (single-player): own it, top up its mana, and switch
 * to it if it is more powerful than the current weapon. Returns true if the
 * pickup should be consumed. */
static dbool Hexen_GiveWeapon(player_t *player, weapontype_t weaponType)
{
  dbool gaveMana;
  dbool gaveWeapon;

  if (weaponType == WP_SECOND)
    gaveMana = P_GiveMana(player, MANA_1, 25);
  else
    gaveMana = P_GiveMana(player, MANA_2, 25);

  if (player->weaponowned[weaponType])
    gaveWeapon = false;
  else
  {
    gaveWeapon = true;
    player->weaponowned[weaponType] = true;
    if (weaponType > player->readyweapon)
      player->pendingweapon = weaponType; /* switch to more powerful weapon */
  }
  return gaveWeapon || gaveMana;
}

/* Collect a fourth-weapon piece (single-player).  Always grants 20+20 mana;
 * tracks the piece bit, and when all three are held grants and switches to
 * the fourth weapon (Fighter: Quietus).  Returns the sound to play. */
static int Hexen_GiveWeaponPiece(player_t *player, pclass_t matchClass,
                                 int pieceValue, dbool *gaveWeapon)
{
  *gaveWeapon = false;

  if ((pclass_t) player->class != matchClass)
  {
    /* wrong class: pick up only for the mana */
    int gaveMana = (int)P_GiveMana(player, MANA_1, 20)
                 + (int)P_GiveMana(player, MANA_2, 20);
    if (!gaveMana)
      return -1;                  /* didn't need it; leave in world */
    return hexen_sfx_pickup_weapon;
  }

  P_GiveMana(player, MANA_1, 20);
  P_GiveMana(player, MANA_2, 20);
  player->pieces |= pieceValue;
  if (player->pieces == (WPIECE1 | WPIECE2 | WPIECE3))
  {
    *gaveWeapon = true;
    player->weaponowned[WP_FOURTH] = true;
    if (WP_FOURTH > player->readyweapon)
      player->pendingweapon = (weapontype_t)WP_FOURTH;
    return hexen_sfx_weapon_build;
  }
  return hexen_sfx_pickup_weapon;
}

/* Hexen key names, indexed 0-10 (lock argument minus one). */
const char *TextKeyMessages[11] = {
  "STEEL KEY",
  "CAVE KEY",
  "AXE KEY",
  "FIRE KEY",
  "EMERALD KEY",
  "DUNGEON KEY",
  "SILVER KEY",
  "RUSTED KEY",
  "HORN KEY",
  "SWAMP KEY",
  "CASTLE KEY"
};

static dbool P_GiveKey(player_t *player, card_t key)
{
  if (player->cards[key])
    return false;
  player->bonuscount += BONUSADD;
  player->cards[key] = true;
  return true;
}

static void Hexen_P_TouchSpecialThing(mobj_t *special, mobj_t *toucher)
{
  player_t *player;
  int       sound;
  fixed_t   delta = special->z - toucher->z;

  if (delta > toucher->height || delta < -8 * FRACUNIT)
    return;            /* out of reach */
  if (toucher->health <= 0)
    return;            /* toucher is dead */

  sound  = hexen_sfx_pickup_weapon;
  player = toucher->player;

  switch (special->sprite)
  {
    case HEXEN_SPR_MAN1:           /* blue mana */
      if (!P_GiveMana(player, MANA_1, 15))
        return;
      player->message = "BLUE MANA";
      sound = hexen_sfx_pickup_item;
      break;
    case HEXEN_SPR_MAN2:           /* green mana */
      if (!P_GiveMana(player, MANA_2, 15))
        return;
      player->message = "GREEN MANA";
      sound = hexen_sfx_pickup_item;
      break;
    case HEXEN_SPR_MAN3:           /* combined mana */
    {
      dbool got = P_GiveMana(player, MANA_1, 20);
      got = P_GiveMana(player, MANA_2, 20) || got;
      if (!got)
        return;
      player->message = "COMBINED MANA";
      sound = hexen_sfx_pickup_item;
      break;
    }
    case HEXEN_SPR_KEY1:           /* Hexen keys: STEEL .. CASTLE */
    case HEXEN_SPR_KEY2:
    case HEXEN_SPR_KEY3:
    case HEXEN_SPR_KEY4:
    case HEXEN_SPR_KEY5:
    case HEXEN_SPR_KEY6:
    case HEXEN_SPR_KEY7:
    case HEXEN_SPR_KEY8:
    case HEXEN_SPR_KEY9:
    case HEXEN_SPR_KEYA:
    case HEXEN_SPR_KEYB:
      if (!P_GiveKey(player, special->sprite - HEXEN_SPR_KEY1))
        return;
      player->message = TextKeyMessages[special->sprite - HEXEN_SPR_KEY1];
      sound = hexen_sfx_pickup_key;
      break;
    case HEXEN_SPR_ARM1:           /* Mesh Armor */
      if (!Hexen_P_GiveArmor(player, ARMOR_ARMOR, -1))
        return;
      player->message = "MESH ARMOR";
      sound = hexen_sfx_pickup_item;
      break;
    case HEXEN_SPR_ARM2:           /* Falcon Shield */
      if (!Hexen_P_GiveArmor(player, ARMOR_SHIELD, -1))
        return;
      player->message = "FALCON SHIELD";
      sound = hexen_sfx_pickup_item;
      break;
    case HEXEN_SPR_ARM3:           /* Platinum Helm */
      if (!Hexen_P_GiveArmor(player, ARMOR_HELMET, -1))
        return;
      player->message = "PLATINUM HELM";
      sound = hexen_sfx_pickup_item;
      break;
    case HEXEN_SPR_ARM4:           /* Amulet of Warding */
      if (!Hexen_P_GiveArmor(player, ARMOR_AMULET, -1))
        return;
      player->message = "AMULET OF WARDING";
      sound = hexen_sfx_pickup_item;
      break;
    case HEXEN_SPR_WFAX:           /* Timon's Axe (Fighter 2nd) */
      if (player->class != PCLASS_FIGHTER || !Hexen_GiveWeapon(player, WP_SECOND))
        return;
      player->message = "TIMON'S AXE";
      break;
    case HEXEN_SPR_WFHM:           /* Hammer of Retribution (Fighter 3rd) */
      if (player->class != PCLASS_FIGHTER || !Hexen_GiveWeapon(player, WP_THIRD))
        return;
      player->message = "HAMMER OF RETRIBUTION";
      break;
    case HEXEN_SPR_WFR1:           /* Quietus piece 1 (Fighter 4th) */
    case HEXEN_SPR_WFR2:           /* Quietus piece 2 */
    case HEXEN_SPR_WFR3:           /* Quietus piece 3 */
    {
      dbool gaveWeapon;
      int   pieceValue = (special->sprite == HEXEN_SPR_WFR1) ? WPIECE1
                       : (special->sprite == HEXEN_SPR_WFR2) ? WPIECE2 : WPIECE3;
      int   s = Hexen_GiveWeaponPiece(player, PCLASS_FIGHTER, pieceValue,
                                      &gaveWeapon);
      if (s < 0)
        return;
      sound = s;
      player->message = gaveWeapon ? "QUIETUS" : "SEGMENT OF QUIETUS";
      break;
    }
    case HEXEN_SPR_WCSS:           /* Serpent Staff (Cleric 2nd) */
      if (player->class != PCLASS_CLERIC || !Hexen_GiveWeapon(player, WP_SECOND))
        return;
      player->message = "SERPENT STAFF";
      break;
    case HEXEN_SPR_WCFM:           /* Firestorm / Flame Strike (Cleric 3rd) */
      if (player->class != PCLASS_CLERIC || !Hexen_GiveWeapon(player, WP_THIRD))
        return;
      player->message = "FIRESTORM";
      break;
    case HEXEN_SPR_WCH1:           /* Wraithverge piece 1 (Cleric 4th) */
    case HEXEN_SPR_WCH2:           /* Wraithverge piece 2 */
    case HEXEN_SPR_WCH3:           /* Wraithverge piece 3 */
    {
      dbool gaveWeapon;
      int   pieceValue = (special->sprite == HEXEN_SPR_WCH1) ? WPIECE1
                       : (special->sprite == HEXEN_SPR_WCH2) ? WPIECE2 : WPIECE3;
      int   s = Hexen_GiveWeaponPiece(player, PCLASS_CLERIC, pieceValue,
                                      &gaveWeapon);
      if (s < 0)
        return;
      sound = s;
      player->message = gaveWeapon ? "WRAITHVERGE" : "SEGMENT OF WRAITHVERGE";
      break;
    }
    case HEXEN_SPR_WMCS:           /* Frost Shards / Cone of Shards (Mage 2nd) */
      if (player->class != PCLASS_MAGE || !Hexen_GiveWeapon(player, WP_SECOND))
        return;
      player->message = "FROST SHARDS";
      break;
    case HEXEN_SPR_WMLG:           /* Arc of Death (Mage 3rd) */
      if (player->class != PCLASS_MAGE || !Hexen_GiveWeapon(player, WP_THIRD))
        return;
      player->message = "ARC OF DEATH";
      break;
    case HEXEN_SPR_WMS1:           /* Bloodscourge piece 1 (Mage 4th) */
    case HEXEN_SPR_WMS2:           /* Bloodscourge piece 2 */
    case HEXEN_SPR_WMS3:           /* Bloodscourge piece 3 */
    {
      dbool gaveWeapon;
      int   pieceValue = (special->sprite == HEXEN_SPR_WMS1) ? WPIECE1
                       : (special->sprite == HEXEN_SPR_WMS2) ? WPIECE2 : WPIECE3;
      int   s = Hexen_GiveWeaponPiece(player, PCLASS_MAGE, pieceValue,
                                      &gaveWeapon);
      if (s < 0)
        return;
      sound = s;
      player->message = gaveWeapon ? "BLOODSCOURGE" : "SEGMENT OF BLOODSCOURGE";
      break;
    }
    case HEXEN_SPR_SUMN:           /* Dark Servant (summon Minotaur) */
      if (!P_GiveArtifact(player, hexen_arti_summon, special))
        return;
      sound = hexen_sfx_pickup_artifact;
      player->message = "DARK SERVANT";
      break;
    case HEXEN_SPR_ATLP:           /* Chaos Device (self teleport) */
      if (!P_GiveArtifact(player, hexen_arti_teleport, special))
        return;
      sound = hexen_sfx_pickup_artifact;
      player->message = "CHAOS DEVICE";
      break;
    case HEXEN_SPR_TELO:           /* Banishment Device (teleport other) */
      if (!P_GiveArtifact(player, hexen_arti_teleportother, special))
        return;
      sound = hexen_sfx_pickup_artifact;
      player->message = "BANISHMENT DEVICE";
      break;
    case HEXEN_SPR_BRAC:           /* Dragonskin Bracers (armor boost) */
      if (!P_GiveArtifact(player, hexen_arti_boostarmor, special))
        return;
      sound = hexen_sfx_pickup_artifact;
      player->message = "DRAGONSKIN BRACERS";
      break;
    case HEXEN_SPR_HRAD:           /* Mystic Ambit Incant (radius boon) */
      if (!P_GiveArtifact(player, hexen_arti_healingradius, special))
        return;
      sound = hexen_sfx_pickup_artifact;
      player->message = "MYSTIC AMBIT INCANT";
      break;
    case HEXEN_SPR_PTN1:           /* Crystal Vial (instant 10 health) */
      if (!P_GiveBody(player, 10))
        return;
      player->message = "CRYSTAL VIAL";
      sound = hexen_sfx_pickup_item;
      break;
    /* Puzzle artifacts (hub-quest items; used on matching special-129
     * lines or things via P_UsePuzzleItem). */
    case HEXEN_SPR_ASKU:
      if (!P_GiveArtifact(player, hexen_arti_puzzskull, special))
        return;
      sound = hexen_sfx_pickup_artifact;
      player->message = "YORICK'S SKULL";
      break;
    case HEXEN_SPR_ABGM:
      if (!P_GiveArtifact(player, hexen_arti_puzzgembig, special))
        return;
      sound = hexen_sfx_pickup_artifact;
      player->message = "HEART OF D'SPARIL";
      break;
    case HEXEN_SPR_AGMR:
      if (!P_GiveArtifact(player, hexen_arti_puzzgemred, special))
        return;
      sound = hexen_sfx_pickup_artifact;
      player->message = "RUBY PLANET";
      break;
    case HEXEN_SPR_AGMG:
      if (!P_GiveArtifact(player, hexen_arti_puzzgemgreen1, special))
        return;
      sound = hexen_sfx_pickup_artifact;
      player->message = "EMERALD PLANET";
      break;
    case HEXEN_SPR_AGG2:
      if (!P_GiveArtifact(player, hexen_arti_puzzgemgreen2, special))
        return;
      sound = hexen_sfx_pickup_artifact;
      player->message = "EMERALD PLANET";
      break;
    case HEXEN_SPR_AGMB:
      if (!P_GiveArtifact(player, hexen_arti_puzzgemblue1, special))
        return;
      sound = hexen_sfx_pickup_artifact;
      player->message = "SAPPHIRE PLANET";
      break;
    case HEXEN_SPR_AGB2:
      if (!P_GiveArtifact(player, hexen_arti_puzzgemblue2, special))
        return;
      sound = hexen_sfx_pickup_artifact;
      player->message = "SAPPHIRE PLANET";
      break;
    case HEXEN_SPR_ABK1:
      if (!P_GiveArtifact(player, hexen_arti_puzzbook1, special))
        return;
      sound = hexen_sfx_pickup_artifact;
      player->message = "DAEMON CODEX";
      break;
    case HEXEN_SPR_ABK2:
      if (!P_GiveArtifact(player, hexen_arti_puzzbook2, special))
        return;
      sound = hexen_sfx_pickup_artifact;
      player->message = "LIBER OSCURA";
      break;
    case HEXEN_SPR_ASK2:
      if (!P_GiveArtifact(player, hexen_arti_puzzskull2, special))
        return;
      sound = hexen_sfx_pickup_artifact;
      player->message = "FLAME MASK";
      break;
    case HEXEN_SPR_AFWP:
      if (!P_GiveArtifact(player, hexen_arti_puzzfweapon, special))
        return;
      sound = hexen_sfx_pickup_artifact;
      player->message = "GLAIVE SEAL";
      break;
    case HEXEN_SPR_ACWP:
      if (!P_GiveArtifact(player, hexen_arti_puzzcweapon, special))
        return;
      sound = hexen_sfx_pickup_artifact;
      player->message = "HOLY RELIC";
      break;
    case HEXEN_SPR_AMWP:
      if (!P_GiveArtifact(player, hexen_arti_puzzmweapon, special))
        return;
      sound = hexen_sfx_pickup_artifact;
      player->message = "SIGIL OF THE MAGUS";
      break;
    case HEXEN_SPR_AGER:
      if (!P_GiveArtifact(player, hexen_arti_puzzgear1, special))
        return;
      sound = hexen_sfx_pickup_artifact;
      player->message = "CLOCK GEAR";
      break;
    case HEXEN_SPR_AGR2:
      if (!P_GiveArtifact(player, hexen_arti_puzzgear2, special))
        return;
      sound = hexen_sfx_pickup_artifact;
      player->message = "CLOCK GEAR";
      break;
    case HEXEN_SPR_AGR3:
      if (!P_GiveArtifact(player, hexen_arti_puzzgear3, special))
        return;
      sound = hexen_sfx_pickup_artifact;
      player->message = "CLOCK GEAR";
      break;
    case HEXEN_SPR_AGR4:
      if (!P_GiveArtifact(player, hexen_arti_puzzgear4, special))
        return;
      sound = hexen_sfx_pickup_artifact;
      player->message = "CLOCK GEAR";
      break;
    case HEXEN_SPR_PTN2:           /* Quartz Flask (heal 25) */
      if (!P_GiveArtifact(player, hexen_arti_health, special))
        return;
      sound = hexen_sfx_pickup_artifact;
      player->message = "QUARTZ FLASK";
      break;
    case HEXEN_SPR_SPHL:           /* Mystic Urn (heal 100) */
      if (!P_GiveArtifact(player, hexen_arti_superhealth, special))
        return;
      sound = hexen_sfx_pickup_artifact;
      player->message = "MYSTIC URN";
      break;
    case HEXEN_SPR_INVU:           /* Icon of the Defender (invulnerability) */
      if (!P_GiveArtifact(player, hexen_arti_invulnerability, special))
        return;
      sound = hexen_sfx_pickup_artifact;
      player->message = "ICON OF THE DEFENDER";
      break;
    case HEXEN_SPR_TRCH:           /* Torch */
      if (!P_GiveArtifact(player, hexen_arti_torch, special))
        return;
      sound = hexen_sfx_pickup_artifact;
      player->message = "TORCH";
      break;
    case HEXEN_SPR_PORK:           /* Porkalator (egg) */
      if (!P_GiveArtifact(player, hexen_arti_egg, special))
        return;
      sound = hexen_sfx_pickup_artifact;
      player->message = "PORKALATOR";
      break;
    case HEXEN_SPR_SOAR:           /* Wings of Wrath (flight) */
      if (!P_GiveArtifact(player, hexen_arti_fly, special))
        return;
      sound = hexen_sfx_pickup_artifact;
      player->message = "WINGS OF WRATH";
      break;
    case HEXEN_SPR_SPED:           /* Boots of Speed */
      if (!P_GiveArtifact(player, hexen_arti_speed, special))
        return;
      sound = hexen_sfx_pickup_artifact;
      player->message = "BOOTS OF SPEED";
      break;
    case HEXEN_SPR_BMAN:           /* Krater of Might (boost mana) */
      if (!P_GiveArtifact(player, hexen_arti_boostmana, special))
        return;
      sound = hexen_sfx_pickup_artifact;
      player->message = "KRATER OF MIGHT";
      break;
    case HEXEN_SPR_PSBG:           /* Flechette (poison bag) */
      if (!P_GiveArtifact(player, hexen_arti_poisonbag, special))
        return;
      sound = hexen_sfx_pickup_artifact;
      player->message = "FLECHETTE";
      break;
    case HEXEN_SPR_BLST:           /* Disc of Repulsion (blast radius) */
      if (!P_GiveArtifact(player, hexen_arti_blastradius, special))
        return;
      sound = hexen_sfx_pickup_artifact;
      player->message = "DISC OF REPULSION";
      break;
    default:
      /* Unhandled Hexen pickup (other classes' weapons, weapon pieces,
       * artifacts, keys): leave it in the world rather than removing it. */
      return;
  }

  if (special->flags & MF_COUNTITEM)
    player->itemcount++;
  /* A picked-up thing fires its action special with the toucher as the
   * activator (vanilla Hexen, e.g. picking up the key that seals a door). */
  if (special->special)
  {
    byte b[5];
    int  a;
    for (a = 0; a < 5; a++)
      b[a] = (byte) special->special_args[a];
    P_ExecuteHexenLineSpecial(special->special, b, NULL, 0, toucher);
    special->special = 0;
  }
  P_RemoveMobj(special);
  player->bonuscount += BONUSADD;
  if (!comp[comp_sound] || player == &players[displayplayer])
    S_StartSound(player->mo, sound | PICKUP_SOUND);
}
// killough 11/98: make static
//
// KillMobj
//
static void P_KillMobj(mobj_t *source, mobj_t *target)
{
  target->flags &= ~(MF_SHOOTABLE|MF_FLOAT|MF_SKULLFLY);

  if (!(target->flags & MF_DONTFALL))
    target->flags &= ~MF_NOGRAVITY;

  /* Raven: a corpse must not keep pass-over/under (z-clip) behaviour, or its
   * floorz can be computed over other things and it never settles onto the
   * real floor -- a flying monster (e.g. the Gargoyle) would then hang in its
   * in-air death frame instead of falling and running its crash/gib state. */
  target->flags2 &= ~MF2_PASSMOBJ;

  target->flags |= MF_CORPSE|MF_DROPOFF;
  target->height >>= 2;

  if (!((target->flags ^ MF_COUNTKILL) & (MF_FRIEND | MF_COUNTKILL)))
    totallive--;

  /* Hexen: a dying monster (or the bell) fires its action special, with the
   * dying thing as the activator (vanilla behaviour).  The boss sorcerer's
   * special is an ACS script number instead. */
  if (hexen && target->special &&
      (target->flags & MF_COUNTKILL || target->type == HEXEN_MT_ZBELL))
  {
    if (target->type == HEXEN_MT_SORCBOSS)
    {
      byte dummyArgs[3] = {0, 0, 0};
      P_StartACS(target->special, 0, dummyArgs, target, NULL, 0);
    }
    else
    {
      byte b[5];
      int  a;
      for (a = 0; a < 5; a++)
        b[a] = (byte) target->special_args[a];
      P_ExecuteHexenLineSpecial(target->special, b, NULL, 0, target);
    }
  }

  if (source && source->player)
    {
      // count for intermission
      if (target->flags & MF_COUNTKILL)
        source->player->killcount++;
      if (target->player)
        source->player->frags[target->player-players]++;
    }
    else
      if (target->flags & MF_COUNTKILL) { /* Add to kills tally */
  if ((compatibility_level < lxdoom_1_compatibility) || !netgame) {
    if (!netgame)
      // count all monster deaths,
      // even those caused by other monsters
      players[0].killcount++;
  } else
    if (!deathmatch) {
      // try and find a player to give the kill to, otherwise give the
      // kill to a random player.  this fixes the missing monsters bug
      // in coop - rain
      // CPhipps - not a bug as such, but certainly an inconsistency.
      if (target->lastenemy && target->lastenemy->health > 0
    && target->lastenemy->player) // Fighting a player
          target->lastenemy->player->killcount++;
        else {
        // cph - randomely choose a player in the game to be credited
        //  and do it uniformly between the active players
        unsigned int activeplayers = 0, player, i;

        for (player = 0; player<MAXPLAYERS; player++)
    if (playeringame[player])
      activeplayers++;

        if (activeplayers) {
    player = P_Random(pr_friends) % activeplayers;

    for (i=0; i<MAXPLAYERS; i++)
      if (playeringame[i])
        if (!player--)
          players[i].killcount++;
        }
      }
    }
      }

  if (target->player)
    {
      // count environment kills against you
      if (!source)
        target->player->frags[target->player-players]++;

      target->flags &= ~MF_SOLID;
      target->player->playerstate = PST_DEAD;
      P_DropWeapon (target->player);

      if (target->player == &players[consoleplayer] && (automapmode & am_active))
        AM_Stop();    // don't die in auto map; switch view prior to dying
    }

  /* Heretic uses a more lenient extreme-death (gib) threshold than Doom:
   * a thing gibs once its health falls below negative half its spawn
   * health, where Doom requires it to fall below negative full spawn
   * health. Using the Doom threshold under Heretic makes monsters far too
   * hard to gib, so they leave a normal-death body where the extreme
   * death (which removes the body) should have played. */
  {
    int gib_threshold = heretic ? -(target->info->spawnhealth >> 1)
                                 : -target->info->spawnhealth;
    if (target->health < gib_threshold && target->info->xdeathstate)
      P_SetMobjState (target, target->info->xdeathstate);
    else
      P_SetMobjState (target, target->info->deathstate);
  }

  target->tics -= P_Random(pr_killtics)&3;

  if (target->tics < 1)
    target->tics = 1;

  // Drop stuff.
  // This determines the kind of object spawned
  // during the death frame of a thing.
  //
  // Raven: Heretic mobjinfo has no droppeditem field, so every Heretic
  // actor's droppeditem reads as 0 -- which is NOT MT_NULL (-1), so this
  // Doom path fired on every Heretic death and spawned mobjinfo[0] (an
  // empty type that renders state 0 / the IMPX gargoyle sprite). Heretic
  // does its own drops from A_NoBlocking via P_DropItem, so skip this.
  if (!heretic && target->info->droppeditem != MT_NULL)
  {
    mobj_t     *mo;
    mo = P_SpawnMobj (target->x,target->y,ONFLOORZ, target->info->droppeditem);
    mo->flags |= MF_DROPPED;    // special versions of items
  }
}

//
// P_DamageMobj
// Damages both enemies and players
// "inflictor" is the thing that caused the damage
//  creature or missile, can be NULL (slime, etc)
// "source" is the thing to target after taking damage
//  creature or NULL
// Source and inflictor are the same for melee attacks.
// Source can be NULL for slime, barrel explosions
// and other environmental stuff.
//

/* MBF21: two things in the same (non-default) infighting group will not
 * retaliate against each other after taking damage.  Inert below complevel
 * 21 because all groups are IG_DEFAULT unless an MBF21 deh patch changed
 * them and mbf21_features is active. */
static dbool P_InfightingImmune(mobj_t *target, mobj_t *source)
{
  return
    mobjinfo[target->type].infighting_group != IG_DEFAULT &&
    mobjinfo[target->type].infighting_group == mobjinfo[source->type].infighting_group;
}

/* Hexen poison: P_PoisonPlayer raises the player's poison level (the tick in
 * P_PlayerThink then drains it as P_PoisonDamage hits); P_PoisonDamage is the
 * armor-ignoring damage path poison uses, after Raven's code. */
void P_PoisonPlayer(player_t *player, mobj_t *poisoner, int poison)
{
  if ((player->cheats & CF_GODMODE) || player->powers[pw_invulnerability])
    return;
  player->poisoncount += poison;
  player->poisoner = poisoner;
  if (player->poisoncount > 100)
    player->poisoncount = 100;
}

void P_PoisonDamage(player_t *player, mobj_t *source, int damage,
                    dbool playPainSound)
{
  mobj_t *target = player->mo;
  mobj_t *inflictor = source;

  if (target->health <= 0)
    return;
  if (target->flags2 & MF2_INVULNERABLE && damage < 10000)
    return;
  if (gameskill == sk_baby)
    damage >>= 1;               /* take half damage in trainer mode */
  if (damage < 1000 &&
      ((player->cheats & CF_GODMODE) || player->powers[pw_invulnerability]))
    return;
  player->health -= damage;
  if (player->health < 0)
    player->health = 0;
  player->attacker = source;

  target->health -= damage;
  if (target->health <= 0)
  {                             /* death */
    target->special1.i = damage;
    if (inflictor && !player->morphTics)
    {                           /* flame/ice death */
      if ((inflictor->flags2 & MF2_FIREDAMAGE) &&
          (target->health > -50) && (damage > 25))
        target->flags2 |= MF2_FIREDAMAGE;
      if (inflictor->flags2 & MF2_ICEDAMAGE)
        target->flags2 |= MF2_ICEDAMAGE;
    }
    P_KillMobj(source, target);
    return;
  }
  if (!(leveltime & 63) && playPainSound)
    P_SetMobjState(target, target->info->painstate);
}

void P_DamageMobj(mobj_t *target,mobj_t *inflictor, mobj_t *source, int damage)
{
  player_t *player;
  dbool   justhit = FALSE;          /* killough 11/98 */

  /* killough 8/31/98: allow bouncers to take damage */
  if (!(target->flags & (MF_SHOOTABLE | MF_BOUNCES)))
    return; // shouldn't happen...

  if (target->health <= 0)
    return;

  /* Hexen: the Banishment Device's projectiles teleport their victim away
   * instead of damaging it - players always, monsters unless they are
   * serpents, bosses, or not counted kills. */
  if (hexen && inflictor)
  {
    switch (inflictor->type)
    {
      case HEXEN_MT_POISONDART:
        if (target->player)
        {
          P_PoisonPlayer(target->player, source, 20);
          damage >>= 1;
        }
        break;
      case HEXEN_MT_POISONCLOUD:
        if (target->player)
        {
          if (target->player->poisoncount < 4)
          {
            P_PoisonDamage(target->player, source,
                           15 + (P_Random(pr_heretic) & 15), false);
            P_PoisonPlayer(target->player, source, 50);
            S_StartSound(target, hexen_sfx_player_poisoncough);
          }
          return;
        }
        else if (!(target->flags & MF_COUNTKILL))
        {                       /* clouds only hurt players and monsters */
          return;
        }
        break;
      case HEXEN_MT_FSWORD_MISSILE:
        if (target->player)
          damage -= damage >> 2;
        break;
      case HEXEN_MT_TELOTHER_FX1:
      case HEXEN_MT_TELOTHER_FX2:
      case HEXEN_MT_TELOTHER_FX3:
      case HEXEN_MT_TELOTHER_FX4:
      case HEXEN_MT_TELOTHER_FX5:
        if (target->player ||
            ((target->flags & MF_COUNTKILL) &&
             target->type != HEXEN_MT_SERPENT &&
             target->type != HEXEN_MT_SERPENTLEADER &&
             !(target->flags2 & MF2_BOSS)))
        {
          P_TeleportOther(target);
        }
        return;
      default:
        break;
    }
  }

  /* Hexen: a thing flagged MF2_INVULNERABLE shrugs off all ordinary damage
   * (used by the Centaur while it raises its shield).  A telefrag-scale
   * 10000+ hit still goes through.  For a player it is absolute. */
  if (hexen && (target->flags2 & MF2_INVULNERABLE) && damage < 10000)
  {
    if (target->player)
      return;                   /* for the player, no exceptions */
    if (inflictor)
    {
      switch (inflictor->type)
      {
        /* these inflictors aren't foiled by invulnerability */
        case HEXEN_MT_HOLY_FX:
        case HEXEN_MT_POISONCLOUD:
        case HEXEN_MT_FIREBOMB:
          break;
        default:
          return;
      }
    }
    else
      return;
  }

  if (target->flags & MF_SKULLFLY)
    target->momx = target->momy = target->momz = 0;

  player = target->player;
  if (player && gameskill == sk_baby)
    damage >>= 1;   // take half damage in trainer mode

  // Some close combat weapons should not
  // inflict thrust and push the victim out of reach,
  // thus kick away unless using the chainsaw.

  /* MBF21: a weapon flagged WPF_NOTHRUST imparts no thrust.  The chainsaw
   * carries WPF_NOTHRUST by default, so this reproduces the vanilla
   * chainsaw exception; below complevel 21 the hardcoded chainsaw check is
   * used unchanged. */
  if (inflictor && !(target->flags & MF_NOCLIP) &&
      (!source || !source->player ||
       (mbf21_features
        ? !(weaponinfo[source->player->readyweapon].flags & WPF_NOTHRUST)
        : source->player->readyweapon != WP_CHAINSAW)))
    {
      unsigned ang = R_PointToAngle2 (inflictor->x, inflictor->y,
                                      target->x,    target->y);

      fixed_t thrust = damage*(FRACUNIT>>3)*100/target->info->mass;

      // make fall forwards sometimes
      if ( damage < 40 && damage > target->health
           && target->z - inflictor->z > 64*FRACUNIT
           && P_Random(pr_damagemobj) & 1)
        {
          ang += ANG180;
          thrust *= 4;
        }

      ang >>= ANGLETOFINESHIFT;
      target->momx += FixedMul (thrust, finecosine[ang]);
      target->momy += FixedMul (thrust, finesine[ang]);

      /* killough 11/98: thrust objects hanging off ledges */
      if (target->intflags & MIF_FALLING && target->gear >= MAXGEAR)
        target->gear = 0;
    }

  // player specific
  if (player)
    {
      // end of game hell hack
      if (target->subsector->sector->special == 11 && damage >= target->health)
        damage = target->health - 1;

      // Below certain threshold,
      // ignore damage in GOD mode, or with INVUL power.
      // killough 3/26/98: make god mode 100% god mode in non-compat mode

      if ((damage < 1000 || (!comp[comp_god] && (player->cheats&CF_GODMODE))) &&
          (player->cheats&CF_GODMODE || player->powers[pw_invulnerability]))
        return;

      if (hexen)
        {
          /* Hexen armor: the absorbed fraction is the per-class innate save
           * plus the four armor pieces (capped at 100%); each piece is then
           * worn down in proportion to its class weight. */
          int     i;
          int     saved;
          int     cls = player->class;
          fixed_t savedPercent = hexen_class_armor[cls].auto_armor_save
                               + player->hexen_armorpoints[ARMOR_ARMOR]
                               + player->hexen_armorpoints[ARMOR_SHIELD]
                               + player->hexen_armorpoints[ARMOR_HELMET]
                               + player->hexen_armorpoints[ARMOR_AMULET];
          if (savedPercent)
            {
              if (savedPercent > 100 * FRACUNIT)
                savedPercent = 100 * FRACUNIT;
              for (i = 0; i < NUMARMOR; i++)
                {
                  if (player->hexen_armorpoints[i])
                    {
                      player->hexen_armorpoints[i] -= FixedDiv(
                        FixedMul(damage << FRACBITS,
                                 hexen_class_armor[cls].armor_increment[i]),
                        300 * FRACUNIT);
                      if (player->hexen_armorpoints[i] < 2 * FRACUNIT)
                        player->hexen_armorpoints[i] = 0;
                    }
                }
              saved = FixedDiv(FixedMul(damage << FRACBITS, savedPercent),
                               100 * FRACUNIT);
              if (saved > savedPercent * 2)
                saved = savedPercent * 2;
              damage -= saved >> FRACBITS;
            }
        }
      else if (player->armortype)
        {
          int saved = player->armortype == 1 ? damage/3 : damage/2;
          if (player->armorpoints <= saved)
            {
              // armor is used up
              saved = player->armorpoints;
              player->armortype = 0;
            }
          player->armorpoints -= saved;
          damage -= saved;
        }

      player->health -= damage;       // mirror mobj health here for Dave
      if (player->health < 0)
        player->health = 0;

      player->attacker = source;
      player->damagecount += damage;  // add damage after armor / invuln

      if (player->damagecount > 100)
        player->damagecount = 100;  // teleport stomp does 10k points...
    }

  // do the damage
  target->health -= damage;
  if (target->health <= 0)
    {
      P_KillMobj (source, target);
      return;
    }

  // killough 9/7/98: keep track of targets so that friends can help friends
  if (mbf_features)
    {
      /* If target is a player, set player's target to source,
       * so that a friend can tell who's hurting a player
       */
      if (player)
  P_SetTarget(&target->target, source);

      /* killough 9/8/98:
       * If target's health is less than 50%, move it to the front of its list.
       * This will slightly increase the chances that enemies will choose to
       * "finish it off", but its main purpose is to alert friends of danger.
       */
      if (target->health*2 < target->info->spawnhealth)
  {
    thinker_t *cap = &thinkerclasscap[target->flags & MF_FRIEND ?
             th_friends : th_enemies];
    (target->thinker.cprev->cnext = target->thinker.cnext)->cprev =
      target->thinker.cprev;
    (target->thinker.cnext = cap->cnext)->cprev = &target->thinker;
    (target->thinker.cprev = cap)->cnext = &target->thinker;
  }
    }

  if (P_Random (pr_painchance) < target->info->painchance &&
      !(target->flags & MF_SKULLFLY)) { //killough 11/98: see below
    if (mbf_features)
      justhit = TRUE;
    else
      target->flags |= MF_JUSTHIT;    // fight back!

    /* Hexen: centaurs and ettins caught in a poison cloud whimper */
    if (hexen && inflictor && inflictor->type == HEXEN_MT_POISONCLOUD)
    {
      if (target->flags & MF_COUNTKILL && P_Random(pr_heretic) < 128 &&
          !S_GetSoundPlayingInfo(target, hexen_sfx_puppybeat) &&
          (target->type == HEXEN_MT_CENTAUR ||
           target->type == HEXEN_MT_CENTAURLEADER ||
           target->type == HEXEN_MT_ETTIN))
        S_StartSound(target, hexen_sfx_puppybeat);
    }
    P_SetMobjState(target, target->info->painstate);
  }

  target->reactiontime = 0;           // we're awake now...

  /* killough 9/9/98: cleaned up, made more consistent: */

  /* MBF21: a source with DMGIGNORED is not retaliated against (archvile);
   * a target with NOTHRESHOLD has no targeting threshold (always retargets).
   * flags2 is zero outside complevel 21, so vanilla behaviour is preserved. */
  if (source && source != target && !(source->flags & MF_NOTARGET) &&
      !(source->flags2 & MF2_DMGIGNORED) &&
      !(mbf21_features && P_InfightingImmune(target, source)) &&
      (!target->threshold || (target->flags2 & MF2_NOTHRESHOLD) ||
       (target->flags & MF_QUICKTORETALIATE)) &&
      ((source->flags ^ target->flags) & MF_FRIEND ||
       monster_infighting ||
       !mbf_features))
    {
      /* if not intent on another player, chase after this one
       *
       * killough 2/15/98: remember last enemy, to prevent
       * sleeping early; 2/21/98: Place priority on players
       * killough 9/9/98: cleaned up, made more consistent:
       */

      if (!target->lastenemy || target->lastenemy->health <= 0 ||
    (!mbf_features ?
     !target->lastenemy->player :
     !((target->flags ^ target->lastenemy->flags) & MF_FRIEND) &&
     target->target != source)) // remember last enemy - killough
  P_SetTarget(&target->lastenemy, target->target);

      P_SetTarget(&target->target, source);       // killough 11/98
      target->threshold = BASETHRESHOLD;
      if (target->state == &states[target->info->spawnstate]
          && target->info->seestate != S_NULL)
        P_SetMobjState (target, target->info->seestate);
    }

  /* killough 11/98: Don't attack a friend, unless hit by that friend.
   * cph 2006/04/01 - implicitly this is only if mbf_features */
  if (justhit && (target->target == source || !target->target ||
      !(target->flags & target->target->flags & MF_FRIEND)))
    target->flags |= MF_JUSTHIT;    // fight back!
}
