/* u_dynlight.c: parse ZDoom GLDEFS point-light definitions and their sprite
 * frame bindings.  See u_dynlight.h for the format and scope. */

#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "doomstat.h"
#include "m_fixed.h"
#include "tables.h"
#include "w_wad.h"
#include "lprintf.h"
#include "u_scanner.h"
#include "u_dynlight.h"

static dynlight_def_t *defs;
static int             num_defs, cap_defs;

typedef struct
{
  char sprite[8];      /* 4-char sprite name, NUL-padded */
  int  defidx;
} dynlight_bind_t;

static dynlight_bind_t *binds;
static int              num_binds, cap_binds;
static dbool            dl_loaded;

static const char *const defs_lumps[] =
{
  "GLDEFS", "DOOMDEFS", "HTICDEFS", "HEXNDEFS", "STRFDEFS"
};

static int dl_find_def(const char *name)
{
  int i;
  for (i = 0; i < num_defs; i++)
    if (!strcasecmp(defs[i].name, name))
      return i;
  return -1;
}

static dynlight_def_t *dl_add_def(void)
{
  if (num_defs == cap_defs)
  {
    int nc = cap_defs ? cap_defs * 2 : 32;
    dynlight_def_t *nd = realloc(defs, nc * sizeof(*nd));
    if (!nd)
      return NULL;
    defs = nd;
    cap_defs = nc;
  }
  return &defs[num_defs];
}

static void dl_add_bind(const char *sprite, int defidx)
{
  dynlight_bind_t *b;
  if (num_binds == cap_binds)
  {
    int nc = cap_binds ? cap_binds * 2 : 64;
    dynlight_bind_t *nb = realloc(binds, nc * sizeof(*nb));
    if (!nb)
      return;
    binds = nb;
    cap_binds = nc;
  }
  b = &binds[num_binds++];
  memset(b->sprite, 0, sizeof(b->sprite));
  memcpy(b->sprite, sprite, 4);
  b->defidx = defidx;
}

/* read a numeric token as a double (handles int/float and a leading '-') */
static dbool dl_number(u_scanner_t *s, double *out)
{
  if (!U_GetNextToken(s, TRUE))
    return false;
  if (s->token == '-')
  {
    if (!U_GetNextToken(s, TRUE))
      return false;
    if (s->token == TK_IntConst)   *out = -(double)s->number;
    else if (s->token == TK_FloatConst) *out = -s->decimal;
    else return false;
    return true;
  }
  if (s->token == TK_IntConst)        *out = (double)s->number;
  else if (s->token == TK_FloatConst) *out = s->decimal;
  else return false;
  return true;
}

/* skip a brace-balanced block whose '{' has just been consumed */
static void dl_skip_block(u_scanner_t *s)
{
  int depth = 1;
  while (depth > 0 && U_GetNextToken(s, TRUE))
  {
    if (s->token == '{')      depth++;
    else if (s->token == '}') depth--;
  }
}

/* parse a "<kind> NAME { color R G B; size N; ... }" light block; the kind
 * keyword has already been consumed.  `kind` is the dynlight_kind_t. */
static void dl_parse_light(u_scanner_t *s, int kind)
{
  dynlight_def_t d;
  double v0, v1, v2;

  memset(&d, 0, sizeof(d));
  d.r = d.g = d.b = 1.0f;
  d.size = 0;

  if (!U_GetNextToken(s, TRUE) ||
      (s->token != TK_Identifier && s->token != TK_StringConst))
    return;
  strncpy(d.name, s->string, sizeof(d.name) - 1);
  if (!U_CheckToken(s, '{'))
    return;

  while (U_GetNextToken(s, TRUE) && s->token != '}')
  {
    if (s->token != TK_Identifier)
      continue;
    if (!strcasecmp(s->string, "color"))
    {
      if (dl_number(s, &v0) && dl_number(s, &v1) && dl_number(s, &v2))
      {
        /* accept either 0..1 or 0..255 colour scales */
        if (v0 > 1.0 || v1 > 1.0 || v2 > 1.0)
          { v0 /= 255.0; v1 /= 255.0; v2 /= 255.0; }
        d.r = (float)v0; d.g = (float)v1; d.b = (float)v2;
      }
    }
    else if (!strcasecmp(s->string, "size") || !strcasecmp(s->string, "radius"))
    {
      if (dl_number(s, &v0))
        d.size = (int)v0;
    }
    else if (!strcasecmp(s->string, "secondarysize") ||
             !strcasecmp(s->string, "secondaryradius"))
    {
      if (dl_number(s, &v0))
        d.size2 = (int)v0;
    }
    else if (!strcasecmp(s->string, "interval"))   /* pulse: seconds */
    {
      if (dl_number(s, &v0))
        d.interval = (int)(v0 * TICRATE + 0.5);
    }
    else if (!strcasecmp(s->string, "chance"))      /* flicker: 0..1 */
    {
      if (dl_number(s, &v0))
        d.chance = (int)(v0 * 360.0 + 0.5);
    }
    /* offset/subtractive/attenuate/... : ignored */
  }

  /* Resolve the animation kind now, folding degenerate cases back to a static
   * point light so the renderer never has to guard against them. */
  d.kind = kind;
  if (kind == DL_PULSE && (d.size2 <= 0 || d.interval <= 0))
    d.kind = DL_POINT;
  if (kind == DL_FLICKER)
  {
    if (d.size2 <= 0)   d.kind   = DL_POINT;
    if (d.chance <= 0)  d.chance = 180;   /* GLDEFS default ~50% */
  }

  /* luminous strength = brightest channel; skip pure-black lights */
  d.strength = d.r > d.g ? (d.r > d.b ? d.r : d.b) : (d.g > d.b ? d.g : d.b);
  if (d.size > 0 && d.strength > 0.0f && dl_find_def(d.name) < 0)
  {
    dynlight_def_t *slot = dl_add_def();
    if (slot)
    {
      *slot = d;
      num_defs++;
    }
  }
}

/* parse an "object CLASS { frame SPR { light NAME } ... }" block; the object
 * keyword has already been consumed.  Bindings are keyed by the sprite. */
static void dl_parse_object(u_scanner_t *s)
{
  /* class name */
  if (!U_GetNextToken(s, TRUE) ||
      (s->token != TK_Identifier && s->token != TK_StringConst))
    return;
  if (!U_CheckToken(s, '{'))
    return;

  while (U_GetNextToken(s, TRUE) && s->token != '}')
  {
    if (s->token == TK_Identifier && !strcasecmp(s->string, "frame"))
    {
      char spr[8];
      /* the frame token gives the sprite; take its first four chars */
      if (!U_GetNextToken(s, TRUE) ||
          (s->token != TK_Identifier && s->token != TK_StringConst))
        continue;
      memset(spr, 0, sizeof(spr));
      strncpy(spr, s->string, 4);
      if (!U_CheckToken(s, '{'))
        continue;
      while (U_GetNextToken(s, TRUE) && s->token != '}')
      {
        if (s->token == TK_Identifier && !strcasecmp(s->string, "light"))
        {
          if (U_GetNextToken(s, TRUE) &&
              (s->token == TK_Identifier || s->token == TK_StringConst))
          {
            int di = dl_find_def(s->string);
            if (di >= 0 && spr[0])
              dl_add_bind(spr, di);
          }
        }
      }
    }
    else if (s->token == '{')
      dl_skip_block(s);          /* unknown nested block */
  }
}

static void dl_parse_lump(int lump)
{
  u_scanner_t s = U_ScanOpen(W_CacheLumpNum(lump), W_LumpLength(lump),
                             "GLDEFS");
  while (U_GetNextToken(&s, TRUE))
  {
    if (s.token != TK_Identifier)
      continue;
    if (!strcasecmp(s.string, "pointlight"))
      dl_parse_light(&s, DL_POINT);
    else if (!strcasecmp(s.string, "pulselight"))
      dl_parse_light(&s, DL_PULSE);
    else if (!strcasecmp(s.string, "flickerlight"))
      dl_parse_light(&s, DL_FLICKER);
    else if (!strcasecmp(s.string, "object"))
      dl_parse_object(&s);
    else if (U_CheckToken(&s, '{'))
      dl_skip_block(&s);         /* skybox / glow / brightmap / ... */
  }
  U_ScanClose(&s);
  W_UnlockLumpNum(lump);
}

void U_LoadDynLights(void)
{
  size_t k;

  if (dl_loaded)
    return;
  dl_loaded = true;

  for (k = 0; k < sizeof(defs_lumps) / sizeof(defs_lumps[0]); k++)
  {
    int lump = -1;
    while ((lump = W_FindNumFromName(defs_lumps[k], lump)) >= 0)
      dl_parse_lump(lump);
  }

  if (num_binds)
    lprintf(LO_INFO, "U_LoadDynLights: %d lights, %d sprite bindings\n",
            num_defs, num_binds);
}

int U_DynLightRadius(const dynlight_def_t *d, int tics, unsigned seed)
{
  if (d->kind == DL_PULSE)
  {
    /* sine oscillation size <-> size2 over `interval` tics (finesine keeps
     * it integer and deterministic; phase locked to leveltime). */
    unsigned ang  = ((unsigned)tics * FINEANGLES / (unsigned)d->interval)
                    & (FINEANGLES - 1);
    int      sinv = finesine[ang];              /* -FRACUNIT..FRACUNIT     */
    int      frac = (sinv + FRACUNIT) >> 1;     /* 0..FRACUNIT             */
    return d->size + (int)(((int64_t)(d->size2 - d->size) * frac) >> FRACBITS);
  }
  if (d->kind == DL_FLICKER)
  {
    /* per-tic hash of (tics, seed): deterministic, per-light, no game RNG. */
    unsigned h = (unsigned)tics * 1664525u + seed * 1013904223u;
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
    return ((int)(h % 360u) < d->chance) ? d->size : d->size2;
  }
  return d->size;
}

const dynlight_def_t *U_DynLightForSprite(const char *sprname)
{
  int i;
  if (!num_binds)
    return NULL;
  for (i = 0; i < num_binds; i++)
    if (!strncasecmp(binds[i].sprite, sprname, 4))
      return &defs[binds[i].defidx];
  return NULL;
}

dbool U_DynLightsPresent(void)
{
  return num_binds > 0;
}

int U_DynLightCount(void)
{
  return num_defs;
}

void U_FreeDynLights(void)
{
  free(defs);  defs = NULL;   num_defs = cap_defs = 0;
  free(binds); binds = NULL;  num_binds = cap_binds = 0;
  dl_loaded = false;
}
