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
 *-----------------------------------------------------------------------------*/

#define PITCH short_pitch

#if (R_DRAWCOLUMN_PIPELINE & RDC_FUZZ)
#define GETDESTCOLOR16(col) GETBLENDED16_9406(col, 0)
#else
#define GETDESTCOLOR16(col) (col)
#endif

//
// R_FlushWholeOpaque
//
// Flushes the entire columns in the buffer, one at a time.
// This is used when a quad flush isn't possible.
// Opaque version -- no remapping whatsoever.
//
static void R_FLUSHWHOLE_FUNCNAME(void)
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
#if (R_DRAWCOLUMN_PIPELINE & RDC_FUZZ)
         // SoM 7-28-04: Fix the fuzz problem.
         *dest = GETDESTCOLOR16(dest[fuzzoffset[fuzzpos]]);
         
         // Clamp table lookup index.
         if(++fuzzpos == FUZZTABLE) 
            fuzzpos = 0;
#else
         *dest = *source;
#endif

         source += 4;
         dest += SURFACE_SHORT_PITCH;
      }
   }
}

//
// R_FlushHTOpaque
//
// Flushes the head and tail of columns in the buffer in
// preparation for a quad flush.
// Opaque version -- no remapping whatsoever.
//
static void R_FLUSHHEADTAIL_FUNCNAME(void)
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
#if (R_DRAWCOLUMN_PIPELINE & RDC_FUZZ)
            // SoM 7-28-04: Fix the fuzz problem.
            *dest = GETDESTCOLOR16(dest[fuzzoffset[fuzzpos]]);
            
            // Clamp table lookup index.
            if(++fuzzpos == FUZZTABLE) 
               fuzzpos = 0;
#else
            *dest = *source;
#endif

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
#if (R_DRAWCOLUMN_PIPELINE & RDC_FUZZ)
            // SoM 7-28-04: Fix the fuzz problem.
            *dest = GETDESTCOLOR16(dest[fuzzoffset[fuzzpos]]);
            
            // Clamp table lookup index.
            if(++fuzzpos == FUZZTABLE) 
               fuzzpos = 0;
#else
            *dest = *source;
#endif

            source += 4;
            dest += SURFACE_SHORT_PITCH;
         }
      }         
      ++colnum;
   }
}

static void R_FLUSHQUAD_FUNCNAME(void)
{
   uint16_t *source = &short_tempbuf[commontop << 2];
   uint16_t *dest   = drawvars.short_topleft + commontop * SURFACE_SHORT_PITCH + startx;
   int count;
#if (R_DRAWCOLUMN_PIPELINE & RDC_FUZZ)
   int fuzz1, fuzz2, fuzz3, fuzz4;

   fuzz1 = fuzzpos;
   fuzz2 = (fuzz1 + tempyl[1]) % FUZZTABLE;
   fuzz3 = (fuzz2 + tempyl[2]) % FUZZTABLE;
   fuzz4 = (fuzz3 + tempyl[3]) % FUZZTABLE;
#endif

   count = commonbot - commontop + 1;

#if (R_DRAWCOLUMN_PIPELINE & RDC_FUZZ)
   while(--count >= 0)
   {
      dest[0] = GETDESTCOLOR16(dest[0 + fuzzoffset[fuzz1]]);
      dest[1] = GETDESTCOLOR16(dest[1 + fuzzoffset[fuzz2]]);
      dest[2] = GETDESTCOLOR16(dest[2 + fuzzoffset[fuzz3]]);
      dest[3] = GETDESTCOLOR16(dest[3 + fuzzoffset[fuzz4]]);
      fuzz1 = (fuzz1 + 1) % FUZZTABLE;
      fuzz2 = (fuzz2 + 1) % FUZZTABLE;
      fuzz3 = (fuzz3 + 1) % FUZZTABLE;
      fuzz4 = (fuzz4 + 1) % FUZZTABLE;
      source += 4 * sizeof(uint8_t);
      dest += SURFACE_SHORT_PITCH * sizeof(uint8_t);
   }
#else
   while(--count >= 0)
   {
      dest[0] = source[0];
      dest[1] = source[1];
      dest[2] = source[2];
      dest[3] = source[3];
      source += 4;
      dest += SURFACE_SHORT_PITCH;
   }
#endif
}

#undef GETDESTCOLOR16

#undef PITCH

#undef R_DRAWCOLUMN_PIPELINE
#undef R_FLUSHWHOLE_FUNCNAME
#undef R_FLUSHHEADTAIL_FUNCNAME
#undef R_FLUSHQUAD_FUNCNAME
