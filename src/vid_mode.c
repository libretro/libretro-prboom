/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
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
 *      Active output pixel format.  Set once by the libretro layer in
 *      retro_load_game (from the "Color Format" core option, clamped to
 *      what the frontend accepts) before any surface, palette or LUT is
 *      built, and constant thereafter.  Defaults to the historical
 *      RGB565 renderer so any path that runs before negotiation -- and
 *      every non-libretro consumer -- behaves exactly as before.
 *
 *-----------------------------------------------------------------------------*/

#include "vid_mode.h"

int vid_mode       = VID_MODE565;
int vid_pixelbytes = 2;
