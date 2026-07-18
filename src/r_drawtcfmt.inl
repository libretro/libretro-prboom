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
 *      Per-format truecolor blend kernels: everything that reads AND
 *      writes framebuffer pixels (translucency, additive, lerp, fuzz,
 *      water volume shading, dynamic-light tints, view tint, flat
 *      averaging).  Included twice by r_drawtc.c -- once for XRGB8888,
 *      once for XRGB2101010 -- with compile-time channel constants so
 *      the SIMD paths fold their masks and shifts.
 *
 *      Required macros per instantiation:
 *        RDF(name)     token-pasting name mangler (name##8888 / name##A2)
 *        RDF_RSHIFT    red field offset   (16 / 20)
 *        RDF_GSHIFT    green field offset ( 8 / 10)
 *        RDF_CMAX      channel maximum    (255 / 1023)
 *        RDF_M5050     50/50 blend mask: all channel bits except each
 *                      field's lowest (0x00FEFEFE / 0x3FEFFBFE)
 *        RDF_UP53      shift widening a 5-bit 565 unit to this channel (3 / 5)
 *        RDF_UP62      shift widening a 6-bit 565 unit to this channel (2 / 4)
 *
 *      The scalar functions are the bit-exact references; every vector
 *      kernel reproduces them lane-for-lane (the same discipline the
 *      RGB565 kernels in r_draw.c follow).
 *
 *-----------------------------------------------------------------------------*/

/* ---- scalar blend references --------------------------------------------- */

static INLINE uint32_t RDF(tc_blend5050_)(uint32_t s, uint32_t d)
{
  return (((s & RDF_M5050) >> 1) + ((d & RDF_M5050) >> 1));
}

/* dst + (src-dst)*a/32 per channel, a in 0..32 (alpha glass).  Convex, so
 * the result never leaves [0, RDF_CMAX] and needs no clamp. */
static INLINE uint32_t RDF(tc_lerpa_)(uint32_t s, uint32_t d, int a)
{
  int dr = (int)((d >> RDF_RSHIFT) & RDF_CMAX);
  int dg = (int)((d >> RDF_GSHIFT) & RDF_CMAX);
  int db = (int)( d                & RDF_CMAX);
  int r  = dr + ((((int)((s >> RDF_RSHIFT) & RDF_CMAX) - dr) * a) >> 5);
  int g  = dg + ((((int)((s >> RDF_GSHIFT) & RDF_CMAX) - dg) * a) >> 5);
  int b  = db + ((((int)( s                & RDF_CMAX) - db) * a) >> 5);
  return ((uint32_t)r << RDF_RSHIFT) | ((uint32_t)g << RDF_GSHIFT) | (uint32_t)b;
}

/* dst += src*a/32 per channel, saturating (additive light beam). */
static INLINE uint32_t RDF(tc_adda_)(uint32_t s, uint32_t d, int a)
{
  int r = (int)((d >> RDF_RSHIFT) & RDF_CMAX) + ((((int)((s >> RDF_RSHIFT) & RDF_CMAX)) * a) >> 5);
  int g = (int)((d >> RDF_GSHIFT) & RDF_CMAX) + ((((int)((s >> RDF_GSHIFT) & RDF_CMAX)) * a) >> 5);
  int b = (int)( d                & RDF_CMAX) + ((((int)( s                & RDF_CMAX)) * a) >> 5);
  if (r > RDF_CMAX) r = RDF_CMAX;
  if (g > RDF_CMAX) g = RDF_CMAX;
  if (b > RDF_CMAX) b = RDF_CMAX;
  return ((uint32_t)r << RDF_RSHIFT) | ((uint32_t)g << RDF_GSHIFT) | (uint32_t)b;
}

/* (c*15 + o)/16 per channel -- the fuzz darken toward o (o = 0: 94% keep). */
static INLINE uint32_t RDF(tc_blend9406_)(uint32_t c, uint32_t o)
{
  int r = ((int)((c >> RDF_RSHIFT) & RDF_CMAX) * 15 + (int)((o >> RDF_RSHIFT) & RDF_CMAX)) >> 4;
  int g = ((int)((c >> RDF_GSHIFT) & RDF_CMAX) * 15 + (int)((o >> RDF_GSHIFT) & RDF_CMAX)) >> 4;
  int b = ((int)( c                & RDF_CMAX) * 15 + (int)( o                & RDF_CMAX)) >> 4;
  return ((uint32_t)r << RDF_RSHIFT) | ((uint32_t)g << RDF_GSHIFT) | (uint32_t)b;
}

/* ---- vector helpers -------------------------------------------------------
 * Channel-planar arithmetic on four 32-bit pixels per vector.  A channel is
 * isolated into the LOW 16 bits of each 32-bit lane (value <= 1023, so it
 * fits with the high half zero); channel * alpha (<= 1023*32 = 32736) then
 * fits int16, letting mullo_epi16 produce the exact product in the lane's
 * low half.  For the signed LERP diff, the low half carries the two's-
 * complement value and a shift-pair re-sign-extends the product. */
#if defined(WALL_RUN_SSE2)

#define RDF_XR(v) _mm_and_si128(_mm_srli_epi32(v, RDF_RSHIFT), _mm_set1_epi32(RDF_CMAX))
#define RDF_XG(v) _mm_and_si128(_mm_srli_epi32(v, RDF_GSHIFT), _mm_set1_epi32(RDF_CMAX))
#define RDF_XB(v) _mm_and_si128(v,                             _mm_set1_epi32(RDF_CMAX))
#define RDF_XPACK(r,g,b) _mm_or_si128(_mm_or_si128( \
          _mm_slli_epi32(r, RDF_RSHIFT), _mm_slli_epi32(g, RDF_GSHIFT)), b)
/* (ch_diff * a) >> 5 with sign: mullo on the low 16-bit half, then
 * sign-extend the 16-bit product back across the 32-bit lane. */
#define RDF_XMULA_S(v, va) _mm_srai_epi32(_mm_srai_epi32( \
          _mm_slli_epi32(_mm_mullo_epi16(v, va), 16), 16), 5)
/* unsigned ch * a >> 5 (ch <= 1023, a <= 32: product < 2^15, high half 0) */
#define RDF_XMULA_U(v, va) _mm_srli_epi32(_mm_mullo_epi16(v, va), 5)
/* min(v, CMAX) on 32-bit lanes (SSE2 has no pmin for epi32) */
#define RDF_XCLAMP(v) { \
          __m128i _gt = _mm_cmpgt_epi32(v, _mm_set1_epi32(RDF_CMAX)); \
          v = _mm_or_si128(_mm_and_si128(_gt, _mm_set1_epi32(RDF_CMAX)), \
                           _mm_andnot_si128(_gt, v)); }

#elif defined(WALL_RUN_NEON)

#define RDF_XR(v) vandq_u32(vshrq_n_u32(v, RDF_RSHIFT), vdupq_n_u32(RDF_CMAX))
#define RDF_XG(v) vandq_u32(vshrq_n_u32(v, RDF_GSHIFT), vdupq_n_u32(RDF_CMAX))
#define RDF_XB(v) vandq_u32(v,                          vdupq_n_u32(RDF_CMAX))
#define RDF_XPACK(r,g,b) vorrq_u32(vorrq_u32( \
          vshlq_n_u32(r, RDF_RSHIFT), vshlq_n_u32(g, RDF_GSHIFT)), b)

#endif

/* ---- translucent flushers (50/50; ALTTRANS blends twice toward dst) ------ */

static void RDF(R_FlushWholeTL)(void)
{
   int alt = (tc_temptype == COL_ALTTRANS);

   while(--tc_temp_x >= 0)
   {
      int yl           = tc_tempyl[tc_temp_x];
      uint32_t *source = &tc_tempbuf[tc_temp_x + (yl << 2)];
      uint32_t *dest   = ((uint32_t *)drawvars.int_topleft) + yl * SURFACE_SHORT_PITCH + tc_startx + tc_temp_x;
      int   count      = tc_tempyh[tc_temp_x] - yl + 1;

      while(--count >= 0)
      {
         uint32_t px = RDF(tc_blend5050_)(*source, *dest);
         if (alt)
            px = RDF(tc_blend5050_)(px, *dest);
         *dest   = px;
         source += 4;
         dest   += SURFACE_SHORT_PITCH;
      }
   }
}

static void RDF(R_FlushHTTL)(void)
{
   uint32_t *source;
   uint32_t *dest;
   int count, colnum = 0;
   int yl, yh;
   int alt = (tc_temptype == COL_ALTTRANS);

   while(colnum < 4)
   {
      yl = tc_tempyl[colnum];
      yh = tc_tempyh[colnum];

      if(yl < tc_commontop)
      {
         source = &tc_tempbuf[colnum + (yl << 2)];
         dest   = ((uint32_t *)drawvars.int_topleft) + yl * SURFACE_SHORT_PITCH + tc_startx + colnum;
         count  = tc_commontop - yl;

         while(--count >= 0)
         {
            uint32_t px = RDF(tc_blend5050_)(*source, *dest);
            if (alt)
               px = RDF(tc_blend5050_)(px, *dest);
            *dest   = px;
            source += 4;
            dest   += SURFACE_SHORT_PITCH;
         }
      }

      if(yh > tc_commonbot)
      {
         source = &tc_tempbuf[colnum + ((tc_commonbot + 1) << 2)];
         dest   = ((uint32_t *)drawvars.int_topleft) + (tc_commonbot + 1) * SURFACE_SHORT_PITCH + tc_startx + colnum;
         count  = yh - tc_commonbot;

         while(--count >= 0)
         {
            uint32_t px = RDF(tc_blend5050_)(*source, *dest);
            if (alt)
               px = RDF(tc_blend5050_)(px, *dest);
            *dest   = px;
            source += 4;
            dest   += SURFACE_SHORT_PITCH;
         }
      }
      ++colnum;
   }
}

static void RDF(R_FlushQuadTL)(void)
{
   uint32_t *source = &tc_tempbuf[tc_commontop << 2];
   uint32_t *dest   = ((uint32_t *)drawvars.int_topleft) + tc_commontop * SURFACE_SHORT_PITCH + tc_startx;
   int        count = tc_commonbot - tc_commontop + 1;
   int        alt   = (tc_temptype == COL_ALTTRANS);

   /* One row (4 pixels = one full vector) per step: the transpose buffer
    * row is contiguous, the packed mask/shift/add is the scalar
    * tc_blend5050_ per 32-bit lane -- bit-identical output. */
#if defined(WALL_RUN_SSE2)
   {
      const __m128i mask = _mm_set1_epi32((int)RDF_M5050);
      while (count > 0)
      {
         __m128i s  = _mm_loadu_si128((const __m128i *)source);
         __m128i d  = _mm_loadu_si128((const __m128i *)dest);
         __m128i dh = _mm_srli_epi32(_mm_and_si128(d, mask), 1);
         __m128i px = _mm_add_epi32(_mm_srli_epi32(_mm_and_si128(s, mask), 1), dh);
         if (alt)
            px = _mm_add_epi32(_mm_srli_epi32(_mm_and_si128(px, mask), 1), dh);
         _mm_storeu_si128((__m128i *)dest, px);
         source += 4;
         dest   += SURFACE_SHORT_PITCH;
         count--;
      }
   }
#elif defined(WALL_RUN_NEON)
   {
      const uint32x4_t mask = vdupq_n_u32(RDF_M5050);
      while (count > 0)
      {
         uint32x4_t s  = vld1q_u32(source);
         uint32x4_t d  = vld1q_u32(dest);
         uint32x4_t dh = vshrq_n_u32(vandq_u32(d, mask), 1);
         uint32x4_t px = vaddq_u32(vshrq_n_u32(vandq_u32(s, mask), 1), dh);
         if (alt)
            px = vaddq_u32(vshrq_n_u32(vandq_u32(px, mask), 1), dh);
         vst1q_u32(dest, px);
         source += 4;
         dest   += SURFACE_SHORT_PITCH;
         count--;
      }
   }
#endif

   while(--count >= 0)
   {
      int i;
      for (i = 0; i < 4; i++)
      {
         uint32_t px = RDF(tc_blend5050_)(source[i], dest[i]);
         if (alt)
            px = RDF(tc_blend5050_)(px, dest[i]);
         dest[i] = px;
      }
      source += 4;
      dest += SURFACE_SHORT_PITCH;
   }
}

/* ---- additive flushers (dst += src*a/32, saturating) --------------------- */

static void RDF(R_FlushWholeADD)(void)
{
   while(--tc_temp_x >= 0)
   {
      int yl           = tc_tempyl[tc_temp_x];
      uint32_t *source = &tc_tempbuf[tc_temp_x + (yl << 2)];
      uint32_t *dest   = ((uint32_t *)drawvars.int_topleft) + yl * SURFACE_SHORT_PITCH + tc_startx + tc_temp_x;
      int   count      = tc_tempyh[tc_temp_x] - yl + 1;

      while(--count >= 0)
      {
         *dest   = RDF(tc_adda_)(*source, *dest, tc_tl_alpha);
         source += 4;
         dest   += SURFACE_SHORT_PITCH;
      }
   }
}

static void RDF(R_FlushHTADD)(void)
{
   uint32_t *source;
   uint32_t *dest;
   int count, colnum = 0;
   int yl, yh;

   while(colnum < 4)
   {
      yl = tc_tempyl[colnum];
      yh = tc_tempyh[colnum];

      if(yl < tc_commontop)
      {
         source = &tc_tempbuf[colnum + (yl << 2)];
         dest   = ((uint32_t *)drawvars.int_topleft) + yl * SURFACE_SHORT_PITCH + tc_startx + colnum;
         count  = tc_commontop - yl;

         while(--count >= 0)
         {
            *dest   = RDF(tc_adda_)(*source, *dest, tc_tl_alpha);
            source += 4;
            dest   += SURFACE_SHORT_PITCH;
         }
      }

      if(yh > tc_commonbot)
      {
         source = &tc_tempbuf[colnum + ((tc_commonbot + 1) << 2)];
         dest   = ((uint32_t *)drawvars.int_topleft) + (tc_commonbot + 1) * SURFACE_SHORT_PITCH + tc_startx + colnum;
         count  = yh - tc_commonbot;

         while(--count >= 0)
         {
            *dest   = RDF(tc_adda_)(*source, *dest, tc_tl_alpha);
            source += 4;
            dest   += SURFACE_SHORT_PITCH;
         }
      }
      ++colnum;
   }
}

static void RDF(R_FlushQuadADD)(void)
{
   uint32_t *source = &tc_tempbuf[tc_commontop << 2];
   uint32_t *dest   = ((uint32_t *)drawvars.int_topleft) + tc_commontop * SURFACE_SHORT_PITCH + tc_startx;
   int        count = tc_commonbot - tc_commontop + 1;

#if defined(WALL_RUN_SSE2)
   {
      /* alpha in the low 16 bits of each 32-bit lane; the channels sit in
       * the low half too, so mullo_epi16 yields the exact product. */
      const __m128i va = _mm_set1_epi32(tc_tl_alpha);
      while (count > 0)
      {
         __m128i s = _mm_loadu_si128((const __m128i *)source);
         __m128i d = _mm_loadu_si128((const __m128i *)dest);
         __m128i r = _mm_add_epi32(RDF_XR(d), RDF_XMULA_U(RDF_XR(s), va));
         __m128i g = _mm_add_epi32(RDF_XG(d), RDF_XMULA_U(RDF_XG(s), va));
         __m128i b = _mm_add_epi32(RDF_XB(d), RDF_XMULA_U(RDF_XB(s), va));
         RDF_XCLAMP(r) RDF_XCLAMP(g) RDF_XCLAMP(b)
         _mm_storeu_si128((__m128i *)dest, RDF_XPACK(r, g, b));
         source += 4;
         dest   += SURFACE_SHORT_PITCH;
         count--;
      }
   }
#elif defined(WALL_RUN_NEON)
   {
      const uint32x4_t va = vdupq_n_u32((uint32_t)tc_tl_alpha);
      const uint32x4_t cm = vdupq_n_u32(RDF_CMAX);
      while (count > 0)
      {
         uint32x4_t s = vld1q_u32(source);
         uint32x4_t d = vld1q_u32(dest);
         uint32x4_t r = vminq_u32(vaddq_u32(RDF_XR(d), vshrq_n_u32(vmulq_u32(RDF_XR(s), va), 5)), cm);
         uint32x4_t g = vminq_u32(vaddq_u32(RDF_XG(d), vshrq_n_u32(vmulq_u32(RDF_XG(s), va), 5)), cm);
         uint32x4_t b = vminq_u32(vaddq_u32(RDF_XB(d), vshrq_n_u32(vmulq_u32(RDF_XB(s), va), 5)), cm);
         vst1q_u32(dest, RDF_XPACK(r, g, b));
         source += 4;
         dest   += SURFACE_SHORT_PITCH;
         count--;
      }
   }
#endif

   while(--count >= 0)
   {
      int i;
      for (i = 0; i < 4; i++)
         dest[i] = RDF(tc_adda_)(source[i], dest[i], tc_tl_alpha);
      source += 4;
      dest += SURFACE_SHORT_PITCH;
   }
}

/* ---- lerp flushers (dst + (src-dst)*a/32, alpha glass) ------------------- */

static void RDF(R_FlushWholeLERP)(void)
{
   while(--tc_temp_x >= 0)
   {
      int yl           = tc_tempyl[tc_temp_x];
      uint32_t *source = &tc_tempbuf[tc_temp_x + (yl << 2)];
      uint32_t *dest   = ((uint32_t *)drawvars.int_topleft) + yl * SURFACE_SHORT_PITCH + tc_startx + tc_temp_x;
      int   count      = tc_tempyh[tc_temp_x] - yl + 1;

      while(--count >= 0)
      {
         *dest   = RDF(tc_lerpa_)(*source, *dest, tc_tl_alpha);
         source += 4;
         dest   += SURFACE_SHORT_PITCH;
      }
   }
}

static void RDF(R_FlushHTLERP)(void)
{
   uint32_t *source;
   uint32_t *dest;
   int count, colnum = 0;
   int yl, yh;

   while(colnum < 4)
   {
      yl = tc_tempyl[colnum];
      yh = tc_tempyh[colnum];

      if(yl < tc_commontop)
      {
         source = &tc_tempbuf[colnum + (yl << 2)];
         dest   = ((uint32_t *)drawvars.int_topleft) + yl * SURFACE_SHORT_PITCH + tc_startx + colnum;
         count  = tc_commontop - yl;

         while(--count >= 0)
         {
            *dest   = RDF(tc_lerpa_)(*source, *dest, tc_tl_alpha);
            source += 4;
            dest   += SURFACE_SHORT_PITCH;
         }
      }

      if(yh > tc_commonbot)
      {
         source = &tc_tempbuf[colnum + ((tc_commonbot + 1) << 2)];
         dest   = ((uint32_t *)drawvars.int_topleft) + (tc_commonbot + 1) * SURFACE_SHORT_PITCH + tc_startx + colnum;
         count  = yh - tc_commonbot;

         while(--count >= 0)
         {
            *dest   = RDF(tc_lerpa_)(*source, *dest, tc_tl_alpha);
            source += 4;
            dest   += SURFACE_SHORT_PITCH;
         }
      }
      ++colnum;
   }
}

static void RDF(R_FlushQuadLERP)(void)
{
   uint32_t *source = &tc_tempbuf[tc_commontop << 2];
   uint32_t *dest   = ((uint32_t *)drawvars.int_topleft) + tc_commontop * SURFACE_SHORT_PITCH + tc_startx;
   int        count = tc_commonbot - tc_commontop + 1;

#if defined(WALL_RUN_SSE2)
   {
      const __m128i va = _mm_set1_epi32(tc_tl_alpha);
      while (count > 0)
      {
         __m128i s  = _mm_loadu_si128((const __m128i *)source);
         __m128i d  = _mm_loadu_si128((const __m128i *)dest);
         __m128i dr = RDF_XR(d), dg = RDF_XG(d), db = RDF_XB(d);
         __m128i r  = _mm_add_epi32(dr, RDF_XMULA_S(_mm_sub_epi32(RDF_XR(s), dr), va));
         __m128i g  = _mm_add_epi32(dg, RDF_XMULA_S(_mm_sub_epi32(RDF_XG(s), dg), va));
         __m128i b  = _mm_add_epi32(db, RDF_XMULA_S(_mm_sub_epi32(RDF_XB(s), db), va));
         _mm_storeu_si128((__m128i *)dest, RDF_XPACK(r, g, b));
         source += 4;
         dest   += SURFACE_SHORT_PITCH;
         count--;
      }
   }
#elif defined(WALL_RUN_NEON)
   {
      const int32x4_t va = vdupq_n_s32(tc_tl_alpha);
      while (count > 0)
      {
         uint32x4_t s  = vld1q_u32(source);
         uint32x4_t d  = vld1q_u32(dest);
         int32x4_t  dr = vreinterpretq_s32_u32(RDF_XR(d));
         int32x4_t  dg = vreinterpretq_s32_u32(RDF_XG(d));
         int32x4_t  db = vreinterpretq_s32_u32(RDF_XB(d));
         int32x4_t  r  = vaddq_s32(dr, vshrq_n_s32(vmulq_s32(vsubq_s32(vreinterpretq_s32_u32(RDF_XR(s)), dr), va), 5));
         int32x4_t  g  = vaddq_s32(dg, vshrq_n_s32(vmulq_s32(vsubq_s32(vreinterpretq_s32_u32(RDF_XG(s)), dg), va), 5));
         int32x4_t  b  = vaddq_s32(db, vshrq_n_s32(vmulq_s32(vsubq_s32(vreinterpretq_s32_u32(RDF_XB(s)), db), va), 5));
         vst1q_u32(dest, RDF_XPACK(vreinterpretq_u32_s32(r),
                                   vreinterpretq_u32_s32(g),
                                   vreinterpretq_u32_s32(b)));
         source += 4;
         dest   += SURFACE_SHORT_PITCH;
         count--;
      }
   }
#endif

   while(--count >= 0)
   {
      int i;
      for (i = 0; i < 4; i++)
         dest[i] = RDF(tc_lerpa_)(source[i], dest[i], tc_tl_alpha);
      source += 4;
      dest += SURFACE_SHORT_PITCH;
   }
}

/* ---- fuzz flushers (Spectre: offset-read the framebuffer, 94% darken) ---- */

static void RDF(R_FlushWholeFuzz)(void)
{
   uint32_t *source;
   uint32_t *dest;
   int  count, yl;

   while(--tc_temp_x >= 0)
   {
      yl     = tc_tempyl[tc_temp_x];
      source = &tc_tempbuf[tc_temp_x + (yl << 2)];
      dest   = ((uint32_t *)drawvars.int_topleft) + yl * SURFACE_SHORT_PITCH + tc_startx + tc_temp_x;
      count  = tc_tempyh[tc_temp_x] - yl + 1;

      while(--count >= 0)
      {
         *dest = RDF(tc_blend9406_)(dest[tc_fuzzoffset[tc_fuzzpos]], 0);

         if(++tc_fuzzpos == TC_FUZZTABLE)
            tc_fuzzpos = 0;

         source += 4;
         dest += SURFACE_SHORT_PITCH;
      }
   }
}

static void RDF(R_FlushHTFuzz)(void)
{
   uint32_t *source;
   uint32_t *dest;
   int count, colnum = 0;
   int yl, yh;

   while(colnum < 4)
   {
      yl = tc_tempyl[colnum];
      yh = tc_tempyh[colnum];

      if(yl < tc_commontop)
      {
         source = &tc_tempbuf[colnum + (yl << 2)];
         dest   = ((uint32_t *)drawvars.int_topleft) + yl * SURFACE_SHORT_PITCH + tc_startx + colnum;
         count  = tc_commontop - yl;

         while(--count >= 0)
         {
            *dest = RDF(tc_blend9406_)(dest[tc_fuzzoffset[tc_fuzzpos]], 0);

            if(++tc_fuzzpos == TC_FUZZTABLE)
               tc_fuzzpos = 0;

            source += 4;
            dest += SURFACE_SHORT_PITCH;
         }
      }

      if(yh > tc_commonbot)
      {
         source = &tc_tempbuf[colnum + ((tc_commonbot + 1) << 2)];
         dest   = ((uint32_t *)drawvars.int_topleft) + (tc_commonbot + 1) * SURFACE_SHORT_PITCH + tc_startx + colnum;
         count  = yh - tc_commonbot;

         while(--count >= 0)
         {
            *dest = RDF(tc_blend9406_)(dest[tc_fuzzoffset[tc_fuzzpos]], 0);

            if(++tc_fuzzpos == TC_FUZZTABLE)
               tc_fuzzpos = 0;

            source += 4;
            dest += SURFACE_SHORT_PITCH;
         }
      }
      ++colnum;
   }
}

static void RDF(R_FlushQuadFuzz)(void)
{
   uint32_t *source = &tc_tempbuf[tc_commontop << 2];
   uint32_t *dest   = ((uint32_t *)drawvars.int_topleft) + tc_commontop * SURFACE_SHORT_PITCH + tc_startx;
   int fuzz1        = tc_fuzzpos;
   int fuzz2        = (fuzz1 + tc_tempyl[1]) % TC_FUZZTABLE;
   int fuzz3        = (fuzz2 + tc_tempyl[2]) % TC_FUZZTABLE;
   int fuzz4        = (fuzz3 + tc_tempyl[3]) % TC_FUZZTABLE;
   int count        = tc_commonbot - tc_commontop + 1;

   while(--count >= 0)
   {
      dest[0] = RDF(tc_blend9406_)(dest[0 + tc_fuzzoffset[fuzz1]], 0);
      dest[1] = RDF(tc_blend9406_)(dest[1 + tc_fuzzoffset[fuzz2]], 0);
      dest[2] = RDF(tc_blend9406_)(dest[2 + tc_fuzzoffset[fuzz3]], 0);
      dest[3] = RDF(tc_blend9406_)(dest[3 + tc_fuzzoffset[fuzz4]], 0);
      fuzz1 = (fuzz1 + 1) % TC_FUZZTABLE;
      fuzz2 = (fuzz2 + 1) % TC_FUZZTABLE;
      fuzz3 = (fuzz3 + 1) % TC_FUZZTABLE;
      fuzz4 = (fuzz4 + 1) % TC_FUZZTABLE;
      source += 4 * sizeof(uint8_t);
      dest += SURFACE_SHORT_PITCH * sizeof(uint8_t);
   }
}

/* ---- translucent 3D-floor span (50/50 against the framebuffer) ----------- */

static void RDF(R_DrawSpanTL)(draw_span_vars_t *dsvars)
{
   unsigned count = dsvars->x2 - dsvars->x1 + 1;
   fixed_t xfrac = dsvars->xfrac;
   fixed_t yfrac = dsvars->yfrac;
   const fixed_t xstep = dsvars->xstep;
   const fixed_t ystep = dsvars->ystep;
   const uint8_t *source = dsvars->source;
   uint32_t *dest = ((uint32_t *)drawvars.int_topleft) + dsvars->y * SCREENWIDTH + dsvars->x1;
   const uint32_t *lut = R_GetComposedColormapTC(dsvars->colormap);

#if defined(WALL_RUN_SSE2)
   if (count >= 8)
   {
      unsigned blocks = count >> 2;
      __m128i vx  = _mm_set_epi32(xfrac + 3*xstep, xfrac + 2*xstep,
                                  xfrac + xstep,   xfrac);
      __m128i vy  = _mm_set_epi32(yfrac + 3*ystep, yfrac + 2*ystep,
                                  yfrac + ystep,   yfrac);
      const __m128i vxs   = _mm_set1_epi32(xstep << 2);
      const __m128i vys   = _mm_set1_epi32(ystep << 2);
      const __m128i m63   = _mm_set1_epi32(63);
      const __m128i m4032 = _mm_set1_epi32(4032);
      const __m128i mlow  = _mm_set1_epi32((int)RDF_M5050);
      unsigned consumed   = blocks << 2;

      while (blocks--)
      {
         uint32_t idx[4];
         uint32_t s4[4];
         __m128i xt   = _mm_and_si128(_mm_srai_epi32(vx, 16), m63);
         __m128i yt   = _mm_and_si128(_mm_srai_epi32(vy, 10), m4032);
         __m128i spot = _mm_or_si128(xt, yt);
         __m128i src4, dst4, blended;

         _mm_storeu_si128((__m128i *)idx, spot);

         s4[0] = lut[source[idx[0]]];
         s4[1] = lut[source[idx[1]]];
         s4[2] = lut[source[idx[2]]];
         s4[3] = lut[source[idx[3]]];
         src4 = _mm_loadu_si128((const __m128i *)s4);
         dst4 = _mm_loadu_si128((const __m128i *)dest);
         blended = _mm_add_epi32(
                      _mm_srli_epi32(_mm_and_si128(src4, mlow), 1),
                      _mm_srli_epi32(_mm_and_si128(dst4, mlow), 1));
         _mm_storeu_si128((__m128i *)dest, blended);
         dest += 4;

         vx = _mm_add_epi32(vx, vxs);
         vy = _mm_add_epi32(vy, vys);
      }

      xfrac += (fixed_t)consumed * xstep;
      yfrac += (fixed_t)consumed * ystep;
      count -= consumed;
   }
#elif defined(WALL_RUN_NEON)
   if (count >= 8)
   {
      unsigned blocks = count >> 2;
      const int32_t xbase[4] = { xfrac, xfrac + xstep,
                                 xfrac + 2*xstep, xfrac + 3*xstep };
      const int32_t ybase[4] = { yfrac, yfrac + ystep,
                                 yfrac + 2*ystep, yfrac + 3*ystep };
      int32x4_t vx = vld1q_s32(xbase);
      int32x4_t vy = vld1q_s32(ybase);
      const int32x4_t vxs   = vdupq_n_s32(xstep << 2);
      const int32x4_t vys   = vdupq_n_s32(ystep << 2);
      const int32x4_t m63   = vdupq_n_s32(63);
      const int32x4_t m4032 = vdupq_n_s32(4032);
      const uint32x4_t mlow = vdupq_n_u32(RDF_M5050);
      unsigned consumed     = blocks << 2;

      while (blocks--)
      {
         uint32_t idx[4];
         uint32_t s4[4];
         uint32x4_t src4, dst4, blended;
         int32x4_t xt   = vandq_s32(vshrq_n_s32(vx, 16), m63);
         int32x4_t yt   = vandq_s32(vshrq_n_s32(vy, 10), m4032);
         int32x4_t spot = vorrq_s32(xt, yt);

         vst1q_u32(idx, vreinterpretq_u32_s32(spot));

         s4[0] = lut[source[idx[0]]];
         s4[1] = lut[source[idx[1]]];
         s4[2] = lut[source[idx[2]]];
         s4[3] = lut[source[idx[3]]];
         src4 = vld1q_u32(s4);
         dst4 = vld1q_u32(dest);
         blended = vaddq_u32(vshrq_n_u32(vandq_u32(src4, mlow), 1),
                             vshrq_n_u32(vandq_u32(dst4, mlow), 1));
         vst1q_u32(dest, blended);
         dest += 4;

         vx = vaddq_s32(vx, vxs);
         vy = vaddq_s32(vy, vys);
      }

      xfrac += (fixed_t)consumed * xstep;
      yfrac += (fixed_t)consumed * ystep;
      count -= consumed;
   }
#endif

   while (count)
   {
      const fixed_t xtemp = (xfrac >> 16) & 63;
      const fixed_t ytemp = (yfrac >> 10) & 4032;
      const fixed_t spot = xtemp | ytemp;
      uint32_t s = lut[source[spot]];
      xfrac += xstep;
      yfrac += ystep;
      *dest = RDF(tc_blend5050_)(s, *dest);
      dest++;
      count--;
   }
}

/* ---- submerged-water volume shading -------------------------------------- */

/* keep is the scene fraction /32; bl is an additive blue in 565-blue units,
 * widened to this format's channel. */
static INLINE uint32_t RDF(tc_waterdarken1_)(uint32_t d, int keep, int bluelift)
{
   int nr = (((int)((d >> RDF_RSHIFT) & RDF_CMAX)) * keep) >> 5;
   int ng = (((int)((d >> RDF_GSHIFT) & RDF_CMAX)) * keep) >> 5;
   int nb = ((((int)( d               & RDF_CMAX)) * keep) >> 5) + (bluelift << RDF_UP53);
   if (nb > RDF_CMAX) nb = RDF_CMAX;
   return ((uint32_t)nr << RDF_RSHIFT) | ((uint32_t)ng << RDF_GSHIFT) | (uint32_t)nb;
}

static void RDF(R_WaterDarkenSpan)(int y, int x1, int x2, int surf_y)
{
   uint32_t *dest = ((uint32_t *)drawvars.int_topleft) + y * SURFACE_SHORT_PITCH + x1;
   int n = x2 - x1 + 1;
   int depth = y - surf_y, keep, bl;
   if (n <= 0) return;
   if (!tc_water_lut_ready) R_BuildWaterLUTTC();
   if (depth < 0) depth = 0;
   if (depth >= TC_MAXWATERDEPTH) depth = TC_MAXWATERDEPTH - 1;
   keep = tc_water_keep_lut[depth];
   bl   = tc_water_bl_lut[depth];
#if defined(WALL_RUN_SSE2)
   {
      const __m128i vkeep = _mm_set1_epi32(keep);
      const __m128i vbl   = _mm_set1_epi32(bl << RDF_UP53);
      while (n >= 4)
      {
         __m128i d = _mm_loadu_si128((const __m128i *)dest);
         __m128i r = RDF_XMULA_U(RDF_XR(d), vkeep);
         __m128i g = RDF_XMULA_U(RDF_XG(d), vkeep);
         __m128i b = _mm_add_epi32(RDF_XMULA_U(RDF_XB(d), vkeep), vbl);
         RDF_XCLAMP(b)
         _mm_storeu_si128((__m128i *)dest, RDF_XPACK(r, g, b));
         dest += 4; n -= 4;
      }
   }
#elif defined(WALL_RUN_NEON)
   {
      const uint32x4_t vkeep = vdupq_n_u32((uint32_t)keep);
      const uint32x4_t vbl   = vdupq_n_u32((uint32_t)(bl << RDF_UP53));
      const uint32x4_t cm    = vdupq_n_u32(RDF_CMAX);
      while (n >= 4)
      {
         uint32x4_t d = vld1q_u32(dest);
         uint32x4_t r = vshrq_n_u32(vmulq_u32(RDF_XR(d), vkeep), 5);
         uint32x4_t g = vshrq_n_u32(vmulq_u32(RDF_XG(d), vkeep), 5);
         uint32x4_t b = vminq_u32(vaddq_u32(vshrq_n_u32(vmulq_u32(RDF_XB(d), vkeep), 5), vbl), cm);
         vst1q_u32(dest, RDF_XPACK(r, g, b));
         dest += 4; n -= 4;
      }
   }
#endif
   while (n-- > 0)
   {
      *dest = RDF(tc_waterdarken1_)(*dest, keep, bl);
      dest++;
   }
}

/* Fade targets are the 565 reference targets rescaled to this channel
 * width, rounding to nearest so both formats land on the same colour. */
#define RDF_T5(v)  (((v) * RDF_CMAX + 15) / 31)
#define RDF_T6(v)  (((v) * RDF_CMAX + 31) / 63)

static void RDF(R_WaterSurfaceBand)(int x, int yl, int yh, int surf_line, int band_h)
{
   uint32_t *dest = ((uint32_t *)drawvars.int_topleft) + yl * SURFACE_SHORT_PITCH + x;
   int y;
   for (y = yl; y <= yh; y++, dest += SURFACE_SHORT_PITCH)
   {
      int row = y - surf_line;
      int fade, rip, b, g, r;
      uint32_t d;
      if (row < 0 || row >= band_h) continue;
      fade = 12 - (row * 12) / band_h;
      rip  = tc_water_ripple[(x + (row << 1)) & 7];
      fade += rip;
      if (fade <= 0) continue;
      d = *dest;
      r = (int)((d >> RDF_RSHIFT) & RDF_CMAX);
      g = (int)((d >> RDF_GSHIFT) & RDF_CMAX);
      b = (int)( d                & RDF_CMAX);
      b += (fade * (RDF_T5(29) - b)) >> 5;
      g += (fade * (RDF_T6(22) - g)) >> 6;
      r += (fade * (RDF_T5(10) - r)) >> 6;
      if (b > RDF_CMAX) b = RDF_CMAX;
      if (b < 0)  b = 0;
      if (g > RDF_CMAX) g = RDF_CMAX;
      if (g < 0)  g = 0;
      if (r > RDF_CMAX) r = RDF_CMAX;
      if (r < 0)  r = 0;
      *dest = ((uint32_t)r << RDF_RSHIFT) | ((uint32_t)g << RDF_GSHIFT) | (uint32_t)b;
   }
}

static void RDF(R_WaterSurfaceLift)(int x, int y0, int y1, int bandtop)
{
   uint32_t *dest = ((uint32_t *)drawvars.int_topleft) + y0 * SURFACE_SHORT_PITCH + x;
   int span = y1 - bandtop + 1;
   int y;
   if (span < 1) span = 1;
   for (y = y0; y <= y1; y++, dest += SURFACE_SHORT_PITCH)
   {
      uint32_t d = *dest;
      int r = (int)((d >> RDF_RSHIFT) & RDF_CMAX);
      int g = (int)((d >> RDF_GSHIFT) & RDF_CMAX);
      int b = (int)( d                & RDF_CMAX);
      int row  = y - bandtop;
      int fade = 14 - (row * 9) / span;
      if (fade < 4)
         fade = 4;
      b += (fade * (RDF_T5(31) - b)) >> 5;
      g += (fade * (RDF_T6(26) - g)) >> 6;
      r += (fade * (RDF_T5(8)  - r)) >> 6;
      if (b > RDF_CMAX) b = RDF_CMAX;
      if (b < 0)  b = 0;
      if (g > RDF_CMAX) g = RDF_CMAX;
      if (g < 0)  g = 0;
      if (r > RDF_CMAX) r = RDF_CMAX;
      if (r < 0)  r = 0;
      *dest = ((uint32_t)r << RDF_RSHIFT) | ((uint32_t)g << RDF_GSHIFT) | (uint32_t)b;
   }
}

static void RDF(R_WaterDarkenColumn)(int x, int yl, int yh, int surf_y)
{
   uint32_t *dest = ((uint32_t *)drawvars.int_topleft) + yl * SURFACE_SHORT_PITCH + x;
   int y, depth;
   if (!tc_water_lut_ready) R_BuildWaterLUTTC();
   for (y = yl; y <= yh; y++, dest += SURFACE_SHORT_PITCH)
   {
      depth = y - surf_y; if (depth < 0) depth = 0;
      if (depth >= TC_MAXWATERDEPTH) depth = TC_MAXWATERDEPTH - 1;
      *dest = RDF(tc_waterdarken1_)(*dest, tc_water_keep_lut[depth],
                                    tc_water_bl_lut[depth]);
   }
}

#undef RDF_T5
#undef RDF_T6

/* ---- dynamic-light tints, view tint, flat average ------------------------ */

/* Additively tint a 256-entry composed LUT toward a light's chroma; the
 * ar/ag/ab inputs are in 565 channel units (see r_dynlight.c). */
static void RDF(R_TintLUT)(uint32_t *dst, const uint32_t *src, int ar, int ag, int ab)
{
   int i;
   for (i = 0; i < 256; i++)
   {
      uint32_t px = src[i];
      int r = (int)((px >> RDF_RSHIFT) & RDF_CMAX) + (ar << RDF_UP53);
      int g = (int)((px >> RDF_GSHIFT) & RDF_CMAX) + (ag << RDF_UP62);
      int b = (int)( px               & RDF_CMAX) + (ab << RDF_UP53);
      if (r > RDF_CMAX) r = RDF_CMAX;
      if (g > RDF_CMAX) g = RDF_CMAX;
      if (b > RDF_CMAX) b = RDF_CMAX;
      dst[i] = ((uint32_t)r << RDF_RSHIFT) | ((uint32_t)g << RDF_GSHIFT) | (uint32_t)b;
   }
}

/* Per-pixel saturating tint over one vertical run (the wall-tint replay). */
static void RDF(R_WallTintRun)(int x, int yl, int yh, int ar, int ag, int ab)
{
   uint32_t *d = ((uint32_t *)drawvars.int_topleft) + yl * SURFACE_SHORT_PITCH + x;
   int n = yh - yl + 1;
   for (; n > 0; n--, d += SURFACE_SHORT_PITCH)
   {
      uint32_t px = *d;
      int r = (int)((px >> RDF_RSHIFT) & RDF_CMAX) + (ar << RDF_UP53);
      int g = (int)((px >> RDF_GSHIFT) & RDF_CMAX) + (ag << RDF_UP62);
      int b = (int)( px               & RDF_CMAX) + (ab << RDF_UP53);
      if (r > RDF_CMAX) r = RDF_CMAX;
      if (g > RDF_CMAX) g = RDF_CMAX;
      if (b > RDF_CMAX) b = RDF_CMAX;
      *d = ((uint32_t)r << RDF_RSHIFT) | ((uint32_t)g << RDF_GSHIFT) | (uint32_t)b;
   }
}

/* Horizontal saturating tint run (the plane dynamic-light chroma pass). */
static void RDF(R_TintSpan)(int y, int x1, int x2, int ar, int ag, int ab)
{
   uint32_t *d = ((uint32_t *)drawvars.int_topleft) + y * SURFACE_SHORT_PITCH + x1;
   int n = x2 - x1 + 1;
   int wr = ar << RDF_UP53, wg = ag << RDF_UP62, wb = ab << RDF_UP53;
   if (wr > RDF_CMAX) wr = RDF_CMAX;
   if (wg > RDF_CMAX) wg = RDF_CMAX;
   if (wb > RDF_CMAX) wb = RDF_CMAX;

#if defined(WALL_RUN_SSE2)
   if (n >= 4)
   {
      const __m128i vr = _mm_set1_epi32(wr);
      const __m128i vg = _mm_set1_epi32(wg);
      const __m128i vb = _mm_set1_epi32(wb);
      while (n >= 4)
      {
         __m128i px = _mm_loadu_si128((const __m128i *)d);
         __m128i r  = _mm_add_epi32(RDF_XR(px), vr);
         __m128i g  = _mm_add_epi32(RDF_XG(px), vg);
         __m128i b  = _mm_add_epi32(RDF_XB(px), vb);
         RDF_XCLAMP(r) RDF_XCLAMP(g) RDF_XCLAMP(b)
         _mm_storeu_si128((__m128i *)d, RDF_XPACK(r, g, b));
         d += 4;
         n -= 4;
      }
   }
#elif defined(WALL_RUN_NEON)
   if (n >= 4)
   {
      const uint32x4_t vr = vdupq_n_u32((uint32_t)wr);
      const uint32x4_t vg = vdupq_n_u32((uint32_t)wg);
      const uint32x4_t vb = vdupq_n_u32((uint32_t)wb);
      const uint32x4_t cm = vdupq_n_u32(RDF_CMAX);
      while (n >= 4)
      {
         uint32x4_t px = vld1q_u32(d);
         uint32x4_t r  = vminq_u32(vaddq_u32(RDF_XR(px), vr), cm);
         uint32x4_t g  = vminq_u32(vaddq_u32(RDF_XG(px), vg), cm);
         uint32x4_t b  = vminq_u32(vaddq_u32(RDF_XB(px), vb), cm);
         vst1q_u32(d, RDF_XPACK(r, g, b));
         d += 4;
         n -= 4;
      }
   }
#endif

   for (; n > 0; n--, d++)
   {
      uint32_t px = *d;
      int r = (int)((px >> RDF_RSHIFT) & RDF_CMAX) + wr;
      int g = (int)((px >> RDF_GSHIFT) & RDF_CMAX) + wg;
      int b = (int)( px               & RDF_CMAX) + wb;
      if (r > RDF_CMAX) r = RDF_CMAX;
      if (g > RDF_CMAX) g = RDF_CMAX;
      if (b > RDF_CMAX) b = RDF_CMAX;
      *d = ((uint32_t)r << RDF_RSHIFT) | ((uint32_t)g << RDF_GSHIFT) | (uint32_t)b;
   }
}

/* Blend the whole 3D view 50/50 toward a constant colour (underwater). */
static void RDF(R_TintView)(uint32_t color)
{
   uint32_t *base = (uint32_t *)drawvars.int_topleft;
   int y;
#if defined(WALL_RUN_SSE2)
   const __m128i vcol = _mm_set1_epi32((int)color);
   const __m128i mlow = _mm_set1_epi32((int)RDF_M5050);
   const __m128i vch  = _mm_srli_epi32(_mm_and_si128(vcol, mlow), 1);
#elif defined(WALL_RUN_NEON)
   const uint32x4_t vcol = vdupq_n_u32(color);
   const uint32x4_t mlow = vdupq_n_u32(RDF_M5050);
   const uint32x4_t vch  = vshrq_n_u32(vandq_u32(vcol, mlow), 1);
#endif

   for (y = 0; y < viewheight; y++)
   {
      uint32_t *row = base + y * SCREENWIDTH;
      int x = 0;
#if defined(WALL_RUN_SSE2)
      for (; x + 4 <= viewwidth; x += 4)
      {
         __m128i px = _mm_loadu_si128((const __m128i *)(row + x));
         __m128i bl = _mm_add_epi32(vch, _mm_srli_epi32(_mm_and_si128(px, mlow), 1));
         _mm_storeu_si128((__m128i *)(row + x), bl);
      }
#elif defined(WALL_RUN_NEON)
      for (; x + 4 <= viewwidth; x += 4)
      {
         uint32x4_t px = vld1q_u32(row + x);
         uint32x4_t bl = vaddq_u32(vch, vshrq_n_u32(vandq_u32(px, mlow), 1));
         vst1q_u32(row + x, bl);
      }
#endif
      for (; x < viewwidth; x++)
         row[x] = RDF(tc_blend5050_)(color, row[x]);
   }
}

/* Mean colour of a flat in this format, cached on (picnum, palette). */
static uint32_t RDF(R_FlatAverageColor)(int picnum)
{
   static int             lastpic = -1;
   static const uint32_t *lastpal = NULL;
   static uint32_t        lastcol = 0;
   const uint8_t   *flat;
   unsigned long    sr = 0, sg = 0, sb = 0, n = 0;
   int              i, synthetic;
   int              lump = firstflat + flattranslation[picnum];

   if (picnum == lastpic && V_PaletteTC == lastpal)
      return lastcol;

   synthetic = R_IsSyntheticFlat(picnum);
   flat = synthetic ? R_GetSyntheticFlat(picnum)
                    : (const uint8_t *)W_CacheLumpNum(lump);

   for (i = 0; i < 4096; i += 7)
   {
      uint32_t c = VID_PALTC(flat[i], VID_NUMCOLORWEIGHTS - 1);
      sr += (c >> RDF_RSHIFT) & RDF_CMAX;
      sg += (c >> RDF_GSHIFT) & RDF_CMAX;
      sb +=  c                & RDF_CMAX;
      n++;
   }

   if (!synthetic)
      W_UnlockLumpNum(lump);

   if (!n)
      n = 1;
   lastcol = ((uint32_t)(sr / n) << RDF_RSHIFT) |
             ((uint32_t)(sg / n) << RDF_GSHIFT) |
              (uint32_t)(sb / n);
   lastpic = picnum;
   lastpal = V_PaletteTC;
   return lastcol;
}

#if defined(WALL_RUN_SSE2) || defined(WALL_RUN_NEON)
#undef RDF_XR
#undef RDF_XG
#undef RDF_XB
#undef RDF_XPACK
#ifdef RDF_XMULA_S
#undef RDF_XMULA_S
#endif
#ifdef RDF_XMULA_U
#undef RDF_XMULA_U
#endif
#ifdef RDF_XCLAMP
#undef RDF_XCLAMP
#endif
#endif
