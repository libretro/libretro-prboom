/* u_decaldef.h: ZDoom DECALDEF definitions for the software renderer.
 *
 * A DECALDEF lump describes wall decals -- the small textures (bullet
 * chips, blood splats, scorch marks) that get stamped onto walls when a
 * shot or projectile impacts them:
 *
 *     decal NailBloodSplat1
 *     {
 *         pic BSPLAT1
 *         shade "BloodDefault"
 *         x-scale 0.75
 *         y-scale 0.75
 *         randomflipx
 *         randomflipy
 *     }
 *
 *     decalgroup NailBloodSplat   // weighted random pick among decals
 *     {
 *         NailBloodSplat1 2
 *         NailBloodSplat3 5
 *         ...
 *     }
 *
 * This module only parses the definitions and resolves each pic to a
 * texture; placing decals on walls when something hits them, and drawing
 * them, are later stages.  Faders / stretchers / combiners / animators
 * (decal animation) are parsed past but not yet acted on. */

#ifndef U_DECALDEF_H
#define U_DECALDEF_H

#include "doomtype.h"
#include "m_fixed.h"

/* render-style flags on a decal */
enum
{
  DECAL_FLIPX      = 0x01,   /* always flipped horizontally        */
  DECAL_FLIPY      = 0x02,   /* always flipped vertically          */
  DECAL_RANDFLIPX  = 0x04,   /* randomly flipped horizontally      */
  DECAL_RANDFLIPY  = 0x08,   /* randomly flipped vertically        */
  DECAL_FULLBRIGHT = 0x10,   /* ignore sector light                */
  DECAL_ADD        = 0x20    /* additive blend                     */
};

typedef struct
{
  char    name[64];          /* decal name (binding key)           */
  int     texnum;            /* resolved pic; texture# or patch lump */
  int     pic_is_patch;      /* 0: texnum is a wall texture; 1: a patch lump */
  fixed_t xscale, yscale;    /* 16.16; default 1.0                 */
  fixed_t alpha;             /* 16.16; default 1.0                 */
  unsigned flags;
  int     has_shade;         /* 1 if a shade colour was specified  */
  int     shade_r, shade_g, shade_b;  /* shade target colour, 0..255 */
  int     shade_trans;       /* built luminance->shade table id, or -1 */
  char    lowerdecal[64];    /* spawned beneath this one, or empty */
} decaldef_t;

/* A decalgroup: weighted list of member decal indices. */
typedef struct
{
  char  name[64];
  int  *members;             /* indices into the decal table       */
  int  *weights;
  int   count;
  int   total_weight;
} decalgroup_t;

/* Parse the DECALDEF lump(s).  Safe to call once after the wad and the
 * texture list are ready; a second call is a no-op. */
void U_ScanDecalPics(void);  /* pre-scan pic names (before PNG materialise) */
int  U_IsDecalPic(const char *name);
void U_LoadDecalDefs(void);

/* Look up a decal or group by name (case-insensitive).  Returns an index
 * into the decal table, resolving a group name to one of its members by
 * weighted random pick.  -1 if unknown. */
int  U_DecalNumForName(const char *name);

/* Direct accessors. */
const decaldef_t *U_DecalDef(int idx);
int  U_DecalCount(void);
void U_FreeDecalDefs(void);

#endif
