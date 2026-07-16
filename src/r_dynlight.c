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
#include "r_main.h"
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

    /* View-frustum cull: a light that cannot reach anything on screen is
     * dead weight in every per-primitive query below.  Transform the light
     * to view space and drop it when its whole sphere is behind the camera
     * or well outside the horizontal FOV cone (generous half-angle so a
     * light grazing the edge, whose radius may still touch a visible
     * surface, is kept). */
    {
      fixed_t trx = mo->x - viewx, trY = mo->y - viewy;
      fixed_t tz  = FixedMul(trx, viewcos) + FixedMul(trY, viewsin);
      int     rad = d->size;
      if ((tz >> FRACBITS) < -rad)
        continue;                       /* entire sphere behind the view */
      if (tz > 0)
      {
        fixed_t tx  = FixedMul(trx, viewsin) - FixedMul(trY, viewcos);
        int     txm = tx >> FRACBITS, tzm = tz >> FRACBITS;
        if (txm < 0) txm = -txm;
        /* Conservative: only cull when the light's whole sphere clears the
         * FOV wedge.  The 2*tz wedge (~63 deg half-angle) is wider than any
         * normal FOV, and the 2*rad margin covers the sphere's reach toward
         * the frustum edge, so an edge-grazing light is never dropped. */
        if (txm - 2 * rad > 2 * tzm)
          continue;
      }
    }

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

/* Per-plane light sublist.  A flat has a constant world z, so a light's
 * vertical term (planez - lz)^2 is constant across the whole plane: fold it
 * into an effective 2D radius r_eff^2 = r^2 - dz^2 once, drop lights that do
 * not reach the plane, and the per-span work becomes a cheap 2D query over
 * just the reaching lights. */
typedef struct
{
  int       x, y, reff;
  long long reff2, r2;
  int       strength;
} plane_light_t;

static plane_light_t plane_lights[DL_MAX_ACTIVE];
static int           num_plane_lights;

int R_PlanePrepareLights(int planez)
{
  int i;
  num_plane_lights = 0;
  for (i = 0; i < num_active; i++)
  {
    const active_light_t *a = &active[i];
    long long dz  = planez - a->z;
    long long dz2 = dz * dz;
    plane_light_t *p;
    long long lo, hi;

    if (dz2 >= a->r2)
      continue;                         /* light never reaches this plane */
    p = &plane_lights[num_plane_lights++];
    p->x = a->x; p->y = a->y;
    p->reff2 = a->r2 - dz2;
    p->r2 = a->r2;
    p->strength = a->strength;
    /* integer sqrt of reff2 for the AABB span test */
    lo = 0; hi = a->radius;
    while (lo < hi)
    {
      long long m = (lo + hi + 1) >> 1;
      if (m * m <= p->reff2) lo = m; else hi = m - 1;
    }
    p->reff = (int)lo;
  }
  return num_plane_lights;
}

int R_PlaneBoost(int wx, int wy)
{
  int i, boost = 0;
  for (i = 0; i < num_plane_lights; i++)
  {
    const plane_light_t *p = &plane_lights[i];
    int dx = wx - p->x, dy = wy - p->y;
    long long d2 = (long long)dx * dx + (long long)dy * dy;
    if (d2 >= p->reff2)
      continue;
    boost += (int)((long long)p->strength * (p->reff2 - d2) / p->r2);
  }
  return boost;
}

int R_PlaneSpanLit(int wx1, int wy1, int wx2, int wy2)
{
  int i;
  int lox = wx1 < wx2 ? wx1 : wx2, hix = wx1 < wx2 ? wx2 : wx1;
  int loy = wy1 < wy2 ? wy1 : wy2, hiy = wy1 < wy2 ? wy2 : wy1;
  for (i = 0; i < num_plane_lights; i++)
  {
    const plane_light_t *p = &plane_lights[i];
    if (p->x >= lox - p->reff && p->x <= hix + p->reff &&
        p->y >= loy - p->reff && p->y <= hiy + p->reff)
      return 1;
  }
  return 0;
}
