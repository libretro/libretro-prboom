/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2001 by
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
 *      Handles in-memory caching of WAD lumps
 *
 *-----------------------------------------------------------------------------
 */

// use config.h if autoconf made one -- josh
#include "config.h"

#include "doomstat.h"
#include "doomtype.h"

#include "w_wad.h"
#include "z_zone.h"
#include "lprintf.h"

static struct {
  void *cache;
  unsigned int locks;
} *cachelump;

/* W_InitCache
 *
 * cph 2001/07/07 - split from W_Init
 */
void W_InitCache(void)
{
  // set up caching
  cachelump = calloc(sizeof *cachelump, numlumps);
  if (!cachelump)
    I_Error ("W_Init: Couldn't allocate lumpcache");
}

void W_DoneCache(void)
{
   /* Explicitly free every cached lump block before releasing the
    * cachelump array itself.  Two reasons:
    *
    *  - The Z_Malloc below uses a back-pointer to &cachelump[lump]
    *    .cache (so PU_CACHE auto-purges NULL the slot correctly --
    *    see comment on W_CacheLumpNum).  At Z_Close time, PU_STATIC
    *    blocks are freed before PU_CACHE.  cachelump itself is
    *    PU_STATIC; if we let Z_Close handle it, cachelump would be
    *    freed first, then each PU_CACHE cache block's Z_Free would
    *    write *(&cachelump[lump].cache) = NULL through a now-
    *    dangling back-pointer.  Heap corruption.  By explicitly
    *    freeing cache blocks here while cachelump is still live,
    *    Z_Close has no cachelump-pointing blocks left to encounter.
    *
    *  - Locked cache blocks are PU_STATIC and never auto-purge.
    *    Without this loop, they'd survive until Z_Close at
    *    retro_deinit and only get reclaimed at process end.  This
    *    is the same per-session-leak shape we fixed elsewhere. */
   if (cachelump)
   {
      int i;
      for (i = 0; i < numlumps; i++)
         if (cachelump[i].cache)
            free(cachelump[i].cache);
      free(cachelump);
      cachelump = NULL;
   }
}

/* W_InvalidateLumpCache
 * Drop the cached copy of a lump so the next W_CacheLumpNum re-reads
 * it from its (possibly replaced) backing store.  W_ReplaceLumpData
 * calls this: without it, a lump cached before replacement keeps
 * serving the old bytes forever.  Invalidating a locked lump would
 * yank memory out from under a live caller, so that is a bug. */
void W_InvalidateLumpCache(int lump)
{
  if (!cachelump || lump < 0 || lump >= numlumps)
    return;
  if (!cachelump[lump].cache)
    return;
  if (cachelump[lump].locks > 0)
    I_Error("W_InvalidateLumpCache: lump %.8s is locked",
            lumpinfo[lump].name);
  Z_Free(cachelump[lump].cache);
  cachelump[lump].cache = NULL;
}

/* W_CacheLumpNum
 * killough 4/25/98: simplified
 * CPhipps - modified for new lump locking scheme
 *           returns a const*
 */

const void *W_CacheLumpNum(int lump)
{
  const int locks = 1;

  /* Pass &cachelump[lump].cache as the user back-pointer so that
   * when this PU_CACHE block is auto-purged (Z_Malloc's cache-
   * purge under MEMORY_LOW pressure, or an explicit Z_FreeTags
   * (PU_CACHE)), the cachelump[lump].cache slot is NULLed.
   * Otherwise the next W_CacheLumpNum sees a non-NULL but freed
   * pointer, skips the re-read branch, and returns it -- caller
   * dereferences freed memory.  Same bug shape as r_patch.c
   * patch->data; fires routinely under MEMORY_LOW.
   *
   * The deinit-time concern (cachelump is PU_STATIC, freed before
   * PU_CACHE blocks at Z_Close, so a back-pointer would dangle)
   * is handled by W_DoneCache: it explicitly frees every
   * cachelump[i].cache BEFORE freeing cachelump itself.  By the
   * time Z_Close runs, no blocks back-pointing into cachelump
   * remain. */
  if (!cachelump[lump].cache)      // read the lump in
  {
    cachelump[lump].cache = Z_Malloc(W_LumpLength(lump), PU_CACHE,
                                     &cachelump[lump].cache);
    W_ReadLump(lump, cachelump[lump].cache);
  }

  /* cph - if wasn't locked but now is, tell z_zone to hold it */
  if (!cachelump[lump].locks && locks) {
    Z_ChangeTag(cachelump[lump].cache,PU_STATIC);
  }
  cachelump[lump].locks += locks;

  return cachelump[lump].cache;
}

const void *W_LockLumpNum(int lump)
{
  return W_CacheLumpNum(lump);
}

/*
 * W_UnlockLumpNum
 *
 * CPhipps - this changes (should reduce) the number of locks on a lump
 */

void W_UnlockLumpNum(int lump)
{
  const int unlocks = 1;

  // invalid lump, ignore unlock
  if (lump < 0) return;

  cachelump[lump].locks -= unlocks;
  /* cph - Note: must only tell z_zone to make purgeable if currently locked,
   * else it might already have been purged
   */
  if (unlocks && !cachelump[lump].locks)
    Z_ChangeTag(cachelump[lump].cache, PU_CACHE);
}

