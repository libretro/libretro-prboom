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
 *      Worker pool for the threaded wall replay.
 *
 *      This header deliberately declares nothing but plain C functions and
 *      exposes no libretro-common type.  rthreads.h pulls in
 *      retro_miscellaneous.h, which on Windows pulls in windows.h -- and the
 *      build force-includes z_zone.h into every translation unit, which
 *      macro-redefines malloc/free/calloc/realloc/strdup.  Windows system
 *      headers do not survive that combination and the TU fails to parse
 *      (observed on MinGW).  Confining every threading include to
 *      r_wallmt.c, which is compiled with Z_ZONE_NO_ALLOC_OVERRIDE, keeps
 *      windows.h away from the hijacked allocator names and keeps the rest
 *      of the renderer free of libretro-common headers.
 *
 *      All entry points are safe to call when the core is built without
 *      threads; R_WallMTEnsure then simply reports failure and the caller
 *      keeps its serial path.
 *
 *-----------------------------------------------------------------------------
 */

#ifndef __R_WALLMT__
#define __R_WALLMT__

typedef void (*wallmt_fn)(void *arg);

/* Bring the pool up to `workers` threads, rebuilding it only when that
 * count changes.  Returns non-zero when a pool of that size is available;
 * zero means the caller must run its work serially. */
int  R_WallMTEnsure(int workers);

/* Queue one work item.  Only valid after a successful R_WallMTEnsure. */
void R_WallMTSubmit(wallmt_fn fn, void *arg);

/* Block until every queued item of this dispatch has finished. */
void R_WallMTWait(void);

/* Join and release the workers.  Safe to call when no pool exists. */
void R_WallMTShutdown(void);

/* Guards the rare shared append in R_WallTintRecord (r_draw.c).  Both are
 * no-ops until R_WallMTTintLockInit has run, and on builds without
 * threads. */
void R_WallMTTintLockInit(void);
void R_WallMTTintLock(void);
void R_WallMTTintUnlock(void);

#endif
