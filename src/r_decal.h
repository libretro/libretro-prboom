/* r_decal.h: placed wall decals.
 *
 * A "placed decal" is one live instance stamped on a wall: which line and
 * side it sits on, where along the wall (offset from v1) and at what
 * height, which DECALDEF entry supplies the texture and render flags, and
 * the resolved flip state (randomflipx/y are decided once, at spawn).
 *
 * This stage stores and spawns decals; drawing them is the next stage. */

#ifndef R_DECAL_H
#define R_DECAL_H

#include "doomtype.h"
#include "m_fixed.h"
#include "r_defs.h"

typedef struct
{
  int      line;             /* index into lines[]                     */
  int      side;             /* 0 = front side, 1 = back side          */
  fixed_t  offset;           /* distance along the wall from v1        */
  fixed_t  z;                /* world height of the decal's centre     */
  int      decal;            /* index into the DECALDEF table          */
  unsigned flags;            /* resolved DECAL_* (randomflip decided)  */
} placed_decal_t;

/* Stamp a decal onto the wall the trace hit.  (x,y,z) is the impact point;
 * li is the line; decalnum indexes the DECALDEF table (already resolved
 * from a name/group by the caller).  No-op if decalnum < 0 or the def has
 * no texture. */
void R_SpawnDecal(const line_t *li, fixed_t x, fixed_t y, fixed_t z,
                  int decalnum);

/* Spawn by decal/group name (convenience for callers that have a name,
 * e.g. a DECORATE "Decal" property). */
void R_SpawnDecalByName(const line_t *li, fixed_t x, fixed_t y, fixed_t z,
                        const char *name);

/* Discard all placed decals (called at level setup). */
void R_ClearDecals(void);

/* Accessors for the renderer (next stage). */
int                    R_DecalListCount(void);
const placed_decal_t  *R_DecalListEntry(int i);

#endif

/* Render all placed decals that fall on this drawseg's wall, for screen
 * columns in [x1, x2].  Called both interleaved from R_DrawSprite (when the
 * seg is behind a sprite, so the sprite then overdraws the decal -> correct
 * occlusion) and from the final masked pass for whatever columns no sprite
 * covered.  A per-column frame guard ensures each column is drawn at most
 * once, so the interleave adds no overdraw. */
struct drawseg_s;
void R_DrawDecalsForSeg(struct drawseg_s *ds, int x1, int x2);

/* Reset the per-column decal draw guard at the start of a frame's masked
 * pass, before any decals are rendered. */
void R_DecalsBeginFrame(void);
