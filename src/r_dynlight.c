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
  int cr, cg, cb;               /* chroma (colour minus grey) in 565 channels */
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
      int     rad = d->size2 > d->size ? d->size2 : d->size; /* max reach */
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
      unsigned seed = (unsigned)(mo->x >> FRACBITS) * 73856093u
                    ^ (unsigned)(mo->y >> FRACBITS) * 19349663u;
      int rad = U_DynLightRadius(d, leveltime, seed);
      if (rad < 1) rad = 1;
      a->x      = mo->x >> FRACBITS;
      a->y      = mo->y >> FRACBITS;
      a->z      = (mo->z + (mo->height >> 1)) >> FRACBITS;
      a->radius = rad;
      a->r2     = (int64_t)rad * rad;
      a->strength = (int)(d->strength * DL_MAX_BOOST + 0.5f);
      /* Chroma = colour minus its grey (min) component, in 565 channel units.
       * A white/grey light has zero chroma, so it adds no tint and leaves the
       * output identical to the luma-only path; only saturated lights tint. */
      {
        float mn = d->r < d->g ? (d->r < d->b ? d->r : d->b)
                               : (d->g < d->b ? d->g : d->b);
        a->cr = (int)((d->r - mn) * 31.0f + 0.5f);
        a->cg = (int)((d->g - mn) * 63.0f + 0.5f);
        a->cb = (int)((d->b - mn) * 31.0f + 0.5f);
      }
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
  dl_tint_r = dl_tint_g = dl_tint_b = 0;

  for (i = 0; i < num_active; i++)
  {
    const active_light_t *a = &active[i];
    int dx = wx - a->x, dy = wy - a->y, dz = wz - a->z;
    int64_t d2 = (int64_t)dx * dx + (int64_t)dy * dy + (int64_t)dz * dz;
    int b;
    if (d2 >= a->r2)
      continue;
    /* quadratic (1 - d^2/r^2) falloff, no sqrt */
    b = (int)((int64_t)a->strength * (a->r2 - d2) / a->r2);
    boost += b;
    if (a->cr | a->cg | a->cb)
    {
      dl_tint_r += b * a->cr;
      dl_tint_g += b * a->cg;
      dl_tint_b += b * a->cb;
    }
  }
  return boost;
}

/* Per-seg light sublist.  A light whose 2D distance to the wall segment
 * exceeds its radius is beyond reach at every point on the seg (the wall
 * point's (x,y) lies on the seg line), so it contributes zero to every column
 * and can be dropped up front -- the vertical per-band boost then loops only
 * the lights that actually touch this wall.  Exact: filtering by point-to-
 * segment distance changes nothing the full loop would have added. */
static active_light_t seg_lights[DL_MAX_ACTIVE];
static int            num_seg_lights;

int R_SegPrepareLights(const seg_t *seg)
{
  int i, ax, ay, bx, by;
  int64_t abx, aby, len2;

  num_seg_lights = 0;
  if (!num_active || !seg->v1 || !seg->v2)
    return 0;

  ax = seg->v1->x >> FRACBITS; ay = seg->v1->y >> FRACBITS;
  bx = seg->v2->x >> FRACBITS; by = seg->v2->y >> FRACBITS;
  abx = bx - ax; aby = by - ay;
  len2 = abx * abx + aby * aby;

  for (i = 0; i < num_active; i++)
  {
    const active_light_t *a = &active[i];
    int64_t apx = a->x - ax, apy = a->y - ay;
    int64_t dot = apx * abx + apy * aby;
    int64_t d2;

    if (len2 == 0 || dot <= 0)
      d2 = apx * apx + apy * apy;                 /* closest point is v1 */
    else if (dot >= len2)
    {
      int64_t bpx = a->x - bx, bpy = a->y - by;
      d2 = bpx * bpx + bpy * bpy;                 /* closest point is v2 */
    }
    else
      /* perpendicular foot inside the seg; round the subtracted term up so
       * the distance is never overestimated (never a false exclusion). */
      d2 = (apx * apx + apy * apy) - (dot * dot + len2 - 1) / len2;

    if (d2 < a->r2)
      seg_lights[num_seg_lights++] = *a;
  }
  return num_seg_lights;
}

/* Per-column wall lighting.  A wall column has a fixed world (x,y), so filter
 * the seg's lights by horizontal reach once per column (dropping any the
 * column is out of range of) and fold the horizontal distance into a vertical
 * reach r_eff^2 = r^2 - hd^2.  The per-band boost is then a cheap 1D vertical
 * query over only the reaching lights -- algebraically identical to the full
 * 3D R_DynLightBoost, but a fraction of the work on tall walls. */
/* scale = (strength << DL_SCALE_SHIFT) / r2, precomputed per column so the
 * per-band boost is a multiply-shift instead of a divide (the divide was the
 * hot loop's dominant cost on tall walls). */
#define DL_SCALE_SHIFT 20
typedef struct
{
  int       lz;
  long long reff2, scale;
  int       cr, cg, cb;
} seg_col_light_t;

static seg_col_light_t seg_col_lights[DL_MAX_ACTIVE];
static int             num_seg_col_lights;

int R_SegColumnPrepare(int wx, int wy)
{
  int i;
  num_seg_col_lights = 0;
  for (i = 0; i < num_seg_lights; i++)
  {
    const active_light_t *a = &seg_lights[i];
    long long dx = wx - a->x, dy = wy - a->y;
    long long hd2 = dx * dx + dy * dy;
    seg_col_light_t *c;
    if (hd2 >= a->r2)
      continue;                          /* column out of this light's reach */
    c = &seg_col_lights[num_seg_col_lights++];
    c->lz = a->z;
    c->reff2 = a->r2 - hd2;
    c->scale = ((long long)a->strength << DL_SCALE_SHIFT) / a->r2;
    c->cr = a->cr; c->cg = a->cg; c->cb = a->cb;
  }
  return num_seg_col_lights;
}

int R_SegColumnBoost(int wz)
{
  int i, boost = 0;
  dl_tint_r = dl_tint_g = dl_tint_b = 0;
  for (i = 0; i < num_seg_col_lights; i++)
  {
    const seg_col_light_t *c = &seg_col_lights[i];
    long long dz = wz - c->lz;
    long long dz2 = dz * dz;
    int b;
    if (dz2 >= c->reff2)
      continue;
    b = (int)(((c->reff2 - dz2) * c->scale) >> DL_SCALE_SHIFT);
    boost += b;
    if (c->cr | c->cg | c->cb)
    {
      dl_tint_r += b * c->cr;
      dl_tint_g += b * c->cg;
      dl_tint_b += b * c->cb;
    }
  }
  return boost;
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
  int       cr, cg, cb;         /* chroma, carried from the active light */
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
    p->cr = a->cr; p->cg = a->cg; p->cb = a->cb;
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

/* Boost-weighted chroma accumulated by the most recent R_PlaneBoost call, in
 * 565 channel units scaled by boost (the caller shifts down to a per-pixel
 * additive tint).  Stays zero when every contributing light is white. */
int dl_tint_r, dl_tint_g, dl_tint_b;
/* Frontend toggle (prboom-dynlight_wall_falloff): 0 = single boost per wall
 * column (fast, default), 1 = per-band vertical light pool on walls. */
int dynlight_wall_falloff = 0;

/* Per-row plane lighting.  A span row's world points lie on the straight
 * segment (a,b) (planar mapping is linear along the row), so filter the
 * plane's lights by exact point-to-segment distance once per span and give
 * the per-chunk boost only the lights that can reach the row (the same
 * arithmetic as the full loop, just over far fewer lights).  A small margin on
 * the filter covers the integer truncation of the endpoint/mid world
 * coordinates, so the chunk-level (d2 >= reff2) test still decides
 * contribution and the output is bit-identical to the unfiltered loop. */
#define DL_ROW_MARGIN 4
typedef struct
{
  int       x, y;
  long long reff2, r2;
  int       strength, cr, cg, cb;
} row_light_t;

static row_light_t row_lights[DL_MAX_ACTIVE];
static int         num_row_lights;

int R_PlaneRowPrepare(int ax, int ay, int bx, int by)
{
  int i;
  long long abx = (long long)bx - ax, aby = (long long)by - ay;
  long long den = abx * abx + aby * aby;
  num_row_lights = 0;
  for (i = 0; i < num_plane_lights; i++)
  {
    const plane_light_t *p = &plane_lights[i];
    long long apx = (long long)p->x - ax, apy = (long long)p->y - ay;
    long long num = apx * abx + apy * aby;
    long long d2;
    row_light_t *r;
    if (num <= 0 || den == 0)
      d2 = apx * apx + apy * apy;
    else if (num >= den)
    {
      long long bpx = (long long)p->x - bx, bpy = (long long)p->y - by;
      d2 = bpx * bpx + bpy * bpy;
    }
    else
      d2 = apx * apx + apy * apy - (num / den) * num
           - ((num % den) * num) / den;      /* |AP|^2 - num^2/den, no overflow */
    {
      long long margin = p->reff + DL_ROW_MARGIN;
      if (d2 >= margin * margin)
        continue;
    }
    r = &row_lights[num_row_lights++];
    r->x = p->x; r->y = p->y;
    r->reff2 = p->reff2;
    r->r2 = p->r2;
    r->strength = p->strength;
    r->cr = p->cr; r->cg = p->cg; r->cb = p->cb;
  }
  return num_row_lights;
}

int R_PlaneRowBoost(int wx, int wy)
{
  int i, boost = 0;
  dl_tint_r = dl_tint_g = dl_tint_b = 0;
  for (i = 0; i < num_row_lights; i++)
  {
    const row_light_t *r = &row_lights[i];
    int dx = wx - r->x, dy = wy - r->y;
    long long d2 = (long long)dx * dx + (long long)dy * dy;
    int b;
    if (d2 >= r->reff2)
      continue;
    b = (int)((long long)r->strength * (r->reff2 - d2) / r->r2);
    boost += b;
    if (r->cr | r->cg | r->cb)
    {
      dl_tint_r += b * r->cr;
      dl_tint_g += b * r->cg;
      dl_tint_b += b * r->cb;
    }
  }
  return boost;
}

