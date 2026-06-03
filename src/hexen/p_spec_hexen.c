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

/* --- Floors ---------------------------------------------------------------
 *
 * Value-driven floor movers.  These reuse the engine's T_MoveFloor thinker,
 * which moves the sector floor to floordestheight and then removes itself;
 * the Hexen movement types fall through its texture/special switch unchanged.
 * Until the sound-sequence subsystem lands the movement sound is the engine's
 * default (sfx_stnmov inside T_MoveFloor). */
int Hexen_EV_DoFloor(line_t *line, byte *args, floor_e floortype)
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
        floor->crush = TRUE;
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
        floor->floordestheight = args[2] * FRACUNIT * 8;
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
          P_RemoveActiveCeiling(ceiling);
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
          P_RemoveActiveCeiling(ceiling);
      }
      /* crushed: the crusher types simply keep pushing */
      break;
  }
}

int Hexen_EV_DoCeiling(line_t *line, byte *args, ceiling_e type)
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
        ceiling->crush = TRUE;
        ceiling->topheight = sec->ceilingheight;
        ceiling->bottomheight = sec->floorheight + 8 * FRACUNIT;
        ceiling->direction = -1;
        break;
      case CLEV_CRUSHANDRAISE:
        ceiling->topheight = sec->ceilingheight;
        ceiling->crush = TRUE;
        ceiling->bottomheight = sec->floorheight + 8 * FRACUNIT;
        ceiling->direction = -1;
        break;
      case CLEV_LOWERANDCRUSH:
        ceiling->crush = TRUE;
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
      default:
        rtn = 0;
        break;
    }

    ceiling->tag  = sec->tag;
    ceiling->type = type;

    if (rtn)
      P_AddActiveCeiling(ceiling);
    else
    {
      sec->ceilingdata = NULL;
      P_RemoveThinker(&ceiling->thinker);
    }
  }
  return rtn;
}

int Hexen_EV_CeilingCrushStop(line_t *line, byte *args)
{
  ceilinglist_t *cl;

  (void) line;

  for (cl = activeceilings; cl; cl = cl->next)
  {
    ceiling_t *ceiling = cl->ceiling;
    if (ceiling->tag == args[0])
    {
      P_RemoveActiveCeiling(ceiling);
      return 1;
    }
  }
  return 0;
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
    P_RemoveThinker(&pillar->thinker);
  }
}

int EV_BuildPillar(line_t *line, byte *args, int crush)
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
    pillar->crush       = (crush && args[3]) ? TRUE : FALSE;
  }
  return rtn;
}

int EV_OpenPillar(line_t *line, byte *args)
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
  }
  return rtn;
}

/* --- Platforms ------------------------------------------------------------
 *
 * Lifts.  These reuse the engine's T_PlatRaise thinker and active-plat list;
 * the Hexen movement types set low/high/wait/status and otherwise fall
 * through the thinker's Doom change-type handling.  Movement sound is the
 * engine default until the sound-sequence subsystem lands. */
int EV_DoHexenPlat(line_t *line, byte *args, plattype_e type, int amount)
{
  int       secnum;
  int       rtn = 0;
  sector_t *sec;
  plat_t   *plat;

  (void) line;
  (void) amount;

  HEXEN_FOR_TAGGED_SECTORS(secnum, args[0])
  {
    sec = &sectors[secnum];
    if (sec->floordata || sec->ceilingdata)
      continue;

    rtn = 1;
    plat = Z_Malloc(sizeof(*plat), PU_LEVEL, 0);
    memset(plat, 0, sizeof(*plat));
    P_AddThinker(&plat->thinker);
    plat->type = type;
    plat->sector = sec;
    plat->sector->floordata = plat;
    plat->thinker.function.arg1 = (void (*)(void *))T_PlatRaise;
    plat->crush = FALSE;
    plat->tag   = args[0];
    plat->speed = args[1] * (FRACUNIT / 8);

    switch (type)
    {
      case PLAT_DOWNWAITUPSTAY:
        plat->low = P_FindLowestFloorSurrounding(sec) + 8 * FRACUNIT;
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
        plat->low = P_FindLowestFloorSurrounding(sec) + 8 * FRACUNIT;
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
  }
  return rtn;
}

void Hexen_EV_StopPlat(line_t *line, byte *args)
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
    case 30:                    /* Pillar_Open */
      ok = EV_OpenPillar(line, args);
      break;
    case 94:                    /* Pillar_BuildAndCrush */
      ok = EV_BuildPillar(line, args, 1);
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
