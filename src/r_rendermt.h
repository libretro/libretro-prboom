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
 *      Worker pool shared by the threaded renderer passes: the wall
 *      replay (r_drawcmd.c) and the opaque plane fill (r_plane.c).
 *
 *      This header deliberately declares nothing but plain C functions and
 *      exposes no libretro-common type.  rthreads.h pulls in
 *      retro_miscellaneous.h, which on Windows pulls in windows.h -- and the
 *      build force-includes z_zone.h into every translation unit, which
 *      macro-redefines malloc/free/calloc/realloc/strdup.  Windows system
 *      headers do not survive that combination and the TU fails to parse
 *      (observed on MinGW).  Confining every threading include to
 *      r_rendermt.c, which is compiled with Z_ZONE_NO_ALLOC_OVERRIDE, keeps
 *      windows.h away from the hijacked allocator names and keeps the rest
 *      of the renderer free of libretro-common headers.
 *
 *      All entry points are safe to call when the core is built without
 *      threads; R_RenderMTEnsure then simply reports failure and the caller
 *      keeps its serial path.
 *
 *-----------------------------------------------------------------------------
 */

#ifndef __R_RENDERMT__
#define __R_RENDERMT__

#include <stddef.h>

typedef void (*rendermt_fn)(void *arg);

/* Bring the pool up to `workers` threads, rebuilding it only when that
 * count changes.  Returns non-zero when a pool of that size is available;
 * zero means the caller must run its work serially. */
int  R_RenderMTEnsure(int workers);

/* Run fn() on `n` work items in parallel, item i being
 * (char *)base + i * elemsize.  Slots are fixed -- worker i always takes
 * item i -- so there is no queue, no allocation and no contended lock to
 * acquire work; a dispatch is one broadcast and a join.  `n` may be fewer
 * than the pool size: workers above n wake and go straight back to sleep.
 * That matters because the wall and plane passes share one pool and size
 * their dispatches independently -- sizing the pool to each in turn would
 * tear it down and rebuild it twice a frame.  Returns immediately; pair
 * with R_RenderMTWait.  The caller is expected to process a further item
 * itself in between rather than idle. */
void R_RenderMTRun(rendermt_fn fn, void *base, size_t elemsize, int n);

/* Block until the dispatch has finished. */
void R_RenderMTWait(void);

/* Join and release the workers.  Safe to call when no pool exists. */
void R_RenderMTShutdown(void);

/* Guards the rare shared append in R_WallTintRecord (r_draw.c).  Both are
 * no-ops until R_RenderMTTintLockInit has run, and on builds without
 * threads. */
void R_RenderMTTintLockInit(void);
void R_RenderMTTintLock(void);
void R_RenderMTTintUnlock(void);

#endif
