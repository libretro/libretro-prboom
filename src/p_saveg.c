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
 *      Archiving: SaveGame I/O.
 *
 *-----------------------------------------------------------------------------*/

#include "doomstat.h"
#include "r_main.h"
#include "p_maputl.h"
#include "p_spec.h"
#include "p_tick.h"
#include "p_saveg.h"
#include "map_format.h"
#include "m_random.h"
#include "am_map.h"
#include "p_enemy.h"
#include "hexen/p_acs.h"
#include "hexen/sn_sonix.h"
#include "lprintf.h"

uint8_t *save_p;

// Pads save_p to a 4-byte boundary
//  so that the load/save works on SGI&Gecko.
#define PADSAVEP()    do { save_p += (4 - ((uintptr_t) save_p & 3)) & 3; } while (0)
//
// P_ArchivePlayers
//
void P_ArchivePlayers (void)
{
  int i;

  CheckSaveGame(sizeof(player_t) * MAXPLAYERS); // killough
  for (i=0 ; i<MAXPLAYERS ; i++)
    if (playeringame[i])
      {
        int      j;
        player_t *dest;

        PADSAVEP();
        dest = (player_t *) save_p;
        memcpy(dest, &players[i], sizeof(player_t));
        save_p += sizeof(player_t);
        for (j=0; j<NUMPSPRITES; j++)
          if (dest->psprites[j].state)
            dest->psprites[j].state =
              (state_t *)(dest->psprites[j].state-states);
      }
}

//
// P_UnArchivePlayers
//
void P_UnArchivePlayers (void)
{
  int i;

  for (i=0 ; i<MAXPLAYERS ; i++)
    if (playeringame[i])
      {
        int j;

        PADSAVEP();

        memcpy(&players[i], save_p, sizeof(player_t));
        save_p += sizeof(player_t);

        // will be set when unarc thinker
        players[i].mo = NULL;
        players[i].message = NULL;
        players[i].attacker = NULL;

        for (j=0 ; j<NUMPSPRITES ; j++)
          if (players[i]. psprites[j].state)
            players[i]. psprites[j].state =
              &states[ (uintptr_t)players[i].psprites[j].state ];
      }
}


//
// P_ArchiveWorld
//
void P_ArchiveWorld (void)
{
  int            i;
  const sector_t *sec;
  const line_t   *li;
  const side_t   *si;
  short          *put;

  // killough 3/22/98: fix bug caused by hoisting save_p too early
  // killough 10/98: adjust size for changes below
  size_t size =
    (sizeof(short)*5 + sizeof sec->floorheight + sizeof sec->ceilingheight)
    * numsectors + sizeof(short)*3*numlines + 4;

  for (i=0; i<numlines; i++)
    {
      if (lines[i].sidenum[0] != NO_INDEX)
        size +=
    sizeof(short)*3 + sizeof si->textureoffset + sizeof si->rowoffset;
      if (lines[i].sidenum[1] != NO_INDEX)
  size +=
    sizeof(short)*3 + sizeof si->textureoffset + sizeof si->rowoffset;
    }

  CheckSaveGame(size); // killough

  PADSAVEP();                // killough 3/22/98

  put = (short *)save_p;

  // do sectors
  for (i=0, sec = sectors ; i<numsectors ; i++,sec++)
    {
      // killough 10/98: save full floor & ceiling heights, including fraction
      memcpy(put, &sec->floorheight, sizeof sec->floorheight);
      put = (void *)((char *) put + sizeof sec->floorheight);
      memcpy(put, &sec->ceilingheight, sizeof sec->ceilingheight);
      put = (void *)((char *) put + sizeof sec->ceilingheight);

      *put++ = sec->floorpic;
      *put++ = sec->ceilingpic;
      *put++ = sec->lightlevel;
      *put++ = sec->special;            // needed?   yes -- transfer types
      *put++ = sec->tag;                // needed?   need them -- killough
    }

  // do lines
  for (i=0, li = lines ; i<numlines ; i++,li++)
    {
      int j;

      *put++ = li->flags;
      *put++ = li->special;
      *put++ = li->tag;

      for (j=0; j<2; j++)
        if (li->sidenum[j] != NO_INDEX)
          {
      si = &sides[li->sidenum[j]];

      // killough 10/98: save full sidedef offsets,
      // preserving fractional scroll offsets

      memcpy(put, &si->textureoffset, sizeof si->textureoffset);
      put = (void *)((char *) put + sizeof si->textureoffset);
      memcpy(put, &si->rowoffset, sizeof si->rowoffset);
      put = (void *)((char *) put + sizeof si->rowoffset);

            *put++ = si->toptexture;
            *put++ = si->bottomtexture;
            *put++ = si->midtexture;
          }
    }
  save_p = (uint8_t*) put;
}



//
// P_UnArchiveWorld
//
void P_UnArchiveWorld (void)
{
  int          i;
  sector_t     *sec;
  line_t       *li;
  short        *get;

  PADSAVEP();                // killough 3/22/98

  get = (short *) save_p;

  // do sectors
  for (i=0, sec = sectors ; i<numsectors ; i++,sec++)
    {
      // killough 10/98: load full floor & ceiling heights, including fractions

      memcpy(&sec->floorheight, get, sizeof sec->floorheight);
      get = (void *)((char *) get + sizeof sec->floorheight);
      memcpy(&sec->ceilingheight, get, sizeof sec->ceilingheight);
      get = (void *)((char *) get + sizeof sec->ceilingheight);

      sec->floorpic = *get++;
      sec->ceilingpic = *get++;
      sec->lightlevel = *get++;
      sec->special = *get++;
      sec->tag = *get++;
      sec->ceilingdata = 0; //jff 2/22/98 now three thinker fields, not two
      sec->floordata = 0;
      sec->lightingdata = 0;
      sec->soundtarget = 0;
    }

  // do lines
  for (i=0, li = lines ; i<numlines ; i++,li++)
    {
      int j;

      li->flags = *get++;
      li->special = *get++;
      li->tag = *get++;
      for (j=0 ; j<2 ; j++)
        if (li->sidenum[j] != NO_INDEX)
          {
            side_t *si = &sides[li->sidenum[j]];

      // killough 10/98: load full sidedef offsets, including fractions

      memcpy(&si->textureoffset, get, sizeof si->textureoffset);
      get = (void *)((char *) get + sizeof si->textureoffset);
      memcpy(&si->rowoffset, get, sizeof si->rowoffset);
      get = (void *)((char *) get + sizeof si->rowoffset);

            si->toptexture = *get++;
            si->bottomtexture = *get++;
            si->midtexture = *get++;
          }
    }
  save_p = (uint8_t*) get;
}

//
// Thinkers
//

typedef enum {
  tc_end,
  tc_mobj
} thinkerclass_t;

// phares 9/13/98: Moved this code outside of P_ArchiveThinkers so the
// thinker indices could be used by the code that saves sector info.

static intptr_t number_of_thinkers;

void P_ThinkerToIndex(void)
{
  thinker_t *th;

  // killough 2/14/98:
  // count the number of thinkers, and mark each one with its index, using
  // the prev field as a placeholder, since it can be restored later.

  number_of_thinkers = 0;
  for (th = thinkercap.next ; th != &thinkercap ; th=th->next)
    if (th->function.arg1 == (void (*)(void *))P_MobjThinker)
      th->prev = (thinker_t *) ++number_of_thinkers;
}

// phares 9/13/98: Moved this code outside of P_ArchiveThinkers so the
// thinker indices could be used by the code that saves sector info.

void P_IndexToThinker(void)
{
  // killough 2/14/98: restore prev pointers
  thinker_t *th;
  thinker_t *prev = &thinkercap;

  for (th = thinkercap.next ; th != &thinkercap ; prev=th, th=th->next)
    th->prev = prev;
}

//
// P_ArchiveThinkers
//
// 2/14/98 killough: substantially modified to fix savegame bugs

/* Hexen and Heretic overload mobj special1/special2 as either an int or a
 * mobj pointer depending on the thing type.  Pointer-valued specials must be
 * converted to thinker indices on save and back on load, exactly like
 * target/tracer; integer-valued ones must be left alone (vanilla solves the
 * same union with per-type knowledge in RestoreMobj).  These are all the
 * types whose specials hold pointers across tics. */

#define PSF_SPECIAL1 1
#define PSF_SPECIAL2 2

static int P_PointerSpecialFields(int type)
{
  if (hexen)
    switch (type)
    {
      case HEXEN_MT_HOLY_TAIL:                  /* next segment + parent  */
      case HEXEN_MT_LIGHTNING_FLOOR:            /* partner + zap links    */
      case HEXEN_MT_LIGHTNING_CEILING:
        return PSF_SPECIAL1 | PSF_SPECIAL2;
      case HEXEN_MT_HOLY_FX:                    /* seek target            */
      case HEXEN_MT_SORCFX1:                    /* seek target            */
      case HEXEN_MT_BISH_FX:                    /* seek target            */
      case HEXEN_MT_MSTAFF_FX2:                 /* seek target            */
      case HEXEN_MT_KORAX_SPIRIT1:              /* seek target            */
      case HEXEN_MT_KORAX_SPIRIT2:
      case HEXEN_MT_KORAX_SPIRIT3:
      case HEXEN_MT_KORAX_SPIRIT4:
      case HEXEN_MT_KORAX_SPIRIT5:
      case HEXEN_MT_KORAX_SPIRIT6:
      case HEXEN_MT_DRAGON:                     /* destination map spot   */
      case HEXEN_MT_MINOTAUR:                   /* summoning master       */
      case HEXEN_MT_SUMMON_FX:                  /* thrower                */
      case HEXEN_MT_THRUSTFLOOR_UP:             /* impaled dirt clump     */
      case HEXEN_MT_THRUSTFLOOR_DOWN:
        return PSF_SPECIAL1;
      case HEXEN_MT_LIGHTNING_ZAP:              /* parent bolt            */
        return PSF_SPECIAL2;
      default:
        return 0;
    }
  if (heretic)
    switch (type)
    {
      case HERETIC_MT_MACEFX4:                  /* seek target            */
      case HERETIC_MT_HORNRODFX2:
      case HERETIC_MT_MUMMYFX1:
      case HERETIC_MT_PHOENIXFX1:
      case HERETIC_MT_WHIRLWIND:
        return PSF_SPECIAL1;
      case HERETIC_MT_POD:                      /* pod generator          */
        return PSF_SPECIAL2;
      default:
        return 0;
    }
  return 0;
}

void P_ArchiveThinkers (void)
{
  thinker_t *th;

  CheckSaveGame(sizeof brain);      // killough 3/26/98: Save boss brain state
  memcpy(save_p, &brain, sizeof brain);
  save_p += sizeof brain;

  /* check that enough room is available in savegame buffer
   * - killough 2/14/98
   * cph - use number_of_thinkers saved by P_ThinkerToIndex above
   * size per object is sizeof(mobj_t) - 2*sizeof(void*) - 4*sizeof(fixed_t) plus
   * padded type (4) plus 5*sizeof(void*), i.e. sizeof(mobj_t) + 4 +
   * 3*sizeof(void*)
   * cph - +1 for the tc_end
   */
  CheckSaveGame(number_of_thinkers*(sizeof(mobj_t)-3*sizeof(fixed_t)+4+3*sizeof(void*)) +1);

  // save off the current thinkers
  for (th = thinkercap.next ; th != &thinkercap ; th=th->next)
    if (th->function.arg1 == (void (*)(void *))P_MobjThinker)
      {
        mobj_t *mobj;

        *save_p++ = tc_mobj;
        PADSAVEP();
        mobj = (mobj_t *)save_p;

        if (raven)
        {
          /* The legacy stream layout below predates the Heretic/Hexen
           * fields, which were appended after the old end of mobj_t; its
           * fixed "all but the last words" arithmetic would truncate them
           * (losing tid, the death action special, the damage override and
           * floorclip).  Raven saves are not format-compatible with anything
           * else anyway (see the RVN tag), so store the full struct. */
          memcpy (mobj, th, sizeof(*mobj));
          save_p += sizeof(*mobj);
          mobj->state = (state_t *)(mobj->state - states);
          mobj->touching_sectorlist = NULL;

          if (mobj->lastenemy)
            mobj->lastenemy = mobj->lastenemy->thinker.function.arg1 ==
              (void (*)(void *))P_MobjThinker ?
              (mobj_t *) mobj->lastenemy->thinker.prev : NULL;
        }
        else
        {
	/* cph 2006/07/30 - 
	 * The end of mobj_t changed from
	 *  dbool   invisible;
	 *  mobj_t* lastenemy;
	 *  mobj_t* above_monster;
	 *  mobj_t* below_monster;
	 *  void* touching_sectorlist;
	 * to
	 *  mobj_t* lastenemy;
	 *  void* touching_sectorlist;
         *  fixed_t PrevX, PrevY, PrevZ, padding;
	 * at prboom 2.4.4. There is code here to preserve the savegame format.
	 *
	 * touching_sectorlist is reconstructed anyway, so we now leave off the
	 * last 2 words of mobj_t, write 5 words of 0 and then write lastenemy
	 * into the second of these.
	 */
        memcpy (mobj, th, sizeof(*mobj) - 2*sizeof(void*));
        save_p += sizeof(*mobj) - 2*sizeof(void*) - 4*sizeof(fixed_t);
        memset (save_p, 0, 5*sizeof(void*));
        mobj->state = (state_t *)(mobj->state - states);
        }

        // killough 2/14/98: convert pointers into indices.
        // Fixes many savegame problems, by properly saving
        // target and tracer fields. Note: we store NULL if
        // the thinker pointed to by these fields is not a
        // mobj thinker.

        if (mobj->target)
          mobj->target = mobj->target->thinker.function.arg1 ==
            (void (*)(void *))P_MobjThinker ?
            (mobj_t *) mobj->target->thinker.prev : NULL;

        if (mobj->tracer)
          mobj->tracer = mobj->tracer->thinker.function.arg1 ==
            (void (*)(void *))P_MobjThinker ?
            (mobj_t *) mobj->tracer->thinker.prev : NULL;

        /* pointer-valued raven specials get the same index treatment;
         * integer-valued ones are saved verbatim by the memcpy above */
        if (raven)
        {
          int psf = P_PointerSpecialFields(mobj->type);

          if ((psf & PSF_SPECIAL1) && mobj->special1.m)
            mobj->special1.m = mobj->special1.m->thinker.function.arg1 ==
              (void (*)(void *))P_MobjThinker ?
              (mobj_t *) mobj->special1.m->thinker.prev : NULL;

          if ((psf & PSF_SPECIAL2) && mobj->special2.m)
            mobj->special2.m = mobj->special2.m->thinker.function.arg1 ==
              (void (*)(void *))P_MobjThinker ?
              (mobj_t *) mobj->special2.m->thinker.prev : NULL;
        }

        // killough 2/14/98: new field: save last known enemy. Prevents
        // monsters from going to sleep after killing monsters and not
        // seeing player anymore.

        if (!raven)
        {
        if (((mobj_t*)th)->lastenemy && ((mobj_t*)th)->lastenemy->thinker.function.arg1 == (void (*)(void *))P_MobjThinker) {
          memcpy (save_p + sizeof(void*), &(((mobj_t*)th)->lastenemy->thinker.prev), sizeof(void*));
	}

        // killough 2/14/98: end changes

        save_p += 5*sizeof(void*);
        }

        if (mobj->player)
          mobj->player = (player_t *)((mobj->player-players) + 1);
      }

  // add a terminating marker
  *save_p++ = tc_end;

  // killough 9/14/98: save soundtargets
  {
    int i;
    CheckSaveGame(numsectors * sizeof(mobj_t *));       // killough 9/14/98
    for (i = 0; i < numsectors; i++)
    {
      mobj_t *target = sectors[i].soundtarget;

      // Fix crash on reload when a soundtarget points to a removed corpse
      // (prboom bug #1590350)
      if (target && target->thinker.function.arg1 == (void (*)(void *))P_MobjThinker)
        target = (mobj_t *) target->thinker.prev;
      else
        target = NULL;

      if (target)
        memcpy(save_p, &target, sizeof target);
      else
        memset(save_p, 0, sizeof target);

      save_p += sizeof target;
    }
  }

  /* Running ACS scripts are thinkers too; they are archived here, rather
   * than with the specials, because the activator fixup needs the thinker
   * indices that P_ThinkerToIndex stored in the prev fields (and the load
   * side needs the mobj translation table). */
  if (hexen)
  {
    int acs_count = 0;

    for (th = thinkercap.next; th != &thinkercap; th = th->next)
      if (th->function.arg1 == (void (*)(void *)) T_InterpretACS)
        acs_count++;

    CheckSaveGame(sizeof(acs_count) + acs_count * (sizeof(acs_t) + 4));
    memcpy(save_p, &acs_count, sizeof(acs_count));
    save_p += sizeof(acs_count);

    for (th = thinkercap.next; th != &thinkercap; th = th->next)
      if (th->function.arg1 == (void (*)(void *)) T_InterpretACS)
      {
        acs_t *acs;

        PADSAVEP();
        acs = (acs_t *) save_p;
        memcpy(acs, th, sizeof(*acs));
        save_p += sizeof(*acs);

        if (acs->activator)
          acs->activator = acs->activator->thinker.function.arg1 ==
            (void (*)(void *))P_MobjThinker ?
            (mobj_t *) acs->activator->thinker.prev : NULL;

        /* 0 = no line, otherwise index + 1 */
        acs->line = acs->line ?
          (line_t *) (uintptr_t) ((acs->line - lines) + 1) : NULL;
      }
  }
}

/*
 * killough 11/98
 *
 * Same as P_SetTarget() in p_tick.c, except that the target is nullified
 * first, so that no old target's reference count is decreased (when loading
 * savegames, old targets are indices, not really pointers to targets).
 */

static void P_SetNewTarget(mobj_t **mop, mobj_t *targ)
{
  *mop = NULL;
  P_SetTarget(mop, targ);
}

//
// P_UnArchiveThinkers
//
// 2/14/98 killough: substantially modified to fix savegame bugs
//

// savegame file stores ints in the corresponding * field; this function
// safely casts them back to int.
static int P_GetMobj(mobj_t* mi, size_t s)
{
  uintptr_t i = (uintptr_t)mi;
  if (i >= s) I_Error("Corrupt savegame");
  return (int)i;
}

void P_UnArchiveThinkers (void)
{
  thinker_t *th;
  mobj_t    **mobj_p;    // killough 2/14/98: Translation table
  size_t    size;        // killough 2/14/98: size of or index into table

  totallive = 0;
  // killough 3/26/98: Load boss brain state
  memcpy(&brain, save_p, sizeof brain);
  save_p += sizeof brain;

  // remove all the current thinkers
  for (th = thinkercap.next; th != &thinkercap; )
    {
      thinker_t *next = th->next;
      if (th->function.arg1 == (void (*)(void *))P_MobjThinker)
      {
        P_RemoveMobj ((mobj_t *) th);
        P_RemoveThinkerDelayed(th); // fix mobj leak
      }
      else
        Z_Free (th);
      th = next;
    }
  P_InitThinkers ();

  // killough 2/14/98: count number of thinkers by skipping through them
  {
    uint8_t *sp = save_p;     // save pointer and skip header
    for (size = 1; *save_p++ == tc_mobj; size++)  // killough 2/14/98
      {                     // skip all entries, adding up count
        PADSAVEP();
	/* cph 2006/07/30 - see comment below for change in layout of mobj_t */
        save_p += raven ? sizeof(mobj_t)
                        : sizeof(mobj_t)+3*sizeof(void*)-4*sizeof(fixed_t);
      }

    if (*--save_p != tc_end)
      I_Error ("P_UnArchiveThinkers: Unknown tclass %i in savegame", *save_p);

    // first table entry special: 0 maps to NULL
    *(mobj_p = malloc(size * sizeof *mobj_p)) = 0;   // table of pointers
    save_p = sp;           // restore save pointer
  }

  // read in saved thinkers
  for (size = 1; *save_p++ == tc_mobj; size++)    // killough 2/14/98
    {
      mobj_t *mobj = Z_Malloc(sizeof(mobj_t), PU_LEVEL, NULL);

      // killough 2/14/98 -- insert pointers to thinkers into table, in order:
      mobj_p[size] = mobj;

      PADSAVEP();
      /* cph 2006/07/30 - 
       * The end of mobj_t changed from
       *  dbool   invisible;
       *  mobj_t* lastenemy;
       *  mobj_t* above_monster;
       *  mobj_t* below_monster;
       *  void* touching_sectorlist;
       * to
       *  mobj_t* lastenemy;
       *  void* touching_sectorlist;
       *  fixed_t PrevX, PrevY, PrevZ;
       * at prboom 2.4.4. There is code here to preserve the savegame format.
       *
       * touching_sectorlist is reconstructed anyway, so we now read in all 
       * but the last 5 words from the savegame (filling all but the last 2
       * fields of our current mobj_t. We then pull lastenemy from the 2nd of
       * the 5 leftover words, and skip the others.
       */
      if (raven)
      {
        /* full-struct layout; see the matching branch in P_ArchiveThinkers */
        memcpy (mobj, save_p, sizeof(mobj_t));
        save_p += sizeof(mobj_t);
        mobj->touching_sectorlist = NULL;
      }
      else
      {
      memcpy (mobj, save_p, sizeof(mobj_t)-2*sizeof(void*)-4*sizeof(fixed_t));
      save_p += sizeof(mobj_t)-sizeof(void*)-4*sizeof(fixed_t);
      memcpy (&(mobj->lastenemy), save_p, sizeof(void*));
      save_p += 4*sizeof(void*);
      }
      mobj->state = states + (uintptr_t) mobj->state;

      if (mobj->player)
        (mobj->player = &players[(uintptr_t) mobj->player - 1]) -> mo = mobj;

      // avoid glitchy interpolation
      mobj->PrevX = mobj->x;
      mobj->PrevY = mobj->y;
      mobj->PrevZ = mobj->z;

      P_SetThingPosition (mobj);
      mobj->info = &mobjinfo[mobj->type];

      // killough 2/28/98:
      // Fix for falling down into a wall after savegame loaded:
      //      mobj->floorz = mobj->subsector->sector->floorheight;
      //      mobj->ceilingz = mobj->subsector->sector->ceilingheight;

      mobj->thinker.function.arg1 = (void (*)(void *))P_MobjThinker;
      P_AddThinker (&mobj->thinker);

      if (!((mobj->flags ^ MF_COUNTKILL) & (MF_FRIEND | MF_COUNTKILL | MF_CORPSE)))
        totallive++;
    }

  // killough 2/14/98: adjust target and tracer fields, plus
  // lastenemy field, to correctly point to mobj thinkers.
  // NULL entries automatically handled by first table entry.
  //
  // killough 11/98: use P_SetNewTarget() to set fields

  for (th = thinkercap.next ; th != &thinkercap ; th=th->next)
    {
      P_SetNewTarget(&((mobj_t *) th)->target,
        mobj_p[P_GetMobj(((mobj_t *)th)->target,size)]);

      P_SetNewTarget(&((mobj_t *) th)->tracer,
        mobj_p[P_GetMobj(((mobj_t *)th)->tracer,size)]);

      P_SetNewTarget(&((mobj_t *) th)->lastenemy,
        mobj_p[P_GetMobj(((mobj_t *)th)->lastenemy,size)]);

      /* pointer-valued raven specials (see P_ArchiveThinkers) */
      if (raven)
      {
        int psf = P_PointerSpecialFields(((mobj_t *) th)->type);

        if (psf & PSF_SPECIAL1)
          P_SetNewTarget(&((mobj_t *) th)->special1.m,
            mobj_p[P_GetMobj(((mobj_t *) th)->special1.m, size)]);

        if (psf & PSF_SPECIAL2)
          P_SetNewTarget(&((mobj_t *) th)->special2.m,
            mobj_p[P_GetMobj(((mobj_t *) th)->special2.m, size)]);
      }
    }

  {  // killough 9/14/98: restore soundtargets
    int i;
    for (i = 0; i < numsectors; i++)
    {
      mobj_t *target;
      memcpy(&target, save_p, sizeof target);
      save_p += sizeof target;

      // Must verify soundtarget. See P_ArchiveThinkers.
      // Check if 'saved' soundtarget pointer was NULL
      // or otherwise invalid.  Cache the P_GetMobj() result so
      // the bounds check is unambiguously unsigned (the original
      // int >= size_t comparison flagged -Wsign-compare) and so
      // we don't run the same call twice on the success path.
      if (!target)
        sectors[i].soundtarget = 0;
      else
      {
        int idx = P_GetMobj(target, size);
        if (idx < 0 || (size_t)idx >= size)
          sectors[i].soundtarget = 0;
        else
          P_SetNewTarget(&sectors[i].soundtarget, mobj_p[idx]);
      }
    }
  }

  /* restore running ACS scripts (see the matching block in
   * P_ArchiveThinkers); needs the mobj table for the activator */
  if (hexen)
  {
    int acs_count;
    int i;

    memcpy(&acs_count, save_p, sizeof(acs_count));
    save_p += sizeof(acs_count);

    for (i = 0; i < acs_count; i++)
    {
      acs_t *acs = Z_Malloc(sizeof(acs_t), PU_LEVEL, NULL);

      PADSAVEP();
      memcpy(acs, save_p, sizeof(*acs));
      save_p += sizeof(*acs);

      P_SetNewTarget(&acs->activator,
        mobj_p[P_GetMobj(acs->activator, size)]);
      acs->line = acs->line ?
        &lines[(uintptr_t) acs->line - 1] : NULL;
      acs->thinker.function.arg1 = (void (*)(void *)) T_InterpretACS;
      P_AddThinker(&acs->thinker);
    }
  }

  free(mobj_p);    // free translation table

  /* ZDoom Doom-in-Hexen: rebuild the thing-id table from the restored
   * mobjs so Teleport(tid) keeps working after a load (the Hexen game
   * does the same from G_DoLoadGame). */
  if (map_format.zdoom)
    P_CreateTIDList();

  // killough 3/26/98: Spawn icon landings:
  if (gamemode == commercial)
    P_SpawnBrainTargets();
}

//
// P_ArchiveSpecials
//
enum {
  tc_ceiling,
  tc_door,
  tc_floor,
  tc_plat,
  tc_flash,
  tc_strobe,
  tc_glow,
  tc_elevator,    //jff 2/22/98 new elevator type thinker
  tc_scroll,      // killough 3/7/98: new scroll effect thinker
  tc_pusher,      // phares 3/22/98:  new push/pull effect thinker
  tc_flicker,     // killough 10/4/98
  tc_endspecials
} specials_e;

//
// Things to handle:
//
// T_MoveCeiling, (ceiling_t: sector_t * swizzle), - active list
// T_VerticalDoor, (vldoor_t: sector_t * swizzle),
// T_MoveFloor, (floormove_t: sector_t * swizzle),
// T_LightFlash, (lightflash_t: sector_t * swizzle),
// T_StrobeFlash, (strobe_t: sector_t *),
// T_Glow, (glow_t: sector_t *),
// T_PlatRaise, (plat_t: sector_t *), - active list
// T_MoveElevator, (plat_t: sector_t *), - active list      // jff 2/22/98
// T_Scroll                                                 // killough 3/7/98
// T_Pusher                                                 // phares 3/22/98
// T_FireFlicker                                            // killough 10/4/98
//

void P_ArchiveSpecials (void)
{
  thinker_t *th;
  size_t    size = 0;          // killough

  // save off the current thinkers (memory size calculation -- killough)

  for (th = thinkercap.next ; th != &thinkercap ; th=th->next)
    if (!th->function.arg1)
      {
        platlist_t *pl;
        ceilinglist_t *cl;     //jff 2/22/98 need this for ceilings too now
        for (pl=activeplats; pl; pl=pl->next)
          if (pl->plat == (plat_t *) th)   // killough 2/14/98
            {
              size += 4+sizeof(plat_t);
              goto end;
            }
        for (cl=activeceilings; cl; cl=cl->next) // search for activeceiling
          if (cl->ceiling == (ceiling_t *) th)   //jff 2/22/98
            {
              size += 4+sizeof(ceiling_t);
              goto end;
            }
      end:;
      }
    else
      size +=
        th->function.arg1==(void (*)(void *))T_MoveCeiling  ? 4+sizeof(ceiling_t) :
        th->function.arg1==(void (*)(void *))T_VerticalDoor ? 4+sizeof(vldoor_t)  :
        th->function.arg1==(void (*)(void *))T_MoveFloor    ? 4+sizeof(floormove_t):
        th->function.arg1==(void (*)(void *))T_PlatRaise    ? 4+sizeof(plat_t)    :
        th->function.arg1==(void (*)(void *))T_LightFlash   ? 4+sizeof(lightflash_t):
        th->function.arg1==(void (*)(void *))T_StrobeFlash  ? 4+sizeof(strobe_t)  :
        th->function.arg1==(void (*)(void *))T_Glow         ? 4+sizeof(glow_t)    :
        th->function.arg1==(void (*)(void *))T_MoveElevator ? 4+sizeof(elevator_t):
        th->function.arg1==(void (*)(void *))T_Scroll       ? 4+sizeof(scroll_t)  :
        th->function.arg1==(void (*)(void *))T_Pusher       ? 4+sizeof(pusher_t)  :
        th->function.arg1==(void (*)(void *))T_FireFlicker? 4+sizeof(fireflicker_t) :
      0;

  CheckSaveGame(size + 1);    // killough; cph: +1 for the tc_endspecials

  // save off the current thinkers
  for (th=thinkercap.next; th!=&thinkercap; th=th->next)
    {
      if (!th->function.arg1)
        {
          platlist_t *pl;
          ceilinglist_t *cl;    //jff 2/22/98 add iter variable for ceilings

          // killough 2/8/98: fix plat original height bug.
          // Since acv==NULL, this could be a plat in stasis.
          // so check the active plats list, and save this
          // plat (jff: or ceiling) even if it is in stasis.

          for (pl=activeplats; pl; pl=pl->next)
            if (pl->plat == (plat_t *) th)      // killough 2/14/98
              goto plat;

          for (cl=activeceilings; cl; cl=cl->next)
            if (cl->ceiling == (ceiling_t *) th)      //jff 2/22/98
              goto ceiling;

          continue;
        }

      if (th->function.arg1 == (void (*)(void *))T_MoveCeiling)
        {
          ceiling_t *ceiling;
        ceiling:                               // killough 2/14/98
          *save_p++ = tc_ceiling;
          PADSAVEP();
          ceiling = (ceiling_t *)save_p;
          memcpy (ceiling, th, sizeof(*ceiling));
          save_p += sizeof(*ceiling);
          ceiling->sector = (sector_t *)(ceiling->sector - sectors);
          continue;
        }

      if (th->function.arg1 == (void (*)(void *))T_VerticalDoor)
        {
          vldoor_t *door;
          *save_p++ = tc_door;
          PADSAVEP();
          door = (vldoor_t *) save_p;
          memcpy (door, th, sizeof *door);
          save_p += sizeof(*door);
          door->sector = (sector_t *)(door->sector - sectors);
          //jff 1/31/98 archive line remembered by door as well
          door->line = (line_t *) (door->line ? door->line-lines : -1);
          continue;
        }

      if (th->function.arg1 == (void (*)(void *))T_MoveFloor)
        {
          floormove_t *floor;
          *save_p++ = tc_floor;
          PADSAVEP();
          floor = (floormove_t *)save_p;
          memcpy (floor, th, sizeof(*floor));
          save_p += sizeof(*floor);
          floor->sector = (sector_t *)(floor->sector - sectors);
          continue;
        }

      if (th->function.arg1 == (void (*)(void *))T_PlatRaise)
        {
          plat_t *plat;
        plat:   // killough 2/14/98: added fix for original plat height above
          *save_p++ = tc_plat;
          PADSAVEP();
          plat = (plat_t *)save_p;
          memcpy (plat, th, sizeof(*plat));
          save_p += sizeof(*plat);
          plat->sector = (sector_t *)(plat->sector - sectors);
          continue;
        }

      if (th->function.arg1 == (void (*)(void *))T_LightFlash)
        {
          lightflash_t *flash;
          *save_p++ = tc_flash;
          PADSAVEP();
          flash = (lightflash_t *)save_p;
          memcpy (flash, th, sizeof(*flash));
          save_p += sizeof(*flash);
          flash->sector = (sector_t *)(flash->sector - sectors);
          continue;
        }

      if (th->function.arg1 == (void (*)(void *))T_StrobeFlash)
        {
          strobe_t *strobe;
          *save_p++ = tc_strobe;
          PADSAVEP();
          strobe = (strobe_t *)save_p;
          memcpy (strobe, th, sizeof(*strobe));
          save_p += sizeof(*strobe);
          strobe->sector = (sector_t *)(strobe->sector - sectors);
          continue;
        }

      if (th->function.arg1 == (void (*)(void *))T_Glow)
        {
          glow_t *glow;
          *save_p++ = tc_glow;
          PADSAVEP();
          glow = (glow_t *)save_p;
          memcpy (glow, th, sizeof(*glow));
          save_p += sizeof(*glow);
          glow->sector = (sector_t *)(glow->sector - sectors);
          continue;
        }

      // killough 10/4/98: save flickers
      if (th->function.arg1 == (void (*)(void *))T_FireFlicker)
        {
          fireflicker_t *flicker;
          *save_p++ = tc_flicker;
          PADSAVEP();
          flicker = (fireflicker_t *)save_p;
          memcpy (flicker, th, sizeof(*flicker));
          save_p += sizeof(*flicker);
          flicker->sector = (sector_t *)(flicker->sector - sectors);
          continue;
        }

      //jff 2/22/98 new case for elevators
      if (th->function.arg1 == (void (*)(void *))T_MoveElevator)
        {
          elevator_t *elevator;         //jff 2/22/98
          *save_p++ = tc_elevator;
          PADSAVEP();
          elevator = (elevator_t *)save_p;
          memcpy (elevator, th, sizeof(*elevator));
          save_p += sizeof(*elevator);
          elevator->sector = (sector_t *)(elevator->sector - sectors);
          continue;
        }

      // killough 3/7/98: Scroll effect thinkers
      if (th->function.arg1 == (void (*)(void *))T_Scroll)
        {
          *save_p++ = tc_scroll;
          memcpy (save_p, th, sizeof(scroll_t));
          save_p += sizeof(scroll_t);
          continue;
        }

      // phares 3/22/98: Push/Pull effect thinkers

      if (th->function.arg1 == (void (*)(void *))T_Pusher)
        {
          *save_p++ = tc_pusher;
          memcpy (save_p, th, sizeof(pusher_t));
          save_p += sizeof(pusher_t);
          continue;
        }
    }

  // add a terminating marker
  *save_p++ = tc_endspecials;
}


//
// P_UnArchiveSpecials
//
void P_UnArchiveSpecials (void)
{
  uint8_t tclass;

  // read in saved thinkers
  while ((tclass = *save_p++) != tc_endspecials)  // killough 2/14/98
    switch (tclass)
      {
      case tc_ceiling:
        PADSAVEP();
        {
          ceiling_t *ceiling = Z_Malloc (sizeof(*ceiling), PU_LEVEL, NULL);
          memcpy (ceiling, save_p, sizeof(*ceiling));
          save_p += sizeof(*ceiling);
          ceiling->sector = &sectors[(uintptr_t)ceiling->sector];
          ceiling->sector->ceilingdata = ceiling; //jff 2/22/98

          if (ceiling->thinker.function.arg1)
            ceiling->thinker.function.arg1 = (void (*)(void *))T_MoveCeiling;

          P_AddThinker (&ceiling->thinker);
          P_AddActiveCeiling(ceiling);
          break;
        }

      case tc_door:
        PADSAVEP();
        {
          vldoor_t *door = Z_Malloc (sizeof(*door), PU_LEVEL, NULL);
          memcpy (door, save_p, sizeof(*door));
          save_p += sizeof(*door);
          door->sector = &sectors[(uintptr_t)door->sector];

          //jff 1/31/98 unarchive line remembered by door as well
          door->line = (intptr_t)door->line!=-1? &lines[(uintptr_t)door->line] : NULL;

          door->sector->ceilingdata = door;       //jff 2/22/98
          door->thinker.function.arg1 = (void (*)(void *))T_VerticalDoor;
          P_AddThinker (&door->thinker);
          break;
        }

      case tc_floor:
        PADSAVEP();
        {
          floormove_t *floor = Z_Malloc (sizeof(*floor), PU_LEVEL, NULL);
          memcpy (floor, save_p, sizeof(*floor));
          save_p += sizeof(*floor);
          floor->sector = &sectors[(uintptr_t)floor->sector];
          floor->sector->floordata = floor; //jff 2/22/98
          floor->thinker.function.arg1 = (void (*)(void *))T_MoveFloor;
          P_AddThinker (&floor->thinker);
          break;
        }

      case tc_plat:
        PADSAVEP();
        {
          plat_t *plat = Z_Malloc (sizeof(*plat), PU_LEVEL, NULL);
          memcpy (plat, save_p, sizeof(*plat));
          save_p += sizeof(*plat);
          plat->sector = &sectors[(uintptr_t)plat->sector];
          plat->sector->floordata = plat; //jff 2/22/98

          if (plat->thinker.function.arg1)
            plat->thinker.function.arg1 = (void (*)(void *))T_PlatRaise;

          P_AddThinker (&plat->thinker);
          P_AddActivePlat(plat);
          break;
        }

      case tc_flash:
        PADSAVEP();
        {
          lightflash_t *flash = Z_Malloc (sizeof(*flash), PU_LEVEL, NULL);
          memcpy (flash, save_p, sizeof(*flash));
          save_p += sizeof(*flash);
          flash->sector = &sectors[(uintptr_t)flash->sector];
          flash->thinker.function.arg1 = (void (*)(void *))T_LightFlash;
          P_AddThinker (&flash->thinker);
          break;
        }

      case tc_strobe:
        PADSAVEP();
        {
          strobe_t *strobe = Z_Malloc (sizeof(*strobe), PU_LEVEL, NULL);
          memcpy (strobe, save_p, sizeof(*strobe));
          save_p += sizeof(*strobe);
          strobe->sector = &sectors[(uintptr_t)strobe->sector];
          strobe->thinker.function.arg1 = (void (*)(void *))T_StrobeFlash;
          P_AddThinker (&strobe->thinker);
          break;
        }

      case tc_glow:
        PADSAVEP();
        {
          glow_t *glow = Z_Malloc (sizeof(*glow), PU_LEVEL, NULL);
          memcpy (glow, save_p, sizeof(*glow));
          save_p += sizeof(*glow);
          glow->sector = &sectors[(uintptr_t)glow->sector];
          glow->thinker.function.arg1 = (void (*)(void *))T_Glow;
          P_AddThinker (&glow->thinker);
          break;
        }

      case tc_flicker:           // killough 10/4/98
        PADSAVEP();
        {
          fireflicker_t *flicker = Z_Malloc (sizeof(*flicker), PU_LEVEL, NULL);
          memcpy (flicker, save_p, sizeof(*flicker));
          save_p += sizeof(*flicker);
          flicker->sector = &sectors[(uintptr_t)flicker->sector];
          flicker->thinker.function.arg1 = (void (*)(void *))T_FireFlicker;
          P_AddThinker (&flicker->thinker);
          break;
        }

        //jff 2/22/98 new case for elevators
      case tc_elevator:
        PADSAVEP();
        {
          elevator_t *elevator = Z_Malloc (sizeof(*elevator), PU_LEVEL, NULL);
          memcpy (elevator, save_p, sizeof(*elevator));
          save_p += sizeof(*elevator);
          elevator->sector = &sectors[(uintptr_t)elevator->sector];
          elevator->sector->floordata = elevator; //jff 2/22/98
          elevator->sector->ceilingdata = elevator; //jff 2/22/98
          elevator->thinker.function.arg1 = (void (*)(void *))T_MoveElevator;
          P_AddThinker (&elevator->thinker);
          break;
        }

      case tc_scroll:       // killough 3/7/98: scroll effect thinkers
        {
          scroll_t *scroll = Z_Malloc (sizeof(scroll_t), PU_LEVEL, NULL);
          memcpy (scroll, save_p, sizeof(scroll_t));
          save_p += sizeof(scroll_t);
          scroll->thinker.function.arg1 = (void (*)(void *))T_Scroll;
          P_AddThinker(&scroll->thinker);
          break;
        }

      case tc_pusher:   // phares 3/22/98: new Push/Pull effect thinkers
        {
          pusher_t *pusher = Z_Malloc (sizeof(pusher_t), PU_LEVEL, NULL);
          memcpy (pusher, save_p, sizeof(pusher_t));
          save_p += sizeof(pusher_t);
          pusher->thinker.function.arg1 = (void (*)(void *))T_Pusher;
          pusher->source = P_GetPushThing(pusher->affectee);
          P_AddThinker(&pusher->thinker);
          break;
        }

      default:
        I_Error("P_UnarchiveSpecials: Unknown tclass %i in savegame", tclass);
      }
}

// killough 2/16/98: save/restore random number generator state information

void P_ArchiveRNG(void)
{
  CheckSaveGame(sizeof rng);
  memcpy(save_p, &rng, sizeof rng);
  save_p += sizeof rng;
}

void P_UnArchiveRNG(void)
{
  memcpy(&rng, save_p, sizeof rng);
  save_p += sizeof rng;
}

// killough 2/22/98: Save/restore automap state
// killough 2/22/98: Save/restore automap state
void P_ArchiveMap(void)
{
  int zero = 0, one = 1;
  CheckSaveGame(2 * sizeof zero + sizeof markpointnum +
                markpointnum * sizeof *markpoints +
                sizeof automapmode + sizeof one);

  memcpy(save_p, &automapmode, sizeof automapmode);
  save_p += sizeof automapmode;
  memcpy(save_p, &one, sizeof one);   // CPhipps - used to be viewactive, now
  save_p += sizeof one;               // that's worked out locally by D_Display
  memcpy(save_p, &zero, sizeof zero); // CPhipps - used to be followplayer
  save_p += sizeof zero;              //  that is now part of automapmode
  memcpy(save_p, &zero, sizeof zero); // CPhipps - used to be automap_grid, ditto
  save_p += sizeof zero;
  memcpy(save_p, &markpointnum, sizeof markpointnum);
  save_p += sizeof markpointnum;

  if (markpointnum)
    {
      memcpy(save_p, markpoints, sizeof *markpoints * markpointnum);
      save_p += markpointnum * sizeof *markpoints;
    }
}

void P_UnArchiveMap(void)
{
  int unused;
  memcpy(&automapmode, save_p, sizeof automapmode);
  save_p += sizeof automapmode;
  memcpy(&unused, save_p, sizeof unused);
  save_p += sizeof unused;
  memcpy(&unused, save_p, sizeof unused);
  save_p += sizeof unused;
  memcpy(&unused, save_p, sizeof unused);
  save_p += sizeof unused;

  if (automapmode & am_active)
    AM_Start();

  memcpy(&markpointnum, save_p, sizeof markpointnum);
  save_p += sizeof markpointnum;

  if (markpointnum)
    {
      while (markpointnum >= markpointnum_max)
        markpoints = realloc(markpoints, sizeof *markpoints *
         (markpointnum_max = markpointnum_max ? markpointnum_max*2 : 16));
      memcpy(markpoints, save_p, markpointnum * sizeof *markpoints);
      save_p += markpointnum * sizeof *markpoints;
    }
}



/* ======================================================================
 * Hexen world state: ACS variables and the deferred-script store, the
 * per-map script table, polyobjects, and active sound sequences.  All of
 * this matches the vanilla save layout in spirit; every block is gated on
 * hexen so doom and heretic savegames are byte-identical to before.
 * ====================================================================== */

static void P_SaveInt(int v)
{
  memcpy(save_p, &v, sizeof(v));
  save_p += sizeof(v);
}

static int P_LoadInt(void)
{
  int v;

  memcpy(&v, save_p, sizeof(v));
  save_p += sizeof(v);
  return v;
}

/* world variables + scripts deferred for other hub maps */

void P_ArchiveACS(void)
{
  size_t size;

  if (!hexen)
    return;

  size = sizeof(WorldVars) + sizeof(ACSStore);
  CheckSaveGame(size);

  memcpy(save_p, WorldVars, sizeof(WorldVars));
  save_p += sizeof(WorldVars);
  memcpy(save_p, ACSStore, sizeof(ACSStore));
  save_p += sizeof(ACSStore);
}

void P_UnArchiveACS(void)
{
  if (!hexen)
    return;

  memcpy(WorldVars, save_p, sizeof(WorldVars));
  save_p += sizeof(WorldVars);
  memcpy(ACSStore, save_p, sizeof(ACSStore));
  save_p += sizeof(ACSStore);
}

/* per-script state (suspended/terminating, wait values) + map variables */

void P_ArchiveScripts(void)
{
  size_t size;

  if (!hexen)
    return;

  size = sizeof(*ACSInfo) * ACScriptCount + sizeof(MapVars);
  CheckSaveGame(size);

  memcpy(save_p, ACSInfo, sizeof(*ACSInfo) * ACScriptCount);
  save_p += sizeof(*ACSInfo) * ACScriptCount;
  memcpy(save_p, MapVars, sizeof(MapVars));
  save_p += sizeof(MapVars);
}

void P_UnArchiveScripts(void)
{
  if (!hexen)
    return;

  memcpy(ACSInfo, save_p, sizeof(*ACSInfo) * ACScriptCount);
  save_p += sizeof(*ACSInfo) * ACScriptCount;
  memcpy(MapVars, save_p, sizeof(MapVars));
  save_p += sizeof(MapVars);
}

/* polyobjects: per-seg geometry plus the rotation/translation state */

static void P_ArchiveVertex(vertex_t *v)
{
  P_SaveInt(v->x);
  P_SaveInt(v->y);
}

static void P_UnArchiveVertex(vertex_t *v)
{
  v->x = P_LoadInt();
  v->y = P_LoadInt();
}

void P_ArchivePolyobjs(void)
{
  int i;

  if (!hexen)
    return;

  for (i = 0; i < po_NumPolyobjs; i++)
  {
    int seg_i;
    polyobj_t *po;

    po = &polyobjs[i];

    CheckSaveGame(po->numsegs * 13 * sizeof(int) + 3 * sizeof(int));

    for (seg_i = 0; seg_i < po->numsegs; ++seg_i)
    {
      seg_t *seg;
      line_t *line;

      seg = po->segs[seg_i];
      line = seg->linedef;

      P_ArchiveVertex(seg->v1);
      P_ArchiveVertex(seg->v2);

      P_SaveInt(seg->angle);
      P_SaveInt(line->slopetype);
      P_SaveInt(line->bbox[0]);
      P_SaveInt(line->bbox[1]);
      P_SaveInt(line->bbox[2]);
      P_SaveInt(line->bbox[3]);
      P_SaveInt(line->dx);
      P_SaveInt(line->dy);

      P_ArchiveVertex(&po->originalPts[seg_i]);
      P_ArchiveVertex(&po->prevPts[seg_i]);
    }

    P_SaveInt(po->angle);
    P_SaveInt(po->startSpot.x);
    P_SaveInt(po->startSpot.y);
  }
}

void P_UnArchivePolyobjs(void)
{
  void UnLinkPolyobj(polyobj_t *po);
  void LinkPolyobj(polyobj_t *po);
  void ResetPolySubSector(polyobj_t *po);

  int i;

  if (!hexen)
    return;

  for (i = 0; i < po_NumPolyobjs; i++)
  {
    int seg_i;
    polyobj_t *po;

    po = &polyobjs[i];

    UnLinkPolyobj(po);

    for (seg_i = 0; seg_i < po->numsegs; ++seg_i)
    {
      seg_t *seg;
      line_t *line;

      seg = po->segs[seg_i];
      line = seg->linedef;

      P_UnArchiveVertex(seg->v1);
      P_UnArchiveVertex(seg->v2);

      seg->angle = P_LoadInt();
      line->slopetype = P_LoadInt();
      line->bbox[0] = P_LoadInt();
      line->bbox[1] = P_LoadInt();
      line->bbox[2] = P_LoadInt();
      line->bbox[3] = P_LoadInt();
      line->dx = P_LoadInt();
      line->dy = P_LoadInt();

      P_UnArchiveVertex(&po->originalPts[seg_i]);
      P_UnArchiveVertex(&po->prevPts[seg_i]);
    }

    po->angle = P_LoadInt();
    po->startSpot.x = P_LoadInt();
    po->startSpot.y = P_LoadInt();

    LinkPolyobj(po);
    ResetPolySubSector(po);
  }
}

/* active sound sequences: which script, where in it, and what it's
 * attached to (a sector's sound origin or a polyobj's start spot) */

void P_ArchiveSounds(void)
{
  seqnode_t *node;
  sector_t *sec;
  int difference;
  int i;

  if (!hexen)
    return;

  CheckSaveGame(sizeof(int) + ActiveSequences * (6 * sizeof(int) + 1));
  P_SaveInt(ActiveSequences);

  for (node = SequenceListHead; node; node = node->next)
  {
    P_SaveInt(node->sequence);
    P_SaveInt(node->delayTics);
    P_SaveInt(node->volume);

    difference = SN_GetSequenceOffset(node->sequence, node->sequencePtr);
    P_SaveInt(difference);
    P_SaveInt(node->currentSoundID);

    for (i = 0; i < po_NumPolyobjs; i++)
    {
      if (node->mobj == (mobj_t *) &polyobjs[i].startSpot)
        break;
    }

    if (i == po_NumPolyobjs)
    {                  /* sound is attached to a sector, not a polyobj */
      sec = R_PointInSubsector(node->mobj->x, node->mobj->y)->sector;
      difference = (int) (sec - sectors);
      *save_p++ = 0;   /* 0 -- sector sound origin */
    }
    else
    {
      difference = i;
      *save_p++ = 1;   /* 1 -- polyobj sound origin */
    }

    P_SaveInt(difference);
  }
}

void P_UnArchiveSounds(void)
{
  int i;
  int numSequences;
  int sequence;
  int delayTics;
  int volume;
  int seqOffset;
  int soundID;
  byte polySnd;
  int secNum;
  int seq_count;
  mobj_t *sndMobj;

  if (!hexen)
    return;

  SN_StopAllSequences();

  numSequences = P_LoadInt();

  i = 0;
  while (i < numSequences)
  {
    sequence = P_LoadInt();
    delayTics = P_LoadInt();
    volume = P_LoadInt();
    seqOffset = P_LoadInt();
    soundID = P_LoadInt();
    polySnd = *save_p++;
    secNum = P_LoadInt();

    if (!polySnd)
      sndMobj = (mobj_t *) &sectors[secNum].soundorg;
    else
      sndMobj = (mobj_t *) &polyobjs[secNum].startSpot;

    /* SN_StartSequence prepends, so the just-started node is index 0;
     * indexing by i here would apply each record's saved position to an
     * earlier sequence's node, walking shorter scripts out of bounds. */
    seq_count = ActiveSequences;
    SN_StartSequence(sndMobj, sequence);
    if (ActiveSequences > seq_count)
      SN_ChangeNodeData(0, seqOffset, delayTics, volume, soundID);
    i++;
  }
}
