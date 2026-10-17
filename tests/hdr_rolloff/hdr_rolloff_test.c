/* Regression lane: HDR10 highlight rolloff and automatic emissive boost
 * (src/vid_mode.c).
 *
 *  - content up to paper white encodes identically with and without a
 *    known peak (vid_sdr_to_pq and the palette encode are untouched),
 *  - with a peak, the emissive boost table is monotonic and never exceeds
 *    the PQ code of the peak,
 *  - an unknown peak, or one at or below paper white, leaves the boost
 *    table exactly as the plain multiply builds it,
 *  - auto puts a paper-white emissive texel at 95% of the peak.
 *
 *   ./hdr_rolloff_test
 */
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "vid_mode.h"

static int failures = 0;

static void check(int ok, const char *what)
{
   if (!ok)
   {
      printf("FAIL: %s\n", what);
      failures++;
   }
}

static int pq_code(double nits)
{
   int v = (int)(VID_PQEncode(nits) * 1023.0 + 0.5);
   return v < 0 ? 0 : (v > 1023 ? 1023 : v);
}

/* The boost table as a plain multiply with no rolloff. */
static void plain_boost(uint16_t *out, double k)
{
   int i;
   for (i = 0; i < 1024; i++)
      out[i] = (uint16_t)pq_code(VID_PQDecode((double)i / 1023.0) * k);
}

static int palette_matches(void)
{
   int c, r1, g1, b1, r2, g2, b2;
   float keep = vid_max_nits;
   for (c = 0; c < 256; c += 3)
   {
      double r = c / 255.0, g = (255 - c) / 255.0, b = (c * 7 % 256) / 255.0;
      vid_max_nits = 0.0f;
      VID_EncodeHDR10(r, g, b, 1.0, &r1, &g1, &b1);
      vid_max_nits = keep;
      VID_EncodeHDR10(r, g, b, 1.0, &r2, &g2, &b2);
      if (r1 != r2 || g1 != g2 || b1 != b2)
         return 0;
   }
   return 1;
}

int main(void)
{
   static const float  pws[3]   = { 100.0f, 200.0f, 300.0f };
   static const float  peaks[3] = { 400.0f, 1000.0f, 4000.0f };
   static const int    classes[4] = { VID_EMIT_2X, VID_EMIT_4X,
                                      VID_EMIT_8X, VID_EMIT_AUTO };
   uint16_t sdr_ref[1024], plain[1024];
   int a, b, c, i;

   vid_mode         = VID_MODEHDR10;
   vid_expand_gamut = VID_GAMUT_ACCURATE;

   for (a = 0; a < 3; a++)
   {
      vid_paper_white_nits = pws[a];

      /* Unknown peak: identity rolloff, 2x for auto. */
      vid_max_nits   = 0.0f;
      vid_emit_class = VID_EMIT_8X;
      VID_BuildHDRTables();
      memcpy(sdr_ref, vid_sdr_to_pq, sizeof(sdr_ref));
      plain_boost(plain, 8.0);
      check(!memcmp(plain, vid_pq_boost, sizeof(plain)),
            "unknown peak changes the boost table");
      vid_emit_class = VID_EMIT_AUTO;
      check(VID_EmitScale() == 2.0, "auto without a peak is not 2x");

      /* Peak at paper white: no headroom, identity. */
      vid_max_nits   = vid_paper_white_nits;
      vid_emit_class = VID_EMIT_4X;
      VID_BuildHDRTables();
      plain_boost(plain, 4.0);
      check(!memcmp(plain, vid_pq_boost, sizeof(plain)),
            "peak at paper white changes the boost table");

      for (b = 0; b < 3; b++)
      {
         int top;
         vid_max_nits = peaks[b];
         top          = pq_code(vid_max_nits);

         check(palette_matches(), "known peak changes the palette encode");

         for (c = 0; c < 4; c++)
         {
            vid_emit_class = classes[c];
            VID_BuildHDRTables();
            check(!memcmp(sdr_ref, vid_sdr_to_pq, sizeof(sdr_ref)),
                  "known peak changes vid_sdr_to_pq");
            for (i = 1; i < 1024; i++)
               if (vid_pq_boost[i] < vid_pq_boost[i - 1])
                  break;
            check(i == 1024, "boost table is not monotonic");
            for (i = 0; i < 1024; i++)
               if (vid_pq_boost[i] > top)
                  break;
            check(i == 1024, "boost exceeds the peak");
         }

         /* Auto: a paper-white texel lands at 95% of the peak. */
         vid_emit_class = VID_EMIT_AUTO;
         {
            double k   = VID_EmitScale();
            double out = VID_RollOff(vid_paper_white_nits * k);
            if (k > 1.0 && k < 16.0)
               check(fabs(out - 0.95 * vid_max_nits) < 0.5,
                     "auto does not land at 95% of the peak");
            printf("paper white %4.0f, peak %4.0f: auto %.2fx -> %.0f "
                   "nits\n", vid_paper_white_nits, vid_max_nits, k, out);
         }
      }
   }

   if (!failures)
      printf("PASS\n");
   return failures ? 1 : 0;
}
