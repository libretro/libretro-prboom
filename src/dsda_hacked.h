/* dsda_hacked.h
 *
 * DSDHacked support: dynamic growth of the dehacked-editable tables
 * (states, mobjinfo, sprite names, sfx, music) beyond their vanilla
 * counts.  A dehacked patch may reference an arbitrarily high index; the
 * accessors below grow the relevant table on demand (doubling capacity and
 * zero/sentinel-initialising the new entries) and return a pointer to the
 * requested slot.
 *
 * The tables themselves (states, mobjinfo, sprnames, S_sfx, S_music) and
 * their runtime counts (num_states, num_mobj_types, num_sprites, num_sfx,
 * num_music) live in info.c / sounds.c; this module owns the growth logic
 * and the deh_codeptr / seenstate companion arrays that must grow with
 * states.
 */

#ifndef __DSDA_HACKED__
#define __DSDA_HACKED__

#include "info.h"
#include "sounds.h"
#include "d_think.h"

/* Runtime table sizes (start at the vanilla NUM* counts). */
extern int num_states;
extern int num_mobj_types;
extern int num_sprites;
extern int num_sfx;
extern int num_music;

/* Companion to states[], grows in lockstep: the per-state original action
 * pointer used by the BEX [CODEPTR] cross-reference. */
extern actionf_t *deh_codeptr;

/* Grow-on-demand accessors.  Each ensures the table is large enough to hold
 * 'index', then returns a pointer to that slot. */
state_t    *dsda_GetState(int index);
mobjinfo_t *dsda_GetMobjInfo(int index);
sfxinfo_t  *dsda_GetSfx(int index);
musicinfo_t*dsda_GetMusic(int index);
/* Sprite names are an array of char*; this ensures capacity and returns the
 * slot so the caller can assign a duplicated name string. */
const char **dsda_GetSprite(int index);

/* One-time initialisation: copy the static seed tables into growable
 * allocations and set the num_* counters.  Called from D_BuildBEXTables
 * before any dehacked patch is processed. */
void dsda_InitTables(void);

#endif
