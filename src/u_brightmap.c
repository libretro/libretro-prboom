/* u_brightmap.c: parse ZDoom GLDEFS/DOOMDEFS brightmap definitions.
 * See u_brightmap.h for the format and what a brightmap is. */

#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "doomstat.h"
#include "w_wad.h"
#include "lprintf.h"
#include "u_scanner.h"
#include "r_data.h"
#include "r_patch.h"
#include "r_state.h"
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

/* ---- per-texture mask build (stage 2) ---------------------------------- */

/* One built mask, column-major (mask[col*height + row]), 1 = fullbright.
 * Indexed for O(1) render lookup by texture number through bm_tex_index. */
static unsigned char **bm_tex_masks;    /* [numtextures], NULL if none      */
static int             bm_num_tex;
static int             bm_built;

/* Per-sprite-lump masks, indexed by (lump - firstspritelump), same
 * column-major layout as the texture masks. */
static unsigned char **bm_spr_masks;
static int             bm_num_spr;

/* PLAYPAL luminance >= this (0..255) counts as a "bright" mask texel.  A
 * brightmap is authored white-on-black, so the glowing texels land near
 * full white and the rest near black; the midpoint cleanly separates
 * them and tolerates the palette-nearest snap the mask PNG went through
 * when it was materialised. */
#define BM_LUMA_THRESHOLD 96

static unsigned char bm_bright_index[256];   /* palette index -> is-bright */

static void bm_build_luma_table(void)
{
  int        plump = (W_CheckNumForName)("PLAYPAL", ns_global);
  const unsigned char *pal;
  int        i;

  memset(bm_bright_index, 0, sizeof(bm_bright_index));
  if (plump < 0)
    return;
  pal = (const unsigned char *)W_CacheLumpNum(plump);
  for (i = 0; i < 256; i++)
  {
    int r = pal[i * 3 + 0];
    int g = pal[i * 3 + 1];
    int b = pal[i * 3 + 2];
    /* Rec.601 luma, integer */
    int y = (77 * r + 150 * g + 29 * b) >> 8;
    bm_bright_index[i] = (unsigned char)(y >= BM_LUMA_THRESHOLD);
  }
  W_UnlockLumpNum(plump);
}

/* Build a tw x th column-major bright mask from a materialised mask
 * patch.  The cached rpatch stores pixels column-major full height with
 * 0xff for transparent; a texel is bright when it is opaque and its
 * palette colour is luminous.  Nearest-resampled to the target size. */
static unsigned char *bm_mask_from_patch(int maplump, int tw, int th)
{
  const rpatch_t *p = R_CachePatchNum(maplump);
  unsigned char  *m;
  int             x, y, pw, ph;

  if (!p || p->width <= 0 || p->height <= 0)
  {
    if (p)
      R_UnlockPatchNum(maplump);
    return NULL;
  }
  pw = p->width;
  ph = p->height;
  m  = calloc((size_t)tw * (size_t)th, 1);
  if (!m)
  {
    R_UnlockPatchNum(maplump);
    return NULL;
  }

  for (x = 0; x < tw; x++)
  {
    int sx = x * pw / tw;
    const unsigned char *col = p->pixels + (size_t)sx * ph;
    for (y = 0; y < th; y++)
    {
      int sy = y * ph / th;
      unsigned char texel = col[sy];
      if (texel != 0xff && bm_bright_index[texel])
        m[(size_t)x * th + y] = 1;
    }
  }
  R_UnlockPatchNum(maplump);
  return m;
}

void U_BuildBrightmasks(void)
{
  int i, built = 0;

  if (bm_built)
    return;
  bm_built = true;
  if (!num_bmaps)
    return;

  bm_num_tex   = numtextures;
  bm_tex_masks = calloc((size_t)bm_num_tex, sizeof(*bm_tex_masks));
  if (!bm_tex_masks)
  {
    bm_num_tex = 0;
    return;
  }

  bm_build_luma_table();

  for (i = 0; i < num_bmaps; i++)
  {
    const brightmap_def_t *d = &bmaps[i];
    int texnum;

    int tw, th;

    if (d->kind != BM_TEXTURE)
      continue;                         /* flats/sprites: later stages */
    texnum = R_CheckTextureNumForName(d->name);
    if (texnum < 0 || texnum >= bm_num_tex || bm_tex_masks[texnum])
      continue;                         /* unknown, or already built */

    tw = textures[texnum]->width;
    th = textures[texnum]->height;
    if (tw <= 0 || th <= 0)
      continue;

    bm_tex_masks[texnum] = bm_mask_from_patch(d->maplump, tw, th);
    if (bm_tex_masks[texnum])
      built++;
  }

  if (built)
    lprintf(LO_INFO, "U_BuildBrightmasks: %d texture masks\n", built);

  /* Sprite masks: keyed by absolute sprite lump, stored by
   * (lump - firstspritelump).  The sprite frame is itself a patch, so the
   * mask is sized to its dimensions and indexed exactly like the sprite's
   * own column pixels at render time. */
  if (numspritelumps > 0)
  {
    int sbuilt = 0;
    bm_num_spr   = numspritelumps;
    bm_spr_masks = calloc((size_t)bm_num_spr, sizeof(*bm_spr_masks));
    if (!bm_spr_masks)
    {
      bm_num_spr = 0;
      return;
    }
    for (i = 0; i < num_bmaps; i++)
    {
      const brightmap_def_t *d = &bmaps[i];
      int lump, idx, sw, sh;
      const rpatch_t *sp;

      if (d->kind != BM_SPRITE)
        continue;
      lump = (W_CheckNumForName)(d->name, ns_sprites);
      if (lump < 0)
        lump = (W_CheckNumForName)(d->name, ns_global);
      if (lump < firstspritelump || lump > lastspritelump)
        continue;
      idx = lump - firstspritelump;
      if (bm_spr_masks[idx])
        continue;                       /* already built */

      sp = R_CachePatchNum(lump);
      if (!sp)
        continue;
      sw = sp->width;
      sh = sp->height;
      R_UnlockPatchNum(lump);
      if (sw <= 0 || sh <= 0)
        continue;

      bm_spr_masks[idx] = bm_mask_from_patch(d->maplump, sw, sh);
      if (bm_spr_masks[idx])
        sbuilt++;
    }
    if (sbuilt)
      lprintf(LO_INFO, "U_BuildBrightmasks: %d sprite masks\n", sbuilt);
  }
}

const unsigned char *U_BrightmaskForTexture(int texnum)
{
  if (!bm_tex_masks || texnum < 0 || texnum >= bm_num_tex)
    return NULL;
  return bm_tex_masks[texnum];
}

const unsigned char *U_BrightmaskForSprite(int spritelump)
{
  /* spritelump is relative to firstspritelump, as stored in vissprite.patch */
  if (!bm_spr_masks || spritelump < 0 || spritelump >= bm_num_spr)
    return NULL;
  return bm_spr_masks[spritelump];
}

void U_FreeBrightmaps(void)
{
  int i;
  if (bm_tex_masks)
  {
    for (i = 0; i < bm_num_tex; i++)
      free(bm_tex_masks[i]);
    free(bm_tex_masks);
    bm_tex_masks = NULL;
  }
  if (bm_spr_masks)
  {
    for (i = 0; i < bm_num_spr; i++)
      free(bm_spr_masks[i]);
    free(bm_spr_masks);
    bm_spr_masks = NULL;
  }
  bm_num_tex = 0;
  bm_num_spr = 0;
  bm_built   = 0;
  free(bmaps);
  bmaps = NULL;
  num_bmaps = cap_bmaps = 0;
  bm_loaded = false;
}
