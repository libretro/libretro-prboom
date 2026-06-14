/* ProTracker .MOD music player backend.
 *
 * Some mods ship their music as Amiga tracker modules (.mod files under
 * music/) rather than MIDI/MUS or MP3.  This wraps pocketmod (a small public-domain/MIT
 * single-header ProTracker player) in the engine's music_player_t interface
 * so a .MOD lump autodetects and plays alongside the other backends.
 *
 * pocketmod renders interleaved float stereo; the engine's mixer wants
 * interleaved signed-16 stereo at the init samplerate, so render() decodes
 * into a small float scratch buffer and converts.  The whole decoder state
 * lives in a heap-allocated context (it carries per-channel and sample
 * tables), allocated once at init. */

#include <stdlib.h>
#include <string.h>

#include "lprintf.h"
#include "musicplayer.h"

#define POCKETMOD_IMPLEMENTATION
#include "../deps/pocketmod/pocketmod.h"

static pocketmod_context *mod_ctx;
static const void        *mod_data;   /* the lump (owned by the caller) */
static int                mod_len;
static int                mod_rate;
static int                mod_volume;  /* 0..15 */
static int                mod_looping;
static int                mod_playing;
static int                mod_paused;

/* scratch space for one render burst of float stereo before s16 convert */
#define MOD_SCRATCH_FRAMES 1024
static float mod_scratch[MOD_SCRATCH_FRAMES * 2];

static const char *mod_name(void)
{
  return "pocketmod tracker player";
}

static int mod_init(int samplerate)
{
  mod_rate = samplerate;
  if (!mod_ctx)
    mod_ctx = (pocketmod_context *)malloc(sizeof(pocketmod_context));
  return mod_ctx != NULL;
}

static void mod_shutdown(void)
{
  if (mod_ctx)
  {
    free(mod_ctx);
    mod_ctx = NULL;
  }
  mod_data    = NULL;
  mod_playing = 0;
}

static void mod_setvolume(int v)
{
  mod_volume = v;
}

static void mod_pause(void)
{
  mod_paused = 1;
}

static void mod_resume(void)
{
  mod_paused = 0;
}

/* A .MOD is identified by a 4-byte tag at offset 1080: "M.K."/"M!K!" for the
 * classic 4-channel format, "FLTx", "xCHN", "xxCH" for the multi-channel
 * variants.  Reject anything that does not carry one of those (and is not big
 * enough to hold the header) so the other backends keep their formats. */
static int mod_is_module(const unsigned char *d, unsigned len)
{
  const unsigned char *t;
  if (len < 1084)
    return 0;
  t = d + 1080;
  if (!memcmp(t, "M.K.", 4) || !memcmp(t, "M!K!", 4) ||
      !memcmp(t, "M&K!", 4) || !memcmp(t, "FLT4", 4) ||
      !memcmp(t, "FLT8", 4) || !memcmp(t, "4CHN", 4) ||
      !memcmp(t, "6CHN", 4) || !memcmp(t, "8CHN", 4))
    return 1;
  /* "xxCH"/"xxCN" multi-channel tags (e.g. 16CH, 32CH) */
  if ((t[2] == 'C' && t[3] == 'H') || (t[2] == 'C' && t[3] == 'N'))
    return 1;
  return 0;
}

static const void *mod_registersong(const void *data, unsigned len)
{
  if (!mod_ctx || !mod_is_module((const unsigned char *)data, len))
    return NULL;
  /* pocketmod_init copies what it needs by reference; the lump persists
   * until unregistersong, matching the interface contract. */
  if (!pocketmod_init(mod_ctx, data, (int)len, mod_rate))
  {
    lprintf(LO_WARN, "mod_registersong: pocketmod_init failed\n");
    return NULL;
  }
  mod_data    = data;
  mod_len     = (int)len;
  mod_playing = 0;
  mod_paused  = 0;
  /* a non-NULL handle: use the lump pointer as the opaque handle */
  return data;
}

static void mod_unregistersong(const void *handle)
{
  (void)handle;
  mod_data    = NULL;
  mod_playing = 0;
}

static void mod_play(const void *handle, int looping)
{
  (void)handle;
  mod_looping = looping;
  mod_playing = 1;
  mod_paused  = 0;
  /* restart from the top */
  if (mod_ctx && mod_data)
    pocketmod_init(mod_ctx, mod_data, mod_len, mod_rate);
}

static void mod_stop(void)
{
  mod_playing = 0;
}

static void mod_render(void *dest, unsigned nsamp)
{
  short *out = (short *)dest;

  if (!mod_playing || mod_paused || !mod_ctx)
  {
    memset(dest, 0, nsamp * 4);   /* s16 stereo: 4 bytes per frame */
    return;
  }

  while (nsamp > 0)
  {
    unsigned want = nsamp > MOD_SCRATCH_FRAMES ? MOD_SCRATCH_FRAMES : nsamp;
    int      got_bytes;
    unsigned got_frames, i;

    got_bytes = pocketmod_render(mod_ctx, mod_scratch,
                                 (int)(want * 2 * sizeof(float)));
    got_frames = (unsigned)got_bytes / (2 * sizeof(float));

    if (got_frames == 0)
    {
      /* end of module: loop or fall silent */
      if (mod_looping && mod_data)
      {
        pocketmod_init(mod_ctx, mod_data, mod_len, mod_rate);
        continue;
      }
      memset(out, 0, nsamp * 4);
      mod_playing = 0;
      return;
    }

    for (i = 0; i < got_frames; i++)
    {
      /* scale by the 0..15 music volume, clamp to s16 */
      float l = mod_scratch[i * 2 + 0] * (float)mod_volume / 15.0f;
      float r = mod_scratch[i * 2 + 1] * (float)mod_volume / 15.0f;
      int   li = (int)(l * 32767.0f);
      int   ri = (int)(r * 32767.0f);
      if (li >  32767) li =  32767;
      if (li < -32768) li = -32768;
      if (ri >  32767) ri =  32767;
      if (ri < -32768) ri = -32768;
      *out++ = (short)li;
      *out++ = (short)ri;
    }

    nsamp -= got_frames;
  }
}

/* Save/restore playback position so a save state (and runahead and rewind,
 * which save and restore every frame) resumes tracker music where it was
 * instead of forcing the generic music layer's render-replay -- which for a
 * module re-runs the whole song from the start on every restore.
 *
 * pocketmod_context is mostly read-only song data (sample/pattern pointers and
 * the per-sample tables) that pocketmod_init rebuilds deterministically from
 * the lump, plus the 32-entry channel array (1152 bytes) and a handful of
 * scalar position/timing fields.  Saving the whole 1.7KB context would not fit
 * the fixed music-state budget, so only the dynamic state is serialized: the
 * scalar position/timing/loop/LFO fields and just the channels actually in use
 * (num_channels of them).  On restore we pocketmod_init from the lump to
 * rebuild the read-only data and pointers, then splice the saved dynamic state
 * back in.  The result resumes sample-exact.
 *
 * Wire format (little-endian-host, matching the other backends):
 *   uint32_t magic    = 'MODS'
 *   uint32_t version  = 1
 *   uint32_t looping
 *   uint32_t num_channels       (sanity: must match the re-init'd song)
 *   -- scalar dynamic state --
 *   float    samples_per_tick
 *   int32_t  ticks_per_line
 *   uint8_t  visited[16]
 *   int32_t  loop_count
 *   uint8_t  pattern_delay
 *   uint32_t lfo_rng
 *   int8_t   pattern, line
 *   int16_t  tick
 *   float    sample
 *   -- per-channel state --
 *   _pocketmod_chan channels[num_channels] */
#define MOD_STATE_MAGIC   0x4D4F4453u  /* 'MODS' */
#define MOD_STATE_VERSION 1u

typedef struct {
  float          samples_per_tick;
  int            ticks_per_line;   /* 32-bit on every MSVC target          */
  unsigned char  visited[16];
  int            loop_count;
  unsigned char  pattern_delay;
  unsigned int   lfo_rng;          /* 32-bit                                */
  signed char    pattern;
  signed char    line;
  short          tick;             /* 16-bit                                */
  float          sample;
} mod_state_scalars_t;

static size_t mod_serialize(void *dest, size_t cap)
{
  unsigned int hdr[4];
  mod_state_scalars_t sc;
  unsigned nch;
  size_t   need;

  if (!mod_ctx || !mod_data || !mod_playing)
    return 0;                  /* nothing playing -> no state to record */
  nch  = mod_ctx->num_channels;
  if (nch == 0 || nch > POCKETMOD_MAX_CHANNELS)
    return 0;
  need = sizeof hdr + sizeof sc + nch * sizeof(_pocketmod_chan);
  if (!dest)
    return need;               /* size-query mode */
  if (cap < need)
    return 0;                  /* won't fit the budget -> defer to replay */

  hdr[0] = MOD_STATE_MAGIC;
  hdr[1] = MOD_STATE_VERSION;
  hdr[2] = (unsigned int)(mod_looping ? 1 : 0);
  hdr[3] = (unsigned int)nch;

  sc.samples_per_tick = mod_ctx->samples_per_tick;
  sc.ticks_per_line   = (int)mod_ctx->ticks_per_line;
  memcpy(sc.visited, mod_ctx->visited, sizeof sc.visited);
  sc.loop_count       = (int)mod_ctx->loop_count;
  sc.pattern_delay    = mod_ctx->pattern_delay;
  sc.lfo_rng          = mod_ctx->lfo_rng;
  sc.pattern          = mod_ctx->pattern;
  sc.line             = mod_ctx->line;
  sc.tick             = mod_ctx->tick;
  sc.sample           = mod_ctx->sample;

  memcpy(dest, hdr, sizeof hdr);
  memcpy((unsigned char *)dest + sizeof hdr, &sc, sizeof sc);
  memcpy((unsigned char *)dest + sizeof hdr + sizeof sc,
         mod_ctx->channels, nch * sizeof(_pocketmod_chan));
  return need;
}

static int mod_unserialize(const void *src, size_t size)
{
  unsigned int hdr[4];
  mod_state_scalars_t sc;
  const unsigned char *p;
  unsigned nch;

  if (!mod_ctx || !mod_data)                          return 0;
  if (size < sizeof hdr + sizeof sc)                  return 0;
  memcpy(hdr, src, sizeof hdr);
  if (hdr[0] != MOD_STATE_MAGIC)                      return 0;
  if (hdr[1] != MOD_STATE_VERSION)                    return 0;
  nch = hdr[3];
  if (nch == 0 || nch > POCKETMOD_MAX_CHANNELS)       return 0;
  if (size < sizeof hdr + sizeof sc + nch * sizeof(_pocketmod_chan)) return 0;

  /* Rebuild read-only song data + pointers from the same lump. */
  if (!pocketmod_init(mod_ctx, mod_data, mod_len, mod_rate))
    return 0;                  /* can't resume -> defer to render-replay */
  if (mod_ctx->num_channels != nch)
    return 0;                  /* state was taken under a different module */

  memcpy(&sc, (const unsigned char *)src + sizeof hdr, sizeof sc);
  mod_ctx->samples_per_tick = sc.samples_per_tick;
  mod_ctx->ticks_per_line   = (int)sc.ticks_per_line;
  memcpy(mod_ctx->visited, sc.visited, sizeof mod_ctx->visited);
  mod_ctx->loop_count       = (int)sc.loop_count;
  mod_ctx->pattern_delay    = sc.pattern_delay;
  mod_ctx->lfo_rng          = sc.lfo_rng;
  mod_ctx->pattern          = sc.pattern;
  mod_ctx->line             = sc.line;
  mod_ctx->tick             = sc.tick;
  mod_ctx->sample           = sc.sample;

  p = (const unsigned char *)src + sizeof hdr + sizeof sc;
  memcpy(mod_ctx->channels, p, nch * sizeof(_pocketmod_chan));

  mod_looping = (hdr[2] != 0);
  mod_playing = 1;
  mod_paused  = 0;
  return 1;
}

const music_player_t mod_player =
{
  mod_name,
  mod_init,
  mod_shutdown,
  mod_setvolume,
  mod_pause,
  mod_resume,
  mod_registersong,
  mod_unregistersong,
  mod_play,
  mod_stop,
  mod_render,
  mod_serialize,
  mod_unserialize
};
