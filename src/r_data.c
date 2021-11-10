/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2002 by
 *  Jess Haas, Nicolas Kalkhof, Colin Phipps, Florian Schulze
 *  Copyright 2005, 2006 by
 *  Florian Schulze, Colin Phipps, Neil Stevens, Andrey Budko
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 *  02111-1307, USA.
 *
 * DESCRIPTION:
 *      Preparation of data for rendering,
 *      generation of lookups, caching, retrieval by name.
 *
 *-----------------------------------------------------------------------------*/

#include "config.h"
#include "doomstat.h"
#include "w_wad.h"
#include "r_draw.h"
#include "r_main.h"
#include "r_sky.h"
#include "i_system.h"
#include "r_bsp.h"
#include "r_things.h"
#include "p_tick.h"
#include "lprintf.h"  // jff 08/03/98 - declaration of lprintf
#include "p_tick.h"

//
// Graphics.
// DOOM graphics for walls and sprites
// is stored in vertical runs of opaque pixels (posts).
// A column is composed of zero or more posts,
// a patch or sprite is composed of zero or more columns.
//

//
// Texture definition.
// Each texture is composed of one or more patches,
// with patches being lumps stored in the WAD.
// The lumps are referenced by number, and patched
// into the rectangular texture space using origin
// and possibly other attributes.
//

typedef struct
{
  short originx;
  short originy;
  short patch;
  short stepdir;         // unused in Doom but might be used in Phase 2 Boom
  short colormap;        // unused in Doom but might be used in Phase 2 Boom
} PACKEDATTR mappatch_t;


typedef struct
{
  char       name[8];
  char       pad2[4];      // unused
  short      width;
  short      height;
  char       pad[4];       // unused in Doom but might be used in Boom Phase 2
  short      patchcount;
  mappatch_t patches[1];
} PACKEDATTR maptexture_t;

// A maptexturedef_t describes a rectangular texture, which is composed
// of one or more mappatch_t structures that arrange graphic patches.

// killough 4/17/98: make firstcolormaplump,lastcolormaplump external
int firstcolormaplump, lastcolormaplump;      // killough 4/17/98

int       firstflat, lastflat, numflats;
int       firstspritelump, lastspritelump, numspritelumps;
int       numtextures;
texture_t **textures; // proff - 04/05/2000 removed static for OpenGL
fixed_t   *textureheight; //needed for texture pegging (and TFE fix - killough)
int       *flattranslation;             // for global animation
int       *texturetranslation;

//
// R_GetTextureColumn
//

const uint8_t *R_GetTextureColumn(const rpatch_t *texpatch, int col)
{
   while (col < 0)
      col += texpatch->width;
   col &= texpatch->widthmask;

   return texpatch->columns[col].pixels;
}

//
// R_InitTextures
// Initializes the texture list
//  with the textures from the world map.
//

static void R_InitTextures (void)
{
  const maptexture_t *mtexture;
  texture_t    *texture;
  const mappatch_t   *mpatch;
  texpatch_t   *patch;
  int  i, j;
  int         maptex_lump[2] = {-1, -1};
  const int  *maptex;
  const int  *maptex1, *maptex2;
  char name[9];
  int names_lump; // cph - new wad lump handling
  const char *names; // cph -
  const char *name_p;// const*'s
  int  *patchlookup;
  int  totalwidth;
  int  nummappatches;
  int  offset;
  int  maxoff, maxoff2;
  int  numtextures1, numtextures2;
  const int *directory;
  int  errors = 0;

  // Load the patch names from pnames.lmp.
  name[8] = 0;
  names = W_CacheLumpNum(names_lump = W_GetNumForName("PNAMES"));
  nummappatches = LONG(*((const int *)names));
  name_p = names+4;
  patchlookup = Z_Malloc(nummappatches * sizeof(*patchlookup), PU_STATIC, 0);  // killough

  for (i=0 ; i<nummappatches ; i++)
    {
      strncpy (name,name_p+i*8, 8);
      patchlookup[i] = W_CheckNumForName(name);
      if (patchlookup[i] == -1)
        {
          // killough 4/17/98:
          // Some wads use sprites as wall patches, so repeat check and
          // look for sprites this time, but only if there were no wall
          // patches found. This is the same as allowing for both, except
          // that wall patches always win over sprites, even when they
          // appear first in a wad. This is a kludgy solution to the wad
          // lump namespace problem.

          patchlookup[i] = (W_CheckNumForName)(name, ns_sprites);
        }
    }
  W_UnlockLumpNum(names_lump); // cph - release the lump

  // Load the map texture definitions from textures.lmp.
  // The data is contained in one or two lumps,
  //  TEXTURE1 for shareware, plus TEXTURE2 for commercial.

  maptex = maptex1 = W_CacheLumpNum(maptex_lump[0] = W_GetNumForName("TEXTURE1"));
  numtextures1 = LONG(*maptex);
  maxoff = W_LumpLength(maptex_lump[0]);
  directory = maptex+1;

  if (W_CheckNumForName("TEXTURE2") != -1)
    {
      maptex2 = W_CacheLumpNum(maptex_lump[1] = W_GetNumForName("TEXTURE2"));
      numtextures2 = LONG(*maptex2);
      maxoff2 = W_LumpLength(maptex_lump[1]);
    }
  else
    {
      maptex2 = NULL;
      numtextures2 = 0;
      maxoff2 = 0;
    }
  numtextures = numtextures1 + numtextures2;

  textures      = Z_Malloc(numtextures * sizeof(*textures), PU_STATIC, 0);
  textureheight = Z_Malloc(numtextures * sizeof(*textureheight), PU_STATIC, 0);

  totalwidth = 0;

  for (i=0 ; i<numtextures ; i++, directory++)
    {
      if (i == numtextures1)
        {
          // Start looking in second texture file.
          maptex = maptex2;
          maxoff = maxoff2;
          directory = maptex+1;
        }

      offset = LONG(*directory);

      if (offset > maxoff)
        I_Error("R_InitTextures: Bad texture directory");

      mtexture = (const maptexture_t *) ( (const uint8_t *)maptex + offset);

      texture = textures[i] =
        Z_Malloc(sizeof(texture_t) +
                 sizeof(texpatch_t)*(SHORT(mtexture->patchcount)-1),
                 PU_STATIC, 0);

      texture->width = SHORT(mtexture->width);
      texture->height = SHORT(mtexture->height);
      texture->patchcount = SHORT(mtexture->patchcount);

      {
        unsigned j;
        for(j=0;j<sizeof(texture->name);j++)
          texture->name[j]=mtexture->name[j];
      }

      mpatch = mtexture->patches;
      patch = texture->patches;

      for (j=0 ; j<texture->patchcount ; j++, mpatch++, patch++)
        {
          patch->originx = SHORT(mpatch->originx);
          patch->originy = SHORT(mpatch->originy);
          patch->patch = patchlookup[SHORT(mpatch->patch)];
          if (patch->patch == -1)
            {
              //jff 8/3/98 use logical output routine
              lprintf(LO_ERROR,"\nR_InitTextures: Missing patch %d in texture %.8s\n",
                     SHORT(mpatch->patch), texture->name); // killough 4/17/98
              ++errors;
            }
        }

      for (j=1; j*2 <= texture->width; j<<=1)
        ;
      texture->widthmask = j-1;
      textureheight[i] = texture->height<<FRACBITS;

      totalwidth += texture->width;
    }

  Z_Free(patchlookup);         // killough

  for (i=0; i<2; i++) // cph - release the TEXTUREx lumps
    if (maptex_lump[i] != -1)
      W_UnlockLumpNum(maptex_lump[i]);

  if (errors)
    I_Error("R_InitTextures: %d errors", errors);

  if (errors)
    I_Error("R_InitTextures: %d errors", errors);

  // Create translation table for global animation.

  texturetranslation =
    Z_Malloc((numtextures+1) * sizeof(*texturetranslation), PU_STATIC, 0);

  for (i=0 ; i<numtextures ; i++)
    texturetranslation[i] = i;

  // killough 1/31/98: Initialize texture hash table
  for (i = 0; i<numtextures; i++)
    textures[i]->index = -1;
  while (--i >= 0)
    {
      int j = W_LumpNameHash(textures[i]->name) % (unsigned) numtextures;
      textures[i]->next = textures[j]->index;   // Prepend to chain
      textures[j]->index = i;
    }
}

//
// R_InitFlats
//
static void R_InitFlats(void)
{
  int i;

  firstflat = W_GetNumForName("F_START") + 1;
  lastflat  = W_GetNumForName("F_END") - 1;
  numflats  = lastflat - firstflat + 1;

  /* Create translation table for global animation. */

  flattranslation =
    Z_Malloc((numflats+1)*sizeof(*flattranslation), PU_STATIC, 0);
  for (i=0 ; i<numflats ; i++)
    flattranslation[i] = i;
}

//
// R_InitSpriteLumps
// Finds the width and hoffset of all sprites in the wad,
// so the sprite does not need to be cached completely
// just for having the header info ready during rendering.
//
static void R_InitSpriteLumps(void)
{
  firstspritelump = W_GetNumForName("S_START") + 1;
  lastspritelump = W_GetNumForName("S_END") - 1;
  numspritelumps = lastspritelump - firstspritelump + 1;
}

//
// R_InitColormaps
//
// killough 3/20/98: rewritten to allow dynamic colormaps
// and to remove unnecessary 256-byte alignment
//
// killough 4/4/98: Add support for C_START/C_END markers
//

static void R_InitColormaps(void)
{
  int i;
  firstcolormaplump = W_CheckNumForName("C_START");
  lastcolormaplump  = W_CheckNumForName("C_END");
  numcolormaps = lastcolormaplump - firstcolormaplump;
  colormaps = Z_Malloc(sizeof(*colormaps) * MAX(1,numcolormaps), PU_STATIC, 0);
  colormaps[0] = (const lighttable_t *)W_CacheLumpName("COLORMAP");
  for (i=1; i<numcolormaps; i++)
    colormaps[i] = (const lighttable_t *)W_CacheLumpNum(i+firstcolormaplump);

  if(numcolormaps == 0) {
    const lighttable_t defaultmap[1] = {1};
    colormaps[1] = defaultmap;
    numcolormaps = 2;
  }
}

// killough 4/4/98: get colormap number from name
// killough 4/11/98: changed to return -1 for illegal names
// killough 4/17/98: changed to use ns_colormaps tag

int R_ColormapNumForName(const char *name)
{
  register int i = 0;
  if (strncasecmp(name,"COLORMAP",8))     // COLORMAP predefined to return 0
    if ((i = (W_CheckNumForName)(name, ns_colormaps)) != -1)
      i -= firstcolormaplump;
  return i;
}

/*
 * R_ColourMap
 *
 * cph 2001/11/17 - unify colour maping logic in a single place; 
 *  obsoletes old c_scalelight stuff
 */

static INLINE int between(int l,int u,int x)
{
   return (l > x ? l : x > u ? u : x);
}

const lighttable_t* R_ColourMap(int lightlevel, fixed_t spryscale)
{
  if (fixedcolormap)
     return fixedcolormap;

  if (curline)
  {
     if (curline->v1->y == curline->v2->y)
        lightlevel -= 1 << LIGHTSEGSHIFT;
     else
        if (curline->v1->x == curline->v2->x)
           lightlevel += 1 << LIGHTSEGSHIFT;
  }

  lightlevel += extralight << LIGHTSEGSHIFT;

  /* cph 2001/11/17 -
   * Work out what colour map to use, remembering to clamp it to the number of
   * colour maps we actually have. This formula is basically the one from the
   * original source, just brought into one place. The main difference is it
   * throws away less precision in the lightlevel half, so it supports 32
   * light levels in WADs compared to Doom's 16.
   *
   * Note we can make it more accurate if we want - we should keep all the
   * precision until the final step, so slight scale differences can count
   * against slight light level variations.
   */
   return fullcolormap + between(0,NUMCOLORMAPS-1,
         ((256-lightlevel)*2*NUMCOLORMAPS/256) - 4
         - (FixedMul(spryscale,pspriteiscale)/2 >> LIGHTSCALESHIFT)
         )*256;
}

//
// R_InitData
// Locates all the lumps
//  that will be used by all views
// Must be called after W_Init.
//

void R_InitData(void)
{
  lprintf(LO_INFO, "Textures\n");
  R_InitTextures();
  lprintf(LO_INFO, "Flats\n");
  R_InitFlats();
  lprintf(LO_INFO, "Sprites\n");
  R_InitSpriteLumps();
  lprintf(LO_INFO, "Colormaps\n");
  R_InitColormaps();                    // killough 3/20/98
}

//
// R_FlatNumForName
// Retrieval, get a flat number for a flat name.
//
// killough 4/17/98: changed to use ns_flats namespace
//

int R_FlatNumForName(const char *name)    // killough -- const added
{
  int i = (W_CheckNumForName)(name, ns_flats);
  if (i == -1)
    I_Error("R_FlatNumForName: %.8s not found", name);
  return i - firstflat;
}

//
// R_CheckTextureNumForName
// Check whether texture is available.
// Filter out NoTexture indicator.
//
// Rewritten by Lee Killough to use hash table for fast lookup. Considerably
// reduces the time needed to start new levels. See w_wad.c for comments on
// the hashing algorithm, which is also used for lump searches.
//
// killough 1/21/98, 1/31/98
//

int R_CheckTextureNumForName(const char *name)
{
  int i = NO_TEXTURE;
  if (*name != '-')     // "NoTexture" marker.
    {
      i = textures[W_LumpNameHash(name) % (unsigned) numtextures]->index;
      while (i >= 0 && strncasecmp(textures[i]->name,name,8))
        i = textures[i]->next;
    }
  return i;
}

//
// R_TextureNumForName
// Calls R_CheckTextureNumForName,
//  aborts with error message.
//

int R_TextureNumForName(const char *name)  // const added -- killough
{
  int i = R_CheckTextureNumForName(name);
  if (i == -1)
    I_Error("R_TextureNumForName: %.8s not found", name);
  return i;
}

//
// R_SafeTextureNumForName
// Calls R_CheckTextureNumForName, and changes any error to NO_TEXTURE
int R_SafeTextureNumForName(const char *name, int snum)
{
  int i = R_CheckTextureNumForName(name);
  if (i == -1) {
    i = NO_TEXTURE; // e6y - return "no texture"
    lprintf(LO_DEBUG,"bad texture '%s' in sidedef %d\n",name,snum);
  }
  return i;
}

//
// R_PrecacheLevel
// Preloads all relevant graphics for the level.
//
// Totally rewritten by Lee Killough to use less memory,
// to avoid using alloca(), and to improve performance.
// cph - new wad lump handling, calls cache functions but acquires no locks

static INLINE void precache_lump(int l)
{
  W_CacheLumpNum(l); W_UnlockLumpNum(l);
}

void R_PrecacheLevel(void)
{
  register int i;
  register uint8_t *hitlist;

  if (demoplayback)
    return;

  {
    size_t size = numflats > numsprites  ? numflats : numsprites;
    hitlist = Z_Malloc(((size_t)numtextures > size) ? (size_t)numtextures : size, PU_STATIC, 0);
  }

  // Precache flats.

  memset(hitlist, 0, numflats);

  for (i = numsectors; --i >= 0; )
    hitlist[sectors[i].floorpic] = hitlist[sectors[i].ceilingpic] = 1;

  for (i = numflats; --i >= 0; )
    if (hitlist[i])
      precache_lump(firstflat + i);

  // Precache textures.

  memset(hitlist, 0, numtextures);

  for (i = numsides; --i >= 0;)
    hitlist[sides[i].bottomtexture] =
      hitlist[sides[i].toptexture] =
      hitlist[sides[i].midtexture] = 1;

  // Sky texture is always present.
  // Note that F_SKY1 is the name used to
  //  indicate a sky floor/ceiling as a flat,
  //  while the sky texture is stored like
  //  a wall texture, with an episode dependend
  //  name.

  hitlist[skytexture] = 1;

  for (i = numtextures; --i >= 0; )
    if (hitlist[i])
      {
        texture_t *texture = textures[i];
        int j = texture->patchcount;
        while (--j >= 0)
          precache_lump(texture->patches[j].patch);
      }

  // Precache sprites.
  memset(hitlist, 0, numsprites);

  {
    thinker_t *th = NULL;
    while ((th = P_NextThinker(th,th_all)) != NULL)
      if (th->function == P_MobjThinker)
        hitlist[((mobj_t *)th)->sprite] = 1;
  }

  for (i=numsprites; --i >= 0;)
    if (hitlist[i])
      {
        int j = sprites[i].numframes;
        while (--j >= 0)
          {
            short *sflump = sprites[i].spriteframes[j].lump;
            int k = 7;
            do
              precache_lump(firstspritelump + sflump[k]);
            while (--k >= 0);
          }
      }
  Z_Free(hitlist);
}

// Proff - Added for OpenGL
void R_SetPatchNum(patchnum_t *patchnum, const char *name)
{
  const rpatch_t *patch = R_CachePatchName(name);
  if (!patch)
  {
    I_Error("R_SetPatchNum: cannot load patch '%s'", name);
    return;
  }

  patchnum->width = patch->width;
  patchnum->height = patch->height;
  patchnum->leftoffset = patch->leftoffset;
  patchnum->topoffset = patch->topoffset;
  patchnum->lumpnum = W_GetNumForName(name);
  R_UnlockPatchName(name);
}
