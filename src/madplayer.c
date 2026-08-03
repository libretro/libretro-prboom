/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *
 *  Copyright (C) 2011 by
 *  Nicholai Main
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
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 *  02111-1307, USA.
 *
 * DESCRIPTION:
 *
 *  MP3 music player backend over libretro-common's rmp3 decoder
 *  (libretro/libretro-common/formats/mp3/rmp3.c), replacing libmad.
 *  The file keeps its historical name so the surrounding build and
 *  the mp_player references stay put.
 *
 *  rmp3's pull API borrows the whole lump and keeps a byte cursor into
 *  it, which is exactly the shape the old libmad wrapper had.  The two
 *  output pipelines match the engine's two render lanes directly:
 *  rmp3_read_s16 synthesises in Q28 fixed point straight to s16, and
 *  rmp3_read_f32 stays float end to end, so each lane pays no format
 *  round-trip on top of the decoder.
 *
 *---------------------------------------------------------------------
 */

#include "config.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "lprintf.h"
#include "musicplayer.h"

#include <formats/rmp3.h>


static rmp3       *mp_m;          /* heap: rmp3 embeds ~30 KB of decoder */

static int mp_looping = 0;
static int mp_volume = 0;         /* 0-15 */
static int mp_samplerate_target = 0;
static int mp_paused = 0;
static int mp_playing = 0;

static const void *mp_data;
static int mp_len;

/* Save-state cursor.  rmp3 has no sample-accurate O(1) seek (MP3 frames
 * lean on the bit reservoir of their predecessors, so rmp3_seek_to_frame
 * decodes from the top), which is far too slow for runahead's
 * per-frame restores.  Instead the wrapper tracks the lump byte offset
 * at which the decoder began locating the block it is currently
 * draining; a restore re-initialises rmp3 on the lump's tail from that
 * offset and discards the intra-block frames already consumed.  Same
 * scheme, and the same inaudible one-frame bit-reservoir transient
 * (~26ms), as the old libmad re-buffer-at-frame restore.
 *
 * mp_base_off is the lump offset rmp3's buffer currently starts at (0
 * normally, the restored offset after an unserialize), so lump offsets
 * are always mp_base_off + readPos. */
static int mp_base_off;           /* lump offset of rmp3's buffer start  */
static int mp_frame_off;          /* lump offset where the current block's
                                     sync search began                   */

static const char *mp_name (void)
{
  return "rmp3 mp3 player";
}

static int mp_init (int samplerate)
{
  mp_samplerate_target = samplerate;
  if (!mp_m)
    mp_m = (rmp3 *)malloc(sizeof(rmp3));
  return mp_m != NULL;
}

static void mp_shutdown (void)
{
  if (mp_m)
  {
    rmp3_uninit(mp_m);
    free(mp_m);
    mp_m = NULL;
  }
  mp_data    = NULL;
  mp_playing = 0;
}

/* ---- MP3 stream sniffing ------------------------------------------------
 *
 * The MP3 standard has no file header; the only signature is the frame
 * sync itself, and 11 set bits arise in random data all the time.  The
 * old libmad wrapper demanded that 80% of up to 100 header probes
 * succeed; this parser is stricter: it requires a chain of consecutive
 * frames, each header found exactly where the previous frame's computed
 * length says it must be.  A false sync fails the chain immediately,
 * because random bytes do not contain back-to-back consistent headers.
 */

static const short mp_br_v1[3][16] = {   /* MPEG1, layer I/II/III, kbps */
  { 0,32,64,96,128,160,192,224,256,288,320,352,384,416,448,0 },
  { 0,32,48,56, 64, 80, 96,112,128,160,192,224,256,320,384,0 },
  { 0,32,40,48, 56, 64, 80, 96,112,128,160,192,224,256,320,0 },
};
static const short mp_br_v2[3][16] = {   /* MPEG2/2.5 */
  { 0,32,48,56, 64, 80, 96,112,128,144,160,176,192,224,256,0 },
  { 0, 8,16,24, 32, 40, 48, 56, 64, 80, 96,112,128,144,160,0 },
  { 0, 8,16,24, 32, 40, 48, 56, 64, 80, 96,112,128,144,160,0 },
};
static const int mp_sr_tab[4][3] = {     /* by version bits */
  { 11025, 12000,  8000 },   /* 00: MPEG2.5 */
  { 0, 0, 0 },               /* 01: reserved */
  { 22050, 24000, 16000 },   /* 10: MPEG2   */
  { 44100, 48000, 32000 },   /* 11: MPEG1   */
};

/* Parse the 4-byte header at d; return the whole frame's byte length,
 * or 0 if it is not a valid, fixed-bitrate frame header.  (Free-format
 * streams -- bitrate index 0 -- are legal MP3 but carry no length in
 * the header; they are vanishingly rare in the wild and are simply not
 * matched by the sniffer.  rmp3 itself still decodes them if some
 * other evidence routes the lump here.) */
static int mp_frame_len (const unsigned char *d)
{
  int ver, layer, br_idx, sr_idx, pad, br, sr;

  if (d[0] != 0xFF || (d[1] & 0xE0) != 0xE0)
    return 0;
  ver    = (d[1] >> 3) & 3;          /* 0=2.5, 2=2, 3=1        */
  layer  = (d[1] >> 1) & 3;          /* 3=I, 2=II, 1=III       */
  br_idx = (d[2] >> 4) & 15;
  sr_idx = (d[2] >> 2) & 3;
  pad    = (d[2] >> 1) & 1;
  if (ver == 1 || layer == 0 || br_idx == 0 || br_idx == 15 || sr_idx == 3)
    return 0;
  sr = mp_sr_tab[ver][sr_idx];
  br = (ver == 3 ? mp_br_v1 : mp_br_v2)[3 - layer][br_idx] * 1000;
  if (layer == 3)                    /* layer I */
    return (12 * br / sr + pad) * 4;
  if (layer == 1 && ver != 3)        /* layer III, MPEG2/2.5 */
    return 72 * br / sr + pad;
  return 144 * br / sr + pad;        /* layer II, and layer III MPEG1 */
}

/* Frames a valid chain must run before the lump is believed to be MP3
 * (or until it cleanly reaches the end of the lump while chained). */
#define MP_SNIFF_CHAIN 6
/* How far into the lump the first sync may sit (past any ID3v2 tag,
 * which is skipped by its own stated size first). */
#define MP_SNIFF_RANGE 4096

static int mp_is_mp3 (const unsigned char *d, unsigned len)
{
  unsigned pos = 0, start, limit;

  /* ID3v2: "ID3", 2 version bytes, 1 flag byte, 4-byte synchsafe size. */
  if (len >= 10 && d[0]=='I' && d[1]=='D' && d[2]=='3' &&
      !((d[6]|d[7]|d[8]|d[9]) & 0x80))
  {
    unsigned tag = ((unsigned)(d[6] & 0x7F) << 21) |
                   ((unsigned)(d[7] & 0x7F) << 14) |
                   ((unsigned)(d[8] & 0x7F) <<  7) |
                    (unsigned)(d[9] & 0x7F);
    if (10 + tag < len)
      pos = 10 + tag;
  }

  limit = pos + MP_SNIFF_RANGE;
  if (limit > len)
    limit = len;

  for (start = pos; start + 4 <= limit; start++)
  {
    unsigned p = start;
    int chained = 0;
    while (p + 4 <= len)
    {
      int fl = mp_frame_len(d + p);
      if (fl <= 0)
        break;
      chained++;
      p += (unsigned)fl;
      if (chained >= MP_SNIFF_CHAIN)
        return 1;
    }
    /* A shorter chain that runs cleanly off the end of the lump (small
     * lump, or a truncated final frame) still counts as MP3. */
    if (chained >= 2 && p + 4 > len)
      return 1;
  }
  return 0;
}

static const void *mp_registersong (const void *data, unsigned len)
{
  if (!mp_m || len < 4)
    return NULL;

  if (!mp_is_mp3((const unsigned char *)data, len))
    return NULL;

  /* rmp3_init_memory validates by decoding the first frame header. */
  if (!rmp3_init_memory(mp_m, data, len))
  {
    lprintf (LO_WARN, "mp_registersong: rmp3_init_memory failed\n");
    return NULL;
  }

  lprintf (LO_INFO, "mp_registersong succeed. samplerate %u channels %u\n",
           (unsigned)mp_m->sampleRate, (unsigned)mp_m->channels);

  mp_data     = data;
  mp_len      = (int)len;
  mp_base_off = 0;
  mp_frame_off= 0;
  /* handle not used */
  return data;
}

static void mp_setvolume (int v)
{
  mp_volume = v;
}

static void mp_pause (void)
{
  mp_paused = 1;
}

static void mp_resume (void)
{
  mp_paused = 0;
}

static void mp_unregistersong (const void *handle)
{ /* the lump is owned by the caller; rmp3 borrows it */
  (void)handle;
  mp_data = NULL;
  mp_playing = 0;
}

static void mp_play (const void *handle, int looping)
{
  (void)handle;
  if (mp_m && mp_data)
    rmp3_init_memory(mp_m, mp_data, (size_t)mp_len);
  mp_base_off  = 0;
  mp_frame_off = 0;
  mp_playing = 1;
  mp_looping = looping;
}

static void mp_stop (void)
{
  mp_playing = 0;
}

/* Rewind the decoder to the true start of the lump (looping). */
static int mp_rewind (void)
{
  if (!mp_m || !mp_data)
    return 0;
  if (!rmp3_init_memory(mp_m, mp_data, (size_t)mp_len))
    return 0;
  mp_base_off  = 0;
  mp_frame_off = 0;
  return 1;
}

/* Pull exactly 'want' stereo frames at the stream's native rate into
 * dest (s16 or float lane), applying the 0..15 volume and duplicating
 * mono, looping at end of stream when asked.  Returns 0 when the
 * stream ended and looping is off (dest is zero-filled from there).
 *
 * Reads are capped so a single rmp3_read never crosses into a new MP3
 * block: whenever the buffered block is spent, the lump offset the
 * decoder will search from is recorded (mp_frame_off) and exactly one
 * frame is pulled to decode the next block.  That bounded extra call
 * per ~1152 frames is what keeps the serialize cursor (block offset +
 * intra-block position) exact at all times. */
static int mp_pull (void *dest, unsigned want, int is_float)
{
  short *sout = (short *)dest;
  float *fout = (float *)dest;
  /* One MPEG block is at most 1152 frames per granule pair; a static
   * scratch at the decoder's own max keeps the copy loop simple. */
  static int16_t s_tmp[RMP3_MAX_SAMPLES_PER_FRAME];
  static float   f_tmp[RMP3_MAX_SAMPLES_PER_FRAME];

  while (want > 0)
  {
    unsigned n, got, i, ch;

    if (mp_m->framesRemaining == 0)
      mp_frame_off = mp_base_off + (int)mp_m->readPos;

    n = mp_m->framesRemaining ? mp_m->framesRemaining : 1;
    if (n > want)
      n = want;
    ch = mp_m->channels == 1 ? 1 : 2;
    /* cap by scratch capacity in samples */
    if (n * ch > RMP3_MAX_SAMPLES_PER_FRAME)
      n = RMP3_MAX_SAMPLES_PER_FRAME / ch;

    if (is_float)
      got = (unsigned)rmp3_read_f32(mp_m, n, f_tmp);
    else
      got = (unsigned)rmp3_read_s16(mp_m, n, s_tmp);

    if (got == 0)
    {
      if (mp_looping && mp_rewind())
        continue;
      if (is_float)
        memset(fout, 0, (size_t)want * 2 * sizeof(float));
      else
        memset(sout, 0, (size_t)want * 2 * sizeof(short));
      return 0;
    }

    if (is_float)
    {
      const float g = (float)mp_volume * (1.0f / 15.0f);
      if (ch == 2)
        for (i = 0; i < got * 2; i++)
          *fout++ = f_tmp[i] * g;
      else
        for (i = 0; i < got; i++)
        {
          float s = f_tmp[i] * g;
          *fout++ = s;
          *fout++ = s;
        }
    }
    else
    {
      const int vol = mp_volume;
      if (ch == 2)
        for (i = 0; i < got * 2; i++)
          *sout++ = (short)((int)s_tmp[i] * vol / 15);
      else
        for (i = 0; i < got; i++)
        {
          short s = (short)((int)s_tmp[i] * vol / 15);
          *sout++ = s;
          *sout++ = s;
        }
    }
    want -= got;
  }
  return 1;
}

static void mp_render_ex (void *dest, unsigned nsamp)
{
  if (!mp_playing || mp_paused)
  {
    memset (dest, 0, nsamp * 4);
    return;
  }
  if (!mp_pull (dest, nsamp, 0))
    mp_playing = 0;
}

static void mp_render_ex_f (void *dest, unsigned nsamp)
{
  if (!mp_playing || mp_paused)
  {
    memset (dest, 0, nsamp * 8);
    return;
  }
  if (!mp_pull (dest, nsamp, 1))
    mp_playing = 0;
}

/* Linear stream resampler (moved here long ago from libretro_sound.c's
 * I_ResampleStream; this is its only caller).  16-bit signed interleaved
 * stereo; body unchanged.  The (unsigned) promotion of the int16 samples
 * is deliberate: for negative samples the weighted sum wraps mod 2^32,
 * the logical >>16 keeps bits 16..31, and the int16 store truncates to
 * exactly the arithmetic-shift result -- defined behavior, bit-correct,
 * but floor rather than round.
 *
 * One fix against the inherited body: the interpolation of the final
 * output sample can land its integer cursor on the last source frame
 * and read the frame after it, which for upsampling ratios (stream
 * rate below the engine rate, e.g. a 32 kHz MP3 into a 44.1 kHz mix)
 * is one frame past the allocation -- a latent out-of-bounds read the
 * sanitizer sweep caught.  The buffer now carries a guard frame
 * duplicating the last real frame, clamping the interpolation at the
 * burst edge exactly the way the ogg backend clamps its own. */
static void mp_resample_stream (void *dest, unsigned nsamp,
      void (*proc)(void *dest, unsigned nsamp),
      unsigned sratein, unsigned srateout)
{
   unsigned i;
   int                   j   = 0;
   int16_t           *sout   = dest;
   static int16_t     *sin   = NULL;
   static unsigned sinsamp   = 0;
   static unsigned remainder = 0;
   unsigned step             = (sratein << 16) / (unsigned) srateout;
   unsigned nreq             = (step * nsamp + remainder) >> 16;

   if (nreq > sinsamp)
   {
      sin = realloc(sin, (nreq + 2) * 4);   /* +1 carry +1 guard frame */
      if (!sinsamp) // avoid pop when first starting stream
         sin[0] = sin[1] = 0;
      sinsamp = nreq;
   }

   proc (sin + 2, nreq);
   /* guard frame: clamp interpolation at the burst edge */
   sin[nreq * 2 + 2] = sin[nreq * 2];
   sin[nreq * 2 + 3] = sin[nreq * 2 + 1];

   for (i = 0; i < nsamp; i++)
   {
      *sout++ = ((unsigned) sin[j + 0] * (0x10000 - remainder) +
            (unsigned) sin[j + 2] * remainder) >> 16;
      *sout++ = ((unsigned) sin[j + 1] * (0x10000 - remainder) +
            (unsigned) sin[j + 3] * remainder) >> 16;
      remainder += step;
      j += remainder >> 16 << 1;
      remainder &= 0xffff;
   }
   sin[0] = sin[nreq * 2];
   sin[1] = sin[nreq * 2 + 1];
}

/* Float twin: same cursor arithmetic and edge-carry scheme, but the source,
 * interpolation and output stay float end to end -- no int16 quantization
 * between rmp3's float synthesis and the float mixer.  Separate static
 * state from the s16 lane; only one lane is active per session (the output
 * format is negotiated once at init). */
static void mp_resample_stream_f (void *dest, unsigned nsamp,
      void (*proc)(void *dest, unsigned nsamp),
      unsigned sratein, unsigned srateout)
{
   unsigned i;
   int                   j   = 0;
   float             *sout   = dest;
   static float       *sin   = NULL;
   static unsigned sinsamp   = 0;
   static unsigned remainder = 0;
   unsigned step             = (sratein << 16) / (unsigned) srateout;
   unsigned nreq             = (step * nsamp + remainder) >> 16;

   if (nreq > sinsamp)
   {
      sin = realloc(sin, (nreq + 2) * 2 * sizeof(float));
      if (!sinsamp) /* avoid pop when first starting stream */
         sin[0] = sin[1] = 0.0f;
      sinsamp = nreq;
   }

   proc (sin + 2, nreq);
   /* guard frame: clamp interpolation at the burst edge */
   sin[nreq * 2 + 2] = sin[nreq * 2];
   sin[nreq * 2 + 3] = sin[nreq * 2 + 1];

   for (i = 0; i < nsamp; i++)
   {
      float w1 = (float) remainder * (1.0f / 65536.0f);
      float w0 = 1.0f - w1;

      *sout++ = sin[j + 0] * w0 + sin[j + 2] * w1;
      *sout++ = sin[j + 1] * w0 + sin[j + 3] * w1;
      remainder += step;
      j += remainder >> 16 << 1;
      remainder &= 0xffff;
   }
   sin[0] = sin[nreq * 2];
   sin[1] = sin[nreq * 2 + 1];
}

static void mp_render (void *dest, unsigned nsamp)
{
  unsigned src_rate = (mp_m && mp_m->sampleRate) ? mp_m->sampleRate : 44100;
  mp_resample_stream (dest, nsamp, mp_render_ex, src_rate,
                      mp_samplerate_target);
}

/* Float render lane (render_float): decode through rmp3's native float
 * pipeline and resample in float.  The s16 lane quantizes to int16
 * before its resampler and is then widened by the mixer -- a round-trip
 * this skips. */
static void mp_render_float (void *dest, unsigned nsamp)
{
  unsigned src_rate = (mp_m && mp_m->sampleRate) ? mp_m->sampleRate : 44100;
  mp_resample_stream_f (dest, nsamp, mp_render_ex_f, src_rate,
                        mp_samplerate_target);
}

/* Save/restore playback position so a save state (and runahead and rewind,
 * which save and restore every frame) resumes MP3 music near where it was
 * instead of forcing the generic music layer's render-replay -- which for an
 * MP3 stream re-decodes from the start on every restore.
 *
 * serialize records the lump byte offset the current MPEG block's sync
 * search began at (mp_frame_off) plus the intra-block cursor
 * (framesConsumed); unserialize re-initialises rmp3 on the lump's tail
 * from that offset and discards the cursor's worth of frames.  MP3
 * blocks are independently decodable from a sync apart from the
 * bit-reservoir, so the resumed audio carries a tiny one-frame
 * transient (~26ms) -- inaudible, and far cheaper than the O(position)
 * decode-from-the-top that rmp3_seek_to_frame would cost every restore.
 *
 * Wire format (little-endian-host, matching the other backends):
 *   uint32_t magic   = 'MP3S'
 *   uint32_t version = 2       (v1 was the libmad byte-offset scheme;
 *                               same idea, different intra-frame cursor)
 *   uint32_t looping
 *   int32_t  frame_offset      (lump offset of the current block)
 *   int32_t  frames_consumed   (intra-block PCM cursor)
 *   int32_t  reserved (0)                                           */
#define MP3_STATE_MAGIC   0x4D503353u  /* 'MP3S' */
#define MP3_STATE_VERSION 2u

static size_t mp_serialize (void *dest, size_t cap)
{
  uint32_t hdr[3];
  int32_t  body[3];
  size_t   need = sizeof hdr + sizeof body;

  if (!mp_playing || !mp_data || !mp_m)
    return 0;                    /* nothing playing -> no state to record */
  if (mp_frame_off < 0 || mp_frame_off > mp_len)
    return 0;                    /* position outside the lump -> defer */
  if (!dest)
    return need;                 /* size-query mode */
  if (cap < need)
    return 0;

  hdr[0]  = MP3_STATE_MAGIC;
  hdr[1]  = MP3_STATE_VERSION;
  hdr[2]  = (uint32_t)(mp_looping ? 1 : 0);
  body[0] = (int32_t)mp_frame_off;
  body[1] = (int32_t)mp_m->framesConsumed;
  body[2] = 0;
  memcpy(dest, hdr, sizeof hdr);
  memcpy((unsigned char *)dest + sizeof hdr, body, sizeof body);
  return need;
}

static int mp_unserialize (const void *src, size_t size)
{
  uint32_t hdr[3];
  int32_t  body[3];
  long     off, consumed;

  if (!mp_data || !mp_m)                            return 0;
  if (size < sizeof hdr + sizeof body)              return 0;
  memcpy(hdr, src, sizeof hdr);
  if (hdr[0] != MP3_STATE_MAGIC)                    return 0;
  if (hdr[1] != MP3_STATE_VERSION)                  return 0;
  memcpy(body, (const unsigned char *)src + sizeof hdr, sizeof body);
  off      = (long)body[0];
  consumed = (long)body[1];
  if (off < 0 || off >= mp_len)                     return 0;
  if (consumed < 0 || consumed > RMP3_MAX_SAMPLES_PER_FRAME) return 0;

  /* Re-open the decoder on the lump's tail at the saved block. */
  if (!rmp3_init_memory(mp_m, (const unsigned char *)mp_data + off,
                        (size_t)(mp_len - off)))
    return 0;                    /* could not resume here -> defer to replay */
  mp_base_off  = (int)off;
  mp_frame_off = (int)off;

  /* Restore the intra-block cursor by decoding and discarding. */
  if (consumed > 0)
    rmp3_read_s16(mp_m, (uint64_t)consumed, NULL);

  mp_looping = (hdr[2] != 0);
  mp_playing = 1;
  mp_paused  = 0;
  return 1;
}


const music_player_t mp_player =
{
  mp_name,
  mp_init,
  mp_shutdown,
  mp_setvolume,
  mp_pause,
  mp_resume,
  mp_registersong,
  mp_unregistersong,
  mp_play,
  mp_stop,
  mp_render,
  mp_serialize,
  mp_unserialize,
  mp_render_float
};
