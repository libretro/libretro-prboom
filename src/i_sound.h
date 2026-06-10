/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2000 by
 *  Jess Haas, Nicolas Kalkhof, Colin Phipps, Florian Schulze
 *  Copyright 2005, 2006 by
 *  Florian Schulze, Colin Phipps, Neil Stevens, Andrey Budko
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
 *      System interface, sound.
 *
 *-----------------------------------------------------------------------------*/

#ifndef __I_SOUND__
#define __I_SOUND__

#include "sounds.h"
#include "doomtype.h"

#define SNDSERV
#undef SNDINTR

#ifndef SNDSERV
#include "l_soundgen.h"
#endif

extern int mus_opl_gain; // NSM  fine tune OPL output level

/* User-selected MIDI playback hardware:
 *   0 = Off (no MIDI playback at all)
 *   1 = Adlib (OPL2/OPL3 emulation, opl_synth_player)
 *   2 = Fluidsynth (only valid if HAVE_LIBFLUIDSYNTH; falls back
 *       to silence if the build doesn't include fluidsynth)
 *   last = libretro raw MIDI output (libretro_midi_player): streams
 *       MIDI events to the frontend's MIDI interface for host-side
 *       synthesis; declines (silence) if the frontend exposes none.
 *       The numeric value is 3 when fluidsynth is built, 2 otherwise,
 *       since the Fluidsynth entry is compiled in conditionally -- see
 *       midi_player_opts[] in m_menu.c and the dispatch in
 *       libretro_sound.c, which use matching #ifdefs. */
extern int midi_player;

// Init at program start...
void I_InitSound(void);

// ... shut down and relase at program termination.
void I_ShutdownSound(void);

//
//  SFX I/O
//

// Initialize channels?
void I_SetChannels(void);

// Get raw data lump index for sound descriptor.
int I_GetSfxLumpNum (sfxinfo_t *sfxinfo);

// Starts a sound in a particular sound channel.
int I_StartSound(int id, int channel, int vol, int sep, int pitch, int priority);

// Stops a sound channel.
void I_StopSound(int handle);

// Called by S_*() functions
//  to see if a channel is still playing.
// Returns 0 if no longer playing, 1 if playing.
dbool   I_SoundIsPlaying(int handle);

// Updates the volume, separation,
//  and pitch of a sound channel.
void I_UpdateSoundParams(int handle, int vol, int sep, int pitch);

//
//  MUSIC I/O
//
void I_InitMusic(void);
void I_ShutdownMusic(void);

void I_UpdateMusic(void);

// Volume.
void I_SetMusicVolume(int volume);

// PAUSE game handling.
void I_PauseSong(int handle);
void I_ResumeSong(int handle);

// Registers a song handle to song data.
int I_RegisterSong(const void *data, size_t len);

// cournia - tries to load a music file
int I_RegisterMusicFile( const char* filename, musicinfo_t *music );

// Called by anything that wishes to start music.
//  plays a song, and when the song is done,
//  starts playing it again in an endless loop.
// Horrible thing to do, considering.
void I_PlaySong(int handle, int looping);

// Stops a song over 3 seconds.
void I_StopSong(int handle);

// See above (register), then think backwards
void I_UnRegisterSong(int handle);

/* Returns 1 if the currently registered track is being decoded by
 * the MP3 player (mp_player), 0 otherwise (MIDI player, or nothing
 * registered).  Used by S_RestartMusic to skip MIDI-hardware-driven
 * restarts on MP3 streams. */
int I_MusicIsMP3(void);

/* Save-state support for music.  These dispatch to the active music
 * backend's optional serialize/unserialize callbacks; backends that
 * don't implement them produce zero-byte saves, which the caller
 * treats as "no music state recorded" and restore as a no-op. */
size_t I_MusicSerializeMaxSize(void);
size_t I_MusicSerialize(void *dest, size_t cap);
int    I_MusicUnserialize(const void *src, size_t size);

#endif
