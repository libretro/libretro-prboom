/* u_zsecact.c: ZDoom sector action things.  See u_zsecact.h. */

#include <string.h>

#include "doomstat.h"
#include "doomtype.h"
#include "lprintf.h"
#include "m_fixed.h"
#include "map_format.h"
#include "r_main.h"
#include "z_zone.h"
#include "u_zsecact.h"

typedef struct
{
  sector_t *sector;
  int       type;
  int       special;
  int       args[5];
} zsecact_t;

static zsecact_t *actions;
static int num_actions, cap_actions;

void U_ZSecActClear(void)
{
  num_actions = 0;
}

void U_ZSecActRegister(fixed_t x, fixed_t y, int type, int special,
                       const int *args)
{
  zsecact_t *a;

  if (num_actions == cap_actions)
  {
    int nc = cap_actions ? cap_actions * 2 : 64;
    zsecact_t *na = Z_Malloc(nc * sizeof(*na), PU_STATIC, 0);
    if (actions)
    {
      memcpy(na, actions, num_actions * sizeof(*na));
      Z_Free(actions);
    }
    actions = na;
    cap_actions = nc;
  }
  a = &actions[num_actions++];
  a->sector = R_PointInSubsector(x, y)->sector;
  a->type = type;
  a->special = special;
  memcpy(a->args, args, sizeof(a->args));
}

void U_ZSecActTrigger(sector_t *sector, int type, mobj_t *activator)
{
  /* An action's special can move the activator into another action's
   * sector (Teleport_NoFog chains are MyHouse's whole trick), which
   * re-enters this function from the movement code.  Legitimate chains
   * are short; a marker pair teleporting at each other would not be. */
  static int depth;
  int i;

  if (!num_actions || !activator || !activator->player)
    return;
  if (depth >= 8)
    return;

  depth++;
  for (i = 0; i < num_actions; i++)
  {
    zsecact_t *a = &actions[i];
    if (a->sector != sector || a->type != type || !a->special)
      continue;
    if (map_format.execute_line_special)
      map_format.execute_line_special(a->special, a->args, NULL, 0,
                                      activator);
    /* the special may have moved the activator out of this sector;
     * stop walking stale matches for the old sector */
    if (activator->subsector->sector != sector)
      break;
  }
  depth--;
}
