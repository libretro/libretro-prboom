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
 *      Bench harness for the render worker pool (src/r_rendermt.c).  Runs
 *      the dispatch/join round trip the wall and plane passes perform twice
 *      a frame, with a slice body of a chosen duration, and reports what the
 *      round trip costs on top of that body.  Run it by hand against two
 *      checkouts to compare pool implementations; the numbers that matter
 *      are the two runs relative to each other on one machine, since the
 *      absolute cost is a property of the host's scheduler.
 *
 *      Build (from the repo root):
 *
 *        cc -O2 -o rendermt_bench tools/rendermt_bench.c src/r_rendermt.c \
 *           libretro/libretro-common/rthreads/rthreads.c \
 *           libretro/libretro-common/rthreads/retro_eventcount.c \
 *           -Isrc -Ilibretro/libretro-common/include -DHAVE_THREADS \
 *           -DINLINE=inline -lpthread
 *
 *      Drop retro_eventcount.c from that line for a tree whose pool does
 *      not use it.  Usage:
 *
 *        ./rendermt_bench [workers] [iterations] [slice_usec]
 *
 *-----------------------------------------------------------------------------
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "r_rendermt.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

#define BENCH_MAX_SLICES 16

typedef struct
{
   unsigned long spin;     /* iterations of the slice body               */
   unsigned long sink;     /* written by the slice, read at the end, so
                            * the body cannot be optimised away         */
} bench_slice_t;

static bench_slice_t bench_slices[BENCH_MAX_SLICES];

static double bench_now_usec(void)
{
#if defined(_WIN32)
   LARGE_INTEGER freq, now;
   QueryPerformanceFrequency(&freq);
   QueryPerformanceCounter(&now);
   return (double)now.QuadPart * 1000000.0 / (double)freq.QuadPart;
#else
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (double)ts.tv_sec * 1000000.0 + (double)ts.tv_nsec / 1000.0;
#endif
}

/* A slice body with no memory traffic to share and no syscall in it, so
 * what the harness measures around it is dispatch and join alone. */
static void bench_slice(void *arg)
{
   bench_slice_t          *s   = (bench_slice_t *)arg;
   /* volatile, or the recurrence below is replaced by its closed form and
    * the body costs nothing however long the trip count says it is -- which
    * calibration then answers by asking for an absurd one. */
   volatile unsigned long  acc = s->sink;
   unsigned long           i;

   for (i = 0; i < s->spin; i++)
      acc = acc * 1103515245UL + 12345UL;

   s->sink = acc;
}

/* How many iterations of the body come to one microsecond here. */
static unsigned long bench_calibrate(void)
{
   unsigned long spin = 1000;

   for (;;)
   {
      bench_slice_t s;
      double        t0, t1;

      s.spin = spin;
      s.sink = 1;
      t0     = bench_now_usec();
      bench_slice(&s);
      t1     = bench_now_usec();

      if (t1 - t0 >= 2000.0)
         return (unsigned long)((double)spin / (t1 - t0));
      if (spin > 400000000UL)
         return spin / 1000;
      spin *= 4;
   }
}

static int bench_cmp(const void *a, const void *b)
{
   double x = *(const double *)a;
   double y = *(const double *)b;
   return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
   int            workers  = (argc > 1) ? atoi(argv[1]) : 4;
   int            iters    = (argc > 2) ? atoi(argv[2]) : 2000;
   double         slice_us = (argc > 3) ? atof(argv[3]) : 100.0;
   int            nslices;
   int            i;
   unsigned long  per_usec;
   unsigned long  spin;
   double        *samples;
   double         total    = 0.0;
   double         body     = 0.0;
   double         t0, t1;

   if (workers < 1 || workers >= BENCH_MAX_SLICES)
      workers = 4;
   if (iters < 1)
      iters = 2000;
   if (slice_us < 0.0)
      slice_us = 0.0;

   /* The dispatching thread takes a slice of its own, exactly as the wall
    * and plane passes do, so the pool holds one fewer thread than there
    * are slices. */
   nslices  = workers + 1;
   per_usec = bench_calibrate();
   spin     = (unsigned long)(slice_us * (double)per_usec);

   if (!R_RenderMTEnsure(workers))
   {
      fprintf(stderr, "rendermt_bench: no pool of %d workers available\n",
            workers);
      return 1;
   }

   if (!(samples = (double *)malloc(sizeof(*samples) * (size_t)iters)))
   {
      R_RenderMTShutdown();
      return 1;
   }

   for (i = 0; i < BENCH_MAX_SLICES; i++)
   {
      bench_slices[i].spin = spin;
      bench_slices[i].sink = (unsigned long)(i + 1);
   }

   /* One slice on its own, for what the body costs with no pool involved. */
   t0 = bench_now_usec();
   for (i = 0; i < iters; i++)
      bench_slice(&bench_slices[0]);
   t1   = bench_now_usec();
   body = (t1 - t0) / (double)iters;

   for (i = 0; i < 64; i++)
   {
      R_RenderMTRun(bench_slice, bench_slices, sizeof(bench_slices[0]),
            nslices - 1);
      bench_slice(&bench_slices[nslices - 1]);
      R_RenderMTWait();
   }

   for (i = 0; i < iters; i++)
   {
      t0 = bench_now_usec();
      R_RenderMTRun(bench_slice, bench_slices, sizeof(bench_slices[0]),
            nslices - 1);
      bench_slice(&bench_slices[nslices - 1]);
      R_RenderMTWait();
      t1         = bench_now_usec();
      samples[i] = t1 - t0;
      total     += samples[i];
   }

   qsort(samples, (size_t)iters, sizeof(*samples), bench_cmp);

   printf("workers        %d (%d slices, dispatcher takes one)\n",
         workers, nslices);
   printf("iterations     %d\n", iters);
   printf("slice body     %.2f us asked, %.2f us measured serially\n",
         slice_us, body);
   printf("round trip     mean %.2f us  median %.2f us  p95 %.2f us"
          "  min %.2f us\n",
         total / (double)iters,
         samples[iters / 2],
         samples[(int)((double)iters * 0.95)],
         samples[0]);
   printf("over the body  mean %.2f us  median %.2f us\n",
         total / (double)iters - body, samples[iters / 2] - body);

   free(samples);
   R_RenderMTShutdown();

   /* Keeps the slice results live. */
   for (i = 0; i < BENCH_MAX_SLICES; i++)
      total += (double)bench_slices[i].sink;

   return (total == 0.0) ? 1 : 0;
}
