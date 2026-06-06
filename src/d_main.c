/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2004 by
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
 *  DOOM main program (D_DoomMain),
 *  plus functions to determine game mode (shareware, registered),
 *  parse command line parameters, configure game parameters (turbo),
 *  and call the startup functions.
 *
 *-----------------------------------------------------------------------------
 */


#ifdef _MSC_VER
#define    F_OK    0    /* Check for file existence */
#define    W_OK    2    /* Check for write permission */
#define    R_OK    4    /* Check for read permission */
#include <io.h>
#include <direct.h>
#else
#include <unistd.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <file/file_path.h>
#include <streams/file_stream.h>

#include "config.h"
#include "doomdef.h"
#include "doomtype.h"
#include "doomstat.h"
#include "hexen/sn_sonix.h"
#include "d_net.h"
#include "dstrings.h"
#include "sounds.h"
#include "z_zone.h"
#include "w_wad.h"
#include "prboom_wad_data.h"
#include "s_sound.h"
#include "v_video.h"
#include "f_finale.h"
#include "f_wipe.h"
#include "m_argv.h"
#include "m_misc.h"
#include "m_menu.h"
#include "i_main.h"
#include "i_system.h"
#include "i_sound.h"
#include "i_video.h"
#include "g_game.h"
#include "hu_stuff.h"
#include "wi_stuff.h"
#include "st_stuff.h"
#include "am_map.h"
#include "p_setup.h"
#include "r_draw.h"
#include "r_main.h"
#include "hexen/p_mapinfo.h"
#include "r_fps.h"
#include "d_main.h"
#include "d_deh.h"  // Ty 04/08/98 - Externalizations
#include "dsda_hacked.h"
#include "lprintf.h"  // jff 08/03/98 - declaration of lprintf
#include "am_map.h"
#include "u_mapinfo.h"
#include "u_decorate.h"
#include "u_zmapinfo.h"

void GetFirstMap(int *ep, int *map); // Ty 08/29/98 - add "-warp x" functionality
static void D_PageDrawer(void);

/* Don't include file_stream_transforms.h but instead
just forward declare the prototypes */
int64_t rfread(void* buffer,
   size_t elem_size, size_t elem_count, RFILE* stream);
int rfscanf(RFILE * stream, const char * format, ...);

// CPhipps - removed wadfiles[] stuff

// jff 1/24/98 add new versions of these variables to remember command line
dbool clnomonsters;   // checkparm of -nomonsters
dbool clrespawnparm;  // checkparm of -respawn
dbool clfastparm;     // checkparm of -fast
// jff 1/24/98 end definition of command line version of play mode switches

dbool nomonsters;     // working -nomonsters
dbool respawnparm;    // working -respawn
dbool fastparm;       // working -fast

//jff 1/22/98 parms for disabling music and sound
dbool nosfxparm;
dbool nomusicparm;

//jff 4/18/98
extern dbool inhelpscreens;

skill_t startskill;
int     startepisode;
int     startmap;
dbool autostart;
int ffmap;

dbool advancedemo;
dbool singledemo;

char    wadfile[PATH_MAX+1];       // primary wad file
char    mapdir[PATH_MAX+1];        // directory of development maps
char    baseiwad[PATH_MAX+1];      // jff 3/23/98: iwad directory
char    basesavegame[PATH_MAX+1];  // killough 2/16/98: savegame directory

//jff 4/19/98 list of standard IWAD names
const char *const standard_iwads[]=
{
  "doom2f.wad",
  "doom2.wad",
  "plutonia.wad",
  "tnt.wad",
  "freedoom2.wad",
  "doom.wad",
  "doomu.wad", /* CPhipps - alow doomu.wad */
  "freedoom1.wad",
  "freedoom.wad",
  "doom1.wad",
};
static const int nstandard_iwads = sizeof standard_iwads/sizeof*standard_iwads;

/*
 * D_PostEvent - Event handling
 *
 * Called by I/O functions when an event is received.
 * Try event handlers for each code area in turn.
 * cph - in the TRUE spirit of the Boom source, let the
 *  short ciruit operator madness begin!
 */

void D_PostEvent(event_t *ev)
{
  /* cph - suppress all input events at game start
   * FIXME: This is a lousy kludge */
  if (gametic < 3) return;
  (void)(
    M_Responder(ev) ||
      (gamestate == GS_LEVEL && (
        HU_Responder(ev) ||
        ST_Responder(ev) ||
        AM_Responder(ev)
      )) ||
    G_Responder(ev));
}

//
// D_Wipe
//
// CPhipps - moved the screen wipe code from D_Display to here
// The screens to wipe between are already stored, this just does the timing
// and screen updating

// Make D_Wipe reentrant. We cannot stick around for an indefinite amount of time in this loop. (Themaister).
static dbool in_d_wipe;

static void D_Wipe(void)
{
   in_d_wipe = !wipe_ScreenWipe(1);
   M_Drawer();                   // menu is drawn even on top of wipes
   I_FinishUpdate();             // page flip or blit buffer
}

//
// D_Display
//  draw current display, possibly wiping it from the previous
//

// wipegamestate can be set to -1 to force a wipe on the next draw
gamestate_t    wipegamestate = GS_DEMOSCREEN;
extern dbool setsizeneeded;
extern int     showMessages;

void D_Display (void)
{
  dbool wipe, viewactive;
  static gamestate_t oldgamestate = -1;

  /* Wipe re-entry path: while a wipe is in progress, every
   * subsequent retro_run goes here so the wipe blend can advance
   * one step per display frame without the rest of the draw
   * pipeline running.  Bracket it with I_StartDisplay /
   * I_EndDisplay so the direct-framebuffer acquisition happens
   * (otherwise the blender would write to a stale fb pointer
   * left over from before the wipe).  wipe_ScreenWipe reads the
   * fresh screens[0].data we just bound and writes blended
   * pixels into the current frontend buffer. */
  if (in_d_wipe)
  {
     if (!I_StartDisplay())
        return;
     D_Wipe();
     I_EndDisplay();
     return;
  }

  /* Issue #183: detect the wipe transition and capture the start
   * screen BEFORE I_StartDisplay rebinds screens[0] to a fresh
   * frontend buffer.  Under libretro direct-render, every
   * I_StartDisplay swaps screens[0].data to whatever buffer the
   * frontend hands us via GET_CURRENT_SOFTWARE_FRAMEBUFFER -- that
   * buffer has never been written by us, so wipe_StartScreen run
   * after I_StartDisplay would capture stale / uninitialised
   * pixels into wipe_scr_start.  The wipe then animates from that
   * garbage downward instead of from the previous frame, and the
   * "old screen sliding off the bottom" portion of the melt shows
   * column-wise garbage right up until the last few wipe ticks.
   *
   * Running wipe_StartScreen here, between the previous frame's
   * I_FinishUpdate (which restored screens[0].data to the
   * persistent screen_buf and -- as part of the same #183 fix --
   * snapshotted the just-presented frame into screen_buf) and this
   * frame's I_StartDisplay, means screens[0] still points at
   * screen_buf with the previous frame's pixels in it.  Capturing
   * from there yields the correct wipe-start content.
   *
   * The fallback (non-direct-render) path is unaffected: there
   * screens[0].data is always screen_buf, and screen_buf is the
   * actual render target across frames, so both before-
   * I_StartDisplay and after-I_StartDisplay readings return the
   * same content. */
  if ((wipe = gamestate != wipegamestate))
    wipe_StartScreen();

  if (!I_StartDisplay())
    return;

  if (gamestate != GS_LEVEL) { // Not a level
    switch (oldgamestate) {
    case GS_UNDEFINED:
    case GS_LEVEL:
      V_SetPalette(0); // cph - use default (basic) palette
    default:
      break;
    }

    switch (gamestate) {
    case GS_INTERMISSION:
      WI_Drawer();
      break;
    case GS_FINALE:
      F_Drawer();
      break;
    case GS_DEMOSCREEN:
      D_PageDrawer();
      break;
    default:
      break;
    }
  } else if (gametic != basetic) { // In a level
    HU_Erase();

    if (setsizeneeded) {               // change the view size if needed
      R_ExecuteSetViewSize();
      oldgamestate = -1;            // force background redraw
    }

    // Work out if the player view is visible
    viewactive = (!(automapmode & am_active) || (automapmode & am_overlay)) && !inhelpscreens;

    /* Note: the upstream redrawborderstuff / borderwillneedredraw
     * state machine that gates partial vs full status-bar redraws
     * has been replaced by an unconditional refresh below.  Under
     * libretro direct-render the frontend rotates framebuffers
     * each frame, so cross-frame partial-redraw optimisations
     * write to a buffer that doesn't get displayed -- forcing
     * a full BG -> FG status-bar copy each frame is the simplest
     * correct alternative, and the cost (~20 KB at 320x200) is
     * a small fraction of the full-screen memcpy we avoid by
     * direct-rendering. */

    // Now do the drawing
    /* A failed or incomplete level load (e.g. a UDMF map whose node format
     * we cannot decode) can leave the player with no mobj.  R_SetupFrame
     * dereferences player->mo immediately, so guard the 3D view here rather
     * than crash; the frame just renders empty. */
    if (viewactive && players[displayplayer].mo)
      R_RenderPlayerView (&players[displayplayer]);
    if (automapmode & am_active)
      AM_Drawer();
    ST_Drawer(
        ((viewheight != SCREENHEIGHT)
         || ((automapmode & am_active) && !(automapmode & am_overlay))),
        TRUE,
        (menuactive == mnact_full));
    HU_Drawer();
  }

  oldgamestate = wipegamestate = gamestate;

  // draw pause pic
  if (paused && (menuactive != mnact_full)) {
    // Simplified the "logic" here and no need for x-coord caching - POPE
    {
      /* Raven names its pause graphic PAUSED; M_PAUSE is Doom's.  Looking
       * up the wrong one crashes the patch cache. */
      const char *pause_patch = raven ? "PAUSED" : "M_PAUSE";
      V_DrawNamePatch((320 - V_NamePatchWidth(pause_patch))/2, 4,
                      0, pause_patch, CR_DEFAULT, VPT_STRETCH);
    }
  }

  // menus go directly to the screen
  M_Drawer();          // menu is drawn even on top of everything

  /* Input collection lives entirely in TryRunTics, which runs
   * once per retro_run.  The upstream code polled input again
   * here -- at display rates higher than the 35 Hz gametic rate
   * this fed every poll-result into the SAME outstanding ticcmd
   * via the angleturn += / buttons |= accumulation in
   * D_BuildNewTiccmds's "else" branch, doubling any turn input
   * across consecutive frames and making the same physical mouse
   * motion produce different in-game turns at different display
   * fps.  TryRunTics is the single source of truth. */

  // normal update
  if (!wipe)
    I_FinishUpdate ();              // page flip or blit buffer
  else {
    // wipe update
    wipe_EndScreen();
    D_Wipe();
  }

  I_EndDisplay();
}

int has_exited;

/* I_SafeExit
 * This function is called instead of exit() by functions that might be called
 * during the exit process (i.e. after exit() has already been called)
 * Prevent infinitely recursive exits -- killough
 */

void I_SafeExit(int rc)
{
  if (!has_exited)    /* If it hasn't exited yet, exit now -- killough */
  {
      has_exited=rc ? 2 : 1;
  }
}

//
//  DEMO LOOP
//

static int  demosequence;         // killough 5/2/98: made static
static int  pagetic;
static const char *pagename; // CPhipps - const
dbool bfgedition = 0;

//
// D_PageTicker
// Handles timing for warped projection
//
void D_PageTicker(void)
{
  if (--pagetic < 0)
    D_AdvanceDemo();
}

//
// D_PageDrawer
//
// The page/title screen is a single full-screen patch stretched to the
// current resolution.  At high internal resolutions (e.g. 2560x1200) that
// stretch runs the per-pixel column drawer over millions of pixels, and
// D_Display calls this every frame -- so a static, unchanging image was
// being fully re-rasterised 120 times a second (the title screen measured
// ~10 ms/frame at 4K while live gameplay is ~2 ms).
//
// Since the result depends only on the page lump, the screen dimensions
// and the palette, rasterise it once into a persistent buffer and memcpy
// that into the frame buffer on subsequent frames.  A full-screen memcpy
// of SCREENWIDTH*SCREENHEIGHT*2 bytes is a cheap streaming copy compared
// with the stretched-patch redraw.  The cache is rebuilt whenever the
// page, the resolution or the palette (V_Palette16) changes.  The
// M_DrawCredits() branch (pagename == NULL) is not cached.
//
static uint16_t   *page_cache       = NULL;
static const char *page_cache_name  = NULL;
static int         page_cache_w     = -1;
static int         page_cache_h     = -1;
static const uint16_t *page_cache_pal = NULL;

void D_FreePageCache(void)
{
  free(page_cache);
  page_cache      = NULL;
  page_cache_name = NULL;
  page_cache_w    = -1;
  page_cache_h    = -1;
  page_cache_pal  = NULL;
}

static void D_PageDrawer(void)
{
  // proff/nicolas 09/14/98 -- now stretchs bitmaps to fullscreen!
  // CPhipps - updated for new patch drawing
  // proff - added M_DrawCredits

  /* Heretic and Hexen's full-screen lumps (TITLE, CREDIT, HELP1, HELP2)
   * are raw 320x200 bitmaps, not Doom patch_t graphics, so they render
   * through the raw-screen blit rather than the patch decoder.  They are
   * otherwise just as static and just as expensive to re-stretch every
   * frame, so they share the same page cache; only the render-on-miss call
   * differs. */
  if (pagename)
  {
    const size_t fb_bytes = (size_t)SCREENWIDTH * SCREENHEIGHT
                            * SURFACE_PIXEL_DEPTH;

    if (!page_cache
        || page_cache_name != pagename
        || page_cache_w    != SCREENWIDTH
        || page_cache_h    != SCREENHEIGHT
        || page_cache_pal  != V_Palette16)
    {
      /* (Re)build the cache.  Render the stretched image once into the
       * persistent page_cache by temporarily pointing screens[0] and the
       * renderer's cached top-left at it, then restore.  The frontend
       * buffer always has SCREENWIDTH pitch when direct-rendering (the
       * direct path is gated on pitch == SCREENPITCH), so the cache is
       * layout-compatible with both the direct and fallback buffers. */
      unsigned char  *saved_data       = screens[0].data;
      uint16_t       *saved_short_tl    = drawvars.short_topleft;
      unsigned int   *saved_int_tl      = drawvars.int_topleft;

      if (!page_cache)
        page_cache = (uint16_t*)malloc((size_t)MAX_SCREENWIDTH
                                       * MAX_SCREENHEIGHT
                                       * SURFACE_PIXEL_DEPTH);

      if (page_cache)
      {
        screens[0].data        = (unsigned char *)page_cache;
        drawvars.short_topleft = page_cache;
        drawvars.int_topleft   = (unsigned int *)page_cache;

        if (raven)
          V_DrawRawScreen(pagename);
        else
          V_DrawNamePatchFS(0, 0, 0, pagename, CR_DEFAULT, VPT_STRETCH);

        screens[0].data        = saved_data;
        drawvars.short_topleft = saved_short_tl;
        drawvars.int_topleft   = saved_int_tl;

        page_cache_name = pagename;
        page_cache_w    = SCREENWIDTH;
        page_cache_h    = SCREENHEIGHT;
        page_cache_pal  = V_Palette16;
      }
      else
      {
        /* Allocation failed -- fall back to drawing directly every
         * frame (correct, just not accelerated). */
        if (raven)
          V_DrawRawScreen(pagename);
        else
          V_DrawNamePatchFS(0, 0, 0, pagename, CR_DEFAULT, VPT_STRETCH);
        return;
      }
    }

    memcpy(screens[0].data, page_cache, fb_bytes);
  }
  else if (!heretic)
    M_DrawCredits();
}

//
// D_AdvanceDemo
// Called after each demo or intro demosequence finishes
//
void D_AdvanceDemo (void)
{
  if(!singledemo)
    advancedemo = TRUE;
}

/* killough 11/98: functions to perform demo sequences
 * cphipps 10/99: constness fixes
 */

static void D_SetPageName(const char *name)
{
  /* Heretic and Hexen's full-screen title lump is named TITLE, not Doom's
   * TITLEPIC; the rest of the demo-sequence page names (HELP1/HELP2/CREDIT)
   * match.  Map the one that differs so the boot title does not abort on a
   * missing lump. */
  if (raven && name && !strcmp(name, "TITLEPIC"))
    name = "TITLE";
  pagename = name;
}

static void D_DrawTitle1(const char *name)
{
  /* mus_intro maps to the Heretic title lump (MUS_TITL) under Heretic and to
   * Doom's title music otherwise; see the Heretic remap in S_ChangeMusic. */
  S_StartMusic(mus_intro);
  pagetic = (TICRATE*170)/35;
  if (W_CheckNumForName("SIGILINT") != -1) // Sigil: Longer wait before playing a demo to give the title theme time to end.
    pagetic = (TICRATE*404)/35;
  D_SetPageName(name);
}

static void D_DrawTitle2(const char *name)
{
  if (raven)
    S_StartMusic(mus_intro);    /* raven title theme (per-game remap) */
  else
    S_StartMusic(mus_dm2ttl);
  D_SetPageName(name);
}

/* killough 11/98: tabulate demo sequences
 */

static struct
{
  void (*func)(const char *);
  const char *name;
} const demostates[][4] =
  {
    {
      {D_DrawTitle1, "TITLEPIC"},
      {D_DrawTitle1, "TITLEPIC"},
      {D_DrawTitle2, "TITLEPIC"},
      {D_DrawTitle1, "TITLEPIC"},
    },

    {
      {G_DeferedPlayDemo, "demo1"},
      {G_DeferedPlayDemo, "demo1"},
      {G_DeferedPlayDemo, "demo1"},
      {G_DeferedPlayDemo, "demo1"},
    },
    {
      {D_SetPageName, NULL},
      {D_SetPageName, NULL},
      {D_SetPageName, NULL},
      {D_SetPageName, NULL},
    },

    {
      {G_DeferedPlayDemo, "demo2"},
      {G_DeferedPlayDemo, "demo2"},
      {G_DeferedPlayDemo, "demo2"},
      {G_DeferedPlayDemo, "demo2"},
    },

    {
      {D_SetPageName, "HELP2"},
      {D_SetPageName, "HELP2"},
      {D_SetPageName, "CREDIT"},
      {D_DrawTitle1,  "TITLEPIC"},
    },

    {
      {G_DeferedPlayDemo, "demo3"},
      {G_DeferedPlayDemo, "demo3"},
      {G_DeferedPlayDemo, "demo3"},
      {G_DeferedPlayDemo, "demo3"},
    },

    {
      {NULL},
      {NULL},
      {NULL},
      {D_SetPageName, "CREDIT"},
    },

    {
      {NULL},
      {NULL},
      {NULL},
      {G_DeferedPlayDemo, "demo4"},
    },

    {
      {NULL},
      {NULL},
      {NULL},
      {NULL},
    }
  };

/*
 * This cycles through the demo sequences.
 * killough 11/98: made table-driven
 */

void D_DoAdvanceDemo(void)
{
  players[consoleplayer].playerstate = PST_LIVE;  /* not reborn */
  advancedemo = usergame = paused = FALSE;
  gameaction = ga_nothing;

  pagetic = TICRATE * 11;         /* killough 11/98: default behavior */
  gamestate = GS_DEMOSCREEN;


  if (!demostates[++demosequence][gamemode].func)
    demosequence = 0;
  demostates[demosequence][gamemode].func
    (demostates[demosequence][gamemode].name);
}

//
// D_StartTitle
//
void D_StartTitle (void)
{
  gameaction = ga_nothing;
  demosequence = -1;
  D_AdvanceDemo();
}

//
// D_AddFile
//
// Rewritten by Lee Killough
//
// Ty 08/29/98 - add source parm to indicate where this came from
// CPhipps - static, const char* parameter
//         - source is an enum
//         - modified to allocate & use new wadfiles array
void D_AddFile (const char *file, wad_source_t source)
{
  size_t gwa_filename_len;
  size_t file_len         = strlen(file);
  char *gwa_filename      = NULL;

  wadfiles = realloc(wadfiles, sizeof(*wadfiles)*(numwadfiles+1));
  /* Zero the fresh slot: realloc leaves it uninitialised, and a non-NULL
   * embedded_data here would make W_AddFile wrongly treat this file WAD as
   * a baked-in one and dereference garbage.  memset also covers handle/data
   * and guards against future struct fields. */
  memset(&wadfiles[numwadfiles], 0, sizeof(wadfiles[numwadfiles]));
  wadfiles[numwadfiles].name =
    AddDefaultExtension(strcpy(malloc(file_len + 5), file), ".wad");
  wadfiles[numwadfiles].src = source; // Ty 08/29/98
  numwadfiles++;
  // proff: automatically try to add the gwa files
  // proff - moved from w_wad.c
  gwa_filename     = AddDefaultExtension(strcpy(malloc(file_len + 5), file), ".wad");
  gwa_filename_len = strlen(gwa_filename);

  if (gwa_filename_len > 4
      && !strcasecmp(gwa_filename+(gwa_filename_len - 4),".wad"))
  {
    char *ext = &gwa_filename[gwa_filename_len - 4];
    ext[1]    = 'g'; ext[2] = 'w'; ext[3] = 'a';
    wadfiles  = realloc(wadfiles, sizeof(*wadfiles)*(numwadfiles+1));
    memset(&wadfiles[numwadfiles], 0, sizeof(wadfiles[numwadfiles]));
    wadfiles[numwadfiles].name = gwa_filename;
    wadfiles[numwadfiles].src = source; // Ty 08/29/98
    numwadfiles++;
  }
  else
  {
    /* Not a .wad path -- the gwa_filename buffer we allocated above
     * is unused.  Without this free, every D_AddFile of a non-.wad
     * file (e.g. .deh, .bex, .lmp) leaked one ~PATH_MAX-byte malloc
     * per call. */
    free(gwa_filename);
  }
}

// killough 10/98: support -dehout filename
// cph - made const, don't cache results
static const char *D_dehout(void)
{
  int p = M_CheckParm("-dehout");
  if (!p)
    p = M_CheckParm("-bexout");
  return (p && ++p < myargc ? myargv[p] : NULL);
}

//
// CheckIWAD
//
// Verify a file is indeed tagged as an IWAD
// Scan its lumps for levelnames and return gamemode as indicated
// Detect missing wolf levels in DOOM II
//
// The filename to check is passed in iwadname, the gamemode detected is
// returned in gmode, hassec returns the presence of secret levels
//
// jff 4/19/98 Add routine to test IWAD for validity and determine
// the gamemode from it. Also note if DOOM II, whether secret levels exist
// CPhipps - const char* for iwadname, made static
static bool CheckIWAD(const char *iwadname,GameMode_t *gmode,dbool *hassec)
{
  RFILE *fp     = NULL;

  if ((fp = filestream_open(iwadname, 
		  RETRO_VFS_FILE_ACCESS_READ,
		  RETRO_VFS_FILE_ACCESS_HINT_NONE
		  )))
  {
    int ud=0,rg=0,sw=0,cm=0,sc=0,htic=0,hexn=0;

    // Identify IWAD correctly
    wadinfo_t header;

    // read IWAD header
    /* Accept either "IWAD" or "PWAD" identification.  The libretro
     * frontend's retro_load_game promotes PWAD-magic files that
     * carry a PLAYPAL lump (a complete standalone game like
     * chex.wad) to -iwad routing; this is where that routing lands.
     * The rest of the CheckIWAD logic -- scanning the lump table for
     * level markers -- is identical for either magic, so just widen
     * the accepted header strings here. */
    if (rfread(&header, sizeof(header), 1, fp) == 1 &&
        (!strncmp(header.identification, "IWAD", 4) ||
         !strncmp(header.identification, "PWAD", 4)))
    {
      int64_t length;
      filelump_t *fileinfo;

      // read IWAD directory
      header.numlumps = LONG(header.numlumps);
      header.infotableofs = LONG(header.infotableofs);
      length = header.numlumps;
      fileinfo = malloc(length*sizeof(filelump_t));
      if (filestream_seek (fp, header.infotableofs, SEEK_SET) ||
        rfread (fileinfo, sizeof(filelump_t), length, fp) != length)
      I_Error("CheckIWAD: failed to read directory %s",iwadname);

      // scan directory for levelname lumps
      while (length--)
        if (fileinfo[length].name[0] == 'E' &&
              fileinfo[length].name[2] == 'M' &&
              fileinfo[length].name[4] == 0)
        {
          if (fileinfo[length].name[1] == '4')
            ++ud;
          else if (fileinfo[length].name[1] == '3')
            ++rg;
          else if (fileinfo[length].name[1] == '2')
            ++rg;
          else if (fileinfo[length].name[1] == '1')
            ++sw;
        }
        else if (fileinfo[length].name[0] == 'M' &&
                    fileinfo[length].name[1] == 'A' &&
                    fileinfo[length].name[2] == 'P' &&
                    fileinfo[length].name[5] == 0)
        {
          ++cm;
          if (fileinfo[length].name[3] == '3')
              if (fileinfo[length].name[4] == '1' ||
                  fileinfo[length].name[4] == '2')
                ++sc;
        }
        else if (!strncmp(fileinfo[length].name, "M_HTIC", 6))
        {
          /* M_HTIC is the Heretic main-menu title graphic; it is unique to
           * a Heretic IWAD (Doom never carries it).  This distinguishes a
           * Heretic IWAD from Doom even though both use ExMy level names.
           * NOTE: Hexen IWADs also carry M_HTIC, so Hexen must be tested
           * before Heretic below (see the hexn check). */
          ++htic;
        }
        else if (!strncmp(fileinfo[length].name, "MAPINFO", 7) &&
                 fileinfo[length].name[7] == 0)
        {
          /* MAPINFO is unique to a Hexen IWAD among the games this core
           * supports (Doom/Doom2 and Heretic carry no such lump), so it is
           * the unambiguous Hexen signature. */
          ++hexn;
        }

      free(fileinfo);
      }
      else // missing IWAD tag in header
      {
        filestream_close(fp);
        return I_Error("CheckIWAD: IWAD tag %s not present", iwadname);
      }

      filestream_close(fp);

    // Determine game mode from levels present
    // Must be a full set for whichever mode is present
    // Lack of wolf-3d levels also detected here

    *gmode = indetermined;
    *hassec = FALSE;
    if (hexn)
    {
      /* Hexen IWAD.  Hexen uses MAP## levels like commercial Doom and shares
       * a great deal of code with Heretic, so set both the hexen flag and the
       * raven umbrella (raven == heretic || hexen).  The full game has 30+
       * maps; the 4-map demo IWAD also carries MAPINFO, so treat anything
       * with the Hexen signature as commercial and let the map count stand. */
      extern dbool hexen;
      extern dbool raven;
      hexen       = true;
      raven       = true;
      gamemission = hexen_mission;
      *gmode      = commercial;
    }
    else if (htic)
    {
      /* Heretic IWAD.  Episodes use the same ExMy markers as Doom, so the
       * sw/rg counts above already tallied them: shareware is E1 only,
       * the registered/extended set has E1..E3 (and the SoSR E4..E5).
       * Map the Doom gamemode slots onto Heretic's episode layout. */
      extern dbool heretic;
      extern dbool raven;
      heretic = true;
      raven   = true;
      gamemission = heretic_mission;
      if (rg >= 18 || ud >= 9)
        *gmode = registered;   /* full Heretic (3+ episodes) */
      else
        *gmode = shareware;    /* Heretic shareware (E1 only) */
    }
    else if (cm>=30)
    {
      *gmode = commercial;
      *hassec = sc>=2;
    }
    else if (ud>=9)
      *gmode = retail;
    else if (rg>=18)
      *gmode = registered;
    else if (sw>=9)
      *gmode = shareware;
  }
  else // error from access call
  {
    filestream_close(fp);
    return I_Error("CheckIWAD: IWAD %s not readable", iwadname);
  }

  return true;
}


/*
 * FindIWADFIle
 *
 * Search for one of the standard IWADs
 * CPhipps  - static, proper prototype
 *    - 12/1999 - rewritten to use I_FindFile
 */
static char *FindIWADFile(void)
{
  int   i, x;
  char  * iwad  = NULL;

  i = M_CheckParm("-iwad");
  lprintf(LO_DEBUG, "i: %d\n", i);

  for(x = 0; x < 32; x++)
     lprintf(LO_DEBUG, "myargv[%d]: %s\n", x, myargv[x]);

  if (i && (++i < myargc)) {
    iwad = I_FindFile(myargv[i], NULL);
  } else {
    for (i=0; !iwad && i<nstandard_iwads; i++)
      iwad = I_FindFile(standard_iwads[i], NULL);
  }
  return iwad;
}

//
// IdentifyVersion
//
// Set the location of the defaults file and the savegame root
// Locate and validate an IWAD file
// Determine gamemode from the IWAD
//
// supports IWADs with custom names. Also allows the -iwad parameter to
// specify which iwad is being searched for if several exist in one dir.
// The -iwad parm may specify:
//
// 1) a specific pathname, which must exist (.wad optional)
// 2) or a directory, which must contain a standard IWAD,
// 3) or a filename, which must be found in one of the standard places:
//   a) current dir,
//   b) exe dir
//   c) $DOOMWADDIR
//   d) or $HOME
//
// jff 4/19/98 rewritten to use a more advanced search algorithm

static bool IdentifyVersion (void)
{
  int         i;    //jff 3/24/98 index of args on commandline
  char *iwad = NULL;

  // set save path to -save parm or current dir

  strcpy(basesavegame,I_DoomExeDir());
  lprintf(LO_INFO, "IdentifyVersion: basesavegame: %s\n", basesavegame);

  // locate the IWAD and determine game mode from it

  iwad = FindIWADFile();
  lprintf(LO_INFO, "iwad: %s\n", iwad ? iwad : "(none)");

  if (iwad && *iwad)
  {
    //jff 9/3/98 use logical output routine
    lprintf(LO_CONFIRM,"IWAD found: %s\n",iwad); //jff 4/20/98 print only if found
    if (!CheckIWAD(iwad,&gamemode,&haswolflevels))
       return false;

    /* jff 8/23/98 set gamemission global appropriately in all cases
     * cphipps 12/1999 - no version output here, leave that to the caller
     */
    if (raven)
    {
      /* CheckIWAD already recognised a Heretic or Hexen IWAD and set
       * gamemission = heretic_mission / hexen_mission; do not overwrite it
       * with a Doom mission below.  (Hexen and commercial Doom both use
       * MAP## levels, so without this the commercial branch would reset
       * gamemission to doom2 and the game would identify and behave as
       * Doom 2 -- loading the Doom status bar, switch list and so on.) */
    }
    else
    switch(gamemode)
    {
      case retail:
      case registered:
      case shareware:
        gamemission = doom;
        break;
      case commercial:
        i = strlen(iwad);
        gamemission = doom2;
        if (i>=10 && !strncasecmp(iwad+i-10,"doom2f.wad",10))
          language=french;
        else if (i>=7 && !strncasecmp(iwad+i-7,"tnt.wad",7))
          gamemission = pack_tnt;
        else if (i>=12 && !strncasecmp(iwad+i-12,"plutonia.wad",12))
          gamemission = pack_plut;
        break;
      default:
        gamemission = none;
        break;
    }
    if (gamemode == indetermined)
      //jff 9/3/98 use logical output routine
      lprintf(LO_WARN,"Unknown Game Version, may not work\n");
    D_AddFile(iwad,source_iwad);
    free(iwad);
  }
  else
    return I_Error("IdentifyVersion: IWAD not found\n");

  return true;
}



// killough 5/3/98: old code removed
//
// Find a Response File
//

#define MAXARGVS 100

static bool FindResponseFile (void)
{
   int i;

   for (i = 1;i < myargc;i++)
      if (myargv[i][0] == '@')
      {
         int  size;
         int  index;
         int indexinfile;
         uint8_t *file = NULL;
         const char **moreargs = malloc(myargc * sizeof(const char*));
         const char **newargv;
         // proff 04/05/2000: Added for searching responsefile
         char fname[PATH_MAX+1];

         strcpy(fname,&myargv[i][1]);
         AddDefaultExtension(fname,".rsp");

         // READ THE RESPONSE FILE INTO MEMORY
         // proff 04/05/2000: changed for searching responsefile
         // cph 2002/08/09 - use M_ReadFile for simplicity
         size = M_ReadFile(fname, &file);
         // proff 04/05/2000: Added for searching responsefile
         if (size < 0)
         {
            strcat(strcpy(fname,I_DoomExeDir()),&myargv[i][1]);
            AddDefaultExtension(fname,".rsp");
            size = M_ReadFile(fname, &file);
         }

         if (size < 0)
            return I_Error("No such response file: %s",fname);

         //jff 9/3/98 use logical output routine
         lprintf(LO_CONFIRM,"Found response file %s\n",fname);
         // proff 04/05/2000: Added check for empty rsp file
         if (size<=0)
         {
            int k;
            lprintf(LO_ERROR,"\nResponse file empty!\n");

            newargv = calloc(sizeof(char *),MAXARGVS);
            newargv[0] = myargv[0];
            for (k = 1,index = 1;k < myargc;k++)
            {
               if (i!=k)
                  newargv[index++] = myargv[k];
            }
            myargc = index; myargv = newargv;
            return true;
         }

         // KEEP ALL CMDLINE ARGS FOLLOWING @RESPONSEFILE ARG
         memcpy((void *)moreargs,&myargv[i+1],(index = myargc - i - 1) * sizeof(myargv[0]));

         {
            const char *firstargv = myargv[0];
            newargv = calloc(sizeof(char *),MAXARGVS);
            newargv[0] = firstargv;
         }

         {
            uint8_t *infile = file;
            indexinfile = 0;
            indexinfile++;  // SKIP PAST ARGV[0] (KEEP IT)
            do {
               while (size > 0 && isspace((unsigned char)*infile)) { infile++; size--; }
               if (size > 0) {
                  char *s = malloc(size+1);
                  char *p = s;
                  int quoted = 0;

                  while (size > 0) {
                     // Whitespace terminates the token unless quoted
                     if (!quoted && isspace((unsigned char)*infile)) break;
                     if (*infile == '\"') {
                        // Quotes are removed but remembered
                        infile++; size--; quoted ^= 1;
                     } else {
                        *p++ = *infile++; size--;
                     }
                  }
                  if (quoted)
                     return I_Error("Runaway quoted string in response file");

                  // Terminate string, realloc and add to argv
                  *p = 0;
                  newargv[indexinfile++] = realloc(s,strlen(s)+1);
               }
            } while(size > 0);
         }
         free(file);

         memcpy((void *)&newargv[indexinfile],moreargs,index*sizeof(moreargs[0]));
         free((void *)moreargs);

         myargc = indexinfile+index; myargv = newargv;

         // DISPLAY ARGS
         //jff 9/3/98 use logical output routine
         lprintf(LO_CONFIRM,"%d command-line args:\n",myargc);
         for (index=1;index<myargc;index++)
            //jff 9/3/98 use logical output routine
            lprintf(LO_CONFIRM,"%s\n",myargv[index]);
         break;
      }

   return true;
}

//
// DoLooseFiles
//
// Take any file names on the command line before the first switch parm
// and insert the appropriate -file, or -deh switch in front
// of them.
//
// Note that more than one -file, etc. entry on the command line won't
// work, so we have to go get all the valid ones if any that show up
// after the loose ones.  This means that boom fred.wad -file wilma
// will still load fred.wad and wilma.wad, in that order.
// The response file code kludges up its own version of myargv[] and
// unfortunately we have to do the same here because that kludge only
// happens if there _is_ a response file.  Truth is, it's more likely
// that there will be a need to do one or the other so it probably
// isn't important.  We'll point off to the original argv[], or the
// area allocated in FindResponseFile, or our own areas from strdups.
//
// CPhipps - OUCH! Writing into *myargv is too dodgy, damn

static void DoLooseFiles(void)
{
  char *wads[MAXARGVS];  // store the respective loose filenames
  char *lmps[MAXARGVS];
  char *dehs[MAXARGVS];
  int wadcount = 0;      // count the loose filenames
  int lmpcount = 0;
  int dehcount = 0;
  int i,j,p;
  const char **tmyargv;  // use these to recreate the argv array
  int tmyargc;
  dbool skip[MAXARGVS]; // CPhipps - should these be skipped at the end

  for (i=0; i<MAXARGVS; i++)
    skip[i] = FALSE;

  for (i=1;i<myargc;i++)
  {
    if (*myargv[i] == '-') break;  // quit at first switch

    // so now we must have a loose file.  Find out what kind and store it.
    j = strlen(myargv[i]);
    if (!strcasecmp(&myargv[i][j-4],".wad"))
      wads[wadcount++] = strdup(myargv[i]);
    if (!strcasecmp(&myargv[i][j-4],".lmp"))
      lmps[lmpcount++] = strdup(myargv[i]);
    if (!strcasecmp(&myargv[i][j-4],".deh"))
      dehs[dehcount++] = strdup(myargv[i]);
    if (!strcasecmp(&myargv[i][j-4],".bex"))
      dehs[dehcount++] = strdup(myargv[i]);
    if (myargv[i][j-4] != '.')  // assume wad if no extension
      wads[wadcount++] = strdup(myargv[i]);
    skip[i] = TRUE; // nuke that entry so it won't repeat later
  }

  // Now, if we didn't find any loose files, we can just leave.
  if (wadcount+lmpcount+dehcount == 0) return;  // ******* early return ****

  if ((p = M_CheckParm ("-file")))
  {
    skip[p] = TRUE;    // nuke the entry
    while (++p != myargc && *myargv[p] != '-')
    {
      wads[wadcount++] = strdup(myargv[p]);
      skip[p] = TRUE;  // null any we find and save
    }
  }

  if ((p = M_CheckParm ("-deh")))
  {
    skip[p] = TRUE;    // nuke the entry
    while (++p != myargc && *myargv[p] != '-')
    {
      dehs[dehcount++] = strdup(myargv[p]);
      skip[p] = TRUE;  // null any we find and save
    }
  }

  if ((p = M_CheckParm ("-playdemo")))
  {
    skip[p] = true;    // nuke the entry
    while (++p != myargc && *myargv[p] != '-')
    {
      lmps[lmpcount++] = strdup(myargv[p]);
      skip[p] = true;  // null any we find and save
    }
  }

  // Now go back and redo the whole myargv array with our stuff in it.
  // First, create a new myargv array to copy into
  tmyargv = calloc(sizeof(char *),MAXARGVS);
  tmyargv[0] = myargv[0]; // invocation
  tmyargc = 1;

  // put our stuff into it
  if (wadcount > 0)
  {
    tmyargv[tmyargc++] = strdup("-file"); // put the switch in
    for (i=0;i<wadcount;)
      tmyargv[tmyargc++] = wads[i++]; // allocated by strdup above
  }

  // for -deh
  if (dehcount > 0)
  {
    tmyargv[tmyargc++] = strdup("-deh");
    for (i=0;i<dehcount;)
      tmyargv[tmyargc++] = dehs[i++];
  }

  // for -playdemo
  if (lmpcount > 0)
  {
    tmyargv[tmyargc++] = strdup("-playdemo");
    for (i=0;i<lmpcount;)
      tmyargv[tmyargc++] = lmps[i++];
  }

  // then copy everything that's there now
  for (i=1;i<myargc;i++)
  {
    if (!skip[i])  // skip any zapped entries
      tmyargv[tmyargc++] = myargv[i];  // pointers are still valid
  }
  // now make the global variables point to our array
  myargv = tmyargv;
  myargc = tmyargc;
}

/* cph - MBF-like wad/deh/bex autoload code */
const char *wad_files[MAXLOADFILES], *deh_files[MAXLOADFILES];

//
// D_DoomMainSetup
//
// CPhipps - the old contents of D_DoomMain, but moved out of the main
//  line of execution so its stack space can be freed

bool D_DoomMainSetup(void)
{
  int p;

#ifndef PSX
  setbuf(stdout,NULL);
#endif

  // proff 04/05/2000: Added support for include response files
  /* proff 2001/7/1 - Moved up, so -config can be in response files */
  {
    dbool rsp_found;
    int i;

    do {
      rsp_found=FALSE;
      for (i=0; i<myargc; i++)
        if (myargv[i][0]=='@')
          rsp_found=TRUE;
      if (!FindResponseFile())
         goto failed;
    } while (rsp_found==TRUE);
  }

  lprintf(LO_INFO,"M_LoadDefaults: Load system defaults.\n");
  M_LoadDefaults();              // load before initing other systems

  // figgi 09/18/00-- added switch to force classic bsp nodes
  if (M_CheckParm ("-forceoldbsp"))
  {
    extern dbool forceOldBsp;
    forceOldBsp = TRUE;
  }

  D_BuildBEXTables(); // haleyjd

  DoLooseFiles();  // Ty 08/29/98 - handle "loose" files on command line
  if (!IdentifyVersion())
     goto failed;

  /* Hexen binds both Use and Jump, but the stock key defaults put key_use
   * and key_jump on the spacebar (harmless in Doom/Heretic, which have no
   * jump).  The gamepad layouts post these keycodes into the event queue,
   * so identical values make every Use press jump and every Jump press
   * use.  When they collide under Hexen, move Use to 'e' per the Hexen
   * keyboard scheme (spacebar is Jump, E is Use - see kbd_hexen_desc). */
  if (hexen && key_use == key_jump)
    key_use = 'e';

  /* D_BuildBEXTables above seeded the dynamic tables before the game type
   * was known (it runs before IdentifyVersion), so they were seeded as
   * Doom. Now that IdentifyVersion has set heretic/gamemission, re-seed so
   * Heretic gets its own states/mobjinfo/sprites/sounds. dsda_InitTables
   * frees the previous copies first, so re-running is safe. For Doom this
   * is a harmless reseed of identical data. */
  dsda_InitTables();

  /* Select the weapon frame table for the game now that the type is known,
   * so the player's weapon psprite is driven by the correct (Heretic vs
   * Doom) weapon states. */
  D_InitWeaponInfo();

  // prboom.wad is baked into the core: add it as an embedded WAD here,
  // after the IWAD but before everything else.  The engine no longer looks
  // for prboom.wad on the filesystem -- the compiled-in copy is always used,
  // so the data is guaranteed present and cannot be overridden or omitted.
  {
    char *embed_name = malloc(sizeof(PACKAGE ".wad"));
    wadfiles = realloc(wadfiles, sizeof(*wadfiles) * (numwadfiles + 1));
    memset(&wadfiles[numwadfiles], 0, sizeof(wadfiles[numwadfiles]));
    /* name is a label only (never opened); malloc'd so W_ReleaseAllWads
     * frees it uniformly with every other entry. */
    strcpy(embed_name, PACKAGE ".wad");
    wadfiles[numwadfiles].name = embed_name;
    wadfiles[numwadfiles].src  = source_pre;
    wadfiles[numwadfiles].embedded_data   = prboom_wad_data;
    wadfiles[numwadfiles].embedded_length = (int)prboom_wad_data_len;
    numwadfiles++;
  }

  // e6y: DEH files preloaded in wrong order
  // http://sourceforge.net/tracker/index.php?func=detail&aid=1418158&group_id=148658&atid=772943
  // The dachaked stuff has been moved below an autoload

  // jff 1/24/98 set both working and command line value of play parms
  nomonsters = clnomonsters = M_CheckParm ("-nomonsters");
  respawnparm = clrespawnparm = M_CheckParm ("-respawn");
  fastparm = clfastparm = M_CheckParm ("-fast");
  // jff 1/24/98 end of set to both working and command line value

  if (M_CheckParm ("-altdeath"))
    deathmatch = 2;
  else
    if (M_CheckParm ("-deathmatch"))
      deathmatch = 1;

  {
    // CPhipps - localise title variable
    // print title for every printed line
    // cph - code cleaned and made smaller
    const char* doomverstr;

    if (hexen)
    {
      doomverstr = "Hexen: Beyond Heretic";
    }
    else if (heretic)
    {
      /* The Doom gamemode slots are reused for Heretic (see IdentifyVersion):
       * shareware = E1 only, registered = the full game. Distinguish the
       * Shadow of the Serpent Riders release by its extra episodes (E4). */
      if (gamemode == shareware)
        doomverstr = "Heretic Shareware";
      else if (W_CheckNumForName("E4M1") >= 0)
        doomverstr = "Heretic: Shadow of the Serpent Riders";
      else
        doomverstr = "Heretic Registered";
    }
    else
    switch ( gamemode ) {
    case retail:
      doomverstr = "The Ultimate DOOM";
      break;
    case shareware:
      doomverstr = "DOOM Shareware";
      break;
    case registered:
      doomverstr = "DOOM Registered";
      break;
    case commercial:  // Ty 08/27/98 - fixed gamemode vs gamemission
      switch (gamemission)
      {
        case pack_plut:
    doomverstr = "DOOM 2: Plutonia Experiment";
          break;
        case pack_tnt:
          doomverstr = "DOOM 2: TNT - Evilution";
          break;
        default:
          doomverstr = "DOOM 2: Hell on Earth";
          break;
      }
      break;
    default:
      doomverstr = "Public DOOM";
      break;
    }

    /* cphipps - the main display. This shows the build date, copyright, and game type */
    lprintf(LO_INFO,"PrBoom, playing: %s\n"
      "PrBoom is released under the GNU General Public license v2.0.\n"
      "You are welcome to redistribute it under certain conditions.\n"
      "It comes with ABSOLUTELY NO WARRANTY. See the file COPYING for details.\n",
      doomverstr);
  }

  modifiedgame = FALSE;

  // get skill / episode / map from parms

  startskill = sk_none; // jff 3/24/98 was sk_medium, just note not picked
  startepisode = 1;
  startmap = 1;
  autostart = FALSE;

  if ((p = M_CheckParm ("-skill")) && p < myargc-1)
    {
      startskill = myargv[p+1][0]-'1';
      autostart = TRUE;
    }

  if ((p = M_CheckParm ("-episode")) && p < myargc-1)
    {
      startepisode = myargv[p+1][0]-'0';
      startmap = 1;
      autostart = TRUE;
    }

  if ((p = M_CheckParm ("-warp")) ||      // killough 5/2/98
       (p = M_CheckParm ("-wart")))
       // Ty 08/29/98 - moved this check later so we can have -warp alone: && p < myargc-1)
  {
    startmap = 0; // Ty 08/29/98 - allow "-warp x" to go to first map in wad(s)
    autostart = TRUE; // Ty 08/29/98 - move outside the decision tree
    if (gamemode == commercial)
    {
      if (p < myargc-1)
        startmap = atoi(myargv[p+1]);   // Ty 08/29/98 - add test if last parm
    }
    else    // 1/25/98 killough: fix -warp xxx from crashing Doom 1 / UD
    {
      if (p < myargc-2)
      {
        startepisode = atoi(myargv[++p]);
        startmap = atoi(myargv[p+1]);
      }
    }
  }
  // Ty 08/29/98 - later we'll check for startmap=0 and autostart=TRUE
  // as a special case that -warp * was used.  Actually -warp with any
  // non-numeric will do that but we'll only document "*"

  //jff 1/22/98 add command line parms to disable sound and music
  {
    int nosound = M_CheckParm("-nosound");
    nomusicparm = nosound || M_CheckParm("-nomusic");
    nosfxparm   = nosound || M_CheckParm("-nosfx");
  }
  //jff end of sound/music command line parms

  //proff 11/22/98: Added setting of viewangleoffset
  p = M_CheckParm("-viewangle");
  if (p)
  {
    viewangleoffset = atoi(myargv[p+1]);
    viewangleoffset = viewangleoffset<0 ? 0 : (viewangleoffset>7 ? 7 : viewangleoffset);
    viewangleoffset = (8-viewangleoffset) * ANG45;
  }

  // init subsystems

  G_ReloadDefaults();    // killough 3/4/98: set defaults just loaded.
  // jff 3/24/98 this sets startskill if it was -1

  //jff 9/3/98 use logical output routine
  lprintf(LO_INFO,"V_Init: allocate screens.\n");
  V_Init();

  // CPhipps - autoloading of wads
  // Designed to be general, instead of specific to boomlump.wad
  // Some people might find this useful
  // cph - support MBF -noload parameter
  if (!M_CheckParm("-noload")) {
    // only autoloaded wads here - autoloaded patches moved down below W_Init
    int i;

    for (i=0; i<MAXLOADFILES; i++) {
      const char *fname = wad_files[i];
      char *fpath;

      if (!(fname && *fname)) continue;
      // Filename is now stored as a zero terminated string
      fpath = I_FindFile(fname, NULL);
      if (!fpath)
        lprintf(LO_WARN, "Failed to autoload %s\n", fname);
      else {
        D_AddFile(fpath,source_auto_load);
        modifiedgame = TRUE;
        free(fpath);
      }
    }
  }

  // add any files specified on the command line with -file wadfile
  // to the wad list

  // killough 1/31/98, 5/2/98: reload hack removed, -wart same as -warp now.

  if ((p = M_CheckParm ("-file")))
  {
    // the parms after p are wadfile/lump names,
    // until end of parms or another - preceded parm
    modifiedgame = TRUE;            // homebrew levels
    while (++p != myargc && *myargv[p] != '-')
      D_AddFile(myargv[p],source_pwad);
  }

  p = M_CheckParm("-playdemo");
  if (p && p < myargc-1)
  {
    char file[PATH_MAX+1];      // cph - localised
    strcpy(file,myargv[p+1]);
    AddDefaultExtension(file,".lmp");     // killough
    D_AddFile (file,source_lmp);
    //jff 9/3/98 use logical output routine
    lprintf(LO_CONFIRM,"Playing demo %s\n",file);
    if ((p = M_CheckParm ("-ffmap")) && p < myargc-1) {
      ffmap = atoi(myargv[p+1]);
    }
  }

  // 1/18/98 killough: Z_Init() call moved to i_main.c

  // CPhipps - move up netgame init
  //jff 9/3/98 use logical output routine
  lprintf(LO_INFO,"D_InitNetGame: Checking for network game.\n");
  D_InitNetGame();

  //jff 9/3/98 use logical output routine
  lprintf(LO_INFO,"W_Init: Init WADfiles.\n");
  W_Init(); // CPhipps - handling of wadfiles init changed

  lprintf(LO_INFO,"\n");     // killough 3/6/98: add a newline, by popular demand :)

  // e6y
  // option to disable automatic loading of dehacked-in-wad lump
  if (!M_CheckParm ("-nodeh"))
    // MBF-style DeHackEd in wad support: load all lumps, not just the last one
    for (p = -1; (p = W_ListNumFromName("DEHACKED", p)) >= 0; )
      // Split loading DEHACKED lumps into IWAD/autoload and PWADs/others
      if (lumpinfo[p].source == source_iwad
          || lumpinfo[p].source == source_pre
          || lumpinfo[p].source == source_auto_load)
        ProcessDehFile(NULL, D_dehout(), p); // cph - add dehacked-in-a-wad support

   if (bfgedition)
    {
      int lump = (W_CheckNumForName)("BFGDEH", ns_prboom);
      if (lump != -1)
      {
        ProcessDehFile(NULL, D_dehout(), lump);
      }
    }

  if (!M_CheckParm("-noload"))
  {
    // now do autoloaded dehacked patches, after IWAD patches but before PWAD
    int i;

    for (i=0; i<MAXLOADFILES; i++) {
      const char *fname = deh_files[i];
      char *fpath;

      if (!(fname && *fname)) continue;
      // Filename is now stored as a zero terminated string
      fpath = I_FindFile(fname, NULL);
      if (!fpath)
        lprintf(LO_WARN, "Failed to autoload %s\n", fname);
      else {
        ProcessDehFile(fpath, D_dehout(), 0);
        // this used to set modifiedgame here, but patches shouldn't
        free(fpath);
      }
    }
  }

  if (!M_CheckParm ("-nodeh"))
    for (p = -1; (p = W_ListNumFromName("DEHACKED", p)) >= 0; )
      if (!(lumpinfo[p].source == source_iwad
            || lumpinfo[p].source == source_pre
            || lumpinfo[p].source == source_auto_load))
        ProcessDehFile(NULL, D_dehout(), p);

  // Load command line dehacked patches after WAD dehacked patches

  // e6y: DEH files preloaded in wrong order
  // http://sourceforge.net/tracker/index.php?func=detail&aid=1418158&group_id=148658&atid=772943

  // ty 03/09/98 do dehacked stuff
  // Using -deh in BOOM, others use -dehacked.
  // Ty 03/18/98 also allow .bex extension.  .bex overrides if both exist.

  p = M_CheckParm ("-deh");
  if (p)
  {
    char file[PATH_MAX+1];      // cph - localised
    // the parms after p are deh/bex file names,
    // until end of parms or another - preceded parm
    // Ty 04/11/98 - Allow multiple -deh files in a row

    while (++p != myargc && *myargv[p] != '-')
    {
      AddDefaultExtension(strcpy(file, myargv[p]), ".bex");
      if (!path_is_valid(file))
      {
        AddDefaultExtension(strcpy(file, myargv[p]), ".deh");
	if (!path_is_valid(file))
          I_Error("D_DoomMainSetup: Cannot find .deh or .bex file named %s",
                  myargv[p]);
      }
      // during the beta we have debug output to dehout.txt
      ProcessDehFile(file,D_dehout(),0);
    }
  }

  V_InitColorTranslation(); //jff 4/24/98 load color translation lumps

  // killough 2/22/98: copyright / "modified game" / SPA banners removed

  // Ty 04/08/98 - Add 5 lines of misc. data, only if nonblank
  // The expectation is that these will be set in a .bex file
  //jff 9/3/98 use logical output routine
  if (*startup1) lprintf(LO_INFO,"%s",startup1);
  if (*startup2) lprintf(LO_INFO,"%s",startup2);
  if (*startup3) lprintf(LO_INFO,"%s",startup3);
  if (*startup4) lprintf(LO_INFO,"%s",startup4);
  if (*startup5) lprintf(LO_INFO,"%s",startup5);
  // End new startup strings

  //jff 9/3/98 use logical output routine
  lprintf(LO_INFO,"M_Init: Init miscellaneous info.\n");
  M_Init();

  // if not explicitly disabled, load UMAPINFO
  if (!M_CheckParm("-nomapinfo"))
  {
    for (p = -1; (p = W_ListNumFromName("UMAPINFO", p)) >= 0; )
    {
      const char *data;
      lprintf(LO_INFO,"U_ParseMapInfo: Loading Custom Episode and Map Information.\n");
      data = (const char *)W_CacheLumpNum(p);
      U_ParseMapInfo(data, W_LumpLength(p));
    }

    /* ZDoom wads carry episode structure in a MAPINFO lump instead;
     * translate it into the UMAPINFO tables when no UMAPINFO took
     * precedence.  Hexen and Heretic interpret MAPINFO themselves. */
    if (!hexen && !heretic && !U_mapinfo.mapcount &&
        (p = W_CheckNumForName("MAPINFO")) >= 0)
    {
      const char *data;
      lprintf(LO_INFO,"U_ParseZMapInfo: Translating ZDoom MAPINFO.\n");
      data = (const char *)W_CacheLumpNum(p);
      U_ParseZMapInfo(data, W_LumpLength(p));
    }
  }


  /* ZDoom DECORATE decorations: register static props before the sprite
   * definitions and the editor-number hash freeze (Doom game only -- the
   * shared mobjinfo table would expose them to Heretic lookups too). */
  if (!hexen && !heretic && W_CheckNumForName("DECORATE") >= 0)
    U_RegisterDecorateThings();

  //jff 9/3/98 use logical output routine
  lprintf(LO_INFO,"R_Init: Init DOOM refresh daemon - ");
  R_Init();

  if (hexen)
  {
    lprintf(LO_INFO,"\nP_LoadMapInfo: Parsing Hexen MAPINFO.\n");
    P_LoadMapInfo();
    SN_InitSequenceScript();
  }

  //jff 9/3/98 use logical output routine
  lprintf(LO_INFO,"\nP_Init: Init Playloop state.\n");
  P_Init();

  //jff 9/3/98 use logical output routine
  lprintf(LO_INFO,"I_Init: Setting up machine state.\n");
  I_Init();

  //jff 9/3/98 use logical output routine
  lprintf(LO_INFO,"S_Init: Setting up sound.\n");
  S_Init(snd_SfxVolume /* *8 */, snd_MusicVolume /* *8*/ );

  //jff 9/3/98 use logical output routine
  lprintf(LO_INFO,"HU_Init: Setting up heads up display.\n");
  HU_Init();

  if (!(M_CheckParm("-nodraw") && M_CheckParm("-nosound")))
    I_InitGraphics();

  //jff 9/3/98 use logical output routine
  lprintf(LO_INFO,"ST_Init: Init status bar.\n");
  ST_Init();

  idmusnum = -1; //jff 3/17/98 insure idmus number is blank


  if ((p = M_CheckParm("-playdemo")) && ++p < myargc)
  {
	singledemo = TRUE;
	G_DeferedPlayDemo(myargv[p]);
  }

  // start the apropriate game based on parms

  /* Hexen is class-based; until the class-selection menu exists, default
   * every player to the Fighter (matching the Fighter foundation defaults
   * the runtime tables are seeded with).  This makes player->class correct
   * at spawn so the class-indexed weapon/mana code resolves to the Fighter
   * rather than the empty PCLASS_NULL column. */
  if (hexen)
  {
    int pc;
    pclass_t startclass = PCLASS_FIGHTER;
    int p = M_CheckParm("-class");

    if (p && p + 1 < myargc)
    {
      const char *c = myargv[p + 1];
      if (!strcasecmp(c, "cleric") || !strcasecmp(c, "c"))
        startclass = PCLASS_CLERIC;
      else if (!strcasecmp(c, "mage") || !strcasecmp(c, "m"))
        startclass = PCLASS_MAGE;
      else /* "fighter"/"f"/anything else */
        startclass = PCLASS_FIGHTER;
    }

    for (pc = 0; pc < MAXPLAYERS; pc++)
      PlayerClass[pc] = startclass;
  }

  if (gameaction != ga_playdemo)
  {
    if (autostart)
    {
      // sets first map and first episode if unknown
      GetFirstMap(&startepisode, &startmap);
      G_InitNew(startskill, startepisode, startmap);
    }
    else
      D_StartTitle(); // start up intro loop
  }
  return true;

failed:
  return false;
}

//
// D_DoomMain
//

void D_DoomLoop(void)
{
   if (ffmap == gamemap) ffmap = 0;

   TryRunTics (); // will run at least one tic

   // killough 3/16/98: change consoleplayer to displayplayer
   if (players[displayplayer].mo) // cph 2002/08/10
      S_UpdateSounds(players[displayplayer].mo);// move positional sounds

   /* Always render the next frame.  The libretro frontend drives
    * one D_Display per retro_run; there is no equivalent of the
    * netgame "skip-display-when-already-rendered-during-tic-wait"
    * optimisation that the original WasRenderedInTryRunTics flag
    * gated, because libretro's TryRunTics is a non-blocking single
    * tic-step that never calls D_Display itself. */
   D_Display();
}

//foward decl
void M_QuitDOOM(int choice);

void D_DoomDeinit(void)
{
  lprintf(LO_INFO,"D_DoomDeinit:\n");
  /* Deinit, in dependency order:
   *   - level data (PU_LEVEL/PU_LEVSPEC) before anything else, since
   *     mobj thinkers etc. don't reference subsystem state being torn
   *     down later;
   *   - render-derived data (patches, screens, palettes) before sound,
   *     music, and wad cleanup, so we don't accidentally re-cache a
   *     lump after the wads close;
   *   - W_ReleaseAllWads last so file handles stay open through the
   *     rest of deinit, even though nothing currently re-reads.
   *
   * W_ReleaseAllWads (rather than W_Exit) does the FULL teardown:
   * close handles, free wadfile data + name strings, free wadfiles
   * array, free lumpinfo, and W_DoneCache to free cachelump.
   * Without this, every retro_load_game leaked:
   *   - cachelump (~32KB for shareware DOOM)
   *   - lumpinfo (~100KB for DOOM2)
   *   - wadfiles array
   *   - wadfile name strings
   * and -- worse -- the next session's W_Init iterated the previous
   * session's stale wadfiles[] entries and re-opened the previous
   * IWAD on top of the new one, accumulating both lump tables
   * (with session 1's lumps shadowing session 2's where names
   * collided).  Z_Close at retro_deinit eventually reclaims the
   * memory but only at process end -- between sessions everything
   * stayed orphaned and broken.
   *
   * Nothing between D_DoomDeinit returning and the next
   * D_DoomMainSetup's W_Init touches cachelump / wadfiles /
   * lumpinfo, so the full teardown is safe.
   */
  M_QuitDOOM(0);
  M_SaveDefaults();
  P_Deinit();
  R_FlushAllPatches();
  R_Deinit();
  AM_Deinit();
  G_Deinit();
  D_FreePageCache();
  V_FreeScreens();
  V_DestroyTrueColorPalette();
  S_Shutdown();
  I_ShutdownSound();
  I_ShutdownMusic();
  U_FreeMapInfo();
  D_FreeBEXTables();
  M_FreeDefaults();
  W_ReleaseAllWads();
}

//
// GetFirstMap
//
// Ty 08/29/98 - determine first available map from the loaded wads and run it
//

void GetFirstMap(int *ep, int *map)
{
  short int i,j; // used to generate map name
  dbool done = FALSE;  // Ty 09/13/98 - to exit inner loops
  char test[9];  // MAPxx or ExMx plus terminator for testing
  char name[9];  // MAPxx or ExMx plus terminator for display
  dbool newlevel = FALSE;  // Ty 10/04/98 - to test for new level
  int ix;  // index for lookup

  strcpy(name,""); // initialize
  if (*map == 0) // unknown so go search for first changed one
  {
    *ep = 1;
    *map = 1; // default E1M1 or MAP01
    if (gamemode == commercial)
    {
      for (i=1;!done && i<33;i++)  // Ty 09/13/98 - add use of !done
      {
        sprintf(test,"MAP%02d",i);
        ix = W_CheckNumForName(test);
        if (ix != -1)  // Ty 10/04/98 avoid -1 subscript
        {
          if (lumpinfo[ix].source == source_pwad)
          {
            *map = i;
            strcpy(name,test);  // Ty 10/04/98
            done = TRUE;  // Ty 09/13/98
            newlevel = TRUE; // Ty 10/04/98
          }
          else
          {
            if (!*name)  // found one, not pwad.  First default.
               strcpy(name,test);
          }
        }
      }
    }
    else // one of the others
    {
      strcpy(name,"E1M1");  // Ty 10/04/98 - default for display
      for (i=1;!done && i<5;i++)  // Ty 09/13/98 - add use of !done
      {
        for (j=1;!done && j<10;j++)  // Ty 09/13/98 - add use of !done
        {
          sprintf(test,"E%dM%d",i,j);
          ix = W_CheckNumForName(test);
          if (ix != -1)  // Ty 10/04/98 avoid -1 subscript
          {
            if (lumpinfo[ix].source == source_pwad)
            {
              *ep = i;
              *map = j;
              strcpy(name,test); // Ty 10/04/98
              done = TRUE;  // Ty 09/13/98
              newlevel = TRUE; // Ty 10/04/98
            }
            else
            {
              if (!*name)  // found one, not pwad.  First default.
                 strcpy(name,test);
            }
          }
        }
      }
    }
    //jff 9/3/98 use logical output routine
    lprintf(LO_CONFIRM,"Auto-warping to first %slevel: %s\n",
      newlevel ? "new " : "", name);  // Ty 10/04/98 - new level test
  }
}
