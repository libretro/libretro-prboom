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
 *  Main loop menu stuff.
 *  Default Config File.
 *  PCX Screenshots.
 *
 *-----------------------------------------------------------------------------*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <errno.h>
#ifdef _MSC_VER
#include <io.h>
#include <compat/msvc.h>
#endif
#include <fcntl.h>
#include <sys/stat.h>

#include "doomstat.h"
#include "m_argv.h"
#include "g_game.h"
#include "m_menu.h"
#include "am_map.h"
#include "w_wad.h"
#include "i_system.h"
#include "i_sound.h"
#include "i_video.h"
#include "v_video.h"
#include "hu_stuff.h"
#include "st_stuff.h"
#include "dstrings.h"
#include "m_misc.h"
#include "s_sound.h"
#include "sounds.h"
#include "lprintf.h"
#include "d_main.h"
#include "r_draw.h"
#include "r_demo.h"
#include "r_fps.h"
#include "r_sky.h"

#ifdef _WIN32
   #define DIR_SLASH_STR "\\"
#else
   #define DIR_SLASH_STR "/"
#endif

extern boolean r_wigglefix;

/*
 * M_WriteFile
 *
 * killough 9/98: rewritten to use stdio and to flash disk icon
 */

boolean M_WriteFile(char const *name, void *source, int length)
{
  FILE *fp;

  errno = 0;

  if (!(fp = fopen(name, "wb")))       // Try opening file
    return 0;                          // Could not open file for writing

  length = fwrite(source, 1, length, fp) == (size_t)length;   // Write data
  fclose(fp);

  if (!length)                         // Remove partially written file
    remove(name);

  return length;
}

/*
 * M_ReadFile
 *
 * killough 9/98: rewritten to use stdio and to flash disk icon
 */

int M_ReadFile(char const *name, uint8_t **buffer)
{
   FILE *fp;

   if ((fp = fopen(name, "rb")))
   {
      size_t length;

      fseek(fp, 0, SEEK_END);
      length = ftell(fp);
      fseek(fp, 0, SEEK_SET);
      *buffer = Z_Malloc(length, PU_STATIC, 0);
      if (fread(*buffer, 1, length, fp) == length)
      {
         fclose(fp);
         return length;
      }
      fclose(fp);
   }

   /* cph 2002/08/10 - this used to return 0 on error, but that's ambiguous,
    * because we could have a legit 0-length file. So make it -1. */
   return -1;
}

//
// DEFAULTS
//

int usemouse;
boolean    precache = TRUE; /* if TRUE, load all graphics at start */

extern int viewwidth;
extern int viewheight;

extern int tran_filter_pct;            // killough 2/21/98

extern int screenblocks;
extern int showMessages;

extern int movement_maxviewpitch;

int         mus_pause_opt; // 0 = kill music, 1 = pause, 2 = continue
int         mus_load_external; // 0 = never load external music files, 1 = always load it, 2 = only from iwads

extern const char* chat_macros[];

extern const char* S_music_files[]; // cournia

/* cph - Some MBF stuff parked here for now
 * killough 10/98
 */
int map_point_coordinates;

default_t defaults[] =
{
  {"Misc settings",{NULL},{0},UL,UL,def_none,ss_none, NULL, NULL},
  {"default_compatibility_level",{(int*)&default_compatibility_level, NULL},
   {-1, NULL},-1,MAX_COMPATIBILITY_LEVEL-1,
   def_int,ss_none, NULL, NULL}, // compatibility level" - CPhipps
  {"menu_background", {(int*)&menu_background, NULL}, {1, NULL}, 0, 1,
   def_bool,ss_gen, NULL, NULL}, // do Boom fullscreen menus have backgrounds?
  {"max_player_corpse", {&bodyquesize, NULL}, {32, NULL},-1,UL,   // killough 2/8/98
   def_int,ss_gen, NULL, NULL}, // number of dead bodies in view supported (-1 = no limit)
  {"flashing_hom",{&flashing_hom, NULL},{0, NULL},0,1,
   def_bool,ss_gen, NULL, NULL}, // killough 10/98 - enable flashing HOM indicator
  {"demo_insurance",{&default_demo_insurance, NULL},{2, NULL},0,2,  // killough 3/31/98
   def_int,ss_none, NULL, NULL}, // 1=take special steps ensuring demo sync, 2=only during recordings
  {"level_precache",{(int*)&precache, NULL},{0, NULL},0,1,
   def_bool,ss_none, NULL, NULL}, // precache level data?
  {"demo_smoothturns", {&demo_smoothturns, NULL},  {0, NULL},0,1,
   def_bool,ss_gen, NULL, NULL},
  {"demo_smoothturnsfactor", {&demo_smoothturnsfactor, NULL},  {6, NULL},1,SMOOTH_PLAYING_MAXFACTOR,
   def_int,ss_gen, NULL, NULL},

  {"Files",{NULL},{0},UL,UL,def_none,ss_none, NULL, NULL},
  /* cph - MBF-like wad/deh/bex autoload code */
  {"wadfile_1",{NULL,&wad_files[0]},{0,""},UL,UL,def_str,ss_gen, NULL, NULL},
  {"wadfile_2",{NULL,&wad_files[1]},{0,""},UL,UL,def_str,ss_gen, NULL, NULL},
  {"wadfile_3",{NULL,&wad_files[2]},{0,""},UL,UL,def_str,ss_gen, NULL, NULL},
  {"wadfile_4",{NULL,&wad_files[3]},{0,""},UL,UL,def_str,ss_gen, NULL, NULL},
  {"wadfile_5",{NULL,&wad_files[4]},{0,""},UL,UL,def_str,ss_gen, NULL, NULL},
  {"wadfile_6",{NULL,&wad_files[5]},{0,""},UL,UL,def_str,ss_gen, NULL, NULL},
  {"wadfile_7",{NULL,&wad_files[6]},{0,""},UL,UL,def_str,ss_gen, NULL, NULL},
  {"wadfile_8",{NULL,&wad_files[7]},{0,""},UL,UL,def_str,ss_gen, NULL, NULL},
  {"dehfile_1",{NULL,&deh_files[0]},{0,""},UL,UL,def_str,ss_gen, NULL, NULL},
  {"dehfile_2",{NULL,&deh_files[1]},{0,""},UL,UL,def_str,ss_gen, NULL, NULL},

  {"Game settings",{NULL},{0},UL,UL,def_none,ss_none, NULL, NULL},
  {"default_skill",{&defaultskill, NULL},{3, NULL},1,5, // jff 3/24/98 allow default skill setting
   def_int,ss_gen, NULL, NULL}, // selects default skill 1=TYTD 2=NTR 3=HMP 4=UV 5=NM
  {"weapon_recoil",{&default_weapon_recoil, NULL},{0, NULL},0,1,
   def_bool,ss_weap, &weapon_recoil, NULL},
  /* killough 10/98 - toggle between SG/SSG and Fist/Chainsaw */
  {"doom_weapon_toggles",{&doom_weapon_toggles, NULL}, {1, NULL}, 0, 1,
   def_bool, ss_weap, NULL, NULL },
  {"player_bobbing",{&default_player_bobbing, NULL},{1, NULL},0,1,         // phares 2/25/98
   def_bool,ss_weap, &player_bobbing, NULL},
  {"monsters_remember",{&default_monsters_remember, NULL},{1, NULL},0,1,   // killough 3/1/98
   def_bool,ss_enem, &monsters_remember, NULL},
   /* MBF AI enhancement options */
  {"monster_infighting",{&default_monster_infighting, NULL}, {1, NULL}, 0, 1,
   def_bool, ss_enem, &monster_infighting, NULL},
  {"monster_backing",{&default_monster_backing, NULL}, {0, NULL}, 0, 1,
   def_bool, ss_enem, &monster_backing, NULL},
  {"monster_avoid_hazards",{&default_monster_avoid_hazards, NULL}, {1, NULL}, 0, 1,
   def_bool, ss_enem, &monster_avoid_hazards, NULL},
  {"monkeys",{&default_monkeys, NULL}, {0, NULL}, 0, 1,
   def_bool, ss_enem, &monkeys, NULL},
  {"monster_friction",{&default_monster_friction, NULL}, {1, NULL}, 0, 1,
   def_bool, ss_enem, &monster_friction, NULL},
  {"help_friends",{&default_help_friends, NULL}, {1, NULL}, 0, 1,
   def_bool, ss_enem, &help_friends, NULL},
  {"allow_pushers",{&default_allow_pushers, NULL},{1, NULL},0,1,
   def_bool,ss_weap, &allow_pushers, NULL},
  {"variable_friction",{&default_variable_friction, NULL},{1, NULL},0,1,
   def_bool,ss_weap, &variable_friction, NULL},
   /* End of MBF AI extras */

  {"sts_always_red",{&sts_always_red, NULL},{1, NULL},0,1, // no color changes on status bar
   def_bool,ss_stat, NULL, NULL},
  {"sts_pct_always_gray",{&sts_pct_always_gray, NULL},{0, NULL},0,1, // 2/23/98 chg default
   def_bool,ss_stat, NULL, NULL}, // makes percent signs on status bar always gray
  {"sts_traditional_keys",{&sts_traditional_keys, NULL},{0, NULL},0,1,  // killough 2/28/98
   def_bool,ss_stat, NULL, NULL}, // disables doubled card and skull key display on status bar
  {"show_messages",{&showMessages, NULL},{1, NULL},0,1,
   def_bool,ss_none, NULL, NULL}, // enables message display
  {"autorun",{&autorun, NULL},{0, NULL},0,1,  // killough 3/6/98: preserve autorun across games
   def_bool,ss_none, NULL, NULL},

  {"Compatibility settings",{NULL},{0},UL,UL,def_none,ss_none, NULL, NULL},
  {"comp_zombie",{&default_comp[comp_zombie], NULL},{0, NULL},0,1,def_bool,ss_comp,&comp[comp_zombie], NULL},
  {"comp_infcheat",{&default_comp[comp_infcheat], NULL},{0, NULL},0,1,def_bool,ss_comp,&comp[comp_infcheat], NULL},
  {"comp_stairs",{&default_comp[comp_stairs], NULL},{0, NULL},0,1,def_bool,ss_comp,&comp[comp_stairs], NULL},
  {"comp_telefrag",{&default_comp[comp_telefrag], NULL},{0, NULL},0,1,def_bool,ss_comp,&comp[comp_telefrag], NULL},
  {"comp_dropoff",{&default_comp[comp_dropoff], NULL},{0, NULL},0,1,def_bool,ss_comp,&comp[comp_dropoff], NULL},
  {"comp_falloff",{&default_comp[comp_falloff], NULL},{0, NULL},0,1,def_bool,ss_comp,&comp[comp_falloff], NULL},
  {"comp_staylift",{&default_comp[comp_staylift], NULL},{0, NULL},0,1,def_bool,ss_comp,&comp[comp_staylift], NULL},
  {"comp_doorstuck",{&default_comp[comp_doorstuck], NULL},{0, NULL},0,1,def_bool,ss_comp,&comp[comp_doorstuck], NULL},
  {"comp_pursuit",{&default_comp[comp_pursuit], NULL},{0, NULL},0,1,def_bool,ss_comp,&comp[comp_pursuit], NULL},
  {"comp_vile",{&default_comp[comp_vile], NULL},{0, NULL},0,1,def_bool,ss_comp,&comp[comp_vile], NULL},
  {"comp_pain",{&default_comp[comp_pain], NULL},{0, NULL},0,1,def_bool,ss_comp,&comp[comp_pain], NULL},
  {"comp_skull",{&default_comp[comp_skull], NULL},{0, NULL},0,1,def_bool,ss_comp,&comp[comp_skull], NULL},
  {"comp_blazing",{&default_comp[comp_blazing], NULL},{0, NULL},0,1,def_bool,ss_comp,&comp[comp_blazing], NULL},
  {"comp_doorlight",{&default_comp[comp_doorlight], NULL},{0, NULL},0,1,def_bool,ss_comp,&comp[comp_doorlight], NULL},
  {"comp_god",{&default_comp[comp_god], NULL},{0, NULL},0,1,def_bool,ss_comp,&comp[comp_god], NULL},
  {"comp_skymap",{&default_comp[comp_skymap], NULL},{0, NULL},0,1,def_bool,ss_comp,&comp[comp_skymap], NULL},
  {"comp_floors",{&default_comp[comp_floors], NULL},{0, NULL},0,1,def_bool,ss_comp,&comp[comp_floors], NULL},
  {"comp_model",{&default_comp[comp_model], NULL},{0, NULL},0,1,def_bool,ss_comp,&comp[comp_model], NULL},
  {"comp_zerotags",{&default_comp[comp_zerotags], NULL},{0, NULL},0,1,def_bool,ss_comp,&comp[comp_zerotags], NULL},
  {"comp_moveblock",{&default_comp[comp_moveblock], NULL},{0, NULL},0,1,def_bool,ss_comp,&comp[comp_moveblock], NULL},
  {"comp_sound",{&default_comp[comp_sound], NULL},{0, NULL},0,1,def_bool,ss_comp,&comp[comp_sound], NULL},
  {"comp_666",{&default_comp[comp_666], NULL},{0, NULL},0,1,def_bool,ss_comp,&comp[comp_666], NULL},
  {"comp_soul",{&default_comp[comp_soul], NULL},{0, NULL},0,1,def_bool,ss_comp,&comp[comp_soul], NULL},
  {"comp_maskedanim",{&default_comp[comp_maskedanim], NULL},{0, NULL},
     0,1,def_bool,ss_comp,&comp[comp_maskedanim], NULL},

  {"Sound settings",{NULL},{0},UL,UL,def_none,ss_none, NULL, NULL},
  {"pitched_sounds",{&pitched_sounds, NULL},{0, NULL},0,1, // killough 2/21/98
   def_bool,ss_gen, NULL, NULL}, // enables variable pitch in sound effects (from id's original code)
  {"samplerate",{&snd_samplerate, NULL},{11025, NULL},11025,48000, def_int,ss_none, NULL, NULL},
  {"sfx_volume",{&snd_SfxVolume, NULL},{8, NULL},0,15, def_int,ss_none, NULL, NULL},
  {"music_volume",{&snd_MusicVolume, NULL},{8, NULL},0,15, def_int,ss_none, NULL, NULL},
  {"mus_pause_opt",{&mus_pause_opt, NULL},{2, NULL},0,2, // CPhipps - music pausing
   def_int, ss_none, NULL, NULL}, // 0 = kill music when paused, 1 = pause music, 2 = let music continue
  {"mus_load_external", {&mus_load_external, NULL},  {2, NULL},0,2,
    def_int,ss_gen, NULL, NULL}, // 0 = never load external music files, 1 = always load it, 2 = only from iwads
  {"snd_channels",{&default_numChannels, NULL},{8, NULL},1,32,
   def_int,ss_gen, NULL, NULL}, // number of audio events simultaneously // killough

  {"Video settings",{NULL, NULL},{0, NULL},UL,UL,def_none,ss_none, NULL, NULL},
  {"screenblocks",{&screenblocks, NULL},{0, NULL},0,1,  // used to be int 3-11 (now just boolean)
   def_int,ss_none, NULL, NULL},
  {"usegamma",{&usegamma, NULL},{0, NULL},0,4, //jff 3/6/98 fix erroneous upper limit in range
   def_int,ss_gen, NULL, NULL}, // gamma correction level // killough 1/18/98
  {"uncapped_framerate", {&movement_smooth, NULL},  {3, NULL},0,14,
   def_int,ss_gen, NULL, NULL},
  {"filter_wall",{(int*)&drawvars.filterwall, NULL},{RDRAW_FILTER_POINT, NULL},
   RDRAW_FILTER_POINT, RDRAW_FILTER_ROUNDED, def_int,ss_gen, NULL, NULL},
  {"filter_floor",{(int*)&drawvars.filterfloor, NULL},{RDRAW_FILTER_POINT, NULL},
   RDRAW_FILTER_POINT, RDRAW_FILTER_ROUNDED, def_int,ss_gen, NULL, NULL},
  {"filter_sprite",{(int*)&drawvars.filtersprite, NULL},{RDRAW_FILTER_POINT, NULL},
   RDRAW_FILTER_POINT, RDRAW_FILTER_ROUNDED, def_int,ss_gen, NULL, NULL},
  {"filter_z",{(int*)&drawvars.filterz, NULL},{RDRAW_FILTER_POINT, NULL},
   RDRAW_FILTER_POINT, RDRAW_FILTER_LINEAR, def_int,ss_gen, NULL, NULL},
  {"filter_patch",{(int*)&drawvars.filterpatch, NULL},{RDRAW_FILTER_POINT, NULL},
   RDRAW_FILTER_POINT, RDRAW_FILTER_ROUNDED, def_int,ss_gen, NULL, NULL},
  {"filter_threshold",{(int*)&drawvars.mag_threshold, NULL},{49152, NULL},
   0, UL, def_int,ss_none, NULL, NULL},
  {"sprite_edges",{(int*)&drawvars.sprite_edges, NULL},{RDRAW_MASKEDCOLUMNEDGE_SQUARE, NULL},
   RDRAW_MASKEDCOLUMNEDGE_SQUARE, RDRAW_MASKEDCOLUMNEDGE_SLOPED, def_int,ss_gen, NULL, NULL},
  {"patch_edges",{(int*)&drawvars.patch_edges, NULL},{RDRAW_MASKEDCOLUMNEDGE_SQUARE, NULL},
   RDRAW_MASKEDCOLUMNEDGE_SQUARE, RDRAW_MASKEDCOLUMNEDGE_SLOPED, def_int,ss_gen, NULL, NULL},
  {"render_stretchsky",{&r_stretchsky, NULL},{1, NULL},0,1,
   def_bool,ss_gen,NULL, NULL},
  {"r_wiggle_fix",{(int*)&r_wiggle_fix, NULL},{1, NULL},0,1,
   def_bool,ss_gen,NULL, NULL},

  {"Mouse settings",{NULL, NULL},{0, NULL},UL,UL,def_none,ss_none, NULL, NULL},
  //jff 4/3/98 allow unlimited sensitivity
  {"mouse_sensitivity_horiz",{&mouseSensitivity_horiz, NULL},{40, NULL},0,UL,
   def_int,ss_none, NULL, NULL}, /* adjust horizontal (x) mouse sensitivity killough/mead */
  //jff 4/3/98 allow unlimited sensitivity
  {"mouse_sensitivity_vert",{&mouseSensitivity_vert, NULL},{40, NULL},0,UL,
   def_int,ss_none, NULL, NULL}, /* adjust vertical (y) mouse sensitivity killough/mead */
  //jff 3/8/98 allow -1 in mouse bindings to disable mouse function
  {"mouseb_fire",{&mousebfire, NULL},{0, NULL},-1,MAX_MOUSEB,
   def_int,ss_keys, NULL, NULL}, // mouse button number to use for fire
  {"mouseb_strafe",{&mousebstrafe, NULL},{1, NULL},-1,MAX_MOUSEB,
   def_int,ss_keys, NULL, NULL}, // mouse button number to use for strafing
  {"mouseb_forward",{&mousebforward, NULL},{2, NULL},-1,MAX_MOUSEB,
   def_int,ss_keys, NULL, NULL}, // mouse button number to use for forward motion
  {"mouseb_backward",{&mousebbackward, NULL},{-1, NULL},-1,MAX_MOUSEB,
   def_int,ss_keys, NULL, NULL}, // mouse button number to use for backward motion
  // Freelook settings
  {"movement_mouselook",{&movement_mouselook, NULL},{0, NULL},0,1,
   def_bool,ss_gen, NULL, NULL}, // enables use of mouselook
  {"movement_mouseinvert",{&movement_mouseinvert, NULL},{0, NULL},0,1,
   def_bool,ss_gen, NULL, NULL}, // whether to invert the mouse vertical
  {"movement_maxviewpitch",{&movement_maxviewpitch, NULL},{32, NULL},0,80,
   def_int,ss_gen, NULL, NULL}, // maximum/minimum pitch when looking up and down

// For key bindings, the values stored in the key_* variables       // phares
// are the internal Doom Codes. The values stored in the default.cfg
// file are the keyboard codes.
// CPhipps - now they're the doom codes, so default.cfg can be portable

  {"Key bindings",{NULL},{0},UL,UL,def_none,ss_none, NULL, NULL},
  {"key_right",       {&key_right, NULL},          {KEYD_RIGHTARROW, NULL},
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to turn right
  {"key_left",        {&key_left, NULL},           {KEYD_LEFTARROW, NULL} ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to turn left
  {"key_up",          {&key_up, NULL},             {KEYD_UPARROW, NULL}   ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to move forward
  {"key_down",        {&key_down, NULL},           {KEYD_DOWNARROW, NULL},
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to move backward
  {"key_menu_right",  {&key_menu_right, NULL},     {KEYD_RIGHTARROW, NULL},// phares 3/7/98
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to move right in a menu  //     |
  {"key_menu_left",   {&key_menu_left, NULL},      {KEYD_LEFTARROW, NULL} ,//     V
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to move left in a menu
  {"key_menu_up",     {&key_menu_up, NULL},        {KEYD_UPARROW, NULL}   ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to move up in a menu
  {"key_menu_down",   {&key_menu_down, NULL},      {KEYD_DOWNARROW, NULL} ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to move down in a menu
  {"key_menu_backspace",{&key_menu_backspace, NULL},{KEYD_BACKSPACE, NULL} ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // delete key in a menu
  {"key_menu_escape", {&key_menu_escape, NULL},    {KEYD_ESCAPE, NULL}    ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to leave a menu      ,   // phares 3/7/98
  {"key_menu_enter",  {&key_menu_enter, NULL},     {KEYD_ENTER, NULL}     ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to select from menu
  {"key_setup",       {&key_setup, NULL},          {KEYD_HOME, NULL},
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, //e6y: key for entering setup menu
  {"key_strafeleft",  {&key_strafeleft, NULL},     {',', NULL}           ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to strafe left
  {"key_straferight", {&key_straferight, NULL},    {'.', NULL}           ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to strafe right

  {"key_fire",        {&key_fire, NULL},           {KEYD_RCTRL, NULL}     ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // duh
  {"key_use",         {&key_use, NULL},            {' ', NULL}           ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to open a door, use a switch
  {"key_strafe",      {&key_strafe, NULL},         {KEYD_RALT, NULL}      ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to use with arrows to strafe
  {"key_speed",       {&key_speed, NULL},          {KEYD_RSHIFT, NULL}    ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to run

  {"key_savegame",    {&key_savegame, NULL},       {KEYD_F2, NULL}        ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to save current game
  {"key_loadgame",    {&key_loadgame, NULL},       {KEYD_F3, NULL}        ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to restore from saved games
  {"key_soundvolume", {&key_soundvolume, NULL},    {KEYD_F4, NULL}        ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to bring up sound controls
  {"key_hud",         {&key_hud, NULL},            {KEYD_F5, NULL}        ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to adjust HUD
  {"key_quicksave",   {&key_quicksave, NULL},      {KEYD_F6, NULL}        ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to to quicksave
  {"key_endgame",     {&key_endgame, NULL},        {KEYD_F7, NULL}        ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to end the game
  {"key_messages",    {&key_messages, NULL},       {KEYD_F8, NULL}        ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to toggle message enable
  {"key_quickload",   {&key_quickload, NULL},      {KEYD_F9, NULL}        ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to load from quicksave
  {"key_quit",        {&key_quit, NULL},           {KEYD_F10, NULL}       ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to quit game
  {"key_gamma",       {&key_gamma, NULL},          {KEYD_F11, NULL}       ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to adjust gamma correction
  {"key_spy",         {&key_spy, NULL},            {KEYD_F12, NULL}       ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to view from another coop player's view
  {"key_pause",       {&key_pause, NULL},          {KEYD_PAUSE, NULL}     ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to pause the game
  {"key_autorun",     {&key_autorun, NULL},        {KEYD_CAPSLOCK, NULL}  ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to toggle always run mode
  {"key_chat",        {&key_chat, NULL},           {'t', NULL}            ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to enter a chat message
  {"key_backspace",   {&key_backspace, NULL},      {KEYD_BACKSPACE, NULL} ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // backspace key
  {"key_enter",       {&key_enter, NULL},          {KEYD_ENTER, NULL}     ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to select from menu or see last message
  {"key_map",         {&key_map, NULL},            {KEYD_TAB, NULL}       ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to toggle automap display
  {"key_map_right",   {&key_map_right, NULL},      {KEYD_RIGHTARROW, NULL},// phares 3/7/98
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to shift automap right   //     |
  {"key_map_left",    {&key_map_left, NULL},       {KEYD_LEFTARROW, NULL} ,//     V
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to shift automap left
  {"key_map_up",      {&key_map_up, NULL},         {KEYD_UPARROW, NULL}   ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to shift automap up
  {"key_map_down",    {&key_map_down, NULL},       {KEYD_DOWNARROW, NULL} ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to shift automap down
  {"key_map_zoomin",  {&key_map_zoomin, NULL},      {'=', NULL}           ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to enlarge automap
  {"key_map_zoomout", {&key_map_zoomout, NULL},     {'-', NULL}           ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to reduce automap
  {"key_map_gobig",   {&key_map_gobig, NULL},       {'0', NULL}           ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL},  // key to get max zoom for automap
  {"key_map_follow",  {&key_map_follow, NULL},      {'f', NULL}           ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to toggle follow mode
  {"key_map_mark",    {&key_map_mark, NULL},        {'m', NULL}           ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to drop a marker on automap
  {"key_map_clear",   {&key_map_clear, NULL},       {'c', NULL}           ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to clear all markers on automap
  {"key_map_grid",    {&key_map_grid, NULL},        {'g', NULL}           ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to toggle grid display over automap
  {"key_map_rotate",  {&key_map_rotate, NULL},      {'r', NULL}           ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to toggle rotating the automap to match the player's orientation
  {"key_map_overlay", {&key_map_overlay, NULL},     {'o', NULL}           ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to toggle overlaying the automap on the rendered display
  {"key_reverse",     {&key_reverse, NULL},         {'/', NULL}           ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to spin 180 instantly
  {"key_zoomin",      {&key_zoomin, NULL},          {'=', NULL}           ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to enlarge display
  {"key_zoomout",     {&key_zoomout, NULL},         {'-', NULL}           ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to reduce display
  {"key_chatplayer1", {&destination_keys[0], NULL}, {'g', NULL}            ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to chat with player 1
  // killough 11/98: fix 'i'/'b' reversal
  {"key_chatplayer2", {&destination_keys[1], NULL}, {'i', NULL}            ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to chat with player 2
  {"key_chatplayer3", {&destination_keys[2], NULL}, {'b', NULL}            ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to chat with player 3
  {"key_chatplayer4", {&destination_keys[3], NULL}, {'r', NULL}            ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to chat with player 4
  {"key_weapontoggle",{&key_weapontoggle, NULL},    {'0', NULL}            ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to toggle between two most preferred weapons with ammo
  {"key_weaponcycleup",{&key_weaponcycleup, NULL},     {'m', NULL}         ,
	  0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to switch to the next weapon
  {"key_weaponcycledown",{&key_weaponcycledown, NULL}, {'n', NULL}         ,
	  0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to switch to the previous weapon
  {"key_weapon1",     {&key_weapon1, NULL},         {'1', NULL}            ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to switch to weapon 1 (fist/chainsaw)
  {"key_weapon2",     {&key_weapon2, NULL},         {'2', NULL}            ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to switch to weapon 2 (pistol)
  {"key_weapon3",     {&key_weapon3, NULL},         {'3', NULL}            ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to switch to weapon 3 (supershotgun/shotgun)
  {"key_weapon4",     {&key_weapon4, NULL},         {'4', NULL}            ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to switch to weapon 4 (chaingun)
  {"key_weapon5",     {&key_weapon5, NULL},         {'5', NULL}            ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to switch to weapon 5 (rocket launcher)
  {"key_weapon6",     {&key_weapon6, NULL},         {'6', NULL}            ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to switch to weapon 6 (plasma rifle)
  {"key_weapon7",     {&key_weapon7, NULL},         {'7', NULL}            ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to switch to weapon 7 (bfg9000)         //    ^
  {"key_weapon8",     {&key_weapon8, NULL},         {'8', NULL}            ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to switch to weapon 8 (chainsaw)        //    |
  {"key_weapon9",     {&key_weapon9, NULL},         {'9', NULL}            ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to switch to weapon 9 (supershotgun)    // phares

  // killough 2/22/98: screenshot key
  {"key_screenshot",  {&key_screenshot, NULL},      {'*', NULL}            ,
   0,MAX_KEY,def_key,ss_keys, NULL, NULL}, // key to take a screenshot
  {"Chat macros",{NULL},{0},UL,UL,def_none,ss_none, NULL, NULL},
  {"chatmacro0", {0,&chat_macros[0]}, {0,HUSTR_CHATMACRO0},UL,UL,
   def_str,ss_chat, NULL, NULL}, // chat string associated with 0 key
  {"chatmacro1", {0,&chat_macros[1]}, {0,HUSTR_CHATMACRO1},UL,UL,
   def_str,ss_chat, NULL, NULL}, // chat string associated with 1 key
  {"chatmacro2", {0,&chat_macros[2]}, {0,HUSTR_CHATMACRO2},UL,UL,
   def_str,ss_chat, NULL, NULL}, // chat string associated with 2 key
  {"chatmacro3", {0,&chat_macros[3]}, {0,HUSTR_CHATMACRO3},UL,UL,
   def_str,ss_chat, NULL, NULL}, // chat string associated with 3 key
  {"chatmacro4", {0,&chat_macros[4]}, {0,HUSTR_CHATMACRO4},UL,UL,
   def_str,ss_chat, NULL, NULL}, // chat string associated with 4 key
  {"chatmacro5", {0,&chat_macros[5]}, {0,HUSTR_CHATMACRO5},UL,UL,
   def_str,ss_chat, NULL, NULL}, // chat string associated with 5 key
  {"chatmacro6", {0,&chat_macros[6]}, {0,HUSTR_CHATMACRO6},UL,UL,
   def_str,ss_chat, NULL, NULL}, // chat string associated with 6 key
  {"chatmacro7", {0,&chat_macros[7]}, {0,HUSTR_CHATMACRO7},UL,UL,
   def_str,ss_chat, NULL, NULL}, // chat string associated with 7 key
  {"chatmacro8", {0,&chat_macros[8]}, {0,HUSTR_CHATMACRO8},UL,UL,
   def_str,ss_chat, NULL, NULL}, // chat string associated with 8 key
  {"chatmacro9", {0,&chat_macros[9]}, {0,HUSTR_CHATMACRO9},UL,UL,
   def_str,ss_chat, NULL, NULL}, // chat string associated with 9 key

  {"Automap settings",{NULL},{0},UL,UL,def_none,ss_none, NULL, NULL},
  //jff 1/7/98 defaults for automap colors
  //jff 4/3/98 remove -1 in lower range, 0 now disables new map features
  {"mapcolor_back", {&mapcolor_back, NULL}, {247, NULL},0,255,  // black //jff 4/6/98 new black
   def_colour,ss_auto, NULL, NULL}, // color used as background for automap
  {"mapcolor_grid", {&mapcolor_grid, NULL}, {104, NULL},0,255,  // dk gray
   def_colour,ss_auto, NULL, NULL}, // color used for automap grid lines
  {"mapcolor_wall", {&mapcolor_wall, NULL}, {23, NULL},0,255,   // red-brown
   def_colour,ss_auto, NULL, NULL}, // color used for one side walls on automap
  {"mapcolor_fchg", {&mapcolor_fchg, NULL}, {55, NULL},0,255,   // lt brown
   def_colour,ss_auto, NULL, NULL}, // color used for lines floor height changes across
  {"mapcolor_cchg", {&mapcolor_cchg, NULL}, {215, NULL},0,255,  // orange
   def_colour,ss_auto, NULL, NULL}, // color used for lines ceiling height changes across
  {"mapcolor_clsd", {&mapcolor_clsd, NULL}, {208, NULL},0,255,  // white
   def_colour,ss_auto, NULL, NULL}, // color used for lines denoting closed doors, objects
  {"mapcolor_rkey", {&mapcolor_rkey, NULL}, {175, NULL},0,255,  // red
   def_colour,ss_auto, NULL, NULL}, // color used for red key sprites
  {"mapcolor_bkey", {&mapcolor_bkey, NULL}, {204, NULL},0,255,  // blue
   def_colour,ss_auto, NULL, NULL}, // color used for blue key sprites
  {"mapcolor_ykey", {&mapcolor_ykey, NULL}, {231, NULL},0,255,  // yellow
   def_colour,ss_auto, NULL, NULL}, // color used for yellow key sprites
  {"mapcolor_rdor", {&mapcolor_rdor, NULL}, {175, NULL},0,255,  // red
   def_colour,ss_auto, NULL, NULL}, // color used for closed red doors
  {"mapcolor_bdor", {&mapcolor_bdor, NULL}, {204, NULL},0,255,  // blue
   def_colour,ss_auto, NULL, NULL}, // color used for closed blue doors
  {"mapcolor_ydor", {&mapcolor_ydor, NULL}, {231, NULL},0,255,  // yellow
   def_colour,ss_auto, NULL, NULL}, // color used for closed yellow doors
  {"mapcolor_tele", {&mapcolor_tele, NULL}, {119, NULL},0,255,  // dk green
   def_colour,ss_auto, NULL, NULL}, // color used for teleporter lines
  {"mapcolor_secr", {&mapcolor_secr, NULL}, {252, NULL},0,255,  // purple
   def_colour,ss_auto, NULL, NULL}, // color used for lines around secret sectors
  {"mapcolor_exit", {&mapcolor_exit, NULL}, {0, NULL},0,255,    // none
   def_colour,ss_auto, NULL, NULL}, // color used for exit lines
  {"mapcolor_unsn", {&mapcolor_unsn, NULL}, {104, NULL},0,255,  // dk gray
   def_colour,ss_auto, NULL, NULL}, // color used for lines not seen without computer map
  {"mapcolor_flat", {&mapcolor_flat, NULL}, {88, NULL},0,255,   // lt gray
   def_colour,ss_auto, NULL, NULL}, // color used for lines with no height changes
  {"mapcolor_sprt", {&mapcolor_sprt, NULL}, {112, NULL},0,255,  // green
   def_colour,ss_auto, NULL, NULL}, // color used as things
  {"mapcolor_item", {&mapcolor_item, NULL}, {231, NULL},0,255,  // yellow
   def_colour,ss_auto, NULL, NULL}, // color used for counted items
  {"mapcolor_hair", {&mapcolor_hair, NULL}, {208, NULL},0,255,  // white
   def_colour,ss_auto, NULL, NULL}, // color used for dot crosshair denoting center of map
  {"mapcolor_sngl", {&mapcolor_sngl, NULL}, {208, NULL},0,255,  // white
   def_colour,ss_auto, NULL, NULL}, // color used for the single player arrow
  {"mapcolor_me",   {&mapcolor_me, NULL}, {112, NULL},0,255, // green
   def_colour,ss_auto, NULL, NULL}, // your (player) colour
  {"mapcolor_enemy",   {&mapcolor_enemy, NULL}, {177, NULL},0,255,
   def_colour,ss_auto, NULL, NULL},
  {"mapcolor_frnd",   {&mapcolor_frnd, NULL}, {112, NULL},0,255,
   def_colour,ss_auto, NULL, NULL},
  //jff 3/9/98 add option to not show secrets til after found
  {"map_secret_after", {&map_secret_after,  NULL}, {0, NULL},0,1, // show secret after gotten
   def_bool,ss_auto, NULL, NULL}, // prevents showing secret sectors till after entered
  {"map_point_coord", {&map_point_coordinates, NULL}, {0, NULL},0,1,
   def_bool,ss_auto, NULL, NULL},
  //jff 1/7/98 end additions for automap
  {"automapmode", {(int*)&automapmode, NULL}, {0, NULL}, 0, 31, // CPhipps - remember automap mode
   def_hex,ss_none, NULL, NULL}, // automap mode

  {"Heads-up display settings",{NULL},{0},UL,UL,def_none,ss_none, NULL, NULL},
  //jff 2/16/98 defaults for color ranges in hud and status
  {"hudcolor_titl", {&hudcolor_titl, NULL}, {5, NULL},0,9,  // gold range
   def_int,ss_auto, NULL, NULL}, // color range used for automap level title
  {"hudcolor_xyco", {&hudcolor_xyco, NULL}, {3, NULL},0,9,  // green range
   def_int,ss_auto, NULL, NULL}, // color range used for automap coordinates
  {"hudcolor_mesg", {&hudcolor_mesg, NULL}, {6, NULL},0,9,  // red range
   def_int,ss_mess, NULL, NULL}, // color range used for messages during play
  {"hudcolor_chat", {&hudcolor_chat, NULL}, {5, NULL},0,9,  // gold range
   def_int,ss_mess, NULL, NULL}, // color range used for chat messages and entry
  {"hudcolor_list", {&hudcolor_list, NULL}, {5, NULL},0,9,  // gold range  //jff 2/26/98
   def_int,ss_mess, NULL, NULL}, // color range used for message review
  {"hud_msg_lines", {&hud_msg_lines, NULL}, {1, NULL},1,16,  // 1 line scrolling window
   def_int,ss_mess, NULL, NULL}, // number of messages in review display (1=disable)
  {"hud_list_bgon", {&hud_list_bgon, NULL}, {0, NULL},0,1,  // solid window bg ena //jff 2/26/98
   def_bool,ss_mess, NULL, NULL}, // enables background window behind message review

  {"health_red",    {&health_red, NULL}   , {25, NULL},0,200, // below is red
   def_int,ss_stat, NULL, NULL}, // amount of health for red to yellow transition
  {"health_yellow", {&health_yellow, NULL}, {50, NULL},0,200, // below is yellow
   def_int,ss_stat, NULL, NULL}, // amount of health for yellow to green transition
  {"health_green",  {&health_green, NULL} , {100, NULL},0,200,// below is green, above blue
   def_int,ss_stat, NULL, NULL}, // amount of health for green to blue transition
  {"armor_red",     {&armor_red, NULL}    , {25, NULL},0,200, // below is red
   def_int,ss_stat, NULL, NULL}, // amount of armor for red to yellow transition
  {"armor_yellow",  {&armor_yellow, NULL} , {50, NULL},0,200, // below is yellow
   def_int,ss_stat, NULL, NULL}, // amount of armor for yellow to green transition
  {"armor_green",   {&armor_green, NULL}  , {100, NULL},0,200,// below is green, above blue
   def_int,ss_stat, NULL, NULL}, // amount of armor for green to blue transition
  {"ammo_red",      {&ammo_red, NULL}     , {25, NULL},0,100, // below 25% is red
   def_int,ss_stat, NULL, NULL}, // percent of ammo for red to yellow transition
  {"ammo_yellow",   {&ammo_yellow, NULL}  , {50, NULL},0,100, // below 50% is yellow, above green
   def_int,ss_stat, NULL, NULL}, // percent of ammo for yellow to green transition

  // HUD and status feature controls
  {"hud_mode", {(int*)&hud_mode, NULL},  {0, NULL},0,2, // whether & how hud is displayed
   def_int,ss_none, NULL, NULL}, // enables and selects display mode of HUD
  {"hud_showstats", {&hud_showstats, NULL},  {1, NULL},0,1, // show secrets/items/kills HUD line
   def_bool,ss_stat, NULL, NULL}, // enables display of kills/items/secrets on HUD
  {"hud_showkeys", {&hud_showkeys, NULL},  {1, NULL},0,1, // show keys HUD line
   def_bool,ss_stat, NULL, NULL}, // enables the display of keys on HUD
  {"hud_showweapons", {&hud_showweapons, NULL},  {1, NULL},0,1, // show weapons HUD line
   def_bool,ss_stat, NULL, NULL}, // enables the display of weapons on HUD

  {"Weapon preferences",{NULL},{0},UL,UL,def_none,ss_none, NULL, NULL},
  // killough 2/8/98: weapon preferences set by user:
  {"weapon_choice_1", {&weapon_preferences[0][0], NULL}, {6, NULL}, 0,9,
   def_int,ss_weap, NULL, NULL}, // first choice for weapon (best)
  {"weapon_choice_2", {&weapon_preferences[0][1], NULL}, {9, NULL}, 0,9,
   def_int,ss_weap, NULL, NULL}, // second choice for weapon
  {"weapon_choice_3", {&weapon_preferences[0][2], NULL}, {4, NULL}, 0,9,
   def_int,ss_weap, NULL, NULL}, // third choice for weapon
  {"weapon_choice_4", {&weapon_preferences[0][3], NULL}, {3, NULL}, 0,9,
   def_int,ss_weap, NULL, NULL}, // fourth choice for weapon
  {"weapon_choice_5", {&weapon_preferences[0][4], NULL}, {2, NULL}, 0,9,
   def_int,ss_weap, NULL, NULL}, // fifth choice for weapon
  {"weapon_choice_6", {&weapon_preferences[0][5], NULL}, {8, NULL}, 0,9,
   def_int,ss_weap, NULL, NULL}, // sixth choice for weapon
  {"weapon_choice_7", {&weapon_preferences[0][6], NULL}, {5, NULL}, 0,9,
   def_int,ss_weap, NULL, NULL}, // seventh choice for weapon
  {"weapon_choice_8", {&weapon_preferences[0][7], NULL}, {7, NULL}, 0,9,
   def_int,ss_weap, NULL, NULL}, // eighth choice for weapon
  {"weapon_choice_9", {&weapon_preferences[0][8], NULL}, {1, NULL}, 0,9,
   def_int,ss_weap, NULL, NULL}, // ninth choice for weapon (worst)

// cournia - support for arbitrary music file (defaults are mp3)

	{"Music", {NULL},{0},UL,UL,def_none,ss_none, NULL, NULL},

	//Fixed support for Doom, Doom 2, and Doom Final
	//You must use a seperate folder for each wad to be able to have all Dooms use their correct music
	//In ~/RetroPie/roms/ports/ have a folder for each game, put the wad of each game in their related folder
	//Then put all the music in their folder
	//Also don't forget to include the prboom.wad into each game folder

	//An example folder structure would be like so
	// roms
	// +ports
	// ++doom
	// +++doom.wad
	// +++prboom.wad
	// +++doomusic.mp3
	// ++doom2
	// +++doom2.wad
	// +++prboom.wad
	// +++doom2music.mp3

	//An example launcher script in ~/RetroPie/roms/ports/*.sh would be like this
	// #!/bin/bash
	// /opt/retropie/supplementary/runcommand/runcommand.sh 0 "/opt/retropie/emulators/retroarch/bin/retroarch -L /opt/retropie/libretrocores/lr-prboom/prboom_libretro.so --config /opt/retropie/configs/doom/retroarch.cfg /home/pi/RetroPie/roms/ports/doom2/doom2.wad" "lr-prboom"
	//You must change the folder name and wad name for each game you want and save it as a .sh file in ~/RetroPie/roms/ports/

	//Although all 3 games (Doom 2, Plutonia, TNT) use the same file names, the files are not the same

	//Begin Doom 1
	{"mus_bunny", {0,&S_music_files[mus_bunny]}, {0,"bunny.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e1m1", {0,&S_music_files[mus_e1m1]}, {0,"e1m1.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e1m2", {0,&S_music_files[mus_e1m2]}, {0,"e1m2.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e1m3", {0,&S_music_files[mus_e1m3]}, {0,"e1m3.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e1m4", {0,&S_music_files[mus_e1m4]}, {0,"e1m4.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e1m5", {0,&S_music_files[mus_e1m5]}, {0,"e1m5.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e1m6", {0,&S_music_files[mus_e1m6]}, {0,"e1m6.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e1m7", {0,&S_music_files[mus_e1m7]}, {0,"e1m7.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e1m8", {0,&S_music_files[mus_e1m8]}, {0,"e1m8.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e1m9", {0,&S_music_files[mus_e1m9]}, {0,"e1m9.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e2m1", {0,&S_music_files[mus_e2m1]}, {0,"e2m1.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e2m2", {0,&S_music_files[mus_e2m2]}, {0,"e2m2.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e2m3", {0,&S_music_files[mus_e2m3]}, {0,"e2m3.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e2m4", {0,&S_music_files[mus_e2m4]}, {0,"e2m4.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e2m5", {0,&S_music_files[mus_e2m5]}, {0,"e1m5.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e2m6", {0,&S_music_files[mus_e2m6]}, {0,"e2m6.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e2m7", {0,&S_music_files[mus_e2m7]}, {0,"e2m7.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e2m8", {0,&S_music_files[mus_e2m8]}, {0,"e2m8.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e2m9", {0,&S_music_files[mus_e2m9]}, {0,"e2m9.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e3m1", {0,&S_music_files[mus_e3m1]}, {0,"e3m1.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e3m2", {0,&S_music_files[mus_e3m2]}, {0,"e3m2.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e3m3", {0,&S_music_files[mus_e3m3]}, {0,"e3m3.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e3m4", {0,&S_music_files[mus_e3m4]}, {0,"e3m4.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e3m5", {0,&S_music_files[mus_e3m5]}, {0,"e3m5.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e3m6", {0,&S_music_files[mus_e3m6]}, {0,"e3m6.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e3m7", {0,&S_music_files[mus_e3m7]}, {0,"e3m7.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e3m8", {0,&S_music_files[mus_e3m8]}, {0,"e3m8.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e3m9", {0,&S_music_files[mus_e3m9]}, {0,"e3m9.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e4m1", {0,&S_music_files[mus_e4m1]}, {0,"e3m4.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e4m2", {0,&S_music_files[mus_e4m2]}, {0,"e3m2.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e4m3", {0,&S_music_files[mus_e4m3]}, {0,"e3m3.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e4m4", {0,&S_music_files[mus_e4m4]}, {0,"e1m5.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e4m5", {0,&S_music_files[mus_e4m5]}, {0,"e2m7.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e4m6", {0,&S_music_files[mus_e4m6]}, {0,"e2m4.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e4m7", {0,&S_music_files[mus_e4m7]}, {0,"e2m6.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e4m8", {0,&S_music_files[mus_e4m8]}, {0,"e2m5.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e4m9", {0,&S_music_files[mus_e4m9]}, {0,"e1m9.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e5m1", {0,&S_music_files[mus_e5m1]}, {0,"e5m1.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e5m2", {0,&S_music_files[mus_e5m2]}, {0,"e5m2.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e5m3", {0,&S_music_files[mus_e5m3]}, {0,"e5m3.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e5m4", {0,&S_music_files[mus_e5m4]}, {0,"e5m4.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e5m5", {0,&S_music_files[mus_e5m5]}, {0,"e5m5.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e5m6", {0,&S_music_files[mus_e5m6]}, {0,"e5m6.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e5m7", {0,&S_music_files[mus_e5m7]}, {0,"e5m7.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e5m8", {0,&S_music_files[mus_e5m8]}, {0,"e5m8.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_e5m9", {0,&S_music_files[mus_e5m9]}, {0,"e5m9.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_inter", {0,&S_music_files[mus_inter]}, {0,"inter.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_introa", {0,&S_music_files[mus_introa]}, {0,"intro.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_intro", {0,&S_music_files[mus_intro]}, {0,"intro.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_victor", {0,&S_music_files[mus_victor]}, {0,"victor.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	//End Doom 1
	//Begin Doom 2 & Final Doom
	{"mus_adrian", {0,&S_music_files[mus_adrian]}, {0,"adrian.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_ampie", {0,&S_music_files[mus_ampie]}, {0,"ampie.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_betwee", {0,&S_music_files[mus_betwee]}, {0,"betwee.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_count2", {0,&S_music_files[mus_count2]}, {0,"count2.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_countd", {0,&S_music_files[mus_countd]}, {0,"countd.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_ddtbl2", {0,&S_music_files[mus_ddtbl2]}, {0,"ddtbl2.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_ddtbl3", {0,&S_music_files[mus_ddtbl3]}, {0,"ddtbl3.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_ddtblu", {0,&S_music_files[mus_ddtblu]}, {0,"ddtblu.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_dead2", {0,&S_music_files[mus_dead2]}, {0,"dead2.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_dead", {0,&S_music_files[mus_dead]}, {0,"dead.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_dm2int", {0,&S_music_files[mus_dm2int]}, {0,"dm2int.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_dm2ttl", {0,&S_music_files[mus_dm2ttl]}, {0,"dm2ttl.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_doom2", {0,&S_music_files[mus_doom2]}, {0,"doom2.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_doom", {0,&S_music_files[mus_doom]}, {0,"doom.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_evil", {0,&S_music_files[mus_evil]}, {0,"evil.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_in_cit", {0,&S_music_files[mus_in_cit]}, {0,"in_cit.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_messag", {0,&S_music_files[mus_messag]}, {0,"messag.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_messg2", {0,&S_music_files[mus_messg2]}, {0,"messg2.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_openin", {0,&S_music_files[mus_openin]}, {0,"openin.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_read_m", {0,&S_music_files[mus_read_m]}, {0,"read_m.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_romer2", {0,&S_music_files[mus_romer2]}, {0,"romer2.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_romero", {0,&S_music_files[mus_romero]}, {0,"romero.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_runni2", {0,&S_music_files[mus_runni2]}, {0,"runni2.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_runnin", {0,&S_music_files[mus_runnin]}, {0,"runnin.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_shawn2", {0,&S_music_files[mus_shawn2]}, {0,"shawn2.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_shawn3", {0,&S_music_files[mus_shawn3]}, {0,"shawn3.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_shawn", {0,&S_music_files[mus_shawn]}, {0,"shawn.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_stalks", {0,&S_music_files[mus_stalks]}, {0,"stalks.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_stlks2", {0,&S_music_files[mus_stlks2]}, {0,"stlks2.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_stlks3", {0,&S_music_files[mus_stlks3]}, {0,"stlks3.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_tense", {0,&S_music_files[mus_tense]}, {0,"tense.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_theda2", {0,&S_music_files[mus_theda2]}, {0,"theda2.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_theda3", {0,&S_music_files[mus_theda3]}, {0,"theda3.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_the_da", {0,&S_music_files[mus_the_da]}, {0,"the_da.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	{"mus_ultima", {0,&S_music_files[mus_ultima]}, {0,"ultima.mp3"},UL,UL,
   def_str,ss_none, NULL, NULL},
	//End Doom 2 and Final Doom
};

int numdefaults;
static char *defaultfile;

//
// M_SaveDefaults
//

void M_SaveDefaults (void)
{
  int   i;
  FILE* f;

  f = fopen (defaultfile, "w");
  if (!f)
    return; // can't write the file, but don't complain

  // 3/3/98 explain format of file

  fprintf(f,"# Doom config file\n"
          "#\n"
          "# Format:\n"
          "#  variable   value\n"
          "#\n"
          "# Lines starting with '#' are comments\n"
          "# When saved, default values are commented out\n");

  for (i = 0 ; i < numdefaults ; i++) {
    if (defaults[i].type == def_none) {
      // CPhipps - pure headers
      fprintf(f, "\n## %s\n", defaults[i].name);
    } else {
      // CPhipps - modified for new default_t form
      if (!IS_STRING(defaults[i])) //jff 4/10/98 kill super-hack on pointer value
      {
        // CPhipps - remove keycode hack
        // killough 3/6/98: use spaces instead of tabs for uniform justification
        if (defaults[i].type == def_hex)
          fprintf (f,"%s%-25s 0x%x\n",
                   (defaults[i].defaultvalue.i == *(defaults[i].location.pi))?"#":"",
                   defaults[i].name,*(defaults[i].location.pi));
        else
          fprintf (f,"%s%-25s %5i\n",
                   (defaults[i].defaultvalue.i == *(defaults[i].location.pi))?"#":"",
                   defaults[i].name,*(defaults[i].location.pi));
      }
      else
      {
        fprintf (f,"%s%-25s \"%s\"\n",
                 (strcmp(defaults[i].defaultvalue.psz,*(defaults[i].location.ppsz)) == 0)?"#":"",
                 defaults[i].name,*(defaults[i].location.ppsz));
      }
    }
  }

  fclose (f);
}

/*
 * M_LookupDefault
 *
 * cph - mimic MBF function for now. Yes it's crap.
 */

struct default_s *M_LookupDefault(const char *name)
{
  int i;
  for (i = 0 ; i < numdefaults ; i++)
    if ((defaults[i].type != def_none) && !strcmp(name, defaults[i].name))
      return &defaults[i];
  I_Error("M_LookupDefault: %s not found",name);
  return NULL;
}

#define NUMCHATSTRINGS 10 // phares 4/13/98

/*
 * M_LoadDefaultsFile
 *
 * Load configuration file
 */

void M_LoadDefaultsFile (char *file, boolean basedefault)
{
  int   i;
  int   len;
  FILE* f;
  char  def[80];
  char  strparm[100];
  char* newstring = NULL;   // killough
  int   parm;
  boolean isstring;

  // read the file in, overriding any set defaults
  f = fopen (file, "r");
  if (f)
  {
    while (!feof(f))
    {
      isstring = FALSE;
      if (fscanf (f, "%79s %[^\n]\n", def, strparm) == 2)
      {
        //jff 3/3/98 skip lines not starting with an alphanum

        if (!isalnum(def[0]))
          continue;

        if (strparm[0] == '"') {
          // get a string default

          isstring = TRUE;
          len = strlen(strparm);
          newstring = (char *) malloc(len);
          strparm[len-1] = 0; // clears trailing double-quote mark
          strcpy(newstring, strparm+1); // clears leading double-quote mark
        } else if ((strparm[0] == '0') && (strparm[1] == 'x')) {
          // CPhipps - allow ints to be specified in hex
          sscanf(strparm+2, "%x", &parm);
        } else {
          sscanf(strparm, "%i", &parm);
          // Keycode hack removed
        }

        for (i = 0 ; i < numdefaults ; i++)
          if ((defaults[i].type != def_none) && !strcmp(def, defaults[i].name))
          {
            // CPhipps - safety check
            if (isstring != IS_STRING(defaults[i])) {
              lprintf(LO_WARN, "M_LoadDefaults: Type mismatch reading %s\n", defaults[i].name);
              continue;
            }
            if (!isstring)
            {
              //jff 3/4/98 range check numeric parameters
              if ((defaults[i].minvalue==UL || defaults[i].minvalue<=parm) &&
                  (defaults[i].maxvalue==UL || defaults[i].maxvalue>=parm))
              {
                *(defaults[i].location.pi) = parm;
                if(basedefault)
                  defaults[i].defaultvalue.i = parm;
              }
            }
            else
            {
              union { const char **c; char **s; } u; // type punning via unions

              u.c = defaults[i].location.ppsz;
              free(*(u.s));
              *(u.s) = newstring;

              if(basedefault)
                defaults[i].defaultvalue.psz = strdup(newstring);
            }
            break;
          }
      }
    }
    fclose (f);
  }
  //jff 3/4/98 redundant range checks for hud deleted here
}

//
// M_LoadDefaults
//

void M_LoadDefaults (void)
{
  int   i;
  char* basefile;

  // set everything to base values

  numdefaults = sizeof(defaults)/sizeof(defaults[0]);
  for (i = 0 ; i < numdefaults ; i++) {
    if (defaults[i].location.ppsz)
      *defaults[i].location.ppsz = strdup(defaults[i].defaultvalue.psz);
    if (defaults[i].location.pi)
      *defaults[i].location.pi = defaults[i].defaultvalue.i;
  }


  // check if a base default file was provided, to load as base values

  i = M_CheckParm ("-baseconfig");
  if (i && i < myargc-1)
  {
    basefile = strdup(myargv[i+1]);
    lprintf (LO_CONFIRM, " default file with base values: %s\n", basefile);
    M_LoadDefaultsFile(basefile, TRUE);
  }

  // check for a custom default file

  i = M_CheckParm ("-config");
  if (i && i < myargc-1)
    defaultfile = strdup(myargv[i+1]);
  else {
    const char* exedir = I_DoomExeDir();
    defaultfile = malloc(PATH_MAX+1);
    /* get config file from same directory as executable */
    snprintf(defaultfile, PATH_MAX,
            "%s%s%sboom.cfg", exedir,
            HasTrailingSlash(exedir) ? "" : DIR_SLASH_STR,
            "pr");
  }

  lprintf (LO_CONFIRM, " default file: %s\n",defaultfile);

  M_LoadDefaultsFile(defaultfile, FALSE);
}
