/* w_pk3.c: PK3/ZIP archive to in-memory WAD translation.
 *
 * The archive is walked once with miniz's in-memory ZIP reader and
 * re-emitted as a single PWAD image:
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

#include "miniz.h"

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
      const unsigned char *base = li->wadfile ?
        (li->wadfile->embedded_data ? li->wadfile->embedded_data
                                    : li->wadfile->data) : NULL;
      if (base && base + li->position == pk3_paths[i].data)
        return lump;
    }
  }
  return -1;
}

dbool W_IsPK3(const unsigned char *data, int length)
{
  return length >= 4 &&
         data[0] == 'P' && data[1] == 'K' &&
         data[2] == 0x03 && data[3] == 0x04;
}

/* ---- synthesized-directory builder -------------------------------------- */

typedef struct
{
  unsigned char *data;     /* growing lump data area (past the header)   */
  int            data_len;
  int            data_cap;
  filelump_t    *dir;      /* growing directory                          */
  int            dir_len;
  int            dir_cap;
} pk3_build_t;

static int pk3_grow(pk3_build_t *b, int add)
{
  if (b->data_len + add > b->data_cap)
  {
    int cap = b->data_cap ? b->data_cap : 1 << 20;
    while (b->data_len + add > cap)
      cap = cap << 1;
    b->data = realloc(b->data, cap);
    if (!b->data)
      return 0;
    b->data_cap = cap;
  }
  return 1;
}

/* Append one lump.  data may be NULL for zero-size markers. */
static int pk3_add_lump(pk3_build_t *b, const char *name,
                        const void *data, int size)
{
  filelump_t *fl;

  if (size && !pk3_grow(b, size))
    return 0;
  if (b->dir_len == b->dir_cap)
  {
    b->dir_cap = b->dir_cap ? b->dir_cap * 2 : 256;
    b->dir = realloc(b->dir, b->dir_cap * sizeof(filelump_t));
    if (!b->dir)
      return 0;
  }
  fl = &b->dir[b->dir_len++];
  /* +12: lump data is laid out after the wadinfo_t header */
  fl->filepos = LONG(b->data_len + 12);
  fl->size    = LONG(size);
  /* filelump_t names are 8 bytes, NUL-padded but not NUL-terminated,
   * which is exactly the case -Wstringop-truncation flags strncpy for;
   * zero-fill and copy the clamped length instead. */
  {
    size_t n = strlen(name);
    if (n > 8)
      n = 8;
    memset(fl->name, 0, 8);
    memcpy(fl->name, name, n);
  }
  if (size)
  {
    memcpy(b->data + b->data_len, data, size);
    b->data_len += size;
  }
  return 1;
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

/* Expand a root-level .wad member: append its lumps verbatim. */
static int pk3_add_inner_wad(pk3_build_t *b, const char *member,
                             const unsigned char *wad, int len)
{
  wadinfo_t  header;
  filelump_t fl;
  int        i, numlumps, infotableofs;

  if (len < (int)sizeof(wadinfo_t))
    return 0;
  memcpy(&header, wad, sizeof(header));
  if (strncmp(header.identification, "IWAD", 4) &&
      strncmp(header.identification, "PWAD", 4))
  {
    lprintf(LO_WARN, "W_TranslatePK3: %s is not a wad, skipped\n", member);
    return 1;     /* tolerated, not fatal */
  }
  numlumps     = LONG(header.numlumps);
  infotableofs = LONG(header.infotableofs);
  if (numlumps < 0 ||
      infotableofs < 0 ||
      (int64_t)infotableofs + (int64_t)numlumps * 16 > (int64_t)len)
  {
    lprintf(LO_WARN, "W_TranslatePK3: %s has a corrupt directory, skipped\n",
            member);
    return 1;
  }
  for (i = 0; i < numlumps; i++)
  {
    char name[9];
    int  pos, size;

    memcpy(&fl, wad + infotableofs + i * 16, sizeof(fl));
    pos  = LONG(fl.filepos);
    size = LONG(fl.size);
    if (size < 0 || pos < 0 || (int64_t)pos + size > (int64_t)len)
    {
      lprintf(LO_WARN, "W_TranslatePK3: %s lump %d out of bounds, skipped\n",
              member, i);
      continue;
    }
    memset(name, 0, sizeof(name));
    strncpy(name, fl.name, 8);
    if (!pk3_add_lump(b, name, wad + pos, size))
      return 0;
  }
  return 1;
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

unsigned char *W_TranslatePK3(const unsigned char *zip, int zip_length,
                              int *out_length, const char *archive_name)
{
  mz_zip_archive za;
  pk3_build_t    b;
  unsigned char *image;
  wadinfo_t      header;
  int            pass, n, i, image_len;

  memset(&za, 0, sizeof(za));
  memset(&b, 0, sizeof(b));
  if (!mz_zip_reader_init_mem(&za, zip, (size_t)zip_length, 0))
  {
    lprintf(LO_WARN, "W_TranslatePK3: %s: not a readable ZIP archive\n",
            archive_name);
    return NULL;
  }
  n = (int)mz_zip_reader_get_num_files(&za);

  /* Emission order: root files + inner wads (zip order), then the
   * sprite and flat marker groups, then the deferred quarantine.
   * Reordering across folders is safe -- only same-name precedence
   * matters to the lump hash, and names cannot collide across these
   * groups once the deferred formats are quarantined. */
  for (pass = PK3_PASS_ROOT; pass <= PK3_PASS_DEFERRED; pass++)
  {
    int emitted = 0;

    for (i = 0; i < n; i++)
    {
      mz_zip_archive_file_stat st;
      char   name[9];
      void  *data;
      size_t size;

      if (!mz_zip_reader_file_stat(&za, (mz_uint)i, &st))
        continue;
      if (mz_zip_reader_is_file_a_directory(&za, (mz_uint)i))
        continue;
      if (st.m_uncomp_size > 0x7fffffff)
        continue;

      /* Pass classification needs the bytes (format sniff), so files
       * are extracted exactly once on their matching pass: extract,
       * classify, and either emit or drop the buffer. */
      data = mz_zip_reader_extract_to_heap(&za, (mz_uint)i, &size, 0);
      if (!data)
      {
        lprintf(LO_WARN, "W_TranslatePK3: %s: failed to extract %s\n",
                archive_name, st.m_filename);
        continue;
      }
      if (pk3_pass_of(st.m_filename, data, (int)size) != pass)
      {
        free(data);
        continue;
      }

      pk3_lump_name(name, st.m_filename);
      if (!name[0])
      {
        free(data);
        continue;
      }

      if (pass == PK3_PASS_ROOT && pk3_is_wad_image(data, (int)size))
      {
        if (!pk3_add_inner_wad(&b, st.m_filename, data, (int)size))
          goto oom;
      }
      else
      {
        if (!emitted)
        {
          /* open this pass's marker group */
          if (pass == PK3_PASS_SPRITES  && !pk3_add_lump(&b, "SS_START", NULL, 0)) goto oom;
          if (pass == PK3_PASS_FLATS    && !pk3_add_lump(&b, "FF_START", NULL, 0)) goto oom;
          if (pass == PK3_PASS_TEXTURES && !pk3_add_lump(&b, "TX_START", NULL, 0)) goto oom;
          if (pass == PK3_PASS_DEFERRED && !pk3_add_lump(&b, "PD_START", NULL, 0)) goto oom;
        }
        /* record this member's full archive path against the data offset it
         * is about to occupy, so a later full-path lookup can pick it out of
         * any same-basename collision (filepos mirrors pk3_add_lump). */
        pk3_path_record(st.m_filename, b.data_len + 12);
        if (!pk3_add_lump(&b, name, data, (int)size))
          goto oom;
        emitted = 1;
      }
      free(data);
      data = NULL;
      continue;
oom:
      free(data);
      free(b.data);
      free(b.dir);
      mz_zip_reader_end(&za);
      lprintf(LO_WARN, "W_TranslatePK3: %s: out of memory\n", archive_name);
      return NULL;
    }

    if (emitted)
    {
      int ok = 1;
      if (pass == PK3_PASS_SPRITES)  ok = pk3_add_lump(&b, "SS_END", NULL, 0);
      if (pass == PK3_PASS_FLATS)    ok = pk3_add_lump(&b, "FF_END", NULL, 0);
      if (pass == PK3_PASS_TEXTURES) ok = pk3_add_lump(&b, "TX_END", NULL, 0);
      if (pass == PK3_PASS_DEFERRED) ok = pk3_add_lump(&b, "PD_END", NULL, 0);
      if (!ok)
      {
        free(b.data);
        free(b.dir);
        mz_zip_reader_end(&za);
        return NULL;
      }
    }
  }
  mz_zip_reader_end(&za);

  /* assemble: header + lump data + directory */
  image_len = 12 + b.data_len + b.dir_len * 16;
  image = malloc(image_len);
  if (!image)
  {
    free(b.data);
    free(b.dir);
    return NULL;
  }
  memcpy(header.identification, "PWAD", 4);
  header.numlumps     = LONG(b.dir_len);
  header.infotableofs = LONG(12 + b.data_len);
  memcpy(image, &header, 12);
  if (b.data_len)
    memcpy(image + 12, b.data, b.data_len);
  memcpy(image + 12 + b.data_len, b.dir, b.dir_len * 16);
  free(b.data);
  free(b.dir);

  /* the image is now the wadfile's data buffer: resolve each recorded path's
   * data address against it so W_PK3LumpForPath can match lumps later. */
  pk3_path_bind(image);

  lprintf(LO_INFO,
          "W_TranslatePK3: %s: %d lumps synthesized from %d archive members\n",
          archive_name, b.dir_len, n);
  *out_length = image_len;
  return image;
}
