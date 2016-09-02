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

/*
 * R_FlushWhole16
 *
 * Flushes the entire columns in the buffer, one at a time.
 * This is used when a quad flush isn't possible.
 * Opaque version -- no remapping whatsoever.
*/
static void R_FlushWhole16(void)
{
   while(--temp_x >= 0)
   {
      int yl           = tempyl[temp_x];
      uint16_t *source = &short_tempbuf[temp_x + (yl << 2)];
      uint16_t *dest   = drawvars.short_topleft + yl * SURFACE_SHORT_PITCH + startx + temp_x;
      int   count      = tempyh[temp_x] - yl + 1;
      
      while(--count >= 0)
      {
         *dest   = *source;
         source += 4;
         dest   += SURFACE_SHORT_PITCH;
      }
   }
}

//
// R_FlushHT16
//
// Flushes the head and tail of columns in the buffer in
// preparation for a quad flush.
// Opaque version -- no remapping whatsoever.
//
static void R_FlushHT16(void)
{
   uint16_t *source;
   uint16_t *dest;
   int count, colnum = 0;
   int yl, yh;

   while(colnum < 4)
   {
      yl = tempyl[colnum];
      yh = tempyh[colnum];
      
      // flush column head
      if(yl < commontop)
      {
         source = &short_tempbuf[colnum + (yl << 2)];
         dest   = drawvars.short_topleft + yl * SURFACE_SHORT_PITCH + startx + colnum;
         count  = commontop - yl;
         
         while(--count >= 0)
         {
            *dest = *source;
            source += 4;
            dest += SURFACE_SHORT_PITCH;
         }
      }
      
      // flush column tail
      if(yh > commonbot)
      {
         source = &short_tempbuf[colnum + ((commonbot + 1) << 2)];
         dest   = drawvars.short_topleft + (commonbot + 1) * SURFACE_SHORT_PITCH + startx + colnum;
         count  = yh - commonbot;
         
         while(--count >= 0)
         {
            *dest = *source;

            source += 4;
            dest += SURFACE_SHORT_PITCH;
         }
      }         
      ++colnum;
   }
}

static void R_FlushQuad16(void)
{
   uint16_t *source = &short_tempbuf[commontop << 2];
   uint16_t *dest   = drawvars.short_topleft + commontop * SURFACE_SHORT_PITCH + startx;
   int        count = commonbot - commontop + 1;

   while(--count >= 0)
   {
      dest[0] = source[0];
      dest[1] = source[1];
      dest[2] = source[2];
      dest[3] = source[3];
      source += 4;
      dest += SURFACE_SHORT_PITCH;
   }
}

/*
 * R_FlushWholeFuzz16
 *
 * Flushes the entire columns in the buffer, one at a time.
 * This is used when a quad flush isn't possible.
 * Opaque version -- no remapping whatsoever.
*/
static void R_FlushWholeFuzz16(void)
{
   uint16_t *source;
   uint16_t *dest;
   int  count, yl;

   while(--temp_x >= 0)
   {
      yl     = tempyl[temp_x];
      source = &short_tempbuf[temp_x + (yl << 2)];
      dest   = drawvars.short_topleft + yl * SURFACE_SHORT_PITCH + startx + temp_x;
      count  = tempyh[temp_x] - yl + 1;
      
      while(--count >= 0)
      {
         // SoM 7-28-04: Fix the fuzz problem.
         *dest = GETBLENDED16_9406(dest[fuzzoffset[fuzzpos]], 0);
         
         // Clamp table lookup index.
         if(++fuzzpos == FUZZTABLE) 
            fuzzpos = 0;

         source += 4;
         dest += SURFACE_SHORT_PITCH;
      }
   }
}

//
// R_FlushHTFuzz16
//
// Flushes the head and tail of columns in the buffer in
// preparation for a quad flush.
// Opaque version -- no remapping whatsoever.
//
static void R_FlushHTFuzz16(void)
{
   uint16_t *source;
   uint16_t *dest;
   int count, colnum = 0;
   int yl, yh;

   while(colnum < 4)
   {
      yl = tempyl[colnum];
      yh = tempyh[colnum];
      
      // flush column head
      if(yl < commontop)
      {
         source = &short_tempbuf[colnum + (yl << 2)];
         dest   = drawvars.short_topleft + yl * SURFACE_SHORT_PITCH + startx + colnum;
         count  = commontop - yl;
         
         while(--count >= 0)
         {
            // SoM 7-28-04: Fix the fuzz problem.
            *dest = GETBLENDED16_9406(dest[fuzzoffset[fuzzpos]], 0);
            
            // Clamp table lookup index.
            if(++fuzzpos == FUZZTABLE) 
               fuzzpos = 0;

            source += 4;
            dest += SURFACE_SHORT_PITCH;
         }
      }
      
      // flush column tail
      if(yh > commonbot)
      {
         source = &short_tempbuf[colnum + ((commonbot + 1) << 2)];
         dest   = drawvars.short_topleft + (commonbot + 1) * SURFACE_SHORT_PITCH + startx + colnum;
         count  = yh - commonbot;
         
         while(--count >= 0)
         {
            // SoM 7-28-04: Fix the fuzz problem.
            *dest = GETBLENDED16_9406(dest[fuzzoffset[fuzzpos]], 0);
            
            // Clamp table lookup index.
            if(++fuzzpos == FUZZTABLE) 
               fuzzpos = 0;

            source += 4;
            dest += SURFACE_SHORT_PITCH;
         }
      }         
      ++colnum;
   }
}

static void R_FlushQuadFuzz16(void)
{
   uint16_t *source = &short_tempbuf[commontop << 2];
   uint16_t *dest   = drawvars.short_topleft + commontop * SURFACE_SHORT_PITCH + startx;
   int fuzz1        = fuzzpos;
   int fuzz2        = (fuzz1 + tempyl[1]) % FUZZTABLE;
   int fuzz3        = (fuzz2 + tempyl[2]) % FUZZTABLE;
   int fuzz4        = (fuzz3 + tempyl[3]) % FUZZTABLE;
   int count        = commonbot - commontop + 1;

   while(--count >= 0)
   {
      dest[0] = GETBLENDED16_9406(dest[0 + fuzzoffset[fuzz1]], 0);
      dest[1] = GETBLENDED16_9406(dest[1 + fuzzoffset[fuzz2]], 0);
      dest[2] = GETBLENDED16_9406(dest[2 + fuzzoffset[fuzz3]], 0);
      dest[3] = GETBLENDED16_9406(dest[3 + fuzzoffset[fuzz4]], 0);
      fuzz1 = (fuzz1 + 1) % FUZZTABLE;
      fuzz2 = (fuzz2 + 1) % FUZZTABLE;
      fuzz3 = (fuzz3 + 1) % FUZZTABLE;
      fuzz4 = (fuzz4 + 1) % FUZZTABLE;
      source += 4 * sizeof(uint8_t);
      dest += SURFACE_SHORT_PITCH * sizeof(uint8_t);
   }
}

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

// no color mapping
static void R_DrawColumn16_PointUV(draw_column_vars_t *dcvars)
{
   int count;

   uint16_t *dest;

   fixed_t frac;
   const fixed_t fracstep = dcvars->iscale;



   const fixed_t slope_texu = dcvars->texu;
   count = dcvars->yh - dcvars->yl;

   if (count < 0)
      return;





   frac = dcvars->texturemid + (dcvars->yl-centery)*fracstep;


   if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED) {



      if (dcvars->yl != 0) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += 0xffff-(slope_texu & 0xffff);
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += slope_texu & 0xffff;
         }
      }
      if (dcvars->yh != viewheight-1) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
      }
      if (count <= 0) return;
   }



   {

      if(temp_x == 4 ||
            (temp_x && (temptype != (COL_OPAQUE) || temp_x + startx != dcvars->x)))
         R_FlushColumns();

      if(!temp_x)
      {
         startx = dcvars->x;
         tempyl[0] = commontop = dcvars->yl;
         tempyh[0] = commonbot = dcvars->yh;
         temptype = (COL_OPAQUE);





         R_FlushWholeColumns = R_FlushWhole16;
         R_FlushHTColumns = R_FlushHT16;
         R_FlushQuadColumn = R_FlushQuad16;

         dest = &short_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tempyl[temp_x] = dcvars->yl;
         tempyh[temp_x] = dcvars->yh;

         if(dcvars->yl > commontop)
            commontop = dcvars->yl;
         if(dcvars->yh < commonbot)
            commonbot = dcvars->yh;


         dest = &short_tempbuf[(dcvars->yl << 2) + temp_x];

      }
      temp_x += 1;
   }



   {
      const uint8_t *source = dcvars->source;
      const lighttable_t *colormap = dcvars->colormap;
      const uint8_t *translation = dcvars->translation;
      count++;







      if (dcvars->texheight == 128)
      {

         while(count--)
         {
            *dest = (V_Palette16[ ((source[(frac & ((127<<16)|0xffff))>>16]))*64 + ((64 -1)) ]);
            ;
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (V_Palette16[ ((source[(frac)>>16]))*64 + ((64 -1)) ]);
            ;
            dest += 4;
            frac += fracstep;
         }
      }
      else
      {
         unsigned heightmask = dcvars->texheight-1;
         if (! (dcvars->texheight & heightmask))
         {
            fixed_t fixedt_heightmask = (heightmask<<16)|0xffff;
            while ((count-=2)>=0)
            {
               *dest = (V_Palette16[ ((source[(frac & fixedt_heightmask)>>16]))*64 + ((64 -1)) ]);
               ;
               dest += 4;
               frac += fracstep;
               *dest = (V_Palette16[ ((source[(frac & fixedt_heightmask)>>16]))*64 + ((64 -1)) ]);
               ;
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (V_Palette16[ ((source[(frac & fixedt_heightmask)>>16]))*64 + ((64 -1)) ]);
            ;
         }
         else
         {
            fixed_t nextfrac = 0;

            heightmask++;
            heightmask <<= 16;

            if (frac < 0)
               while ((frac += heightmask) < 0);
            else
               while (frac >= (int)heightmask)
                  frac -= heightmask;
            while (count--)
            {





               *dest = (V_Palette16[ ((source[(frac)>>16]))*64 + ((64 -1)) ]);
               ;
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;



            }
         }
      }
   }
}

// simple depth color mapping
static void R_DrawColumn16_PointUV_PointZ(draw_column_vars_t *dcvars)
{
   int count;

   uint16_t *dest;

   fixed_t frac;
   const fixed_t fracstep = dcvars->iscale;



   const fixed_t slope_texu = dcvars->texu;
   count = dcvars->yh - dcvars->yl;







   if (count < 0)
      return;





   frac = dcvars->texturemid + (dcvars->yl-centery)*fracstep;


   if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED) {



      if (dcvars->yl != 0) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += 0xffff-(slope_texu & 0xffff);
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += slope_texu & 0xffff;
         }
      }
      if (dcvars->yh != viewheight-1) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
      }
      if (count <= 0) return;
   }



   {

      if(temp_x == 4 ||
            (temp_x && (temptype != (COL_OPAQUE) || temp_x + startx != dcvars->x)))
         R_FlushColumns();

      if(!temp_x)
      {
         startx = dcvars->x;
         tempyl[0] = commontop = dcvars->yl;
         tempyh[0] = commonbot = dcvars->yh;
         temptype = (COL_OPAQUE);





         R_FlushWholeColumns = R_FlushWhole16;
         R_FlushHTColumns = R_FlushHT16;
         R_FlushQuadColumn = R_FlushQuad16;

         dest = &short_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tempyl[temp_x] = dcvars->yl;
         tempyh[temp_x] = dcvars->yh;

         if(dcvars->yl > commontop)
            commontop = dcvars->yl;
         if(dcvars->yh < commonbot)
            commonbot = dcvars->yh;


         dest = &short_tempbuf[(dcvars->yl << 2) + temp_x];

      }
      temp_x += 1;
   }



   {
      const uint8_t *source = dcvars->source;
      const lighttable_t *colormap = dcvars->colormap;
      const uint8_t *translation = dcvars->translation;
      count++;







      if (dcvars->texheight == 128)
      {

         while(count--)
         {
            *dest = (V_Palette16[ (colormap[(source[(frac & ((127<<16)|0xffff))>>16])])*64 + ((64 -1)) ]);
            ;
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (V_Palette16[ (colormap[(source[(frac)>>16])])*64 + ((64 -1)) ]);
            ;
            dest += 4;
            frac += fracstep;
         }
      }
      else
      {
         unsigned heightmask = dcvars->texheight-1;
         if (! (dcvars->texheight & heightmask))
         {
            fixed_t fixedt_heightmask = (heightmask<<16)|0xffff;
            while ((count-=2)>=0)
            {
               *dest = (V_Palette16[ (colormap[(source[(frac & fixedt_heightmask)>>16])])*64 + ((64 -1)) ]);
               ;
               dest += 4;
               frac += fracstep;
               *dest = (V_Palette16[ (colormap[(source[(frac & fixedt_heightmask)>>16])])*64 + ((64 -1)) ]);
               ;
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (V_Palette16[ (colormap[(source[(frac & fixedt_heightmask)>>16])])*64 + ((64 -1)) ]);
            ;
         }
         else
         {
            fixed_t nextfrac = 0;

            heightmask++;
            heightmask <<= 16;

            if (frac < 0)
               while ((frac += heightmask) < 0);
            else
               while (frac >= (int)heightmask)
                  frac -= heightmask;
            while (count--)
            {





               *dest = (V_Palette16[ (colormap[(source[(frac)>>16])])*64 + ((64 -1)) ]);
               ;
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;



            }
         }
      }
   }

}

// z-dither
static void R_DrawColumn16_PointUV_LinearZ(draw_column_vars_t *dcvars)
{
   int count;

   uint16_t *dest;

   fixed_t frac;
   const fixed_t fracstep = dcvars->iscale;



   const fixed_t slope_texu = dcvars->texu;
   count = dcvars->yh - dcvars->yl;







   if (count < 0)
      return;





   frac = dcvars->texturemid + (dcvars->yl-centery)*fracstep;


   if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED) {



      if (dcvars->yl != 0) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += 0xffff-(slope_texu & 0xffff);
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += slope_texu & 0xffff;
         }
      }
      if (dcvars->yh != viewheight-1) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
      }
      if (count <= 0) return;
   }



   {

      if(temp_x == 4 ||
            (temp_x && (temptype != (COL_OPAQUE) || temp_x + startx != dcvars->x)))
         R_FlushColumns();

      if(!temp_x)
      {
         startx = dcvars->x;
         tempyl[0] = commontop = dcvars->yl;
         tempyh[0] = commonbot = dcvars->yh;
         temptype = (COL_OPAQUE);





         R_FlushWholeColumns = R_FlushWhole16;
         R_FlushHTColumns = R_FlushHT16;
         R_FlushQuadColumn = R_FlushQuad16;

         dest = &short_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tempyl[temp_x] = dcvars->yl;
         tempyh[temp_x] = dcvars->yh;

         if(dcvars->yl > commontop)
            commontop = dcvars->yl;
         if(dcvars->yh < commonbot)
            commonbot = dcvars->yh;


         dest = &short_tempbuf[(dcvars->yl << 2) + temp_x];

      }
      temp_x += 1;
   }



   {
      const uint8_t *source = dcvars->source;
      const lighttable_t *colormap = dcvars->colormap;
      const uint8_t *translation = dcvars->translation;

      int y = dcvars->yl;
      const int x = dcvars->x;


      const int fracz = (dcvars->z >> 6) & 255;
      const uint8_t *dither_colormaps[2] = { dcvars->colormap, dcvars->nextcolormap };
      count++;







      if (dcvars->texheight == 128)
      {

         while(count--)
         {
            *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(frac & ((127<<16)|0xffff))>>16])]))*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(frac)>>16])]))*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else
      {
         unsigned heightmask = dcvars->texheight-1;
         if (! (dcvars->texheight & heightmask))
         {
            fixed_t fixedt_heightmask = (heightmask<<16)|0xffff;
            while ((count-=2)>=0)
            {
               *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(frac & fixedt_heightmask)>>16])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(frac & fixedt_heightmask)>>16])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(frac & fixedt_heightmask)>>16])]))*64 + ((64 -1)) ]);
            (y++);
         }
         else
         {
            fixed_t nextfrac = 0;

            heightmask++;
            heightmask <<= 16;

            if (frac < 0)
               while ((frac += heightmask) < 0);
            else
               while (frac >= (int)heightmask)
                  frac -= heightmask;
            while (count--)
            {





               *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(frac)>>16])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;



            }
         }
      }
   }

}

// bilinear with no color mapping
static void R_DrawColumn16_LinearUV(draw_column_vars_t *dcvars)
{
   int count;

   uint16_t *dest;

   fixed_t frac;
   const fixed_t fracstep = dcvars->iscale;

   const fixed_t slope_texu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;






   if (dcvars->iscale > drawvars.mag_threshold) {
      R_GetDrawColumnFunc(RDC_PIPELINE_STANDARD,
            RDRAW_FILTER_POINT,
            drawvars.filterz)(dcvars);
      return;
   }
   count = dcvars->yh - dcvars->yl;







   if (count < 0)
      return;



   frac = dcvars->texturemid - ((1<<16)>>1) + (dcvars->yl-centery)*fracstep;




   if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED) {



      if (dcvars->yl != 0) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += 0xffff-(slope_texu & 0xffff);
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += slope_texu & 0xffff;
         }
      }
      if (dcvars->yh != viewheight-1) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
      }
      if (count <= 0) return;
   }



   {

      if(temp_x == 4 ||
            (temp_x && (temptype != (COL_OPAQUE) || temp_x + startx != dcvars->x)))
         R_FlushColumns();

      if(!temp_x)
      {
         startx = dcvars->x;
         tempyl[0] = commontop = dcvars->yl;
         tempyh[0] = commonbot = dcvars->yh;
         temptype = (COL_OPAQUE);





         R_FlushWholeColumns = R_FlushWhole16;
         R_FlushHTColumns = R_FlushHT16;
         R_FlushQuadColumn = R_FlushQuad16;

         dest = &short_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tempyl[temp_x] = dcvars->yl;
         tempyh[temp_x] = dcvars->yh;

         if(dcvars->yl > commontop)
            commontop = dcvars->yl;
         if(dcvars->yh < commonbot)
            commonbot = dcvars->yh;


         dest = &short_tempbuf[(dcvars->yl << 2) + temp_x];

      }
      temp_x += 1;
   }



   {
      const uint8_t *source = dcvars->source;
      const lighttable_t *colormap = dcvars->colormap;
      const uint8_t *translation = dcvars->translation;

      int y = dcvars->yl;
      const int x = dcvars->x;






      const uint8_t *nextsource = dcvars->nextsource;
      const unsigned int filter_fracu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;







      count++;







      if (dcvars->texheight == 128)
      {

         while(count--)
         {
            *dest = (( V_Palette16[ ((nextsource[((frac+(1<<16)) & ((127<<16)|0xffff))>>16]))*64 + ((filter_fracu*((frac & ((127<<16)|0xffff))&0xffff))>>(32-6)) ] + V_Palette16[ ((source[((frac+(1<<16)) & ((127<<16)|0xffff))>>16]))*64 + (((0xffff-filter_fracu)*((frac & ((127<<16)|0xffff))&0xffff))>>(32-6)) ] + V_Palette16[ ((source[(frac & ((127<<16)|0xffff))>>16]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & ((127<<16)|0xffff))&0xffff)))>>(32-6)) ] + V_Palette16[ ((nextsource[(frac & ((127<<16)|0xffff))>>16]))*64 + ((filter_fracu*(0xffff-((frac & ((127<<16)|0xffff))&0xffff)))>>(32-6)) ]));
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (( V_Palette16[ ((nextsource[((frac+(1<<16)))>>16]))*64 + ((filter_fracu*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ ((source[((frac+(1<<16)))>>16]))*64 + (((0xffff-filter_fracu)*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ ((source[(frac)>>16]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac)&0xffff)))>>(32-6)) ] + V_Palette16[ ((nextsource[(frac)>>16]))*64 + ((filter_fracu*(0xffff-((frac)&0xffff)))>>(32-6)) ]));
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else
      {
         unsigned heightmask = dcvars->texheight-1;
         if (! (dcvars->texheight & heightmask))
         {
            fixed_t fixedt_heightmask = (heightmask<<16)|0xffff;
            while ((count-=2)>=0)
            {
               *dest = (( V_Palette16[ ((nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16]))*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((source[((frac+(1<<16)) & fixedt_heightmask)>>16]))*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((source[(frac & fixedt_heightmask)>>16]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_Palette16[ ((nextsource[(frac & fixedt_heightmask)>>16]))*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (( V_Palette16[ ((nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16]))*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((source[((frac+(1<<16)) & fixedt_heightmask)>>16]))*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((source[(frac & fixedt_heightmask)>>16]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_Palette16[ ((nextsource[(frac & fixedt_heightmask)>>16]))*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (( V_Palette16[ ((nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16]))*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((source[((frac+(1<<16)) & fixedt_heightmask)>>16]))*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((source[(frac & fixedt_heightmask)>>16]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_Palette16[ ((nextsource[(frac & fixedt_heightmask)>>16]))*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
            (y++);
         }
         else
         {
            fixed_t nextfrac = 0;

            heightmask++;
            heightmask <<= 16;

            if (frac < 0)
               while ((frac += heightmask) < 0);
            else
               while (frac >= (int)heightmask)
                  frac -= heightmask;


            nextfrac = frac + (1<<16);
            while (nextfrac >= (int)heightmask)
               nextfrac -= heightmask;




            while (count--)
            {





               *dest = (( V_Palette16[ ((nextsource[(nextfrac)>>16]))*64 + ((filter_fracu*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ ((source[(nextfrac)>>16]))*64 + (((0xffff-filter_fracu)*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ ((source[(frac)>>16]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac)&0xffff)))>>(32-6)) ] + V_Palette16[ ((nextsource[(frac)>>16]))*64 + ((filter_fracu*(0xffff-((frac)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;

               if ((nextfrac += fracstep) >= (int)heightmask) nextfrac -= heightmask;;

            }
         }
      }
   }

}

// bilinear with simple depth color mapping
static void R_DrawColumn16_LinearUV_PointZ(draw_column_vars_t *dcvars)
{
   int count;

   uint16_t *dest;

   fixed_t frac;
   const fixed_t fracstep = dcvars->iscale;

   const fixed_t slope_texu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;






   if (dcvars->iscale > drawvars.mag_threshold) {
      R_GetDrawColumnFunc(RDC_PIPELINE_STANDARD,
            RDRAW_FILTER_POINT,
            drawvars.filterz)(dcvars);
      return;
   }
   count = dcvars->yh - dcvars->yl;







   if (count < 0)
      return;



   frac = dcvars->texturemid - ((1<<16)>>1) + (dcvars->yl-centery)*fracstep;




   if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED) {



      if (dcvars->yl != 0) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += 0xffff-(slope_texu & 0xffff);
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += slope_texu & 0xffff;
         }
      }
      if (dcvars->yh != viewheight-1) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
      }
      if (count <= 0) return;
   }



   {

      if(temp_x == 4 ||
            (temp_x && (temptype != (COL_OPAQUE) || temp_x + startx != dcvars->x)))
         R_FlushColumns();

      if(!temp_x)
      {
         startx = dcvars->x;
         tempyl[0] = commontop = dcvars->yl;
         tempyh[0] = commonbot = dcvars->yh;
         temptype = (COL_OPAQUE);





         R_FlushWholeColumns = R_FlushWhole16;
         R_FlushHTColumns = R_FlushHT16;
         R_FlushQuadColumn = R_FlushQuad16;

         dest = &short_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tempyl[temp_x] = dcvars->yl;
         tempyh[temp_x] = dcvars->yh;

         if(dcvars->yl > commontop)
            commontop = dcvars->yl;
         if(dcvars->yh < commonbot)
            commonbot = dcvars->yh;


         dest = &short_tempbuf[(dcvars->yl << 2) + temp_x];

      }
      temp_x += 1;
   }



   {
      const uint8_t *source = dcvars->source;
      const lighttable_t *colormap = dcvars->colormap;
      const uint8_t *translation = dcvars->translation;

      int y = dcvars->yl;
      const int x = dcvars->x;






      const uint8_t *nextsource = dcvars->nextsource;
      const unsigned int filter_fracu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;







      count++;







      if (dcvars->texheight == 128)
      {

         while(count--)
         {
            *dest = (( V_Palette16[ (colormap[(nextsource[((frac+(1<<16)) & ((127<<16)|0xffff))>>16])])*64 + ((filter_fracu*((frac & ((127<<16)|0xffff))&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(source[((frac+(1<<16)) & ((127<<16)|0xffff))>>16])])*64 + (((0xffff-filter_fracu)*((frac & ((127<<16)|0xffff))&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(source[(frac & ((127<<16)|0xffff))>>16])])*64 + (((0xffff-filter_fracu)*(0xffff-((frac & ((127<<16)|0xffff))&0xffff)))>>(32-6)) ] + V_Palette16[ (colormap[(nextsource[(frac & ((127<<16)|0xffff))>>16])])*64 + ((filter_fracu*(0xffff-((frac & ((127<<16)|0xffff))&0xffff)))>>(32-6)) ]));
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (( V_Palette16[ (colormap[(nextsource[((frac+(1<<16)))>>16])])*64 + ((filter_fracu*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(source[((frac+(1<<16)))>>16])])*64 + (((0xffff-filter_fracu)*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(source[(frac)>>16])])*64 + (((0xffff-filter_fracu)*(0xffff-((frac)&0xffff)))>>(32-6)) ] + V_Palette16[ (colormap[(nextsource[(frac)>>16])])*64 + ((filter_fracu*(0xffff-((frac)&0xffff)))>>(32-6)) ]));
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else
      {
         unsigned heightmask = dcvars->texheight-1;
         if (! (dcvars->texheight & heightmask))
         {
            fixed_t fixedt_heightmask = (heightmask<<16)|0xffff;
            while ((count-=2)>=0)
            {
               *dest = (( V_Palette16[ (colormap[(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])])*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(source[((frac+(1<<16)) & fixedt_heightmask)>>16])])*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(source[(frac & fixedt_heightmask)>>16])])*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_Palette16[ (colormap[(nextsource[(frac & fixedt_heightmask)>>16])])*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (( V_Palette16[ (colormap[(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])])*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(source[((frac+(1<<16)) & fixedt_heightmask)>>16])])*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(source[(frac & fixedt_heightmask)>>16])])*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_Palette16[ (colormap[(nextsource[(frac & fixedt_heightmask)>>16])])*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (( V_Palette16[ (colormap[(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])])*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(source[((frac+(1<<16)) & fixedt_heightmask)>>16])])*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(source[(frac & fixedt_heightmask)>>16])])*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_Palette16[ (colormap[(nextsource[(frac & fixedt_heightmask)>>16])])*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
            (y++);
         }
         else
         {
            fixed_t nextfrac = 0;

            heightmask++;
            heightmask <<= 16;

            if (frac < 0)
               while ((frac += heightmask) < 0);
            else
               while (frac >= (int)heightmask)
                  frac -= heightmask;


            nextfrac = frac + (1<<16);
            while (nextfrac >= (int)heightmask)
               nextfrac -= heightmask;




            while (count--)
            {





               *dest = (( V_Palette16[ (colormap[(nextsource[(nextfrac)>>16])])*64 + ((filter_fracu*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(source[(nextfrac)>>16])])*64 + (((0xffff-filter_fracu)*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(source[(frac)>>16])])*64 + (((0xffff-filter_fracu)*(0xffff-((frac)&0xffff)))>>(32-6)) ] + V_Palette16[ (colormap[(nextsource[(frac)>>16])])*64 + ((filter_fracu*(0xffff-((frac)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;

               if ((nextfrac += fracstep) >= (int)heightmask) nextfrac -= heightmask;;

            }
         }
      }
   }

}

// bilinear + z-dither
static void R_DrawColumn16_LinearUV_LinearZ(draw_column_vars_t *dcvars)
{
   int count;

   uint16_t *dest;

   fixed_t frac;
   const fixed_t fracstep = dcvars->iscale;

   const fixed_t slope_texu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;






   if (dcvars->iscale > drawvars.mag_threshold) {
      R_GetDrawColumnFunc(RDC_PIPELINE_STANDARD,
            RDRAW_FILTER_POINT,
            drawvars.filterz)(dcvars);
      return;
   }
   count = dcvars->yh - dcvars->yl;







   if (count < 0)
      return;



   frac = dcvars->texturemid - ((1<<16)>>1) + (dcvars->yl-centery)*fracstep;




   if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED) {



      if (dcvars->yl != 0) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += 0xffff-(slope_texu & 0xffff);
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += slope_texu & 0xffff;
         }
      }
      if (dcvars->yh != viewheight-1) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
      }
      if (count <= 0) return;
   }



   {

      if(temp_x == 4 ||
            (temp_x && (temptype != (COL_OPAQUE) || temp_x + startx != dcvars->x)))
         R_FlushColumns();

      if(!temp_x)
      {
         startx = dcvars->x;
         tempyl[0] = commontop = dcvars->yl;
         tempyh[0] = commonbot = dcvars->yh;
         temptype = (COL_OPAQUE);





         R_FlushWholeColumns = R_FlushWhole16;
         R_FlushHTColumns = R_FlushHT16;
         R_FlushQuadColumn = R_FlushQuad16;

         dest = &short_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tempyl[temp_x] = dcvars->yl;
         tempyh[temp_x] = dcvars->yh;

         if(dcvars->yl > commontop)
            commontop = dcvars->yl;
         if(dcvars->yh < commonbot)
            commonbot = dcvars->yh;


         dest = &short_tempbuf[(dcvars->yl << 2) + temp_x];

      }
      temp_x += 1;
   }



   {
      const uint8_t *source = dcvars->source;
      const lighttable_t *colormap = dcvars->colormap;
      const uint8_t *translation = dcvars->translation;

      int y = dcvars->yl;
      const int x = dcvars->x;


      const int fracz = (dcvars->z >> 6) & 255;
      const uint8_t *dither_colormaps[2] = { dcvars->colormap, dcvars->nextcolormap };


      const uint8_t *nextsource = dcvars->nextsource;
      const unsigned int filter_fracu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;







      count++;







      if (dcvars->texheight == 128)
      {

         while(count--)
         {
            *dest = (( V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(nextsource[((frac+(1<<16)) & ((127<<16)|0xffff))>>16])]))*64 + ((filter_fracu*((frac & ((127<<16)|0xffff))&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[((frac+(1<<16)) & ((127<<16)|0xffff))>>16])]))*64 + (((0xffff-filter_fracu)*((frac & ((127<<16)|0xffff))&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(frac & ((127<<16)|0xffff))>>16])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & ((127<<16)|0xffff))&0xffff)))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(nextsource[(frac & ((127<<16)|0xffff))>>16])]))*64 + ((filter_fracu*(0xffff-((frac & ((127<<16)|0xffff))&0xffff)))>>(32-6)) ]));
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (( V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(nextsource[((frac+(1<<16)))>>16])]))*64 + ((filter_fracu*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[((frac+(1<<16)))>>16])]))*64 + (((0xffff-filter_fracu)*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(frac)>>16])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac)&0xffff)))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(nextsource[(frac)>>16])]))*64 + ((filter_fracu*(0xffff-((frac)&0xffff)))>>(32-6)) ]));
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else
      {
         unsigned heightmask = dcvars->texheight-1;
         if (! (dcvars->texheight & heightmask))
         {
            fixed_t fixedt_heightmask = (heightmask<<16)|0xffff;
            while ((count-=2)>=0)
            {
               *dest = (( V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])]))*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[((frac+(1<<16)) & fixedt_heightmask)>>16])]))*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(frac & fixedt_heightmask)>>16])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(nextsource[(frac & fixedt_heightmask)>>16])]))*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (( V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])]))*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[((frac+(1<<16)) & fixedt_heightmask)>>16])]))*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(frac & fixedt_heightmask)>>16])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(nextsource[(frac & fixedt_heightmask)>>16])]))*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (( V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])]))*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[((frac+(1<<16)) & fixedt_heightmask)>>16])]))*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(frac & fixedt_heightmask)>>16])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(nextsource[(frac & fixedt_heightmask)>>16])]))*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
            (y++);
         }
         else
         {
            fixed_t nextfrac = 0;

            heightmask++;
            heightmask <<= 16;

            if (frac < 0)
               while ((frac += heightmask) < 0);
            else
               while (frac >= (int)heightmask)
                  frac -= heightmask;


            nextfrac = frac + (1<<16);
            while (nextfrac >= (int)heightmask)
               nextfrac -= heightmask;




            while (count--)
            {





               *dest = (( V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(nextsource[(nextfrac)>>16])]))*64 + ((filter_fracu*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(nextfrac)>>16])]))*64 + (((0xffff-filter_fracu)*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(source[(frac)>>16])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac)&0xffff)))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(nextsource[(frac)>>16])]))*64 + ((filter_fracu*(0xffff-((frac)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;

               if ((nextfrac += fracstep) >= (int)heightmask) nextfrac -= heightmask;;

            }
         }
      }
   }

}

/* rounded with no color mapping */
static void R_DrawColumn16_RoundedUV(draw_column_vars_t *dcvars)
{
  int count;

  uint16_t *dest;

  fixed_t frac;
  const fixed_t fracstep = dcvars->iscale;



  const fixed_t slope_texu = dcvars->texu;




  if (dcvars->iscale > drawvars.mag_threshold) {
    R_GetDrawColumnFunc(RDC_PIPELINE_STANDARD,
                        RDRAW_FILTER_POINT,
                        drawvars.filterz)(dcvars);
    return;
  }
  count = dcvars->yh - dcvars->yl;







  if (count < 0)
    return;





    frac = dcvars->texturemid + (dcvars->yl-centery)*fracstep;


  if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED) {



    if (dcvars->yl != 0) {
      if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

        int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
        dcvars->yl += shift;
        count -= shift;
        frac += 0xffff-(slope_texu & 0xffff);
      }
      else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

        int shift = ((slope_texu & 0xffff)/dcvars->iscale);
        dcvars->yl += shift;
        count -= shift;
        frac += slope_texu & 0xffff;
      }
    }
    if (dcvars->yh != viewheight-1) {
      if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

        int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
        dcvars->yh -= shift;
        count -= shift;
      }
      else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

        int shift = ((slope_texu & 0xffff)/dcvars->iscale);
        dcvars->yh -= shift;
        count -= shift;
      }
    }
    if (count <= 0) return;
  }



   {

      if(temp_x == 4 ||
         (temp_x && (temptype != (COL_OPAQUE) || temp_x + startx != dcvars->x)))
         R_FlushColumns();

      if(!temp_x)
      {
         startx = dcvars->x;
         tempyl[0] = commontop = dcvars->yl;
         tempyh[0] = commonbot = dcvars->yh;
         temptype = (COL_OPAQUE);





         R_FlushWholeColumns = R_FlushWhole16;
         R_FlushHTColumns = R_FlushHT16;
         R_FlushQuadColumn = R_FlushQuad16;

         dest = &short_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tempyl[temp_x] = dcvars->yl;
         tempyh[temp_x] = dcvars->yh;

         if(dcvars->yl > commontop)
            commontop = dcvars->yl;
         if(dcvars->yh < commonbot)
            commonbot = dcvars->yh;


         dest = &short_tempbuf[(dcvars->yl << 2) + temp_x];

      }
      temp_x += 1;
   }



   {
      const uint8_t *source = dcvars->source;
      const lighttable_t *colormap = dcvars->colormap;
      const uint8_t *translation = dcvars->translation;

      int y = dcvars->yl;
      const int x = dcvars->x;
      const uint8_t *prevsource = dcvars->prevsource;
      const uint8_t *nextsource = dcvars->nextsource;
      const unsigned int filter_fracu = (dcvars->source == dcvars->nextsource) ? 0 : (dcvars->texu>>8) & 0xff;


      count++;







      if (dcvars->texheight == 128)
      {

         while(count--)
         {
            *dest = (V_Palette16[ ((filter_getScale2xQuadColors( source[ ((frac & ((127<<16)|0xffff))>>16) ], source[ (((0)>(((frac & ((127<<16)|0xffff))>>16)-1)?(0):(((frac & ((127<<16)|0xffff))>>16)-1))) ], nextsource[ ((frac & ((127<<16)|0xffff))>>16) ], source[ (((frac+(1<<16)) & ((127<<16)|0xffff))>>16) ], prevsource[ ((frac & ((127<<16)|0xffff))>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & ((127<<16)|0xffff))>>8) & 0xff)>>(8-6)) ] ]))*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (V_Palette16[ ((filter_getScale2xQuadColors( source[ ((frac)>>16) ], source[ (((0)>(((frac)>>16)-1)?(0):(((frac)>>16)-1))) ], nextsource[ ((frac)>>16) ], source[ (((frac+(1<<16)))>>16) ], prevsource[ ((frac)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac)>>8) & 0xff)>>(8-6)) ] ]))*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else
      {
         unsigned heightmask = dcvars->texheight-1;
         if (! (dcvars->texheight & heightmask))
         {
            fixed_t fixedt_heightmask = (heightmask<<16)|0xffff;
            while ((count-=2)>=0)
            {
               *dest = (V_Palette16[ ((filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (V_Palette16[ ((filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (V_Palette16[ ((filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ]))*64 + ((64 -1)) ]);
            (y++);
         }
         else
         {
            fixed_t nextfrac = 0;

            heightmask++;
            heightmask <<= 16;

            if (frac < 0)
               while ((frac += heightmask) < 0);
            else
               while (frac >= (int)heightmask)
                  frac -= heightmask;


            nextfrac = frac + (1<<16);
            while (nextfrac >= (int)heightmask)
               nextfrac -= heightmask;




            while (count--)
            {





               *dest = (V_Palette16[ ((filter_getScale2xQuadColors( source[ ((frac)>>16) ], source[ (((0)>(((frac)>>16)-1)?(0):(((frac)>>16)-1))) ], nextsource[ ((frac)>>16) ], source[ ((nextfrac)>>16) ], prevsource[ ((frac)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac)>>8) & 0xff)>>(8-6)) ] ]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;

               if ((nextfrac += fracstep) >= (int)heightmask) nextfrac -= heightmask;;

            }
         }
      }
   }

}

/* rounded with simple depth color mapping */
static void R_DrawColumn16_RoundedUV_PointZ(draw_column_vars_t *dcvars)
{
  int count;

  uint16_t *dest;

  fixed_t frac;
  const fixed_t fracstep = dcvars->iscale;



  const fixed_t slope_texu = dcvars->texu;




  if (dcvars->iscale > drawvars.mag_threshold) {
    R_GetDrawColumnFunc(RDC_PIPELINE_STANDARD,
                        RDRAW_FILTER_POINT,
                        drawvars.filterz)(dcvars);
    return;
  }
  count = dcvars->yh - dcvars->yl;







  if (count < 0)
    return;





    frac = dcvars->texturemid + (dcvars->yl-centery)*fracstep;


  if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED) {



    if (dcvars->yl != 0) {
      if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

        int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
        dcvars->yl += shift;
        count -= shift;
        frac += 0xffff-(slope_texu & 0xffff);
      }
      else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

        int shift = ((slope_texu & 0xffff)/dcvars->iscale);
        dcvars->yl += shift;
        count -= shift;
        frac += slope_texu & 0xffff;
      }
    }
    if (dcvars->yh != viewheight-1) {
      if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

        int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
        dcvars->yh -= shift;
        count -= shift;
      }
      else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

        int shift = ((slope_texu & 0xffff)/dcvars->iscale);
        dcvars->yh -= shift;
        count -= shift;
      }
    }
    if (count <= 0) return;
  }



   {

      if(temp_x == 4 ||
         (temp_x && (temptype != (COL_OPAQUE) || temp_x + startx != dcvars->x)))
         R_FlushColumns();

      if(!temp_x)
      {
         startx = dcvars->x;
         tempyl[0] = commontop = dcvars->yl;
         tempyh[0] = commonbot = dcvars->yh;
         temptype = (COL_OPAQUE);





         R_FlushWholeColumns = R_FlushWhole16;
         R_FlushHTColumns = R_FlushHT16;
         R_FlushQuadColumn = R_FlushQuad16;

         dest = &short_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tempyl[temp_x] = dcvars->yl;
         tempyh[temp_x] = dcvars->yh;

         if(dcvars->yl > commontop)
            commontop = dcvars->yl;
         if(dcvars->yh < commonbot)
            commonbot = dcvars->yh;


         dest = &short_tempbuf[(dcvars->yl << 2) + temp_x];

      }
      temp_x += 1;
   }



   {
      const uint8_t *source = dcvars->source;
      const lighttable_t *colormap = dcvars->colormap;
      const uint8_t *translation = dcvars->translation;

      int y = dcvars->yl;
      const int x = dcvars->x;
      const uint8_t *prevsource = dcvars->prevsource;
      const uint8_t *nextsource = dcvars->nextsource;
      const unsigned int filter_fracu = (dcvars->source == dcvars->nextsource) ? 0 : (dcvars->texu>>8) & 0xff;


      count++;







      if (dcvars->texheight == 128)
      {

         while(count--)
         {
            *dest = (V_Palette16[ (colormap[(filter_getScale2xQuadColors( source[ ((frac & ((127<<16)|0xffff))>>16) ], source[ (((0)>(((frac & ((127<<16)|0xffff))>>16)-1)?(0):(((frac & ((127<<16)|0xffff))>>16)-1))) ], nextsource[ ((frac & ((127<<16)|0xffff))>>16) ], source[ (((frac+(1<<16)) & ((127<<16)|0xffff))>>16) ], prevsource[ ((frac & ((127<<16)|0xffff))>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & ((127<<16)|0xffff))>>8) & 0xff)>>(8-6)) ] ])])*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (V_Palette16[ (colormap[(filter_getScale2xQuadColors( source[ ((frac)>>16) ], source[ (((0)>(((frac)>>16)-1)?(0):(((frac)>>16)-1))) ], nextsource[ ((frac)>>16) ], source[ (((frac+(1<<16)))>>16) ], prevsource[ ((frac)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac)>>8) & 0xff)>>(8-6)) ] ])])*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else
      {
         unsigned heightmask = dcvars->texheight-1;
         if (! (dcvars->texheight & heightmask))
         {
            fixed_t fixedt_heightmask = (heightmask<<16)|0xffff;
            while ((count-=2)>=0)
            {
               *dest = (V_Palette16[ (colormap[(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])])*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (V_Palette16[ (colormap[(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])])*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (V_Palette16[ (colormap[(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])])*64 + ((64 -1)) ]);
            (y++);
         }
         else
         {
            fixed_t nextfrac = 0;

            heightmask++;
            heightmask <<= 16;

            if (frac < 0)
               while ((frac += heightmask) < 0);
            else
               while (frac >= (int)heightmask)
                  frac -= heightmask;


            nextfrac = frac + (1<<16);
            while (nextfrac >= (int)heightmask)
               nextfrac -= heightmask;




            while (count--)
            {





               *dest = (V_Palette16[ (colormap[(filter_getScale2xQuadColors( source[ ((frac)>>16) ], source[ (((0)>(((frac)>>16)-1)?(0):(((frac)>>16)-1))) ], nextsource[ ((frac)>>16) ], source[ ((nextfrac)>>16) ], prevsource[ ((frac)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac)>>8) & 0xff)>>(8-6)) ] ])])*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;

               if ((nextfrac += fracstep) >= (int)heightmask) nextfrac -= heightmask;;

            }
         }
      }
   }

}

/* rounded + z-dither */
static void R_DrawColumn16_RoundedUV_LinearZ(draw_column_vars_t *dcvars)
{
  int count;

  uint16_t *dest;

  fixed_t frac;
  const fixed_t fracstep = dcvars->iscale;



  const fixed_t slope_texu = dcvars->texu;




  if (dcvars->iscale > drawvars.mag_threshold) {
    R_GetDrawColumnFunc(RDC_PIPELINE_STANDARD,
                        RDRAW_FILTER_POINT,
                        drawvars.filterz)(dcvars);
    return;
  }
  count = dcvars->yh - dcvars->yl;







  if (count < 0)
    return;





    frac = dcvars->texturemid + (dcvars->yl-centery)*fracstep;


  if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED) {



    if (dcvars->yl != 0) {
      if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

        int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
        dcvars->yl += shift;
        count -= shift;
        frac += 0xffff-(slope_texu & 0xffff);
      }
      else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

        int shift = ((slope_texu & 0xffff)/dcvars->iscale);
        dcvars->yl += shift;
        count -= shift;
        frac += slope_texu & 0xffff;
      }
    }
    if (dcvars->yh != viewheight-1) {
      if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

        int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
        dcvars->yh -= shift;
        count -= shift;
      }
      else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

        int shift = ((slope_texu & 0xffff)/dcvars->iscale);
        dcvars->yh -= shift;
        count -= shift;
      }
    }
    if (count <= 0) return;
  }



   {

      if(temp_x == 4 ||
         (temp_x && (temptype != (COL_OPAQUE) || temp_x + startx != dcvars->x)))
         R_FlushColumns();

      if(!temp_x)
      {
         startx = dcvars->x;
         tempyl[0] = commontop = dcvars->yl;
         tempyh[0] = commonbot = dcvars->yh;
         temptype = (COL_OPAQUE);





         R_FlushWholeColumns = R_FlushWhole16;
         R_FlushHTColumns = R_FlushHT16;
         R_FlushQuadColumn = R_FlushQuad16;

         dest = &short_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tempyl[temp_x] = dcvars->yl;
         tempyh[temp_x] = dcvars->yh;

         if(dcvars->yl > commontop)
            commontop = dcvars->yl;
         if(dcvars->yh < commonbot)
            commonbot = dcvars->yh;


         dest = &short_tempbuf[(dcvars->yl << 2) + temp_x];

      }
      temp_x += 1;
   }



   {
      const uint8_t *source = dcvars->source;
      const lighttable_t *colormap = dcvars->colormap;
      const uint8_t *translation = dcvars->translation;

      int y = dcvars->yl;
      const int x = dcvars->x;


      const int fracz = (dcvars->z >> 6) & 255;
      const uint8_t *dither_colormaps[2] = { dcvars->colormap, dcvars->nextcolormap };






      const uint8_t *prevsource = dcvars->prevsource;
      const uint8_t *nextsource = dcvars->nextsource;
      const unsigned int filter_fracu = (dcvars->source == dcvars->nextsource) ? 0 : (dcvars->texu>>8) & 0xff;


      count++;







      if (dcvars->texheight == 128)
      {

         while(count--)
         {
            *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(filter_getScale2xQuadColors( source[ ((frac & ((127<<16)|0xffff))>>16) ], source[ (((0)>(((frac & ((127<<16)|0xffff))>>16)-1)?(0):(((frac & ((127<<16)|0xffff))>>16)-1))) ], nextsource[ ((frac & ((127<<16)|0xffff))>>16) ], source[ (((frac+(1<<16)) & ((127<<16)|0xffff))>>16) ], prevsource[ ((frac & ((127<<16)|0xffff))>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & ((127<<16)|0xffff))>>8) & 0xff)>>(8-6)) ] ])]))*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(filter_getScale2xQuadColors( source[ ((frac)>>16) ], source[ (((0)>(((frac)>>16)-1)?(0):(((frac)>>16)-1))) ], nextsource[ ((frac)>>16) ], source[ (((frac+(1<<16)))>>16) ], prevsource[ ((frac)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac)>>8) & 0xff)>>(8-6)) ] ])]))*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else
      {
         unsigned heightmask = dcvars->texheight-1;
         if (! (dcvars->texheight & heightmask))
         {
            fixed_t fixedt_heightmask = (heightmask<<16)|0xffff;
            while ((count-=2)>=0)
            {
               *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])]))*64 + ((64 -1)) ]);
            (y++);
         }
         else
         {
            fixed_t nextfrac = 0;

            heightmask++;
            heightmask <<= 16;

            if (frac < 0)
               while ((frac += heightmask) < 0);
            else
               while (frac >= (int)heightmask)
                  frac -= heightmask;


            nextfrac = frac + (1<<16);
            while (nextfrac >= (int)heightmask)
               nextfrac -= heightmask;




            while (count--)
            {





               *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(filter_getScale2xQuadColors( source[ ((frac)>>16) ], source[ (((0)>(((frac)>>16)-1)?(0):(((frac)>>16)-1))) ], nextsource[ ((frac)>>16) ], source[ ((nextfrac)>>16) ], prevsource[ ((frac)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac)>>8) & 0xff)>>(8-6)) ] ])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;

               if ((nextfrac += fracstep) >= (int)heightmask) nextfrac -= heightmask;;

            }
         }
      }
   }
}

//
// R_DrawTranslatedColumn
// Used to draw player sprites
//  with the green colorramp mapped to others.
// Could be used with different translation
//  tables, e.g. the lighter colored version
//  of the BaronOfHell, the HellKnight, uses
//  identical sprites, kinda brightened up.
//

static void R_DrawTranslatedColumn16_PointUV(draw_column_vars_t *dcvars)
{
  int count;

  uint16_t *dest;

  fixed_t frac;
  const fixed_t fracstep = dcvars->iscale;



  const fixed_t slope_texu = dcvars->texu;
  count = dcvars->yh - dcvars->yl;







  if (count < 0)
    return;





    frac = dcvars->texturemid + (dcvars->yl-centery)*fracstep;


  if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED) {



    if (dcvars->yl != 0) {
      if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

        int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
        dcvars->yl += shift;
        count -= shift;
        frac += 0xffff-(slope_texu & 0xffff);
      }
      else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

        int shift = ((slope_texu & 0xffff)/dcvars->iscale);
        dcvars->yl += shift;
        count -= shift;
        frac += slope_texu & 0xffff;
      }
    }
    if (dcvars->yh != viewheight-1) {
      if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

        int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
        dcvars->yh -= shift;
        count -= shift;
      }
      else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

        int shift = ((slope_texu & 0xffff)/dcvars->iscale);
        dcvars->yh -= shift;
        count -= shift;
      }
    }
    if (count <= 0) return;
  }



   {

      if(temp_x == 4 ||
         (temp_x && (temptype != (COL_OPAQUE) || temp_x + startx != dcvars->x)))
         R_FlushColumns();

      if(!temp_x)
      {
         startx = dcvars->x;
         tempyl[0] = commontop = dcvars->yl;
         tempyh[0] = commonbot = dcvars->yh;
         temptype = (COL_OPAQUE);





         R_FlushWholeColumns = R_FlushWhole16;
         R_FlushHTColumns = R_FlushHT16;
         R_FlushQuadColumn = R_FlushQuad16;

         dest = &short_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tempyl[temp_x] = dcvars->yl;
         tempyh[temp_x] = dcvars->yh;

         if(dcvars->yl > commontop)
            commontop = dcvars->yl;
         if(dcvars->yh < commonbot)
            commonbot = dcvars->yh;


         dest = &short_tempbuf[(dcvars->yl << 2) + temp_x];

      }
      temp_x += 1;
   }



   {
      const uint8_t *source = dcvars->source;
      const lighttable_t *colormap = dcvars->colormap;
      const uint8_t *translation = dcvars->translation;
      count++;







      if (dcvars->texheight == 128)
      {

         while(count--)
         {
            *dest = (V_Palette16[ ((translation[(source[(frac & ((127<<16)|0xffff))>>16])]))*64 + ((64 -1)) ]);
            ;
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (V_Palette16[ ((translation[(source[(frac)>>16])]))*64 + ((64 -1)) ]);
            ;
            dest += 4;
            frac += fracstep;
         }
      }
      else
      {
         unsigned heightmask = dcvars->texheight-1;
         if (! (dcvars->texheight & heightmask))
         {
            fixed_t fixedt_heightmask = (heightmask<<16)|0xffff;
            while ((count-=2)>=0)
            {
               *dest = (V_Palette16[ ((translation[(source[(frac & fixedt_heightmask)>>16])]))*64 + ((64 -1)) ]);
               ;
               dest += 4;
               frac += fracstep;
               *dest = (V_Palette16[ ((translation[(source[(frac & fixedt_heightmask)>>16])]))*64 + ((64 -1)) ]);
               ;
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (V_Palette16[ ((translation[(source[(frac & fixedt_heightmask)>>16])]))*64 + ((64 -1)) ]);
            ;
         }
         else
         {
            fixed_t nextfrac = 0;

            heightmask++;
            heightmask <<= 16;

            if (frac < 0)
               while ((frac += heightmask) < 0);
            else
               while (frac >= (int)heightmask)
                  frac -= heightmask;
            while (count--)
            {





               *dest = (V_Palette16[ ((translation[(source[(frac)>>16])]))*64 + ((64 -1)) ]);
               ;
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;



            }
         }
      }
   }

}

static void R_DrawTranslatedColumn16_PointUV_PointZ(draw_column_vars_t *dcvars)
{
  int count;

  uint16_t *dest;

  fixed_t frac;
  const fixed_t fracstep = dcvars->iscale;



  const fixed_t slope_texu = dcvars->texu;
  count = dcvars->yh - dcvars->yl;







  if (count < 0)
    return;





    frac = dcvars->texturemid + (dcvars->yl-centery)*fracstep;


  if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED) {



    if (dcvars->yl != 0) {
      if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

        int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
        dcvars->yl += shift;
        count -= shift;
        frac += 0xffff-(slope_texu & 0xffff);
      }
      else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

        int shift = ((slope_texu & 0xffff)/dcvars->iscale);
        dcvars->yl += shift;
        count -= shift;
        frac += slope_texu & 0xffff;
      }
    }
    if (dcvars->yh != viewheight-1) {
      if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

        int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
        dcvars->yh -= shift;
        count -= shift;
      }
      else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

        int shift = ((slope_texu & 0xffff)/dcvars->iscale);
        dcvars->yh -= shift;
        count -= shift;
      }
    }
    if (count <= 0) return;
  }



   {

      if(temp_x == 4 ||
         (temp_x && (temptype != (COL_OPAQUE) || temp_x + startx != dcvars->x)))
         R_FlushColumns();

      if(!temp_x)
      {
         startx = dcvars->x;
         tempyl[0] = commontop = dcvars->yl;
         tempyh[0] = commonbot = dcvars->yh;
         temptype = (COL_OPAQUE);





         R_FlushWholeColumns = R_FlushWhole16;
         R_FlushHTColumns = R_FlushHT16;
         R_FlushQuadColumn = R_FlushQuad16;

         dest = &short_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tempyl[temp_x] = dcvars->yl;
         tempyh[temp_x] = dcvars->yh;

         if(dcvars->yl > commontop)
            commontop = dcvars->yl;
         if(dcvars->yh < commonbot)
            commonbot = dcvars->yh;


         dest = &short_tempbuf[(dcvars->yl << 2) + temp_x];

      }
      temp_x += 1;
   }



   {
      const uint8_t *source = dcvars->source;
      const lighttable_t *colormap = dcvars->colormap;
      const uint8_t *translation = dcvars->translation;
      count++;







      if (dcvars->texheight == 128)
      {

         while(count--)
         {
            *dest = (V_Palette16[ (colormap[(translation[(source[(frac & ((127<<16)|0xffff))>>16])])])*64 + ((64 -1)) ]);
            ;
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (V_Palette16[ (colormap[(translation[(source[(frac)>>16])])])*64 + ((64 -1)) ]);
            ;
            dest += 4;
            frac += fracstep;
         }
      }
      else
      {
         unsigned heightmask = dcvars->texheight-1;
         if (! (dcvars->texheight & heightmask))
         {
            fixed_t fixedt_heightmask = (heightmask<<16)|0xffff;
            while ((count-=2)>=0)
            {
               *dest = (V_Palette16[ (colormap[(translation[(source[(frac & fixedt_heightmask)>>16])])])*64 + ((64 -1)) ]);
               ;
               dest += 4;
               frac += fracstep;
               *dest = (V_Palette16[ (colormap[(translation[(source[(frac & fixedt_heightmask)>>16])])])*64 + ((64 -1)) ]);
               ;
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (V_Palette16[ (colormap[(translation[(source[(frac & fixedt_heightmask)>>16])])])*64 + ((64 -1)) ]);
            ;
         }
         else
         {
            fixed_t nextfrac = 0;

            heightmask++;
            heightmask <<= 16;

            if (frac < 0)
               while ((frac += heightmask) < 0);
            else
               while (frac >= (int)heightmask)
                  frac -= heightmask;
            while (count--)
            {





               *dest = (V_Palette16[ (colormap[(translation[(source[(frac)>>16])])])*64 + ((64 -1)) ]);
               ;
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;



            }
         }
      }
   }

}


static void R_DrawTranslatedColumn16_PointUV_LinearZ(draw_column_vars_t *dcvars)
{
  int count;

  uint16_t *dest;

  fixed_t frac;
  const fixed_t fracstep = dcvars->iscale;



  const fixed_t slope_texu = dcvars->texu;
  count = dcvars->yh - dcvars->yl;







  if (count < 0)
    return;





    frac = dcvars->texturemid + (dcvars->yl-centery)*fracstep;


  if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED) {



    if (dcvars->yl != 0) {
      if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

        int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
        dcvars->yl += shift;
        count -= shift;
        frac += 0xffff-(slope_texu & 0xffff);
      }
      else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

        int shift = ((slope_texu & 0xffff)/dcvars->iscale);
        dcvars->yl += shift;
        count -= shift;
        frac += slope_texu & 0xffff;
      }
    }
    if (dcvars->yh != viewheight-1) {
      if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

        int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
        dcvars->yh -= shift;
        count -= shift;
      }
      else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

        int shift = ((slope_texu & 0xffff)/dcvars->iscale);
        dcvars->yh -= shift;
        count -= shift;
      }
    }
    if (count <= 0) return;
  }



   {

      if(temp_x == 4 ||
         (temp_x && (temptype != (COL_OPAQUE) || temp_x + startx != dcvars->x)))
         R_FlushColumns();

      if(!temp_x)
      {
         startx = dcvars->x;
         tempyl[0] = commontop = dcvars->yl;
         tempyh[0] = commonbot = dcvars->yh;
         temptype = (COL_OPAQUE);





         R_FlushWholeColumns = R_FlushWhole16;
         R_FlushHTColumns = R_FlushHT16;
         R_FlushQuadColumn = R_FlushQuad16;

         dest = &short_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tempyl[temp_x] = dcvars->yl;
         tempyh[temp_x] = dcvars->yh;

         if(dcvars->yl > commontop)
            commontop = dcvars->yl;
         if(dcvars->yh < commonbot)
            commonbot = dcvars->yh;


         dest = &short_tempbuf[(dcvars->yl << 2) + temp_x];

      }
      temp_x += 1;
   }



   {
      const uint8_t *source = dcvars->source;
      const lighttable_t *colormap = dcvars->colormap;
      const uint8_t *translation = dcvars->translation;

      int y = dcvars->yl;
      const int x = dcvars->x;


      const int fracz = (dcvars->z >> 6) & 255;
      const uint8_t *dither_colormaps[2] = { dcvars->colormap, dcvars->nextcolormap };
      count++;







      if (dcvars->texheight == 128)
      {

         while(count--)
         {
            *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(frac & ((127<<16)|0xffff))>>16])])]))*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(frac)>>16])])]))*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else
      {
         unsigned heightmask = dcvars->texheight-1;
         if (! (dcvars->texheight & heightmask))
         {
            fixed_t fixedt_heightmask = (heightmask<<16)|0xffff;
            while ((count-=2)>=0)
            {
               *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(frac & fixedt_heightmask)>>16])])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(frac & fixedt_heightmask)>>16])])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(frac & fixedt_heightmask)>>16])])]))*64 + ((64 -1)) ]);
            (y++);
         }
         else
         {
            fixed_t nextfrac = 0;

            heightmask++;
            heightmask <<= 16;

            if (frac < 0)
               while ((frac += heightmask) < 0);
            else
               while (frac >= (int)heightmask)
                  frac -= heightmask;
            while (count--)
            {





               *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(frac)>>16])])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;



            }
         }
      }
   }

}

static void R_DrawTranslatedColumn16_LinearUV(draw_column_vars_t *dcvars)
{
  int count;

  uint16_t *dest;

  fixed_t frac;
  const fixed_t fracstep = dcvars->iscale;

  const fixed_t slope_texu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;






  if (dcvars->iscale > drawvars.mag_threshold) {
    R_GetDrawColumnFunc(RDC_PIPELINE_TRANSLATED,
                        RDRAW_FILTER_POINT,
                        drawvars.filterz)(dcvars);
    return;
  }
  count = dcvars->yh - dcvars->yl;







  if (count < 0)
    return;



    frac = dcvars->texturemid - ((1<<16)>>1) + (dcvars->yl-centery)*fracstep;




  if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED) {



    if (dcvars->yl != 0) {
      if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

        int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
        dcvars->yl += shift;
        count -= shift;
        frac += 0xffff-(slope_texu & 0xffff);
      }
      else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

        int shift = ((slope_texu & 0xffff)/dcvars->iscale);
        dcvars->yl += shift;
        count -= shift;
        frac += slope_texu & 0xffff;
      }
    }
    if (dcvars->yh != viewheight-1) {
      if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

        int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
        dcvars->yh -= shift;
        count -= shift;
      }
      else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

        int shift = ((slope_texu & 0xffff)/dcvars->iscale);
        dcvars->yh -= shift;
        count -= shift;
      }
    }
    if (count <= 0) return;
  }



   {

      if(temp_x == 4 ||
         (temp_x && (temptype != (COL_OPAQUE) || temp_x + startx != dcvars->x)))
         R_FlushColumns();

      if(!temp_x)
      {
         startx = dcvars->x;
         tempyl[0] = commontop = dcvars->yl;
         tempyh[0] = commonbot = dcvars->yh;
         temptype = (COL_OPAQUE);





         R_FlushWholeColumns = R_FlushWhole16;
         R_FlushHTColumns = R_FlushHT16;
         R_FlushQuadColumn = R_FlushQuad16;

         dest = &short_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tempyl[temp_x] = dcvars->yl;
         tempyh[temp_x] = dcvars->yh;

         if(dcvars->yl > commontop)
            commontop = dcvars->yl;
         if(dcvars->yh < commonbot)
            commonbot = dcvars->yh;


         dest = &short_tempbuf[(dcvars->yl << 2) + temp_x];

      }
      temp_x += 1;
   }



   {
      const uint8_t *source = dcvars->source;
      const lighttable_t *colormap = dcvars->colormap;
      const uint8_t *translation = dcvars->translation;

      int y = dcvars->yl;
      const int x = dcvars->x;






      const uint8_t *nextsource = dcvars->nextsource;
      const unsigned int filter_fracu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;







      count++;







      if (dcvars->texheight == 128)
      {

         while(count--)
         {
            *dest = (( V_Palette16[ ((translation[(nextsource[((frac+(1<<16)) & ((127<<16)|0xffff))>>16])]))*64 + ((filter_fracu*((frac & ((127<<16)|0xffff))&0xffff))>>(32-6)) ] + V_Palette16[ ((translation[(source[((frac+(1<<16)) & ((127<<16)|0xffff))>>16])]))*64 + (((0xffff-filter_fracu)*((frac & ((127<<16)|0xffff))&0xffff))>>(32-6)) ] + V_Palette16[ ((translation[(source[(frac & ((127<<16)|0xffff))>>16])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & ((127<<16)|0xffff))&0xffff)))>>(32-6)) ] + V_Palette16[ ((translation[(nextsource[(frac & ((127<<16)|0xffff))>>16])]))*64 + ((filter_fracu*(0xffff-((frac & ((127<<16)|0xffff))&0xffff)))>>(32-6)) ]));
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (( V_Palette16[ ((translation[(nextsource[((frac+(1<<16)))>>16])]))*64 + ((filter_fracu*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ ((translation[(source[((frac+(1<<16)))>>16])]))*64 + (((0xffff-filter_fracu)*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ ((translation[(source[(frac)>>16])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac)&0xffff)))>>(32-6)) ] + V_Palette16[ ((translation[(nextsource[(frac)>>16])]))*64 + ((filter_fracu*(0xffff-((frac)&0xffff)))>>(32-6)) ]));
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else
      {
         unsigned heightmask = dcvars->texheight-1;
         if (! (dcvars->texheight & heightmask))
         {
            fixed_t fixedt_heightmask = (heightmask<<16)|0xffff;
            while ((count-=2)>=0)
            {
               *dest = (( V_Palette16[ ((translation[(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])]))*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((translation[(source[((frac+(1<<16)) & fixedt_heightmask)>>16])]))*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((translation[(source[(frac & fixedt_heightmask)>>16])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_Palette16[ ((translation[(nextsource[(frac & fixedt_heightmask)>>16])]))*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (( V_Palette16[ ((translation[(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])]))*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((translation[(source[((frac+(1<<16)) & fixedt_heightmask)>>16])]))*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((translation[(source[(frac & fixedt_heightmask)>>16])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_Palette16[ ((translation[(nextsource[(frac & fixedt_heightmask)>>16])]))*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (( V_Palette16[ ((translation[(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])]))*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((translation[(source[((frac+(1<<16)) & fixedt_heightmask)>>16])]))*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((translation[(source[(frac & fixedt_heightmask)>>16])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_Palette16[ ((translation[(nextsource[(frac & fixedt_heightmask)>>16])]))*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
            (y++);
         }
         else
         {
            fixed_t nextfrac = 0;

            heightmask++;
            heightmask <<= 16;

            if (frac < 0)
               while ((frac += heightmask) < 0);
            else
               while (frac >= (int)heightmask)
                  frac -= heightmask;


            nextfrac = frac + (1<<16);
            while (nextfrac >= (int)heightmask)
               nextfrac -= heightmask;




            while (count--)
            {





               *dest = (( V_Palette16[ ((translation[(nextsource[(nextfrac)>>16])]))*64 + ((filter_fracu*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ ((translation[(source[(nextfrac)>>16])]))*64 + (((0xffff-filter_fracu)*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ ((translation[(source[(frac)>>16])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac)&0xffff)))>>(32-6)) ] + V_Palette16[ ((translation[(nextsource[(frac)>>16])]))*64 + ((filter_fracu*(0xffff-((frac)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;

               if ((nextfrac += fracstep) >= (int)heightmask) nextfrac -= heightmask;;

            }
         }
      }
   }

}

static void R_DrawTranslatedColumn16_LinearUV_PointZ(draw_column_vars_t *dcvars)
{
  int count;

  uint16_t *dest;

  fixed_t frac;
  const fixed_t fracstep = dcvars->iscale;

  const fixed_t slope_texu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;






  if (dcvars->iscale > drawvars.mag_threshold) {
    R_GetDrawColumnFunc(RDC_PIPELINE_TRANSLATED,
                        RDRAW_FILTER_POINT,
                        drawvars.filterz)(dcvars);
    return;
  }
  count = dcvars->yh - dcvars->yl;







  if (count < 0)
    return;



    frac = dcvars->texturemid - ((1<<16)>>1) + (dcvars->yl-centery)*fracstep;




  if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED) {



    if (dcvars->yl != 0) {
      if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

        int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
        dcvars->yl += shift;
        count -= shift;
        frac += 0xffff-(slope_texu & 0xffff);
      }
      else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

        int shift = ((slope_texu & 0xffff)/dcvars->iscale);
        dcvars->yl += shift;
        count -= shift;
        frac += slope_texu & 0xffff;
      }
    }
    if (dcvars->yh != viewheight-1) {
      if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

        int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
        dcvars->yh -= shift;
        count -= shift;
      }
      else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

        int shift = ((slope_texu & 0xffff)/dcvars->iscale);
        dcvars->yh -= shift;
        count -= shift;
      }
    }
    if (count <= 0) return;
  }



   {

      if(temp_x == 4 ||
         (temp_x && (temptype != (COL_OPAQUE) || temp_x + startx != dcvars->x)))
         R_FlushColumns();

      if(!temp_x)
      {
         startx = dcvars->x;
         tempyl[0] = commontop = dcvars->yl;
         tempyh[0] = commonbot = dcvars->yh;
         temptype = (COL_OPAQUE);





         R_FlushWholeColumns = R_FlushWhole16;
         R_FlushHTColumns = R_FlushHT16;
         R_FlushQuadColumn = R_FlushQuad16;

         dest = &short_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tempyl[temp_x] = dcvars->yl;
         tempyh[temp_x] = dcvars->yh;

         if(dcvars->yl > commontop)
            commontop = dcvars->yl;
         if(dcvars->yh < commonbot)
            commonbot = dcvars->yh;


         dest = &short_tempbuf[(dcvars->yl << 2) + temp_x];

      }
      temp_x += 1;
   }



   {
      const uint8_t *source = dcvars->source;
      const lighttable_t *colormap = dcvars->colormap;
      const uint8_t *translation = dcvars->translation;

      int y = dcvars->yl;
      const int x = dcvars->x;






      const uint8_t *nextsource = dcvars->nextsource;
      const unsigned int filter_fracu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;







      count++;







      if (dcvars->texheight == 128)
      {

         while(count--)
         {
            *dest = (( V_Palette16[ (colormap[(translation[(nextsource[((frac+(1<<16)) & ((127<<16)|0xffff))>>16])])])*64 + ((filter_fracu*((frac & ((127<<16)|0xffff))&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(translation[(source[((frac+(1<<16)) & ((127<<16)|0xffff))>>16])])])*64 + (((0xffff-filter_fracu)*((frac & ((127<<16)|0xffff))&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(translation[(source[(frac & ((127<<16)|0xffff))>>16])])])*64 + (((0xffff-filter_fracu)*(0xffff-((frac & ((127<<16)|0xffff))&0xffff)))>>(32-6)) ] + V_Palette16[ (colormap[(translation[(nextsource[(frac & ((127<<16)|0xffff))>>16])])])*64 + ((filter_fracu*(0xffff-((frac & ((127<<16)|0xffff))&0xffff)))>>(32-6)) ]));
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (( V_Palette16[ (colormap[(translation[(nextsource[((frac+(1<<16)))>>16])])])*64 + ((filter_fracu*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(translation[(source[((frac+(1<<16)))>>16])])])*64 + (((0xffff-filter_fracu)*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(translation[(source[(frac)>>16])])])*64 + (((0xffff-filter_fracu)*(0xffff-((frac)&0xffff)))>>(32-6)) ] + V_Palette16[ (colormap[(translation[(nextsource[(frac)>>16])])])*64 + ((filter_fracu*(0xffff-((frac)&0xffff)))>>(32-6)) ]));
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else
      {
         unsigned heightmask = dcvars->texheight-1;
         if (! (dcvars->texheight & heightmask))
         {
            fixed_t fixedt_heightmask = (heightmask<<16)|0xffff;
            while ((count-=2)>=0)
            {
               *dest = (( V_Palette16[ (colormap[(translation[(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])])])*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(translation[(source[((frac+(1<<16)) & fixedt_heightmask)>>16])])])*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(translation[(source[(frac & fixedt_heightmask)>>16])])])*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_Palette16[ (colormap[(translation[(nextsource[(frac & fixedt_heightmask)>>16])])])*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (( V_Palette16[ (colormap[(translation[(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])])])*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(translation[(source[((frac+(1<<16)) & fixedt_heightmask)>>16])])])*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(translation[(source[(frac & fixedt_heightmask)>>16])])])*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_Palette16[ (colormap[(translation[(nextsource[(frac & fixedt_heightmask)>>16])])])*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (( V_Palette16[ (colormap[(translation[(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])])])*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(translation[(source[((frac+(1<<16)) & fixedt_heightmask)>>16])])])*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(translation[(source[(frac & fixedt_heightmask)>>16])])])*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_Palette16[ (colormap[(translation[(nextsource[(frac & fixedt_heightmask)>>16])])])*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
            (y++);
         }
         else
         {
            fixed_t nextfrac = 0;

            heightmask++;
            heightmask <<= 16;

            if (frac < 0)
               while ((frac += heightmask) < 0);
            else
               while (frac >= (int)heightmask)
                  frac -= heightmask;


            nextfrac = frac + (1<<16);
            while (nextfrac >= (int)heightmask)
               nextfrac -= heightmask;




            while (count--)
            {





               *dest = (( V_Palette16[ (colormap[(translation[(nextsource[(nextfrac)>>16])])])*64 + ((filter_fracu*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(translation[(source[(nextfrac)>>16])])])*64 + (((0xffff-filter_fracu)*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(translation[(source[(frac)>>16])])])*64 + (((0xffff-filter_fracu)*(0xffff-((frac)&0xffff)))>>(32-6)) ] + V_Palette16[ (colormap[(translation[(nextsource[(frac)>>16])])])*64 + ((filter_fracu*(0xffff-((frac)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;

               if ((nextfrac += fracstep) >= (int)heightmask) nextfrac -= heightmask;;

            }
         }
      }
   }

}

static void R_DrawTranslatedColumn16_LinearUV_LinearZ(draw_column_vars_t *dcvars)
{
  int count;

  uint16_t *dest;

  fixed_t frac;
  const fixed_t fracstep = dcvars->iscale;

  const fixed_t slope_texu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;






  if (dcvars->iscale > drawvars.mag_threshold) {
    R_GetDrawColumnFunc(RDC_PIPELINE_TRANSLATED,
                        RDRAW_FILTER_POINT,
                        drawvars.filterz)(dcvars);
    return;
  }
  count = dcvars->yh - dcvars->yl;







  if (count < 0)
    return;



    frac = dcvars->texturemid - ((1<<16)>>1) + (dcvars->yl-centery)*fracstep;




  if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED) {



    if (dcvars->yl != 0) {
      if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

        int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
        dcvars->yl += shift;
        count -= shift;
        frac += 0xffff-(slope_texu & 0xffff);
      }
      else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

        int shift = ((slope_texu & 0xffff)/dcvars->iscale);
        dcvars->yl += shift;
        count -= shift;
        frac += slope_texu & 0xffff;
      }
    }
    if (dcvars->yh != viewheight-1) {
      if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

        int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
        dcvars->yh -= shift;
        count -= shift;
      }
      else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

        int shift = ((slope_texu & 0xffff)/dcvars->iscale);
        dcvars->yh -= shift;
        count -= shift;
      }
    }
    if (count <= 0) return;
  }



   {

      if(temp_x == 4 ||
         (temp_x && (temptype != (COL_OPAQUE) || temp_x + startx != dcvars->x)))
         R_FlushColumns();

      if(!temp_x)
      {
         startx = dcvars->x;
         tempyl[0] = commontop = dcvars->yl;
         tempyh[0] = commonbot = dcvars->yh;
         temptype = (COL_OPAQUE);





         R_FlushWholeColumns = R_FlushWhole16;
         R_FlushHTColumns = R_FlushHT16;
         R_FlushQuadColumn = R_FlushQuad16;

         dest = &short_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tempyl[temp_x] = dcvars->yl;
         tempyh[temp_x] = dcvars->yh;

         if(dcvars->yl > commontop)
            commontop = dcvars->yl;
         if(dcvars->yh < commonbot)
            commonbot = dcvars->yh;


         dest = &short_tempbuf[(dcvars->yl << 2) + temp_x];

      }
      temp_x += 1;
   }



   {
      const uint8_t *source = dcvars->source;
      const lighttable_t *colormap = dcvars->colormap;
      const uint8_t *translation = dcvars->translation;

      int y = dcvars->yl;
      const int x = dcvars->x;


      const int fracz = (dcvars->z >> 6) & 255;
      const uint8_t *dither_colormaps[2] = { dcvars->colormap, dcvars->nextcolormap };


      const uint8_t *nextsource = dcvars->nextsource;
      const unsigned int filter_fracu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;







      count++;







      if (dcvars->texheight == 128)
      {

         while(count--)
         {
            *dest = (( V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(nextsource[((frac+(1<<16)) & ((127<<16)|0xffff))>>16])])]))*64 + ((filter_fracu*((frac & ((127<<16)|0xffff))&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[((frac+(1<<16)) & ((127<<16)|0xffff))>>16])])]))*64 + (((0xffff-filter_fracu)*((frac & ((127<<16)|0xffff))&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(frac & ((127<<16)|0xffff))>>16])])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & ((127<<16)|0xffff))&0xffff)))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(nextsource[(frac & ((127<<16)|0xffff))>>16])])]))*64 + ((filter_fracu*(0xffff-((frac & ((127<<16)|0xffff))&0xffff)))>>(32-6)) ]));
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (( V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(nextsource[((frac+(1<<16)))>>16])])]))*64 + ((filter_fracu*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[((frac+(1<<16)))>>16])])]))*64 + (((0xffff-filter_fracu)*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(frac)>>16])])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac)&0xffff)))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(nextsource[(frac)>>16])])]))*64 + ((filter_fracu*(0xffff-((frac)&0xffff)))>>(32-6)) ]));
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else
      {
         unsigned heightmask = dcvars->texheight-1;
         if (! (dcvars->texheight & heightmask))
         {
            fixed_t fixedt_heightmask = (heightmask<<16)|0xffff;
            while ((count-=2)>=0)
            {
               *dest = (( V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])])]))*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[((frac+(1<<16)) & fixedt_heightmask)>>16])])]))*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(frac & fixedt_heightmask)>>16])])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(nextsource[(frac & fixedt_heightmask)>>16])])]))*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (( V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])])]))*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[((frac+(1<<16)) & fixedt_heightmask)>>16])])]))*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(frac & fixedt_heightmask)>>16])])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(nextsource[(frac & fixedt_heightmask)>>16])])]))*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (( V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(nextsource[((frac+(1<<16)) & fixedt_heightmask)>>16])])]))*64 + ((filter_fracu*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[((frac+(1<<16)) & fixedt_heightmask)>>16])])]))*64 + (((0xffff-filter_fracu)*((frac & fixedt_heightmask)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(frac & fixedt_heightmask)>>16])])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(nextsource[(frac & fixedt_heightmask)>>16])])]))*64 + ((filter_fracu*(0xffff-((frac & fixedt_heightmask)&0xffff)))>>(32-6)) ]));
            (y++);
         }
         else
         {
            fixed_t nextfrac = 0;

            heightmask++;
            heightmask <<= 16;

            if (frac < 0)
               while ((frac += heightmask) < 0);
            else
               while (frac >= (int)heightmask)
                  frac -= heightmask;


            nextfrac = frac + (1<<16);
            while (nextfrac >= (int)heightmask)
               nextfrac -= heightmask;




            while (count--)
            {





               *dest = (( V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(nextsource[(nextfrac)>>16])])]))*64 + ((filter_fracu*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(nextfrac)>>16])])]))*64 + (((0xffff-filter_fracu)*((frac)&0xffff))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(source[(frac)>>16])])]))*64 + (((0xffff-filter_fracu)*(0xffff-((frac)&0xffff)))>>(32-6)) ] + V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(nextsource[(frac)>>16])])]))*64 + ((filter_fracu*(0xffff-((frac)&0xffff)))>>(32-6)) ]));
               (y++);
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;

               if ((nextfrac += fracstep) >= (int)heightmask) nextfrac -= heightmask;;

            }
         }
      }
   }

}

static void R_DrawTranslatedColumn16_RoundedUV(draw_column_vars_t *dcvars)
{
  int count;
  uint16_t *dest;
  fixed_t frac;
  const fixed_t fracstep = dcvars->iscale;
  const fixed_t slope_texu = dcvars->texu;

  if (dcvars->iscale > drawvars.mag_threshold)
  {
    R_GetDrawColumnFunc(RDC_PIPELINE_TRANSLATED,
                        RDRAW_FILTER_POINT,
                        drawvars.filterz)(dcvars);
    return;
  }
  count = dcvars->yh - dcvars->yl;

  if (count < 0)
    return;

  frac = dcvars->texturemid + (dcvars->yl-centery)*fracstep;

  if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED)
  {
     if (dcvars->yl != 0) {
        if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

           int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
           dcvars->yl += shift;
           count -= shift;
           frac += 0xffff-(slope_texu & 0xffff);
        }
        else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

           int shift = ((slope_texu & 0xffff)/dcvars->iscale);
           dcvars->yl += shift;
           count -= shift;
           frac += slope_texu & 0xffff;
        }
     }
     if (dcvars->yh != viewheight-1) {
        if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

           int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
           dcvars->yh -= shift;
           count -= shift;
        }
        else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

           int shift = ((slope_texu & 0xffff)/dcvars->iscale);
           dcvars->yh -= shift;
           count -= shift;
        }
     }
     if (count <= 0) return;
  }

   {

      if(temp_x == 4 ||
         (temp_x && (temptype != (COL_OPAQUE) || temp_x + startx != dcvars->x)))
         R_FlushColumns();

      if(!temp_x)
      {
         startx = dcvars->x;
         tempyl[0] = commontop = dcvars->yl;
         tempyh[0] = commonbot = dcvars->yh;
         temptype = (COL_OPAQUE);





         R_FlushWholeColumns = R_FlushWhole16;
         R_FlushHTColumns = R_FlushHT16;
         R_FlushQuadColumn = R_FlushQuad16;

         dest = &short_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tempyl[temp_x] = dcvars->yl;
         tempyh[temp_x] = dcvars->yh;

         if(dcvars->yl > commontop)
            commontop = dcvars->yl;
         if(dcvars->yh < commonbot)
            commonbot = dcvars->yh;


         dest = &short_tempbuf[(dcvars->yl << 2) + temp_x];

      }
      temp_x += 1;
   }



   {
      const uint8_t *source = dcvars->source;
      const lighttable_t *colormap = dcvars->colormap;
      const uint8_t *translation = dcvars->translation;

      int y = dcvars->yl;
      const int x = dcvars->x;
      const uint8_t *prevsource = dcvars->prevsource;
      const uint8_t *nextsource = dcvars->nextsource;
      const unsigned int filter_fracu = (dcvars->source == dcvars->nextsource) ? 0 : (dcvars->texu>>8) & 0xff;


      count++;







      if (dcvars->texheight == 128)
      {

         while(count--)
         {
            *dest = (V_Palette16[ ((translation[(filter_getScale2xQuadColors( source[ ((frac & ((127<<16)|0xffff))>>16) ], source[ (((0)>(((frac & ((127<<16)|0xffff))>>16)-1)?(0):(((frac & ((127<<16)|0xffff))>>16)-1))) ], nextsource[ ((frac & ((127<<16)|0xffff))>>16) ], source[ (((frac+(1<<16)) & ((127<<16)|0xffff))>>16) ], prevsource[ ((frac & ((127<<16)|0xffff))>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & ((127<<16)|0xffff))>>8) & 0xff)>>(8-6)) ] ])]))*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (V_Palette16[ ((translation[(filter_getScale2xQuadColors( source[ ((frac)>>16) ], source[ (((0)>(((frac)>>16)-1)?(0):(((frac)>>16)-1))) ], nextsource[ ((frac)>>16) ], source[ (((frac+(1<<16)))>>16) ], prevsource[ ((frac)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac)>>8) & 0xff)>>(8-6)) ] ])]))*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else
      {
         unsigned heightmask = dcvars->texheight-1;
         if (! (dcvars->texheight & heightmask))
         {
            fixed_t fixedt_heightmask = (heightmask<<16)|0xffff;
            while ((count-=2)>=0)
            {
               *dest = (V_Palette16[ ((translation[(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (V_Palette16[ ((translation[(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (V_Palette16[ ((translation[(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])]))*64 + ((64 -1)) ]);
            (y++);
         }
         else
         {
            fixed_t nextfrac = 0;

            heightmask++;
            heightmask <<= 16;

            if (frac < 0)
               while ((frac += heightmask) < 0);
            else
               while (frac >= (int)heightmask)
                  frac -= heightmask;


            nextfrac = frac + (1<<16);
            while (nextfrac >= (int)heightmask)
               nextfrac -= heightmask;




            while (count--)
            {





               *dest = (V_Palette16[ ((translation[(filter_getScale2xQuadColors( source[ ((frac)>>16) ], source[ (((0)>(((frac)>>16)-1)?(0):(((frac)>>16)-1))) ], nextsource[ ((frac)>>16) ], source[ ((nextfrac)>>16) ], prevsource[ ((frac)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac)>>8) & 0xff)>>(8-6)) ] ])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;

               if ((nextfrac += fracstep) >= (int)heightmask) nextfrac -= heightmask;;

            }
         }
      }
   }

}

static void R_DrawTranslatedColumn16_RoundedUV_PointZ(draw_column_vars_t *dcvars)
{
  int count;

  uint16_t *dest;

  fixed_t frac;
  const fixed_t fracstep = dcvars->iscale;
  const fixed_t slope_texu = dcvars->texu;

  if (dcvars->iscale > drawvars.mag_threshold) {
    R_GetDrawColumnFunc(RDC_PIPELINE_TRANSLATED,
                        RDRAW_FILTER_POINT,
                        drawvars.filterz)(dcvars);
    return;
  }
  count = dcvars->yh - dcvars->yl;







  if (count < 0)
    return;

  frac = dcvars->texturemid + (dcvars->yl-centery)*fracstep;

  if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED)
  {
     if (dcvars->yl != 0) {
        if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

           int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
           dcvars->yl += shift;
           count -= shift;
           frac += 0xffff-(slope_texu & 0xffff);
        }
        else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

           int shift = ((slope_texu & 0xffff)/dcvars->iscale);
           dcvars->yl += shift;
           count -= shift;
           frac += slope_texu & 0xffff;
        }
     }
     if (dcvars->yh != viewheight-1) {
        if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

           int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
           dcvars->yh -= shift;
           count -= shift;
        }
        else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

           int shift = ((slope_texu & 0xffff)/dcvars->iscale);
           dcvars->yh -= shift;
           count -= shift;
        }
     }
     if (count <= 0) return;
  }



   {

      if(temp_x == 4 ||
         (temp_x && (temptype != (COL_OPAQUE) || temp_x + startx != dcvars->x)))
         R_FlushColumns();

      if(!temp_x)
      {
         startx = dcvars->x;
         tempyl[0] = commontop = dcvars->yl;
         tempyh[0] = commonbot = dcvars->yh;
         temptype = (COL_OPAQUE);





         R_FlushWholeColumns = R_FlushWhole16;
         R_FlushHTColumns = R_FlushHT16;
         R_FlushQuadColumn = R_FlushQuad16;

         dest = &short_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tempyl[temp_x] = dcvars->yl;
         tempyh[temp_x] = dcvars->yh;

         if(dcvars->yl > commontop)
            commontop = dcvars->yl;
         if(dcvars->yh < commonbot)
            commonbot = dcvars->yh;


         dest = &short_tempbuf[(dcvars->yl << 2) + temp_x];

      }
      temp_x += 1;
   }



   {
      const uint8_t *source = dcvars->source;
      const lighttable_t *colormap = dcvars->colormap;
      const uint8_t *translation = dcvars->translation;

      int y = dcvars->yl;
      const int x = dcvars->x;
      const uint8_t *prevsource = dcvars->prevsource;
      const uint8_t *nextsource = dcvars->nextsource;
      const unsigned int filter_fracu = (dcvars->source == dcvars->nextsource) ? 0 : (dcvars->texu>>8) & 0xff;


      count++;







      if (dcvars->texheight == 128)
      {

         while(count--)
         {
            *dest = (V_Palette16[ (colormap[(translation[(filter_getScale2xQuadColors( source[ ((frac & ((127<<16)|0xffff))>>16) ], source[ (((0)>(((frac & ((127<<16)|0xffff))>>16)-1)?(0):(((frac & ((127<<16)|0xffff))>>16)-1))) ], nextsource[ ((frac & ((127<<16)|0xffff))>>16) ], source[ (((frac+(1<<16)) & ((127<<16)|0xffff))>>16) ], prevsource[ ((frac & ((127<<16)|0xffff))>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & ((127<<16)|0xffff))>>8) & 0xff)>>(8-6)) ] ])])])*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (V_Palette16[ (colormap[(translation[(filter_getScale2xQuadColors( source[ ((frac)>>16) ], source[ (((0)>(((frac)>>16)-1)?(0):(((frac)>>16)-1))) ], nextsource[ ((frac)>>16) ], source[ (((frac+(1<<16)))>>16) ], prevsource[ ((frac)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac)>>8) & 0xff)>>(8-6)) ] ])])])*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else
      {
         unsigned heightmask = dcvars->texheight-1;
         if (! (dcvars->texheight & heightmask))
         {
            fixed_t fixedt_heightmask = (heightmask<<16)|0xffff;
            while ((count-=2)>=0)
            {
               *dest = (V_Palette16[ (colormap[(translation[(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])])])*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (V_Palette16[ (colormap[(translation[(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])])])*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (V_Palette16[ (colormap[(translation[(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])])])*64 + ((64 -1)) ]);
            (y++);
         }
         else
         {
            fixed_t nextfrac = 0;

            heightmask++;
            heightmask <<= 16;

            if (frac < 0)
               while ((frac += heightmask) < 0);
            else
               while (frac >= (int)heightmask)
                  frac -= heightmask;


            nextfrac = frac + (1<<16);
            while (nextfrac >= (int)heightmask)
               nextfrac -= heightmask;




            while (count--)
            {





               *dest = (V_Palette16[ (colormap[(translation[(filter_getScale2xQuadColors( source[ ((frac)>>16) ], source[ (((0)>(((frac)>>16)-1)?(0):(((frac)>>16)-1))) ], nextsource[ ((frac)>>16) ], source[ ((nextfrac)>>16) ], prevsource[ ((frac)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac)>>8) & 0xff)>>(8-6)) ] ])])])*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;

               if ((nextfrac += fracstep) >= (int)heightmask) nextfrac -= heightmask;;

            }
         }
      }
   }

}

static void R_DrawTranslatedColumn16_RoundedUV_LinearZ(draw_column_vars_t *dcvars)
{
   int count;
   uint16_t *dest;
   fixed_t frac;
   const fixed_t fracstep = dcvars->iscale;
   const fixed_t slope_texu = dcvars->texu;

   if (dcvars->iscale > drawvars.mag_threshold)
   {
      R_GetDrawColumnFunc(RDC_PIPELINE_TRANSLATED,
            RDRAW_FILTER_POINT,
            drawvars.filterz)(dcvars);
      return;
   }

   count = dcvars->yh - dcvars->yl;

   if (count < 0)
      return;

   frac = dcvars->texturemid + (dcvars->yl-centery)*fracstep;


   if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED) {



      if (dcvars->yl != 0) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += 0xffff-(slope_texu & 0xffff);
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += slope_texu & 0xffff;
         }
      }
      if (dcvars->yh != viewheight-1) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
      }
      if (count <= 0) return;
   }



   {

      if(temp_x == 4 ||
            (temp_x && (temptype != (COL_OPAQUE) || temp_x + startx != dcvars->x)))
         R_FlushColumns();

      if(!temp_x)
      {
         startx = dcvars->x;
         tempyl[0] = commontop = dcvars->yl;
         tempyh[0] = commonbot = dcvars->yh;
         temptype = (COL_OPAQUE);





         R_FlushWholeColumns = R_FlushWhole16;
         R_FlushHTColumns = R_FlushHT16;
         R_FlushQuadColumn = R_FlushQuad16;

         dest = &short_tempbuf[dcvars->yl << 2];

      }
      else
      {
         tempyl[temp_x] = dcvars->yl;
         tempyh[temp_x] = dcvars->yh;

         if(dcvars->yl > commontop)
            commontop = dcvars->yl;
         if(dcvars->yh < commonbot)
            commonbot = dcvars->yh;


         dest = &short_tempbuf[(dcvars->yl << 2) + temp_x];

      }
      temp_x += 1;
   }



   {
      const uint8_t *source = dcvars->source;
      const lighttable_t *colormap = dcvars->colormap;
      const uint8_t *translation = dcvars->translation;

      int y = dcvars->yl;
      const int x = dcvars->x;


      const int fracz = (dcvars->z >> 6) & 255;
      const uint8_t *dither_colormaps[2] = { dcvars->colormap, dcvars->nextcolormap };






      const uint8_t *prevsource = dcvars->prevsource;
      const uint8_t *nextsource = dcvars->nextsource;
      const unsigned int filter_fracu = (dcvars->source == dcvars->nextsource) ? 0 : (dcvars->texu>>8) & 0xff;


      count++;







      if (dcvars->texheight == 128)
      {

         while(count--)
         {
            *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(filter_getScale2xQuadColors( source[ ((frac & ((127<<16)|0xffff))>>16) ], source[ (((0)>(((frac & ((127<<16)|0xffff))>>16)-1)?(0):(((frac & ((127<<16)|0xffff))>>16)-1))) ], nextsource[ ((frac & ((127<<16)|0xffff))>>16) ], source[ (((frac+(1<<16)) & ((127<<16)|0xffff))>>16) ], prevsource[ ((frac & ((127<<16)|0xffff))>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & ((127<<16)|0xffff))>>8) & 0xff)>>(8-6)) ] ])])]))*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else if (dcvars->texheight == 0)
      {

         while (count--)
         {
            *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(filter_getScale2xQuadColors( source[ ((frac)>>16) ], source[ (((0)>(((frac)>>16)-1)?(0):(((frac)>>16)-1))) ], nextsource[ ((frac)>>16) ], source[ (((frac+(1<<16)))>>16) ], prevsource[ ((frac)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac)>>8) & 0xff)>>(8-6)) ] ])])]))*64 + ((64 -1)) ]);
            (y++);
            dest += 4;
            frac += fracstep;
         }
      }
      else
      {
         unsigned heightmask = dcvars->texheight-1;
         if (! (dcvars->texheight & heightmask))
         {
            fixed_t fixedt_heightmask = (heightmask<<16)|0xffff;
            while ((count-=2)>=0)
            {
               *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
               *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               frac += fracstep;
            }
            if (count & 1)
               *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(filter_getScale2xQuadColors( source[ ((frac & fixedt_heightmask)>>16) ], source[ (((0)>(((frac & fixedt_heightmask)>>16)-1)?(0):(((frac & fixedt_heightmask)>>16)-1))) ], nextsource[ ((frac & fixedt_heightmask)>>16) ], source[ (((frac+(1<<16)) & fixedt_heightmask)>>16) ], prevsource[ ((frac & fixedt_heightmask)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac & fixedt_heightmask)>>8) & 0xff)>>(8-6)) ] ])])]))*64 + ((64 -1)) ]);
            (y++);
         }
         else
         {
            fixed_t nextfrac = 0;

            heightmask++;
            heightmask <<= 16;

            if (frac < 0)
               while ((frac += heightmask) < 0);
            else
               while (frac >= (int)heightmask)
                  frac -= heightmask;


            nextfrac = frac + (1<<16);
            while (nextfrac >= (int)heightmask)
               nextfrac -= heightmask;




            while (count--)
            {





               *dest = (V_Palette16[ ((dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x)&(4 -1)] < (fracz)) ? 1 : 0)][(translation[(filter_getScale2xQuadColors( source[ ((frac)>>16) ], source[ (((0)>(((frac)>>16)-1)?(0):(((frac)>>16)-1))) ], nextsource[ ((frac)>>16) ], source[ ((nextfrac)>>16) ], prevsource[ ((frac)>>16) ] ) [ filter_roundedUVMap[ ((filter_fracu>>(8-6))<<6) + ((((frac)>>8) & 0xff)>>(8-6)) ] ])])]))*64 + ((64 -1)) ]);
               (y++);
               dest += 4;
               if ((frac += fracstep) >= (int)heightmask) frac -= heightmask;;

               if ((nextfrac += fracstep) >= (int)heightmask) nextfrac -= heightmask;;

            }
         }
      }
   }
}

//
// Framebuffer postprocessing.
// Creates a fuzzy image by copying pixels
//  from adjacent ones to left and right.
// Used with an all black colormap, this
//  could create the SHADOW effect,
//  i.e. spectres and invisible players.
//

static void R_DrawFuzzColumn16_PointUV(draw_column_vars_t *dcvars)
{
  int count;
  fixed_t frac;
  const fixed_t fracstep = dcvars->iscale;
  const fixed_t slope_texu = dcvars->texu;

  if (!dcvars->yl)
    dcvars->yl = 1;

  if (dcvars->yh == viewheight-1)
    dcvars->yh = viewheight - 2;

  count = dcvars->yh - dcvars->yl;

  if (count < 0)
    return;

  frac = dcvars->texturemid + (dcvars->yl-centery)*fracstep;

  if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED)
  {
     if (dcvars->yl != 0) {
        if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

           int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
           dcvars->yl += shift;
           count -= shift;
           frac += 0xffff-(slope_texu & 0xffff);
        }
        else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

           int shift = ((slope_texu & 0xffff)/dcvars->iscale);
           dcvars->yl += shift;
           count -= shift;
           frac += slope_texu & 0xffff;
        }
     }
     if (dcvars->yh != viewheight-1) {
        if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

           int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
           dcvars->yh -= shift;
           count -= shift;
        }
        else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

           int shift = ((slope_texu & 0xffff)/dcvars->iscale);
           dcvars->yh -= shift;
           count -= shift;
        }
     }
     if (count <= 0) return;
  }

  {

     if(temp_x == 4 ||
           (temp_x && (temptype != (COL_FUZZ) || temp_x + startx != dcvars->x)))
        R_FlushColumns();

     if(!temp_x)
     {
        startx = dcvars->x;
        tempyl[0] = commontop = dcvars->yl;
        tempyh[0] = commonbot = dcvars->yh;
        temptype = (COL_FUZZ);



        tempfuzzmap = fullcolormap;

        R_FlushWholeColumns = R_FlushWholeFuzz16;
        R_FlushHTColumns = R_FlushHTFuzz16;
        R_FlushQuadColumn = R_FlushQuadFuzz16;



     }
     else
     {
        tempyl[temp_x] = dcvars->yl;
        tempyh[temp_x] = dcvars->yh;

        if(dcvars->yl > commontop)
           commontop = dcvars->yl;
        if(dcvars->yh < commonbot)
           commonbot = dcvars->yh;




     }
     temp_x += 1;
  }
}

static void R_DrawFuzzColumn16_PointUV_PointZ(draw_column_vars_t *dcvars)
{
  int count;
  fixed_t frac;
  const fixed_t fracstep = dcvars->iscale;
  const fixed_t slope_texu = dcvars->texu;

  if (!dcvars->yl)
    dcvars->yl = 1;

  if (dcvars->yh == viewheight-1)
    dcvars->yh = viewheight - 2;

  count = dcvars->yh - dcvars->yl;

  if (count < 0)
    return;

  frac = dcvars->texturemid + (dcvars->yl-centery)*fracstep;

  if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED)
  {

     if (dcvars->yl != 0) {
        if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

           int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
           dcvars->yl += shift;
           count -= shift;
           frac += 0xffff-(slope_texu & 0xffff);
        }
        else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

           int shift = ((slope_texu & 0xffff)/dcvars->iscale);
           dcvars->yl += shift;
           count -= shift;
           frac += slope_texu & 0xffff;
        }
     }
     if (dcvars->yh != viewheight-1) {
        if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

           int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
           dcvars->yh -= shift;
           count -= shift;
        }
        else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

           int shift = ((slope_texu & 0xffff)/dcvars->iscale);
           dcvars->yh -= shift;
           count -= shift;
        }
     }
     if (count <= 0) return;
  }

  if(temp_x == 4 ||
        (temp_x && (temptype != (COL_FUZZ) || temp_x + startx != dcvars->x)))
     R_FlushColumns();

  if(!temp_x)
  {
     startx = dcvars->x;
     tempyl[0] = commontop = dcvars->yl;
     tempyh[0] = commonbot = dcvars->yh;
     temptype = (COL_FUZZ);



     tempfuzzmap = fullcolormap;

     R_FlushWholeColumns = R_FlushWholeFuzz16;
     R_FlushHTColumns = R_FlushHTFuzz16;
     R_FlushQuadColumn = R_FlushQuadFuzz16;



  }
  else
  {
     tempyl[temp_x] = dcvars->yl;
     tempyh[temp_x] = dcvars->yh;

     if(dcvars->yl > commontop)
        commontop = dcvars->yl;
     if(dcvars->yh < commonbot)
        commonbot = dcvars->yh;




  }
  temp_x += 1;
}

static void R_DrawFuzzColumn16_PointUV_LinearZ(draw_column_vars_t *dcvars)
{
  int count;
  fixed_t frac;
  const fixed_t fracstep = dcvars->iscale;
  const fixed_t slope_texu = dcvars->texu;

  if (!dcvars->yl)
    dcvars->yl = 1;

  if (dcvars->yh == viewheight-1)
    dcvars->yh = viewheight - 2;

  count = dcvars->yh - dcvars->yl;

  if (count < 0)
    return;

  frac = dcvars->texturemid + (dcvars->yl-centery)*fracstep;

  if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED)
  {
     if (dcvars->yl != 0) {
        if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

           int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
           dcvars->yl += shift;
           count -= shift;
           frac += 0xffff-(slope_texu & 0xffff);
        }
        else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

           int shift = ((slope_texu & 0xffff)/dcvars->iscale);
           dcvars->yl += shift;
           count -= shift;
           frac += slope_texu & 0xffff;
        }
     }
     if (dcvars->yh != viewheight-1) {
        if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

           int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
           dcvars->yh -= shift;
           count -= shift;
        }
        else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

           int shift = ((slope_texu & 0xffff)/dcvars->iscale);
           dcvars->yh -= shift;
           count -= shift;
        }
     }
     if (count <= 0) return;
  }

  if(temp_x == 4 ||
        (temp_x && (temptype != (COL_FUZZ) || temp_x + startx != dcvars->x)))
     R_FlushColumns();

  if(!temp_x)
  {
     startx = dcvars->x;
     tempyl[0] = commontop = dcvars->yl;
     tempyh[0] = commonbot = dcvars->yh;
     temptype = (COL_FUZZ);



     tempfuzzmap = fullcolormap;

     R_FlushWholeColumns = R_FlushWholeFuzz16;
     R_FlushHTColumns = R_FlushHTFuzz16;
     R_FlushQuadColumn = R_FlushQuadFuzz16;



  }
  else
  {
     tempyl[temp_x] = dcvars->yl;
     tempyh[temp_x] = dcvars->yh;

     if(dcvars->yl > commontop)
        commontop = dcvars->yl;
     if(dcvars->yh < commonbot)
        commonbot = dcvars->yh;




  }
  temp_x += 1;
}

static void R_DrawFuzzColumn16_LinearUV(draw_column_vars_t *dcvars)
{
  int count;
  fixed_t frac;
  const fixed_t fracstep = dcvars->iscale;
  const fixed_t slope_texu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;

  if (dcvars->iscale > drawvars.mag_threshold)
  {
    R_GetDrawColumnFunc(RDC_PIPELINE_FUZZ,
                        RDRAW_FILTER_POINT,
                        drawvars.filterz)(dcvars);
    return;
  }

  if (!dcvars->yl)
    dcvars->yl = 1;


  if (dcvars->yh == viewheight-1)
    dcvars->yh = viewheight - 2;

  count = dcvars->yh - dcvars->yl;

  if (count < 0)
    return;

  frac = dcvars->texturemid - ((1<<16)>>1) + (dcvars->yl-centery)*fracstep;

  if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED)
  {
     if (dcvars->yl != 0) {
        if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

           int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
           dcvars->yl += shift;
           count -= shift;
           frac += 0xffff-(slope_texu & 0xffff);
        }
        else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

           int shift = ((slope_texu & 0xffff)/dcvars->iscale);
           dcvars->yl += shift;
           count -= shift;
           frac += slope_texu & 0xffff;
        }
     }
     if (dcvars->yh != viewheight-1) {
        if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

           int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
           dcvars->yh -= shift;
           count -= shift;
        }
        else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

           int shift = ((slope_texu & 0xffff)/dcvars->iscale);
           dcvars->yh -= shift;
           count -= shift;
        }
     }
     if (count <= 0) return;
  }

  if(temp_x == 4 ||
        (temp_x && (temptype != (COL_FUZZ) || temp_x + startx != dcvars->x)))
     R_FlushColumns();

  if(!temp_x)
  {
     startx = dcvars->x;
     tempyl[0] = commontop = dcvars->yl;
     tempyh[0] = commonbot = dcvars->yh;
     temptype = (COL_FUZZ);



     tempfuzzmap = fullcolormap;

     R_FlushWholeColumns = R_FlushWholeFuzz16;
     R_FlushHTColumns = R_FlushHTFuzz16;
     R_FlushQuadColumn = R_FlushQuadFuzz16;



  }
  else
  {
     tempyl[temp_x] = dcvars->yl;
     tempyh[temp_x] = dcvars->yh;

     if(dcvars->yl > commontop)
        commontop = dcvars->yl;
     if(dcvars->yh < commonbot)
        commonbot = dcvars->yh;




  }
  temp_x += 1;
}

static void R_DrawFuzzColumn16_LinearUV_PointZ(draw_column_vars_t *dcvars)
{
   int count;
   fixed_t frac;
   const fixed_t fracstep = dcvars->iscale;
   const fixed_t slope_texu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;

   if (dcvars->iscale > drawvars.mag_threshold)
   {
      R_GetDrawColumnFunc(RDC_PIPELINE_FUZZ,
            RDRAW_FILTER_POINT,
            drawvars.filterz)(dcvars);
      return;
   }




   if (!dcvars->yl)
      dcvars->yl = 1;


   if (dcvars->yh == viewheight-1)
      dcvars->yh = viewheight - 2;

   count = dcvars->yh - dcvars->yl;
   if (count < 0)
      return;

   frac = dcvars->texturemid - ((1<<16)>>1) + (dcvars->yl-centery)*fracstep;

   if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED)
   {
      if (dcvars->yl != 0) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += 0xffff-(slope_texu & 0xffff);
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += slope_texu & 0xffff;
         }
      }
      if (dcvars->yh != viewheight-1) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
      }
      if (count <= 0) return;
   }

   if(temp_x == 4 ||
         (temp_x && (temptype != (COL_FUZZ) || temp_x + startx != dcvars->x)))
      R_FlushColumns();

   if(!temp_x)
   {
      startx = dcvars->x;
      tempyl[0] = commontop = dcvars->yl;
      tempyh[0] = commonbot = dcvars->yh;
      temptype = (COL_FUZZ);



      tempfuzzmap = fullcolormap;

      R_FlushWholeColumns = R_FlushWholeFuzz16;
      R_FlushHTColumns = R_FlushHTFuzz16;
      R_FlushQuadColumn = R_FlushQuadFuzz16;



   }
   else
   {
      tempyl[temp_x] = dcvars->yl;
      tempyh[temp_x] = dcvars->yh;

      if(dcvars->yl > commontop)
         commontop = dcvars->yl;
      if(dcvars->yh < commonbot)
         commonbot = dcvars->yh;




   }
   temp_x += 1;
}

static void R_DrawFuzzColumn16_LinearUV_LinearZ(draw_column_vars_t *dcvars)
{
  int count;



  fixed_t frac;
  const fixed_t fracstep = dcvars->iscale;

  const fixed_t slope_texu = (dcvars->source == dcvars->nextsource) ? 0 : dcvars->texu & 0xffff;






  if (dcvars->iscale > drawvars.mag_threshold) {
    R_GetDrawColumnFunc(RDC_PIPELINE_FUZZ,
                        RDRAW_FILTER_POINT,
                        drawvars.filterz)(dcvars);
    return;
  }




  if (!dcvars->yl)
    dcvars->yl = 1;


  if (dcvars->yh == viewheight-1)
    dcvars->yh = viewheight - 2;







  count = dcvars->yh - dcvars->yl;







  if (count < 0)
    return;



    frac = dcvars->texturemid - ((1<<16)>>1) + (dcvars->yl-centery)*fracstep;




  if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED) {



    if (dcvars->yl != 0) {
      if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

        int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
        dcvars->yl += shift;
        count -= shift;
        frac += 0xffff-(slope_texu & 0xffff);
      }
      else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

        int shift = ((slope_texu & 0xffff)/dcvars->iscale);
        dcvars->yl += shift;
        count -= shift;
        frac += slope_texu & 0xffff;
      }
    }
    if (dcvars->yh != viewheight-1) {
      if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

        int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
        dcvars->yh -= shift;
        count -= shift;
      }
      else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

        int shift = ((slope_texu & 0xffff)/dcvars->iscale);
        dcvars->yh -= shift;
        count -= shift;
      }
    }
    if (count <= 0) return;
  }



   {

      if(temp_x == 4 ||
         (temp_x && (temptype != (COL_FUZZ) || temp_x + startx != dcvars->x)))
         R_FlushColumns();

      if(!temp_x)
      {
         startx = dcvars->x;
         tempyl[0] = commontop = dcvars->yl;
         tempyh[0] = commonbot = dcvars->yh;
         temptype = (COL_FUZZ);



         tempfuzzmap = fullcolormap;

         R_FlushWholeColumns = R_FlushWholeFuzz16;
         R_FlushHTColumns = R_FlushHTFuzz16;
         R_FlushQuadColumn = R_FlushQuadFuzz16;



      }
      else
      {
         tempyl[temp_x] = dcvars->yl;
         tempyh[temp_x] = dcvars->yh;

         if(dcvars->yl > commontop)
            commontop = dcvars->yl;
         if(dcvars->yh < commonbot)
            commonbot = dcvars->yh;




      }
      temp_x += 1;
   }
}

static void R_DrawFuzzColumn16_RoundedUV(draw_column_vars_t *dcvars)
{
  int count;



  fixed_t frac;
  const fixed_t fracstep = dcvars->iscale;



  const fixed_t slope_texu = dcvars->texu;




  if (dcvars->iscale > drawvars.mag_threshold) {
    R_GetDrawColumnFunc(RDC_PIPELINE_FUZZ,
                        RDRAW_FILTER_POINT,
                        drawvars.filterz)(dcvars);
    return;
  }




  if (!dcvars->yl)
    dcvars->yl = 1;


  if (dcvars->yh == viewheight-1)
    dcvars->yh = viewheight - 2;







  count = dcvars->yh - dcvars->yl;







  if (count < 0)
    return;





    frac = dcvars->texturemid + (dcvars->yl-centery)*fracstep;


  if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED) {



    if (dcvars->yl != 0) {
      if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

        int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
        dcvars->yl += shift;
        count -= shift;
        frac += 0xffff-(slope_texu & 0xffff);
      }
      else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

        int shift = ((slope_texu & 0xffff)/dcvars->iscale);
        dcvars->yl += shift;
        count -= shift;
        frac += slope_texu & 0xffff;
      }
    }
    if (dcvars->yh != viewheight-1) {
      if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

        int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
        dcvars->yh -= shift;
        count -= shift;
      }
      else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

        int shift = ((slope_texu & 0xffff)/dcvars->iscale);
        dcvars->yh -= shift;
        count -= shift;
      }
    }
    if (count <= 0) return;
  }



   {

      if(temp_x == 4 ||
         (temp_x && (temptype != (COL_FUZZ) || temp_x + startx != dcvars->x)))
         R_FlushColumns();

      if(!temp_x)
      {
         startx = dcvars->x;
         tempyl[0] = commontop = dcvars->yl;
         tempyh[0] = commonbot = dcvars->yh;
         temptype = (COL_FUZZ);



         tempfuzzmap = fullcolormap;

         R_FlushWholeColumns = R_FlushWholeFuzz16;
         R_FlushHTColumns = R_FlushHTFuzz16;
         R_FlushQuadColumn = R_FlushQuadFuzz16;



      }
      else
      {
         tempyl[temp_x] = dcvars->yl;
         tempyh[temp_x] = dcvars->yh;

         if(dcvars->yl > commontop)
            commontop = dcvars->yl;
         if(dcvars->yh < commonbot)
            commonbot = dcvars->yh;




      }
      temp_x += 1;
   }
}

static void R_DrawFuzzColumn16_RoundedUV_PointZ(draw_column_vars_t *dcvars)
{
  int count;



  fixed_t frac;
  const fixed_t fracstep = dcvars->iscale;



  const fixed_t slope_texu = dcvars->texu;




  if (dcvars->iscale > drawvars.mag_threshold) {
    R_GetDrawColumnFunc(RDC_PIPELINE_FUZZ,
                        RDRAW_FILTER_POINT,
                        drawvars.filterz)(dcvars);
    return;
  }




  if (!dcvars->yl)
    dcvars->yl = 1;


  if (dcvars->yh == viewheight-1)
    dcvars->yh = viewheight - 2;







  count = dcvars->yh - dcvars->yl;







  if (count < 0)
    return;





    frac = dcvars->texturemid + (dcvars->yl-centery)*fracstep;


  if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED) {



    if (dcvars->yl != 0) {
      if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

        int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
        dcvars->yl += shift;
        count -= shift;
        frac += 0xffff-(slope_texu & 0xffff);
      }
      else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

        int shift = ((slope_texu & 0xffff)/dcvars->iscale);
        dcvars->yl += shift;
        count -= shift;
        frac += slope_texu & 0xffff;
      }
    }
    if (dcvars->yh != viewheight-1) {
      if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

        int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
        dcvars->yh -= shift;
        count -= shift;
      }
      else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

        int shift = ((slope_texu & 0xffff)/dcvars->iscale);
        dcvars->yh -= shift;
        count -= shift;
      }
    }
    if (count <= 0) return;
  }



   {

      if(temp_x == 4 ||
         (temp_x && (temptype != (COL_FUZZ) || temp_x + startx != dcvars->x)))
         R_FlushColumns();

      if(!temp_x)
      {
         startx = dcvars->x;
         tempyl[0] = commontop = dcvars->yl;
         tempyh[0] = commonbot = dcvars->yh;
         temptype = (COL_FUZZ);



         tempfuzzmap = fullcolormap;

         R_FlushWholeColumns = R_FlushWholeFuzz16;
         R_FlushHTColumns = R_FlushHTFuzz16;
         R_FlushQuadColumn = R_FlushQuadFuzz16;



      }
      else
      {
         tempyl[temp_x] = dcvars->yl;
         tempyh[temp_x] = dcvars->yh;

         if(dcvars->yl > commontop)
            commontop = dcvars->yl;
         if(dcvars->yh < commonbot)
            commonbot = dcvars->yh;




      }
      temp_x += 1;
   }
}

static void R_DrawFuzzColumn16_RoundedUV_LinearZ(draw_column_vars_t *dcvars)
{
   int count;



   fixed_t frac;
   const fixed_t fracstep = dcvars->iscale;



   const fixed_t slope_texu = dcvars->texu;




   if (dcvars->iscale > drawvars.mag_threshold) {
      R_GetDrawColumnFunc(RDC_PIPELINE_FUZZ,
            RDRAW_FILTER_POINT,
            drawvars.filterz)(dcvars);
      return;
   }




   if (!dcvars->yl)
      dcvars->yl = 1;


   if (dcvars->yh == viewheight-1)
      dcvars->yh = viewheight - 2;







   count = dcvars->yh - dcvars->yl;







   if (count < 0)
      return;





   frac = dcvars->texturemid + (dcvars->yl-centery)*fracstep;


   if (dcvars->drawingmasked && dcvars->edgetype == RDRAW_MASKEDCOLUMNEDGE_SLOPED) {



      if (dcvars->yl != 0) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += 0xffff-(slope_texu & 0xffff);
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_TOP_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yl += shift;
            count -= shift;
            frac += slope_texu & 0xffff;
         }
      }
      if (dcvars->yh != viewheight-1) {
         if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_UP) {

            int shift = ((0xffff-(slope_texu & 0xffff))/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
         else if (dcvars->edgeslope & RDRAW_EDGESLOPE_BOT_DOWN) {

            int shift = ((slope_texu & 0xffff)/dcvars->iscale);
            dcvars->yh -= shift;
            count -= shift;
         }
      }
      if (count <= 0) return;
   }



   {

      if(temp_x == 4 ||
            (temp_x && (temptype != (COL_FUZZ) || temp_x + startx != dcvars->x)))
         R_FlushColumns();

      if(!temp_x)
      {
         startx = dcvars->x;
         tempyl[0] = commontop = dcvars->yl;
         tempyh[0] = commonbot = dcvars->yh;
         temptype = (COL_FUZZ);



         tempfuzzmap = fullcolormap;

         R_FlushWholeColumns = R_FlushWholeFuzz16;
         R_FlushHTColumns = R_FlushHTFuzz16;
         R_FlushQuadColumn = R_FlushQuadFuzz16;



      }
      else
      {
         tempyl[temp_x] = dcvars->yl;
         tempyh[temp_x] = dcvars->yh;

         if(dcvars->yl > commontop)
            commontop = dcvars->yl;
         if(dcvars->yh < commonbot)
            commonbot = dcvars->yh;




      }
      temp_x += 1;
   }
}

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

static void R_DrawSpan16_PointUV_PointZ(draw_span_vars_t *dsvars)
{
   unsigned count = dsvars->x2 - dsvars->x1 + 1;
   fixed_t xfrac = dsvars->xfrac;
   fixed_t yfrac = dsvars->yfrac;
   const fixed_t xstep = dsvars->xstep;
   const fixed_t ystep = dsvars->ystep;
   const uint8_t *source = dsvars->source;


   const uint8_t *colormap = dsvars->colormap;

   uint16_t *dest = drawvars.short_topleft + dsvars->y* SCREENWIDTH + dsvars->x1;
   while (count)
   {
      const fixed_t xtemp = (xfrac >> 16) & 63;
      const fixed_t ytemp = (yfrac >> 10) & 4032;

      const fixed_t spot = xtemp | ytemp;
      xfrac += xstep;
      yfrac += ystep;
      *dest++ = V_Palette16[ (colormap[(source[spot])])*64 + ((64 -1)) ];
      count--;
   }
}

static void R_DrawSpan16_PointUV_LinearZ(draw_span_vars_t *dsvars)
{
   unsigned count = dsvars->x2 - dsvars->x1 + 1;
   fixed_t xfrac = dsvars->xfrac;
   fixed_t yfrac = dsvars->yfrac;
   const fixed_t xstep = dsvars->xstep;
   const fixed_t ystep = dsvars->ystep;
   const uint8_t *source = dsvars->source;




   uint16_t *dest = drawvars.short_topleft + dsvars->y* SCREENWIDTH + dsvars->x1;

   const int y = dsvars->y;
   int x1 = dsvars->x1;


   const int fracz = (dsvars->z >> 12) & 255;
   const uint8_t *dither_colormaps[2] = { dsvars->colormap, dsvars->nextcolormap };


   while (count) {
      const fixed_t xtemp = (xfrac >> 16) & 63;
      const fixed_t ytemp = (yfrac >> 10) & 4032;

      const fixed_t spot = xtemp | ytemp;
      xfrac += xstep;
      yfrac += ystep;
      *dest++ = V_Palette16[ (dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x1)&(4 -1)] < (fracz)) ? 1 : 0)][(source[spot])])*64 + ((64 -1)) ];
      count--;

      x1--;


   }
}

static void R_DrawSpan16_LinearUV_PointZ(draw_span_vars_t *dsvars)
{
   if ((D_abs(dsvars->xstep) > drawvars.mag_threshold)
         || (D_abs(dsvars->ystep) > drawvars.mag_threshold))
   {
      R_GetDrawSpanFunc(RDRAW_FILTER_POINT,
            drawvars.filterz)(dsvars);
      return;
   }

   {
      unsigned count = dsvars->x2 - dsvars->x1 + 1;
      fixed_t xfrac = dsvars->xfrac;
      fixed_t yfrac = dsvars->yfrac;
      const fixed_t xstep = dsvars->xstep;
      const fixed_t ystep = dsvars->ystep;
      const uint8_t *source = dsvars->source;


      const uint8_t *colormap = dsvars->colormap;

      uint16_t *dest = drawvars.short_topleft + dsvars->y* SCREENWIDTH + dsvars->x1;

      const int y = dsvars->y;
      int x1 = dsvars->x1;






      while (count) {


         *dest++ = ( V_Palette16[ (colormap[(source[ ((((xfrac)+(1<<16))>>16)&0x3f) | ((((yfrac)+(1<<16))>>10)&0xfc0)])])*64 + ((unsigned int)(((xfrac)&0xffff)*((yfrac)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(source[ (((xfrac)>>16)&0x3f) | ((((yfrac)+(1<<16))>>10)&0xfc0)])])*64 + ((unsigned int)((0xffff-((xfrac)&0xffff))*((yfrac)&0xffff))>>(32-6)) ] + V_Palette16[ (colormap[(source[ (((xfrac)>>16)&0x3f) | (((yfrac)>>10)&0xfc0)])])*64 + ((unsigned int)((0xffff-((xfrac)&0xffff))*(0xffff-((yfrac)&0xffff)))>>(32-6)) ] + V_Palette16[ (colormap[(source[ ((((xfrac)+(1<<16))>>16)&0x3f) | (((yfrac)>>10)&0xfc0)])])*64 + ((unsigned int)(((xfrac)&0xffff)*(0xffff-((yfrac)&0xffff)))>>(32-6)) ]);
         xfrac += xstep;
         yfrac += ystep;
         count--;
      }
   }
}

static void R_DrawSpan16_LinearUV_LinearZ(draw_span_vars_t *dsvars)
{
   if ((D_abs(dsvars->xstep) > drawvars.mag_threshold)
         || (D_abs(dsvars->ystep) > drawvars.mag_threshold))
   {
      R_GetDrawSpanFunc(RDRAW_FILTER_POINT,
            drawvars.filterz)(dsvars);
      return;
   }

   {
      unsigned count = dsvars->x2 - dsvars->x1 + 1;
      fixed_t xfrac = dsvars->xfrac;
      fixed_t yfrac = dsvars->yfrac;
      const fixed_t xstep = dsvars->xstep;
      const fixed_t ystep = dsvars->ystep;
      const uint8_t *source = dsvars->source;




      uint16_t *dest = drawvars.short_topleft + dsvars->y* SCREENWIDTH + dsvars->x1;

      const int y = dsvars->y;
      int x1 = dsvars->x1;


      const int fracz = (dsvars->z >> 12) & 255;
      const uint8_t *dither_colormaps[2] = { dsvars->colormap, dsvars->nextcolormap };


      while (count) {


         *dest++ = ( V_Palette16[ (dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x1)&(4 -1)] < (fracz)) ? 1 : 0)][(source[ ((((xfrac)+(1<<16))>>16)&0x3f) | ((((yfrac)+(1<<16))>>10)&0xfc0)])])*64 + ((unsigned int)(((xfrac)&0xffff)*((yfrac)&0xffff))>>(32-6)) ] + V_Palette16[ (dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x1)&(4 -1)] < (fracz)) ? 1 : 0)][(source[ (((xfrac)>>16)&0x3f) | ((((yfrac)+(1<<16))>>10)&0xfc0)])])*64 + ((unsigned int)((0xffff-((xfrac)&0xffff))*((yfrac)&0xffff))>>(32-6)) ] + V_Palette16[ (dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x1)&(4 -1)] < (fracz)) ? 1 : 0)][(source[ (((xfrac)>>16)&0x3f) | (((yfrac)>>10)&0xfc0)])])*64 + ((unsigned int)((0xffff-((xfrac)&0xffff))*(0xffff-((yfrac)&0xffff)))>>(32-6)) ] + V_Palette16[ (dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x1)&(4 -1)] < (fracz)) ? 1 : 0)][(source[ ((((xfrac)+(1<<16))>>16)&0x3f) | (((yfrac)>>10)&0xfc0)])])*64 + ((unsigned int)(((xfrac)&0xffff)*(0xffff-((yfrac)&0xffff)))>>(32-6)) ]);
         xfrac += xstep;
         yfrac += ystep;
         count--;

         x1--;
      }
   }
}

static void R_DrawSpan16_RoundedUV_PointZ(draw_span_vars_t *dsvars)
{
   if ((D_abs(dsvars->xstep) > drawvars.mag_threshold)
         || (D_abs(dsvars->ystep) > drawvars.mag_threshold))
   {
      R_GetDrawSpanFunc(RDRAW_FILTER_POINT,
            drawvars.filterz)(dsvars);
      return;
   }

   {
      unsigned count = dsvars->x2 - dsvars->x1 + 1;
      fixed_t xfrac = dsvars->xfrac;
      fixed_t yfrac = dsvars->yfrac;
      const fixed_t xstep = dsvars->xstep;
      const fixed_t ystep = dsvars->ystep;
      const uint8_t *source = dsvars->source;


      const uint8_t *colormap = dsvars->colormap;

      uint16_t *dest = drawvars.short_topleft + dsvars->y* SCREENWIDTH + dsvars->x1;
      while (count) {
         *dest++ = V_Palette16[ (colormap[(filter_getScale2xQuadColors( source[ (((xfrac)>>16)&0x3f) | (((yfrac)>>10)&0xfc0) ], source[ (((xfrac)>>16)&0x3f) | ((((yfrac)-(1<<16))>>10)&0xfc0) ], source[ ((((xfrac)+(1<<16))>>16)&0x3f) | (((yfrac)>>10)&0xfc0) ], source[ (((xfrac)>>16)&0x3f) | ((((yfrac)+(1<<16))>>10)&0xfc0) ], source[ ((((xfrac)-(1<<16))>>16)&0x3f) | (((yfrac)>>10)&0xfc0) ] ) [ filter_roundedUVMap[ (((((xfrac)>>8) & 0xff)>>(8-6))<<6) + ((((yfrac)>>8) & 0xff)>>(8-6)) ] ])])*64 + ((64 -1)) ];
         xfrac += xstep;
         yfrac += ystep;
         count--;
      }
   }
}

static void R_DrawSpan16_RoundedUV_LinearZ(draw_span_vars_t *dsvars)
{
   if ((D_abs(dsvars->xstep) > drawvars.mag_threshold)
         || (D_abs(dsvars->ystep) > drawvars.mag_threshold))
   {
      R_GetDrawSpanFunc(RDRAW_FILTER_POINT,
            drawvars.filterz)(dsvars);
      return;
   }

   {
      unsigned count = dsvars->x2 - dsvars->x1 + 1;
      fixed_t xfrac = dsvars->xfrac;
      fixed_t yfrac = dsvars->yfrac;
      const fixed_t xstep = dsvars->xstep;
      const fixed_t ystep = dsvars->ystep;
      const uint8_t *source = dsvars->source;




      uint16_t *dest = drawvars.short_topleft + dsvars->y* SCREENWIDTH + dsvars->x1;

      const int y = dsvars->y;
      int x1 = dsvars->x1;


      const int fracz = (dsvars->z >> 12) & 255;
      const uint8_t *dither_colormaps[2] = { dsvars->colormap, dsvars->nextcolormap };


      while (count) {
         *dest++ = V_Palette16[ (dither_colormaps[((filter_ditherMatrix[(y)&(4 -1)][(x1)&(4 -1)] < (fracz)) ? 1 : 0)][(filter_getScale2xQuadColors( source[ (((xfrac)>>16)&0x3f) | (((yfrac)>>10)&0xfc0) ], source[ (((xfrac)>>16)&0x3f) | ((((yfrac)-(1<<16))>>10)&0xfc0) ], source[ ((((xfrac)+(1<<16))>>16)&0x3f) | (((yfrac)>>10)&0xfc0) ], source[ (((xfrac)>>16)&0x3f) | ((((yfrac)+(1<<16))>>10)&0xfc0) ], source[ ((((xfrac)-(1<<16))>>16)&0x3f) | (((yfrac)>>10)&0xfc0) ] ) [ filter_roundedUVMap[ (((((xfrac)>>8) & 0xff)>>(8-6))<<6) + ((((yfrac)>>8) & 0xff)>>(8-6)) ] ])])*64 + ((64 -1)) ];
         xfrac += xstep;
         yfrac += ystep;
         count--;

         x1--;
      }
   }
}

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
