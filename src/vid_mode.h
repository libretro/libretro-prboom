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
 *      VID_MODEHDR10    -- HDR10 (RETRO_PIXEL_FORMAT_HDR10_2101010): the
 *                          same 10-bit-per-channel layout, but the samples
 *                          are PQ-encoded Rec.2020 absolute luminance, so
 *                          the core decides how bright each pixel is and
 *                          emissive content can exceed SDR white.
 *
 *-----------------------------------------------------------------------------*/

#ifndef __VID_MODE_H__
#define __VID_MODE_H__

#include <stdint.h>

enum vid_mode_e
{
  VID_MODE565 = 0,
  VID_MODE8888,
  VID_MODEHDR10        /* PQ-encoded Rec.2020, 10bpc -- absolute luminance */
};

extern int vid_mode;        /* enum vid_mode_e; set once at load */
extern int vid_pixelbytes;  /* 2 for VID_MODE565, else 4 */

#define VID_TRUECOLOR (vid_mode != VID_MODE565)
#define VID_HDR       (vid_mode == VID_MODEHDR10)

/* ---- HDR10 output ---------------------------------------------------------
 * In HDR mode the surface carries absolute luminance, PQ-encoded over
 * Rec.2020, so the core -- not the frontend -- decides how bright each pixel
 * is.  Ordinary image content is mapped to `vid_paper_white_nits` so it looks
 * exactly as it does in SDR; anything the renderer marks as emissive is
 * scaled above that and genuinely glows on an HDR display.
 *
 * Because PQ is strongly non-linear, blending encoded samples directly would
 * be far more wrong than the gamma-space blending the SDR paths do (a 50/50
 * mix lands ~63% dark instead of ~38%).  The read-modify-write kernels
 * therefore convert through vid_pq_to_sdr[] / vid_sdr_to_pq[], which are
 * inverses over the SDR range: blends run on the same 10-bit gamma-encoded
 * values the XRGB8888 path uses, so translucency, fuzz, water and light
 * tints look identical in every format.  Blending clears the emissive scale,
 * which is what you want -- a highlight seen through glass is no longer a
 * highlight. */
extern float vid_paper_white_nits;      /* frontend's SDR white, default 200 */
extern uint16_t vid_pq_to_sdr[1024];    /* PQ code -> 10-bit gamma (clamped)  */
extern uint16_t vid_sdr_to_pq[1024];    /* 10-bit gamma -> PQ code at paper white */
extern uint16_t vid_pq_boost[1024];     /* PQ code -> same colour, N x brighter  */
extern int      vid_emit_class;         /* strength of the emissive boost        */

/* Frontend "Colour Boost".  Mirrors RETRO_ENVIRONMENT_GET_HDR_EXPAND_GAMUT;
 * the core must apply the same rotation the frontend applies to SDR content
 * or saturation changes when switching between an SDR format and HDR10. */
#define VID_GAMUT_ACCURATE 0
#define VID_GAMUT_EXPANDED 1
#define VID_GAMUT_WIDE     2
#define VID_GAMUT_SUPER    3
extern int      vid_expand_gamut;

/* Which HDR swapchain the frontend is presenting with.  The two accept the
 * same PQ frame but differ in what they do to its primaries: HDR10 presents
 * them unchanged, scRGB applies Rec.2020 -> Rec.709 on the way.  Since the
 * gamut choice is ours to make, we have to compensate for that rotation or
 * it silently undoes it. */
#define VID_HDROUT_OFF   0
#define VID_HDROUT_HDR10 1
#define VID_HDROUT_SCRGB 2
extern int      vid_hdr_output;

/* Emissive classes.  The renderer tags a colour table entry with one of
 * these; the palette build multiplies that entry's luminance before the PQ
 * encode.  Class 0 is ordinary content at exactly paper white. */
#define VID_EMIT_NONE  0
#define VID_EMIT_2X    1
#define VID_EMIT_4X    2
#define VID_EMIT_8X    3
extern const float vid_emit_scale[4];

/* Colour-space helpers (vid_mode.c).  Build-time only -- no per-pixel use. */
double VID_PQEncode(double nits);
double VID_PQDecode(double signal);
double VID_SRGBToLinear(double c);
double VID_LinearToSRGB(double l);
void   VID_709To2020(double *r, double *g, double *b);
void   VID_EncodeHDR10(double r, double g, double b, double emit,
                       int *or_, int *og, int *ob);
void   VID_BuildHDRTables(void);

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
                   : (vid_mode == VID_MODEHDR10 ? 1023 : 255))
#define VID_CMAX_G (vid_mode == VID_MODE565 ? 63 \
                   : (vid_mode == VID_MODEHDR10 ? 1023 : 255))
#define VID_CMAX_B VID_CMAX_R

/* Per-channel field width used to CARRY such an amount between the producer
 * and the drawing kernels (see draw_column_vars_t::tint).  Ten bits is the
 * widest channel any supported format has, and three of them fit in the
 * 32-bit field with two bits to spare. */
#define VID_TINT_BITS  10
#define VID_TINT_MASK  1023

#endif /* __VID_MODE_H__ */
