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
 *      Active output pixel format, and the colour-space machinery the
 *      HDR10 format needs.
 *
 *      The format is chosen once by the libretro layer in retro_load_game,
 *      before any surface, palette or lookup table is built, and is
 *      constant thereafter.  It defaults to RGB565 so anything that runs
 *      before negotiation behaves exactly as it always did.
 *
 *-----------------------------------------------------------------------------*/

#include <math.h>

#include "vid_mode.h"

int   vid_mode              = VID_MODE565;
int   vid_pixelbytes        = 2;
float vid_paper_white_nits  = 200.0f;
float vid_max_nits          = 0.0f;

uint16_t vid_pq_to_sdr[1024];
uint16_t vid_sdr_to_pq[1024];
uint16_t vid_pq_boost[1024];
int      vid_emit_class = VID_EMIT_2X;
int      vid_expand_gamut = VID_GAMUT_ACCURATE;

/* ---- SMPTE ST.2084 (PQ) ---------------------------------------------------
 * Absolute luminance, 0..10000 nits, in the normalised 0..1 signal the
 * 10-bit samples carry.  These are the constants from the standard. */
#define PQ_M1 (2610.0 / 16384.0)
#define PQ_M2 ((2523.0 / 4096.0) * 128.0)
#define PQ_C1 (3424.0 / 4096.0)
#define PQ_C2 ((2413.0 / 4096.0) * 32.0)
#define PQ_C3 ((2392.0 / 4096.0) * 32.0)
#define PQ_MAXNITS 10000.0

double VID_PQEncode(double nits)
{
  double y, ym;
  if (nits <= 0.0)
    return 0.0;
  if (nits > PQ_MAXNITS)
    nits = PQ_MAXNITS;
  y  = nits / PQ_MAXNITS;
  ym = pow(y, PQ_M1);
  return pow((PQ_C1 + PQ_C2 * ym) / (1.0 + PQ_C3 * ym), PQ_M2);
}

double VID_PQDecode(double signal)
{
  double p, num, den;
  if (signal <= 0.0)
    return 0.0;
  p   = pow(signal, 1.0 / PQ_M2);
  num = p - PQ_C1;
  if (num < 0.0)
    num = 0.0;
  den = PQ_C2 - PQ_C3 * p;
  if (den < 1e-9)
    den = 1e-9;
  return PQ_MAXNITS * pow(num / den, 1.0 / PQ_M1);
}

/* Display transfer, used to move between the engine's gamma-encoded colours
 * and the linear light PQ needs.  The palette is already gamma-corrected by
 * the engine's own table, so this is the display transfer only.
 *
 * This must be the SAME curve the frontend applies to SDR content, or an HDR
 * frame will not match the SDR one: RetroArch's HDR composition linearises
 * with a pure 2.4 power (pow(sdr, 2.4), identically in the Vulkan and D3D
 * shaders), so that is what we invert here.  The sRGB piecewise curve is the
 * tempting choice and is wrong for this purpose -- its linear toe near black
 * lifts a 0.05 code by 5x and a 0.02 code by 18x, which lands as raised
 * blacks and flat contrast while highlights stay put. */
double VID_SRGBToLinear(double c)
{
  if (c <= 0.0)
    return 0.0;
  return pow(c, 2.4);
}

double VID_LinearToSRGB(double l)
{
  if (l <= 0.0)
    return 0.0;
  return pow(l, 1.0 / 2.4);
}

/* Rec.709 -> the target primaries, applied to linear light before the PQ
 * encode.  Which rotation is correct depends on the frontend's "Colour
 * Boost" setting, because that setting is implemented as a deliberate
 * mismatch: for anything other than Accurate the frontend rotates SDR
 * content into a space NARROWER than Rec.2020 (or leaves it in Rec.709
 * entirely) and then lets the display read it as Rec.2020, and the
 * resulting over-saturation is the effect the user asked for.
 *
 * A core encoding Rec.2020 itself has to make the same choice, or the same
 * scene changes saturation when the core switches between an SDR format and
 * HDR10.  Doing the "proper" conversion unconditionally is what makes the
 * HDR image look washed out beside the SDR one: on Super a saturated red
 * reaches 1.66 through the SDR path and only 1.0 through ours.
 *
 * Matrices are the frontend's own (hdr_common.glsl), so the two agree
 * exactly rather than approximately. */
void VID_709To2020(double *r, double *g, double *b)
{
  double lr = *r, lg = *g, lb = *b;

  switch (vid_expand_gamut)
  {
    case VID_GAMUT_EXPANDED:   /* Rec.709 -> a slightly wider space */
      *r =  0.6274040 * lr +  0.3292820 * lg + 0.0433136 * lb;
      *g =  0.0457456 * lr +  0.9417770 * lg + 0.0124772 * lb;
      *b = -0.00121055 * lr + 0.0176041 * lg + 0.9836070 * lb;
      break;
    case VID_GAMUT_WIDE:       /* Rec.709 -> DCI-P3 */
      *r =  0.8215873 * lr +  0.1763479 * lg +  0.0020641 * lb;
      *g =  0.0328261 * lr +  0.9695096 * lg + -0.0023367 * lb;
      *b =  0.0188038 * lr +  0.0725063 * lg +  0.9086907 * lb;
      break;
    case VID_GAMUT_SUPER:      /* no rotation: stays Rec.709 */
      break;
    case VID_GAMUT_ACCURATE:   /* proper Rec.709 -> Rec.2020 */
    default:
      *r = 0.6274040 * lr + 0.3292820 * lg + 0.0433136 * lb;
      *g = 0.0690970 * lr + 0.9195400 * lg + 0.0113612 * lb;
      *b = 0.0163916 * lr + 0.0880132 * lg + 0.8955950 * lb;
      break;
  }
}

/* Highlight rolloff toward the frontend's peak, vid_max_nits.
 *
 * The frontend presents HDR10 samples as they are, so luminance above what
 * the display reaches is clipped there and an emissive highlight turns into
 * a flat patch.  Below the knee, halfway between paper white and the peak,
 * light passes unchanged; above it the curve approaches the peak
 * asymptotically with unit slope at the knee, so gradation survives all the
 * way up.  Ordinary content never exceeds paper white and is never touched.
 * With no headroom above paper white, or an unknown peak, it is identity. */
double VID_RollOff(double nits)
{
  double pw = vid_paper_white_nits;
  double m  = vid_max_nits;
  double s, r;
  if (m <= pw)
    return nits;
  s = 0.5 * (pw + m);
  if (nits <= s)
    return nits;
  r = m - s;
  return s + r * (1.0 - exp(-(nits - s) / r));
}

/* Emissive multiplier for the active class.  Auto puts the brightest
 * emissive texel (a paper-white colour) at 95% of the frontend's peak after
 * the rolloff: with the knee at s and range r = m - s, that input is
 * s + r * ln(r / (0.05 * m)).  An unknown peak gives the 2x default. */
double VID_EmitScale(void)
{
  switch (vid_emit_class)
  {
    case VID_EMIT_NONE: return 1.0;
    case VID_EMIT_4X:   return 4.0;
    case VID_EMIT_8X:   return 8.0;
    case VID_EMIT_AUTO:
      {
        double pw = vid_paper_white_nits;
        double m  = vid_max_nits;
        double s, r, k;
        if (pw <= 0.0 || m <= 0.0)
          return 2.0;
        if (m <= pw)
          return 1.0;
        s = 0.5 * (pw + m);
        r = m - s;
        k = (s + r * log(r / (0.05 * m))) / pw;
        return k < 1.0 ? 1.0 : (k > 16.0 ? 16.0 : k);
      }
    case VID_EMIT_2X:
    default:            return 2.0;
  }
}

/* Encode one gamma-encoded Rec.709 channel triple, scaled by `emit`, into
 * PQ Rec.2020 10-bit samples.  `emit` is the emissive multiplier: 1.0 puts
 * the colour exactly at paper white, higher values push it above. */
void VID_EncodeHDR10(double r, double g, double b, double emit,
                     int *or_, int *og, int *ob)
{
  double lr = VID_SRGBToLinear(r) * vid_paper_white_nits * emit;
  double lg = VID_SRGBToLinear(g) * vid_paper_white_nits * emit;
  double lb = VID_SRGBToLinear(b) * vid_paper_white_nits * emit;
  int    v;

  VID_709To2020(&lr, &lg, &lb);
  lr = VID_RollOff(lr);
  lg = VID_RollOff(lg);
  lb = VID_RollOff(lb);

  v = (int)(VID_PQEncode(lr) * 1023.0 + 0.5); *or_ = v < 0 ? 0 : (v > 1023 ? 1023 : v);
  v = (int)(VID_PQEncode(lg) * 1023.0 + 0.5); *og  = v < 0 ? 0 : (v > 1023 ? 1023 : v);
  v = (int)(VID_PQEncode(lb) * 1023.0 + 0.5); *ob  = v < 0 ? 0 : (v > 1023 ? 1023 : v);
}

/* Build the blend conversion tables.
 *
 * The renderer blends in gamma-encoded space, as it always has; matching
 * that in HDR mode is what keeps translucency, fuzz, water and light tints
 * looking the same in every colour format.  These tables move a single
 * channel between its PQ code and the equivalent 10-bit gamma value at
 * paper white, so the existing integer blend arithmetic can be reused
 * unchanged between the two conversions.
 *
 * Note the asymmetry: PQ codes above paper white have no gamma-encoded
 * equivalent and clamp to 1023.  That is deliberate -- a blended pixel
 * stops being emissive, which is physically what you want when a glowing
 * surface is seen through glass or smeared by the fuzz effect. */
void VID_BuildHDRTables(void)
{
  int i;

  for (i = 0; i < 1024; i++)
  {
    double nits = VID_PQDecode((double)i / 1023.0);
    double lin  = nits / (vid_paper_white_nits > 0.0 ? vid_paper_white_nits : 1.0);
    double srgb = VID_LinearToSRGB(lin > 1.0 ? 1.0 : lin);
    int    v    = (int)(srgb * 1023.0 + 0.5);
    vid_pq_to_sdr[i] = (uint16_t)(v < 0 ? 0 : (v > 1023 ? 1023 : v));
  }

  for (i = 0; i < 1024; i++)
  {
    double lin  = VID_SRGBToLinear((double)i / 1023.0) * vid_paper_white_nits;
    int    v    = (int)(VID_PQEncode(lin) * 1023.0 + 0.5);
    vid_sdr_to_pq[i] = (uint16_t)(v < 0 ? 0 : (v > 1023 ? 1023 : v));
  }

  /* Emissive boost: take a PQ code and return the code for the same colour
   * at VID_EmitScale() times the luminance, rolled off toward the peak.
   * Working on the encoded value keeps this a single table lookup per
   * entry, so making a colour table emissive costs one pass over its 256
   * entries rather than a second palette. */
  {
    double k = VID_EmitScale();
    for (i = 0; i < 1024; i++)
    {
      double nits = VID_RollOff(VID_PQDecode((double)i / 1023.0) * k);
      int    v    = (int)(VID_PQEncode(nits) * 1023.0 + 0.5);
      vid_pq_boost[i] = (uint16_t)(v < 0 ? 0 : (v > 1023 ? 1023 : v));
    }
  }
}
