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

/* Cheap AABB test: could any active light reach this seg? */
int R_SegLit(const seg_t *seg);

/* Cheap vertical test: could any active light reach a plane at this z? */
int R_PlaneLit(int planez);

#endif
