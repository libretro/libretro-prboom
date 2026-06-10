/* u_brightmap.h: ZDoom GLDEFS/DOOMDEFS brightmap definitions for the
 * software renderer.
 *
 * A brightmap is a per-texel mask the same size as a texture, flat or
 * sprite: where the mask byte is non-zero the texel is drawn at full
 * brightness (ignoring the sector / distance light level), which is how
 * ZDoom packs make computer screens, lamps, lava veins, weapon LEDs and
 * the like glow in the dark.  The definitions live in *DEFS lumps
 * (GLDEFS, DOOMDEFS, ...) as
 *
 *     brightmap texture NAME { map "brightmaps/x.png" [iwad] }
 *     brightmap flat    NAME { ... }
 *     brightmap sprite  NAME { ... }
 *
 * This module only parses those blocks and resolves each to the lump that
 * holds the mask image; building the per-texture mask and applying it in
 * the column drawers is done elsewhere. */

#ifndef U_BRIGHTMAP_H
#define U_BRIGHTMAP_H

#include "doomtype.h"

typedef enum
{
  BM_TEXTURE,
  BM_FLAT,
  BM_SPRITE
} brightmap_kind_t;

typedef struct
{
  char             name[9];   /* target texture/flat/sprite, NUL-padded   */
  brightmap_kind_t kind;
  int              maplump;    /* lump holding the mask image (>= 0)        */
} brightmap_def_t;

/* Parse every *DEFS lump's brightmap blocks into the table.  Safe to call
 * once after W_Init; a second call is a no-op. */
void U_LoadBrightmaps(void);

/* Look up a brightmap definition by target name and kind, or NULL. */
const brightmap_def_t *U_BrightmapFor(const char *name, brightmap_kind_t kind);

/* Build the per-texture bright masks from the parsed definitions.  Call
 * after R_InitTextures so texture numbers and composite dimensions are
 * available; a second call is a no-op. */
void U_BuildBrightmasks(void);

/* O(1) render-time lookup: the column-major (mask[col*height + row]) bright
 * mask for a wall texture, or NULL if it has none.  1 = fullbright texel. */
const unsigned char *U_BrightmaskForTexture(int texnum);

/* Number of definitions parsed (diagnostics / tests). */
int U_BrightmapCount(void);

void U_FreeBrightmaps(void);

#endif
