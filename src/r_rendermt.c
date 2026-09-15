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
 *      Worker pool shared by the threaded renderer passes.  The single
 *      translation unit permitted to include libretro-common's threading
 *      headers; see r_rendermt.h for why that matters.
 *
 *      Allocations here use the C library, never the zone: the pool's
 *      blocks are allocated on, or freed by, worker threads, and the zone
 *      has one global block list and no locking.
 *
 *-----------------------------------------------------------------------------
 */

#include "r_rendermt.h"

#include <stdint.h>

#ifdef HAVE_THREADS

#include <rthreads/rthreads.h>
#include <rthreads/retro_eventcount.h>
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
 * work.  A dispatch publishes the generation with a release store and
 * notifies an eventcount; a completion is one atomic decrement, and the
 * decrement that lands on zero notifies a second one.  Neither side holds a
 * lock: a worker wakes straight onto the generation word rather than into a
 * mutex the other workers are queued on, and a notify with nobody parked
 * costs one read-modify-write and one load.  The join spins briefly before
 * parking, because at these frame times the workers are still running when
 * the caller arrives and a trip through the kernel costs more than the wait
 * itself; the spin is confined to builds whose atomics are lock-free, since
 * elsewhere spinning only keeps the thread being waited on from running. */

/* Eight, not sixteen.  Measured on a 16-core 9950X3D at 1920x1200: four
 * threads gave 1.97x and eight 1.92x, while sixteen came in at 0.74x with
 * *both* threaded stages slower than single-threaded.  Beyond eight there is
 * nothing to gain on any scene tested and a large amount to lose, so the
 * ceiling is set where the measurements stop improving. */
#define RENDERMT_MAX  8
/* Bounded spin on the completion count before falling back to the condvar.
 * The previous budget of 20000 flat iterations was self-defeating: at ~35
 * cycles per pause that is about 150us of spinning at 4.7GHz -- comparable
 * to the whole parallel saving -- and every iteration re-read the same line
 * the workers were issuing fetch_sub on, so the caller was actively
 * invalidating the counter it was waiting for.  Slices are balanced to
 * within a couple of percent, so the real wait is short; a small budget with
 * a doubling backoff catches it while touching the line far less often. */
#define RENDERMT_SPIN      1024
#define RENDERMT_BACKOFF_MAX 64

#if defined(__i386__) || defined(__x86_64__)
#define RENDERMT_RELAX() __builtin_ia32_pause()
#elif defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
#include <intrin.h>
#define RENDERMT_RELAX() _mm_pause()
#elif defined(__aarch64__)
#define RENDERMT_RELAX() __asm__ __volatile__("yield" ::: "memory")
/* YIELD is an ARMv6K addition.  Older 32-bit cores -- armv5te on Miyoo, and
 * anything else built for arm926ej-s or below -- reject the mnemonic at
 * assembly time, so the hint has to be gated rather than emitted for every
 * __arm__ target.  GCC and clang both define __ARM_ARCH; __ARM_ARCH_6*K* is
 * checked as well because plain armv6 (no K) is 6 but has no YIELD. */
#elif defined(__arm__) && (defined(__ARM_ARCH_6K__)  || \
                           defined(__ARM_ARCH_6KZ__) || \
                           defined(__ARM_ARCH_6ZK__) || \
                           (defined(__ARM_ARCH) && __ARM_ARCH >= 7))
#define RENDERMT_RELAX() __asm__ __volatile__("yield" ::: "memory")
#else
#define RENDERMT_RELAX() ((void)0)
#endif

static sthread_t         *mt_thread[RENDERMT_MAX];
static retro_eventcount_t mt_go;
static retro_eventcount_t mt_done;
static int                mt_ec_ready;
static slock_t           *wall_tint_lock;

static int                mt_nthreads;
static int                mt_nactive;
static rendermt_fn          mt_fn;
static char              *mt_base;
static size_t             mt_elem;
/* Written by the dispatching thread only; the release store of
 * mt_generation is what publishes it and the four fields above to the
 * workers, so they are read only after an acquire load of that word. */
static int                mt_gen;
static retro_atomic_int_t mt_generation;
static retro_atomic_int_t mt_quit;
static retro_atomic_int_t mt_pending;

static void rendermt_worker(void *arg)
{
   int me   = (int)(intptr_t)arg;
   int seen = 0;
   int gen  = 0;

   sthread_setname("prboom-render");

   for (;;)
   {
      for (;;)
      {
         int key;

         gen = retro_atomic_load_acquire_int(&mt_generation);
         if (gen != seen || retro_atomic_load_acquire_int(&mt_quit))
            break;

         /* Re-read inside the wait window: a dispatch that lands from
          * here on either shows up in this second read or makes the
          * commit return without sleeping. */
         key = retro_eventcount_prepare_wait(&mt_go);
         gen = retro_atomic_load_acquire_int(&mt_generation);
         if (gen != seen || retro_atomic_load_acquire_int(&mt_quit))
         {
            retro_eventcount_cancel_wait(&mt_go);
            break;
         }
         retro_eventcount_commit_wait(&mt_go, key);
      }

      if (retro_atomic_load_acquire_int(&mt_quit))
         return;
      seen = gen;

      if (me < mt_nactive)
         mt_fn(mt_base + (size_t)me * mt_elem);

      if (retro_atomic_fetch_sub_int(&mt_pending, 1) == 1)
         retro_eventcount_notify(&mt_done);
   }
}

static void rendermt_teardown(void)
{
   int i;

   if (mt_nthreads > 0)
   {
      retro_atomic_store_release_int(&mt_quit, 1);
      retro_atomic_store_release_int(&mt_generation, ++mt_gen);
      retro_eventcount_notify(&mt_go);

      for (i = 0; i < mt_nthreads; i++)
         if (mt_thread[i])
         {
            sthread_join(mt_thread[i]);
            mt_thread[i] = NULL;
         }
      mt_nthreads = 0;
   }
   retro_atomic_store_release_int(&mt_quit, 0);
}

int R_RenderMTEnsure(int workers)
{
   int i;

   if (workers < 1)
      return 0;
   if (workers > RENDERMT_MAX)
      workers = RENDERMT_MAX;
   if (mt_nthreads == workers)
      return 1;

   rendermt_teardown();

   if (!mt_ec_ready)
   {
      if (!retro_eventcount_init(&mt_go))
      {
         retro_eventcount_free(&mt_go);
         return 0;
      }
      if (!retro_eventcount_init(&mt_done))
      {
         retro_eventcount_free(&mt_done);
         retro_eventcount_free(&mt_go);
         return 0;
      }
      mt_ec_ready = 1;
   }

   retro_atomic_store_release_int(&mt_pending, 0);
   mt_gen = 0;
   retro_atomic_store_release_int(&mt_generation, 0);

   for (i = 0; i < workers; i++)
   {
      mt_thread[i] = sthread_create(rendermt_worker, (void *)(intptr_t)i);
      if (!mt_thread[i])
      {
         mt_nthreads = i;
         rendermt_teardown();
         return 0;
      }
   }
   mt_nthreads = workers;
   return 1;
}

void R_RenderMTRun(rendermt_fn fn, void *base, size_t elemsize, int n)
{
   if (!fn || n < 1 || n > mt_nthreads)
      return;

   mt_fn      = fn;
   mt_base    = (char *)base;
   mt_elem    = elemsize;
   mt_nactive = n;
   /* Every worker wakes and decrements; those at or above n simply have no
    * item to run.  Counting all of them keeps the join a single compare. */
   retro_atomic_store_release_int(&mt_pending, mt_nthreads);
   /* Publishes the four fields above and the count: a worker reads them
    * only after acquire-loading this word. */
   retro_atomic_store_release_int(&mt_generation, ++mt_gen);
   retro_eventcount_notify(&mt_go);
}

void R_RenderMTWait(void)
{
   if (mt_nthreads < 1)
      return;

#if defined(RETRO_ATOMIC_LOCK_FREE)
   /* Only where the atomics are real instructions.  Where they are not --
    * the volatile fallback, and the EE, which masks interrupts around a
    * read-modify-write and reschedules out of one -- a spinning thread is
    * taking time from the very workers it is waiting on. */
   {
      int spins   = RENDERMT_SPIN;
      int backoff = 1;

      while (spins > 0)
      {
         int k;
         if (retro_atomic_load_acquire_int(&mt_pending) == 0)
            return;
         for (k = 0; k < backoff; k++)
            RENDERMT_RELAX();
         spins -= backoff;
         if (backoff < RENDERMT_BACKOFF_MAX)
            backoff <<= 1;
      }
   }
#endif

   for (;;)
   {
      int key;

      if (retro_atomic_load_acquire_int(&mt_pending) == 0)
         return;

      key = retro_eventcount_prepare_wait(&mt_done);
      if (retro_atomic_load_acquire_int(&mt_pending) == 0)
      {
         retro_eventcount_cancel_wait(&mt_done);
         return;
      }
      retro_eventcount_commit_wait(&mt_done, key);
   }
}

void R_RenderMTShutdown(void)
{
   rendermt_teardown();
   if (mt_ec_ready)
   {
      retro_eventcount_free(&mt_done);
      retro_eventcount_free(&mt_go);
      mt_ec_ready = 0;
   }
   if (wall_tint_lock) { slock_free(wall_tint_lock); wall_tint_lock = NULL; }
}

void R_RenderMTTintLockInit(void)
{
   if (!wall_tint_lock)
      wall_tint_lock = slock_new();
}

void R_RenderMTTintLock(void)
{
   if (wall_tint_lock)
      slock_lock(wall_tint_lock);
}

void R_RenderMTTintUnlock(void)
{
   if (wall_tint_lock)
      slock_unlock(wall_tint_lock);
}

#else /* !HAVE_THREADS */

int  R_RenderMTEnsure(int workers) { (void)workers; return 0; }
void R_RenderMTRun(rendermt_fn fn, void *base, size_t elemsize, int n)
                                 { (void)fn; (void)base; (void)elemsize; (void)n; }
void R_RenderMTWait(void)          { }
void R_RenderMTShutdown(void)                 { }
void R_RenderMTTintLockInit(void)             { }
void R_RenderMTTintLock(void)                 { }
void R_RenderMTTintUnlock(void)               { }

#endif
