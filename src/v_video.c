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
 *  Gamma correction LUT stuff.
 *  Color range translation support
 *  Functions to draw patches (by post) directly to screen.
 *  Functions to blit a block to the screen.
 *
 *-----------------------------------------------------------------------------
 */

#include "doomdef.h"
#include "r_main.h"
#include "r_draw.h"
#include "m_bbox.h"
#include "w_wad.h"   /* needed for color translation lump lookup */
#include "v_video.h"
#include "i_video.h"
#include "r_filter.h"
#include "lprintf.h"

// Each screen is [SCREENWIDTH*SCREENHEIGHT];
screeninfo_t screens[NUM_SCREENS];

/* jff 4/24/98 initialize this at runtime */
const uint8_t *colrngs[CR_LIMIT];

int usegamma;

/*
 * V_InitColorTranslation
 *
 * Loads the color translation tables from predefined lumps at game start
 * No return
 *
 * Used for translating text colors from the red palette range
 * to other colors. The first nine entries can be used to dynamically
 * switch the output of text color thru the HUlib_drawText routine
 * by embedding ESCn in the text to obtain color n. Symbols for n are
 * provided in v_video.h.
 *
 * cphipps - constness of crdef_t stuff fixed
 */

typedef struct {
  const char *name;
  const uint8_t **map;
} crdef_t;

// killough 5/2/98: table-driven approach
static const crdef_t crdefs[] = {
  {"CRBRICK",  &colrngs[CR_BRICK ]},
  {"CRTAN",    &colrngs[CR_TAN   ]},
  {"CRGRAY",   &colrngs[CR_GRAY  ]},
  {"CRGREEN",  &colrngs[CR_GREEN ]},
  {"CRBROWN",  &colrngs[CR_BROWN ]},
  {"CRGOLD",   &colrngs[CR_GOLD  ]},
  {"CRRED",    &colrngs[CR_RED   ]},
  {"CRBLUE",   &colrngs[CR_BLUE  ]},
  {"CRORANGE", &colrngs[CR_ORANGE]},
  {"CRYELLOW", &colrngs[CR_YELLOW]},
  {"CRBLUE2",  &colrngs[CR_BLUE2]},
  {NULL}
};

// killough 5/2/98: tiny engine driven by table above
void V_InitColorTranslation(void)
{
  register const crdef_t *p;
  for (p=crdefs; p->name; p++)
    *p->map = W_CacheLumpName(p->name);
}

//
// V_CopyRect
//
// Copies a source rectangle in a screen buffer to a destination
// rectangle in another screen buffer. Source origin in srcx,srcy,
// destination origin in destx,desty, common size in width and height.
// Source buffer specfified by srcscrn, destination buffer by destscrn.
//
// Marks the destination rectangle on the screen dirty.
//
// No return.
//
void V_CopyRect(int srcx, int srcy, int srcscrn, int width,
                int height, int destx, int desty, int destscrn,
                enum patch_translation_e flags)
{
  uint8_t *src;
  uint8_t *dest;

  if (flags & VPT_STRETCH)
  {
    srcx=srcx*SCREENWIDTH/320;
    srcy=srcy*SCREENHEIGHT/200;
    width=width*SCREENWIDTH/320;
    height=height*SCREENHEIGHT/200;
    destx=destx*SCREENWIDTH/320;
    desty=desty*SCREENHEIGHT/200;
  }

  src = screens[srcscrn].data + SURFACE_BYTE_PITCH * srcy + srcx * SURFACE_PIXEL_DEPTH;
  dest = screens[destscrn].data + SURFACE_BYTE_PITCH * desty + destx * SURFACE_PIXEL_DEPTH;

  for ( ; height>0 ; height--)
    {
      memcpy (dest, src, width * SURFACE_PIXEL_DEPTH);
      src += SURFACE_BYTE_PITCH;
      dest += SURFACE_BYTE_PITCH;
    }
}

/*
 * V_DrawBackground tiles a 64x64 patch over the entire screen, providing the
 * background for the Help and Setup screens, and plot text betwen levels.
 * cphipps - used to have M_DrawBackground, but that was used the framebuffer
 * directly, so this is my code from the equivalent function in f_finale.c
 */

// FIXME: restore v_video.inl
#define GETCOL16(col) (VID_PAL16(col, VID_COLORWEIGHTMASK))

// draw a stretched 64x64 flat to the top left corner of the screen
#define V_DRAWFLAT(SCRN, TYPE, GETCOL) { \
  const int width = (64 * SCREENWIDTH) / 320; \
  int height = (64 * SCREENHEIGHT) / 200; \
  fixed_t dx = (320 << FRACBITS) / SCREENWIDTH; \
  TYPE *dest = (TYPE *)screens[SCRN].data; \
  \
  while (height--) { \
    const uint8_t *const src_row = src + 64*((height*200)/SCREENHEIGHT); \
    TYPE *const dst_row = dest + SURFACE_SHORT_PITCH * height; \
    int x; \
    fixed_t tx; \
    \
    for (x=0, tx=0; x<width; x++, tx += dx) { \
      uint8_t col = src_row[tx >> FRACBITS]; \
      dst_row[x] = GETCOL(col); \
    } \
  } \
}

void V_DrawBackground(const char* flatname, int scrn)
{
  /* erase the entire screen to a tiled background */
  int         x,y;
  int         lump;
  const int   w = (64*SCREENWIDTH/320), h = (64*SCREENHEIGHT/200);

  // killough 4/17/98:
  const uint8_t *src = W_CacheLumpNum(lump = firstflat + R_FlatNumForName(flatname));

  /* V_DrawBlock(0, 0, scrn, 64, 64, src, 0); */

  V_DRAWFLAT(scrn, int16_t, GETCOL16);

  /* end V_DrawBlock */

  for (y=0 ; y<SCREENHEIGHT ; y+=h)
    for (x=y ? 0 : w; x<SCREENWIDTH ; x+=w)
      V_CopyRect(0, 0, scrn, ((SCREENWIDTH-x) < w) ? (SCREENWIDTH-x) : w,
     ((SCREENHEIGHT-y) < h) ? (SCREENHEIGHT-y) : h, x, y, scrn, VPT_NONE);
  W_UnlockLumpNum(lump);
}

//
// V_Init
//
// Allocates the 4 full screen buffers in low DOS memory
// No return
//

void V_Init (void)
{
  int  i;

  // reset the all
  for (i = 0; i<NUM_SCREENS; i++) {
    screens[i].data = NULL;
    screens[i].not_on_heap = FALSE;
    screens[i].height = 0;
  }
}

//
// V_DrawMemPatch
//
// CPhipps - unifying patch drawing routine, handles all cases and combinations
//  of stretching, flipping and translating
//
// This function is big, hopefully not too big that gcc can't optimise it well.
// In fact it packs pretty well, there is no big performance lose for all this merging;
// the inner loops themselves are just the same as they always were
// (indeed, laziness of the people who wrote the 'clones' of the original V_DrawPatch
//  means that their inner loops weren't so well optimised, so merging code may even speed them).
//
//
static void V_DrawMemPatch(int x, int y, int scrn, const rpatch_t *patch,
        int cm, enum patch_translation_e flags)
{
   int   left, right, top, bottom;
   R_DrawColumn_f colfunc;
   draw_column_vars_t dcvars;
   draw_vars_t olddrawvars = drawvars;
   int col = 0;
   int w   = (patch->width << 16) - 1; // CPhipps - -1 for faster flipping
   int DX  = (SCREENWIDTH<<16) / 320;
   int DXI = (320<<16) / SCREENWIDTH;
   int DY = (SCREENHEIGHT<<16) / 200;
   int DYI = (200<<16) / SCREENHEIGHT;
   const uint8_t *trans = translationtables + 256*((cm-CR_LIMIT)-1);

   if (cm<CR_LIMIT)
      trans=colrngs[cm];

   y -= patch->topoffset;
   x -= patch->leftoffset;

   // CPhipps - auto-no-stretch if not high-res
   if (flags & VPT_STRETCH)
      if ((SCREENWIDTH==320) && (SCREENHEIGHT==200))
         flags &= ~VPT_STRETCH;

   // CPhipps - null translation pointer => no translation
   if (!trans)
      flags &= ~VPT_TRANS;

   // CPhipps - move stretched patch drawing code here
   //         - reformat initialisers, move variables into inner blocks

   R_SetDefaultDrawColumnVars(&dcvars);

   drawvars.short_topleft = (uint16_t*)screens[scrn].data;
   drawvars.int_topleft   = (uint32_t*)screens[scrn].data;

   if (!(flags & VPT_STRETCH))
   {
      DX = 1 << 16;
      DXI = 1 << 16;
      DY = 1 << 16;
      DYI = 1 << 16;
   }

   colfunc = R_GetDrawColumnFunc(RDC_PIPELINE_STANDARD, drawvars.filterpatch, RDRAW_FILTER_NONE);

   if (flags & VPT_TRANS)
   {
      colfunc = R_GetDrawColumnFunc(RDC_PIPELINE_TRANSLATED, drawvars.filterpatch, RDRAW_FILTER_NONE);
      dcvars.translation = trans;
   }

   left   = ( x * DX ) >> FRACBITS;
   top    = ( y * DY ) >> FRACBITS;
   right  = ( (x + patch->width) * DX ) >> FRACBITS;
   bottom = ( (y + patch->height) * DY ) >> FRACBITS;

   dcvars.texheight     = patch->height;
   dcvars.iscale        = DYI;
   dcvars.drawingmasked = MAX(patch->width, patch->height) > 8;
   dcvars.edgetype      = drawvars.patch_edges;

   if (drawvars.filterpatch == RDRAW_FILTER_LINEAR)
   {
      // bias the texture u coordinate
      if (patch->isNotTileable)
         col = -(FRACUNIT>>1);
      else
         col = (patch->width<<FRACBITS)-(FRACUNIT>>1);
   }

   for (dcvars.x=left; dcvars.x<right; dcvars.x++, col+= DXI)
   {
      int i;
      const int colindex          = (flags & VPT_FLIP) ? ((w - col)>>16): (col>>16);
      const rcolumn_t *column     = R_GetPatchColumn(patch, colindex);
      const rcolumn_t *prevcolumn = R_GetPatchColumn(patch, colindex-1);
      const rcolumn_t *nextcolumn = R_GetPatchColumn(patch, colindex+1);

      // ignore this column if it's to the left of our clampRect
      if (dcvars.x < 0)
         continue;
      if (dcvars.x >= SCREENWIDTH)
         break;

      dcvars.texu = ((flags & VPT_FLIP) 
            ? ((patch->width<<FRACBITS)-col) : col) % (patch->width<<FRACBITS);

      // step through the posts in a column
      for (i=0; i<column->numPosts; i++) {
         const rpost_t *post = &column->posts[i];
         int yoffset = 0;

         dcvars.yl = (((y + post->topdelta) * DY)>>FRACBITS);
         dcvars.yh = (((y + post->topdelta + post->length) * DY - (FRACUNIT>>1))>>FRACBITS);
         dcvars.edgeslope = post->slope;

         if ((dcvars.yh < 0) || (dcvars.yh < top))
            continue;
         if ((dcvars.yl >= SCREENHEIGHT) || (dcvars.yl >= bottom))
            continue;

         if (dcvars.yh >= bottom) {
            dcvars.yh = bottom-1;
            dcvars.edgeslope &= ~RDRAW_EDGESLOPE_BOT_MASK;
         }
         if (dcvars.yh >= SCREENHEIGHT) {
            dcvars.yh = SCREENHEIGHT-1;
            dcvars.edgeslope &= ~RDRAW_EDGESLOPE_BOT_MASK;
         }

         if (dcvars.yl < 0) {
            yoffset = 0-dcvars.yl;
            dcvars.yl = 0;
            dcvars.edgeslope &= ~RDRAW_EDGESLOPE_TOP_MASK;
         }
         if (dcvars.yl < top) {
            yoffset = top-dcvars.yl;
            dcvars.yl = top;
            dcvars.edgeslope &= ~RDRAW_EDGESLOPE_TOP_MASK;
         }

         dcvars.source = column->pixels + post->topdelta + yoffset;
         dcvars.prevsource = prevcolumn ? prevcolumn->pixels + post->topdelta + yoffset: dcvars.source;
         dcvars.nextsource = nextcolumn ? nextcolumn->pixels + post->topdelta + yoffset: dcvars.source;

         dcvars.texturemid = -((dcvars.yl-centery)*dcvars.iscale);

         colfunc(&dcvars);
      }
   }

   R_ResetColumnBuffer();
   drawvars = olddrawvars;
}

// CPhipps - some simple, useful wrappers for that function, for drawing patches from wads

// CPhipps - GNU C only suppresses generating a copy of a function if it is
// static inline; other compilers have different behaviour.
// This inline is _only_ for the function below

void V_DrawNumPatch(int x, int y, int scrn, int lump,
         int cm, enum patch_translation_e flags)
{
  if(lump < 0)
    return I_Error("V_DrawNumPatch: missing lump won't be drawn");
    
  V_DrawMemPatch(x, y, scrn, R_CachePatchNum(lump), cm, flags);
  R_UnlockPatchNum(lump);
}

uint16_t *V_Palette16 = NULL;
static uint16_t *Palettes16 = NULL;
static int currentPaletteIndex = 0;

#define DONT_ROUND_ABOVE 220
//
// V_UpdateTrueColorPalette
//
void V_UpdateTrueColorPalette(void) {
  int i, w, p;
  int paletteNum = currentPaletteIndex;
  static int usegammaOnLastPaletteGeneration = -1;
  int pplump         = W_GetNumForName("PLAYPAL");
  int gtlump         = (W_CheckNumForName)("GAMMATBL",ns_prboom);
  const uint8_t *pal = W_CacheLumpNum(pplump);

  // opengl doesn't use the gamma
  const uint8_t *const gtable = 
    (const uint8_t *)W_CacheLumpNum(gtlump) + 
    (256*(usegamma))
  ;

  int numPals = W_LumpLength(pplump) / (3*256);
  
  if (usegammaOnLastPaletteGeneration != usegamma) {
    if (Palettes16) free(Palettes16);
    Palettes16 = NULL;
    usegammaOnLastPaletteGeneration = usegamma;      
  }
  
  if (!Palettes16)
  {
     // set short palette
     Palettes16 = malloc(numPals*256*sizeof(uint16_t)*VID_NUMCOLORWEIGHTS);
     for (p=0; p<numPals; p++)
     {
        for (i=0; i<256; i++)
        {
           uint8_t r = gtable[pal[(256*p+i)*3+0]];
           uint8_t g = gtable[pal[(256*p+i)*3+1]];
           uint8_t b = gtable[pal[(256*p+i)*3+2]];

           // ideally, we should always round up, but very bright colors
           // overflow the blending adds, so they don't get rounded.
           float roundUpR = (r > DONT_ROUND_ABOVE) ? 0 : 0.5f;
           float roundUpG = (g > DONT_ROUND_ABOVE) ? 0 : 0.5f;
           float roundUpB = (b > DONT_ROUND_ABOVE) ? 0 : 0.5f;

           for (w=0; w<VID_NUMCOLORWEIGHTS; w++)
           {
              float t = (float)(w)/(float)(VID_NUMCOLORWEIGHTS-1);
              int nr  = (int)((r>>3)*t+roundUpR);
              int ng  = (int)((g>>2)*t+roundUpG);
              int nb  = (int)((b>>3)*t+roundUpB);
              Palettes16[((p*256+i)*VID_NUMCOLORWEIGHTS)+w] = (
                    (nr<<11) | (ng<<5) | nb
                    );
           }
        }
     }
  }

  V_Palette16 = Palettes16 + paletteNum*256*VID_NUMCOLORWEIGHTS;
   
  W_UnlockLumpNum(pplump);
  W_UnlockLumpNum(gtlump);
}


//---------------------------------------------------------------------------
// V_DestroyTrueColorPalette
//---------------------------------------------------------------------------
static void V_DestroyTrueColorPalette(void)
{
    if (Palettes16) free(Palettes16);
    Palettes16 = NULL;
    V_Palette16 = NULL;
}

void V_DestroyUnusedTrueColorPalettes(void)
{
   V_DestroyTrueColorPalette();
}

//
// V_SetPalette
//
// CPhipps - New function to set the palette to palette number pal.
// Handles loading of PLAYPAL and calls I_SetPalette

void V_SetPalette(int pal)
{
	currentPaletteIndex = pal;

	I_SetPalette(pal);
	// V_SetPalette can be called as part of the gamma setting before
	// we've loaded any wads, which prevents us from reading the palette - POPE
	if (W_CheckNumForName("PLAYPAL") >= 0)
		V_UpdateTrueColorPalette();
}

//
// V_FillRect
//
// CPhipps - New function to fill a rectangle with a given colour
void V_FillRect(int x, int y, int width, int height, uint8_t colour)
{
  uint16_t *dest = (uint16_t*)screens[0].data + x + y* SURFACE_SHORT_PITCH;
  uint16_t c = VID_PAL16(colour, VID_COLORWEIGHTMASK);
  while (height--)
  {
     memset(dest, c, width * sizeof(uint16_t));
     dest += SURFACE_SHORT_PITCH;
  }
}

const char *default_videomode;

//
// V_InitMode
//
void V_InitMode(void) {
   lprintf(LO_INFO, "V_InitMode: using 16 bit video mode\n");
  R_FilterInit();
}

//
// V_GetNumPixelBits
//
int V_GetNumPixelBits(void) {
    return 16;
}

//
// V_GetPixelDepth
//
int V_GetPixelDepth(void) {
  return 2;
}

//
// V_AllocScreen
//
void V_AllocScreen(screeninfo_t *scrn) {
  if (!scrn->not_on_heap)
    if (( SURFACE_BYTE_PITCH * scrn->height) > 0)
      scrn->data = malloc( SURFACE_BYTE_PITCH * scrn->height);
}

//
// V_AllocScreens
//
void V_AllocScreens(void) {
  int i;

  for (i=0; i<NUM_SCREENS; i++)
    V_AllocScreen(&screens[i]);
}

//
// V_FreeScreen
//
void V_FreeScreen(screeninfo_t *scrn) {
  if (!scrn->not_on_heap) {
    free(scrn->data);
    scrn->data = NULL;
  }
}

//
// V_FreeScreens
//
void V_FreeScreens(void) {
  int i;

  for (i=0; i<NUM_SCREENS; i++)
    V_FreeScreen(&screens[i]);
}

void V_PlotPixel(int scrn, int x, int y, uint8_t color)
{
   ((uint16_t*)screens[scrn].data)[x + SURFACE_SHORT_PITCH *y] = VID_PAL16(color, VID_COLORWEIGHTMASK);
}

//
// WRAP_V_DrawLine()
//
// Draw a line in the frame buffer.
// Classic Bresenham w/ whatever optimizations needed for speed
//
// Passed the frame coordinates of line, and the color to be drawn
// Returns nothing
//
void V_DrawLine(fline_t* fl, int color)
{
  int dx = fl->b.x - fl->a.x;
  int ax = 2 * (dx<0 ? -dx : dx);
  int sx = dx<0 ? -1 : 1;

  int dy = fl->b.y - fl->a.y;
  int ay = 2 * (dy<0 ? -dy : dy);
  int sy = dy<0 ? -1 : 1;

  int x  = fl->a.x;
  int y  = fl->a.y;

  if (ax > ay)
  {
    int d = ay - ax/2;
    while (1)
    {
      V_PlotPixel(0, x,y, (uint8_t)color);
      if (x == fl->b.x) return;
      if (d>=0)
      {
        y += sy;
        d -= ax;
      }
      x += sx;
      d += ay;
    }
  }
  else
  {
    int d = ax - ay/2;
    while (1)
    {
      V_PlotPixel(0, x, y, (uint8_t)color);
      if (y == fl->b.y) return;
      if (d >= 0)
      {
        x += sx;
        d -= ay;
      }
      y += sy;
      d += ax;
    }
  }
}
