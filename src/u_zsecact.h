/* u_zsecact.h: ZDoom sector action things (editor numbers 9982-9999 and the
 * 9040-9048 damage/death/health block).
 *
 * Sector actions are invisible markers whose special + args run with
 * the triggering actor as activator when a sector event occurs.  No
 * mobj is spawned: P_SpawnMapThing registers the marker's sector and
 * payload here, and the movement / damage code raises the events.
 * Implemented events are Enter (9998), HitFloor (9999), and the player
 * Damage (9048/9047/9044) and Death (9046/9045/9043) actions; the rest
 * of the family registers and stays dormant.  Activation follows the
 * ZDoom default: players only. */

#ifndef __U_ZSECACT__
#define __U_ZSECACT__

#include "doomtype.h"
#include "r_defs.h"
#include "p_mobj.h"

#define ZSECACT_ENTER    9998
#define ZSECACT_HITFLOOR 9999

/* 9040-9048 block: the editor number is the registered type, and a sector
 * event maps to the set of editor numbers that respond to it.  Damage and
 * Death come in floor / ceiling / 3D-floor variants; this port has no 3D
 * floors, so the floor and 3D variants both fire on a normal damage/death
 * and the ceiling variant stays dormant. */
#define ZSECACT_HEALTHFLOOR   9040
#define ZSECACT_HEALTHCEILING 9041
#define ZSECACT_HEALTH3D      9042
#define ZSECACT_DEATH3D       9043
#define ZSECACT_DAMAGE3D      9044
#define ZSECACT_DEATHCEILING  9045
#define ZSECACT_DEATHFLOOR    9046
#define ZSECACT_DAMAGECEILING 9047
#define ZSECACT_DAMAGEFLOOR   9048

/* forget all registrations (call at level setup, before things spawn) */
void U_ZSecActClear(void);

/* record a sector action thing at map coordinates (x, y) */
void U_ZSecActRegister(fixed_t x, fixed_t y, int type, int special,
                       const int *args);

/* raise an event on a sector; runs every matching action's special
 * with `activator`.  Bounded against teleport-chain recursion. */
void U_ZSecActTrigger(sector_t *sector, int type, mobj_t *activator);

/* raise the player-damage sector action for the sector the actor stands in */
void U_ZSecActDamage(mobj_t *activator);

/* raise the player-death sector action for the sector the actor stands in */
void U_ZSecActDeath(mobj_t *activator);

#endif
