/* Roland Sound Canvas music player: SC-55 (sc55.c) and SC-88 family
 * (sc88.c).  One of the two is in use at a time; sp_unit says which.
 *
 * The emulators run the units' own firmware, so unlike the OPL player
 * this one has a machine to boot and a real cost per sample.  The emulator
 * therefore lives on a worker thread.  The caller's thread keeps the
 * sequencer: it stamps each MIDI event with the output frame it belongs
 * to and queues it, tells the worker how far it may run, and takes
 * finished audio back.  Both queues are single-producer single-consumer
 * rings on retro_atomic indices; nothing here takes a lock, and every
 * wait is an eventcount wait.
 *
 * The worker runs SC55_LEAD frames ahead of what has been played, which
 * is how late the music is against the sequencer.  Until the firmware
 * is taking input (see sp_power_on) the sequencer does not start, so
 * the first song begins late instead of losing its opening.
 *
 * The chip's frames stay in float from the emulator to the mixer.  The
 * 16-bit path quantizes once, at the very end, with rounding and TPDF
 * dither; upstream's 16-bit path was a bare arithmetic shift, which
 * truncates toward minus infinity.
 *
 * Without HAVE_THREADS the same code runs inline from render.
 *
 * ROM files are found by content: see sp_find_unit.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include <streams/file_stream.h>
#include <file/file_path.h>
#include <retro_dirent.h>

#if defined(HAVE_THREADS) && !defined(SC55_NO_THREAD)
#include <rthreads/rthreads.h>
#include <rthreads/retro_eventcount.h>
#include <retro_atomic.h>
#if defined(RETRO_ATOMIC_LOCK_FREE)
#define SC55_THREADED 1
#endif
#endif

/* The game's headers come last: m_swap.h defines LONG() and SHORT(),
 * which the Windows headers the ones above pull in use as type names. */
#include "sc55player.h"
#include "sc55.h"
#include "sc88.h"
#include "midifile.h"
#include "lprintf.h"
#include "i_system.h"
#include "g_game.h"

#ifdef SC55_THREADED
typedef retro_atomic_size_t sp_size_t;
typedef retro_atomic_int_t  sp_int_t;
#define SP_LOAD_SIZE(p)     retro_atomic_load_acquire_size(p)
#define SP_STORE_SIZE(p, v) retro_atomic_store_release_size(p, v)
#define SP_LOAD_INT(p)      retro_atomic_load_acquire_int(p)
#define SP_STORE_INT(p, v)  retro_atomic_store_release_int(p, v)
#else
typedef size_t sp_size_t;
typedef int    sp_int_t;
#define SP_LOAD_SIZE(p)     (*(p))
#define SP_STORE_SIZE(p, v) (*(p) = (v))
#define SP_LOAD_INT(p)      (*(p))
#define SP_STORE_INT(p, v)  (*(p) = (v))
#endif

#define SC55_AUDIO_RING  16384               /* frames, power of two */
#define SC55_EVENT_RING  4096                /* events, power of two */
#define SC55_BLOCK       64                  /* output frames per worker pass */
#define SC55_LEAD_MS     80
#define SC55_HISTORY     32768               /* frames kept for a state load to step back over */
#define SC55_TAPS        32
#define SC55_PHASES      128
#define SC55_NATIVE_MAX  2048                /* native frames per device call */
#ifndef SC55_BOOT_CAP_S
#define SC55_BOOT_CAP_S  12                  /* give up waiting for the firmware */
#endif
#ifndef SC88_BOOT_CAP_S
#define SC88_BOOT_CAP_S  30                  /* SC-88: its factory reset alone is ~11 s */
#endif
#define SC55_SETTLE_MS   100                 /* after the firmware answers */
#define SC55_RESET_MS    100                 /* for a song's channel reset to be taken in */
#define SC55_TAIL_MS     2500                /* a song that does not loop rings out this long */

/* What the player needs of an emulator. */
typedef struct
{
   const char *tag;                 /* in messages */
   const char *thread;
   void     *(*find)(void);         /* find the ROMs and build the unit, or say what is missing */
   void      (*destroy)(void *dev);
   void      (*midi)(void *dev, const unsigned char *data, size_t len);
   uint32_t  (*voices)(const void *dev);
   unsigned  (*rate)(const void *dev);
   size_t    (*run)(void *dev, int32_t *frames, size_t count);
   float       scale;               /* 1 / full scale of its frames */
   unsigned    boot_cap_s;
} sp_unit_t;

static const sp_unit_t *sp_unit;

typedef struct
{
   size_t        time;     /* output frame the bytes belong to */
   unsigned char len;
   unsigned char data[7];
} sp_event_t;

/* ---- shared between the two sides ---------------------------------- */

static sp_event_t *ev_ring;
static sp_size_t   ev_w;             /* caller */
static sp_size_t   ev_r;             /* worker */
static float      *au_ring;
static sp_size_t   au_w;             /* worker */
static sp_size_t   au_r;             /* caller */
static sp_size_t   sp_target;        /* caller: the worker may produce up to here */
static sp_int_t    sp_epoch_req;     /* caller */
static sp_int_t    sp_epoch_ack;     /* worker */
static sp_int_t    sp_ready;         /* worker: firmware is up */
static sp_int_t    sp_quit;
#ifdef SC55_THREADED
static sthread_t         *sp_thread;
static retro_eventcount_t sp_ec_work;
static retro_eventcount_t sp_ec_ack;
static int                sp_ec_up;
#endif

/* ---- worker side ---------------------------------------------------- */

static void    *eng_dev;
static unsigned eng_native_rate;
static size_t   eng_out_pos;          /* output frames produced this epoch */
static size_t   eng_boot_frames;      /* native frames run while booting */
static size_t   eng_settle_left;      /* native frames to run once listening */
static int      eng_epoch;
static float   *eng_kernel;           /* (SC55_PHASES + 1) * SC55_TAPS */
static float   *eng_hist;             /* native frames, stereo */
static size_t   eng_hist_len;         /* frames held */
static double   eng_pos;              /* read position in eng_hist */
static double   eng_step;             /* native frames per output frame */
static int32_t *eng_native;           /* device scratch */

/* ---- caller side ---------------------------------------------------- */

static int            sp_rate;
static int            sp_volume  = 15;
static int            sp_playing;
static int            sp_paused;
static int            sp_looping;
static int            sp_tried;       /* ROMs looked for this session */
static int            sp_open_ok;
static midi_file_t   *sp_midifile;
static midi_event_t **sp_events;
static unsigned       sp_eventpos;
static double         sp_spmc;
static double         sp_next_time;   /* output frame of the next event */
static size_t         sp_consumed;    /* frames handed to the mixer this epoch */
static size_t         sp_lead;
static uint32_t       sp_dither = 0x9e3779b9u;
static int            sp_announced;   /* "ready" has been logged */
static size_t         sp_short;       /* frames the worker was late with */
static float         *sp_hist;        /* the last SC55_HISTORY frames handed out */
static uint64_t       sp_clock;       /* frames handed out for this song, as the game counts them */
static uint64_t       sp_clock_max;   /* frames ever produced for this song */
static uint32_t       sp_song_hash;   /* identifies the registered song */
static uint32_t       sp_song_id;     /* identifies this playing of it */
static uint32_t       sp_plays;
static size_t         sp_tail;        /* frames left to ring out after the last event */
static float          sp_fade = 0.0f; /* 0..1: how far the unit's audio has faded back in */
static float          sp_carry[2];    /* what was sounding when the audio stopped, dying away */
static float          sp_last[2];     /* the last frame handed out */
static float          sp_gain_now = -1.0f; /* the gain the last block ended on */

/* Queued at power-on: a GS reset, which the mkII needs once because its
 * firmware does not initialise everything by itself, then a note the
 * unit cannot be heard playing (channel 16 at volume 0).  The firmware
 * works through its MIDI input in order and is busy with the reset for
 * several seconds, so the moment that note's voice is keyed is the
 * moment the unit will act on what it is sent. */
static const unsigned char sp_power_on[] =
{
   0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7,
   0xbf, 7, 0,
   0x9f, 60, 1
};
/* Sent once the probe has answered: end it and put channel 16 back. */
static const unsigned char sp_probe_end[] =
{
   0x8f, 60, 0,
   0xbf, 120, 0, 121, 0, 7, 100
};

/* ---- resampler ------------------------------------------------------ */

static double sp_bessel_i0(double x)
{
   double sum = 1.0, term = 1.0;
   int k;
   for (k = 1; k < 32; k++)
   {
      term *= (x / (2.0 * k)) * (x / (2.0 * k));
      sum  += term;
      if (term < sum * 1e-12)
         break;
   }
   return sum;
}

/* Kaiser-windowed sinc, one row per fractional position.  The cutoff
 * follows the lower of the two rates so the 66 kHz stream is band
 * limited before it is decimated. */
static int eng_build_kernel(void)
{
   const double pi   = 3.14159265358979323846;
   const double beta = 9.0;
   double fc = (double)sp_rate / (double)eng_native_rate;
   int p, k;

   if (fc > 1.0)
      fc = 1.0;
   fc *= 0.94;

   eng_kernel = (float*)malloc((SC55_PHASES + 1) * SC55_TAPS * sizeof(float));
   if (!eng_kernel)
      return 0;
   for (p = 0; p <= SC55_PHASES; p++)
   {
      double frac = (double)p / SC55_PHASES;
      double sum  = 0.0;
      float *row  = eng_kernel + p * SC55_TAPS;
      for (k = 0; k < SC55_TAPS; k++)
      {
         double x = (double)(k - SC55_TAPS / 2 + 1) - frac;
         double w = 2.0 * x / SC55_TAPS;
         double s = (x == 0.0) ? 1.0 : sin(pi * fc * x) / (pi * fc * x);
         double v;
         w = (w * w >= 1.0) ? 0.0
           : sp_bessel_i0(beta * sqrt(1.0 - w * w)) / sp_bessel_i0(beta);
         v      = s * w;
         row[k] = (float)v;
         sum   += v;
      }
      for (k = 0; k < SC55_TAPS; k++)
         row[k] = (float)(row[k] / sum);
   }
   return 1;
}

/* Run the device for `n` native frames and append them to the history. */
static void eng_pull_native(size_t n)
{
   const float scale = sp_unit->scale;
   while (n)
   {
      size_t chunk = n < SC55_NATIVE_MAX ? n : SC55_NATIVE_MAX;
      size_t i;
      float *dst = eng_hist + eng_hist_len * 2;
      sp_unit->run(eng_dev, eng_native, chunk);
      for (i = 0; i < chunk * 2; i++)
         dst[i] = (float)eng_native[i] * scale;
      eng_hist_len += chunk;
      n            -= chunk;
   }
}

/* Produce `n` output frames (n <= SC55_BLOCK) into `out`. */
static void eng_resample(float *out, size_t n)
{
   size_t need = (size_t)(eng_pos + eng_step * (double)n) + SC55_TAPS + 1;
   size_t i, drop;

   if (need > eng_hist_len)
      eng_pull_native(need - eng_hist_len);

   for (i = 0; i < n; i++)
   {
      size_t  base = (size_t)eng_pos;
      double  frac = (eng_pos - (double)base) * SC55_PHASES;
      int     ph   = (int)frac;
      float   t    = (float)(frac - ph);
      const float *k0 = eng_kernel + ph * SC55_TAPS;
      const float *k1 = k0 + SC55_TAPS;
      const float *in = eng_hist + base * 2;
      float l = 0.0f, r = 0.0f;
      int k;
      for (k = 0; k < SC55_TAPS; k++)
      {
         float c = k0[k] + (k1[k] - k0[k]) * t;
         l += in[k * 2]     * c;
         r += in[k * 2 + 1] * c;
      }
      out[i * 2]     = l;
      out[i * 2 + 1] = r;
      eng_pos += eng_step;
   }

   /* Keep the tail the next block's taps still reach back into. */
   drop = (size_t)eng_pos;
   if (drop)
   {
      memmove(eng_hist, eng_hist + drop * 2,
            (eng_hist_len - drop) * 2 * sizeof(float));
      eng_hist_len -= drop;
      eng_pos      -= (double)drop;
   }
}

/* ---- worker --------------------------------------------------------- */

/* Run a slice of the boot; returns non-zero once the unit is usable. */
static int eng_boot_slice(size_t native_frames)
{
   if (SP_LOAD_INT(&sp_ready))
      return 1;
   while (native_frames)
   {
      size_t chunk = native_frames < SC55_NATIVE_MAX ? native_frames : SC55_NATIVE_MAX;
      sp_unit->run(eng_dev, eng_native, chunk);
      native_frames   -= chunk;
      eng_boot_frames += chunk;
      if (eng_settle_left)
      {
         if (eng_settle_left <= chunk)
         {
            SP_STORE_INT(&sp_ready, 1);
            return 1;
         }
         eng_settle_left -= chunk;
      }
      else if (sp_unit->voices(eng_dev)
            || eng_boot_frames >= (size_t)eng_native_rate * sp_unit->boot_cap_s)
      {
         sp_unit->midi(eng_dev, sp_probe_end, sizeof(sp_probe_end));
         eng_settle_left = (size_t)eng_native_rate * SC55_SETTLE_MS / 1000;
      }
   }
   return 0;
}

/* Take up a flush the caller asked for.  Returns non-zero if one was
 * taken. */
static int eng_check_epoch(void)
{
   int req = SP_LOAD_INT(&sp_epoch_req);
   if (req == eng_epoch)
      return 0;
   SP_STORE_SIZE(&ev_r, SP_LOAD_SIZE(&ev_w));
   eng_out_pos = 0;
   eng_epoch   = req;
   SP_STORE_INT(&sp_epoch_ack, req);
#ifdef SC55_THREADED
   retro_eventcount_notify(&sp_ec_ack);
#endif
   return 1;
}

/* Produce audio up to the caller's target.  Returns non-zero if there
 * was anything to do. */
static int eng_produce(void)
{
   float  block[SC55_BLOCK * 2];
   int    did = 0;

   for (;;)
   {
      size_t target = SP_LOAD_SIZE(&sp_target);
      size_t w      = SP_LOAD_SIZE(&au_w);
      size_t r      = SP_LOAD_SIZE(&au_r);
      size_t er, ew, n, i;

      if (eng_check_epoch())
      {
         did = 1;
         continue;
      }
      if (SP_LOAD_INT(&sp_quit))
         break;
      if (eng_out_pos >= target
            || SC55_AUDIO_RING - (w - r) < SC55_BLOCK)
         break;

      n = target - eng_out_pos;
      if (n > SC55_BLOCK)
         n = SC55_BLOCK;

      /* Events due now go to the unit; the next one bounds the block. */
      er = SP_LOAD_SIZE(&ev_r);
      ew = SP_LOAD_SIZE(&ev_w);
      while (er != ew)
      {
         const sp_event_t *ev = &ev_ring[er & (SC55_EVENT_RING - 1)];
         if (ev->time > eng_out_pos)
         {
            if (ev->time - eng_out_pos < n)
               n = ev->time - eng_out_pos;
            break;
         }
         sp_unit->midi(eng_dev, ev->data, ev->len);
         er++;
      }
      SP_STORE_SIZE(&ev_r, er);

      eng_resample(block, n);
      for (i = 0; i < n; i++)
      {
         float *dst = au_ring + ((w + i) & (SC55_AUDIO_RING - 1)) * 2;
         dst[0] = block[i * 2];
         dst[1] = block[i * 2 + 1];
      }
      SP_STORE_SIZE(&au_w, w + n);
      eng_out_pos += n;
      did = 1;
   }
   return did;
}

#ifdef SC55_THREADED
static void sp_worker(void *unused)
{
   (void)unused;
   sthread_setname(sp_unit->thread);
   for (;;)
   {
      int key;
      if (SP_LOAD_INT(&sp_quit))
         break;
      if (!SP_LOAD_INT(&sp_ready))
      {
         /* Boot in slices so a flush or a quit is still seen promptly. */
         eng_check_epoch();
         eng_boot_slice(SC55_NATIVE_MAX);
         continue;
      }
      if (eng_produce())
         continue;
      key = retro_eventcount_prepare_wait(&sp_ec_work);
      if (SP_LOAD_INT(&sp_quit) || eng_produce())
      {
         retro_eventcount_cancel_wait(&sp_ec_work);
         continue;
      }
      retro_eventcount_commit_wait(&sp_ec_work, key);
   }
}
#endif

static void sp_wake(void)
{
#ifdef SC55_THREADED
   retro_eventcount_notify(&sp_ec_work);
#endif
}

/* ---- device --------------------------------------------------------- */

/* ---- finding the ROMs ----------------------------------------------- */

/* ROM dumps are recognised by SHA-256, as the Nuked-SC55 fork does, so
 * it does not matter what the files are called.  The list is the
 * fork's (standard_romsets.cpp) without the JV-880. */
typedef struct
{
   int         model;
   const char *label;
   const char *rom[SC55_ROM_COUNT];   /* hex digests; NULL = not used */
} sp_romset_t;

static const sp_romset_t sp_romsets[] =
{
#ifdef SC55_TEST_ROMSET
   SC55_TEST_ROMSET
#endif
   { SC55_MODEL_MK2, "SC-55mkII",
     { "8a1eb33c7599b746c0c50283e4349a1bb1773b5c0ec0e9661219bf6c067d2042",
       "a4c9fd821059054c7e7681d61f49ce6f42ed2fe407a7ec1ba0dfdc9722582ce0",
       "b0b5f865a403f7308b4be8d0ed3ba2ed1c22db881b8a8326769dea222f6431d8",
       "c6429e21b9b3a02fbd68ef0b2053668433bee0bccd537a71841bc70b8874243b",
       "5b753f6cef4cfc7fcafe1430fecbb94a739b874e55356246a46abe24097ee491",
       NULL } },
   { SC55_MODEL_MK2, "SC-55mkII (CTF-patched)",
     { "8a1eb33c7599b746c0c50283e4349a1bb1773b5c0ec0e9661219bf6c067d2042",
       "64f8c9daf1021cf86ea4ddf03a29b81b5ea0c18e74f462833023436388bb9dc4",
       "b0b5f865a403f7308b4be8d0ed3ba2ed1c22db881b8a8326769dea222f6431d8",
       "c6429e21b9b3a02fbd68ef0b2053668433bee0bccd537a71841bc70b8874243b",
       "5b753f6cef4cfc7fcafe1430fecbb94a739b874e55356246a46abe24097ee491",
       NULL } },
   { SC55_MODEL_MK2, "SC-55mkII (CTF-patched)",
     { "8a1eb33c7599b746c0c50283e4349a1bb1773b5c0ec0e9661219bf6c067d2042",
       "14d14778caf46ffa9e3d608aa8e9c1a60c32bd4a536c26af3b2e1d81784c60f9",
       "b0b5f865a403f7308b4be8d0ed3ba2ed1c22db881b8a8326769dea222f6431d8",
       "c6429e21b9b3a02fbd68ef0b2053668433bee0bccd537a71841bc70b8874243b",
       "5b753f6cef4cfc7fcafe1430fecbb94a739b874e55356246a46abe24097ee491",
       NULL } },
   { SC55_MODEL_MK2, "SC-55mkII (CTF-patched)",
     { "8a1eb33c7599b746c0c50283e4349a1bb1773b5c0ec0e9661219bf6c067d2042",
       "10b3f09485a74bb014f1a940d5c67f380c7979b62891d540d788154c83f17430",
       "b0b5f865a403f7308b4be8d0ed3ba2ed1c22db881b8a8326769dea222f6431d8",
       "c6429e21b9b3a02fbd68ef0b2053668433bee0bccd537a71841bc70b8874243b",
       "5b753f6cef4cfc7fcafe1430fecbb94a739b874e55356246a46abe24097ee491",
       NULL } },
   { SC55_MODEL_MK2, "SC-55mkII (CTF-patched)",
     { "8a1eb33c7599b746c0c50283e4349a1bb1773b5c0ec0e9661219bf6c067d2042",
       "a2c720be1ab9115930d27f821a413c0366b7bf0c4ddfe0dadc5086136a1a4345",
       "b0b5f865a403f7308b4be8d0ed3ba2ed1c22db881b8a8326769dea222f6431d8",
       "c6429e21b9b3a02fbd68ef0b2053668433bee0bccd537a71841bc70b8874243b",
       "5b753f6cef4cfc7fcafe1430fecbb94a739b874e55356246a46abe24097ee491",
       NULL } },
   { SC55_MODEL_MK2, "SC-55mkII (CTF-patched)",
     { "8a1eb33c7599b746c0c50283e4349a1bb1773b5c0ec0e9661219bf6c067d2042",
       "16cec615da10089beffe6de5129ba8ba33fa1bf017a5e6b78ad1d6d15cf4708e",
       "b0b5f865a403f7308b4be8d0ed3ba2ed1c22db881b8a8326769dea222f6431d8",
       "c6429e21b9b3a02fbd68ef0b2053668433bee0bccd537a71841bc70b8874243b",
       "5b753f6cef4cfc7fcafe1430fecbb94a739b874e55356246a46abe24097ee491",
       NULL } },
   { SC55_MODEL_MK2, "SC-55mkII (CTF-patched)",
     { "8a1eb33c7599b746c0c50283e4349a1bb1773b5c0ec0e9661219bf6c067d2042",
       "c22bf7d34a3406530924d750b007bbdb470f3216c65086edb6e53023383ee907",
       "b0b5f865a403f7308b4be8d0ed3ba2ed1c22db881b8a8326769dea222f6431d8",
       "c6429e21b9b3a02fbd68ef0b2053668433bee0bccd537a71841bc70b8874243b",
       "5b753f6cef4cfc7fcafe1430fecbb94a739b874e55356246a46abe24097ee491",
       NULL } },
   { SC55_MODEL_ST, "SC-55ST",
     { "8a1eb33c7599b746c0c50283e4349a1bb1773b5c0ec0e9661219bf6c067d2042",
       "03517ac0a3b1ad8b69a1a4ee045e0c21da0170027bd1ba1bd3cf72cd017bbe6a",
       "b0b5f865a403f7308b4be8d0ed3ba2ed1c22db881b8a8326769dea222f6431d8",
       "c6429e21b9b3a02fbd68ef0b2053668433bee0bccd537a71841bc70b8874243b",
       "5b753f6cef4cfc7fcafe1430fecbb94a739b874e55356246a46abe24097ee491",
       NULL } },
   { SC55_MODEL_MK1, "SC-55",
     { "b4ecf44bc0520322b0d114d397951d3bf92ca6fa51d0d27b2407df58a6be2efe",
       "014e2e21ea30de7a1e4f1cdea14dd9a719960535e257a9e40e98dbb1a5870226",
       NULL,
       "5655509a531804f97ea2d7ef05b8fec20ebf46216b389a84c44169257a4d2007",
       "c655b159792d999b90df9e4fa782cf56411ba1eaa0bb3ac2bdaf09e1391006b1",
       "334b2d16be3c2362210fdbec1c866ad58badeb0f84fd9bf5d0ac599baf077cc2" } },
   { SC55_MODEL_MK1, "SC-55",
     { "2fe88ec39f3ef4b1de8cdf74527419467975c47f7aacfcd07605e01d54bd89b5",
       "ec064d6c4fc70ec990911089d966043cb819fba0e26e6f6afdd0a05e5301b91b",
       NULL,
       "5655509a531804f97ea2d7ef05b8fec20ebf46216b389a84c44169257a4d2007",
       "c655b159792d999b90df9e4fa782cf56411ba1eaa0bb3ac2bdaf09e1391006b1",
       "334b2d16be3c2362210fdbec1c866ad58badeb0f84fd9bf5d0ac599baf077cc2" } },
   { SC55_MODEL_MK1, "SC-55",
     { "7e1bacd1d7c62ed66e465ba05597dcd60dfc13fc23de0287fdbce6cf906c6544",
       "22ce6ca59e6332143b335525e81fab501ea6fccce4b7e2f3bfc2cc8bf6612ff6",
       NULL,
       "5655509a531804f97ea2d7ef05b8fec20ebf46216b389a84c44169257a4d2007",
       "c655b159792d999b90df9e4fa782cf56411ba1eaa0bb3ac2bdaf09e1391006b1",
       "334b2d16be3c2362210fdbec1c866ad58badeb0f84fd9bf5d0ac599baf077cc2" } },
   { SC55_MODEL_MK1, "SC-55",
     { "7e1bacd1d7c62ed66e465ba05597dcd60dfc13fc23de0287fdbce6cf906c6544",
       "effc6132d68f7e300aaef915ccdd08aba93606c22d23e580daf9ea6617913af1",
       NULL,
       "5655509a531804f97ea2d7ef05b8fec20ebf46216b389a84c44169257a4d2007",
       "c655b159792d999b90df9e4fa782cf56411ba1eaa0bb3ac2bdaf09e1391006b1",
       "334b2d16be3c2362210fdbec1c866ad58badeb0f84fd9bf5d0ac599baf077cc2" } },
   { SC55_MODEL_MK1, "SC-55",
     { "24a65c97cdbaa847d6f59193523ce63c73394b4b693a6517ee79441f2fb8a3ee",
       "f5dac35d450ab986570a209dff3816eec75cee669e161f54b51224b467dd0bcc",
       NULL,
       "5655509a531804f97ea2d7ef05b8fec20ebf46216b389a84c44169257a4d2007",
       "c655b159792d999b90df9e4fa782cf56411ba1eaa0bb3ac2bdaf09e1391006b1",
       "334b2d16be3c2362210fdbec1c866ad58badeb0f84fd9bf5d0ac599baf077cc2" } },
   { SC55_MODEL_SC155, "SC-155",
     { "24a65c97cdbaa847d6f59193523ce63c73394b4b693a6517ee79441f2fb8a3ee",
       "ceb7b9d3d9d264efe5dc3ba992b94f3be35eb6d0451abc574b6f6b5dc3db237b",
       NULL,
       "5655509a531804f97ea2d7ef05b8fec20ebf46216b389a84c44169257a4d2007",
       "c655b159792d999b90df9e4fa782cf56411ba1eaa0bb3ac2bdaf09e1391006b1",
       "334b2d16be3c2362210fdbec1c866ad58badeb0f84fd9bf5d0ac599baf077cc2" } },
   { SC55_MODEL_CM300, "CM-300/SCC-1",
     { "72ed35481efbf25b3c492b83183655d17a3b266ecb30ffbc6dc977e6a8d261b2",
       "0283d32e6993a0265710c4206463deb937b0c3a4819b69f471a0eca5865719f9",
       NULL,
       "40c093cbfb4441a5c884e623f882a80b96b2527f9fd431e074398d206c0f073d",
       "9bbbcac747bd6f7a2693f4ef10633db8ab626f17d3d9c47c83c3839d4dd2f613",
       "5b753f6cef4cfc7fcafe1430fecbb94a739b874e55356246a46abe24097ee491" } },
   { SC55_MODEL_CM300, "CM-300/SCC-1",
     { "72ed35481efbf25b3c492b83183655d17a3b266ecb30ffbc6dc977e6a8d261b2",
       "fef1acb1969525d66238be5e7811108919b07a4df5fbab656ad084966373483f",
       NULL,
       "40c093cbfb4441a5c884e623f882a80b96b2527f9fd431e074398d206c0f073d",
       "9bbbcac747bd6f7a2693f4ef10633db8ab626f17d3d9c47c83c3839d4dd2f613",
       "5b753f6cef4cfc7fcafe1430fecbb94a739b874e55356246a46abe24097ee491" } },
   { SC55_MODEL_CM300, "CM-300/SCC-1",
     { "9ec66abb5231b6c6f46f48b33d5412703041037d69a6803626ac402f25552af2",
       "f89442734fdebacae87c7707c01b2d7fdbf5940abae738987aee912d34b5882e",
       NULL,
       "40c093cbfb4441a5c884e623f882a80b96b2527f9fd431e074398d206c0f073d",
       "9bbbcac747bd6f7a2693f4ef10633db8ab626f17d3d9c47c83c3839d4dd2f613",
       "5b753f6cef4cfc7fcafe1430fecbb94a739b874e55356246a46abe24097ee491" } },
   { SC55_MODEL_SCB55, "SCB-55",
     { "00df835d3f97fc8b0059db63f36d608eec2bfd1f51ad54eb5af52c868c1111b1",
       "541be4d0b1ef0d07bb042ba67ffd099c8a5d746aac4cd24ce8842c034379f213",
       NULL,
       "c6429e21b9b3a02fbd68ef0b2053668433bee0bccd537a71841bc70b8874243b",
       NULL,
       "5b753f6cef4cfc7fcafe1430fecbb94a739b874e55356246a46abe24097ee491" } },
   { SC55_MODEL_RLP3237, "RLP-3237",
     { "00df835d3f97fc8b0059db63f36d608eec2bfd1f51ad54eb5af52c868c1111b1",
       "e0a3d6d9b05e82374a0d289901273ce560ce1ead86459c75f844158b32d204a9",
       NULL,
       "dae2a8bc0fd3bcaf3f5e3ab6c4c6fd30e2663bf26ca17afe52924874c0afc4e2",
       NULL,
       NULL } },
};
#define SP_ROMSETS ((int)(sizeof(sp_romsets) / sizeof(sp_romsets[0])))

static const char *const sp_slot_name[SC55_ROM_COUNT] =
{
   "ROM1 (32K)", "ROM2", "sub-MCU ROM (4K)", "wave ROM 1", "wave ROM 2", "wave ROM 3"
};

#define SP_MAX_FOUND 48
typedef struct
{
   char  hex[65];
   char *path;
} sp_found_t;

static sp_found_t sp_found[SP_MAX_FOUND];
static int        sp_nfound;

#define SP_ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sp_sha256_block(uint32_t *h, const unsigned char *p)
{
   static const uint32_t k[64] = {
      0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
      0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
      0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
      0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
      0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
      0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
      0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
      0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
   };
   uint32_t w[64], v[8], t1, t2;
   int i;

   for (i = 0; i < 16; i++)
      w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16)
           | ((uint32_t)p[i * 4 + 2] << 8) | p[i * 4 + 3];
   for (; i < 64; i++)
   {
      uint32_t s0 = SP_ROR(w[i - 15], 7) ^ SP_ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = SP_ROR(w[i - 2], 17) ^ SP_ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
   }
   memcpy(v, h, sizeof(v));
   for (i = 0; i < 64; i++)
   {
      t1 = v[7] + (SP_ROR(v[4], 6) ^ SP_ROR(v[4], 11) ^ SP_ROR(v[4], 25))
         + ((v[4] & v[5]) ^ (~v[4] & v[6])) + k[i] + w[i];
      t2 = (SP_ROR(v[0], 2) ^ SP_ROR(v[0], 13) ^ SP_ROR(v[0], 22))
         + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
      v[7] = v[6]; v[6] = v[5]; v[5] = v[4]; v[4] = v[3] + t1;
      v[3] = v[2]; v[2] = v[1]; v[1] = v[0]; v[0] = t1 + t2;
   }
   for (i = 0; i < 8; i++)
      h[i] += v[i];
}

static void sp_sha256(const unsigned char *data, size_t len, char *hex)
{
   static const char digits[] = "0123456789abcdef";
   uint32_t h[8] = {
      0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u
   };
   unsigned char tail[128];
   size_t full = len / 64, rem = len % 64, tail_len, i;

   for (i = 0; i < full; i++)
      sp_sha256_block(h, data + i * 64);
   memset(tail, 0, sizeof(tail));
   memcpy(tail, data + full * 64, rem);
   tail[rem] = 0x80;
   tail_len  = (rem < 56) ? 64 : 128;
   for (i = 0; i < 8; i++)
      tail[tail_len - 1 - i] = (unsigned char)(((uint64_t)len * 8) >> (i * 8));
   sp_sha256_block(h, tail);
   if (tail_len == 128)
      sp_sha256_block(h, tail + 64);
   for (i = 0; i < 32; i++)
   {
      unsigned char b = (unsigned char)(h[i / 4] >> (24 - (i % 4) * 8));
      hex[i * 2]     = digits[b >> 4];
      hex[i * 2 + 1] = digits[b & 15];
   }
   hex[64] = '\0';
}

/* What the directory scan keeps: set by the unit's finder. */
static int  (*sp_scan_size)(int64_t size);
static void (*sp_scan_hash)(unsigned char *data, size_t len, char *hex);
static int  (*sp_scan_known)(const char *hex);

static int sp_sc55_size(int64_t size)
{
   return size == 0x1000 || size == 0x8000 || size == 0x40000
       || size == 0x80000 || size == 0x100000 || size == 0x200000;
}

static void sp_sc55_hash(unsigned char *data, size_t len, char *hex)
{
   sp_sha256(data, len, hex);
}

static int sp_known_digest(const char *hex)
{
   int r, k;
   for (r = 0; r < SP_ROMSETS; r++)
      for (k = 0; k < SC55_ROM_COUNT; k++)
         if (sp_romsets[r].rom[k] && !strcmp(sp_romsets[r].rom[k], hex))
            return 1;
   return 0;
}

static const char *sp_found_path(const char *hex)
{
   int i;
   for (i = 0; i < sp_nfound; i++)
      if (!strcmp(sp_found[i].hex, hex))
         return sp_found[i].path;
   return NULL;
}

/* A folder a ROM set is likely kept in. */
static int sp_rom_folder(const char *name)
{
   char low[64];
   size_t i;
   for (i = 0; name[i] && i < sizeof(low) - 1; i++)
      low[i] = (char)((name[i] >= 'A' && name[i] <= 'Z') ? name[i] + 32 : name[i]);
   low[i] = '\0';
   return strstr(low, "sc55") || strstr(low, "sc-55")
       || strstr(low, "sc88") || strstr(low, "sc-88")
       || strstr(low, "roland") || strstr(low, "nuked")
       || strstr(low, "88emu") || strstr(low, "gearmulator");
}

/* Hash every file in `dir` that has the size of a ROM and keep the ones
 * on the unit's list.  One level of likely-named subfolders is looked into. */
static void sp_scan_dir(const char *dir, int depth)
{
   struct RDIR *d = retro_opendir(dir);
   char path[2048];

   if (!d)
      return;
   if (retro_dirent_error(d))
   {
      retro_closedir(d);
      return;
   }
   while (retro_readdir(d))
   {
      const char *name = retro_dirent_get_name(d);
      void       *buf  = NULL;
      int64_t     len  = 0;
      int64_t     size;
      char        hex[65];

      size_t      dl   = strlen(dir);
      size_t      nl   = name ? strlen(name) : 0;

      if (!nl || name[0] == '.' || dl + nl + 2 > sizeof(path))
         continue;
      memcpy(path, dir, dl);
      path[dl] = '/';
      memcpy(path + dl + 1, name, nl + 1);
      if (retro_dirent_is_dir(d, NULL))
      {
         if (depth == 0 && sp_rom_folder(name))
            sp_scan_dir(path, 1);
         continue;
      }
      size = path_get_size(path);
      if (!sp_scan_size(size))
         continue;
      if (sp_nfound == SP_MAX_FOUND)
         break;
      if (!filestream_read_file(path, &buf, &len) || !buf || len != size)
      {
         free(buf);
         continue;
      }
      sp_scan_hash((unsigned char*)buf, (size_t)len, hex);
      free(buf);
      if (sp_scan_known(hex) && !sp_found_path(hex))
      {
         size_t n = strlen(path) + 1;
         char  *copy = (char*)malloc(n);
         if (copy)
         {
            memcpy(copy, path, n);
            memcpy(sp_found[sp_nfound].hex, hex, sizeof(hex));
            sp_found[sp_nfound].path = copy;
            sp_nfound++;
         }
      }
   }
   retro_closedir(d);
}

static int sp_load_path(sc55_t *dev, int slot, const char *path)
{
   void   *buf = NULL;
   int64_t len = 0;
   int     ok  = 0;

   if (filestream_read_file(path, &buf, &len) && buf && len > 0)
      ok = sc55_load_rom(dev, slot, (const unsigned char*)buf, (size_t)len);
   free(buf);
   return ok;
}

/* The classic file names, for a dump that is not on the list. */
static sc55_t *sp_load_by_name(int model)
{
   static const char *mk2[SC55_ROM_COUNT] =
      { "rom1.bin", "rom2.bin", "rom_sm.bin", "waverom1.bin", "waverom2.bin", NULL };
   static const char *mk1[SC55_ROM_COUNT] =
      { "sc55_rom1.bin", "sc55_rom2.bin", NULL,
        "sc55_waverom1.bin", "sc55_waverom2.bin", "sc55_waverom3.bin" };
   const char **names = (model == SC55_MODEL_MK1) ? mk1 : mk2;
   sc55_t *dev = sc55_new(model);
   int i;

   if (!dev)
      return NULL;
   for (i = 0; i < SC55_ROM_COUNT; i++)
   {
      char *path;
      int   ok;
      if (!names[i])
         continue;
      path = I_FindFile(names[i], NULL);
      ok   = path && sp_load_path(dev, i, path);
      free(path);
      if (!ok)
      {
         sc55_free(dev);
         return NULL;
      }
   }
   sc55_reset(dev);
   return dev;
}

/* Find a ROM set and build the unit, or say what is missing. */
static sc55_t *sp_find_unit(void)
{
   char    dirs[3][1024];
   int     ndirs = 0, r, k, best = -1, best_have = 0;
   sc55_t *dev = NULL;

   sp_nfound     = 0;
   sp_scan_size  = sp_sc55_size;
   sp_scan_hash  = sp_sc55_hash;
   sp_scan_known = sp_known_digest;
   for (k = 0; k < 3; k++)
      if (I_SearchDir(k, dirs[ndirs], sizeof(dirs[ndirs])))
      {
         sp_scan_dir(dirs[ndirs], 0);
         ndirs++;
      }

   for (r = 0; r < SP_ROMSETS && !dev; r++)
   {
      int need = 0, have = 0;
      for (k = 0; k < SC55_ROM_COUNT; k++)
         if (sp_romsets[r].rom[k])
         {
            need++;
            if (sp_found_path(sp_romsets[r].rom[k]))
               have++;
         }
      if (have > best_have)
      {
         best      = r;
         best_have = have;
      }
      if (have != need)
         continue;
      dev = sc55_new(sp_romsets[r].model);
      for (k = 0; dev && k < SC55_ROM_COUNT; k++)
         if (sp_romsets[r].rom[k]
               && !sp_load_path(dev, k, sp_found_path(sp_romsets[r].rom[k])))
         {
            sc55_free(dev);
            dev = NULL;
         }
      if (dev)
      {
         sc55_reset(dev);
         lprintf(LO_INFO, "SC55: using the %s ROM set.\n", sp_romsets[r].label);
      }
   }

   if (!dev)
      dev = sp_load_by_name(SC55_MODEL_MK2);
   if (!dev)
      dev = sp_load_by_name(SC55_MODEL_MK1);

   if (!dev)
   {
      lprintf(LO_WARN, "SC55: no complete ROM set, playing Adlib instead.  "
            "ROM files are recognised by content, whatever they are called, "
            "in these folders (and subfolders named sc55, roland or nuked):\n");
      for (k = 0; k < ndirs; k++)
         lprintf(LO_WARN, "SC55:   %s\n", dirs[k]);
      if (best >= 0)
      {
         lprintf(LO_WARN, "SC55: closest is the %s set, still missing:\n",
               sp_romsets[best].label);
         for (k = 0; k < SC55_ROM_COUNT; k++)
            if (sp_romsets[best].rom[k] && !sp_found_path(sp_romsets[best].rom[k]))
               lprintf(LO_WARN, "SC55:   %s, sha256 %s\n",
                     sp_slot_name[k], sp_romsets[best].rom[k]);
      }
      else
         lprintf(LO_WARN, "SC55: none of the files there is a known SC-55 ROM.\n");
      doom_printf("SC55: no ROM set found, playing Adlib");
   }

   for (k = 0; k < sp_nfound; k++)
      free(sp_found[k].path);
   sp_nfound = 0;
   return dev;
}

/* ---- the SC-55 as a unit -------------------------------------------- */

static void *sp_sc55_find(void)                  { return sp_find_unit(); }
static void  sp_sc55_destroy(void *d)            { sc55_free((sc55_t*)d); }
static void  sp_sc55_midi(void *d, const unsigned char *b, size_t n) { sc55_midi((sc55_t*)d, b, n); }
static uint32_t sp_sc55_voices(const void *d)    { return sc55_voices((const sc55_t*)d); }
static unsigned sp_sc55_rate(const void *d)      { return sc55_rate((const sc55_t*)d); }
static size_t sp_sc55_run(void *d, int32_t *f, size_t n) { return sc55_run((sc55_t*)d, f, n); }

/* The chip's samples are 20 bits shifted up by 12; upstream's integer
 * paths play them one bit hotter still, so full scale is 2^30. */
static const sp_unit_t sp_sc55 =
{
   "SC55", "prboom-sc55", sp_sc55_find, sp_sc55_destroy, sp_sc55_midi,
   sp_sc55_voices, sp_sc55_rate, sp_sc55_run, 1.0f / 1073741824.0f, SC55_BOOT_CAP_S
};

/* ---- the SC-88 family ----------------------------------------------- */

/* 88emu identifies dumps by MD5, the control ROM's taken in the CPU's
 * byte order (sc88_normalize_firmware); these are its digests. */
typedef struct
{
   int         model;
   const char *label;
   const char *rom[SC88_ROM_COUNT];
} sp88_romset_t;

#define SP88_IC325 "6a92b7de3ac7b8205d29ec4497644beb"
#define SP88_IC326 "d98f4b255d3a7dc830d92c71c25ce2eb"
#define SP88_IC327 "c05b103d4db110b3962431173cc72967"
#define SP88_IC328 "bccc26c34cac0d5509e8efb645043b5b"
#define SP88_PRO_A "bd33b20bb5e8f51e436e141f81a75c14"   /* R01567167, 8 MB */
#define SP88_PRO_B "125ea2056dc208f04a141b3dcdffdb1b"   /* R01567178, 8 MB */
#define SP88_PRO_C "48c3887c9a2a574b907242640fa0a320"   /* R01233667, 4 MB */

/* In order of preference. */
static const sp88_romset_t sp88_romsets[] =
{
#ifdef SC88_TEST_ROMSET
   SC88_TEST_ROMSET
#endif
   { SC88_MODEL_SC88PRO, "SC-88Pro 1.02",
     { "9d4c2f123b4451d8ee75c3b982760f28", SP88_PRO_A, SP88_PRO_B, SP88_PRO_C, NULL } },
   { SC88_MODEL_SC88PRO, "SC-88Pro (SC-GS A '96)",
     { "784b3ea762b5f96cabdceb33d121d5e4", SP88_PRO_A, SP88_PRO_B, SP88_PRO_C, NULL } },
   { SC88_MODEL_SC88PRO, "VE-GS Pro",
     { "f836d9491075c28c1c0587d4876cec65", SP88_PRO_A, SP88_PRO_B, SP88_PRO_C, NULL } },
   { SC88_MODEL_SC88VL, "SC-88VL 1.04",
     { "25e016e93c8a44ba3c35584462b56d72", SP88_IC325, SP88_IC326, SP88_IC327, SP88_IC328 } },
   { SC88_MODEL_SC88, "SC-88 1.01",
     { "0ac771782ea58a53af590ebdf140d517", SP88_IC325, SP88_IC326, SP88_IC327, SP88_IC328 } }
};
#define SP88_ROMSETS ((int)(sizeof(sp88_romsets) / sizeof(sp88_romsets[0])))

/* The SC-88Pro board's own 4 MB mask ROMs: each 8 MB part as two
 * halves, joined when the whole is not there. */
typedef struct { const char *whole, *first, *second; } sp88_halves_t;

static const sp88_halves_t sp88_halves[] =
{
   { SP88_PRO_A, "decc8a499b69e68ee8b121d73410c36b", "69933f0a2a3f6f4ab53932f40ba63f74" },
   { SP88_PRO_B, "bda725bd1cf8c3911f906314ca162bed", "94e94038993600555b738e8ccc24d8b8" }
};

static const char *const sp88_slot_name[SC88_ROM_COUNT] =
{
   "control ROM", "wave ROM 1", "wave ROM 2", "wave ROM 3", "wave ROM 4"
};

#define SP_ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void sp_md5_block(uint32_t *h, const unsigned char *p)
{
   static const uint32_t k[64] = {
      0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
      0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
      0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
      0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
      0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
      0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
      0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
      0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u, 0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u
   };
   static const unsigned char r[64] = {
      7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
      5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
      4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
      6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21
   };
   uint32_t w[16], a = h[0], b = h[1], c = h[2], d = h[3];
   int i;
   for (i = 0; i < 16; i++)
      w[i] = (uint32_t)p[i * 4] | ((uint32_t)p[i * 4 + 1] << 8)
           | ((uint32_t)p[i * 4 + 2] << 16) | ((uint32_t)p[i * 4 + 3] << 24);
   for (i = 0; i < 64; i++)
   {
      uint32_t f, t;
      int g;
      if (i < 16)      { f = (b & c) | (~b & d); g = i; }
      else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
      else if (i < 48) { f = b ^ c ^ d;          g = (3 * i + 5) & 15; }
      else             { f = c ^ (b | ~d);       g = (7 * i) & 15; }
      t = d;
      d = c;
      c = b;
      b = b + SP_ROL(a + f + k[i] + w[g], r[i]);
      a = t;
   }
   h[0] += a; h[1] += b; h[2] += c; h[3] += d;
}

static void sp_md5(const unsigned char *data, size_t len, char *hex)
{
   static const char digits[] = "0123456789abcdef";
   uint32_t h[4] = { 0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u };
   unsigned char tail[128];
   size_t full = len / 64, rem = len % 64, tail_len, i;

   for (i = 0; i < full; i++)
      sp_md5_block(h, data + i * 64);
   memset(tail, 0, sizeof(tail));
   memcpy(tail, data + full * 64, rem);
   tail[rem] = 0x80;
   tail_len  = (rem < 56) ? 64 : 128;
   for (i = 0; i < 8; i++)
      tail[tail_len - 8 + i] = (unsigned char)(((uint64_t)len * 8) >> (i * 8));
   sp_md5_block(h, tail);
   if (tail_len == 128)
      sp_md5_block(h, tail + 64);
   for (i = 0; i < 16; i++)
   {
      unsigned char v = (unsigned char)(h[i / 4] >> ((i % 4) * 8));
      hex[i * 2]     = digits[v >> 4];
      hex[i * 2 + 1] = digits[v & 15];
   }
   hex[32] = '\0';
}

static int sp88_size(int64_t size)
{
   return size == 0x80000 || size == 0x100000 || size == 0x200000
       || size == 0x400000 || size == 0x800000;
}

/* A control ROM is digested in the CPU's byte order. */
static void sp88_hash(unsigned char *data, size_t len, char *hex)
{
   if (len == 0x80000 || len == 0x100000)
      sc88_normalize_firmware(data, len);
   sp_md5(data, len, hex);
}

static int sp88_known(const char *hex)
{
   int r, k;
   for (r = 0; r < SP88_ROMSETS; r++)
      for (k = 0; k < SC88_ROM_COUNT; k++)
         if (sp88_romsets[r].rom[k] && !strcmp(sp88_romsets[r].rom[k], hex))
            return 1;
   for (r = 0; r < (int)(sizeof(sp88_halves) / sizeof(sp88_halves[0])); r++)
      if (!strcmp(sp88_halves[r].first, hex) || !strcmp(sp88_halves[r].second, hex))
         return 1;
   return 0;
}

static const sp88_halves_t *sp88_halves_of(const char *whole)
{
   int i;
   for (i = 0; i < (int)(sizeof(sp88_halves) / sizeof(sp88_halves[0])); i++)
      if (!strcmp(sp88_halves[i].whole, whole))
         return &sp88_halves[i];
   return NULL;
}

/* Whether a ROM of the set is there, whole or as its two halves. */
static int sp88_have(const char *hex)
{
   const sp88_halves_t *h;
   if (sp_found_path(hex))
      return 1;
   h = sp88_halves_of(hex);
   return h && sp_found_path(h->first) && sp_found_path(h->second);
}

static int sp88_load(sc88_t *dev, int slot, const char *hex)
{
   const char *path = sp_found_path(hex);
   const sp88_halves_t *h;
   void   *a = NULL, *b = NULL;
   int64_t la = 0, lb = 0;
   int     ok = 0;

   if (path)
   {
      if (filestream_read_file(path, &a, &la) && a && la > 0)
         ok = sc88_load_rom(dev, slot, (const unsigned char*)a, (size_t)la);
      free(a);
      return ok;
   }
   h = sp88_halves_of(hex);
   if (h && filestream_read_file(sp_found_path(h->first), &a, &la) && a
         && filestream_read_file(sp_found_path(h->second), &b, &lb) && b)
   {
      unsigned char *whole = (unsigned char*)malloc((size_t)(la + lb));
      if (whole)
      {
         memcpy(whole, a, (size_t)la);
         memcpy(whole + la, b, (size_t)lb);
         ok = sc88_load_rom(dev, slot, whole, (size_t)(la + lb));
         free(whole);
      }
   }
   free(a);
   free(b);
   return ok;
}

static void *sp88_find(void)
{
   char    dirs[3][1024];
   int     ndirs = 0, r, k, best = -1, best_have = 0;
   sc88_t *dev = NULL;

   sp_nfound     = 0;
   sp_scan_size  = sp88_size;
   sp_scan_hash  = sp88_hash;
   sp_scan_known = sp88_known;
   for (k = 0; k < 3; k++)
      if (I_SearchDir(k, dirs[ndirs], sizeof(dirs[ndirs])))
      {
         sp_scan_dir(dirs[ndirs], 0);
         ndirs++;
      }

   for (r = 0; r < SP88_ROMSETS && !dev; r++)
   {
      int need = 0, have = 0;
      for (k = 0; k < SC88_ROM_COUNT; k++)
         if (sp88_romsets[r].rom[k])
         {
            need++;
            if (sp88_have(sp88_romsets[r].rom[k]))
               have++;
         }
      if (have > best_have)
      {
         best      = r;
         best_have = have;
      }
      if (have != need)
         continue;
      dev = sc88_new(sp88_romsets[r].model);
      for (k = 0; dev && k < SC88_ROM_COUNT; k++)
         if (sp88_romsets[r].rom[k] && !sp88_load(dev, k, sp88_romsets[r].rom[k]))
         {
            sc88_free(dev);
            dev = NULL;
         }
      if (dev)
      {
         sc88_reset(dev);
         lprintf(LO_INFO, "SC88: using the %s ROM set.\n", sp88_romsets[r].label);
      }
   }

   if (!dev)
   {
      lprintf(LO_WARN, "SC88: no complete ROM set, playing Adlib instead.  "
            "ROM files are recognised by content, whatever they are called, "
            "in these folders (and subfolders named sc88, roland, 88emu or gearmulator):\n");
      for (k = 0; k < ndirs; k++)
         lprintf(LO_WARN, "SC88:   %s\n", dirs[k]);
      if (best >= 0)
      {
         lprintf(LO_WARN, "SC88: closest is the %s set, still missing:\n",
               sp88_romsets[best].label);
         for (k = 0; k < SC88_ROM_COUNT; k++)
            if (sp88_romsets[best].rom[k] && !sp88_have(sp88_romsets[best].rom[k]))
               lprintf(LO_WARN, "SC88:   %s, md5 %s\n",
                     sp88_slot_name[k], sp88_romsets[best].rom[k]);
      }
      else
         lprintf(LO_WARN, "SC88: none of the files there is a known SC-88 ROM.\n");
      doom_printf("SC88: no ROM set found, playing Adlib");
   }

   for (k = 0; k < sp_nfound; k++)
      free(sp_found[k].path);
   sp_nfound = 0;
   return dev;
}

static void  sp88_destroy(void *d)            { sc88_free((sc88_t*)d); }
static void  sp88_midi(void *d, const unsigned char *b, size_t n) { sc88_midi((sc88_t*)d, b, n); }
static uint32_t sp88_voices(const void *d)    { return sc88_voices((const sc88_t*)d); }
static unsigned sp88_rate(const void *d)      { return sc88_rate((const sc88_t*)d); }
static size_t sp88_run(void *d, int32_t *f, size_t n) { return sc88_run((sc88_t*)d, f, n); }

/* The XP's output words are 24 bits. */
static const sp_unit_t sp_sc88 =
{
   "SC88", "prboom-sc88", sp88_find, sp88_destroy, sp88_midi,
   sp88_voices, sp88_rate, sp88_run, 1.0f / 8388608.0f, SC88_BOOT_CAP_S
};

static void sp_close(void)
{
   if (sp_short && sp_rate && sp_unit)
      lprintf(LO_WARN, "%s: the emulation fell behind by %lu ms in all; "
            "the music was stretched by that much.\n",
            sp_unit->tag, (unsigned long)(sp_short * 1000 / (size_t)sp_rate));
   sp_short = 0;
#ifdef SC55_THREADED
   if (sp_thread)
   {
      SP_STORE_INT(&sp_quit, 1);
      retro_eventcount_notify(&sp_ec_work);
      sthread_join(sp_thread);
      sp_thread = NULL;
   }
   if (sp_ec_up)
   {
      retro_eventcount_free(&sp_ec_work);
      retro_eventcount_free(&sp_ec_ack);
      sp_ec_up = 0;
   }
#endif
   if (eng_dev)
      sp_unit->destroy(eng_dev);
   free(eng_kernel);
   free(eng_hist);
   free(eng_native);
   free(ev_ring);
   free(au_ring);
   free(sp_hist);
   sp_hist    = NULL;
   eng_dev    = NULL;
   eng_kernel = NULL;
   eng_hist   = NULL;
   eng_native = NULL;
   ev_ring    = NULL;
   au_ring    = NULL;
   sp_open_ok   = 0;
   sp_announced = 0;
}

/* Find the ROMs and bring the unit up.  Tried once per session. */
static int sp_open(void)
{
   if (sp_tried)
      return sp_open_ok;
   sp_tried = 1;

   eng_dev = sp_unit->find();
   if (!eng_dev)
      return 0;

   sp_unit->midi(eng_dev, sp_power_on, sizeof(sp_power_on));
   eng_native_rate = sp_unit->rate(eng_dev);
   eng_step        = (double)eng_native_rate / (double)sp_rate;
   eng_pos         = 0.0;
   eng_hist_len    = 0;
   eng_out_pos     = 0;
   eng_boot_frames = 0;
   eng_settle_left = 0;
   eng_epoch       = 0;
   sp_lead         = (size_t)sp_rate * SC55_LEAD_MS / 1000;

   eng_native = (int32_t*)malloc(SC55_NATIVE_MAX * 2 * sizeof(int32_t));
   eng_hist   = (float*)malloc((size_t)(eng_step * SC55_BLOCK + SC55_TAPS
               + SC55_NATIVE_MAX + 16) * 2 * sizeof(float));
   ev_ring    = (sp_event_t*)calloc(SC55_EVENT_RING, sizeof(sp_event_t));
   au_ring    = (float*)calloc(SC55_AUDIO_RING * 2, sizeof(float));
   sp_hist    = (float*)calloc(SC55_HISTORY * 2, sizeof(float));
   if (!eng_native || !eng_hist || !ev_ring || !au_ring || !sp_hist
         || !eng_build_kernel())
   {
      lprintf(LO_WARN, "%s: could not start the unit, playing Adlib instead.\n", sp_unit->tag);
      sp_close();
      return 0;
   }

   SP_STORE_SIZE(&ev_w, 0);
   SP_STORE_SIZE(&ev_r, 0);
   SP_STORE_SIZE(&au_w, 0);
   SP_STORE_SIZE(&au_r, 0);
   SP_STORE_SIZE(&sp_target, 0);
   SP_STORE_INT(&sp_epoch_req, 0);
   SP_STORE_INT(&sp_epoch_ack, 0);
   SP_STORE_INT(&sp_ready, 0);
   SP_STORE_INT(&sp_quit, 0);
   sp_consumed = 0;

#ifdef SC55_THREADED
   if (!retro_eventcount_init(&sp_ec_work))
   {
      lprintf(LO_WARN, "%s: could not start the unit, playing Adlib instead.\n", sp_unit->tag);
      sp_close();
      return 0;
   }
   if (!retro_eventcount_init(&sp_ec_ack))
   {
      retro_eventcount_free(&sp_ec_work);
      lprintf(LO_WARN, "%s: could not start the unit, playing Adlib instead.\n", sp_unit->tag);
      sp_close();
      return 0;
   }
   sp_ec_up  = 1;
   sp_thread = sthread_create(sp_worker, NULL);
   if (!sp_thread)
   {
      lprintf(LO_WARN, "%s: could not start the unit, playing Adlib instead.\n", sp_unit->tag);
      sp_close();
      return 0;
   }
#endif
   lprintf(LO_INFO, "%s: unit up at %u Hz, resampled to %d Hz.\n",
         sp_unit->tag, eng_native_rate, sp_rate);
   sp_open_ok = 1;
   return 1;
}

/* Make `unit` the one in use, putting the other one away. */
static void sp_select(const sp_unit_t *unit)
{
   if (sp_unit == unit)
      return;
   sp_playing = 0;
   sp_close();
   sp_tried = 0;
   sp_unit  = unit;
}

int I_SC55Available(void)
{
   sp_select(&sp_sc55);
   return sp_open();
}

int I_SC88Available(void)
{
   sp_select(&sp_sc88);
   return sp_open();
}

/* ---- caller side: queue and flush ----------------------------------- */

static void sp_post(size_t time, const unsigned char *bytes, size_t len)
{
   while (len)
   {
      size_t w = SP_LOAD_SIZE(&ev_w);
      size_t r = SP_LOAD_SIZE(&ev_r);
      sp_event_t *ev;
      size_t n = len < sizeof(ev->data) ? len : sizeof(ev->data);

      if (w - r >= SC55_EVENT_RING)
         return;  /* the worker is far behind; drop rather than block */
      ev       = &ev_ring[w & (SC55_EVENT_RING - 1)];
      ev->time = time;
      ev->len  = (unsigned char)n;
      memcpy(ev->data, bytes, n);
      SP_STORE_SIZE(&ev_w, w + 1);
      bytes += n;
      len   -= n;
   }
}

static void sp_post3(size_t time, int a, int b, int c)
{
   unsigned char m[3];
   m[0] = (unsigned char)a;
   m[1] = (unsigned char)b;
   m[2] = (unsigned char)c;
   sp_post(time, m, 3);
}

static void sp_all_notes_off(size_t time)
{
   int ch;
   for (ch = 0; ch < 16; ch++)
   {
      unsigned char m[5];
      m[0] = (unsigned char)(0xb0 | ch);
      m[1] = 123;  /* all notes off */
      m[2] = 0;
      m[3] = 120;  /* all sound off, so nothing rings on */
      m[4] = 0;
      sp_post(time, m, sizeof(m));
   }
}

/* Release every note but let it ring: the end of the track. */
static void sp_release_notes(size_t time)
{
   int ch;
   for (ch = 0; ch < 16; ch++)
      sp_post3(time, 0xb0 | ch, 123, 0);
}

/* Put every channel back to its power-on state for a new song.  A GS
 * reset would do it in one message, but the unit stops listening for
 * seconds after one. */
static void sp_reset_channels(size_t time)
{
   int ch;
   /* Silence first, on every channel, and the rest a moment later: the
    * firmware does not get round to cutting the voices until it has
    * worked through everything it was sent, and the rest is 300 bytes,
    * which kept the old notes sounding for another 55 ms. */
   for (ch = 0; ch < 16; ch++)
      sp_post3(time, 0xb0 | ch, 120, 0);
   time += (size_t)sp_rate / 50;
   for (ch = 0; ch < 16; ch++)
   {
      unsigned char m[21];
      size_t n = 0;
      m[n++] = (unsigned char)(0xb0 | ch);
      m[n++] = 121; m[n++] = 0;    /* reset all controllers */
      m[n++] = 0;   m[n++] = 0;    /* bank */
      m[n++] = 7;   m[n++] = 100;  /* volume */
      m[n++] = 10;  m[n++] = 64;   /* pan */
      m[n++] = 91;  m[n++] = 40;   /* reverb send */
      m[n++] = 93;  m[n++] = 0;    /* chorus send */
      m[n++] = 101; m[n++] = 0;    /* pitch bend range: two semitones */
      m[n++] = 100; m[n++] = 0;
      m[n++] = 6;   m[n++] = 2;
      m[n++] = (unsigned char)(0xc0 | ch);
      m[n++] = 0;                  /* program */
      sp_post(time, m, n);
   }
}

/* Empty both queues and start the frame count again.  The worker is
 * told to stop producing first, so nothing of the old epoch arrives
 * after the audio ring has been cleared. */
static void sp_flush(void)
{
   int req = SP_LOAD_INT(&sp_epoch_req) + 1;

   SP_STORE_SIZE(&sp_target, 0);
   SP_STORE_INT(&sp_epoch_req, req);
#ifdef SC55_THREADED
   {
      int tries;
      retro_eventcount_notify(&sp_ec_work);
      /* The worker acknowledges within one block of work.  The wait is
       * bounded all the same: a stuck worker costs the music, not the
       * game. */
      for (tries = 0; tries < 20; tries++)
      {
         int key;
         if (SP_LOAD_INT(&sp_epoch_ack) == req)
            break;
         key = retro_eventcount_prepare_wait(&sp_ec_ack);
         if (SP_LOAD_INT(&sp_epoch_ack) == req)
         {
            retro_eventcount_cancel_wait(&sp_ec_ack);
            break;
         }
         retro_eventcount_commit_wait_timeout(&sp_ec_ack, key, 25000);
      }
   }
#else
   eng_check_epoch();
#endif
   SP_STORE_SIZE(&au_r, SP_LOAD_SIZE(&au_w));
   sp_consumed = 0;
}

/* ---- sequencer ------------------------------------------------------ */

static void sp_apply_event(midi_event_t *ev, size_t time)
{
   unsigned ch = ev->data.channel.channel & 15;
   switch (ev->event_type)
   {
      case MIDI_EVENT_NOTE_OFF:
      case MIDI_EVENT_NOTE_ON:
      case MIDI_EVENT_AFTERTOUCH:
      case MIDI_EVENT_CONTROLLER:
      case MIDI_EVENT_PITCH_BEND:
         sp_post3(time, (int)(ev->event_type | ch),
               (int)ev->data.channel.param1, (int)ev->data.channel.param2);
         break;
      case MIDI_EVENT_PROGRAM_CHANGE:
      case MIDI_EVENT_CHAN_AFTERTOUCH:
      {
         unsigned char m[2];
         m[0] = (unsigned char)(ev->event_type | ch);
         m[1] = (unsigned char)ev->data.channel.param1;
         sp_post(time, m, 2);
         break;
      }
      case MIDI_EVENT_SYSEX:
      case MIDI_EVENT_SYSEX_SPLIT:
      {
         unsigned      l = ev->data.sysex.length;
         unsigned char b = (unsigned char)ev->event_type;
         sp_post(time, &b, 1);
         sp_post(time, ev->data.sysex.data, l);
         if (l == 0 || ev->data.sysex.data[l - 1] != 0xf7)
         {
            b = 0xf7;
            sp_post(time, &b, 1);
         }
         break;
      }
      case MIDI_EVENT_META:
         if (ev->data.meta.type == MIDI_META_SET_TEMPO)
            sp_spmc = MIDI_spmc(sp_midifile, ev, (unsigned)sp_rate);
         break;
      default:
         break;
   }
}

/* Queue every event that falls before output frame `until`. */
static void sp_sequence(size_t until)
{
   while (sp_playing && !sp_tail && sp_next_time < (double)until)
   {
      midi_event_t *ev   = sp_events[sp_eventpos];
      size_t        time = (size_t)sp_next_time;

      sp_apply_event(ev, time);
      if (ev->event_type == MIDI_EVENT_META
            && ev->data.meta.type == MIDI_META_END_OF_TRACK)
      {
         sp_release_notes(time);
         if (!sp_looping)
         {
            /* No more events; sp_pull_new stops the song once the
             * last notes and the reverb have had time to die away. */
            sp_tail = (size_t)sp_rate * SC55_TAIL_MS / 1000;
            break;
         }
         sp_eventpos = 0;
      }
      else
         sp_eventpos++;
      sp_next_time += (double)sp_events[sp_eventpos]->delta_time * sp_spmc;
      /* A track of nothing but an end marker must still move on. */
      if (sp_eventpos == 0
            && sp_events[0]->event_type == MIDI_EVENT_META
            && sp_events[0]->data.meta.type == MIDI_META_END_OF_TRACK)
         sp_next_time += (double)sp_rate;
   }
}

/* Start a clean epoch with every channel back to its defaults. */
static void sp_begin(void)
{
   sp_flush();
   sp_reset_channels(0);
   sp_tail      = 0;
   sp_clock     = 0;
   sp_clock_max = 0;
   sp_song_id   = sp_song_hash + (++sp_plays) * 0x9e3779b9u;
   sp_next_time = (double)sp_rate * SC55_RESET_MS / 1000.0;
   if (sp_events)
      sp_next_time += (double)sp_events[sp_eventpos]->delta_time * sp_spmc;
}

/* ---- music_player_t ------------------------------------------------- */

static const char *sp_name(void)
{
   return "sc55 emulation";
}

static int sp_init(int samplerate)
{
   if (eng_dev && samplerate != sp_rate)
   {
      sp_close();
      sp_tried = 0;
   }
   sp_rate = samplerate;
   return 1;
}

static void sp_shutdown(void)
{
   sp_playing = 0;
   sp_close();
   sp_tried = 0;
}

static void sp_setvolume(int v)
{
   sp_volume = v;
}

static void sp_pause(void)
{
   sp_paused = 1;
}

static void sp_resume(void)
{
   sp_paused = 0;
}

static const void *sp_registersong(const void *data, unsigned len)
{
   midimem_t mf;

   if (!sp_open())
      return NULL;

   mf.len  = len;
   mf.pos  = 0;
   mf.data = data;
   sp_midifile = MIDI_LoadFile(&mf);
   if (!sp_midifile)
      return NULL;
   sp_events = MIDI_GenerateFlatList(sp_midifile);
   if (!sp_events)
   {
      MIDI_FreeFile(sp_midifile);
      sp_midifile = NULL;
      return NULL;
   }
   sp_eventpos = 0;
   sp_spmc     = MIDI_spmc(sp_midifile, NULL, (unsigned)sp_rate);
   {
      const unsigned char *b = (const unsigned char*)data;
      unsigned i;
      sp_song_hash = 2166136261u ^ len;
      for (i = 0; i < len && i < 256; i++)
         sp_song_hash = (sp_song_hash ^ b[i]) * 16777619u;
   }
   return data;
}

static void sp_stop(void)
{
   if (!sp_playing)
      return;
   sp_playing = 0;
   if (sp_open_ok)
   {
      /* Drop what was queued and silence the unit now. */
      sp_flush();
      sp_all_notes_off(0);
      SP_STORE_SIZE(&sp_target, sp_lead);
      sp_wake();
   }
}

static void sp_unregistersong(const void *handle)
{
   (void)handle;
   sp_stop();
   if (sp_events)
   {
      MIDI_DestroyFlatList(sp_events);
      sp_events = NULL;
   }
   if (sp_midifile)
   {
      MIDI_FreeFile(sp_midifile);
      sp_midifile = NULL;
   }
}

static void sp_play(const void *handle, int looping)
{
   (void)handle;
   if (!sp_open_ok || !sp_events)
      return;
   sp_eventpos = 0;
   sp_looping  = looping;
   sp_paused   = 0;
   sp_spmc     = MIDI_spmc(sp_midifile, NULL, (unsigned)sp_rate);
   sp_playing  = 1;
   sp_begin();
}

/* Fill `dest`, already zeroed, with `n` frames of new audio from the
 * unit; returns how many are real, the rest staying silence. */
static unsigned sp_pull_new(float *dest, unsigned n)
{
   size_t w, r, got, i;

#ifndef SC55_THREADED
   /* Inline: boot at up to twice real time, then produce on demand. */
   if (!SP_LOAD_INT(&sp_ready))
      eng_boot_slice((size_t)((double)n * eng_step * 2.0) + 1);
#endif
   if (!SP_LOAD_INT(&sp_ready))
      return 0;   /* the sequencer waits for the unit */
   if (!sp_announced)
   {
      sp_announced = 1;
      lprintf(LO_INFO, "%s: firmware ready, music starts.\n", sp_unit->tag);
   }

   sp_sequence(sp_consumed + n + sp_lead);
   SP_STORE_SIZE(&sp_target, sp_consumed + n + sp_lead);
   sp_wake();
#ifndef SC55_THREADED
   eng_produce();
#endif

   w   = SP_LOAD_SIZE(&au_w);
   r   = SP_LOAD_SIZE(&au_r);
   got = w - r;
   if (got > n)
      got = n;
   for (i = 0; i < got; i++)
   {
      const float *src = au_ring + ((r + i) & (SC55_AUDIO_RING - 1)) * 2;
      dest[i * 2]     = src[0];
      dest[i * 2 + 1] = src[1];
   }
   SP_STORE_SIZE(&au_r, r + got);
   /* A short read is not counted: the song stretches by the gap
    * instead of dropping what the worker had not finished. */
   if (sp_consumed)   /* the first read of a song finds the queue still filling */
      sp_short += n - got;
   sp_consumed += got;
   if (sp_tail)
   {
      if (sp_tail > got)
         sp_tail -= got;
      else
      {
         sp_tail    = 0;
         sp_playing = 0;
      }
   }
   sp_wake();
   return (unsigned)got;
}

/* Fill `dest` with `n` frames of float audio.
 *
 * The unit cannot be wound back, so a state load that steps back a
 * short way (run-ahead, rewind) is served from what was handed out
 * before: the same frames again, until the game is back where the unit
 * is.  The music then comes out exactly as it would have without the
 * load. */
/* The unit's audio stops dead in several places: a song is stopped or
 * changed, the game pauses, a state is loaded, a song runs out, or the
 * worker is late.  Cutting a waveform off at whatever value it has is a
 * click, and so is starting it there.  So where the audio stops, its
 * last value is carried on and dies away over a few milliseconds, and
 * where it starts it fades in over the same time.
 *
 * `real` frames at the start of `buf` are the unit's; the rest are a
 * gap, zero on entry.  With no gap about, a frame passes through
 * untouched. */
#define SC55_DECLICK_MS 4

static void sp_declick(float *buf, unsigned n, unsigned real)
{
   const float step  = 1000.0f / ((float)sp_rate * SC55_DECLICK_MS);
   /* down 60 dB over the same span */
   const float decay = (float)pow(0.001, (double)step);
   unsigned k;

   if (real == n && sp_fade >= 1.0f && sp_carry[0] == 0.0f && sp_carry[1] == 0.0f)
   {
      if (n)
      {
         sp_last[0] = buf[(n - 1) * 2];
         sp_last[1] = buf[(n - 1) * 2 + 1];
      }
      return;
   }
   for (k = 0; k < n; k++)
   {
      float l = buf[k * 2], r = buf[k * 2 + 1];
      if (k < real)
      {
         if (sp_fade < 1.0f)
         {
            l *= sp_fade;
            r *= sp_fade;
            sp_fade += step;
            if (sp_fade > 1.0f)
               sp_fade = 1.0f;
         }
      }
      else
      {
         if (sp_fade > 0.0f)
         {
            /* the audio has just stopped: carry on from where it was */
            sp_carry[0] = sp_last[0];
            sp_carry[1] = sp_last[1];
            sp_fade     = 0.0f;
         }
         l = r = 0.0f;
      }
      sp_carry[0] *= decay;
      sp_carry[1] *= decay;
      if (sp_carry[0] > -1e-7f && sp_carry[0] < 1e-7f)
         sp_carry[0] = 0.0f;
      if (sp_carry[1] > -1e-7f && sp_carry[1] < 1e-7f)
         sp_carry[1] = 0.0f;
      l += sp_carry[0];
      r += sp_carry[1];
      buf[k * 2]     = l;
      buf[k * 2 + 1] = r;
      sp_last[0]     = l;
      sp_last[1]     = r;
   }
}

static void sp_pull(float *dest, unsigned n)
{
   unsigned i = 0;

   memset(dest, 0, (size_t)n * 2 * sizeof(float));
   if (!sp_open_ok || !sp_playing || sp_paused || !sp_events)
   {
      if (sp_rate > 0)
         sp_declick(dest, n, 0);
      return;
   }

   while (i < n && sp_clock < sp_clock_max)
   {
      const float *src = sp_hist + (size_t)(sp_clock & (SC55_HISTORY - 1)) * 2;
      dest[i * 2]     = src[0];
      dest[i * 2 + 1] = src[1];
      sp_clock++;
      i++;
   }
   if (i < n)
   {
      unsigned k;
      unsigned got = sp_pull_new(dest + i * 2, n - i);
      sp_declick(dest + i * 2, n - i, got);
      for (k = i; k < n; k++)
      {
         float *dst = sp_hist + (size_t)(sp_clock_max & (SC55_HISTORY - 1)) * 2;
         dst[0] = dest[k * 2];
         dst[1] = dest[k * 2 + 1];
         sp_clock_max++;
      }
      sp_clock = sp_clock_max;
   }
}

/* The game's music volume, 0 to 15, as a gain.  The curve and the
 * factor are fitted to the Adlib player, measured on the same song at
 * the same settings, so that changing MIDI Hardware does not change how
 * loud the music is or what the volume slider does to it. */
static float sp_gain(void)
{
   return 2.0f * (float)pow((double)sp_volume / 15.0, 1.75);
}

/* Apply the volume to `n` frames.  A change of volume is spread over
 * the block; applied at once it is a step in the waveform, and dragging
 * the slider is a run of them. */
static void sp_apply_gain(float *buf, unsigned n, float scale)
{
   float    target = sp_gain();
   float    from   = sp_gain_now < 0.0f ? target : sp_gain_now;
   unsigned i;

   if (from == target)
   {
      float g = target * scale;
      for (i = 0; i < n * 2; i++)
         buf[i] *= g;
   }
   else
   {
      float inc = (target - from) / (float)n;
      for (i = 0; i < n; i++)
      {
         float g = (from + inc * (float)(i + 1)) * scale;
         buf[i * 2]     *= g;
         buf[i * 2 + 1] *= g;
      }
   }
   sp_gain_now = target;
}

static void sp_render_float(void *vdest, unsigned nsamp)
{
   float *dest = (float*)vdest;

   sp_pull(dest, nsamp);
   sp_apply_gain(dest, nsamp, 1.0f);
}

static void sp_render(void *vdest, unsigned nsamp)
{
   int16_t *dest = (int16_t*)vdest;
   float    tmp[256 * 2];

   while (nsamp)
   {
      unsigned n = nsamp < 256 ? nsamp : 256;
      unsigned i;
      int      silent = 1;
      sp_pull(tmp, n);
      sp_apply_gain(tmp, n, 32768.0f);
      for (i = 0; i < n * 2 && silent; i++)
         if (tmp[i] != 0.0f)
            silent = 0;
      /* Nothing playing is exact silence, not dither noise. */
      for (i = 0; silent && i < n * 2; i++)
         dest[i] = 0;
      for (i = 0; !silent && i < n * 2; i++)
      {
         /* TPDF dither of one step peak to peak each way, then round
          * to nearest: the only quantization on this path. */
         float d, v;
         int   q;
         sp_dither = sp_dither * 1664525u + 1013904223u;
         d  = (float)(sp_dither >> 16) * (1.0f / 65536.0f);
         sp_dither = sp_dither * 1664525u + 1013904223u;
         d -= (float)(sp_dither >> 16) * (1.0f / 65536.0f);
         v  = tmp[i] + d;
         q  = (int)floor((double)v + 0.5);
         if (q > 32767)  q = 32767;
         if (q < -32768) q = -32768;
         dest[i] = (int16_t)q;
      }
      dest  += n * 2;
      nsamp -= n;
   }
}

/* State: where the sequencer is, and which frame of which playing the
 * game had reached.  The core keeps 512 bytes for a music player's
 * state, so the unit itself (about 80 KB of machine state, plus what
 * is in flight between the two threads) is not saved.  A load that
 * steps a short way back is absorbed by sp_pull.  Any other load
 * starts the channels afresh, restores what the song had set on them
 * by the saved event, starts the notes that were held there again and
 * carries on from that event. */
#define SP_STATE_MAGIC 0x53433535u  /* 'SC55' */

typedef struct
{
   uint32_t magic;
   uint32_t eventpos;
   uint32_t flags;
   uint32_t song_id;
   uint64_t clock;
   double   spmc;
} sp_state_t;

static size_t sp_serialize(void *dest, size_t cap)
{
   sp_state_t s;
   if (!sp_events || !sp_playing)
      return 0;
   if (!dest)
      return sizeof(s);
   if (cap < sizeof(s))
      return 0;
   s.magic    = SP_STATE_MAGIC;
   s.eventpos = (uint32_t)sp_eventpos;
   s.flags    = (sp_looping ? 1u : 0u) | (sp_paused ? 2u : 0u);
   s.song_id  = sp_song_id;
   s.clock    = sp_clock;
   s.spmc     = sp_spmc;
   memcpy(dest, &s, sizeof(s));
   return sizeof(s);
}

static int sp_unserialize(const void *src, size_t size)
{
   sp_state_t s;
   uint32_t   i;

   if (!sp_events || !sp_open_ok || size < sizeof(s))
      return 0;
   memcpy(&s, src, sizeof(s));
   if (s.magic != SP_STATE_MAGIC)
      return 0;

   /* The same playing of the same song, a short way back: step back
    * and let sp_pull hand that stretch out again. */
   if (sp_playing && s.song_id == sp_song_id
         && s.clock <= sp_clock_max
         && sp_clock_max - s.clock <= SC55_HISTORY)
   {
      sp_clock  = s.clock;
      sp_paused = (s.flags & 2u) != 0;
      return 1;
   }

   sp_eventpos = 0;
   sp_spmc     = MIDI_spmc(sp_midifile, NULL, (unsigned)sp_rate);
   sp_begin();
   /* Work out what the song had set up by the saved event (programs,
    * controllers, bend, bend range) and which notes were still held,
    * and send only that.  Sending every event again would overrun the
    * queue on a long song. */
   {
      static short         cc[16][128];
      static unsigned char held[16][128];
      short  prog[16], bend_l[16], bend_m[16], range[16];
      size_t t0 = (size_t)((double)sp_rate * SC55_RESET_MS / 1000.0);
      int    ch, c;

      memset(held, 0, sizeof(held));
      for (ch = 0; ch < 16; ch++)
      {
         prog[ch] = bend_l[ch] = bend_m[ch] = range[ch] = -1;
         for (c = 0; c < 128; c++)
            cc[ch][c] = -1;
      }
      for (i = 0; i < s.eventpos; i++)
      {
         midi_event_t *ev = sp_events[i];
         int p1, p2;
         if (ev->event_type == MIDI_EVENT_META
               && ev->data.meta.type == MIDI_META_END_OF_TRACK)
            break;
         ch = (int)(ev->data.channel.channel & 15);
         p1 = (int)(ev->data.channel.param1 & 127);
         p2 = (int)(ev->data.channel.param2 & 127);
         switch (ev->event_type)
         {
            case MIDI_EVENT_NOTE_ON:
               held[ch][p1] = (unsigned char)p2;
               break;
            case MIDI_EVENT_NOTE_OFF:
               held[ch][p1] = 0;
               break;
            case MIDI_EVENT_CONTROLLER:
               if (p1 == 6 && cc[ch][101] == 0 && cc[ch][100] == 0)
                  range[ch] = (short)p2;
               if (p1 == 120 || p1 == 123)
                  memset(held[ch], 0, sizeof(held[ch]));
               cc[ch][p1] = (short)p2;
               break;
            case MIDI_EVENT_PROGRAM_CHANGE:
               prog[ch] = (short)p1;
               break;
            case MIDI_EVENT_PITCH_BEND:
               bend_l[ch] = (short)p1;
               bend_m[ch] = (short)p2;
               break;
            case MIDI_EVENT_META:
            case MIDI_EVENT_SYSEX:
            case MIDI_EVENT_SYSEX_SPLIT:
               sp_apply_event(ev, t0);
               break;
            default:
               break;
         }
      }
      for (ch = 0; ch < 16; ch++)
      {
         if (cc[ch][0] >= 0)
            sp_post3(t0, 0xb0 | ch, 0, cc[ch][0]);
         if (cc[ch][32] >= 0)
            sp_post3(t0, 0xb0 | ch, 32, cc[ch][32]);
         if (prog[ch] >= 0)
         {
            unsigned char m[2];
            m[0] = (unsigned char)(0xc0 | ch);
            m[1] = (unsigned char)prog[ch];
            sp_post(t0, m, 2);
         }
         if (range[ch] >= 0)
         {
            sp_post3(t0, 0xb0 | ch, 101, 0);
            sp_post3(t0, 0xb0 | ch, 100, 0);
            sp_post3(t0, 0xb0 | ch, 6, range[ch]);
         }
         for (c = 1; c < 120; c++)
            if (cc[ch][c] >= 0 && c != 32 && c != 6 && c != 38
                  && (c < 96 || c > 101))
               sp_post3(t0, 0xb0 | ch, c, cc[ch][c]);
         if (bend_l[ch] >= 0)
            sp_post3(t0, 0xe0 | ch, bend_l[ch], bend_m[ch]);
      }
      /* Notes held across the saved point start again; percussion is
       * left out, a drum hit from before the save is not wanted back. */
      for (ch = 0; ch < 16; ch++)
         for (c = 0; ch != 9 && c < 128; c++)
            if (held[ch][c])
               sp_post3(t0, 0x90 | ch, c, held[ch][c]);
   }
   sp_eventpos  = i;
   sp_spmc      = s.spmc;
   sp_looping   = (s.flags & 1u) != 0;
   sp_paused    = (s.flags & 2u) != 0;
   sp_playing   = 1;
   sp_next_time = (double)sp_rate * SC55_RESET_MS / 1000.0
                + (double)sp_events[sp_eventpos]->delta_time * sp_spmc;
   sp_clock     = s.clock;
   sp_clock_max = s.clock;
   return 1;
}

static const void *sp_sc55_registersong(const void *data, unsigned len)
{
   sp_select(&sp_sc55);
   return sp_registersong(data, len);
}

static const char *sp88_name(void)
{
   return "sc88 emulation";
}

static const void *sp88_registersong(const void *data, unsigned len)
{
   sp_select(&sp_sc88);
   return sp_registersong(data, len);
}

const music_player_t sc55_player =
{
   sp_name,
   sp_init,
   sp_shutdown,
   sp_setvolume,
   sp_pause,
   sp_resume,
   sp_sc55_registersong,
   sp_unregistersong,
   sp_play,
   sp_stop,
   sp_render,
   sp_serialize,
   sp_unserialize,
   sp_render_float
};

const music_player_t sc88_player =
{
   sp88_name,
   sp_init,
   sp_shutdown,
   sp_setvolume,
   sp_pause,
   sp_resume,
   sp88_registersong,
   sp_unregistersong,
   sp_play,
   sp_stop,
   sp_render,
   sp_serialize,
   sp_unserialize,
   sp_render_float
};
