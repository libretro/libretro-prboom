/* dsda_hacked.h -- DSDHacked: dynamic growth of the dehacked-editable
 * tables (states, mobjinfo, sprite names, sfx, music) beyond their vanilla
 * counts.  Faithful port of dsda-doom's per-table growth, trimmed to this
 * Doom-only core.
 *
 * The tables (states, mobjinfo, sprnames, S_sfx, S_music) and their runtime
 * counts (num_states, num_mobj_types, num_sprites, num_sfx, num_music) live
 * here; the static seed arrays in info.c / sounds.c remain pristine so the
 * tables can be rebuilt from them on every (re)load.
 */

#ifndef __DSDA_HACKED__
#define __DSDA_HACKED__

#include "info.h"
#include "sounds.h"
#include "d_think.h"

extern int num_states;
extern int num_mobj_types;
extern int num_sprites;
extern int num_sfx;
extern int num_music;

/* Grows with states[]: per-state original action pointer (BEX [CODEPTR]). */
extern actionf_t *deh_codeptr;

/* Grow-on-demand accessors: ensure the table covers 'index', return the slot. */
state_t     *dsda_GetState(int index);
mobjinfo_t  *dsda_GetMobjInfo(int index);
sfxinfo_t   *dsda_GetSfx(int index);
musicinfo_t *dsda_GetMusic(int index);
const char **dsda_GetSprite(int index);

/* (Re)build the growable tables from the static seed arrays.  Safe to call
 * on every load: it frees any prior allocation and starts fresh, so the
 * tables never dangle after a Z_Close between content loads. */
void dsda_InitTables(void);

/* Frees the growable copies and resets the table globals to the static
 * seeds.  Call at teardown before the zone allocator is closed. */
void dsda_FreeTables(void);

#endif
