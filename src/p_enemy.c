/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2000,2002 by
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
 *      Enemy thinking, AI.
 *      Action Pointer Functions
 *      that are associated with states/frames.
 *
 *-----------------------------------------------------------------------------*/

#include "doomstat.h"
#include "m_random.h"
#include "r_main.h"
#include "p_maputl.h"
#include "p_map.h"
#include "p_setup.h"
#include "p_spec.h"
#include "s_sound.h"
#include "sounds.h"
#include "p_inter.h"
#include "g_game.h"
#include "p_enemy.h"
#include "p_tick.h"
#include "m_bbox.h"
#include "lprintf.h"

#ifdef PSX
#include <stddef.h>
#endif

static mobj_t *current_actor;

typedef enum
{
  DI_EAST,
  DI_NORTHEAST,
  DI_NORTH,
  DI_NORTHWEST,
  DI_WEST,
  DI_SOUTHWEST,
  DI_SOUTH,
  DI_SOUTHEAST,
  DI_NODIR,
  NUMDIRS
} dirtype_t;

static void P_NewChaseDir(mobj_t *actor);
void P_ZBumpCheck(mobj_t *);                                        // phares

extern void retro_set_rumble_damage(int damage, float duration);

//
// ENEMY THINKING
// Enemies are allways spawned
// with targetplayer = -1, threshold = 0
// Most monsters are spawned unaware of all players,
// but some can be made preaware
//

//
// Called by P_NoiseAlert.
// Recursively traverse adjacent sectors,
// sound blocking lines cut off traversal.
//
// killough 5/5/98: reformatted, cleaned up

static void P_RecursiveSound(sector_t *sec, int soundblocks,
           mobj_t *soundtarget)
{
  int i;

  // wake up all monsters in this sector
  if (sec->validcount == validcount && sec->soundtraversed <= soundblocks+1)
    return;             // already flooded

  sec->validcount = validcount;
  sec->soundtraversed = soundblocks+1;
  P_SetTarget(&sec->soundtarget, soundtarget);

  for (i=0; i<sec->linecount; i++)
    {
      sector_t *other;
      line_t *check = sec->lines[i];

      if (!(check->flags & ML_TWOSIDED))
        continue;

      P_LineOpening(check);

      if (openrange <= 0)
        continue;       // closed door

      other=sides[check->sidenum[sides[check->sidenum[0]].sector==sec]].sector;

      if (!(check->flags & ML_SOUNDBLOCK))
        P_RecursiveSound(other, soundblocks, soundtarget);
      else
        if (!soundblocks)
          P_RecursiveSound(other, 1, soundtarget);
    }
}

//
// P_NoiseAlert
// If a monster yells at a player,
// it will alert other monsters to the player.
//
void P_NoiseAlert(mobj_t *target, mobj_t *emitter)
{
  validcount++;
  P_RecursiveSound(emitter->subsector->sector, 0, target);
}

//
// P_CheckMeleeRange
//

dbool P_CheckMeleeRange(mobj_t *actor)
{
  mobj_t *pl = actor->target;
  /* MBF21: use the per-thing melee range (default MELEERANGE) instead of
   * the hardcoded constant; LONGMELEE bumps it further (revenant).  Gated
   * on mbf21_features so vanilla/boom/mbf/prboom keep the exact constant. */
  fixed_t range = MELEERANGE;
  if (mbf21_features)
    {
      range = actor->info->meleerange;
      if (actor->flags2 & MF2_LONGMELEE)
        range += 12*FRACUNIT;
    }

  return  // killough 7/18/98: friendly monsters don't attack other friends
    pl && !(actor->flags & pl->flags & MF_FRIEND) &&
    (P_AproxDistance(pl->x-actor->x, pl->y-actor->y) <
     range - 20*FRACUNIT + pl->info->radius) &&
    P_CheckSight(actor, actor->target);
}

//
// P_HitFriend()
//
// killough 12/98
// This function tries to prevent shooting at friends

static dbool   P_HitFriend(mobj_t *actor)
{
  return actor->flags & MF_FRIEND && actor->target &&
    (P_AimLineAttack(actor,
         R_PointToAngle2(actor->x, actor->y,
             actor->target->x, actor->target->y),
         P_AproxDistance(actor->x-actor->target->x,
             actor->y-actor->target->y), 0),
     linetarget) && linetarget != actor->target &&
    !((linetarget->flags ^ actor->flags) & MF_FRIEND);
}

//
// P_CheckMissileRange
//
static dbool   P_CheckMissileRange(mobj_t *actor)
{
  fixed_t dist;

  if (!P_CheckSight(actor, actor->target))
    return FALSE;

  if (actor->flags & MF_JUSTHIT)
    {      // the target just hit the enemy, so fight back!
      actor->flags &= ~MF_JUSTHIT;

      /* killough 7/18/98: no friendly fire at corpses
       * killough 11/98: prevent too much infighting among friends
       * cph - yikes, talk about fitting everything on one line... */

      return
  !(actor->flags & MF_FRIEND) ||
  (actor->target->health > 0 &&
   (!(actor->target->flags & MF_FRIEND) ||
    (actor->target->player ?
     monster_infighting || P_Random(pr_defect) >128 :
     !(actor->target->flags & MF_JUSTHIT) && P_Random(pr_defect) >128)));
    }

  /* killough 7/18/98: friendly monsters don't attack other friendly
   * monsters or players (except when attacked, and then only once)
   */
  if (actor->flags & actor->target->flags & MF_FRIEND)
    return FALSE;

  if (actor->reactiontime)
    return FALSE;       // do not attack yet

  // OPTIMIZE: get this from a global checksight
  dist = P_AproxDistance ( actor->x-actor->target->x,
                           actor->y-actor->target->y) - 64*FRACUNIT;

  if (!actor->info->meleestate)
    dist -= 128*FRACUNIT;       // no melee attack, so fire more

  dist >>= FRACBITS;

  /* MBF21: SHORTMRANGE caps the attack distance (archvile).  flags2 is
   * zero outside complevel 21, so vanilla behaviour is unchanged. */
  if ((actor->flags2 & MF2_SHORTMRANGE) && dist > 14*64)
    return FALSE;

  if (actor->info->maxattackrange > 0 && dist > actor->info->maxattackrange)
    return FALSE;     // too far away

  if (actor->info->meleestate != S_NULL && dist < actor->info->meleethreshold)
    return false;     // close enough for melee attack, don't fire

  // higher attack probability like Cyberdemon, Spiderboss, Revenant and Lost Soul
  if (actor->flags & MF_MISSILEMORE)
    dist >>= 1;

  /* MBF21: RANGEHALF uses half distance for the probability roll;
   * HIGHERMPROB lowers the minimum-distance floor from 150 to 50. */
  if (actor->flags2 & MF2_RANGEHALF)
    dist >>= 1;

  // Some mobs (eg. Cyberdemon) have a minimum attack chance
  if (dist > actor->info->minmissilechance)
    dist = actor->info->minmissilechance;

  if ((actor->flags2 & MF2_HIGHERMPROB) && dist > 50)
    dist = 50;

  if (P_Random(pr_missrange) < dist)
    return FALSE;

  if (P_HitFriend(actor))
    return FALSE;

  return TRUE;
}

/*
 * P_IsOnLift
 *
 * killough 9/9/98:
 *
 * Returns TRUE if the object is on a lift. Used for AI,
 * since it may indicate the need for crowded conditions,
 * or that a monster should stay on the lift for a while
 * while it goes up or down.
 */

static dbool   P_IsOnLift(const mobj_t *actor)
{
  const sector_t *sec = actor->subsector->sector;
  line_t line;
  int l;

  memset(&line, 0, sizeof(line));

  // Short-circuit: it's on a lift which is active.
  if (sec->floordata && ((thinker_t *) sec->floordata)->function.arg1==(void (*)(void *))T_PlatRaise)
    return TRUE;

  // Check to see if it's in a sector which can be activated as a lift.
  if ((line.tag = sec->tag))
    for (l = -1; (l = P_FindLineFromLineTag(&line, l)) >= 0;)
      switch (lines[l].special)
  {
  case  10: case  14: case  15: case  20: case  21: case  22:
  case  47: case  53: case  62: case  66: case  67: case  68:
  case  87: case  88: case  95: case 120: case 121: case 122:
  case 123: case 143: case 162: case 163: case 181: case 182:
  case 144: case 148: case 149: case 211: case 227: case 228:
  case 231: case 232: case 235: case 236:
    return TRUE;
  }

  return FALSE;
}

/*
 * P_IsUnderDamage
 *
 * killough 9/9/98:
 *
 * Returns nonzero if the object is under damage based on
 * their current position. Returns 1 if the damage is moderate,
 * -1 if it is serious. Used for AI.
 */

static int P_IsUnderDamage(mobj_t *actor)
{
  const struct msecnode_s *seclist;
  const ceiling_t *cl;             // Crushing ceiling
  int dir = 0;
  for (seclist=actor->touching_sectorlist; seclist; seclist=seclist->m_tnext)
    if ((cl = seclist->m_sector->ceilingdata) &&
  cl->thinker.function.arg1 == (void (*)(void *))T_MoveCeiling)
      dir |= cl->direction;
  return dir;
}

//
// P_Move
// Move in the current direction,
// returns FALSE if the move is blocked.
//

static fixed_t xspeed[8] = {FRACUNIT,47000,0,-47000,-FRACUNIT,-47000,0,47000};
static fixed_t yspeed[8] = {0,47000,FRACUNIT,47000,0,-47000,-FRACUNIT,-47000};

// 1/11/98 killough: Limit removed on special lines crossed
extern  line_t **spechit;          // New code -- killough
extern  int    numspechit;

static dbool   P_Move(mobj_t *actor, dbool   dropoff) /* killough 9/12/98 */
{
  fixed_t tryx, tryy, deltax, deltay, origx, origy;
  dbool   try_ok;
  int movefactor = ORIG_FRICTION_FACTOR;    // killough 10/98
  int friction = ORIG_FRICTION;
  int speed;

  /* A blasted thing (Disc of Repulsion) is being carried by the blast's
   * momentum; don't let its own step-move override the throw until the blast
   * wears off (ResetBlasted clears the flag once it comes to rest). */
  if (actor->flags2 & MF2_BLASTED)
    return TRUE;

  if (actor->movedir == DI_NODIR)
    return FALSE;

  // killough 10/98: make monsters get affected by ice and sludge too:

  if (monster_friction)
    movefactor = P_GetMoveFactor(actor, &friction);

  speed = actor->info->speed;

  if (friction < ORIG_FRICTION &&     // sludge
      !(speed = ((ORIG_FRICTION_FACTOR - (ORIG_FRICTION_FACTOR-movefactor)/2)
     * speed) / ORIG_FRICTION_FACTOR))
    speed = 1;      // always give the monster a little bit of speed

  tryx = (origx = actor->x) + (deltax = speed * xspeed[actor->movedir]);
  tryy = (origy = actor->y) + (deltay = speed * yspeed[actor->movedir]);

  try_ok = P_TryMove(actor, tryx, tryy, dropoff);

  // killough 10/98:
  // Let normal momentum carry them, instead of steptoeing them across ice.

  if (try_ok && friction > ORIG_FRICTION)
    {
      actor->x = origx;
      actor->y = origy;
      movefactor *= FRACUNIT / ORIG_FRICTION_FACTOR / 4;
      actor->momx += FixedMul(deltax, movefactor);
      actor->momy += FixedMul(deltay, movefactor);
    }

  if (!try_ok)
    {      // open any specials
      int good;

      if (actor->flags & MF_FLOAT && floatok)
        {
          if (actor->z < tmfloorz)          // must adjust height
            actor->z += FLOATSPEED;
          else
            actor->z -= FLOATSPEED;

          actor->flags |= MF_INFLOAT;

    return TRUE;
        }

      if (!numspechit)
        return FALSE;

      actor->movedir = DI_NODIR;

      /* if the special is not a door that can be opened, return FALSE
       *
       * killough 8/9/98: this is what caused monsters to get stuck in
       * doortracks, because it thought that the monster freed itself
       * by opening a door, even if it was moving towards the doortrack,
       * and not the door itself.
       *
       * killough 9/9/98: If a line blocking the monster is activated,
       * return TRUE 90% of the time. If a line blocking the monster is
       * not activated, but some other line is, return FALSE 90% of the
       * time. A bit of randomness is needed to ensure it's free from
       * lockups, but for most cases, it returns the correct result.
       *
       * Do NOT simply return FALSE 1/4th of the time (causes monsters to
       * back out when they shouldn't, and creates secondary stickiness).
       */

      for (good = FALSE; numspechit--; )
        if (P_UseSpecialLine(actor, spechit[numspechit], 0))
    good |= spechit[numspechit] == blockline ? 1 : 2;

      /* cph - compatibility maze here
       * Boom v2.01 and orig. Doom return "good"
       * Boom v2.02 and LxDoom return good && (P_Random(pr_trywalk)&3)
       * MBF plays even more games
       */
      if (!good || comp[comp_doorstuck]) return good;
      if (!mbf_features)
  return (P_Random(pr_trywalk)&3); /* jff 8/13/98 */
      else /* finally, MBF code */
  return ((P_Random(pr_opendoor) >= 230) ^ (good & 1));
    }
  else
    actor->flags &= ~MF_INFLOAT;

  /* killough 11/98: fall more slowly, under gravity, if felldown==TRUE */
  if (!(actor->flags & MF_FLOAT) &&
      (!felldown || !mbf_features))
    actor->z = actor->floorz;

  return TRUE;
}

/*
 * P_SmartMove
 *
 * killough 9/12/98: Same as P_Move, except smarter
 */

static dbool   P_SmartMove(mobj_t *actor)
{
  mobj_t *target = actor->target;
  int on_lift, dropoff = FALSE, under_damage;

  /* killough 9/12/98: Stay on a lift if target is on one */
  on_lift = !comp[comp_staylift]
    && target && target->health > 0
    && target->subsector->sector->tag==actor->subsector->sector->tag &&
    P_IsOnLift(actor);

  under_damage = monster_avoid_hazards && P_IsUnderDamage(actor);

  // killough 10/98: allow dogs to drop off of taller ledges sometimes.
  // dropoff==1 means always allow it, dropoff==2 means only up to 128 high,
  // and only if the target is immediately on the other side of the line.

  if (!P_Move(actor, dropoff))
    return FALSE;

  // killough 9/9/98: avoid crushing ceilings or other damaging areas
  if (
      (on_lift && P_Random(pr_stayonlift) < 230 &&      // Stay on lift
       !P_IsOnLift(actor))
      ||
      (monster_avoid_hazards && !under_damage &&  // Get away from damage
       (under_damage = P_IsUnderDamage(actor)) &&
       (under_damage < 0 || P_Random(pr_avoidcrush) < 200))
      )
    actor->movedir = DI_NODIR;    // avoid the area (most of the time anyway)

  return TRUE;
}

//
// TryWalk
// Attempts to move actor on
// in its current (ob->moveangle) direction.
// If blocked by either a wall or an actor
// returns FALSE
// If move is either clear or blocked only by a door,
// returns TRUE and sets...
// If a door is in the way,
// an OpenDoor call is made to start it opening.
//

static dbool   P_TryWalk(mobj_t *actor)
{
  if (!P_SmartMove(actor))
    return FALSE;
  actor->movecount = P_Random(pr_trywalk)&15;
  return TRUE;
}

//
// P_DoNewChaseDir
//
// killough 9/8/98:
//
// Most of P_NewChaseDir(), except for what
// determines the new direction to take
//

static void P_DoNewChaseDir(mobj_t *actor, fixed_t deltax, fixed_t deltay)
{
  dirtype_t xdir, ydir, tdir;
  dirtype_t olddir = actor->movedir;
  dirtype_t turnaround = olddir;

  if (turnaround != DI_NODIR)         // find reverse direction
    turnaround ^= 4;

  xdir =
    deltax >  10*FRACUNIT ? DI_EAST :
    deltax < -10*FRACUNIT ? DI_WEST : DI_NODIR;

  ydir =
    deltay < -10*FRACUNIT ? DI_SOUTH :
    deltay >  10*FRACUNIT ? DI_NORTH : DI_NODIR;

  // try direct route
  if (xdir != DI_NODIR && ydir != DI_NODIR && turnaround !=
      (actor->movedir = deltay < 0 ? deltax > 0 ? DI_SOUTHEAST : DI_SOUTHWEST :
       deltax > 0 ? DI_NORTHEAST : DI_NORTHWEST) && P_TryWalk(actor))
    return;

  // try other directions
  if (P_Random(pr_newchase) > 200 || D_abs(deltay)>D_abs(deltax))
    tdir = xdir, xdir = ydir, ydir = tdir;

  if ((xdir == turnaround ? xdir = DI_NODIR : xdir) != DI_NODIR &&
      (actor->movedir = xdir, P_TryWalk(actor)))
    return;         // either moved forward or attacked

  if ((ydir == turnaround ? ydir = DI_NODIR : ydir) != DI_NODIR &&
      (actor->movedir = ydir, P_TryWalk(actor)))
    return;

  // there is no direct path to the player, so pick another direction.
  if (olddir != DI_NODIR && (actor->movedir = olddir, P_TryWalk(actor)))
    return;

  // randomly determine direction of search
  if (P_Random(pr_newchasedir) & 1)
    {
      for (tdir = DI_EAST; tdir <= DI_SOUTHEAST; tdir++)
        if (tdir != turnaround && (actor->movedir = tdir, P_TryWalk(actor)))
    return;
    }
  else
    for (tdir = DI_SOUTHEAST; tdir != (dirtype_t)(DI_EAST-1); tdir--)
      if (tdir != turnaround && (actor->movedir = tdir, P_TryWalk(actor)))
  return;

  if ((actor->movedir = turnaround) != DI_NODIR && !P_TryWalk(actor))
    actor->movedir = DI_NODIR;
}

//
// killough 11/98:
//
// Monsters try to move away from tall dropoffs.
//
// In Doom, they were never allowed to hang over dropoffs,
// and would remain stuck if involuntarily forced over one.
// This logic, combined with p_map.c (P_TryMove), allows
// monsters to free themselves without making them tend to
// hang over dropoffs.

static fixed_t dropoff_deltax, dropoff_deltay, floorz;

static dbool   PIT_AvoidDropoff(line_t *line)
{
  if (line->backsector                          && // Ignore one-sided linedefs
      tmbbox[BOXRIGHT]  > line->bbox[BOXLEFT]   &&
      tmbbox[BOXLEFT]   < line->bbox[BOXRIGHT]  &&
      tmbbox[BOXTOP]    > line->bbox[BOXBOTTOM] && // Linedef must be contacted
      tmbbox[BOXBOTTOM] < line->bbox[BOXTOP]    &&
      P_BoxOnLineSide(tmbbox, line) == -1)
    {
      fixed_t front = line->frontsector->floorheight;
      fixed_t back  = line->backsector->floorheight;
      angle_t angle;

      // The monster must contact one of the two floors,
      // and the other must be a tall dropoff (more than 24).

      if (back == floorz && front < floorz - STEPSIZE)
  angle = R_PointToAngle2(0,0,line->dx,line->dy);   // front side dropoff
      else
  if (front == floorz && back < floorz - STEPSIZE)
    angle = R_PointToAngle2(line->dx,line->dy,0,0); // back side dropoff
  else
    return TRUE;

      // Move away from dropoff at a standard speed.
      // Multiple contacted linedefs are cumulative (e.g. hanging over corner)
      dropoff_deltax -= finesine[angle >> ANGLETOFINESHIFT]*32;
      dropoff_deltay += finecosine[angle >> ANGLETOFINESHIFT]*32;
    }
  return TRUE;
}

//
// Driver for above
//

static fixed_t P_AvoidDropoff(mobj_t *actor)
{
  int yh=((tmbbox[BOXTOP]   = actor->y+actor->radius)-bmaporgy)>>MAPBLOCKSHIFT;
  int yl=((tmbbox[BOXBOTTOM]= actor->y-actor->radius)-bmaporgy)>>MAPBLOCKSHIFT;
  int xh=((tmbbox[BOXRIGHT] = actor->x+actor->radius)-bmaporgx)>>MAPBLOCKSHIFT;
  int xl=((tmbbox[BOXLEFT]  = actor->x-actor->radius)-bmaporgx)>>MAPBLOCKSHIFT;
  int bx, by;

  floorz = actor->z;            // remember floor height

  dropoff_deltax = dropoff_deltay = 0;

  // check lines

  validcount++;
  for (bx=xl ; bx<=xh ; bx++)
    for (by=yl ; by<=yh ; by++)
      P_BlockLinesIterator(bx, by, PIT_AvoidDropoff);  // all contacted lines

  return dropoff_deltax | dropoff_deltay;   // Non-zero if movement prescribed
}

//
// P_NewChaseDir
//
// killough 9/8/98: Split into two functions
//

static void P_NewChaseDir(mobj_t *actor)
{
  mobj_t *target = actor->target;
  fixed_t deltax = target->x - actor->x;
  fixed_t deltay = target->y - actor->y;

  // killough 8/8/98: sometimes move away from target, keeping distance
  //
  // 1) Stay a certain distance away from a friend, to avoid being in their way
  // 2) Take advantage over an enemy without missiles, by keeping distance

  actor->strafecount = 0;

  if (mbf_features) {
    if (actor->floorz - actor->dropoffz > STEPSIZE &&
  actor->z <= actor->floorz &&
  !(actor->flags & (MF_DROPOFF|MF_FLOAT)) &&
  !comp[comp_dropoff] &&
  P_AvoidDropoff(actor)) /* Move away from dropoff */
      {
  P_DoNewChaseDir(actor, dropoff_deltax, dropoff_deltay);

  // If moving away from dropoff, set movecount to 1 so that
  // small steps are taken to get monster away from dropoff.

  actor->movecount = 1;
  return;
      }
    else
      {
  fixed_t dist = P_AproxDistance(deltax, deltay);

  // Move away from friends when too close, except
  // in certain situations (e.g. a crowded lift)

  if (actor->flags & target->flags & MF_FRIEND &&
      distfriend << FRACBITS > dist &&
      !P_IsOnLift(target) && !P_IsUnderDamage(actor))
  {
    deltax = -deltax, deltay = -deltay;
  } else
    if (target->health > 0 && (actor->flags ^ target->flags) & MF_FRIEND)
      {   // Live enemy target
        if (monster_backing &&
      actor->info->missilestate && actor->type != MT_SKULL &&
      ((!target->info->missilestate && dist < MELEERANGE*2) ||
       (target->player && dist < MELEERANGE*3 &&
        (target->player->readyweapon == WP_FIST ||
         target->player->readyweapon == WP_CHAINSAW))))
    {       // Back away from melee attacker
      actor->strafecount = P_Random(pr_enemystrafe) & 15;
      deltax = -deltax, deltay = -deltay;
    }
      }
      }
  }

  P_DoNewChaseDir(actor, deltax, deltay);

  // If strafing, set movecount to strafecount so that old Doom
  // logic still works the same, except in the strafing part

  if (actor->strafecount)
    actor->movecount = actor->strafecount;
}

//
// P_IsVisible
//
// killough 9/9/98: whether a target is visible to a monster
//

static dbool   P_IsVisible(mobj_t *actor, mobj_t *mo, dbool   allaround)
{
  if (!allaround)
    {
      angle_t an = R_PointToAngle2(actor->x, actor->y,
           mo->x, mo->y) - actor->angle;
      if (an > ANG90 && an < ANG270 &&
    P_AproxDistance(mo->x-actor->x, mo->y-actor->y) > MELEERANGE)
  return FALSE;
    }
  return P_CheckSight(actor, mo);
}

//
// PIT_FindTarget
//
// killough 9/5/98
//
// Finds monster targets for other monsters
//

static int current_allaround;

static dbool   PIT_FindTarget(mobj_t *mo)
{
  mobj_t *actor = current_actor;

  if (!((mo->flags ^ actor->flags) & MF_FRIEND &&        // Invalid target
  mo->health > 0 && (mo->flags & MF_ISMONSTER)))
    return TRUE;

  // If the monster is already engaged in a one-on-one attack
  // with a healthy friend, don't attack around 60% the time
  {
    const mobj_t *targ = mo->target;
    if (targ && targ->target == mo &&
  P_Random(pr_skiptarget) > 100 &&
  (targ->flags ^ mo->flags) & MF_FRIEND &&
  targ->health*2 >= targ->info->spawnhealth)
      return TRUE;
  }

  if (!P_IsVisible(actor, mo, current_allaround))
    return TRUE;

  P_SetTarget(&actor->lastenemy, actor->target);  // Remember previous target
  P_SetTarget(&actor->target, mo);                // Found target

  // Move the selected monster to the end of its associated
  // list, so that it gets searched last next time.

  {
    thinker_t *cap = &thinkerclasscap[mo->flags & MF_FRIEND ?
             th_friends : th_enemies];
    (mo->thinker.cprev->cnext = mo->thinker.cnext)->cprev = mo->thinker.cprev;
    (mo->thinker.cprev = cap->cprev)->cnext = &mo->thinker;
    (mo->thinker.cnext = cap)->cprev = &mo->thinker;
  }

  return FALSE;
}

//
// P_LookForPlayers
// If allaround is FALSE, only look 180 degrees in front.
// Returns TRUE if a player is targeted.
//

static dbool   P_LookForPlayers(mobj_t *actor, dbool   allaround)
{
  player_t *player;
  int stop, stopc, c;

  if (actor->flags & MF_FRIEND)
    {  // killough 9/9/98: friendly monsters go about players differently
      int anyone;

#if 0
      if (!allaround) // If you want friendly monsters not to awaken unprovoked
  return FALSE;
#endif

      // Go back to a player, no matter whether it's visible or not
      for (anyone=0; anyone<=1; anyone++)
  for (c=0; c<MAXPLAYERS; c++)
    if (playeringame[c] && players[c].playerstate==PST_LIVE &&
        (anyone || P_IsVisible(actor, players[c].mo, allaround)))
      {
        P_SetTarget(&actor->target, players[c].mo);

        // killough 12/98:
        // get out of refiring loop, to avoid hitting player accidentally

        if (actor->info->missilestate)
    {
      P_SetMobjState(actor, actor->info->seestate);
      actor->flags &= ~MF_JUSTHIT;
    }

        return TRUE;
      }

      return FALSE;
    }

  // Change mask of 3 to (MAXPLAYERS-1) -- killough 2/15/98:
  stop = (actor->lastlook-1)&(MAXPLAYERS-1);

  c = 0;

  stopc = !mbf_features &&
    !demo_compatibility && monsters_remember ?
    MAXPLAYERS : 2;       // killough 9/9/98

  for (;; actor->lastlook = (actor->lastlook+1)&(MAXPLAYERS-1))
    {
      if (!playeringame[actor->lastlook])
  continue;

      // killough 2/15/98, 9/9/98:
      if (c++ == stopc || actor->lastlook == stop)  // done looking
      {
        // e6y
        // Fixed Boom incompatibilities. The following code was missed.
        // There are no more desyncs on Donce's demos on horror.wad

        // Use last known enemy if no players sighted -- killough 2/15/98:
        if (!mbf_features && !demo_compatibility && monsters_remember)
        {
          if (actor->lastenemy && actor->lastenemy->health > 0)
          {
            actor->target = actor->lastenemy;
            actor->lastenemy = NULL;
            return TRUE;
          }
        }

        return FALSE;
      }

      player = &players[actor->lastlook];

      if (player->health <= 0)
  continue;               // dead

      if (!P_IsVisible(actor, player->mo, allaround))
  continue;

      P_SetTarget(&actor->target, player->mo);

      /* killough 9/9/98: give monsters a threshold towards getting players
       * (we don't want it to be too easy for a player with dogs :)
       */
      if (!comp[comp_pursuit])
  actor->threshold = 60;

      return TRUE;
    }
}

//
// Friendly monsters, by Lee Killough 7/18/98
//
// Friendly monsters go after other monsters first, but
// also return to owner if they cannot find any targets.
// A marine's best friend :)  killough 7/18/98, 9/98
//

static dbool   P_LookForMonsters(mobj_t *actor, dbool   allaround)
{
  thinker_t *cap, *th;

  if (demo_compatibility)
    return FALSE;

  if (actor->lastenemy && actor->lastenemy->health > 0 && monsters_remember &&
      !(actor->lastenemy->flags & actor->flags & MF_FRIEND)) // not friends
    {
      P_SetTarget(&actor->target, actor->lastenemy);
      P_SetTarget(&actor->lastenemy, NULL);
      return TRUE;
    }

  /* Old demos do not support monster-seeking bots */
  if (!mbf_features)
    return FALSE;

  // Search the threaded list corresponding to this object's potential targets
  cap = &thinkerclasscap[actor->flags & MF_FRIEND ? th_enemies : th_friends];

  // Search for new enemy

  if (cap->cnext != cap)        // Empty list? bail out early
    {
      int x = (actor->x - bmaporgx)>>MAPBLOCKSHIFT;
      int y = (actor->y - bmaporgy)>>MAPBLOCKSHIFT;
      int d;

      current_actor = actor;
      current_allaround = allaround;

      // Search first in the immediate vicinity.

      if (!P_BlockThingsIterator(x, y, PIT_FindTarget))
  return TRUE;

      for (d=1; d<5; d++)
  {
    int i = 1 - d;
    do
      if (!P_BlockThingsIterator(x+i, y-d, PIT_FindTarget) ||
    !P_BlockThingsIterator(x+i, y+d, PIT_FindTarget))
        return TRUE;
    while (++i < d);
    do
      if (!P_BlockThingsIterator(x-d, y+i, PIT_FindTarget) ||
    !P_BlockThingsIterator(x+d, y+i, PIT_FindTarget))
        return TRUE;
    while (--i + d >= 0);
  }

      {   // Random number of monsters, to prevent patterns from forming
  int n = (P_Random(pr_friends) & 31) + 15;

  for (th = cap->cnext; th != cap; th = th->cnext)
    if (--n < 0)
      {
        // Only a subset of the monsters were searched. Move all of
        // the ones which were searched so far, to the end of the list.

        (cap->cnext->cprev = cap->cprev)->cnext = cap->cnext;
        (cap->cprev = th->cprev)->cnext = cap;
        (th->cprev = cap)->cnext = th;
        break;
     }
    else
      if (!PIT_FindTarget((mobj_t *) th))   // If target sighted
        return TRUE;
      }
    }

  return FALSE;  // No monster found
}

//
// P_LookForTargets
//
// killough 9/5/98: look for targets to go after, depending on kind of monster
//

static dbool   P_LookForTargets(mobj_t *actor, int allaround)
{
  return actor->flags & MF_FRIEND ?
    P_LookForMonsters(actor, allaround) || P_LookForPlayers (actor, allaround):
    P_LookForPlayers (actor, allaround) || P_LookForMonsters(actor, allaround);
}

//
// P_HelpFriend
//
// killough 9/8/98: Help friends in danger of dying
//

static dbool   P_HelpFriend(mobj_t *actor)
{
  thinker_t *cap, *th;

  // If less than 33% health, self-preservation rules
  if (actor->health*3 < actor->info->spawnhealth)
    return FALSE;

  current_actor = actor;
  current_allaround = TRUE;

  // Possibly help a friend under 50% health
  cap = &thinkerclasscap[actor->flags & MF_FRIEND ? th_friends : th_enemies];

  for (th = cap->cnext; th != cap; th = th->cnext)
    if (((mobj_t *) th)->health*2 >= ((mobj_t *) th)->info->spawnhealth)
      {
  if (P_Random(pr_helpfriend) < 180)
    break;
      }
    else
      if (((mobj_t *) th)->flags & MF_JUSTHIT &&
    ((mobj_t *) th)->target &&
    ((mobj_t *) th)->target != actor->target &&
    !PIT_FindTarget(((mobj_t *) th)->target))
  {
    // Ignore any attacking monsters, while searching for friend
    actor->threshold = BASETHRESHOLD;
    return TRUE;
  }

  return FALSE;
}

//
// A_KeenDie
// DOOM II special, map 32.
// Uses special tag 666.
//
void A_KeenDie(mobj_t* mo)
{
  thinker_t *th;
  line_t   junk;

  A_Fall(mo);

  // scan the remaining thinkers to see if all Keens are dead

  for (th = thinkercap.next ; th != &thinkercap ; th=th->next)
    if (th->function.arg1 == (void (*)(void *))P_MobjThinker)
      {
        mobj_t *mo2 = (mobj_t *) th;
        if (mo2 != mo && mo2->type == mo->type && mo2->health > 0)
          return;                           // other Keen not dead
      }

  junk.tag = 666;
  EV_DoDoor(&junk,open);
}


//
// ACTION ROUTINES
//

//
// A_Look
// Stay in state until a player is sighted.
//

void A_Look(mobj_t *actor)
{
  mobj_t *targ = actor->subsector->sector->soundtarget;
  actor->threshold = 0; // any shot will wake up

  /* killough 7/18/98:
   * Friendly monsters go after other monsters first, but
   * also return to player, without attacking them, if they
   * cannot find any targets. A marine's best friend :)
   */
  actor->pursuecount = 0;

  if (!(actor->flags & MF_FRIEND && P_LookForTargets(actor, FALSE)) &&
      !((targ = actor->subsector->sector->soundtarget) &&
  targ->flags & MF_SHOOTABLE &&
  (P_SetTarget(&actor->target, targ),
   !(actor->flags & MF_AMBUSH) || P_CheckSight(actor, targ))) &&
      (actor->flags & MF_FRIEND || !P_LookForTargets(actor, FALSE)))
    return;

  // go into chase state

  if (actor->info->seesound)
    {
      int sound;
      switch (actor->info->seesound)
        {
        case sfx_posit1:
        case sfx_posit2:
        case sfx_posit3:
          sound = sfx_posit1+P_Random(pr_see)%3;
          break;

        case sfx_bgsit1:
        case sfx_bgsit2:
          sound = sfx_bgsit1+P_Random(pr_see)%2;
          break;

        default:
          sound = actor->info->seesound;
          break;
        }
      if ((actor->flags & MF_FULLVOLSIGHT) ||
          (actor->flags2 & (MF2_FULLVOLSOUNDS | MF2_BOSS))) /* MBF21 */
        S_StartSound(NULL, sound);          // full volume
      else
        S_StartSound(actor, sound);
    }
  P_SetMobjState(actor, actor->info->seestate);
}

//
// A_KeepChasing
//
// killough 10/98:
// Allows monsters to continue movement while attacking
//

#if 0
static void A_KeepChasing(mobj_t *actor)
{
  if (actor->movecount)
    {
      actor->movecount--;
      if (actor->strafecount)
        actor->strafecount--;
      P_SmartMove(actor);
    }
}
#endif

//
// A_Chase
// Actor has a melee attack,
// so it tries to close as fast as possible
//

void A_Chase(mobj_t *actor)
{
  if (actor->reactiontime)
    actor->reactiontime--;

  if (actor->threshold) { /* modify target threshold */
    if (!actor->target || actor->target->health <= 0)
      actor->threshold = 0;
    else
      actor->threshold--;
  }

  /* turn towards movement direction if not there yet
   * killough 9/7/98: keep facing towards target if strafing or backing out
   */

  if (actor->strafecount)
    A_FaceTarget(actor);
  else if (actor->movedir < 8)
    {
      int delta = (actor->angle &= (7<<29)) - (actor->movedir << 29);
      if (delta > 0)
        actor->angle -= ANG90/2;
      else
        if (delta < 0)
          actor->angle += ANG90/2;
    }

  if (!actor->target || !(actor->target->flags&MF_SHOOTABLE))
    {
      if (!P_LookForTargets(actor,TRUE)) // look for a new target
  P_SetMobjState(actor, actor->info->spawnstate); // no new target
      return;
    }

  // do not attack twice in a row
  if (actor->flags & MF_JUSTATTACKED)
    {
      actor->flags &= ~MF_JUSTATTACKED;
      if (gameskill != sk_nightmare && !fastparm)
        P_NewChaseDir(actor);
      return;
    }

  // check for melee attack
  if (actor->info->meleestate && P_CheckMeleeRange(actor))
    {
      if (actor->info->attacksound)
        S_StartSound(actor, actor->info->attacksound);
      P_SetMobjState(actor, actor->info->meleestate);
      /* killough 8/98: remember an attack
      * cph - DEMOSYNC? */
      if (!actor->info->missilestate)
  actor->flags |= MF_JUSTHIT;
      return;
    }

  // check for missile attack
  if (actor->info->missilestate)
    if (!(gameskill < sk_nightmare && !fastparm && actor->movecount))
      if (P_CheckMissileRange(actor))
        {
          P_SetMobjState(actor, actor->info->missilestate);
          actor->flags |= MF_JUSTATTACKED;
          return;
        }

  if (!actor->threshold) {
    if (!mbf_features)
      {   /* killough 9/9/98: for backward demo compatibility */
  if (netgame && !P_CheckSight(actor, actor->target) &&
      P_LookForPlayers(actor, TRUE))
    return;
      }
    /* killough 7/18/98, 9/9/98: new monster AI */
    else if (help_friends && P_HelpFriend(actor))
      return;      /* killough 9/8/98: Help friends in need */
    /* Look for new targets if current one is bad or is out of view */
    else if (actor->pursuecount)
      actor->pursuecount--;
    else {
  /* Our pursuit time has expired. We're going to think about
   * changing targets */
  actor->pursuecount = BASETHRESHOLD;

  /* Unless (we have a live target
   *         and it's not friendly
   *         and we can see it)
   *  try to find a new one; return if sucessful */

  if (!(actor->target && actor->target->health > 0 &&
        ((comp[comp_pursuit] && !netgame) ||
         (((actor->target->flags ^ actor->flags) & MF_FRIEND ||
     (!(actor->flags & MF_FRIEND) && monster_infighting)) &&
    P_CheckSight(actor, actor->target))))
      && P_LookForTargets(actor, TRUE))
        return;

  /* (Current target was good, or no new target was found.)
   *
   * If monster is a missile-less friend, give up pursuit and
   * return to player, if no attacks have occurred recently.
   */

  if (!actor->info->missilestate && actor->flags & MF_FRIEND) {
    if (actor->flags & MF_JUSTHIT)          /* if recent action, */
      actor->flags &= ~MF_JUSTHIT;          /* keep fighting */
    else if (P_LookForPlayers(actor, TRUE)) /* else return to player */
      return;
  }
    }
  }

  if (actor->strafecount)
    actor->strafecount--;

  // chase towards player
  if (--actor->movecount<0 || !P_SmartMove(actor))
    P_NewChaseDir(actor);

  // make active sound
  if (actor->info->activesound && P_Random(pr_see)<3)
    S_StartSound(actor, actor->info->activesound);
}

//
// A_FaceTarget
//
void A_FaceTarget(mobj_t *actor)
{
  if (!actor->target)
    return;
  actor->flags &= ~MF_AMBUSH;
  actor->angle = R_PointToAngle2(actor->x, actor->y,
                                 actor->target->x, actor->target->y);
  if (actor->target->flags & MF_SHADOW)
    { // killough 5/5/98: remove dependence on order of evaluation:
      int t = P_Random(pr_facetarget);
      actor->angle += (t-P_Random(pr_facetarget))<<21;
    }
}

//
// A_PosAttack
//

void A_PosAttack(mobj_t *actor)
{
  int angle, damage, slope, t;

  if (!actor->target)
    return;
  A_FaceTarget(actor);
  angle = actor->angle;
  slope = P_AimLineAttack(actor, angle, MISSILERANGE, 0); /* killough 8/2/98 */
  S_StartSound(actor, sfx_pistol);

  // killough 5/5/98: remove dependence on order of evaluation:
  t = P_Random(pr_posattack);
  angle += (t - P_Random(pr_posattack))<<20;
  damage = (P_Random(pr_posattack)%5 + 1)*3;
  P_LineAttack(actor, angle, MISSILERANGE, slope, damage);
}

void A_SPosAttack(mobj_t* actor)
{
  int i, bangle, slope;

  if (!actor->target)
    return;
  S_StartSound(actor, sfx_shotgn);
  A_FaceTarget(actor);
  bangle = actor->angle;
  slope = P_AimLineAttack(actor, bangle, MISSILERANGE, 0); /* killough 8/2/98 */
  for (i=0; i<3; i++)
    {  // killough 5/5/98: remove dependence on order of evaluation:
      int t = P_Random(pr_sposattack);
      int angle = bangle + ((t - P_Random(pr_sposattack))<<20);
      int damage = ((P_Random(pr_sposattack)%5)+1)*3;
      P_LineAttack(actor, angle, MISSILERANGE, slope, damage);
    }
}

void A_CPosAttack(mobj_t *actor)
{
  int angle, bangle, damage, slope, t;

  if (!actor->target)
    return;
  S_StartSound(actor, sfx_shotgn);
  A_FaceTarget(actor);
  bangle = actor->angle;
  slope = P_AimLineAttack(actor, bangle, MISSILERANGE, 0); /* killough 8/2/98 */

  // killough 5/5/98: remove dependence on order of evaluation:
  t = P_Random(pr_cposattack);
  angle = bangle + ((t - P_Random(pr_cposattack))<<20);
  damage = ((P_Random(pr_cposattack)%5)+1)*3;
  P_LineAttack(actor, angle, MISSILERANGE, slope, damage);
}

void A_CPosRefire(mobj_t *actor)
{
  // keep firing unless target got out of sight
  A_FaceTarget(actor);

  /* killough 12/98: Stop firing if a friend has gotten in the way */
  if (P_HitFriend(actor))
    goto stop;

  /* killough 11/98: prevent refiring on friends continuously */
  if (P_Random(pr_cposrefire) < 40) {
    if (actor->target && actor->flags & actor->target->flags & MF_FRIEND)
      goto stop;
    else
      return;
  }

  if (!actor->target || actor->target->health <= 0
      || !P_CheckSight(actor, actor->target))
stop:  P_SetMobjState(actor, actor->info->seestate);
}

void A_SpidRefire(mobj_t* actor)
{
  // keep firing unless target got out of sight
  A_FaceTarget(actor);

  /* killough 12/98: Stop firing if a friend has gotten in the way */
  if (P_HitFriend(actor))
    goto stop;

  if (P_Random(pr_spidrefire) < 10)
    return;

  // killough 11/98: prevent refiring on friends continuously
  if (!actor->target || actor->target->health <= 0
      || actor->flags & actor->target->flags & MF_FRIEND
      || !P_CheckSight(actor, actor->target))
    stop: P_SetMobjState(actor, actor->info->seestate);
}

void A_BspiAttack(mobj_t *actor)
{
  if (!actor->target)
    return;
  A_FaceTarget(actor);
  P_SpawnMissile(actor, actor->target, MT_ARACHPLAZ);  // launch a missile
}

//
// A_TroopAttack
//

void A_TroopAttack(mobj_t *actor)
{
  if (!actor->target)
    return;
  A_FaceTarget(actor);
  if (P_CheckMeleeRange(actor))
    {
      int damage;
      S_StartSound(actor, sfx_claw);
      damage = (P_Random(pr_troopattack)%8+1)*3;
      P_DamageMobj(actor->target, actor, actor, damage);
      return;
    }
  P_SpawnMissile(actor, actor->target, MT_TROOPSHOT);  // launch a missile
}

void A_SargAttack(mobj_t *actor)
{
  if (!actor->target)
    return;
  A_FaceTarget(actor);
  if (P_CheckMeleeRange(actor))
    {
      int damage = ((P_Random(pr_sargattack)%10)+1)*4;
      P_DamageMobj(actor->target, actor, actor, damage);
    }
}

void A_HeadAttack(mobj_t *actor)
{
  if (!actor->target)
    return;
  A_FaceTarget (actor);
  if (P_CheckMeleeRange(actor))
    {
      int damage = (P_Random(pr_headattack)%6+1)*10;
      P_DamageMobj(actor->target, actor, actor, damage);
      return;
    }
  P_SpawnMissile(actor, actor->target, MT_HEADSHOT);  // launch a missile
}

void A_CyberAttack(mobj_t *actor)
{
  if (!actor->target)
    return;
  A_FaceTarget(actor);
  P_SpawnMissile(actor, actor->target, MT_ROCKET);
}

void A_BruisAttack(mobj_t *actor)
{
  if (!actor->target)
    return;
  if (P_CheckMeleeRange(actor))
    {
      int damage;
      S_StartSound(actor, sfx_claw);
      damage = (P_Random(pr_bruisattack)%8+1)*10;
      P_DamageMobj(actor->target, actor, actor, damage);
      return;
    }
  P_SpawnMissile(actor, actor->target, MT_BRUISERSHOT);  // launch a missile
}

//
// A_SkelMissile
//

void A_SkelMissile(mobj_t *actor)
{
  mobj_t *mo;

  if (!actor->target)
    return;

  A_FaceTarget (actor);
  actor->z += 16*FRACUNIT;      // so missile spawns higher
  mo = P_SpawnMissile (actor, actor->target, MT_TRACER);
  actor->z -= 16*FRACUNIT;      // back to normal

  mo->x += mo->momx;
  mo->y += mo->momy;
  P_SetTarget(&mo->tracer, actor->target);
}

int     TRACEANGLE = 0xc000000;

void A_Tracer(mobj_t *actor)
{
  angle_t       exact;
  fixed_t       dist;
  fixed_t       slope;
  mobj_t        *dest;
  mobj_t        *th;

  /* killough 1/18/98: this is why some missiles do not have smoke
   * and some do. Also, internal demos start at random gametics, thus
   * the bug in which revenants cause internal demos to go out of sync.
   *
   * killough 3/6/98: fix revenant internal demo bug by subtracting
   * levelstarttic from gametic.
   *
   * killough 9/29/98: use new "basetic" so that demos stay in sync
   * during pauses and menu activations, while retaining old demo sync.
   *
   * leveltime would have been better to use to start with in Doom, but
   * since old demos were recorded using gametic, we must stick with it,
   * and improvise around it (using leveltime causes desync across levels).
   */

  if ((gametic-basetic) & 3)
    return;

  // spawn a puff of smoke behind the rocket
  P_SpawnPuff(actor->x, actor->y, actor->z);

  th = P_SpawnMobj (actor->x-actor->momx,
                    actor->y-actor->momy,
                    actor->z, MT_SMOKE);

  th->momz = FRACUNIT;
  th->tics -= P_Random(pr_tracer) & 3;
  if (th->tics < 1)
    th->tics = 1;

  // adjust direction
  dest = actor->tracer;

  if (!dest || dest->health <= 0)
    return;

  // change angle
  exact = R_PointToAngle2(actor->x, actor->y, dest->x, dest->y);

  if (exact != actor->angle) {
    if (exact - actor->angle > 0x80000000)
      {
        actor->angle -= TRACEANGLE;
        if (exact - actor->angle < 0x80000000)
          actor->angle = exact;
      }
    else
      {
        actor->angle += TRACEANGLE;
        if (exact - actor->angle > 0x80000000)
          actor->angle = exact;
      }
  }

  exact = actor->angle>>ANGLETOFINESHIFT;
  actor->momx = FixedMul(actor->info->speed, finecosine[exact]);
  actor->momy = FixedMul(actor->info->speed, finesine[exact]);

  // change slope
  dist = P_AproxDistance(dest->x - actor->x, dest->y - actor->y);

  dist = dist / actor->info->speed;

  if (dist < 1)
    dist = 1;

  slope = (dest->z+40*FRACUNIT - actor->z) / dist;

  if (slope < actor->momz)
    actor->momz -= FRACUNIT/8;
  else
    actor->momz += FRACUNIT/8;
}

void A_SkelWhoosh(mobj_t *actor)
{
  if (!actor->target)
    return;
  A_FaceTarget(actor);
  S_StartSound(actor,sfx_skeswg);
}

void A_SkelFist(mobj_t *actor)
{
  if (!actor->target)
    return;
  A_FaceTarget(actor);
  if (P_CheckMeleeRange(actor))
    {
      int damage = ((P_Random(pr_skelfist)%10)+1)*6;
      S_StartSound(actor, sfx_skepch);
      P_DamageMobj(actor->target, actor, actor, damage);
    }
}

//
// PIT_VileCheck
// Detect a corpse that could be raised.
//

mobj_t* corpsehit;
mobj_t* vileobj;
fixed_t viletryx;
fixed_t viletryy;
/* MBF21: A_HealChase heals corpses within an arbitrary radius; A_VileChase
 * uses the archvile's radius.  PIT_VileCheck reads this. */
fixed_t viletryradius;

static dbool   PIT_VileCheck(mobj_t *thing)
{
  int     maxdist;
  dbool   check;

  if (!(thing->flags & MF_CORPSE) )
    return TRUE;        // not a monster

  if (thing->tics != -1)
    return TRUE;        // not lying still yet

  if (thing->info->raisestate == S_NULL)
    return TRUE;        // monster doesn't have a raise state

  maxdist = thing->info->radius + viletryradius;

  if (D_abs(thing->x-viletryx) > maxdist || D_abs(thing->y-viletryy) > maxdist)
    return TRUE;                // not actually touching

// Check to see if the radius and height are zero. If they are      // phares
// then this is a crushed monster that has been turned into a       //   |
// gib. One of the options may be to ignore this guy.               //   V

// Option 1: the original, buggy method, -> ghost (compatibility)
// Option 2: ressurect the monster, but not as a ghost
// Option 3: ignore the gib

//    if (Option3)                                                  //   ^
//        if ((thing->height == 0) && (thing->radius == 0))         //   |
//            return TRUE;                                          // phares

    corpsehit = thing;
    corpsehit->momx = corpsehit->momy = 0;
    if (comp[comp_vile])                                            // phares
      {                                                             //   |
        corpsehit->height <<= 2;                                    //   V
        check = P_CheckPosition(corpsehit,corpsehit->x,corpsehit->y);
        corpsehit->height >>= 2;
      }
    else
      {
        int height,radius;

        height = corpsehit->height; // save temporarily
        radius = corpsehit->radius; // save temporarily
        corpsehit->height = corpsehit->info->height;
        corpsehit->radius = corpsehit->info->radius;
        corpsehit->flags |= MF_SOLID;
        check = P_CheckPosition(corpsehit,corpsehit->x,corpsehit->y);
        corpsehit->height = height; // restore
        corpsehit->radius = radius; // restore                      //   ^
        corpsehit->flags &= ~MF_SOLID;
      }                                                             //   |
                                                                    // phares
    if (!check)
      return TRUE;              // doesn't fit here
    return FALSE;               // got one, so stop checking
}

//
// A_VileChase
// Check for ressurecting a body
//

void A_VileChase(mobj_t* actor)
{
  int xl, xh;
  int yl, yh;
  int bx, by;

  if (actor->movedir != DI_NODIR)
    {
      // check for corpses to raise
      viletryx =
        actor->x + actor->info->speed*xspeed[actor->movedir];
      viletryy =
        actor->y + actor->info->speed*yspeed[actor->movedir];

      viletryradius = mobjinfo[MT_VILE].radius;

      xl = (viletryx - bmaporgx - MAXRADIUS*2)>>MAPBLOCKSHIFT;
      xh = (viletryx - bmaporgx + MAXRADIUS*2)>>MAPBLOCKSHIFT;
      yl = (viletryy - bmaporgy - MAXRADIUS*2)>>MAPBLOCKSHIFT;
      yh = (viletryy - bmaporgy + MAXRADIUS*2)>>MAPBLOCKSHIFT;

      vileobj = actor;
      for (bx=xl ; bx<=xh ; bx++)
        {
          for (by=yl ; by<=yh ; by++)
            {
              // Call PIT_VileCheck to check
              // whether object is a corpse
              // that canbe raised.
              if (!P_BlockThingsIterator(bx,by,PIT_VileCheck))
                {
      mobjinfo_t *info;

                  // got one!
                  mobj_t* temp = actor->target;
                  actor->target = corpsehit;
                  A_FaceTarget(actor);
                  actor->target = temp;

                  P_SetMobjState(actor, S_VILE_HEAL1);
                  S_StartSound(corpsehit, sfx_slop);
                  info = corpsehit->info;

                  P_SetMobjState(corpsehit,info->raisestate);

                  if (comp[comp_vile])                              // phares
                    corpsehit->height <<= 2;                        //   |
                  else                                              //   V
                    {
                      corpsehit->height = info->height; // fix Ghost bug
                      corpsehit->radius = info->radius; // fix Ghost bug
                    }                                               // phares

      /* killough 7/18/98:
       * friendliness is transferred from AV to raised corpse
       */
      corpsehit->flags =
        (info->flags & ~MF_FRIEND) | (actor->flags & MF_FRIEND);

      corpsehit->intflags = corpsehit->intflags | MIF_RESURRECTED;//mark as resurrected

      if (!((corpsehit->flags ^ MF_COUNTKILL) & (MF_FRIEND | MF_COUNTKILL)))
          totallive++;

      corpsehit->health = info->spawnhealth;
      P_SetTarget(&corpsehit->target, NULL);  // killough 11/98

      if (mbf_features)
        {         /* kilough 9/9/98 */
          P_SetTarget(&corpsehit->lastenemy, NULL);
          corpsehit->flags &= ~MF_JUSTHIT;
        }

      /* killough 8/29/98: add to appropriate thread */
      P_UpdateThinker(&corpsehit->thinker);

                  return;
                }
            }
        }
    }
  A_Chase(actor);  // Return to normal attack.
}

//
// A_VileStart
//

void A_VileStart(mobj_t *actor)
{
  S_StartSound(actor, sfx_vilatk);
}

//
// A_Fire
// Keep fire in front of player unless out of sight
//

void A_StartFire(mobj_t *actor)
{
  S_StartSound(actor,sfx_flamst);
  A_Fire(actor);
}

void A_FireCrackle(mobj_t* actor)
{
  S_StartSound(actor,sfx_flame);
  A_Fire(actor);
}

void A_Fire(mobj_t *actor)
{
  unsigned an;
  mobj_t *dest = actor->tracer;

  if (!dest)
    return;

  // don't move it if the vile lost sight
  if (!P_CheckSight(actor->target, dest) )
    return;

  an = dest->angle >> ANGLETOFINESHIFT;

  P_UnsetThingPosition(actor);
  actor->x = dest->x + FixedMul(STEPSIZE, finecosine[an]);
  actor->y = dest->y + FixedMul(STEPSIZE, finesine[an]);
  actor->z = dest->z;
  P_SetThingPosition(actor);
}

//
// A_VileTarget
// Spawn the hellfire
//

void A_VileTarget(mobj_t *actor)
{
  mobj_t *fog;

  if (!actor->target)
    return;

  A_FaceTarget(actor);

  // killough 12/98: fix Vile fog coordinates // CPhipps - compatibility optioned
  fog = P_SpawnMobj(actor->target->x,
    (compatibility_level < lxdoom_1_compatibility) ? actor->target->x : actor->target->y,
                    actor->target->z,MT_FIRE);

  P_SetTarget(&actor->tracer, fog);
  P_SetTarget(&fog->target, actor);
  P_SetTarget(&fog->tracer, actor->target);
  A_Fire(fog);
}

//
// A_VileAttack
//

void A_VileAttack(mobj_t *actor)
{
  mobj_t *fire;
  int    an;

  if (!actor->target)
    return;

  A_FaceTarget(actor);

  if (!P_CheckSight(actor, actor->target))
    return;

  S_StartSound(actor, sfx_barexp);
  P_DamageMobj(actor->target, actor, actor, 20);
  actor->target->momz = 1000*FRACUNIT/actor->target->info->mass;

  an = actor->angle >> ANGLETOFINESHIFT;

  fire = actor->tracer;

  if (!fire)
    return;

  // move the fire between the vile and the player
  fire->x = actor->target->x - FixedMul (STEPSIZE, finecosine[an]);
  fire->y = actor->target->y - FixedMul (STEPSIZE, finesine[an]);
  P_RadiusAttack(fire, actor, 70);
}

//
// Mancubus attack,
// firing three missiles (bruisers)
// in three different directions?
// Doesn't look like it.
//

#define FATSPREAD       (ANG90/8)

void A_FatRaise(mobj_t *actor)
{
  A_FaceTarget(actor);
  S_StartSound(actor, sfx_manatk);
}

void A_FatAttack1(mobj_t *actor)
{
  mobj_t *mo;
  int    an;

  if (!actor->target)
    return;

  A_FaceTarget(actor);

  // Change direction  to ...
  actor->angle += FATSPREAD;

  P_SpawnMissile(actor, actor->target, MT_FATSHOT);

  mo = P_SpawnMissile (actor, actor->target, MT_FATSHOT);
  mo->angle += FATSPREAD;
  an = mo->angle >> ANGLETOFINESHIFT;
  mo->momx = FixedMul(mo->info->speed, finecosine[an]);
  mo->momy = FixedMul(mo->info->speed, finesine[an]);
}

void A_FatAttack2(mobj_t *actor)
{
  mobj_t *mo;
  int    an;

  if (!actor->target)
    return;

  A_FaceTarget(actor);
  // Now here choose opposite deviation.
  actor->angle -= FATSPREAD;
  P_SpawnMissile(actor, actor->target, MT_FATSHOT);

  mo = P_SpawnMissile(actor, actor->target, MT_FATSHOT);
  mo->angle -= FATSPREAD*2;
  an = mo->angle >> ANGLETOFINESHIFT;
  mo->momx = FixedMul(mo->info->speed, finecosine[an]);
  mo->momy = FixedMul(mo->info->speed, finesine[an]);
}

void A_FatAttack3(mobj_t *actor)
{
  mobj_t *mo;
  int    an;

  if (!actor->target)
    return;

  A_FaceTarget(actor);

  mo = P_SpawnMissile(actor, actor->target, MT_FATSHOT);
  mo->angle -= FATSPREAD/2;
  an = mo->angle >> ANGLETOFINESHIFT;
  mo->momx = FixedMul(mo->info->speed, finecosine[an]);
  mo->momy = FixedMul(mo->info->speed, finesine[an]);

  mo = P_SpawnMissile(actor, actor->target, MT_FATSHOT);
  mo->angle += FATSPREAD/2;
  an = mo->angle >> ANGLETOFINESHIFT;
  mo->momx = FixedMul(mo->info->speed, finecosine[an]);
  mo->momy = FixedMul(mo->info->speed, finesine[an]);
}


//
// SkullAttack
// Fly at the player like a missile.
//
#define SKULLSPEED              (20*FRACUNIT)

void A_SkullAttack(mobj_t *actor)
{
  mobj_t  *dest;
  angle_t an;
  int     dist;

  if (!actor->target)
    return;

  dest = actor->target;
  actor->flags |= MF_SKULLFLY;

  S_StartSound(actor, actor->info->attacksound);
  A_FaceTarget(actor);
  an = actor->angle >> ANGLETOFINESHIFT;
  actor->momx = FixedMul(SKULLSPEED, finecosine[an]);
  actor->momy = FixedMul(SKULLSPEED, finesine[an]);
  dist = P_AproxDistance(dest->x - actor->x, dest->y - actor->y);
  dist = dist / SKULLSPEED;

  if (dist < 1)
    dist = 1;
  actor->momz = (dest->z+(dest->height>>1) - actor->z) / dist;
}

//
// A_BetaSkullAttack
// The flying skull had different behavior on Beta Doom
//

void A_BetaSkullAttack(void *a)
{
  mobj_t *actor = (mobj_t *)a;
  int damage;

  if (compatibility_level < mbf_compatibility)
    return;

  if (!actor->target || actor->target->type == MT_SKULL)
    return;

  S_StartSound(actor, actor->info->attacksound);
  A_FaceTarget(actor);
  damage = (P_Random(pr_skullfly)%8+1)*actor->info->damage;
  P_DamageMobj(actor->target, actor, actor, damage);
}

//
// A_Stop
// The flying skull had different behavior on Beta Doom
//

void A_Stop(void *a)
{
  mobj_t *actor = (mobj_t *)a;
  if (compatibility_level < mbf_compatibility)
    return;

  actor->momx = actor->momy = actor->momz = 0;
}

//
// A_PainShootSkull
// Spawn a lost soul and launch it at the target
//

static void A_PainShootSkull(mobj_t *actor, angle_t angle)
{
  fixed_t       x,y,z;
  mobj_t        *newmobj;
  angle_t       an;
  int           prestep;

// The original code checked for 20 skulls on the level,            // phares
// and wouldn't spit another one if there were. If not in           // phares
// compatibility mode, we remove the limit.                         // phares
                                                                    // phares
  if (comp[comp_pain]) /* killough 10/98: compatibility-optioned */
    {
      // count total number of skulls currently on the level
      int count = 0;
      thinker_t *currentthinker = NULL;
      while ((currentthinker = P_NextThinker(currentthinker,th_all)) != NULL)
        if ((currentthinker->function.arg1 == (void (*)(void *))P_MobjThinker)
            && ((mobj_t *)currentthinker)->type == MT_SKULL)
          count++;
      if (count > 20)                                               // phares
        return;                                                     // phares
    }

  // okay, there's room for another one

  an = angle >> ANGLETOFINESHIFT;

  prestep = 4*FRACUNIT + 3*(actor->info->radius + mobjinfo[MT_SKULL].radius)/2;

  x = actor->x + FixedMul(prestep, finecosine[an]);
  y = actor->y + FixedMul(prestep, finesine[an]);
  z = actor->z + 8*FRACUNIT;

  if (comp[comp_skull])   /* killough 10/98: compatibility-optioned */
    newmobj = P_SpawnMobj(x, y, z, MT_SKULL);                     // phares
  else                                                            //   V
    {
      // Check whether the Lost Soul is being fired through a 1-sided
      // wall or an impassible line, or a "monsters can't cross" line.
      // If it is, then we don't allow the spawn. This is a bug fix, but
      // it should be considered an enhancement, since it may disturb
      // existing demos, so don't do it in compatibility mode.

      if (Check_Sides(actor,x,y))
        return;

      newmobj = P_SpawnMobj(x, y, z, MT_SKULL);

      // Check to see if the new Lost Soul's z value is above the
      // ceiling of its new sector, or below the floor. If so, kill it.

      if ((newmobj->z >
           (newmobj->subsector->sector->ceilingheight - newmobj->height)) ||
          (newmobj->z < newmobj->subsector->sector->floorheight))
        {
          // kill it immediately
          P_DamageMobj(newmobj,actor,actor,10000);
          return;                                                 //   ^
        }                                                         //   |
     }                                                            // phares

  /* killough 7/20/98: PEs shoot lost souls with the same friendliness */
  newmobj->flags = (newmobj->flags & ~MF_FRIEND) | (actor->flags & MF_FRIEND);

  /* killough 8/29/98: add to appropriate thread */
  P_UpdateThinker(&newmobj->thinker);

  // Check for movements.
  // killough 3/15/98: don't jump over dropoffs:

  if (!P_TryMove(newmobj, newmobj->x, newmobj->y, FALSE))
    {
      // kill it immediately
      P_DamageMobj(newmobj, actor, actor, 10000);
      return;
    }

  P_SetTarget(&newmobj->target, actor->target);
  A_SkullAttack(newmobj);
}

//
// A_PainAttack
// Spawn a lost soul and launch it at the target
//

void A_PainAttack(mobj_t *actor)
{
  if (!actor->target)
    return;
  A_FaceTarget(actor);
  A_PainShootSkull(actor, actor->angle);
}

void A_PainDie(mobj_t *actor)
{
  A_Fall(actor);
  A_PainShootSkull(actor, actor->angle+ANG90);
  A_PainShootSkull(actor, actor->angle+ANG180);
  A_PainShootSkull(actor, actor->angle+ANG270);
}

void A_Scream(mobj_t *actor)
{
  int sound;

  switch (actor->info->deathsound)
    {
    case 0:
      return;

    case sfx_podth1:
    case sfx_podth2:
    case sfx_podth3:
      sound = sfx_podth1 + P_Random(pr_scream)%3;
      break;

    case sfx_bgdth1:
    case sfx_bgdth2:
      sound = sfx_bgdth1 + P_Random(pr_scream)%2;
      break;

    default:
      sound = actor->info->deathsound;
      break;
    }

  // Check for bosses.
  if ((actor->flags & MF_FULLVOLDEATH) ||
      (actor->flags2 & (MF2_FULLVOLSOUNDS | MF2_BOSS))) /* MBF21 */
    S_StartSound(NULL, sound); // full volume
  else
    S_StartSound(actor, sound);
}

void A_XScream(mobj_t *actor)
{
  S_StartSound(actor, sfx_slop);
}

void A_Pain(mobj_t *actor)
{
  if (actor->info->painsound)
    S_StartSound(actor, actor->info->painsound);
}

void A_Fall(mobj_t *actor)
{
  // actor is on ground, it can be walked over
  actor->flags &= ~MF_SOLID;
}

//
// A_Explode
//
void A_Explode(mobj_t *thingy)
{
  int damage   = 128;
  int distance = 128;

  /* Heretic gives several actors their own blast damage/radius, and the
   * firebomb raises itself before bursting. Without this they all used the
   * Doom 128/128 default. */
  if (heretic)
  {
    switch (thingy->type)
    {
      case HERETIC_MT_FIREBOMB:        /* time bomb of the ancients */
        thingy->z += 32 * FRACUNIT;
        thingy->flags &= ~MF_SHADOW;
        break;
      case HERETIC_MT_MNTRFX2:         /* minotaur floor fire */
        damage = 24;
        distance = damage;
        break;
      case HERETIC_MT_SOR2FX1:         /* D'Sparil missile */
        damage = 80 + (P_Random(pr_heretic) & 31);
        distance = damage;
        break;
      default:
        break;
    }
  }

  P_RadiusAttackEx(thingy, thingy->target, damage, distance);
  retro_set_rumble_damage(60, 500.f);
}

//
// A_BossDeath
// Possibly trigger special effects
// if on first boss level
//

void A_BossDeath(mobj_t *mo)
{
  thinker_t *th;
  line_t    junk;
  int       i;

  /* MBF21: when the dying thing carries MBF21 boss flags, qualification
   * and the triggered action are driven by those flags instead of the
   * hardcoded type/map checks below.  Gated on mbf21_features so vanilla/
   * boom/mbf/prboom demos take the unchanged path. */
  if (mbf21_features && (mo->flags2 &
        (MF2_MAP07BOSS1 | MF2_MAP07BOSS2 | MF2_E1M8BOSS | MF2_E2M8BOSS |
         MF2_E3M8BOSS  | MF2_E4M6BOSS  | MF2_E4M8BOSS)))
  {
    /* make sure there is a player alive for victory */
    for (i = 0; i < MAXPLAYERS; i++)
      if (playeringame[i] && players[i].health > 0)
        break;
    if (i == MAXPLAYERS)
      return;

    /* all bosses of this type dead? */
    for (th = thinkercap.next; th != &thinkercap; th = th->next)
      if (th->function.arg1 == (void (*)(void *))P_MobjThinker)
      {
        mobj_t *mo2 = (mobj_t *) th;
        if (mo2 != mo && mo2->type == mo->type && mo2->health > 0)
          return;
      }

    memset(&junk, 0, sizeof(junk));
    if (mo->flags2 & MF2_MAP07BOSS1)
      { junk.tag = 666; EV_DoFloor(&junk, FLEV_LOWERFLOORTOLOWEST); }
    if (mo->flags2 & MF2_MAP07BOSS2)
      { junk.tag = 667; EV_DoFloor(&junk, FLEV_RAISETOTEXTURE); }
    if (mo->flags2 & MF2_E1M8BOSS)
      { junk.tag = 666; EV_DoFloor(&junk, FLEV_LOWERFLOORTOLOWEST); }
    if (mo->flags2 & MF2_E2M8BOSS)
      G_ExitLevel();
    if (mo->flags2 & MF2_E3M8BOSS)
      G_ExitLevel();
    if (mo->flags2 & MF2_E4M6BOSS)
      { junk.tag = 666; EV_DoDoor(&junk, blazeOpen); }
    if (mo->flags2 & MF2_E4M8BOSS)
      { junk.tag = 666; EV_DoFloor(&junk, FLEV_LOWERFLOORTOLOWEST); }
    return;
  }

  // numbossactions == 0 means to use the defaults.
  // numbossactions == -1 means to do nothing.
  // positive values mean to check the list of boss actions and run all that apply.
  if (gamemapinfo && gamemapinfo->numbossactions != 0)
  {
    if (gamemapinfo->numbossactions < 0) return; // -1 clears all bossaction

    for (i = 0; i < gamemapinfo->numbossactions; i++)
    {
      if (gamemapinfo->bossactions[i].type == mo->type)
        break;
    }
    if (i >= gamemapinfo->numbossactions)
      return;	// no matches found
  }
  else if (gamemode == commercial)
    {
      if (gamemap != 7)
        return;

      if ((mo->type != MT_FATSO)
          && (mo->type != MT_BABY))
        return;
    }
  else
    {
      // e6y
      // Additional check of gameepisode is necessary, because
      // there is no right or wrong solution for E4M6 in original EXEs,
      // there's nothing to emulate.
      if (comp[comp_666] && gameepisode < 4)
      {
        // e6y
        // Only following checks are present in doom2.exe ver. 1.666 and 1.9
        // instead of separate checks for each episode in doomult.exe, plutonia.exe and tnt.exe
        // There is no more desync on doom.wad\episode3.lmp
        // http://www.doomworld.com/idgames/index.php?id=6909
        if (gamemap != 8)
          return;
        if (mo->type == MT_BRUISER && gameepisode != 1)
          return;
      }
      else
      {
      switch(gameepisode)
        {
        case 1:
          if (gamemap != 8)
            return;

          if (mo->type != MT_BRUISER)
            return;
          break;

        case 2:
          if (gamemap != 8)
            return;

          if (mo->type != MT_CYBORG)
            return;
          break;

        case 3:
          if (gamemap != 8)
            return;

          if (mo->type != MT_SPIDER)
            return;

          break;

        case 4:
          switch(gamemap)
            {
            case 6:
              if (mo->type != MT_CYBORG)
                return;
              break;

            case 8:
              if (mo->type != MT_SPIDER)
                return;
              break;

            default:
              return;
              break;
            }
          break;

        case 5: // sigil
          return;

        default:
          if (gamemap != 8)
            return;
          break;
        }
      }

    }

  // make sure there is a player alive for victory
  for (i=0; i<MAXPLAYERS; i++)
    if (playeringame[i] && players[i].health > 0)
      break;

  if (i==MAXPLAYERS)
    return;     // no one left alive, so do not end game

    // scan the remaining thinkers to see
    // if all bosses are dead
  for (th = thinkercap.next ; th != &thinkercap ; th=th->next)
    if (th->function.arg1 == (void (*)(void *))P_MobjThinker)
      {
        mobj_t *mo2 = (mobj_t *) th;
        if (mo2 != mo && mo2->type == mo->type && mo2->health > 0)
          return;         // other boss not dead
      }

  // victory! apply BossDeath effect
  if (gamemapinfo && gamemapinfo->numbossactions != 0)
  {
    for (i = 0; i < gamemapinfo->numbossactions; i++)
      if (gamemapinfo->bossactions[i].type == mo->type)
      {
        player_t fakeplayer = {0};
        fakeplayer.health = 1; // non-zombie fake player
        fakeplayer.mo = NULL;  // don't play sounds, teleport, etc
        junk = *lines;
        junk.special = (short)gamemapinfo->bossactions[i].special;
        junk.tag = (short)gamemapinfo->bossactions[i].tag;
        // Treat the boss as a temporary fake player to activate the special
        // it'll be excluded from teleportation since fakeplayer->mo == NULL
        mo->player = &fakeplayer;
        if (!P_UseSpecialLine(mo, &junk, 0))
          P_CrossSpecialLine(&junk, 0, mo);
        mo->player = NULL;
      }
  }
  else if ( gamemode == commercial)
    {
      if (gamemap == 7)
        {
          if (mo->type == MT_FATSO)
            {
              junk.tag = 666;
              EV_DoFloor(&junk,FLEV_LOWERFLOORTOLOWEST);
              return;
            }

          if (mo->type == MT_BABY)
            {
              junk.tag = 667;
              EV_DoFloor(&junk, FLEV_RAISETOTEXTURE);
              return;
            }
        }
    }
  else
    {
      switch(gameepisode)
        {
        case 1:
          junk.tag = 666;
          EV_DoFloor(&junk, FLEV_LOWERFLOORTOLOWEST);
          return;
        case 4:
          switch(gamemap)
            {
            case 6:
              junk.tag = 666;
              EV_DoDoor(&junk, blazeOpen);
              return;
            case 8:
              junk.tag = 666;
              EV_DoFloor(&junk, FLEV_LOWERFLOORTOLOWEST);
              return;
            }
        }
    }
  G_ExitLevel();
}


void A_Hoof (mobj_t* mo)
{
    S_StartSound(mo, sfx_hoof);
    A_Chase(mo);
}

void A_Metal(mobj_t *mo)
{
  S_StartSound(mo, sfx_metal);
  A_Chase(mo);
}

void A_BabyMetal(mobj_t *mo)
{
  S_StartSound(mo, sfx_bspwlk);
  A_Chase(mo);
}

void A_OpenShotgun2(player_t *player, pspdef_t *psp)
{
  S_StartSound(player->mo, sfx_dbopn);
}

void A_LoadShotgun2(player_t *player, pspdef_t *psp)
{
  S_StartSound(player->mo, sfx_dbload);
}

void A_CloseShotgun2(player_t *player, pspdef_t *psp)
{
  S_StartSound(player->mo, sfx_dbcls);
  A_ReFire(player,psp);
}

// killough 2/7/98: Remove limit on icon landings:
mobj_t **braintargets;
int    numbraintargets_alloc;
int    numbraintargets;

struct brain_s brain;   // killough 3/26/98: global state of boss brain

// killough 3/26/98: initialize icon landings at level startup,
// rather than at boss wakeup, to prevent savegame-related crashes

void P_SpawnBrainTargets(void)  // killough 3/26/98: renamed old function
{
  thinker_t *thinker;

  // find all the target spots
  numbraintargets = 0;
  brain.targeton = 0;
  brain.easy = 0;           // killough 3/26/98: always init easy to 0

  for (thinker = thinkercap.next ;
       thinker != &thinkercap ;
       thinker = thinker->next)
    if (thinker->function.arg1 == (void (*)(void *))P_MobjThinker)
      {
        mobj_t *m = (mobj_t *) thinker;

        if (m->type == MT_BOSSTARGET )
          {   // killough 2/7/98: remove limit on icon landings:
            if (numbraintargets >= numbraintargets_alloc)
              braintargets = realloc(braintargets,
                      (numbraintargets_alloc = numbraintargets_alloc ?
                       numbraintargets_alloc*2 : 32) *sizeof *braintargets);
            braintargets[numbraintargets++] = m;
          }
      }
}

void A_BrainAwake(mobj_t *mo)
{
  S_StartSound(NULL,sfx_bossit); // killough 3/26/98: only generates sound now
}

void A_BrainPain(mobj_t *mo)
{
  S_StartSound(NULL,sfx_bospn);
}

void A_BrainScream(mobj_t *mo)
{
  int x;
  for (x=mo->x - 196*FRACUNIT ; x< mo->x + 320*FRACUNIT ; x+= FRACUNIT*8)
    {
      int y = mo->y - 320*FRACUNIT;
      int z = 128 + P_Random(pr_brainscream)*2*FRACUNIT;
      mobj_t *th = P_SpawnMobj (x,y,z, MT_ROCKET);
      th->momz = P_Random(pr_brainscream)*512;
      P_SetMobjState(th, S_BRAINEXPLODE1);
      th->tics -= P_Random(pr_brainscream)&7;
      if (th->tics < 1)
        th->tics = 1;
    }
  S_StartSound(NULL,sfx_bosdth);
}

void A_BrainExplode(mobj_t *mo)
{  // killough 5/5/98: remove dependence on order of evaluation:
  int t = P_Random(pr_brainexp);
  int x = mo->x + (t - P_Random(pr_brainexp))*2048;
  int y = mo->y;
  int z = 128 + P_Random(pr_brainexp)*2*FRACUNIT;
  mobj_t *th = P_SpawnMobj(x,y,z, MT_ROCKET);
  th->momz = P_Random(pr_brainexp)*512;
  P_SetMobjState(th, S_BRAINEXPLODE1);
  th->tics -= P_Random(pr_brainexp)&7;
  if (th->tics < 1)
    th->tics = 1;
}

void A_BrainDie(mobj_t *mo)
{
  G_ExitLevel();
}

void A_BrainSpit(mobj_t *mo)
{
  mobj_t *targ, *newmobj;

  if (!numbraintargets)     // killough 4/1/98: ignore if no targets
    return;

  brain.easy ^= 1;          // killough 3/26/98: use brain struct
  if (gameskill <= sk_easy && !brain.easy)
    return;

  // shoot a cube at current target
  targ = braintargets[brain.targeton++]; // killough 3/26/98:
  brain.targeton %= numbraintargets;     // Use brain struct for targets

  // spawn brain missile
  newmobj = P_SpawnMissile(mo, targ, MT_SPAWNSHOT);
  P_SetTarget(&newmobj->target, targ);
  newmobj->reactiontime = (short)(((targ->y-mo->y)/newmobj->momy)/newmobj->state->tics);

  // killough 7/18/98: brain friendliness is transferred
  newmobj->flags = (newmobj->flags & ~MF_FRIEND) | (mo->flags & MF_FRIEND);

  // killough 8/29/98: add to appropriate thread
  P_UpdateThinker(&newmobj->thinker);

  S_StartSound(NULL, sfx_bospit);
}

// travelling cube sound
void A_SpawnSound(mobj_t *mo)
{
  S_StartSound(mo,sfx_boscub);
  A_SpawnFly(mo);
}

void A_SpawnFly(mobj_t *mo)
{
  mobj_t *newmobj;
  mobj_t *fog;
  mobj_t *targ;
  int    r;
  mobjtype_t type;

  if (--mo->reactiontime)
    return;     // still flying

  targ = mo->target;

  // First spawn teleport fog.
  fog = P_SpawnMobj(targ->x, targ->y, targ->z, MT_SPAWNFIRE);
  S_StartSound(fog, sfx_telept);

  // Randomly select monster to spawn.
  r = P_Random(pr_spawnfly);

  // Probability distribution (kind of :), decreasing likelihood.
  if ( r<50 )
    type = MT_TROOP;
  else if (r<90)
    type = MT_SERGEANT;
  else if (r<120)
    type = MT_SHADOWS;
  else if (r<130)
    type = MT_PAIN;
  else if (r<160)
    type = MT_HEAD;
  else if (r<162)
    type = MT_VILE;
  else if (r<172)
    type = MT_UNDEAD;
  else if (r<192)
    type = MT_BABY;
  else if (r<222)
    type = MT_FATSO;
  else if (r<246)
    type = MT_KNIGHT;
  else
    type = MT_BRUISER;

  newmobj = P_SpawnMobj(targ->x, targ->y, targ->z, type);

  /* killough 7/18/98: brain friendliness is transferred */
  newmobj->flags = (newmobj->flags & ~MF_FRIEND) | (mo->flags & MF_FRIEND);

  /* killough 8/29/98: add to appropriate thread */
  P_UpdateThinker(&newmobj->thinker);

  if (P_LookForTargets(newmobj,TRUE))      /* killough 9/4/98 */
    P_SetMobjState(newmobj, newmobj->info->seestate);

    // telefrag anything in this spot
  P_TeleportMove(newmobj, newmobj->x, newmobj->y, TRUE); /* killough 8/9/98 */

  // remove self (i.e., cube).
  P_RemoveMobj(mo);
}

void A_PlayerScream(mobj_t *mo)
{
  int sound = sfx_pldeth;  // Default death sound.
  if (gamemode != shareware && mo->health < -50)
    sound = sfx_pdiehi;   // IF THE PLAYER DIES LESS THAN -50% WITHOUT GIBBING
  S_StartSound(mo, sound);
}

/* cph - MBF-added codepointer functions */

// killough 11/98: kill an object
void A_Die(mobj_t *actor)
{
  P_DamageMobj(actor, NULL, NULL, actor->health);
}

//
// A_Detonate
// killough 8/9/98: same as A_Explode, except that the damage is variable
//

void A_Detonate(mobj_t *mo)
{
  P_RadiusAttack(mo, mo->target, mo->info->damage);
  retro_set_rumble_damage(60, 500.f);
}

//
// killough 9/98: a mushroom explosion effect, sorta :)
// Original idea: Linguica
//

void A_Mushroom(mobj_t *actor)
{
  int i, j, n = actor->info->damage;

  A_Explode(actor);  // First make normal explosion

  // Now launch mushroom cloud
  for (i = -n; i <= n; i += 8)
    for (j = -n; j <= n; j += 8)
      {
  mobj_t target = *actor, *mo;
  target.x += i << FRACBITS;    // Aim in many directions from source
  target.y += j << FRACBITS;
  target.z += P_AproxDistance(i,j) << (FRACBITS+2); // Aim up fairly high
  mo = P_SpawnMissile(actor, &target, MT_FATSHOT);  // Launch fireball
  mo->momx >>= 1;
  mo->momy >>= 1;                                   // Slow it down a bit
  mo->momz >>= 1;
  mo->flags &= ~MF_NOGRAVITY;   // Make debris fall under gravity
      }
}

//
// killough 11/98
//
// The following were inspired by Len Pitre
//
// A small set of highly-sought-after code pointers
//

void A_Spawn(mobj_t *mo)
{
  if (mo->state->misc1)
    {
      /* mobj_t *newmobj = */
      P_SpawnMobj(mo->x, mo->y, (mo->state->misc2 << FRACBITS) + mo->z,
      mo->state->misc1 - 1);
      /* CPhipps - no friendlyness (yet)
   newmobj->flags = (newmobj->flags & ~MF_FRIEND) | (mo->flags & MF_FRIEND);
      */
    }
}

void A_Turn(mobj_t *mo)
{
  mo->angle += (unsigned int)(((uint64_t) mo->state->misc1 << 32) / 360);
}

void A_Face(mobj_t *mo)
{
  mo->angle = (unsigned int)(((uint64_t) mo->state->misc1 << 32) / 360);
}

void A_Scratch(mobj_t *mo)
{
  mo->target && (A_FaceTarget(mo), P_CheckMeleeRange(mo)) ?
    mo->state->misc2 ? S_StartSound(mo, mo->state->misc2) : (void) 0,
    P_DamageMobj(mo->target, mo, mo, mo->state->misc1) : (void) 0;
}

void A_PlaySound(mobj_t *mo)
{
  S_StartSound(mo->state->misc2 ? NULL : mo, mo->state->misc1);
}

void A_RandomJump(mobj_t *mo)
{
  if (P_Random(pr_randomjump) < mo->state->misc2)
    P_SetMobjState(mo, mo->state->misc1);
}

//
// This allows linedef effects to be activated inside deh frames.
//

void A_LineEffect(mobj_t *mo)
{
  static line_t junk;
  player_t player;
  player_t *oldplayer;
  junk = *lines;
  oldplayer = mo->player;
  mo->player = &player;
  player.health = 100;
  junk.special = (short)mo->state->misc1;
  if (!junk.special)
    return;
  junk.tag = (short)mo->state->misc2;
  if (!P_UseSpecialLine(mo, &junk, 0))
    P_CrossSpecialLine(&junk, 0, mo);
  mo->state->misc1 = junk.special;
  mo->player = oldplayer;
}

/* ====================================================================
 * MBF21 codepointers (thing side)
 *
 * Each reads its parameters from the calling state's args[] and is inert
 * unless mbf21_features is active (the actor's state->args are only
 * populated by an MBF21 deh patch, and the gate makes them no-ops at lower
 * complevels regardless).  Mechanics follow the MBF21 spec.
 * ==================================================================== */

/* Heal a nearby corpse within 'radius', like the archvile but
 * parameterised; returns TRUE if one was raised.  Adapted from
 * A_VileChase. */
static dbool P_HealCorpse(mobj_t *actor, fixed_t radius,
                          statenum_t healstate, int healsound)
{
  int xl, xh, yl, yh, bx, by;

  if (actor->movedir != DI_NODIR)
  {
    viletryx = actor->x + actor->info->speed*xspeed[actor->movedir];
    viletryy = actor->y + actor->info->speed*yspeed[actor->movedir];
    viletryradius = radius;

    xl = (viletryx - bmaporgx - MAXRADIUS*2)>>MAPBLOCKSHIFT;
    xh = (viletryx - bmaporgx + MAXRADIUS*2)>>MAPBLOCKSHIFT;
    yl = (viletryy - bmaporgy - MAXRADIUS*2)>>MAPBLOCKSHIFT;
    yh = (viletryy - bmaporgy + MAXRADIUS*2)>>MAPBLOCKSHIFT;

    vileobj = actor;
    for (bx=xl ; bx<=xh ; bx++)
      for (by=yl ; by<=yh ; by++)
        if (!P_BlockThingsIterator(bx,by,PIT_VileCheck))
        {
          mobjinfo_t *info;
          mobj_t *temp = actor->target;
          actor->target = corpsehit;
          A_FaceTarget(actor);
          actor->target = temp;

          P_SetMobjState(actor, healstate);
          S_StartSound(corpsehit, healsound);
          info = corpsehit->info;

          P_SetMobjState(corpsehit,info->raisestate);

          if (comp[comp_vile])
            corpsehit->height <<= 2;
          else
          {
            corpsehit->height = info->height;
            corpsehit->radius = info->radius;
          }

          corpsehit->flags =
            (info->flags & ~MF_FRIEND) | (actor->flags & MF_FRIEND);
          corpsehit->intflags = corpsehit->intflags | MIF_RESURRECTED;

          if (!((corpsehit->flags ^ MF_COUNTKILL) & (MF_FRIEND | MF_COUNTKILL)))
            totallive++;

          corpsehit->health = info->spawnhealth;
          P_SetTarget(&corpsehit->target, NULL);

          if (mbf_features)
          {
            P_SetTarget(&corpsehit->lastenemy, NULL);
            corpsehit->flags &= ~MF_JUSTHIT;
          }

          P_UpdateThinker(&corpsehit->thinker);
          return TRUE;
        }
  }
  return FALSE;
}

/* True if t2 lies within 'fov' of t1's facing angle. */
static dbool P_CheckFov(mobj_t *t1, mobj_t *t2, angle_t fov)
{
  angle_t angle, minang, maxang;
  angle = R_PointToAngle2(t1->x, t1->y, t2->x, t2->y);
  minang = t1->angle - fov / 2;
  maxang = t1->angle + fov / 2;
  return (minang > maxang) ? (angle >= minang || angle <= maxang)
                           : (angle >= minang && angle <= maxang);
}

/* Melee-range check for A_MonsterMeleeAttack (distance + sight, ignoring
 * friend-on-friend). */
static dbool P_CheckRange(mobj_t *actor, fixed_t range)
{
  mobj_t *pl = actor->target;
  return pl &&
    !(actor->flags & pl->flags & MF_FRIEND) &&
    P_AproxDistance(pl->x-actor->x, pl->y-actor->y) < range &&
    P_CheckSight(actor, actor->target);
}

/* Home a missile toward *seekTarget by up to turnMax per call (halving the
 * turn rate once within thresh).  Adapted from A_Tracer's seeking math. */
static dbool P_SeekerMissile(mobj_t *actor, mobj_t **seekTarget,
                             angle_t thresh, angle_t turnMax)
{
  angle_t exact, delta;
  fixed_t dist, slope;
  int dir;
  mobj_t *target = *seekTarget;

  if (!target)
    return FALSE;
  if (!(target->flags & MF_SHOOTABLE)) /* target died */
  {
    *seekTarget = NULL;
    return FALSE;
  }

  exact = R_PointToAngle2(actor->x, actor->y, target->x, target->y);
  if (exact > actor->angle)
  {
    delta = exact - actor->angle;
    dir = 1;                 /* clockwise */
  }
  else
  {
    delta = actor->angle - exact;
    dir = 0;
  }
  if (delta > thresh)
  {
    delta >>= 1;
    if (delta > turnMax)
      delta = turnMax;
  }
  if (dir)
    actor->angle += delta;
  else
    actor->angle -= delta;

  exact = actor->angle >> ANGLETOFINESHIFT;
  actor->momx = FixedMul(actor->info->speed, finecosine[exact]);
  actor->momy = FixedMul(actor->info->speed, finesine[exact]);

  dist = P_AproxDistance(target->x - actor->x, target->y - actor->y);
  dist = dist / actor->info->speed;
  if (dist < 1)
    dist = 1;
  slope = (target->z + (target->height >> 1) -
           (actor->z + (actor->height >> 1))) / dist;
  actor->momz = slope;
  return TRUE;
}

/* Search surrounding blockmap for a valid auto-target within fov/distance.
 * Adapted from Hexen's rough monster search (as used by dsda). */
mobj_t *P_RoughTargetSearch(mobj_t *mo, angle_t fov, int distance)
{
  int startX, startY, count, bx, by;
  mobj_t *link;

  startX = (mo->x - bmaporgx) >> MAPBLOCKSHIFT;
  startY = (mo->y - bmaporgy) >> MAPBLOCKSHIFT;

  for (count = 0; count <= distance; count++)
    for (by = startY - count; by <= startY + count; by++)
    {
      if (by < 0 || by >= bmapheight)
        continue;
      for (bx = startX - count; bx <= startX + count; bx++)
      {
        /* only scan the ring at radius 'count' */
        if (count && bx != startX - count && bx != startX + count &&
            by != startY - count && by != startY + count)
          continue;
        if (bx < 0 || bx >= bmapwidth)
          continue;
        for (link = blocklinks[by*bmapwidth + bx]; link; link = link->bnext)
        {
          if (!(link->flags & MF_SHOOTABLE))
            continue;
          if (link == mo->target)
            continue;
          if (mo->target &&
              !((link->flags ^ mo->target->flags) & MF_FRIEND) &&
              mo->target->target != link &&
              !(deathmatch && link->player && mo->target->player))
            continue;
          if (fov > 0 && !P_CheckFov(mo, link, fov))
            continue;
          if (!P_CheckSight(mo, link))
            continue;
          return link;
        }
      }
    }
  return NULL;
}

void A_SpawnObject(mobj_t *actor)
{
  int type, angle, ofs_x, ofs_y, ofs_z, vel_x, vel_y, vel_z;
  angle_t an;
  int fan, dx, dy;
  mobj_t *mo;

  if (!mbf21_features || !actor->state->args[0])
    return;

  type  = actor->state->args[0] - 1;
  angle = actor->state->args[1];
  ofs_x = actor->state->args[2];
  ofs_y = actor->state->args[3];
  ofs_z = actor->state->args[4];
  vel_x = actor->state->args[5];
  vel_y = actor->state->args[6];
  vel_z = actor->state->args[7];

  an = actor->angle + (unsigned int)(((int64_t)angle << 16) / 360);
  fan = an >> ANGLETOFINESHIFT;
  dx = FixedMul(ofs_x, finecosine[fan]) - FixedMul(ofs_y, finesine[fan]);
  dy = FixedMul(ofs_x, finesine[fan])   + FixedMul(ofs_y, finecosine[fan]);

  mo = P_SpawnMobj(actor->x + dx, actor->y + dy, actor->z + ofs_z, type);
  if (!mo)
    return;

  mo->angle = an;
  mo->momx = FixedMul(vel_x, finecosine[fan]) - FixedMul(vel_y, finesine[fan]);
  mo->momy = FixedMul(vel_x, finesine[fan])   + FixedMul(vel_y, finecosine[fan]);
  mo->momz = vel_z;

  if (mo->info->flags & (MF_MISSILE | MF_BOUNCES))
  {
    if (actor->info->flags & (MF_MISSILE | MF_BOUNCES))
    {
      P_SetTarget(&mo->target, actor->target);
      P_SetTarget(&mo->tracer, actor->tracer);
    }
    else
    {
      P_SetTarget(&mo->target, actor);
      P_SetTarget(&mo->tracer, actor->target);
    }
  }
}

void A_MonsterProjectile(mobj_t *actor)
{
  int type, angle, pitch, spawnofs_xy, spawnofs_z, an;
  mobj_t *mo;

  if (!mbf21_features || !actor->target || !actor->state->args[0])
    return;

  type        = actor->state->args[0] - 1;
  angle       = actor->state->args[1];
  pitch       = actor->state->args[2];
  spawnofs_xy = actor->state->args[3];
  spawnofs_z  = actor->state->args[4];

  A_FaceTarget(actor);
  mo = P_SpawnMissile(actor, actor->target, type);
  if (!mo)
    return;

  mo->angle += (unsigned int)(((int64_t)angle << 16) / 360);
  an = mo->angle >> ANGLETOFINESHIFT;
  mo->momx = FixedMul(mo->info->speed, finecosine[an]);
  mo->momy = FixedMul(mo->info->speed, finesine[an]);
  mo->momz += FixedMul(mo->info->speed, DegToSlope(pitch));

  an = (actor->angle - ANG90) >> ANGLETOFINESHIFT;
  mo->x += FixedMul(spawnofs_xy, finecosine[an]);
  mo->y += FixedMul(spawnofs_xy, finesine[an]);
  mo->z += spawnofs_z;

  P_SetTarget(&mo->tracer, actor->target);
}

void A_MonsterMeleeAttack(mobj_t *actor)
{
  int damagebase, damagemod, hitsound, range, damage;

  if (!mbf21_features || !actor->target)
    return;

  damagebase = actor->state->args[0];
  damagemod  = actor->state->args[1];
  hitsound   = actor->state->args[2];
  range      = actor->state->args[3];

  if (range == 0)
    range = actor->info->meleerange;
  range += actor->target->info->radius - 20 * FRACUNIT;

  A_FaceTarget(actor);
  if (!P_CheckRange(actor, range))
    return;

  S_StartSound(actor, hitsound);
  /* MBF21 allows damagemod == 0 (deterministic damage); guard the modulo. */
  damage = ((damagemod > 0 ? (P_Random(pr_mbf21) % damagemod) : 0) + 1) * damagebase;
  P_DamageMobj(actor->target, actor, actor, damage);
}

void A_RadiusDamage(mobj_t *actor)
{
  if (!mbf21_features || !actor->state)
    return;
  /* args[0] = damage, args[1] = blast radius */
  P_RadiusAttackEx(actor, actor->target,
                   actor->state->args[0], actor->state->args[1]);
}

void A_NoiseAlert(mobj_t *actor)
{
  if (!mbf21_features || !actor->target)
    return;
  P_NoiseAlert(actor->target, actor);
}

void A_HealChase(mobj_t *actor)
{
  int state, sound;
  if (!mbf21_features || !actor)
    return;
  state = actor->state->args[0];
  sound = actor->state->args[1];
  if (!P_HealCorpse(actor, actor->info->radius, state, sound))
    A_Chase(actor);
}

void A_SeekTracer(mobj_t *actor)
{
  angle_t threshold, maxturnangle;
  if (!mbf21_features || !actor)
    return;
  threshold    = FixedToAngle(actor->state->args[0]);
  maxturnangle = FixedToAngle(actor->state->args[1]);
  P_SeekerMissile(actor, &actor->tracer, threshold, maxturnangle);
}

void A_FindTracer(mobj_t *actor)
{
  angle_t fov;
  int dist;
  if (!mbf21_features || !actor || actor->tracer)
    return;
  fov  = FixedToAngle(actor->state->args[0]);
  dist =             (actor->state->args[1]);
  P_SetTarget(&actor->tracer, P_RoughTargetSearch(actor, fov, dist));
}

void A_ClearTracer(mobj_t *actor)
{
  if (!mbf21_features || !actor)
    return;
  P_SetTarget(&actor->tracer, NULL);
}

void A_JumpIfHealthBelow(mobj_t *actor)
{
  int state, health;
  if (!mbf21_features || !actor)
    return;
  state  = actor->state->args[0];
  health = actor->state->args[1];
  if (actor->health < health)
    P_SetMobjState(actor, state);
}

void A_JumpIfTargetInSight(mobj_t *actor)
{
  int state;
  angle_t fov;
  if (!mbf21_features || !actor || !actor->target)
    return;
  state =             (actor->state->args[0]);
  fov   = FixedToAngle(actor->state->args[1]);
  if (fov > 0 && !P_CheckFov(actor, actor->target, fov))
    return;
  if (P_CheckSight(actor, actor->target))
    P_SetMobjState(actor, state);
}

void A_JumpIfTargetCloser(mobj_t *actor)
{
  int state, distance;
  if (!mbf21_features || !actor || !actor->target)
    return;
  state    = actor->state->args[0];
  distance = actor->state->args[1];
  if (distance > P_AproxDistance(actor->x - actor->target->x,
                                 actor->y - actor->target->y))
    P_SetMobjState(actor, state);
}

void A_JumpIfTracerInSight(mobj_t *actor)
{
  int state;
  angle_t fov;
  if (!mbf21_features || !actor || !actor->tracer)
    return;
  state =             (actor->state->args[0]);
  fov   = FixedToAngle(actor->state->args[1]);
  if (fov > 0 && !P_CheckFov(actor, actor->tracer, fov))
    return;
  if (P_CheckSight(actor, actor->tracer))
    P_SetMobjState(actor, state);
}

void A_JumpIfTracerCloser(mobj_t *actor)
{
  int state, distance;
  if (!mbf21_features || !actor || !actor->tracer)
    return;
  state    = actor->state->args[0];
  distance = actor->state->args[1];
  if (distance > P_AproxDistance(actor->x - actor->tracer->x,
                                 actor->y - actor->tracer->y))
    P_SetMobjState(actor, state);
}

void A_JumpIfFlagsSet(mobj_t *actor)
{
  int state;
  uint64_t flags, flags2;
  if (!mbf21_features || !actor)
    return;
  state  = actor->state->args[0];
  /* args[] is a 32-bit (signed) long on LLP64 targets; cast through
   * uint32_t first so a flag value with bit 31 set is not sign-extended
   * into the high 32 bits of the 64-bit flag word. */
  flags  = (uint64_t)(uint32_t)actor->state->args[1];
  flags2 = (uint64_t)(uint32_t)actor->state->args[2];
  if ((actor->flags & flags) == flags &&
      (actor->flags2 & flags2) == flags2)
    P_SetMobjState(actor, state);
}

void A_AddFlags(mobj_t *actor)
{
  uint64_t flags, flags2;
  dbool update_blockmap;
  if (!mbf21_features || !actor)
    return;
  /* see A_JumpIfFlagsSet: avoid sign-extending a bit-31 flag value */
  flags  = (uint64_t)(uint32_t)actor->state->args[0];
  flags2 = (uint64_t)(uint32_t)actor->state->args[1];
  update_blockmap = ((flags & MF_NOBLOCKMAP) && !(actor->flags & MF_NOBLOCKMAP))
                 || ((flags & MF_NOSECTOR)   && !(actor->flags & MF_NOSECTOR));
  if (update_blockmap)
    P_UnsetThingPosition(actor);
  actor->flags  |= flags;
  actor->flags2 |= flags2;
  if (update_blockmap)
    P_SetThingPosition(actor);
}

void A_RemoveFlags(mobj_t *actor)
{
  uint64_t flags, flags2;
  dbool update_blockmap;
  if (!mbf21_features || !actor)
    return;
  /* see A_JumpIfFlagsSet: avoid sign-extending a bit-31 flag value */
  flags  = (uint64_t)(uint32_t)actor->state->args[0];
  flags2 = (uint64_t)(uint32_t)actor->state->args[1];
  update_blockmap = ((flags & MF_NOBLOCKMAP) && (actor->flags & MF_NOBLOCKMAP))
                 || ((flags & MF_NOSECTOR)   && (actor->flags & MF_NOSECTOR));
  if (update_blockmap)
    P_UnsetThingPosition(actor);
  actor->flags  &= ~flags;
  actor->flags2 &= ~flags2;
  if (update_blockmap)
    P_SetThingPosition(actor);
}

/* ------------------------------------------------------------------------
 * Hexen monster codepointers (active port).
 *
 * Ported from the dormant vanilla-Hexen actor code and adapted to this
 * core's API.  These let Hexen enemies actually attack; the look/chase/
 * face/pain/death pointers they share with the Doom/Heretic AI are already
 * implemented.  Started with the Ettin (the first enemy encountered).
 * --------------------------------------------------------------------- */

#define HX_HITDICE(a) ((1 + (P_Random(pr_heretic) & 7)) * (a))

int P_SubRandom(void);  /* heretic/p_action.c */

/* Hexen breakable pottery.  ZPottery decorations are shootable; on death
 * they run A_PotteryExplode, flinging a handful of pottery-bit gibs.  Each
 * bit picks a random resting sprite (A_PotteryChooseBit) and lingers until
 * a player looks at it, then crumbles (A_PotteryCheck). */
void A_PotteryExplode(mobj_t *actor)
{
  mobj_t *mo = NULL;
  int     i;

  for (i = (P_Random(pr_heretic) & 3) + 3; i; i--)
  {
    mo = P_SpawnMobj(actor->x, actor->y, actor->z, HEXEN_MT_POTTERYBIT1);
    if (!mo)
      continue;
    P_SetMobjState(mo, mo->info->spawnstate + (P_Random(pr_heretic) % 5));
    mo->momz = ((P_Random(pr_heretic) & 7) + 5) * (3 * FRACUNIT / 4);
    mo->momx = P_SubRandom() << (FRACBITS - 6);
    mo->momy = P_SubRandom() << (FRACBITS - 6);
  }
  if (mo)
    S_StartSound(mo, hexen_sfx_pottery_explode);
  /* (Pots scripted to drop an item on break need the ACS thing-spawn args,
   * which aren't wired up here; the plain shatter is unaffected.) */
  P_RemoveMobj(actor);
}

void A_PotteryChooseBit(mobj_t *actor)
{
  P_SetMobjState(actor, actor->info->deathstate + (P_Random(pr_heretic) % 5) + 1);
  actor->tics = 256 + (P_Random(pr_heretic) << 1);
}

void A_PotteryCheck(mobj_t *actor)
{
  mobj_t *pmo;

  if (netgame)
    return;
  pmo = players[consoleplayer].mo;
  if (pmo && P_CheckSight(actor, pmo) &&
      (abs((int)R_PointToAngle2(pmo->x, pmo->y, actor->x, actor->y)
           - (int)pmo->angle) <= ANG45))
  {
    /* a player is looking at the bit: back up one state (the waiting frame) */
    P_SetMobjState(actor, (statenum_t)(actor->state - &states[0] - 1));
  }
}

/* Hexen tree leaf-spawner.  ZLeafSpawner things (the trees) periodically
 * fling a few leaf sprites out on the wind; each leaf drifts, occasionally
 * gets another upward gust (A_LeafThrust), and fades out (A_LeafCheck). */
void A_LeafSpawn(mobj_t *actor)
{
  mobj_t *mo;
  int     i;

  for (i = (P_Random(pr_heretic) & 3) + 1; i; i--)
  {
    fixed_t x = actor->x + (P_SubRandom() << 14);
    fixed_t y = actor->y + (P_SubRandom() << 14);
    fixed_t z = actor->z + (P_Random(pr_heretic) << 14);
    mobjtype_t type = HEXEN_MT_LEAF1 + (P_Random(pr_heretic) & 1);

    mo = P_SpawnMobj(x, y, z, type);
    if (mo)
    {
      P_ThrustMobj(mo, actor->angle, (P_Random(pr_heretic) << 9) + 3 * FRACUNIT);
      P_SetTarget(&mo->target, actor);
      mo->special1.i = 0;
    }
  }
}

void A_LeafThrust(mobj_t *actor)
{
  if (P_Random(pr_heretic) > 96)
    return;
  actor->momz += (P_Random(pr_heretic) << 9) + FRACUNIT;
}

void A_LeafCheck(mobj_t *actor)
{
  actor->special1.i++;
  if (actor->special1.i >= 20)
  {
    P_SetMobjState(actor, HEXEN_S_NULL);
    return;
  }
  if (P_Random(pr_heretic) > 64)
  {
    if (!actor->momx && !actor->momy && actor->target)
      P_ThrustMobj(actor, actor->target->angle,
                   (P_Random(pr_heretic) << 9) + FRACUNIT);
    return;
  }
  P_SetMobjState(actor, HEXEN_S_LEAF1_8);
  actor->momz = (P_Random(pr_heretic) << 9) + FRACUNIT;
  if (actor->target)
    P_ThrustMobj(actor, actor->target->angle,
                 (P_Random(pr_heretic) << 9) + 2 * FRACUNIT);
  actor->flags |= MF_MISSILE;
}

void A_EttinAttack(mobj_t *actor)
{
  if (!actor->target)
    return;
  if (P_CheckMeleeRange(actor))
    P_DamageMobj(actor->target, actor, actor, HX_HITDICE(2));
}

/* Hexen: the Centaur raises its shield when hurt, becoming briefly
 * invulnerable and reflecting missiles, then either lowers it or lunges
 * into melee.  A_SetReflective also makes the Centaur invulnerable; the
 * generic forms are shared with other reflective Hexen actors. */
void A_SetInvulnerable(mobj_t *actor)
{
  actor->flags2 |= MF2_INVULNERABLE;
}

void A_UnSetInvulnerable(mobj_t *actor)
{
  actor->flags2 &= ~MF2_INVULNERABLE;
}

void A_SetReflective(mobj_t *actor)
{
  actor->flags2 |= MF2_REFLECTIVE;
  if (actor->type == HEXEN_MT_CENTAUR || actor->type == HEXEN_MT_CENTAURLEADER)
    A_SetInvulnerable(actor);
}

void A_UnSetReflective(mobj_t *actor)
{
  actor->flags2 &= ~MF2_REFLECTIVE;
  if (actor->type == HEXEN_MT_CENTAUR || actor->type == HEXEN_MT_CENTAURLEADER)
    A_UnSetInvulnerable(actor);
}

void A_CentaurDefend(mobj_t *actor)
{
  A_FaceTarget(actor);
  if (P_CheckMeleeRange(actor) && P_Random(pr_heretic) < 32)
  {
    A_UnSetInvulnerable(actor);
    P_SetMobjState(actor, actor->info->meleestate);
  }
}

void A_CentaurAttack(mobj_t *actor)
{
  if (!actor->target)
    return;
  if (P_CheckMeleeRange(actor))
    P_DamageMobj(actor->target, actor, actor, P_Random(pr_heretic) % 7 + 3);
}

void A_WraithMelee(mobj_t *actor)
{
  int amount;

  if (!actor->target)
    return;
  /* steal health from the target */
  if (P_CheckMeleeRange(actor) && (P_Random(pr_heretic) < 220))
  {
    amount = HX_HITDICE(2);
    P_DamageMobj(actor->target, actor, actor, amount);
    actor->health += amount;
  }
}

void A_WraithMissile(mobj_t *actor)
{
  mobj_t *mo;

  if (!actor->target)
    return;
  mo = P_SpawnMissile(actor, actor->target, HEXEN_MT_WRAITHFX1);
  if (mo)
    S_StartSound(actor, hexen_sfx_wraith_missile_fire);
}

void A_DemonAttack1(mobj_t *actor)
{
  if (!actor->target)
    return;
  if (P_CheckMeleeRange(actor))
    P_DamageMobj(actor->target, actor, actor, HX_HITDICE(2));
}

void A_DemonAttack2(mobj_t *actor)
{
  mobj_t *mo;
  int     fireBall;

  if (!actor->target)
    return;
  if (actor->type == HEXEN_MT_DEMON)
    fireBall = HEXEN_MT_DEMONFX1;
  else
    fireBall = HEXEN_MT_DEMON2FX1;
  mo = P_SpawnMissile(actor, actor->target, fireBall);
  if (mo)
  {
    mo->z += 30 * FRACUNIT;
    S_StartSound(actor, hexen_sfx_demon_missile_fire);
  }
}

/* ------------------------------------------------------------------------
 * Serpent (HEXEN_MT_SERPENT / HEXEN_MT_SERPENTLEADER)
 *
 * The burrowing snake: it swims hidden beneath a liquid floor, humps up to
 * the surface to strike, melees with its head, and (the "leader" variant)
 * spits a missile.  It is constrained to its starting floor terrain -- a
 * move that would change the floor flat is reverted -- so it never leaves
 * the water/lava/sludge it spawned in.  On death the head detaches.
 *
 * Adapted to this core: pr_hexen -> pr_heretic, S_StartMobjSound ->
 * S_StartSound, HITDICE -> HX_HITDICE, the 4-arg P_TryMove, and the
 * fork's fast-monster test (nightmare skill or -fast).
 * --------------------------------------------------------------------- */

#define SERPENT_FAST (gameskill == sk_nightmare || fastparm)

static dbool P_CheckMeleeRange2(mobj_t *actor)
{
  mobj_t *mo;
  fixed_t dist;

  if (!actor->target)
    return false;
  mo = actor->target;
  dist = P_AproxDistance(mo->x - actor->x, mo->y - actor->y);
  if (dist >= MELEERANGE * 2 || dist < MELEERANGE)
    return false;
  if (!P_CheckSight(actor, mo))
    return false;
  if (mo->z > actor->z + actor->height)
    return false;               /* target is higher */
  if (actor->z > mo->z + mo->height)
    return false;               /* attacker is higher */
  return true;
}

void A_SerpentChase(mobj_t *actor)
{
  int delta;
  int oldX, oldY, oldFloor;

  if (actor->reactiontime)
    actor->reactiontime--;
  if (actor->threshold)
    actor->threshold--;

  if (SERPENT_FAST)
  {
    actor->tics -= actor->tics / 2;
    if (actor->tics < 3)
      actor->tics = 3;
  }

  /* turn towards movement direction if not there yet */
  if (actor->movedir < 8)
  {
    actor->angle &= (7 << 29);
    delta = actor->angle - (actor->movedir << 29);
    if (delta > 0)
      actor->angle -= ANG90 / 2;
    else if (delta < 0)
      actor->angle += ANG90 / 2;
  }

  if (!actor->target || !(actor->target->flags & MF_SHOOTABLE))
  {
    if (P_LookForPlayers(actor, true))
      return;
    P_SetMobjState(actor, actor->info->spawnstate);
    return;
  }

  /* don't attack twice in a row */
  if (actor->flags & MF_JUSTATTACKED)
  {
    actor->flags &= ~MF_JUSTATTACKED;
    if (!SERPENT_FAST)
      P_NewChaseDir(actor);
    return;
  }

  /* check for melee attack */
  if (actor->info->meleestate && P_CheckMeleeRange(actor))
  {
    if (actor->info->attacksound)
      S_StartSound(actor, actor->info->attacksound);
    P_SetMobjState(actor, actor->info->meleestate);
    return;
  }

  if (netgame && !actor->threshold && !P_CheckSight(actor, actor->target))
  {
    if (P_LookForPlayers(actor, true))
      return;
  }

  /* chase towards player, but stay on the same floor terrain */
  oldX = actor->x;
  oldY = actor->y;
  oldFloor = actor->subsector->sector->floorpic;
  if (--actor->movecount < 0 || !P_Move(actor, false))
    P_NewChaseDir(actor);
  if (actor->subsector->sector->floorpic != oldFloor)
  {
    P_TryMove(actor, oldX, oldY, 0);
    P_NewChaseDir(actor);
  }

  if (actor->info->activesound && P_Random(pr_heretic) < 3)
    S_StartSound(actor, actor->info->activesound);
}

void A_SerpentWalk(mobj_t *actor)
{
  int delta;

  if (actor->reactiontime)
    actor->reactiontime--;
  if (actor->threshold)
    actor->threshold--;

  if (SERPENT_FAST)
  {
    actor->tics -= actor->tics / 2;
    if (actor->tics < 3)
      actor->tics = 3;
  }

  if (actor->movedir < 8)
  {
    actor->angle &= (7 << 29);
    delta = actor->angle - (actor->movedir << 29);
    if (delta > 0)
      actor->angle -= ANG90 / 2;
    else if (delta < 0)
      actor->angle += ANG90 / 2;
  }

  if (!actor->target || !(actor->target->flags & MF_SHOOTABLE))
  {
    if (P_LookForPlayers(actor, true))
      return;
    P_SetMobjState(actor, actor->info->spawnstate);
    return;
  }

  if (actor->flags & MF_JUSTATTACKED)
  {
    actor->flags &= ~MF_JUSTATTACKED;
    if (!SERPENT_FAST)
      P_NewChaseDir(actor);
    return;
  }

  if (actor->info->meleestate && P_CheckMeleeRange(actor))
  {
    if (actor->info->attacksound)
      S_StartSound(actor, actor->info->attacksound);
    P_SetMobjState(actor, HEXEN_S_SERPENT_ATK1);
    return;
  }

  if (netgame && !actor->threshold && !P_CheckSight(actor, actor->target))
  {
    if (P_LookForPlayers(actor, true))
      return;
  }

  if (--actor->movecount < 0 || !P_Move(actor, false))
    P_NewChaseDir(actor);
}

void A_SerpentHumpDecide(mobj_t *actor)
{
  if (actor->type == HEXEN_MT_SERPENTLEADER)
  {
    if (P_Random(pr_heretic) > 30)
      return;
    else if (P_Random(pr_heretic) < 40)
    {
      P_SetMobjState(actor, HEXEN_S_SERPENT_SURFACE1);
      return;
    }
  }
  else if (P_Random(pr_heretic) > 3)
    return;

  if (!P_CheckMeleeRange(actor))
  {
    if (actor->type == HEXEN_MT_SERPENTLEADER && P_Random(pr_heretic) < 128)
    {
      P_SetMobjState(actor, HEXEN_S_SERPENT_SURFACE1);
    }
    else
    {
      P_SetMobjState(actor, HEXEN_S_SERPENT_HUMP1);
      S_StartSound(actor, hexen_sfx_serpent_active);
    }
  }
}

void A_SerpentCheckForAttack(mobj_t *actor)
{
  if (!actor->target)
    return;
  if (actor->type == HEXEN_MT_SERPENTLEADER)
  {
    if (!P_CheckMeleeRange(actor))
    {
      P_SetMobjState(actor, HEXEN_S_SERPENT_ATK1);
      return;
    }
  }
  if (P_CheckMeleeRange2(actor))
  {
    P_SetMobjState(actor, HEXEN_S_SERPENT_WALK1);
  }
  else if (P_CheckMeleeRange(actor))
  {
    if (P_Random(pr_heretic) < 32)
      P_SetMobjState(actor, HEXEN_S_SERPENT_WALK1);
    else
      P_SetMobjState(actor, HEXEN_S_SERPENT_ATK1);
  }
}

void A_SerpentChooseAttack(mobj_t *actor)
{
  if (!actor->target || P_CheckMeleeRange(actor))
    return;
  if (actor->type == HEXEN_MT_SERPENTLEADER)
    P_SetMobjState(actor, HEXEN_S_SERPENT_MISSILE1);
}

void A_SerpentMeleeAttack(mobj_t *actor)
{
  if (!actor->target)
    return;
  if (P_CheckMeleeRange(actor))
  {
    P_DamageMobj(actor->target, actor, actor, HX_HITDICE(5));
    S_StartSound(actor, hexen_sfx_serpent_meleehit);
  }
  if (P_Random(pr_heretic) < 96)
    A_SerpentCheckForAttack(actor);
}

void A_SerpentMissileAttack(mobj_t *actor)
{
  if (!actor->target)
    return;
  P_SpawnMissile(actor, actor->target, HEXEN_MT_SERPENTFX);
}

void A_SerpentHide(mobj_t *actor)
{
  actor->flags2 |= MF2_DONTDRAW;
  actor->floorclip = 0;
}

void A_SerpentUnHide(mobj_t *actor)
{
  actor->flags2 &= ~MF2_DONTDRAW;
  actor->floorclip = 24 * FRACUNIT;
}

void A_SerpentRaiseHump(mobj_t *actor)
{
  actor->floorclip -= 4 * FRACUNIT;
}

void A_SerpentLowerHump(mobj_t *actor)
{
  actor->floorclip += 4 * FRACUNIT;
}

void A_SerpentBirthScream(mobj_t *actor)
{
  S_StartSound(actor, hexen_sfx_serpent_birth);
}

void A_SerpentDiveSound(mobj_t *actor)
{
  S_StartSound(actor, hexen_sfx_serpent_active);
}

void A_SerpentHeadPop(mobj_t *actor)
{
  P_SpawnMobj(actor->x, actor->y, actor->z + 45 * FRACUNIT,
              HEXEN_MT_SERPENT_HEAD);
}

void A_SerpentHeadCheck(mobj_t *actor)
{
  if (actor->z <= actor->floorz)
  {
    if (P_GetThingFloorType(actor) >= FLOOR_LIQUID)
    {
      P_HitFloor(actor);
      P_SetMobjState(actor, HEXEN_S_NULL);
    }
    else
    {
      P_SetMobjState(actor, HEXEN_S_SERPENT_HEAD_X1);
    }
  }
}

void A_SerpentSpawnGibs(mobj_t *actor)
{
  mobj_t *mo;
  int     r1, r2;
  int     i;
  static const int gib[3] =
    { HEXEN_MT_SERPENT_GIB1, HEXEN_MT_SERPENT_GIB2, HEXEN_MT_SERPENT_GIB3 };

  for (i = 0; i < 3; i++)
  {
    r1 = P_Random(pr_heretic);
    r2 = P_Random(pr_heretic);
    mo = P_SpawnMobj(actor->x + ((r2 - 128) << 12),
                     actor->y + ((r1 - 128) << 12),
                     actor->floorz + FRACUNIT, gib[i]);
    if (mo)
    {
      mo->momx = (P_Random(pr_heretic) - 128) << 6;
      mo->momy = (P_Random(pr_heretic) - 128) << 6;
      mo->floorclip = 6 * FRACUNIT;
    }
  }
}

/* ------------------------------------------------------------------------
 * Fire Demon / Afrit (HEXEN_MT_FIREDEMON)
 *
 * A floating monster that bobs up and down, strafes around its target,
 * sheds a shower of bouncing fire rocks, and lobs a fireball.  Uses a
 * custom float/strafe chase rather than the standard ground AI.
 *
 * Adapted to this core: pr_hexen -> pr_heretic, S_StartMobjSound ->
 * S_StartSound.  FaceMovementDirection and FIREDEMON_ATTACK_RANGE are
 * provided locally; FloatBobOffsets, P_SetTarget, P_CheckMissileRange and
 * the chase helpers already exist.
 * --------------------------------------------------------------------- */

extern fixed_t FloatBobOffsets[64];
#define FIREDEMON_ATTACK_RANGE (64 * 8 * FRACUNIT)

static void FaceMovementDirection(mobj_t *actor)
{
  switch (actor->movedir)
  {
    case DI_EAST:      actor->angle = 0   << 24; break;
    case DI_NORTHEAST: actor->angle = 32  << 24; break;
    case DI_NORTH:     actor->angle = 64  << 24; break;
    case DI_NORTHWEST: actor->angle = 96  << 24; break;
    case DI_WEST:      actor->angle = 128 << 24; break;
    case DI_SOUTHWEST: actor->angle = 160 << 24; break;
    case DI_SOUTH:     actor->angle = 192 << 24; break;
    case DI_SOUTHEAST: actor->angle = 224 << 24; break;
    default: break;
  }
}

void A_FiredSpawnRock(mobj_t *actor)
{
  mobj_t *mo;
  int     x, y, z;
  int     rtype = 0;

  switch (P_Random(pr_heretic) % 5)
  {
    case 0: rtype = HEXEN_MT_FIREDEMON_FX1; break;
    case 1: rtype = HEXEN_MT_FIREDEMON_FX2; break;
    case 2: rtype = HEXEN_MT_FIREDEMON_FX3; break;
    case 3: rtype = HEXEN_MT_FIREDEMON_FX4; break;
    case 4: rtype = HEXEN_MT_FIREDEMON_FX5; break;
  }

  x = actor->x + ((P_Random(pr_heretic) - 128) << 12);
  y = actor->y + ((P_Random(pr_heretic) - 128) << 12);
  z = actor->z + ((P_Random(pr_heretic)) << 11);
  mo = P_SpawnMobj(x, y, z, rtype);
  if (mo)
  {
    P_SetTarget(&mo->target, actor);
    mo->momx = (P_Random(pr_heretic) - 128) << 10;
    mo->momy = (P_Random(pr_heretic) - 128) << 10;
    mo->momz = (P_Random(pr_heretic) << 10);
    mo->special1.i = 2;        /* number of bounces */
  }

  actor->special2.i = 0;
  actor->flags &= ~MF_JUSTATTACKED;
}

void A_FiredRocks(mobj_t *actor)
{
  A_FiredSpawnRock(actor);
  A_FiredSpawnRock(actor);
  A_FiredSpawnRock(actor);
  A_FiredSpawnRock(actor);
  A_FiredSpawnRock(actor);
}

void A_FiredChase(mobj_t *actor)
{
  int      weaveindex = actor->special1.i;
  mobj_t  *target = actor->target;
  angle_t  ang;
  fixed_t  dist;

  if (actor->reactiontime)
    actor->reactiontime--;
  if (actor->threshold)
    actor->threshold--;

  /* float up and down */
  actor->z += FloatBobOffsets[weaveindex];
  actor->special1.i = (weaveindex + 2) & 63;

  /* keep above a minimum height */
  if (actor->z < actor->floorz + (64 * FRACUNIT))
    actor->z += 2 * FRACUNIT;

  if (!actor->target || !(actor->target->flags & MF_SHOOTABLE))
  {
    P_LookForPlayers(actor, true);
    return;
  }

  /* strafe */
  if (actor->special2.i > 0)
  {
    actor->special2.i--;
  }
  else
  {
    actor->special2.i = 0;
    actor->momx = actor->momy = 0;
    dist = P_AproxDistance(actor->x - target->x, actor->y - target->y);
    if (dist < FIREDEMON_ATTACK_RANGE)
    {
      if (P_Random(pr_heretic) < 30)
      {
        ang = R_PointToAngle2(actor->x, actor->y, target->x, target->y);
        if (P_Random(pr_heretic) < 128)
          ang += ANG90;
        else
          ang -= ANG90;
        ang >>= ANGLETOFINESHIFT;
        actor->momx = FixedMul(8 * FRACUNIT, finecosine[ang]);
        actor->momy = FixedMul(8 * FRACUNIT, finesine[ang]);
        actor->special2.i = 3;  /* strafe time */
      }
    }
  }

  FaceMovementDirection(actor);

  /* normal movement */
  if (!actor->special2.i)
  {
    if (--actor->movecount < 0 || !P_Move(actor, false))
      P_NewChaseDir(actor);
  }

  /* missile attack */
  if (!(actor->flags & MF_JUSTATTACKED))
  {
    if (P_CheckMissileRange(actor) && (P_Random(pr_heretic) < 20))
    {
      P_SetMobjState(actor, actor->info->missilestate);
      actor->flags |= MF_JUSTATTACKED;
      return;
    }
  }
  else
  {
    actor->flags &= ~MF_JUSTATTACKED;
  }

  if (actor->info->activesound && P_Random(pr_heretic) < 3)
    S_StartSound(actor, actor->info->activesound);
}

void A_FiredAttack(mobj_t *actor)
{
  mobj_t *mo;

  if (!actor->target)
    return;
  mo = P_SpawnMissile(actor, actor->target, HEXEN_MT_FIREDEMON_FX6);
  if (mo)
    S_StartSound(actor, hexen_sfx_fired_attack);
}

void A_FiredSplotch(mobj_t *actor)
{
  mobj_t *mo;

  mo = P_SpawnMobj(actor->x, actor->y, actor->z, HEXEN_MT_FIREDEMON_SPLOTCH1);
  if (mo)
  {
    mo->momx = (P_Random(pr_heretic) - 128) << 11;
    mo->momy = (P_Random(pr_heretic) - 128) << 11;
    mo->momz = FRACUNIT * 3 + (P_Random(pr_heretic) << 10);
  }
  mo = P_SpawnMobj(actor->x, actor->y, actor->z, HEXEN_MT_FIREDEMON_SPLOTCH2);
  if (mo)
  {
    mo->momx = (P_Random(pr_heretic) - 128) << 11;
    mo->momy = (P_Random(pr_heretic) - 128) << 11;
    mo->momz = FRACUNIT * 3 + (P_Random(pr_heretic) << 10);
  }
}

int P_SubRandom(void);  /* heretic/p_action.c */

/* --------------------------------------------------------------------------
 * Bishop -- a floating caster that bobs in the air, fires homing missiles
 * (HEXEN_MT_BISH_FX, which seek and weave), and teleport-blurs around when
 * pressed.  dsda-doom p_enemy.c.
 * ------------------------------------------------------------------------ */
void A_BishopAttack(mobj_t *actor)
{
  if (!actor->target)
    return;
  S_StartSound(actor, actor->info->attacksound);
  if (P_CheckMeleeRange(actor))
  {
    P_DamageMobj(actor->target, actor, actor, HX_HITDICE(4));
    return;
  }
  actor->special1.i = (P_Random(pr_heretic) & 3) + 5;   /* burst count */
}

void A_BishopAttack2(mobj_t *actor)
{
  mobj_t *mo;

  if (!actor->target || !actor->special1.i)
  {
    actor->special1.i = 0;
    P_SetMobjState(actor, HEXEN_S_BISHOP_WALK1);
    return;
  }
  mo = P_SpawnMissile(actor, actor->target, HEXEN_MT_BISH_FX);
  if (mo)
  {
    P_SetTarget(&mo->special1.m, actor->target);
    mo->special2.i = 16;          /* high word x/y weave, low word z */
  }
  actor->special1.i--;
}

void A_BishopChase(mobj_t *actor)
{
  actor->z -= FloatBobOffsets[actor->special2.i] >> 1;
  actor->special2.i = (actor->special2.i + 4) & 63;
  actor->z += FloatBobOffsets[actor->special2.i] >> 1;
}

void A_BishopDecide(mobj_t *actor)
{
  if (P_Random(pr_heretic) < 220)
    return;
  P_SetMobjState(actor, HEXEN_S_BISHOP_BLUR1);
}

void A_BishopDoBlur(mobj_t *actor)
{
  actor->special1.i = (P_Random(pr_heretic) & 3) + 3;   /* number of blurs */
  if (P_Random(pr_heretic) < 120)
    P_ThrustMobj(actor, actor->angle + ANG90, 11 * FRACUNIT);
  else if (P_Random(pr_heretic) > 125)
    P_ThrustMobj(actor, actor->angle - ANG90, 11 * FRACUNIT);
  else
    P_ThrustMobj(actor, actor->angle, 11 * FRACUNIT);    /* forward */
  S_StartSound(actor, hexen_sfx_bishop_blur);
}

void A_BishopSpawnBlur(mobj_t *actor)
{
  mobj_t *mo;

  if (!--actor->special1.i)
  {
    actor->momx = 0;
    actor->momy = 0;
    if (P_Random(pr_heretic) > 96)
      P_SetMobjState(actor, HEXEN_S_BISHOP_WALK1);
    else
      P_SetMobjState(actor, HEXEN_S_BISHOP_ATK1);
  }
  mo = P_SpawnMobj(actor->x, actor->y, actor->z, HEXEN_MT_BISHOPBLUR);
  if (mo)
    mo->angle = actor->angle;
}

void A_BishopPuff(mobj_t *actor)
{
  mobj_t *mo;

  mo = P_SpawnMobj(actor->x, actor->y, actor->z + 40 * FRACUNIT,
                   HEXEN_MT_BISHOP_PUFF);
  if (mo)
    mo->momz = FRACUNIT / 2;
}

void A_BishopPainBlur(mobj_t *actor)
{
  mobj_t *mo;
  int     r1, r2, r3;

  if (P_Random(pr_heretic) < 64)
  {
    P_SetMobjState(actor, HEXEN_S_BISHOP_BLUR1);
    return;
  }
  r1 = P_SubRandom();
  r2 = P_SubRandom();
  r3 = P_SubRandom();
  mo = P_SpawnMobj(actor->x + (r3 << 12), actor->y + (r2 << 12),
                   actor->z + (r1 << 11), HEXEN_MT_BISHOPPAINBLUR);
  if (mo)
    mo->angle = actor->angle;
}

void A_BishopMissileSeek(mobj_t *actor)
{
  /* Use the file-local 4-arg seeker (aims at target centre, equivalent to
   * the 5-arg variant with seekcenter); the global 5-arg P_SeekerMissile is
   * shadowed by the static one in this translation unit. */
  P_SeekerMissile(actor, &actor->special1.m, ANG1 * 2, ANG1 * 3);
}

void A_BishopMissileWeave(mobj_t *actor)
{
  fixed_t newX, newY;
  int     weaveXY, weaveZ;
  int     angle;

  weaveXY = actor->special2.i >> 16;
  weaveZ = actor->special2.i & 0xFFFF;
  angle = (actor->angle + ANG90) >> ANGLETOFINESHIFT;
  newX = actor->x - FixedMul(finecosine[angle], FloatBobOffsets[weaveXY] << 1);
  newY = actor->y - FixedMul(finesine[angle], FloatBobOffsets[weaveXY] << 1);
  weaveXY = (weaveXY + 2) & 63;
  newX += FixedMul(finecosine[angle], FloatBobOffsets[weaveXY] << 1);
  newY += FixedMul(finesine[angle], FloatBobOffsets[weaveXY] << 1);
  P_TryMove(actor, newX, newY, 0);
  actor->z -= FloatBobOffsets[weaveZ];
  weaveZ = (weaveZ + 2) & 63;
  actor->z += FloatBobOffsets[weaveZ];
  actor->special2.i = weaveZ + (weaveXY << 16);
}

/* --------------------------------------------------------------------------
 * Hexen Minotaur (Dark Servant) lifetime behaviour.  The combat codepointers
 * (A_MinotaurAtk1/2/3, A_MinotaurDecide, A_MinotaurCharge, A_MntrFloorFire)
 * are shared with the Heretic Maulotaur and live in heretic/p_action.c, now
 * game-aware.  The functions below are Hexen-only: the summoned minotaur
 * fades in on spawn, hunts the nearest valid target (attacking monsters when
 * it has a player master), roams when idle, and stomps itself after
 * MAULATORTICS of life.  Ported from dsda-doom p_enemy.c.
 * ------------------------------------------------------------------------ */
#define MINOTAUR_LOOK_DIST (16 * 54 * FRACUNIT)

dbool P_SetMobjStateNF(mobj_t *mobj, statenum_t state);  /* heretic/p_action.c */
dbool P_TestMobjLocation(mobj_t *mobj);                  /* heretic/p_action.c */
dbool P_GivePower(player_t *player, int power);          /* p_inter.c */
void  A_MinotaurLook(mobj_t *actor);

void A_MinotaurFade0(mobj_t *actor)
{
  actor->flags &= ~MF_ALTSHADOW;
  actor->flags |= MF_SHADOW;
}

void A_MinotaurFade1(mobj_t *actor)
{
  actor->flags &= ~MF_SHADOW;
  actor->flags |= MF_ALTSHADOW;
}

void A_MinotaurFade2(mobj_t *actor)
{
  actor->flags &= ~MF_SHADOW;
  actor->flags &= ~MF_ALTSHADOW;
}

/* The summoned Minotaur self-destructs after MAULATORTICS.  special_args[0]
 * holds the leveltime at which it was spawned (set by the Dark Servant
 * summon when that artifact lands); a map-placed minotaur with a zero start
 * simply ages from level start.  Returns false if the actor was killed. */
static dbool CheckMinotaurAge(mobj_t *mo)
{
  if ((unsigned)(leveltime - mo->special_args[0]) >= (unsigned)MAULATORTICS)
  {
    P_DamageMobj(mo, NULL, NULL, 10000);
    return FALSE;
  }
  return TRUE;
}

void A_MinotaurRoam(mobj_t *actor)
{
  actor->flags &= ~MF_SHADOW;     /* in case pain skipped his fade-in */
  actor->flags &= ~MF_ALTSHADOW;

  if (!CheckMinotaurAge(actor))
    return;

  if (P_Random(pr_heretic) < 30)
    A_MinotaurLook(actor);        /* adjust to closest target */

  if (P_Random(pr_heretic) < 6)
  {
    actor->movedir = P_Random(pr_heretic) % 8;
    FaceMovementDirection(actor);
  }
  if (!P_Move(actor, false))
  {
    if (P_Random(pr_heretic) & 1)
      actor->movedir = (actor->movedir + 1) % 8;
    else
      actor->movedir = (actor->movedir + 7) % 8;
    FaceMovementDirection(actor);
  }
}

void A_MinotaurLook(mobj_t *actor)
{
  mobj_t    *mo = NULL;
  player_t  *player;
  thinker_t *think;
  fixed_t    dist;
  int        i;
  mobj_t    *master = actor->special1.m;

  P_SetTarget(&actor->target, NULL);
  if (deathmatch)                 /* quick search for players */
  {
    for (i = 0; i < MAXPLAYERS; i++)
    {
      if (!playeringame[i])
        continue;
      player = &players[i];
      mo = player->mo;
      if (mo == master)
        continue;
      if (mo->health <= 0)
        continue;
      dist = P_AproxDistance(actor->x - mo->x, actor->y - mo->y);
      if (dist > MINOTAUR_LOOK_DIST)
        continue;
      P_SetTarget(&actor->target, mo);
      break;
    }
  }

  if (!actor->target)             /* near-player monster search */
  {
    if (master && (master->health > 0) && (master->player))
      mo = P_RoughTargetSearch(master, 0, 20);
    else
      mo = P_RoughTargetSearch(actor, 0, 20);
    P_SetTarget(&actor->target, mo);
  }

  if (!actor->target)             /* normal monster search */
  {
    for (think = thinkercap.next; think != &thinkercap; think = think->next)
    {
      if (think->function.arg1 != (void (*)(void *))P_MobjThinker)
        continue;
      mo = (mobj_t *) think;
      if (!(mo->flags & MF_COUNTKILL))
        continue;
      if (mo->health <= 0)
        continue;
      if (!(mo->flags & MF_SHOOTABLE))
        continue;
      dist = P_AproxDistance(actor->x - mo->x, actor->y - mo->y);
      if (dist > MINOTAUR_LOOK_DIST)
        continue;
      if ((mo == master) || (mo == actor))
        continue;
      if ((mo->type == HEXEN_MT_MINOTAUR)
          && (mo->special1.m == actor->special1.m))
        continue;
      P_SetTarget(&actor->target, mo);
      break;                      /* found mobj to attack */
    }
  }

  if (actor->target)
    P_SetMobjStateNF(actor, HEXEN_S_MNTR_WALK1);
  else
    P_SetMobjStateNF(actor, HEXEN_S_MNTR_ROAM1);
}

void A_MinotaurChase(mobj_t *actor)
{
  actor->flags &= ~MF_SHADOW;     /* in case pain skipped his fade-in */
  actor->flags &= ~MF_ALTSHADOW;

  if (!CheckMinotaurAge(actor))
    return;

  if (P_Random(pr_heretic) < 30)
    A_MinotaurLook(actor);        /* adjust to closest target */

  if (!actor->target || (actor->target->health <= 0)
      || !(actor->target->flags & MF_SHOOTABLE))
  {                               /* look for a new target */
    P_SetMobjState(actor, HEXEN_S_MNTR_LOOK1);
    return;
  }

  FaceMovementDirection(actor);
  actor->reactiontime = 0;

  if (actor->info->meleestate && P_CheckMeleeRange(actor))
  {
    if (actor->info->attacksound)
      S_StartSound(actor, actor->info->attacksound);
    P_SetMobjState(actor, actor->info->meleestate);
    return;
  }

  if (actor->info->missilestate && P_CheckMissileRange(actor))
  {
    P_SetMobjState(actor, actor->info->missilestate);
    return;
  }

  if (!P_Move(actor, false))
    P_NewChaseDir(actor);

  if (actor->info->activesound && P_Random(pr_heretic) < 6)
    S_StartSound(actor, actor->info->activesound);
}

void A_SmokePuffExit(mobj_t *actor)
{
  P_SpawnMobj(actor->x, actor->y, actor->z, HEXEN_MT_MNTRSMOKEEXIT);
}

/* A_Summon -- the Dark Servant's HEXEN_MT_SUMMON_FX missile runs this on
 * impact: spawn a Minotaur bound to the summoning player as its master, with
 * the spawn time recorded so CheckMinotaurAge can stomp it after its life
 * expires.  If the minotaur doesn't fit, drop the artifact back instead. */
void A_Summon(mobj_t *actor)
{
  mobj_t *mo;
  mobj_t *master;

  mo = P_SpawnMobj(actor->x, actor->y, actor->z, HEXEN_MT_MINOTAUR);
  if (!mo)
    return;

  if (P_TestMobjLocation(mo) == false || !actor->special1.m)
  {                             /* didn't fit - revert to the artifact */
    P_SetMobjState(mo, HEXEN_S_NULL);
    mo = P_SpawnMobj(actor->x, actor->y, actor->z, HEXEN_MT_SUMMONMAULATOR);
    if (mo)
      mo->flags |= MF_DROPPED;
    return;
  }

  /* special_args is int[] in this fork, so the spawn time goes straight into
   * slot 0 (dsda byte-packs it across four slots); CheckMinotaurAge reads
   * special_args[0] the same way. */
  mo->special_args[0] = leveltime;
  master = actor->special1.m;
  if (master->flags & MF_CORPSE)
  {                             /* master already dead - no master */
    P_SetTarget(&mo->special1.m, NULL);
  }
  else
  {
    P_SetTarget(&mo->special1.m, actor->special1.m);
    if (master->player)
      P_GivePower(master->player, pw_minotaur);
  }

  P_SpawnMobj(actor->x, actor->y, actor->z, HEXEN_MT_MNTRSMOKE);
  S_StartSound(actor, hexen_sfx_maulator_active);
}
