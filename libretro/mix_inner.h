/* Shared per-sample mix body. Included once per output format by the
 * mix_chunk_* functions in libretro_sound.c. The caller defines:
 *   MIX_ACC               accumulator type (int / float)
 *   MIX_SEED0 / MIX_SEED1 seed L/R from the music already in the buffer
 *   MIX_ADD(acc, s, v)    add channel sample s at volume v (0..127)
 *   MIX_STORE0/1(a)       clamp + store L/R
 *   MIX_ADVANCE           advance the output cursor one frame
 * Channel interpolation and stepping are identical across formats, so they
 * live here and are single-sourced; only the seed/add/store differ.
 *
 * NOTE: this file is intentionally header-guard-free -- it is a textual body
 * meant to be #included multiple times with different macro definitions. */
{
   MIX_ACC dl = MIX_SEED0;
   MIX_ACC dr = MIX_SEED1;
   int     j;
   for (j = 0; j < n_active; j++)
   {
      channel_t *c = active[j];
      int        a = c->cur[0];
      int        s = a + ((((int)c->cur[1] - a) * (int)(c->frac >> 8)) >> 8);
      c->frac += c->step_fx;
      c->cur  += c->frac >> 16;
      c->frac &= 0xffff;
      dl = MIX_ADD(dl, s, c->leftvol);
      dr = MIX_ADD(dr, s, c->rightvol);
   }
   MIX_STORE0(dl);
   MIX_STORE1(dr);
   MIX_ADVANCE;
}
