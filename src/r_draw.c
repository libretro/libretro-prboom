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
 *      The actual span/column drawing functions.
 *      Here find the main potential for optimization,
 *       e.g. inline assembly, different algorithms.
 *
 *-----------------------------------------------------------------------------*/

#include "doomstat.h"
#include "w_wad.h"
#include "r_main.h"
#include "r_draw.h"
#include "r_filter.h"
#include "v_video.h"
#include "st_stuff.h"
#include "g_game.h"
#include "am_map.h"
#include "lprintf.h"

//
// All drawing to the view buffer is accomplished in this file.
// The other refresh files only know about ccordinates,
//  not the architecture of the frame buffer.
// Conveniently, the frame buffer is a linear one,
//  and we need only the base address,
//  and the total size == width*height*depth/8.,
//

uint8_t *viewimage;
int  viewwidth;
int  scaledviewwidth;
int  viewheight;

// Color tables for different players,
//  translate a limited part to another
//  (color ramps used for  suit colors).
//

//
// R_DrawColumn
// Source is the top of the column to scale.
//

// SoM: OPTIMIZE for ANYRES
typedef enum
{
   COL_NONE,
   COL_OPAQUE,
   COL_TRANS,
   COL_FLEXTRANS,
   COL_FUZZ,
   COL_FLEXADD
} columntype_e;

static int    temp_x = 0;
static int    tempyl[4], tempyh[4];
static uint16_t short_tempbuf[MAX_SCREENHEIGHT * 4];
static int    startx = 0;
static int    temptype = COL_NONE;
static int    commontop, commonbot;
// SoM 7-28-04: Fix the fuzz problem.
static const uint8_t   *tempfuzzmap;

//
// Spectre/Invisibility.
//

#define FUZZTABLE 50
#define FUZZOFF 1

static const int fuzzoffset_org[FUZZTABLE] = {
  FUZZOFF,-FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,
  FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,
  FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,
  FUZZOFF,-FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,
  FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,-FUZZOFF,FUZZOFF,
  FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,
  FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF,FUZZOFF,-FUZZOFF,FUZZOFF
};

static int fuzzoffset[FUZZTABLE];

static int fuzzpos = 0;

// render pipelines
#define RDC_STANDARD      1
#define RDC_TRANSLATED    4
#define RDC_FUZZ          8
// no color mapping
#define RDC_NOCOLMAP     16
// filter modes
#define RDC_DITHERZ      32
#define RDC_BILINEAR     64
#define RDC_ROUNDED     128

draw_vars_t drawvars = { 
  NULL, // short_topleft
  NULL, // int_topleft
  RDRAW_FILTER_POINT, // filterwall
  RDRAW_FILTER_POINT, // filterfloor
  RDRAW_FILTER_POINT, // filtersprite
  RDRAW_FILTER_POINT, // filterz
  RDRAW_FILTER_POINT, // filterpatch

  RDRAW_MASKEDCOLUMNEDGE_SQUARE, // sprite_edges
  RDRAW_MASKEDCOLUMNEDGE_SQUARE, // patch_edges

  // 49152 = FRACUNIT * 0.75
  // 81920 = FRACUNIT * 1.25
  49152 // mag_threshold
};

//
// Error functions that will abort if R_FlushColumns tries to flush 
// columns without a column type.
//

static void R_FlushWholeError(void)
{
   I_Error("R_FlushWholeColumns called without being initialized.\n");
}

static void R_FlushHTError(void)
{
   I_Error("R_FlushHTColumns called without being initialized.\n");
}

static void R_QuadFlushError(void)
{
   I_Error("R_FlushQuadColumn called without being initialized.\n");
}

static void (*R_FlushWholeColumns)(void) = R_FlushWholeError;
static void (*R_FlushHTColumns)(void)    = R_FlushHTError;
static void (*R_FlushQuadColumn)(void) = R_QuadFlushError;

static void R_FlushColumns(void)
{
   if(temp_x != 4 || commontop >= commonbot)
      R_FlushWholeColumns();
   else
   {
      R_FlushHTColumns();
      R_FlushQuadColumn();
   }
   temp_x = 0;
}

//
// R_ResetColumnBuffer
//
// haleyjd 09/13/04: new function to call from main rendering loop
// which gets rid of the unnecessary reset of various variables during
// column drawing.
//
void R_ResetColumnBuffer(void)
{
   // haleyjd 10/06/05: this must not be done if temp_x == 0!
   if(temp_x)
      R_FlushColumns();

   temptype            = COL_NONE;
   R_FlushWholeColumns = R_FlushWholeError;
   R_FlushHTColumns    = R_FlushHTError;
   R_FlushQuadColumn   = R_QuadFlushError;
}

#define R_DRAWCOLUMN_PIPELINE RDC_STANDARD
#define R_FLUSHWHOLE_FUNCNAME R_FlushWhole16
#define R_FLUSHHEADTAIL_FUNCNAME R_FlushHT16
#define R_FLUSHQUAD_FUNCNAME R_FlushQuad16
#include "r_drawflush.inl"

#define R_DRAWCOLUMN_PIPELINE RDC_FUZZ
#define R_FLUSHWHOLE_FUNCNAME R_FlushWholeFuzz16
#define R_FLUSHHEADTAIL_FUNCNAME R_FlushHTFuzz16
#define R_FLUSHQUAD_FUNCNAME R_FlushQuadFuzz16
#include "r_drawflush.inl"

//
// R_DrawColumn
//

//
// A column is a vertical slice/span from a wall texture that,
//  given the DOOM style restrictions on the view orientation,
//  will always have constant z depth.
// Thus a special case loop for very fast rendering can
//  be used. It has also been used with Wolfenstein 3D.
//

uint8_t *translationtables;

#define R_DRAWCOLUMN_PIPELINE_TYPE RDC_PIPELINE_STANDARD
#define R_DRAWCOLUMN_PIPELINE_BASE RDC_STANDARD

#define R_DRAWCOLUMN_FUNCNAME_COMPOSITE(postfix) R_DrawColumn16 ## postfix
#define R_FLUSHWHOLE_FUNCNAME R_FlushWhole16
#define R_FLUSHHEADTAIL_FUNCNAME R_FlushHT16
#define R_FLUSHQUAD_FUNCNAME R_FlushQuad16
#include "r_drawcolpipeline.inl"

#undef R_DRAWCOLUMN_PIPELINE_BASE
#undef R_DRAWCOLUMN_PIPELINE_TYPE

//
// R_DrawTranslatedColumn
// Used to draw player sprites
//  with the green colorramp mapped to others.
// Could be used with different translation
//  tables, e.g. the lighter colored version
//  of the BaronOfHell, the HellKnight, uses
//  identical sprites, kinda brightened up.
//

#define R_DRAWCOLUMN_PIPELINE_TYPE RDC_PIPELINE_TRANSLATED
#define R_DRAWCOLUMN_PIPELINE_BASE RDC_TRANSLATED

#define R_DRAWCOLUMN_FUNCNAME_COMPOSITE(postfix) R_DrawTranslatedColumn16 ## postfix
#define R_FLUSHWHOLE_FUNCNAME R_FlushWhole16
#define R_FLUSHHEADTAIL_FUNCNAME R_FlushHT16
#define R_FLUSHQUAD_FUNCNAME R_FlushQuad16
#include "r_drawcolpipeline.inl"

#undef R_DRAWCOLUMN_PIPELINE_BASE
#undef R_DRAWCOLUMN_PIPELINE_TYPE

//
// Framebuffer postprocessing.
// Creates a fuzzy image by copying pixels
//  from adjacent ones to left and right.
// Used with an all black colormap, this
//  could create the SHADOW effect,
//  i.e. spectres and invisible players.
//

#define R_DRAWCOLUMN_PIPELINE_TYPE RDC_PIPELINE_FUZZ
#define R_DRAWCOLUMN_PIPELINE_BASE RDC_FUZZ

#define R_DRAWCOLUMN_FUNCNAME_COMPOSITE(postfix) R_DrawFuzzColumn16 ## postfix
#define R_FLUSHWHOLE_FUNCNAME R_FlushWholeFuzz16
#define R_FLUSHHEADTAIL_FUNCNAME R_FlushHTFuzz16
#define R_FLUSHQUAD_FUNCNAME R_FlushQuadFuzz16
#include "r_drawcolpipeline.inl"

#undef R_DRAWCOLUMN_PIPELINE_BASE
#undef R_DRAWCOLUMN_PIPELINE_TYPE

static R_DrawColumn_f drawcolumnfuncs[RDRAW_FILTER_MAXFILTERS][RDRAW_FILTER_MAXFILTERS][RDC_PIPELINE_MAXPIPELINES] = {
    {
      {NULL, NULL, NULL},
      {R_DrawColumn16_PointUV,
       R_DrawTranslatedColumn16_PointUV,
       R_DrawFuzzColumn16_PointUV,},
      {R_DrawColumn16_LinearUV,
       R_DrawTranslatedColumn16_LinearUV,
       R_DrawFuzzColumn16_LinearUV,},
      {R_DrawColumn16_RoundedUV,
       R_DrawTranslatedColumn16_RoundedUV,
       R_DrawFuzzColumn16_RoundedUV,},
    },
    {
      {NULL, NULL, NULL},
      {R_DrawColumn16_PointUV_PointZ,
       R_DrawTranslatedColumn16_PointUV_PointZ,
       R_DrawFuzzColumn16_PointUV_PointZ,},
      {R_DrawColumn16_LinearUV_PointZ,
       R_DrawTranslatedColumn16_LinearUV_PointZ,
       R_DrawFuzzColumn16_LinearUV_PointZ,},
      {R_DrawColumn16_RoundedUV_PointZ,
       R_DrawTranslatedColumn16_RoundedUV_PointZ,
       R_DrawFuzzColumn16_RoundedUV_PointZ,},
    },
    {
      {NULL, NULL, NULL},
      {R_DrawColumn16_PointUV_LinearZ,
       R_DrawTranslatedColumn16_PointUV_LinearZ,
       R_DrawFuzzColumn16_PointUV_LinearZ,},
      {R_DrawColumn16_LinearUV_LinearZ,
       R_DrawTranslatedColumn16_LinearUV_LinearZ,
       R_DrawFuzzColumn16_LinearUV_LinearZ,},
      {R_DrawColumn16_RoundedUV_LinearZ,
       R_DrawTranslatedColumn16_RoundedUV_LinearZ,
       R_DrawFuzzColumn16_RoundedUV_LinearZ,},
    },
};

R_DrawColumn_f R_GetDrawColumnFunc(enum column_pipeline_e type,
                                   enum draw_filter_type_e filter,
                                   enum draw_filter_type_e filterz) {
  R_DrawColumn_f result = drawcolumnfuncs[filterz][filter][type];
  if (result == NULL)
    I_Error("R_GetDrawColumnFunc: undefined function (%d, %d, %d)",
            type, filter, filterz);
  return result;
}

void R_SetDefaultDrawColumnVars(draw_column_vars_t *dcvars)
{
   dcvars->x             = 0;
   dcvars->yl            = 0;
   dcvars->yh            = 0;
   dcvars->z             = 0;
   dcvars->iscale        = 0;
   dcvars->texturemid    = 0;
   dcvars->texheight     = 0;
   dcvars->texu          = 0;
   dcvars->source        = NULL;
   dcvars->prevsource    = NULL;
   dcvars->nextsource    = NULL;
   dcvars->colormap      = colormaps[0];
   dcvars->nextcolormap  = colormaps[0];
   dcvars->translation   = NULL;
   dcvars->edgeslope     = 0;
   dcvars->drawingmasked = 0;
   dcvars->edgetype      = drawvars.sprite_edges;
}

//
// R_InitTranslationTables
// Creates the translation tables to map
//  the green color ramp to gray, brown, red.
// Assumes a given structure of the PLAYPAL.
// Could be read from a lump instead.
//

uint8_t playernumtotrans[MAXPLAYERS];
extern lighttable_t *(*c_zlight)[LIGHTLEVELS][MAXLIGHTZ];

void R_InitTranslationTables (void)
{
   int i, j;
#define MAXTRANS 3
   uint8_t transtocolour[MAXTRANS];

   // killough 5/2/98:
   // Remove dependency of colormaps aligned on 256-byte boundary

   if (!translationtables) // CPhipps - allow multiple calls
      translationtables = Z_Malloc(256*MAXTRANS, PU_STATIC, 0);

   for (i=0; i<MAXTRANS; i++)
      transtocolour[i] = 255;

   for (i=0; i<MAXPLAYERS; i++)
   {
      uint8_t wantcolour = mapcolor_plyr[i];
      playernumtotrans[i] = 0;
      if (wantcolour != 0x70) // Not green, would like translation
         for (j=0; j<MAXTRANS; j++)
         {
            if (transtocolour[j] == 255)
            {
               transtocolour[j] = wantcolour;
               playernumtotrans[i] = j+1;
               break;
            }
         }
   }

   // translate just the 16 green colors
   for (i=0; i<256; i++)
   {
      if (i >= 0x70 && i<= 0x7f)
      {
         // CPhipps - configurable player colours
         translationtables[i]     = colormaps[0][((i&0xf)<<9) + transtocolour[0]];
         translationtables[i+256] = colormaps[0][((i&0xf)<<9) + transtocolour[1]];
         translationtables[i+512] = colormaps[0][((i&0xf)<<9) + transtocolour[2]];
      }
      else  // Keep all other colors as is.
      {
         translationtables[i]     = i;
         translationtables[i+256] = i;
         translationtables[i+512] = i;
      }
   }
}

//
// R_DrawSpan
// With DOOM style restrictions on view orientation,
//  the floors and ceilings consist of horizontal slices
//  or spans with constant z depth.
// However, rotation around the world z axis is possible,
//  thus this mapping, while simpler and faster than
//  perspective correct texture mapping, has to traverse
//  the texture at an angle in all but a few cases.
// In consequence, flats are not stored by column (like walls),
//  and the inner loop has to step in texture space u and v.
//

#define R_DRAWSPAN_FUNCNAME R_DrawSpan16_PointUV_PointZ
#define R_DRAWSPAN_PIPELINE (RDC_STANDARD)
#include "r_drawspan.inl"

#define R_DRAWSPAN_FUNCNAME R_DrawSpan16_PointUV_LinearZ
#define R_DRAWSPAN_PIPELINE (RDC_STANDARD | RDC_DITHERZ)
#include "r_drawspan.inl"

#define R_DRAWSPAN_FUNCNAME R_DrawSpan16_LinearUV_PointZ
#define R_DRAWSPAN_PIPELINE (RDC_STANDARD | RDC_BILINEAR)
#include "r_drawspan.inl"

#define R_DRAWSPAN_FUNCNAME R_DrawSpan16_LinearUV_LinearZ
#define R_DRAWSPAN_PIPELINE (RDC_STANDARD | RDC_BILINEAR | RDC_DITHERZ)
#include "r_drawspan.inl"

#define R_DRAWSPAN_FUNCNAME R_DrawSpan16_RoundedUV_PointZ
#define R_DRAWSPAN_PIPELINE (RDC_STANDARD | RDC_ROUNDED)
#include "r_drawspan.inl"

#define R_DRAWSPAN_FUNCNAME R_DrawSpan16_RoundedUV_LinearZ
#define R_DRAWSPAN_PIPELINE (RDC_STANDARD | RDC_ROUNDED | RDC_DITHERZ)
#include "r_drawspan.inl"

static R_DrawSpan_f drawspanfuncs[RDRAW_FILTER_MAXFILTERS][RDRAW_FILTER_MAXFILTERS] = {
    {
      NULL,
      NULL,
      NULL,
      NULL,
    },
    {
      NULL,
      R_DrawSpan16_PointUV_PointZ,
      R_DrawSpan16_LinearUV_PointZ,
      R_DrawSpan16_RoundedUV_PointZ,
    },
    {
      NULL,
      R_DrawSpan16_PointUV_LinearZ,
      R_DrawSpan16_LinearUV_LinearZ,
      R_DrawSpan16_RoundedUV_LinearZ,
    },
    {
      NULL,
      NULL,
      NULL,
      NULL,
    },
};

R_DrawSpan_f R_GetDrawSpanFunc(enum draw_filter_type_e filter,
                               enum draw_filter_type_e filterz) {
  R_DrawSpan_f result = drawspanfuncs[filterz][filter];
  if (result == NULL)
    I_Error("R_GetDrawSpanFunc: undefined function (%d, %d)",
            filter, filterz);
  return result;
}

void R_DrawSpan(draw_span_vars_t *dsvars) {
  R_GetDrawSpanFunc(drawvars.filterfloor, drawvars.filterz)(dsvars);
}

//
// R_InitBuffer
// Creats lookup tables that avoid
//  multiplies and other hazzles
//  for getting the framebuffer address
//  of a pixel to draw.
//

void R_InitBuffer(int width, int height)
{
  int i=0;
  // Handle resize,
  //  e.g. smaller view windows
  //  with border and/or status bar.

  // Same with base row offset.

  drawvars.short_topleft = (unsigned short *)(screens[0].data);
  drawvars.int_topleft = (unsigned int *)(screens[0].data);

  for (i=0; i<FUZZTABLE; i++)
	  fuzzoffset[i] = fuzzoffset_org[i] * SURFACE_SHORT_PITCH;
}
