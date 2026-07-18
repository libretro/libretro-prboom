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

/* ---- 32-bit channel layout constants -------------------------------------
 * XRGB8888:    R at 16 (8 bits), G at 8 (8 bits), B at 0 (8 bits)
 * XRGB2101010: R at 20 (10 bits), G at 10 (10 bits), B at 0 (10 bits)
 * (Both little-endian packed words, top bits ignored by the frontend.) */

/* Pack an 8-bit-per-channel colour into the ACTIVE truecolor format.
 * For 2101010 each 8-bit channel is expanded to 10 bits by bit
 * replication ((c<<2)|(c>>6)), the standard depth-expansion that maps
 * 0->0 and 255->1023 exactly. */
static INLINE unsigned int VID_PackRGB888(int r, int g, int b)
{
  if (vid_mode == VID_MODE2101010)
    return (((unsigned)((r << 2) | (r >> 6)) << 20) |
            ((unsigned)((g << 2) | (g >> 6)) << 10) |
             (unsigned)((b << 2) | (b >> 6)));
  return (((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b);
}

/* 50/50 blend of two packed pixels in the ACTIVE truecolor format:
 * clear each channel's low bit, halve, add -- the 32-bit analogue of
 * TL_BLEND565.  The masks drop the lowest bit of every channel field so
 * the shift cannot bleed across field boundaries. */
#define VID_TC5050_MASK8888 0x00FEFEFEu
#define VID_TC5050_MASKA2   0x3FEFFBFEu

static INLINE unsigned int VID_TCBlend5050(unsigned int s, unsigned int d)
{
  unsigned int m = (vid_mode == VID_MODE2101010) ? VID_TC5050_MASKA2
                                                 : VID_TC5050_MASK8888;
  return (((s & m) >> 1) + ((d & m) >> 1));
}

/* 94/6 blend toward a colour (the fuzz darken): per-channel
 * (c*15 + o)/16.  8888 can use the 565-style two-group trick (the X
 * byte and the gap above each field absorb the *15 carry); 2101010 has
 * adjacent fields and must unpack. */
static INLINE unsigned int VID_TCBlend9406(unsigned int c, unsigned int o)
{
  if (vid_mode == VID_MODE2101010)
  {
    int r = (int)((c >> 20) & 0x3FF), g = (int)((c >> 10) & 0x3FF), b = (int)(c & 0x3FF);
    int orr = (int)((o >> 20) & 0x3FF), og = (int)((o >> 10) & 0x3FF), ob = (int)(o & 0x3FF);
    r = (r * 15 + orr) >> 4;
    g = (g * 15 + og) >> 4;
    b = (b * 15 + ob) >> 4;
    return ((unsigned)r << 20) | ((unsigned)g << 10) | (unsigned)b;
  }
  return (((((c & 0x00FF00FFu) * 15 + (o & 0x00FF00FFu)) >> 4) & 0x00FF00FFu) |
          ((((c & 0x0000FF00u) * 15 + (o & 0x0000FF00u)) >> 4) & 0x0000FF00u));
}

/* Per-channel saturating add of a tint given in RGB565 CHANNEL UNITS
 * (r,b in 0..31, g in 0..63), the units every dynamic-light producer
 * emits (see r_dynlight.c / R_TintLUT).  The 565 units are upscaled to
 * the active format's channel width (<<3 / <<2 for 8-bit, <<5 / <<4
 * for 10-bit), preserving producer code untouched and saturating
 * identically at the top of each channel. */
static INLINE unsigned int VID_TCAddSat565Units(unsigned int px,
                                                int ar, int ag, int ab)
{
  if (vid_mode == VID_MODE2101010)
  {
    int r = (int)((px >> 20) & 0x3FF) + (ar << 5);
    int g = (int)((px >> 10) & 0x3FF) + (ag << 4);
    int b = (int)( px        & 0x3FF) + (ab << 5);
    if (r > 0x3FF) r = 0x3FF;
    if (g > 0x3FF) g = 0x3FF;
    if (b > 0x3FF) b = 0x3FF;
    return ((unsigned)r << 20) | ((unsigned)g << 10) | (unsigned)b;
  }
  else
  {
    int r = (int)((px >> 16) & 0xFF) + (ar << 3);
    int g = (int)((px >>  8) & 0xFF) + (ag << 2);
    int b = (int)( px        & 0xFF) + (ab << 3);
    if (r > 0xFF) r = 0xFF;
    if (g > 0xFF) g = 0xFF;
    if (b > 0xFF) b = 0xFF;
    return ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b;
  }
}

#endif /* __VID_MODE_H__ */
