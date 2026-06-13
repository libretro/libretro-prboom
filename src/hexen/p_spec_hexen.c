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
#include "r_main.h"
#include "p_spec.h"
#include "p_enemy.h"
#include "hexen/po_man.h"
#include "g_game.h"
#include "lprintf.h"
#include "p_tick.h"
#include "p_map.h"
#include "r_demo.h"
#include "s_sound.h"
#include "sounds.h"
#include "z_zone.h"
#include "m_random.h"
#include "hexen/p_spec_hexen.h"
#include "map_format.h"
#include "hexen/p_lightning.h"
#include "hexen/sn_sonix.h"
#include <stdio.h>
#include "p_inter.h"
#include "hexen/p_acs.h"
#include "heretic/p_action.h"

/* These have no shared prototype header in this fork. */
void P_ThrustMobj(mobj_t *mo, angle_t angle, fixed_t move);
void P_DamageMobj(mobj_t *target, mobj_t *inflictor, mobj_t *source,
                  int damage);

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

int P_FindSectorFromTag(int tag, int start)
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

/* --- Mover sounds ----------------------------------------------------------
 * In the Hexen game these forward to the sound-sequence layer.  ZDoom
 * Doom-in-Hexen maps run the same movers in a Doom game, where no sequence
 * data exists (and sector seqType is meaningless), so play the classic Doom
 * point sounds instead: doors by direction, platform strokes pstart/pstop;
 * movers whose Doom thinkers already emit their own sounds pass doom_sfx 0. */

static void Hexen_DoorStartSound(sector_t *sec, int direction)
{
  if (hexen)
    SN_StartSequence((mobj_t *) &sec->soundorg, SEQ_DOOR_STONE + sec->seqType);
  else
    S_StartSound((mobj_t *) &sec->soundorg,
                 direction > 0 ? g_sfx_doropn : g_sfx_dorcls);
}

static void Hexen_MoverStartSound(sector_t *sec, int doom_sfx)
{
  if (hexen)
    SN_StartSequence((mobj_t *) &sec->soundorg, SEQ_PLATFORM + sec->seqType);
  else if (doom_sfx)
    S_StartSound((mobj_t *) &sec->soundorg, doom_sfx);
}

static void Hexen_MoverStopSound(sector_t *sec, int doom_sfx)
{
  if (hexen)
    SN_StopSequence((mobj_t *) &sec->soundorg);
  else if (doom_sfx)
    S_StartSound((mobj_t *) &sec->soundorg, doom_sfx);
}

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
          Hexen_DoorStartSound(door->sector, -1);
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
        Hexen_MoverStopSound(door->sector, 0);
        switch (door->type)
        {
          case DREV_NORMAL:
          case DREV_CLOSE:
            door->sector->ceilingdata = NULL;
            P_TagFinished(door->sector->tag);
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
        Hexen_MoverStopSound(door->sector, 0);
        switch (door->type)
        {
          case DREV_NORMAL:
            door->direction = 0;        /* wait at top */
            door->topcountdown = door->topwait;
            break;
          case DREV_CLOSE30THENOPEN:
          case DREV_OPEN:
            door->sector->ceilingdata = NULL;
            P_TagFinished(door->sector->tag);
            P_RemoveThinker(&door->thinker);
            break;
          default:
            break;
        }
      }
      break;
  }
}

int Hexen_EV_DoDoor(line_t *line, int *args, vldoor_e type)
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
        Hexen_DoorStartSound(sec, -1);
        break;
      case DREV_CLOSE30THENOPEN:
        door->topheight = sec->ceilingheight;
        door->direction = -1;
        break;
      case DREV_NORMAL:
      case DREV_OPEN:
        door->direction = 1;
        door->topheight = P_FindLowestCeilingSurrounding(sec) - 4 * FRACUNIT;
        Hexen_DoorStartSound(sec, 1);
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
  Hexen_DoorStartSound(sec, 1);
  return true;
}

/* --- Locked doors ---------------------------------------------------------
 * args[3] carries the lock id (key required).  Key handling is part of the
 * inventory/key layer; for now an unlocked check passes through. */
static dbool CheckedLockedDoor(mobj_t *mo, byte lock)
{
  static char lockedBuffer[80];

  if (!mo || !mo->player)
    return false;
  if (!lock)
    return true;
  if (lock > 11)
    return false;
  if (!mo->player->cards[lock - 1])
  {
    snprintf(lockedBuffer, sizeof(lockedBuffer),
             "YOU NEED THE %s", TextKeyMessages[lock - 1]);
    mo->player->message = lockedBuffer;
    S_StartSound(mo, hexen_sfx_door_locked);
    return false;
  }
  return true;
}

/* --- Generic (Boom-generalized) door ---------------------------------------
 *
 * Special 202 Generic_Door(tag, speed, kind, delay, lock) encapsulates Boom's
 * generalized doors.  kind selects the motion (0 open-wait-close, 1 open-stay,
 * 2 close-stay, 3 close-wait-open); delay is the wait in octics; a zero tag
 * means the local door whose sector is on the line's back side.  This builds a
 * vertical-door thinker directly so the techdemo's use-activated chamber doors
 * (which ship as Generic_Door rather than Door_Raise) actually open.  The
 * +128 light-tag variant is not modelled; it falls through as a plain door. */
static dbool Hexen_EV_GenericDoor(line_t *line, int *args, mobj_t *mo)
{
  int      kind  = args[2] & 0x7f;     /* strip the 0x80 light-tag flag */
  fixed_t  speed = args[1] * (FRACUNIT / 8);
  int      wait  = args[3] * 8;        /* delay is in octics */
  vldoor_e type;
  dbool    rtn = false;

  if (!CheckedLockedDoor(mo, (byte)args[4]))
    return false;

  switch (kind)
  {
    case 1:  type = DREV_OPEN;            break;  /* open and stay */
    case 2:  type = DREV_CLOSE;           break;  /* close and stay */
    case 3:  type = DREV_CLOSE30THENOPEN; break;  /* close, wait, open */
    default: type = DREV_NORMAL;          break;  /* open, wait, close */
  }

  if (args[0] != 0)
  {
    /* tagged: act on every sector carrying the tag */
    int       secnum;
    sector_t *sec;
    HEXEN_FOR_TAGGED_SECTORS(secnum, args[0])
    {
      vldoor_t *door;
      sec = &sectors[secnum];
      if (sec->floordata || sec->ceilingdata)
        continue;
      rtn = true;
      door = Z_Malloc(sizeof(*door), PU_LEVEL, 0);
      memset(door, 0, sizeof(*door));
      P_AddThinker(&door->thinker);
      sec->ceilingdata = door;
      door->thinker.function.arg1 = (void (*)(void *))T_HexenVerticalDoor;
      door->sector  = sec;
      door->line    = line;
      door->type    = type;
      door->speed   = speed;
      door->topwait = wait;
      if (type == DREV_CLOSE || type == DREV_CLOSE30THENOPEN)
      {
        door->topheight = (type == DREV_CLOSE)
          ? P_FindLowestCeilingSurrounding(sec) - 4 * FRACUNIT
          : sec->ceilingheight;
        door->direction = -1;
        if (type == DREV_CLOSE)
          Hexen_DoorStartSound(sec, -1);
      }
      else
      {
        door->topheight = P_FindLowestCeilingSurrounding(sec) - 4 * FRACUNIT;
        door->direction = 1;
        Hexen_DoorStartSound(sec, 1);
      }
    }
    return rtn;
  }
  else
  {
    /* local door: the moving sector is on the line's back side */
    sector_t *sec;
    vldoor_t *door;
    if (line->sidenum[1] == NO_INDEX)
      return false;
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
    door->type      = type;
    door->speed     = speed;
    door->topwait   = wait;
    if (type == DREV_CLOSE || type == DREV_CLOSE30THENOPEN)
    {
      door->topheight = (type == DREV_CLOSE)
        ? P_FindLowestCeilingSurrounding(sec) - 4 * FRACUNIT
        : sec->ceilingheight;
      door->direction = -1;
      if (type == DREV_CLOSE)
        Hexen_DoorStartSound(sec, -1);
    }
    else
    {
      door->topheight = P_FindLowestCeilingSurrounding(sec) - 4 * FRACUNIT;
      door->direction = 1;
      Hexen_DoorStartSound(sec, 1);
    }
    return true;
  }
}

/* --- Floors ---------------------------------------------------------------
 *
 * Value-driven floor movers.  These reuse the engine's T_MoveFloor thinker,
 * which moves the sector floor to floordestheight and then removes itself;
 * the Hexen movement types fall through its texture/special switch unchanged.
 * Until the sound-sequence subsystem lands the movement sound is the engine's
 * default (sfx_stnmov inside T_MoveFloor). */
int Hexen_EV_DoFloor(line_t *line, int *args, floor_e floortype)
{
  int          secnum;
  int          rtn = 0;
  sector_t    *sec;
  floormove_t *floor;
  fixed_t      speed = args[1] * (FRACUNIT / 8);

  (void) line;

  HEXEN_FOR_TAGGED_SECTORS(secnum, args[0])
  {
    sec = &sectors[secnum];
    if (sec->floordata || sec->ceilingdata)
      continue;

    rtn = 1;
    floor = Z_Malloc(sizeof(*floor), PU_LEVEL, 0);
    memset(floor, 0, sizeof(*floor));
    P_AddThinker(&floor->thinker);
    sec->floordata = floor;
    floor->thinker.function.arg1 = (void (*)(void *))T_MoveFloor;
    floor->type   = floortype;
    floor->crush  = FALSE;
    floor->sector = sec;
    floor->speed  = speed;
    if (floortype == FLEV_LOWERTIMES8INSTANT ||
        floortype == FLEV_RAISETIMES8INSTANT)
      floor->speed = 2000 << FRACBITS;

    switch (floortype)
    {
      case FLEV_LOWERFLOOR:
        floor->direction = -1;
        floor->floordestheight = P_FindHighestFloorSurrounding(sec);
        break;
      case FLEV_LOWERFLOORTOLOWEST:
        floor->direction = -1;
        floor->floordestheight = P_FindLowestFloorSurrounding(sec);
        break;
      case FLEV_LOWERFLOORBYVALUE:
        floor->direction = -1;
        floor->floordestheight = sec->floorheight - args[2] * FRACUNIT;
        break;
      case FLEV_LOWERTIMES8INSTANT:
      case FLEV_LOWERBYVALUETIMES8:
        floor->direction = -1;
        floor->floordestheight = sec->floorheight - args[2] * FRACUNIT * 8;
        break;
      case FLEV_RAISEFLOORCRUSH:
        floor->crush = args[2];       /* crush damage */
        floor->direction = 1;
        floor->floordestheight = sec->ceilingheight - 8 * FRACUNIT;
        break;
      case FLEV_RAISEFLOOR:
        floor->direction = 1;
        floor->floordestheight = P_FindLowestCeilingSurrounding(sec);
        if (floor->floordestheight > sec->ceilingheight)
          floor->floordestheight = sec->ceilingheight;
        break;
      case FLEV_RAISEFLOORTONEAREST:
        floor->direction = 1;
        floor->floordestheight =
          P_FindNextHighestFloor(sec, sec->floorheight);
        break;
      case FLEV_RAISEFLOORBYVALUE:
        floor->direction = 1;
        floor->floordestheight = sec->floorheight + args[2] * FRACUNIT;
        break;
      case FLEV_RAISETIMES8INSTANT:
      case FLEV_RAISEBYVALUETIMES8:
        floor->direction = 1;
        floor->floordestheight = sec->floorheight + args[2] * FRACUNIT * 8;
        break;
      case FLEV_MOVETOVALUETIMES8:
      case FLEV_MOVETOVALUE:
        floor->floordestheight = args[2] * FRACUNIT;
        if (floortype == FLEV_MOVETOVALUETIMES8)
          floor->floordestheight *= 8;
        if (args[3])
          floor->floordestheight = -floor->floordestheight;
        if (floor->floordestheight > sec->floorheight)
          floor->direction = 1;
        else if (floor->floordestheight < sec->floorheight)
          floor->direction = -1;
        else
        {
          /* already at the target: undo this thinker */
          sec->floordata = NULL;
          P_RemoveThinker(&floor->thinker);
          rtn = 0;
        }
        break;
      default:
        sec->floordata = NULL;
        P_RemoveThinker(&floor->thinker);
        rtn = 0;
        break;
    }
    if (sec->floordata == floor)
      Hexen_MoverStartSound(sec, 0); /* doom floors sound in T_MoveFloor */
  }
  return rtn;
}

/* --- Ceilings -------------------------------------------------------------
 *
 * A dedicated Hexen ceiling thinker (the Doom T_MoveCeiling does not remove
 * plain movers at their destination, and its crusher handling differs).  One-
 * shot movers stop on arrival; crush-and-raise reverses and keeps cycling;
 * crush-raise-and-stay reverses up after one down stroke. */
void T_HexenMoveCeiling(ceiling_t *ceiling)
{
  result_e res;

  switch (ceiling->direction)
  {
    case 1:                     /* up */
      res = T_MovePlane(ceiling->sector, ceiling->speed,
                        ceiling->topheight, FALSE, 1, ceiling->direction);
      if (res == RES_PASTDEST)
      {
        if (ceiling->type == CLEV_CRUSHANDRAISE)
        {
          ceiling->direction = -1;
          ceiling->speed = ceiling->speed * 2;
        }
        else
        {
          Hexen_MoverStopSound(ceiling->sector, 0);
          P_TagFinished(ceiling->sector->tag);
          P_RemoveActiveCeiling(ceiling);
        }
      }
      break;

    case -1:                    /* down */
      res = T_MovePlane(ceiling->sector, ceiling->speed,
                        ceiling->bottomheight, ceiling->crush, 1,
                        ceiling->direction);
      if (res == RES_PASTDEST)
      {
        if (ceiling->type == CLEV_CRUSHANDRAISE ||
            ceiling->type == CLEV_CRUSHRAISEANDSTAY)
        {
          ceiling->direction = 1;
          ceiling->speed = ceiling->speed / 2;
        }
        else
        {
          Hexen_MoverStopSound(ceiling->sector, 0);
          P_TagFinished(ceiling->sector->tag);
          P_RemoveActiveCeiling(ceiling);
        }
      }
      /* crushed: the crusher types simply keep pushing */
      break;
  }
}

int Hexen_EV_DoCeiling(line_t *line, int *args, ceiling_e type)
{
  int        secnum;
  int        rtn = 0;
  sector_t  *sec;
  ceiling_t *ceiling;

  (void) line;

  HEXEN_FOR_TAGGED_SECTORS(secnum, args[0])
  {
    sec = &sectors[secnum];
    if (sec->floordata || sec->ceilingdata)
      continue;

    rtn = 1;
    ceiling = Z_Malloc(sizeof(*ceiling), PU_LEVEL, 0);
    memset(ceiling, 0, sizeof(*ceiling));
    P_AddThinker(&ceiling->thinker);
    sec->ceilingdata = ceiling;
    ceiling->thinker.function.arg1 = (void (*)(void *))T_HexenMoveCeiling;
    ceiling->sector = sec;
    ceiling->crush  = FALSE;
    ceiling->speed  = args[1] * (FRACUNIT / 8);

    switch (type)
    {
      case CLEV_CRUSHRAISEANDSTAY:
        ceiling->crush = args[2];     /* crush damage */
        ceiling->topheight = sec->ceilingheight;
        ceiling->bottomheight = sec->floorheight + 8 * FRACUNIT;
        ceiling->direction = -1;
        break;
      case CLEV_CRUSHANDRAISE:
        ceiling->topheight = sec->ceilingheight;
        ceiling->crush = args[2];     /* crush damage */
        ceiling->bottomheight = sec->floorheight + 8 * FRACUNIT;
        ceiling->direction = -1;
        break;
      case CLEV_LOWERANDCRUSH:
        ceiling->crush = args[2];     /* crush damage */
        ceiling->bottomheight = sec->floorheight + 8 * FRACUNIT;
        ceiling->direction = -1;
        break;
      case CLEV_LOWERTOFLOOR:
        ceiling->bottomheight = sec->floorheight;
        ceiling->direction = -1;
        break;
      case CLEV_RAISETOHIGHEST:
        ceiling->topheight = P_FindHighestCeilingSurrounding(sec);
        ceiling->direction = 1;
        break;
      case CLEV_LOWERBYVALUE:
        ceiling->bottomheight = sec->ceilingheight - args[2] * FRACUNIT;
        ceiling->direction = -1;
        break;
      case CLEV_LOWERTIMES8INSTANT:
        ceiling->bottomheight = sec->ceilingheight - args[2] * FRACUNIT * 8;
        ceiling->direction = -1;
        ceiling->speed = 2000 << FRACBITS;
        break;
      case CLEV_RAISETIMES8INSTANT:
        ceiling->topheight = sec->ceilingheight + args[2] * FRACUNIT * 8;
        ceiling->direction = 1;
        ceiling->speed = 2000 << FRACBITS;
        break;
      case CLEV_RAISEBYVALUE:
        ceiling->topheight = sec->ceilingheight + args[2] * FRACUNIT;
        ceiling->direction = 1;
        break;
      case CLEV_MOVETOVALUETIMES8:
      {
        int destHeight = args[2] * FRACUNIT * 8;
        if (args[3])
          destHeight = -destHeight;
        if (sec->ceilingheight <= destHeight)
        {
          ceiling->direction = 1;
          ceiling->topheight = destHeight;
          if (sec->ceilingheight == destHeight)
            rtn = 0;
        }
        else
        {
          ceiling->direction = -1;
          ceiling->bottomheight = destHeight;
        }
        break;
      }
      case CLEV_MOVETOVALUE:
      {
        /* Ceiling_MoveToValue(tag, speed, height, negative): move the
         * ceiling to an absolute height given in raw map units. */
        int destHeight = args[2] * FRACUNIT;
        if (args[3])
          destHeight = -destHeight;
        if (sec->ceilingheight <= destHeight)
        {
          ceiling->direction = 1;
          ceiling->topheight = destHeight;
          if (sec->ceilingheight == destHeight)
            rtn = 0;
        }
        else
        {
          ceiling->direction = -1;
          ceiling->bottomheight = destHeight;
        }
        break;
      }
      case CLEV_RAISETONEAREST:
        /* Ceiling_RaiseToNearest: raise to the next higher neighbour. */
        ceiling->topheight =
          P_FindNextHighestCeiling(sec, sec->ceilingheight);
        if (ceiling->topheight <= sec->ceilingheight)
          rtn = 0;
        else
          ceiling->direction = 1;
        break;
      default:
        rtn = 0;
        break;
    }

    ceiling->tag  = sec->tag;
    ceiling->type = type;

    if (rtn)
    {
      P_AddActiveCeiling(ceiling);
      Hexen_MoverStartSound(sec, 0);
    }
    else
    {
      sec->ceilingdata = NULL;
      P_RemoveThinker(&ceiling->thinker);
    }
  }
  return rtn;
}

int Hexen_EV_CeilingCrushStop(line_t *line, int *args)
{
  ceilinglist_t *cl;

  (void) line;

  for (cl = activeceilings; cl; cl = cl->next)
  {
    ceiling_t *ceiling = cl->ceiling;
    if (ceiling->tag == args[0])
    {
      Hexen_MoverStopSound(ceiling->sector, 0);
      P_RemoveActiveCeiling(ceiling);
      return 1;
    }
  }
  return 0;
}

/* --- Lights ---------------------------------------------------------------
 *
 * Per-sector light-level effects: instant raise/lower/change, plus animated
 * fade / glow / flicker / strobe driven by a small thinker.  No geometry
 * moves, so these need no interpolation. */
void T_HexenLight(light_t *light)
{
  if (light->count)
  {
    light->count--;
    return;
  }
  switch (light->type)
  {
    case LITE_FADE:
      light->sector->lightlevel =
        ((light->sector->lightlevel << FRACBITS) + light->value2) >> FRACBITS;
      if (light->tics2 == 1)
      {
        if (light->sector->lightlevel >= light->value1)
        {
          light->sector->lightlevel = light->value1;
          P_RemoveThinker(&light->thinker);
        }
      }
      else if (light->sector->lightlevel <= light->value1)
      {
        light->sector->lightlevel = light->value1;
        P_RemoveThinker(&light->thinker);
      }
      break;
    case LITE_GLOW:
      light->sector->lightlevel =
        ((light->sector->lightlevel << FRACBITS) + light->tics1) >> FRACBITS;
      if (light->tics2 == 1)
      {
        if (light->sector->lightlevel >= light->value1)
        {
          light->sector->lightlevel = light->value1;
          light->tics1 = -light->tics1;
          light->tics2 = -1;
        }
      }
      else if (light->sector->lightlevel <= light->value2)
      {
        light->sector->lightlevel = light->value2;
        light->tics1 = -light->tics1;
        light->tics2 = 1;
      }
      break;
    case LITE_FLICKER:
      if (light->sector->lightlevel == light->value1)
      {
        light->sector->lightlevel = light->value2;
        light->count = (P_Random(pr_heretic) & 7) + 1;
      }
      else
      {
        light->sector->lightlevel = light->value1;
        light->count = (P_Random(pr_heretic) & 31) + 1;
      }
      break;
    case LITE_STROBE:
      if (light->sector->lightlevel == light->value1)
      {
        light->sector->lightlevel = light->value2;
        light->count = light->tics2;
      }
      else
      {
        light->sector->lightlevel = light->value1;
        light->count = light->tics1;
      }
      break;
    default:
      break;
  }
}

int EV_SpawnLight(line_t *line, int *args, lighttype_t type)
{
  int       secnum;
  int       rtn = 0;
  sector_t *sec;
  light_t  *light;
  dbool     think;
  int       arg1 = args[1];
  int       arg2 = args[2];
  int       arg3 = args[3];
  int       arg4 = args[4];

  (void) line;

  HEXEN_FOR_TAGGED_SECTORS(secnum, args[0])
  {
    think = false;
    sec = &sectors[secnum];

    light = Z_Malloc(sizeof(*light), PU_LEVEL, 0);
    memset(light, 0, sizeof(*light));
    light->type = type;
    light->sector = sec;
    light->count = 0;
    rtn = 1;

    switch (type)
    {
      case LITE_RAISEBYVALUE:
        sec->lightlevel += arg1;
        if (sec->lightlevel > 255) sec->lightlevel = 255;
        break;
      case LITE_LOWERBYVALUE:
        sec->lightlevel -= arg1;
        if (sec->lightlevel < 0) sec->lightlevel = 0;
        break;
      case LITE_CHANGETOVALUE:
        sec->lightlevel = arg1;
        if (sec->lightlevel < 0) sec->lightlevel = 0;
        else if (sec->lightlevel > 255) sec->lightlevel = 255;
        break;
      case LITE_FADE:
        think = true;
        light->value1 = arg1;       /* destination lightlevel */
        light->value2 = FixedDiv((arg1 - sec->lightlevel) << FRACBITS,
                                 arg2 << FRACBITS);  /* delta */
        light->tics2 = (sec->lightlevel <= arg1) ? 1 : -1;
        break;
      case LITE_GLOW:
        think = true;
        light->value1 = arg1;       /* upper */
        light->value2 = arg2;       /* lower */
        light->tics1 = FixedDiv((arg1 - sec->lightlevel) << FRACBITS,
                                arg3 << FRACBITS);   /* delta */
        light->tics2 = (sec->lightlevel <= arg1) ? 1 : -1;
        break;
      case LITE_FLICKER:
        think = true;
        light->value1 = arg1;
        light->value2 = arg2;
        sec->lightlevel = light->value1;
        light->count = (P_Random(pr_heretic) & 64) + 1;
        break;
      case LITE_STROBE:
        think = true;
        light->value1 = arg1;
        light->value2 = arg2;
        light->tics1 = arg3;
        light->tics2 = arg4;
        light->count = arg3;
        sec->lightlevel = light->value1;
        break;
      default:
        rtn = 0;
        break;
    }

    if (think)
    {
      P_AddThinker(&light->thinker);
      light->thinker.function.arg1 = (void (*)(void *))T_HexenLight;
    }
    else
      Z_Free(light);
  }
  return rtn;
}

/* --- Pillars --------------------------------------------------------------
 *
 * A pillar drives a sector's floor and ceiling together: build closes them to
 * a meeting height, open separates a closed sector.  The two planes move at
 * independently scaled speeds so they arrive together. */
void T_HexenBuildPillar(pillar_t *pillar)
{
  result_e res1;
  result_e res2;

  res1 = T_MovePlane(pillar->sector, pillar->floorSpeed, pillar->floordest,
                     pillar->crush, 0, pillar->direction);
  res2 = T_MovePlane(pillar->sector, pillar->ceilingSpeed, pillar->ceilingdest,
                     pillar->crush, 1, -pillar->direction);
  if (res1 == RES_PASTDEST && res2 == RES_PASTDEST)
  {
    pillar->sector->floordata = NULL;
    Hexen_MoverStopSound(pillar->sector, 0);
    P_TagFinished(pillar->sector->tag);
    P_RemoveThinker(&pillar->thinker);
  }
}

int EV_BuildPillar(line_t *line, int *args, int crush)
{
  int       secnum;
  int       rtn = 0;
  sector_t *sec;
  pillar_t *pillar;
  int       newHeight;

  (void) line;

  HEXEN_FOR_TAGGED_SECTORS(secnum, args[0])
  {
    sec = &sectors[secnum];
    if (sec->floordata || sec->ceilingdata)
      continue;
    if (sec->floorheight == sec->ceilingheight)
      continue;                 /* already closed */

    rtn = 1;
    if (!args[2])
      newHeight = sec->floorheight +
        ((sec->ceilingheight - sec->floorheight) / 2);
    else
      newHeight = sec->floorheight + (args[2] << FRACBITS);

    pillar = Z_Malloc(sizeof(*pillar), PU_LEVEL, 0);
    memset(pillar, 0, sizeof(*pillar));
    sec->floordata = pillar;
    P_AddThinker(&pillar->thinker);
    pillar->thinker.function.arg1 = (void (*)(void *))T_HexenBuildPillar;
    pillar->sector = sec;

    if (!args[2])
      pillar->ceilingSpeed = pillar->floorSpeed = args[1] * (FRACUNIT / 8);
    else if (newHeight - sec->floorheight > sec->ceilingheight - newHeight)
    {
      pillar->floorSpeed = args[1] * (FRACUNIT / 8);
      pillar->ceilingSpeed = FixedMul(sec->ceilingheight - newHeight,
                                      FixedDiv(pillar->floorSpeed,
                                               newHeight - sec->floorheight));
    }
    else
    {
      pillar->ceilingSpeed = args[1] * (FRACUNIT / 8);
      pillar->floorSpeed = FixedMul(newHeight - sec->floorheight,
                                    FixedDiv(pillar->ceilingSpeed,
                                             sec->ceilingheight - newHeight));
    }
    pillar->floordest   = newHeight;
    pillar->ceilingdest = newHeight;
    pillar->direction   = 1;
    Hexen_MoverStartSound(sec, 0);
    pillar->crush       = crush ? args[3] : 0;  /* crush damage */
  }
  return rtn;
}

int EV_OpenPillar(line_t *line, int *args)
{
  int       secnum;
  int       rtn = 0;
  sector_t *sec;
  pillar_t *pillar;

  (void) line;

  HEXEN_FOR_TAGGED_SECTORS(secnum, args[0])
  {
    sec = &sectors[secnum];
    if (sec->floordata || sec->ceilingdata)
      continue;
    if (sec->floorheight != sec->ceilingheight)
      continue;                 /* not closed */

    rtn = 1;
    pillar = Z_Malloc(sizeof(*pillar), PU_LEVEL, 0);
    memset(pillar, 0, sizeof(*pillar));
    sec->floordata = pillar;
    P_AddThinker(&pillar->thinker);
    pillar->thinker.function.arg1 = (void (*)(void *))T_HexenBuildPillar;
    pillar->sector = sec;
    pillar->crush  = FALSE;

    if (!args[2])
      pillar->floordest = P_FindLowestFloorSurrounding(sec);
    else
      pillar->floordest = sec->floorheight - (args[2] << FRACBITS);
    if (!args[3])
      pillar->ceilingdest = P_FindHighestCeilingSurrounding(sec);
    else
      pillar->ceilingdest = sec->ceilingheight + (args[3] << FRACBITS);

    if (sec->floorheight - pillar->floordest >=
        pillar->ceilingdest - sec->ceilingheight)
    {
      pillar->floorSpeed = args[1] * (FRACUNIT / 8);
      pillar->ceilingSpeed = FixedMul(sec->ceilingheight - pillar->ceilingdest,
                                      FixedDiv(pillar->floorSpeed,
                                               pillar->floordest -
                                               sec->floorheight));
    }
    else
    {
      pillar->ceilingSpeed = args[1] * (FRACUNIT / 8);
      pillar->floorSpeed = FixedMul(pillar->floordest - sec->floorheight,
                                    FixedDiv(pillar->ceilingSpeed,
                                             sec->ceilingheight -
                                             pillar->ceilingdest));
    }
    pillar->direction = -1;     /* open */
    Hexen_MoverStartSound(sec, 0);
  }
  return rtn;
}

/* --- Platforms ------------------------------------------------------------
 *
 * Lifts.  These reuse the engine's T_PlatRaise thinker and active-plat list;
 * the Hexen movement types set low/high/wait/status and otherwise fall
 * through the thinker's Doom change-type handling.  Movement sound is the
 * engine default until the sound-sequence subsystem lands. */
static void Hexen_BuildPlat(sector_t *sec, int *args, plattype_e type,
                            int amount)
{
  plat_t *plat = Z_Malloc(sizeof(*plat), PU_LEVEL, 0);
  memset(plat, 0, sizeof(*plat));
  P_AddThinker(&plat->thinker);
  plat->type = type;
  plat->sector = sec;
  plat->sector->floordata = plat;
  plat->thinker.function.arg1 = (void (*)(void *))T_HexenPlatRaise;
  plat->crush = FALSE;
  plat->tag   = args[0];
  plat->speed = args[1] * (FRACUNIT / 8);

  switch (type)
  {
    case PLAT_DOWNWAITUPSTAY:
      /* `amount` is the lip in map units (8 for the Hexen specials,
       * args[3] for ZDoom's Plat_*Lip variants) */
      plat->low = P_FindLowestFloorSurrounding(sec) + amount * FRACUNIT;
      if (plat->low > sec->floorheight)
        plat->low = sec->floorheight;
      plat->high   = sec->floorheight;
      plat->wait   = args[2];
      plat->status = PLAT_DOWN;
      break;
    case PLAT_DOWNBYVALUEWAITUPSTAY:
      plat->low = sec->floorheight - args[3] * 8 * FRACUNIT;
      if (plat->low > sec->floorheight)
        plat->low = sec->floorheight;
      plat->high   = sec->floorheight;
      plat->wait   = args[2];
      plat->status = PLAT_DOWN;
      break;
    case PLAT_UPWAITDOWNSTAY:
      plat->high = P_FindHighestFloorSurrounding(sec);
      if (plat->high < sec->floorheight)
        plat->high = sec->floorheight;
      plat->low    = sec->floorheight;
      plat->wait   = args[2];
      plat->status = PLAT_UP;
      break;
    case PLAT_UPBYVALUEWAITDOWNSTAY:
      plat->high = sec->floorheight + args[3] * 8 * FRACUNIT;
      if (plat->high < sec->floorheight)
        plat->high = sec->floorheight;
      plat->low    = sec->floorheight;
      plat->wait   = args[2];
      plat->status = PLAT_UP;
      break;
    case PLAT_PERPETUALRAISE:
      plat->low = P_FindLowestFloorSurrounding(sec) + amount * FRACUNIT;
      if (plat->low > sec->floorheight)
        plat->low = sec->floorheight;
      plat->high = P_FindHighestFloorSurrounding(sec);
      if (plat->high < sec->floorheight)
        plat->high = sec->floorheight;
      plat->wait   = args[2];
      plat->status = (P_Random(pr_heretic) & 1) ? PLAT_UP : PLAT_DOWN;
      break;
    default:
      break;
  }
  P_AddActivePlat(plat);
  Hexen_MoverStartSound(sec, g_sfx_pstart);
}

int EV_DoHexenPlat(line_t *line, int *args, plattype_e type, int amount)
{
  int       secnum;
  int       rtn = 0;
  sector_t *sec;

  /* A zero tag means a manual lift: act on the sector on the far side of the
   * line the player used, exactly like vanilla Doom's tag-0 plats.  Without
   * this, P_FindSectorFromTag(0) walks every untagged sector in the map and
   * the lift the player is standing on never moves -- the player can ride
   * nothing.  Hexen specials normally carry a real tag, so only the
   * manually-activated ZDoom-style lifts (e.g. Plat_DownWaitUpStay with no
   * tag, playeruse) take this path. */
  if (args[0] == 0)
  {
    if (!line || !line->backsector)
      return 0;
    sec = line->backsector;
    if (sec->floordata || sec->ceilingdata)
      return 0;
    Hexen_BuildPlat(sec, args, type, amount);
    return 1;
  }

  HEXEN_FOR_TAGGED_SECTORS(secnum, args[0])
  {
    sec = &sectors[secnum];
    if (sec->floordata || sec->ceilingdata)
      continue;
    rtn = 1;
    Hexen_BuildPlat(sec, args, type, amount);
  }
  return rtn;
}

void Hexen_EV_StopPlat(line_t *line, int *args)
{
  platlist_t *pl;
  platlist_t *next;

  (void) line;

  for (pl = activeplats; pl; pl = next)
  {
    next = pl->next;
    if (pl->plat->tag == args[0])
      P_RemoveActivePlat(pl->plat);
  }
}

/* --- Stairs ---------------------------------------------------------------
 *
 * Build a flight of steps starting from the tagged sector(s) and propagating
 * across two-sided lines into neighbouring sectors that carry the matching
 * stair build-marker special and share the start floor texture.  Each step is
 * a plain one-shot FLEV_RAISEBUILDSTEP floor mover (T_MoveFloor removes it on
 * arrival).  NORMAL builds every step at the same speed; SYNC scales each
 * step's speed so the whole flight finishes together.  The per-step start
 * delay and auto-reset (PHASED stairs, and the NORMAL delay/reset args) are
 * not modelled; the stairs still build to their final shape. */

#define STAIR_SECTOR_TYPE  26
#define STAIR_QUEUE_SIZE   32

typedef enum
{
  STAIRS_NORMAL,
  STAIRS_SYNC,
  STAIRS_PHASED
} stairs_e;

static struct
{
  sector_t *sector;
  int       type;
  int       height;
} StairQueue[STAIR_QUEUE_SIZE];

static int StairQueueHead;
static int StairQueueTail;

static int s_StepDelta;
static int s_Direction;
static int s_Speed;
static int s_Texture;
static int s_StartHeight;

static void QueueStairSector(sector_t *sec, int type, int height)
{
  if ((StairQueueTail + 1) % STAIR_QUEUE_SIZE == StairQueueHead)
    return;                     /* too many branches; drop quietly */
  StairQueue[StairQueueTail].sector = sec;
  StairQueue[StairQueueTail].type = type;
  StairQueue[StairQueueTail].height = height;
  StairQueueTail = (StairQueueTail + 1) % STAIR_QUEUE_SIZE;
}

static sector_t *DequeueStairSector(int *type, int *height)
{
  sector_t *sec;

  if (StairQueueHead == StairQueueTail)
    return NULL;
  *type   = StairQueue[StairQueueHead].type;
  *height = StairQueue[StairQueueHead].height;
  sec     = StairQueue[StairQueueHead].sector;
  StairQueueHead = (StairQueueHead + 1) % STAIR_QUEUE_SIZE;
  return sec;
}

static void ProcessStairSector(sector_t *sec, int type, int height,
                               stairs_e stairsType)
{
  int          i;
  sector_t    *tsec;
  floormove_t *floor;

  height += s_StepDelta;

  floor = Z_Malloc(sizeof(*floor), PU_LEVEL, 0);
  memset(floor, 0, sizeof(*floor));
  P_AddThinker(&floor->thinker);
  sec->floordata = floor;
  floor->thinker.function.arg1 = (void (*)(void *))T_MoveFloor;
  floor->type = FLEV_RAISEBUILDSTEP;
  floor->direction = s_Direction;
  floor->sector = sec;
  floor->floordestheight = height;
  floor->crush = FALSE;

  if (stairsType == STAIRS_SYNC)
    floor->speed = FixedMul(s_Speed, FixedDiv(height - s_StartHeight,
                                              s_StepDelta));
  else
    floor->speed = s_Speed;

  Hexen_MoverStartSound(sec, 0);

  /* Propagate to adjacent stair sectors. */
  for (i = 0; i < sec->linecount; i++)
  {
    line_t *ln = sec->lines[i];
    if (!(ln->flags & ML_TWOSIDED))
      continue;

    tsec = ln->frontsector;
    if (tsec && tsec->special == type + STAIR_SECTOR_TYPE &&
        !tsec->floordata && !tsec->ceilingdata &&
        tsec->floorpic == s_Texture && tsec->validcount != validcount)
    {
      QueueStairSector(tsec, type ^ 1, height);
      tsec->validcount = validcount;
    }
    tsec = ln->backsector;
    if (tsec && tsec->special == type + STAIR_SECTOR_TYPE &&
        !tsec->floordata && !tsec->ceilingdata &&
        tsec->floorpic == s_Texture && tsec->validcount != validcount)
    {
      QueueStairSector(tsec, type ^ 1, height);
      tsec->validcount = validcount;
    }
  }
}

int Hexen_EV_BuildStairs(line_t *line, int *args, int direction,
                         int stairsType)
{
  int       secnum;
  int       type;
  int       height;
  sector_t *sec;
  sector_t *qSec;

  (void) line;

  s_Direction  = direction;
  s_StepDelta  = s_Direction * (args[2] * FRACUNIT);
  s_Speed      = args[1] * (FRACUNIT / 8);

  StairQueueHead = StairQueueTail = 0;
  validcount++;

  HEXEN_FOR_TAGGED_SECTORS(secnum, args[0])
  {
    sec = &sectors[secnum];
    s_Texture     = sec->floorpic;
    s_StartHeight = sec->floorheight;

    if (sec->floordata || sec->ceilingdata)
      continue;                 /* already moving */

    QueueStairSector(sec, 0, sec->floorheight);
    sec->special = 0;           /* consume the build marker */
  }

  while ((qSec = DequeueStairSector(&type, &height)) != NULL)
    ProcessStairSector(qSec, type, height, (stairs_e) stairsType);

  return 1;
}

/* --- Thing projectile / spawn ---------------------------------------------
 *
 * Spawn a projectile (134/136) or a thing (135/137) at every mapspot sharing
 * the given thing id.  The spawn-type argument indexes the Hexen thing-type
 * table below. */
const mobjtype_t TranslateThingType[] = {
    HEXEN_MT_MAPSPOT,                 /* T_NONE */
    HEXEN_MT_CENTAUR,                 /* T_CENTAUR */
    HEXEN_MT_CENTAURLEADER,           /* T_CENTAURLEADER */
    HEXEN_MT_DEMON,                   /* T_DEMON */
    HEXEN_MT_ETTIN,                   /* T_ETTIN */
    HEXEN_MT_FIREDEMON,               /* T_FIREGARGOYLE */
    HEXEN_MT_SERPENT,                 /* T_WATERLURKER */
    HEXEN_MT_SERPENTLEADER,           /* T_WATERLURKERLEADER */
    HEXEN_MT_WRAITH,                  /* T_WRAITH */
    HEXEN_MT_WRAITHB,                 /* T_WRAITHBURIED */
    HEXEN_MT_FIREBALL1,               /* T_FIREBALL1 */
    HEXEN_MT_MANA1,                   /* T_MANA1 */
    HEXEN_MT_MANA2,                   /* T_MANA2 */
    HEXEN_MT_SPEEDBOOTS,              /* T_ITEMBOOTS */
    HEXEN_MT_ARTIEGG,                 /* T_ITEMEGG */
    HEXEN_MT_ARTIFLY,                 /* T_ITEMFLIGHT */
    HEXEN_MT_SUMMONMAULATOR,          /* T_ITEMSUMMON */
    HEXEN_MT_TELEPORTOTHER,           /* T_ITEMTPORTOTHER */
    HEXEN_MT_ARTITELEPORT,            /* T_ITEMTELEPORT */
    HEXEN_MT_BISHOP,                  /* T_BISHOP */
    HEXEN_MT_ICEGUY,                  /* T_ICEGOLEM */
    HEXEN_MT_BRIDGE,                  /* T_BRIDGE */
    HEXEN_MT_BOOSTARMOR,              /* T_DRAGONSKINBRACERS */
    HEXEN_MT_HEALINGBOTTLE,           /* T_ITEMHEALTHPOTION */
    HEXEN_MT_HEALTHFLASK,             /* T_ITEMHEALTHFLASK */
    HEXEN_MT_ARTISUPERHEAL,           /* T_ITEMHEALTHFULL */
    HEXEN_MT_BOOSTMANA,               /* T_ITEMBOOSTMANA */
    HEXEN_MT_FW_AXE,                  /* T_FIGHTERAXE */
    HEXEN_MT_FW_HAMMER,               /* T_FIGHTERHAMMER */
    HEXEN_MT_FW_SWORD1,               /* T_FIGHTERSWORD1 */
    HEXEN_MT_FW_SWORD2,               /* T_FIGHTERSWORD2 */
    HEXEN_MT_FW_SWORD3,               /* T_FIGHTERSWORD3 */
    HEXEN_MT_CW_SERPSTAFF,            /* T_CLERICSTAFF */
    HEXEN_MT_CW_HOLY1,                /* T_CLERICHOLY1 */
    HEXEN_MT_CW_HOLY2,                /* T_CLERICHOLY2 */
    HEXEN_MT_CW_HOLY3,                /* T_CLERICHOLY3 */
    HEXEN_MT_MW_CONE,                 /* T_MAGESHARDS */
    HEXEN_MT_MW_STAFF1,               /* T_MAGESTAFF1 */
    HEXEN_MT_MW_STAFF2,               /* T_MAGESTAFF2 */
    HEXEN_MT_MW_STAFF3,               /* T_MAGESTAFF3 */
    HEXEN_MT_EGGFX,                   /* T_MORPHBLAST */
    HEXEN_MT_ROCK1,                   /* T_ROCK1 */
    HEXEN_MT_ROCK2,                   /* T_ROCK2 */
    HEXEN_MT_ROCK3,                   /* T_ROCK3 */
    HEXEN_MT_DIRT1,                   /* T_DIRT1 */
    HEXEN_MT_DIRT2,                   /* T_DIRT2 */
    HEXEN_MT_DIRT3,                   /* T_DIRT3 */
    HEXEN_MT_DIRT4,                   /* T_DIRT4 */
    HEXEN_MT_DIRT5,                   /* T_DIRT5 */
    HEXEN_MT_DIRT6,                   /* T_DIRT6 */
    HEXEN_MT_ARROW,                   /* T_ARROW */
    HEXEN_MT_DART,                    /* T_DART */
    HEXEN_MT_POISONDART,              /* T_POISONDART */
    HEXEN_MT_RIPPERBALL,              /* T_RIPPERBALL */
    HEXEN_MT_SGSHARD1,                /* T_STAINEDGLASS1 */
    HEXEN_MT_SGSHARD2,                /* T_STAINEDGLASS2 */
    HEXEN_MT_SGSHARD3,                /* T_STAINEDGLASS3 */
    HEXEN_MT_SGSHARD4,                /* T_STAINEDGLASS4 */
    HEXEN_MT_SGSHARD5,                /* T_STAINEDGLASS5 */
    HEXEN_MT_SGSHARD6,                /* T_STAINEDGLASS6 */
    HEXEN_MT_SGSHARD7,                /* T_STAINEDGLASS7 */
    HEXEN_MT_SGSHARD8,                /* T_STAINEDGLASS8 */
    HEXEN_MT_SGSHARD9,                /* T_STAINEDGLASS9 */
    HEXEN_MT_SGSHARD0,                /* T_STAINEDGLASS0 */
    HEXEN_MT_PROJECTILE_BLADE,        /* T_BLADE */
    HEXEN_MT_ICESHARD,                /* T_ICESHARD */
    HEXEN_MT_FLAME_SMALL,             /* T_FLAME_SMALL */
    HEXEN_MT_FLAME_LARGE,             /* T_FLAME_LARGE */
    HEXEN_MT_ARMOR_1,                 /* T_MESHARMOR */
    HEXEN_MT_ARMOR_2,                 /* T_FALCONSHIELD */
    HEXEN_MT_ARMOR_3,                 /* T_PLATINUMHELM */
    HEXEN_MT_ARMOR_4,                 /* T_AMULETOFWARDING */
    HEXEN_MT_ARTIPOISONBAG,           /* T_ITEMFLECHETTE */
    HEXEN_MT_ARTITORCH,               /* T_ITEMTORCH */
    HEXEN_MT_BLASTRADIUS,             /* T_ITEMREPULSION */
    HEXEN_MT_MANA3,                   /* T_MANA3 */
    HEXEN_MT_ARTIPUZZSKULL,           /* T_PUZZSKULL */
    HEXEN_MT_ARTIPUZZGEMBIG,          /* T_PUZZGEMBIG */
    HEXEN_MT_ARTIPUZZGEMRED,          /* T_PUZZGEMRED */
    HEXEN_MT_ARTIPUZZGEMGREEN1,       /* T_PUZZGEMGREEN1 */
    HEXEN_MT_ARTIPUZZGEMGREEN2,       /* T_PUZZGEMGREEN2 */
    HEXEN_MT_ARTIPUZZGEMBLUE1,        /* T_PUZZGEMBLUE1 */
    HEXEN_MT_ARTIPUZZGEMBLUE2,        /* T_PUZZGEMBLUE2 */
    HEXEN_MT_ARTIPUZZBOOK1,           /* T_PUZZBOOK1 */
    HEXEN_MT_ARTIPUZZBOOK2,           /* T_PUZZBOOK2 */
    HEXEN_MT_KEY1,                    /* T_METALKEY */
    HEXEN_MT_KEY2,                    /* T_SMALLMETALKEY */
    HEXEN_MT_KEY3,                    /* T_AXEKEY */
    HEXEN_MT_KEY4,                    /* T_FIREKEY */
    HEXEN_MT_KEY5,                    /* T_GREENKEY */
    HEXEN_MT_KEY6,                    /* T_MACEKEY */
    HEXEN_MT_KEY7,                    /* T_SILVERKEY */
    HEXEN_MT_KEY8,                    /* T_RUSTYKEY */
    HEXEN_MT_KEY9,                    /* T_HORNKEY */
    HEXEN_MT_KEYA,                    /* T_SERPENTKEY */
    HEXEN_MT_WATER_DRIP,              /* T_WATERDRIP */
    HEXEN_MT_FLAME_SMALL_TEMP,        /* T_TEMPSMALLFLAME */
    HEXEN_MT_FLAME_SMALL,             /* T_PERMSMALLFLAME */
    HEXEN_MT_FLAME_LARGE_TEMP,        /* T_TEMPLARGEFLAME */
    HEXEN_MT_FLAME_LARGE,             /* T_PERMLARGEFLAME */
    HEXEN_MT_DEMON_MASH,              /* T_DEMON_MASH */
    HEXEN_MT_DEMON2_MASH,             /* T_DEMON2_MASH */
    HEXEN_MT_ETTIN_MASH,              /* T_ETTIN_MASH */
    HEXEN_MT_CENTAUR_MASH,            /* T_CENTAUR_MASH */
    HEXEN_MT_THRUSTFLOOR_UP,          /* T_THRUSTSPIKEUP */
    HEXEN_MT_THRUSTFLOOR_DOWN,        /* T_THRUSTSPIKEDOWN */
    HEXEN_MT_WRAITHFX4,               /* T_FLESH_DRIP1 */
    HEXEN_MT_WRAITHFX5,               /* T_FLESH_DRIP2 */
    HEXEN_MT_WRAITHFX2                /* T_SPARK_DRIP */
};

#define TT_COUNT ((int)(sizeof(TranslateThingType) / sizeof(TranslateThingType[0])))

int EV_ThingProjectile(int *args, int gravity)
{
  int        tid = args[0];
  int        searcher = -1;
  int        success = 0;
  angle_t    angle;
  int        fineAngle;
  fixed_t    speed;
  fixed_t    vspeed;
  mobjtype_t moType;
  mobj_t    *mobj;

  if (args[1] >= TT_COUNT)
    return 0;
  moType = TranslateThingType[args[1]];
  if (nomonsters && (mobjinfo[moType].flags & MF_COUNTKILL))
    return 0;

  angle     = (int) args[2] << 24;
  fineAngle = angle >> ANGLETOFINESHIFT;
  speed     = (int) args[3] << 13;
  vspeed    = (int) args[4] << 13;

  while ((mobj = P_FindMobjFromTID((short) tid, &searcher)) != NULL)
  {
    mobj_t *newMobj = P_SpawnMobj(mobj->x, mobj->y, mobj->z, moType);
    if (newMobj->info->seesound)
      S_StartSound(newMobj, newMobj->info->seesound);
    P_SetTarget(&newMobj->target, mobj);        /* originator */
    newMobj->angle = angle;
    newMobj->momx = FixedMul(speed, finecosine[fineAngle]);
    newMobj->momy = FixedMul(speed, finesine[fineAngle]);
    newMobj->momz = vspeed;
    newMobj->flags |= MF_DROPPED;               /* don't respawn */
    if (gravity)
    {
      newMobj->flags &= ~MF_NOGRAVITY;
      newMobj->flags2 |= MF2_LOGRAV;
    }
    /* fork P_CheckMissileSpawn is void: it launches the missile and handles
     * an immediate collision internally, so the projectile is considered
     * spawned regardless. */
    P_CheckMissileSpawn(newMobj);
    success = 1;
  }
  return success;
}

int EV_ThingSpawn(int *args, int fog)
{
  int        tid = args[0];
  int        searcher = -1;
  int        success = 0;
  angle_t    angle;
  mobjtype_t moType;
  mobj_t    *mobj;

  if (args[1] >= TT_COUNT)
    return 0;
  moType = TranslateThingType[args[1]];
  if (nomonsters && (mobjinfo[moType].flags & MF_COUNTKILL))
    return 0;

  angle = (int) args[2] << 24;

  while ((mobj = P_FindMobjFromTID((short) tid, &searcher)) != NULL)
  {
    fixed_t z = (mobjinfo[moType].flags2 & MF2_FLOATBOB)
                  ? mobj->z - mobj->floorz : mobj->z;
    mobj_t *newMobj = P_SpawnMobj(mobj->x, mobj->y, z, moType);
    if (!P_TestMobjLocation(newMobj))
      P_RemoveMobj(newMobj);                    /* didn't fit */
    else
    {
      newMobj->angle = angle;
      if (fog)
      {
        mobj_t *fogMobj = P_SpawnMobj(mobj->x, mobj->y,
                                      mobj->z + TELEFOGHEIGHT, HEXEN_MT_TFOG);
        S_StartSound(fogMobj, hexen_sfx_teleport);
      }
      newMobj->flags |= MF_DROPPED;
      if (newMobj->flags2 & MF2_FLOATBOB)
        newMobj->special1.i = newMobj->z - newMobj->floorz;
      success = 1;
    }
  }
  return success;
}

/* --- Thing specials -------------------------------------------------------
 *
 * Operate on the mobjs sharing a thing id.  Activate/Deactivate wake or sleep
 * dormant monsters (the common, gameplay-meaningful case); the decoration
 * light/animation toggles are a later refinement.  Remove deletes the thing;
 * Destroy kills a shootable thing outright. */
static dbool ActivateThing(mobj_t *mobj)
{
  if (mobj->flags & MF_COUNTKILL)
  {                             /* monster: clear dormancy */
    if (mobj->flags2 & MF2_DORMANT)
    {
      mobj->flags2 &= ~MF2_DORMANT;
      mobj->tics = 1;
      return true;
    }
    return false;
  }
  switch (mobj->type)           /* decorations: light up / animate */
  {
    case HEXEN_MT_ZTWINEDTORCH:
    case HEXEN_MT_ZTWINEDTORCH_UNLIT:
      P_SetMobjState(mobj, HEXEN_S_ZTWINEDTORCH_1);
      S_StartSound(mobj, hexen_sfx_ignite);
      break;
    case HEXEN_MT_ZWALLTORCH:
    case HEXEN_MT_ZWALLTORCH_UNLIT:
      P_SetMobjState(mobj, HEXEN_S_ZWALLTORCH1);
      S_StartSound(mobj, hexen_sfx_ignite);
      break;
    case HEXEN_MT_ZGEMPEDESTAL:
      P_SetMobjState(mobj, HEXEN_S_ZGEMPEDESTAL2);
      break;
    case HEXEN_MT_ZWINGEDSTATUENOSKULL:
      P_SetMobjState(mobj, HEXEN_S_ZWINGEDSTATUENOSKULL2);
      break;
    case HEXEN_MT_THRUSTFLOOR_UP:
    case HEXEN_MT_THRUSTFLOOR_DOWN:
      if (mobj->special_args[0] == 0)
      {
        S_StartSound(mobj, hexen_sfx_thrustspike_lower);
        mobj->flags2 &= ~MF2_DONTDRAW;
        P_SetMobjState(mobj, mobj->special_args[1] ? HEXEN_S_BTHRUSTRAISE1
                                                   : HEXEN_S_THRUSTRAISE1);
      }
      break;
    case HEXEN_MT_ZFIREBULL:
    case HEXEN_MT_ZFIREBULL_UNLIT:
      P_SetMobjState(mobj, HEXEN_S_ZFIREBULL_BIRTH);
      S_StartSound(mobj, hexen_sfx_ignite);
      break;
    case HEXEN_MT_ZBELL:
      if (mobj->health > 0)
        P_DamageMobj(mobj, NULL, NULL, 10);     /* 'ring' the bell */
      break;
    case HEXEN_MT_ZCAULDRON:
    case HEXEN_MT_ZCAULDRON_UNLIT:
      P_SetMobjState(mobj, HEXEN_S_ZCAULDRON1);
      S_StartSound(mobj, hexen_sfx_ignite);
      break;
    case HEXEN_MT_FLAME_SMALL:
      S_StartSound(mobj, hexen_sfx_ignite);
      P_SetMobjState(mobj, HEXEN_S_FLAME_SMALL1);
      break;
    case HEXEN_MT_FLAME_LARGE:
      S_StartSound(mobj, hexen_sfx_ignite);
      P_SetMobjState(mobj, HEXEN_S_FLAME_LARGE1);
      break;
    case HEXEN_MT_BAT_SPAWNER:
      P_SetMobjState(mobj, HEXEN_S_SPAWNBATS1);
      break;
    default:
      return false;
  }
  return true;
}

static dbool DeactivateThing(mobj_t *mobj)
{
  if (mobj->flags & MF_COUNTKILL)
  {                             /* monster: go dormant */
    if (!(mobj->flags2 & MF2_DORMANT))
    {
      mobj->flags2 |= MF2_DORMANT;
      mobj->tics = -1;
      return true;
    }
    return false;
  }
  switch (mobj->type)           /* decorations: extinguish / go dormant */
  {
    case HEXEN_MT_ZTWINEDTORCH:
    case HEXEN_MT_ZTWINEDTORCH_UNLIT:
      P_SetMobjState(mobj, HEXEN_S_ZTWINEDTORCH_UNLIT);
      break;
    case HEXEN_MT_ZWALLTORCH:
    case HEXEN_MT_ZWALLTORCH_UNLIT:
      P_SetMobjState(mobj, HEXEN_S_ZWALLTORCH_U);
      break;
    case HEXEN_MT_THRUSTFLOOR_UP:
    case HEXEN_MT_THRUSTFLOOR_DOWN:
      if (mobj->special_args[0] == 1)
      {
        S_StartSound(mobj, hexen_sfx_thrustspike_raise);
        P_SetMobjState(mobj, mobj->special_args[1] ? HEXEN_S_BTHRUSTLOWER
                                                   : HEXEN_S_THRUSTLOWER);
      }
      break;
    case HEXEN_MT_ZFIREBULL:
    case HEXEN_MT_ZFIREBULL_UNLIT:
      P_SetMobjState(mobj, HEXEN_S_ZFIREBULL_DEATH);
      break;
    case HEXEN_MT_ZCAULDRON:
    case HEXEN_MT_ZCAULDRON_UNLIT:
      P_SetMobjState(mobj, HEXEN_S_ZCAULDRON_U);
      break;
    case HEXEN_MT_FLAME_SMALL:
      P_SetMobjState(mobj, HEXEN_S_FLAME_SDORM1);
      break;
    case HEXEN_MT_FLAME_LARGE:
      P_SetMobjState(mobj, HEXEN_S_FLAME_LDORM1);
      break;
    case HEXEN_MT_BAT_SPAWNER:
      P_SetMobjState(mobj, HEXEN_S_SPAWNBATS_OFF);
      break;
    default:
      return false;
  }
  return true;
}

int EV_ThingActivate(int tid)
{
  mobj_t *mobj;
  int     searcher = -1;
  int     success = 0;

  while ((mobj = P_FindMobjFromTID((short) tid, &searcher)) != NULL)
    if (ActivateThing(mobj))
      success = 1;
  return success;
}

int EV_ThingDeactivate(int tid)
{
  mobj_t *mobj;
  int     searcher = -1;
  int     success = 0;

  while ((mobj = P_FindMobjFromTID((short) tid, &searcher)) != NULL)
    if (DeactivateThing(mobj))
      success = 1;
  return success;
}

int EV_ThingRemove(int tid)
{
  mobj_t *mobj;
  int     searcher = -1;
  int     success = 0;

  while ((mobj = P_FindMobjFromTID((short) tid, &searcher)) != NULL)
  {
    if (mobj->type == HEXEN_MT_BRIDGE)
    {                           /* bridges fade out instead of vanishing */
      A_BridgeRemove(mobj);
      return 1;
    }
    P_RemoveMobj(mobj);
    success = 1;
    searcher = -1;              /* the list shifted; restart the scan */
  }
  return success;
}

int EV_ThingDestroy(int tid)
{
  mobj_t *mobj;
  int     searcher = -1;
  int     success = 0;

  while ((mobj = P_FindMobjFromTID((short) tid, &searcher)) != NULL)
    if (mobj->flags & MF_SHOOTABLE)
    {
      P_DamageMobj(mobj, NULL, NULL, 10000);
      success = 1;
    }
  return success;
}

/* Thing_SpawnFacing(tid, type, nofog, newtid): spawn an actor at every mapspot
 * with the given tid, keeping the spot's own facing angle (unlike Thing_Spawn,
 * which takes an explicit angle byte). */
int EV_ThingSpawnFacing(int *args, int fog)
{
  int        tid = args[0];
  int        searcher = -1;
  int        success = 0;
  mobjtype_t moType;
  mobj_t    *mobj;

  if (args[1] >= TT_COUNT)
    return 0;
  moType = TranslateThingType[args[1]];
  if (nomonsters && (mobjinfo[moType].flags & MF_COUNTKILL))
    return 0;

  while ((mobj = P_FindMobjFromTID((short) tid, &searcher)) != NULL)
  {
    fixed_t z = (mobjinfo[moType].flags2 & MF2_FLOATBOB)
                  ? mobj->z - mobj->floorz : mobj->z;
    mobj_t *newMobj = P_SpawnMobj(mobj->x, mobj->y, z, moType);
    if (!P_TestMobjLocation(newMobj))
      P_RemoveMobj(newMobj);
    else
    {
      newMobj->angle = mobj->angle;     /* inherit the spot's facing */
      if (fog)
      {
        mobj_t *fogMobj = P_SpawnMobj(mobj->x, mobj->y,
                                      mobj->z + TELEFOGHEIGHT, HEXEN_MT_TFOG);
        S_StartSound(fogMobj, hexen_sfx_teleport);
      }
      newMobj->flags |= MF_DROPPED;
      if (newMobj->flags2 & MF2_FLOATBOB)
        newMobj->special1.i = newMobj->z - newMobj->floorz;
      if (args[3])
        P_InsertMobjIntoTIDList(newMobj, (short) args[3]);
      success = 1;
    }
  }
  return success;
}

/* Thing_Move(tid, mapspot, nofog): warp the tid'd actor onto the mapspot with
 * the matching id.  A direct placement (P_TeleportMove) keeps it simple; the
 * fog is cosmetic and omitted. */
int EV_ThingMove(int tid, int mapspot, int nofog)
{
  mobj_t *mobj, *dest;
  int     s1 = -1, s2 = -1;
  int     success = 0;

  (void) nofog;
  dest = P_FindMobjFromTID((short) mapspot, &s2);
  if (!dest)
    return 0;
  while ((mobj = P_FindMobjFromTID((short) tid, &s1)) != NULL)
  {
    fixed_t ox = mobj->x, oy = mobj->y, oz = mobj->z;
    if (P_TeleportMove(mobj, dest->x, dest->y, false))
    {
      mobj->z = dest->z;
      mobj->angle = dest->angle;
      success = 1;
    }
    else
    {
      mobj->x = ox; mobj->y = oy; mobj->z = oz;
    }
  }
  return success;
}

/* Thing_Stop(tid): zero an actor's momentum. */
int EV_ThingStop(int tid)
{
  mobj_t *mobj;
  int     searcher = -1;
  int     success = 0;

  while ((mobj = P_FindMobjFromTID((short) tid, &searcher)) != NULL)
  {
    mobj->momx = mobj->momy = mobj->momz = 0;
    if (mobj->player)
      mobj->player->momx = mobj->player->momy = 0;
    success = 1;
  }
  return success;
}

/* Thing_Raise(tid): resurrect a corpse, like an archvile's heal. */
int EV_ThingRaise(int tid)
{
  mobj_t *mobj;
  int     searcher = -1;
  int     success = 0;

  while ((mobj = P_FindMobjFromTID((short) tid, &searcher)) != NULL)
    if (A_RaiseMobj(mobj))
      success = 1;
  return success;
}

/* Thing_ChangeTID(oldtid, newtid): re-key actors in the TID list. */
int EV_ThingChangeTID(int oldtid, int newtid)
{
  mobj_t *mobj;
  int     searcher = -1;
  int     success = 0;

  while ((mobj = P_FindMobjFromTID((short) oldtid, &searcher)) != NULL)
  {
    P_RemoveMobjFromTIDList(mobj);
    if (newtid)
      P_InsertMobjIntoTIDList(mobj, (short) newtid);
    else
      mobj->tid = 0;
    searcher = -1;              /* the list mutated; restart the walk */
    success = 1;
  }
  return success;
}

/* Thing_Hate(hater, hatee): point one actor's aggression at another. */
int EV_ThingHate(int hater, int hatee)
{
  mobj_t *mobj, *target;
  int     s1 = -1, s2 = -1;
  int     success = 0;

  target = P_FindMobjFromTID((short) hatee, &s2);
  if (!target)
    return 0;
  while ((mobj = P_FindMobjFromTID((short) hater, &s1)) != NULL)
    if (mobj->flags & MF_SHOOTABLE)
    {
      P_SetTarget(&mobj->target, target);
      if (mobj->info->seestate &&
          mobj->state == &states[mobj->info->spawnstate])
        P_SetMobjState(mobj, mobj->info->seestate);
      success = 1;
    }
  return success;
}

/* SetThingSpecial(tid, special, arg1..arg3): stamp an actor's activation
 * special, used so a later Thing_Activate runs it. */
int EV_SetThingSpecial(int *args)
{
  mobj_t *mobj;
  int     searcher = -1;
  int     success = 0;

  while ((mobj = P_FindMobjFromTID((short) args[0], &searcher)) != NULL)
  {
    mobj->special         = args[1];
    mobj->special_args[0] = args[2];
    mobj->special_args[1] = args[3];
    mobj->special_args[2] = args[4];
    mobj->special_args[3] = 0;
    mobj->special_args[4] = 0;
    success = 1;
  }
  return success;
}

/* SetLineSpecial(id, special, arg1, arg2, arg3): rewrite the special on every
 * line carrying the given line id.  As a line-special the dispatcher passes
 * five ints: [id, special, arg1, arg2, arg3]; the new line keeps the first
 * three of its own args (the most a line-special call can carry) and clears
 * the rest.  (Scripts usually reach this through the PCD_SETLINESPECIAL
 * opcode, which carries all five new args; this path covers the rarer
 * line-activated form.) */
int EV_SetLineSpecial(int *args)
{
  int i;
  int success = 0;

  for (i = 0; i < numlines; i++)
    if (lines[i].tag == args[0])
    {
      lines[i].special = (short) args[1];
      lines[i].args[0] = args[2];
      lines[i].args[1] = args[3];
      lines[i].args[2] = args[4];
      lines[i].args[3] = 0;
      lines[i].args[4] = 0;
      success = 1;
    }
  return success;
}

/* Line_SetBlockMonsters(lineid, on): toggle the monster-block flag on the
 * tagged lines. */
int EV_SetLineBlockMonsters(int lineid, int on)
{
  int i;
  int success = 0;

  for (i = 0; i < numlines; i++)
    if (lines[i].tag == lineid)
    {
      if (on)
        lines[i].flags |= ML_BLOCKMONSTERS;
      else
        lines[i].flags &= ~ML_BLOCKMONSTERS;
      success = 1;
    }
  return success;
}

/* --- Teleport -------------------------------------------------------------
 *
 * Hexen teleports send the activating mobj to a teleport-destination mapspot
 * found by thing id (TID), optionally with the teleport fog/sound. */
dbool P_HexenTeleport(mobj_t *thing, fixed_t x, fixed_t y, angle_t angle,
                      dbool useFog)
{
  fixed_t   oldx = thing->x;
  fixed_t   oldy = thing->y;
  fixed_t   oldz = thing->z;
  fixed_t   aboveFloor = thing->z - thing->floorz;
  fixed_t   fogDelta;
  unsigned  an;
  mobj_t   *fog;
  player_t *player;

  if (!P_TeleportMove(thing, x, y, false))
    return false;

  if (thing->player)
  {
    player = thing->player;
    if (player->powers[pw_flight] && aboveFloor)
    {
      thing->z = thing->floorz + aboveFloor;
      if (thing->z + thing->height > thing->ceilingz)
        thing->z = thing->ceilingz - thing->height;
      player->viewz = thing->z + player->viewheight;
    }
    else
    {
      thing->z = thing->floorz;
      player->viewz = thing->z + player->viewheight;
      if (useFog)
        player->lookdir = 0;
    }
  }
  else if (thing->flags & MF_MISSILE)
  {
    thing->z = thing->floorz + aboveFloor;
    if (thing->z + thing->height > thing->ceilingz)
      thing->z = thing->ceilingz - thing->height;
  }
  else
    thing->z = thing->floorz;

  if (useFog)
  {
    fogDelta = (thing->flags & MF_MISSILE) ? 0 : TELEFOGHEIGHT;
    fog = P_SpawnMobj(oldx, oldy, oldz + fogDelta,
                      hexen ? HEXEN_MT_TFOG : MT_TFOG);
    S_StartSound(fog, hexen ? hexen_sfx_teleport : sfx_telept);
    an = angle >> ANGLETOFINESHIFT;
    fog = P_SpawnMobj(x + 20 * finecosine[an], y + 20 * finesine[an],
                      thing->z + fogDelta,
                      hexen ? HEXEN_MT_TFOG : MT_TFOG);
    S_StartSound(fog, hexen ? hexen_sfx_teleport : sfx_telept);
    if (thing->player &&
        !thing->player->powers[pw_weaponlevel2] &&
        !thing->player->powers[pw_speed])
      thing->reactiontime = 18;     /* freeze player ~0.5s */
    thing->angle = angle;
  }

  if (thing->flags2 & MF2_FOOTCLIP)
  {
    if (thing->z == thing->subsector->sector->floorheight &&
        P_GetThingFloorType(thing) > FLOOR_SOLID)
      thing->floorclip = 10 * FRACUNIT;
    else
      thing->floorclip = 0;
  }

  if (thing->flags & MF_MISSILE)
  {
    an = angle >> ANGLETOFINESHIFT;
    thing->momx = FixedMul(thing->info->speed, finecosine[an]);
    thing->momy = FixedMul(thing->info->speed, finesine[an]);
  }
  else if (useFog)
    thing->momx = thing->momy = thing->momz = 0;

  if (thing->player)
    R_ResetAfterTeleport(thing->player);

  return true;
}

dbool EV_HexenTeleportTagged(int tid, int sectortag, mobj_t *thing,
                             dbool fog)
{
  int     count;
  int     searcher;
  int     i;
  mobj_t *mo;

  if (!thing || (thing->flags2 & MF2_NOTELEPORT))
    return false;

  /* Count candidate destinations.  When a sector tag is given (ZDoom's
   * Teleport second arg), only destinations standing in a sector carrying
   * that tag qualify -- this is how a map disambiguates many same-tid
   * teleport spots.  A zero tag accepts any destination (Hexen behaviour). */
  count = 0;
  searcher = -1;
  while ((mo = P_FindMobjFromTID((short) tid, &searcher)) != NULL)
    if (sectortag == 0 || mo->subsector->sector->tag == sectortag)
      count++;
  if (count == 0)
    return false;

  count = 1 + (P_Random(pr_heretic) % count);
  searcher = -1;
  mo = NULL;
  for (i = 0; i < count; )
  {
    mobj_t *cand = P_FindMobjFromTID((short) tid, &searcher);
    if (cand == NULL)
      break;
    if (sectortag == 0 || cand->subsector->sector->tag == sectortag)
    {
      mo = cand;
      i++;
    }
  }

  if (mo == NULL)
    return false;

  return P_HexenTeleport(thing, mo->x, mo->y, mo->angle, fog);
}

dbool EV_HexenTeleport(int tid, mobj_t *thing, dbool fog)
{
  return EV_HexenTeleportTagged(tid, 0, thing, fog);
}

/* --- Floor waggle ---------------------------------------------------------
 *
 * Oscillates a sector floor with the FloatBob offset table for a timed
 * duration, easing the amplitude in (expand) and out (reduce). */
extern fixed_t FloatBobOffsets[64];

void T_FloorWaggle(planeWaggle_t *waggle)
{
  switch (waggle->state)
  {
    case WGLSTATE_EXPAND:
      if ((waggle->scale += waggle->scaleDelta) >= waggle->targetScale)
      {
        waggle->scale = waggle->targetScale;
        waggle->state = WGLSTATE_STABLE;
      }
      break;
    case WGLSTATE_REDUCE:
      if ((waggle->scale -= waggle->scaleDelta) <= 0)
      {
        waggle->sector->floorheight = waggle->originalHeight;
        P_ChangeSector(waggle->sector, true);
        waggle->sector->floordata = NULL;
        P_TagFinished(waggle->sector->tag);
        P_RemoveThinker(&waggle->thinker);
        return;
      }
      break;
    case WGLSTATE_STABLE:
      if (waggle->ticker != -1)
        if (!--waggle->ticker)
          waggle->state = WGLSTATE_REDUCE;
      break;
  }
  waggle->accumulator += waggle->accDelta;
  waggle->sector->floorheight = waggle->originalHeight +
    FixedMul(FloatBobOffsets[(waggle->accumulator >> FRACBITS) & 63],
             waggle->scale);
  P_ChangeSector(waggle->sector, true);
}

int EV_StartFloorWaggle(int tag, int height, int speed, int offset, int timer)
{
  int            secnum;
  int            rtn = 0;
  sector_t      *sector;
  planeWaggle_t *waggle;

  HEXEN_FOR_TAGGED_SECTORS(secnum, tag)
  {
    sector = &sectors[secnum];
    if (sector->floordata || sector->ceilingdata)
      continue;                 /* already busy */

    rtn = 1;
    waggle = Z_Malloc(sizeof(*waggle), PU_LEVEL, 0);
    memset(waggle, 0, sizeof(*waggle));
    sector->floordata = waggle;
    waggle->thinker.function.arg1 = (void (*)(void *))T_FloorWaggle;
    waggle->sector = sector;
    waggle->originalHeight = sector->floorheight;
    waggle->accumulator = offset * FRACUNIT;
    waggle->accDelta = speed << 10;
    waggle->scale = 0;
    waggle->targetScale = height << 10;
    waggle->scaleDelta = waggle->targetScale /
                         (35 + ((3 * 35) * height) / 255);
    waggle->ticker = timer ? timer * 35 : -1;
    waggle->state = WGLSTATE_EXPAND;
    P_AddThinker(&waggle->thinker);
  }
  return rtn;
}

/* --- Dispatcher ----------------------------------------------------------- */

int Hexen_EV_FloorCrushStop(line_t *line, int *args)
{
  thinker_t *think;
  int        rtn = 0;

  (void) line;
  (void) args;

  for (think = thinkercap.next; think != &thinkercap; think = think->next)
  {
    floormove_t *floor;
    if (think->function.arg1 != (void (*)(void *))T_MoveFloor)
      continue;
    floor = (floormove_t *) think;
    if (floor->type != FLEV_RAISEFLOORCRUSH)
      continue;
    Hexen_MoverStopSound(floor->sector, 0);
    floor->sector->floordata = NULL;
    P_TagFinished(floor->sector->tag);
    P_RemoveThinker(&floor->thinker);
    rtn = 1;
  }
  return rtn;
}

int Hexen_EV_DoFloorAndCeiling(line_t *line, int *args, int raise)
{
  int       secnum;
  int       floorOk;
  int       ceilingOk;
  sector_t *sec;

  if (raise)
  {
    floorOk = Hexen_EV_DoFloor(line, args, FLEV_RAISEFLOORBYVALUE);
    HEXEN_FOR_TAGGED_SECTORS(secnum, args[0])
    {
      sec = &sectors[secnum];
      sec->floordata = NULL;
      sec->ceilingdata = NULL;
    }
    ceilingOk = Hexen_EV_DoCeiling(line, args, CLEV_RAISEBYVALUE);
  }
  else
  {
    floorOk = Hexen_EV_DoFloor(line, args, FLEV_LOWERFLOORBYVALUE);
    HEXEN_FOR_TAGGED_SECTORS(secnum, args[0])
    {
      sec = &sectors[secnum];
      sec->floordata = NULL;
      sec->ceilingdata = NULL;
    }
    ceilingOk = Hexen_EV_DoCeiling(line, args, CLEV_LOWERBYVALUE);
  }
  return floorOk | ceilingOk;
}

int Hexen_EV_SectorSoundChange(int *args)
{
  int secnum;
  int rtn = 0;

  if (!args[0])
    return 0;

  HEXEN_FOR_TAGGED_SECTORS(secnum, args[0])
  {
    sectors[secnum].seqType = args[1];
    rtn = 1;
  }
  return rtn;
}


/* Hexen line identification.  Hexen linedefs carry no Doom tag; lines that
 * ACS scripts address by id instead carry special 121 (Line_SetIdentification)
 * with the id in args[0].  Raven registers those lines at level setup and
 * clears the special; the ACS line opcodes then resolve ids through the
 * registry. */
#define MAX_TAGGED_LINES 64

static struct
{
  line_t *line;
  int     lineTag;
} TaggedLines[MAX_TAGGED_LINES];
static int TaggedLineCount;

void P_InitHexenTaggedLines(void)
{
  int i;

  TaggedLineCount = 0;
  for (i = 0; i < numlines; i++)
  {
    if (lines[i].special == 121)
    {                           /* Line_SetIdentification */
      if (lines[i].args[0])
      {
        if (TaggedLineCount == MAX_TAGGED_LINES)
          I_Error("P_InitHexenTaggedLines: MAX_TAGGED_LINES (%d) exceeded.",
                  MAX_TAGGED_LINES);
        TaggedLines[TaggedLineCount].line = &lines[i];
        TaggedLines[TaggedLineCount++].lineTag = lines[i].args[0];
      }
      lines[i].special = 0;
    }
  }
}

line_t *P_FindHexenLine(int lineTag, int *searchPosition)
{
  int i;

  for (i = *searchPosition + 1; i < TaggedLineCount; i++)
  {
    if (TaggedLines[i].lineTag == lineTag)
    {
      *searchPosition = i;
      return TaggedLines[i].line;
    }
  }
  *searchPosition = -1;
  return NULL;
}

dbool P_ExecuteHexenLineSpecial(int special, int *args, line_t *line,
                                int side, mobj_t *mo)
{
  dbool ok = false;

  switch (special)
  {
    case 1:                     /* Polyobj_StartLine (consumed at spawn) */
    case 5:                     /* Polyobj_ExplicitLine (consumed at spawn) */
      break;
    case 2:                     /* Polyobj_RotateLeft */
      ok = EV_RotatePoly(line, args, 1, false);
      break;
    case 3:                     /* Polyobj_RotateRight */
      ok = EV_RotatePoly(line, args, -1, false);
      break;
    case 4:                     /* Polyobj_Move */
      ok = EV_MovePoly(line, args, false, false);
      break;
    case 6:                     /* Polyobj_MoveTimes8 */
      ok = EV_MovePoly(line, args, true, false);
      break;
    case 7:                     /* Polyobj_DoorSwing */
      ok = EV_OpenPolyDoor(line, args, PODOOR_SWING);
      break;
    case 8:                     /* Polyobj_DoorSlide */
      ok = EV_OpenPolyDoor(line, args, PODOOR_SLIDE);
      break;
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
    case 202:                   /* Generic_Door (Boom generalized) */
      ok = Hexen_EV_GenericDoor(line, args, mo);
      break;
    case 70:                    /* Teleport(tid, sectortag, flags) */
      if (side == 0)
        ok = EV_HexenTeleportTagged(args[0], args[1], mo,
                                    (args[2] & 1) ? false : true);
      break;
    case 71:                    /* Teleport_NoFog: Hexen (tid, tag); ZDoom
                                 * (tid, useang, sectortag, keepheight) */
      if (side == 0)
      {
        int tag = map_format.zdoom ? args[2] : args[1];
        ok = EV_HexenTeleportTagged(args[0], tag, mo, false);
      }
      break;
    case 80:                    /* ACS_Execute */
      ok = P_StartACS(args[0], args[1], &args[2], mo, line, side);
      break;
    case 81:                    /* ACS_Suspend */
      ok = P_SuspendACS(args[0], args[1]);
      break;
    case 82:                    /* ACS_Terminate */
      ok = P_TerminateACS(args[0], args[1]);
      break;
    case 83:                    /* ACS_LockedExecute */
      if (CheckedLockedDoor(mo, args[4]))
      {
        int newArgs[5];
        newArgs[0] = args[0];
        newArgs[1] = args[1];
        newArgs[2] = args[2];
        newArgs[3] = args[3];
        newArgs[4] = 0;
        ok = P_StartACS(newArgs[0], newArgs[1], &newArgs[2], mo, line, side);
      }
      break;
    case 20:                    /* Floor_LowerByValue */
      ok = Hexen_EV_DoFloor(line, args, FLEV_LOWERFLOORBYVALUE);
      break;
    case 21:                    /* Floor_LowerToLowest */
      ok = Hexen_EV_DoFloor(line, args, FLEV_LOWERFLOORTOLOWEST);
      break;
    case 22:                    /* Floor_LowerToNearest */
      ok = Hexen_EV_DoFloor(line, args, FLEV_LOWERFLOOR);
      break;
    case 23:                    /* Floor_RaiseByValue */
      ok = Hexen_EV_DoFloor(line, args, FLEV_RAISEFLOORBYVALUE);
      break;
    case 24:                    /* Floor_RaiseToHighest */
      ok = Hexen_EV_DoFloor(line, args, FLEV_RAISEFLOOR);
      break;
    case 25:                    /* Floor_RaiseToNearest */
      ok = Hexen_EV_DoFloor(line, args, FLEV_RAISEFLOORTONEAREST);
      break;
    case 28:                    /* Floor_RaiseAndCrush */
      ok = Hexen_EV_DoFloor(line, args, FLEV_RAISEFLOORCRUSH);
      break;
    case 29:                    /* Pillar_Build (no crush) */
      ok = EV_BuildPillar(line, args, 0);
      break;
    case 26:                    /* Stairs_BuildDownNormal */
      ok = Hexen_EV_BuildStairs(line, args, -1, STAIRS_NORMAL);
      break;
    case 27:                    /* Stairs_BuildUpNormal */
      ok = Hexen_EV_BuildStairs(line, args, 1, STAIRS_NORMAL);
      break;
    case 31:                    /* Stairs_BuildDownSync */
      ok = Hexen_EV_BuildStairs(line, args, -1, STAIRS_SYNC);
      break;
    case 32:                    /* Stairs_BuildUpSync */
      ok = Hexen_EV_BuildStairs(line, args, 1, STAIRS_SYNC);
      break;
    case 30:                    /* Pillar_Open */
      ok = EV_OpenPillar(line, args);
      break;
    case 90:                    /* Polyobj_OR_RotateLeft */
      ok = EV_RotatePoly(line, args, 1, true);
      break;
    case 91:                    /* Polyobj_OR_RotateRight */
      ok = EV_RotatePoly(line, args, -1, true);
      break;
    case 92:                    /* Polyobj_OR_Move */
      ok = EV_MovePoly(line, args, false, true);
      break;
    case 93:                    /* Polyobj_OR_MoveTimes8 */
      ok = EV_MovePoly(line, args, true, true);
      break;
    case 94:                    /* Pillar_BuildAndCrush */
      ok = EV_BuildPillar(line, args, 1);
      break;
    case 72:                    /* ThrustThing */
      if (side == 0 && mo)
      {
        P_ThrustMobj(mo, args[0] * (ANG90 / 64), args[1] << FRACBITS);
        ok = true;
      }
      break;
    case 73:                    /* DamageThing */
      if (mo)
      {
        P_DamageMobj(mo, NULL, NULL, args[0] ? args[0] : 10000);
        ok = true;
      }
      break;
    case 74:                    /* Teleport_NewMap */
      if (side == 0)
      { /* only the front side teleports, and only living players */
        if (!(mo && mo->player && mo->player->playerstate == PST_DEAD))
        {
          G_Completed(args[0], args[1]);
          ok = true;
        }
      }
      break;
    case 75:                    /* Teleport_EndGame */
      if (side == 0)
      {
        if (!(mo && mo->player && mo->player->playerstate == PST_DEAD))
        {
          ok = true;
          if (deathmatch)
            G_Completed(1, 0); /* deathmatch victory loops to map 1 */
          else
            G_Completed(-1, 0);
        }
      }
      break;
    case 109:                   /* ForceLightning */
      P_ForceLightning();
      ok = true;
      break;
    case 120:                   /* Quake */
      ok = A_LocalQuake(args, mo);
      break;
    case 110:                   /* Light_RaiseByValue */
      ok = EV_SpawnLight(line, args, LITE_RAISEBYVALUE);
      break;
    case 111:                   /* Light_LowerByValue */
      ok = EV_SpawnLight(line, args, LITE_LOWERBYVALUE);
      break;
    case 112:                   /* Light_ChangeToValue */
      ok = EV_SpawnLight(line, args, LITE_CHANGETOVALUE);
      break;
    case 113:                   /* Light_Fade */
      ok = EV_SpawnLight(line, args, LITE_FADE);
      break;
    case 114:                   /* Light_Glow */
      ok = EV_SpawnLight(line, args, LITE_GLOW);
      break;
    case 115:                   /* Light_Flicker */
      ok = EV_SpawnLight(line, args, LITE_FLICKER);
      break;
    case 116:                   /* Light_Strobe */
      ok = EV_SpawnLight(line, args, LITE_STROBE);
      break;
    case 130:                   /* Thing_Activate */
      ok = EV_ThingActivate(args[0]);
      break;
    case 131:                   /* Thing_Deactivate */
      ok = EV_ThingDeactivate(args[0]);
      break;
    case 132:                   /* Thing_Remove */
      ok = EV_ThingRemove(args[0]);
      break;
    case 133:                   /* Thing_Destroy */
      ok = EV_ThingDestroy(args[0]);
      break;
    case 134:                   /* Thing_Projectile */
      ok = EV_ThingProjectile(args, 0);
      break;
    case 135:                   /* Thing_Spawn */
      ok = EV_ThingSpawn(args, 1);
      break;
    case 136:                   /* Thing_ProjectileGravity */
      ok = EV_ThingProjectile(args, 1);
      break;
    case 137:                   /* Thing_SpawnNoFog */
      ok = EV_ThingSpawn(args, 0);
      break;
    case 139:                   /* Thing_SpawnFacing */
      ok = EV_ThingSpawnFacing(args, 0);
      break;
    case 47:                    /* Ceiling_MoveToValue */
      ok = Hexen_EV_DoCeiling(line, args, CLEV_MOVETOVALUE);
      break;
    case 252:                   /* Ceiling_RaiseToNearest */
      ok = Hexen_EV_DoCeiling(line, args, CLEV_RAISETONEAREST);
      break;
    case 125:                   /* Thing_Move(tid, mapspot, nofog) */
      ok = EV_ThingMove(args[0], args[1], args[2]);
      break;
    case 145:                   /* Thing_SetGoal / Thing_Stop */
      ok = EV_ThingStop(args[0]);
      break;
    case 173:                   /* Thing_Raise(tid) */
      ok = EV_ThingRaise(args[0]);
      break;
    case 176:                   /* Thing_ChangeTID(oldtid, newtid) */
      ok = EV_ThingChangeTID(args[0], args[1]);
      break;
    case 177:                   /* Thing_Hate(hater, hatee, type) */
      ok = EV_ThingHate(args[0], args[1]);
      break;
    case 127:                   /* SetThingSpecial(tid, spec, a1..a4 partial) */
      ok = EV_SetThingSpecial(args);
      break;
    case 155:                   /* SetLineBlocking-ish (Line_SetBlockMonsters
                                 * uses 156); 155 is Line_AlignFloor in ZDoom,
                                 * recognised as a no-op decoration */
      ok = true;
      break;
    case 156:                   /* Line_SetBlockMonsters(lineid, on) */
      ok = EV_SetLineBlockMonsters(args[0], args[1]);
      break;
    case 278:                   /* SetLineSpecial(id, spec, a1..a5) */
      ok = EV_SetLineSpecial(args);
      break;
    case 35:                    /* Floor_RaiseByValueTimes8 */
      ok = Hexen_EV_DoFloor(line, args, FLEV_RAISEBYVALUETIMES8);
      break;
    case 36:                    /* Floor_LowerByValueTimes8 */
      ok = Hexen_EV_DoFloor(line, args, FLEV_LOWERBYVALUETIMES8);
      break;
    case 40:                    /* Ceiling_LowerByValue */
      ok = Hexen_EV_DoCeiling(line, args, CLEV_LOWERBYVALUE);
      break;
    case 41:                    /* Ceiling_RaiseByValue */
      ok = Hexen_EV_DoCeiling(line, args, CLEV_RAISEBYVALUE);
      break;
    case 42:                    /* Ceiling_CrushAndRaise */
      ok = Hexen_EV_DoCeiling(line, args, CLEV_CRUSHANDRAISE);
      break;
    case 43:                    /* Ceiling_LowerAndCrush */
      ok = Hexen_EV_DoCeiling(line, args, CLEV_LOWERANDCRUSH);
      break;
    case 44:                    /* Ceiling_CrushStop */
      ok = Hexen_EV_CeilingCrushStop(line, args);
      break;
    case 46:                    /* Floor_CrushStop */
      ok = Hexen_EV_FloorCrushStop(line, args);
      break;
    case 95:                    /* FloorAndCeiling_LowerByValue */
      ok = Hexen_EV_DoFloorAndCeiling(line, args, 0);
      break;
    case 96:                    /* FloorAndCeiling_RaiseByValue */
      ok = Hexen_EV_DoFloorAndCeiling(line, args, 1);
      break;
    case 140:                   /* Sector_SoundChange */
      ok = Hexen_EV_SectorSoundChange(args);
      break;
    case 138:                   /* Floor_Waggle */
      ok = EV_StartFloorWaggle(args[0], args[1], args[2], args[3], args[4]);
      break;
    case 45:                    /* Ceiling_CrushRaiseAndStay */
      ok = Hexen_EV_DoCeiling(line, args, CLEV_CRUSHRAISEANDSTAY);
      break;
    case 69:                    /* Ceiling_MoveToValueTimes8 */
      ok = Hexen_EV_DoCeiling(line, args, CLEV_MOVETOVALUETIMES8);
      break;
    case 66:                    /* Floor_LowerInstant */
      ok = Hexen_EV_DoFloor(line, args, FLEV_LOWERTIMES8INSTANT);
      break;
    case 67:                    /* Floor_RaiseInstant */
      ok = Hexen_EV_DoFloor(line, args, FLEV_RAISETIMES8INSTANT);
      break;
    case 68:                    /* Floor_MoveToValueTimes8 */
      ok = Hexen_EV_DoFloor(line, args, FLEV_MOVETOVALUETIMES8);
      break;
    case 60:                    /* Plat_PerpetualRaise */
      ok = EV_DoHexenPlat(line, args, PLAT_PERPETUALRAISE, 0);
      break;
    case 61:                    /* Plat_Stop */
      Hexen_EV_StopPlat(line, args);
      ok = true;
      break;
    case 62:                    /* Plat_DownWaitUpStay */
      ok = EV_DoHexenPlat(line, args, PLAT_DOWNWAITUPSTAY, 0);
      break;
    case 63:                    /* Plat_DownByValueWaitUpStay */
      ok = EV_DoHexenPlat(line, args, PLAT_DOWNBYVALUEWAITUPSTAY, 0);
      break;
    case 64:                    /* Plat_UpWaitDownStay */
      ok = EV_DoHexenPlat(line, args, PLAT_UPWAITDOWNSTAY, 0);
      break;
    case 65:                    /* Plat_UpByValueWaitDownStay */
      ok = EV_DoHexenPlat(line, args, PLAT_UPBYVALUEWAITDOWNSTAY, 0);
      break;
    /* Recognised-but-unmodelled specials.  These are cosmetic or rendering
     * features the software engine does not implement (ZDoom colored sector
     * lighting/fog, generalised scrollers, dynamic-light tweaks, animated
     * door textures, fake-floor height transfer).  They are acknowledged as
     * handled so that scripts which call them -- and especially scripts that
     * branch on their result -- keep running instead of stalling on a
     * spurious failure.  The visual effect is simply absent. */
    case 14:                    /* Door_Animated (texture anim only) */
    case 15:                    /* Autosave */
    case 16:                    /* Transfer_WallLight */
    case 38:                    /* Ceiling_Waggle (cosmetic ceiling bob) */
    case 51:                    /* Sector_SetLink */
    case 52:                    /* Scroll_Wall / Scroll_Texture_Left */
    case 53:                    /* Scroll_Texture_Right */
    case 117:                   /* Light_Stop */
    case 180:                   /* Sector_SetTranslucent */
    case 205:                   /* Generic_Crusher (use Ceiling_* instead) */
    case 209:                   /* Transfer_Heights (fake floors) */
    case 212:                   /* Sector_SetColor (colored lighting) */
    case 213:                   /* Sector_SetFade (colored fog) */
    case 214:                   /* Sector_SetDamage */
    case 217:                   /* Scroll_Texture_Model / SetTextureScale */
    case 218:                   /* Scroll_Floor */
    case 222:                   /* Scroll_Texture_Both */
    case 223:                   /* Sector_SetFriction */
    case 224:                   /* Sector_SetGravity */
    case 225:                   /* Sector_SetColorGradient-ish */
    case 228:                   /* Sector_SetCurrentHeight / wind */
    case 234:                   /* Light_ChangeToValueTimes */
      ok = true;
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
  int  args[5];
  dbool repeat;
  dbool ok;

  if (!line->special)
    return false;
  if (GET_SPAC(line->flags) != activation)
    return false;

  COLLAPSE_SPECIAL_ARGS(args, line->args);
  ok = map_format.execute_line_special(line->special, args, line, side, mo);
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

/* Vanilla hexen's CheckForPushSpecial: things bumping or shooting into a
 * line fire its push or impact activation.  Following dsda-doom's default,
 * a missile activates impact lines as its shooter (mo->target) so ACS
 * scripts see a sensible activator; PUSHWALL movers (the player classes)
 * fire push lines as themselves. */
void P_CheckForPushSpecial(line_t *line, int side, mobj_t *mobj)
{
  if (line->special)
  {
    if (mobj->flags2 & MF2_PUSHWALL)
    {
      P_ActivateHexenLine(line, mobj, side, SPAC_PUSH);
    }
    else if (mobj->flags2 & MF2_IMPACT)
    {
      if (!(mobj->flags & MF_MISSILE) || !mobj->target)
        P_ActivateHexenLine(line, mobj, side, SPAC_IMPACT);
      else
        P_ActivateHexenLine(line, mobj->target, side, SPAC_IMPACT);
    }
  }
}

dbool P_UseHexenSpecialLine(mobj_t *thing, line_t *line, int side)
{
  return P_ActivateHexenLine(line, thing, side, SPAC_USE);
}
