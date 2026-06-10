/* r_voxel.c: software voxel rasteriser.
 *
 * Stage 2 wires the voxel vissprite through the renderer: R_ProjectSprite
 * places and bounds a voxel model and R_DrawVisSprite routes it here.  The
 * model-space rotation and per-column rasterisation are added in a later
 * stage; for now this is a no-op so voxel things are detected and sorted
 * without being drawn (and without disturbing the rest of the scene). */

#include "doomtype.h"
#include "r_defs.h"
#include "r_main.h"
#include "u_voxel.h"
#include "r_voxel.h"

void R_DrawVoxel(vissprite_t *vis)
{
  const voxel_model_t *vox = (const voxel_model_t *)vis->voxel;
  if (!vox)
    return;

  /* Rasterisation lands in the next stage.  Nothing is drawn yet. */
  (void)vis;
}
