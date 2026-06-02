/* Declarations for the Hexen data tables (see hexen/sounds.c, and the
 * actor/state/sprite tables added in later commits).  Inert until selected
 * by the seed system in dsda_hacked.c. */
#ifndef __HEXEN_HEXEN__
#define __HEXEN_HEXEN__

#include "doomtype.h"
#include "info.h"
#include "sounds.h"

extern sfxinfo_t hexen_S_sfx[HEXEN_NUMSFX];
extern const char *hexen_sprnames[HEXEN_NUMSPRITES + 1];
extern state_t hexen_states[HEXEN_NUMSTATES];

#endif
