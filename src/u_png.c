/* u_png.c: PNG decoding for ZDoom pk3 assets.  See u_png.h.
 *
 * Decoding is RPNG's job (libretro-common); this file owns the Doom
 * side: palette mapping, patch/flat synthesis, and the lump
 * materialization pass.
 *
 * RPNG with supports_rgba=true emits 0xAABBGGRR words (R in the low
 * byte).  Alpha is cut at 50%: >= 128 opaque, below transparent.
 *
 * Tall patches: column posts carry byte topdeltas, so rows past 254
 * are unreachable in the vanilla encoding.  The established DeePsea
 * convention -- which this engine's r_patch.c readers already decode --
 * treats a post whose topdelta is <= the accumulated top as relative:
 *   top = (topdelta <= top) ? top + topdelta : topdelta
 * The writer below emits zero-length stepping posts to bridge gaps the
 * encoding cannot express directly.
 */

#include <stdlib.h>
#include <string.h>

#include "doomstat.h"
#include "doomtype.h"
#include "lprintf.h"
#include "w_wad.h"
#include "z_zone.h"
#include "u_decaldef.h"
#include "u_png.h"
#include "u_ztextures.h"

#include <formats/rpng.h>
#include <formats/rjpeg.h>
#include <formats/image.h>

dbool U_PNGIsPNG(const unsigned char *d, int len)
{
  static const unsigned char sig[8] =
    { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
  return d && len >= 8 && !memcmp(d, sig, 8);
}

/* ---- palette mapping ----------------------------------------------------- */

static byte zpng_lut[32768];      /* RGB555 -> nearest PLAYPAL index */
static dbool zpng_lut_built;

static void zpng_build_lut(void)
{
  const byte *pal = W_CacheLumpName("PLAYPAL");
  int c;

  for (c = 0; c < 32768; c++)
  {
    int r = ((c >> 10) & 31) << 3;
    int g = ((c >> 5) & 31) << 3;
    int b = (c & 31) << 3;
    int best = 0, bestdist = 0x7FFFFFFF, i;
    for (i = 0; i < 256; i++)
    {
      int dr = r - pal[i * 3];
      int dg = g - pal[i * 3 + 1];
      int db = b - pal[i * 3 + 2];
      int dist = dr * dr + dg * dg + db * db;
      if (dist < bestdist)
      {
        bestdist = dist;
        best = i;
        if (!dist)
          break;
      }
    }
    zpng_lut[c] = (byte)best;
  }
  W_UnlockLumpName("PLAYPAL");
  zpng_lut_built = true;
}

/* When set, treat a near-black texel as transparent in addition to the
 * alpha test.  ZDoom decal graphics are grayscale coverage masks (white
 * splat on a black field, no alpha channel), so without this the black
 * field materialises as an opaque rectangle around the mark.  Only the
 * decal patch builder sets this; ordinary graphics keep black opaque. */
static int zpng_luma_cut;

/* map one 0xAABBGGRR pixel; returns -1 for transparent */
static int zpng_map(unsigned px)
{
  int r, g, b;
  if ((px >> 24) < 128)
    return -1;
  r = px & 0xFF;
  g = (px >> 8) & 0xFF;
  b = (px >> 16) & 0xFF;
  if (zpng_luma_cut && (r * 77 + g * 150 + b * 29) < (16 << 8))
    return -1;                      /* luminance < ~16: transparent       */
  return zpng_lut[((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)];
}

/* ---- decode -------------------------------------------------------------- */

/* grAb: nonstandard chunk carrying sprite offsets (two BE int32s) */
static void zpng_grab(const unsigned char *d, int len,
                      int *leftoffset, int *topoffset)
{
  int p = 8;
  *leftoffset = 0;
  *topoffset = 0;
  while (p + 12 <= len)
  {
    int clen = (d[p] << 24) | (d[p+1] << 16) | (d[p+2] << 8) | d[p+3];
    if (clen < 0 || p + 12 + clen > len)
      return;
    if (!memcmp(d + p + 4, "grAb", 4) && clen >= 8)
    {
      *leftoffset = (int)((d[p+8] << 24) | (d[p+9] << 16) |
                          (d[p+10] << 8) | d[p+11]);
      *topoffset = (int)((d[p+12] << 24) | (d[p+13] << 16) |
                         (d[p+14] << 8) | d[p+15]);
      return;
    }
    if (!memcmp(d + p + 4, "IDAT", 4))
      return;                       /* offsets precede image data */
    p += 12 + clen;
  }
}

/* count of conversions resampled to a TEXTURES-declared world size,
 * reported by U_PNGMaterializeLumps */
static int zpng_rescaled;

static unsigned *zpng_decode(const unsigned char *d, int len,
                             unsigned *w, unsigned *h)
{
  rpng_t *rpng = rpng_alloc();
  unsigned *out = NULL;
  int r;

  if (!rpng)
    return NULL;
  if (!rpng_set_buf_ptr(rpng, (void *)d, (size_t)len) ||
      !rpng_start(rpng))
  {
    rpng_free(rpng);
    return NULL;
  }
  while (rpng_iterate_image(rpng))
    ;
  if (!rpng_is_valid(rpng))
  {
    rpng_free(rpng);
    return NULL;
  }
  do
  {
    r = rpng_process_image(rpng, (void **)&out, (size_t)len, w, h, true);
  } while (r == IMAGE_PROCESS_NEXT);
  rpng_free(rpng);
  if (r != IMAGE_PROCESS_END || !out || !*w || !*h ||
      *w > 4096 || *h > 4096)
  {
    (free)(out);
    return NULL;
  }

  /* Allocator boundary: rpng compiles without the z_zone malloc macros
   * (raw libc -- memalign on GEKKO), so its buffer must never meet the
   * zone free.  Copy it into zone memory here, the single crossing
   * point, and release the original with the real libc free --
   * parenthesized to suppress the macro.  Everything downstream owns a
   * zone buffer and frees it normally. */
  {
    unsigned *zoned = malloc((size_t)*w * (size_t)*h * 4);
    if (zoned)
      memcpy(zoned, out, (size_t)*w * (size_t)*h * 4);
    (free)(out);
    return zoned;
  }
}

/* ----------------------------------------------------------------------------
 * Full-colour cache for HUD/portrait art.
 *
 * Most pk3 PNGs are materialized into PLAYPAL-indexed Doom patches so the
 * column renderer can draw them.  That round-trips full-colour artwork through
 * a 256-entry palette, which is fine for pixel-art sprites but visibly bands a
 * smooth-shaded portrait and throws away the per-texel alpha (it survives only
 * as a 1-bit cut in the patch posts).  The dialogue overlay wants the original
 * colour and a real alpha channel, so for the global-namespace art that feeds
 * it we record where each candidate image's *original* PNG bytes live (the
 * pk3 buffer the lump pointed at before materialization repointed it at a
 * patch) and decode them to a native 0xAABBGGRR image lazily, the first time
 * the overlay actually draws that lump.  The pk3 data stays mapped, so this
 * holds no image memory until a portrait is shown, and only the handful a
 * conversation uses are ever expanded. */
typedef struct
{
  int             lump;
  int             w, h;
  wadfile_info_t *src_wad; /* original lump source, snapshotted pre-replace   */
  int             src_pos;
  int             src_len;
  unsigned       *argb;    /* decoded 0xAABBGGRR, NULL until first draw        */
  dbool           tried;   /* decode attempted (avoid retrying a bad image)    */
} u_argb_entry_t;

static u_argb_entry_t *u_argb_cache;
static int             u_argb_count;
static int             u_argb_cap;

/* Record where a global-namespace image's original PNG bytes live, for later
 * lazy decode.  Called at materialization, before the lump is repointed. */
static void u_argb_keep_src(int lump)
{
  int i;
  for (i = 0; i < u_argb_count; i++)
    if (u_argb_cache[i].lump == lump)
      return;
  if (u_argb_count == u_argb_cap)
  {
    int ncap = u_argb_cap ? u_argb_cap * 2 : 32;
    u_argb_entry_t *nc = realloc(u_argb_cache, ncap * sizeof(*nc));
    if (!nc)
      return;
    u_argb_cache = nc;
    u_argb_cap   = ncap;
  }
  memset(&u_argb_cache[u_argb_count], 0, sizeof(u_argb_cache[u_argb_count]));
  u_argb_cache[u_argb_count].lump    = lump;
  u_argb_cache[u_argb_count].src_wad = lumpinfo[lump].wadfile;
  u_argb_cache[u_argb_count].src_pos = lumpinfo[lump].position;
  u_argb_cache[u_argb_count].src_len = W_LumpLength(lump);
  u_argb_count++;
}

const unsigned *U_PNGCacheRGBA(int lump, int *w, int *h)
{
  int i;
  for (i = 0; i < u_argb_count; i++)
    if (u_argb_cache[i].lump == lump)
    {
      u_argb_entry_t *e = &u_argb_cache[i];
      if (!e->argb && !e->tried)
      {
        e->tried = true;
        if (e->src_wad && e->src_len > 8)
        {
          unsigned char *png = malloc((size_t)e->src_len);
          if (png)
          {
            unsigned aw = 0, ah = 0;
            /* read the original bytes straight from the still-mapped source */
            if (e->src_wad->embedded_data)
              memcpy(png, &e->src_wad->embedded_data[e->src_pos],
                     (size_t)e->src_len);
#ifndef MEMORY_LOW
            else
              memcpy(png, &e->src_wad->data[e->src_pos], (size_t)e->src_len);
#endif
            if (U_PNGIsPNG(png, e->src_len))
              e->argb = zpng_decode(png, e->src_len, &aw, &ah);
            free(png);
            if (e->argb) { e->w = (int)aw; e->h = (int)ah; }
          }
        }
      }
      if (!e->argb)
        return NULL;
      if (w) *w = e->w;
      if (h) *h = e->h;
      return e->argb;
    }
  return NULL;
}

/* JPEG sibling of zpng_decode: rjpeg (libretro-common) emits the same
 * 0xAABBGGRR words with supports_rgba=true, so the patch/flat synthesis
 * below is shared.  Used for the JPEG skybox/texture members ZDoom packs
 * ship (e.g. ZDCMP2's SKYB_*.jpg). */
static unsigned *zjpeg_decode(const unsigned char *d, int len,
                              unsigned *w, unsigned *h)
{
  rjpeg_t *rjpeg = rjpeg_alloc();
  unsigned *out = NULL;
  int r;

  if (!rjpeg)
    return NULL;
  if (!rjpeg_set_buf_ptr(rjpeg, (void *)d, (size_t)len) ||
      !rjpeg_start(rjpeg))
  {
    rjpeg_free(rjpeg);
    return NULL;
  }
  while (rjpeg_iterate_image(rjpeg))
    ;
  if (!rjpeg_is_valid(rjpeg))
  {
    rjpeg_free(rjpeg);
    return NULL;
  }
  do
  {
    r = rjpeg_process_image(rjpeg, (void **)&out, (size_t)len, w, h, true);
  } while (r == IMAGE_PROCESS_NEXT);
  rjpeg_free(rjpeg);
  if (r != IMAGE_PROCESS_END || !out || !*w || !*h ||
      *w > 4096 || *h > 4096)
  {
    (free)(out);
    return NULL;
  }

  /* Same allocator-boundary copy as zpng_decode: rjpeg is vendored verbatim
   * and allocates with raw libc, so cross into zone memory here. */
  {
    unsigned *zoned = malloc((size_t)*w * (size_t)*h * 4);
    if (zoned)
      memcpy(zoned, out, (size_t)*w * (size_t)*h * 4);
    (free)(out);
    return zoned;
  }
}

/* Decode a PNG or JPEG lump to 0xAABBGGRR pixels, dispatching on signature. */
static unsigned *zimg_decode(const unsigned char *d, int len,
                             unsigned *w, unsigned *h)
{
  if (len >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF)
    return zjpeg_decode(d, len, w, h);
  return zpng_decode(d, len, w, h);
}

/* ---- patch synthesis ----------------------------------------------------- */

void *U_PNGToPatchSized(const unsigned char *d, int len,
                        int tw, int th, int *out_size)
{
  unsigned w, h;
  unsigned *argb;
  int leftoffset, topoffset;
  unsigned char *patch;
  int *columnofs;
  unsigned char *p;
  size_t cap;
  unsigned x;

  if (!zpng_lut_built)
    zpng_build_lut();
  argb = zimg_decode(d, len, &w, &h);
  if (!argb)
    return NULL;
  zpng_grab(d, len, &leftoffset, &topoffset);

  /* TEXTURES-lump scale: resample to the world size before patch
   * synthesis (nearest), since the renderer has no per-texture scale.
   * grAb offsets stay unscaled; wall textures do not consume them. */
  if (tw > 0 && th > 0 && (tw != (int)w || th != (int)h))
  {
    unsigned *rs = malloc((size_t)tw * (size_t)th * 4);
    int rx, ry;
    if (!rs)
    {
      free(argb);
      return NULL;
    }
    for (ry = 0; ry < th; ry++)
      for (rx = 0; rx < tw; rx++)
        rs[(size_t)ry * tw + rx] =
          argb[((size_t)ry * h / th) * w + ((size_t)rx * w / tw)];
    free(argb);
    argb = rs;
    w = (unsigned)tw;
    h = (unsigned)th;
  }

  /* worst case per column: every other pixel opaque -> h/2 posts of 1
   * pixel (4 + 1 byte each... pad generously) plus stepping posts */
  cap = 8 + 4 * (size_t)w + (size_t)w * (3 * (size_t)h + 96) + 64;
  patch = malloc(cap);
  if (!patch)
  {
    free(argb);
    return NULL;
  }

  /* patch header: width, height, leftoffset, topoffset (LE shorts) */
  patch[0] = (unsigned char)(w & 0xFF);
  patch[1] = (unsigned char)(w >> 8);
  patch[2] = (unsigned char)(h & 0xFF);
  patch[3] = (unsigned char)(h >> 8);
  patch[4] = (unsigned char)(leftoffset & 0xFF);
  patch[5] = (unsigned char)((leftoffset >> 8) & 0xFF);
  patch[6] = (unsigned char)(topoffset & 0xFF);
  patch[7] = (unsigned char)((topoffset >> 8) & 0xFF);

  columnofs = (int *)(patch + 8);
  p = patch + 8 + 4 * w;

  for (x = 0; x < w; x++)
  {
    int rtop = -1;                  /* reader's accumulated top */
    unsigned y = 0;

    columnofs[x] = (int)(p - patch);
    while (y < h)
    {
      unsigned start, runlen, k;
      int td;

      /* next opaque run, capped at 254 rows per post */
      while (y < h && zpng_map(argb[y * w + x]) < 0)
        y++;
      if (y >= h)
        break;
      start = y;
      while (y < h && y - start < 254 && zpng_map(argb[y * w + x]) >= 0)
        y++;
      runlen = y - start;

      /* step until 'start' is encodable: absolute (td = start, needs
       * start > rtop and start <= 254) or relative (td = start - rtop,
       * needs 0 <= td <= rtop).  Zero-length posts advance the reader by
       * 254 each: absolute jump to 254 while rtop < 254, relative +254
       * after, so this terminates and never overshoots. */
      while (!(((int)start > rtop && start <= 254) ||
               (rtop >= 0 && (int)start >= rtop &&
                (int)start - rtop <= rtop && (int)start - rtop <= 254)))
      {
        rtop = (254 > rtop) ? 254 : rtop + 254;
        p[0] = 254; p[1] = 0; p[2] = 0; p[3] = 0;
        p += 4;
      }
      td = ((int)start > rtop && start <= 254) ?
           (int)start : (int)start - rtop;

      p[0] = (unsigned char)td;
      p[1] = (unsigned char)runlen;
      p[2] = 0;                     /* pad before pixels */
      for (k = 0; k < runlen; k++)
        p[3 + k] = (unsigned char)zpng_map(argb[(start + k) * w + x]);
      p[3 + runlen] = 0;            /* pad after pixels */
      p += 4 + runlen;
      rtop = (int)start;
    }
    *p++ = 0xFF;                    /* end of column */
  }

  free(argb);
  *out_size = (int)(p - patch);
  return patch;
}

/* ---- flat synthesis ------------------------------------------------------ */

void *U_PNGToFlat(const unsigned char *d, int len, int *out_size)
{
  unsigned w, h;
  unsigned *argb;
  unsigned char *flat;
  int x, y;

  if (!zpng_lut_built)
    zpng_build_lut();
  argb = zimg_decode(d, len, &w, &h);
  if (!argb)
    return NULL;
  flat = malloc(64 * 64);
  if (!flat)
  {
    free(argb);
    return NULL;
  }
  for (y = 0; y < 64; y++)
    for (x = 0; x < 64; x++)
    {
      unsigned sx = (unsigned)x * w / 64;
      unsigned sy = (unsigned)y * h / 64;
      int idx = zpng_map(argb[sy * w + sx]);
      flat[y * 64 + x] = (unsigned char)(idx < 0 ? 0 : idx);
    }
  free(argb);
  *out_size = 64 * 64;
  return flat;
}

/* ---- lump materialization ------------------------------------------------ */

void *U_PNGToPatch(const unsigned char *d, int len, int *out_size)
{
  return U_PNGToPatchSized(d, len, 0, 0, out_size);
}

/* As U_PNGToPatch, but treats a near-black texel as transparent: ZDoom
 * decal graphics are grayscale coverage masks with no alpha, so the black
 * field must be cut or it materialises as an opaque box around the mark. */
void *U_PNGToPatchDecal(const unsigned char *d, int len, int *out_size)
{
  void *patch;
  zpng_luma_cut = 1;
  patch = U_PNGToPatchSized(d, len, 0, 0, out_size);
  zpng_luma_cut = 0;
  return patch;
}

/* one marker pair; inner wads contribute their own SS/FF groups, so
 * the caller walks every instance */
/* Convert a single lump in place if it carries PNG or JPEG data; lumps that
 * are already patch/flat data (or too short) are left untouched, so this is
 * safe to call on a lump more than once. */
static void zpng_convert_one(int i, dbool as_flat, dbool scaled, int *count)
{
  const unsigned char *raw;
  int rawlen = W_LumpLength(i);
  void *conv;
  int convlen;

  if (rawlen < 8)
    return;
  raw = W_CacheLumpNum(i);
  if (!U_PNGIsPNG(raw, rawlen) &&
      !(rawlen >= 3 && raw[0] == 0xFF && raw[1] == 0xD8 && raw[2] == 0xFF))
  {
    W_UnlockLumpNum(i);
    return;
  }
  if (as_flat)
    conv = U_PNGToFlat(raw, rawlen, &convlen);
  else if (!scaled && U_IsDecalPic(lumpinfo[i].name))
    /* a grayscale decal mask: cut the black field so only the mark draws */
    conv = U_PNGToPatchDecal(raw, rawlen, &convlen);
  else
  {
    int tw = 0, th = 0;
    if (scaled && U_ZTexturesTargetSize(lumpinfo[i].name, &tw, &th))
      zpng_rescaled++;
    conv = U_PNGToPatchSized(raw, rawlen, tw, th, &convlen);
  }
  W_UnlockLumpNum(i);
  if (!conv)
  {
    lprintf(LO_WARN, "U_PNGMaterializeLumps: %.8s failed to decode\n",
            lumpinfo[i].name);
    return;
  }
  /* Global-namespace art (HUD frames, dialogue portraits) may also be wanted
   * in native colour for the full-colour overlay blit.  Record where its
   * original PNG bytes live now, before the lump is repointed; the decode
   * happens lazily on first draw so unused art costs nothing. */
  if (!as_flat && lumpinfo[i].li_namespace == ns_global)
    u_argb_keep_src(i);
  W_ReplaceLumpData(i, conv, convlen);
  (*count)++;
}

static void zpng_convert_one_range(int s, int e, dbool as_flat,
                                   dbool scaled, int *count)
{
  int i;

  for (i = s + 1; i < e; i++)
    zpng_convert_one(i, as_flat, scaled, count);
}

/* Convert every PNG/JPEG lump that the archive assigned to a namespace by
 * folder (pk3) rather than by SS_/FF_/TX_ markers (wad).  ZDoom packs keep
 * their sprites under sprites/... with no marker lumps, so the marker scan
 * above misses them and they reach the patch renderer as raw PNG -- the
 * thing that draws them then parses PNG bytes as a Doom patch and crashes.
 * Already-converted lumps are skipped (no longer PNG), so running this after
 * the marker scan double-covers nothing. */
static void zpng_convert_namespace(lumpinfo_namespace_t ns, dbool as_flat,
                                   dbool scaled, int *count)
{
  int i;
  for (i = 0; i < numlumps; i++)
    if (lumpinfo[i].li_namespace == ns)
      zpng_convert_one(i, as_flat, scaled, count);
}

static void zpng_convert_range(const char *smark, const char *emark,
                               dbool as_flat, dbool scaled, int *count)
{
  /* positional scan: W_FindNumFromName iterates hash chains (latest
   * first), which cannot pair start/end markers.  Inner wads bring
   * their own marker groups, so every [smark, emark] pair counts. */
  int i, s = -1;

  for (i = 0; i < numlumps; i++)
  {
    if (!strncasecmp(lumpinfo[i].name, smark, 8))
      s = i;
    else if (s >= 0 && !strncasecmp(lumpinfo[i].name, emark, 8))
    {
      zpng_convert_one_range(s, i, as_flat, scaled, count);
      s = -1;
    }
  }
}

static int zpng_rescaled;

void U_PNGMaterializeLumps(void)
{
  int n = 0;

  zpng_rescaled = 0;
  zpng_convert_range("SS_START", "SS_END", false, false, &n);
  zpng_convert_range("FF_START", "FF_END", true, false, &n);
  zpng_convert_range("TX_START", "TX_END", false, true, &n);
  /* pk3 namespace-assigned members (subfolders, no marker lumps) */
  zpng_convert_namespace(ns_sprites, false, false, &n);
  zpng_convert_namespace(ns_flats, true, false, &n);
  zpng_convert_namespace(ns_zdoom_tx, false, true, &n);
  /* Global-namespace PNG/JPEG lumps -- ZDoom packs put menu/HUD/status-bar
   * art and other patches under graphics/ (and sometimes at the archive
   * root), which the pk3 translator now routes to ns_global rather than
   * the deferred quarantine.  Convert them to Doom patches here, before
   * any patch-cache code reads them, so the patch renderer never sees raw
   * PNG bytes.  zpng_convert_one sniffs each lump and skips non-PNG/JPEG
   * data, so walking the large global namespace only touches real images. */
  zpng_convert_namespace(ns_global, false, false, &n);
  if (n)
    lprintf(LO_INFO, "U_PNGMaterializeLumps: %d PNG/JPEG lumps converted "
            "(%d rescaled per TEXTURES)\n", n, zpng_rescaled);
}
