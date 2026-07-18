/* r_dynlight.h: runtime side of the GLDEFS point-light approximation.
 *
 * Once per rendered frame R_CollectDynLights walks the mobj list and records
 * every thing whose sprite has a GLDEFS light binding as an active light.
 * The wall/flat/sprite drawers then raise a surface point's effective light
 * level by R_DynLightBoost, a monochrome 1/d^2-style falloff, so surfaces
 * near a light are shaded brighter.  All math is integer (map units). */

#ifndef R_DYNLIGHT_H
#define R_DYNLIGHT_H

#include "doomtype.h"
#include "r_defs.h"

/* Rebuild the active-light list from the current mobjs.  Cheap no-op when no
 * GLDEFS light bindings were parsed. */
void R_CollectDynLights(void);

/* Non-zero if any light is active this frame (renderer fast-out). */
int R_DynLightsActive(void);

/* Light-level boost (0..) at a world point given in map units. */
int R_DynLightBoost(int wx, int wy, int wz);

/* Build the per-seg light sublist (lights whose reach touches this wall);
 * returns the count.  R_SegBoost queries it per (x,y,z) wall point. */
int R_SegPrepareLights(const seg_t *seg);
/* Per-column horizontal filter (call once per wall column) + 1D vertical
 * boost (call per band).  R_SegColumnPrepare returns the reaching-light count;
 * zero means the column is unlit and can skip the per-band split entirely. */
int R_SegColumnPrepare(int wx, int wy);
int R_SegColumnBoost(int wz);

/* Build the per-plane light sublist (lights reaching this z, vertical term
 * folded into a 2D radius); returns the count.  R_PlaneRowPrepare then
 * filters that set down to the lights reaching one span row (exact
 * point-to-segment distance) and R_PlaneRowBoost queries those per chunk. */
int R_PlanePrepareLights(int planez);
int R_PlaneRowPrepare(int ax, int ay, int bx, int by);
int R_PlaneRowBoost(int wx, int wy);
/* GLDEFS wall glow (glowing wall textures pooling onto flats): per-plane
 * collection by plane height, per-row conservative filter, per-chunk
 * point-to-segment boost.  Call GlowRowBoost after RowBoost (tint zeroing). */
int R_SectorWallGlow(int secnum);
int R_PlaneGlowPrepare(int planez);
int R_PlaneGlowRowPrepare(int ax, int ay, int bx, int by);
int R_PlaneGlowRowBoost(int wx, int wy);

/* Boost-weighted chroma from the most recent R_PlaneBoost / R_SegBoost call
 * (565 channel units * boost).  Zero unless a saturated light contributed;
 * the surface code shifts these down to a per-pixel additive tint. */
extern int dl_tint_r, dl_tint_g, dl_tint_b;
extern int dynlight_wall_falloff;
#define DL_TINT_SHIFT 8   /* boost-weighted chroma -> per-pixel channel add */

#endif
