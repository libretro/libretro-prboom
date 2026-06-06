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
 *      Mission begin melt/wipe screen special effect.
 *
 *-----------------------------------------------------------------------------
 */


#include "config.h"

#include "z_zone.h"
#include "doomdef.h"
#include "i_video.h"
#include "v_video.h"
#include "m_random.h"
#include "f_wipe.h"

//
// SCREEN WIPE PACKAGE
//

// Parts re-written to support true-color video modes. Column-major
// formatting removed. - POPE

// CPhipps - macros for the source and destination screens
#define SRC_SCR 2
#define DEST_SCR 3

static screeninfo_t wipe_scr_start;
static screeninfo_t wipe_scr_end;
static screeninfo_t wipe_scr;

static int y_lookup[MAX_SCREENWIDTH];

static int wipe_initMelt(int ticks)
{
  int i;

  // copy start screen to main screen
  for(i=0;i<SCREENHEIGHT;i++)
    memcpy(wipe_scr.data+i * SURFACE_BYTE_PITCH,
           wipe_scr_start.data+i * SURFACE_BYTE_PITCH,
           SCREENWIDTH * SURFACE_PIXEL_DEPTH);

  // setup initial column positions (y<0 => not ready to scroll yet)
  y_lookup[0] = -(M_Random()%16);
  for (i=1;i<SCREENWIDTH;i++)
    {
      int r = (M_Random()%3) - 1;
      y_lookup[i] = y_lookup[i-1] + r;
      if (y_lookup[i] > 0)
        y_lookup[i] = 0;
      else
        if (y_lookup[i] == -16)
          y_lookup[i] = -15;
    }
  return 0;
}

static int wipe_doMelt(int ticks)
{
   dbool   done = TRUE;
   int i;

   while (ticks--)
   {
      int y;

      /* Advance every column's melt position by one tick.  The melt
       * math is unchanged; it is only split out of the paint loop so
       * the paint can run row-major below. */
      for (i = 0; i < SCREENWIDTH; i++)
      {
         if (y_lookup[i] < 0)
         {
            y_lookup[i]++;
            done = FALSE;
         }
         else if (y_lookup[i] < SCREENHEIGHT)
         {
            /* cph 2001/07/29: melt rate depends on SCREENHEIGHT
             * so the wipe takes the same wall-clock time at any
             * vertical resolution. */
            int dy = (y_lookup[i] < 16) ? y_lookup[i]+1
                                        : SCREENHEIGHT/25;
            if (y_lookup[i] + dy > SCREENHEIGHT)
               dy = SCREENHEIGHT - y_lookup[i];
            y_lookup[i] += dy;
            done = FALSE;
         }
         /* else: column already at SCREENHEIGHT, no advance. */
      }

      /* Paint the frame row-major.  The old loop walked columns and
       * copied two bytes at a time with a full row pitch between
       * writes -- at 2560x1600 that is four million strided
       * write-allocates per wipe frame, which is what made the melt
       * hitch at high resolutions.  Per destination row, a pixel at
       * column i shows the end screen when the column's boundary
       * (max(0, y_lookup[i])) is still below this row, else the
       * start screen scrolled down by that boundary, exactly as
       * before; the writes are now sequential, and since the melt
       * boundaries vary slowly across columns the end-screen pixels
       * form long horizontal runs that copy with memcpy.
       *
       * Under the direct-render path screens[0].data (= wipe_scr.data)
       * rotates per frame, so every frame still paints the full
       * screen rather than relying on previous frames' writes. */
      for (y = 0; y < SCREENHEIGHT; y++)
      {
         uint16_t *drow =
            (uint16_t *)(wipe_scr.data + (size_t)y * SURFACE_BYTE_PITCH);
         const uint16_t *erow =
            (const uint16_t *)(wipe_scr_end.data + (size_t)y * SURFACE_BYTE_PITCH);

         i = 0;
         while (i < SCREENWIDTH)
         {
            int b = y_lookup[i] < 0 ? 0 : y_lookup[i];

            if (b > y)
            {
               /* End-screen run: extend while the boundary stays
                * below this row. */
               int run = i + 1;
               while (run < SCREENWIDTH)
               {
                  int rb = y_lookup[run] < 0 ? 0 : y_lookup[run];
                  if (rb <= y)
                     break;
                  run++;
               }
               memcpy(&drow[i], &erow[i],
                      (size_t)(run - i) * SURFACE_PIXEL_DEPTH);
               i = run;
            }
            else
            {
               /* Start-screen pixel, scrolled down by the boundary:
                * dest row y reads source row y - b. */
               const uint16_t *srow =
                  (const uint16_t *)(wipe_scr_start.data
                     + (size_t)(y - b) * SURFACE_BYTE_PITCH);
               drow[i] = srow[i];
               i++;
            }
         }
      }
   }
   return done;
}

// CPhipps - modified to allocate and deallocate screens[2 to 3] as needed, saving memory

static int wipe_exitMelt(int ticks)
{
  V_FreeScreen(&wipe_scr_start);
  wipe_scr_start.height = 0;
  V_FreeScreen(&wipe_scr_end);
  wipe_scr_end.height = 0;
  // Paranoia
  screens[SRC_SCR] = wipe_scr_start;
  screens[DEST_SCR] = wipe_scr_end;
  return 0;
}

int wipe_StartScreen(void)
{
  wipe_scr_start.height = SCREENHEIGHT;
  wipe_scr_start.not_on_heap = FALSE;
  V_AllocScreen(&wipe_scr_start);
  if (&screens[SRC_SCR] != &wipe_scr_start)
    V_FreeScreen(&screens[SRC_SCR]);
  screens[SRC_SCR] = wipe_scr_start;
  V_CopyRect(0, 0, 0,       SCREENWIDTH, SCREENHEIGHT, 0, 0, SRC_SCR, VPT_NONE ); // Copy start screen to buffer
  return 0;
}

int wipe_EndScreen(void)
{
  wipe_scr_end.height = SCREENHEIGHT;
  wipe_scr_end.not_on_heap = FALSE;
  V_AllocScreen(&wipe_scr_end);
  if (&screens[DEST_SCR] != &wipe_scr_end)
    V_FreeScreen(&screens[DEST_SCR]);
  screens[DEST_SCR] = wipe_scr_end;
  V_CopyRect(0, 0, 0,       SCREENWIDTH, SCREENHEIGHT, 0, 0, DEST_SCR, VPT_NONE); // Copy end screen to buffer
  V_CopyRect(0, 0, SRC_SCR, SCREENWIDTH, SCREENHEIGHT, 0, 0, 0       , VPT_NONE); // restore start screen
  return 0;
}

// killough 3/5/98: reformatted and cleaned up
int wipe_ScreenWipe(int ticks)
{
   static dbool   go;                               // when zero, stop the wipe
   if (!go)                                         // initial stuff
   {
      go = 1;
      wipe_scr = screens[0];
      wipe_initMelt(ticks);
   }
   /* Refresh wipe_scr.data each call: under direct-render
    * (libretro SW framebuffer), screens[0].data points at the
    * frontend's current frame buffer and rotates per frame.
    * The wipe blender writes pixels through wipe_scr.data, so we
    * have to re-bind to the live screens[0].data on every entry.
    * Other fields (height, not_on_heap) are stable so the
    * struct-copy pattern from initial setup still works. */
   wipe_scr.data = screens[0].data;
   // do a piece of wipe-in
   if (wipe_doMelt(ticks))     // final stuff
   {
      wipe_exitMelt(ticks);
      go = 0;
   }
   return !go;
}
