/* Tracker module music player backend (MOD/S3M/XM).
 *
 * Some mods ship their music as tracker modules under music/ rather than
 * MIDI/MUS, MP3 or Ogg.  This wraps libretro-common's rmodtracker
 * replayer (libretro/libretro-common/formats/mod/rmodtracker.c) in the
 * engine's music_player_t interface, replacing the pocketmod-based
 * ProTracker player.  rmodtracker autodetects MOD, S3M and XM, so the
 * backend now also plays the Scream Tracker 3 and FastTracker 2 modules
 * ZDoom-family mods commonly carry, which pocketmod could not.
 *
 * The replayer synthesises interleaved signed-16 stereo directly at
 * whatever rate it is opened with, so the module is opened at the
 * engine's mix rate and render() decodes straight into the output with
 * no resampling stage.  The fixed-point pipeline is bit-identical
 * across compilers and architectures; the float lane runs the same
 * integer sequencer with float sample arithmetic.  The 0..15 music
 * volume is applied at the copy, scaling down only, so no re-clamp is
 * needed on either lane.
 *
 * Save states record the playback position in frames since the start
 * of the sequence ('MODS' v3).  rmodtracker_seek() restores it
 * exactly: a module's state at any moment is the deterministic result
 * of every row played before it, and the replayer walks there with the
 * mixing skipped, dropping bounded snapshots on the first walk so
 * runahead/rewind's per-frame restores cost at most a few seconds of
 * sequencer stepping after the first.  This replaces pocketmod's
 * channel-array state splice, which required reaching into the decoder
 * struct; the wire format necessarily changes with the decoder
 * (pocketmod state cannot resume an rmodtracker context), so v2 states
 * fall back to the generic layer's render-replay, which remains
 * correct. */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "lprintf.h"
#include "musicplayer.h"

#include <formats/rmodtracker.h>

static rmodtracker *mod_rmt;
static const void  *mod_data;   /* the lump (owned by the caller) */
static int          mod_len;
static int          mod_rate;
static int          mod_volume;  /* 0..15 */
static int          mod_looping;
static int          mod_playing;
static int          mod_paused;
static int          mod_pos;     /* frames since sequence start (mix rate) */
static int          mod_dur;     /* frames in one pass of the sequence    */

static const char *mod_name(void)
{
  return "rmodtracker module player (MOD/S3M/XM)";
}

static int mod_init(int samplerate)
{
  mod_rate = samplerate;
  if (mod_rate < RMODTRACKER_RATE_MIN) mod_rate = RMODTRACKER_RATE_MIN;
  if (mod_rate > RMODTRACKER_RATE_MAX) mod_rate = RMODTRACKER_RATE_MAX;
  return 1;
}

static void mod_close(void)
{
  if (mod_rmt)
  {
    rmodtracker_close(mod_rmt);
    mod_rmt = NULL;
  }
}

static void mod_shutdown(void)
{
  mod_close();
  mod_data    = NULL;
  mod_playing = 0;
}

static void mod_setvolume(int v)
{
  mod_volume = v < 0 ? 0 : (v > 15 ? 15 : v);
}

static void mod_pause(void)
{
  mod_paused = 1;
}

static void mod_resume(void)
{
  mod_paused = 0;
}

/* Cheap signature pre-check so the (copying) open is only attempted on
 * plausible module data.  MOD carries a 4-byte tag at offset 1080
 * ("M.K.", "M!K!", "FLTx", "xCHN", "xxCH"...), S3M the "SCRM" magic at
 * offset 44, XM the fixed 17-byte "Extended Module: " prefix.
 * rmodtracker's own parser remains the authority: anything passing
 * here that it cannot parse is still rejected at open. */
static int mod_is_module(const unsigned char *d, unsigned len)
{
  const unsigned char *t;
  if (len >= 17 && !memcmp(d, "Extended Module: ", 17))
    return 1;                                     /* XM  */
  if (len >= 48 && !memcmp(d + 44, "SCRM", 4))
    return 1;                                     /* S3M */
  if (len < 1084)
    return 0;
  t = d + 1080;                                   /* MOD */
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
  if (!mod_is_module((const unsigned char *)data, len))
    return NULL;

  mod_close();
  mod_rmt = rmodtracker_open_memory_rate(data, len, mod_rate);
  if (!mod_rmt)
  {
    lprintf(LO_WARN, "mod_registersong: rmodtracker rejected the module\n");
    return NULL;
  }
  mod_dur     = rmodtracker_duration_frames(mod_rmt);
  mod_data    = data;
  mod_len     = (int)len;
  mod_playing = 0;
  mod_paused  = 0;
  mod_pos     = 0;
  /* a non-NULL handle: use the lump pointer as the opaque handle */
  return data;
}

static void mod_unregistersong(const void *handle)
{
  (void)handle;
  mod_close();
  mod_data    = NULL;
  mod_playing = 0;
}

static void mod_play(const void *handle, int looping)
{
  (void)handle;
  mod_looping = looping;
  mod_playing = 1;
  mod_paused  = 0;
  mod_pos     = 0;
  if (mod_rmt)
    rmodtracker_rewind(mod_rmt);
}

static void mod_stop(void)
{
  mod_playing = 0;
}

/* Shared render body: pull frames from the replayer, apply the 0..15
 * volume at the copy (scaling down only, so no re-clamp), loop or fall
 * silent at the end of the module.  is_float selects the lane. */
static void mod_render_core(void *dest, unsigned nsamp, int is_float)
{
  short *sout = (short *)dest;
  float *fout = (float *)dest;
  /* Per-burst scratch at the replayer's own output layout. */
  static int16_t s_tmp[1024 * 2];
  static float   f_tmp[1024 * 2];

  if (!mod_playing || mod_paused || !mod_rmt)
  {
    memset(dest, 0, (size_t)nsamp * (is_float ? 8 : 4));
    return;
  }

  while (nsamp > 0)
  {
    unsigned want = nsamp > 1024 ? 1024 : nsamp, got, i;

    /* A module's sequence loops by construction (the order table wraps),
     * so the replayer keeps producing forever and a short read only
     * happens for modules that genuinely halt (a stop effect).  "End of
     * the song" for the non-looping case is therefore one pass through
     * the sequence, counted here against the replayer's own duration. */
    if (!mod_looping && mod_dur > 0)
    {
      if (mod_pos >= mod_dur)
      {
        memset(is_float ? (void *)fout : (void *)sout, 0,
               (size_t)nsamp * (is_float ? 8 : 4));
        mod_playing = 0;
        return;
      }
      if (want > (unsigned)(mod_dur - mod_pos))
        want = (unsigned)(mod_dur - mod_pos);
    }

    if (is_float)
      got = (unsigned)rmodtracker_get_samples_float_interleaved(mod_rmt,
            f_tmp, want);
    else
      got = (unsigned)rmodtracker_get_samples_s16_interleaved(mod_rmt,
            s_tmp, want);

    if (got == 0)
    {
      /* end of module: loop or fall silent */
      if (mod_looping)
      {
        rmodtracker_rewind(mod_rmt);
        mod_pos = 0;
        continue;
      }
      memset(is_float ? (void *)fout : (void *)sout, 0,
             (size_t)nsamp * (is_float ? 8 : 4));
      mod_playing = 0;
      return;
    }

    if (is_float)
    {
      const float g = (float)mod_volume * (1.0f / 15.0f);
      for (i = 0; i < got * 2; i++)
        *fout++ = f_tmp[i] * g;
    }
    else
    {
      const int vol = mod_volume;
      for (i = 0; i < got * 2; i++)
        *sout++ = (short)((int)s_tmp[i] * vol / 15);
    }

    mod_pos += (int)got;
    if (mod_looping && mod_dur > 0 && mod_pos >= mod_dur)
      mod_pos -= mod_dur;              /* the sequence looped internally */
    nsamp -= got;
  }
}

static void mod_render(void *dest, unsigned nsamp)
{
  mod_render_core(dest, nsamp, 0);
}

static void mod_render_float(void *dest, unsigned nsamp)
{
  mod_render_core(dest, nsamp, 1);
}

/* Save/restore playback position so a save state (and runahead and
 * rewind, which save and restore every frame) resumes tracker music
 * where it was instead of forcing the generic music layer's
 * render-replay.
 *
 * Wire format (little-endian-host, matching the other backends):
 *   uint32_t magic    = 'MODS'
 *   uint32_t version  = 3        (v1/v2 were pocketmod state splices;
 *                                 those states defer to render-replay)
 *   uint32_t looping
 *   int32_t  position            (frames since sequence start)       */
#define MOD_STATE_MAGIC   0x4D4F4453u  /* 'MODS' */
#define MOD_STATE_VERSION 3u

static size_t mod_serialize(void *dest, size_t cap)
{
  uint32_t hdr[3];
  int32_t  pos;
  size_t   need = sizeof hdr + sizeof pos;

  if (!mod_rmt || !mod_data || !mod_playing)
    return 0;                  /* nothing playing -> no state to record */
  if (!dest)
    return need;               /* size-query mode */
  if (cap < need)
    return 0;

  hdr[0] = MOD_STATE_MAGIC;
  hdr[1] = MOD_STATE_VERSION;
  hdr[2] = (uint32_t)(mod_looping ? 1 : 0);
  pos    = (int32_t)mod_pos;
  memcpy(dest, hdr, sizeof hdr);
  memcpy((unsigned char *)dest + sizeof hdr, &pos, sizeof pos);
  return need;
}

static int mod_unserialize(const void *src, size_t size)
{
  uint32_t hdr[3];
  int32_t  pos;

  if (!mod_rmt || !mod_data)                          return 0;
  if (size < sizeof hdr + sizeof pos)                 return 0;
  memcpy(hdr, src, sizeof hdr);
  if (hdr[0] != MOD_STATE_MAGIC)                      return 0;
  if (hdr[1] != MOD_STATE_VERSION)                    return 0;
  memcpy(&pos, (const unsigned char *)src + sizeof hdr, sizeof pos);
  if (pos < 0)                                        return 0;
  if (mod_dur > 0 && pos >= mod_dur)                  return 0;

  mod_pos     = rmodtracker_seek(mod_rmt, (int)pos);
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
  mod_unserialize,
  mod_render_float
};
