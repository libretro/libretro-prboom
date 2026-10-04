/* Roland SC-55 music player.
 *
 * sc55.c runs the unit's own firmware, so unlike the OPL player this
 * one has a machine to boot and a real cost per sample.  The emulator
 * therefore lives on a worker thread.  The caller's thread keeps the
 * sequencer: it stamps each MIDI event with the output frame it belongs
 * to and queues it, tells the worker how far it may run, and takes
 * finished audio back.  Both queues are single-producer single-consumer
 * rings on retro_atomic indices; nothing here takes a lock, and every
 * wait is an eventcount wait.
 *
 * The worker runs SC55_LEAD frames ahead of what has been played, which
 * is how late the music is against the sequencer.  Until the firmware
 * has enabled its MIDI input the sequencer does not start, so the first
 * song begins late instead of losing its opening.
 *
 * The chip's frames stay in float from the emulator to the mixer.  The
 * 16-bit path quantizes once, at the very end, with rounding and TPDF
 * dither; upstream's 16-bit path was a bare arithmetic shift, which
 * truncates toward minus infinity.
 *
 * Without HAVE_THREADS the same code runs inline from render.
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

#include "sc55player.h"
#include "sc55.h"
#include "midifile.h"
#include "lprintf.h"
#include "i_system.h"

#if defined(HAVE_THREADS)
#include <rthreads/rthreads.h>
#include <rthreads/retro_eventcount.h>
#include <retro_atomic.h>
#if defined(RETRO_ATOMIC_LOCK_FREE)
#define SC55_THREADED 1
#endif
#endif

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
#define SC55_TAPS        32
#define SC55_PHASES      128
#define SC55_NATIVE_MAX  2048                /* native frames per device call */
#ifndef SC55_BOOT_CAP_S
#define SC55_BOOT_CAP_S  12                  /* give up waiting for the firmware */
#endif
#define SC55_SETTLE_MS   250                 /* after the firmware is listening */
#define SC55_RESET_MS    60                  /* the unit ignores input after a GS reset */

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

static sc55_t  *eng_dev;
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

static const unsigned char gs_reset[11] =
   { 0xf0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7f, 0x00, 0x41, 0xf7 };

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
   /* The chip's samples are 20 bits shifted up by 12; upstream's integer
    * paths play them one bit hotter still, so full scale is 2^30. */
   const float scale = 1.0f / 1073741824.0f;
   while (n)
   {
      size_t chunk = n < SC55_NATIVE_MAX ? n : SC55_NATIVE_MAX;
      size_t i;
      float *dst = eng_hist + eng_hist_len * 2;
      sc55_run(eng_dev, eng_native, chunk);
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
      sc55_run(eng_dev, eng_native, chunk);
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
      else if (sc55_ready(eng_dev)
            || eng_boot_frames >= (size_t)eng_native_rate * SC55_BOOT_CAP_S)
         eng_settle_left = (size_t)eng_native_rate * SC55_SETTLE_MS / 1000;
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
         sc55_midi(eng_dev, ev->data, ev->len);
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
   sthread_setname("prboom-sc55");
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

static int sp_load_rom(sc55_t *dev, int slot, const char *name)
{
   char   *path = I_FindFile(name, NULL);
   void   *buf  = NULL;
   int64_t len  = 0;
   int     ok   = 0;

   if (!path)
      return 0;
   if (filestream_read_file(path, &buf, &len) && buf && len > 0)
      ok = sc55_load_rom(dev, slot, (const unsigned char*)buf, (size_t)len);
   free(buf);
   free(path);
   return ok;
}

static sc55_t *sp_load_model(int model)
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
      if (names[i] && !sp_load_rom(dev, i, names[i]))
      {
         sc55_free(dev);
         return NULL;
      }
   sc55_reset(dev);
   return dev;
}

static void sp_close(void)
{
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
      sc55_free(eng_dev);
   free(eng_kernel);
   free(eng_hist);
   free(eng_native);
   free(ev_ring);
   free(au_ring);
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

   eng_dev = sp_load_model(SC55_MODEL_MK2);
   if (!eng_dev)
      eng_dev = sp_load_model(SC55_MODEL_MK1);
   if (!eng_dev)
   {
      lprintf(LO_WARN, "SC55: no complete ROM set found (mkII: rom1.bin, "
            "rom2.bin, rom_sm.bin, waverom1.bin, waverom2.bin; mk1: "
            "sc55_rom1.bin, sc55_rom2.bin, sc55_waverom1-3.bin).\n");
      return 0;
   }

   eng_native_rate = sc55_rate(eng_dev);
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
   if (!eng_native || !eng_hist || !ev_ring || !au_ring || !eng_build_kernel())
   {
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
      sp_close();
      return 0;
   }
   if (!retro_eventcount_init(&sp_ec_ack))
   {
      retro_eventcount_free(&sp_ec_work);
      sp_close();
      return 0;
   }
   sp_ec_up  = 1;
   sp_thread = sthread_create(sp_worker, NULL);
   if (!sp_thread)
   {
      sp_close();
      return 0;
   }
#endif
   lprintf(LO_INFO, "SC55: unit up at %u Hz, resampled to %d Hz.\n",
         eng_native_rate, sp_rate);
   sp_open_ok = 1;
   return 1;
}

int I_SC55Available(void)
{
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
      sp_post3(time, 0xb0 | ch, 123, 0);
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
   while (sp_playing && sp_next_time < (double)until)
   {
      midi_event_t *ev   = sp_events[sp_eventpos];
      size_t        time = (size_t)sp_next_time;

      sp_apply_event(ev, time);
      if (ev->event_type == MIDI_EVENT_META
            && ev->data.meta.type == MIDI_META_END_OF_TRACK)
      {
         sp_all_notes_off(time);
         if (!sp_looping)
         {
            sp_playing = 0;
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

/* Start a clean epoch with the unit reset to its power-on sound set. */
static void sp_begin(void)
{
   sp_flush();
   sp_all_notes_off(0);
   sp_post(0, gs_reset, sizeof(gs_reset));
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

/* Fill `dest` with `n` frames of float audio; returns how many are
 * real, the rest being silence. */
static unsigned sp_pull(float *dest, unsigned n)
{
   size_t w, r, got, i;

   memset(dest, 0, (size_t)n * 2 * sizeof(float));
   if (!sp_open_ok || !sp_playing || sp_paused || !sp_events)
      return 0;

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
      lprintf(LO_INFO, "SC55: firmware ready, music starts.\n");
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
   sp_consumed += got;
   sp_wake();
   return (unsigned)got;
}

static void sp_render_float(void *vdest, unsigned nsamp)
{
   float   *dest = (float*)vdest;
   float    gain = (float)sp_volume / 15.0f;
   unsigned i;

   sp_pull(dest, nsamp);
   for (i = 0; i < nsamp * 2; i++)
      dest[i] *= gain;
}

static void sp_render(void *vdest, unsigned nsamp)
{
   int16_t *dest = (int16_t*)vdest;
   float    gain = (float)sp_volume / 15.0f * 32767.0f;
   float    tmp[256 * 2];

   while (nsamp)
   {
      unsigned n = nsamp < 256 ? nsamp : 256;
      unsigned i;
      sp_pull(tmp, n);
      for (i = 0; i < n * 2; i++)
      {
         /* TPDF dither of one step peak to peak each way, then round
          * to nearest: the only quantization on this path. */
         float d, v;
         int   q;
         sp_dither = sp_dither * 1664525u + 1013904223u;
         d  = (float)(sp_dither >> 16) * (1.0f / 65536.0f);
         sp_dither = sp_dither * 1664525u + 1013904223u;
         d -= (float)(sp_dither >> 16) * (1.0f / 65536.0f);
         v  = tmp[i] * gain + d;
         q  = (int)floor((double)v + 0.5);
         if (q > 32767)  q = 32767;
         if (q < -32768) q = -32768;
         dest[i] = (int16_t)q;
      }
      dest  += n * 2;
      nsamp -= n;
   }
}

/* State: where the sequencer is.  The unit itself is not saved; on
 * restore it is reset and the song's program and controller state is
 * replayed up to the saved event. */
#define SP_STATE_MAGIC 0x53433535u  /* 'SC55' */

typedef struct
{
   uint32_t magic;
   uint32_t eventpos;
   uint32_t flags;
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

   sp_eventpos = 0;
   sp_spmc     = MIDI_spmc(sp_midifile, NULL, (unsigned)sp_rate);
   sp_begin();
   for (i = 0; i < s.eventpos; i++)
   {
      midi_event_t *ev = sp_events[i];
      if (ev->event_type == MIDI_EVENT_META
            && ev->data.meta.type == MIDI_META_END_OF_TRACK)
         break;
      if (ev->event_type != MIDI_EVENT_NOTE_ON
            && ev->event_type != MIDI_EVENT_NOTE_OFF)
         sp_apply_event(ev, (size_t)((double)sp_rate * SC55_RESET_MS / 1000.0));
   }
   sp_eventpos  = i;
   sp_spmc      = s.spmc;
   sp_looping   = (s.flags & 1u) != 0;
   sp_paused    = (s.flags & 2u) != 0;
   sp_playing   = 1;
   sp_next_time = (double)sp_rate * SC55_RESET_MS / 1000.0
                + (double)sp_events[sp_eventpos]->delta_time * sp_spmc;
   return 1;
}

const music_player_t sc55_player =
{
   sp_name,
   sp_init,
   sp_shutdown,
   sp_setvolume,
   sp_pause,
   sp_resume,
   sp_registersong,
   sp_unregistersong,
   sp_play,
   sp_stop,
   sp_render,
   sp_serialize,
   sp_unserialize,
   sp_render_float
};
