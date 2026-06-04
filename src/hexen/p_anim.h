/* Emacs style mode select   -*- C -*-
 *-----------------------------------------------------------------------------
 *
 *  Hexen flat/texture animation and scrolling line specials.
 *
 *  Hexen does not use Doom's ANIMATED lump; its animations are defined by
 *  the ANIMDEFS script in the IWAD (per-frame tic counts, optionally
 *  randomised), and its scrolling walls are the per-line specials 100-103
 *  driven once per tic.  Derived from Raven's p_anim.c via dsda-doom.
 *
 *-----------------------------------------------------------------------------
 */

#ifndef __P_ANIM_H__
#define __P_ANIM_H__

/* Parse the ANIMDEFS lump (startup; no-op outside hexen). */
void P_InitFTAnims(void);

/* Collect the scrolling lines (specials 100-103) for this level
 * (P_SpawnSpecials; no-op outside hexen). */
void P_SpawnLineSpecials(void);

/* Advance flat/texture animations and scroll the collected lines, once per
 * tic (P_UpdateSpecials; no-op outside hexen). */
void P_AnimateHexenSurfaces(void);

#endif
