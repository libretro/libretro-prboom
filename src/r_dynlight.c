/* r_dynlight.c: collect GLDEFS-bound point lights from the mobjs and shade
 * nearby surfaces brighter.  See r_dynlight.h. */

#include <string.h>

#include "doomstat.h"
#include "d_think.h"
#include "p_tick.h"
#include "p_mobj.h"
#include "info.h"
#include "m_fixed.h"
#include "u_dynlight.h"
#include "r_state.h"
#include "r_dynlight.h"

extern int numsprites;

#define DL_MAX_ACTIVE  128
#define DL_MAX_BOOST   200      /* light-level added at a light's centre */

typedef struct
{
  int x, y, z;                  /* map units */
  int radius;                   /* map units */
  int64_t r2;                 /* radius^2   */
  int strength;                 /* 0..DL_MAX_BOOST */
} active_light_t;

static active_light_t active[DL_MAX_ACTIVE];
static int            num_active;

void R_CollectDynLights(void)
{
  thinker_t *th;

  num_active = 0;
  if (!U_DynLightsPresent())
    return;

  for (th = thinkercap.next; th != &thinkercap; th = th->next)
  {
    const mobj_t *mo;
    const dynlight_def_t *d;

    if (th->function.arg1 != (void (*)(void *))P_MobjThinker)
      continue;
    mo = (const mobj_t *)th;
    if ((unsigned)mo->sprite >= (unsigned)numsprites)
      continue;
    d = U_DynLightForSprite(sprnames[mo->sprite]);
    if (!d || d->size <= 0)
      continue;

    if (num_active >= DL_MAX_ACTIVE)
      break;
    {
      active_light_t *a = &active[num_active++];
      a->x      = mo->x >> FRACBITS;
      a->y      = mo->y >> FRACBITS;
      a->z      = (mo->z + (mo->height >> 1)) >> FRACBITS;
      a->radius = d->size;
      a->r2     = (int64_t)d->size * d->size;
      a->strength = (int)(d->strength * DL_MAX_BOOST + 0.5f);
    }
  }
}

int R_DynLightsActive(void)
{
  return num_active;
}

int R_DynLightBoost(int wx, int wy, int wz)
{
  int i, boost = 0;

  for (i = 0; i < num_active; i++)
  {
    const active_light_t *a = &active[i];
    int dx = wx - a->x, dy = wy - a->y, dz = wz - a->z;
    int64_t d2 = (int64_t)dx * dx + (int64_t)dy * dy + (int64_t)dz * dz;
    if (d2 >= a->r2)
      continue;
    /* quadratic (1 - d^2/r^2) falloff, no sqrt */
    boost += (int)((int64_t)a->strength * (a->r2 - d2) / a->r2);
  }
  return boost;
}

int R_SegLit(const seg_t *seg)
{
  int i;
  int x1, x2, y1, y2;

  if (!num_active || !seg->v1 || !seg->v2)
    return 0;

  x1 = seg->v1->x >> FRACBITS; x2 = seg->v2->x >> FRACBITS;
  y1 = seg->v1->y >> FRACBITS; y2 = seg->v2->y >> FRACBITS;
  if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
  if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }

  for (i = 0; i < num_active; i++)
  {
    const active_light_t *a = &active[i];
    if (a->x >= x1 - a->radius && a->x <= x2 + a->radius &&
        a->y >= y1 - a->radius && a->y <= y2 + a->radius)
      return 1;
  }
  return 0;
}

int R_PlaneLit(int planez)
{
  int i;
  for (i = 0; i < num_active; i++)
  {
    int dz = planez - active[i].z;
    if (dz < 0) dz = -dz;
    if (dz < active[i].radius)
      return 1;
  }
  return 0;
}
