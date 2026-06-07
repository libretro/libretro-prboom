/* Emacs style mode select   -*- C89 -*-
 *-----------------------------------------------------------------------------
 *
 * ZDoom SNDINFO monster-sound remaps for Doom-engine games.
 *
 *-----------------------------------------------------------------------------
 */

#ifndef __U_ZSNDINFO__
#define __U_ZSNDINFO__

/* Parse a ZDoom-form SNDINFO lump ("logical/name dslump" lines) and
 * route monster sound bindings onto mobjinfo's sound fields; no-op
 * when no such lump exists or in Raven games. */
void U_ZDoomLoadSndInfo(void);

#endif
