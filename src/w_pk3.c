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
  memset(fl->name, 0, 8);
  strncpy(fl->name, name, 8);
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
#define PK3_PASS_GLOBAL   3   /* every other folder, native formats */
#define PK3_PASS_DEFERRED 4   /* modern formats from any folder     */

static int pk3_pass_of(const char *path, const unsigned char *d, int len)
{
  const char *slash = strchr(path, '/');

  if (!slash)
    return PK3_PASS_ROOT;
  if (pk3_is_deferred_format(d, len))
    return PK3_PASS_DEFERRED;
  if (!strncasecmp(path, "sprites/", 8))
    return PK3_PASS_SPRITES;
  if (!strncasecmp(path, "flats/", 6))
    return PK3_PASS_FLATS;
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

      if (pass == PK3_PASS_ROOT && size >= 4 &&
          (!memcmp(data, "PWAD", 4) || !memcmp(data, "IWAD", 4)))
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
          if (pass == PK3_PASS_DEFERRED && !pk3_add_lump(&b, "PD_START", NULL, 0)) goto oom;
        }
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

  lprintf(LO_INFO,
          "W_TranslatePK3: %s: %d lumps synthesized from %d archive members\n",
          archive_name, b.dir_len, n);
  *out_length = image_len;
  return image;
}
