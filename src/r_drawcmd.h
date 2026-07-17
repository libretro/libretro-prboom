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
 *      Retained wall-column draw records.
 *
 *      The BSP walk emits fully clipped wall columns into a per-frame
 *      command buffer instead of rasterizing them inline; the buffer is
 *      replayed in emission order once the walk completes.  This is the
 *      seam that separates scene extraction from rasterization: the
 *      software consumer below replays through the existing column
 *      drawers (and is pixel-identical to inline drawing by
 *      construction), and later consumers can batch, reorder, shard
 *      across threads, or hand the records to a hardware backend.
 *
 *-----------------------------------------------------------------------------*/

#ifndef __R_DRAWCMD__
#define __R_DRAWCMD__

#include "r_draw.h"
#include "doomtype.h"

/* Append one wall column.  The dcvars snapshot is copied by value; the
 * texture data it points at must stay resident until replay, which the
 * frame-scoped composite locks below guarantee. */
void R_DrawCmdEmitColumn(const draw_column_vars_t *dcvars, R_DrawColumn_f fn);

/* Transfer ownership of one composite-texture lock (taken with
 * R_CacheTextureCompositePatchNum) to the command buffer.  The caller
 * must not unlock it; replay unlocks every adopted texture after the
 * last record has drawn. */
void R_DrawCmdAdoptTextureLock(int texnum);

/* Replay all records in emission order through their column drawers,
 * then release the adopted texture locks and reset the buffer. */
void R_DrawCmdReplay(void);

/* The kernel class a column would replay through (0 = its own drawer fn).
 * Exposed so the emit site can decide, with the same test, whether a colour
 * tint can ride in dcvars.tint (kernel path) or must be recorded for the
 * RMW replay pass (fn path). */
int R_DrawCmdColumnKernelClass(const draw_column_vars_t *dc, R_DrawColumn_f fn);

#endif
