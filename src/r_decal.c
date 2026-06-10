/* r_decal.c: store and spawn placed wall decals.  See r_decal.h.
 * Rendering is a later stage; this stage only records where decals land. */

#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "m_fixed.h"
#include "tables.h"
#include "r_state.h"         /* lines, numlines        */
#include "p_maputl.h"        /* P_PointOnLineSide      */
#include "u_decaldef.h"
#include "r_decal.h"

/* A modest ring of live decals: once full, the oldest is overwritten, so
 * heavy fire never grows memory without bound (ZDoom caps decals too). */
#define MAX_DECALS 256

static placed_decal_t decal_list[MAX_DECALS];
static int            decal_count;   /* number currently stored (<= MAX)  */
static int            decal_head;    /* next slot to write (ring cursor)  */

void R_ClearDecals(void)
{
  decal_count = 0;
  decal_head  = 0;
}

void R_SpawnDecal(const line_t *li, fixed_t x, fixed_t y, fixed_t z,
                  int decalnum)
{
  const decaldef_t *def;
  placed_decal_t   *pd;
  fixed_t dx, dy;
  int side;
  unsigned flags;

  if (!li || decalnum < 0)
    return;
  def = U_DecalDef(decalnum);
  if (!def || def->texnum < 0)
    return;                            /* nothing to draw later          */

  /* Distance along the wall from v1 to the impact point, by projecting
   * (x,y)-v1 onto the wall direction.  All fixed-point: this runs in the
   * game-logic path, so no floating point.  proj = dot(P-v1, d) / |d|. */
  dx = x - li->v1->x;
  dy = y - li->v1->y;
  {
    fixed_t len = P_AproxDistance(li->dx, li->dy);
    int64_t dot;
    fixed_t proj;
    if (len < FRACUNIT)
      return;
    /* dx,dy,li->dx,li->dy are 16.16 world coords.  The along-wall offset in
     * 16.16 is dot(P-v1, d) / |d|: the two 65536^2 factors in the dot and
     * the one in |d| leave a single 65536, i.e. a fixed-point result. */
    dot  = (int64_t)dx * li->dx + (int64_t)dy * li->dy;
    proj = (fixed_t)(dot / len);
    side = P_PointOnLineSide(x, y, li);

    /* Resolve random flips once, now.  Decals are purely cosmetic, so they
     * must NOT draw from the game RNG (pr_misc) -- doing so would advance
     * the deterministic sequence and desync demos.  A private generator
     * keeps placement decorative-only. */
    flags = def->flags;
    if (flags & (DECAL_RANDFLIPX | DECAL_RANDFLIPY))
    {
      static unsigned decal_rng = 0x1234567u;
      decal_rng = decal_rng * 1103515245u + 12345u;
      if ((flags & DECAL_RANDFLIPX) && (decal_rng & 0x10000))
        flags |= DECAL_FLIPX;
      decal_rng = decal_rng * 1103515245u + 12345u;
      if ((flags & DECAL_RANDFLIPY) && (decal_rng & 0x10000))
        flags |= DECAL_FLIPY;
      flags &= ~(DECAL_RANDFLIPX | DECAL_RANDFLIPY);
    }

    pd = &decal_list[decal_head];
    pd->line   = (int)(li - lines);
    pd->side   = side;
    pd->offset = (fixed_t)proj;
    pd->z      = z;
    pd->decal  = decalnum;
    pd->flags  = flags;

    decal_head = (decal_head + 1) % MAX_DECALS;
    if (decal_count < MAX_DECALS)
      decal_count++;
  }

  /* lowerdecal: stamp the companion decal just beneath this one, once. */
  if (def->lowerdecal[0])
  {
    int lower = U_DecalNumForName(def->lowerdecal);
    const decaldef_t *ld = U_DecalDef(lower);
    if (ld && ld->texnum >= 0 && lower != decalnum)
      R_SpawnDecal(li, x, y, z - (8 << FRACBITS), lower);
  }
}

void R_SpawnDecalByName(const line_t *li, fixed_t x, fixed_t y, fixed_t z,
                        const char *name)
{
  if (name && *name)
    R_SpawnDecal(li, x, y, z, U_DecalNumForName(name));
}

int R_DecalListCount(void)
{
  return decal_count;
}

const placed_decal_t *R_DecalListEntry(int i)
{
  if (i < 0 || i >= decal_count)
    return NULL;
  return &decal_list[i];
}
