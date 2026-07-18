/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2000 by
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
 *  Refresh of things, i.e. objects represented by sprites.
 *
 *-----------------------------------------------------------------------------*/

#include "doomstat.h"
#include "dsda_hacked.h"
#include "w_wad.h"
#include "r_main.h"
#include "r_bsp.h"
#include "r_segs.h"
#include "r_draw.h"
#include "r_things.h"
#include "r_plane.h"
#include "r_dynlight.h"
#include "r_drawtc.h"
#include "vid_mode.h"
#include "r_decal.h"
#include "u_decorate.h"
#include "r_fps.h"
#include "v_video.h"
#include "lprintf.h"
#include "u_brightmap.h"
#include "u_voxel.h"
#include "r_voxel.h"

#define MINZ        (FRACUNIT*4)
#define BASEYCENTER 100

typedef struct {
  int x1;
  int x2;
  int column;
  int topclip;
  int bottomclip;
} maskdraw_t;

//
// Sprite rotation 0 is facing the viewer,
//  rotation 1 is one angle turn CLOCKWISE around the axis.
// This is not the same as the angle,
//  which increases counter clockwise (protractor).
// There was a lot of stuff grabbed wrong, so I changed it...
//

fixed_t pspritescale;
fixed_t pspriteiscale;
// proff 11/06/98: Added for high-res
fixed_t pspriteyscale;

// constant arrays
//  used for psprite clipping and initializing clipping

int negonearray[MAX_SCREENWIDTH];        // killough 2/8/98: // dropoff overflow
int screenheightarray[MAX_SCREENWIDTH];  // change to MAX_* // dropoff overflow

//
// INITIALIZATION FUNCTIONS
//

// variables used to look up and range check thing_t sprites patches

spritedef_t *sprites;
int numsprites;

#define MAX_SPRITE_FRAMES 30          /* Macroized -- killough 1/25/98 */

static spriteframe_t sprtemp[MAX_SPRITE_FRAMES];
static int maxframe;

//
// R_InstallSpriteLump
// Local function for R_InitSprites.
//

static void R_InstallSpriteLump(int lump, unsigned frame,
                                unsigned rotation, dbool   flipped)
{
   if (frame >= MAX_SPRITE_FRAMES || rotation > 8)
   {
      /* I_Error logs and returns in this core rather than aborting, so the
       * function must bail out here: falling through would index sprtemp[]
       * (length MAX_SPRITE_FRAMES) with the out-of-range frame and corrupt
       * memory.  A frame letter past 'A'+MAX_SPRITE_FRAMES-1 reaches this --
       * some PK3 sprite lumps (e.g. high-frame scene sprites) carry such
       * letters -- so skip the offending lump instead of installing it. */
      I_Error("R_InstallSpriteLump: Bad frame characters in lump %i", lump);
      return;
   }

   if ((int) frame > maxframe)
      maxframe = frame;

   if (rotation == 0)
   {    // the lump should be used for all rotations
      int r;
      for (r=0 ; r<8 ; r++)
         if (sprtemp[frame].lump[r]==-1)
         {
            sprtemp[frame].lump[r] = lump - firstspritelump;
            sprtemp[frame].flip[r] = (uint8_t) flipped;
            sprtemp[frame].rotate = FALSE; //jff 4/24/98 if any subbed, rotless
         }
      return;
   }

   // the lump is only used for one rotation

   if (sprtemp[frame].lump[--rotation] == -1)
   {
      sprtemp[frame].lump[rotation] = lump - firstspritelump;
      sprtemp[frame].flip[rotation] = (uint8_t) flipped;
      sprtemp[frame].rotate = TRUE; //jff 4/24/98 only change if rot used
   }
}

//
// R_InitSpriteDefs
// Pass a null terminated list of sprite names
// (4 chars exactly) to be used.
//
// Builds the sprite rotation matrixes to account
// for horizontally flipped sprites.
//
// Will report an error if the lumps are inconsistent.
// Only called at startup.
//
// Sprite lump names are 4 characters for the actor,
//  a letter for the frame, and a number for the rotation.
//
// A sprite that is flippable will have an additional
//  letter/number appended.
//
// The rotation character can be 0 to signify no rotations.
//
// 1/25/98, 1/31/98 killough : Rewritten for performance
//
// Empirically verified to have excellent hash
// properties across standard Doom sprites:

#define R_SpriteNameHash(s) ((unsigned)((s)[0]-((s)[1]*3-(s)[3]*2-(s)[2])*2))

/* DECORATE-bearing wads (ZDoom-targeted pwads such as chex3.wad) redefine
 * their decorations' state sequences in DECORATE, which this engine cannot
 * read.  When such a wad replaces only a subset of a sprite's frames, the
 * vanilla state tables keep cycling through the full sequence and the
 * remaining frames leak the older wad's art mid-animation (chex3 carries
 * only frame A of BAR1/COL5, so Doom's barrel/pillar art shows every other
 * cycle).  The DECORATE wad never meant the older art to appear, so unify
 * the sprite to its art: slots sourced from older wads borrow the nearest
 * same-rotation frame the DECORATE wad does provide.  Wads without a
 * DECORATE lump are untouched -- partial sprite overrides there (smooth
 * weapon packs, single-frame fix wads) mix sources intentionally. */
static void R_UnifyDecorateSprite(const wadfile_info_t *decorate_wad)
{
   int f, r, f2;
   int has_dec = 0, has_old = 0;

   for (f = 0; f <= maxframe; f++)
      for (r = 0; r < 8; r++)
         if (sprtemp[f].lump[r] != -1)
         {
            if (lumpinfo[firstspritelump + sprtemp[f].lump[r]].wadfile
                == decorate_wad)
               has_dec = 1;
            else
               has_old = 1;
         }

   if (!has_dec || !has_old)
      return;

   /* Only unify a sprite the DECORATE wad genuinely *replaces* -- one where it
    * supplies art at the same frame letters the older wad already had.  A mod
    * that merely *adds* new frames (e.g. ZDCMP2 ships BSPI Q-Y gib-death frames
    * while the walk/attack frames A-P come from the IWAD, with no frame-letter
    * overlap) is not replacing anything: its monster animates through the IWAD
    * frames on purpose.  Borrowing a DECORATE death frame into those IWAD walk
    * slots would collapse the whole animation onto one sprite (the monster then
    * moves and fights while showing a single frozen frame).  Detect a real
    * replacement by a frame the both wads supply; if there is none, the
    * DECORATE frames are purely additive and nothing is unified. */
   {
      int replaces = 0;
      for (f = 0; f <= maxframe && !replaces; f++)
      {
         int dec_here = 0, old_here = 0;
         for (r = 0; r < 8; r++)
            if (sprtemp[f].lump[r] != -1)
            {
               if (lumpinfo[firstspritelump + sprtemp[f].lump[r]].wadfile
                   == decorate_wad)
                  dec_here = 1;
               else
                  old_here = 1;
            }
         if (dec_here && old_here)
            replaces = 1;       /* same frame from both wads: a real override */
      }
      if (!replaces)
         return;                /* additive-only: leave the older frames intact */
   }

   for (f = 0; f <= maxframe; f++)
      for (r = 0; r < 8; r++)
      {
         if (sprtemp[f].lump[r] == -1)
            continue;
         if (lumpinfo[firstspritelump + sprtemp[f].lump[r]].wadfile
             == decorate_wad)
            continue;

         /* nearest frame letter below, then above, with this rotation
          * supplied by the DECORATE wad */
         {
            int found = -1;
            for (f2 = f - 1; f2 >= 0 && found < 0; f2--)
               if (sprtemp[f2].lump[r] != -1
                   && lumpinfo[firstspritelump + sprtemp[f2].lump[r]].wadfile
                      == decorate_wad)
                  found = f2;
            for (f2 = f + 1; f2 <= maxframe && found < 0; f2++)
               if (sprtemp[f2].lump[r] != -1
                   && lumpinfo[firstspritelump + sprtemp[f2].lump[r]].wadfile
                      == decorate_wad)
                  found = f2;
            if (found >= 0)
            {
               sprtemp[f].lump[r] = sprtemp[found].lump[r];
               sprtemp[f].flip[r] = sprtemp[found].flip[r];
            }
         }
      }
}

static void R_InitSpriteDefs(const char * const * namelist)
{
   size_t numentries = lastspritelump-firstspritelump+1;
   struct { int index, next; } *hash;
   int i;
   const wadfile_info_t *decorate_wad = NULL;
   int decorate_lump;

   if (!numentries || !*namelist)
      return;

   decorate_lump = W_CheckNumForName("DECORATE");
   if (decorate_lump >= 0)
      decorate_wad = lumpinfo[decorate_lump].wadfile;

   // DSDHacked: the sprite-name table can be grown by dehacked and may
   // contain NULL gaps at unfilled indices, so its length is the runtime
   // num_sprites count rather than a NULL-terminator scan (which would stop
   // at the first gap and, post-growth, could miss the terminator entirely).
   numsprites = num_sprites;

   sprites = Z_Malloc(numsprites *sizeof(*sprites), PU_STATIC, NULL);
   /* Zero-init: the for(i) loop below only writes sprites[i].numframes
    * and sprites[i].spriteframes inside `if (j >= 0)` -- so any sprite
    * whose name has no matching lumps in this WAD is left with
    * uninitialized numframes/spriteframes.  Garbage numframes was
    * latent (downstream readers in r_data.c / f_finale.c trust it),
    * and garbage spriteframes crashes R_Deinit's free loop on Win32
    * GFlags-painted heap memory. */
   memset(sprites, 0, numsprites * sizeof(*sprites));

   // Create hash table based on just the first four letters of each sprite
   // killough 1/31/98

   hash = malloc(sizeof(*hash)*numentries); // allocate hash table

   for (i=0; (size_t)i<numentries; i++)             // initialize hash table as empty
      hash[i].index = -1;

   for (i=0; (size_t)i<numentries; i++)             // Prepend each sprite to hash chain
   {                                      // prepend so that later ones win
      int j = R_SpriteNameHash(lumpinfo[i+firstspritelump].name) % numentries;
      hash[i].next = hash[j].index;
      hash[j].index = i;
   }

   // scan all the lump names for each of the names,
   //  noting the highest frame letter.

   for (i=0 ; i<numsprites ; i++)
   {
      const char *spritename = namelist[i];
      int j;

      if (!spritename)   /* DSDHacked: unfilled sparse index, no sprite here */
         continue;

      j = hash[R_SpriteNameHash(spritename) % numentries].index;

      if (j >= 0)
      {
         memset(sprtemp, -1, sizeof(sprtemp));
         maxframe = -1;
         do
         {
            register lumpinfo_t *lump = lumpinfo + j + firstspritelump;

            // Fast portable comparison -- killough
            // (using int pointer cast is nonportable):

            if (!((lump->name[0] ^ spritename[0]) |
                     (lump->name[1] ^ spritename[1]) |
                     (lump->name[2] ^ spritename[2]) |
                     (lump->name[3] ^ spritename[3])))
            {
               R_InstallSpriteLump(j+firstspritelump,
                     lump->name[4] - 'A',
                     lump->name[5] - '0',
                     FALSE);
               if (lump->name[6])
                  R_InstallSpriteLump(j+firstspritelump,
                        lump->name[6] - 'A',
                        lump->name[7] - '0',
                        TRUE);
            }
         }
         while ((j = hash[j].next) >= 0);

         /* only when DECORATE actually redefines this sprite's sequence:
          * a wad can carry DECORATE for new actors while intentionally
          * mixing old and new art on untouched vanilla sprites (nova4.wad
          * reskins the zombies' walk frames but keeps doom2's death
          * frames; unifying those drew corpses with standing art) */
         if (decorate_wad && maxframe >= 0 &&
             U_DecorateMentionsSprite(spritename))
            R_UnifyDecorateSprite(decorate_wad);

         // check the frames that were found for completeness
         if ((sprites[i].numframes = ++maxframe))  // killough 1/31/98
         {
            int frame;
            for (frame = 0; frame < maxframe; frame++)
               switch ((int) sprtemp[frame].rotate)
               {
                  case -1:
                     /* No patch for this frame.  Vanilla Doom treats a gap in a
                      * sprite's frame run as fatal, but ZDoom DECORATE mods
                      * routinely define sparse frames (e.g. the hdoom female-imp
                      * sprites skip frame 'H', and the scene sprites skip '\\'),
                      * relying on the intermediate frames simply not drawing.
                      * Leave this frame a non-drawable hole -- single rotation,
                      * sentinel lump -- so the frames that DO exist still render
                      * and only the absent frame shows nothing.  The projection
                      * path already skips a frame whose lump is < 0. */
                     sprtemp[frame].rotate  = 0;
                     sprtemp[frame].lump[0] = -1;
                     sprtemp[frame].flip[0] = 0;
                     break;

                  case 0:
                     // only the first rotation is needed
                     break;

                  case 1:
                     /* A frame that supplies some but not all eight rotations:
                      * fill the missing rotations with the sentinel lump rather
                      * than aborting, matching the gap handling above. */
                     {
                        int rotation;
                        for (rotation=0 ; rotation<8 ; rotation++)
                           if (sprtemp[frame].lump[rotation] == -1)
                           {
                              sprtemp[frame].lump[rotation] = -1;
                              sprtemp[frame].flip[rotation] = 0;
                           }
                        break;
                     }
               }
            // allocate space for the frames present and copy sprtemp to it
            sprites[i].spriteframes =
               Z_Malloc (maxframe * sizeof(spriteframe_t), PU_STATIC, NULL);
            memcpy (sprites[i].spriteframes, sprtemp,
                  maxframe*sizeof(spriteframe_t));
         }
      }
   }
   free(hash);             // free hash table
}

//
// GAME FUNCTIONS
//

static vissprite_t *vissprites, **vissprite_ptrs;  // killough
static size_t num_vissprite, num_vissprite_alloc, num_vissprite_ptrs;

/* e6y/entryway (via PrBoom+): drawseg X-range partitioning for sprite
 * clipping. R_DrawSprite has to test each sprite against the drawsegs that
 * could obscure it; the classic loop scans every drawseg for every sprite,
 * which becomes quadratic on maps with thousands of drawsegs and sprites.
 *
 * Once per frame we collect the silhouette/masked drawsegs into three
 * pre-extracted lists: range 0 holds all of them, range 1 only those that
 * touch the left half of the screen (x1 < cx), range 2 only those touching
 * the right half (x2 >= cx). A sprite lying entirely on one side then scans
 * just that half's list; a sprite straddling the centre uses the full list.
 * Each entry is a packed {x1, x2, user} record so the hot x1/x2 reject test
 * reads a small contiguous array instead of chasing full drawseg_t structs,
 * which also cuts cache misses. */
typedef struct drawseg_xrange_item_s
{
  short x1, x2;
  drawseg_t *user;
} drawseg_xrange_item_t;

typedef struct drawsegs_xrange_s
{
  drawseg_xrange_item_t *items;
  int count;
} drawsegs_xrange_t;

#define DS_RANGES_COUNT 3
static drawsegs_xrange_t drawsegs_xranges[DS_RANGES_COUNT];

static drawseg_xrange_item_t *drawsegs_xrange;
static unsigned int drawsegs_xrange_size = 0;
static int drawsegs_xrange_count = 0;

//
// R_InitSprites
// Called at program start.
//

void R_InitSprites(const char * const *namelist)
{
   int i;
   for (i=0; i<MAX_SCREENWIDTH; i++)    // killough 2/8/98
      negonearray[i] = -1;
   R_InitSpriteDefs(namelist);
}

//
// R_ClearSprites
// Called at frame start.
//

void R_ClearSprites (void)
{
   num_vissprite = 0;            // killough
}

//
// R_NewVisSprite
//

static vissprite_t *R_NewVisSprite(void)
{
   if (num_vissprite >= num_vissprite_alloc)             // killough
   {
      size_t num_vissprite_alloc_prev = num_vissprite_alloc;

      num_vissprite_alloc = num_vissprite_alloc ? num_vissprite_alloc*2 : 128;
      vissprites = realloc(vissprites,num_vissprite_alloc*sizeof(*vissprites));

      //e6y: set all fields to zero
      memset(vissprites + num_vissprite_alloc_prev, 0,
            (num_vissprite_alloc - num_vissprite_alloc_prev)*sizeof(*vissprites));
   }
   return vissprites + num_vissprite++;
}

//
// R_DrawMaskedColumn
// Used for sprites and masked mid textures.
// Masked means: partly transparent, i.e. stored
//  in posts/runs of opaque pixels.
//

int   *mfloorclip;   // dropoff overflow
int   *mceilingclip; // dropoff overflow
fixed_t spryscale;
#ifndef DIRECT_COLUMN_MINPX
#define DIRECT_COLUMN_MINPX 64
#endif
fixed_t sprtopscreen;

/* Per-column fullbright mask base for the sprite currently being drawn by
 * R_DrawVisSprite (column-major, aligned to the sprite patch column the
 * posts index into), or NULL.  Set per column before R_DrawMaskedColumn
 * and consumed there; only the sprite path uses it, so the masked
 * midtexture caller in r_segs.c leaves it NULL. */
static const uint8_t *sprite_brightcol;

void R_DrawMaskedColumn(
      const rpatch_t *patch,
      R_DrawColumn_f colfunc,
      draw_column_vars_t *dcvars,
      const rcolumn_t *column,
      const rcolumn_t *prevcolumn,
      const rcolumn_t *nextcolumn
      )
{
  int     i;
  int     topscreen;
  int     bottomscreen;
  fixed_t basetexturemid = dcvars->texturemid;

  dcvars->texheight = patch->height; // killough 11/98
  for (i=0; i<column->numPosts; i++) {
    const rpost_t *post = &column->posts[i];

    // calculate unclipped screen coordinates for post
    topscreen = sprtopscreen + spryscale*post->topdelta;
    bottomscreen = topscreen + spryscale*post->length;

    dcvars->yl = (topscreen+FRACUNIT-1)>>FRACBITS;
    dcvars->yh = (bottomscreen-1)>>FRACBITS;

    if (dcvars->yh >= mfloorclip[dcvars->x])
      dcvars->yh = mfloorclip[dcvars->x]-1;

    if (dcvars->yl <= mceilingclip[dcvars->x])
      dcvars->yl = mceilingclip[dcvars->x]+1;

    /* mceilingclip's default sentinel is -1, so a sprite whose top projects
     * above the viewport (a tall or oversized sprite) can leave yl negative.
     * The column drawers index the temp buffer and the framebuffer by yl, so a
     * negative yl reads/writes before those buffers -- a torn strip of
     * framebuffer garbage.  Clamp to the top of the screen. */
    if (dcvars->yl < 0)
      dcvars->yl = 0;

    // killough 3/2/98, 3/27/98: Failsafe against overflow/crash:
    if (dcvars->yl <= dcvars->yh && dcvars->yh < viewheight)
    {
      dcvars->source = column->pixels + post->topdelta;
      dcvars->prevsource = prevcolumn->pixels + post->topdelta;
      dcvars->nextsource = nextcolumn->pixels + post->topdelta;
      dcvars->brightmask = sprite_brightcol
                           ? sprite_brightcol + post->topdelta : NULL;

      dcvars->texturemid = basetexturemid - (post->topdelta<<FRACBITS);

      dcvars->edgeslope = post->slope;
      // Drawn by either R_DrawColumn
      //  or (SHADOW) R_DrawFuzzColumn.
      /* Default-skybox reveal mask: masked draws (sprites, two-sided mid
       * textures) cover their pixels so the skybox composite -- which runs
       * after R_DrawMasked -- cannot paint over them (r_plane.c). */
      if (sky_reveal_active)
        R_SkyRevealCoverCol(dcvars->x, dcvars->yl, dcvars->yh);
      dcvars->drawingmasked = 1; // POPE
      colfunc (dcvars);
      dcvars->drawingmasked = 0; // POPE
    }
  }
  dcvars->texturemid = basetexturemid;
}

//
// R_DrawVisSprite
//  mfloorclip and mceilingclip should also be set.
//
// CPhipps - new wad lump handling, *'s to const*'s
/* Direct framebuffer path for opaque, point-sampled, square-edged
 * sprite columns.  The temp-buffer machinery writes every masked
 * pixel twice (column buffer, then flush), pays a batching preamble
 * per drawn post, and -- because a column's second post fails the
 * batcher's x-continuity test -- degenerates to a flush per post on
 * multi-post columns.  For the plain opaque pipeline none of that
 * buys anything, so this walks the same posts with the same clip and
 * frac arithmetic as R_DrawMaskedColumn and writes the framebuffer
 * once.
 *
 * The texel index wraps by the patch height: the point drawers do
 * the same through their height mask or modulo prep, which a post's
 * fractionally negative first-row frac (FixedDiv-rounded iscale) can
 * reach.  Both styles reduce to index mod texheight for the values a
 * post produces. */
/* The post walk, clipping and frac arithmetic are width-independent, so a
 * single copy serves both surfaces; only the colour table and the store
 * differ.  `lutTC` is the composed truecolor table (built from V_PaletteTC),
 * so an opaque sprite texel is written at the surface's full channel width
 * without passing through a 565 value at any point. */
static void R_DrawMaskedColumnDirect(const rpatch_t *patch,
                                     draw_column_vars_t *dcvars,
                                     const rcolumn_t *column,
                                     const uint16_t *lut,
                                     const uint32_t *lutTC)
{
  const int tc = (lutTC != NULL);
  const fixed_t iscale = dcvars->iscale;
  const int     spr_th = patch->height;
  const int     x      = dcvars->x;
  int i;

  for (i = 0; i < column->numPosts; i++)
  {
    const rpost_t *post = &column->posts[i];
    int topscreen    = sprtopscreen + spryscale * post->topdelta;
    int bottomscreen = topscreen + spryscale * post->length;
    int yl = (topscreen + FRACUNIT - 1) >> FRACBITS;
    int yh = (bottomscreen - 1) >> FRACBITS;

    if (yh >= mfloorclip[x])
      yh = mfloorclip[x] - 1;
    if (yl <= mceilingclip[x])
      yl = mceilingclip[x] + 1;
    /* mceilingclip's default sentinel is -1, so the clamp above can leave yl
     * negative for a sprite whose top projects above the viewport (e.g. a tall
     * or oversized sprite).  The destination pointer and run length below are
     * derived from yl, so a negative yl writes the column into memory before
     * the surface -- a torn strip of framebuffer garbage.  The canonical
     * R_DrawMaskedColumn path relies on the column drawer clamping this; the
     * direct path inlines the write, so clamp here too.  frac is recomputed
     * from the clamped yl below, keeping the texture sample aligned. */
    if (yl < 0)
      yl = 0;

    if (yl <= yh && yh < viewheight)
    {
      const uint8_t *source = column->pixels + post->topdelta;
      fixed_t frac = (dcvars->texturemid - (post->topdelta << FRACBITS))
                   + (yl - centery) * iscale;
      uint16_t *dest   = drawvars.short_topleft
                       + yl * SURFACE_SHORT_PITCH + x;
      uint32_t *destTC = ((uint32_t *)drawvars.int_topleft)
                       + yl * SURFACE_SHORT_PITCH + x;
      int count = yh - yl + 1;
      /* iscale (= FRACUNIT/scale) is always positive, so frac is monotonic
       * over the run: if the first and last texel indices both fall inside
       * [0, spr_th) every index in between does too.  In that case -- which
       * is effectively always, for a normally projected sprite -- the inner
       * loop needs no per-pixel wrap, removing two branches from the hottest
       * column loop.  The wrapping loop is kept as the exact fallback for the
       * rare out-of-range frac (oversized/clipped projections). */
      int idx0 = frac >> 16;
      int idxN = (frac + (count - 1) * iscale) >> 16;
      if (sky_reveal_active)
        R_SkyRevealCoverCol(x, yl, yh);
      if (count > 0 && idx0 >= 0 && idx0 < spr_th
                    && idxN >= 0 && idxN < spr_th)
      {
        if (tc)
          while (count--)
          {
            *destTC = lutTC[ source[frac >> 16] ];
            destTC += SURFACE_SHORT_PITCH;
            frac   += iscale;
          }
        else
          while (count--)
          {
            *dest = lut[ source[frac >> 16] ];
            dest += SURFACE_SHORT_PITCH;
            frac += iscale;
          }
      }
      else
      {
        if (tc)
          while (count--)
          {
            int idx = frac >> 16;
            while (idx < 0)
              idx += spr_th;
            while (idx >= spr_th)
              idx -= spr_th;
            *destTC = lutTC[ source[idx] ];
            destTC += SURFACE_SHORT_PITCH;
            frac   += iscale;
          }
        else
          while (count--)
          {
            int idx = frac >> 16;
            while (idx < 0)
              idx += spr_th;
            while (idx >= spr_th)
              idx -= spr_th;
            *dest = lut[ source[idx] ];
            dest += SURFACE_SHORT_PITCH;
            frac += iscale;
          }
      }
    }
  }
}

static void R_DrawVisSprite(vissprite_t *vis, int x1, int x2)
{
  int      texturecolumn;
  fixed_t  frac;
  const rpatch_t *patch;
  const uint8_t  *sprmask;
  R_DrawColumn_f colfunc;
  draw_column_vars_t dcvars;
  enum draw_filter_type_e filter;
  enum draw_filter_type_e filterz;

  /* Voxel models take a separate rasteriser; they carry no patch. */
  if (vis->voxel)
  {
    R_DrawVoxel(vis);
    return;
  }

  patch   = R_CachePatchNum(vis->patch+firstspritelump);
  sprmask = U_BrightmaskForSprite(vis->patch);
  R_SetDefaultDrawColumnVars(&dcvars);
  if (vis->mobjflags & MF_PLAYERSPRITE) {
    dcvars.edgetype = drawvars.patch_edges;
    filter = drawvars.filterpatch;
    filterz = RDRAW_FILTER_POINT;
  } else {
    dcvars.edgetype = drawvars.sprite_edges;
    filter = drawvars.filtersprite;
    filterz = drawvars.filterz;
  }

  dcvars.colormap = vis->colormap;
  dcvars.nextcolormap = dcvars.colormap; // for filtering -- POPE

  /* Raven translucent sprites: Hexen MF_SHADOW draws at half intensity and
   * MF_ALTSHADOW fainter still; Heretic ghosts use the same path.  The
   * sprite keeps its lit colormap and the column flushers blend. */
  if (raven && (vis->mobjflags & (MF_SHADOW | MF_ALTSHADOW)) && vis->colormap)
    R_SetSpriteTranslucency((vis->mobjflags & MF_ALTSHADOW) ? 2 : 1);
  /* DECORATE render style: an actor with RenderStyle Add (vis->translucent==2)
   * blends additively, any other translucency (==1) alpha-lerps; the weight is
   * vis->alpha (0..32).  Modes 3/4 are the per-alpha additive/lerp flushers.
   * The raven shadow path above takes precedence (it has set its own mode). */
  else if (vis->translucent && vis->colormap)
  {
    R_SetSpriteTranslucency(vis->translucent == 2 ? 3 : 4);
    R_SetTransAlpha(vis->alpha);
  }

  // killough 4/11/98: rearrange and handle translucent sprites
  // mixed with translucent/non-translucenct 2s normals

  if (!dcvars.colormap)   // NULL colormap = shadow draw
    colfunc = R_GetDrawColumnFunc(RDC_PIPELINE_FUZZ, filter, filterz);    // killough 3/14/98
  else
    if (vis->xlat && U_DecorateTranslationOK(vis->xlat))
    {
      /* DECORATE Translation: an arbitrary palette remap built from the
       * actor's Translation property (e.g. the recoloured imp fireball).  The
       * pointer is validated against the built-table storage so a stale or
       * unset vissprite slot can never feed a wild pointer to the column
       * drawer. */
      colfunc = R_GetDrawColumnFunc(RDC_PIPELINE_TRANSLATED, filter, filterz);
      dcvars.translation = vis->xlat;
    }
    else if (vis->mobjflags & MF_TRANSLATION)
    {
      colfunc = R_GetDrawColumnFunc(RDC_PIPELINE_TRANSLATED, filter, filterz);
      dcvars.translation = translationtables - 256 +
        ((vis->mobjflags & MF_TRANSLATION) >> (MF_TRANSSHIFT-8) );
    }
    else
      colfunc = R_GetDrawColumnFunc(RDC_PIPELINE_STANDARD, filter, filterz); // killough 3/14/98, 4/11/98

  // proff 11/06/98: Changed for high-res
  dcvars.iscale = FixedDiv (FRACUNIT, vis->scale);
  dcvars.texturemid = vis->texturemid;
  frac = vis->startfrac;
  if (filter == RDRAW_FILTER_LINEAR)
    frac -= (FRACUNIT>>1);
  spryscale = vis->scale;
  sprtopscreen = centeryfrac - FixedMul(dcvars.texturemid,spryscale);

  // make sure the player weapon is in a static position on the screen
  if(vis->mobjflags & MF_PLAYERSPRITE)
  {
    dcvars.texturemid += FixedMul(((centery - viewheight/2)<<FRACBITS), dcvars.iscale);
    sprtopscreen += (viewheight/2 - centery)<<FRACBITS;
  }

  {
    /* Opaque + point + square edges takes the direct path; the raven
     * shadow modes blend in the column flushers even on the standard
     * pipeline, so they stay on the machinery along with fuzz,
     * translated, filtered, and sloped-edge sprites. */
    int run_cls = R_WallColumnKernelClass(colfunc);

    if ((run_cls == 1 || run_cls == 2) &&
        !sprmask &&
        dcvars.edgetype != RDRAW_MASKEDCOLUMNEDGE_SLOPED &&
        !vis->translucent &&
        !(raven && (vis->mobjflags & (MF_SHADOW | MF_ALTSHADOW)) &&
          vis->colormap))
    {
      const int tc = VID_TRUECOLOR;
      const uint16_t *lut = tc ? NULL
          : ((run_cls == 1) ? R_ComposedPalette()
                            : R_ComposedColormap(dcvars.colormap));
      const uint32_t *lutTC = !tc ? NULL
          : ((run_cls == 1) ? R_ComposedPaletteTC()
                            : R_ComposedColormapTC(dcvars.colormap));
      uint16_t tinted_lut[256];
      uint32_t tinted_lutTC[256];
      const int spr_tint = (vis->tint_r | vis->tint_g | vis->tint_b);

      /* Coloured light: tint the composed LUT once for the whole sprite, then
       * route every column through the direct writer so only opaque texels
       * (which index the LUT) get the colour and transparent posts are left
       * alone.  White/unlit sprites keep the existing direct/batched split. */
      if (spr_tint)
      {
         if (tc)
         {
            R_TintLUTTC(tinted_lutTC, lutTC,
                        vis->tint_r, vis->tint_g, vis->tint_b);
            lutTC = tinted_lutTC;
         }
         else
         {
            R_TintLUT(tinted_lut, lut, vis->tint_r, vis->tint_g, vis->tint_b);
            lut = tinted_lut;
         }
      }

      /* A previous sprite's tail may still be batched in the temp
       * buffer; its flush must land before any direct writes, not
       * after. */
      R_ResetColumnBuffer();
      for (dcvars.x=vis->x1 ; dcvars.x<=vis->x2 ; dcvars.x++, frac += vis->xiscale)
      {
        const rcolumn_t *column;

        texturecolumn = frac>>FRACBITS;
        column = R_GetPatchColumnClamped(patch, texturecolumn);

        /* Single-post columns batch well in the temp machinery (four
         * adjacent columns flush as 4-wide rows); a column's second
         * post fails the batcher's x-continuity test and forces a
         * flush per post, so multi-post columns go direct.  The two
         * paths never touch the same pixels -- columns are disjoint
         * in x -- so they interleave safely within a sprite. */
        /* Tall columns: the temp-buffer batcher writes every pixel twice
         * (colfunc -> short_tempbuf, then R_FlushWhole16 -> framebuffer),
         * which the 4-wide flush only amortises for short runs.  A close
         * sprite's columns are hundreds of pixels tall, so the second pass
         * dominates; send those straight to the single-pass direct writer.
         * Multi-post columns already go direct (the batcher flushes per
         * post for them).  Output is bit-identical either way.  The height
         * test is overflow-safe: spryscale>>8 keeps the product well within
         * 32 bits for any realistic scale/post length. */
        if (spr_tint || column->numPosts > 1 ||
            (column->numPosts == 1 &&
             (((spryscale >> 8) * column->posts[0].length) >> (FRACBITS - 8))
               >= DIRECT_COLUMN_MINPX))
        {
          dcvars.texheight = patch->height;
          R_DrawMaskedColumnDirect(patch, &dcvars, column, lut, lutTC);
        }
        else
        {
          dcvars.texu = frac;
          R_DrawMaskedColumn(
            patch,
            colfunc,
            &dcvars,
            column,
            R_GetPatchColumnClamped(patch, texturecolumn-1),
            R_GetPatchColumnClamped(patch, texturecolumn+1)
          );
        }
      }
    }
    else
    {
      for (dcvars.x=vis->x1 ; dcvars.x<=vis->x2 ; dcvars.x++, frac += vis->xiscale)
      {
        texturecolumn = frac>>FRACBITS;
        dcvars.texu = frac;

        if (sprmask)
        {
          int c = texturecolumn;
          if (c < 0) c = 0;
          else if (c >= patch->width) c = patch->width - 1;
          sprite_brightcol = sprmask + (size_t)c * patch->height;
        }

        R_DrawMaskedColumn(
          patch,
          colfunc,
          &dcvars,
          R_GetPatchColumnClamped(patch, texturecolumn),
          R_GetPatchColumnClamped(patch, texturecolumn-1),
          R_GetPatchColumnClamped(patch, texturecolumn+1)
        );
      }
      sprite_brightcol = NULL;
    }
  }
  R_UnlockPatchNum(vis->patch+firstspritelump); // cph - release lump

  R_SetSpriteTranslucency(0);
}

//
// R_ProjectSprite
// Generates a vissprite for a thing if it might be visible.
//

static void R_ProjectSprite (mobj_t* thing, int lightlevel)
{
   int spr_tr = 0, spr_tg = 0, spr_tb = 0;
   fixed_t   gzt;               // killough 3/27/98
   fixed_t   tx;
   fixed_t   xscale;
   int       x1;
   int       x2;
   spritedef_t   *sprdef;
   spriteframe_t *sprframe;
   int       lump;
   dbool     flip;
   vissprite_t *vis;
   fixed_t   iscale;
   int heightsec;      // killough 3/27/98

   // transform the origin point
   fixed_t tr_x, tr_y;
   fixed_t fx, fy, fz;
   fixed_t gxt, gyt;
   fixed_t tz;
   int width;

   /* Hexen: buried thrust spikes, dormant wraiths etc. are flagged
    * MF2_DONTDRAW and must not be rendered. */
   if (raven && (thing->flags2 & MF2_DONTDRAW))
      return;

   if (movement_smooth)
   {
      /* Match R_InterpolateView's pause/menu policy: when the
       * camera is frozen at the post-tic position (frac=FRACUNIT),
       * sprites must freeze at the post-tic position too.  Using
       * tic_vars.frac here while the camera uses FRACUNIT puts the
       * local player's own thing at a non-zero distance from the
       * camera, so the standard `tz < MINZ` skip no longer fires
       * and the player's sprite gets projected over the viewport
       * at huge scale (visible as a large pulsating blob when the
       * menu is opened mid-movement at >35 fps). */
      fixed_t frac = (paused || (menuactive && !demoplayback))
                     ? FRACUNIT : tic_vars.frac;
      fx = thing->PrevX + FixedMul (frac, thing->x - thing->PrevX);
      fy = thing->PrevY + FixedMul (frac, thing->y - thing->PrevY);
      fz = thing->PrevZ + FixedMul (frac, thing->z - thing->PrevZ);
   }
   else
   {
      fx = thing->x;
      fy = thing->y;
      fz = thing->z;
   }

   /* Dynamic point lights: raise the whole sprite's light level by the lights
    * reaching the thing's centre, and capture their boost-weighted chroma so
    * the sprite can be tinted toward a coloured light (white lights leave the
    * tint zero).  One boost per sprite covers the voxel and patch paths. */
   spr_tr = spr_tg = spr_tb = 0;
   if (R_DynLightsActive())
   {
      int dyn = R_DynLightBoost(fx >> FRACBITS, fy >> FRACBITS,
                                (fz + (thing->height >> 1)) >> FRACBITS);
      if (dyn)
      {
         lightlevel += dyn;
         if (lightlevel > 255)
            lightlevel = 255;
      }
      spr_tr = dl_tint_r >> DL_TINT_SHIFT;
      spr_tg = dl_tint_g >> DL_TINT_SHIFT;
      spr_tb = dl_tint_b >> DL_TINT_SHIFT;
   }

   tr_x = fx - viewx;
   tr_y = fy - viewy;

   gxt = FixedMul(tr_x,viewcos);
   gyt = -FixedMul(tr_y,viewsin);

   tz = gxt-gyt;

   // thing is behind view plane?
   if (tz < MINZ)
      return;

   gxt = -FixedMul(tr_x,viewsin);
   gyt = FixedMul(tr_y,viewcos);
   tx = -(gyt+gxt);

   // too far off the side?
   // (cull before the FixedDiv below: tx/tz are already known here, so a
   // side-culled sprite -- common on wide-open maps with many things off
   // to the sides -- skips the 64-bit projection divide it would never
   // use.  The cull condition is unchanged, so the set of rendered sprites
   // and the output are identical.)
   if (D_abs(tx)>(tz<<2))
      return;

   xscale = FixedDiv(projectionx, tz);

   /* DECORATE Scale: multiply the projected sprite size by the actor's scale.
    * xscale drives the horizontal extent (x1/x2 below) and is mirrored into
    * vis->scale for the vertical, so scaling it here shrinks the billboard
    * uniformly and makes the per-column texture step (iscale) coarser -- which
    * is exactly what stops an oversized native sprite from smearing.  A zero
    * (vanilla) or FRACUNIT scale leaves xscale untouched, so vanilla and
    * unscaled actors are bit-exact. */
   if (thing->info && thing->info->spritescale > 0 &&
       thing->info->spritescale != FRACUNIT)
      xscale = FixedMul(xscale, thing->info->spritescale);

   // Do not attempt to render special TNT1 invisible sprite
   if (thing->sprite == SPR_TNT1) return;

   /* A DSDHacked thing can carry a sprite index past the built sprite
    * table (numsprites), e.g. an extended projectile whose sprite was
    * never defined.  Indexing sprites[]/sprnames[] with it reads out of
    * bounds; the old fallback block then passed a garbage sprnames[]
    * pointer to I_Error's "%s" and crashed in strlen.  Don't render a
    * thing whose sprite is not a real table entry. */
   if ((unsigned)thing->sprite >= (unsigned)numsprites)
      return;

   // decide which patch to use for sprite relative to player
   sprdef = &sprites[thing->sprite];

   /* If the sprite has no frames (an extended sprite whose name was never
    * registered, or whose lumps are absent), there is nothing to draw.
    * The old code substituted states[S_NULL].sprite (SPR_TROO) -- which
    * made such things render as an imp -- and worse, wrote that into the
    * shared sprites[] entry, permanently corrupting it for every other
    * actor using the same sprite.  Just skip rendering instead. */
   if (!sprdef->numframes || !sprdef->spriteframes)
      return;

   /* Voxel sprites: if this thing's sprite is bound to a KVX model, emit a
    * voxel vissprite (a 3D box footprint) instead of the flat billboard.
    * The model is centred on the thing via its pivot; its screen extent
    * comes from the horizontal radius (max of the two ground dimensions,
    * since the model yaws) and the vertical span, both 1 map unit per
    * voxel.  The rasteriser (a later stage) draws it; here we only place
    * and bound it so it sorts among the other vissprites. */
   {
      const voxel_model_t *vox = U_VoxelForSprite(thing->sprite);
      if (vox)
      {
         fixed_t rad  = ((vox->xsiz > vox->ysiz ? vox->xsiz : vox->ysiz)
                         << FRACBITS) / 2;
         fixed_t half = FixedMul(rad, xscale);
         int vx1 = (centerxfrac - half) >> FRACBITS;
         int vx2 = ((centerxfrac + half) >> FRACBITS) - 1;
         fixed_t vgzt;

         if (vx1 > viewwidth || vx2 < 0)
            return;

         /* top of the model in world z: thing origin plus the full vertical
          * span (1 map unit per voxel).  The rasteriser refines per column. */
         vgzt = fz + ((fixed_t)vox->zsiz << FRACBITS);

         vis = R_NewVisSprite();
         vis->heightsec = thing->subsector->sector->heightsec;
         vis->mobjflags = thing->flags;
         vis->xlat      = thing->translation;
         vis->scale     = FixedDiv(projectiony, tz);
         vis->gx        = fx;
         vis->gy        = fy;
         vis->gz        = fz;
         vis->gzt       = vgzt;
         vis->texturemid = vis->gzt - viewz;
         vis->x1 = vx1 < 0 ? 0 : vx1;
         vis->x2 = vx2 >= viewwidth ? viewwidth - 1 : vx2;
         vis->startfrac = 0;
         vis->xiscale   = 0;
         vis->patch     = -1;
         vis->voxel     = vox;
         /* model yaw in world space; the rasteriser rotates the model's
          * horizontal cells by it, then the view transform handles the
          * camera.  Offset by ANG90 so the KVX +x axis points along the
          * actor's facing. */
         vis->voxangle  = thing->angle;
         vis->tint_r = vis->tint_g = vis->tint_b = 0;

         if (!raven && (thing->flags & MF_SHADOW))
            vis->colormap = NULL;
         else if (fixedcolormap)
            vis->colormap = fixedcolormap;
         else if (thing->frame & FF_FULLBRIGHT)
            vis->colormap = fullcolormap;
         else
            vis->colormap = R_ColourMap(lightlevel, xscale);
         return;
      }
   }

   sprframe = &sprdef->spriteframes[thing->frame & FF_FRAMEMASK];

   if (sprframe->rotate)
   {
      // choose a different rotation based on player view
      angle_t ang = R_PointToAngle(fx, fy);
      unsigned rot = (ang-thing->angle+(unsigned)(ANG45/2)*9)>>29;
      lump = sprframe->lump[rot];
      flip = (dbool) sprframe->flip[rot];
   }
   else
   {
      // use single rotation for all views
      lump = sprframe->lump[0];
      flip = (dbool) sprframe->flip[0];
   }

   /* A DSDHacked sprite frame may name a rotation that was never installed
    * (sentinel lump -1), or carry an out-of-range lump from a partial
    * sprite in the WAD.  R_CachePatchNum would then return NULL (or index
    * the patch table out of bounds), and the patch->width dereference below
    * crashed.  Skip rendering a sprite whose lump is not a real entry. */
   if (lump < 0 || lump >= numspritelumps)
      return;

   {
      const rpatch_t* patch = R_CachePatchNum(lump+firstspritelump);

      /* calculate edges of the shape
       * cph 2003/08/1 - fraggle points out that this offset must be flipped
       * if the sprite is flipped; e.g. FreeDoom imp is messed up by this. */
      if (flip) {
         tx -= (patch->width - patch->leftoffset) << FRACBITS;
      } else {
         tx -= patch->leftoffset << FRACBITS;
      }
      x1 = (centerxfrac + FixedMul(tx,xscale)) >> FRACBITS;

      tx += patch->width<<FRACBITS;
      x2 = ((centerxfrac + FixedMul (tx,xscale) ) >> FRACBITS) - 1;

      gzt = fz + (patch->topoffset << FRACBITS);
      width = patch->width;
      R_UnlockPatchNum(lump+firstspritelump);
   }

   // off the side?
   if (x1 > viewwidth || x2 < 0)
      return;

   // killough 4/9/98: clip things which are out of view due to height
   // e6y: fix of hanging decoration disappearing in Batman Doom MAP02
   // centeryfrac -> viewheightfrac
   if (fz  > viewz + FixedDiv(viewheightfrac, xscale) ||
         gzt < viewz - FixedDiv(viewheightfrac-viewheight, xscale))
      return;

   // killough 3/27/98: exclude things totally separated
   // from the viewer, by either water or fake ceilings
   // killough 4/11/98: improve sprite clipping for underwater/fake ceilings

   heightsec = thing->subsector->sector->heightsec;

   if (heightsec != -1)   // only clip things which are in special sectors
   {
      int phs = viewplayer->mo->subsector->sector->heightsec;
      if (phs != -1 && viewz < sectors[phs].floorheight ?
            fz >= sectors[heightsec].floorheight :
            gzt < sectors[heightsec].floorheight)
         return;
      if (phs != -1 && viewz > sectors[phs].ceilingheight ?
            gzt < sectors[heightsec].ceilingheight &&
            viewz >= sectors[heightsec].ceilingheight :
            fz >= sectors[heightsec].ceilingheight)
         return;
   }

   // store information in a vissprite
   vis = R_NewVisSprite ();

   // killough 3/27/98: save sector for special clipping later
   vis->heightsec = heightsec;

   vis->mobjflags = thing->flags;
   vis->xlat      = thing->translation;   /* DECORATE custom colour remap */
   /* DECORATE render style: carry the actor's translucency/additive style and
    * alpha weight onto the vissprite so R_DrawVisSprite can blend it.  Opaque
    * actors (every vanilla type) set these to 0 and draw unchanged.  The slot
    * is reused between frames, so this is assigned unconditionally. */
   if (thing->info)
   {
      vis->translucent = thing->info->render_style;
      vis->alpha       = (unsigned char)thing->info->render_alpha;
   }
   else
   {
      vis->translucent = 0;
      vis->alpha       = 0;
   }
   // proff 11/06/98: Changed for high-res
   vis->scale = FixedDiv(projectiony, tz);
   /* DECORATE Scale: match the vertical projection to the horizontal xscale
    * adjustment above so the sprite scales uniformly (vis->scale feeds
    * spryscale/iscale/texturemid in R_DrawVisSprite). */
   if (thing->info && thing->info->spritescale > 0 &&
       thing->info->spritescale != FRACUNIT)
      vis->scale = FixedMul(vis->scale, thing->info->spritescale);
   vis->gx = fx;
   vis->gy = fy;
   vis->gz = fz;
   vis->gzt = gzt;                          // killough 3/27/98
   vis->texturemid = vis->gzt - viewz;
   vis->x1 = x1 < 0 ? 0 : x1;
   vis->x2 = x2 >= viewwidth ? viewwidth-1 : x2;
   iscale = FixedDiv (FRACUNIT, xscale);

   if (flip)
   {
      vis->startfrac = (width<<FRACBITS)-1;
      vis->xiscale = -iscale;
   }
   else
   {
      vis->startfrac = 0;
      vis->xiscale = iscale;
   }

   if (vis->x1 > x1)
      vis->startfrac += vis->xiscale*(vis->x1-x1);
   vis->patch = lump;
   vis->voxel = NULL;   /* this vissprite is a billboard, not a voxel */
   vis->tint_r = vis->tint_g = vis->tint_b = 0;

   // get light level
   if (!raven && (thing->flags & MF_SHADOW))
      vis->colormap = NULL;             // shadow draw (Doom spectre fuzz);
                                        // Raven shadow things draw lit but
                                        // translucent (see R_DrawVisSprite)
   else if (fixedcolormap)
      vis->colormap = fixedcolormap;      // fixed map
   else if (thing->frame & FF_FULLBRIGHT)
      vis->colormap = fullcolormap;     // full bright  // killough 3/20/98
   else
   {      // diminished light
      vis->colormap = R_ColourMap(lightlevel,xscale);
      vis->tint_r = spr_tr; vis->tint_g = spr_tg; vis->tint_b = spr_tb;
   }
}

//
// R_AddSprites
// During BSP traversal, this adds sprites by sector.
//
// killough 9/18/98: add lightlevel as parameter, fixing underwater lighting
void R_AddSprites(subsector_t* subsec, int lightlevel)
{
   sector_t* sec=subsec->sector;
   mobj_t *thing;

   // BSP is traversed by subsector.
   // A sector might have been split into several
   //  subsectors during BSP building.
   // Thus we check whether its already added.

   if (sec->validcount == validcount)
      return;

   // Well, now it will be done.
   sec->validcount = validcount;

   // Handle all things in sector.

#ifdef PRBOOM_RENDER_PROFILE
   {
      extern double prof_sprproj_usec;
      extern double I_RenderProfileUsec(void);
      double _t0 = I_RenderProfileUsec();
      for (thing = sec->thinglist; thing; thing = thing->snext)
         R_ProjectSprite(thing, lightlevel);
      prof_sprproj_usec += (I_RenderProfileUsec() - _t0);
   }
#else
   for (thing = sec->thinglist; thing; thing = thing->snext)
      R_ProjectSprite(thing, lightlevel);
#endif
}

//
// R_DrawPSprite
//

static void R_DrawPSprite (pspdef_t *psp, int lightlevel)
{
   int           x1, x2;
   spritedef_t   *sprdef;
   spriteframe_t *sprframe;
   int           lump;
   dbool         flip;
   vissprite_t   *vis;
   vissprite_t   avis;
   int           width;
   fixed_t       topoffset;
   fixed_t       sx, sy;

   // decide which patch to use

   /* DSDHacked: a weapon's flash/attack frame can name an extended sprite
    * that has no patches in the loaded WADs (numframes == 0, so
    * spriteframes is NULL), or a sprite index past the built table.  Unlike
    * R_ProjectSprite this routine had no guard, so firing such a weapon
    * dereferenced a NULL spriteframes / out-of-bounds spritedef and crashed.
    * A missing psprite simply isn't drawn. */
   if ((unsigned)psp->state->sprite >= (unsigned)numsprites)
      return;

   sprdef = &sprites[psp->state->sprite];

   if (!sprdef->numframes || !sprdef->spriteframes)
      return;

   sprframe = &sprdef->spriteframes[psp->state->frame & FF_FRAMEMASK];

   lump = sprframe->lump[0];
   flip = (dbool) sprframe->flip[0];

   /* As in R_ProjectSprite: a frame's rotation lump may be the uninstalled
    * sentinel (-1) or out of range for a partial sprite; don't draw it
    * rather than dereference a NULL/garbage patch. */
   if (lump < 0 || lump >= numspritelumps)
      return;

   /* Weapon sprite interpolation: the bob and the raise/lower motion
    * step per tic, so at uncapped framerates the gun judders against
    * the interpolated world.  Blend from the previous tic's position
    * with the same fraction the rest of the frame uses.  A weapon
    * change repositions the sprite discontinuously and swaps to a
    * different graphic, so only blend while the previous state exists
    * and draws the same sprite. */
   sx = psp->sx;
   sy = psp->sy;
   if (movement_smooth)
   {
      const psp_inter_t *old =
         &psp_oldpos[viewplayer - players][psp - viewplayer->psprites];

      if (old->state && old->state->sprite == psp->state->sprite)
      {
         sx = old->sx + FixedMul (tic_vars.frac, psp->sx - old->sx);
         sy = old->sy + FixedMul (tic_vars.frac, psp->sy - old->sy);
      }
   }

   {
      const rpatch_t* patch = R_CachePatchNum(lump+firstspritelump);
      // calculate edges of the shape
      fixed_t       tx;
      tx = sx-160*FRACUNIT;

      tx -= patch->leftoffset<<FRACBITS;
      x1 = (centerxfrac + FixedMul (tx,pspritescale))>>FRACBITS;

      tx += patch->width<<FRACBITS;
      x2 = ((centerxfrac + FixedMul (tx, pspritescale) ) >>FRACBITS) - 1;

      width = patch->width;
      topoffset = patch->topoffset<<FRACBITS;
      R_UnlockPatchNum(lump+firstspritelump);
   }

   // off the side
   if (x2 < 0 || x1 > viewwidth)
      return;

   // store information in a vissprite
   vis = &avis;
   vis->mobjflags = MF_PLAYERSPRITE;
   /* avis is an uninitialised stack vissprite; R_DrawVisSprite reads the
    * DECORATE render-style fields and the dynamic-light tint, none of which
    * the weapon uses, so clear them here.  Left as stack garbage, a nonzero
    * translucent would draw the weapon alpha-blended (often to invisibility). */
   vis->translucent = 0;
   vis->alpha       = 0;
   vis->tint_r = vis->tint_g = vis->tint_b = 0;
   vis->xlat      = NULL;
   // killough 12/98: fix psprite positioning problem
   vis->texturemid = (BASEYCENTER<<FRACBITS) /* +  FRACUNIT/2 */ -
      (sy-topoffset);

   /* Heretic draws weapons lower when the status bar is hidden (full view).
    * Vanilla/dsda apply a per-weapon downward offset in that case; without
    * it the weapon floats at its status-bar-up position, leaving it
    * hovering in mid-screen at fullscreen.  Indices follow the Heretic
    * readyweapon order (staff, gold wand, crossbow, blaster, skull rod,
    * phoenix rod, mace, gauntlets, beak).  R_FullView is viewheight ==
    * SCREENHEIGHT (no status bar). */
   if (raven && viewheight == SCREENHEIGHT)
   {
      static const fixed_t heretic_psprite_sy[NUMWEAPONS] =
      {
         0,              /* staff      */
         5  * FRACUNIT,  /* gold wand  */
         15 * FRACUNIT,  /* crossbow   */
         15 * FRACUNIT,  /* blaster    */
         15 * FRACUNIT,  /* skull rod  */
         15 * FRACUNIT,  /* phoenix rod*/
         15 * FRACUNIT,  /* mace       */
         15 * FRACUNIT,  /* gauntlets  */
         15 * FRACUNIT   /* beak       */
      };
      int rw = viewplayer->readyweapon;
      if (rw >= 0 && rw < NUMWEAPONS)
         vis->texturemid -= heretic_psprite_sy[rw];
   }

   vis->x1 = x1 < 0 ? 0 : x1;
   vis->x2 = x2 >= viewwidth ? viewwidth-1 : x2;
   // proff 11/06/98: Added for high-res
   vis->scale = pspriteyscale;

   if (flip)
   {
      vis->xiscale = -pspriteiscale;
      vis->startfrac = (width<<FRACBITS)-1;
   }
   else
   {
      vis->xiscale = pspriteiscale;
      vis->startfrac = 0;
   }

   if (vis->x1 > x1)
      vis->startfrac += vis->xiscale*(vis->x1-x1);

   vis->patch = lump;
   vis->voxel = NULL;

   if (raven && (viewplayer->mo->flags & MF_SHADOW ||
                 viewplayer->mo->flags2 & MF2_DONTDRAW ||
                 viewplayer->powers[pw_invisibility]))
   {
      /* Raven: an invisible player's weapon draws translucent, fainter when
       * the player is fully cloaked (after dsda-doom). */
      if (viewplayer->mo->flags2 & MF2_DONTDRAW)
         vis->mobjflags |= MF_SHADOW;
      else
         vis->mobjflags |= MF_ALTSHADOW;
      if (fixedcolormap)
         vis->colormap = fixedcolormap;
      else
         vis->colormap = R_ColourMap(lightlevel,
               FixedMul(pspritescale, 0x2b000));
   }
   else if (viewplayer->powers[pw_invisibility] > 4*32
         || viewplayer->powers[pw_invisibility] & 8)
      vis->colormap = NULL;                    // shadow draw
   else if (fixedcolormap)
      vis->colormap = fixedcolormap;           // fixed color
   else if (psp->state->frame & FF_FULLBRIGHT)
      vis->colormap = fullcolormap;            // full bright // killough 3/20/98
   else
      // add a fudge factor to better match the original game
      vis->colormap = R_ColourMap(lightlevel,
            FixedMul(pspritescale, 0x2b000));  // local light

   R_DrawVisSprite(vis, vis->x1, vis->x2);
}

//
// R_DrawPlayerSprites
//

void R_DrawPlayerSprites(void)
{
   int i, lightlevel = viewplayer->mo->subsector->sector->lightlevel;
   pspdef_t *psp;

   // clip to screen bounds
   mfloorclip = screenheightarray;
   mceilingclip = negonearray;

   // add all active psprites
   for (i=0, psp=viewplayer->psprites; i<NUMPSPRITES; i++,psp++)
      if (psp->state)
         R_DrawPSprite (psp, lightlevel);
}

//
// R_SortVisSprites
//
// Rewritten by Lee Killough to avoid using unnecessary
// linked lists, and to use faster sorting algorithm.
//

// killough 9/2/98: merge sort

static void msort(vissprite_t **s, vissprite_t **t, int n)
{
   if (n >= 16)
   {
      int n1 = n/2, n2 = n - n1;
      vissprite_t **s1 = s, **s2 = s + n1, **d = t;

      msort(s1, t, n1);
      msort(s2, t, n2);

      while ((*s1)->scale > (*s2)->scale ?
            (*d++ = *s1++, --n1) : (*d++ = *s2++, --n2));

      if (n2)
         memcpy(d, s2, n2 * sizeof(void *));
      else
         memcpy(d, s1, n1 * sizeof(void *));

      memcpy(s, t, n * sizeof(void *));
   }
   else
   {
      int i;
      for (i = 1; i < n; i++)
      {
         vissprite_t *temp = s[i];
         if (s[i-1]->scale < temp->scale)
         {
            int j = i;
            while ((s[j] = s[j-1])->scale < temp->scale && --j);
            s[j] = temp;
         }
      }
   }
}

void R_SortVisSprites (void)
{
   if (num_vissprite)
   {
      int i = num_vissprite;

      // If we need to allocate more pointers for the vissprites,
      // allocate as many as were allocated for sprites -- killough
      // killough 9/22/98: allocate twice as many

      if (num_vissprite_ptrs < num_vissprite*2)
      {
         free(vissprite_ptrs);  // better than realloc -- no preserving needed
         vissprite_ptrs = malloc((num_vissprite_ptrs = num_vissprite_alloc*2)
               * sizeof *vissprite_ptrs);
      }

      while (--i>=0)
         vissprite_ptrs[i] = vissprites+i;

      // killough 9/22/98: replace qsort with merge sort, since the keys
      // are roughly in order to begin with, due to BSP rendering.

      msort(vissprite_ptrs, vissprite_ptrs + num_vissprite, num_vissprite);
   }
}

//
// R_DrawSprite
//

static void R_DrawSprite (vissprite_t* spr)
{
   drawseg_t *ds;
   int     clipbot[MAX_SCREENWIDTH]; // killough 2/8/98: // dropoff overflow
   int     cliptop[MAX_SCREENWIDTH]; // change to MAX_*  // dropoff overflow
   int     x;
   int     r1;
   int     r2;
   fixed_t scale;
   fixed_t lowscale;

   for (x = spr->x1 ; x<=spr->x2 ; x++)
      clipbot[x] = cliptop[x] = -2;

   // Scan drawsegs from end to start for obscuring segs.
   // The first drawseg that has a greater scale is the clip seg.

   // Modified by Lee Killough:
   // (pointer check was originally nonportable
   // and buggy, by going past LEFT end of array):

   //    for (ds=ds_p-1 ; ds >= drawsegs ; ds--)    old buggy code

   // e6y: optimization -- walk only the pre-partitioned drawseg X-range
   //      that R_DrawMasked selected for this sprite, rather than every
   //      drawseg in the frame.
   if (drawsegs_xrange_size)
   {
      const drawseg_xrange_item_t *last =
         &drawsegs_xrange[drawsegs_xrange_count - 1];
      drawseg_xrange_item_t *curr = &drawsegs_xrange[-1];

      while (++curr <= last)
      {
         // determine if the drawseg obscures the sprite
         if (curr->x1 > spr->x2 || curr->x2 < spr->x1)
            continue;      // does not cover sprite

         ds = curr->user;

         r1 = ds->x1 < spr->x1 ? spr->x1 : ds->x1;
         r2 = ds->x2 > spr->x2 ? spr->x2 : ds->x2;

         if (ds->scale1 > ds->scale2)
         {
            lowscale = ds->scale2;
            scale = ds->scale1;
         }
         else
         {
            lowscale = ds->scale1;
            scale = ds->scale2;
         }

         if (scale < spr->scale || (lowscale < spr->scale &&
                  !R_PointOnSegSide (spr->gx, spr->gy, ds->curline)))
         {
            if (ds->maskedtexturecol)       // masked mid texture?
               R_RenderMaskedSegRange(ds, r1, r2);
            /* This seg's wall is behind the sprite, so any decal on it in
             * this x-range must be drawn now and then overdrawn by the
             * sprite -- otherwise the final decal pass would paint it on
             * top of the sprite.  The per-column guard keeps the final
             * pass from redrawing these columns. */
            R_DrawDecalsForSeg(ds, r1, r2);
            continue;               // seg is behind sprite
         }

         // clip this piece of the sprite
         // killough 3/27/98: optimized and made much shorter

         if (ds->silhouette&SIL_BOTTOM && spr->gz < ds->bsilheight) //bottom sil
            for (x=r1 ; x<=r2 ; x++)
               if (clipbot[x] == -2)
                  clipbot[x] = ds->sprbottomclip[x];

         if (ds->silhouette&SIL_TOP && spr->gzt > ds->tsilheight)   // top sil
            for (x=r1 ; x<=r2 ; x++)
               if (cliptop[x] == -2)
                  cliptop[x] = ds->sprtopclip[x];
      }
   }


   // killough 3/27/98:
   // Clip the sprite against deep water and/or fake ceilings.
   // killough 4/9/98: optimize by adding mh
   // killough 4/11/98: improve sprite clipping for underwater/fake ceilings
   // killough 11/98: fix disappearing sprites

   if (spr->heightsec != -1)  // only things in specially marked sectors
   {
      fixed_t h,mh;
      int phs = viewplayer->mo->subsector->sector->heightsec;
      if ((mh = sectors[spr->heightsec].floorheight) > spr->gz &&
            (h = centeryfrac - FixedMul(mh-=viewz, spr->scale)) >= 0 &&
            (h >>= FRACBITS) < viewheight) {
         if (mh <= 0 || (phs != -1 && viewz > sectors[phs].floorheight))
         {                          // clip bottom
            for (x=spr->x1 ; x<=spr->x2 ; x++)
               if (clipbot[x] == -2 || h < clipbot[x])
                  clipbot[x] = h;
         }
         else                        // clip top
            if (phs != -1 && viewz <= sectors[phs].floorheight) // killough 11/98
               for (x=spr->x1 ; x<=spr->x2 ; x++)
                  if (cliptop[x] == -2 || h > cliptop[x])
                     cliptop[x] = h;
      }

      if ((mh = sectors[spr->heightsec].ceilingheight) < spr->gzt &&
            (h = centeryfrac - FixedMul(mh-viewz, spr->scale)) >= 0 &&
            (h >>= FRACBITS) < viewheight) {
         if (phs != -1 && viewz >= sectors[phs].ceilingheight)
         {                         // clip bottom
            for (x=spr->x1 ; x<=spr->x2 ; x++)
               if (clipbot[x] == -2 || h < clipbot[x])
                  clipbot[x] = h;
         }
         else                       // clip top
            for (x=spr->x1 ; x<=spr->x2 ; x++)
               if (cliptop[x] == -2 || h > cliptop[x])
                  cliptop[x] = h;
      }
   }
   // killough 3/27/98: end special clipping for deep water / fake ceilings

   // all clipping has been performed, so draw the sprite
   // check for unclipped columns

   for (x = spr->x1 ; x<=spr->x2 ; x++) {
      if (clipbot[x] == -2)
         clipbot[x] = viewheight;

      if (cliptop[x] == -2)
         cliptop[x] = -1;
   }

   mfloorclip = clipbot;
   mceilingclip = cliptop;
   R_DrawVisSprite (spr, spr->x1, spr->x2);
}

//
// R_DrawMasked
//

void R_DrawMasked(void)
{
   int i;
   drawseg_t *ds;
   int cx = SCREENWIDTH / 2;

   R_SortVisSprites();

   /* reset the per-column decal guard before any decals are emitted (the
    * interleaved per-sprite passes below are the first to draw them) */
   R_DecalsBeginFrame();

   // e6y: build the per-frame drawseg X-range lists used by R_DrawSprite.
   // Only silhouette/masked drawsegs can clip sprites, so collect those into
   // range 0 (all), range 1 (touches left half), range 2 (touches right
   // half). Skipped entirely when there are no sprites to draw.
   for (i = 0; i < DS_RANGES_COUNT; i++)
      drawsegs_xranges[i].count = 0;

   if (num_vissprite > 0)
   {
      if (drawsegs_xrange_size < maxdrawsegs)
      {
         drawsegs_xrange_size = 2 * maxdrawsegs;
         for (i = 0; i < DS_RANGES_COUNT; i++)
            drawsegs_xranges[i].items = Z_Realloc(
               drawsegs_xranges[i].items,
               drawsegs_xrange_size * sizeof(drawsegs_xranges[i].items[0]),
               PU_STATIC, NULL);
      }

      for (ds = ds_p; ds-- > drawsegs; )
      {
         if (ds->silhouette || ds->maskedtexturecol)
         {
            drawsegs_xranges[0].items[drawsegs_xranges[0].count].x1 = ds->x1;
            drawsegs_xranges[0].items[drawsegs_xranges[0].count].x2 = ds->x2;
            drawsegs_xranges[0].items[drawsegs_xranges[0].count].user = ds;

            if (ds->x1 < cx)
            {
               drawsegs_xranges[1].items[drawsegs_xranges[1].count] =
                  drawsegs_xranges[0].items[drawsegs_xranges[0].count];
               drawsegs_xranges[1].count++;
            }
            if (ds->x2 >= cx)
            {
               drawsegs_xranges[2].items[drawsegs_xranges[2].count] =
                  drawsegs_xranges[0].items[drawsegs_xranges[0].count];
               drawsegs_xranges[2].count++;
            }

            drawsegs_xranges[0].count++;
         }
      }
   }

   // 3D-floor slab faces first, so sprites in front draw over them
   for (ds=ds_p ; ds-- > drawsegs ; )
      if (ds->curline && ds->curline->backsector &&
          ds->curline->backsector->ffloors)
         R_RenderThickSides(ds);

   // draw all vissprites back to front

   for (i = num_vissprite ;--i>=0; )
   {
      vissprite_t *spr = vissprite_ptrs[i];

      if (spr->x2 < cx)
      {
         drawsegs_xrange       = drawsegs_xranges[1].items;
         drawsegs_xrange_count = drawsegs_xranges[1].count;
      }
      else if (spr->x1 >= cx)
      {
         drawsegs_xrange       = drawsegs_xranges[2].items;
         drawsegs_xrange_count = drawsegs_xranges[2].count;
      }
      else
      {
         drawsegs_xrange       = drawsegs_xranges[0].items;
         drawsegs_xrange_count = drawsegs_xranges[0].count;
      }

      R_DrawSprite(spr);         // killough
   }

   // render any remaining masked mid textures

   // Modified by Lee Killough:
   // (pointer check was originally nonportable
   // and buggy, by going past LEFT end of array):

   //    for (ds=ds_p-1 ; ds >= drawsegs ; ds--)    old buggy code

   for (ds=ds_p ; ds-- > drawsegs ; )  // new -- killough
      if (ds->maskedtexturecol)
         R_RenderMaskedSegRange(ds, ds->x1, ds->x2);

   /* wall decals: the interleaved passes in R_DrawSprite already drew the
    * columns of each seg that sat behind a sprite (and the sprite drew over
    * them); this final sweep covers the remaining columns -- those no
    * sprite occluded.  The per-column guard makes that a no-op where the
    * interleave already ran, so there is no overdraw. */
   for (ds=ds_p ; ds-- > drawsegs ; )
      R_DrawDecalsForSeg(ds, ds->x1, ds->x2);


   // draw the psprites on top of everything
   //  but does not draw on side views
   if (!viewangleoffset && !viewpitchoffset)
      R_DrawPlayerSprites ();
}
