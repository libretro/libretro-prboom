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


#ifndef MUSICPLAYER_H
#define MUSICPLAYER_H

#include <stddef.h>

/*
Anything that implements all of these functions can play music in prboomplus.

render: If audio output isn't delivered by render (say, for midi out), then the
player won't be recordable with video capture.  In that case, still write 0s to
the buffer when render is called.

thread safety: Locks are handled in i_sound.c.  Don't worry about it.

Timing: If you're outputting to render, your timing should come solely from the
calls to render, and not some external timing source.  That's why things stay
synced.
*/

typedef struct
{
  // descriptive name of the player, such as "OPL2 Synth"
  const char *(*name)(void);

  // samplerate is in hz.  return is 1 for success
  int (*init)(int samplerate);

  // deallocate structures, cleanup, ...
  void (*shutdown)(void);

  // set volume, 0 = off, 15 = max
  void (*setvolume)(int v);

  // pause currently running song.
  void (*pause)(void);

  // undo pause
  void (*resume)(void);

  // return a player-specific handle, or NULL on failure.
  // data does not belong to player, but it will persist as long as unregister is not called
  const void *(*registersong)(const void *data, unsigned len);

  // deallocate structures, etc.  data is no longer valid
  void (*unregistersong)(const void *handle);

  void (*play)(const void *handle, int looping);

  // stop
  void (*stop)(void);

  // s16 stereo, with samplerate as specified in init.  player needs to be able to handle
  // just about anything for nsamp.  render can be called even during pause+stop.
  void (*render)(void *dest, unsigned nsamp);

  // Optional: save player-specific playback state (song position,
  // tempo, channel state, etc.) so a save state can resume music
  // from the same point on load.  Returns the number of bytes
  // written to dest, or 0 if there is nothing to save, dest is too
  // small, or the player does not implement state save.  Pass dest
  // == NULL to query the maximum size the player might write.
  //
  // Implementations must be efficient: this is called every save
  // state, including high-frequency saves driven by features such
  // as runahead.  Targeting tens of bytes is ideal; hundreds is
  // tolerable.  Avoid kilobytes.
  size_t (*serialize)(void *dest, size_t cap);

  // Optional: restore the state previously written by serialize().
  // Returns 1 on success, 0 if the data is unrecognised, mismatched
  // against the currently loaded song, or the player does not
  // implement state restore.  On failure the player should leave
  // playback running as-is; the caller treats this as a soft error.
  int (*unserialize)(const void *src, size_t size);

  // Optional: render float stereo, normalized to [-1.0, 1.0], at the
  // samplerate given to init().  Only used when the libretro frontend
  // negotiated float audio output (RETRO_ENVIRONMENT_GET_AUDIO_SAMPLE_
  // BATCH_FLOAT); the caller passes a float* dest and the same nsamp
  // contract as render().  Leave NULL for integer-native backends
  // (MOD/MP3) -- the caller renders them via render() and widens to
  // float.  Implementing this lets backends with a float-native
  // stage (Ogg via stb_vorbis, MIDI via fluidsynth, OPL via its
  // float FIR resampler) skip a float->int16->float round-trip and
  // feed the float mixer directly.
  void (*render_float)(void *dest, unsigned nsamp);
} music_player_t;

#endif /* MUSICPLAYER_H */
