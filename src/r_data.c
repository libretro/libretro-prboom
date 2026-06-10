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
#include "u_png.h"
#include "u_ztextures.h"
#include "u_brightmap.h"
#include "u_decaldef.h"
#include "r_draw.h"
#include "v_video.h"
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
texture_t **textures;
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

/* Resolve a ZDoom TEXTURES patch name to a lump that is a usable Doom patch,
 * preferring the TX namespace then the global one.  Fills the pw and ph
 * outputs with the patch's pixel dimensions when non-NULL.  Returns -1 if the
 * name does not resolve to a valid patch (e.g. an undecoded modern lump). */
static int zt_resolve_patch(const char *name, int *pw, int *ph)
{
  int lp = (W_CheckNumForName)(name, ns_zdoom_tx);
  if (lp < 0)
    lp = (W_CheckNumForName)(name, ns_global);
  if (lp < 0 || W_LumpLength(lp) < 8)
    return -1;
  {
    const unsigned char *hdr = W_CacheLumpNum(lp);
    int w = (short)(hdr[0] | (hdr[1] << 8));
    int h = (short)(hdr[2] | (hdr[3] << 8));
    W_UnlockLumpNum(lp);
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096)
      return -1;
    if (pw) *pw = w;
    if (ph) *ph = h;
  }
  return lp;
}

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
  int  numflattex;
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

  /* ZDoom-targeted wads put flats on walls (chex3.wad references its
   * CJFCOMM* flats from over 800 sidedefs); reserve slots so each flat
   * name not shadowed by a TEXTUREx entry becomes a 64x64 wall texture */
  numflattex = 0;
  for (i = 0; i < numlumps; i++)
    if ((lumpinfo[i].li_namespace == ns_flats && W_LumpLength(i) >= 64 * 64)
        || (lumpinfo[i].li_namespace == ns_zdoom_tx && W_LumpLength(i) >= 8))
      numflattex++;

  textures      = Z_Malloc((numtextures + numflattex + num_ztextures) * sizeof(*textures), PU_STATIC, 0);
  textureheight = Z_Malloc((numtextures + numflattex + num_ztextures) * sizeof(*textureheight), PU_STATIC, 0);

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

  /* append the flats as wall textures; TEXTUREx names shadow flats, and
   * among same-named flats the later lump wins, both as in ZDoom */
  {
    int k;
    for (k = 0; k < numlumps; k++)
    {
      int t2;
      if (lumpinfo[k].li_namespace != ns_flats || W_LumpLength(k) < 64 * 64)
        continue;
      for (t2 = 0; t2 < numtextures; t2++)
        if (!strncasecmp(textures[t2]->name, lumpinfo[k].name, 8))
          break;
      if (t2 < numtextures)
      {
        /* a later flat of the same name replaces the appended entry */
        if (t2 >= numtextures1 + numtextures2)
          textures[t2]->patches[0].patch = k;
        continue;
      }
      texture = textures[numtextures] =
        Z_Malloc(sizeof(texture_t), PU_STATIC, 0);
      texture->width = 64;
      texture->height = 64;
      texture->patchcount = 1;
      texture->patches[0].originx = 0;
      texture->patches[0].originy = 0;
      texture->patches[0].patch = k;
      /* texture names are 8 bytes, NUL-padded but not NUL-terminated --
       * the strncpy pattern -Wstringop-truncation flags; zero-fill and
       * copy a clamped length instead */
      {
        size_t n = strlen(lumpinfo[k].name);
        if (n > sizeof(texture->name))
          n = sizeof(texture->name);
        memset(texture->name, 0, sizeof(texture->name));
        memcpy(texture->name, lumpinfo[k].name, n);
      }
      texture->widthmask = 63;
      textureheight[numtextures] = 64 << FRACBITS;
      numtextures++;
    }
  }

  /* append ZDoom TEXTURES/ members as standalone single-patch wall
   * textures, dimensions from the (materialized) patch headers.  ZDoom
   * gives the TX namespace precedence, so a TX entry replaces any
   * same-named TEXTUREx definition. */
  {
    int k;
    for (k = 0; k < numlumps; k++)
    {
      int t2, j2, pw, ph;
      const unsigned char *hdr;
      if (lumpinfo[k].li_namespace != ns_zdoom_tx || W_LumpLength(k) < 8)
        continue;
      hdr = W_CacheLumpNum(k);
      pw = (short)(hdr[0] | (hdr[1] << 8));
      ph = (short)(hdr[2] | (hdr[3] << 8));
      W_UnlockLumpNum(k);
      if (pw <= 0 || ph <= 0 || pw > 4096 || ph > 4096)
      {
        lprintf(LO_WARN, "R_InitTextures: TX lump %.8s has bad "
                "dimensions %dx%d\n", lumpinfo[k].name, pw, ph);
        continue;
      }
      for (t2 = 0; t2 < numtextures; t2++)
        if (!strncasecmp(textures[t2]->name, lumpinfo[k].name, 8))
          break;
      if (t2 < numtextures)
        texture = textures[t2] = Z_Malloc(sizeof(texture_t), PU_STATIC, 0);
      else
        texture = textures[numtextures] =
          Z_Malloc(sizeof(texture_t), PU_STATIC, 0);
      texture->width = (short)pw;
      texture->height = (short)ph;
      texture->patchcount = 1;
      texture->patches[0].originx = 0;
      texture->patches[0].originy = 0;
      texture->patches[0].patch = k;
      {
        size_t n = strlen(lumpinfo[k].name);
        if (n > sizeof(texture->name))
          n = sizeof(texture->name);
        memset(texture->name, 0, sizeof(texture->name));
        memcpy(texture->name, lumpinfo[k].name, n);
      }
      for (j2 = 1; j2 * 2 <= texture->width; j2 <<= 1)
        ;
      texture->widthmask = j2 - 1;
      if (t2 < numtextures)
        textureheight[t2] = texture->height << FRACBITS;
      else
      {
        textureheight[numtextures] = texture->height << FRACBITS;
        numtextures++;
      }
    }
  }

  /* TEXTURES-lump definitions whose name is not already a texture: a
   * definition like CAR05 = { Patch CAR02, XScale 1.24 } scales another
   * texture's patch under a new name.  Register as a single-patch
   * texture at the definition's world size (declared dims divided by
   * scale); the patch itself was resampled to its own world size at
   * materialization, so matching definitions line up exactly and any
   * residual mismatch crops or tiles in the composite. */
  {
    int k;
    for (k = 0; k < num_ztextures; k++)
    {
      const ztexture_t *zt = &ztextures[k];
      int t2, j2, ww, wh, pc, vi, vc, fill, pw = 0, ph = 0;

      for (t2 = 0; t2 < numtextures; t2++)
        if (!strncasecmp(textures[t2]->name, zt->name, 8))
          break;
      if (t2 < numtextures)
        continue;                   /* lump-backed texture wins */

      pc = zt->patchcount;
      if (pc < 1 || !zt->plist)
        continue;

      ww = (int)(zt->width / zt->xscale + 0.5);
      wh = (int)(zt->height / zt->yscale + 0.5);
      if (ww < 1) ww = 1;
      if (wh < 1) wh = 1;
      if (ww > 4096 || wh > 4096)
        continue;

      /* count the patches that resolve to a usable Doom patch */
      vc = 0;
      for (vi = 0; vi < pc; vi++)
        if (zt_resolve_patch(zt->plist[vi].name, NULL, NULL) >= 0)
          vc++;
      if (vc == 0)
      {
        lprintf(LO_WARN, "R_InitTextures: TEXTURES def %.8s: no usable "
                "patches (first %.8s)\n", zt->name, zt->patch);
        continue;
      }

      texture = textures[numtextures] =
        Z_Malloc(sizeof(texture_t) + sizeof(texpatch_t) * (vc - 1),
                 PU_STATIC, 0);
      texture->width = (short)ww;
      texture->height = (short)wh;
      texture->patchcount = (short)vc;
      {
        size_t n = strlen(zt->name);
        if (n > sizeof(texture->name))
          n = sizeof(texture->name);
        memset(texture->name, 0, sizeof(texture->name));
        memcpy(texture->name, zt->name, n);
      }

      /* fill the composite, mapping ZDoom canvas offsets through the scale */
      fill = 0;
      for (vi = 0; vi < pc; vi++)
      {
        int lp = zt_resolve_patch(zt->plist[vi].name, &pw, &ph);
        if (lp < 0)
        {
          lprintf(LO_WARN, "R_InitTextures: TEXTURES def %.8s: patch %.8s "
                  "not a usable patch (skipped)\n", zt->name,
                  zt->plist[vi].name);
          continue;
        }
        if (fill == 0 && pc == 1 && (pw != ww || ph != wh))
          lprintf(LO_INFO, "R_InitTextures: TEXTURES def %.8s is %dx%d "
                  "over a %dx%d patch (%.8s)\n",
                  zt->name, ww, wh, pw, ph, zt->plist[vi].name);
        texture->patches[fill].originx =
          (short)(zt->plist[vi].x / zt->xscale + 0.5);
        texture->patches[fill].originy =
          (short)(zt->plist[vi].y / zt->yscale + 0.5);
        texture->patches[fill].patch = lp;
        fill++;
      }

      for (j2 = 1; j2 * 2 <= texture->width; j2 <<= 1)
        ;
      texture->widthmask = j2 - 1;
      textureheight[numtextures] = texture->height << FRACBITS;
      numtextures++;
    }
  }

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

  /* Always allocate at least 2 slots.  Code elsewhere (boom
   * sector colormap selectors) probes colormaps[1] as a fallback,
   * and most plain DOOM WADs lack C_START/C_END markers -- so
   * numcolormaps comes out as 0 (or even negative if only one
   * marker exists).  The original MAX(1, numcolormaps) only
   * sized the array for index 0 and the "if (numcolormaps == 0)"
   * branch below scribbled past the end, corrupting the heap. */
  colormaps = Z_Malloc(sizeof(*colormaps) * MAX(2, numcolormaps), PU_STATIC, 0);
  colormaps[0] = (const lighttable_t *)W_CacheLumpName("COLORMAP");
  for (i=1; i<numcolormaps; i++)
    colormaps[i] = (const lighttable_t *)W_CacheLumpNum(i+firstcolormaplump);

  if(numcolormaps < 2) {
    /* No (or degenerate) C_START..C_END range.  Point the dummy
     * fallback slot at the default COLORMAP -- any code that
     * probes colormaps[1] thus sees a fully-formed 32x256 lighting
     * table and renders with standard lighting.  The previous code
     * here pointed colormaps[1] at a stack-local 1-byte array
     * `defaultmap` that went out of scope on function return,
     * leaving a dangling pointer that the array was also too
     * small to safely hold. */
    colormaps[1] = colormaps[0];
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
  {
     r_fine_lightweight = -1; /* fixed map: no distance fade, use band recovery */
     return fixedcolormap;
  }

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
  {
   /* The returned pointer is still snapped to one of NUMCOLORMAPS bands
    * (everything downstream assumes that).  But for Smooth shading we also
    * publish the same darkness value at SMOOTH_WEIGHTS resolution -- the very
    * "keep all the precision until the final step" refinement noted above --
    * computed by scaling the identical band formula by SMOOTH_WEIGHTS instead
    * of NUMCOLORMAPS, so the two agree exactly at band boundaries.  The
    * distance term's shift is reduced by log2(SMOOTH_WEIGHTS/NUMCOLORMAPS)
    * (= 3 for 256/32) to match the finer scale. */
   int band = between(0,NUMCOLORMAPS-1,
         ((256-lightlevel)*2*NUMCOLORMAPS/256) - 4
         - (FixedMul(spryscale,pspriteiscale)/2 >> LIGHTSCALESHIFT)
         );
   int fine = between(0,SMOOTH_WEIGHTS-1,
         ((256-lightlevel)*2*SMOOTH_WEIGHTS/256) - 4*(SMOOTH_WEIGHTS/NUMCOLORMAPS)
         - (FixedMul(spryscale,pspriteiscale)/2 >> (LIGHTSCALESHIFT-SMOOTH_WEIGHTS_SHIFT))
         );
   /* fine 'darkness' 0=bright..max=dark -> weight max=bright..0=dark */
   r_fine_lightweight = (SMOOTH_WEIGHTS-1) - fine;
   r_fine_colormap    = fullcolormap + band*256;
   return r_fine_colormap;
  }
}

//
// R_InitData
// Locates all the lumps
//  that will be used by all views
// Must be called after W_Init.
//

void R_InitData(void)
{
  /* decode pk3 PNG assets into patches/flats before anything reads them */
  U_ZTexturesLoad();              /* scale targets for materialization */
  U_ScanDecalPics();              /* know decal pics before materialising */
  U_PNGMaterializeLumps();
  lprintf(LO_INFO, "Textures\n");
  R_InitTextures();
  lprintf(LO_INFO, "Flats\n");
  R_InitFlats();
  lprintf(LO_INFO, "Sprites\n");
  R_InitSpriteLumps();
  lprintf(LO_INFO, "Colormaps\n");
  R_InitColormaps();                    // killough 3/20/98
  /* Brightmap definitions reference textures/flats/sprites by name, so
   * parse them once those name tables exist.  The per-texture masks are
   * baked later, from R_Init after the patch cache is initialised. */
  U_LoadBrightmaps();
  /* DECALDEF references textures by name too; parse it here for the same
   * reason.  Placement and rendering of decals are separate stages. */
  U_LoadDecalDefs();
}

//
// R_FlatNumForName
// Retrieval, get a flat number for a flat name.
//
// killough 4/17/98: changed to use ns_flats namespace
//

/* --- Synthetic flats: wall textures used as floors -------------------------
 * ZDoom allows any texture on any surface, so ZDoom-targeted wads
 * (chex3.wad) name wall textures in sector floor/ceiling fields; the name
 * then exists in the texture namespace but not the flats one.  When the
 * name is also a single patch lump (true for all of chex3's cases, whose
 * textures are one same-named 128x128 patch each), decode the patch and
 * point-sample it to the 64x64 raw cell the span renderer draws,
 * registering the result in a side table addressed by flat numbers from
 * numflats upward.  R_DoDrawPlane serves these directly, R_PrecacheLevel
 * skips them, and the animation tables can never match them.  Multi-patch
 * compositing is not attempted; such names keep the placeholder flat. */


static struct synth_flat_s
{
  char name[9];
  uint8_t data[4096];
} *synth_flats;
static int num_synth_flats, cap_synth_flats;
static int num_synth_flats;

dbool R_IsSyntheticFlat(int picnum)
{
  return picnum >= numflats && picnum < numflats + num_synth_flats;
}

const uint8_t *R_GetSyntheticFlat(int picnum)
{
  return synth_flats[picnum - numflats].data;
}

static int R_SynthGrow(void)
{
  if (num_synth_flats == cap_synth_flats)
  {
    int nc = cap_synth_flats ? cap_synth_flats * 2 : 64;
    struct synth_flat_s *ns =
      Z_Malloc(nc * sizeof(*ns), PU_STATIC, 0);
    if (synth_flats)
      memcpy(ns, synth_flats, num_synth_flats * sizeof(*ns));
    synth_flats = ns;
    cap_synth_flats = nc;
  }
  return num_synth_flats;
}

static int R_SynthFlatFromPatch(const char *name)
{
  int            i, lump, w, h, x, y, tex;
  size_t         lumplen;
  const uint8_t *pat;
  uint8_t       *tmp;
  struct synth_flat_s *sf;

  for (i = 0; i < num_synth_flats; i++)
    if (!strncasecmp(synth_flats[i].name, name, 8))
      return numflats + i;

  /* Wall texture on a floor (ZDoom's unified namespace): sample the
   * composite.  This covers both TEXTUREx composites (SHAWN2, BRICK4 on
   * MyHouse floors) and the standalone TX-namespace textures, whose
   * rpatch columns expose a solid per-column pixel buffer. */
  tex = R_CheckTextureNumForName(name);
  if (tex >= 0)
  {
    const rpatch_t *rp = R_CacheTextureCompositePatchNum(tex);
    if (rp && rp->width > 0 && rp->height > 0)
    {
      {
        int slot = R_SynthGrow();   /* grows synth_flats: sequence first */
        sf = &synth_flats[slot];
      }
      memset(sf->name, 0, sizeof(sf->name));
      strncpy(sf->name, name, 8);
      for (y = 0; y < 64; y++)
        for (x = 0; x < 64; x++)
        {
          int sx = x * rp->width / 64;
          int sy = y * rp->height / 64;
          sf->data[y * 64 + x] = rp->columns[sx].pixels[sy];
        }
      R_UnlockTextureCompositePatchNum(tex);
      lprintf(LO_INFO, "R_FlatNumForName: %.8s served from wall texture\n",
              name);
      return numflats + num_synth_flats++;
    }
    if (rp)
      R_UnlockTextureCompositePatchNum(tex);
  }

  /* Raw patch lump in the global namespace (chex3's CJFCOMM* case).
   * Posts decode with the DeePsea tall-patch rule, matching r_patch.c. */
  lump = (W_CheckNumForName)(name, ns_global);
  if (lump < 0)
    return -1;
  lumplen = W_LumpLength(lump);
  if (lumplen < 8)
    return -1;

  pat = W_CacheLumpNum(lump);
  w = (short)(pat[0] | (pat[1] << 8));
  h = (short)(pat[2] | (pat[3] << 8));
  if (w <= 0 || h <= 0 || w > 4096 || h > 4096 ||
      lumplen < 8 + 4 * (size_t)w)
  {
    W_UnlockLumpNum(lump);
    return -1;
  }

  tmp = calloc((size_t)w * h, 1);
  if (!tmp)
  {
    W_UnlockLumpNum(lump);
    return -1;
  }

  for (x = 0; x < w; x++)
  {
    int top = -1;
    size_t ofs = (size_t)(pat[8 + 4 * x]       |
                         (pat[8 + 4 * x + 1] << 8)  |
                         (pat[8 + 4 * x + 2] << 16) |
                ((size_t) pat[8 + 4 * x + 3] << 24));
    /* walk the column's posts, staying inside the lump */
    while (ofs + 1 < lumplen && pat[ofs] != 0xff)
    {
      int topdelta = pat[ofs];
      int len      = pat[ofs + 1];
      if (ofs + 4 + (size_t)len > lumplen)
        break;
      top = (topdelta <= top) ? top + topdelta : topdelta;
      for (y = 0; y < len; y++)
        if (top + y < h)
          tmp[(top + y) * w + x] = pat[ofs + 3 + y];
      ofs += (size_t)len + 4;
    }
  }
  W_UnlockLumpNum(lump);

  {
    int slot = R_SynthGrow();       /* grows synth_flats: sequence first */
    sf = &synth_flats[slot];
  }
  memset(sf->name, 0, sizeof(sf->name));
  strncpy(sf->name, name, 8);
  for (y = 0; y < 64; y++)
    for (x = 0; x < 64; x++)
      sf->data[y * 64 + x] = tmp[(y * h / 64) * w + (x * w / 64)];
  free(tmp);

  lprintf(LO_INFO, "R_FlatNumForName: %.8s served from texture patch\n", name);
  return numflats + num_synth_flats++;
}

int R_FlatNumForName(const char *name)    // killough -- const added
{
  int i = (W_CheckNumForName)(name, ns_flats);
  if (i == -1)
  {
    /* ZDoom-style texture-on-floor: serve the texture's patch as a flat. */
    int synth = R_SynthFlatFromPatch(name);

    /* I_Error is non-fatal in this core (logs and returns).  A missing
     * flat usually means a wrong/absent IWAD; don't fall through and
     * return a negative index that callers use to index flat arrays out
     * of bounds.  Substitute a real flat so the level loads with a
     * placeholder instead of crashing.  Index 0 is NOT safe: doom.wad's
     * flats namespace opens with the zero-byte F1_START marker, and
     * pointing a span at a zero-byte lump reads out of bounds (chex3.wad
     * E1M2, whose sectors name flats that only exist in ZDoom's textures
     * namespace). */
    static int fallback = -2;

    if (synth >= 0)
      return synth;

    I_Error("R_FlatNumForName: %.8s not found", name);

    if (fallback == -2)
    {
      int k;
      fallback = 0;
      for (k = firstflat; k <= lastflat; k++)
        if (W_LumpLength(k) >= 4096)
        {
          fallback = k - firstflat;
          break;
        }
    }
    return fallback;
  }
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
  if (*name != '-' && *name != '\0') // "-" and "" are both "NoTexture".
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
  {
    /* See R_FlatNumForName: I_Error does not abort here, so return a safe
     * texture index (NO_TEXTURE) rather than -1, which callers would use
     * to index the textures[] array out of bounds. */
    I_Error("R_TextureNumForName: %.8s not found", name);
    return NO_TEXTURE;
  }
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
    lprintf(LO_DEBUG,"bad texture '%.8s' in sidedef %d\n",name,snum);
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
  {
    /* synthetic flats (textures on floors) sit beyond the hitlist */
    if (sectors[i].floorpic < numflats)
      hitlist[sectors[i].floorpic] = 1;
    if (sectors[i].ceilingpic < numflats)
      hitlist[sectors[i].ceilingpic] = 1;
  }

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
      if (th->function.arg1 == (void (*)(void *))P_MobjThinker)
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
