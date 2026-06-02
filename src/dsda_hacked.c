/* dsda_hacked.c -- see dsda_hacked.h.  Faithful port of dsda-doom's
 * dsdhacked table growth, trimmed to this Doom-only core.
 *
 * Memory: allocations go through the zone allocator (z_zone.h remaps
 * malloc/realloc/calloc/free to Z_Malloc/Z_Realloc/Z_Calloc/Z_Free with
 * PU_STATIC).  Z_Close frees all PU_STATIC at content unload, so the table
 * pointers would dangle on a subsequent load; dsda_InitTables therefore
 * rebuilds everything from the pristine static seed arrays on every call.
 */

#include <stdlib.h>
#include <string.h>

#include "z_zone.h"
#include "doomtype.h"
#include "m_fixed.h"
#include "info.h"
#include "sounds.h"
#include "d_think.h"
#include "doomstat.h"
#include "heretic/heretic.h"
#include "dsda_hacked.h"

/* The five editable tables live in info.c / sounds.c. */
extern state_t     *states;
extern mobjinfo_t  *mobjinfo;
extern const char **sprnames;
extern sfxinfo_t   *S_sfx;
extern musicinfo_t *S_music;

/* The pristine static seed arrays. */
extern state_t      state_seed[];
extern mobjinfo_t   mobjinfo_seed[];
extern const char  *sprnames_seed[];
extern sfxinfo_t    S_sfx_seed[];
extern musicinfo_t  S_music_seed[];

int num_states;
int num_mobj_types;
int num_sprites;
int num_sfx;
int num_music;

actionf_t *deh_codeptr;

/* DEHEXTRA reserves a fixed block of "free" sound slots that mods (and
 * DSDHacked tools) reference by index without a Sounds block: sfx
 * 500..699 resolve to the WAD lumps DSFRE000..DSFRE199.  vesper et al.
 * rely on this.  We materialise the range at table-init time rather than
 * as a designated-initialiser seed (this core's .c is MSVC-style C89: no
 * [index]= initialisers).
 *
 * I_GetSfxLumpNum() builds the lump name as "ds%s" from sfx->name, so the
 * stored name omits the "ds" prefix (e.g. "pistol" -> DSPISTOL).  The
 * DEHEXTRA names are therefore "fre000".."fre199" -> DSFRE000..DSFRE199.
 *
 * The names must outlive the table and are never individually freed, so
 * back them with a file-scope static buffer that yields stable pointers. */
#define DEHEXTRA_SFX_FIRST 500
#define DEHEXTRA_SFX_COUNT 200
#define DEHEXTRA_SFX_END   (DEHEXTRA_SFX_FIRST + DEHEXTRA_SFX_COUNT) /* 700 */

static char dehextra_sfx_name[DEHEXTRA_SFX_COUNT][7]; /* "freNNN" + NUL */
static int  dehextra_sfx_ready;

static void dsda_BuildDehExtraSfxNames(void)
{
  int n;
  if (dehextra_sfx_ready)
    return;
  for (n = 0; n < DEHEXTRA_SFX_COUNT; n++)
  {
    char *s = dehextra_sfx_name[n];
    s[0] = 'f'; s[1] = 'r'; s[2] = 'e';
    s[3] = (char)('0' + (n / 100) % 10);
    s[4] = (char)('0' + (n / 10)  % 10);
    s[5] = (char)('0' + (n)       % 10);
    s[6] = '\0';
  }
  dehextra_sfx_ready = 1;
}

/* DEHEXTRA also reserves a fixed block of "free" sprite slots: the 100
 * names SP00..SP99 occupy sprite indices 145..244 in the standard
 * DEHEXTRA layout (dsda-doom ships them as stock sprnames entries).  This
 * Doom-only core's sprnames seed stops at the vanilla NUMSPRITES (144),
 * so mods built against the DEHEXTRA base (e.g. Eviternity II) referenced
 * sprites that did not exist -- their things rendered with the wrong
 * sprite or none at all.  Materialise the range here, same as the sounds
 * above; the [SPRITES] block then renames whichever slots the mod uses.
 *
 * sprnames entries are "const char *" and are not individually freed, so
 * back them with a stable file-scope buffer. */
#define DEHEXTRA_SPR_FIRST 145
#define DEHEXTRA_SPR_COUNT 100
#define DEHEXTRA_SPR_END   (DEHEXTRA_SPR_FIRST + DEHEXTRA_SPR_COUNT) /* 245 */

static char dehextra_spr_name[DEHEXTRA_SPR_COUNT][5]; /* "SPNN" + NUL */
static int  dehextra_spr_ready;

static void dsda_BuildDehExtraSpriteNames(void)
{
  int n;
  if (dehextra_spr_ready)
    return;
  for (n = 0; n < DEHEXTRA_SPR_COUNT; n++)
  {
    char *s = dehextra_spr_name[n];
    s[0] = 'S'; s[1] = 'P';
    s[2] = (char)('0' + (n / 10) % 10);
    s[3] = (char)('0' + (n)      % 10);
    s[4] = '\0';
  }
  dehextra_spr_ready = 1;
}

/* MUSINFO uses a scratch music slot one past the last real entry
 * (S_music[NUMMUSIC]); reserve it so that index stays in bounds. */
#define MUSIC_EXTRA 1

/* --- states ----------------------------------------------------------- */

static void reset_states(int from, int to)
{
  int i;
  for (i = from; i < to; ++i)
  {
    states[i].sprite    = SPR_TNT1;
    states[i].tics      = -1;
    states[i].nextstate = (statenum_t)i;
  }
}

static void ensure_states(int limit)
{
  while (limit >= num_states)
  {
    int old = num_states;
    num_states *= 2;

    states = realloc(states, num_states * sizeof(*states));
    memset(states + old, 0, (num_states - old) * sizeof(*states));

    deh_codeptr = realloc(deh_codeptr, num_states * sizeof(*deh_codeptr));
    memset(deh_codeptr + old, 0, (num_states - old) * sizeof(*deh_codeptr));

    reset_states(old, num_states);
  }
}

state_t *dsda_GetState(int index)
{
  ensure_states(index);
  return &states[index];
}

/* --- mobjinfo --------------------------------------------------------- */

static void reset_mobjinfo(int from, int to)
{
  int i;
  for (i = from; i < to; ++i)
  {
    mobjinfo[i].droppeditem      = MT_NULL;
    mobjinfo[i].infighting_group = IG_DEFAULT;
    mobjinfo[i].projectile_group = PG_DEFAULT;
    mobjinfo[i].splash_group     = SG_DEFAULT;
    mobjinfo[i].altspeed         = NO_ALTSPEED;
    mobjinfo[i].meleerange       = 64 * FRACUNIT;
    mobjinfo[i].doomednum        = -1;
  }
}

static void ensure_mobjinfo(int limit)
{
  while (limit >= num_mobj_types)
  {
    int old = num_mobj_types;
    num_mobj_types *= 2;

    mobjinfo = realloc(mobjinfo, num_mobj_types * sizeof(*mobjinfo));
    memset(mobjinfo + old, 0, (num_mobj_types - old) * sizeof(*mobjinfo));

    reset_mobjinfo(old, num_mobj_types);
  }
}

mobjinfo_t *dsda_GetMobjInfo(int index)
{
  ensure_mobjinfo(index);
  return &mobjinfo[index];
}

/* --- sprite names ----------------------------------------------------- */

static void ensure_sprites(int limit)
{
  while (limit >= num_sprites)
  {
    int old = num_sprites;
    num_sprites *= 2;

    sprnames = realloc(sprnames, num_sprites * sizeof(*sprnames));
    memset(sprnames + old, 0, (num_sprites - old) * sizeof(*sprnames));
  }
}

const char **dsda_GetSprite(int index)
{
  ensure_sprites(index);
  return &sprnames[index];
}

/* --- sfx -------------------------------------------------------------- */

static void reset_sfx(int from, int to)
{
  int i;
  for (i = from; i < to; ++i)
  {
    S_sfx[i].priority = 127;
    S_sfx[i].pitch    = -1;
    S_sfx[i].volume   = -1;
    /* S_Init seeds lumpnum/usefulness to -1 only over the static range;
     * mirror that for grown entries so S_StartSound's lazy
     * "if (lumpnum < 0) lumpnum = I_GetSfxLumpNum()" lookup actually runs.
     * Left at the memset's 0, an extended sound would be treated as
     * already-cached at lump 0 and play the wrong data. */
    S_sfx[i].lumpnum    = -1;
    S_sfx[i].usefulness = -1;
  }
}

static void ensure_sfx(int limit)
{
  while (limit >= num_sfx)
  {
    int old = num_sfx;
    num_sfx *= 2;

    S_sfx = realloc(S_sfx, num_sfx * sizeof(*S_sfx));
    memset(S_sfx + old, 0, (num_sfx - old) * sizeof(*S_sfx));

    reset_sfx(old, num_sfx);
  }
}

sfxinfo_t *dsda_GetSfx(int index)
{
  ensure_sfx(index);
  return &S_sfx[index];
}

/* --- music ------------------------------------------------------------ */

static void ensure_music(int limit)
{
  /* Keep the +MUSIC_EXTRA scratch slot valid as the table grows. */
  while (limit >= num_music)
  {
    int old = num_music;
    num_music *= 2;

    S_music = realloc(S_music, (num_music + MUSIC_EXTRA) * sizeof(*S_music));
    memset(S_music + old, 0, (num_music + MUSIC_EXTRA - old) * sizeof(*S_music));
  }
}

musicinfo_t *dsda_GetMusic(int index)
{
  ensure_music(index);
  return &S_music[index];
}

/* --- init ------------------------------------------------------------- */

/* Free the growable copies and point the globals back at the static seeds.
 * Called at teardown (before Z_Close) so that a subsequent dsda_InitTables
 * sees seed pointers rather than dangling ones. */
void dsda_FreeTables(void)
{
  if (states     && states   != state_seed)    free(states);
  if (mobjinfo   && mobjinfo != mobjinfo_seed)  free(mobjinfo);
  if (sprnames   && sprnames != sprnames_seed)  free(sprnames);
  if (S_sfx      && S_sfx    != S_sfx_seed)     free(S_sfx);
  if (S_music    && S_music  != S_music_seed)   free(S_music);
  if (deh_codeptr)                              free(deh_codeptr);

  states      = state_seed;
  mobjinfo    = mobjinfo_seed;
  sprnames    = sprnames_seed;
  S_sfx       = S_sfx_seed;
  S_music     = S_music_seed;
  deh_codeptr = NULL;
}

void dsda_InitTables(void)
{
  int i;

  /* Start fresh: free any previous content load's copies (a content swap
   * without an intervening Z_Close) and re-seed.  Because we always leave
   * the globals pointing at the seeds when not owning a heap copy, the
   * "!= seed" guard never frees a dangling pointer. */
  dsda_FreeTables();

  /* Seed source is game-dependent.  For Doom (the default) the dynamic
   * tables are seeded from the Doom statics exactly as before; for Heretic
   * they are seeded from the Heretic tables (heretic/info.c, heretic/
   * sounds.c).  States, sprites and sounds use Heretic's own 0-based index
   * spaces; mobjinfo is special (see below) because Heretic mobjtypes are
   * high-valued in the shared mobjtype_t enum. */
  if (heretic)
  {
    const state_t   *seed_states   = heretic_states;
    const char     **seed_sprnames = heretic_sprnames;
    const sfxinfo_t *seed_sfx      = heretic_S_sfx;
    int s;

    /* Raven game parameters needed at boot (added incrementally as the
     * boot path requires them). */
    g_mt_player = HERETIC_MT_PLAYER;
    g_s_play = HERETIC_S_PLAY;
    g_s_play_run1 = HERETIC_S_PLAY_RUN1;
    g_s_play_atk1 = HERETIC_S_PLAY_ATK1;
    g_s_play_atk2 = HERETIC_S_PLAY_ATK2;
    g_menu_flat = "FLAT513";

    num_states     = HERETIC_NUMSTATES;
    num_mobj_types = HERETIC_NUMMOBJTYPES; /* full span: Doom slots unused, Heretic at offset */
    num_sprites    = HERETIC_NUMSPRITES;
    num_sfx        = HERETIC_NUMSFX;
    num_music      = NUMMUSIC; /* music table shared/Doom for now */

    states = malloc(num_states * sizeof(*states));
    memcpy(states, seed_states, num_states * sizeof(*states));

    /* mobjinfo: allocate the full unified span and copy the Heretic slice
     * to [i + HERETIC_MT_ZERO], matching the enum numbering so runtime
     * mobjinfo[HERETIC_MT_*] indexes work directly. */
    mobjinfo = malloc(num_mobj_types * sizeof(*mobjinfo));
    memset(mobjinfo, 0, num_mobj_types * sizeof(*mobjinfo));
    for (s = 0; s < HERETIC_NUMMOBJTYPES - HERETIC_MT_ZERO; s++)
      mobjinfo[s + HERETIC_MT_ZERO] = heretic_mobjinfo[s];

    /* sprnames: 0-based, terminator slot after the names. */
    sprnames = malloc((num_sprites + 1) * sizeof(*sprnames));
    memcpy(sprnames, seed_sprnames, num_sprites * sizeof(*sprnames));
    sprnames[num_sprites] = NULL;

    S_sfx = malloc(num_sfx * sizeof(*S_sfx));
    memcpy(S_sfx, seed_sfx, num_sfx * sizeof(*S_sfx));
    /* Re-point any sound-alias links into the heap copy. */
    for (i = 0; i < num_sfx; i++)
      if (S_sfx[i].link)
        S_sfx[i].link = S_sfx + (S_sfx[i].link - seed_sfx);

    S_music = malloc((num_music + MUSIC_EXTRA) * sizeof(*S_music));
    memcpy(S_music, S_music_seed, num_music * sizeof(*S_music));
    memset(S_music + num_music, 0, MUSIC_EXTRA * sizeof(*S_music));

    deh_codeptr = malloc(num_states * sizeof(*deh_codeptr));
    for (i = 0; i < num_states; i++)
      deh_codeptr[i] = states[i].action;

    /* Heretic does not use the DEHEXTRA reserved sprite/sound ranges, so
     * the range-widening below is Doom-only and skipped here. */
    return;
  }

  g_mt_player = MT_PLAYER;
  g_s_play = S_PLAY;
  g_s_play_run1 = S_PLAY_RUN1;
  g_s_play_atk1 = S_PLAY_ATK1;
  g_s_play_atk2 = S_PLAY_ATK2;
  g_menu_flat = "FLOOR4_6";

  num_states     = NUMSTATES;
  num_mobj_types = NUMMOBJTYPES;
  num_sprites    = NUMSPRITES;
  num_sfx        = NUMSFX;
  num_music      = NUMMUSIC;

  states = malloc(num_states * sizeof(*states));
  memcpy(states, state_seed, num_states * sizeof(*states));

  mobjinfo = malloc(num_mobj_types * sizeof(*mobjinfo));
  memcpy(mobjinfo, mobjinfo_seed, num_mobj_types * sizeof(*mobjinfo));

  /* sprnames keeps its trailing NULL terminator slot (seed is [NUMSPRITES+1]
   * with sprnames_seed[NUMSPRITES] == NULL); copy it so any terminator-aware
   * consumer still sees a NULL after the real names. */
  {
    int spr_alloc = num_sprites;
    if (spr_alloc < DEHEXTRA_SPR_END)
      spr_alloc = DEHEXTRA_SPR_END;
    sprnames = malloc((spr_alloc + 1) * sizeof(*sprnames));
    memcpy(sprnames, sprnames_seed, num_sprites * sizeof(*sprnames));
    if (num_sprites < DEHEXTRA_SPR_END)
    {
      int s;
      /* zero the gap between the seed end and the DEHEXTRA range start */
      for (s = num_sprites; s < DEHEXTRA_SPR_FIRST; s++)
        sprnames[s] = NULL;
      dsda_BuildDehExtraSpriteNames();
      for (s = 0; s < DEHEXTRA_SPR_COUNT; s++)
        sprnames[DEHEXTRA_SPR_FIRST + s] = dehextra_spr_name[s];
      num_sprites = DEHEXTRA_SPR_END;
    }
    sprnames[num_sprites] = NULL;
  }

  S_sfx = malloc(num_sfx * sizeof(*S_sfx));
  memcpy(S_sfx, S_sfx_seed, num_sfx * sizeof(*S_sfx));
  /* Re-point sound-alias links (e.g. chgun -> pistol) into the heap copy so
   * pointer arithmetic (link - S_sfx) is computed against the right base. */
  for (i = 0; i < num_sfx; i++)
    if (S_sfx[i].link)
      S_sfx[i].link = S_sfx + (S_sfx[i].link - S_sfx_seed);

  /* DSDHacked/DEHEXTRA: grow to cover the reserved free-sound range and
   * name 500..699 -> DSFRE000..DSFRE199 (see dsda_BuildDehExtraSfxNames).
   * Only widen when the static seed does not already reach that far. */
  if (num_sfx < DEHEXTRA_SFX_END)
  {
    int old_sfx = num_sfx;
    sfxinfo_t *grown = malloc(DEHEXTRA_SFX_END * sizeof(*grown));
    memcpy(grown, S_sfx, old_sfx * sizeof(*grown));
    memset(grown + old_sfx, 0, (DEHEXTRA_SFX_END - old_sfx) * sizeof(*grown));
    /* links were fixed up relative to the previous S_sfx base; recompute
     * against the new base before discarding the old allocation. */
    for (i = 0; i < old_sfx; i++)
      if (grown[i].link)
        grown[i].link = grown + (S_sfx[i].link - S_sfx);
    free(S_sfx);
    S_sfx   = grown;
    num_sfx = DEHEXTRA_SFX_END;

    dsda_BuildDehExtraSfxNames();
    for (i = 0; i < DEHEXTRA_SFX_COUNT; i++)
    {
      sfxinfo_t *sfx = &S_sfx[DEHEXTRA_SFX_FIRST + i];
      sfx->name       = dehextra_sfx_name[i];
      sfx->singularity = FALSE;
      sfx->priority    = 127;
      sfx->pitch       = -1;
      sfx->volume      = -1;
      sfx->usefulness  = -1;
      sfx->lumpnum     = -1;
    }
  }

  /* +MUSIC_EXTRA for the MUSINFO scratch slot at index NUMMUSIC. */
  S_music = malloc((num_music + MUSIC_EXTRA) * sizeof(*S_music));
  memcpy(S_music, S_music_seed, num_music * sizeof(*S_music));
  memset(S_music + num_music, 0, MUSIC_EXTRA * sizeof(*S_music));

  deh_codeptr = calloc(num_states, sizeof(*deh_codeptr));
}
