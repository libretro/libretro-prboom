/* p_vslope.c: thing-based vertex slopes (doomednum 1504/1505).
 *
 * See p_vslope.h.  This follows the reference engine's construction:
 * vertex slopes apply only to sectors with exactly three lines.  The
 * three plane points are the two endpoints of the sector's first line
 * plus the off-line endpoint of its second line.  Each point's Z is the
 * height recorded by a slope vertex thing sitting on that map vertex,
 * or -- if that vertex carries no thing -- the sector's flat floor or
 * ceiling height, so a triangle with even a single tagged vertex still
 * tilts.  A sector with none of its three vertices tagged stays flat.
 *
 * The plane is fitted in doubles at setup and stored as a secplane_t
 * (a*x + b*y + c*z + d = 0, 16.16, c > 0) -- the same representation the
 * Plane_Align path produces, queried at runtime by P_PlaneZatPoint, so
 * the renderer and physics need no changes.
 */

#include <math.h>

#include "doomstat.h"
#include "r_state.h"
#include "r_defs.h"
#include "p_maputl.h"
#include "z_zone.h"
#include "lprintf.h"
#include "p_slope.h"
#include "p_vslope.h"

typedef struct
{
  fixed_t x, y, z;
  int     which;                /* 0 floor, 1 ceiling */
} slopevert_t;

static slopevert_t *svlist  = NULL;
static int          svcount = 0;
static int          svalloc = 0;

void P_ClearSlopeVertices(void)
{
  /* plain malloc grow-array (not PU_LEVEL): survives the zone purge
   * between levels, so it must be freed explicitly. */
  free(svlist);
  svlist  = NULL;
  svcount = 0;
  svalloc = 0;
}

void P_AddSlopeVertex(fixed_t x, fixed_t y, fixed_t z, int which)
{
  if (svcount == svalloc)
  {
    int newalloc = svalloc ? svalloc * 2 : 32;
    slopevert_t *grown =
      (slopevert_t *)realloc(svlist, (size_t)newalloc * sizeof(*svlist));
    if (!grown)
      return;
    svlist  = grown;
    svalloc = newalloc;
  }
  svlist[svcount].x     = x;
  svlist[svcount].y     = y;
  svlist[svcount].z     = z;
  svlist[svcount].which = which;
  svcount++;
}

/* height of the slope vertex sitting exactly on (x,y) for the given
 * plane.  Coordinates match exactly: things and vertices come through
 * the same coordinate path, so a coincident thing lands on the
 * identical fixed_t (matching the reference engine's exact compare). */
static dbool slopevert_z(fixed_t x, fixed_t y, int which, fixed_t *out)
{
  int i;
  for (i = 0; i < svcount; i++)
    if (svlist[i].which == which && svlist[i].x == x && svlist[i].y == y)
    {
      *out = svlist[i].z;
      return TRUE;
    }
  return FALSE;
}

/* fit one plane (which: 0 floor, 1 ceiling) for a triangular sector. */
static void P_VertexSlopeTriangle(sector_t *sec, int which)
{
  const line_t *l0 = sec->lines[0];
  const line_t *l1 = sec->lines[1];
  vertex_t *p1 = l0->v1;
  vertex_t *p2 = l0->v2;
  vertex_t *p3;
  fixed_t flat;
  fixed_t z1, z2, z3;
  dbool t1, t2, t3;
  double v1x, v1y, v1z, v2x, v2y, v2z, v3x, v3y, v3z;
  double e1x, e1y, e1z, e2x, e2y, e2z;
  double nx, ny, nz, len, d;
  secplane_t *pl;

  /* third point: the endpoint of line 1 that is not shared with line 0 */
  if (l1->v1 == p1 || l1->v1 == p2)
    p3 = l1->v2;
  else
    p3 = l1->v1;

  flat = which ? sec->ceilingheight : sec->floorheight;

  /* per-vertex height: slope-vertex thing if present, else flat height.
   * Seed all three with the flat height so an untagged vertex keeps it. */
  z1 = z2 = z3 = flat;
  t1 = slopevert_z(p1->x, p1->y, which, &z1);
  t2 = slopevert_z(p2->x, p2->y, which, &z2);
  t3 = slopevert_z(p3->x, p3->y, which, &z3);
  if (!t1 && !t2 && !t3)
    return;                     /* no tagged vertex: leave flat */

  v1x = p1->x / 65536.0; v1y = p1->y / 65536.0; v1z = z1 / 65536.0;
  v2x = p2->x / 65536.0; v2y = p2->y / 65536.0; v2z = z2 / 65536.0;
  v3x = p3->x / 65536.0; v3y = p3->y / 65536.0; v3z = z3 / 65536.0;

  /* edge vectors from p3, ordered by which side of line 0 p3 lies on so
   * the cross product comes out with a consistent orientation. */
  if (P_PointOnLineSide(p3->x, p3->y, l0) == 0)
  {
    e1x = v2x - v3x; e1y = v2y - v3y; e1z = v2z - v3z;
    e2x = v1x - v3x; e2y = v1y - v3y; e2z = v1z - v3z;
  }
  else
  {
    e1x = v1x - v3x; e1y = v1y - v3y; e1z = v1z - v3z;
    e2x = v2x - v3x; e2y = v2y - v3y; e2z = v2z - v3z;
  }

  nx = e1y * e2z - e1z * e2y;
  ny = e1z * e2x - e1x * e2z;
  nz = e1x * e2y - e1y * e2x;
  len = sqrt(nx * nx + ny * ny + nz * nz);
  if (len == 0.0)
    return;                     /* all three points collinear */
  nx /= len; ny /= len; nz /= len;

  /* this core stores every secplane with c > 0 (P_PlaneZatPoint divides
   * by c and the renderer reads it that way), for floor and ceiling
   * alike -- matching the Plane_Align path. */
  if (nz < 0)
  {
    nx = -nx; ny = -ny; nz = -nz;
  }
  if ((fixed_t)(nz * FRACUNIT) <= 0)
    return;                     /* no vertical component: not a plane */

  d = -(nx * v3x + ny * v3y + nz * v3z);

  pl = (secplane_t *)Z_Malloc(sizeof(*pl), PU_LEVEL, NULL);
  pl->a = (fixed_t)(nx * FRACUNIT);
  pl->b = (fixed_t)(ny * FRACUNIT);
  pl->c = (fixed_t)(nz * FRACUNIT);
  pl->d = (fixed_t)(d  * FRACUNIT);
  if (which)
    sec->ceiling_slope = pl;
  else
    sec->floor_slope = pl;
}

void P_SpawnVertexSlopes(void)
{
  int i, f = 0, c = 0;

  if (!svcount)
    return;

  for (i = 0; i < numsectors; i++)
  {
    sector_t *sec = &sectors[i];

    if (sec->linecount != 3)    /* vertex slopes only tilt triangles */
      continue;

    if (!sec->floor_slope)
    {
      P_VertexSlopeTriangle(sec, 0);
      if (sec->floor_slope)
        f++;
    }
    if (!sec->ceiling_slope)
    {
      P_VertexSlopeTriangle(sec, 1);
      if (sec->ceiling_slope)
        c++;
    }
  }

  if (f || c)
    lprintf(LO_INFO,
            "P_SpawnVertexSlopes: %d floor, %d ceiling vertex slopes\n",
            f, c);
}
