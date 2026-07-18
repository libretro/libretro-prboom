/* Emacs style mode select   -*- C++ -*-
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
 *      Runtime output pixel format ("Color Format" core option).
 *
 *      The format is fixed for the whole session: it is chosen once in
 *      retro_load_game (from the core option, subject to what the
 *      frontend's SET_PIXEL_FORMAT accepts) before any surface is
 *      allocated or any palette LUT is built, and never changes until
 *      the core is reloaded.  Every consumer may therefore read
 *      vid_mode / vid_pixelbytes freely without ordering concerns.
 *
 *      VID_MODE565      -- RGB565, the historical renderer.  All legacy
 *                          16-bit code paths run exactly as before; no
 *                          truecolor code executes.
 *      VID_MODE8888     -- XRGB8888.  32-bit surface, palette and light
 *                          LUTs carry full 8-bit channels.
 *      VID_MODE2101010  -- XRGB2101010 (RETRO_PIXEL_FORMAT_XRGB2101010).
 *                          32-bit surface, LUTs carry 10-bit channels;
 *                          smooth-shading gradients resolve 4x finer
 *                          than 8-bit output can express.
 *
 *-----------------------------------------------------------------------------*/

#ifndef __VID_MODE_H__
#define __VID_MODE_H__

enum vid_mode_e
{
  VID_MODE565 = 0,
  VID_MODE8888,
  VID_MODE2101010
};

extern int vid_mode;        /* enum vid_mode_e; set once at load */
extern int vid_pixelbytes;  /* 2 for VID_MODE565, else 4 */

#define VID_TRUECOLOR (vid_mode != VID_MODE565)

/* ---- 32-bit channel layout ------------------------------------------------
 * XRGB8888:    R at 16 (8 bits), G at 8 (8 bits), B at 0 (8 bits)
 * XRGB2101010: R at 20 (10 bits), G at 10 (10 bits), B at 0 (10 bits)
 * (Both little-endian packed words; the top bits are ignored by the frontend.)
 */

/* Maximum value of one channel in the ACTIVE format.  RGB565 keeps its 5/6/5
 * asymmetry; both truecolor formats are symmetric.
 *
 * Anything that produces an additive colour amount -- dynamic-light chroma,
 * for one -- should scale to THESE, not to the 565 maxima, so the amount
 * carries the output's precision instead of being quantised to 5/6 bits and
 * multiplied back up (which lands on every 8th or 32nd code value and bands
 * exactly where a coloured light's falloff should be smoothest). */
#define VID_CMAX_R (vid_mode == VID_MODE565 ? 31 \
                   : (vid_mode == VID_MODE2101010 ? 1023 : 255))
#define VID_CMAX_G (vid_mode == VID_MODE565 ? 63 \
                   : (vid_mode == VID_MODE2101010 ? 1023 : 255))
#define VID_CMAX_B VID_CMAX_R

/* Per-channel field width used to CARRY such an amount between the producer
 * and the drawing kernels (see draw_column_vars_t::tint).  Ten bits is the
 * widest channel any supported format has, and three of them fit in the
 * 32-bit field with two bits to spare. */
#define VID_TINT_BITS  10
#define VID_TINT_MASK  1023

#endif /* __VID_MODE_H__ */
