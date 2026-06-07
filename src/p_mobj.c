/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2004 by
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
 *      Moving object handling. Spawn functions.
 *
 *-----------------------------------------------------------------------------*/

#include "doomdef.h"
#include "doomstat.h"
#include "m_random.h"
#include "r_main.h"
#include "p_maputl.h"
#include "p_slope.h"
#include "p_map.h"
#include "p_tick.h"
#include "p_spec.h"
#include "map_format.h"
#include "u_decorate.h"
#include "dsda_hacked.h"
#include "sounds.h"
#include "st_stuff.h"
#include "hu_stuff.h"
#include "s_sound.h"
#include "info.h"
#include "g_game.h"
#include "p_inter.h"
#include "p_enemy.h"
#include "lprintf.h"
#include "hexen/po_man.h"
#include "r_demo.h"
#include "u_musinfo.h"

/* Raven (Hexen/Heretic) float-bob offset table: one full sine period over
 * 64 entries, used by floating monsters (e.g. the Fire Demon) and bobbing
 * items to weave up and down. */
fixed_t FloatBobOffsets[64] =
{
       0,   51389,  102283,  152192,
  200636,  247147,  291278,  332604,
  370727,  405280,  435929,  462380,
  484378,  501712,  514213,  521763,
  524287,  521763,  514213,  501712,
  484378,  462380,  435929,  405280,
  370727,  332604,  291278,  247147,
  200636,  152192,  102283,   51389,
      -1,  -51390, -102284, -152193,
 -200637, -247148, -291279, -332605,
 -370728, -405281, -435930, -462381,
 -484380, -501713, -514215, -521764,
 -524288, -521764, -514214, -501713,
 -484379, -462381, -435930, -405280,
 -370728, -332605, -291279, -247148,
 -200637, -152193, -102284,  -51389
};

//
// P_SetMobjState
// Returns TRUE if the mobj is still present.
//

/* Persistent State (config persistent_state, General -> Miscellaneous):
 * expiring debris rests instead of being cleaned up.  Hexen's
 * stained-glass shards land in a rest state that expires after 30
 * tics; ice chunks from a freeze-death shatter melt away after a
 * randomized delay; and blood splats in all three games vanish less
 * than a second after a hit.  Pottery bits and crushed-corpse gibs,
 * by contrast, rest forever in vanilla -- this setting extends that
 * treatment to the rest of the debris.  Guarded off during demos and
 * netgames, since whether a mobj lingers is simulation state.
 *
 * Blood is unbounded -- every hitscan hit spawns a splat -- so resting
 * blood is kept in a ring whose size persistent_blood_cap selects;
 * the oldest splat is recycled when the ring is full.  The ring is
 * reset on level setup, so blood restored by a savegame or hub
 * archive is simply no longer tracked (it persists, bounded by what
 * the save contains). */
int persistent_state;
int persistent_blood_cap; /* choice index into blood_cap_values */

static const int blood_cap_values[] = {256, 512, 1024, 2048, 4096, 0};

#define BLOODQUE_MAX 4096
static mobj_t *bloodque[BLOODQUE_MAX];
static int bloodque_head, bloodque_len;

void P_ResetBloodQueue(void)
{
  bloodque_head = 0;
  bloodque_len = 0;
}

static dbool P_IsBlood(const mobj_t *mobj);

/* A queued resting splat removed by any path other than the ring's own
 * recycling -- a savegame edge, a future cleanup pass -- would leave its
 * ring slot dangling, and the eventual eviction would call P_RemoveMobj
 * on freed memory.  Clear the slot when a queued splat is removed. */
static void P_BloodQueueUnhook(mobj_t *mobj)
{
  int i;

  if (!bloodque_len || !P_IsBlood(mobj))
    return;
  for (i = 0; i < bloodque_len; i++)
  {
    int slot = (bloodque_head - 1 - i + BLOODQUE_MAX) % BLOODQUE_MAX;
    if (bloodque[slot] == mobj)
    {
      bloodque[slot] = NULL;
      return;
    }
  }
}

static dbool P_PersistentDebrisActive(void)
{
  return persistent_state && !demoplayback && !netgame;
}

static dbool P_IsRestingDebris(const mobj_t *mobj)
{
  return hexen &&
         ((mobj->type >= HEXEN_MT_SGSHARD1 &&
           mobj->type <= HEXEN_MT_SGSHARD0) ||
          mobj->type == HEXEN_MT_ICECHUNK);
}

static dbool P_IsBlood(const mobj_t *mobj)
{
  if (hexen)
    return mobj->type == HEXEN_MT_BLOOD ||
           mobj->type == HEXEN_MT_BLOODSPLATTER ||
           mobj->type == HEXEN_MT_AXEBLOOD;
  if (heretic)
    return mobj->type == HERETIC_MT_BLOOD ||
           mobj->type == HERETIC_MT_BLOODSPLATTER;
  return mobj->type == MT_BLOOD;
}

/* pin an expiring rest frame; for blood, enter it into the capped ring */
static void P_PinDebris(mobj_t *mobj)
{
  int cap;

  if (P_IsRestingDebris(mobj))
  {
    mobj->tics = -1;
    return;
  }
  if (!P_IsBlood(mobj))
    return;

  mobj->tics = -1;
  cap = blood_cap_values[persistent_blood_cap];
  if (cap > BLOODQUE_MAX)
    cap = BLOODQUE_MAX;
  if (cap > 0)
  {
    while (bloodque_len >= cap)
    {
      int tail = (bloodque_head - bloodque_len + BLOODQUE_MAX) % BLOODQUE_MAX;
      mobj_t *old = bloodque[tail];
      /* clear the slot before removing: P_RemoveMobj's unhook scan then
       * has nothing to find, and a slot already cleared by the unhook
       * (a queued splat removed by some other path) is simply skipped */
      bloodque[tail] = NULL;
      bloodque_len--;
      if (old)
        P_RemoveMobj(old);
    }
    bloodque[bloodque_head] = mobj;
    bloodque_head = (bloodque_head + 1) % BLOODQUE_MAX;
    bloodque_len++;
  }
}

dbool P_SetMobjState(mobj_t* mobj,statenum_t state)
{
  state_t*  st;

  // killough 4/9/98: remember states seen, to detect cycles:
  // DSDHacked: these are sized to the (growable) state count rather than
  // the fixed vanilla NUMSTATES.
  static statenum_t *seenstate_tab = NULL;    // fast transition table
  static int seenstate_size = 0;
  statenum_t *seenstate;                      // pointer to table
  statenum_t *tempstate = NULL;               // for use with recursion
  static int recursion;                       // detects recursion
  statenum_t i = state;                       // initial state
  dbool ret = TRUE;                         // return value

  if (seenstate_size < num_states)
    {
      seenstate_tab = realloc(seenstate_tab, num_states * sizeof(*seenstate_tab));
      memset(seenstate_tab, 0, num_states * sizeof(*seenstate_tab));
      seenstate_size = num_states;
    }
  seenstate = seenstate_tab;

  if (recursion++)                            // if recursion detected,
    {
      tempstate = calloc(num_states, sizeof(*tempstate));
      seenstate = tempstate;                  // use a private cleared table
    }

  do
    {
    if (state == S_NULL)
      {
      mobj->state = (state_t *) S_NULL;
      P_RemoveMobj (mobj);
      ret = FALSE;
      break;                 // killough 4/9/98
      }

    /* DSDHacked: nextstate comes from editable frame data and may point
     * outside the (grown) state table or be negative.  Indexing states[]
     * and seenstate[] with it would read/write out of bounds, so treat an
     * invalid state like S_NULL: stop the actor rather than crash. */
    if ((unsigned)state >= (unsigned)num_states)
      {
      mobj->state = (state_t *) S_NULL;
      P_RemoveMobj (mobj);
      ret = FALSE;
      break;
      }

    st = &states[state];
    mobj->state = st;
    mobj->tics = st->tics;
    mobj->sprite = st->sprite;
    mobj->frame = st->frame;

    // Modified handling.
    // Call action functions when the state is set

    if (st->action.arg1)
      st->action.arg1(mobj);

    /* Persistent State: a debris mobj entering a finite rest frame that
     * chains to S_NULL is about to expire -- pin it instead.  After the
     * action call, so A_IceSetTics has already re-randomized the melt
     * (and consumed its P_Random) before the pin overrides it. */
    if (mobj->state == st && mobj->tics > 0 && st->nextstate == S_NULL &&
        P_PersistentDebrisActive())
      P_PinDebris(mobj);

    seenstate[state] = 1 + st->nextstate;   // killough 4/9/98

    state = st->nextstate;
    } while (!mobj->tics && !seenstate[state]);   // killough 4/9/98

  if (ret && !mobj->tics)  // killough 4/9/98: detect state cycles
    doom_printf("Warning: State Cycle Detected");

  if (!--recursion)
    for (;(state=seenstate[i]);i=state-1)
      seenstate[i] = 0;  // killough 4/9/98: erase memory of states

  if (tempstate)
    free(tempstate);     // DSDHacked: release the recursion-private table

  return ret;
}


//
// P_ExplodeMissile
//

void P_ExplodeMissile (mobj_t* mo)
{
  /* Heretic: the whirlwind lingers, only exploding after ~60 tics. */
  if (heretic && mo->type == HERETIC_MT_WHIRLWIND)
  {
    if (++mo->special2.i < 60)
      return;
  }

  mo->momx = mo->momy = mo->momz = 0;

  P_SetMobjState (mo, mobjinfo[mo->type].deathstate);

  /* Heretic does not randomize the explosion's death tics. */
  if (!heretic)
  {
    int adj = P_Random(pr_explode)&3;

    /* Persistent State: a pinned death frame (blood splatter resting at
     * -1 tics) must stay pinned; the adjustment would clamp it to 1 and
     * remove the splat a tic later.  The P_Random is consumed either
     * way, keeping the stream identical with the setting on or off. */
    if (mo->tics != -1)
    {
      mo->tics -= adj;

      if (mo->tics < 1)
        mo->tics = 1;
    }
  }

  mo->flags &= ~MF_MISSILE;

  if (mo->info->deathsound)
    S_StartSound (mo, mo->info->deathsound);
}


//
// P_XYMovement
//
// Attempts to move something if it has momentum.
//

static void P_XYMovement (mobj_t* mo)
{
  player_t *player;
  fixed_t xmove, ymove;

  //e6y
  fixed_t   oldx,oldy; // phares 9/10/98: reducing bobbing/momentum on ice

#if 0
  fixed_t   ptryx;
  fixed_t   ptryy;
  fixed_t   xmove;
  fixed_t   ymove;
  fixed_t   oldx,oldy; // phares 9/10/98: reducing bobbing/momentum on ice
                       // when up against walls
#endif
  if (!(mo->momx | mo->momy)) // Any momentum?
    {
    if (mo->flags & MF_SKULLFLY)
      {

      // the skull slammed into something

      mo->flags &= ~MF_SKULLFLY;
      mo->momz = 0;

      P_SetMobjState (mo, mo->info->spawnstate);
      }
    return;
    }

  player = mo->player;

  if (mo->momx > MAXMOVE)
    mo->momx = MAXMOVE;
  else if (mo->momx < -MAXMOVE)
    mo->momx = -MAXMOVE;

  if (mo->momy > MAXMOVE)
    mo->momy = MAXMOVE;
  else if (mo->momy < -MAXMOVE)
    mo->momy = -MAXMOVE;

  xmove = mo->momx;
  ymove = mo->momy;

  oldx = mo->x; // phares 9/10/98: new code to reduce bobbing/momentum
  oldy = mo->y; // when on ice & up against wall. These will be compared
                // to your x,y values later to see if you were able to move

  do
    {
      fixed_t ptryx, ptryy;
      // killough 8/9/98: fix bug in original Doom source:
      // Large negative displacements were never considered.
      // This explains the tendency for Mancubus fireballs
      // to pass through walls.
      // CPhipps - compatibility optioned

      if (xmove > MAXMOVE/2 || ymove > MAXMOVE/2 ||
  (!comp[comp_moveblock]
   && (xmove < -MAXMOVE/2 || ymove < -MAXMOVE/2)))
      {
      ptryx = mo->x + xmove/2;
      ptryy = mo->y + ymove/2;
      xmove >>= 1;
      ymove >>= 1;
      }
    else
      {
      ptryx = mo->x + xmove;
      ptryy = mo->y + ymove;
      xmove = ymove = 0;
      }

    // killough 3/15/98: Allow objects to drop off

    if (!P_TryMove (mo, ptryx, ptryy, TRUE))
      {
    // blocked move

    // killough 8/11/98: bouncing off walls
    // killough 10/98:
    // Add ability for objects other than players to bounce on ice

    if (!(mo->flags & MF_MISSILE) &&
        mbf_features &&
        (mo->flags & MF_BOUNCES ||
         (!player && blockline &&
    variable_friction && mo->z <= mo->floorz &&
    P_GetFriction(mo, NULL) > ORIG_FRICTION)))
      {
        if (blockline)
    {
      fixed_t r = ((blockline->dx >> FRACBITS) * mo->momx +
             (blockline->dy >> FRACBITS) * mo->momy) /
        ((blockline->dx >> FRACBITS)*(blockline->dx >> FRACBITS)+
         (blockline->dy >> FRACBITS)*(blockline->dy >> FRACBITS));
      fixed_t x = FixedMul(r, blockline->dx);
      fixed_t y = FixedMul(r, blockline->dy);

      // reflect momentum away from wall

      mo->momx = x*2 - mo->momx;
      mo->momy = y*2 - mo->momy;

      // if under gravity, slow down in
      // direction perpendicular to wall.

      if (!(mo->flags & MF_NOGRAVITY))
        {
          mo->momx = (mo->momx + x)/2;
          mo->momy = (mo->momy + y)/2;
        }
    }
        else
    mo->momx = mo->momy = 0;
      }
    else
      if (player || (mo->flags2 & MF2_SLIDE))   // try to slide along it
        P_SlideMove (mo);
      else
        if (mo->flags & MF_MISSILE)
    {
      /* Hexen floor-bouncers striking something in the XY plane ricochet
       * off reflective or non-creature blockers at 0.75 speed, explode on
       * players and monsters, and bounce off walls (the Heresiarch's balls
       * stay silent doing it). */
      if (hexen && mo->flags2 & MF2_FLOORBOUNCE)
        {
        if (BlockingMobj)
          {
          if ((BlockingMobj->flags2 & MF2_REFLECTIVE) ||
              (!BlockingMobj->player &&
               !(BlockingMobj->flags & MF_COUNTKILL)))
            {
            fixed_t speed;
            angle_t angle;

            angle = R_PointToAngle2(BlockingMobj->x, BlockingMobj->y,
                                    mo->x, mo->y) +
                    ANG1 * ((P_Random(pr_heretic) % 16) - 8);
            speed = P_AproxDistance(mo->momx, mo->momy);
            speed = FixedMul(speed, (fixed_t)(0.75 * FRACUNIT));
            mo->angle = angle;
            angle >>= ANGLETOFINESHIFT;
            mo->momx = FixedMul(speed, finecosine[angle]);
            mo->momy = FixedMul(speed, finesine[angle]);
            if (mo->info->seesound)
              S_StartSound(mo, mo->info->seesound);
            return;
            }
          /* struck a player or creature: explode (vanilla falls on through
           * to the explosion below) */
          }
        else
          {                     /* struck a wall */
          P_BounceWall(mo);
          switch (mo->type)
            {
            case HEXEN_MT_SORCBALL1:
            case HEXEN_MT_SORCBALL2:
            case HEXEN_MT_SORCBALL3:
            case HEXEN_MT_SORCFX1:
              break;
            default:
              if (mo->info->seesound)
                S_StartSound(mo, mo->info->seesound);
              break;
            }
          return;
          }
        }

      /* Hexen reflective blockers: the Centaur's raised shield bats the
       * missile away within its frontal arc (the Wraithverge spirit and
       * out-of-arc hits just explode), the Heresiarch deflects by 45
       * degrees, anything else reflects with a small scatter.  The missile
       * is re-aimed at half its base speed and now belongs to the
       * reflector; seekers stash the old quarry and hunt the shooter. */
      if (hexen && BlockingMobj &&
          (BlockingMobj->flags2 & MF2_REFLECTIVE))
        {
        dbool explode_it = FALSE;
        angle_t angle = R_PointToAngle2(BlockingMobj->x, BlockingMobj->y,
                                        mo->x, mo->y);

        switch (BlockingMobj->type)
          {
          case HEXEN_MT_CENTAUR:
          case HEXEN_MT_CENTAURLEADER:
            if ((D_abs((int) angle - (int) BlockingMobj->angle) >> 24) > 45 ||
                mo->type == HEXEN_MT_HOLY_FX)
              {
              explode_it = TRUE;
              break;
              }
            /* fall through */
          case HEXEN_MT_SORCBOSS: /* the Heresiarch's full 45-degree deflection */
            if (P_Random(pr_heretic) < 128)
              angle += ANG45;
            else
              angle -= ANG45;
            break;
          default:
            angle += ANG1 * ((P_Random(pr_heretic) % 16) - 8);
            break;
          }

        if (!explode_it)
          {
          mo->angle = angle;
          angle >>= ANGLETOFINESHIFT;
          mo->momx = FixedMul(mo->info->speed >> 1, finecosine[angle]);
          mo->momy = FixedMul(mo->info->speed >> 1, finesine[angle]);
          if (mo->flags2 & MF2_SEEKERMISSILE)
            P_SetTarget(&mo->special1.m, mo->target);
          P_SetTarget(&mo->target, BlockingMobj);
          return;
          }
        }

      // explode a missile

      if (ceilingline &&
          ceilingline->backsector &&
          ceilingline->backsector->ceilingpic == skyflatnum)
        {
        /* Raven sky-line cases: a popped skull drops back down instead of
         * vanishing, and the Wraithverge spirit explodes. */
        if (raven &&
            mo->type == (hexen ? HEXEN_MT_BLOODYSKULL
                               : HERETIC_MT_BLOODYSKULL))
          {
          mo->momx = mo->momy = 0;
          mo->momz = -FRACUNIT;
          return;
          }
        if (hexen && mo->type == HEXEN_MT_HOLY_FX)
          {
          P_ExplodeMissile(mo);
          return;
          }
        if (demo_compatibility ||  // killough
      mo->z > ceilingline->backsector->ceilingheight)
          {
      // Hack to prevent missiles exploding
      // against the sky.
      // Does not handle sky floors.

      P_RemoveMobj (mo);
      return;
          }
        }
      P_ExplodeMissile (mo);
    }
        else // whatever else it is, it is now standing still in (x,y)
    mo->momx = mo->momy = 0;
      }
    } while (xmove || ymove);

  // slow down

#if 0  /* killough 10/98: this is unused code (except maybe in .deh files?) */
  if (player && player->cheats & CF_NOMOMENTUM)
    {
    // debug option for no sliding at all
    mo->momx = mo->momy = 0;
    player->momx = player->momy = 0;         /* killough 10/98 */
    return;
    }
#endif

  /* no friction for missiles or skulls ever, no friction when airborne */
  if (mo->flags & (MF_MISSILE | MF_SKULLFLY) || mo->z > mo->floorz)
    return;

  /* killough 8/11/98: add bouncers
   * killough 9/15/98: add objects falling off ledges
   * killough 11/98: only include bouncers hanging off ledges
   */
  if (((mo->flags & MF_BOUNCES && mo->z > mo->dropoffz) ||
       mo->flags & MF_CORPSE || mo->intflags & MIF_FALLING) &&
      (mo->momx > FRACUNIT/4 || mo->momx < -FRACUNIT/4 ||
       mo->momy > FRACUNIT/4 || mo->momy < -FRACUNIT/4) &&
      mo->floorz != mo->subsector->sector->floorheight)
    return;  // do not stop sliding if halfway off a step with some momentum

  // killough 11/98:
  // Stop voodoo dolls that have come to rest, despite any
  // moving corresponding player, except in old demos:

  if (mo->momx > -STOPSPEED && mo->momx < STOPSPEED &&
      mo->momy > -STOPSPEED && mo->momy < STOPSPEED &&
      (!player || !(player->cmd.forwardmove | player->cmd.sidemove) ||
       (player->mo != mo && compatibility_level >= lxdoom_1_compatibility)))
    {
      // if in a walking frame, stop moving

      // killough 10/98:
      // Don't affect main player when voodoo dolls stop, except in old demos:

//    if ( player&&(unsigned)((player->mo->state - states)- S_PLAY_RUN1) < 4)
//      P_SetMobjState (player->mo, S_PLAY);
      if (player && (unsigned)(player->mo->state - states - g_s_play_run1) < 4
    && (player->mo == mo || compatibility_level >= lxdoom_1_compatibility))
  P_SetMobjState(player->mo, g_s_play);

      mo->momx = mo->momy = 0;

      /* killough 10/98: kill any bobbing momentum too (except in voodoo dolls)
       * cph - DEMOSYNC - needs compatibility check?
       */
      if (player && player->mo == mo)
  player->momx = player->momy = 0;
    }
  else
    {
      /* phares 3/17/98
       *
       * Friction will have been adjusted by friction thinkers for
       * icy or muddy floors. Otherwise it was never touched and
       * remained set at ORIG_FRICTION
       *
       * killough 8/28/98: removed inefficient thinker algorithm,
       * instead using touching_sectorlist in P_GetFriction() to
       * determine friction (and thus only when it is needed).
       *
       * killough 10/98: changed to work with new bobbing method.
       * Reducing player momentum is no longer needed to reduce
       * bobbing, so ice works much better now.
       *
       * cph - DEMOSYNC - need old code for Boom demos?
       */

      //e6y
      if (compatibility_level <= boom_201_compatibility)
      {
        // phares 3/17/98
        // Friction will have been adjusted by friction thinkers for icy
        // or muddy floors. Otherwise it was never touched and
        // remained set at ORIG_FRICTION
        mo->momx = FixedMul(mo->momx,mo->friction);
        mo->momy = FixedMul(mo->momy,mo->friction);
        mo->friction = ORIG_FRICTION; // reset to normal for next tic
      }
      else if (compatibility_level <= lxdoom_1_compatibility)
      {
        // phares 9/10/98: reduce bobbing/momentum when on ice & up against wall

        if ((oldx == mo->x) && (oldy == mo->y)) // Did you go anywhere?
          { // No. Use original friction. This allows you to not bob so much
            // if you're on ice, but keeps enough momentum around to break free
            // when you're mildly stuck in a wall.
          mo->momx = FixedMul(mo->momx,ORIG_FRICTION);
          mo->momy = FixedMul(mo->momy,ORIG_FRICTION);
          }
        else
          { // Yes. Use stored friction.
          mo->momx = FixedMul(mo->momx,mo->friction);
          mo->momy = FixedMul(mo->momy,mo->friction);
          }
        mo->friction = ORIG_FRICTION; // reset to normal for next tic
      }
      else
      {

      fixed_t friction = P_GetFriction(mo, NULL);

      mo->momx = FixedMul(mo->momx, friction);
      mo->momy = FixedMul(mo->momy, friction);

      /* killough 10/98: Always decrease player bobbing by ORIG_FRICTION.
       * This prevents problems with bobbing on ice, where it was not being
       * reduced fast enough, leading to all sorts of kludges being developed.
       */

      if (player && player->mo == mo)     /* Not voodoo dolls */
  {
    player->momx = FixedMul(player->momx, ORIG_FRICTION);
    player->momy = FixedMul(player->momy, ORIG_FRICTION);
  }

      }

    }
}


int P_SubRandom(void);  /* heretic/p_action.c */

/* Floor-bouncing missiles (the stained-glass shards, the Heresiarch's
 * balls and SORCFX1, the Heretic Firemace balls): bounce with per-type
 * energy decay, lose the shards once they slow, and sink into liquids
 * (P_HitFloor splashes) except for the Heresiarch set.  Vanilla plays the
 * seesound once; dsda-doom carries an extra unconditional replay after the
 * type switch, which doubles the sound, so it is not reproduced. */
static void P_FloorBounceMissile(mobj_t *mo)
{
  if (hexen)
  {
    if (P_HitFloor(mo) >= FLOOR_LIQUID)
    {
      switch (mo->type)
      {
        case HEXEN_MT_SORCFX1:
        case HEXEN_MT_SORCBALL1:
        case HEXEN_MT_SORCBALL2:
        case HEXEN_MT_SORCBALL3:
          break;
        default:
          P_RemoveMobj(mo);
          return;
      }
    }
    switch (mo->type)
    {
      case HEXEN_MT_SORCFX1:
        mo->momz = -mo->momz;   /* no energy absorbed */
        break;
      case HEXEN_MT_SGSHARD1:
      case HEXEN_MT_SGSHARD2:
      case HEXEN_MT_SGSHARD3:
      case HEXEN_MT_SGSHARD4:
      case HEXEN_MT_SGSHARD5:
      case HEXEN_MT_SGSHARD6:
      case HEXEN_MT_SGSHARD7:
      case HEXEN_MT_SGSHARD8:
      case HEXEN_MT_SGSHARD9:
      case HEXEN_MT_SGSHARD0:
        mo->momz = FixedMul(mo->momz, (fixed_t)(-0.3 * FRACUNIT));
        if (D_abs(mo->momz) < (FRACUNIT / 2))
        {
          /* Persistent State: a settling shard comes to rest in its
           * lying-down frame (whose expiry P_SetMobjState pins) instead
           * of vanishing the moment it stops bouncing */
          if (persistent_state && !demoplayback && !netgame)
          {
            mo->momx = mo->momy = mo->momz = 0;
            P_SetMobjState(mo, mo->info->deathstate);
          }
          else
            P_SetMobjState(mo, HEXEN_S_NULL);
          return;
        }
        break;
      default:
        mo->momz = FixedMul(mo->momz, (fixed_t)(-0.7 * FRACUNIT));
        break;
    }
    mo->momx = 2 * mo->momx / 3;
    mo->momy = 2 * mo->momy / 3;
    if (mo->info->seesound)
    {
      switch (mo->type)
      {
        case HEXEN_MT_SORCBALL1:
        case HEXEN_MT_SORCBALL2:
        case HEXEN_MT_SORCBALL3:
          if (!mo->special_args[0])
            S_StartSound(mo, mo->info->seesound);
          break;
        default:
          S_StartSound(mo, mo->info->seesound);
          break;
      }
    }
  }
  else
  {
    mo->momz = -mo->momz;
    P_SetMobjState(mo, mobjinfo[mo->type].deathstate);
  }
}

/* Hexen: landing on top of another mobj (squat, falling damage, class
 * landing grunt) -- the on-mobj counterpart of the floor landing in
 * P_ZMovement. */
static void PlayerLandedOnThing(mobj_t *mo, mobj_t *onmobj)
{
  (void) onmobj;

  mo->player->deltaviewheight = mo->momz >> 3;
  if (mo->momz < -23 * FRACUNIT)
  {
    P_FallingDamage(mo->player);
    P_NoiseAlert(mo, mo);
  }
  else if (mo->momz < -GRAVITY * 12 && !mo->player->morphTics)
  {
    S_StartSound(mo, hexen_sfx_player_land);
    switch (mo->player->class)
    {
      case PCLASS_FIGHTER:
        S_StartSound(mo, hexen_sfx_player_fighter_grunt);
        break;
      case PCLASS_CLERIC:
        S_StartSound(mo, hexen_sfx_player_cleric_grunt);
        break;
      case PCLASS_MAGE:
        S_StartSound(mo, hexen_sfx_player_mage_grunt);
        break;
      default:
        break;
    }
  }
  else if (!mo->player->morphTics)
  {
    S_StartSound(mo, hexen_sfx_player_land);
  }
  mo->player->centering = true;   /* vanilla auto-corrects the look pitch */
}

#define SMALLSPLASHCLIP (12 << FRACBITS)

/* Hexen splashes: small drips for light things, the full splash (with a
 * monster-waking noise for players) otherwise, and the splash-time lava
 * contact damage through the fire-typed inflictor. */
static int Hexen_P_HitFloor(mobj_t *thing)
{
  mobj_t *mo;
  dbool smallsplash = FALSE;

  /* Things that don't splash go here */
  switch (thing->type)
  {
    case HEXEN_MT_LEAF1:
    case HEXEN_MT_LEAF2:
    case HEXEN_MT_SPLASH:
    case HEXEN_MT_SLUDGECHUNK:
      return FLOOR_SOLID;
    default:
      break;
  }

  /* Small splash for small masses */
  if (thing->info->mass < 10)
    smallsplash = TRUE;

  switch (P_GetThingFloorType(thing))
  {
    case FLOOR_WATER:
      if (smallsplash)
      {
        mo = P_SpawnMobj(thing->x, thing->y, ONFLOORZ, HEXEN_MT_SPLASHBASE);
        if (mo)
          mo->floorclip += SMALLSPLASHCLIP;
        S_StartSound(mo, hexen_sfx_ambient10);  /* small drip */
      }
      else
      {
        mo = P_SpawnMobj(thing->x, thing->y, ONFLOORZ, HEXEN_MT_SPLASH);
        P_SetTarget(&mo->target, thing);
        mo->momx = P_SubRandom() << 8;
        mo->momy = P_SubRandom() << 8;
        mo->momz = 2 * FRACUNIT + (P_Random(pr_heretic) << 8);
        mo = P_SpawnMobj(thing->x, thing->y, ONFLOORZ, HEXEN_MT_SPLASHBASE);
        if (thing->player)
          P_NoiseAlert(thing, thing);
        S_StartSound(mo, hexen_sfx_water_splash);
      }
      return FLOOR_WATER;
    case FLOOR_LAVA:
      if (smallsplash)
      {
        mo = P_SpawnMobj(thing->x, thing->y, ONFLOORZ, HEXEN_MT_LAVASPLASH);
        if (mo)
          mo->floorclip += SMALLSPLASHCLIP;
      }
      else
      {
        mo = P_SpawnMobj(thing->x, thing->y, ONFLOORZ, HEXEN_MT_LAVASMOKE);
        mo->momz = FRACUNIT + (P_Random(pr_heretic) << 7);
        mo = P_SpawnMobj(thing->x, thing->y, ONFLOORZ, HEXEN_MT_LAVASPLASH);
        if (thing->player)
          P_NoiseAlert(thing, thing);
      }
      S_StartSound(mo, hexen_sfx_lava_sizzle);
      if (thing->player && leveltime & 31)
        P_DamageMobj(thing, &LavaInflictor, NULL, 5);
      return FLOOR_LAVA;
    case FLOOR_SLUDGE:
      if (smallsplash)
      {
        mo = P_SpawnMobj(thing->x, thing->y, ONFLOORZ, HEXEN_MT_SLUDGESPLASH);
        if (mo)
          mo->floorclip += SMALLSPLASHCLIP;
      }
      else
      {
        mo = P_SpawnMobj(thing->x, thing->y, ONFLOORZ, HEXEN_MT_SLUDGECHUNK);
        P_SetTarget(&mo->target, thing);
        mo->momx = P_SubRandom() << 8;
        mo->momy = P_SubRandom() << 8;
        mo->momz = FRACUNIT + (P_Random(pr_heretic) << 8);
        mo = P_SpawnMobj(thing->x, thing->y, ONFLOORZ, HEXEN_MT_SLUDGESPLASH);
        if (thing->player)
          P_NoiseAlert(thing, thing);
      }
      S_StartSound(mo, hexen_sfx_sludge_gloop);
      return FLOOR_SLUDGE;
    default:
      break;
  }
  return FLOOR_SOLID;
}

/* Terrain splashes on landing in a liquid (heretic and hexen tables).  Note
 * this lives here rather than in heretic/p_action.c: that file folds the
 * 'hexen' flag to a constant 0 for its heretic-only codepointers, which
 * would dead-strip the hexen dispatch below. */
int P_HitFloor(mobj_t *thing)
{
  mobj_t *mo;

  if (thing->floorz != thing->subsector->sector->floorheight)
  {                             /* landed on the edge above the liquid */
    return FLOOR_SOLID;
  }

  if (hexen)
    return Hexen_P_HitFloor(thing);

  switch (P_GetThingFloorType(thing))
  {
    case FLOOR_WATER:
      P_SpawnMobj(thing->x, thing->y, ONFLOORZ, HERETIC_MT_SPLASHBASE);
      mo = P_SpawnMobj(thing->x, thing->y, ONFLOORZ, HERETIC_MT_SPLASH);
      P_SetTarget(&mo->target, thing);
      mo->momx = P_SubRandom() << 8;
      mo->momy = P_SubRandom() << 8;
      mo->momz = 2 * FRACUNIT + (P_Random(pr_heretic) << 8);
      S_StartSound(mo, heretic_sfx_gloop);
      return FLOOR_WATER;
    case FLOOR_LAVA:
      P_SpawnMobj(thing->x, thing->y, ONFLOORZ, HERETIC_MT_LAVASPLASH);
      mo = P_SpawnMobj(thing->x, thing->y, ONFLOORZ, HERETIC_MT_LAVASMOKE);
      mo->momz = FRACUNIT + (P_Random(pr_heretic) << 7);
      S_StartSound(mo, heretic_sfx_burn);
      return FLOOR_LAVA;
    case FLOOR_SLUDGE:
      P_SpawnMobj(thing->x, thing->y, ONFLOORZ, HERETIC_MT_SLUDGESPLASH);
      mo = P_SpawnMobj(thing->x, thing->y, ONFLOORZ, HERETIC_MT_SLUDGECHUNK);
      P_SetTarget(&mo->target, thing);
      mo->momx = P_SubRandom() << 8;
      mo->momy = P_SubRandom() << 8;
      mo->momz = FRACUNIT + (P_Random(pr_heretic) << 8);
      return FLOOR_SLUDGE;
    default:
      break;
  }
  return FLOOR_SOLID;
}

//
// P_ZMovement
//
// Attempt vertical movement.

static void P_ZMovement (mobj_t* mo)
{
  /* killough 7/11/98:
   * BFG fireballs bounced on floors and ceilings in Pre-Beta Doom
   * killough 8/9/98: added support for non-missile objects bouncing
   * (e.g. grenade, mine, pipebomb)
   */

  if (mo->flags & MF_BOUNCES && mo->momz) {
    mo->z += mo->momz;
    if (mo->z <= mo->floorz) {                /* bounce off floors */
      mo->z = mo->floorz;
      if (mo->momz < 0) {
        mo->momz = -mo->momz;
  if (!(mo->flags & MF_NOGRAVITY)) { /* bounce back with decay */
    mo->momz = mo->flags & MF_FLOAT ?   // floaters fall slowly
      mo->flags & MF_DROPOFF ?          // DROPOFF indicates rate
      FixedMul(mo->momz, (fixed_t)(FRACUNIT*.85)) :
      FixedMul(mo->momz, (fixed_t)(FRACUNIT*.70)) :
      FixedMul(mo->momz, (fixed_t)(FRACUNIT*.45)) ;

    /* Bring it to rest below a certain speed */
    if (D_abs(mo->momz) <= mo->info->mass*(GRAVITY*4/256))
      mo->momz = 0;
  }

  /* killough 11/98: touchy objects explode on impact */
  if (mo->flags & MF_TOUCHY && mo->intflags & MIF_ARMED
      && mo->health > 0)
    P_DamageMobj(mo, NULL, NULL, mo->health);
  else if (mo->flags & MF_FLOAT && sentient(mo))
    goto floater;
  return;
      }
    } else if (mo->z >= mo->ceilingz - mo->height) {
      /* bounce off ceilings */
      mo->z = mo->ceilingz - mo->height;
      if (mo->momz > 0) {
  if (mo->subsector->sector->ceilingpic != skyflatnum)
    mo->momz = -mo->momz;    /* always bounce off non-sky ceiling */
  else if (mo->flags & MF_MISSILE)
    P_RemoveMobj(mo);        /* missiles don't bounce off skies */
  else if (mo->flags & MF_NOGRAVITY)
    mo->momz = -mo->momz; // bounce unless under gravity

  if (mo->flags & MF_FLOAT && sentient(mo))
    goto floater;

  return;
      }
    } else {
      if (!(mo->flags & MF_NOGRAVITY))      /* free-fall under gravity */
        mo->momz -= mo->info->mass*(GRAVITY/256);

      if (mo->flags & MF_FLOAT && sentient(mo)) goto floater;
      return;
    }

    /* came to a stop */
    mo->momz = 0;

    if (mo->flags & MF_MISSILE) {
  if (ceilingline &&
      ceilingline->backsector &&
      ceilingline->backsector->ceilingpic == skyflatnum &&
      mo->z > ceilingline->backsector->ceilingheight)
    P_RemoveMobj(mo);  /* don't explode on skies */
  else
    P_ExplodeMissile(mo);
    }

    if (mo->flags & MF_FLOAT && sentient(mo)) goto floater;
    return;
  }

  /* killough 8/9/98: end bouncing object code */

  // check for smooth step up

  if (mo->player &&
      mo->player->mo == mo &&  // killough 5/12/98: exclude voodoo dolls
      mo->z < mo->floorz)
    {
    mo->player->viewheight -= mo->floorz-mo->z;
    mo->player->deltaviewheight = (VIEWHEIGHT - mo->player->viewheight)>>3;
    }

  // adjust altitude

  mo->z += mo->momz;

floater:
  if ((mo->flags & MF_FLOAT) && mo->target)

    // float down towards target if too close

    if (!((mo->flags ^ MF_FLOAT) & (MF_FLOAT | MF_SKULLFLY | MF_INFLOAT)) &&
  mo->target)     /* killough 11/98: simplify */
      {
  fixed_t delta;
  if (P_AproxDistance(mo->x - mo->target->x, mo->y - mo->target->y) <
      D_abs(delta = mo->target->z + (mo->height>>1) - mo->z)*3)
    mo->z += delta < 0 ? -FLOATSPEED : FLOATSPEED;
      }

  // clip movement

  if (mo->z <= mo->floorz)
    {
    // hit the floor

    /* Raven missiles resolve floor contact vanilla-style, ahead of the doom
     * landing code: floor-bouncers bounce, the Minotaur floor fire and the
     * floor lightning pass through (so they can climb steps), the Wraithverge
     * spirit strikes the ground, and the rest splash and explode.  Heretic
     * missiles other than floor-bouncers keep the doom flow below. */
    if (raven && (mo->flags & MF_MISSILE) && !(mo->flags & MF_NOCLIP))
      {
      mo->z = mo->floorz;
      if (mo->flags2 & MF2_FLOORBOUNCE)
        {
        P_FloorBounceMissile (mo);
        return;
        }
      if (hexen)
        {
        if (mo->type == HEXEN_MT_MNTRFX2 ||
            mo->type == HEXEN_MT_LIGHTNING_FLOOR)
          return;               /* Minotaur floor fire can go up steps */
        if (mo->type == HEXEN_MT_HOLY_FX)
          {                     /* the spirit struck the ground */
          mo->momz = 0;
          P_HitFloor (mo);
          return;
          }
        P_HitFloor (mo);
        P_ExplodeMissile (mo);
        return;
        }
      }

    /* Blasted (Disc of Repulsion) monsters falling hard are destroyed. */
    if (hexen && (mo->flags & MF_COUNTKILL) && mo->momz < -(23 * FRACUNIT))
      P_DamageMobj (mo, NULL, NULL, 10000);

    /* Note (id):
     *  somebody left this after the setting momz to 0,
     *  kinda useless there.
     * cph - This was the a bug in the linuxdoom-1.10 source which
     *  caused it not to sync Doom 2 v1.9 demos. Someone
     *  added the above comment and moved up the following code. So
     *  demos would desync in close lost soul fights.
     * cph - revised 2001/04/15 -
     * This was a bug in the Doom/Doom 2 source; the following code
     *  is meant to make charging lost souls bounce off of floors, but it 
     *  was incorrectly placed after momz was set to 0.
     *  However, this bug was fixed in Doom95, Final/Ultimate Doom, and 
     *  the v1.10 source release (which is one reason why it failed to sync 
     *  some Doom2 v1.9 demos)
     * I've added a comp_soul compatibility option to make this behavior 
     *  selectable for PrBoom v2.3+. For older demos, we do this here only 
     *  if we're in a compatibility level above Doom 2 v1.9 (in which case we
     *  mimic the bug and do it further down instead)
     */

    if (mo->flags & MF_SKULLFLY &&
	(!comp[comp_soul] ||
	 (compatibility_level > doom2_19_compatibility &&
	  compatibility_level < prboom_4_compatibility)
	))
      mo->momz = -mo->momz; // the skull slammed into something

    if (mo->momz < 0)
      {
  /* killough 11/98: touchy objects explode on impact */
  if (mo->flags & MF_TOUCHY && mo->intflags & MIF_ARMED && mo->health > 0)
    P_DamageMobj(mo, NULL, NULL, mo->health);
  else
    if (mo->player && /* killough 5/12/98: exclude voodoo dolls */
        mo->player->mo == mo && mo->momz < -GRAVITY*8)
      {
        // Squat down.
        // Decrease viewheight for a moment
        // after hitting the ground (hard),
        // and utter appropriate sound.

        mo->player->deltaviewheight = mo->momz>>3;
        if (hexen)
        {
          if (mo->momz < -23 * FRACUNIT)
          {
            P_FallingDamage(mo->player);
            P_NoiseAlert(mo, mo);
          }
          else if (mo->momz < -GRAVITY * 12 && !mo->player->morphTics)
          {
            S_StartSound(mo, hexen_sfx_player_land);
            switch (mo->player->class)
            {
              case PCLASS_FIGHTER:
                S_StartSound(mo, hexen_sfx_player_fighter_grunt);
                break;
              case PCLASS_CLERIC:
                S_StartSound(mo, hexen_sfx_player_cleric_grunt);
                break;
              case PCLASS_MAGE:
                S_StartSound(mo, hexen_sfx_player_mage_grunt);
                break;
              default:
                break;
            }
          }
        }
        else if (heretic)
          S_StartSound(mo, heretic_sfx_plroof);
        /* cph - prevent "oof" when dead */
        else if (comp[comp_sound] || mo->health > 0)
          S_StartSound (mo, sfx_oof);
      }
  /* Raven: spawn terrain splashes when the thing was airborne above the
   * floor last tic (the check needs the pre-zeroed momz).  Missiles are
   * excluded here; they splash once through the explode branch below. */
  if (raven && !(mo->flags & MF_MISSILE) && mo->z - mo->momz > mo->floorz)
    P_HitFloor(mo);

  mo->momz = 0;
      }
    mo->z = mo->floorz;

    /* cph 2001/04/15 - 
     * This is the buggy lost-soul bouncing code referenced above.
     * We've already set momz = 0 normally by this point, so it's useless.
     * However we might still have upward momentum, in which case this will
     * incorrectly reverse it, so we might still need this for demo sync
     */
    if (mo->flags & MF_SKULLFLY &&
	compatibility_level <= doom2_19_compatibility)
      mo->momz = -mo->momz; // the skull slammed into something

    /* Raven: a dying thing with a crashstate (e.g. the Gargoyle) runs its
     * crash sequence when its falling corpse reaches the floor, instead of
     * staying frozen in its in-air death frame. */
    if (mo->info->crashstate && (mo->flags & MF_CORPSE))
      {
      P_SetMobjState(mo, mo->info->crashstate);
      return;
      }

    if ( (mo->flags & MF_MISSILE) && !(mo->flags & MF_NOCLIP) )
      {
      P_ExplodeMissile (mo);
      return;
      }
    }
  else // still above the floor                                     // phares
    if (!(mo->flags & MF_NOGRAVITY))
      {
  /* MBF21: LOGRAV things fall at 1/8 gravity.  flags2 is zero for all
   * non-MBF21 content, so this is naturally inert outside complevel 21. */
  if (mo->flags2 & MF2_LOGRAV)
    {
      if (!mo->momz)
        mo->momz = -(GRAVITY>>3)*2;
      mo->momz -= GRAVITY>>3;
    }
  else
    {
      if (!mo->momz)
        mo->momz = -GRAVITY;
      mo->momz -= GRAVITY;
    }
      }

  if (mo->z + mo->height > mo->ceilingz)
    {
    /* cph 2001/04/15 - 
     * Lost souls were meant to bounce off of ceilings;
     *  new comp_soul compatibility option added
     */
    if (!comp[comp_soul] && mo->flags & MF_SKULLFLY)
      mo->momz = -mo->momz; // the skull slammed into something

    // hit the ceiling

    if (mo->momz > 0)
      mo->momz = 0;

    mo->z = mo->ceilingz - mo->height;

    /* cph 2001/04/15 - 
     * We might have hit a ceiling but had downward momentum (e.g. ceiling is 
     *  lowering on us), so for old demos we must still do the buggy 
     *  momentum reversal here
     */
    if (comp[comp_soul] && mo->flags & MF_SKULLFLY)
      mo->momz = -mo->momz; // the skull slammed into something

    if ( (mo->flags & MF_MISSILE) && !(mo->flags & MF_NOCLIP) )
      {
      P_ExplodeMissile (mo);
      return;
      }
    }
  }

//
// P_NightmareRespawn
//

static void P_NightmareRespawn(mobj_t* mobj)
{
  fixed_t      x;
  fixed_t      y;
  fixed_t      z;
  subsector_t* ss;
  mobj_t*      mo;
  mapthing_t*  mthing;

  x = mobj->spawnpoint.x << FRACBITS;
  y = mobj->spawnpoint.y << FRACBITS;

  /* haleyjd: stupid nightmare respawning bug fix
   *
   * 08/09/00: compatibility added, time to ramble :)
   * This fixes the notorious nightmare respawning bug that causes monsters
   * that didn't spawn at level startup to respawn at the point (0,0)
   * regardless of that point's nature. SMMU and Eternity need this for
   * script-spawned things like Halif Swordsmythe, as well.
   *
   * cph - copied from eternity, except comp_respawnfix becomes comp_respawn
   *   and the logic is reversed (i.e. like the rest of comp_ it *disables*
   *   the fix)
   */
  if(!comp[comp_respawn] && !x && !y)
  {
     // spawnpoint was zeroed out, so use point of death instead
     x = mobj->x;
     y = mobj->y;
  }

  // something is occupying its position?

  if (!P_CheckPosition (mobj, x, y) )
    return; // no respwan

  // spawn a teleport fog at old spot
  // because of removal of the body?

  mo = P_SpawnMobj (mobj->x,
                    mobj->y,
                    mobj->subsector->sector->floorheight + g_telefog_height,
                    g_mt_tfog);

  // initiate teleport sound

  S_StartSound (mo, g_sfx_telept);

  // spawn a teleport fog at the new spot

  ss = R_PointInSubsector (x,y);

  mo = P_SpawnMobj (x, y, ss->sector->floorheight + g_telefog_height, g_mt_tfog);

  S_StartSound (mo, g_sfx_telept);

  // spawn the new monster

  mthing = &mobj->spawnpoint;
  if (mobj->info->flags & MF_SPAWNCEILING)
    z = ONCEILINGZ;
  else
    z = ONFLOORZ;

  // inherit attributes from deceased one

  mo = P_SpawnMobj (x,y,z, mobj->type);
  mo->spawnpoint = mobj->spawnpoint;
  mo->angle = ANG45 * (mthing->angle/45);

  if (mthing->options & MTF_AMBUSH)
    mo->flags |= MF_AMBUSH;

  /* killough 11/98: transfer friendliness from deceased */
  mo->flags = (mo->flags & ~MF_FRIEND) | (mobj->flags & MF_FRIEND);

  mo->reactiontime = 18;

  // remove the old monster,

  P_RemoveMobj (mobj);
}


//
// P_MobjThinker
//

void P_MobjThinker (mobj_t* mobj)
{
  // killough 11/98:
  // removed old code which looked at target references
  // (we use pointer reference counting now)

  if (mobj->type == MT_MUSICCHANGER)
  {
    P_MusInfoMobjThinker(mobj);
    return;
  }

  mobj->PrevX = mobj->x;
  mobj->PrevY = mobj->y;
  mobj->PrevZ = mobj->z;

  // momentum movement
  if (mobj->momx | mobj->momy || mobj->flags & MF_SKULLFLY)
  {
    P_XYMovement(mobj);
    if (mobj->thinker.function.arg1 != (void (*)(void *))P_MobjThinker) // cph - Must've been removed
      return;       // killough - mobj was removed
  }
  else if (mobj->flags2 & MF2_BLASTED)
  {                   /* blast has worn off once the mobj is at rest */
    extern void ResetBlasted(mobj_t *mo);
    ResetBlasted(mobj);
  }

  if (raven && (mobj->flags2 & MF2_FLOATBOB))
  {                             /* floating item bobbing motion */
    mobj->z = mobj->floorz + (hexen ? mobj->special1.i : 0)
            + FloatBobOffsets[(mobj->health++) & 63];
  }
  else if (raven && (mobj->flags2 & MF2_PASSMOBJ) &&
           (mobj->z != mobj->floorz || mobj->momz || BlockingMobj))
  {
    /* Raven z movement is mobj-aware: things can land on, stand on, and
     * pass over or under each other. */
    mobj_t *onmo = P_CheckOnmobj(mobj);

    if (!onmo)
    {
      P_ZMovement(mobj);
      if (mobj->thinker.function.arg1 != (void (*)(void *))P_MobjThinker)
        return;     /* mobj was removed */
      /* This bug is part of the original source: it tests 'flags', not
       * 'flags2', so the clear below effectively never runs. */
      if (hexen && mobj->player && mobj->flags & MF2_ONMOBJ)
        mobj->flags2 &= ~MF2_ONMOBJ;
    }
    else if (mobj->player)
    {
      if (hexen)
      {
        if (mobj->momz < -GRAVITY * 8 && !(mobj->flags2 & MF2_FLY))
          PlayerLandedOnThing(mobj, onmo);
        if (onmo->z + onmo->height - mobj->z <= 24 * FRACUNIT)
        {
          mobj->player->viewheight -= onmo->z + onmo->height - mobj->z;
          mobj->player->deltaviewheight =
            (VIEWHEIGHT - mobj->player->viewheight) >> 3;
          mobj->z = onmo->z + onmo->height;
          mobj->flags2 |= MF2_ONMOBJ;
          mobj->momz = 0;
        }
        else
        {                       /* hit the bottom of the blocking mobj */
          mobj->momz = 0;
        }
      }
      else
      {                         /* heretic */
        if (mobj->momz < 0)
        {
          mobj->flags2 |= MF2_ONMOBJ;
          mobj->momz = 0;
        }
        if (onmo->player || onmo->type == HERETIC_MT_POD)
        {
          mobj->momx = onmo->momx;
          mobj->momy = onmo->momy;
          if (onmo->z < onmo->floorz)
          {
            mobj->z += onmo->floorz - onmo->z;
            if (onmo->player)
            {
              onmo->player->viewheight -= onmo->floorz - onmo->z;
              onmo->player->deltaviewheight =
                (VIEWHEIGHT - onmo->player->viewheight) >> 3;
            }
            onmo->z = onmo->floorz;
          }
        }
      }
    }
  }
  else if (mobj->z != mobj->floorz || mobj->momz)
  {
    P_ZMovement(mobj);
    if (mobj->thinker.function.arg1 != (void (*)(void *))P_MobjThinker) // cph - Must've been removed
      return;       // killough - mobj was removed
  }
  else if (!(mobj->momx | mobj->momy) && !sentient(mobj))
  {                                 // non-sentient objects at rest
    mobj->intflags |= MIF_ARMED;     // arm a mine which has come to rest

    // killough 9/12/98: objects fall off ledges if they are hanging off
    // slightly push off of ledge if hanging more than halfway off

    if (mobj->z > mobj->dropoffz &&      // Only objects contacting dropoff
        !(mobj->flags & MF_NOGRAVITY) && // Only objects which fall
        !comp[comp_falloff]) // Not in old demos
      P_ApplyTorque(mobj);               // Apply torque
    else
      mobj->intflags &= ~MIF_FALLING, mobj->gear = 0;  // Reset torque
  }

  /* MBF21: a grounded, shootable, non-floating monster standing in a
   * kill-monsters sector (special bit 13) is destroyed.  Inert below
   * complevel 21. */
  if (mbf21_features &&
      (mobj->subsector->sector->special & KILL_MONSTERS_MASK) &&
      mobj->z == mobj->floorz &&
      mobj->player == NULL &&
      (mobj->flags & MF_SHOOTABLE) &&
      !(mobj->flags & MF_FLOAT))
  {
    P_DamageMobj(mobj, NULL, NULL, 10000);
    if (mobj->thinker.function.arg1 != (void (*)(void *))P_MobjThinker)
      return; /* removed */
  }

  /* Persistent State: resting debris is NOBLOCKMAP, so P_ChangeSector
   * never height-clips it when its sector's floor moves -- vanilla
   * never notices because the debris is gone within a second.  Follow
   * the floor manually: both down (a lowering platform takes the blood
   * with it) and up (a rising one would otherwise bury it).  Only our
   * pinned debris types ever rest at -1 tics, so the test is exact
   * regardless of the setting's current value. */
  if (mobj->tics == -1 &&
      mobj->z != mobj->subsector->sector->floorheight &&
      (P_IsBlood(mobj) || P_IsRestingDebris(mobj)))
  {
    mobj->floorz = mobj->subsector->sector->floorheight;
    mobj->z = mobj->floorz;
  }

  // cycle through states,
  // calling action functions at transitions

  if (mobj->tics != -1)
  {
    mobj->tics--;

    // you can cycle through multiple states in a tic

    if (!mobj->tics)
      if (!P_SetMobjState (mobj, mobj->state->nextstate) )
        return;     // freed itself
  }
  else
  {

    // check for nightmare respawn

    if (! (mobj->flags & MF_COUNTKILL) )
      return;

    if (!respawnmonsters)
      return;

    mobj->movecount++;

    if (mobj->movecount < 12*35)
      return;

    if (leveltime & 31)
      return;

    if (P_Random (pr_respawn) > 4)
      return;

    P_NightmareRespawn (mobj);
  }

}


//
// P_SpawnMobj
//
/* Heretic helpers defined across translation units / later in this file. */
int  P_FaceMobj(mobj_t *source, mobj_t *target, angle_t *delta);

/* Heretic attack globals: current attack puff type and the last missile
 * spawned (consumed by some Heretic weapon codepointers).  Inert for Doom. */
#ifndef FOOTCLIPSIZE
#define FOOTCLIPSIZE (10*FRACUNIT)
#endif
mobjtype_t PuffType;
mobj_t *PuffSpawned;  /* last puff actor spawned by P_SpawnPuff (Hexen melee feedback) */
mobj_t *PuffSpawned;   /* last puff P_SpawnPuff created (Raven games) */

void S_StartMobjSound(mobj_t *mobj, int sfx_id);  /* heretic/p_action.c */
mobj_t *MissileMobj;

mobj_t* P_SpawnMobj(fixed_t x,fixed_t y,fixed_t z,mobjtype_t type)
{
  mobj_t*     mobj;
  state_t*    st;
  mobjinfo_t* info;

  /* DSDHacked: type can come from editable codepointer args (e.g.
   * A_WeaponProjectile/A_MonsterProjectile use args[0]-1) and may be out
   * of range for the (grown) mobjinfo table.  Indexing mobjinfo[type]
   * would be out of bounds, so refuse the spawn.  Callers that can pass an
   * editable type null-check the result. */
  if ((unsigned)type >= (unsigned)num_mobj_types)
    return NULL;

  mobj = Z_Malloc (sizeof(*mobj), PU_LEVEL, NULL);
  memset (mobj, 0, sizeof (*mobj));
  info = &mobjinfo[type];
  mobj->type = type;
  mobj->info = info;
  mobj->x = x;
  mobj->y = y;
  mobj->radius = info->radius;
  mobj->height = info->height;                                      // phares
  mobj->flags  = info->flags;
  mobj->flags2 = info->flags2; /* MBF21 thing flags */

  /* MBF21 thing flags must not affect sub-21 demos even if a deh patch set
   * them, so strip flags2 entirely below complevel 21.  This is the single
   * choke point that keeps every MF2_* behaviour inert outside MBF21,
   * mirroring the friends/bouncers strip below.  Raven games are exempt:
   * for Heretic and Hexen, flags2 carries core thing behaviour (FOOTCLIP,
   * PASSMOBJ, INVULNERABLE, DONTDRAW, ...) that must survive at every
   * complevel -- Doom demo compatibility does not apply to them. */
  if (!mbf21_features && !raven)
    mobj->flags2 = 0;

  /* killough 8/23/98: no friends, bouncers, or touchy things in old demos */
  if (!mbf_features)
    mobj->flags &= ~(MF_BOUNCES | MF_FRIEND | MF_TOUCHY);
  else
    if (type == MT_PLAYER)         // Except in old demos, players
      mobj->flags |= MF_FRIEND;    // are always friends.

  mobj->health = info->spawnhealth;

  if (gameskill != sk_nightmare)
    mobj->reactiontime = info->reactiontime;

  mobj->lastlook = P_Random (pr_lastlook) % MAXPLAYERS;

  // do not set the state with P_SetMobjState,
  // because action routines can not be called yet

  /* DSDHacked: spawnstate is editable and may be out of range; clamp to
   * S_NULL rather than index states[] out of bounds. */
  if ((unsigned)info->spawnstate >= (unsigned)num_states)
    st = &states[S_NULL];
  else
    st = &states[info->spawnstate];

  mobj->state  = st;
  mobj->tics   = st->tics;
  mobj->sprite = st->sprite;
  mobj->frame  = st->frame;
  mobj->touching_sectorlist = NULL; // NULL head of sector list // phares 3/13/98

  // set subsector and/or block links

  P_SetThingPosition (mobj);

  mobj->dropoffz =           /* killough 11/98: for tracking dropoffs */
  mobj->floorz   = P_FloorZAtPoint(mobj->subsector->sector, x, y);
  mobj->ceilingz = P_CeilingZAtPoint(mobj->subsector->sector, x, y);

  mobj->z = z == ONFLOORZ ? mobj->floorz : z == ONCEILINGZ ?
    mobj->ceilingz - mobj->height : z;

  mobj->PrevX = mobj->x;
  mobj->PrevY = mobj->y;
  mobj->PrevZ = mobj->z;

  mobj->thinker.function.arg1 = (void (*)(void *))P_MobjThinker;

  //e6y
  mobj->friction    = ORIG_FRICTION;                        // phares 3/17/98

  mobj->target = mobj->tracer = mobj->lastenemy = NULL;
  P_AddThinker (&mobj->thinker);
  if (!((mobj->flags ^ MF_COUNTKILL) & (MF_FRIEND | MF_COUNTKILL)))
    totallive++;
  return mobj;
}


static mapthing_t itemrespawnque[ITEMQUESIZE];
static int        itemrespawntime[ITEMQUESIZE];
int        iquehead;
int        iquetail;


//
// P_RemoveMobj
//

void P_RemoveMobj (mobj_t* mobj)
{
  P_BloodQueueUnhook(mobj);

  /* All TID-bearing formats (Hexen and the ZDoom map formats, which
   * build the TID list in P_SetupLevel) must unhook removed mobjs, or
   * TIDMobj keeps a dangling pointer and P_FindMobjFromTID hands out
   * freed memory.  Doom-format mobjs always carry tid 0: no cost. */
  if (mobj->tid != 0)
    P_RemoveMobjFromTIDList(mobj);

  /* Hexen: a mobj leaving the world by any path must leave the corpse
   * queue too, or its slot dangles and A_QueueCorpse's eviction later
   * removes freed memory.  dsda-doom gates this on the corpse flags,
   * but several hexen removal paths reach here without them and were
   * observed dangling on MAP40, so the scan runs for every removal --
   * it is 64 pointer compares. */
  if (hexen)
    A_DeQueueCorpse(mobj);

  if ((mobj->flags & MF_SPECIAL)
      && !(mobj->flags & MF_DROPPED)
      && (mobj->type != MT_INV)
      && (mobj->type != MT_INS))
    {
    itemrespawnque[iquehead] = mobj->spawnpoint;
    itemrespawntime[iquehead] = leveltime;
    iquehead = (iquehead+1)&(ITEMQUESIZE-1);

    // lose one off the end?

    if (iquehead == iquetail)
      iquetail = (iquetail+1)&(ITEMQUESIZE-1);
    }

  // unlink from sector and block lists

  P_UnsetThingPosition (mobj);

  // Delete all nodes on the current sector_list               phares 3/16/98

  if (sector_list)
    {
    P_DelSeclist(sector_list);
    sector_list = NULL;
    }

  // stop any playing sound

  S_StopSound (mobj);

  // killough 11/98:
  //
  // Remove any references to other mobjs.
  //
  // Older demos might depend on the fields being left alone, however,
  // if multiple thinkers reference each other indirectly before the
  // end of the current tic.
  // CPhipps - only leave dead references in old demos; I hope lxdoom_1 level
  // demos are rare and don't rely on this. I hope.

  if ((compatibility_level >= lxdoom_1_compatibility) ||
      (!demoplayback)) {
    P_SetTarget(&mobj->target,    NULL);
    P_SetTarget(&mobj->tracer,    NULL);
    P_SetTarget(&mobj->lastenemy, NULL);
  }
  // free block

  P_RemoveThinker (&mobj->thinker);
}


/*
 * P_FindDoomedNum
 *
 * Finds a mobj type with a matching doomednum
 *
 * killough 8/24/98: rewrote to use hashing
 */

static int P_FindDoomedNum(unsigned type)
{
  static struct { int first, next; } *hash;
  register int i;

  /* Doom and Heretic share one mobjinfo[] array: Doom occupies
   * [0, HERETIC_MT_ZERO) and Heretic [HERETIC_MT_ZERO, ...). Many editor
   * numbers collide between the two games (e.g. doomednum 66 is the Doom
   * Revenant and the Heretic Golem), so the lookup must only consider
   * entries from the game in play -- otherwise Heretic map things spawn
   * the Doom monster that shares the doomednum. */
  int lo = heretic ? (int)HERETIC_MT_ZERO : 0;

  if (!hash)
    {
      /* DSDHacked: size and populate the lookup over the (growable) thing
       * count so map things with high doomednums (new DSDHacked monsters)
       * can be spawned by their editor number. */
      hash = Z_Malloc(sizeof *hash * num_mobj_types, PU_CACHE, (void **) &hash);
      for (i=0; i<num_mobj_types; i++)
  hash[i].first = num_mobj_types;
      for (i=lo; i<num_mobj_types; i++)
  if (mobjinfo[i].doomednum != -1)
    {
      unsigned h = (unsigned) mobjinfo[i].doomednum % num_mobj_types;
      hash[i].next = hash[h].first;
      hash[h].first = i;
    }
    }

  i = hash[type % num_mobj_types].first;
  while ((i < num_mobj_types) && ((unsigned)mobjinfo[i].doomednum != type))
    i = hash[i].next;
  return i;
}

//
// P_RespawnSpecials
//

void P_RespawnSpecials (void)
{
  fixed_t       x;
  fixed_t       y;
  fixed_t       z;
  subsector_t*  ss;
  mobj_t*       mo;
  mapthing_t*   mthing;
  int           i;

  // only respawn items in deathmatch

  if (deathmatch != 2)
    return;

  // nothing left to respawn?

  if (iquehead == iquetail)
    return;

  // wait at least 30 seconds

  if (leveltime - itemrespawntime[iquetail] < 30*35)
    return;

  mthing = &itemrespawnque[iquetail];

  x = mthing->x << FRACBITS;
  y = mthing->y << FRACBITS;

  // spawn a teleport fog at the new spot

  ss = R_PointInSubsector (x,y);
  mo = P_SpawnMobj (x, y, ss->sector->floorheight , MT_IFOG);
  S_StartSound (mo, g_sfx_respawn);

  // find which type to spawn

  /* killough 8/23/98: use table for faster lookup */
  i = P_FindDoomedNum(mthing->type);

  // spawn it

  if (mobjinfo[i].flags & MF_SPAWNCEILING)
    z = ONCEILINGZ;
  else
    z = ONFLOORZ;

  mo = P_SpawnMobj (x,y,z, i);
  mo->spawnpoint = *mthing;
  mo->angle = ANG45 * (mthing->angle/45);

  // pull it from the queue

  iquetail = (iquetail+1)&(ITEMQUESIZE-1);
}

//
// P_SpawnPlayer
// Called when a player is spawned on the level.
// Most of the player structure stays unchanged
//  between levels.
//

extern uint8_t playernumtotrans[MAXPLAYERS];

void P_SpawnPlayer (int n, const mapthing_t* mthing)
{
  player_t* p;
  fixed_t   x;
  fixed_t   y;
  fixed_t   z;
  mobj_t*   mobj;
  int       i;

  // not playing?

  if (!playeringame[n])
    return;

  p = &players[n];

  if (p->playerstate == PST_REBORN)
    G_PlayerReborn (mthing->type-1);

  /* cph 2001/08/14 - use the options field of memorised player starts to
   * indicate whether the start really exists in the level.
   */
  if (!mthing->options)
    I_Error("P_SpawnPlayer: attempt to spawn player at unavailable start point");
  
  x    = mthing->x << FRACBITS;
  y    = mthing->y << FRACBITS;
  z    = ONFLOORZ;
  if (hexen)
  {
    /* Hexen: the player mobj type follows the chosen class - sprites and
     * pain/death sounds differ per class. */
    switch (PlayerClass[n])
    {
      case PCLASS_CLERIC:
        mobj = P_SpawnMobj(x, y, z, HEXEN_MT_PLAYER_CLERIC);
        break;
      case PCLASS_MAGE:
        mobj = P_SpawnMobj(x, y, z, HEXEN_MT_PLAYER_MAGE);
        break;
      default:
        mobj = P_SpawnMobj(x, y, z, HEXEN_MT_PLAYER_FIGHTER);
        break;
    }
  }
  else
    mobj = P_SpawnMobj (x,y,z, g_mt_player);

  // set color translations for player sprites

  mobj->flags |= playernumtotrans[n]<<MF_TRANSSHIFT;

  mobj->angle      = ANG45 * (mthing->angle/45);
  mobj->player     = p;
  mobj->health     = p->health;

  p->mo            = mobj;
  p->playerstate   = PST_LIVE;
  p->refire        = 0;
  p->message       = NULL;
  p->damagecount   = 0;
  p->bonuscount    = 0;
  p->extralight    = 0;
  p->fixedcolormap = 0;
  p->viewheight    = VIEWHEIGHT;

  p->momx = p->momy = 0;   // killough 10/98: initialize bobbing to 0.

  // setup gun psprite

  P_SetupPsprites (p);

  // give all cards in death match mode

  if (deathmatch)
    for (i = 0 ; i < NUMCARDS ; i++)
      p->cards[i] = TRUE;

  if (mthing->type-1 == consoleplayer)
    {
    ST_Start(); // wake up the status bar
    HU_Start(); // wake up the heads up text
    }
    R_SmoothPlaying_Reset(p); // e6y
}

/*
 * P_IsDoomnumAllowed()
 * Based on code taken from P_LoadThings() in src/p_setup.c  Return TRUE
 * if the thing in question is expected to be available in the gamemode used.
 */

dbool P_IsDoomnumAllowed(int doomnum)
{
  /* This filter blocks the Doom II monsters from Doom 1 gamemodes. Under
   * Heretic those same editor numbers (64/65/66/68/69 = Gargoyle, Fire
   * Gargoyle, Golem, Mummy, Mummy Leader, etc.) are core Heretic monsters,
   * so the Doom-specific restriction must not apply. */
  if (heretic)
    return TRUE;

  // Do not spawn cool, new monsters if !commercial
  if (gamemode != commercial)
    switch(doomnum)
      {
      case 64:  // Archvile
      case 65:  // Former Human Commando
      case 66:  // Revenant
      case 67:  // Mancubus
      case 68:  // Arachnotron
      case 69:  // Hell Knight
      case 71:  // Pain Elemental
      case 84:  // Wolf SS
      case 88:  // Boss Brain
      case 89:  // Boss Shooter
        return FALSE;
      }

  return TRUE;
}

//
// Hexen thing-id (TID) list.
//
// Hexen things may carry a thing id; specials and scripts find a mobj by its
// TID.  The list is rebuilt after a level loads and kept in sync as TID-
// bearing mobjs are spawned or removed.  P_LoadThings stages each Hexen
// thing's id and args into these carriers right before P_SpawnMapThing, since
// the engine's narrow mapthing_t (a raw Doom WAD record) cannot hold them.
//

#define MAX_TID_COUNT 200

static int     TIDList[MAX_TID_COUNT + 1];  /* +1 for the 0 terminator */
static mobj_t *TIDMobj[MAX_TID_COUNT];

short hexen_thing_tid = 0;
short hexen_thing_height = 0;
int   hexen_thing_args[5] = {0, 0, 0, 0, 0};
int   hexen_thing_special = 0;

void P_CreateTIDList(void)
{
  int        i = 0;
  thinker_t *t;

  for (t = thinkercap.next; t != &thinkercap; t = t->next)
  {
    mobj_t *mobj;
    if (t->function.arg1 != (void (*)(void *))P_MobjThinker)
      continue;
    mobj = (mobj_t *) t;
    if (mobj->tid != 0)
    {
      if (i == MAX_TID_COUNT)
        I_Error("P_CreateTIDList: MAX_TID_COUNT (%d) exceeded.", MAX_TID_COUNT);
      TIDList[i] = mobj->tid;
      TIDMobj[i++] = mobj;
    }
  }
  TIDList[i] = 0;
}

void P_InsertMobjIntoTIDList(mobj_t *mobj, short tid)
{
  int i;
  int index = -1;

  for (i = 0; TIDList[i] != 0; i++)
    if (TIDList[i] == -1)
    {
      index = i;
      break;
    }
  if (index == -1)
  {
    if (i == MAX_TID_COUNT)
      I_Error("P_InsertMobjIntoTIDList: MAX_TID_COUNT (%d) exceeded.",
              MAX_TID_COUNT);
    index = i;
    TIDList[index + 1] = 0;
  }
  mobj->tid = tid;
  TIDList[index] = tid;
  TIDMobj[index] = mobj;
}

void P_RemoveMobjFromTIDList(mobj_t *mobj)
{
  int i;

  for (i = 0; TIDList[i] != 0; i++)
    if (TIDMobj[i] == mobj)
    {
      TIDList[i] = -1;
      TIDMobj[i] = NULL;
      mobj->tid = 0;
      return;
    }
  mobj->tid = 0;
}

mobj_t *P_SpawnKoraxMissile(fixed_t x, fixed_t y, fixed_t z,
                            mobj_t *source, mobj_t *dest, mobjtype_t type)
{
  mobj_t *th;
  angle_t an;
  int dist;

  z -= source->floorclip;
  th = P_SpawnMobj(x, y, z, type);
  if (th->info->seesound)
    S_StartSound(th, th->info->seesound);
  P_SetTarget(&th->target, source);    /* originator */
  an = R_PointToAngle2(x, y, dest->x, dest->y);
  if (dest->flags & MF_SHADOW)
  {                                    /* invisible target */
    an += P_SubRandom() << 21;
  }
  th->angle = an;
  an >>= ANGLETOFINESHIFT;
  th->momx = FixedMul(th->info->speed, finecosine[an]);
  th->momy = FixedMul(th->info->speed, finesine[an]);
  dist = P_AproxDistance(dest->x - x, dest->y - y);
  dist = dist / th->info->speed;
  if (dist < 1)
    dist = 1;
  th->momz = (dest->z - z + (30 * FRACUNIT)) / dist;
  P_CheckMissileSpawn(th);
  return th;
}

mobj_t *P_FindMobjFromTID(short tid, int *searchPosition)
{
  int i;

  for (i = *searchPosition + 1; TIDList[i] != 0; i++)
    if (TIDList[i] == tid)
    {
      *searchPosition = i;
      return TIDMobj[i];
    }
  *searchPosition = -1;
  return NULL;
}

//
// P_SpawnMapThing
// The fields of the mapthing should
// already be in host byte order.
//

void P_SpawnMapThing (const mapthing_t* mthing)
{
  int     i;
  //int     bit;
  mobj_t* mobj;
  fixed_t x;
  fixed_t y;
  fixed_t z;
  int options = mthing->options; /* cph 2001/07/07 - make writable copy */
  short thingtype = mthing->type;
  int iden_num = 0;

  // killough 2/26/98: Ignore type-0 things as NOPs
  // phares 5/14/98: Ignore Player 5-8 starts (for now)

  switch(thingtype)
  {
    case 0:
    case DEN_PLAYER5:
    case DEN_PLAYER6:
    case DEN_PLAYER7:
    case DEN_PLAYER8:
      return;
  }

  /* Hexen: polyobject anchor and start-spot things are load-time
   * bookkeeping handled by PO_Init, not world mobjs (and the start spots
   * are counted here so PO_Init knows how many to allocate). */
  if (PO_Detect(thingtype))
    return;

  /* Hexen places player starts 5-8 at editor numbers 9100-9103 (players 1-4
   * use 1-4 as in Doom).  This core is single-player, so the extra starts are
   * recorded as inactive and otherwise ignored -- without this they fall
   * through to the doomednum lookup and spam "Unknown Thing type". */
  if (hexen && thingtype >= 9100 && thingtype <= 9103)
    return;

  // killough 11/98: clear flags unused by Doom
  //
  // We clear the flags unused in Doom if we see flag mask 256 set, since
  // it is reserved to be 0 under the new scheme. A 1 in this reserved bit
  // indicates it's a Doom wad made by a Doom editor which puts 1's in
  // bits that weren't used in Doom (such as HellMaker wads). So we should
  // then simply ignore all upper bits.

  if (!map_format.hexen &&
      (demo_compatibility ||
      (compatibility_level >= lxdoom_1_compatibility  &&
       options & MTF_RESERVED))) {
    if (!demo_compatibility) // cph - Add warning about bad thing flags
      lprintf(LO_WARN, "P_SpawnMapThing: correcting bad flags (%u) (thing type %d)\n",
        options, thingtype);
    options &= MTF_EASY|MTF_NORMAL|MTF_HARD|MTF_AMBUSH|MTF_NOTSINGLE;
  }

  /* Heretic: firemace things are spawn-spot candidates, not direct spawns;
   * P_CloseWeapons picks at most one of them after the things load.  The
   * collection happens before the options filtering on purpose -- vanilla
   * flags the spots multiplayer-only precisely so that nothing spawns from
   * them directly, while the spot list ignores the options. */
  if (heretic && thingtype == 2002)
  {
    P_AddMaceSpot(mthing->x << FRACBITS, mthing->y << FRACBITS);
    return;
  }

  /* Heretic: boss-spot markers (editor number 56) are D'Sparil teleport
   * destinations, not spawnable things. */
  if (heretic && thingtype == 56)
  {
    void P_AddBossSpot(fixed_t bx, fixed_t by, angle_t bangle);  /* heretic/p_action.c */

    P_AddBossSpot(mthing->x << FRACBITS, mthing->y << FRACBITS,
                  ANG45 * (mthing->angle / 45));
    return;
  }

  // count deathmatch start positions

  // doom2.exe has at most 10 deathmatch starts
  if (thingtype == 11)
  {
    if (!(!compatibility || deathmatch_p-deathmatchstarts < 10)) {
      return;
    } else {
      // 1/11/98 killough -- new code removes limit on deathmatch starts:

      size_t offset = deathmatch_p - deathmatchstarts;

      if (offset >= num_deathmatchstarts)
      {
        num_deathmatchstarts = num_deathmatchstarts ?
                 num_deathmatchstarts*2 : 16;
        deathmatchstarts = realloc(deathmatchstarts,
                   num_deathmatchstarts *
                   sizeof(*deathmatchstarts));
        deathmatch_p = deathmatchstarts + offset;
      }
      memcpy(deathmatch_p++, mthing, sizeof(*mthing));
      (deathmatch_p-1)->options = 1;
      return;
    }
  }

  // check for players specially

  if (thingtype <= 4 && thingtype > 0)  // killough 2/26/98 -- fix crashes
  {

    // save spots for respawning in coop games
    /* Hexen: a map may carry several starts per player, distinguished by the
     * position number in the thing's args[0]; a Teleport_NewMap line names
     * which one the player arrives at (RebornPosition).  Doom and Heretic
     * starts are always position 0. */
    {
      int pos = hexen ? hexen_thing_args[0] : 0;
      if (pos < 0 || pos >= MAX_PLAYER_STARTS)
        pos = 0;
      playerstarts[pos][thingtype-1] = *mthing;
      /* cph 2006/07/24 - use the otherwise-unused options field to flag that
       * this start is present (so we know which elements of the array are
       * filled in, in effect). Also note that the call below to P_SpawnPlayer
       * must use the playerstarts version with this field set */
      playerstarts[pos][thingtype-1].options = 1;

      if (!deathmatch && pos == (hexen ? RebornPosition : 0))
        P_SpawnPlayer (thingtype-1, &playerstarts[pos][thingtype-1]);
    }
    return;
  }

  // check for apropriate skill level

  if (map_format.hexen)
  {
    /* Hexen-format things filter by positive "appears in this game mode"
     * bits rather than Doom's NOT* bits; ZDoom Doom-in-Hexen maps use the
     * same bits.  The character-class bits only mean anything in the Hexen
     * game itself -- ZDoom-format Doom wads set them on everything, so they
     * must be ignored there. */
    extern pclass_t PlayerClass[];
    int classbit;

    if (!netgame)
    {
      if (!(options & MTF_HEXEN_GSINGLE))
        return;
    }
    else if (deathmatch)
    {
      if (!(options & MTF_HEXEN_GDEATHMATCH))
        return;
    }
    else
    {
      if (!(options & MTF_HEXEN_GCOOP))
        return;
    }

    if (hexen)
    {
      switch (PlayerClass[consoleplayer])
      {
        case PCLASS_FIGHTER: classbit = MTF_HEXEN_FIGHTER; break;
        case PCLASS_CLERIC:  classbit = MTF_HEXEN_CLERIC;  break;
        case PCLASS_MAGE:    classbit = MTF_HEXEN_MAGE;    break;
        default:             classbit = 0;                break;
      }
      if (!netgame && classbit && !(options & classbit))
        return;
    }
  }
  else
  {
    /* jff "not single" thing flag */
    if (!netgame && options & MTF_NOTSINGLE)
      return;

    //jff 3/30/98 implement "not deathmatch" thing flag

    if (netgame && deathmatch && options & MTF_NOTDM)
      return;

    //jff 3/30/98 implement "not cooperative" thing flag

    if (netgame && !deathmatch && options & MTF_NOTCOOP)
      return;
  }

  // killough 11/98: simplify
  if (gameskill == sk_baby || gameskill == sk_easy ?
      !(options & MTF_EASY) :
      gameskill == sk_hard || gameskill == sk_nightmare ?
      !(options & MTF_HARD) : !(options & MTF_NORMAL))
    return;

  // find which type to spawn

  /* Heretic/Raven ambient sound sequence markers occupy editor numbers
   * 1200-1299 (ambient sfx) and 1400-1409 (sound sequences). They spawn
   * no map object. The 1200-1299 markers register an ambient-sound
   * sequence with the ambient subsystem (P_AmbientSound plays them); the
   * 1400-1409 sound-sequence markers are not yet implemented and are
   * consumed quietly so they don't fall through to the mobjinfo lookup. */
  if (raven && thingtype >= 1200 && thingtype < 1300)
  {
    P_AddAmbientSfx(thingtype - 1200);
    return;
  }
  if (raven && thingtype >= 1400 && thingtype < 1410)
  {
    /* Sound-sequence marker: tag the sector it stands in with the sequence
     * type (0-9) so its movers pick the matching door/platform sequence. */
    if (hexen)
    {
      subsector_t *ss = R_PointInSubsector(mthing->x << FRACBITS,
                                           mthing->y << FRACBITS);
      if (ss && ss->sector)
        ss->sector->seqType = thingtype - 1400;
    }
    return;
  }

  // Thing types from 14100 to 14164 are used for MUSINFO entities
  // this means they are actually the same MusicChanger thingtype but
  // each different type should have a different ambient music id.
  // -- See https://doomwiki.org/wiki/MUSINFO --
  if (thingtype >= 14100 && thingtype <= 14164)
  {
    iden_num = thingtype - 14100; // Ambient music to change
    i = MT_MUSICCHANGER;
  }
  else // killough 8/23/98: use table for faster lookup
    i = P_FindDoomedNum(thingtype);

  // phares 5/16/98:
  // Do not abort because of an unknown thing. Ignore it, but post a
  // warning message for the player.
  //
  // P_FindDoomedNum returns num_mobj_types (the grown count) on a miss,
  // which equals NUMMOBJTYPES only when the tables have not grown; compare
  // against the value actually returned so Heretic / DSDHacked misses are
  // caught instead of indexing past the array.

  if (i >= num_mobj_types)
  {
    /* ZDoom Doom-in-Hexen maps: a DECORATE actor with a new editor number
     * may be a reskinned base-game monster -- spawn it as the class its
     * inheritance chain roots in (chex3.wad's Larva and Quadrumpus).
     * ZDoom editor-only things (cameras, particle fountains, view stacks)
     * are skipped without the warning. */
    if (map_format.zdoom)
    {
      int alias;
      if (U_IsInertZDoomThing(thingtype))
        return;
      alias = U_DecorateAliasDoomedNum(thingtype);
      if (alias >= 0)
        i = P_FindDoomedNum(alias);
    }
    if (i >= num_mobj_types)
    {
      doom_printf("Unknown Thing type %i at (%i, %i)",mthing->type,mthing->x,mthing->y);
      return;
    }
  }

  // don't spawn keycards and players in deathmatch

  if (deathmatch && mobjinfo[i].flags & MF_NOTDMATCH)
    return;

  // don't spawn any monsters if -nomonsters

  if (nomonsters && (mobjinfo[i].flags & MF_ISMONSTER))
    return;

  // spawn it

  x = mthing->x << FRACBITS;
  y = mthing->y << FRACBITS;

  if (mobjinfo[i].flags & MF_SPAWNCEILING)
    z = ONCEILINGZ;
  else
    z = ONFLOORZ;

  mobj = P_SpawnMobj (x,y,z, i);
  mobj->spawnpoint = *mthing;
  mobj->iden_num = iden_num;

  /* Hexen-format maps (the Hexen game and ZDoom Doom-in-Hexen): carry the
   * thing id and scripted-special arguments onto the mobj (staged by
   * P_LoadThings).  In Hexen, P_CreateTIDList picks the tid up after the
   * level finishes loading. */
  if (map_format.hexen)
  {
    int a;
    mobj->tid = hexen_thing_tid;
    mobj->special = hexen_thing_special;
    for (a = 0; a < 5; a++)
      mobj->special_args[a] = hexen_thing_args[a];

    /* The hexen thing height is floor-relative (or ceiling-relative for
     * ceiling spawners): apply it like vanilla so mid-air placements --
     * hanging decorations, floating items -- land where the map says. */
    if (z == ONFLOORZ)
      mobj->z += hexen_thing_height << FRACBITS;
    else if (z == ONCEILINGZ)
      mobj->z -= hexen_thing_height << FRACBITS;
  }

  if (raven && (mobj->flags2 & MF2_FLOATBOB))
  {                             /* seed a random starting bob phase */
    mobj->health = P_Random(pr_heretic);
    if (hexen)
      mobj->special1.i = hexen_thing_height << FRACBITS;
  }

  if (mobj->tics > 0)
    mobj->tics = 1 + (P_Random (pr_spawnthing) % mobj->tics);

  /* The MBF friend bit (0x80) is Doom-format only: on hexen-format maps
   * that bit position is the third character-class visibility flag, which
   * editors set on every thing -- read raw, every chex3.wad monster
   * spawned MBF-friendly, allied with the player and excluded from the
   * kill count, and the friendly look path acquired the player sight-free
   * at spawn.  ZDoom's hexen-format friendly bit is 0x2000. */
  if (!(mobj->flags & MF_FRIEND) &&
      (map_format.hexen ? (map_format.zdoom &&
                           (options & MTF_ZDOOM_FRIENDLY))
                        : (options & MTF_FRIEND)) &&
      mbf_features)
  {
    mobj->flags |= MF_FRIEND;            // killough 10/98:
    P_UpdateThinker(&mobj->thinker);     // transfer friendliness flag
  }

  /* killough 7/20/98: exclude friends */
  if (!((mobj->flags ^ MF_COUNTKILL) & (MF_FRIEND | MF_COUNTKILL)))
    totalkills++;

  if (mobj->flags & MF_COUNTITEM)
    totalitems++;

  mobj->angle = ANG45 * (mthing->angle/45);
  if (options & MTF_AMBUSH)
    mobj->flags |= MF_AMBUSH;

  /* Hexen: dormant things spawn frozen (tics -1 stops the state machine)
   * and flagged so damage passes through; Thing_Activate wakes them. */
  if (hexen && options & MTF_HEXEN_DORMANT)
  {
    mobj->flags2 |= MF2_DORMANT;
    if (mobj->type == HEXEN_MT_ICEGUY)
      P_SetMobjState(mobj, HEXEN_S_ICEGUY_DORMANT);
    mobj->tics = -1;
  }
}


//
// GAME SPAWN FUNCTIONS
//

//
// P_SpawnPuff
//

extern fixed_t attackrange;

void P_SpawnPuff(fixed_t x,fixed_t y,fixed_t z)
{
  mobj_t* th;
  int t;

  /* Heretic and Hexen use their own puff actor (PuffType, set by the firing
   * weapon) rather than the Doom MT_PUFF, and play the puff's own sound.
   * Without this, a Raven hitscan/melee spawned the Doom puff -- which has no
   * valid Raven sprite/states -- so nothing drew at the impact (or, worse, a
   * stray Doom puff actor lingered).  Hexen additionally plays the puff's
   * seesound when a thing was hit, and gives the punch/hammer puffs a small
   * upward drift. */
  if (raven)
  {
    th = P_SpawnMobj(x, y, z + (P_SubRandom() << 10), PuffType);
    PuffSpawned = th;
    if (hexen && linetarget && th->info->seesound)
      S_StartMobjSound(th, th->info->seesound);
    else if (th->info->attacksound)
      S_StartMobjSound(th, th->info->attacksound);
    switch (PuffType)
    {
      case HERETIC_MT_BEAKPUFF:
      case HERETIC_MT_STAFFPUFF:
      case HEXEN_MT_PUNCHPUFF:
        th->momz = FRACUNIT;
        break;
      case HERETIC_MT_GAUNTLETPUFF1:
      case HERETIC_MT_GAUNTLETPUFF2:
      case HEXEN_MT_HAMMERPUFF:
        th->momz = (fixed_t)(FRACUNIT * 4 / 5); /* .8 */
        break;
      default:
        break;
    }
    PuffSpawned = th;
    return;
  }

  // killough 5/5/98: remove dependence on order of evaluation:
  t = P_Random(pr_spawnpuff);
  z += (t - P_Random(pr_spawnpuff))<<10;

  th = P_SpawnMobj (x,y,z, MT_PUFF);
  th->momz = FRACUNIT;
  th->tics -= P_Random(pr_spawnpuff)&3;

  if (th->tics < 1)
    th->tics = 1;

  // don't make punches spark on the wall

  if (attackrange == MELEERANGE)
    P_SetMobjState (th, S_PUFF3);
}

/* Hexen blood effects for hitscan/melee hits on fleshy things: a spray of
 * blood drops (shift 10, weight 3 per Raven's Hexen values) and, for the
 * axe, the big AXEBLOOD splat. */
/* Raven: the spray a ripper missile tears out of whatever it is passing
 * through. */
void P_RipperBlood(mobj_t *mo, mobj_t *bleeder)
{
  mobj_t *th;
  fixed_t x, y, z;

  x = mo->x + (P_SubRandom() << 12);
  y = mo->y + (P_SubRandom() << 12);
  z = mo->z + (P_SubRandom() << 12);
  th = P_SpawnMobj(x, y, z, hexen ? HEXEN_MT_BLOOD : HERETIC_MT_BLOOD);
  if (!hexen)
    th->flags |= MF_NOGRAVITY;
  th->momx = mo->momx >> 1;
  th->momy = mo->momy >> 1;
  th->tics += P_Random(pr_heretic) & 3;
}

void P_BloodSplatter(fixed_t x, fixed_t y, fixed_t z, mobj_t *originator)
{
  mobj_t *mo;

  mo = P_SpawnMobj(x, y, z, HEXEN_MT_BLOODSPLATTER);
  P_SetTarget(&mo->target, originator);
  mo->momx = P_SubRandom() << 10;
  mo->momy = P_SubRandom() << 10;
  mo->momz = FRACUNIT * 3;
}

void P_BloodSplatter2(fixed_t x, fixed_t y, fixed_t z, mobj_t *originator)
{
  mobj_t *mo;
  int r1, r2;

  r1 = P_Random(pr_heretic);
  r2 = P_Random(pr_heretic);
  mo = P_SpawnMobj(x + ((r2 - 128) << 11), y + ((r1 - 128) << 11), z,
                   HEXEN_MT_AXEBLOOD);
  P_SetTarget(&mo->target, originator);
}


//
// P_SpawnBlood
//
void P_SpawnBlood(fixed_t x,fixed_t y,fixed_t z,int damage)
{
  mobj_t* th;
  // killough 5/5/98: remove dependence on order of evaluation:
  int t = P_Random(pr_spawnblood);
  z += (t - P_Random(pr_spawnblood))<<10;
  th = P_SpawnMobj(x,y,z, MT_BLOOD);
  th->momz = FRACUNIT*2;
  th->tics -= P_Random(pr_spawnblood)&3;

  if (th->tics < 1)
    th->tics = 1;

  if (damage <= 12 && damage >= 9)
    P_SetMobjState (th,S_BLOOD2);
  else if (damage < 9)
    P_SetMobjState (th,S_BLOOD3);
}


//
// P_CheckMissileSpawn
// Moves the missile forward a bit
//  and possibly explodes it right there.
//

void P_CheckMissileSpawn (mobj_t* th)
{
  th->tics -= P_Random(pr_missile)&3;
  if (th->tics < 1)
    th->tics = 1;

  // move a little forward so an angle can
  // be computed if it immediately explodes

  th->x += (th->momx>>1);
  th->y += (th->momy>>1);
  th->z += (th->momz>>1);

  // killough 8/12/98: for non-missile objects (e.g. grenades)
  if (!(th->flags & MF_MISSILE) && mbf_features)
    return;

  // killough 3/15/98: no dropoff (really = don't care for missiles)

  if (!P_TryMove (th, th->x, th->y, FALSE))
    P_ExplodeMissile (th);
}


//
// P_SpawnMissile
//

mobj_t* P_SpawnMissile(mobj_t* source,mobj_t* dest,mobjtype_t type)
{
  mobj_t* th;
  angle_t an;
  int     dist;

  th = P_SpawnMobj (source->x,source->y,source->z + 4*8*FRACUNIT,type);
  if (!th)
    return NULL;

  if (th->info->seesound)
    S_StartSound (th, th->info->seesound);

  P_SetTarget(&th->target, source);    // where it came from
  an = R_PointToAngle2 (source->x, source->y, dest->x, dest->y);

  // fuzzy player

  if (dest->flags & MF_SHADOW)
    {  // killough 5/5/98: remove dependence on order of evaluation:
    int t = P_Random(pr_shadow);
    an += (t - P_Random(pr_shadow))<<20;
    }

  th->angle = an;
  an >>= ANGLETOFINESHIFT;
  th->momx = FixedMul (th->info->speed, finecosine[an]);
  th->momy = FixedMul (th->info->speed, finesine[an]);

  dist = P_AproxDistance (dest->x - source->x, dest->y - source->y);
  dist = dist / th->info->speed;

  if (dist < 1)
    dist = 1;

  th->momz = (dest->z - source->z) / dist;
  P_CheckMissileSpawn (th);

  return th;
}


//
// P_SpawnPlayerMissile
// Tries to aim at a nearby monster
//

mobj_t *P_SpawnPlayerMissile(mobj_t* source,mobjtype_t type)
{
  mobj_t *th;
  fixed_t x, y, z, slope = 0;

  // see which target is to be aimed at

  angle_t an = source->angle;

  // killough 7/19/98: autoaiming was not in original beta
  {
     // killough 8/2/98: prefer autoaiming at enemies
     uint64_t mask = mbf_features ? MF_FRIEND : 0;

     do
     {
        slope = P_AimLineAttack(source, an, 16*64*FRACUNIT, mask);
        if (!linetarget)
           slope = P_AimLineAttack(source, an += 1<<26, 16*64*FRACUNIT, mask);
        if (!linetarget)
           slope = P_AimLineAttack(source, an -= 2<<26, 16*64*FRACUNIT, mask);
        if (!linetarget)
           an = source->angle, slope = 0;
     }
     while (mask && (mask=0, !linetarget));  // killough 8/2/98
  }

  x = source->x;
  y = source->y;
  z = source->z + 4*8*FRACUNIT;
  if (raven && (source->flags2 & MF2_FEETARECLIPPED))
    z -= FOOTCLIPSIZE;          /* heretic: fire from sunken feet */

  /* heretic consumers (A_FireSkullRodPL2) read the spawned missile from
   * this global because they need it even if it exploded on spawn. */
  MissileMobj = th = P_SpawnMobj (x,y,z, type);
  if (!th)
    return NULL;

  if (th->info->seesound)
    S_StartSound (th, th->info->seesound);

  P_SetTarget(&th->target, source);
  th->angle = an;
  th->momx = FixedMul(th->info->speed,finecosine[an>>ANGLETOFINESHIFT]);
  th->momy = FixedMul(th->info->speed,finesine[an>>ANGLETOFINESHIFT]);
  th->momz = FixedMul(th->info->speed,slope);

  P_CheckMissileSpawn(th);
  return th;
  }

void P_BlasterMobjThinker(mobj_t * mobj)
{
    int i;
    fixed_t xfrac;
    fixed_t yfrac;
    fixed_t zfrac;
    fixed_t z;
    dbool changexy;

    mobj->PrevX = mobj->x;
    mobj->PrevY = mobj->y;
    mobj->PrevZ = mobj->z;

    // Handle movement
    if (mobj->momx || mobj->momy || (mobj->z != mobj->floorz) || mobj->momz)
    {
        xfrac = mobj->momx >> 3;
        yfrac = mobj->momy >> 3;
        zfrac = mobj->momz >> 3;
        changexy = xfrac || yfrac;
        for (i = 0; i < 8; i++)
        {
            if (changexy)
            {
                if (!P_TryMove(mobj, mobj->x + xfrac, mobj->y + yfrac, false))
                {               // Blocked move
                    P_ExplodeMissile(mobj);
                    return;
                }
            }
            mobj->z += zfrac;
            if (mobj->z <= mobj->floorz)
            {                   // Hit the floor
                mobj->z = mobj->floorz;
                P_HitFloor(mobj);
                P_ExplodeMissile(mobj);
                return;
            }
            if (mobj->z + mobj->height > mobj->ceilingz)
            {                   // Hit the ceiling
                mobj->z = mobj->ceilingz - mobj->height;
                P_ExplodeMissile(mobj);
                return;
            }
            if (changexy)
            {
                if (hexen)
                {
                    /* Hexen reuses this fast-mover thinker for two Mage
                     * missiles.  The Sapphire Wand bolt lays a light smoke
                     * trail; the Cleric flame missile (Frost?  no -- the
                     * Mage's MT_CFLAME) periodically drops a floor flame.
                     * Spawning Heretic's blaster smoke here instead left a
                     * permanent Heretic trail actor behind every wand shot. */
                    if (mobj->type == HEXEN_MT_MWAND_MISSILE &&
                        (P_Random(pr_heretic) < 128))
                    {
                        z = mobj->z - 8 * FRACUNIT;
                        if (z < mobj->floorz)
                            z = mobj->floorz;
                        P_SpawnMobj(mobj->x, mobj->y, z, HEXEN_MT_MWANDSMOKE);
                    }
                    else if (mobj->type != HEXEN_MT_MWAND_MISSILE &&
                             !--mobj->special1.i)
                    {
                        mobj_t *fl;
                        mobj->special1.i = 4;
                        z = mobj->z - 12 * FRACUNIT;
                        if (z < mobj->floorz)
                            z = mobj->floorz;
                        fl = P_SpawnMobj(mobj->x, mobj->y, z,
                                         HEXEN_MT_CFLAMEFLOOR);
                        if (fl)
                            fl->angle = mobj->angle;
                    }
                }
                else if (P_Random(pr_heretic) < 64)
                {
                    z = mobj->z - 8 * FRACUNIT;
                    if (z < mobj->floorz)
                    {
                        z = mobj->floorz;
                    }
                    P_SpawnMobj(mobj->x, mobj->y, z, HERETIC_MT_BLASTERSMOKE);
                }
            }
        }
    }
    // Advance the state
    if (mobj->tics != -1)
    {
        mobj->tics--;
        while (!mobj->tics)
        {
            if (!P_SetMobjState(mobj, mobj->state->nextstate))
            {                   // mobj was removed
                return;
            }
        }
    }
}
/* Heretic: spawn a player missile at an explicit angle, mirroring this
 * core's P_SpawnPlayerMissile autoaim (rather than dsda's aim_t system). */
mobj_t *P_SPMAngle(mobj_t *source, mobjtype_t type, angle_t angle)
{
  mobj_t *th;
  fixed_t x, y, z, slope = 0;
  angle_t an = angle;
  uint64_t mask = 0;

  slope = P_AimLineAttack(source, an, 16 * 64 * FRACUNIT, mask);
  if (!linetarget)
  {
    slope = P_AimLineAttack(source, an += 1 << 26, 16 * 64 * FRACUNIT, mask);
    if (!linetarget)
    {
      slope = P_AimLineAttack(source, an -= 2 << 26, 16 * 64 * FRACUNIT, mask);
      if (!linetarget)
      {
        an = angle;
        slope = 0;
      }
    }
  }

  x = source->x;
  y = source->y;
  z = source->z + 4 * 8 * FRACUNIT;
  if (source->flags2 & MF2_FEETARECLIPPED)
    z -= FOOTCLIPSIZE;

  th = P_SpawnMobj(x, y, z, type);
  if (th->info->seesound)
    S_StartSound(th, th->info->seesound);
  P_SetTarget(&th->target, source);
  th->angle = an;
  th->momx = FixedMul(th->info->speed, finecosine[an >> ANGLETOFINESHIFT]);
  th->momy = FixedMul(th->info->speed, finesine[an >> ANGLETOFINESHIFT]);
  th->momz = FixedMul(th->info->speed, slope);
  P_CheckMissileSpawn(th);
  return th;
}

/* Hexen: spawn a missile travelling at an explicit angle/speed/vertical
 * velocity (rather than aimed at a target).  Used by the Mage's Cone of
 * Shards, whose shards reproduce outward in fixed directions. */
mobj_t *P_SpawnMissileAngleSpeed(mobj_t *source, mobjtype_t type,
                                 angle_t angle, fixed_t momz, fixed_t speed)
{
  fixed_t z;
  mobj_t *mo;

  z = source->z - source->floorclip;
  mo = P_SpawnMobj(source->x, source->y, z, type);
  P_SetTarget(&mo->target, source);
  mo->angle = angle;
  angle >>= ANGLETOFINESHIFT;
  mo->momx = FixedMul(speed, finecosine[angle]);
  mo->momy = FixedMul(speed, finesine[angle]);
  mo->momz = momz;
  P_CheckMissileSpawn(mo);
  return mo;
}
mobj_t *P_SPMAngleXYZ(mobj_t *source, fixed_t x, fixed_t y, fixed_t z,
                      mobjtype_t type, angle_t angle)
{
  mobj_t *th;
  fixed_t slope = 0;
  angle_t an = angle;
  uint64_t mask = 0;

  slope = P_AimLineAttack(source, an, 16 * 64 * FRACUNIT, mask);
  if (!linetarget)
  {
    slope = P_AimLineAttack(source, an += 1 << 26, 16 * 64 * FRACUNIT, mask);
    if (!linetarget)
    {
      slope = P_AimLineAttack(source, an -= 2 << 26, 16 * 64 * FRACUNIT, mask);
      if (!linetarget)
      {
        an = angle;
        slope = 0;
      }
    }
  }

  z += 4 * 8 * FRACUNIT;
  if (source->flags2 & MF2_FEETARECLIPPED)
    z -= FOOTCLIPSIZE;

  th = P_SpawnMobj(x, y, z, type);
  if (th->info->seesound)
    S_StartSound(th, th->info->seesound);
  P_SetTarget(&th->target, source);
  th->angle = an;
  th->momx = FixedMul(th->info->speed, finecosine[an >> ANGLETOFINESHIFT]);
  th->momy = FixedMul(th->info->speed, finesine[an >> ANGLETOFINESHIFT]);
  th->momz = FixedMul(th->info->speed, slope);
  P_CheckMissileSpawn(th);
  return th;
}

/* Heretic: spawn a missile travelling at a fixed angle with explicit momz. */
mobj_t *P_SpawnMissileXYZ(fixed_t x, fixed_t y, fixed_t z,
                          mobj_t *source, mobj_t *dest, mobjtype_t type)
{
  mobj_t *th;
  angle_t an;
  int dist;

  z -= source->floorclip;
  th = P_SpawnMobj(x, y, z, type);
  if (th->info->seesound)
    S_StartSound(th, th->info->seesound);
  P_SetTarget(&th->target, source);   /* originator */
  an = R_PointToAngle2(source->x, source->y, dest->x, dest->y);
  if (dest->flags & MF_SHADOW)
  {                                   /* invisible target */
    an += P_SubRandom() << 21;
  }
  th->angle = an;
  an >>= ANGLETOFINESHIFT;
  th->momx = FixedMul(th->info->speed, finecosine[an]);
  th->momy = FixedMul(th->info->speed, finesine[an]);
  dist = P_AproxDistance(dest->x - source->x, dest->y - source->y);
  dist = dist / th->info->speed;
  if (dist < 1)
    dist = 1;
  th->momz = (dest->z - source->z) / dist;
  P_CheckMissileSpawn(th);
  return th;
}

mobj_t *P_SpawnMissileAngle(mobj_t *source, mobjtype_t type, angle_t angle, fixed_t momz)
{
  fixed_t z;
  mobj_t *mo;

  switch (type)
  {
    case HERETIC_MT_MNTRFX1:    /* Minotaur swing attack missile */
      z = source->z + 40 * FRACUNIT;
      break;
    case HERETIC_MT_MNTRFX2:    /* Minotaur floor fire missile */
      z = ONFLOORZ;
      break;
    case HERETIC_MT_SRCRFX1:    /* Sorcerer Demon fireball */
      z = source->z + 48 * FRACUNIT;
      break;
    default:
      z = source->z + 32 * FRACUNIT;
      break;
  }
  if (source->flags2 & MF2_FEETARECLIPPED)
    z -= FOOTCLIPSIZE;

  mo = P_SpawnMobj(source->x, source->y, z, type);
  if (mo->info->seesound)
    S_StartSound(mo, mo->info->seesound);
  P_SetTarget(&mo->target, source);
  mo->angle = angle;
  angle >>= ANGLETOFINESHIFT;
  mo->momx = FixedMul(mo->info->speed, finecosine[angle]);
  mo->momy = FixedMul(mo->info->speed, finesine[angle]);
  mo->momz = momz;
  P_CheckMissileSpawn(mo);
  return mo;
}


/* Heretic helpers ported from dsda-doom. */
int P_GetPlayerNum(player_t * player)
{
    int i;

    for (i = 0; i < MAXPLAYERS; i++)
    {
        if (player == &players[i])
        {
            return (i);
        }
    }
    return (0);
}

void P_ThrustMobj(mobj_t * mo, angle_t angle, fixed_t move)
{
    angle >>= ANGLETOFINESHIFT;
    mo->momx += FixedMul(move, finecosine[angle]);
    mo->momy += FixedMul(move, finesine[angle]);
}

dbool P_SeekerMissile(mobj_t * actor, mobj_t ** seekTarget, angle_t thresh, angle_t turnMax, dbool seekcenter)
{
    int dir;
    int dist;
    angle_t delta;
    angle_t angle;
    mobj_t *target;

    target = *seekTarget;
    if (target == NULL)
    {
        return (false);
    }
    if (!(target->flags & MF_SHOOTABLE))
    {                           // Target died
        *seekTarget = NULL;
        return (false);
    }
    dir = P_FaceMobj(actor, target, &delta);
    if (delta > thresh)
    {
        delta >>= 1;
        if (delta > turnMax)
        {
            delta = turnMax;
        }
    }
    if (dir)
    {                           // Turn clockwise
        actor->angle += delta;
    }
    else
    {                           // Turn counter clockwise
        actor->angle -= delta;
    }
    angle = actor->angle >> ANGLETOFINESHIFT;
    actor->momx = FixedMul(actor->info->speed, finecosine[angle]);
    actor->momy = FixedMul(actor->info->speed, finesine[angle]);
    if (actor->z + actor->height < target->z ||
        target->z + target->height < actor->z || seekcenter)
    {                           // Need to seek vertically
        dist = P_AproxDistance(target->x - actor->x, target->y - actor->y);
        dist = dist / actor->info->speed;
        if (dist < 1)
        {
            dist = 1;
        }
        actor->momz = (target->z + (seekcenter ? target->height/2 : 0) - actor->z) / dist;
    }
    return (true);
}

#ifndef ANGLE_MAX
#define ANGLE_MAX 0xffffffff
#endif
int P_FaceMobj(mobj_t * source, mobj_t * target, angle_t * delta)
{
    angle_t diff;
    angle_t angle1;
    angle_t angle2;

    angle1 = source->angle;
    angle2 = R_PointToAngle2(source->x, source->y, target->x, target->y);
    if (angle2 > angle1)
    {
        diff = angle2 - angle1;
        if (diff > ANG180)
        {
            *delta = ANGLE_MAX - diff;
            return (0);
        }
        else
        {
            *delta = diff;
            return (1);
        }
    }
    else
    {
        diff = angle1 - angle2;
        if (diff > ANG180)
        {
            *delta = ANGLE_MAX - diff;
            return (1);
        }
        else
        {
            *delta = diff;
            return (0);
        }
    }
}