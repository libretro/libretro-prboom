/* u_zsecact.h: ZDoom sector action things (editor numbers 9982-9999).
 *
 * Sector actions are invisible markers whose special + args run with
 * the triggering actor as activator when a sector event occurs.  No
 * mobj is spawned: P_SpawnMapThing registers the marker's sector and
 * payload here, and the movement code raises the events.  Implemented
 * events are Enter (9998) and HitFloor (9999) -- the two MyHouse uses,
 * where Teleport_NoFog actions implement its seamless space folding;
 * the rest of the family registers and stays dormant.  Activation
 * follows the ZDoom default: players only. */

#ifndef __U_ZSECACT__
#define __U_ZSECACT__

#include "doomtype.h"
#include "r_defs.h"
#include "p_mobj.h"

#define ZSECACT_ENTER    9998
#define ZSECACT_HITFLOOR 9999

/* forget all registrations (call at level setup, before things spawn) */
void U_ZSecActClear(void);

/* record a sector action thing at map coordinates (x, y) */
void U_ZSecActRegister(fixed_t x, fixed_t y, int type, int special,
                       const int *args);

/* raise an event on a sector; runs every matching action's special
 * with `activator`.  Bounded against teleport-chain recursion. */
void U_ZSecActTrigger(sector_t *sector, int type, mobj_t *activator);

#endif
