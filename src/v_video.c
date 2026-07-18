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
#include "u_png.h"
#include "vid_mode.h"

/* SIMD guards for the native-colour full-screen blit (V_DrawRGBAFullScreen).
 * Same guard shape as the renderer's column blenders. */
#if defined(__SSE2__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2) || defined(_M_X64)
#define V_ARGB_SSE2 1
#include <emmintrin.h>
#elif defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(_M_ARM64)
#define V_ARGB_NEON 1
#include <arm_neon.h>
#endif

#if defined(V_ARGB_SSE2) || defined(V_ARGB_NEON)
#if defined(_MSC_VER)
#define V_ARGB_ALIGN16 __declspec(align(16))
#else
#define V_ARGB_ALIGN16 __attribute__((aligned(16)))
#endif
#endif

// Each screen is [SCREENWIDTH*SCREENHEIGHT];
screeninfo_t screens[NUM_SCREENS];


// array of pointers to color translation tables
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
  const uint8_t defaultmap[256];
} crdef_t;

// killough 5/2/98: table-driven approach
static const crdef_t crdefs[] = {
  {"CRBRICK",  &colrngs[CR_BRICK ],
   { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 46, 46, 46, 46, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 46, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 16, 18, 20, 22, 24, 26, 28, 30, 32, 34, 36, 38, 40, 42, 44, 46, 192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223, 224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255 }
  },
  {"CRTAN",    &colrngs[CR_TAN   ],
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 78, 45, 78, 78, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 78, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 48, 50, 52, 54, 56, 58, 60, 62, 64, 66, 68, 70, 72, 74, 76, 78, 192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223, 224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255 }
  },
  {"CRGRAY",   &colrngs[CR_GRAY  ],
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 110, 110, 46, 110, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 110, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 80, 82, 84, 86, 88, 90, 92, 94, 96, 98, 100, 102, 104, 106, 108, 110, 192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223, 224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255 }
  },
  {"CRGREEN",  &colrngs[CR_GREEN ],
   { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 127, 127, 46, 127, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 127, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223, 224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255 }
  },
  {"CRBROWN",  &colrngs[CR_BROWN ],
   { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 143, 143, 46, 143, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 143, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223, 224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255 }
  },
  {"CRGOLD",   &colrngs[CR_GOLD  ],
   { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 167, 167, 46, 167, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 167, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 160, 160, 161, 161, 162, 162, 163, 163, 164, 164, 165, 165, 166, 166, 167, 167, 192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223, 224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255 }
  },
  {"CRRED",    &colrngs[CR_RED   ],
   { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 191, 191, 46, 191, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 191, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191, 192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223, 224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255 }
  },
  {"CRBLUE",   &colrngs[CR_BLUE  ],
   { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 203, 203, 46, 203, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 203, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 196, 196, 197, 197, 198, 198, 199, 199, 200, 200, 201, 201, 202, 202, 203, 203, 192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223, 224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255 }
  },
  {"CRORANGE", &colrngs[CR_ORANGE],
   { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 223, 223, 46, 223, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 223, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223, 192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223, 224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255 }
  },
  {"CRYELLOW", &colrngs[CR_YELLOW],
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 231, 231, 46, 231, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 231, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 224, 224, 225, 225, 226, 226, 227, 227, 228, 228, 229, 229, 230, 230, 231, 231, 192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223, 224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255 }
  },
  {"CRBLUE2",  &colrngs[CR_BLUE2],
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 207, 207, 46, 207, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 207, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175, 200, 200, 201, 201, 202, 202, 203, 203, 204, 204, 205, 205, 206, 206, 207, 207, 192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223, 224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255 }
  },
  {"CRLIGHT", &colrngs[CR_LIGHT],
    {   0,   1,   2,   3,   4,   5,   6,   6,   7,   9,  10,  10,  11,  13,  14,  15,
       16,  17,  18,  19,  20,  21,  22,  23,  24,  24,  25,  25,  26,  26,  27,  27,
       28,  28,  29,  29,  30,  30,  31,  31,  32,  33,  34,  35,  36,  37,  38,  39,
       48,  49,  50,  51,  52,  53,  54,  55,  56,  56,  57,  57,  58,  58,  59,  59,
       60,  60,  61,  61,  62,  62,  63,  63,  64,  65,  66,  67,  68,  69,  70,  71,
       80,  81,  82,  83,  84,  84,  85,  85,  86,  86,  87,  87,  88,  88,  89,  89,
       90,  90,  91,  91,  93,  94,  95,  95,  96,  97,  98,  99, 100, 101, 102, 103,
      112, 113, 114, 114, 115, 115, 116, 116, 117, 117, 118, 118, 119, 119, 120, 121,
      128, 129, 130, 131, 132, 133, 134, 135, 136, 136, 137, 137, 138, 138, 139, 139,
      140, 141, 142, 143, 144, 145, 146, 147, 152, 152, 153, 153, 154, 154, 155, 156,
      226, 227, 228, 161, 162, 163, 164, 165, 168, 169, 170, 171, 172, 172, 173, 173,
      208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223,
      192, 192, 193, 193, 194, 194, 195, 195, 196, 196, 197, 197, 198, 198, 199, 199,
      224, 225, 226, 227, 211, 211, 212, 212, 213, 213, 214, 214, 215, 215, 216, 216,
      224, 225, 226, 227, 228, 229, 230, 231, 232, 232, 233, 233, 236, 236, 237, 237,
      240, 241, 241, 242, 242, 243, 243, 244, 248, 249, 250, 251, 251, 252, 252, 255
    }
  },
  {NULL}
};


const uint8_t gammatable[5*256] = {
  // Gamma Correction OFF
    1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,
    17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,
    33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,
    49,50,51,52,53,54,55,56,57,58,59,60,61,62,63,64,
    65,66,67,68,69,70,71,72,73,74,75,76,77,78,79,80,
    81,82,83,84,85,86,87,88,89,90,91,92,93,94,95,96,
    97,98,99,100,101,102,103,104,105,106,107,108,109,110,111,112,
    113,114,115,116,117,118,119,120,121,122,123,124,125,126,127,128,
    128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,
    144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,
    160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,
    176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,
    192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,
    208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,
    224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239,
    240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255,
  // Gamma level 1
    2,4,5,7,8,10,11,12,14,15,16,18,19,20,21,23,
    24,25,26,27,29,30,31,32,33,34,36,37,38,39,40,41,
    42,44,45,46,47,48,49,50,51,52,54,55,56,57,58,59,
    60,61,62,63,64,65,66,67,69,70,71,72,73,74,75,76,
    77,78,79,80,81,82,83,84,85,86,87,88,89,90,91,92,
    93,94,95,96,97,98,99,100,101,102,103,104,105,106,107,108,
    109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,124,
    125,126,127,128,129,129,130,131,132,133,134,135,136,137,138,139,
    140,141,142,143,144,145,146,147,148,148,149,150,151,152,153,154,
    155,156,157,158,159,160,161,162,163,163,164,165,166,167,168,169,
    170,171,172,173,174,175,175,176,177,178,179,180,181,182,183,184,
    185,186,186,187,188,189,190,191,192,193,194,195,196,196,197,198,
    199,200,201,202,203,204,205,205,206,207,208,209,210,211,212,213,
    214,214,215,216,217,218,219,220,221,222,222,223,224,225,226,227,
    228,229,230,230,231,232,233,234,235,236,237,237,238,239,240,241,
    242,243,244,245,245,246,247,248,249,250,251,252,252,253,254,255,
  // Gamma level 2
    4,7,9,11,13,15,17,19,21,22,24,26,27,29,30,32,
    33,35,36,38,39,40,42,43,45,46,47,48,50,51,52,54,
    55,56,57,59,60,61,62,63,65,66,67,68,69,70,72,73,
    74,75,76,77,78,79,80,82,83,84,85,86,87,88,89,90,
    91,92,93,94,95,96,97,98,100,101,102,103,104,105,106,107,
    108,109,110,111,112,113,114,114,115,116,117,118,119,120,121,122,
    123,124,125,126,127,128,129,130,131,132,133,133,134,135,136,137,
    138,139,140,141,142,143,144,144,145,146,147,148,149,150,151,152,
    153,153,154,155,156,157,158,159,160,160,161,162,163,164,165,166,
    166,167,168,169,170,171,172,172,173,174,175,176,177,178,178,179,
    180,181,182,183,183,184,185,186,187,188,188,189,190,191,192,193,
    193,194,195,196,197,197,198,199,200,201,201,202,203,204,205,206,
    206,207,208,209,210,210,211,212,213,213,214,215,216,217,217,218,
    219,220,221,221,222,223,224,224,225,226,227,228,228,229,230,231,
    231,232,233,234,235,235,236,237,238,238,239,240,241,241,242,243,
    244,244,245,246,247,247,248,249,250,251,251,252,253,254,254,255,
  // Gamma level 3
    8,12,16,19,22,24,27,29,31,34,36,38,40,41,43,45,
    47,49,50,52,53,55,57,58,60,61,63,64,65,67,68,70,
    71,72,74,75,76,77,79,80,81,82,84,85,86,87,88,90,
    91,92,93,94,95,96,98,99,100,101,102,103,104,105,106,107,
    108,109,110,111,112,113,114,115,116,117,118,119,120,121,122,123,
    124,125,126,127,128,129,130,131,132,133,134,135,135,136,137,138,
    139,140,141,142,143,143,144,145,146,147,148,149,150,150,151,152,
    153,154,155,155,156,157,158,159,160,160,161,162,163,164,165,165,
    166,167,168,169,169,170,171,172,173,173,174,175,176,176,177,178,
    179,180,180,181,182,183,183,184,185,186,186,187,188,189,189,190,
    191,192,192,193,194,195,195,196,197,197,198,199,200,200,201,202,
    202,203,204,205,205,206,207,207,208,209,210,210,211,212,212,213,
    214,214,215,216,216,217,218,219,219,220,221,221,222,223,223,224,
    225,225,226,227,227,228,229,229,230,231,231,232,233,233,234,235,
    235,236,237,237,238,238,239,240,240,241,242,242,243,244,244,245,
    246,246,247,247,248,249,249,250,251,251,252,253,253,254,254,255,
  // Gamma level 4
    16,23,28,32,36,39,42,45,48,50,53,55,57,60,62,64,
    66,68,69,71,73,75,76,78,80,81,83,84,86,87,89,90,
    92,93,94,96,97,98,100,101,102,103,105,106,107,108,109,110,
    112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,128,
    128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,
    143,144,145,146,147,148,149,150,150,151,152,153,154,155,155,156,
    157,158,159,159,160,161,162,163,163,164,165,166,166,167,168,169,
    169,170,171,172,172,173,174,175,175,176,177,177,178,179,180,180,
    181,182,182,183,184,184,185,186,187,187,188,189,189,190,191,191,
    192,193,193,194,195,195,196,196,197,198,198,199,200,200,201,202,
    202,203,203,204,205,205,206,207,207,208,208,209,210,210,211,211,
    212,213,213,214,214,215,216,216,217,217,218,219,219,220,220,221,
    221,222,223,223,224,224,225,225,226,227,227,228,228,229,229,230,
    230,231,232,232,233,233,234,234,235,235,236,236,237,237,238,239,
    239,240,240,241,241,242,242,243,243,244,244,245,245,246,246,247,
    247,248,248,249,249,250,250,251,251,252,252,253,254,254,255,255
};


// killough 5/2/98: tiny engine driven by table above
void V_InitColorTranslation(void)
{
  register const crdef_t *p;
  for (p=crdefs; p->name; p++) {
    int lump = W_CheckNumForName(p->name);
    if (lump == -1)
      *p->map = p->defaultmap;
    else
      *p->map = W_CacheLumpName(p->name);
  }
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
/* Truecolor twin: the palette index is looked up straight in V_PaletteTC, so
 * the flat lands on the surface at the format's full channel width -- there
 * is no 565 stage anywhere on this path. */
#define GETCOLTC(col) (VID_PALTC(col, VID_COLORWEIGHTMASK))

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
  const uint8_t *src;
  int         flatnum = R_FlatNumForName(flatname);

  /* The tiled background is identical every frame for a given flat,
   * palette and resolution, but the finale (and the menu drawn over it)
   * re-tile it on every frame -- at high internal resolutions that is
   * millions of pixels of scale-blit + strided copy per frame for a
   * static image.  Cache the finished background and, when nothing that
   * affects it has changed, just memcpy the cache into the target screen.
   * Keyed on (flatnum, palette, dimensions): the palette key (V_Palette16)
   * catches gamma changes and mid-finale palette swaps (e.g. Heretic's
   * E2END), exactly as R_GetComposedColormap keys its LUT. */
  static uint8_t  *bg_cache       = NULL;
  static int       bg_flatnum     = -1;
  static const uint16_t *bg_pal   = NULL;
  static int       bg_w           = -1;
  static int       bg_h           = -1;
  const  size_t    surf_bytes     = (size_t)SURFACE_BYTE_PITCH * SCREENHEIGHT;

  /* A missing flat (e.g. a Doom-only menu flat requested under another
   * game) yields flatnum -1; firstflat-1 then caches a bogus lump and the
   * tiling loop dereferences a NULL source. Skip drawing instead. */
  if (flatnum < 0)
    return;

  /* Fast path: the cached background is still valid -- copy it straight
   * into the requested screen and we are done. */
  if (bg_cache && flatnum == bg_flatnum && V_Palette16 == bg_pal &&
      SCREENWIDTH == bg_w && SCREENHEIGHT == bg_h)
  {
    memcpy(screens[scrn].data, bg_cache, surf_bytes);
    return;
  }

  // killough 4/17/98:
  src = W_CacheLumpNum(lump = firstflat + flatnum);

  /* R_FlatNumForName returns 0 (not -1) for a flat that is absent from the
   * IWAD -- e.g. the menu background flat under shareware Heretic, which
   * lacks FLAT513.  flatnum 0 passes the < 0 guard above but firstflat+0
   * can resolve to a lump that is not a real 64x64 flat (or, when the flat
   * namespace is empty, a zero-length / NULL lump).  V_DRAWFLAT then
   * dereferences src unconditionally and crashes.  Bail out if the lump is
   * not a usable 64x64 (4096-byte) flat. */
  if (!src || W_LumpLength(lump) < 4096)
  {
    W_UnlockLumpNum(lump);
    return;
  }

  /* V_DrawBlock(0, 0, scrn, 64, 64, src, 0); */

  if (VID_TRUECOLOR)
    V_DRAWFLAT(scrn, uint32_t, GETCOLTC)
  else
    V_DRAWFLAT(scrn, int16_t, GETCOL16);

  /* end V_DrawBlock */

  for (y=0 ; y<SCREENHEIGHT ; y+=h)
    for (x=y ? 0 : w; x<SCREENWIDTH ; x+=w)
      V_CopyRect(0, 0, scrn, ((SCREENWIDTH-x) < w) ? (SCREENWIDTH-x) : w,
     ((SCREENHEIGHT-y) < h) ? (SCREENHEIGHT-y) : h, x, y, scrn, VPT_NONE);
  W_UnlockLumpNum(lump);

  /* Snapshot the just-built background so subsequent frames (same flat,
   * palette and resolution) take the memcpy fast path above.  Realloc to
   * the current surface size; a resolution change invalidates the key, so
   * a stale-size cache is never read. */
  {
    uint8_t *nc = (uint8_t *)realloc(bg_cache, surf_bytes);
    if (nc)
    {
      bg_cache   = nc;
      memcpy(bg_cache, screens[scrn].data, surf_bytes);
      bg_flatnum = flatnum;
      bg_pal     = V_Palette16;
      bg_w       = SCREENWIDTH;
      bg_h       = SCREENHEIGHT;
    }
    else
    {
      /* Allocation failed: drop any stale cache so we never copy from a
       * wrong-size buffer; the slow rebuild path still drew correctly. */
      free(bg_cache);
      bg_cache   = NULL;
      bg_flatnum = -1;
    }
  }
}

/*
 * V_DrawRawScreenSection
 *
 * Draw a horizontal band of a raw full-screen image: source rows
 * [src_row, src_row + num_rows) placed so that source row src_row lands at
 * destination 200-space row dst_row.  Used by the Heretic E3 finale, which
 * scrolls one raw screen up off the top while the next scrolls in from the
 * bottom.  Geometry (aspect-preserving fit, horizontal centring) matches
 * V_DrawRawScreen; the caller is responsible for clearing/!filling the
 * background as needed (the finale draws two adjoining sections that cover
 * the image area between them).
 */
void V_DrawRawScreenSection(const char *lump_name, int src_row,
                            int dst_row, int num_rows)
{
  int          i, j;
  int          lump_num   = W_CheckNumForName(lump_name);
  int          lump_len;
  int          lump_width;
  int          x_offset;
  float        x_factor, y_factor;
  const uint8_t *raw;

  if (lump_num < 0 || num_rows <= 0)
    return;

  lump_len   = W_LumpLength(lump_num);
  lump_width = lump_len / 200;
  if (lump_width <= 0)
    return;

  x_factor = (float)SCREENWIDTH  / (float)lump_width;
  y_factor = (float)SCREENHEIGHT / 200.0f;
  if (y_factor < x_factor)
    x_factor = y_factor;
  y_factor = x_factor;                  /* uniform scale (aspect preserved) */

  x_offset = (int)((SCREENWIDTH - (x_factor * lump_width)) / 2);

  raw = (const uint8_t *)W_CacheLumpNum(lump_num);

  for (i = 0; i < lump_width; i++)
  {
    int x     = (int)(i * x_factor);
    int width = (int)((i + 1) * x_factor) - x;
    int x_pos = x_offset + x;

    if (width <= 0 || x_pos < 0 || x_pos > SCREENWIDTH - width)
      continue;

    for (j = 0; j < num_rows; j++)
    {
      int srcrow = src_row + j;
      int dstrow = dst_row + j;
      int y, height;

      if (srcrow < 0 || srcrow >= 200 || dstrow < 0 || dstrow >= 200)
        continue;

      y      = (int)(dstrow * y_factor);
      height = (int)((dstrow + 1) * y_factor) - y;
      if (height <= 0)
        continue;

      V_FillRect(x_pos, y, width, height, raw[srcrow * lump_width + i]);
    }
  }

  W_UnlockLumpNum(lump_num);
}

/*
 * V_DrawRawScreen
 *
 * Heretic (and other Raven) full-screen images such as TITLE, CREDIT,
 * HELP1/HELP2 and the finale screens are stored as raw column-major
 * 8-bit bitmaps (width * 200 bytes), not as Doom patch_t graphics, so
 * they cannot go through the patch drawer.  This is a reduced,
 * software-only blit: it stretches the source image to the current
 * screen dimensions with simple nearest-sampling via V_FillRect.  No
 * widescreen / aspect-ratio modes or GL path -- those are not present
 * in this core.
 */
void V_DrawRawScreen(const char *lump_name)
{
  int          i, j;
  int          lump_num   = W_CheckNumForName(lump_name);
  int          lump_len;
  int          lump_width;
  int          x_offset;
  float        x_factor, y_factor;
  const uint8_t *raw;

  if (lump_num < 0)
    return;

  lump_len   = W_LumpLength(lump_num);
  lump_width = lump_len / 200;          /* raw images are 200 rows tall */
  if (lump_width <= 0)
    return;

  x_factor = (float)SCREENWIDTH  / (float)lump_width;
  y_factor = (float)SCREENHEIGHT / 200.0f;
  if (y_factor < x_factor)
    x_factor = y_factor;                /* keep aspect, letterbox sides */

  x_offset = (int)((SCREENWIDTH - (x_factor * lump_width)) / 2);

  raw = (const uint8_t *)W_CacheLumpNum(lump_num);

  /* The image is centred and aspect-preserved, so it can leave bars on the
   * sides (x_offset > 0) or top/bottom (scaled height < SCREENHEIGHT).
   * Under the libretro rotating-buffer model those bars are never otherwise
   * painted, so clear the whole framebuffer to black first -- otherwise they
   * keep whatever the previous screen (e.g. the menu we came from) left
   * there. */
  if (x_offset > 0 || (int)(x_factor * 200.0f) < SCREENHEIGHT)
    V_FillRect(0, 0, SCREENWIDTH, SCREENHEIGHT, 0);

  /* Source is row-major: byte index = row*width + column. */
  for (i = 0; i < lump_width; i++)
  {
    int x     = (int)(i * x_factor);
    int width = (int)((i + 1) * x_factor) - x;
    int x_pos = x_offset + x;

    if (width <= 0 || x_pos < 0 || x_pos > SCREENWIDTH - width)
      continue;

    for (j = 0; j < 200; j++)
    {
      int y      = (int)(j * y_factor);
      int height = (int)((j + 1) * y_factor) - y;

      if (height <= 0)
        continue;

      V_FillRect(x_pos, y, width, height, raw[j * lump_width + i]);
    }
  }

  W_UnlockLumpNum(lump_num);
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
   /* V_DrawMemPatch only mutates drawvars.short_topleft and
    * drawvars.int_topleft (line ~398 below).  Saving / restoring the
    * full draw_vars_t struct on every patch draw was unnecessary --
    * with the status bar, HUD, and menu drawing dozens of patches per
    * frame, those struct copies added up.  Just save the two pointer
    * fields we actually touch. */
   unsigned short *old_short_topleft = drawvars.short_topleft;
   unsigned int   *old_int_topleft   = drawvars.int_topleft;
   int col = 0;
   int w   = (patch->width << 16) - 1; // CPhipps - -1 for faster flipping
   int DX  = (SCREENWIDTH<<16) / 320;
   int DXI = (320<<16) / SCREENWIDTH;
   int DY = (SCREENHEIGHT<<16) / 200;
   int DYI = (200<<16) / SCREENHEIGHT;
   const uint8_t *trans = translationtables + 256*((cm-CR_LIMIT)-1);

   /* Full-screen page art that is not the vanilla 320x200 size (ZDoom
    * hi-res title cards, etc.): scale by the patch's own dimensions so
    * the whole image maps onto the whole screen, instead of the 320x200
    * virtual page that would blow a 640x480 card up to ~2x screen size.
    * Falls back to the regular factors for 320-wide / 200-tall art, so
    * vanilla and widescreen-but-200-tall patches are unchanged. */
   if ((flags & VPT_FITSCREEN) && patch->width > 0 && patch->height > 0)
   {
      if (patch->width != 320)
      {
         DX  = (SCREENWIDTH << 16) / patch->width;
         DXI = (patch->width << 16) / SCREENWIDTH;
      }
      if (patch->height != 200)
      {
         DY  = (SCREENHEIGHT << 16) / patch->height;
         DYI = (patch->height << 16) / SCREENHEIGHT;
      }
   }

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
   drawvars.int_topleft   = (unsigned int*)screens[scrn].data;

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

   /* prevcolumn/nextcolumn (and the prevsource/nextsource they feed) are
    * only consumed by the RDRAW_FILTER_LINEAR edge-interpolation column
    * functions. For RDRAW_FILTER_NONE -- which is what all UI patches (menu,
    * HUD, status bar) use -- they are dead, so skip the two extra
    * R_GetPatchColumn lookups per output column in that case. At a high
    * internal resolution each patch spans many output columns, so this
    * removes a large number of redundant lookups per frame. */
   {
   const int filter_linear = (drawvars.filterpatch == RDRAW_FILTER_LINEAR);
   for (dcvars.x=left; dcvars.x<right; dcvars.x++, col+= DXI)
   {
      int i;
      const int colindex          = (flags & VPT_FLIP) ? ((w - col)>>16): (col>>16);
      const rcolumn_t *column     = R_GetPatchColumn(patch, colindex);
      const rcolumn_t *prevcolumn = filter_linear ? R_GetPatchColumn(patch, colindex-1) : NULL;
      const rcolumn_t *nextcolumn = filter_linear ? R_GetPatchColumn(patch, colindex+1) : NULL;

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
   }

   R_ResetColumnBuffer();
   drawvars.short_topleft = old_short_topleft;
   drawvars.int_topleft   = old_int_topleft;
}

// CPhipps - some simple, useful wrappers for that function, for drawing patches from wads

// CPhipps - GNU C only suppresses generating a copy of a function if it is
// static inline; other compilers have different behaviour.
// This inline is _only_ for the function below

void V_DrawNumPatch(int x, int y, int scrn, int lump,
         int cm, enum patch_translation_e flags)
{
  if(lump < 0)
  {
    I_Error("V_DrawNumPatch: missing lump won't be drawn");
    return;
  }
    
  V_DrawMemPatch(x, y, scrn, R_CachePatchNum(lump), cm, flags);
  R_UnlockPatchNum(lump);
}

/* [FG] fullscreen page draw: automatically center patches wider than 320
 * that carry no horizontal offset.  The KEX re-release add-on wads ship
 * 426-wide title cards with a zero leftoffset, which a plain draw at
 * (0,0) leaves left-aligned with the right side cropped (issue #195);
 * id's own widescreen packs set leftoffset and center through the normal
 * offset path.  Matches Woof / dsda-doom. */
static int V_WidePatchCenter(const rpatch_t *patch, int x)
{
  if (patch->width > 320 && patch->leftoffset == 0)
    x -= (patch->width - 320) / 2;
  return x;
}

void V_DrawNumPatchFS(int x, int y, int scrn, int lump,
         int cm, enum patch_translation_e flags)
{
  const rpatch_t *patch;

  if(lump < 0)
  {
    I_Error("V_DrawNumPatchFS: missing lump won't be drawn");
    return;
  }

  patch = R_CachePatchNum(lump);
  /* A full-screen page taller than the vanilla 200 is ZDoom-style hi-res
   * art (e.g. a 640x480 TITLEPIC); fit it to the screen by its own size
   * rather than the 320x200 virtual page, which would scale it well past
   * the screen edges.  The fit fills the whole frame, so the 320-based
   * wide-centering used for 200-tall widescreen cards does not apply. */
  if ((flags & VPT_STRETCH) && patch->height > 200)
    V_DrawMemPatch(0, 0, scrn, patch, cm,
                   (enum patch_translation_e)(flags | VPT_FITSCREEN));
  else
    V_DrawMemPatch(V_WidePatchCenter(patch, x), y, scrn, patch, cm, flags);
  R_UnlockPatchNum(lump);
}

/*
 * V_DrawRGBAFullScreenTC
 *
 * Truecolor blit of a native-colour full-screen image.  This is the single
 * biggest quantisation win in the 2D layer: the source is a 24/32-bit PNG
 * (full-colour title cards, the help screen, the intermission backdrop), so
 * the RGB565 path discards 3/2/3 bits per channel and visibly bands images
 * that were authored smooth.  Here the source bytes reach the surface with
 * no precision lost and no intermediate format:
 *
 *   XRGB8888    -- a pure channel reorder (source 0xAABBGGRR -> 0x00RRGGBB).
 *                  No arithmetic at all, so the blit is bit-lossless.
 *   XRGB2101010 -- each 8-bit channel widened to 10 by bit replication,
 *                  (c<<2)|(c>>6), which maps 0->0 and 255->1023 exactly and
 *                  is the correct one-step expansion (NOT a narrow-then-
 *                  widen round trip).
 *
 * Sampling stays point/nearest, matching the 16-bit path, so geometry is
 * identical -- only the colour precision changes.
 */
static void V_DrawRGBAFullScreenTC(int scrn, const unsigned *argb,
                                   int aw, int ah)
{
  uint32_t *surf   = (uint32_t *)screens[scrn].data;
  int sx_step      = (aw << 16) / SCREENWIDTH;
  int sy_step      = (ah << 16) / SCREENHEIGHT;
  const int deep   = (vid_mode == VID_MODEHDR10);
  int oy;

  for (oy = 0; oy < SCREENHEIGHT; oy++)
  {
    int srcy = (oy * sy_step) >> 16;
    const unsigned *srow;
    uint32_t *row = surf + (size_t)oy * SURFACE_SHORT_PITCH;
    int ox = 0, sx = 0;
    if (srcy >= ah) srcy = ah - 1;
    srow = argb + (size_t)srcy * aw;

#if defined(V_ARGB_SSE2) || defined(V_ARGB_NEON)
    /* Four pixels per pass: gather the (strided) source texels, then do the
     * reorder / widen in vector lanes.  Bit-identical to the scalar tail. */
    for (; ox + 4 <= SCREENWIDTH; ox += 4)
    {
      V_ARGB_ALIGN16 uint32_t sp[4];
      int j;
      for (j = 0; j < 4; j++, sx += sx_step)
        sp[j] = srow[sx >> 16];
#if defined(V_ARGB_SSE2)
      {
        __m128i s  = _mm_load_si128((const __m128i *)sp);
        __m128i m8 = _mm_set1_epi32(0xff);
        __m128i o;
        if (deep)
        {
          __m128i r = _mm_and_si128(s, m8);
          __m128i g = _mm_and_si128(_mm_srli_epi32(s, 8), m8);
          __m128i b = _mm_and_si128(_mm_srli_epi32(s, 16), m8);
          r = _mm_or_si128(_mm_slli_epi32(r, 2), _mm_srli_epi32(r, 6));
          g = _mm_or_si128(_mm_slli_epi32(g, 2), _mm_srli_epi32(g, 6));
          b = _mm_or_si128(_mm_slli_epi32(b, 2), _mm_srli_epi32(b, 6));
          o = _mm_or_si128(_mm_or_si128(_mm_slli_epi32(r, 20),
                                        _mm_slli_epi32(g, 10)), b);
        }
        else
        {
          /* (r<<16) | g | (b>>16) -- swap the R and B bytes, drop alpha */
          o = _mm_or_si128(
                _mm_or_si128(_mm_slli_epi32(_mm_and_si128(s, m8), 16),
                             _mm_and_si128(s, _mm_set1_epi32(0xff00))),
                _mm_and_si128(_mm_srli_epi32(s, 16), m8));
        }
        _mm_storeu_si128((__m128i *)(row + ox), o);
      }
#else /* V_ARGB_NEON */
      {
        uint32x4_t s  = vld1q_u32(sp);
        uint32x4_t m8 = vdupq_n_u32(0xff);
        uint32x4_t o;
        if (deep)
        {
          uint32x4_t r = vandq_u32(s, m8);
          uint32x4_t g = vandq_u32(vshrq_n_u32(s, 8), m8);
          uint32x4_t b = vandq_u32(vshrq_n_u32(s, 16), m8);
          r = vorrq_u32(vshlq_n_u32(r, 2), vshrq_n_u32(r, 6));
          g = vorrq_u32(vshlq_n_u32(g, 2), vshrq_n_u32(g, 6));
          b = vorrq_u32(vshlq_n_u32(b, 2), vshrq_n_u32(b, 6));
          o = vorrq_u32(vorrq_u32(vshlq_n_u32(r, 20),
                                  vshlq_n_u32(g, 10)), b);
        }
        else
        {
          o = vorrq_u32(
                vorrq_u32(vshlq_n_u32(vandq_u32(s, m8), 16),
                          vandq_u32(s, vdupq_n_u32(0xff00))),
                vandq_u32(vshrq_n_u32(s, 16), m8));
        }
        vst1q_u32(row + ox, o);
      }
#endif
    }
#endif

    for (; ox < SCREENWIDTH; ox++, sx += sx_step)
    {
      unsigned p = srow[sx >> 16];
      int r = (int)(p & 0xff);
      int g = (int)((p >> 8) & 0xff);
      int b = (int)((p >> 16) & 0xff);
      if (deep)
        row[ox] = ((uint32_t)((r << 2) | (r >> 6)) << 20) |
                  ((uint32_t)((g << 2) | (g >> 6)) << 10) |
                   (uint32_t)((b << 2) | (b >> 6));
      else
        row[ox] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
    }
  }
}

/*
 * V_DrawRGBAFullScreen
 *
 * Draw a full-screen art lump from its native-colour image, when one was
 * retained for it (full-colour title cards, the menu help screen and the
 * intermission backdrop ship as 24/32-bit PNGs).  The materialized patch the
 * normal path would draw is PLAYPAL-indexed, which bands these smooth images
 * badly; here the original image is point-sampled, stretched to the surface,
 * and written straight to RGB565 at full channel precision.  Returns 1 when it
 * drew, 0 when the lump has no native copy (caller falls back to the indexed
 * patch path).  Opaque: these are backdrops, so source alpha is ignored.
 */
int V_DrawRGBAFullScreen(int scrn, int lump)
{
  const unsigned *argb;
  uint16_t *surf;
  int aw = 0, ah = 0, oy;
  int sx_step, sy_step;

  if (lump < 0)
    return 0;
  argb = U_PNGCacheRGBA(lump, &aw, &ah);
  if (!argb || aw <= 0 || ah <= 0)
    return 0;

  if (VID_TRUECOLOR)
  {
    V_DrawRGBAFullScreenTC(scrn, argb, aw, ah);
    return 1;
  }

  surf    = (uint16_t *)screens[scrn].data;
  sx_step = (aw << 16) / SCREENWIDTH;
  sy_step = (ah << 16) / SCREENHEIGHT;

  for (oy = 0; oy < SCREENHEIGHT; oy++)
  {
    int srcy = (oy * sy_step) >> 16;
    const unsigned *srow;
    uint16_t *row = surf + (size_t)oy * SURFACE_SHORT_PITCH;
    int ox = 0, sx = 0;
    if (srcy >= ah) srcy = ah - 1;
    srow = argb + (size_t)srcy * aw;

#if defined(V_ARGB_SSE2) || defined(V_ARGB_NEON)
    /* Eight pixels per pass: gather the (strided) source texels, then narrow
     * 0xAABBGGRR -> 565 in vector lanes.  Bit-identical to the scalar tail. */
    for (; ox + 8 <= SCREENWIDTH; ox += 8)
    {
      V_ARGB_ALIGN16 uint32_t sp[8];
      int j;
      for (j = 0; j < 8; j++, sx += sx_step)
        sp[j] = srow[sx >> 16];
#if defined(V_ARGB_SSE2)
      {
        __m128i s0 = _mm_load_si128((const __m128i *)sp);
        __m128i s1 = _mm_load_si128((const __m128i *)(sp + 4));
        __m128i m8 = _mm_set1_epi32(0xff);
        /* R in low byte, B in high byte (0xAABBGGRR) */
        __m128i r  = _mm_packs_epi32(_mm_srli_epi32(_mm_and_si128(s0, m8), 3),
                                     _mm_srli_epi32(_mm_and_si128(s1, m8), 3));
        __m128i g  = _mm_packs_epi32(
                       _mm_srli_epi32(_mm_and_si128(_mm_srli_epi32(s0, 8), m8), 2),
                       _mm_srli_epi32(_mm_and_si128(_mm_srli_epi32(s1, 8), m8), 2));
        __m128i b  = _mm_packs_epi32(
                       _mm_srli_epi32(_mm_and_si128(_mm_srli_epi32(s0, 16), m8), 3),
                       _mm_srli_epi32(_mm_and_si128(_mm_srli_epi32(s1, 16), m8), 3));
        __m128i o  = _mm_or_si128(_mm_or_si128(_mm_slli_epi16(r, 11),
                                  _mm_slli_epi16(g, 5)), b);
        _mm_storeu_si128((__m128i *)(row + ox), o);
      }
#else /* V_ARGB_NEON */
      {
        uint32x4_t s0 = vld1q_u32(sp);
        uint32x4_t s1 = vld1q_u32(sp + 4);
        uint16x8_t r  = vcombine_u16(
                          vmovn_u32(vshrq_n_u32(vandq_u32(s0, vdupq_n_u32(0xff)), 3)),
                          vmovn_u32(vshrq_n_u32(vandq_u32(s1, vdupq_n_u32(0xff)), 3)));
        uint16x8_t g  = vcombine_u16(
                          vmovn_u32(vshrq_n_u32(vandq_u32(vshrq_n_u32(s0, 8), vdupq_n_u32(0xff)), 2)),
                          vmovn_u32(vshrq_n_u32(vandq_u32(vshrq_n_u32(s1, 8), vdupq_n_u32(0xff)), 2)));
        uint16x8_t b  = vcombine_u16(
                          vmovn_u32(vshrq_n_u32(vandq_u32(vshrq_n_u32(s0, 16), vdupq_n_u32(0xff)), 3)),
                          vmovn_u32(vshrq_n_u32(vandq_u32(vshrq_n_u32(s1, 16), vdupq_n_u32(0xff)), 3)));
        uint16x8_t o  = vorrq_u16(vorrq_u16(vshlq_n_u16(r, 11),
                                  vshlq_n_u16(g, 5)), b);
        vst1q_u16(row + ox, o);
      }
#endif
    }
#endif

    for (; ox < SCREENWIDTH; ox++, sx += sx_step)
    {
      unsigned p = srow[sx >> 16];
      int r = (int)(p & 0xff);
      int g = (int)((p >> 8) & 0xff);
      int b = (int)((p >> 16) & 0xff);
      row[ox] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
  }
  return 1;
}


/*
 * V_DrawNumPatchFullScreenCached
 *
 * Draws a full-screen patch (a 320x200 art lump such as CREDIT / HELP2 /
 * VICTORY2 / ENDPIC) stretched to the whole surface, caching the rendered
 * result.  These finale/help art screens redraw the same static lump every
 * frame; at a high internal resolution the VPT_STRETCH blit scales ~3M
 * output pixels through the column drawer each frame (measured at ~11 ms at
 * 2560x1200 -- essentially the entire frame for a screen that draws nothing
 * else).  The output only depends on (lump, palette, resolution, colour
 * remap), so cache it and memcpy on a hit.
 *
 * Keyed like the other caches in this file: the palette pointer
 * (V_Palette16) catches gamma changes and mid-screen palette swaps, the
 * dimensions catch a resolution/aspect change (and the pitch change that
 * comes with it).  Only the explicit full-screen art callers use this; the
 * hundreds of small dynamic HUD / menu / status-bar patches keep going
 * through V_DrawMemPatch unchanged.
 */
void V_DrawNumPatchFullScreenCached(int scrn, int lump, int cm)
{
  static uint8_t        *fs_cache  = NULL;
  static int             fs_lump   = -1;
  static const uint16_t *fs_pal    = NULL;
  static int             fs_cm     = -2;
  static int             fs_w      = -1;
  static int             fs_h      = -1;
  const  size_t          surf_bytes = (size_t)SURFACE_BYTE_PITCH * SCREENHEIGHT;

  if (lump < 0)
    return;

  /* Fast path: cached image still valid -> straight copy. */
  if (fs_cache && lump == fs_lump && V_Palette16 == fs_pal && cm == fs_cm &&
      SCREENWIDTH == fs_w && SCREENHEIGHT == fs_h)
  {
    memcpy(screens[scrn].data, fs_cache, surf_bytes);
    return;
  }

  /* Miss: render the stretched patch the normal way, then snapshot it.
   * Wide offset-less patches are centered like every full-screen draw. */
  if (!V_DrawRGBAFullScreen(scrn, lump))
  {
    const rpatch_t *patch = R_CachePatchNum(lump);
    if (patch->height > 200)
      V_DrawMemPatch(0, 0, scrn, patch, cm,
                     (enum patch_translation_e)(VPT_STRETCH | VPT_FITSCREEN));
    else
      V_DrawMemPatch(V_WidePatchCenter(patch, 0), 0, scrn, patch, cm,
                     VPT_STRETCH);
    R_UnlockPatchNum(lump);
  }

  {
    uint8_t *nc = (uint8_t *)realloc(fs_cache, surf_bytes);
    if (nc)
    {
      fs_cache = nc;
      memcpy(fs_cache, screens[scrn].data, surf_bytes);
      fs_lump  = lump;
      fs_pal   = V_Palette16;
      fs_cm    = cm;
      fs_w     = SCREENWIDTH;
      fs_h     = SCREENHEIGHT;
    }
    else
    {
      free(fs_cache);
      fs_cache = NULL;
      fs_lump  = -1;
    }
  }
}

uint16_t *V_Palette16 = NULL;
static uint16_t *Palettes16 = NULL;
static int currentPaletteIndex = 0;

/* ---- truecolor palette (XRGB8888 / XRGB2101010) --------------------------
 * Built in lockstep with the 16-bit palette by every path that produces one
 * (PLAYPAL expansion, raw-palette swap, pain/bonus blend), so the two always
 * describe the same colours.  V_Palette16 stays the cache key everywhere it
 * already is -- it changes exactly when V_PaletteTC does -- so the flat /
 * fullscreen / composed-LUT caches need no changes.
 *
 * Layout matches V_Palette16: [colour*VID_NUMCOLORWEIGHTS + weight], each
 * entry premultiplied by weight/(VID_NUMCOLORWEIGHTS-1) so the filtered
 * paths can sum four taps.  Channels are the format's native width (8 or
 * 10 bits), which is the whole point: the light ramp and the composed LUTs
 * resolve gradients at full output precision instead of 5/6 bits. */
uint32_t *V_PaletteTC = NULL;
static uint32_t *PalettesTC   = NULL;
static uint32_t *RawPaletteTC = NULL;
static uint32_t *BlendPalTC[2];

/* Pack one gamma-corrected 8-bit colour at weight t into the active format.
 * roundUp reproduces the 16-bit builder's DONT_ROUND_ABOVE behaviour: very
 * bright colours are not rounded up, so the four-tap filtered sum cannot
 * overflow a channel field into its neighbour. */
static INLINE uint32_t V_PackTC(int r, int g, int b, float t,
                                float roundUpR, float roundUpG, float roundUpB)
{
  if (vid_mode == VID_MODEHDR10)
  {
    /* HDR10: the sample is absolute luminance, not a display-referred
     * value.  Build the colour the SDR path would have produced, then state
     * what it means in nits -- ordinary content sits exactly at the
     * frontend's paper white, which is what makes an HDR frame match the
     * SDR one everywhere except where the renderer marks a colour emissive. */
    int    er, eg, eb;
    double sr = ((double)r * t + roundUpR) / 255.0;
    double sg = ((double)g * t + roundUpG) / 255.0;
    double sb = ((double)b * t + roundUpB) / 255.0;
    if (sr < 0.0) sr = 0.0; else if (sr > 1.0) sr = 1.0;
    if (sg < 0.0) sg = 0.0; else if (sg > 1.0) sg = 1.0;
    if (sb < 0.0) sb = 0.0; else if (sb > 1.0) sb = 1.0;
    VID_EncodeHDR10(sr, sg, sb, 1.0, &er, &eg, &eb);
    return ((uint32_t)er << 20) | ((uint32_t)eg << 10) | (uint32_t)eb;
  }
  else
  {
    int nr = (int)(r * t + roundUpR);
    int ng = (int)(g * t + roundUpG);
    int nb = (int)(b * t + roundUpB);
    return ((uint32_t)nr << 16) | ((uint32_t)ng << 8) | (uint32_t)nb;
  }
}

#define DONT_ROUND_ABOVE 220
//
// V_UpdateTrueColorPalette
//
/* The engine expects PLAYPAL to carry the 14 standard palettes: the base
 * palette, eight red damage-flash palettes, four gold item-pickup palettes
 * and one green radiation-suit palette.  Some ZDoom-targeted mods ship a
 * PLAYPAL with only the base palette (768 bytes) and rely on the source port
 * to synthesise the flash palettes at runtime.  When the lump is short, the
 * code that indexes a flash palette would read past the lump (and past the
 * Palettes16 buffer), tinting the screen with garbage on damage.  Reproduce
 * the colours id's palette generator (dcolors) baked into the stock lump:
 * blend the base palette toward red / gold / green by the canonical amounts. */
#define V_NUM_STD_PALETTES 14
static void V_SynthPaletteColor(const uint8_t *base, int palnum, int idx,
                                uint8_t *out)
{
  /* base RGB for this colour index in palette 0 */
  int br = base[idx*3+0], bg = base[idx*3+1], bb = base[idx*3+2];
  int tr, tg, tb;       /* blend target */
  int num = 0, den = 1; /* blend fraction num/den */

  if (palnum >= 1 && palnum <= 8)        /* red damage flash */
  {
    tr = 255; tg = 0; tb = 0;
    num = palnum; den = 9;               /* level/9, matches the stock lump */
  }
  else if (palnum >= 9 && palnum <= 12)  /* gold item pickup */
  {
    tr = 215; tg = 186; tb = 69;
    num = palnum - 8; den = 8;           /* (level+1)/8 -> 1/8..4/8 */
  }
  else if (palnum == 13)                 /* green radiation suit */
  {
    tr = 0; tg = 255; tb = 0;
    num = 1; den = 8;                    /* 1/8 */
  }
  else                                   /* base palette, no blend */
  {
    out[0] = (uint8_t)br; out[1] = (uint8_t)bg; out[2] = (uint8_t)bb;
    return;
  }

  out[0] = (uint8_t)(br + ((tr - br) * num) / den);
  out[1] = (uint8_t)(bg + ((tg - bg) * num) / den);
  out[2] = (uint8_t)(bb + ((tb - bb) * num) / den);
}

void V_UpdateTrueColorPalette(void) {
  int i, w, p;
  int paletteNum = currentPaletteIndex;
  static int usegammaOnLastPaletteGeneration = -1;
  int pplump         = W_GetNumForName("PLAYPAL");
  int gtlump         = (W_CheckNumForName)("GAMMATBL",ns_prboom);
  const uint8_t *pal = W_CacheLumpNum(pplump);
  const uint8_t *const gtable =
    ((gtlump == -1)? gammatable : (const uint8_t *)W_CacheLumpNum(gtlump)) +
    (256*(usegamma))
  ;

  int numPals = W_LumpLength(pplump) / (3*256);
  int genPals = (numPals < V_NUM_STD_PALETTES) ? V_NUM_STD_PALETTES : numPals;

  if (usegammaOnLastPaletteGeneration != usegamma) {
    if (Palettes16) free(Palettes16);
    Palettes16 = NULL;
    if (PalettesTC) free(PalettesTC);
    PalettesTC = NULL;
    usegammaOnLastPaletteGeneration = usegamma;      
  }
  
  if (!Palettes16)
  {
     // set short palette
     Palettes16 = malloc(genPals*256*sizeof(uint16_t)*VID_NUMCOLORWEIGHTS);
     if (VID_TRUECOLOR && !PalettesTC)
        PalettesTC = malloc(genPals*256*sizeof(uint32_t)*VID_NUMCOLORWEIGHTS);
     for (p=0; p<genPals; p++)
     {
        for (i=0; i<256; i++)
        {
           uint8_t r, g, b;
           float roundUpR, roundUpG, roundUpB;
           if (p < numPals)
           {
              r = gtable[pal[(256*p+i)*3+0]];
              g = gtable[pal[(256*p+i)*3+1]];
              b = gtable[pal[(256*p+i)*3+2]];
           }
           else
           {
              /* lump is missing this flash palette: synthesise it from the
               * base palette so a damage/pickup/radsuit tint still works
               * instead of reading garbage past the lump */
              uint8_t rgb[3];
              V_SynthPaletteColor(pal, p, i, rgb);
              r = gtable[rgb[0]];
              g = gtable[rgb[1]];
              b = gtable[rgb[2]];
           }

           // ideally, we should always round up, but very bright colors
           // overflow the blending adds, so they don't get rounded.
           roundUpR = (r > DONT_ROUND_ABOVE) ? 0 : 0.5f;
           roundUpG = (g > DONT_ROUND_ABOVE) ? 0 : 0.5f;
           roundUpB = (b > DONT_ROUND_ABOVE) ? 0 : 0.5f;

           for (w=0; w<VID_NUMCOLORWEIGHTS; w++)
           {
              float t = (float)(w)/(float)(VID_NUMCOLORWEIGHTS-1);
              if (PalettesTC)
                 PalettesTC[((p*256+i)*VID_NUMCOLORWEIGHTS)+w] =
                    V_PackTC(r, g, b, t, roundUpR, roundUpG, roundUpB);
#if defined(ABGR1555)
              int nr  = (int)((r>>3)*t+roundUpR);
              int ng  = (int)((g>>3)*t+roundUpG);
              int nb  = (int)((b>>3)*t+roundUpB);
              Palettes16[((p*256+i)*VID_NUMCOLORWEIGHTS)+w] = (
                    (nb<<10) | (ng<<5) | nr
                    );
#else
              int nr  = (int)((r>>3)*t+roundUpR);
              int ng  = (int)((g>>2)*t+roundUpG);
              int nb  = (int)((b>>3)*t+roundUpB);
              Palettes16[((p*256+i)*VID_NUMCOLORWEIGHTS)+w] = (
                    (nr<<11) | (ng<<5) | nb
                    );
#endif
           }
        }
     }
  }

  V_Palette16 = Palettes16 + paletteNum*256*VID_NUMCOLORWEIGHTS;
  if (PalettesTC)
    V_PaletteTC = PalettesTC + paletteNum*256*VID_NUMCOLORWEIGHTS;

  W_UnlockLumpNum(pplump);
  W_UnlockLumpNum(gtlump);
}


//---------------------------------------------------------------------------
// V_DestroyTrueColorPalette
//---------------------------------------------------------------------------
void V_DestroyTrueColorPalette(void)
{
    if (Palettes16) free(Palettes16);
    Palettes16 = NULL;
    V_Palette16 = NULL;
    if (PalettesTC) free(PalettesTC);
    PalettesTC = NULL;
    V_PaletteTC = NULL;
}

//
// V_SetRawPalette
//
// Point V_Palette16 at a 16-bit palette built from an arbitrary 256-colour
// (768-byte) palette lump, e.g. Heretic's E2PAL for the episode-2 finale.
// Built into a dedicated static buffer so the normal Palettes16 cache is
// untouched; call V_RestorePalette to revert. The build mirrors
// V_UpdateTrueColorPalette's per-colour, per-weight expansion.
//
static uint16_t *RawPalette16 = NULL;

void V_SetRawPalette(const char *lump_name)
{
  int            i, w;
  int            lump = W_CheckNumForName(lump_name);
  int            gtlump;
  const uint8_t *pal;
  const uint8_t *gtable;

  if (lump < 0 || W_LumpLength(lump) < 768)
    return;

  if (!RawPalette16)
  {
    RawPalette16 = (uint16_t*)malloc(256 * sizeof(uint16_t) * VID_NUMCOLORWEIGHTS);
    if (!RawPalette16)
      return;
  }
  if (VID_TRUECOLOR && !RawPaletteTC)
  {
    RawPaletteTC = (uint32_t*)malloc(256 * sizeof(uint32_t) * VID_NUMCOLORWEIGHTS);
    if (!RawPaletteTC)
      return;
  }

  gtlump = (W_CheckNumForName)("GAMMATBL", ns_prboom);
  pal    = (const uint8_t *)W_CacheLumpNum(lump);
  gtable = ((gtlump == -1) ? gammatable
                           : (const uint8_t *)W_CacheLumpNum(gtlump))
           + (256 * usegamma);

  for (i = 0; i < 256; i++)
  {
    uint8_t r = gtable[pal[i*3+0]];
    uint8_t g = gtable[pal[i*3+1]];
    uint8_t b = gtable[pal[i*3+2]];
    float roundUpR = (r > DONT_ROUND_ABOVE) ? 0 : 0.5f;
    float roundUpG = (g > DONT_ROUND_ABOVE) ? 0 : 0.5f;
    float roundUpB = (b > DONT_ROUND_ABOVE) ? 0 : 0.5f;

    for (w = 0; w < VID_NUMCOLORWEIGHTS; w++)
    {
      float t = (float)(w)/(float)(VID_NUMCOLORWEIGHTS-1);
      if (RawPaletteTC)
        RawPaletteTC[i*VID_NUMCOLORWEIGHTS + w] =
          V_PackTC(r, g, b, t, roundUpR, roundUpG, roundUpB);
#if defined(ABGR1555)
      int nr = (int)((r>>3)*t+roundUpR);
      int ng = (int)((g>>3)*t+roundUpG);
      int nb = (int)((b>>3)*t+roundUpB);
      RawPalette16[i*VID_NUMCOLORWEIGHTS + w] = (uint16_t)((nb<<10)|(ng<<5)|nr);
#else
      int nr = (int)((r>>3)*t+roundUpR);
      int ng = (int)((g>>2)*t+roundUpG);
      int nb = (int)((b>>3)*t+roundUpB);
      RawPalette16[i*VID_NUMCOLORWEIGHTS + w] = (uint16_t)((nr<<11)|(ng<<5)|nb);
#endif
    }
  }

  if (gtlump != -1)
    W_UnlockLumpNum(gtlump);
  W_UnlockLumpNum(lump);

  V_Palette16 = RawPalette16;
  if (RawPaletteTC)
    V_PaletteTC = RawPaletteTC;
}

/* V_SetPaletteBlend: point V_Palette16 at a scratch palette mixing two
 * PLAYPAL palettes, for frame-rate pain/bonus fades (movement_smooth).
 * The blend is computed on the gamma-corrected components, mirroring
 * V_UpdateTrueColorPalette.  Two scratch blocks ping-pong so the
 * V_Palette16 pointer changes whenever the contents do -- the flat and
 * fullscreen draw caches key on that pointer. */
static uint16_t *BlendPal16[2];
static int blend_flip;

void V_SetPaletteBlend(int pal1, int pal2, fixed_t t)
{
  static int last_pal1 = -1, last_pal2 = -1, last_gamma = -1;
  static fixed_t last_t = -1;
  int pplump;
  int gtlump;
  const uint8_t *pal;
  const uint8_t *gtable;
  uint16_t *dst;
  int i, w;

  if (!Palettes16) /* palette machinery not up yet */
  {
    V_SetPalette(pal1);
    return;
  }
  if (pal1 == last_pal1 && pal2 == last_pal2 && t == last_t &&
      usegamma == last_gamma && BlendPal16[blend_flip])
  {
    V_Palette16 = BlendPal16[blend_flip];
    if (BlendPalTC[blend_flip])
      V_PaletteTC = BlendPalTC[blend_flip];
    return;
  }
  last_pal1 = pal1; last_pal2 = pal2; last_t = t; last_gamma = usegamma;

  pplump = W_GetNumForName("PLAYPAL");
  gtlump = (W_CheckNumForName)("GAMMATBL", ns_prboom);
  pal    = W_CacheLumpNum(pplump);
  gtable = ((gtlump == -1) ? gammatable
                           : (const uint8_t *) W_CacheLumpNum(gtlump))
           + (256 * usegamma);

  {
    int numPals = W_LumpLength(pplump) / (3*256);

  blend_flip ^= 1;
  if (!BlendPal16[blend_flip])
    BlendPal16[blend_flip] =
      malloc(256 * sizeof(uint16_t) * VID_NUMCOLORWEIGHTS);
  if (VID_TRUECOLOR && !BlendPalTC[blend_flip])
    BlendPalTC[blend_flip] =
      malloc(256 * sizeof(uint32_t) * VID_NUMCOLORWEIGHTS);
  dst = BlendPal16[blend_flip];

  for (i = 0; i < 256; i++)
  {
    uint8_t s1[3], s2[3];
    uint8_t r1, g1, b1, r2, g2, b2;
    uint8_t r, g, b;
    float roundUpR, roundUpG, roundUpB;
    /* a flash palette the lump does not carry is synthesised, matching the
     * full-palette build path, so blending never reads past the lump */
    if (pal1 < numPals) { s1[0]=pal[(256*pal1+i)*3+0]; s1[1]=pal[(256*pal1+i)*3+1]; s1[2]=pal[(256*pal1+i)*3+2]; }
    else V_SynthPaletteColor(pal, pal1, i, s1);
    if (pal2 < numPals) { s2[0]=pal[(256*pal2+i)*3+0]; s2[1]=pal[(256*pal2+i)*3+1]; s2[2]=pal[(256*pal2+i)*3+2]; }
    else V_SynthPaletteColor(pal, pal2, i, s2);
    r1 = gtable[s1[0]]; g1 = gtable[s1[1]]; b1 = gtable[s1[2]];
    r2 = gtable[s2[0]]; g2 = gtable[s2[1]]; b2 = gtable[s2[2]];
    r = (uint8_t) ((r1 * (FRACUNIT - t) + r2 * t) >> FRACBITS);
    g = (uint8_t) ((g1 * (FRACUNIT - t) + g2 * t) >> FRACBITS);
    b = (uint8_t) ((b1 * (FRACUNIT - t) + b2 * t) >> FRACBITS);
    roundUpR = (r > DONT_ROUND_ABOVE) ? 0 : 0.5f;
    roundUpG = (g > DONT_ROUND_ABOVE) ? 0 : 0.5f;
    roundUpB = (b > DONT_ROUND_ABOVE) ? 0 : 0.5f;

    for (w = 0; w < VID_NUMCOLORWEIGHTS; w++)
    {
      float wt = (float) (w) / (float) (VID_NUMCOLORWEIGHTS - 1);
      if (BlendPalTC[blend_flip])
        BlendPalTC[blend_flip][i * VID_NUMCOLORWEIGHTS + w] =
          V_PackTC(r, g, b, wt, roundUpR, roundUpG, roundUpB);
#if defined(ABGR1555)
      int nr = (int) ((r >> 3) * wt + roundUpR);
      int ng = (int) ((g >> 3) * wt + roundUpG);
      int nb = (int) ((b >> 3) * wt + roundUpB);
      dst[i * VID_NUMCOLORWEIGHTS + w] = (uint16_t) ((nb << 10) | (ng << 5) | nr);
#else
      int nr = (int) ((r >> 3) * wt + roundUpR);
      int ng = (int) ((g >> 2) * wt + roundUpG);
      int nb = (int) ((b >> 3) * wt + roundUpB);
      dst[i * VID_NUMCOLORWEIGHTS + w] = (uint16_t) ((nr << 11) | (ng << 5) | nb);
#endif
    }
  }
  W_UnlockLumpNum(pplump);
  if (gtlump != -1)
    W_UnlockLumpNum(gtlump);
  V_Palette16 = BlendPal16[blend_flip];
  if (BlendPalTC[blend_flip])
    V_PaletteTC = BlendPalTC[blend_flip];
  }
}

//
// V_RestorePalette
//
// Revert V_Palette16 to the standard PLAYPAL-derived palette after a
// V_SetRawPalette swap.
//
void V_RestorePalette(void)
{
  if (Palettes16)
    V_Palette16 = Palettes16 + currentPaletteIndex*256*VID_NUMCOLORWEIGHTS;
  if (PalettesTC)
    V_PaletteTC = PalettesTC + currentPaletteIndex*256*VID_NUMCOLORWEIGHTS;
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
//
// memset() interprets its second argument as unsigned char, which
// silently truncates the 16-bit pixel value down to its low byte
// and then replicates that byte across both bytes of every pixel.
// Any pixel value whose high and low bytes differ comes out wrong
// (for example, palette index 0 at weight 63 = 0x10a2 becomes
// 0xa2a2, a pinkish red instead of dark gray-green).  Use a
// per-pixel store loop instead -- correct for every value, and
// V_FillRect isn't on a hot path so the lost vectorisation
// doesn't matter in practice.
void V_FillRect(int x, int y, int width, int height, uint8_t colour)
{
  int i;
  if (VID_TRUECOLOR)
  {
    /* Palette index -> V_PaletteTC directly: full channel width, no 565
     * intermediate.  V_DrawRawScreen paints whole raw images through this,
     * so it is a real image path, not just a background fill. */
    uint32_t *dest = (uint32_t*)screens[0].data + x + y * SURFACE_SHORT_PITCH;
    uint32_t  c    = VID_PALTC(colour, VID_COLORWEIGHTMASK);
    while (height--)
    {
      for (i = 0; i < width; i++)
        dest[i] = c;
      dest += SURFACE_SHORT_PITCH;
    }
    return;
  }
  {
    uint16_t *dest = (uint16_t*)screens[0].data + x + y * SURFACE_SHORT_PITCH;
    uint16_t  c    = VID_PAL16(colour, VID_COLORWEIGHTMASK);
    while (height--)
    {
      for (i = 0; i < width; i++)
        dest[i] = c;
      dest += SURFACE_SHORT_PITCH;
    }
  }
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
   if (VID_TRUECOLOR)
   {
      ((uint32_t*)screens[scrn].data)[x + SURFACE_SHORT_PITCH *y] = VID_PALTC(color, VID_COLORWEIGHTMASK);
      return;
   }
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

// V_DrawBox()
//
// Draw a box in the frame buffer, stretched to current resolution.
//
// Pass the coordinates of diagonal line of the box, and the color to be drawn
// Returns nothing
//
void V_DrawBox(fline_t* fl, int color)
{
  int x, y, ax, ay, bx, by;

  if (fl->a.x > fl->b.x) {
    ax = fl->b.x;
    bx = fl->a.x;
  } else {
    ax = fl->a.x;
    bx = fl->b.x;
  }
  if (fl->a.y > fl->b.y) {
    ay = fl->b.y;
    by = fl->a.y;
  } else {
    ay = fl->a.y;
    by = fl->b.y;
  }
  ax = ax * SCREENWIDTH / 320;
  bx = bx * SCREENWIDTH / 320;
  ay = ay * SCREENHEIGHT / 200;
  by = by * SCREENHEIGHT / 200;

  for (x = ax; x < bx; x++)
    for (y = ay; y < by; y++)
      V_PlotPixel(0, x,y, (uint8_t)color);
}
