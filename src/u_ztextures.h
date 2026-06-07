/* u_ztextures.h: ZDoom TEXTURES lump parser.
 *
 * Parses new-format TEXTURES definitions (acc/SLADE style):
 *   Texture "NAME", W, H { XScale f  YScale f  Patch "PNAME", x, y }
 * covering the single-patch shape ZDoom-era pk3s use for scaled PNG
 * textures.  Multi-patch composites and per-patch sub-blocks are
 * skipped with a warning.
 *
 * This engine's renderer has no per-texture scale, so scale is applied
 * at asset level: U_PNGMaterializeLumps resamples each defined
 * texture's patch to its world size (declared dims divided by scale),
 * and r_data registers definitions whose name has no lump of its own
 * (e.g. a texture cropping/scaling another texture's patch). */

#ifndef __U_ZTEXTURES__
#define __U_ZTEXTURES__

#include "doomtype.h"

typedef struct
{
  char   name[9];
  int    width, height;          /* declared texel dimensions */
  double xscale, yscale;         /* world size = dims / scale */
  char   patch[9];               /* single source patch lump */
  int    patch_x, patch_y;       /* placement on the texel canvas */
} ztexture_t;

extern ztexture_t *ztextures;
extern int num_ztextures;

/* Parse every lump named TEXTURES (pk3 root TEXTURES.txt and any inner
 * wads' copies).  Idempotent per session; call before
 * U_PNGMaterializeLumps. */
void U_ZTexturesLoad(void);

/* definition by texture name, NULL if none */
const ztexture_t *U_ZTexturesFind(const char *name);

/* world-size target for a patch lump: the first definition whose
 * single patch is `lumpname` placed at 0,0 under the same name.
 * Returns false when the lump should stay at native size. */
dbool U_ZTexturesTargetSize(const char *lumpname, int *w, int *h);

#endif
