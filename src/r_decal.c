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

/* ---- rendering ---------------------------------------------------------
 * Project placed decals onto their wall during the masked pass.  Modelled
 * on R_RenderMaskedSegRange: walk the drawseg's screen columns, and for
 * the columns the decal covers, draw its texture column at the decal's
 * height and scale, clipped to the wall's sprite clip arrays. */

#include <limits.h>
#include "tables.h"
#include "r_main.h"
#include "r_things.h"
#include "r_draw.h"
#include "r_patch.h"
#include "r_data.h"
#include "r_bsp.h"
#include "w_wad.h"

extern angle_t xtoviewangle[];

void R_DrawDecalsForSeg(struct drawseg_s *ds_)
{
  drawseg_t       *ds  = (drawseg_t *)ds_;
  seg_t           *seg = ds->curline;
  const line_t    *ld;
  int              lineidx, segside;
  int              i;
  R_DrawColumn_f   colfunc;
  draw_column_vars_t dcvars;

  if (!seg || !seg->linedef || !seg->sidedef)
    return;
  ld      = seg->linedef;
  lineidx = (int)(ld - lines);
  /* seg side: 0 if this seg runs along the front side, else 1 */
  segside = seg->sidedef == &sides[ld->sidenum[0]] ? 0 : 1;

  colfunc = R_GetDrawColumnFunc(RDC_PIPELINE_STANDARD,
                                drawvars.filterwall, drawvars.filterz);
  R_SetDefaultDrawColumnVars(&dcvars);

  for (i = 0; i < decal_count; i++)
  {
    const placed_decal_t *pd = &decal_list[i];
    const decaldef_t     *def;
    const rpatch_t       *patch;
    fixed_t  segu0, u_left, u_right, dwidth, dheight;
    fixed_t  colscale, rw_scalestep;
    int      x;

    if (pd->line != lineidx || pd->side != segside)
      continue;
    def = U_DecalDef(pd->decal);
    if (!def || def->texnum < 0)
      continue;

    patch   = def->pic_is_patch
                ? R_CachePatchNum(def->texnum)
                : R_CacheTextureCompositePatchNum(def->texnum);
    dwidth  = FixedMul(patch->width  << FRACBITS, def->xscale);
    dheight = FixedMul(patch->height << FRACBITS, def->yscale);

    /* Decal u-range relative to this seg's start.  pd->offset is measured
     * from the linedef v1; the seg starts seg->offset further along, so
     * subtract it to get seg-relative along-wall coordinates, matching the
     * column texu (which also carries the sidedef textureoffset). */
    segu0   = pd->offset - seg->offset + seg->sidedef->textureoffset;
    u_left  = segu0 - (dwidth >> 1);
    u_right = segu0 + (dwidth >> 1);

    rw_scalestep = ds->scalestep;
    colscale     = ds->scale1;

    for (x = ds->x1; x <= ds->x2; x++, colscale += rw_scalestep)
    {
      angle_t angle = (ds->rw_centerangle + xtoviewangle[x]) >> ANGLETOFINESHIFT;
      fixed_t texu  = ds->rw_offset - FixedMul(finetangent[angle], ds->rw_distance);
      fixed_t tx;
      int     col;
      int64_t t;

      if (texu < u_left || texu >= u_right)
        continue;                              /* column not under decal   */

      /* texture column within the decal */
      tx  = FixedDiv(texu - u_left, def->xscale);
      col = (tx >> FRACBITS);
      if (def->flags & DECAL_FLIPX)
        col = patch->width - 1 - col;
      if (col < 0 || col >= patch->width)
        continue;

      /* vertical placement: decal centre at pd->z, half-height up/down */
      {
        fixed_t vscale = FixedMul(colscale, def->yscale);
        if (vscale <= 0)
          continue;
        dcvars.texturemid = (pd->z + (dheight >> 1)) - viewz;
        dcvars.iscale     = 0xffffffffu / (unsigned)vscale;

        t = ((int64_t)centeryfrac << FRACBITS)
            - (int64_t)dcvars.texturemid * vscale;
        if (t > ((int64_t)MAX_SCREENHEIGHT << (FRACBITS * 2)))
          continue;
        sprtopscreen = (long)(t >> FRACBITS);
        /* R_DrawMaskedColumn positions posts with the global spryscale, so
         * it must carry the decal's vertical scale for this column. */
        spryscale = vscale;
      }

      dcvars.x        = x;
      dcvars.colormap = (def->flags & DECAL_FULLBRIGHT)
                          ? fullcolormap
                          : R_ColourMap(seg->frontsector
                                          ? seg->frontsector->lightlevel : 0,
                                        colscale);
      dcvars.nextcolormap = dcvars.colormap;
      dcvars.z        = colscale;

      mfloorclip   = screenheightarray;
      mceilingclip = negonearray;

      R_DrawMaskedColumn(patch, colfunc, &dcvars,
                         R_GetPatchColumnClamped(patch, col),
                         R_GetPatchColumnClamped(patch, col - 1),
                         R_GetPatchColumnClamped(patch, col + 1));
    }

    if (def->pic_is_patch)
      R_UnlockPatchNum(def->texnum);
    else
      R_UnlockTextureCompositePatchNum(def->texnum);
  }
}
