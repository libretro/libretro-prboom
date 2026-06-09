/* u_ztextures.c: ZDoom TEXTURES lump parser.  See u_ztextures.h. */

#include <stdlib.h>
#include <string.h>

#include "doomstat.h"
#include "doomtype.h"
#include "lprintf.h"
#include "w_wad.h"
#include "z_zone.h"
#include "u_scanner.h"
#include "u_ztextures.h"

#define ZT_MAXPATCH 256          /* max composite patches per TEXTURES def */

ztexture_t *ztextures;
int num_ztextures;
static int cap_ztextures;
static dbool zt_loaded;

static void zt_copy_name(char *dst, const char *src)
{
  size_t n = strlen(src);
  if (n > 8)
    n = 8;
  memset(dst, 0, 9);
  memcpy(dst, src, n);
}

static ztexture_t *zt_add(void)
{
  if (num_ztextures == cap_ztextures)
  {
    int nc = cap_ztextures ? cap_ztextures * 2 : 64;
    ztexture_t *nt = Z_Malloc(nc * sizeof(*nt), PU_STATIC, 0);
    if (ztextures)
      memcpy(nt, ztextures, num_ztextures * sizeof(*nt));
    ztextures = nt;
    cap_ztextures = nc;
  }
  return &ztextures[num_ztextures];
}

/* skip a brace-balanced block whose '{' has just been consumed */
static void zt_skip_block(u_scanner_t *s)
{
  int depth = 1;
  while (depth > 0 && U_GetNextToken(s, TRUE))
  {
    if (s->token == '{')
      depth++;
    else if (s->token == '}')
      depth--;
  }
}

static dbool zt_number(u_scanner_t *s, double *out)
{
  if (!U_GetNextToken(s, TRUE))
    return false;
  if (s->token == '-')
  {
    if (!U_GetNextToken(s, TRUE))
      return false;
    if (s->token == TK_IntConst)
      *out = -(double)s->number;
    else if (s->token == TK_FloatConst)
      *out = -s->decimal;
    else
      return false;
    return true;
  }
  if (s->token == TK_IntConst)
    *out = (double)s->number;
  else if (s->token == TK_FloatConst)
    *out = s->decimal;
  else
    return false;
  return true;
}

static void zt_parse_lump(int lump)
{
  u_scanner_t s = U_ScanOpen(W_CacheLumpNum(lump), W_LumpLength(lump),
                             "TEXTURES");

  while (U_GetNextToken(&s, TRUE))
  {
    /* definition kinds; only Texture/WallTexture/Flat carry the shape
     * this engine consumes, but all parse identically */
    if (s.token == TK_Identifier &&
        (!strcasecmp(s.string, "Texture") ||
         !strcasecmp(s.string, "WallTexture") ||
         !strcasecmp(s.string, "Flat") ||
         !strcasecmp(s.string, "Sprite") ||
         !strcasecmp(s.string, "Graphic")))
    {
      ztexture_t def;
      double w = 0, h = 0;
      int patches = 0;
      dbool bad = false;
      ztpatch_t pl[ZT_MAXPATCH];   /* collected composite patches */

      memset(&def, 0, sizeof(def));
      def.xscale = 1.0;
      def.yscale = 1.0;

      /* NAME, W, H { */
      if (!U_GetNextToken(&s, TRUE) ||
          (s.token != TK_StringConst && s.token != TK_Identifier))
        continue;
      zt_copy_name(def.name, s.string);
      if (!U_CheckToken(&s, ','))
        continue;
      if (!zt_number(&s, &w) || !U_CheckToken(&s, ',') || !zt_number(&s, &h))
        continue;
      def.width = (int)w;
      def.height = (int)h;
      if (!U_CheckToken(&s, '{'))
        continue;

      while (U_GetNextToken(&s, TRUE) && s.token != '}')
      {
        if (s.token != TK_Identifier)
          continue;
        if (!strcasecmp(s.string, "XScale"))
        {
          double v;
          if (zt_number(&s, &v) && v > 0)
            def.xscale = v;
        }
        else if (!strcasecmp(s.string, "YScale"))
        {
          double v;
          if (zt_number(&s, &v) && v > 0)
            def.yscale = v;
        }
        else if (!strcasecmp(s.string, "Offset"))
        {
          /* consume "x, y"; offsets are unused for wall textures */
          double a, b;
          if (zt_number(&s, &a) && U_CheckToken(&s, ','))
            zt_number(&s, &b);
        }
        else if (!strcasecmp(s.string, "Patch") ||
                 !strcasecmp(s.string, "Sprite") ||
                 !strcasecmp(s.string, "Graphic"))
        {
          double px = 0, py = 0;
          char pname[9];
          if (!U_GetNextToken(&s, TRUE) ||
              (s.token != TK_StringConst && s.token != TK_Identifier))
          {
            bad = true;
            break;
          }
          zt_copy_name(pname, s.string);
          if (patches == 0)
            zt_copy_name(def.patch, s.string);
          patches++;
          if (U_CheckToken(&s, ','))
          {
            if (!zt_number(&s, &px) || !U_CheckToken(&s, ',') ||
                !zt_number(&s, &py))
            {
              bad = true;
              break;
            }
          }
          if (patches == 1)
          {
            def.patch_x = (int)px;
            def.patch_y = (int)py;
          }
          if (patches <= ZT_MAXPATCH)
          {
            zt_copy_name(pl[patches - 1].name, pname);
            pl[patches - 1].x = (int)px;
            pl[patches - 1].y = (int)py;
          }
          /* per-patch property sub-block (translations etc.): skip */
          if (U_CheckToken(&s, '{'))
            zt_skip_block(&s);
        }
        /* NullTexture, NoDecals, WorldPanning, Optional: bare keywords */
      }

      if (bad || def.width <= 0 || def.height <= 0 ||
          def.width > 4096 || def.height > 4096 || !def.patch[0])
        continue;
      def.patchcount = (patches > ZT_MAXPATCH) ? ZT_MAXPATCH : patches;
      if (def.patchcount > 0)
      {
        def.plist = Z_Malloc(def.patchcount * sizeof(ztpatch_t), PU_STATIC, 0);
        memcpy(def.plist, pl, def.patchcount * sizeof(ztpatch_t));
      }
      if (patches > ZT_MAXPATCH)
        lprintf(LO_WARN, "U_ZTexturesLoad: %.8s uses %d patches; capped at "
                "%d\n", def.name, patches, ZT_MAXPATCH);
      *zt_add() = def;
      num_ztextures++;
    }
  }
  U_ScanClose(&s);
  W_UnlockLumpNum(lump);
}

void U_ZTexturesLoad(void)
{
  int lump = -1;

  if (zt_loaded)
    return;
  zt_loaded = true;
  num_ztextures = 0;

  while ((lump = W_FindNumFromName("TEXTURES", lump)) >= 0)
    zt_parse_lump(lump);
  if (num_ztextures)
    lprintf(LO_INFO, "U_ZTexturesLoad: %d texture definitions\n",
            num_ztextures);
}

const ztexture_t *U_ZTexturesFind(const char *name)
{
  int i;
  for (i = 0; i < num_ztextures; i++)
    if (!strncasecmp(ztextures[i].name, name, 8))
      return &ztextures[i];
  return NULL;
}

dbool U_ZTexturesTargetSize(const char *lumpname, int *w, int *h)
{
  int i;
  for (i = 0; i < num_ztextures; i++)
  {
    const ztexture_t *t = &ztextures[i];
    if (strncasecmp(t->patch, lumpname, 8) ||
        strncasecmp(t->name, lumpname, 8) ||
        t->patch_x || t->patch_y)
      continue;
    if (t->xscale == 1.0 && t->yscale == 1.0)
      return false;                 /* native already correct */
    *w = (int)(t->width / t->xscale + 0.5);
    *h = (int)(t->height / t->yscale + 0.5);
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
    return true;
  }
  return false;
}
