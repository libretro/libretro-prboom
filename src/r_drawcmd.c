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

void R_DrawCmdReplay(void)
{
  int i;

  /* Emission order replay: solid wall columns cover disjoint pixels
   * (the clip arrays guarantee it), so ordering is free for output
   * correctness, but emission order also walks the 4-column batching
   * state machine in r_draw exactly as inline drawing did, keeping the
   * output bit-identical. */
  for (i = 0; i < cmd_count; i++)
    cmds[i].fn(&cmds[i].dc);
  cmd_count = 0;

  /* Every record has drawn; the texture data may move again. */
  for (i = 0; i < lock_count; i++)
    R_UnlockTextureCompositePatchNum(locks[i]);
  lock_count = 0;
}
