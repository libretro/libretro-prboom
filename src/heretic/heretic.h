/* Declarations for the Heretic data tables (see heretic/info.c, heretic/sounds.c).
 * Inert until selected by the seed system. */
#ifndef __HERETIC_HERETIC__
#define __HERETIC_HERETIC__

#include "info.h"
#include "sounds.h"

extern mobjinfo_t heretic_mobjinfo[HERETIC_NUMMOBJTYPES - HERETIC_MT_ZERO];
extern state_t    heretic_states[HERETIC_NUMSTATES];
extern const char *heretic_sprnames[HERETIC_NUMSPRITES + 1];
extern sfxinfo_t  heretic_S_sfx[HERETIC_NUMSFX];

#endif
