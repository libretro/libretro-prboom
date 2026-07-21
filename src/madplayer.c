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
 *---------------------------------------------------------------------
 */



#include "config.h"

#include "musicplayer.h"

#ifndef HAVE_LIBMAD
#include <string.h>

static const char *mp_name (void)
{
  return "mad mp3 player (DISABLED)";
}


static int mp_init (int samplerate)
{
  return 0;
}

const music_player_t mp_player =
{
  mp_name,
  mp_init,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,  /* serialize */
  NULL,  /* unserialize */
  NULL   /* render_float */
};

#else // HAVE_LIBMAD


#include <stdlib.h>
#include <string.h>
#include "lprintf.h"

#include "../libmad/mad.h"

#include "i_sound.h"

static struct mad_stream Stream;
static struct mad_frame  Frame;
static struct mad_synth  Synth;
static struct mad_header Header;


static int mp_looping = 0;
static int mp_volume = 0; // 0-15
static int mp_samplerate_target = 0;
static int mp_paused = 0;
static int mp_playing = 0;

static const void *mp_data;
static int mp_len;


static int mp_leftoversamps = 0; // number of extra samples
                                 // left over in mad decoder
static int mp_leftoversamppos = 0;


static const char *mp_name (void)
{
  return "mad mp3 player";
}


static int mp_init (int samplerate)
{
  mad_stream_init (&Stream);
  mad_frame_init (&Frame);
  mad_synth_init (&Synth);
  mad_header_init (&Header);
  mp_samplerate_target = samplerate;
  return 1;
}

static void mp_shutdown (void)
{
  mad_frame_finish (&Frame);
  mad_stream_finish (&Stream);
  mad_header_finish (&Header);
}

static const void *mp_registersong (const void *data, unsigned len)
{
  int i;
  int maxtry;
  int success = 0;

  // the MP3 standard doesn't include any global file header.  the only way to tell filetype
  // is to start decoding stuff.  you can't be too strict however because MP3 is resilient to
  // crap in the stream.

  // this routine is a bit slower than it could be, but apparently there are lots of files out
  // there with some dodgy stuff at the beginning.
    
  // if the stream begins with an ID3v2 magic, search hard and long for our first valid header
  if (memcmp (data, "ID3", 3) == 0)
    maxtry = 100;
  // otherwise, search for not so long
  else
    maxtry = 20;

  mad_stream_buffer (&Stream, data, len);

  for (i = 0; i < maxtry; i++)
  {
    if (mad_header_decode (&Header, &Stream) != 0)
    {
      if (!MAD_RECOVERABLE (Stream.error))
      {
        lprintf (LO_WARN, "mad_registersong failed: %s\n", mad_stream_errorstr (&Stream));
        return NULL;
      }  
    }
    else
    {
      success++;
    }
  }

  // 80% to pass
  if (success < maxtry * 8 / 10)
  {
    lprintf (LO_WARN, "mad_registersong failed\n");
    return NULL;
  }
  
  lprintf (LO_INFO, "mad_registersong succeed. bitrate %lu samplerate %d\n", Header.bitrate, Header.samplerate);
 
  mp_data = data;
  mp_len = len;
  // handle not used
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
{ // nothing to do
  mp_data = NULL;
  mp_playing = 0;
}

static void mp_play (const void *handle, int looping)
{
  mad_stream_buffer (&Stream, mp_data, mp_len);

  mp_playing = 1;
  mp_looping = looping;
  mp_leftoversamps = 0;
  mp_leftoversamppos = 0;
}

static void mp_stop (void)
{
  mp_playing = 0;
}

// convert from mad's internal fixed point representation
static INLINE short mp_fixtoshort (mad_fixed_t f)
{
  // clip
  if (f < -MAD_F_ONE)
    f = -MAD_F_ONE;
  if (f > MAD_F_ONE)
    f = MAD_F_ONE;    
  // apply volume before conversion to 16bit
  f /= 15;
  f *= mp_volume;
  f >>= (MAD_F_FRACBITS - 15);

  return (short) f;
}

/* Float twin of mp_fixtoshort: clamp in the fixed domain, then scale to
 * normalized [-1,1] with the 0-15 volume folded into one multiply.  Keeps
 * libmad's full 28-bit synthesis resolution instead of dropping 12 bits
 * (plus the truncating /15) through the int16 lane. */
static INLINE float mp_fixtofloat (mad_fixed_t f)
{
  if (f < -MAD_F_ONE)
    f = -MAD_F_ONE;
  if (f > MAD_F_ONE)
    f = MAD_F_ONE;
  return (float) f * ((float) mp_volume *
                      (1.0f / (15.0f * (float) MAD_F_ONE)));
}

/* Shared decode/frame-pump body for both output formats (the flplayer
 * fl_render_core pattern): the libmad frame walk, error handling and
 * leftover-cursor state are single-sourced; only the per-sample
 * conversion and the write width differ.  is_float == 0 expands to the
 * exact arithmetic the int16 path used before this change. */
static void mp_render_core (void *dest, unsigned nsamp, int is_float)
{
  short *sout = (short *) dest;
  float *fout = (float *) dest;

  int localerrors = 0;

  if (!mp_playing || mp_paused)
  {
    memset (dest, 0, nsamp * (is_float ? 8 : 4));
    return;
  }

  while (1)
  {
    // write any leftover data from last MP3 frame
    while (mp_leftoversamps > 0 && nsamp > 0)
    {
      mad_fixed_t l = Synth.pcm.samples[0][mp_leftoversamppos];
      // if mono, just duplicate the first channel again
      mad_fixed_t r = (Synth.pcm.channels == 2)
                    ? Synth.pcm.samples[1][mp_leftoversamppos] : l;

      if (is_float)
      {
        *fout++ = mp_fixtofloat (l);
        *fout++ = mp_fixtofloat (r);
      }
      else
      {
        *sout++ = mp_fixtoshort (l);
        *sout++ = mp_fixtoshort (r);
      }

      mp_leftoversamps -= 1;
      mp_leftoversamppos += 1;
      nsamp -= 1;
    }
    if (nsamp == 0)
      return; // done
    
    // decode next valid MP3 frame
    while (mad_frame_decode (&Frame, &Stream) != 0)
    {
      if (MAD_RECOVERABLE (Stream.error))
      { // unspecified problem with one frame.
        // try the next frame, but bail if we get a bunch of crap in a row;
        // likely indicates a larger problem (and if we don't bail, we could
        // spend arbitrarily long amounts of time looking for the next good
        // packet)
        localerrors++;
        if (localerrors == 10)
        {
          lprintf (LO_WARN, "mad_frame_decode: Lots of errors.  Most recent %s\n", mad_stream_errorstr (&Stream));
          mp_playing = 0;
          memset (is_float ? (void *) fout : (void *) sout, 0,
                  nsamp * (is_float ? 8 : 4));
          return;
        }
      }  
      else if (Stream.error == MAD_ERROR_BUFLEN)
      { // EOF
        // FIXME: in order to not drop the last frame, there must be at least MAD_BUFFER_GUARD
        // of extra bytes (with value 0) at the end of the file.  current implementation
        // drops last frame
        if (mp_looping)
        { // rewind, then go again
          mad_stream_buffer (&Stream, mp_data, mp_len);
          continue;
        }
        else
        { // stop
          mp_playing = 0;
          memset (is_float ? (void *) fout : (void *) sout, 0,
                  nsamp * (is_float ? 8 : 4));
          return;
        }
      }
      else
      { // oh well.
        lprintf (LO_WARN, "mad_frame_decode: Unrecoverable error %s\n", mad_stream_errorstr (&Stream));
        mp_playing = 0;
        memset (is_float ? (void *) fout : (void *) sout, 0,
                nsamp * (is_float ? 8 : 4));
        return;
      }
    }

    // got a good frame, so synth it and dispatch it.
    mad_synth_frame (&Synth, &Frame);
    mp_leftoversamps = Synth.pcm.length;
    mp_leftoversamppos = 0;

  }
  // NOT REACHED
}

static void mp_render_ex (void *dest, unsigned nsamp)
{
  mp_render_core (dest, nsamp, 0);
}

static void mp_render_ex_f (void *dest, unsigned nsamp)
{
  mp_render_core (dest, nsamp, 1);
}

/* Linear stream resampler, moved here from libretro_sound.c
 * (I_ResampleStream): this was its only caller, reached through an extern
 * prototype declared in this .c file.  16-bit signed interleaved stereo;
 * body unchanged.  The (unsigned) promotion of the int16 samples is
 * deliberate: for negative samples the weighted sum wraps mod 2^32, the
 * logical >>16 keeps bits 16..31, and the int16 store truncates to exactly
 * the arithmetic-shift result -- defined behavior, bit-correct, but floor
 * rather than round. */
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
      sin = realloc(sin, (nreq + 1) * 4);
      if (!sinsamp) // avoid pop when first starting stream
         sin[0] = sin[1] = 0;
      sinsamp = nreq;
   }

   proc (sin + 2, nreq);

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
 * between the 28-bit libmad synth and the float mixer.  Separate static
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
      sin = realloc(sin, (nreq + 1) * 2 * sizeof(float));
      if (!sinsamp) /* avoid pop when first starting stream */
         sin[0] = sin[1] = 0.0f;
      sinsamp = nreq;
   }

   proc (sin + 2, nreq);

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
  mp_resample_stream (dest, nsamp, mp_render_ex, Header.samplerate, mp_samplerate_target);
}

/* Float render lane (render_float): decode at 28-bit fixed, convert once to
 * float, resample in float.  The s16 lane quantizes to int16 before its
 * resampler and is then widened by the mixer -- a round-trip this skips. */
static void mp_render_float (void *dest, unsigned nsamp)
{
  mp_resample_stream_f (dest, nsamp, mp_render_ex_f, Header.samplerate, mp_samplerate_target);
}

/* Save/restore playback position so a save state (and runahead and rewind,
 * which save and restore every frame) resumes MP3 music near where it was
 * instead of forcing the generic music layer's render-replay -- which for an
 * MP3 stream re-decodes from the start on every restore.
 *
 * libmad has no sample-accurate seek: the decode position is the byte offset
 * of the current frame within the lump, plus the intra-frame leftover-sample
 * cursor.  serialize records that offset (start of the frame currently being
 * consumed) and the cursor; unserialize re-buffers libmad at the offset,
 * re-decodes that one frame to repopulate the synth, and restores the cursor.
 * MP3 frames are independently decodable from a frame boundary apart from the
 * bit-reservoir, so the resumed audio carries a tiny one-frame transient
 * (~26ms) -- inaudible, and far cheaper than re-decoding the whole track.
 *
 * Wire format (little-endian-host, matching the other backends):
 *   uint32_t magic   = 'MP3S'
 *   uint32_t version = 1
 *   uint32_t looping
 *   int32_t  frame_offset      (byte offset of this_frame; <0 = defer)
 *   int32_t  leftoversamps
 *   int32_t  leftoversamppos */
#define MP3_STATE_MAGIC   0x4D503353u  /* 'MP3S' */
#define MP3_STATE_VERSION 1u

static size_t mp_serialize (void *dest, size_t cap)
{
  uint32_t hdr[3];
  int32_t  body[3];
  size_t   need = sizeof hdr + sizeof body;
  long     off;

  if (!mp_playing || !mp_data)
    return 0;                    /* nothing playing -> no state to record */
  if (!Stream.this_frame || !Stream.buffer)
    return 0;
  off = (long)(Stream.this_frame - (const unsigned char *)mp_data);
  if (off < 0 || off > mp_len)
    return 0;                    /* position outside the lump -> defer */
  if (!dest)
    return need;                 /* size-query mode */
  if (cap < need)
    return 0;

  hdr[0]  = MP3_STATE_MAGIC;
  hdr[1]  = MP3_STATE_VERSION;
  hdr[2]  = (uint32_t)(mp_looping ? 1 : 0);
  body[0] = (int32_t)off;
  body[1] = (int32_t)mp_leftoversamps;
  body[2] = (int32_t)mp_leftoversamppos;
  memcpy(dest, hdr, sizeof hdr);
  memcpy((unsigned char *)dest + sizeof hdr, body, sizeof body);
  return need;
}

static int mp_unserialize (const void *src, size_t size)
{
  uint32_t hdr[3];
  int32_t  body[3];
  long     off;

  if (!mp_data)                                     return 0;
  if (size < sizeof hdr + sizeof body)              return 0;
  memcpy(hdr, src, sizeof hdr);
  if (hdr[0] != MP3_STATE_MAGIC)                    return 0;
  if (hdr[1] != MP3_STATE_VERSION)                  return 0;
  memcpy(body, (const unsigned char *)src + sizeof hdr, sizeof body);
  off = (long)body[0];
  if (off < 0 || off > mp_len)                      return 0;

  mp_looping = (hdr[2] != 0);

  /* Re-buffer libmad from the saved frame and decode it so Synth.pcm holds
   * the frame the leftover cursor indexes into. */
  mad_stream_buffer (&Stream, (const unsigned char *)mp_data + off,
                     (unsigned long)(mp_len - off));
  if (mad_frame_decode (&Frame, &Stream) != 0)
  {
    if (!MAD_RECOVERABLE (Stream.error))
      return 0;                  /* could not resume here -> defer to replay */
  }
  mad_synth_frame (&Synth, &Frame);

  /* Restore the intra-frame cursor, clamped to the freshly synthed frame. */
  mp_leftoversamps   = (int)body[1];
  mp_leftoversamppos = (int)body[2];
  if (mp_leftoversamppos < 0) mp_leftoversamppos = 0;
  if (mp_leftoversamppos > Synth.pcm.length) mp_leftoversamppos = Synth.pcm.length;
  if (mp_leftoversamps < 0) mp_leftoversamps = 0;
  if (mp_leftoversamps > Synth.pcm.length - mp_leftoversamppos)
    mp_leftoversamps = Synth.pcm.length - mp_leftoversamppos;

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

#endif // HAVE_LIBMAD
