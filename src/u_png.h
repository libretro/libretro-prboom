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

/* Decode into a raw 64x64 flat.  malloc()d; caller owns.  NULL on failure. */
void *U_PNGToFlat(const unsigned char *d, int len, int *out_size);

/* Walk the SS/FF/TX marker groups and replace every PNG member's lump
 * bytes with the converted patch/flat (via W_ReplaceLumpData).  Must run
 * after W_Init and before the renderer first reads those lumps. */
void U_PNGMaterializeLumps(void);

#endif
