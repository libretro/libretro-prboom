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
#include "w_wad.h"
#include "lprintf.h"
#include "u_decorate.h"

#define MAX_DECORATE_ACTORS 512
#define MAX_NAME 64

typedef struct
{
  char name[MAX_NAME];
  char parent[MAX_NAME];
  char replaces[MAX_NAME];
  int  doomednum;               /* -1 if none */
} decorate_actor_t;

static decorate_actor_t actors[MAX_DECORATE_ACTORS];
static int num_actors;
static int parsed;              /* one-shot lazy parse */

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

static void parse_decorate(void)
{
  int            lump;
  size_t         len, i;
  const char    *txt;
  int            depth = 0;

  parsed = 1;
  lump = (W_CheckNumForName)("DECORATE", ns_global);
  if (lump < 0)
    return;
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
    else if (depth == 0 && (c == 'a' || c == 'A') && i + 6 < len &&
             !strncasecmp(txt + i, "actor", 5) &&
             (txt[i + 5] == ' ' || txt[i + 5] == '\t') &&
             (i == 0 || txt[i - 1] == '\n'))
    {
      size_t eol = i + 5;
      while (eol < len && txt[eol] != '\n' && txt[eol] != '{')
        eol++;
      parse_header(txt + i + 5, txt + eol);
      i = eol - 1;
    }
  }
  W_UnlockLumpNum(lump);

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
         (doomednum >= 32000 && doomednum <= 32003);  /* editor cameras */
}
