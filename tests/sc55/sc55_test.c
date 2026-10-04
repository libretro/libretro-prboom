/* Golden lane for src/sc55.c.
 *
 * The emulator is a port of the Nuked-SC55 backend and was checked
 * against that backend frame for frame.  This lane holds it there
 * without needing the original or any Roland ROM: both models are run
 * on pseudo-random ROM images with pseudo-random MIDI bytes and PCM
 * register writes, and the audio and the machine state have to hash to
 * the values recorded from the build that matched.
 *
 * The Makefile compiles sc55.c as strict C89 with warnings as errors,
 * which is the other half of the lane.
 *
 *   ./sc55_test          check
 *   ./sc55_test print    print the hashes instead
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sc55.h"

uint32_t sc55_test_hash(const sc55_t *s);
void     sc55_test_pcm_write(sc55_t *s, uint32_t address, uint8_t data);

#define FRAMES 60000

static unsigned long rs;

static unsigned rnd(void)
{
   rs = (rs * 1664525UL + 1013904223UL) & 0xffffffffUL;
   return (unsigned)(rs >> 8);
}

static unsigned char *image(size_t len)
{
   unsigned char *p = (unsigned char*)malloc(len);
   size_t i;
   if (!p)
      exit(2);
   for (i = 0; i < len; i++)
      p[i] = (unsigned char)rnd();
   return p;
}

static void load(sc55_t *s, int slot, size_t len)
{
   unsigned char *p = image(len);
   if (!sc55_load_rom(s, slot, p, len))
   {
      printf("FAIL: slot %d refused a %lu byte image\n", slot, (unsigned long)len);
      exit(1);
   }
   free(p);
}

static void run(int model, unsigned long seed, unsigned long *audio, unsigned long *state)
{
   static int32_t out[2 * 1024];
   unsigned long h = 2166136261UL;
   long   done = 0;
   size_t chunk = 1;
   sc55_t *s;
   int i;

   rs = seed;
   s  = sc55_new(model);
   if (!s)
      exit(2);
   load(s, SC55_ROM_ROM1, 0x8000);
   load(s, SC55_ROM_ROM2, 0x80000);
   if (model == SC55_MODEL_MK2)
      load(s, SC55_ROM_SMROM, 0x1000);
   load(s, SC55_ROM_WAVEROM1, model == SC55_MODEL_MK2 ? 0x200000 : 0x100000);
   load(s, SC55_ROM_WAVEROM2, model == SC55_MODEL_MK2 ? 0x200000 : 0x100000);
   if (model == SC55_MODEL_MK1)
      load(s, SC55_ROM_WAVEROM3, 0x100000);
   sc55_reset(s);

   while (done < FRAMES)
   {
      size_t n, k;
      for (i = 0; i < 8; i++)
         sc55_test_pcm_write(s, rnd() & 0x3f, (unsigned char)rnd());
      if ((rnd() & 3) == 0)
      {
         unsigned char b = (unsigned char)rnd();
         sc55_midi(s, &b, 1);
      }
      /* Odd, changing sizes, so frames a step leaves over are carried. */
      chunk = chunk * 7 % 1021 + 1;
      n = sc55_run(s, out, chunk);
      for (k = 0; k < n * 2; k++)
         h = ((h ^ (unsigned long)(unsigned)out[k]) * 16777619UL) & 0xffffffffUL;
      done += (long)n;
   }
   *audio = h;
   *state = (unsigned long)sc55_test_hash(s);
   sc55_free(s);
}

int main(int argc, char **argv)
{
   static const struct { int model; unsigned long seed, audio, state; } want[] =
   {
      { SC55_MODEL_MK2, 0x1234UL, 0xab968ca5UL, 0xd9ccd413UL },
      { SC55_MODEL_MK1, 0x5678UL, 0xbdbc4ca5UL, 0x4b618e1bUL }
   };
   int print = argc > 1 && !strcmp(argv[1], "print");
   int bad = 0;
   size_t i;

   for (i = 0; i < sizeof(want) / sizeof(want[0]); i++)
   {
      unsigned long audio, state;
      run(want[i].model, want[i].seed, &audio, &state);
      if (print)
         printf("%08lx %08lx\n", audio, state);
      else if (audio != want[i].audio || state != want[i].state)
      {
         printf("FAIL: model %d audio %08lx (want %08lx) state %08lx (want %08lx)\n",
               want[i].model, audio, want[i].audio, state, want[i].state);
         bad = 1;
      }
   }
   if (print)
      return 0;
   if (!bad)
      printf("PASS\n");
   return bad;
}
