/* r_voxel.h: software rasteriser for voxel vissprites.
 *
 * R_ProjectSprite emits a voxel vissprite (vis->voxel != NULL) for any
 * thing whose sprite is bound to a KVX model; R_DrawVisSprite hands it
 * here.  The projection/placement lives in r_things.c with the other
 * vissprite setup; the model-space rotation and column rasterisation live
 * here. */

#ifndef R_VOXEL_H
#define R_VOXEL_H

#include "r_defs.h"

/* Draw one voxel vissprite into the current view. */
void R_DrawVoxel(vissprite_t *vis);

#endif
