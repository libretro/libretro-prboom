/* u_voxel.c: load KVX voxel models and resolve ZDoom VOXELDEF bindings.
 * See u_voxel.h for the format.  This stage builds the models and the
 * sprite->voxel map; the models are rasterised by R_DrawVoxel (r_voxel.c). */

#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "doomstat.h"
#include "w_wad.h"
#include "lprintf.h"
#include "u_scanner.h"
#include "info.h"            /* sprnames */
#include "dsda_hacked.h"     /* num_sprites */
#include "v_video.h"
#include "u_voxel.h"

/* One parsed VOXELDEF binding before sprite resolution. */
typedef struct
{
  char name[9];             /* voxel lump name (also the binding key)     */
  char sprite[5];           /* 4-char sprite name this voxel replaces     */
  dbool override_palette;
} voxdef_t;

static voxel_model_t *models;        /* one per resolved binding          */
static int            *sprite_model; /* [num_sprites] -> model index or -1 */
static int             num_models;
static int             sprite_model_len;
static dbool           vox_loaded;

/* ---- little-endian readers (KVX is LE) --------------------------------- */
static int rd_i32(const unsigned char *p)
{
  return (int)((unsigned)p[0] | ((unsigned)p[1] << 8) |
               ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24));
}
static int rd_i16(const unsigned char *p)
{
  int v = (int)((unsigned)p[0] | ((unsigned)p[1] << 8));
  return (v & 0x8000) ? v - 0x10000 : v;
}

/* Build palette_index -> PLAYPAL nearest-index remap for a model's own
 * 6-bit palette (KVX stores VGA 0..63 components). */
static void vox_build_remap(voxel_model_t *m)
{
  int plump = (W_CheckNumForName)("PLAYPAL", ns_global);
  const unsigned char *pal;
  int i, j;

  if (plump < 0)
  {
    for (i = 0; i < 256; i++)
      m->pal_remap[i] = (unsigned char)i;     /* identity fallback */
    return;
  }
  pal = (const unsigned char *)W_CacheLumpNum(plump);
  for (i = 0; i < 256; i++)
  {
    int r = (m->palette[i * 3 + 0] << 2);     /* 6-bit -> 8-bit */
    int g = (m->palette[i * 3 + 1] << 2);
    int b = (m->palette[i * 3 + 2] << 2);
    int best = 0, bestd = 0x7fffffff;
    for (j = 0; j < 256; j++)
    {
      int dr = r - pal[j * 3 + 0];
      int dg = g - pal[j * 3 + 1];
      int db = b - pal[j * 3 + 2];
      int d  = dr * dr + dg * dg + db * db;
      if (d < bestd) { bestd = d; best = j; }
    }
    m->pal_remap[i] = (unsigned char)best;
  }
  W_UnlockLumpNum(plump);
}

/* Parse one KVX lump into the model.  Returns 1 on success. */
static int vox_load_kvx(int lump, voxel_model_t *m)
{
  const unsigned char *d = (const unsigned char *)W_CacheLumpNum(lump);
  int len = W_LumpLength(lump);
  int xsiz, ysiz, zsiz, p, i, ncol, base, voxbase, datalen;
  const unsigned char *xoff, *xyoff, *voxptr, *palptr;
  int slabcap, sc;

  if (!d || len < 28 + 768)
    return 0;

  xsiz = rd_i32(d + 4);
  ysiz = rd_i32(d + 8);
  zsiz = rd_i32(d + 12);
  if (xsiz <= 0 || ysiz <= 0 || zsiz <= 0 ||
      xsiz > 1024 || ysiz > 1024 || zsiz > 1024)
    return 0;

  m->xsiz = xsiz; m->ysiz = ysiz; m->zsiz = zsiz;
  /* KVX pivots are 8.8 fixed; promote to 16.16. */
  m->xpiv = rd_i32(d + 16) << 8;
  m->ypiv = rd_i32(d + 20) << 8;
  m->zpiv = rd_i32(d + 24) << 8;

  p     = 28;
  xoff  = d + p;                       p += 4 * (xsiz + 1);
  xyoff = d + p;                       p += 2 * xsiz * (ysiz + 1);
  voxbase = p;
  /* the voxel data runs up to the trailing 768-byte palette */
  datalen = len - 768 - voxbase;
  if (datalen < 0)
    return 0;
  voxptr = d + voxbase;
  palptr = d + len - 768;

  /* own a copy of the voxel column bytes so slab col pointers stay valid
   * after the lump is unlocked */
  m->voxdata = malloc((size_t)datalen);
  if (!m->voxdata)
    return 0;
  memcpy(m->voxdata, voxptr, (size_t)datalen);
  memcpy(m->palette, palptr, 768);

  ncol = xsiz * ysiz;
  m->columns = calloc((size_t)ncol, sizeof(*m->columns));
  if (!m->columns)
    return 0;

  /* xoffset[0] is the file offset of the first column relative to voxbase;
   * subtract it to index into voxdata. */
  base = rd_i32(xoff);
  slabcap = 256;
  m->slabs = malloc((size_t)slabcap * sizeof(*m->slabs));
  if (!m->slabs)
    return 0;
  sc = 0;

  for (i = 0; i < xsiz; i++)
  {
    int y;
    int xo = rd_i32(xoff + 4 * i) - base;
    for (y = 0; y < ysiz; y++)
    {
      int s = xo + rd_i16(xyoff + 2 * (i * (ysiz + 1) + y));
      int e = xo + rd_i16(xyoff + 2 * (i * (ysiz + 1) + y + 1));
      voxcolumn_t *c = &m->columns[i * ysiz + y];
      c->first = sc;
      c->count = 0;
      if (s < 0 || e > datalen || s > e)
        continue;
      while (s + 3 <= e)
      {
        int ztop  = m->voxdata[s];
        int zleng = m->voxdata[s + 1];
        int flags = m->voxdata[s + 2];
        if (s + 3 + zleng > e)             /* truncated slab: stop column */
          break;
        if (sc == slabcap)
        {
          voxslab_t *ns;
          slabcap *= 2;
          ns = realloc(m->slabs, (size_t)slabcap * sizeof(*m->slabs));
          if (!ns)
            return 0;
          m->slabs = ns;
        }
        m->slabs[sc].ztop      = (unsigned char)ztop;
        m->slabs[sc].zleng     = (unsigned char)zleng;
        m->slabs[sc].cullflags = (unsigned char)flags;
        m->slabs[sc].pad       = 0;
        m->slabs[sc].col       = m->voxdata + s + 3;
        sc++;
        c->count++;
        s += 3 + zleng;
      }
    }
  }
  m->numslabs = sc;
  W_UnlockLumpNum(lump);
  return 1;
}

/* ---- VOXELDEF parsing -------------------------------------------------- */

static void vox_parse_defs(voxdef_t **out, int *nout)
{
  int lump = (W_CheckNumForName)("VOXELDEF", ns_global);
  u_scanner_t s;
  voxdef_t *defs = NULL;
  int n = 0, cap = 0;

  if (lump < 0)
    return;
  s = U_ScanOpen(W_CacheLumpNum(lump), W_LumpLength(lump), "VOXELDEF");

  /* grammar: NAME = "spritebase" [ { keywords } ]  (ZDoom also allows a
   * bare NAME with no '='; treat the token as both voxel and sprite) */
  while (U_GetNextToken(&s, TRUE))
  {
    voxdef_t d;
    if (s.token != TK_Identifier && s.token != TK_StringConst)
      continue;
    memset(&d, 0, sizeof(d));
    strncpy(d.name, s.string, 8);
    strncpy(d.sprite, s.string, 4);

    if (U_GetNextToken(&s, TRUE) && s.token == '=')
    {
      if (U_GetNextToken(&s, TRUE) &&
          (s.token == TK_StringConst || s.token == TK_Identifier))
        strncpy(d.sprite, s.string, 4);
      /* peek for an options block */
      if (U_GetNextToken(&s, TRUE) && s.token == '{')
      {
        while (U_GetNextToken(&s, TRUE) && s.token != '}')
          if (s.token == TK_Identifier &&
              !strcasecmp(s.string, "overridepalette"))
            d.override_palette = true;
      }
    }

    if (n == cap)
    {
      cap = cap ? cap * 2 : 8;
      defs = realloc(defs, (size_t)cap * sizeof(*defs));
      if (!defs) { U_ScanClose(&s); return; }
    }
    defs[n++] = d;
  }
  U_ScanClose(&s);
  *out = defs;
  *nout = n;
}

static int vox_sprite_num(const char *name4)
{
  int i;
  for (i = 0; i < num_sprites; i++)
    if (sprnames[i] && !strncasecmp(sprnames[i], name4, 4))
      return i;
  return -1;
}

void U_LoadVoxels(void)
{
  voxdef_t *defs = NULL;
  int       ndefs = 0, i;

  if (vox_loaded)
    return;
  vox_loaded = true;

  vox_parse_defs(&defs, &ndefs);
  if (!ndefs)
    return;

  sprite_model_len = num_sprites;
  sprite_model = malloc((size_t)sprite_model_len * sizeof(*sprite_model));
  if (!sprite_model)
  {
    free(defs);
    return;
  }
  for (i = 0; i < sprite_model_len; i++)
    sprite_model[i] = -1;

  models = calloc((size_t)ndefs, sizeof(*models));
  if (!models)
  {
    free(defs);
    return;
  }

  for (i = 0; i < ndefs; i++)
  {
    int lump = (W_CheckNumForName)(defs[i].name, ns_global);
    int sprnum;
    voxel_model_t *m;

    if (lump < 0)
      continue;
    sprnum = vox_sprite_num(defs[i].sprite);
    if (sprnum < 0 || sprnum >= sprite_model_len)
      continue;

    m = &models[num_models];
    memset(m, 0, sizeof(*m));
    strncpy(m->name, defs[i].name, 8);
    m->override_palette = defs[i].override_palette;
    if (!vox_load_kvx(lump, m))
    {
      free(m->voxdata); free(m->columns); free(m->slabs);
      memset(m, 0, sizeof(*m));
      continue;
    }
    vox_build_remap(m);
    sprite_model[sprnum] = num_models;
    num_models++;
  }

  free(defs);
  if (num_models)
    lprintf(LO_INFO, "U_LoadVoxels: %d voxel model(s)\n", num_models);
}

const voxel_model_t *U_VoxelForSprite(int spritenum)
{
  int idx;
  if (!sprite_model || spritenum < 0 || spritenum >= sprite_model_len)
    return NULL;
  idx = sprite_model[spritenum];
  return idx >= 0 ? &models[idx] : NULL;
}

int U_VoxelCount(void)
{
  return num_models;
}

void U_FreeVoxels(void)
{
  int i;
  for (i = 0; i < num_models; i++)
  {
    free(models[i].voxdata);
    free(models[i].columns);
    free(models[i].slabs);
  }
  free(models);
  free(sprite_model);
  models = NULL;
  sprite_model = NULL;
  num_models = sprite_model_len = 0;
  vox_loaded = false;
}
