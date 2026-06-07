/* p_slope.h: sloped sector planes (ZDoom Plane_Align, special 181).
 *
 * A sloped floor or ceiling carries a plane equation
 *   a*x + b*y + c*z + d = 0
 * in 16.16 fixed point with (a,b,c) the unit normal, c != 0.  Flat
 * sectors keep NULL slope pointers and pay nothing.
 */

#ifndef P_SLOPE_H
#define P_SLOPE_H

#include "m_fixed.h"
#include "r_defs.h"

fixed_t P_PlaneZatPoint(const secplane_t *p, fixed_t x, fixed_t y);

/* height of a sector's floor/ceiling at a point (flat sectors return
 * the constant heights) */
fixed_t P_FloorZAtPoint(const sector_t *s, fixed_t x, fixed_t y);
fixed_t P_CeilingZAtPoint(const sector_t *s, fixed_t x, fixed_t y);

/* spawn-time pass over ZDoom Doom-in-Hexen lines: build slope planes
 * from Plane_Align (181) lines and clear their specials */
void P_SpawnZDoomSlopes(void);

#endif
