/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
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
 *      Retained wall-column draw records (see r_drawcmd.h).
 *
 *-----------------------------------------------------------------------------*/

#include <stdlib.h>

#include "doomtype.h"
#include "r_draw.h"
#include "r_data.h"
#include "r_drawcmd.h"
#include "lprintf.h"
#include "doomstat.h"

#include "r_wallmt.h"

#define WALL_RUN_MAX 64

typedef struct
{
  draw_column_vars_t dc;
  R_DrawColumn_f     fn;
} drawcmd_t;

/* The record arena and the adopted-lock list both persist across
 * frames and only grow; counts reset at the end of each replay.  At a
 * 2560-wide view a frame emits a few thousand records, so the arenas
 * settle after the first busy scene and emission is an append with no
 * allocation on the steady path. */
static drawcmd_t *cmds      = NULL;
static int        cmd_count = 0;
static int        cmd_cap   = 0;

static int *locks      = NULL;
static int  lock_count = 0;
static int  lock_cap   = 0;

void R_DrawCmdEmitColumn(const draw_column_vars_t *dcvars, R_DrawColumn_f fn)
{
  drawcmd_t *cmd;

  if (cmd_count == cmd_cap)
  {
    cmd_cap = cmd_cap ? cmd_cap * 2 : 4096;
    cmds = (drawcmd_t *) realloc(cmds, cmd_cap * sizeof(*cmds));
    if (!cmds)
      I_Error("R_DrawCmdEmitColumn: failed to grow the record arena");
  }
  cmd = &cmds[cmd_count++];
  cmd->dc = *dcvars;
  cmd->fn = fn;
}

void R_DrawCmdAdoptTextureLock(int texnum)
{
  if (lock_count == lock_cap)
  {
    lock_cap = lock_cap ? lock_cap * 2 : 256;
    locks = (int *) realloc(locks, lock_cap * sizeof(*locks));
    if (!locks)
      I_Error("R_DrawCmdAdoptTextureLock: failed to grow the lock list");
  }
  locks[lock_count++] = texnum;
}

/* Per-x bucket chains for run formation.  head/stamp are indexed by
 * screen column; a bucket is live only when its stamp matches the
 * current frame epoch, so no per-frame clearing pass is needed.  next
 * chains records that landed on the same column (wall tiers). */
static int bucket_head[MAX_SCREENWIDTH];
static int bucket_tail[MAX_SCREENWIDTH];
static unsigned bucket_stamp[MAX_SCREENWIDTH];
static unsigned bucket_epoch = 0;
static int *bucket_next = NULL;
static int  bucket_next_cap = 0;

/* A record qualifies for the wall-run kernel when its drawer is one
 * the kernel reproduces and its texture height is zero or a power of
 * two (the drawers have a modulo path for other heights that the
 * kernel does not mirror). */
int R_DrawCmdColumnKernelClass(const draw_column_vars_t *dc, R_DrawColumn_f fn)
{
  int cls = R_WallColumnKernelClass(fn);
  int th  = dc->texheight;

  /* A brightmap column needs the per-texel fullbright select, which lives
   * in the column drawers, not the wall-run quad kernel; route it through
   * its fn() by declaring it unclassed.  Brightmap columns are rare, so
   * the common wall path keeps its quad-flush optimization untouched. */
  if (dc->brightmask)
    return 0;

  /* The LinearUV drawers are deliberately not kernel-classed.  A
   * row-major run kernel for them was built and proven bit-identical
   * (it needs the drawers' half-texel frac seed, a signed shift for
   * the texheight==0 index so negative fracs read the column padding
   * like frac>>16 does, and a re-creation of the drawer head that
   * delegates columns with iscale > drawvars.mag_threshold to the
   * point drawer for the current filterz), but it measured ~6%
   * slower than the column-major drawers at 2560x1600: the four
   * texel and four V_Palette16 loads per pixel amortize the
   * temp-buffer overhead the kernel exists to remove, and walking
   * the run row-major touches every lane's texture columns per row
   * where the drawers stream one column sequentially.  Demoting only
   * the mag_threshold columns to the point classes was within noise,
   * since those columns are distant and short and the linear/point
   * class transitions fragment the runs. */
  if (cls && (th & (th - 1)) == 0)
    return cls;
  return 0;
}

static int R_DrawCmdKernelClass(const drawcmd_t *cmd)
{
  return R_DrawCmdColumnKernelClass(&cmd->dc, cmd->fn);
}

/* ---------------------------------------------------------------------------
 * Threaded replay.
 *
 * The BSP walk stays single-threaded and produces one complete command list;
 * only the rasterisation of that list is split.  That is deliberate.  The
 * screenspace-BSP-split used by Rum and Raisin and Eternity parallelises the
 * walk as well, but it duplicates traversal, sprite projection and visplane
 * management in every thread -- which is why that design needs roughly four
 * cores before it pays -- and it can split or drop a sprite straddling a
 * slice boundary, because each thread decides that sprite's visibility on
 * its own.  Here the walk measures ~2.5% of frame time, so parallelising it
 * would buy almost nothing, while leaving it alone keeps the threaded output
 * bit-identical to the single-threaded output.  That is testable, and tested.
 *
 * Ownership: wall columns are disjoint in x and every dependency in the
 * kernel is vertical.  bucket_head/bucket_tail/bucket_stamp are indexed by
 * x, bucket_next by record index, and each record belongs to exactly one
 * column, so a slice owns its columns outright -- no two slices touch the
 * same bucket entry or the same framebuffer pixel.
 *
 * Slices are cut by equal *pixels* rather than equal columns.  The whole
 * command list exists before dispatch, so the exact per-column pixel count
 * is known and one prefix sum splits the work evenly: no inter-frame
 * feedback loop (Rum and Raisin) and no assuming the scene is spread evenly
 * across the screen (Eternity splits columns equally).
 * ------------------------------------------------------------------------ */
#define WALL_MAX_SLICES 16

/* A slice has to carry enough pixels to be worth a wakeup and a join.  This
 * is a floor that stops a light frame -- mostly sky, or a corridor with
 * little wall area -- from being split across every available worker and
 * spending more time dispatching than rasterising.  It is deliberately low:
 * the intent is to rule out the degenerate case, not to second-guess an
 * explicit thread count. */
#define WALL_MIN_SLICE_PX 65536L

typedef struct
{
  int            xlo, xhi;    /* inclusive column range owned by this slice */
  int            remaining;   /* records bucketed within that range         */
  long           px;          /* pixels in that range (diagnostic)          */
  wallscratch_t *ws;
} wallslice_t;

static wallscratch_t wall_scratch[WALL_MAX_SLICES];
static wallslice_t   wall_slices[WALL_MAX_SLICES];
static int           col_count[MAX_SCREENWIDTH];
static int           col_px[MAX_SCREENWIDTH];
static unsigned      col_stamp[MAX_SCREENWIDTH];

/* Requested worker count, 1 = single-threaded.  Set from the core option. */
static int render_threads = 1;

void R_SetRenderThreads(int n)
{
  if (n < 1)               n = 1;
  if (n > WALL_MAX_SLICES) n = WALL_MAX_SLICES;
  render_threads = n;
  R_WallMTTintLockInit();
}

int R_GetRenderThreads(void)
{
  return render_threads;
}

void R_WallReplayShutdown(void)
{
  R_WallMTShutdown();
}

/* One slice's sweep: the single-threaded sweep scoped to a column range,
 * with a private record count and a private scratch. */
static void R_WallSliceSweep(void *arg)
{
  wallslice_t *sl        = (wallslice_t *)arg;
  int          remaining = sl->remaining;
  int          x;

  while (remaining > 0)
  {
    const draw_column_vars_t *run[WALL_RUN_MAX];
    int run_n   = 0;
    int run_cls = 0;
    int last_x  = -2;

    for (x = sl->xlo; x <= sl->xhi; x++)
    {
      int idx = -1;
      int cls = 0;

      if (bucket_stamp[x] == bucket_epoch && bucket_head[x] >= 0)
      {
        idx = bucket_head[x];
        bucket_head[x] = bucket_next[idx];
        remaining--;
        cls = R_DrawCmdKernelClass(&cmds[idx]);
      }

      if (run_n &&
          (idx < 0 || x != last_x + 1 || cls != run_cls ||
           run_n == WALL_RUN_MAX))
      {
        R_DrawWallColumnRun(sl->ws, run, run_n, run_cls == 2);
        run_n = 0;
      }
      if (idx >= 0)
      {
        run[run_n++] = &cmds[idx].dc;
        run_cls = cls;
        last_x  = x;
        if (!remaining)
          break;
      }
    }
    if (run_n)
      R_DrawWallColumnRun(sl->ws, run, run_n, run_cls == 2);
  }
}

/* Cut [minx,maxx] into at most `want` ranges of roughly equal pixel count.
 * Returns the number of slices produced (always >= 1). */
static int R_WallBuildSlices(int minx, int maxx, int want, long totalpx)
{
  long target;
  long acc   = 0;
  int  n     = 0;
  int  start = minx;
  int  x, c;

  if (want < 1)               want = 1;
  if (want > WALL_MAX_SLICES) want = WALL_MAX_SLICES;

  if (want > 1)
  {
    long affordable = totalpx / WALL_MIN_SLICE_PX;
    if (affordable < 1)
      affordable = 1;
    if ((long)want > affordable)
      want = (int)affordable;
  }

  if (want > 1 && totalpx > 0)
  {
    target = totalpx / want;
    for (x = minx; x <= maxx; x++)
    {
      if (col_stamp[x] == bucket_epoch)
        acc += col_px[x];
      /* Close a slice once it holds its share, but never leave fewer
       * columns behind than there are slices still to fill. */
      if (n < want - 1 && acc >= target && (maxx - x) >= (want - n - 1))
      {
        wall_slices[n].xlo = start;
        wall_slices[n].xhi = x;
        wall_slices[n].remaining = 0;
        wall_slices[n].px = 0;
        for (c = start; c <= x; c++)
          if (col_stamp[c] == bucket_epoch)
          {
            wall_slices[n].remaining += col_count[c];
            wall_slices[n].px        += col_px[c];
          }
        wall_slices[n].ws = &wall_scratch[n];
        n++;
        start = x + 1;
        acc = 0;
      }
    }
  }

  wall_slices[n].xlo = start;
  wall_slices[n].xhi = maxx;
  wall_slices[n].remaining = 0;
  wall_slices[n].px = 0;
  for (c = start; c <= maxx; c++)
    if (col_stamp[c] == bucket_epoch)
    {
      wall_slices[n].remaining += col_count[c];
      wall_slices[n].px        += col_px[c];
    }
  wall_slices[n].ws = &wall_scratch[n];
  return n + 1;
}

void R_DrawCmdReplay(void)
{
  int  i;
  int  sweep_remaining = 0;
  int  sweep_minx = SCREENWIDTH, sweep_maxx = -1;
  long total_px = 0;
  int  nslices;

  /* Solid wall columns cover disjoint pixels (the clip arrays
   * guarantee it), so replay order is free for output correctness.
   * Records the wall-run kernel cannot reproduce replay individually
   * through their drawers, in emission order; the rest are bucketed by
   * screen column and rasterized as x-adjacent runs, row-major,
   * straight into the framebuffer. */
  bucket_epoch++;
  if (bucket_next_cap < cmd_cap)
  {
    bucket_next_cap = cmd_cap;
    bucket_next = (int *) realloc(bucket_next, bucket_next_cap * sizeof(int));
    if (!bucket_next)
      I_Error("R_DrawCmdReplay: failed to grow the bucket chain");
  }

  for (i = 0; i < cmd_count; i++)
  {
    int cx = cmds[i].dc.x;

    if (!R_DrawCmdKernelClass(&cmds[i]))
    {
      cmds[i].fn(&cmds[i].dc);
      continue;
    }
    if (bucket_stamp[cx] != bucket_epoch)
    {
      bucket_stamp[cx] = bucket_epoch;
      bucket_head[cx] = -1;
      bucket_tail[cx] = -1;
    }
    /* Append at the tail: sweep k must pop the k-th emitted record of
     * each column, so that where two records cover the same pixel
     * (rare clip-boundary overlaps), the later-emitted one still wins,
     * exactly as in emission-order replay. */
    bucket_next[i] = -1;
    if (bucket_tail[cx] >= 0)
      bucket_next[bucket_tail[cx]] = i;
    else
      bucket_head[cx] = i;
    bucket_tail[cx] = i;
    if (col_stamp[cx] != bucket_epoch)
    {
      col_stamp[cx] = bucket_epoch;
      col_count[cx] = 0;
      col_px[cx]    = 0;
    }
    col_count[cx]++;
    col_px[cx] += cmds[i].dc.yh - cmds[i].dc.yl + 1;
    total_px   += cmds[i].dc.yh - cmds[i].dc.yl + 1;
    sweep_remaining++;
    if (cx < sweep_minx) sweep_minx = cx;
    if (cx > sweep_maxx) sweep_maxx = cx;
  }

  /* Nothing bucketed: no slices to build and no threads to wake. */
  if (sweep_remaining > 0)
  {
    nslices = R_WallBuildSlices(sweep_minx, sweep_maxx,
                                render_threads, total_px);

    /* Size the pool to the option, not to this frame's slice count: the
     * plane pass shares it and sizes its own dispatch independently. */
    if (nslices > 1 && R_WallMTEnsure(render_threads - 1))
    {
      /* Slices 0..n-2 go to the pool; this thread takes the last one
       * rather than blocking on a join it could have spent rasterising. */
      R_WallMTRun(R_WallSliceSweep, wall_slices,
                  sizeof(wall_slices[0]), nslices - 1);
      R_WallSliceSweep(&wall_slices[nslices - 1]);
      R_WallMTWait();
    }
    else
    {
      for (i = 0; i < nslices; i++)
        R_WallSliceSweep(&wall_slices[i]);
    }
  }
  cmd_count = 0;

  /* Every record has drawn; the texture data may move again. */
  for (i = 0; i < lock_count; i++)
    R_UnlockTextureCompositePatchNum(locks[i]);
  lock_count = 0;
}
