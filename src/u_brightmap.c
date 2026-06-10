/* u_brightmap.c: parse ZDoom GLDEFS/DOOMDEFS brightmap definitions.
 * See u_brightmap.h for the format and what a brightmap is. */

#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "doomstat.h"
#include "w_wad.h"
#include "lprintf.h"
#include "u_scanner.h"
#include "u_brightmap.h"

static brightmap_def_t *bmaps;
static int              num_bmaps;
static int              cap_bmaps;
static dbool            bm_loaded;

/* The *DEFS lumps GLDEFS-family parsers scan.  ZDoom reads brightmap
 * blocks out of any of these; ZDCMP2 ships its set in doomdefs.bm, which
 * the pk3 translator names DOOMDEFS. */
static const char *const defs_lumps[] =
{
  "GLDEFS", "DOOMDEFS", "HTICDEFS", "HEXNDEFS", "STRFDEFS"
};

static void bm_copy_name(char *dst, const char *src)
{
  size_t n = strlen(src);
  if (n > 8)
    n = 8;
  memset(dst, 0, 9);
  memcpy(dst, src, n);
}

static brightmap_def_t *bm_add(void)
{
  if (num_bmaps == cap_bmaps)
  {
    int nc = cap_bmaps ? cap_bmaps * 2 : 256;
    brightmap_def_t *nb = realloc(bmaps, nc * sizeof(*nb));
    if (!nb)
      return NULL;
    bmaps = nb;
    cap_bmaps = nc;
  }
  return &bmaps[num_bmaps];
}

/* Resolve a ZDoom path like "brightmaps/comp2.png" to a lump number.  The
 * pk3 translator names a member by its basename up to the first '.',
 * uppercased and truncated to 8 (so brightmaps/comp2.png -> COMP2), which
 * is exactly W_CheckNumForName's key. */
static int bm_resolve_map(const char *path)
{
  const char *base;
  char        name[9];
  int         i;

  base = strrchr(path, '/');
  base = base ? base + 1 : path;
  for (i = 0; i < 8 && base[i] && base[i] != '.'; i++)
    name[i] = (char)((base[i] >= 'a' && base[i] <= 'z')
                       ? base[i] - 'a' + 'A' : base[i]);
  name[i] = 0;
  return (W_CheckNumForName)(name, ns_global);
}

/* Parse one "brightmap <kind> <name> { ... }" block.  The "brightmap"
 * token has just been consumed.  Returns with the closing '}' consumed. */
static void bm_parse_block(u_scanner_t *s)
{
  brightmap_kind_t kind;
  char             target[9];
  char             mappath[128];
  int              have_map = 0;

  if (!U_GetNextToken(s, TRUE))
    return;
  if (!strcasecmp(s->string, "texture"))
    kind = BM_TEXTURE;
  else if (!strcasecmp(s->string, "flat"))
    kind = BM_FLAT;
  else if (!strcasecmp(s->string, "sprite"))
    kind = BM_SPRITE;
  else
    return;                       /* unknown brightmap kind */

  if (!U_GetNextToken(s, TRUE))
    return;
  bm_copy_name(target, s->string);

  /* opening brace */
  if (!U_GetNextToken(s, TRUE) || s->token != '{')
    return;

  mappath[0] = 0;
  while (U_GetNextToken(s, TRUE) && s->token != '}')
  {
    if (s->token == TK_Identifier && !strcasecmp(s->string, "map"))
    {
      if (U_GetNextToken(s, TRUE) &&
          (s->token == TK_StringConst || s->token == TK_Identifier))
      {
        strncpy(mappath, s->string, sizeof(mappath) - 1);
        mappath[sizeof(mappath) - 1] = 0;
        have_map = 1;
      }
    }
    /* other keys (iwad, disablefullbright, ...) are ignored */
  }

  if (have_map)
  {
    int lump = bm_resolve_map(mappath);
    if (lump >= 0)
    {
      brightmap_def_t *d = bm_add();
      if (d)
      {
        memcpy(d->name, target, sizeof(d->name));
        d->kind    = kind;
        d->maplump = lump;
        num_bmaps++;
      }
    }
  }
}

static void bm_parse_lump(int lump)
{
  u_scanner_t s;

  s = U_ScanOpen(W_CacheLumpNum(lump), W_LumpLength(lump), "GLDEFS");
  while (U_GetNextToken(&s, TRUE))
  {
    if (s.token == TK_Identifier && !strcasecmp(s.string, "brightmap"))
      bm_parse_block(&s);
    /* every other top-level GLDEFS structure (glow, skybox, material,
     * pointlight, ...) is skipped token-by-token; brace blocks are
     * walked through by the main loop without special handling. */
  }
  U_ScanClose(&s);
  W_UnlockLumpNum(lump);
}

void U_LoadBrightmaps(void)
{
  size_t k;

  if (bm_loaded)
    return;
  bm_loaded = true;

  for (k = 0; k < sizeof(defs_lumps) / sizeof(defs_lumps[0]); k++)
  {
    int lump = -1;
    /* W_ListNumFromName walks the hash chain so multiple lumps of the
     * same name (e.g. a pwad GLDEFS over the iwad's) all get parsed. */
    while ((lump = W_ListNumFromName(defs_lumps[k], lump)) >= 0)
      bm_parse_lump(lump);
  }

  if (num_bmaps)
    lprintf(LO_INFO, "U_LoadBrightmaps: %d brightmap definitions\n",
            num_bmaps);
}

const brightmap_def_t *U_BrightmapFor(const char *name, brightmap_kind_t kind)
{
  char key[9];
  int  i;

  if (!num_bmaps)
    return NULL;
  bm_copy_name(key, name);
  for (i = 0; i < num_bmaps; i++)
    if (bmaps[i].kind == kind && !strncasecmp(bmaps[i].name, key, 8))
      return &bmaps[i];
  return NULL;
}

int U_BrightmapCount(void)
{
  return num_bmaps;
}

void U_FreeBrightmaps(void)
{
  free(bmaps);
  bmaps = NULL;
  num_bmaps = cap_bmaps = 0;
  bm_loaded = false;
}
