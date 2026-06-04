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
#include "map_format.h"
#include "p_user.h"
#include "r_demo.h"
#include "r_fps.h"
#include "g_game.h"
#include "m_random.h"
#include "s_sound.h"
#include "sounds.h"
#include "p_inter.h"
#include "p_pspr.h"
#include "heretic/p_action.h"
#include "p_tick.h"

// Index of the special effects (INVUL inverse) map.

#define INVERSECOLORMAP 32

//
// Movement.
//

// 16 pixels of bob

#define MAXBOB  0x100000

/* Heretic look-recenter sentinel: a look delta of -8 in the lookfly nibble
 * means "recenter the view" rather than a normal tilt step. */
#define TOCENTER_LOOK (-8)

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
        if (raven && !movement_mouselook)
        {
          /* Heretic keyboard look (look up / down / centre) is a core
           * gameplay feature.  P_MovePlayer maintains player->lookdir (the
           * Heretic look angle, +up / -down, clamped to +90 / -110) and it
           * already drives autoaim slope; feed it into mo->pitch so the
           * view tilts to match.  Scale follows dsda-doom's
           * dsda_PlayerPitch: pitch = lookdir * ANG1 / PI, which keeps the
           * rendered tilt within a comfortable range (~+28 / -35 degrees of
           * view pitch at the +90 / -110 lookdir extremes) rather than a
           * literal degree-for-degree tilt.  The mapping is negated because
           * this renderer's viewpitch is positive looking *down* (centery is
           * shifted down for positive pitch), the opposite of Heretic's
           * lookdir convention where positive is up -- without the negation,
           * looking up tilts the view down into the floor.  lookdir is
           * already clamped in P_MovePlayer, so no P_CheckPitch is applied. */
          mo->pitch = (angle_t)(-(double)player->lookdir * (double)ANG1 / 3.14159265358979323846);
          mlooky = 0;
        }
        else
        {
          mo->pitch += (mlooky << 16);
          P_CheckPitch(&mo->pitch);
          mlooky = 0;
        }
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
     if (mo->state == states+g_s_play)
        P_SetMobjState(mo,g_s_play_run1);
  }

  /* Heretic keyboard look: decode cmd->lookfly into player->lookdir.  The
   * original decode lived only in the HEXEN branch above (compiled out), so
   * lookdir never moved in this build -- which left both the autoaim slope
   * and the view pitch (see P_SetPitch) stuck at level.  Mirror that logic
   * here, gated on raven, so look up / down / centre work.  The low nibble
   * of lookfly is a signed look delta (-8..+7); -8 (TOCENTER) requests a
   * recenter.  lookdir is clamped to Heretic's +90 / -110 range. */
  if (raven)
  {
    int look = cmd->lookfly & 15;
    if (look > 7)
      look -= 16;
    if (look)
    {
      if (look == TOCENTER_LOOK)
        player->centering = true;
      else
      {
        player->lookdir += 5 * look;
        if (player->lookdir > 90 || player->lookdir < -110)
          player->lookdir -= 5 * look;
      }
    }
    if (player->centering)
    {
      if (player->lookdir > 0)
        player->lookdir -= 8;
      else if (player->lookdir < 0)
        player->lookdir += 8;
      if (abs(player->lookdir) < 8)
      {
        player->lookdir = 0;
        player->centering = false;
      }
    }

    /* Heretic/Hexen flight: decode the high nibble of cmd->lookfly into a
     * vertical fly command and apply it while the Wings of Wrath
     * (pw_flight) are active.  This too lived only in the compiled-out
     * HEXEN branch, so flight was half-wired: the artifact set MF2_FLY but
     * the player could never gain or lose height.  Mirror dsda-doom's
     * Raven_P_MovePlayer. */
    {
      int fly = cmd->lookfly >> 4;
      if (fly > 7)
        fly -= 16;
      if (fly && player->powers[pw_flight])
      {
        if (fly != TOCENTER_LOOK)
        {
          player->flyheight = fly * 2;
          if (!(mo->flags2 & MF2_FLY))
          {
            mo->flags2 |= MF2_FLY;
            mo->flags |= MF_NOGRAVITY;
            if (hexen && mo->momz <= -39 * FRACUNIT)
              S_StopSound(mo);   /* cut off the falling scream */
          }
        }
        else
        {
          mo->flags2 &= ~MF2_FLY;
          mo->flags &= ~MF_NOGRAVITY;
        }
      }
      else if (fly > 0)
      {
        P_PlayerUseArtifact(player, arti_fly);
      }
      if (mo->flags2 & MF2_FLY)
      {
        mo->momz = player->flyheight * FRACUNIT;
        if (player->flyheight)
          player->flyheight /= 2;
      }
    }

    /* Hexen jump: a grounded player given upward momentum, with a short
     * cooldown so holding the key cannot pogo.  Morphed (pig) players get a
     * weaker hop.  Mirrors dsda-doom's Hexen jump. */
    if (hexen)
    {
      if (player->jumpTics)
        player->jumpTics--;

      if ((cmd->arti & AFLAG_JUMP) && onground && !player->jumpTics &&
          !(mo->flags2 & MF2_FLY))
      {
        mo->momz = (player->morphTics ? 6 : 9) * FRACUNIT;
        mo->flags2 &= ~MF2_ONMOBJ;
        player->jumpTics = 18;
      }
    }
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

   /* Fall to the ground.  Raven games (Heretic/Hexen) additionally tilt the
    * view: a popped-off bloody skull / ice chunk looks upward, an ordinary
    * corpse settles its lookdir back to level.  This used to live only in
    * the compiled-out HEXEN branch, so a dying Heretic/Hexen player's view
    * never tilted.  Mirrors dsda-doom's P_DeathThink. */
   onground = (player->mo->z <= player->mo->floorz);

   if (raven &&
       (player->mo->type == HEXEN_MT_BLOODYSKULL || player->mo->type == HEXEN_MT_ICECHUNK))
   {  /* flying bloody skull / ice chunk: look upward */
      player->viewheight = 6 * FRACUNIT;
      player->deltaviewheight = 0;
      if (onground && player->lookdir < 60)
      {
         int lookDelta = (60 - player->lookdir) / 8;
         if (lookDelta < 1 && (leveltime & 1))
            lookDelta = 1;
         else if (lookDelta > 6)
            lookDelta = 6;
         player->lookdir += lookDelta;
      }
   }
   else if (!(raven && (player->mo->flags2 & MF2_ICEDAMAGE)))
   {  /* settle to the ground (unless frozen solid) */
      if (player->viewheight > 6*FRACUNIT)
         player->viewheight -= FRACUNIT;
      if (player->viewheight < 6*FRACUNIT)
         player->viewheight = 6*FRACUNIT;
      player->deltaviewheight = 0;
      if (raven)
      {  /* return the death view-tilt to level */
         if (player->lookdir > 0)
            player->lookdir -= 6;
         else if (player->lookdir < 0)
            player->lookdir += 6;
         if (abs(player->lookdir) < 6)
            player->lookdir = 0;
      }
   }
   P_CalcHeight (player);

   if (hexen && player->attacker && player->attacker != player->mo)
   {  /* Hexen watches the killer with a smooth face-turn, fading the damage
       * flash while looking at them.  (Vanilla also faded a poison counter
       * here; this build has no poison system, so only damagecount.) */
      int dir = P_FaceMobj(player->mo, player->attacker, &delta);
      if (delta < ANG1 * 10)
      {
         if (player->damagecount)
            player->damagecount--;
      }
      delta = delta / 8;
      if (delta > ANG1 * 5)
         delta = ANG1 * 5;
      if (dir)
         player->mo->angle += delta;   /* turn clockwise */
      else
         player->mo->angle -= delta;    /* turn counter-clockwise */
   }
   else if (player->attacker && player->attacker != player->mo)
   {
      /* watch killer (Doom/Heretic) */
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

   if (player->cmd.buttons & BT_USE)
   {
      /* Hexen's vanilla code stamped the corpse mobj with special1=class /
       * special2=666 here and reset the inventory cursor; this build derives
       * the reborn class from PlayerClass[] and resets the inventory cursor
       * in G_PlayerReborn, so neither is needed. */
      player->playerstate = PST_REBORN;
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
= Heretic artifact use
=
= P_GiveArtifact stores artifacts in player->inventory[]; these routines
= apply an artifact's effect and remove it.  inv_ptr/curpos track the
= on-screen inventory cursor for the local player (used by the eventual
= inventory bar; kept consistent here so the cursor follows removals).
=
=================
*/

int inv_ptr;
int curpos;
int ArtifactFlash;


void P_PlayerNextArtifact(player_t *player)
{
  if (player == &players[consoleplayer])
  {
    inv_ptr--;
    if (inv_ptr < 6)
    {
      curpos--;
      if (curpos < 0)
        curpos = 0;
    }
    if (inv_ptr < 0)
    {
      inv_ptr = player->inventorySlotNum - 1;
      curpos = (inv_ptr < 6) ? inv_ptr : 6;
    }
    if (inv_ptr >= 0 && inv_ptr < player->inventorySlotNum)
      player->readyArtifact = player->inventory[inv_ptr].type;
  }
}

void P_PlayerRemoveArtifact(player_t *player, int slot)
{
  int i;

  player->artifactCount--;
  if (!(--player->inventory[slot].count))
  {                             /* used the last of this type - compact list */
    player->readyArtifact        = arti_none;
    player->inventory[slot].type = arti_none;
    for (i = slot + 1; i < player->inventorySlotNum; i++)
      player->inventory[i - 1] = player->inventory[i];
    player->inventorySlotNum--;
    if (player == &players[consoleplayer])
    {
      inv_ptr--;
      if (inv_ptr < 6)
      {
        curpos--;
        if (curpos < 0)
          curpos = 0;
      }
      if (inv_ptr >= player->inventorySlotNum)
        inv_ptr = player->inventorySlotNum - 1;
      if (inv_ptr < 0)
        inv_ptr = 0;
      if (player->inventorySlotNum > 0)
        player->readyArtifact = player->inventory[inv_ptr].type;
    }
  }
}

/* Chaos device: warp to the level's first start (or a random DM start). */
static void P_ArtiTele(player_t *player)
{
  fixed_t destX, destY;
  angle_t destAngle;
  fixed_t oldx, oldy, oldz;

  if (deathmatch)
  {
    int selections = deathmatch_p - deathmatchstarts;
    int i = P_Random(pr_heretic) % selections;
    destX = deathmatchstarts[i].x << FRACBITS;
    destY = deathmatchstarts[i].y << FRACBITS;
    destAngle = ANG45 * (deathmatchstarts[i].angle / 45);
  }
  else
  {
    int pos = (hexen && RebornPosition > 0 && RebornPosition < MAX_PLAYER_STARTS &&
               playerstarts[RebornPosition][0].options) ? RebornPosition : 0;
    destX = playerstarts[pos][0].x << FRACBITS;
    destY = playerstarts[pos][0].y << FRACBITS;
    destAngle = ANG45 * (playerstarts[pos][0].angle / 45);
  }

  oldx = player->mo->x;
  oldy = player->mo->y;
  oldz = player->mo->z;
  if (P_TeleportMove(player->mo, destX, destY, FALSE))
  {
    S_StartSound(P_SpawnMobj(oldx, oldy, oldz, g_mt_tfog), g_sfx_telept);
    player->mo->angle = destAngle;
    player->mo->z = player->mo->floorz;
    player->mo->momx = player->mo->momy = player->mo->momz = 0;
    if (!hexen)
      S_StartSound(NULL, heretic_sfx_wpnup); /* full-volume laugh */
  }
}

/* Hexen: teleport a mobj to a destination with fog at both ends - the
 * victim half of the Banishment Device and the monster-eviction rules. */
static void TeleportMobj(mobj_t *victim, fixed_t x, fixed_t y, angle_t angle)
{
  fixed_t oldx = victim->x, oldy = victim->y, oldz = victim->z;

  if (P_TeleportMove(victim, x, y, FALSE))
  {
    S_StartSound(P_SpawnMobj(oldx, oldy, oldz, g_mt_tfog), g_sfx_telept);
    S_StartSound(P_SpawnMobj(x, y, victim->z, g_mt_tfog), g_sfx_telept);
    victim->angle = angle;
    victim->z = victim->floorz;
    victim->momx = victim->momy = victim->momz = 0;
    if (!victim->player)
      victim->reactiontime = 18;
  }
}

static void P_TeleportToPlayerStarts(mobj_t *victim)
{
  int i, selections = 0;
  const mapthing_t *start;

  for (i = 0; i < MAXPLAYERS; i++)
    if (playeringame[i])
      selections++;
  if (!selections)
    return;
  i = P_Random(pr_heretic) % selections;
  start = &playerstarts[0][i];
  TeleportMobj(victim, start->x << FRACBITS, start->y << FRACBITS,
               ANG45 * (start->angle / 45));
}

static void P_TeleportToDeathmatchStarts(mobj_t *victim)
{
  int selections = deathmatch_p - deathmatchstarts;

  if (selections)
  {
    int i = P_Random(pr_heretic) % selections;
    TeleportMobj(victim, deathmatchstarts[i].x << FRACBITS,
                 deathmatchstarts[i].y << FRACBITS,
                 ANG45 * (deathmatchstarts[i].angle / 45));
  }
  else
    P_TeleportToPlayerStarts(victim);
}

void P_TeleportOther(mobj_t *victim)
{
  if (victim->player)
  {
    if (deathmatch)
      P_TeleportToDeathmatchStarts(victim);
    else
      P_TeleportToPlayerStarts(victim);
  }
  else
  {
    /* a monster's death action runs when it is banished */
    if (victim->flags & MF_COUNTKILL && victim->special)
    {
      P_RemoveMobjFromTIDList(victim);
      P_ExecuteHexenLineSpecial(victim->special, victim->special_args,
                                NULL, 0, victim);
      victim->special = 0;
    }
    /* all monsters go to the deathmatch spots */
    P_TeleportToDeathmatchStarts(victim);
  }
}

static void P_ArtiTeleportOther(player_t *player)
{
  mobj_t *mo;

  mo = P_SpawnPlayerMissile(player->mo, HEXEN_MT_TELOTHER_FX1);
  if (mo)
    P_SetTarget(&mo->target, player->mo);
}

#define HEAL_RADIUS_DIST (255 * FRACUNIT)

/* Mystic Ambit Incant: a class-specific boon for every living player in
 * range (the user included). */
static dbool P_HealRadius(player_t *player)
{
  mobj_t    *mo;
  mobj_t    *pmo = player->mo;
  thinker_t *think;
  fixed_t    dist;
  int        effective = false;
  int        amount;

  for (think = thinkercap.next; think != &thinkercap; think = think->next)
  {
    if (think->function.arg1 != (void (*)(void *)) P_MobjThinker)
      continue;
    mo = (mobj_t *) think;
    if (!mo->player)
      continue;
    if (mo->health <= 0)
      continue;
    dist = P_AproxDistance(pmo->x - mo->x, pmo->y - mo->y);
    if (dist > HEAL_RADIUS_DIST)
      continue;

    switch (player->class)
    {
      case PCLASS_FIGHTER:      /* radius armor boost */
        if (Hexen_P_GiveArmor(mo->player, ARMOR_ARMOR, 1) ||
            Hexen_P_GiveArmor(mo->player, ARMOR_SHIELD, 1) ||
            Hexen_P_GiveArmor(mo->player, ARMOR_HELMET, 1) ||
            Hexen_P_GiveArmor(mo->player, ARMOR_AMULET, 1))
        {
          effective = true;
          S_StartSound(mo, hexen_sfx_mysticincant);
        }
        break;
      case PCLASS_CLERIC:       /* radius heal */
        amount = 50 + (P_Random(pr_heretic) % 50);
        if (P_GiveBody(mo->player, amount))
        {
          effective = true;
          S_StartSound(mo, hexen_sfx_mysticincant);
        }
        break;
      case PCLASS_MAGE:         /* radius mana boost */
        amount = 50 + (P_Random(pr_heretic) % 50);
        if (P_GiveMana(mo->player, MANA_1, amount) ||
            P_GiveMana(mo->player, MANA_2, amount))
        {
          effective = true;
          S_StartSound(mo, hexen_sfx_mysticincant);
        }
        break;
      default:
        break;
    }
  }
  return effective;
}

/* Disc of Repulsion (hexen_arti_blastradius) support. */
fixed_t P_AproxDistance(fixed_t dx, fixed_t dy);  /* p_maputl.c */
void    P_NoiseAlert(mobj_t *target, mobj_t *emitter); /* p_enemy.c */
#define BLAST_RADIUS_DIST  (255 * FRACUNIT)
#define BLAST_SPEED        (20 * FRACUNIT)
#define BLAST_FULLSTRENGTH 255

/* Clear the blasted state once a shoved mobj has come to rest (called from
 * P_MobjThinker).  The fork has no ice-death system, so there is no
 * ice-corpse special case to preserve here. */
void ResetBlasted(mobj_t *mo)
{
  mo->flags2 &= ~MF2_BLASTED;
  mo->flags2 &= ~MF2_SLIDE;
}

/* Shove a single victim radially away from the blast source.  The artifact
 * always blasts at full strength: missiles are flung (and a couple of special
 * projectiles are reflected back at the caster), monsters get a mass-scaled
 * upward kick, and a HEXEN_MT_BLASTEFFECT puff trails the victim. */
void P_BlastMobj(mobj_t *source, mobj_t *victim, fixed_t strength)
{
  angle_t angle, ang;
  mobj_t *mo;
  fixed_t x, y, z;

  angle = R_PointToAngle2(source->x, source->y, victim->x, victim->y);
  angle >>= ANGLETOFINESHIFT;

  if (strength < BLAST_FULLSTRENGTH)
  {
    victim->momx = FixedMul(strength, finecosine[angle]);
    victim->momy = FixedMul(strength, finesine[angle]);
    if (!victim->player)
    {
      victim->flags2 |= MF2_SLIDE;
      victim->flags2 |= MF2_BLASTED;
    }
    return;
  }

  /* full-strength blast from the artifact */
  if (victim->flags & MF_MISSILE)
  {
    switch (victim->type)
    {
      case HEXEN_MT_SORCBALL1:   /* don't blast sorcerer balls */
      case HEXEN_MT_SORCBALL2:
      case HEXEN_MT_SORCBALL3:
        return;
      case HEXEN_MT_MSTAFF_FX2:  /* reflect to originator */
        P_SetTarget(&victim->special1.m, victim->target);
        P_SetTarget(&victim->target, source);
        break;
      default:
        break;
    }
  }
  if (victim->type == HEXEN_MT_HOLY_FX)
  {
    if (victim->special1.m == source)
    {
      P_SetTarget(&victim->special1.m, victim->target);
      P_SetTarget(&victim->target, source);
    }
  }

  victim->momx = FixedMul(BLAST_SPEED, finecosine[angle]);
  victim->momy = FixedMul(BLAST_SPEED, finesine[angle]);

  /* blast puff, trailing the victim back toward the source */
  ang = R_PointToAngle2(victim->x, victim->y, source->x, source->y);
  ang >>= ANGLETOFINESHIFT;
  x = victim->x + FixedMul(victim->radius + FRACUNIT, finecosine[ang]);
  y = victim->y + FixedMul(victim->radius + FRACUNIT, finesine[ang]);
  z = victim->z - victim->floorclip + (victim->height >> 1);
  mo = P_SpawnMobj(x, y, z, HEXEN_MT_BLASTEFFECT);
  if (mo)
  {
    mo->momx = victim->momx;
    mo->momy = victim->momy;
  }

  if (victim->flags & MF_MISSILE)
  {
    victim->momz = 8 * FRACUNIT;
    if (mo)
      mo->momz = victim->momz;
  }
  else
    victim->momz = (1000 / victim->info->mass) << FRACBITS;

  if (!victim->player)
  {
    victim->flags2 |= MF2_SLIDE;
    victim->flags2 |= MF2_BLASTED;
  }
}

/* Blast all nearby things away from the player (the Disc of Repulsion).
 * Skips bosses, the player, inert effect mobjs, dead monsters, dormant and
 * underground creatures, splashes and serpents -- everything else within
 * BLAST_RADIUS_DIST is shoved. */
void P_BlastRadius(player_t *player)
{
  mobj_t    *mo;
  mobj_t    *pmo = player->mo;
  thinker_t *think;
  fixed_t    dist;

  S_StartSound(pmo, hexen_sfx_artifact_blast);
  P_NoiseAlert(player->mo, player->mo);

  for (think = thinkercap.next; think != &thinkercap; think = think->next)
  {
    if (think->function.arg1 != (void (*)(void *))P_MobjThinker)
      continue;   /* not a mobj thinker */
    mo = (mobj_t *)think;

    if ((mo == pmo) || (mo->flags2 & MF2_BOSS))
      continue;   /* not a valid target */

    if ((mo->type == HEXEN_MT_POISONCLOUD) ||
        (mo->type == HEXEN_MT_HOLY_FX))
    {
      /* let these special cases through */
    }
    else if ((mo->flags & MF_COUNTKILL) && (mo->health <= 0))
      continue;   /* dead monster */
    else if (!(mo->flags & MF_COUNTKILL) &&
             !(mo->player) && !(mo->flags & MF_MISSILE))
      continue;   /* must be a monster, player, or missile */

    if (mo->flags2 & MF2_DORMANT)
      continue;
    if ((mo->type == HEXEN_MT_WRAITHB) && (mo->flags2 & MF2_DONTDRAW))
      continue;   /* underground wraith */
    if ((mo->type == HEXEN_MT_SPLASHBASE) || (mo->type == HEXEN_MT_SPLASH))
      continue;
    if (mo->type == HEXEN_MT_SERPENT || mo->type == HEXEN_MT_SERPENTLEADER)
      continue;

    dist = P_AproxDistance(pmo->x - mo->x, pmo->y - mo->y);
    if (dist > BLAST_RADIUS_DIST)
      continue;   /* out of range */

    P_BlastMobj(pmo, mo, BLAST_FULLSTRENGTH);
  }
}

dbool P_UseArtifact(player_t *player, int arti)
{
  mobj_t *mo;
  angle_t angle;

  /* Hexen and Heretic number their artifacts differently and the two enums
   * overlap (e.g. hexen_arti_summon == arti_tomeofpower == 5), so the Hexen
   * artifacts are dispatched in their own switch.  Only the items implemented
   * so far are handled; the rest fall through to "couldn't use".  Heretic and
   * Doom keep using the arti_* switch below. */
  if (hexen)
  {
    switch (arti)
    {
      case hexen_arti_teleport:
        P_ArtiTele(player);
        break;
      case hexen_arti_teleportother:
        P_ArtiTeleportOther(player);
        break;
      case hexen_arti_boostarmor:
      {
        int i, count = 0;
        for (i = 0; i < NUMARMOR; i++)
          count += Hexen_P_GiveArmor(player, i, 1); /* 1 point per type */
        if (!count)
          return FALSE;
        break;
      }
      case hexen_arti_healingradius:
        if (!P_HealRadius(player))
          return FALSE;
        break;
      /* Puzzle artifacts: try the use-line/thing in front of the player. */
      case hexen_arti_puzzskull:
      case hexen_arti_puzzgembig:
      case hexen_arti_puzzgemred:
      case hexen_arti_puzzgemgreen1:
      case hexen_arti_puzzgemgreen2:
      case hexen_arti_puzzgemblue1:
      case hexen_arti_puzzgemblue2:
      case hexen_arti_puzzbook1:
      case hexen_arti_puzzbook2:
      case hexen_arti_puzzskull2:
      case hexen_arti_puzzfweapon:
      case hexen_arti_puzzcweapon:
      case hexen_arti_puzzmweapon:
      case hexen_arti_puzzgear1:
      case hexen_arti_puzzgear2:
      case hexen_arti_puzzgear3:
      case hexen_arti_puzzgear4:
        if (P_UsePuzzleItem(player, arti - hexen_arti_firstpuzzitem))
          return TRUE;
        player->message = "YOU CANNOT USE THIS HERE";
        return FALSE;

      case hexen_arti_invulnerability:
        if (!P_GivePower(player, pw_invulnerability))
          return FALSE;
        break;
      case hexen_arti_health:
        if (!P_GiveBody(player, 25))
          return FALSE;
        break;
      case hexen_arti_superhealth:
        if (!P_GiveBody(player, 100))
          return FALSE;
        break;
      case hexen_arti_torch:
        if (!P_GivePower(player, pw_infrared))
          return FALSE;
        break;
      case hexen_arti_egg:
        P_SpawnPlayerMissile(player->mo, HEXEN_MT_EGGFX);
        P_SPMAngle(player->mo, HEXEN_MT_EGGFX, player->mo->angle - (ANG45 / 6));
        P_SPMAngle(player->mo, HEXEN_MT_EGGFX, player->mo->angle + (ANG45 / 6));
        P_SPMAngle(player->mo, HEXEN_MT_EGGFX, player->mo->angle - (ANG45 / 3));
        P_SPMAngle(player->mo, HEXEN_MT_EGGFX, player->mo->angle + (ANG45 / 3));
        break;
      case hexen_arti_fly:
        if (!P_GivePower(player, pw_flight))
          return FALSE;
        if (player->mo->momz <= -35 * FRACUNIT)
          S_StopSound(player->mo);   /* cut the falling scream */
        break;
      case hexen_arti_speed:
        if (!P_GivePower(player, pw_speed))
          return FALSE;
        break;
      case hexen_arti_boostmana:
        if (!P_GiveMana(player, MANA_1, MAX_MANA))
        {
          if (!P_GiveMana(player, MANA_2, MAX_MANA))
            return FALSE;
        }
        else
          P_GiveMana(player, MANA_2, MAX_MANA);
        break;
      case hexen_arti_poisonbag:
        angle = player->mo->angle >> ANGLETOFINESHIFT;
        if (player->class == PCLASS_CLERIC)
        {
          mo = P_SpawnMobj(player->mo->x + 16 * finecosine[angle],
                           player->mo->y + 24 * finesine[angle],
                           player->mo->z - player->mo->floorclip + 8 * FRACUNIT,
                           HEXEN_MT_POISONBAG);
          if (mo)
            P_SetTarget(&mo->target, player->mo);
        }
        else if (player->class == PCLASS_MAGE)
        {
          mo = P_SpawnMobj(player->mo->x + 16 * finecosine[angle],
                           player->mo->y + 24 * finesine[angle],
                           player->mo->z - player->mo->floorclip + 8 * FRACUNIT,
                           HEXEN_MT_FIREBOMB);
          if (mo)
            P_SetTarget(&mo->target, player->mo);
        }
        else   /* Fighter (and pig): a lobbed timed bomb */
        {
          mo = P_SpawnMobj(player->mo->x, player->mo->y,
                           player->mo->z - player->mo->floorclip + 35 * FRACUNIT,
                           HEXEN_MT_THROWINGBOMB);
          if (mo)
          {
            mo->angle = player->mo->angle
                      + (((P_Random(pr_heretic) & 7) - 4) << 24);
            mo->momz = 4 * FRACUNIT
                     + (player->lookdir << (FRACBITS - 4));
            mo->z += player->lookdir << (FRACBITS - 4);
            P_ThrustMobj(mo, mo->angle, mo->info->speed);
            mo->momx += player->mo->momx >> 1;
            mo->momy += player->mo->momy >> 1;
            P_SetTarget(&mo->target, player->mo);
            mo->tics -= P_Random(pr_heretic) & 3;
            P_CheckMissileSpawn(mo);
          }
        }
        break;
      case hexen_arti_summon:
        /* Dark Servant: fire the summoning missile; A_Summon spawns the
         * Minotaur (bound to this player as master) where it lands. */
        mo = P_SpawnPlayerMissile(player->mo, HEXEN_MT_SUMMON_FX);
        if (mo)
        {
          P_SetTarget(&mo->target, player->mo);
          P_SetTarget(&mo->special1.m, player->mo);
          mo->momz = 5 * FRACUNIT;
        }
        break;
      case hexen_arti_blastradius:
        /* Disc of Repulsion: shove every nearby thing away. */
        P_BlastRadius(player);
        break;
      default:
        return FALSE;
    }
    return TRUE;
  }

  switch (arti)
  {
    case arti_invulnerability:
      if (!P_GivePower(player, pw_invulnerability))
        return FALSE;
      break;
    case arti_invisibility:
      if (!P_GivePower(player, pw_invisibility))
        return FALSE;
      break;
    case arti_health:
      if (!P_GiveBody(player, 25))
        return FALSE;
      break;
    case arti_superhealth:
      if (!P_GiveBody(player, 100))
        return FALSE;
      break;
    case arti_tomeofpower:
      if (player->chickenTics)
      {                         /* would undo a chicken morph; the morph
                                 * system is not active for Heretic yet, so
                                 * just clear the timer for now. */
        player->chickenTics = 0;
        S_StartSound(player->mo, heretic_sfx_wpnup);
      }
      else
      {
        if (!P_GivePower(player, pw_weaponlevel2))
          return FALSE;
        /* The weapon-ready code selects the tome-powered ready/attack frames
         * from player->powers[pw_weaponlevel2], so no explicit psprite set
         * is needed here. */
      }
      break;
    case arti_torch:
      if (!P_GivePower(player, pw_infrared))
        return FALSE;
      break;
    case arti_firebomb:
      angle = player->mo->angle >> ANGLETOFINESHIFT;
      mo = P_SpawnMobj(player->mo->x + 24 * finecosine[angle],
                       player->mo->y + 24 * finesine[angle],
                       player->mo->z
                       - 15 * FRACUNIT * (player->mo->flags2 & 1),
                       HERETIC_MT_FIREBOMB);
      P_SetTarget(&mo->target, player->mo);
      break;
    case arti_egg:
      mo = player->mo;
      P_SpawnPlayerMissile(mo, HERETIC_MT_EGGFX);
      P_SPMAngle(mo, HERETIC_MT_EGGFX, mo->angle - (ANG45 / 6));
      P_SPMAngle(mo, HERETIC_MT_EGGFX, mo->angle + (ANG45 / 6));
      P_SPMAngle(mo, HERETIC_MT_EGGFX, mo->angle - (ANG45 / 3));
      P_SPMAngle(mo, HERETIC_MT_EGGFX, mo->angle + (ANG45 / 3));
      break;
    case arti_fly:
      if (!P_GivePower(player, pw_flight))
        return FALSE;
      break;
    case arti_teleport:
      P_ArtiTele(player);
      break;
    default:
      return FALSE;
  }
  return TRUE;
}

void P_PlayerUseArtifact(player_t *player, int arti)
{
  int i;

  for (i = 0; i < player->inventorySlotNum; i++)
  {
    if (player->inventory[i].type == arti)
    {                           /* found a match - try to use it */
      if (P_UseArtifact(player, arti))
      {
        P_PlayerRemoveArtifact(player, i);
        if (player == &players[consoleplayer])
        {
          S_StartSound(NULL, heretic_sfx_artiuse);
          ArtifactFlash = 4;
        }
      }
      else if (!(hexen && arti >= hexen_arti_firstpuzzitem))
      {                         /* couldn't use it - advance the cursor.
                                 * Hexen puzzle items stay selected on
                                 * failure, as in Raven's handler. */
        P_PlayerNextArtifact(player);
      }
      break;
    }
  }
}

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


   if (player->playerstate == PST_DEAD)
   {
      P_DeathThink (player);
      return;
   }


   // Move around.
   // Reactiontime is used to prevent movement
   //  for a bit after a teleport.

   if (player->mo->reactiontime)
      player->mo->reactiontime--;
   else
      P_MovePlayer (player);

   /* Heretic: use the staged artifact (from G_BuildTiccmd) this tic. */
   /* Raven (Heretic/Hexen): use the artifact staged by the local player's
    * input. Only the console player drives the single-player pending-artifact
    * global. */
   if (raven && player == &players[consoleplayer] &&
       pending_artifact > 0 &&
       pending_artifact < (hexen ? HEXEN_NUMARTIFACTS : NUMARTIFACTS))
   {
      P_PlayerUseArtifact(player, pending_artifact);
      pending_artifact = 0;
   }

   P_SetPitch(player); /* Determines view pitch */
   P_CalcHeight (player); /* Determines view height and bobbing */

   // Determine if there's anything about the sector you're in that's
   // going to affect you, like painful floors.

   if (player->mo->subsector->sector->special)
      map_format.player_in_special_sector(player);

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

   if (hexen && player->poisoncount && !(leveltime & 15))
   {
      player->poisoncount -= 5;
      if (player->poisoncount < 0)
         player->poisoncount = 0;
      P_PoisonDamage(player, player->poisoner, 1, true);
   }

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
