/* Emacs style mode select   -*- C -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
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
 * DESCRIPTION:
 *   Hexen scripted line specials.
 *
 *   Hexen replaces Doom's "special + tag" linedef encoding with a byte
 *   special and five byte arguments.  Crossing, using or shooting a special
 *   line runs P_ExecuteHexenLineSpecial, which decodes the special and acts
 *   on the sectors named by its arguments.  This file implements the
 *   dispatcher and the door specials; the remaining special groups (floors,
 *   platforms, lights, teleports, ...) are added on top of it.
 *
 *-----------------------------------------------------------------------------*/

#include <string.h>

#include "doomstat.h"
#include "doomdef.h"
#include "r_state.h"
#include "p_spec.h"
#include "p_tick.h"
#include "s_sound.h"
#include "sounds.h"
#include "z_zone.h"
#include "m_random.h"
#include "hexen/p_spec_hexen.h"

/* Collapse the per-line arg array into a local byte[5] (mirrors dsda). */
#define COLLAPSE_SPECIAL_ARGS(dest, source) \
  { (dest)[0] = (source)[0]; (dest)[1] = (source)[1]; (dest)[2] = (source)[2]; \
    (dest)[3] = (source)[3]; (dest)[4] = (source)[4]; }

void P_ChangeSwitchTexture(line_t *line, int useAgain);

/* Hexen door movement types live in the shared vldoor_e enum (DREV_*). */

/* Iterate the sectors tagged with `tag`.  The Hexen loader leaves line->tag
 * at 0 and carries the tag in args[0], so match on the sector tag directly. */
#define HEXEN_FOR_TAGGED_SECTORS(secvar, tag) \
  for ((secvar) = -1; ((secvar) = P_FindSectorFromTag((tag), (secvar))) >= 0; )

static int P_FindSectorFromTag(int tag, int start)
{
  int i;
  for (i = start + 1; i < numsectors; i++)
    if (sectors[i].tag == tag)
      return i;
  return -1;
}

/* --- Doors ----------------------------------------------------------------
 *
 * Hexen doors reuse the vldoor_t thinker but with their own movement types
 * and an args-driven speed / wait.  Until the sound-sequence subsystem is in
 * place the door uses the engine's existing door sounds. */

void T_HexenVerticalDoor(vldoor_t *door)
{
  result_e res;

  switch (door->direction)
  {
    case 0:                     /* waiting at the top */
      if (!--door->topcountdown)
      {
        if (door->type == DREV_NORMAL)
        {
          door->direction = -1;
          S_StartSound((mobj_t *) &door->sector->soundorg, hexen_sfx_door_heavy_close);
        }
        else if (door->type == DREV_CLOSE30THENOPEN)
        {
          door->direction = 1;
        }
      }
      break;

    case 2:                     /* initial wait */
      if (!--door->topcountdown)
      {
        if (door->type == DREV_NORMAL)
          door->direction = 1;
      }
      break;

    case -1:                    /* moving down */
      res = T_MovePlane(door->sector, door->speed,
                        door->sector->floorheight, FALSE, 1, door->direction);
      if (res == RES_PASTDEST)
      {
        switch (door->type)
        {
          case DREV_NORMAL:
          case DREV_CLOSE:
            door->sector->ceilingdata = NULL;
            P_RemoveThinker(&door->thinker);
            break;
          case DREV_CLOSE30THENOPEN:
            door->direction = 0;
            door->topcountdown = 35 * 30;
            break;
          default:
            break;
        }
      }
      else if (res == RES_CRUSHED)
      {
        if (door->type != DREV_CLOSE)   /* don't reopen a plain close */
          door->direction = 1;
      }
      break;

    case 1:                     /* moving up */
      res = T_MovePlane(door->sector, door->speed,
                        door->topheight, FALSE, 1, door->direction);
      if (res == RES_PASTDEST)
      {
        switch (door->type)
        {
          case DREV_NORMAL:
            door->direction = 0;        /* wait at top */
            door->topcountdown = door->topwait;
            break;
          case DREV_CLOSE30THENOPEN:
          case DREV_OPEN:
            door->sector->ceilingdata = NULL;
            P_RemoveThinker(&door->thinker);
            break;
          default:
            break;
        }
      }
      break;
  }
}

int Hexen_EV_DoDoor(line_t *line, byte *args, vldoor_e type)
{
  int       secnum;
  int       rtn = 0;
  sector_t *sec;
  vldoor_t *door;
  fixed_t   speed = args[1] * (FRACUNIT / 8);

  HEXEN_FOR_TAGGED_SECTORS(secnum, args[0])
  {
    sec = &sectors[secnum];
    if (sec->floordata || sec->ceilingdata)
      continue;

    rtn = 1;
    door = Z_Malloc(sizeof(*door), PU_LEVEL, 0);
    memset(door, 0, sizeof(*door));
    P_AddThinker(&door->thinker);
    sec->ceilingdata = door;
    door->thinker.function.arg1 = (void (*)(void *))T_HexenVerticalDoor;
    door->sector = sec;
    door->line   = line;
    door->type   = type;
    door->speed  = speed;
    door->topwait = args[2];

    switch (type)
    {
      case DREV_CLOSE:
        door->topheight = P_FindLowestCeilingSurrounding(sec) - 4 * FRACUNIT;
        door->direction = -1;
        S_StartSound((mobj_t *) &sec->soundorg, hexen_sfx_door_heavy_close);
        break;
      case DREV_CLOSE30THENOPEN:
        door->topheight = sec->ceilingheight;
        door->direction = -1;
        break;
      case DREV_NORMAL:
      case DREV_OPEN:
        door->direction = 1;
        door->topheight = P_FindLowestCeilingSurrounding(sec) - 4 * FRACUNIT;
        S_StartSound((mobj_t *) &sec->soundorg, hexen_sfx_door_open);
        break;
      default:
        break;
    }
  }
  return rtn;
}

dbool Hexen_EV_VerticalDoor(line_t *line, mobj_t *thing)
{
  sector_t *sec;
  vldoor_t *door;

  /* only the front side can be used; the door sector is on the back */
  sec = sides[line->sidenum[1]].sector;
  if (!sec || sec->floordata || sec->ceilingdata)
    return false;

  door = Z_Malloc(sizeof(*door), PU_LEVEL, 0);
  memset(door, 0, sizeof(*door));
  P_AddThinker(&door->thinker);
  sec->ceilingdata = door;
  door->thinker.function.arg1 = (void (*)(void *))T_HexenVerticalDoor;
  door->sector    = sec;
  door->line      = line;
  door->direction = 1;
  door->type      = (line->special == 11) ? DREV_OPEN : DREV_NORMAL;
  if (line->special == 11)
    line->special = 0;
  door->speed    = line->args[1] * (FRACUNIT / 8);
  door->topwait  = line->args[2];
  door->topheight = P_FindLowestCeilingSurrounding(sec) - 4 * FRACUNIT;
  S_StartSound((mobj_t *) &sec->soundorg, hexen_sfx_door_open);
  return true;
}

/* --- Locked doors ---------------------------------------------------------
 * args[3] carries the lock id (key required).  Key handling is part of the
 * inventory/key layer; for now an unlocked check passes through. */
static dbool CheckedLockedDoor(mobj_t *mo, byte lock)
{
  if (!mo || !mo->player)
    return false;
  if (!lock)
    return true;
  /* TODO: verify the player holds key `lock` once Hexen keys are wired. */
  return true;
}

/* --- Dispatcher ----------------------------------------------------------- */

dbool P_ExecuteHexenLineSpecial(int special, byte *args, line_t *line,
                                int side, mobj_t *mo)
{
  dbool ok = false;

  switch (special)
  {
    case 10:                    /* Door_Close */
      ok = Hexen_EV_DoDoor(line, args, DREV_CLOSE);
      break;
    case 11:                    /* Door_Open */
      ok = args[0] ? Hexen_EV_DoDoor(line, args, DREV_OPEN)
                   : Hexen_EV_VerticalDoor(line, mo);
      break;
    case 12:                    /* Door_Raise */
      ok = args[0] ? Hexen_EV_DoDoor(line, args, DREV_NORMAL)
                   : Hexen_EV_VerticalDoor(line, mo);
      break;
    case 13:                    /* Door_LockedRaise */
      if (CheckedLockedDoor(mo, args[3]))
        ok = args[0] ? Hexen_EV_DoDoor(line, args, DREV_NORMAL)
                     : Hexen_EV_VerticalDoor(line, mo);
      break;
    default:
      /* The remaining special groups (floors, platforms, lights, teleports,
       * polyobjects, ACS, ...) are implemented on top of this dispatcher. */
      break;
  }

  (void) side;
  return ok;
}

/* --- Activation -----------------------------------------------------------
 *
 * A Hexen linedef carries its activation class in flag bits 10-12 and a
 * repeat bit at 9.  Running a special only happens when the way the line was
 * triggered (cross / use / impact) matches its class.  A successful one-shot
 * special clears itself; a successful use also flips the switch texture. */

static dbool P_ActivateHexenLine(line_t *line, mobj_t *mo, int side,
                                 int activation)
{
  byte  args[5];
  dbool repeat;
  dbool ok;

  if (!line->special)
    return false;
  if (GET_SPAC(line->flags) != activation)
    return false;

  COLLAPSE_SPECIAL_ARGS(args, line->args);
  ok = P_ExecuteHexenLineSpecial(line->special, args, line, side, mo);
  if (!ok)
    return false;

  repeat = (line->flags & ML_REPEATSPECIAL) != 0;
  if (!repeat)
    line->special = 0;

  if (activation == SPAC_USE)
    P_ChangeSwitchTexture(line, repeat);

  return true;
}

void P_CrossHexenSpecialLine(line_t *line, int side, mobj_t *thing)
{
  if (thing->player)
    P_ActivateHexenLine(line, thing, side, SPAC_CROSS);
  else
    P_ActivateHexenLine(line, thing, side, SPAC_MCROSS);
}

void P_ShootHexenSpecialLine(mobj_t *thing, line_t *line)
{
  P_ActivateHexenLine(line, thing, 0, SPAC_IMPACT);
}

dbool P_UseHexenSpecialLine(mobj_t *thing, line_t *line, int side)
{
  return P_ActivateHexenLine(line, thing, side, SPAC_USE);
}
