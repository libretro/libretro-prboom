/* u_decorate.c: DECORATE doomednum aliasing.
 *
 * ZDoom-targeted wads define new actors in a DECORATE lump this engine
 * cannot execute.  Many of those actors are reskinned base-game monsters:
 * they inherit (": Parent") or stand in for ("replaces Target") a class
 * that occupies a known editor number, and only carry a new doomednum so
 * maps can place the variant.  chex3.wad's Larva (9050) inherits
 * FlemoidusStridicus, which replaces FlemoidusCycloptisCommonus -- ZDoom's
 * chex name for editor number 3002 -- so the engine can spawn it as the
 * 3002 monster and the wad's sprite replacements supply the look.
 *
 * Only the actor HEADERS are parsed (name, parent, replaces, doomednum);
 * bodies are skipped by brace counting.  An unknown editor number is
 * resolved by walking parent-then-replaces links until reaching either a
 * DECORATE actor that has its own editor number or a base-game class name
 * with a known one.  Actors that root in classes without editor numbers
 * (brand-new decorations with their own sprites) cannot be aliased and
 * stay unspawned.
 *
 * U_IsInertZDoomThing covers ZDoom editor-only map things (particle
 * fountains, interpolation points, camera/view stacks, editor cameras)
 * so they are skipped without the unknown-thing message. */

#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "doomstat.h"
#include "info.h"
#include "m_fixed.h"
#include "w_wad.h"
#include "lprintf.h"
#include "dsda_hacked.h"
#include "u_decorate.h"

#define MAX_DECORATE_ACTORS 1024
#define MAX_NAME 64

typedef struct
{
  char name[MAX_NAME];
  char parent[MAX_NAME];
  char replaces[MAX_NAME];
  int  doomednum;               /* -1 if none */

  /* simple-prop registration (static single-frame actors only) */
  int  radius, height;          /* map units; -1 if unspecified */
  int  solid, nogravity, spawnceiling;
  char sprite[5];               /* 4-char sprite of a "SPRT F -1" spawn */
  int  frame;                   /* 0-based frame letter, FF_FULLBRIGHT or'd */
  int  spawn_static;            /* spawn state parsed and tics == -1 */

  /* animated Spawn sequence (no action functions): a chain of frames the
   * registrar turns into looping/terminating states.  Captured only for a
   * plain "SPRITE <letters> <tics>" sequence ending in loop/stop. */
#define MAX_SPAWN_FRAMES 32
  struct { short frame; short tics; } seq[MAX_SPAWN_FRAMES];
  char seq_sprite[5];           /* sprite the sequence uses (one sprite)  */
  int  seq_len;                 /* number of frames captured              */
  int  seq_loops;              /* 1 = loop back to frame 0, 0 = stop      */
  int  translucent;             /* RenderStyle Translucent / Add          */
  int  alpha;                   /* 16.16 alpha, FRACUNIT if unset         */
} decorate_actor_t;

static decorate_actor_t actors[MAX_DECORATE_ACTORS];
static int num_actors;

/* every 4-char sprite name appearing on a state line anywhere in the
 * DECORATE lump; R_InitSpriteDefs only unifies a sprite's art when the
 * lump actually redefines that sprite's sequence */
#define MAX_DECORATE_SPRITES 1024
static char sprite_names[MAX_DECORATE_SPRITES][5];
static int num_sprite_names;
static int parsed;              /* one-shot lazy parse */

static void parse_body(decorate_actor_t *a, const char *p, const char *end);

/* ZDoom class names with fixed editor numbers, for chains that leave the
 * DECORATE lump.  Doom monsters plus ZDoom's Chex Quest class names (from
 * gzdoom's mapinfo/chex.txt DoomEdNums). */
static const struct { const char *name; int dn; } base_classes[] =
{
  { "ZombieMan",                  3004 },
  { "ShotgunGuy",                 9    },
  { "ChaingunGuy",                65   },
  { "DoomImp",                    3001 },
  { "Demon",                      3002 },
  { "Spectre",                    58   },
  { "LostSoul",                   3006 },
  { "Cacodemon",                  3005 },
  { "HellKnight",                 69   },
  { "BaronOfHell",                3003 },
  { "Arachnotron",                68   },
  { "PainElemental",              71   },
  { "Revenant",                   66   },
  { "Fatso",                      67   },
  { "Archvile",                   64   },
  { "SpiderMastermind",           7    },
  { "Cyberdemon",                 16   },
  { "WolfensteinSS",              84   },
  { "FlemoidusCommonus",          3004 },
  { "FlemoidusBipedicus",         9    },
  { "ArmoredFlemoidusBipedicus",  3001 },
  { "FlemoidusCycloptisCommonus", 3002 },
  { "Flembrane",                  3003 },
  { "ChexSoul",                   3006 },
  { NULL, 0 }
};

static const char *skip_space(const char *p, const char *end)
{
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\r'))
    p++;
  return p;
}

static const char *read_word(const char *p, const char *end,
                             char *out, size_t outsz)
{
  size_t n = 0;
  while (p < end && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' &&
         *p != ':' && *p != '{')
  {
    if (n + 1 < outsz)
      out[n++] = *p;
    p++;
  }
  out[n] = 0;
  return p;
}

/* Parse one "actor ..." header line starting at p (just past "actor"). */
static void parse_header(const char *p, const char *end)
{
  decorate_actor_t *a;
  char word[MAX_NAME];

  if (num_actors == MAX_DECORATE_ACTORS)
    return;
  a = &actors[num_actors];
  memset(a, 0, sizeof(*a));
  a->doomednum = -1;
  a->radius = a->height = -1;
  a->alpha = FRACUNIT;

  p = skip_space(p, end);
  p = read_word(p, end, a->name, sizeof(a->name));
  if (!a->name[0])
    return;

  while (p < end && *p != '\n' && *p != '{')
  {
    p = skip_space(p, end);
    if (p < end && *p == ':')
    {
      p = skip_space(p + 1, end);
      p = read_word(p, end, a->parent, sizeof(a->parent));
      continue;
    }
    p = read_word(p, end, word, sizeof(word));
    if (!word[0])
      break;
    if (!strcasecmp(word, "replaces"))
    {
      p = skip_space(p, end);
      p = read_word(p, end, a->replaces, sizeof(a->replaces));
    }
    else if (word[0] >= '0' && word[0] <= '9')
      a->doomednum = atoi(word);
    /* anything else ("native", editor comments) is ignored */
  }
  num_actors++;
}

/* Lump name a pk3 gives an included file: basename after the last slash, up
 * to the first '.', uppercased, max 8 chars (mirrors pk3_lump_name). */
static void decorate_include_name(char out[9], const char *path, size_t plen)
{
  const char *base = path;
  size_t i, n = 0;
  for (i = 0; i < plen; i++)
    if (path[i] == '/' || path[i] == '\\')
      base = path + i + 1;
  for (i = (size_t)(base - path); i < plen && path[i] != '.' && n < 8; i++)
  {
    char c = path[i];
    out[n++] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
  }
  out[n] = '\0';
}

#define DECORATE_MAX_LUMPS 128
static int parsed_lumps[DECORATE_MAX_LUMPS];
static int num_parsed_lumps;

/* Parse one DECORATE lump's actor definitions, following any #include
 * directives to the lumps the archive synthesised for them.  ZDoom packs
 * (ZDCMP2 etc.) keep a near-empty top-level DECORATE that only #includes the
 * real actor files under actors/..., so without this every custom prop --
 * palms, lamps, corpses, glass -- reads as an unknown thing and never spawns. */
static void parse_decorate_lump(int lump, int incdepth)
{
  size_t      len, i;
  const char *txt;
  int         depth = 0, k;
  int         incs[DECORATE_MAX_LUMPS];
  int         ninc = 0;

  if (lump < 0 || incdepth > 16)
    return;
  for (k = 0; k < num_parsed_lumps; k++)
    if (parsed_lumps[k] == lump)
      return;                         /* already parsed (cycle or dup) */
  if (num_parsed_lumps < DECORATE_MAX_LUMPS)
    parsed_lumps[num_parsed_lumps++] = lump;

  len = W_LumpLength(lump);
  txt = W_CacheLumpNum(lump);

  for (i = 0; i < len; i++)
  {
    char c = txt[i];
    if (c == '{')
      depth++;
    else if (c == '}')
    {
      if (depth > 0)
        depth--;
    }
    else if (depth == 0 && c == '#' && i + 8 < len &&
             !strncasecmp(txt + i, "#include", 8) &&
             (i == 0 || txt[i - 1] == '\n' || txt[i - 1] == '\r'))
    {
      size_t j = i + 8;
      while (j < len && txt[j] != '"' && txt[j] != '\n')
        j++;
      if (j < len && txt[j] == '"')
      {
        size_t s = j + 1, e = j + 1;
        while (e < len && txt[e] != '"' && txt[e] != '\n')
          e++;
        if (e < len && txt[e] == '"')
        {
          char nm[9];
          int  inc;
          decorate_include_name(nm, txt + s, e - s);
          inc = (W_CheckNumForName)(nm, ns_global);
          if (inc >= 0 && ninc < DECORATE_MAX_LUMPS)
            incs[ninc++] = inc;
          j = e;
        }
      }
      i = j;
      continue;
    }
    else if (depth == 0 && (c == 'a' || c == 'A') && i + 6 < len &&
             !strncasecmp(txt + i, "actor", 5) &&
             (txt[i + 5] == ' ' || txt[i + 5] == '\t') &&
             (i == 0 || txt[i - 1] == '\n'))
    {
      size_t eol = i + 5;
      size_t body, bend;
      int    bdepth = 0;
      while (eol < len && txt[eol] != '\n' && txt[eol] != '{')
        eol++;
      parse_header(txt + i + 5, txt + eol);
      /* locate the body braces and parse the new actor's properties */
      body = eol;
      while (body < len && txt[body] != '{')
        body++;
      bend = body;
      while (bend < len)
      {
        if (txt[bend] == '{')
          bdepth++;
        else if (txt[bend] == '}' && --bdepth == 0)
          break;
        bend++;
      }
      if (num_actors > 0 && body < len && bend > body)
        parse_body(&actors[num_actors - 1], txt + body + 1, txt + bend);
      i = bend;
      depth = 0;
      continue;
    }
  }
  W_UnlockLumpNum(lump);

  /* recurse after unlocking, so at most incdepth lumps are cached at once */
  for (k = 0; k < ninc; k++)
    parse_decorate_lump(incs[k], incdepth + 1);
}

static void parse_decorate(void)
{
  int lump;

  parsed = 1;
  lump = (W_CheckNumForName)("DECORATE", ns_global);
  if (lump < 0)
    return;
  num_parsed_lumps = 0;
  parse_decorate_lump(lump, 0);

  if (num_actors)
    lprintf(LO_INFO, "U_ParseDecorate: %d actor headers\n", num_actors);
}

static const decorate_actor_t *find_actor(const char *name)
{
  int i;
  for (i = 0; i < num_actors; i++)
    if (!strcasecmp(actors[i].name, name))
      return &actors[i];
  return NULL;
}

static int base_class_doomednum(const char *name)
{
  int i;
  for (i = 0; base_classes[i].name; i++)
    if (!strcasecmp(base_classes[i].name, name))
      return base_classes[i].dn;
  return -1;
}

/* Resolve a class name to an editor number by walking parent-then-replaces
 * links.  `from` is the actor whose number we are resolving, so its own
 * doomednum (the unknown one) never terminates the walk. */
static int resolve_class(const char *name, const decorate_actor_t *from,
                         int depth)
{
  const decorate_actor_t *a;
  int dn;

  if (depth > 16)
    return -1;

  a = find_actor(name);
  if (!a)
    return base_class_doomednum(name);

  if (a != from && a->doomednum >= 0)
    return a->doomednum;
  if (a->parent[0])
  {
    dn = resolve_class(a->parent, from, depth + 1);
    if (dn >= 0)
      return dn;
  }
  if (a->replaces[0])
    return resolve_class(a->replaces, from, depth + 1);
  return -1;
}

/* Parse the body span of the most recently added actor for the few
 * properties simple static decorations use.  Anything beyond a single
 * "SPRT F -1" spawn frame leaves spawn_static unset and the actor is not
 * registered. */
static void parse_body(decorate_actor_t *a, const char *p, const char *end)
{
  char word[MAX_NAME];
  int  in_spawn = 0;

  while (p < end)
  {
    p = skip_space(p, end);
    if (p >= end)
      break;
    if (*p == '\n' || *p == '{' || *p == '}')
    {
      p++;
      continue;
    }
    p = read_word(p, end, word, sizeof(word));
    if (!word[0])
    {
      p++;
      continue;
    }

    /* read_word stops before ':': a state label arrives as the bare word
     * with the colon still unconsumed */
    if (p < end && *p == ':')
    {
      p++;
      in_spawn = !strcasecmp(word, "Spawn");
      continue;
    }

    if (!strcasecmp(word, "Radius"))
    {
      p = skip_space(p, end);
      p = read_word(p, end, word, sizeof(word));
      a->radius = atoi(word);
    }
    else if (!strcasecmp(word, "Height"))
    {
      p = skip_space(p, end);
      p = read_word(p, end, word, sizeof(word));
      a->height = atoi(word);
    }
    else if (!strcasecmp(word, "+SOLID"))
      a->solid = 1;
    else if (!strcasecmp(word, "+NOGRAVITY"))
      a->nogravity = 1;
    else if (!strcasecmp(word, "+SPAWNCEILING"))
      a->spawnceiling = 1;
    else if (!strcasecmp(word, "RenderStyle"))
    {
      char rs[MAX_NAME];
      p = skip_space(p, end);
      p = read_word(p, end, rs, sizeof(rs));
      if (!strcasecmp(rs, "Translucent") || !strcasecmp(rs, "Add") ||
          !strcasecmp(rs, "Stencil")     || !strcasecmp(rs, "Shaded"))
        a->translucent = 1;
    }
    else if (!strcasecmp(word, "Alpha"))
    {
      char av[MAX_NAME];
      p = skip_space(p, end);
      p = read_word(p, end, av, sizeof(av));
      /* "0.4" -> 16.16; atof avoided (no float determinism worry here,
       * but keep it integer-parsed for consistency) */
      {
        int whole = 0, frac = 0, scale = 1, seen_dot = 0;
        const char *s = av;
        while (*s)
        {
          if (*s == '.') seen_dot = 1;
          else if (*s >= '0' && *s <= '9')
          {
            if (!seen_dot) whole = whole * 10 + (*s - '0');
            else { frac = frac * 10 + (*s - '0'); scale *= 10; }
          }
          s++;
        }
        a->alpha = whole * FRACUNIT + (scale > 1 ? (frac * FRACUNIT) / scale : 0);
        if (a->alpha > FRACUNIT) a->alpha = FRACUNIT;
        if (a->alpha < 0)        a->alpha = 0;
      }
    }
    else if (in_spawn && !a->spawn_static && a->seq_len > 0 &&
             (!strcasecmp(word, "loop") || !strcasecmp(word, "stop") ||
              !strcasecmp(word, "wait")))
    {
      /* terminator for an animated Spawn sequence ("loop"/"wait" repeat,
       * "stop" freezes on the last frame) */
      a->seq_loops = strcasecmp(word, "stop") != 0;
      in_spawn = 0;
    }
    else if (strlen(word) == 4)
    {
      /* a state line is "SPRT ABCD 5 [action]": a 4-char word, a word of
       * frame letters, then a numeric tic count.  Record the sprite name;
       * properties never match (their value words are not all letters or
       * are not followed by a number). */
      char fr[MAX_NAME], tics[MAX_NAME];
      const char *q = skip_space(p, end);
      q = read_word(q, end, fr, sizeof(fr));
      q = skip_space(q, end);
      q = read_word(q, end, tics, sizeof(tics));
      if (fr[0] && (tics[0] == '-' || (tics[0] >= '0' && tics[0] <= '9')))
      {
        size_t fi;
        int letters = 1;
        for (fi = 0; fr[fi]; fi++)
          if (fr[fi] < 'A' || fr[fi] > '_')
            letters = 0;
        if (letters)
        {
          int k;
          for (k = 0; k < num_sprite_names; k++)
            if (!strncasecmp(sprite_names[k], word, 4))
              break;
          if (k == num_sprite_names && k < MAX_DECORATE_SPRITES)
          {
            memcpy(sprite_names[k], word, 4);
            sprite_names[k][4] = 0;
            num_sprite_names++;
          }
        }
      }
      if (in_spawn && !a->spawn_static)
      {
      /* A Spawn-state line is "SPRT <frames> <tics> [BRIGHT] [action]".
       * Two shapes are captured (action functions are otherwise ignored):
       *   "SPRT F -1"     -> a single frozen frame (spawn_static)
       *   "SPRT ABCD 10"  -> an animated sequence, terminated by loop/stop */
      char fr[MAX_NAME], tics[MAX_NAME];
      const char *q = skip_space(p, end);
      q = read_word(q, end, fr, sizeof(fr));
      q = skip_space(q, end);
      q = read_word(q, end, tics, sizeof(tics));
      if (strlen(fr) == 1 && fr[0] >= 'A' && fr[0] <= 'Z' &&
          !strcmp(tics, "-1"))
      {
        const char *r = skip_space(q, end);
        char b[MAX_NAME];
        memcpy(a->sprite, word, 4);
        a->sprite[4] = 0;
        a->frame = fr[0] - 'A';
        read_word(r, end, b, sizeof(b));
        if (!strcasecmp(b, "BRIGHT"))
          a->frame |= FF_FULLBRIGHT;
        a->spawn_static = 1;
        a->seq_len = 0;          /* a frozen frame supersedes any sequence */
        p = q;
      }
      else if (fr[0] >= 'A' && fr[0] <= '_' &&
               (tics[0] >= '0' && tics[0] <= '9') &&
               (a->seq_len == 0 ||
                !strncasecmp(a->seq_sprite, word, 4)))
      {
        /* animated frames: one entry per frame letter, all this sprite.
         * Only a single sprite per Spawn sequence is supported. */
        int t = atoi(tics);
        int bright = 0;
        const char *r = skip_space(q, end);
        char b[MAX_NAME];
        size_t fi;
        read_word(r, end, b, sizeof(b));
        if (!strcasecmp(b, "BRIGHT"))
          bright = FF_FULLBRIGHT;
        if (a->seq_len == 0)
        {
          memcpy(a->seq_sprite, word, 4);
          a->seq_sprite[4] = 0;
        }
        if (t < 0) t = 0;
        if (t > 32767) t = 32767;
        for (fi = 0; fr[fi] && a->seq_len < MAX_SPAWN_FRAMES; fi++)
        {
          if (fr[fi] < 'A' || fr[fi] > '_')
            continue;
          a->seq[a->seq_len].frame = (short)((fr[fi] - 'A') | bright);
          a->seq[a->seq_len].tics  = (short)t;
          a->seq_len++;
        }
        p = q;
      }
      }
    }
  }
}

static dbool engine_knows_doomednum(int dn)
{
  int i;
  for (i = 0; i < num_mobj_types; i++)
    if (mobjinfo[i].doomednum == dn)
      return true;
  return false;
}

/* Register static single-frame DECORATE decorations as real thing types
 * via the DSDHacked growable tables.  Must run before the first
 * P_FindDoomedNum call (its hash is built once) and before R_Init (the
 * sprite definitions are built once from sprnames); d_main calls this
 * right before R_Init, and only for the Doom game. */
void U_RegisterDecorateThings(void)
{
  int i, count = 0, count_mt = 0;
  /* The dsda tables double when their end is touched: allocate
   * sequentially from the counts captured here so ten registrations cost
   * one doubling, not ten. */
  int st_base = num_states;
  int mt_base = num_mobj_types;
  int sp_next = num_sprites;

  if (!parsed)
    parse_decorate();

  for (i = 0; i < num_actors; i++)
  {
    decorate_actor_t *a = &actors[i];
    int st, mt, sp, k;
    mobjinfo_t *info;
    state_t *state;
    int nframes;
    const char *spr;

    if (a->doomednum < 0)
      continue;
    if (!a->spawn_static && a->seq_len <= 0)
      continue;                 /* nothing renderable captured            */
    if (engine_knows_doomednum(a->doomednum))
      continue;
    if (resolve_class(a->name, a, 0) >= 0)
      continue;                 /* monsters etc. handled by aliasing      */

    /* a frozen single frame is a one-entry sequence */
    nframes = a->spawn_static ? 1 : a->seq_len;
    spr     = a->spawn_static ? a->sprite : a->seq_sprite;

    sp = -1;
    for (k = 0; k < sp_next; k++)
      if (sprnames[k] && !strncasecmp(sprnames[k], spr, 4))
      {
        sp = k;
        break;
      }
    if (sp < 0)
    {
      sp = sp_next++;
      *dsda_GetSprite(sp) = strdup(spr);
    }

    /* one state per frame; nextstate chains forward, the last frame loops
     * to the first (animated loop) or freezes on itself (static / stop) */
    {
      int first = st_base + count;
      int f;
      for (f = 0; f < nframes; f++)
      {
        int cur  = st_base + count + f;
        int last = (f == nframes - 1);
        state = dsda_GetState(cur);
        state->sprite = sp;
        if (a->spawn_static)
        {
          state->frame = a->frame;
          state->tics  = -1;
          state->nextstate = cur;
        }
        else
        {
          state->frame = a->seq[f].frame;
          state->tics  = a->seq[f].tics;
          state->nextstate = last
            ? (a->seq_loops ? first : cur)   /* loop back or freeze */
            : cur + 1;
        }
      }
      st = first;
    }

    mt = mt_base + count_mt;
    info = dsda_GetMobjInfo(mt);
    info->doomednum   = a->doomednum;
    info->spawnstate  = st;
    info->spawnhealth = 1000;
    info->mass        = 100;
    info->radius      = (a->radius >= 0 ? a->radius : 20) * FRACUNIT;
    info->height      = (a->height >= 0 ? a->height : 16) * FRACUNIT;
    info->flags       = (a->solid ? MF_SOLID : 0) |
                        (a->nogravity ? MF_NOGRAVITY : 0) |
                        (a->spawnceiling ? (MF_SPAWNCEILING | MF_NOGRAVITY)
                                         : 0) |
                        ((a->translucent || a->alpha < FRACUNIT)
                                         ? MF_TRANSLUCENT : 0);
    count    += nframes;        /* states consumed */
    count_mt += 1;              /* one mobj type   */
  }

  if (count_mt)
    lprintf(LO_INFO, "U_RegisterDecorateThings: %d decorations (%d states)\n",
            count_mt, count);
}

/* does the DECORATE lump redefine this sprite's state sequence? */
dbool U_DecorateMentionsSprite(const char *name)
{
  int i;
  if (!parsed)
    parse_decorate();
  for (i = 0; i < num_sprite_names; i++)
    if (!strncasecmp(sprite_names[i], name, 4))
      return true;
  return false;
}

int U_DecorateAliasDoomedNum(int doomednum)
{
  int i;

  if (!parsed)
    parse_decorate();

  for (i = 0; i < num_actors; i++)
    if (actors[i].doomednum == doomednum)
    {
      int dn = resolve_class(actors[i].name, &actors[i], 0);
      if (dn >= 0 && dn != doomednum)
        lprintf(LO_INFO, "U_DecorateAliasDoomedNum: %s %d -> %d\n",
                actors[i].name, doomednum, dn);
      return (dn != doomednum) ? dn : -1;
    }
  return -1;
}

dbool U_IsInertZDoomThing(int doomednum)
{
  return (doomednum >= 9027 && doomednum <= 9033) ||  /* particle fountains */
         (doomednum >= 9070 && doomednum <= 9078) ||  /* interp/camera/stacks */
         (doomednum >= 32000 && doomednum <= 32003) ||/* editor cameras */
         doomednum == 9024 ||                         /* patrol point */
         doomednum == 9025 ||                         /* security camera */
         doomednum == 9026 ||                         /* spark */
         (doomednum >= 9080 && doomednum <= 9082) ||  /* sky viewpoint/picker/silencer */
         (doomednum >= 9800 && doomednum <= 9859) ||  /* GZDoom dynamic lights */
         (doomednum >= 14001 && doomednum <= 14067) ||/* ambient sounds */
         (doomednum >= 1400 && doomednum <= 1410);    /* sound sequence overrides */
}

/* Spawnable ZDoom utility markers: invisible, intangible mobjs whose
 * whole purpose is to carry a TID -- ACS SpawnSpot anchors and teleport
 * destinations.  Cloned from MT_TELEPORTMAN (S_NULL spawn state,
 * NOBLOCKMAP|NOSECTOR), with actor names so ACS ThingCountName can see
 * them. */
void U_RegisterZDoomUtilityThings(void)
{
  static const struct { int ednum; const char *name; } spots[] =
  {
    { 9001, "MapSpot" },
    { 9013, "MapSpotGravity" },
    { 9043, "TeleportDest3" },
    { 9044, "TeleportDest2" },
  };
  size_t i;

  int base = num_mobj_types;

  /* one growth covers all entries (the table doubles when touched) */
  dsda_GetMobjInfo(base + (int)(sizeof(spots) / sizeof(spots[0])) - 1);
  for (i = 0; i < sizeof(spots) / sizeof(spots[0]); i++)
  {
    mobjinfo_t *info = &mobjinfo[base + (int)i];
    *info = mobjinfo[MT_TELEPORTMAN];
    info->doomednum = spots[i].ednum;
    info->actorname = spots[i].name;
  }
}
