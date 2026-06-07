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
static int R_DrawCmdKernelClass(const drawcmd_t *cmd)
{
  int cls = R_WallColumnKernelClass(cmd->fn);
  int th  = cmd->dc.texheight;

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

void R_DrawCmdReplay(void)
{
  int i, x;
  int sweep_remaining = 0;

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
    sweep_remaining++;
  }

  /* Each sweep pops at most one record per column, so x-adjacent pops
   * form runs of strictly ascending x; a column with several tier
   * records feeds one to each sweep. */
  while (sweep_remaining > 0)
  {
    const draw_column_vars_t *run[WALL_RUN_MAX];
    int run_n = 0;
    int run_cls = 0;
    int last_x = -2;

    for (x = 0; x < SCREENWIDTH; x++)
    {
      int idx = -1;
      int cls = 0;

      if (bucket_stamp[x] == bucket_epoch && bucket_head[x] >= 0)
      {
        idx = bucket_head[x];
        bucket_head[x] = bucket_next[idx];
        sweep_remaining--;
        cls = R_DrawCmdKernelClass(&cmds[idx]);
      }

      if (run_n &&
          (idx < 0 || x != last_x + 1 || cls != run_cls ||
           run_n == WALL_RUN_MAX))
      {
        R_DrawWallColumnRun(run, run_n, run_cls == 2);
        run_n = 0;
      }
      if (idx >= 0)
      {
        run[run_n++] = &cmds[idx].dc;
        run_cls = cls;
        last_x = x;
      }
    }
    if (run_n)
      R_DrawWallColumnRun(run, run_n, run_cls == 2);
  }
  cmd_count = 0;

  /* Every record has drawn; the texture data may move again. */
  for (i = 0; i < lock_count; i++)
    R_UnlockTextureCompositePatchNum(locks[i]);
  lock_count = 0;
}
