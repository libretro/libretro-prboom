/* Emacs style mode select   -*- C -*-
 *-----------------------------------------------------------------------------
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
 *      Worker pool for the threaded wall replay.  The single translation
 *      unit permitted to include libretro-common's threading headers; see
 *      r_wallmt.h for why that matters.
 *
 *      Built with Z_ZONE_NO_ALLOC_OVERRIDE (set in Makefile.common), so the
 *      zone's malloc/free macros are inactive here.  That is required twice
 *      over: windows.h cannot be parsed with those macros in force, and the
 *      pool's own allocations happen on, or are freed by, worker threads --
 *      the zone has one global block list and no locking.
 *
 *-----------------------------------------------------------------------------
 */

#include "r_wallmt.h"

#ifdef HAVE_THREADS

#include <rthreads/rthreads.h>
#include <rthreads/tpool.h>

static tpool_t *wall_pool      = NULL;
static int      wall_pool_size = 0;
static slock_t *wall_tint_lock = NULL;

int R_WallMTEnsure(int workers)
{
   if (workers < 1)
      return 0;
   if (wall_pool && wall_pool_size == workers)
      return 1;
   if (wall_pool)
   {
      tpool_destroy(wall_pool);
      wall_pool      = NULL;
      wall_pool_size = 0;
   }
   if (!(wall_pool = tpool_create((size_t)workers)))
      return 0;
   wall_pool_size = workers;
   return 1;
}

void R_WallMTSubmit(wallmt_fn fn, void *arg)
{
   if (wall_pool)
      tpool_add_work(wall_pool, fn, arg);
}

void R_WallMTWait(void)
{
   if (wall_pool)
      tpool_wait(wall_pool);
}

void R_WallMTShutdown(void)
{
   if (wall_pool)
   {
      tpool_destroy(wall_pool);
      wall_pool      = NULL;
      wall_pool_size = 0;
   }
}

void R_WallMTTintLockInit(void)
{
   if (!wall_tint_lock)
      wall_tint_lock = slock_new();
}

void R_WallMTTintLock(void)
{
   if (wall_tint_lock)
      slock_lock(wall_tint_lock);
}

void R_WallMTTintUnlock(void)
{
   if (wall_tint_lock)
      slock_unlock(wall_tint_lock);
}

#else /* !HAVE_THREADS */

int  R_WallMTEnsure(int workers)            { (void)workers; return 0; }
void R_WallMTSubmit(wallmt_fn fn, void *arg){ (void)fn; (void)arg; }
void R_WallMTWait(void)                     { }
void R_WallMTShutdown(void)                 { }
void R_WallMTTintLockInit(void)             { }
void R_WallMTTintLock(void)                 { }
void R_WallMTTintUnlock(void)               { }

#endif
