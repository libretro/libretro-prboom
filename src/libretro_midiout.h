/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
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
 * DESCRIPTION:
 *      Music player that streams MIDI events to the frontend's raw
 *      MIDI output interface (RETRO_ENVIRONMENT_GET_MIDI_INTERFACE),
 *      letting the host OS / hardware synthesiser produce the audio.
 *
 *---------------------------------------------------------------------
 */

#ifndef LIBRETRO_MIDIOUT_H
#define LIBRETRO_MIDIOUT_H

#include "musicplayer.h"

extern const music_player_t libretro_midi_player;

#endif /* LIBRETRO_MIDIOUT_H */
