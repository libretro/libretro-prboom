/* u_decaldef.c: parse ZDoom DECALDEF definitions.  See u_decaldef.h.
 * This stage builds the decal/decalgroup tables only; placement and
 * rendering come later. */

#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "m_fixed.h"
#include "w_wad.h"
#include "lprintf.h"
#include "u_scanner.h"
#include "r_data.h"          /* R_CheckTextureNumForName */
#include "u_decaldef.h"

static decaldef_t   *decals;
static int           num_decals, cap_decals;
static decalgroup_t *groups;
static int           num_groups, cap_groups;
static dbool         dd_loaded;

static int dd_find_decal(const char *name)
{
  int i;
  for (i = 0; i < num_decals; i++)
    if (!strcasecmp(decals[i].name, name))
      return i;
  return -1;
}

/* Skip a brace-delimited block whose '{' has not yet been consumed (used
 * for fader/stretcher/combiner/animator objects we don't model yet). */
static void dd_skip_block(u_scanner_t *s)
{
  int depth = 0;
  /* consume up to and including the matching close brace */
  while (U_GetNextToken(s, TRUE))
  {
    if (s->token == '{') depth++;
    else if (s->token == '}') { if (--depth <= 0) return; }
  }
}

static void dd_parse_decal(u_scanner_t *s)
{
  decaldef_t d;
  if (!U_GetNextToken(s, TRUE) ||
      (s->token != TK_Identifier && s->token != TK_StringConst))
    return;
  memset(&d, 0, sizeof(d));
  strncpy(d.name, s->string, sizeof(d.name) - 1);
  d.texnum = -1;
  d.xscale = d.yscale = FRACUNIT;
  d.alpha  = FRACUNIT;

  if (!U_GetNextToken(s, TRUE) || s->token != '{')
    return;

  while (U_GetNextToken(s, TRUE) && s->token != '}')
  {
    if (s->token != TK_Identifier)
      continue;
    if (!strcasecmp(s->string, "pic"))
    {
      if (U_GetNextToken(s, TRUE))
      {
        /* A decal pic is usually a graphic/patch (ZDoom keeps BSPLAT etc.
         * in graphics/), but may also be a wall texture.  Prefer a wall
         * texture; fall back to a patch lump in the global namespace. */
        d.texnum = R_CheckTextureNumForName(s->string);
        if (d.texnum >= 0)
          d.pic_is_patch = 0;
        else
        {
          int lump = (W_CheckNumForName)(s->string, ns_global);
          if (lump >= 0)
          {
            d.texnum = lump;
            d.pic_is_patch = 1;
          }
        }
      }
    }
    else if (!strcasecmp(s->string, "x") || !strcasecmp(s->string, "y"))
    {
      /* the scanner splits "x-scale" into 'x' '-' 'scale'; the '-' is
       * dropped, so we match 'x'/'y' then 'scale', then the number. */
      int isx = (s->string[0] == 'x' || s->string[0] == 'X');
      /* "x-scale" tokenises as 'x' '-' 'scale' NUMBER; consume the '-'
       * and the 'scale' word, then read the value. */
      U_CheckToken(s, '-');
      if (U_CheckToken(s, TK_Identifier) && !strcasecmp(s->string, "scale"))
      {
        if (U_CheckToken(s, TK_FloatConst))
        {
          fixed_t v = (fixed_t)(s->decimal * FRACUNIT);
          if (isx) d.xscale = v; else d.yscale = v;
        }
        else if (U_CheckToken(s, TK_IntConst))
        {
          fixed_t v = s->number << FRACBITS;
          if (isx) d.xscale = v; else d.yscale = v;
        }
      }
    }
    else if (!strcasecmp(s->string, "alpha") ||
             !strcasecmp(s->string, "translucent"))
    {
      if (U_GetNextToken(s, TRUE))
      {
        if (s->token == TK_FloatConst)
          d.alpha = (fixed_t)(s->decimal * FRACUNIT);
        else if (s->token == TK_IntConst)
          d.alpha = s->number << FRACBITS;
      }
    }
    else if (!strcasecmp(s->string, "add"))
    {
      d.flags |= DECAL_ADD;
      /* "add" is optionally followed by an amount (e.g. "add 1"); peek for
       * a number and consume it only if present. */
      if (U_CheckToken(s, TK_IntConst))
        d.alpha = s->number << FRACBITS;
      else if (U_CheckToken(s, TK_FloatConst))
        d.alpha = (fixed_t)(s->decimal * FRACUNIT);
    }
    else if (!strcasecmp(s->string, "fullbright"))
      d.flags |= DECAL_FULLBRIGHT;
    else if (!strcasecmp(s->string, "flipx"))
      d.flags |= DECAL_FLIPX;
    else if (!strcasecmp(s->string, "flipy"))
      d.flags |= DECAL_FLIPY;
    else if (!strcasecmp(s->string, "randomflipx"))
      d.flags |= DECAL_RANDFLIPX;
    else if (!strcasecmp(s->string, "randomflipy"))
      d.flags |= DECAL_RANDFLIPY;
    else if (!strcasecmp(s->string, "lowerdecal"))
    {
      if (U_GetNextToken(s, TRUE))
        strncpy(d.lowerdecal, s->string, sizeof(d.lowerdecal) - 1);
    }
    else if (!strcasecmp(s->string, "shade") ||
             !strcasecmp(s->string, "animator") ||
             !strcasecmp(s->string, "color") ||
             !strcasecmp(s->string, "colors"))
    {
      /* consume the single argument token (a string or identifier) */
      U_GetNextToken(s, TRUE);
    }
    /* any other bare keyword is ignored */
  }

  if (num_decals == cap_decals)
  {
    cap_decals = cap_decals ? cap_decals * 2 : 32;
    decals = realloc(decals, (size_t)cap_decals * sizeof(*decals));
  }
  decals[num_decals++] = d;
}

static void dd_parse_group(u_scanner_t *s)
{
  decalgroup_t g;
  int cap = 0;
  if (!U_GetNextToken(s, TRUE) ||
      (s->token != TK_Identifier && s->token != TK_StringConst))
    return;
  memset(&g, 0, sizeof(g));
  strncpy(g.name, s->string, sizeof(g.name) - 1);

  if (!U_GetNextToken(s, TRUE) || s->token != '{')
    return;

  /* members: NAME WEIGHT pairs until '}' */
  while (U_GetNextToken(s, TRUE) && s->token != '}')
  {
    int member, weight = 1;
    if (s->token != TK_Identifier && s->token != TK_StringConst)
      continue;
    member = dd_find_decal(s->string);
    /* optional weight follows the member name */
    if (U_CheckToken(s, TK_IntConst))
      weight = s->number;
    if (member < 0 || weight <= 0)
      continue;
    if (g.count == cap)
    {
      cap = cap ? cap * 2 : 8;
      g.members = realloc(g.members, (size_t)cap * sizeof(*g.members));
      g.weights = realloc(g.weights, (size_t)cap * sizeof(*g.weights));
    }
    g.members[g.count] = member;
    g.weights[g.count] = weight;
    g.total_weight += weight;
    g.count++;
  }

  if (g.count <= 0)
  {
    free(g.members); free(g.weights);
    return;
  }
  if (num_groups == cap_groups)
  {
    cap_groups = cap_groups ? cap_groups * 2 : 16;
    groups = realloc(groups, (size_t)cap_groups * sizeof(*groups));
  }
  groups[num_groups++] = g;
}

static void dd_parse_lump(int lump)
{
  u_scanner_t s = U_ScanOpen(W_CacheLumpNum(lump), W_LumpLength(lump),
                             "DECALDEF");
  while (U_GetNextToken(&s, TRUE))
  {
    if (s.token != TK_Identifier)
      continue;
    if (!strcasecmp(s.string, "decal"))
      dd_parse_decal(&s);
    else if (!strcasecmp(s.string, "decalgroup"))
      dd_parse_group(&s);
    else if (!strcasecmp(s.string, "fader") ||
             !strcasecmp(s.string, "stretcher") ||
             !strcasecmp(s.string, "slider") ||
             !strcasecmp(s.string, "colorchanger") ||
             !strcasecmp(s.string, "combiner") ||
             !strcasecmp(s.string, "animator"))
    {
      /* name then a { } block we don't model yet */
      U_GetNextToken(&s, TRUE);          /* the object's name */
      if (U_GetNextToken(&s, TRUE) && s.token == '{')
        dd_skip_block(&s);
    }
    /* "generator ACTOR DECAL" lines etc. are ignored */
  }
  U_ScanClose(&s);
}

/* Lightweight pre-scan of every DECALDEF lump for the "pic NAME" tokens,
 * with no dependency on the texture tables, so it can run before PNG
 * materialisation.  The result lets the materialiser cut the black field
 * out of grayscale decal-mask graphics (see U_PNGToPatchDecal). */
static char (*dd_pic_names)[16];
static int   dd_pic_count;

static void dd_scan_pics_lump(int lump)
{
  u_scanner_t s = U_ScanOpen(W_CacheLumpNum(lump), W_LumpLength(lump),
                             "DECALDEF");
  while (U_GetNextToken(&s, TRUE))
  {
    if (s.token == TK_Identifier && !strcasecmp(s.string, "pic"))
    {
      if (U_GetNextToken(&s, TRUE) &&
          (s.token == TK_Identifier || s.token == TK_StringConst))
      {
        dd_pic_names = realloc(dd_pic_names,
                               (size_t)(dd_pic_count + 1) * sizeof(*dd_pic_names));
        strncpy(dd_pic_names[dd_pic_count], s.string,
                sizeof(dd_pic_names[0]) - 1);
        dd_pic_names[dd_pic_count][sizeof(dd_pic_names[0]) - 1] = '\0';
        dd_pic_count++;
      }
    }
  }
  U_ScanClose(&s);
}

void U_ScanDecalPics(void)
{
  int lump = -1;
  if (dd_pic_names)
    return;
  while ((lump = (W_ListNumFromName)("DECALDEF", lump)) >= 0)
    dd_scan_pics_lump(lump);
}

int U_IsDecalPic(const char *name)
{
  int i;
  for (i = 0; i < dd_pic_count; i++)
    if (!strncasecmp(dd_pic_names[i], name, 8))
      return 1;
  return 0;
}

void U_LoadDecalDefs(void)
{
  int lump = -1;

  if (dd_loaded)
    return;
  dd_loaded = true;

  /* DECALDEF may appear in several wads; parse each (later ones append). */
  while ((lump = (W_ListNumFromName)("DECALDEF", lump)) >= 0)
    dd_parse_lump(lump);

  if (num_decals)
  {
    int i, resolved = 0;
    for (i = 0; i < num_decals; i++)
      if (decals[i].texnum >= 0)
        resolved++;
    lprintf(LO_INFO, "U_LoadDecalDefs: %d decal(s), %d group(s), "
            "%d/%d pic(s) resolved\n",
            num_decals, num_groups, resolved, num_decals);
    /* A ZDoom pack typically names stock decal graphics (BSPLAT, BLAST,
     * ...) that live in the engine's base resource (gzdoom.pk3), which this
     * core does not ship.  If nothing resolved, the decals parsed fine but
     * have no art to draw -- flag it so a missing resource is diagnosable
     * rather than silently producing invisible decals. */
    if (resolved == 0)
      lprintf(LO_WARN, "U_LoadDecalDefs: no decal pics found; their "
              "graphics are probably in a base resource that is not "
              "loaded\n");
  }
}

int U_DecalNumForName(const char *name)
{
  int i;
  /* exact decal first */
  i = dd_find_decal(name);
  if (i >= 0)
    return i;
  /* then a group: weighted random pick among its members */
  for (i = 0; i < num_groups; i++)
    if (!strcasecmp(groups[i].name, name))
    {
      decalgroup_t *g = &groups[i];
      /* Cosmetic-only: a private generator, never the game RNG (pr_misc),
       * so resolving a group during play cannot desync demos. */
      static unsigned grp_rng = 0x9e3779b9u;
      int r, j;
      grp_rng = grp_rng * 1103515245u + 12345u;
      r = (int)((grp_rng >> 16) % (unsigned)g->total_weight);
      for (j = 0; j < g->count; j++)
      {
        r -= g->weights[j];
        if (r < 0)
          return g->members[j];
      }
      return g->members[g->count - 1];
    }
  return -1;
}

const decaldef_t *U_DecalDef(int idx)
{
  if (idx < 0 || idx >= num_decals)
    return NULL;
  return &decals[idx];
}

int U_DecalCount(void)
{
  return num_decals;
}

void U_FreeDecalDefs(void)
{
  int i;
  for (i = 0; i < num_groups; i++)
  {
    free(groups[i].members);
    free(groups[i].weights);
  }
  free(groups);
  free(decals);
  groups = NULL;
  decals = NULL;
  num_decals = cap_decals = 0;
  num_groups = cap_groups = 0;
  dd_loaded = false;
}
