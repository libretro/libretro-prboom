/* u_png.h: PNG decoding for ZDoom pk3 assets.
 *
 * Rides libretro-common's RPNG decoder.  Converts decoded images into
 * the formats this engine's renderer consumes: column-format patches
 * (PLAYPAL-mapped, alpha-keyed posts, grAb offsets honored, DeePsea
 * tall-patch chains when content exceeds row 254) and raw 64x64 flats
 * (nearest-resampled: the span drawers index 64x64 sources).
 */

#ifndef __U_PNG__
#define __U_PNG__

#include "doomtype.h"

dbool U_PNGIsPNG(const unsigned char *d, int len);

/* Decode into a Doom patch.  malloc()d; caller owns.  NULL on failure. */
void *U_PNGToPatch(const unsigned char *d, int len, int *out_size);
void *U_PNGToPatchDecal(const unsigned char *d, int len, int *out_size);

/* as U_PNGToPatch, resampled to tw x th first (0,0 = native); used to
 * apply TEXTURES-lump scale at materialization time */
void *U_PNGToPatchSized(const unsigned char *d, int len,
                        int tw, int th, int *out_size);

/* Decode into a raw 64x64 flat.  malloc()d; caller owns.  NULL on failure. */
void *U_PNGToFlat(const unsigned char *d, int len, int *out_size);

/* Walk the SS/FF/TX marker groups and replace every PNG member's lump
 * bytes with the converted patch/flat (via W_ReplaceLumpData).  Must run
 * after W_Init and before the renderer first reads those lumps. */
void U_PNGMaterializeLumps(void);

/* Look up the native-colour (0xAABBGGRR) copy of a global-namespace image
 * lump retained at materialization time, for the full-colour overlay blit.
 * Returns NULL if the lump has no cached native image. */
const unsigned *U_PNGCacheRGBA(int lump, int *w, int *h);

#endif
