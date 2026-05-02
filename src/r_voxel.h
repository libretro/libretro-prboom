/* r_voxel.h -- voxel (KVX) model support for libretro-prboom.
 *
 * Turn 1 of voxel-renderer integration: KVX parser only.
 *
 * The KVX format was designed by Ken Silverman for the Build engine
 * (Shadow Warrior, Blood, etc.) and is documented in slab6.txt.  This
 * implementation derives its structure from Andrew Apted's KVX loader
 * in Woof! (src/r_voxel.c, GPL-2.0-or-later, Copyright (C) 2023
 * Andrew Apted) but is a clean rewrite -- not a verbatim port -- with
 * the following corrections / simplifications:
 *
 *   - 32-bit reads of x/y/z size and pivots (Woof's loader truncates
 *     to one byte for size and two bytes for pivot; works for typical
 *     voxels but fragile for large ones).
 *
 *   - Original KVX palette is preserved alongside the slab data so
 *     callers can choose when / how to remap to a target palette
 *     (PLAYPAL or otherwise).  Woof remaps eagerly at load time.
 *
 *   - No D_Doom-specific dependencies (Z_Malloc, sprnames, etc.) in
 *     the loader itself; it takes a raw byte buffer + length and
 *     returns a self-contained kvx_model_t allocated via plain
 *     malloc().  Z_Malloc integration will come at the engine-wiring
 *     stage in a later turn.
 *
 * Format reference:
 *   slab6.txt (Ken Silverman, http://advsys.net/ken/download.htm)
 *   slab6_formats.md by falkreon (https://gist.github.com/falkreon/
 *     8b873ec6797ffad247375fc73614fd08)
 *
 * The format in summary:
 *
 *   Each KVX file contains 1 or 5 mipmap levels followed by a 768-
 *   byte VGA palette (intensities 0-63 per channel).  Each mip:
 *
 *     uint32 numbytes                 // size of this mip excluding numbytes
 *     uint32 xsize, ysize, zsize      // grid dims; zsize == height (Z = down)
 *     uint32 xpivot, ypivot, zpivot   // pivot in 24.8 fixed-point voxel units
 *     uint32 xoffset[xsize+1]         // base offset for each X plane
 *     uint16 xyoffset[xsize][ysize+1] // per-(x,y) column offset relative to
 *                                     // xoffset[x]; the column's slab data
 *                                     // runs from offsets[x][y] to
 *                                     // offsets[x][y+1]
 *     byte   rawslabdata[...]         // packed slabs for every (x,y) column
 *
 *   Per (x,y) column there are zero or more slabs, each:
 *
 *     uint8  ztop                     // top voxel's Z coordinate
 *     uint8  zlen                     // run length (number of color bytes)
 *     uint8  face                     // bits: which faces are exposed
 *     uint8  col[zlen]                // palette indices, top -> bottom
 *
 *   The 'face' byte bits (Apted's convention, matching this loader):
 *     0x01 = LEFT   (-X face)
 *     0x02 = RIGHT  (+X face)
 *     0x04 = BACK   (-Y face)
 *     0x08 = FRONT  (+Y face)
 *     0x10 = TOP    (-Z face; remember Z increases downward)
 *     0x20 = BOTTOM (+Z face)
 *
 *   Slab data is stored linearly per-column.  Apted's trick: subtract
 *   the minimum used offset across all (x,y) columns, then copy only
 *   the bytes actually referenced.  Same trick used here.
 */

#ifndef __R_VOXEL_H__
#define __R_VOXEL_H__

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Slab face bit flags (matches Andrew Apted's Woof loader). */
#define KVX_FACE_LEFT    0x01u
#define KVX_FACE_RIGHT   0x02u
#define KVX_FACE_BACK    0x04u
#define KVX_FACE_FRONT   0x08u
#define KVX_FACE_TOP     0x10u
#define KVX_FACE_BOTTOM  0x20u
#define KVX_FACE_MASK    0x3fu

/* Hard limits.  KVX format technically allows up to 256x256x255; the
 * classic Build Engine renderer caps at 128x128x200.  We follow the
 * higher SLAB6 limit to allow voxel mods authored in modern tools. */
#define KVX_MAX_SIZE   256

/* Single voxel model (one KVX mipmap level). */
typedef struct kvx_model_s
{
   /* Grid dimensions in voxels. */
   int       x_size;
   int       y_size;
   int       z_size;

   /* Pivot (centroid) in Doom 16.16 fixed-point voxel units.
    * The KVX format stores these as 24.8; we normalize to 16.16
    * here so callers can use Doom's standard fixed_t arithmetic. */
   int32_t   x_pivot_fx;
   int32_t   y_pivot_fx;
   int32_t   z_pivot_fx;

   /* Per-column slab data.
    *
    * For column (x,y) (with 0 <= x < x_size, 0 <= y < y_size):
    *   offsets[y * x_size + x]       -> first byte of column in `data`
    *   offsets[(y+1) * x_size + x]   -> first byte AFTER column
    *
    * Note the indexing: the array is of length x_size * (y_size+1),
    * laid out so that offsets[(y+1) * x_size + x] is the natural
    * "end" pointer.  This mirrors KVX's xyoffset[x][y+1] sentinel.
    *
    * Iterate slabs in a column from start to end:
    *   p = data + offsets[ y    * x_size + x];
    *   q = data + offsets[(y+1) * x_size + x];
    *   while (p < q) {
    *     uint8_t ztop  = *p++;
    *     uint8_t zlen  = *p++;
    *     uint8_t face  = *p++;
    *     uint8_t *col  = p;
    *     p += zlen;
    *     -- voxels at (x, y, ztop+0..ztop+zlen-1)
    *     -- colors col[0..zlen-1] are KVX palette indices
    *   }
    */
   int     * offsets;       /* x_size * (y_size + 1) ints */
   uint8_t * data;          /* slab data buffer */
   size_t    data_size;

   /* KVX's own VGA palette.  RGB triples, intensities 0-63 (raw VGA;
    * shift left by 2 to get 0-255).  Always 768 bytes (256 entries).
    * Some KVX files share palettes -- callers can choose to remap
    * to PLAYPAL or render directly with this palette. */
   uint8_t   palette[768];
} kvx_model_t;

/* Parse a KVX byte buffer into a fresh kvx_model_t.
 *
 * Returns NULL on:
 *   - too-short input (< minimum header + 768 palette)
 *   - zero or out-of-range dimensions
 *   - malformed offset tables (a slab data range that would overflow
 *     the input buffer)
 *   - allocation failure
 *
 * The returned model owns its `offsets` and `data` allocations and
 * must be released with R_KVX_Free().  Only the FIRST mipmap level
 * is decoded; subsequent levels (if any, in unstripped Build-engine
 * KVX files) are ignored.  The palette is read from the LAST 768
 * bytes of the input as recommended in slab6.txt -- this works
 * regardless of how many mipmap levels are present.
 *
 * The input buffer is not retained; the caller may free it after
 * this call returns.
 */
kvx_model_t *R_KVX_Load(const void *data, size_t len);

/* Release a model returned by R_KVX_Load. */
void R_KVX_Free(kvx_model_t *m);

/* ------------------------------------------------------------------
 * Sprite-cache prerasterizer (Turn 2 of the voxel renderer)
 *
 * Renders a kvx_model_t into a Doom-format rpatch_t for a single
 * fixed view direction and palette mapping.  The caller can then
 * use the resulting rpatch_t in place of an actual sprite via the
 * normal R_DrawMaskedColumn pipeline.
 *
 * Turn-2 simplifications:
 *   - Single view direction only (camera looking down -Y).  Does
 *     not yet pre-render multiple rotations.  A real voxel mod
 *     needs ~32 rotations cached, which Turn 3 will add.
 *   - Orthographic projection: each voxel becomes one pixel.  No
 *     perspective scaling.  Output dimensions are model x_size by
 *     z_size pixels.
 *   - Palette: caller supplies a 256-entry remap table that maps
 *     KVX palette indices to target-palette (e.g. PLAYPAL) indices.
 *     Pass NULL for an identity mapping (use KVX colors as-is).
 *   - Voxel color 0 is treated as transparent.  KVX models that
 *     actually use palette index 0 for visible voxels will lose
 *     those pixels.  Real KVX content avoids index 0 by convention.
 *
 * Returns NULL on alloc failure.  The returned rpatch_t owns a
 * single contiguous allocation (->data); free with R_KVX_FreeSprite.
 * ------------------------------------------------------------------ */

#ifndef KVX_PARSER_ONLY     /* set when standalone-compiling the parser */
#include "r_patch.h"        /* full rpatch_t definition */

/* Build a sprite-format rpatch_t from a voxel model, viewed along
 * the model's -Y axis (an arbitrary canonical "front view"). */
rpatch_t *R_KVX_RasterizeFront(const kvx_model_t *m,
                               const uint8_t *palette_remap);

/* Free an rpatch_t produced by R_KVX_RasterizeFront. */
void R_KVX_FreeSprite(rpatch_t *p);

/* Build an in-memory test voxel for integration verification when
 * no real .kvx file is available.  Returns an 8x8x8 cube with
 * corners cut off, with each face a different palette color.
 * Caller frees with R_KVX_Free. */
kvx_model_t *R_KVX_BuiltinTestVoxel(void);

/* ------------------------------------------------------------------
 * Engine integration (Turn 2: stub)
 *
 * For Turn 2, a global table maps a single sprite lump number to
 * a prerasterized voxel rpatch_t.  R_KVX_Init() populates it at
 * startup; R_KVX_LookupSprite() is consulted by R_DrawVisSprite
 * before falling through to the WAD lump.
 *
 * This is a placeholder for the real per-sprite, per-frame, per-
 * rotation mapping that arrives in Turn 3 with the VOXELDEF
 * parser.  For now we replace exactly ONE sprite (the medikit) for
 * end-to-end demonstration.
 *
 * Excluded from KVX_NO_ENGINE_GLUE builds (e.g. the standalone
 * prerasterizer test fixture) which don't link the engine. */

#ifndef KVX_NO_ENGINE_GLUE
/* Runtime toggle bound to the "Voxel sprites" menu entry and the
 * "voxel_sprites" cvar (m_misc.c).  Default 0 (off).  Toggled
 * via M_ChangeVoxelSprites in m_menu.c. */
extern int voxel_sprites;

void R_KVX_Init(void);
void R_KVX_Shutdown(void);

/* Look up a voxel-prerasterized rpatch_t for an absolute sprite
 * lump number (i.e. lump + firstspritelump).  Returns NULL if no
 * voxel is registered for this lump, or if voxel_sprites is 0. */
const rpatch_t *R_KVX_LookupSprite(int lump);
#endif

#endif /* !KVX_PARSER_ONLY */

#ifdef __cplusplus
}
#endif

#endif /* __R_VOXEL_H__ */
