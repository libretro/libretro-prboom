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

#include <stdint.h>

#ifdef HAVE_THREADS

#include <rthreads/rthreads.h>
#include <retro_atomic.h>

/* tpool was the first implementation and it did not survive measurement.
 * Per dispatch it heap-allocated a record per work item and funnelled every
 * submission and every worker acquisition through one shared mutex --
 * roughly N allocations and 45 contended lock acquisitions per frame for
 * about a millisecond of work.  On a dual-CCD part that mutex line
 * ping-pongs between chiplets, and the cost grew with thread count: on a
 * 9950X3D at 1920x1200 the replay peaked at 4 threads (1.16x) and was a net
 * *loss* at 16 (0.77x), implying on the order of 90us of dispatch overhead
 * per thread per frame.
 *
 * Slots are fixed instead: worker i always runs item i, so there is no
 * queue to guard, nothing to allocate, and no lock to take in order to find
 * work.  A dispatch is one broadcast; a completion is one atomic decrement.
 * The join spins briefly before parking, because at these frame times the
 * workers are still running when the caller arrives and a condition
 * variable round trip costs more than the wait itself. */

/* Eight, not sixteen.  Measured on a 16-core 9950X3D at 1920x1200: four
 * threads gave 1.97x and eight 1.92x, while sixteen came in at 0.74x with
 * *both* threaded stages slower than single-threaded.  Beyond eight there is
 * nothing to gain on any scene tested and a large amount to lose, so the
 * ceiling is set where the measurements stop improving. */
#define WALLMT_MAX  8
/* Bounded spin on the completion count before falling back to the condvar.
 * The previous budget of 20000 flat iterations was self-defeating: at ~35
 * cycles per pause that is about 150us of spinning at 4.7GHz -- comparable
 * to the whole parallel saving -- and every iteration re-read the same line
 * the workers were issuing fetch_sub on, so the caller was actively
 * invalidating the counter it was waiting for.  Slices are balanced to
 * within a couple of percent, so the real wait is short; a small budget with
 * a doubling backoff catches it while touching the line far less often. */
#define WALLMT_SPIN      1024
#define WALLMT_BACKOFF_MAX 64

#if defined(__i386__) || defined(__x86_64__)
#define WALLMT_RELAX() __builtin_ia32_pause()
#elif defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
#include <intrin.h>
#define WALLMT_RELAX() _mm_pause()
#elif defined(__aarch64__) || defined(__arm__)
#define WALLMT_RELAX() __asm__ __volatile__("yield" ::: "memory")
#else
#define WALLMT_RELAX() ((void)0)
#endif

static sthread_t         *mt_thread[WALLMT_MAX];
static slock_t           *mt_lock;
static scond_t           *mt_go;
static scond_t           *mt_done;
static slock_t           *wall_tint_lock;

static int                mt_nthreads;
static int                mt_nactive;
static int                mt_quit;
static unsigned           mt_generation;
static wallmt_fn          mt_fn;
static char              *mt_base;
static size_t             mt_elem;
static retro_atomic_int_t mt_pending;

static void wallmt_worker(void *arg)
{
   int      me   = (int)(intptr_t)arg;
   unsigned seen = 0;

   for (;;)
   {
      slock_lock(mt_lock);
      while (!mt_quit && mt_generation == seen)
         scond_wait(mt_go, mt_lock);
      if (mt_quit)
      {
         slock_unlock(mt_lock);
         return;
      }
      seen = mt_generation;
      slock_unlock(mt_lock);

      if (me < mt_nactive)
         mt_fn(mt_base + (size_t)me * mt_elem);

      /* Signal under the lock so a caller that has already checked the
       * count and is about to wait cannot miss the wakeup. */
      if (retro_atomic_fetch_sub_int(&mt_pending, 1) == 1)
      {
         slock_lock(mt_lock);
         scond_signal(mt_done);
         slock_unlock(mt_lock);
      }
   }
}

static void wallmt_teardown(void)
{
   int i;

   if (mt_nthreads > 0)
   {
      slock_lock(mt_lock);
      mt_quit = 1;
      mt_generation++;
      scond_broadcast(mt_go);
      slock_unlock(mt_lock);

      for (i = 0; i < mt_nthreads; i++)
         if (mt_thread[i])
         {
            sthread_join(mt_thread[i]);
            mt_thread[i] = NULL;
         }
      mt_nthreads = 0;
   }
   mt_quit = 0;
}

int R_WallMTEnsure(int workers)
{
   int i;

   if (workers < 1)
      return 0;
   if (workers > WALLMT_MAX)
      workers = WALLMT_MAX;
   if (mt_nthreads == workers)
      return 1;

   wallmt_teardown();

   if (!mt_lock && !(mt_lock = slock_new()))
      return 0;
   if (!mt_go   && !(mt_go   = scond_new()))
      return 0;
   if (!mt_done && !(mt_done = scond_new()))
      return 0;

   retro_atomic_store_release_int(&mt_pending, 0);
   mt_generation = 0;

   for (i = 0; i < workers; i++)
   {
      mt_thread[i] = sthread_create(wallmt_worker, (void *)(intptr_t)i);
      if (!mt_thread[i])
      {
         mt_nthreads = i;
         wallmt_teardown();
         return 0;
      }
   }
   mt_nthreads = workers;
   return 1;
}

void R_WallMTRun(wallmt_fn fn, void *base, size_t elemsize, int n)
{
   if (!fn || n < 1 || n > mt_nthreads)
      return;

   slock_lock(mt_lock);
   mt_fn      = fn;
   mt_base    = (char *)base;
   mt_elem    = elemsize;
   mt_nactive = n;
   /* Every worker wakes and decrements; those at or above n simply have no
    * item to run.  Counting all of them keeps the join a single compare. */
   retro_atomic_store_release_int(&mt_pending, mt_nthreads);
   mt_generation++;
   scond_broadcast(mt_go);
   slock_unlock(mt_lock);
}

void R_WallMTWait(void)
{
   int spins   = WALLMT_SPIN;
   int backoff = 1;

   if (mt_nthreads < 1)
      return;

   while (spins > 0)
   {
      int k;
      if (retro_atomic_load_acquire_int(&mt_pending) == 0)
         return;
      for (k = 0; k < backoff; k++)
         WALLMT_RELAX();
      spins -= backoff;
      if (backoff < WALLMT_BACKOFF_MAX)
         backoff <<= 1;
   }

   slock_lock(mt_lock);
   while (retro_atomic_load_acquire_int(&mt_pending) != 0)
      scond_wait(mt_done, mt_lock);
   slock_unlock(mt_lock);
}

void R_WallMTShutdown(void)
{
   wallmt_teardown();
   if (mt_done) { scond_free(mt_done); mt_done = NULL; }
   if (mt_go)   { scond_free(mt_go);   mt_go   = NULL; }
   if (mt_lock) { slock_free(mt_lock); mt_lock = NULL; }
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

int  R_WallMTEnsure(int workers) { (void)workers; return 0; }
void R_WallMTRun(wallmt_fn fn, void *base, size_t elemsize, int n)
                                 { (void)fn; (void)base; (void)elemsize; (void)n; }
void R_WallMTWait(void)          { }
void R_WallMTShutdown(void)                 { }
void R_WallMTTintLockInit(void)             { }
void R_WallMTTintLock(void)                 { }
void R_WallMTTintUnlock(void)               { }

#endif
