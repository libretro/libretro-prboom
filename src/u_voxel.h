/* u_voxel.h: KVX voxel models and ZDoom VOXELDEF bindings for the
 * software renderer.
 *
 * ZDoom packs can replace a sprite with a 3D voxel model (Ken Silverman's
 * Build-engine KVX format).  A VOXELDEF lump binds a voxel lump to a
 * sprite name:
 *
 *     vtola = "vtola" { OverridePalette ... }
 *
 * so any thing whose sprite frame is VTOL* draws the voxel instead of the
 * billboard.  This module only loads the KVX models and resolves the
 * bindings; projecting and rasterising them is done elsewhere.
 *
 * A KVX file is: int32 numbytes; int32 xsiz,ysiz,zsiz; int32 pivot x,y,z
 * (8.8 fixed); int32 xoffset[xsiz+1]; int16 xyoffset[xsiz][ysiz+1]; the
 * voxel column data; then a trailing 768-byte 6-bit VGA palette.  Each
 * (x,y) column is a list of slabs { ztop, zleng, cullflags, col[zleng] }
 * describing vertical runs of coloured voxels. */

#ifndef U_VOXEL_H
#define U_VOXEL_H

#include "doomtype.h"

/* One vertical run of voxels in a column. */
typedef struct
{
  unsigned char ztop;       /* first voxel's z (top)                      */
  unsigned char zleng;      /* number of voxels in the run                */
  unsigned char cullflags;  /* KVX face-visibility bits (0..63)           */
  unsigned char pad;
  const unsigned char *col; /* zleng palette indices, into the model data */
} voxslab_t;

/* A column (x,y): a contiguous span of slabs in the model's slab array. */
typedef struct
{
  int first;                /* index of the first slab                    */
  int count;                /* number of slabs                            */
} voxcolumn_t;

typedef struct
{
  char       name[9];       /* voxel lump / binding name                  */
  int        xsiz, ysiz, zsiz;
  fixed_t    xpiv, ypiv, zpiv;  /* pivot, 16.16 (converted from KVX 8.8)  */

  voxcolumn_t *columns;     /* xsiz*ysiz, indexed [x*ysiz + y]            */
  voxslab_t   *slabs;       /* all slabs, referenced by the columns       */
  int          numslabs;

  unsigned char *voxdata;   /* owned copy the slab col pointers point into */
  unsigned char  palette[768]; /* model's own 6-bit palette (KVX trailing)*/
  unsigned char  pal_remap[256]; /* palette index -> PLAYPAL index         */
  dbool          override_palette; /* VOXELDEF OverridePalette flag        */
} voxel_model_t;

/* Parse VOXELDEF and load every bound KVX model.  Safe to call once after
 * the wad is loaded and PLAYPAL exists; a second call is a no-op. */
void U_LoadVoxels(void);

/* The voxel model bound to a sprite number, or NULL.  spritenum is an
 * index into the sprites[] table (sprnum), matching mobj->sprite. */
const voxel_model_t *U_VoxelForSprite(int spritenum);

int  U_VoxelCount(void);
void U_FreeVoxels(void);

#endif
