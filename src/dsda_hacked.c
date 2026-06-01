/* dsda_hacked.c  -- see dsda_hacked.h for the overview. */

#include <stdlib.h>
#include <string.h>

#include "z_zone.h"
#include "doomtype.h"
#include "m_fixed.h"
#include "info.h"
#include "sounds.h"
#include "d_think.h"
#include "dsda_hacked.h"

/* The five editable tables live in info.c / sounds.c.  Before
 * dsda_InitTables runs they point at the static seed arrays; afterwards
 * they point at growable malloc'd copies. */
extern state_t    *states;
extern mobjinfo_t *mobjinfo;
extern const char **sprnames;
extern sfxinfo_t  *S_sfx;
extern musicinfo_t*S_music;

int num_states;
int num_mobj_types;
int num_sprites;
int num_sfx;
int num_music;

actionf_t *deh_codeptr;

/* Generic doubling growth.  Returns the new count (>= needed). */
static int next_capacity(int current, int needed)
{
  int cap = current;
  if (cap < 1)
    cap = 1;
  while (needed >= cap)
    cap *= 2;
  return cap;
}

/* --- states ----------------------------------------------------------- */

static void ensure_states(int index)
{
  int old = num_states;
  int cap;
  int i;

  if (index < num_states)
    return;

  cap = next_capacity(num_states, index);

  states      = realloc(states, cap * sizeof(*states));
  deh_codeptr = realloc(deh_codeptr, cap * sizeof(*deh_codeptr));

  memset(states + old,      0, (cap - old) * sizeof(*states));
  memset(deh_codeptr + old, 0, (cap - old) * sizeof(*deh_codeptr));

  /* New frames default to a harmless "do nothing, stay put" state, matching
   * dsdhacked: invisible sprite, infinite tics, self-loop. */
  for (i = old; i < cap; i++)
  {
    states[i].sprite    = SPR_TNT1;
    states[i].tics      = -1;
    states[i].nextstate = (statenum_t)i;
  }

  num_states = cap;
}

state_t *dsda_GetState(int index)
{
  ensure_states(index);
  return &states[index];
}

/* --- mobjinfo --------------------------------------------------------- */

static void init_new_mobjinfo(int from, int to)
{
  int i;
  for (i = from; i < to; i++)
  {
    /* sensible inert defaults plus the MBF21 sentinels */
    mobjinfo[i].droppeditem    = MT_NULL;
    mobjinfo[i].infighting_group = IG_DEFAULT;
    mobjinfo[i].projectile_group = PG_DEFAULT;
    mobjinfo[i].splash_group     = SG_DEFAULT;
    mobjinfo[i].altspeed         = NO_ALTSPEED;
    mobjinfo[i].meleerange       = 64 * FRACUNIT;
    mobjinfo[i].doomednum        = -1;
  }
}

static void ensure_mobjinfo(int index)
{
  int old = num_mobj_types;
  int cap;

  if (index < num_mobj_types)
    return;

  cap = next_capacity(num_mobj_types, index);
  mobjinfo = realloc(mobjinfo, cap * sizeof(*mobjinfo));
  memset(mobjinfo + old, 0, (cap - old) * sizeof(*mobjinfo));
  init_new_mobjinfo(old, cap);
  num_mobj_types = cap;
}

mobjinfo_t *dsda_GetMobjInfo(int index)
{
  ensure_mobjinfo(index);
  return &mobjinfo[index];
}

/* --- sprite names ----------------------------------------------------- */

static void ensure_sprites(int index)
{
  int old = num_sprites;
  int cap;
  int i;

  if (index < num_sprites)
    return;

  cap = next_capacity(num_sprites, index);
  /* +1 for the NULL terminator R_InitSpriteDefs scans for. */
  sprnames = realloc(sprnames, (cap + 1) * sizeof(*sprnames));
  for (i = old; i <= cap; i++)
    sprnames[i] = NULL;
  num_sprites = cap;
}

const char **dsda_GetSprite(int index)
{
  ensure_sprites(index);
  return &sprnames[index];
}

/* --- sfx -------------------------------------------------------------- */

static void ensure_sfx(int index)
{
  int old = num_sfx;
  int cap;

  if (index < num_sfx)
    return;

  cap = next_capacity(num_sfx, index);
  S_sfx = realloc(S_sfx, cap * sizeof(*S_sfx));
  memset(S_sfx + old, 0, (cap - old) * sizeof(*S_sfx));
  num_sfx = cap;
}

sfxinfo_t *dsda_GetSfx(int index)
{
  ensure_sfx(index);
  return &S_sfx[index];
}

/* --- music ------------------------------------------------------------ */

static void ensure_music(int index)
{
  int old = num_music;
  int cap;

  if (index < num_music)
    return;

  cap = next_capacity(num_music, index);
  S_music = realloc(S_music, cap * sizeof(*S_music));
  memset(S_music + old, 0, (cap - old) * sizeof(*S_music));
  num_music = cap;
}

musicinfo_t *dsda_GetMusic(int index)
{
  ensure_music(index);
  return &S_music[index];
}

/* --- init ------------------------------------------------------------- */

void dsda_InitTables(void)
{
  static int done = 0;
  state_t    *seed_states;
  mobjinfo_t *seed_mobjinfo;
  const char **seed_sprnames;
  sfxinfo_t  *seed_sfx;
  musicinfo_t*seed_music;

  if (done)
    return;
  done = 1;

  /* The pointers currently reference the static seed arrays in info.c /
   * sounds.c.  Copy each into a heap allocation we can grow, and repoint
   * the global at it.  num_* start at the vanilla counts. */
  num_states     = NUMSTATES;
  num_mobj_types = NUMMOBJTYPES;
  num_sprites    = NUMSPRITES;
  num_sfx        = NUMSFX;
  num_music      = NUMMUSIC;

  seed_states   = states;
  seed_mobjinfo = mobjinfo;
  seed_sprnames = sprnames;
  seed_sfx      = S_sfx;
  seed_music    = S_music;

  states = malloc(num_states * sizeof(*states));
  memcpy(states, seed_states, num_states * sizeof(*states));

  mobjinfo = malloc(num_mobj_types * sizeof(*mobjinfo));
  memcpy(mobjinfo, seed_mobjinfo, num_mobj_types * sizeof(*mobjinfo));

  /* sprnames is NULL-terminated (R_InitSpriteDefs scans for the NULL), so
   * allocate one extra slot and copy the terminator the seed array had. */
  sprnames = malloc((num_sprites + 1) * sizeof(*sprnames));
  memcpy(sprnames, seed_sprnames, num_sprites * sizeof(*sprnames));
  sprnames[num_sprites] = NULL;

  S_sfx = malloc(num_sfx * sizeof(*S_sfx));
  memcpy(S_sfx, seed_sfx, num_sfx * sizeof(*S_sfx));

  S_music = malloc(num_music * sizeof(*S_music));
  memcpy(S_music, seed_music, num_music * sizeof(*S_music));

  deh_codeptr = calloc(num_states, sizeof(*deh_codeptr));
}
