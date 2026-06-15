/* p_vslope.h: thing-based vertex slopes (ZDoom Vertex Floor Z 1504 /
 * Vertex Ceiling Z 1505).
 *
 * A "vertex slope" thing sits on a map vertex and carries a Z height in
 * its spawn-height field.  When exactly three of a sector's vertices
 * carry such a thing for the same plane (floor or ceiling), the sector's
 * floor/ceiling tilts to the plane fitted through those three (x,y,z)
 * points -- the same secplane_t the Plane_Align path produces, so the
 * renderer and physics that already read sector->floor_slope /
 * ceiling_slope need no changes.
 *
 * The things are intercepted at thing-load time (before P_SpawnMapThing)
 * and recorded in full map-unit precision; the plane fit runs once at
 * level setup after the sector->line groups exist.
 */

#ifndef P_VSLOPE_H
#define P_VSLOPE_H

#include "m_fixed.h"

/* which: 0 = floor (thing 1504), 1 = ceiling (thing 1505) */
void P_AddSlopeVertex(fixed_t x, fixed_t y, fixed_t z, int which);

/* discard any vertices recorded for the previous level */
void P_ClearSlopeVertices(void);

/* fit floor/ceiling planes for sectors bounded by three slope vertices */
void P_SpawnVertexSlopes(void);

#endif
