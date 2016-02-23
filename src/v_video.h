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
 *  Gamma correction LUT.
 *  Color range translation support
 *  Functions to draw patches (by post) directly to screen.
 *  Functions to blit a block to the screen.
 *
 *-----------------------------------------------------------------------------*/

#ifndef __V_VIDEO__
#define __V_VIDEO__

#include "doomtype.h"
#include "doomdef.h"
// Needed because we are refering to patches.
#include "r_data.h"

//
// VIDEO
//

#define CENTERY     (100) // SCREENHEIGHT/2 = 200/2 = 100

// Screen 0 is the screen updated by I_Update screen.
// Screen 1 is an extra buffer.

// array of pointers to color translation tables
extern const uint8_t *colrngs[];

// symbolic indices into color translation table pointer array
typedef enum
{
  CR_BRICK,   //0
  CR_TAN,     //1
  CR_GRAY,    //2
  CR_GREEN,   //3
  CR_BROWN,   //4
  CR_GOLD,    //5
  CR_RED,     //6
  CR_BLUE,    //7
  CR_ORANGE,  //8
  CR_YELLOW,  //9
  CR_BLUE2,   //10 // proff
  CR_LIMIT    //11 //jff 2/27/98 added for range check
} crange_idx_e;
//jff 1/16/98 end palette color range additions

#define CR_DEFAULT CR_RED   /* default value for out of range colors */

typedef struct {
  uint8_t *data;       // pointer to the screen content
  boolean not_on_heap; // if set, no malloc or free is preformed and
                       // data never set to NULL. Used i.e. with SDL doublebuffer.
  int height;          // the height of the surface, used when mallocing
} screeninfo_t;

#define NUM_SCREENS 6
extern screeninfo_t screens[NUM_SCREENS];
extern int          usegamma;

// Varying bit-depth support -POPE
//
// For bilinear filtering, each palette color is pre-weighted and put in a
// table for fast blending operations. These macros decide how many weights
// to create for each color. The lower the number, the lower the blend
// accuracy, which can produce very bad artifacts in texture filtering.
#define VID_NUMCOLORWEIGHTS 64
#define VID_COLORWEIGHTMASK (VID_NUMCOLORWEIGHTS-1)
#define VID_COLORWEIGHTBITS 6

// Palettes for converting from 8 bit color to 16 and 32 bit. Also
// contains the weighted versions of each palette color for filtering
// operations
extern uint16_t *V_Palette16;

#define VID_PAL16(color, weight) V_Palette16[ (color)*VID_NUMCOLORWEIGHTS + (weight) ]

extern const char *default_videomode;

void V_InitMode(void);

// video mode query interface
int V_GetNumPixelBits(void);
int V_GetPixelDepth(void);

//jff 4/24/98 loads color translation lumps
void V_InitColorTranslation(void);

// Allocates buffer screens, call before R_Init.
void V_Init (void);

// V_CopyRect
extern void V_CopyRect(int srcx,  int srcy,  int srcscrn,
                             int width, int height,
                             int destx, int desty, int destscrn,
                             enum patch_translation_e flags);

extern void V_FillRect_f(int x, int y,
                             int width, int height, uint8_t colour);

// CPhipps - patch drawing
// Consolidated into the 3 really useful functions:

// V_DrawNumPatch - Draws the patch from lump num
extern void V_DrawNumPatch(int x, int y, int scrn,
                                 int lump, int cm,
                                 enum patch_translation_e flags);

// V_DrawNamePatch - Draws the patch from lump "name"
#define V_DrawNamePatch(x,y,s,n,t,f) V_DrawNumPatch(x,y,s,W_GetNumForName(n),t,f)

/* cph -
 * Functions to return width & height of a patch.
 * Doesn't really belong here, but is often used in conjunction with
 * this code.
 */
#define V_NamePatchWidth(name) R_NumPatchWidth(W_GetNumForName(name))
#define V_NamePatchHeight(name) R_NumPatchHeight(W_GetNumForName(name))

/* cphipps 10/99: function to tile a flat over the screen */
extern void V_DrawBackground(const char* flatname, int scrn);

void V_DestroyUnusedTrueColorPalettes(void);
// CPhipps - function to set the palette to palette number pal.
void V_SetPalette(int pal);

// CPhipps - function to plot a pixel

// V_PlotPixel
extern void V_PlotPixel(int,int,int,uint8_t);

typedef struct
{
  int x, y;
} fpoint_t;

typedef struct
{
  fpoint_t a, b;
} fline_t;

extern void V_DrawLine(fline_t* fl, int color);

void V_AllocScreen(screeninfo_t *scrn);
void V_AllocScreens();
void V_FreeScreen(screeninfo_t *scrn);
void V_FreeScreens();

#endif
