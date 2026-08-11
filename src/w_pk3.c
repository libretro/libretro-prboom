/* w_pk3.c: PK3/ZIP archive to in-memory WAD translation.
 *
 * The archive is walked once with a minimal in-memory ZIP reader
 * (rinflate + encoding_crc32 below) and re-emitted as a single PWAD
 * image:
 *
 *   - root-level files become plain global lumps; the lump name is the
 *     file name up to the first '.', uppercased, truncated to 8 chars
 *     (ZDoom's rule: TEXTURES.txt -> TEXTURES, MAPINFO -> MAPINFO).
 *   - root-level .wad members are expanded inline: their own directories
 *     are appended lump for lump, preserving internal order, so map
 *     directories (MAPxx / TEXTMAP / ZNODES / ... / ENDMAP) and marker
 *     pairs survive intact.  This mirrors ZDoom, which loads root-level
 *     wads inside an archive as part of it.
 *   - sprites/ and flats/ members are wrapped in SS_START/SS_END and
 *     FF_START/FF_END marker pairs so W_Init's marker coalescing files
 *     them into ns_sprites / ns_flats exactly like a conventional PWAD.
 *   - members in modern formats this engine cannot consume yet (PNG,
 *     Ogg, RIFF/WAV, FLAC) are quarantined between PD_START/PD_END
 *     markers, which W_Init coalesces into ns_pk3_deferred: the data is
 *     present for future consumers (PNG patch decoding, sample loaders)
 *     but invisible to ns_global lookups, so a PNG texture sharing a
 *     name with an IWAD patch can never reach the patch renderer.
 *   - everything else (graphics/, sounds/, music/, unknown folders)
 *     lands in the global namespace when it sniffs as a native format
 *     (Doom patch, DMX sound, MUS/MIDI/MP3, plain text).
 *
 * The translator is only built into standard-memory builds: the image
 * lives fully in RAM, replacing the precached archive bytes. */

#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "m_swap.h"
#include "lprintf.h"
#include "w_wad.h"
#include "w_pk3.h"

#include <encodings/deflate.h>
#include <encodings/crc32.h>

/* ---- minimal in-memory ZIP reader ---------------------------------------
 *
 * Replaces miniz's mz_zip reader.  The translator only ever reads a
 * fully-resident archive, member by member, so all that is needed is a
 * central-directory walk and a per-member extract: stored members are a
 * bounds-checked copy, deflated members inflate through libretro-common's
 * clean-room rinflate (raw DEFLATE window), and every extracted member is
 * verified against the directory's CRC-32 with encoding_crc32 -- whose
 * PCLMUL/CRC32-instruction paths make the mandatory integrity pass far
 * cheaper than miniz's bytewise table walk.
 *
 * Zip64 archives (any 0xFFFF/0xFFFFFFFF marker in the fields read here)
 * are not supported and are rejected at open, as under miniz. */

typedef struct
{
  const char    *name;     /* into pk3_zip_t.names, NUL-terminated  */
  unsigned       lho;      /* local file header offset              */
  unsigned       csize;    /* compressed size                       */
  unsigned       usize;    /* uncompressed size                     */
  unsigned       crc;      /* CRC-32 of the uncompressed data       */
  unsigned short method;   /* 0 = store, 8 = deflate                */
  unsigned char  is_dir;
} pk3_zent_t;

typedef struct
{
  const unsigned char *zip;
  int                  zip_len;
  pk3_zent_t          *ent;
  int                  n;
  char                *names;    /* all entry names, NUL-separated  */
  void                *inflate;  /* reusable rinflate stream        */
} pk3_zip_t;

static unsigned pk3_rd16(const unsigned char *p)
{
  return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static unsigned pk3_rd32(const unsigned char *p)
{
  return (unsigned)p[0] | ((unsigned)p[1] << 8) |
         ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static void pk3_zip_close(pk3_zip_t *z)
{
  if (z->inflate)
    rinflate_free(z->inflate);
  free(z->ent);
  free(z->names);
  memset(z, 0, sizeof(*z));
}

/* Parse the end-of-central-directory record and the central directory.
 * Returns 1 on success. */
static int pk3_zip_open(pk3_zip_t *z, const unsigned char *zip, int zip_len)
{
  int      eocd = -1, i, scan_lo;
  unsigned n, cd_size, cd_off, pos;
  size_t   names_len;
  char    *np;

  memset(z, 0, sizeof(*z));
  z->zip     = zip;
  z->zip_len = zip_len;

  /* EOCD: "PK\5\6" + 18 bytes fixed + up to 65535 bytes of comment,
   * scanned backwards from the end. */
  scan_lo = zip_len - 22 - 65535;
  if (scan_lo < 0)
    scan_lo = 0;
  for (i = zip_len - 22; i >= scan_lo; i--)
    if (zip[i] == 'P' && zip[i+1] == 'K' && zip[i+2] == 5 && zip[i+3] == 6)
    {
      eocd = i;
      break;
    }
  if (eocd < 0)
    return 0;

  /* Reject multi-disk archives; treat 0xFFFF/0xFFFFFFFF as zip64. */
  if (pk3_rd16(zip + eocd + 4) != 0 || pk3_rd16(zip + eocd + 6) != 0)
    return 0;
  n       = pk3_rd16(zip + eocd + 10);
  cd_size = pk3_rd32(zip + eocd + 12);
  cd_off  = pk3_rd32(zip + eocd + 16);
  if (n == 0xFFFFu || cd_size == 0xFFFFFFFFu || cd_off == 0xFFFFFFFFu)
    return 0;                                          /* zip64 */
  if ((int64_t)cd_off + cd_size > (int64_t)zip_len)
    return 0;

  z->ent = calloc(n ? n : 1, sizeof(pk3_zent_t));
  /* names: total name bytes cannot exceed the directory size */
  z->names = malloc((size_t)cd_size + n + 1);
  if (!z->ent || !z->names)
  {
    pk3_zip_close(z);
    return 0;
  }

  pos       = cd_off;
  np        = z->names;
  names_len = 0;
  (void)names_len;
  for (i = 0; i < (int)n; i++)
  {
    pk3_zent_t *e = &z->ent[i];
    unsigned    nlen, elen, clen;

    if ((int64_t)pos + 46 > (int64_t)zip_len ||
        pk3_rd32(zip + pos) != 0x02014b50u)
    {
      pk3_zip_close(z);
      return 0;
    }
    e->method = (unsigned short)pk3_rd16(zip + pos + 10);
    e->crc    = pk3_rd32(zip + pos + 16);
    e->csize  = pk3_rd32(zip + pos + 20);
    e->usize  = pk3_rd32(zip + pos + 24);
    nlen      = pk3_rd16(zip + pos + 28);
    elen      = pk3_rd16(zip + pos + 30);
    clen      = pk3_rd16(zip + pos + 32);
    e->lho    = pk3_rd32(zip + pos + 42);
    if (e->csize == 0xFFFFFFFFu || e->usize == 0xFFFFFFFFu ||
        e->lho == 0xFFFFFFFFu)
    {
      pk3_zip_close(z);
      return 0;                                        /* zip64 */
    }
    if ((int64_t)pos + 46 + nlen + elen + clen > (int64_t)zip_len)
    {
      pk3_zip_close(z);
      return 0;
    }
    memcpy(np, zip + pos + 46, nlen);
    np[nlen] = 0;
    e->name  = np;
    np      += nlen + 1;
    e->is_dir = (nlen > 0 && (np[-2] == '/' || np[-2] == '\\'));
    pos += 46 + nlen + elen + clen;
  }
  z->n = (int)n;
  return 1;
}

/* Resolve a member's data offset from its local file header.
 * Returns 1 and writes *doff on success. */
static int pk3_zip_locate(const pk3_zip_t *z, const pk3_zent_t *e,
                          unsigned *doff)
{
  const unsigned char *zip = z->zip;
  unsigned             nlen, elen, off;

  if ((int64_t)e->lho + 30 > (int64_t)z->zip_len ||
      pk3_rd32(zip + e->lho) != 0x04034b50u)
    return 0;
  nlen = pk3_rd16(zip + e->lho + 26);
  elen = pk3_rd16(zip + e->lho + 28);
  off  = e->lho + 30 + nlen + elen;
  if ((int64_t)off + e->csize > (int64_t)z->zip_len)
    return 0;
  if (e->method == 0 && e->csize != e->usize)          /* stored: sizes lie */
    return 0;
  if (e->method != 0 && e->method != 8)                /* unsupported */
    return 0;
  *doff = off;
  return 1;
}

/* Decompress a member's raw DEFLATE stream into dst, writing at most cap
 * bytes.  With cap == usize the whole stream must arrive (END seen and
 * exactly usize bytes written); with cap < usize this is a prefix decode
 * that succeeds as soon as cap bytes exist.  Returns 1 on success. */
static int pk3_zip_inflate(pk3_zip_t *z, const unsigned char *in,
                           size_t in_len, unsigned char *dst,
                           unsigned cap, unsigned usize)
{
  size_t wrote_total = 0;
  int    r;

  if (!z->inflate)
    z->inflate = rinflate_new(-15);                    /* raw DEFLATE */
  else
    rinflate_reset(z->inflate, -15);
  if (!z->inflate)
    return 0;
  rinflate_set_out(z->inflate, dst, cap);
  for (;;)
  {
    size_t rd = 0, wr = 0;
    rinflate_set_in(z->inflate, in, in_len);
    r            = rinflate_process(z->inflate, &rd, &wr);
    in          += rd;
    in_len      -= rd;
    wrote_total += wr;
    if (cap < usize && wrote_total >= cap)             /* prefix satisfied */
      return 1;
    if (r == RDEFLATE_PROCESS_END)
      break;
    if (r != RDEFLATE_PROCESS_NEXT || (rd == 0 && wr == 0))
      return 0;                       /* malformed, or truncated/overlong */
  }
  return wrote_total == cap;
}

/* Extract one member into dst (exactly usize bytes of room) and verify
 * the directory CRC over the result.  Returns 1 on success; dst contents
 * are undefined on failure. */
static int pk3_zip_extract_to(pk3_zip_t *z, const pk3_zent_t *e,
                              unsigned char *dst)
{
  unsigned doff;

  if (!pk3_zip_locate(z, e, &doff))
    return 0;
  if (e->method == 0)
  {
    if (e->usize)
      memcpy(dst, z->zip + doff, e->usize);
  }
  else if (!pk3_zip_inflate(z, z->zip + doff, e->csize,
                            dst, e->usize, e->usize))
    return 0;
  return encoding_crc32(0, dst, e->usize) == e->crc;
}

/* Extract one member to a fresh malloc'd buffer of exactly usize bytes
 * (usize 0 returns a 1-byte allocation so the pointer is non-NULL).
 * Returns NULL on any inconsistency. */
static unsigned char *pk3_zip_extract(pk3_zip_t *z, const pk3_zent_t *e)
{
  unsigned char *out = malloc(e->usize ? e->usize : 1);

  if (out && !pk3_zip_extract_to(z, e, out))
  {
    free(out);
    return NULL;
  }
  return out;
}

/* Decode just the first min(usize, cap) bytes of a member into dst for
 * format sniffing: stored members copy in place, deflated members run a
 * capped prefix inflate.  No CRC check -- the full extraction verifies.
 * Returns the byte count on success, -1 on any inconsistency. */
static int pk3_zip_sniff(pk3_zip_t *z, const pk3_zent_t *e,
                         unsigned char *dst, unsigned cap)
{
  unsigned doff, need = e->usize < cap ? e->usize : cap;

  if (!pk3_zip_locate(z, e, &doff))
    return -1;
  if (!need)
    return 0;
  if (e->method == 0)
    memcpy(dst, z->zip + doff, need);
  else if (!pk3_zip_inflate(z, z->zip + doff, e->csize,
                            dst, need, e->usize))
    return -1;
  return (int)need;
}

/* ---- full-path registry -------------------------------------------------
 *
 * The synthesized lump name is the basename truncated to eight characters,
 * so two archive members that share a basename in different folders collide
 * (decorate/monster/imp.txt and decorate/sex/imp.txt both become "IMP").
 * Lookups that need to follow a full path -- chiefly DECORATE #include, which
 * names "decorate/monster/imp.txt" exactly -- cannot disambiguate by lump name
 * alone.  Record every emitted lump's full archive path against the address
 * of its data inside the synthesized image; that address is stable (the image
 * becomes the wadfile's data buffer), so a path can later be matched to the
 * one global lump whose data lives there.  Paths are stored lowercased with
 * forward slashes for case- and separator-insensitive comparison. */
typedef struct
{
  char                 path[192];
  const unsigned char *data;     /* &image[filepos] once the image is built */
  int                  filepos;  /* offset of the data within the image     */
} pk3_pathent_t;

static pk3_pathent_t *pk3_paths;
static int            pk3_paths_len;
static int            pk3_paths_cap;
static int            pk3_paths_base;  /* first index for the current archive */

static void pk3_path_norm(char *out, size_t outsz, const char *path)
{
  size_t i;
  for (i = 0; path[i] && i + 1 < outsz; i++)
  {
    char c = path[i];
    if (c == '\\') c = '/';
    else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    out[i] = c;
  }
  out[i] = 0;
}

static void pk3_path_record(const char *path, int filepos)
{
  pk3_pathent_t *e;
  if (!path || !path[0])
    return;
  if (pk3_paths_len == pk3_paths_cap)
  {
    int ncap = pk3_paths_cap ? pk3_paths_cap * 2 : 256;
    pk3_pathent_t *np = realloc(pk3_paths, (size_t)ncap * sizeof(*np));
    if (!np)
      return;                       /* registry is best-effort */
    pk3_paths = np;
    pk3_paths_cap = ncap;
  }
  e = &pk3_paths[pk3_paths_len++];
  pk3_path_norm(e->path, sizeof(e->path), path);
  e->data    = NULL;
  e->filepos = filepos;
}

/* Once the image base is known, resolve each pending entry's data address. */
static void pk3_path_bind(const unsigned char *image)
{
  int i;
  for (i = pk3_paths_base; i < pk3_paths_len; i++)
    pk3_paths[i].data = image + pk3_paths[i].filepos;
  pk3_paths_base = pk3_paths_len;
}

/* Public: the global lump number whose data matches the given full archive
 * path, or -1.  Matches by exact normalized path first, then by trailing
 * path (so a leading "./" or namespace prefix on either side still hits). */
int W_PK3LumpForPath(const char *path)
{
  char want[192];
  int  i, lump;
  if (!path || !path[0] || !pk3_paths)
    return -1;
  pk3_path_norm(want, sizeof(want), path);
  for (i = 0; i < pk3_paths_len; i++)
  {
    const char *p = pk3_paths[i].path;
    size_t lp = strlen(p), lw = strlen(want);
    int hit = !strcmp(p, want);
    if (!hit && lp > lw && p[lp - lw - 1] == '/' && !strcmp(p + lp - lw, want))
      hit = 1;                      /* want is a trailing segment of p */
    if (!hit && lw > lp && want[lw - lp - 1] == '/' && !strcmp(want + lw - lp, p))
      hit = 1;                      /* p is a trailing segment of want */
    if (!hit)
      continue;
    /* find the global lump whose data address matches this entry */
    for (lump = 0; lump < numlumps; lump++)
    {
      const lumpinfo_t *li = &lumpinfo[lump];
      const unsigned char *base = NULL;
      if (li->wadfile)
      {
        if (li->wadfile->embedded_data)
          base = li->wadfile->embedded_data;
#ifndef MEMORY_LOW
        else
          base = li->wadfile->data;
#endif
      }
      if (base && base + li->position == pk3_paths[i].data)
        return lump;
    }
  }
  return -1;
}

dbool W_IsPK3(const unsigned char *data, int64_t length)
{
  return length >= 4 &&
         data[0] == 'P' && data[1] == 'K' &&
         data[2] == 0x03 && data[3] == 0x04;
}

/* ZDoom lump naming: basename, up to the first '.', uppercased, max 8. */
static void pk3_lump_name(char out[9], const char *path)
{
  const char *base = strrchr(path, '/');
  int i;

  base = base ? base + 1 : path;
  for (i = 0; i < 8 && base[i] && base[i] != '.'; i++)
    out[i] = (char)((base[i] >= 'a' && base[i] <= 'z')
                      ? base[i] - 'a' + 'A' : base[i]);
  out[i] = 0;
}

static dbool pk3_is_png(const unsigned char *d, int len)
{
  return len >= 4 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G';
}

/* Formats the engine cannot consume yet; these get quarantined. */
static dbool pk3_is_deferred_format(const unsigned char *d, int len)
{
  if (len >= 4)
  {
    if (d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G')
      return TRUE;                                      /* PNG  */
    if (!memcmp(d, "OggS", 4))
      return TRUE;                                      /* Ogg  */
    if (!memcmp(d, "RIFF", 4))
      return TRUE;                                      /* WAV  */
    if (!memcmp(d, "fLaC", 4))
      return TRUE;                                      /* FLAC */
  }
  return FALSE;
}

/* RIFF/WAVE sniff.  WAV used to be quarantined alongside PNG/Ogg/FLAC,
 * but the sfx loader (I_SndLoadSample) now decodes RIFF/WAVE lumps via
 * RWAV, so a WAV that lands in a sound-bearing (global) namespace must
 * stay visible to ns_global lookups instead of being deferred.  It is
 * still deferred inside the renderable namespaces (sprites/flats/
 * textures), where a waveform lump can never be a valid graphic. */
static dbool pk3_is_wav(const unsigned char *d, int len)
{
  return len >= 4 && !memcmp(d, "RIFF", 4);
}

/* Ogg sniff.  Like WAV, Ogg used to be quarantined everywhere, but the sfx
 * loader now decodes Ogg Vorbis lumps too, so an Ogg in a sound-bearing
 * (global) namespace must stay visible to ns_global lookups.  It is still
 * deferred inside the renderable namespaces, where it can never be a graphic. */
static dbool pk3_is_ogg(const unsigned char *d, int len)
{
  return len >= 4 && !memcmp(d, "OggS", 4);
}

/* True only for a byte buffer that is actually a WAD image: the IWAD/PWAD
 * magic AND a directory header (numlumps, infotableofs) that fits inside
 * the buffer.  The bare 4-byte magic is not enough -- ZDoom packs ship
 * text lumps that legitimately begin with the word "IWAD", e.g. a
 * gameinfo.txt whose first line is  IWAD = "DOOM2.WAD"  -- and routing
 * those to the inner-wad expander made the translator read ` = "D...` as
 * a lump count and drop the lump as a "corrupt directory".  Validating
 * the header here keeps such text lumps on the normal global path. */
static dbool pk3_is_wad_image(const unsigned char *d, int len)
{
  wadinfo_t header;
  int       numlumps, infotableofs;

  if (len < (int)sizeof(wadinfo_t))
    return FALSE;
  if (memcmp(d, "IWAD", 4) && memcmp(d, "PWAD", 4))
    return FALSE;
  memcpy(&header, d, sizeof(header));
  numlumps     = LONG(header.numlumps);
  infotableofs = LONG(header.infotableofs);
  if (numlumps < 0 || infotableofs < 0 ||
      (int64_t)infotableofs + (int64_t)numlumps * 16 > (int64_t)len)
    return FALSE;
  return TRUE;
}

/* ---- archive walk -------------------------------------------------------- */

/* Folder classification for one pass of pk3_emit_folder. */
#define PK3_PASS_ROOT     0   /* no '/': root files and inner wads  */
#define PK3_PASS_SPRITES  1
#define PK3_PASS_FLATS    2
#define PK3_PASS_TEXTURES 3   /* textures/: standalone wall textures */
#define PK3_PASS_GLOBAL   4   /* every other folder, native formats */
#define PK3_PASS_DEFERRED 5   /* modern formats from any folder     */

static int pk3_pass_of(const char *path, const unsigned char *d, int len)
{
  const char *slash = strchr(path, '/');

  if (!slash)
    return PK3_PASS_ROOT;
  /* A nested .wad member -- most commonly ZDoom's maps/<name>.wad -- is a
   * complete PWAD/IWAD image.  Route it to the inner-wad pass so its lumps
   * (the map marker, TEXTMAP, named ZNODES, BEHAVIOR, ...) are expanded
   * verbatim and in order, exactly like a root-level wad.  Without this the
   * member was emitted as one opaque lump named after the file, leaving the
   * map's TEXTMAP absent: UDMF detection failed and the binary loader read
   * the text map as binary records (garbage sidedefs, huge bogus allocs). */
  if (pk3_is_wad_image(d, len))
    return PK3_PASS_ROOT;
  /* ZScript is ZDoom's compiled scripting language, which this engine does
   * not run.  Its files live under zscript/ (with an entry zscript.txt at
   * the root) and must never become engine-consumable lumps: gzdoom.pk3
   * ships zscript/actors/doom/dehacked.zs, whose basename maps to the lump
   * name DEHACKED, and the in-wad Dehacked loader would then try to parse
   * 54 KB of class definitions as a Dehacked patch (a flood of "Unmatched
   * Block").  Quarantine the whole tree so it is present but invisible to
   * ns_global lookups. */
  if (!strncasecmp(path, "zscript/", 8) ||
      !strncasecmp(path, "zscript.", 8))
    return PK3_PASS_DEFERRED;
  /* Other GZDoom engine-internal trees that a software Boom engine cannot
   * consume, and whose files collide with magic lump names if exposed:
   *   - shaders/ and shaders_gles/ (and glstuff/) are GLSL/hardware-only;
   *     gzdoom.pk3 ships shaders/pp/colormap.fp, whose basename maps to the
   *     lump name COLORMAP -- the renderer then reads 504 bytes of shader
   *     source as the light/shading table and the whole frame is garbage.
   *   - filter/ holds GZDoom's per-game conditional assets
   *     (filter/game-<name>/...); this engine has no filter mechanism, so
   *     e.g. filter/game-heretic/animated.lmp must not surface as a global
   *     ANIMATED lump and apply the wrong animations.
   * Quarantine these the same way as zscript/: present but invisible to
   * ns_global lookups. */
  if (!strncasecmp(path, "shaders/", 8) ||
      !strncasecmp(path, "shaders_gles/", 13) ||
      !strncasecmp(path, "glstuff/", 8) ||
      !strncasecmp(path, "filter/", 7))
    return PK3_PASS_DEFERRED;
  /* PNG members of the renderable namespaces stay in their groups:
   * U_PNGMaterializeLumps converts them to patches/flats in place
   * before the renderer reads them.  Other modern formats (Ogg, WAV,
   * FLAC) are still quarantined from everywhere. */
  if (!strncasecmp(path, "sprites/", 8))
    return (!pk3_is_png(d, len) && pk3_is_deferred_format(d, len))
           ? PK3_PASS_DEFERRED : PK3_PASS_SPRITES;
  if (!strncasecmp(path, "flats/", 6))
    return (!pk3_is_png(d, len) && pk3_is_deferred_format(d, len))
           ? PK3_PASS_DEFERRED : PK3_PASS_FLATS;
  if (!strncasecmp(path, "textures/", 9))
    return (!pk3_is_png(d, len) && pk3_is_deferred_format(d, len))
           ? PK3_PASS_DEFERRED : PK3_PASS_TEXTURES;
  /* A PNG outside the renderable namespaces -- typically a ZDoom pack's
   * graphics/ patch (menu/HUD/status-bar art, fonts, TITLEPIC, ...) --
   * goes to the global namespace, not the deferred quarantine.  It is
   * safe there because U_PNGMaterializeLumps now also converts global
   * PNG/JPEG lumps to Doom patches before any renderer code reads them,
   * so a global PNG sharing a name with an IWAD patch is materialised
   * rather than fed to the patch parser as raw PNG bytes.  Ogg/FLAC/WAV
   * have no such in-place consumer for the patch path and stay deferred
   * (WAV is resolved separately by the sfx loader on a RIFF sniff). */
  if (pk3_is_png(d, len))
    return PK3_PASS_GLOBAL;
  if (pk3_is_deferred_format(d, len))
  {
    /* WAV and Ogg are now consumable by the sfx loader, so unlike the other
     * deferred formats they stay in the global namespace where
     * I_SndLoadSample can resolve them (e.g. a ZDoom mod shipping
     * sounds/DSPISTOL.wav or sounds/BARNSEE1.ogg).  PNG/FLAC remain
     * quarantined. */
    if (pk3_is_wav(d, len) || pk3_is_ogg(d, len))
      return PK3_PASS_GLOBAL;
    return PK3_PASS_DEFERRED;
  }
  return PK3_PASS_GLOBAL;
}

/* ---- exact-size image emission -------------------------------------------
 *
 * The central directory names every member's uncompressed size up front,
 * so the PWAD image is assembled in place with no intermediate copies:
 * one walk in measure mode totals the data bytes and an upper bound on
 * the directory entries, the image is allocated at exactly that size,
 * and a second walk in emit mode extracts every member directly into
 * its final position (deflated members inflate straight into the image,
 * stored members are one memcpy from the archive).  Peak memory is the
 * source archive plus the image itself -- the old flow held every
 * extracted member and a doubling scratch buffer besides.
 *
 * Both walks run the same code; only c->image selects the mode.  A
 * member that fails extraction in emit mode (late CRC mismatch) keeps
 * its planned data bytes as a hole, so every later member's position
 * still matches the measured layout, and only its directory entry is
 * dropped (the directory is written compactly, so numlumps just ends
 * up smaller than measured). */

typedef struct
{
  signed char    pass;     /* PK3_PASS_*, or -1 = skip                   */
  unsigned char  is_wad;   /* root-level wad image, expanded lump-wise   */
  unsigned char *wad;      /* held bytes for is_wad members              */
} pk3_plan_t;

typedef struct
{
  pk3_zip_t        *z;
  const pk3_plan_t *plan;
  unsigned char    *image;    /* NULL = measure mode                     */
  filelump_t       *dir;      /* NULL in measure mode                    */
  int               data_len; /* data bytes accounted (holes included)   */
  int               dir_len;  /* entries emitted (measure: upper bound)  */
} pk3_emit_t;

/* Append one directory entry at the current data offset. */
static void pk3_emit_dir(pk3_emit_t *c, const char *name, int size)
{
  if (c->dir)
  {
    filelump_t *fl = &c->dir[c->dir_len];
    size_t      n  = strlen(name);

    /* +12: lump data is laid out after the wadinfo_t header */
    fl->filepos = LONG(c->data_len + 12);
    fl->size    = LONG(size);
    /* filelump_t names are 8 bytes, NUL-padded but not NUL-terminated,
     * which is exactly the case -Wstringop-truncation flags strncpy
     * for; zero-fill and copy the clamped length instead. */
    if (n > 8)
      n = 8;
    memset(fl->name, 0, 8);
    memcpy(fl->name, name, n);
  }
  c->dir_len++;
}

/* Append one lump from bytes already in memory (markers pass NULL/0). */
static void pk3_emit_copy(pk3_emit_t *c, const char *name,
                          const void *data, int size)
{
  pk3_emit_dir(c, name, size);
  if (c->image && size)
    memcpy(c->image + 12 + c->data_len, data, size);
  c->data_len += size;
}

/* Append one archive member, extracting it directly into the image. */
static void pk3_emit_member(pk3_emit_t *c, int i, const char *name)
{
  const pk3_zent_t *e = &c->z->ent[i];

  if (!c->image)
    pk3_emit_dir(c, name, (int)e->usize);
  else if (pk3_zip_extract_to(c->z, e, c->image + 12 + c->data_len))
  {
    /* record this member's full archive path against the data offset it
     * occupies, so a later full-path lookup can pick it out of any
     * same-basename collision. */
    pk3_path_record(e->name, c->data_len + 12);
    pk3_emit_dir(c, name, (int)e->usize);
  }
  else
    lprintf(LO_WARN, "W_TranslatePK3: failed to extract %s\n", e->name);
  c->data_len += (int)e->usize;      /* hole kept on failure: layout holds */
}

/* Expand a root-level .wad member: append its lumps verbatim.  The wad
 * image itself was validated by pk3_is_wad_image before being held, so
 * only per-lump bounds can still be bad here. */
static void pk3_emit_inner_wad(pk3_emit_t *c, const char *member,
                               const unsigned char *wad, int len)
{
  wadinfo_t  header;
  filelump_t fl;
  int        i, numlumps, infotableofs;

  memcpy(&header, wad, sizeof(header));
  numlumps     = LONG(header.numlumps);
  infotableofs = LONG(header.infotableofs);
  for (i = 0; i < numlumps; i++)
  {
    char name[9];
    int  pos, size;

    memcpy(&fl, wad + infotableofs + i * 16, sizeof(fl));
    pos  = LONG(fl.filepos);
    size = LONG(fl.size);
    if (size < 0 || pos < 0 || (int64_t)pos + size > (int64_t)len)
    {
      if (c->image)
        lprintf(LO_WARN, "W_TranslatePK3: %s lump %d out of bounds, skipped\n",
                member, i);
      continue;
    }
    memset(name, 0, sizeof(name));
    strncpy(name, fl.name, 8);
    pk3_emit_copy(c, name, wad + pos, size);
  }
}

/* One full walk over the classified members in emission order.  Emission
 * order: root files + inner wads (zip order), then the sprite, flat and
 * texture marker groups, then the deferred quarantine.  Reordering
 * across folders is safe -- only same-name precedence matters to the
 * lump hash, and names cannot collide across these groups once the
 * deferred formats are quarantined. */
static void pk3_emit_walk(pk3_emit_t *c)
{
  int pass, i;

  for (pass = PK3_PASS_ROOT; pass <= PK3_PASS_DEFERRED; pass++)
  {
    int emitted = 0;

    for (i = 0; i < c->z->n; i++)
    {
      const pk3_plan_t *p = &c->plan[i];
      char              name[9];

      if (p->pass != pass)
        continue;
      if (p->is_wad)
      {
        pk3_emit_inner_wad(c, c->z->ent[i].name, p->wad,
                           (int)c->z->ent[i].usize);
        continue;
      }
      if (!emitted)
      {
        /* open this pass's marker group */
        if (pass == PK3_PASS_SPRITES)  pk3_emit_copy(c, "SS_START", NULL, 0);
        if (pass == PK3_PASS_FLATS)    pk3_emit_copy(c, "FF_START", NULL, 0);
        if (pass == PK3_PASS_TEXTURES) pk3_emit_copy(c, "TX_START", NULL, 0);
        if (pass == PK3_PASS_DEFERRED) pk3_emit_copy(c, "PD_START", NULL, 0);
        emitted = 1;
      }
      pk3_lump_name(name, c->z->ent[i].name);
      pk3_emit_member(c, i, name);
    }

    if (emitted)
    {
      if (pass == PK3_PASS_SPRITES)  pk3_emit_copy(c, "SS_END", NULL, 0);
      if (pass == PK3_PASS_FLATS)    pk3_emit_copy(c, "FF_END", NULL, 0);
      if (pass == PK3_PASS_TEXTURES) pk3_emit_copy(c, "TX_END", NULL, 0);
      if (pass == PK3_PASS_DEFERRED) pk3_emit_copy(c, "PD_END", NULL, 0);
    }
  }
}

unsigned char *W_TranslatePK3(const unsigned char *zip, int64_t zip_length,
                              int64_t *out_length, const char *archive_name)
{
  pk3_zip_t      z;
  pk3_plan_t    *plan;
  pk3_emit_t     measure, emit;
  unsigned char *image = NULL;
  unsigned char  sniff[16];   /* >= sizeof(wadinfo_t); every sniffer reads
                                 at most 12 bytes (pk3_is_wad_image) */
  wadinfo_t      header;
  int            i, image_len;

  /* The zip structures this reads - central directory offsets, sizes,
     the end-of-central-directory record - are 32-bit by specification.
     A larger archive needs ZIP64, which this translator does not
     implement, so it is refused here rather than passed down and
     silently narrowed into pk3_zip_open's int. */
  if (zip_length > (int64_t)0x7fffffff)
    return NULL;

  if (!pk3_zip_open(&z, zip, (int)zip_length))
  {
    lprintf(LO_WARN, "W_TranslatePK3: %s: not a readable ZIP archive\n",
            archive_name);
    return NULL;
  }

  plan = calloc(z.n ? z.n : 1, sizeof(*plan));
  if (!plan)
    goto oom;

  /* Classify every member from a prefix decode: the sniffers only read
   * the first bytes, so deflated members run a capped inflate into a
   * 16-byte scratch instead of a full extraction.  Length semantics are
   * preserved by passing the true uncompressed size (pk3_is_wad_image
   * bounds its directory against it). */
  for (i = 0; i < z.n; i++)
  {
    const pk3_zent_t *e = &z.ent[i];
    char              name[9];
    int               got;

    plan[i].pass = -1;
    if (e->is_dir || e->usize > 0x7fffffffu)
      continue;
    got = pk3_zip_sniff(&z, e, sniff, sizeof(sniff));
    if (got < 0)
    {
      lprintf(LO_WARN, "W_TranslatePK3: %s: failed to extract %s\n",
              archive_name, e->name);
      continue;
    }
    pk3_lump_name(name, e->name);
    if (!name[0])
      continue;
    plan[i].pass = (signed char)pk3_pass_of(e->name, sniff, (int)e->usize);

    /* Root-level wad images are the one case that needs full bytes early:
     * their internal directory drives the layout.  Hold them extracted
     * (and CRC-verified) until the emit walk copies their lumps out. */
    if (plan[i].pass == PK3_PASS_ROOT && pk3_is_wad_image(sniff, (int)e->usize))
    {
      plan[i].wad = pk3_zip_extract(&z, e);
      if (!plan[i].wad)
      {
        lprintf(LO_WARN, "W_TranslatePK3: %s: failed to extract %s\n",
                archive_name, e->name);
        plan[i].pass = -1;
        continue;
      }
      plan[i].is_wad = 1;
    }
  }

  /* Measure walk: exact data size and an upper bound on directory
   * entries (emit-time extraction failures only ever shrink it). */
  memset(&measure, 0, sizeof(measure));
  measure.z    = &z;
  measure.plan = plan;
  pk3_emit_walk(&measure);

  if ((int64_t)12 + measure.data_len + (int64_t)measure.dir_len * 16
      > 0x7fffffff)
    goto oom;                        /* image would overflow int offsets */
  image = malloc((size_t)12 + measure.data_len
                 + (size_t)measure.dir_len * 16);
  if (!image)
    goto oom;

  /* Emit walk: extract every member straight into its final position.
   * The directory is assembled in a small aligned side buffer (writing
   * filelump_t structs at an arbitrary data offset would be misaligned)
   * and copied to its slot once the walk is done. */
  emit       = measure;
  emit.image = image;
  emit.dir   = calloc(measure.dir_len ? measure.dir_len : 1,
                      sizeof(filelump_t));
  if (!emit.dir)
    goto oom;
  emit.data_len = 0;
  emit.dir_len  = 0;
  pk3_emit_walk(&emit);

  memcpy(header.identification, "PWAD", 4);
  header.numlumps     = LONG(emit.dir_len);
  header.infotableofs = LONG(12 + measure.data_len);
  memcpy(image, &header, 12);
  if (emit.dir_len)
    memcpy(image + 12 + measure.data_len, emit.dir,
           (size_t)emit.dir_len * 16);
  free(emit.dir);
  image_len = 12 + measure.data_len + emit.dir_len * 16;
  if (emit.dir_len < measure.dir_len)
  {
    /* return the slack from dropped entries; keep the old block if the
     * allocator declines */
    unsigned char *shrunk = realloc(image, image_len);
    if (shrunk)
      image = shrunk;
  }

  for (i = 0; i < z.n; i++)
    free(plan[i].wad);
  free(plan);

  /* the image is now the wadfile's data buffer: resolve each recorded
   * path's data address against it so W_PK3LumpForPath can match lumps
   * later. */
  pk3_path_bind(image);

  lprintf(LO_INFO,
          "W_TranslatePK3: %s: %d lumps synthesized from %d archive members\n",
          archive_name, emit.dir_len, z.n);
  *out_length = image_len;
  pk3_zip_close(&z);
  return image;

oom:
  if (plan)
    for (i = 0; i < z.n; i++)
      free(plan[i].wad);
  free(plan);
  free(image);
  pk3_zip_close(&z);
  lprintf(LO_WARN, "W_TranslatePK3: %s: out of memory\n", archive_name);
  return NULL;
}
