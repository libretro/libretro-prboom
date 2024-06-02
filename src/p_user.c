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
 *      Player related stuff.
 *      Bobbing POV/weapon, movement.
 *      Pending weapon.
 *
 *-----------------------------------------------------------------------------*/

#include "doomstat.h"
#include "d_event.h"
#include "r_main.h"
#include "p_map.h"
#include "p_spec.h"
#include "p_user.h"
#include "r_demo.h"
#include "r_fps.h"
#include "g_game.h"

// Index of the special effects (INVUL inverse) map.

#define INVERSECOLORMAP 32

//
// Movement.
//

// 16 pixels of bob

#define MAXBOB  0x100000

dbool   onground; // whether player is on ground or in air

// max/min values for pitch angle
angle_t viewpitch_min;
angle_t viewpitch_max;


/*
==================
=
= P_Thrust
=
= moves the given origin along a given angle
=
==================
*/

void P_Thrust(player_t* player,angle_t angle,fixed_t move)
{
   angle >>= ANGLETOFINESHIFT;
#ifdef HEXEN
   if(player->powers[pw_flight] && !(player->mo->z <= player->mo->floorz))
   {
      player->mo->momx += FixedMul(move, finecosine[angle]);
      player->mo->momy += FixedMul(move, finesine[angle]);
   }
   else if(P_GetThingFloorType(player->mo) == FLOOR_ICE) // Friction_Low
   {
      player->mo->momx += FixedMul(move>>1, finecosine[angle]);
      player->mo->momy += FixedMul(move>>1, finesine[angle]);
   }
   else
#endif
   {
      player->mo->momx += FixedMul(move,finecosine[angle]);
      player->mo->momy += FixedMul(move,finesine[angle]);
   }
}


/*
 * P_Bob
 * Same as P_Thrust, but only affects bobbing.
 *
 * killough 10/98: We apply thrust separately between the real physical player
 * and the part which affects bobbing. This way, bobbing only comes from player
 * motion, nothing external, avoiding many problems, e.g. bobbing should not
 * occur on conveyors, unless the player walks on one, and bobbing should be
 * reduced at a regular rate, even on ice (where the player coasts).
 */

static void P_Bob(player_t *player, angle_t angle, fixed_t move)
{
  //e6y
  if (!mbf_features)
    return;

  player->momx += FixedMul(move,finecosine[angle >>= ANGLETOFINESHIFT]);
  player->momy += FixedMul(move,finesine[angle]);
}

/*
==================
=
= P_CalcHeight
=
= Calculate the walking / running height adjustment
=
==================
*/

void P_CalcHeight (player_t* player)
{
   int     angle;
   fixed_t bob;

   /* */
   /* regular movement bobbing (needs to be calculated for gun swing even */
   /* if not on ground) */
   /* OPTIMIZE: tablify angle  */

   if (!demo_compatibility && !player_bobbing)
      player->bob = 0;
   else
   {
      fixed_t x = mbf_features ? player->momx : player->mo->momx;
      fixed_t y = mbf_features ? player->momy : player->mo->momy;

      player->bob = (FixedMul(x,x) + FixedMul(y,y)) >> 2;
   }

   //e6y
   if (compatibility_level >= boom_202_compatibility &&
         compatibility_level <= lxdoom_1_compatibility &&
         player->mo->friction > ORIG_FRICTION) // ice?
   {
      if (player->bob > (MAXBOB>>2))
         player->bob = MAXBOB>>2;
   }
   else

      if (player->bob > MAXBOB)
         player->bob = MAXBOB;

   if (!onground || player->cheats & CF_NOMOMENTUM)
   {
      player->viewz = player->mo->z + VIEWHEIGHT;

      if (player->viewz > player->mo->ceilingz-4*FRACUNIT)
         player->viewz = player->mo->ceilingz-4*FRACUNIT;
      return;
   }

   angle = (FINEANGLES/20*leveltime)&FINEMASK;
   bob   = FixedMul(player->bob/2,finesine[angle]);

   /* move viewheight */

   if (player->playerstate == PST_LIVE)
   {
      player->viewheight += player->deltaviewheight;

      if (player->viewheight > VIEWHEIGHT)
      {
         player->viewheight = VIEWHEIGHT;
         player->deltaviewheight = 0;
      }

      if (player->viewheight < VIEWHEIGHT/2)
      {
         player->viewheight = VIEWHEIGHT/2;
         if (player->deltaviewheight <= 0)
            player->deltaviewheight = 1;
      }

      if (player->deltaviewheight)
      {
         player->deltaviewheight += FRACUNIT/4;
         if (!player->deltaviewheight)
            player->deltaviewheight = 1;
      }
   }

   player->viewz = player->mo->z + player->viewheight + bob;

   if (player->viewz > player->mo->ceilingz-4*FRACUNIT)
      player->viewz = player->mo->ceilingz-4*FRACUNIT;
}

/*
=================
=
= P_CheckPitch
=
= Corrects the pitch so it's within the set limits
=
=================
*/
void P_CheckPitch(angle_t *pitch)
{
  if((int)*pitch > (int)viewpitch_max)
    *pitch = viewpitch_max;
  else if((int)*pitch < (int)viewpitch_min)
    *pitch = viewpitch_min;
}

/*
=================
=
= P_SetPitch
=
= Free Look Stuff
=
=================
*/
void P_SetPitch(player_t *player)
{
  mobj_t *mo = player->mo;

  if (player == &players[consoleplayer])
  {
    if (!(demoplayback))
    {
      if (!mo->reactiontime && (!(automapmode & am_active) || (automapmode & am_overlay)))
      {
        mo->pitch += (mlooky << 16);
        P_CheckPitch(&mo->pitch);
        mlooky = 0;
      }
      else
      {
        mo->pitch = 0;
      }
      //R_DemoEx_WriteMLook(mo->pitch);
    }
    else
    {
      //mo->pitch = R_DemoEx_ReadMLook();
      //P_CheckPitch((signed int *)&mo->pitch);
    }
  }
  else
  {
    mo->pitch = 0;
  }
}

/*
=================
=
= P_MovePlayer
=
= Adds momentum if the player is not in the air
=
=================
*/

void P_MovePlayer (player_t* player)
{
  ticcmd_t *cmd = &player->cmd;
  mobj_t *mo = player->mo;

  mo->angle += cmd->angleturn << 16;

#ifdef HEXEN
  onground = (player->mo->z <= player->mo->floorz
        || (player->mo->flags2&MF2_ONMOBJ)) ? true : false;

  if(cmd->forwardmove)
  {
     if(onground || player->mo->flags2&MF2_FLY)
     {
        P_Thrust(player, player->mo->angle, ((int)cmd->forwardmove)*2048);
     }
     else
     {
        P_Thrust(player, player->mo->angle, FRACUNIT>>8);
     }
  }
  if(cmd->sidemove)
  {
     if(onground || player->mo->flags2&MF2_FLY)
     {
        P_Thrust(player, player->mo->angle-ANG90, cmd->sidemove*2048);
     }
     else
     {
        P_Thrust(player, player->mo->angle, FRACUNIT>>8);
     }
  }
  if(cmd->forwardmove || cmd->sidemove)
  {
     if(player->mo->state == &states[PStateNormal[player->class]])
     {
        P_SetMobjState(player->mo, PStateRun[player->class]);
     }
  }

  look = cmd->lookfly&15;
  if(look > 7)
  {
     look -= 16;
  }
  if(look)
  {
     if(look == TOCENTER)
     {
        player->centering = true;
     }
     else
     {
        player->lookdir += 5*look;
        if(player->lookdir > 90 || player->lookdir < -110)
        {
           player->lookdir -= 5*look;
        }
     }
  }
  if(player->centering)
  {
     if(player->lookdir > 0)
     {
        player->lookdir -= 8;
     }
     else if(player->lookdir < 0)
     {
        player->lookdir += 8;
     }
     if(abs(player->lookdir) < 8)
     {
        player->lookdir = 0;
        player->centering = false;
     }
  }
  fly = cmd->lookfly>>4;
  if(fly > 7)
  {
     fly -= 16;
  }
  if(fly && player->powers[pw_flight])
  {
     if(fly != TOCENTER)
     {
        player->flyheight = fly*2;
        if(!(player->mo->flags2&MF2_FLY))
        {
           player->mo->flags2 |= MF2_FLY;
           player->mo->flags |= MF_NOGRAVITY;
           if(player->mo->momz <= -39*FRACUNIT)
           { // stop falling scream
              S_StopSound(player->mo);
           }
        }
     }
     else
     {
        player->mo->flags2 &= ~MF2_FLY;
        player->mo->flags &= ~MF_NOGRAVITY;
     }
  }
  else if(fly > 0)
  {
     P_PlayerUseArtifact(player, arti_fly);
  }
  if(player->mo->flags2&MF2_FLY)
  {
     player->mo->momz = player->flyheight*FRACUNIT;
     if(player->flyheight)
     {
        player->flyheight /= 2;
     }
  }
#else
  onground = mo->z <= mo->floorz;

  // e6y
  if (demo_smoothturns && player == &players[displayplayer])
     R_SmoothPlaying_Add(cmd->angleturn << 16);

  // killough 10/98:
  //
  // We must apply thrust to the player and bobbing separately, to avoid
  // anomalies. The thrust applied to bobbing is always the same strength on
  // ice, because the player still "works just as hard" to move, while the
  // thrust applied to the movement varies with 'movefactor'.

  //e6y
  if ((!demo_compatibility && !mbf_features) || (cmd->forwardmove | cmd->sidemove)) // killough 10/98
  {
     if (onground || mo->flags & MF_BOUNCES) // killough 8/9/98
     {
        int friction, movefactor = P_GetMoveFactor(mo, &friction);

        // killough 11/98:
        // On sludge, make bobbing depend on efficiency.
        // On ice, make it depend on effort.

        int bobfactor =
           friction < ORIG_FRICTION ? movefactor : ORIG_FRICTION_FACTOR;

        if (cmd->forwardmove)
        {
           P_Bob(player,mo->angle,cmd->forwardmove*bobfactor);
           P_Thrust(player,mo->angle,cmd->forwardmove*movefactor);
        }

        if (cmd->sidemove)
        {
           P_Bob(player,mo->angle-ANG90,cmd->sidemove*bobfactor);
           P_Thrust(player,mo->angle-ANG90,cmd->sidemove*movefactor);
        }
     }
     if (mo->state == states+S_PLAY)
        P_SetMobjState(mo,S_PLAY_RUN1);
  }
#endif
}

#define ANG5 (ANG90/18)

/*
=================
=
= P_DeathThink
=
=  Fall on your face when dying.
=  Decrease POV height to floor height.
=
=================
*/

void P_DeathThink (player_t* player)
{
   angle_t angle;
   angle_t delta;

   P_MovePsprites (player);

#ifdef HEXEN
   onground = (player->mo->z <= player->mo->floorz);
   if(player->mo->type == MT_BLOODYSKULL || player->mo->type == MT_ICECHUNK)
   { // Flying bloody skull or flying ice chunk
      player->viewheight = 6*FRACUNIT;
      player->deltaviewheight = 0;
      //player->damagecount = 20;
      if(onground)
      {
         if(player->lookdir < 60)
         {
            lookDelta = (60-player->lookdir)/8;
            if(lookDelta < 1 && (leveltime&1))
            {
               lookDelta = 1;
            }
            else if(lookDelta > 6)
            {
               lookDelta = 6;
            }
            player->lookdir += lookDelta;
         }
      }
   }
   else if(!(player->mo->flags2&MF2_ICEDAMAGE))
   { // Fall to ground (if not frozen)
      player->deltaviewheight = 0;
      if(player->viewheight > 6*FRACUNIT)
      {
         player->viewheight -= FRACUNIT;
      }
      if(player->viewheight < 6*FRACUNIT)
      {
         player->viewheight = 6*FRACUNIT;
      }
      if(player->lookdir > 0)
      {
         player->lookdir -= 6;
      }
      else if(player->lookdir < 0)
      {
         player->lookdir += 6;
      }
      if(abs(player->lookdir) < 6)
      {
         player->lookdir = 0;
      }
   }
#else
   /* fall to the ground */
   if (player->viewheight > 6*FRACUNIT)
      player->viewheight -= FRACUNIT;
   if (player->viewheight < 6*FRACUNIT)
      player->viewheight = 6*FRACUNIT;
   player->deltaviewheight = 0;
   onground = (player->mo->z <= player->mo->floorz);
#endif
   P_CalcHeight (player);

#ifdef HEXEN
   if(player->attacker && player->attacker != player->mo)
   { // Watch killer
      dir = P_FaceMobj(player->mo, player->attacker, &delta);
      if(delta < ANGLE_1*10)
      { // Looking at killer, so fade damage and poison counters
         if(player->damagecount)
         {
            player->damagecount--;
         }
         if(player->poisoncount)
         {
            player->poisoncount--;
         }
      }
      delta = delta/8;
      if(delta > ANGLE_1*5)
      {
         delta = ANGLE_1*5;
      }
      if(dir)
      { // Turn clockwise
         player->mo->angle += delta;
      }
      else
      { // Turn counter clockwise
         player->mo->angle -= delta;
      }
   }
   else if(player->damagecount || player->poisoncount)
   {
      if(player->damagecount)
      {
         player->damagecount--;
      }
      else
      {
         player->poisoncount--;
      }
   }
#else
   if (player->attacker && player->attacker != player->mo)
   {
      /* watch killer */
      angle = R_PointToAngle2 (player->mo->x,
            player->mo->y, player->attacker->x,
            player->attacker->y);
      delta = angle - player->mo->angle;
      if (delta < ANG5 || delta > (unsigned)-ANG5)
      {
         /* Looking at killer, so fade damage flash down. */
         player->mo->angle = angle;
         if (player->damagecount)
            player->damagecount--;
      }
      else if (delta < ANG180)
         player->mo->angle += ANG5;
      else
         player->mo->angle -= ANG5;
   }
   else if (player->damagecount)
      player->damagecount--;
#endif

   if (player->cmd.buttons & BT_USE)
   {
#ifdef HEXEN
      if(player == &players[consoleplayer])
		{
			I_SetPalette((byte *)W_CacheLumpName("PLAYPAL", PU_CACHE));
			inv_ptr = 0;
			curpos = 0;
			newtorch = 0;
			newtorchdelta = 0;
		}
#endif

      player->playerstate = PST_REBORN;

#ifdef HEXEN
      player->mo->special1 = player->class;
		if(player->mo->special1 > 2)
			player->mo->special1 = 0;
		// Let the mobj know the player has entered the reborn state.  Some
		// mobjs need to know when it's ok to remove themselves.
      player->mo->special2 = 666;
#endif
   }

   R_SmoothPlaying_Reset(player); // e6y
}

#ifdef HEXEN
//----------------------------------------------------------------------------
//
// PROC P_MorphPlayerThink
//
//----------------------------------------------------------------------------

void P_MorphPlayerThink(player_t *player)
{
	mobj_t *pmo;

	if(player->morphTics&15)
		return;

	pmo = player->mo;
	if(!(pmo->momx+pmo->momy) && P_Random() < 64)
	{ // Snout sniff
		P_SetPspriteNF(player, ps_weapon, S_SNOUTATK2);
		S_StartSound(pmo, SFX_PIG_ACTIVE1); // snort
		return;
	}
	if(P_Random() < 48)
	{
		if(P_Random() < 128)
		{
			S_StartSound(pmo, SFX_PIG_ACTIVE1);
		}
		else
		{
			S_StartSound(pmo, SFX_PIG_ACTIVE2);
		}
	}
}

//----------------------------------------------------------------------------
//
// FUNC P_UndoPlayerMorph
//
//----------------------------------------------------------------------------

dbool   P_UndoPlayerMorph(player_t *player)
{
   mobj_t *fog;
   mobj_t *mo;
   mobj_t *pmo;
   fixed_t x;
   fixed_t y;
   fixed_t z;
   angle_t angle;
   int playerNum;
   weapontype_t weapon;
   int oldFlags;
   int oldFlags2;
   int oldBeast;

   pmo = player->mo;
   x = pmo->x;
   y = pmo->y;
   z = pmo->z;
   angle = pmo->angle;
   weapon = pmo->special1;
   oldFlags = pmo->flags;
   oldFlags2 = pmo->flags2;
   oldBeast = pmo->type;
   P_SetMobjState(pmo, S_FREETARGMOBJ);
   playerNum = P_GetPlayerNum(player);
   switch(PlayerClass[playerNum])
   {
      case PCLASS_FIGHTER:
         mo = P_SpawnMobj(x, y, z, MT_PLAYER_FIGHTER);
         break;
      case PCLASS_CLERIC:
         mo = P_SpawnMobj(x, y, z, MT_PLAYER_CLERIC);
         break;
      case PCLASS_MAGE:
         mo = P_SpawnMobj(x, y, z, MT_PLAYER_MAGE);
         break;
      default:
         I_Error("P_UndoPlayerMorph:  Unknown player class %d\n",
               player->class);
   }
   if(P_TestMobjLocation(mo) == false)
   { // Didn't fit
      P_RemoveMobj(mo);
      mo = P_SpawnMobj(x, y, z, oldBeast);
      mo->angle = angle;
      mo->health = player->health;
      mo->special1 = weapon;
      mo->player = player;
      mo->flags = oldFlags;
      mo->flags2 = oldFlags2;
      player->mo = mo;
      player->morphTics = 2*35;
      return(false);
   }
   if(player->class == PCLASS_FIGHTER)
   {
      // The first type should be blue, and the third should be the
      // Fighter's original gold color
      if(playerNum == 0)
      {
         mo->flags |= 2<<MF_TRANSSHIFT;
      }
      else if(playerNum != 2)
      {
         mo->flags |= playerNum<<MF_TRANSSHIFT;
      }
   }
   else if(playerNum)
   { // Set color translation bits for player sprites
      mo->flags |= playerNum<<MF_TRANSSHIFT;
   }
   mo->angle = angle;
   mo->player = player;
   mo->reactiontime = 18;
   if(oldFlags2&MF2_FLY)
   {
      mo->flags2 |= MF2_FLY;
      mo->flags |= MF_NOGRAVITY;
   }
   player->morphTics = 0;
   player->health = mo->health = MAXHEALTH;
   player->mo = mo;
   player->class = PlayerClass[playerNum];
   angle >>= ANGLETOFINESHIFT;
   fog = P_SpawnMobj(x+20*finecosine[angle],
         y+20*finesine[angle], z+TELEFOGHEIGHT, MT_TFOG);
   S_StartSound(fog, SFX_TELEPORT);
   P_PostMorphWeapon(player, weapon);
   return(true);
}
#endif

/*
=================
=
= P_PlayerThink
=
=================
*/

void P_PlayerThink (player_t* player)
{
   ticcmd_t*    cmd;
   weapontype_t newweapon;

   if (movement_smooth && &players[displayplayer] == player)
   {
      player->mo->PrevX = player->mo->x;
      player->mo->PrevY = player->mo->y;
      player->prev_viewz = player->viewz;
      player->prev_viewangle = R_SmoothPlaying_Get(player->mo->angle) + viewangleoffset;
      player->prev_viewpitch = player->mo->pitch + viewpitchoffset;
   }
   if (player->mo == 0)
      return;

   // killough 2/8/98, 3/21/98:
   if (player->cheats & CF_NOCLIP)
      player->mo->flags |= MF_NOCLIP;
   else
      player->mo->flags &= ~MF_NOCLIP;

   cmd = &player->cmd;

   /* chain saw run forward */
   if (player->mo->flags & MF_JUSTATTACKED)
   {
      cmd->angleturn     = 0;
      cmd->forwardmove   = 0xc800/512;
      cmd->sidemove      = 0;
      player->mo->flags &= ~MF_JUSTATTACKED;
   }

#ifdef HEXEN
   // messageTics is above the rest of the counters so that messages will
   // 		go away, even in death.
   player->messageTics--; // Can go negative
   if(!player->messageTics || player->messageTics == -1)
   { // Refresh the screen when a message goes away
      player->ultimateMessage = false; // clear out any chat messages.
      player->yellowMessage = false;
      if(player == &players[consoleplayer])
      {
         BorderTopRefresh = true;
      }
   }
   player->worldTimer++;
#endif

   if (player->playerstate == PST_DEAD)
   {
      P_DeathThink (player);
      return;
   }

#ifdef HEXEN
   if(player->jumpTics)
      player->jumpTics--;
   if(player->morphTics)
      P_MorphPlayerThink(player);
#endif

   // Move around.
   // Reactiontime is used to prevent movement
   //  for a bit after a teleport.

   if (player->mo->reactiontime)
      player->mo->reactiontime--;
   else
      P_MovePlayer (player);

   P_SetPitch(player); /* Determines view pitch */
   P_CalcHeight (player); /* Determines view height and bobbing */

   // Determine if there's anything about the sector you're in that's
   // going to affect you, like painful floors.

   if (player->mo->subsector->sector->special)
      P_PlayerInSpecialSector (player);

#ifdef HEXEN
   if((floorType = P_GetThingFloorType(player->mo)) != FLOOR_SOLID)
      P_PlayerOnSpecialFlat(player, floorType);

   switch(player->class)
   {
      case PCLASS_FIGHTER:
         if(player->mo->momz <= -35*FRACUNIT
               && player->mo->momz >= -40*FRACUNIT && !player->morphTics
               && !S_GetSoundPlayingInfo(player->mo,
                  SFX_PLAYER_FIGHTER_FALLING_SCREAM))
         {
            S_StartSound(player->mo,
                  SFX_PLAYER_FIGHTER_FALLING_SCREAM);
         }
         break;
      case PCLASS_CLERIC:
         if(player->mo->momz <= -35*FRACUNIT
               && player->mo->momz >= -40*FRACUNIT && !player->morphTics
               && !S_GetSoundPlayingInfo(player->mo,
                  SFX_PLAYER_CLERIC_FALLING_SCREAM))
         {
            S_StartSound(player->mo,
                  SFX_PLAYER_CLERIC_FALLING_SCREAM);
         }
         break;
      case PCLASS_MAGE:
         if(player->mo->momz <= -35*FRACUNIT
               && player->mo->momz >= -40*FRACUNIT && !player->morphTics
               && !S_GetSoundPlayingInfo(player->mo,
                  SFX_PLAYER_MAGE_FALLING_SCREAM))
         {
            S_StartSound(player->mo,
                  SFX_PLAYER_MAGE_FALLING_SCREAM);
         }
         break;
      default:
         break;
   }
   if(cmd->arti)
   { // Use an artifact
      if((cmd->arti&AFLAG_JUMP) && onground && !player->jumpTics)
      {
         if(player->morphTics)
         {
            player->mo->momz = 6*FRACUNIT;
         }
         else
         {
            player->mo->momz = 9*FRACUNIT;
         }
         player->mo->flags2 &= ~MF2_ONMOBJ;
         player->jumpTics = 18;
      }
      else if(cmd->arti&AFLAG_SUICIDE)
      {
         P_DamageMobj(player->mo, NULL, NULL, 10000);
      }
      if(cmd->arti == NUMARTIFACTS)
      { // use one of each artifact (except puzzle artifacts)
         int i;

         for(i = 1; i < arti_firstpuzzitem; i++)
         {
            P_PlayerUseArtifact(player, i);
         }
      }
      else
      {
         P_PlayerUseArtifact(player, cmd->arti&AFLAG_MASK);
      }
   }
   // Check for weapon change
   if(cmd->buttons&BT_SPECIAL)
   { // A special event has no other buttons
      cmd->buttons = 0;
   }

#endif

   /* Check for weapon change. */

   if(cmd->buttons&BT_CHANGE
#ifdef HEXEN
         && !player->morphTics
#endif
         )
   {
      // The actual changing of the weapon is done when the weapon
      // psprite can do it (A_WeaponReady), so it doesn't happen in
      // the middle of an attack.
      newweapon = (cmd->buttons&BT_WEAPONMASK)>>BT_WEAPONSHIFT;

      // killough 3/22/98: For demo compatibility we must perform the fist
      // and SSG weapons switches here, rather than in G_BuildTiccmd(). For
      // other games which rely on user preferences, we must use the latter.

#ifdef HEXEN
      if(player->weaponowned[newweapon]
            && newweapon != player->readyweapon)
         player->pendingweapon = newweapon;
#else
      if (demo_compatibility)
      { // compatibility mode -- required for old demos -- killough
         if (newweapon == WP_FIST && player->weaponowned[WP_CHAINSAW] &&
               (player->readyweapon != WP_CHAINSAW ||
                !player->powers[pw_strength]))
            newweapon = WP_CHAINSAW;
         if (gamemode == commercial &&
               newweapon == WP_SHOTGUN &&
               player->weaponowned[WP_SUPERSHOTGUN] &&
               player->readyweapon != WP_SUPERSHOTGUN)
            newweapon = WP_SUPERSHOTGUN;
      }

      // killough 2/8/98, 3/22/98 -- end of weapon selection changes

      if (player->weaponowned[newweapon] && newweapon != player->readyweapon)

         // Do not go to plasma or BFG in shareware,
         //  even if cheated.

         if ((newweapon != WP_PLASMA && newweapon != WP_BFG)
               || (gamemode != shareware) )
            player->pendingweapon = newweapon;
#endif
   }

   /* check for use */
   if (cmd->buttons & BT_USE)
   {
      if (!player->usedown)
      {
         P_UseLines (player);
         player->usedown = TRUE;
      }
   }
   else
      player->usedown = FALSE;

#ifdef HEXEN
   // Morph counter
   if(player->morphTics)
   {
      if(!--player->morphTics)
      { // Attempt to undo the pig
         P_UndoPlayerMorph(player);
      }
   }
#endif

   /* cycle psprites */
   P_MovePsprites (player);

   // Counters, time dependent power ups.

   // Strength counts up to diminish fade.

   if (player->powers[pw_strength])
      player->powers[pw_strength]++;

   /* Other Counters */
   if (player->powers[pw_invulnerability])
   {
#ifdef HEXEN
      if(player->class == PCLASS_CLERIC)
      {
         if(!(leveltime&7) && player->mo->flags&MF_SHADOW
               && !(player->mo->flags2&MF2_DONTDRAW))
         {
            player->mo->flags &= ~MF_SHADOW;
            if(!(player->mo->flags&MF_ALTSHADOW))
            {
               player->mo->flags2 |= MF2_DONTDRAW|MF2_NONSHOOTABLE;
            }
         }
         if(!(leveltime&31))
         {
            if(player->mo->flags2&MF2_DONTDRAW)
            {
               if(!(player->mo->flags&MF_SHADOW))
               {
                  player->mo->flags |= MF_SHADOW|MF_ALTSHADOW;
               }
               else
               {
                  player->mo->flags2 &= ~(MF2_DONTDRAW|MF2_NONSHOOTABLE);
               }
            }
            else
            {
               player->mo->flags |= MF_SHADOW;
               player->mo->flags &= ~MF_ALTSHADOW;
            }
         }
      }
      if(!(--player->powers[pw_invulnerability]))
      {
         player->mo->flags2 &= ~(MF2_INVULNERABLE|MF2_REFLECTIVE);
         if(player->class == PCLASS_CLERIC)
         {
            player->mo->flags2 &= ~(MF2_DONTDRAW|MF2_NONSHOOTABLE);
            player->mo->flags &= ~(MF_SHADOW|MF_ALTSHADOW);
         }
      }
#else
      player->powers[pw_invulnerability]--;
#endif
   }

#ifdef HEXEN
   if(player->powers[pw_minotaur])
		player->powers[pw_minotaur]--;
#endif

   if (player->powers[pw_invisibility])
      if (! --player->powers[pw_invisibility] )
         player->mo->flags &= ~MF_SHADOW;


#ifdef HEXEN
   if(player->powers[pw_flight] && netgame)
   {
      if(!--player->powers[pw_flight])
      {
         if(player->mo->z != player->mo->floorz)
         {
            player->centering = true;
         }
         player->mo->flags2 &= ~MF2_FLY;
         player->mo->flags &= ~MF_NOGRAVITY;
         BorderTopRefresh = true; //make sure the sprite's cleared out
      }
   }
   if(player->powers[pw_speed])
      player->powers[pw_speed]--;
#else

   if (player->powers[pw_ironfeet])
      player->powers[pw_ironfeet]--;
#endif

   if (player->damagecount)
      player->damagecount--;

   if (player->bonuscount)
      player->bonuscount--;

#ifdef HEXEN
   if(player->poisoncount && !(leveltime&15))
   {
      player->poisoncount -= 5;
      if(player->poisoncount < 0)
      {
         player->poisoncount = 0;
      }
      P_PoisonDamage(player, player->poisoner, 1, true);
   }
#endif

   if (player->powers[pw_infrared])
   {
#ifdef HEXEN
      if (player->powers[pw_infrared] <= BLINKTHRESHOLD)
      {
         if(player->powers[pw_infrared]&8)
         {
            player->fixedcolormap = 0;
         }
         else
         {
            player->fixedcolormap = 1;
         }
      }
      else if(!(leveltime&16) && player == &players[consoleplayer])
      {
         if(newtorch)
         {
            if(player->fixedcolormap+newtorchdelta > 7
                  || player->fixedcolormap+newtorchdelta < 1
                  || newtorch == player->fixedcolormap)
               newtorch = 0;
            else
               player->fixedcolormap += newtorchdelta;
         }
         else
         {
            newtorch = (M_Random()&7)+1;
            newtorchdelta = (newtorch == player->fixedcolormap) ?
               0 : ((newtorch > player->fixedcolormap) ? 1 : -1);
         }
      }
#else
      player->powers[pw_infrared]--;
#endif
   }
#ifdef HEXEN
   else
   {
      player->fixedcolormap = 0;
   }
#else
   /* Handling colormaps. */
   player->fixedcolormap = player->powers[pw_invulnerability] > 4*32 ||
      player->powers[pw_invulnerability] & 8 ? INVERSECOLORMAP :
      player->powers[pw_infrared] > 4*32 || player->powers[pw_infrared] & 8;
#endif

}
