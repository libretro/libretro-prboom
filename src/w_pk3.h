/* w_pk3.h: PK3/ZIP archive to in-memory WAD translation.
 *
 * ZDoom-targeted mods ship as PK3 archives: a ZIP whose folders map onto
 * lump namespaces and whose root-level .wad members (maps, monster packs)
 * are loaded as if part of the archive.  Rather than teaching the lump
 * manager a third storage type, W_TranslatePK3 inflates the archive into
 * a single synthesized PWAD image that the existing W_AddFile directory
 * parser consumes unchanged (the same way the baked-in prboom.wad is
 * served from a memory buffer). */

#ifndef __W_PK3__
#define __W_PK3__

#include "doomtype.h"

/* ZIP local-file-header magic check */
dbool W_IsPK3(const unsigned char *data, int length);

/* Translate a ZIP archive in memory into a synthesized PWAD image.
 * Returns a malloc'd buffer (caller owns) and its size in *out_length,
 * or NULL on a malformed archive. */
unsigned char *W_TranslatePK3(const unsigned char *zip, int zip_length,
                              int *out_length, const char *archive_name);

#endif
