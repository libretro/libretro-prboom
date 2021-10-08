/* FluidSynth - A Software Synthesizer
 *
 * Copyright (C) 2003  Peter Hanappe and others.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public License
 * as published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *  
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 * 02111-1307, USA
 */

#ifndef _FLUIDSYNTH_MISC_H
#define _FLUIDSYNTH_MISC_H


#ifdef __cplusplus
extern "C" {
#endif


/*
 *
 *  Utility functions
 */

#ifdef WIN32
/** Set the handle to the instance of the application on the Windows
    platform. The handle is needed to open DirectSound. */
FLUIDSYNTH_API void* fluid_get_hinstance(void);
FLUIDSYNTH_API void fluid_set_hinstance(void* hinstance);
#endif


#ifdef __cplusplus
}
#endif

#endif /* _FLUIDSYNTH_MISC_H */
