/* Ogg Vorbis music player backend.
 *
 * Many modern mods ship their music as Ogg Vorbis (.ogg) lumps under music/
 * rather than MIDI/MUS, MP3 or tracker modules.  This wraps the core's private
 * stb_vorbis decoder (deps/stb/stb_vorbis_impl.c, exported with a prb_ prefix
 * so it does not clash with the host RetroArch's own copy) in the engine's
 * music_player_t interface, so an Ogg lump autodetects and plays alongside the
 * other backends.
 *
 * stb_vorbis decodes interleaved signed-16 stereo at the stream's own sample
 * rate; the engine mixer wants interleaved signed-16 stereo at the rate passed
 * to init().  render() pulls a burst of source frames and linearly resamples
 * them to the output rate (a 16.16 fractional step, the same scheme the sfx
 * loader uses), so any Ogg sample rate plays at the correct pitch.  Looping
 * seeks the decoder back to the start. */

#include <stdlib.h>
#include <string.h>

#include "lprintf.h"
#include "musicplayer.h"

/* stb_vorbis (prb_-prefixed; see deps/stb/stb_vorbis_impl.c).  Only the few
 * entry points the streaming music path needs are declared here. */
typedef struct stb_vorbis stb_vorbis;
typedef struct
{
  unsigned int sample_rate;
  int          channels;
  unsigned int setup_memory_required;
  unsigned int setup_temp_memory_required;
  unsigned int temp_memory_required;
  int          max_frame_size;
} prb_stb_vorbis_info;

extern stb_vorbis *prb_stb_vorbis_open_memory(const unsigned char *data, int len,
                                              int *error, void *alloc);
extern prb_stb_vorbis_info prb_stb_vorbis_get_info(stb_vorbis *f);
extern int  prb_stb_vorbis_get_samples_short_interleaved(stb_vorbis *f, int channels,
                                                         short *buffer, int num_shorts);
extern int  prb_stb_vorbis_seek_start(stb_vorbis *f);
extern void prb_stb_vorbis_close(stb_vorbis *f);

static stb_vorbis  *ogg_v;
static const void  *ogg_data;       /* the lump (owned by the caller) */
static int          ogg_len;
static int          ogg_rate;       /* engine output rate */
static int          ogg_src_rate;   /* stream's native rate */
static int          ogg_channels;
static int          ogg_volume;     /* 0..15 */
static int          ogg_looping;
static int          ogg_playing;
static int          ogg_paused;

/* resampler state: source frames buffered, and the 16.16 read cursor */
#define OGG_SRC_FRAMES 4096
static short        ogg_src[OGG_SRC_FRAMES * 2]; /* interleaved s16 stereo src */
static int          ogg_src_have;   /* frames currently in ogg_src           */
static int          ogg_src_pos;    /* next unconsumed source frame          */
static unsigned     ogg_frac;       /* 16.16 fractional position within src  */
static unsigned     ogg_step;       /* 16.16 src-frames per output frame      */

static const char *ogg_name(void)
{
  return "stb_vorbis Ogg player";
}

static int ogg_init(int samplerate)
{
  ogg_rate = samplerate;
  return 1;
}

static void ogg_close_stream(void)
{
  if (ogg_v)
  {
    prb_stb_vorbis_close(ogg_v);
    ogg_v = NULL;
  }
}

static void ogg_shutdown(void)
{
  ogg_close_stream();
  ogg_data    = NULL;
  ogg_playing = 0;
}

static void ogg_setvolume(int v)
{
  ogg_volume = v;
}

static void ogg_pause(void)
{
  ogg_paused = 1;
}

static void ogg_resume(void)
{
  ogg_paused = 0;
}

static const void *ogg_registersong(const void *data, unsigned len)
{
  const unsigned char *d = (const unsigned char *)data;
  prb_stb_vorbis_info info;
  int err = 0;

  /* Ogg streams start with the "OggS" capture pattern. */
  if (len < 4 || d[0] != 'O' || d[1] != 'g' || d[2] != 'g' || d[3] != 'S')
    return NULL;

  ogg_close_stream();
  ogg_v = prb_stb_vorbis_open_memory(d, (int)len, &err, NULL);
  if (!ogg_v)
    return NULL;

  info         = prb_stb_vorbis_get_info(ogg_v);
  ogg_src_rate = info.sample_rate ? (int)info.sample_rate : ogg_rate;
  ogg_channels = info.channels > 0 ? info.channels : 2;
  ogg_step     = (unsigned)(((unsigned long long)ogg_src_rate << 16) /
                            (unsigned long long)(ogg_rate ? ogg_rate : 44100));
  ogg_data     = data;
  ogg_len      = (int)len;
  ogg_playing  = 0;
  ogg_paused   = 0;
  ogg_src_have = 0;
  ogg_src_pos  = 0;
  ogg_frac     = 0;
  return data;
}

static void ogg_unregistersong(const void *handle)
{
  (void)handle;
  ogg_close_stream();
  ogg_data    = NULL;
  ogg_playing = 0;
}

static void ogg_play(const void *handle, int looping)
{
  (void)handle;
  ogg_looping = looping;
  ogg_playing = 1;
  ogg_paused  = 0;
  if (ogg_v)
    prb_stb_vorbis_seek_start(ogg_v);
  ogg_src_have = 0;
  ogg_src_pos  = 0;
  ogg_frac     = 0;
}

static void ogg_stop(void)
{
  ogg_playing = 0;
}

/* Refill ogg_src with up to OGG_SRC_FRAMES interleaved-stereo source frames.
 * Returns the number of frames available (0 at end of stream when not
 * looping).  stb_vorbis returns however many it decoded; on 0 we either seek
 * back (loop) or report end. */
static int ogg_fill(void)
{
  int got;
  ogg_src_pos = 0;
  ogg_src_have = 0;
  if (!ogg_v)
    return 0;
  got = prb_stb_vorbis_get_samples_short_interleaved(ogg_v, 2, ogg_src,
                                                     OGG_SRC_FRAMES * 2);
  if (got <= 0)
  {
    if (ogg_looping)
    {
      prb_stb_vorbis_seek_start(ogg_v);
      got = prb_stb_vorbis_get_samples_short_interleaved(ogg_v, 2, ogg_src,
                                                         OGG_SRC_FRAMES * 2);
    }
    if (got <= 0)
      return 0;
  }
  ogg_src_have = got;   /* get_samples_short_interleaved returns frames */
  return got;
}

static void ogg_render(void *dest, unsigned nsamp)
{
  short *out = (short *)dest;
  int    vol = ogg_volume;

  if (!ogg_playing || ogg_paused || !ogg_v)
  {
    memset(dest, 0, nsamp * 4);   /* s16 stereo: 4 bytes per frame */
    return;
  }

  while (nsamp > 0)
  {
    int base;
    /* ensure the current and next source frame are available */
    while (ogg_src_pos + (int)(ogg_frac >> 16) + 1 >= ogg_src_have)
    {
      /* carry any leftover position past the consumed frames, then refill */
      int consumed = ogg_src_pos + (int)(ogg_frac >> 16);
      if (consumed > 0 && consumed < ogg_src_have)
      {
        int rem = ogg_src_have - consumed, i;
        for (i = 0; i < rem * 2; i++)
          ogg_src[i] = ogg_src[consumed * 2 + i];
        ogg_src_have = rem;
        ogg_src_pos  = 0;
        ogg_frac    &= 0xFFFF;
      }
      else if (consumed >= ogg_src_have)
      {
        ogg_src_pos = 0;
        ogg_frac   &= 0xFFFF;
        if (!ogg_fill())
        {
          memset(out, 0, nsamp * 4);
          ogg_playing = 0;
          return;
        }
      }
      else
      {
        if (!ogg_fill())
        {
          memset(out, 0, nsamp * 4);
          ogg_playing = 0;
          return;
        }
      }
    }

    base = (ogg_src_pos + (int)(ogg_frac >> 16)) * 2;
    {
      /* linear interpolate between frame `base` and the next */
      unsigned f = ogg_frac & 0xFFFF;
      int l0 = ogg_src[base + 0], r0 = ogg_src[base + 1];
      int l1, r1, l, r;
      int nbase = base + 2;
      if ((nbase >> 1) < ogg_src_have) { l1 = ogg_src[nbase + 0]; r1 = ogg_src[nbase + 1]; }
      else                             { l1 = l0; r1 = r0; }
      l = (int)(l0 + (((l1 - l0) * (int)f) >> 16));
      r = (int)(r0 + (((r1 - r0) * (int)f) >> 16));
      l = l * vol / 15;
      r = r * vol / 15;
      if (l >  32767) l =  32767; else if (l < -32768) l = -32768;
      if (r >  32767) r =  32767; else if (r < -32768) r = -32768;
      *out++ = (short)l;
      *out++ = (short)r;
    }
    ogg_frac += ogg_step;
    nsamp--;
  }
}

const music_player_t ogg_player =
{
  ogg_name,
  ogg_init,
  ogg_shutdown,
  ogg_setvolume,
  ogg_pause,
  ogg_resume,
  ogg_registersong,
  ogg_unregistersong,
  ogg_play,
  ogg_stop,
  ogg_render,
  NULL,   /* serialize   -- stream position state not implemented */
  NULL    /* unserialize */
};
