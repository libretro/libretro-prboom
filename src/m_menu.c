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
 *  DOOM selection menu, options, episode etc. (aka Big Font menus)
 *  Sliders and icons. Kinda widget stuff.
 *  Setup Menus.
 *  Extended HELP screens.
 *  Dynamic HELP screen.
 *
 *-----------------------------------------------------------------------------*/

#include <stdlib.h>
#include <fcntl.h>

#include "doomdef.h"
#include "doomstat.h"
#include "dstrings.h"
#include "d_main.h"
#include "v_video.h"
#include "w_wad.h"
#include "r_main.h"
#include "r_sky.h"
#include "hu_stuff.h"
#include "g_game.h"
#include "s_sound.h"
#include "sounds.h"
#include "m_menu.h"
#include "d_deh.h"
#include "m_misc.h"
#include "lprintf.h"
#include "am_map.h"
#include "i_main.h"
#include "i_system.h"
#include "i_video.h"
#include "i_sound.h"
#include "r_demo.h"
#include "r_fps.h"

#include <libretro.h>
#include <streams/file_stream.h>

/* Don't include file_stream_transforms.h but instead
just forward declare the prototype */
int64_t rfread(void* buffer,
   size_t elem_size, size_t elem_count, RFILE* stream);

extern patchnum_t hu_font[HU_FONTSIZE];
extern dbool  message_dontfuckwithme;

extern dbool chat_on;          // in heads-up code
extern int has_exited;

extern angle_t viewpitch_max;
extern angle_t viewpitch_min;

//
// defaulted values
//

int mouseSensitivity_horiz; // has default   //  killough
int mouseSensitivity_vert;  // has default

// Freelook settings
dbool movement_mouselook;
dbool movement_mouseinvert;
int     movement_maxviewpitch;

int showMessages;    // Show messages has default, 0 = off, 1 = on

int hide_setup=1; // killough 5/15/98

// Blocky mode, has default, 0 = high, 1 = normal
//int     detailLevel;    obsolete -- killough
int screenblocks;    // has default

int quickSaveSlot;   // -1 = no quicksave slot picked!

int messageToPrint;  // 1 = message to be printed

// CPhipps - static const
static const char* messageString; // ...and here is the message string!

// message x & y
int     messx;
int     messy;
int     messageLastMenuActive;

dbool messageNeedsInput; // timed message = no input from user

void (*messageRoutine)(int response);

#define SAVESTRINGSIZE  24

/* killough 8/15/98: when changes are allowed to sync-critical variables */
static int allow_changes(void)
{
 return !(demoplayback || netgame);
}

static void M_UpdateCurrent(default_t* def)
{
  /* cph - requires rewrite of m_misc.c */
  if (def->current) {
    if (allow_changes())  /* killough 8/15/98 */
  *def->current = *def->location.pi;
    else if (*def->current != *def->location.pi)
  warn_about_changes(S_LEVWARN); /* killough 8/15/98 */
  }
}

int warning_about_changes, print_warning_about_changes;

/* cphipps - M_DrawBackground renamed and moved to v_video.c */

dbool menu_background = 1; // do Boom fullscreen menus have backgrounds?

static void M_DrawBackground(const char *flat, int scrn)
{
  if (menu_background)
    V_DrawBackground(flat, scrn);
}

// we are going to be entering a savegame string

int saveStringEnter;
int saveSlot;        // which slot to save in
int saveCharIndex;   // which char we're editing
// old save description before edit
char saveOldString[SAVESTRINGSIZE];

dbool inhelpscreens; // indicates we are in or just left a help screen

enum menuactive_e menuactive;    // The menus are up

#define SKULLXOFF  -32
#define LINEHEIGHT  16

char savegamestrings[10][SAVESTRINGSIZE];

//
// MENU TYPEDEFS
//

typedef struct
{
  short status; // 0 = no cursor here, 1 = ok, 2 = arrows ok
  char  name[10];

  // choice = menu item #.
  // if status = 2,
  //   choice=0:leftarrow,1:rightarrow
  void  (*routine)(int choice);
  char  alphaKey; // hotkey in menu
  const char *alttext;
} menuitem_t;

typedef struct menu_s
{
  short           numitems;     // # of menu items
  struct menu_s*  prevMenu;     // previous menu
  menuitem_t*     menuitems;    // menu items
  void            (*routine)(); // draw routine
  short           x;
  short           y;            // x,y of menu
  short           lastOn;       // last item user was on in menu
} menu_t;

short itemOn;           // menu item skull is on (for Big Font menus)
short whichSkull;       // which skull to draw (he blinks)

// graphic name of skulls

const char skullName[2][/*8*/9] = {"M_SKULL1","M_SKULL2"};

menu_t* currentMenu; // current menudef

// phares 3/30/98
// externs added for setup menus

int mapcolor_me;    // cph

extern int map_point_coordinates; // killough 10/98

extern char* chat_macros[];  // chat macros
extern const char* shiftxform;
extern default_t defaults[];
extern int numdefaults;

// end of externs added for setup menus

//
// PROTOTYPES
//
void M_NewGame(int choice);
void M_Episode(int choice);
void M_ChooseSkill(int choice);
void M_LoadGame(int choice);
void M_SaveGame(int choice);
void M_Options(int choice);
void M_EndGame(int choice);
void M_ReadThis(int choice);
void M_ReadThis2(int choice);
void M_QuitDOOM(int choice);

void M_ChangeMessages(int choice);
void M_ChangeSensitivity(int choice);
void M_SfxVol(int choice);
void M_MusicVol(int choice);
/* void M_ChangeDetail(int choice);  unused -- killough */
void M_SizeDisplay(int choice);
void M_StartGame(int choice);
void M_Sound(int choice);

void M_Mouse(int choice, int *sens);      /* killough */
void M_MouseVert(int choice);
void M_MouseHoriz(int choice);
void M_DrawMouse(void);

void M_FinishReadThis(int choice);
void M_FinishHelp(int choice);            // killough 10/98
void M_LoadSelect(int choice);
void M_SaveSelect(int choice);
void M_ReadSaveStrings(void);
void M_QuickSave(void);
void M_QuickLoad(void);

void M_DrawMainMenu(void);
void M_DrawReadThis1(void);
void M_DrawReadThis2(void);
void M_DrawNewGame(void);
void M_DrawEpisode(void);
void M_DrawOptions(void);
void M_DrawSound(void);
void M_DrawLoad(void);
void M_DrawSave(void);
void M_DrawSetup(void);                                     // phares 3/21/98
void M_DrawHelp (void);                                     // phares 5/04/98

void M_DrawSaveLoadBorder(int x,int y);
void M_SetupNextMenu(menu_t *menudef);
void M_DrawThermo(int x,int y,int thermWidth,int thermDot);
void M_DrawEmptyCell(menu_t *menu,int item);
void M_DrawSelCell(menu_t *menu,int item);
void M_WriteText(int x, int y, const char *string, int cm);
int  M_StringWidth(const char *string);
int  M_StringHeight(const char *string);
void M_DrawTitle(int x, int y, const char *patch, int cm,
                 const char *alttext, int altcm);
void M_StartMessage(const char *string,void *routine,dbool input);
void M_StopMessage(void);
void M_ClearMenus (void);

// phares 3/30/98
// prototypes added to support Setup Menus and Extended HELP screens

int  M_GetKeyString(int,int);
void M_Setup(int choice);
void M_KeyBindings(int choice);
void M_Weapons(int);
void M_StatusBar(int);
void M_Automap(int);
void M_Enemy(int);
void M_Messages(int);
void M_ChatStrings(int);
void M_InitExtendedHelp(void);
void M_ExtHelpNextScreen(int);
void M_ExtHelp(int);
static int M_GetPixelWidth(const char*);
void M_DrawKeybnd(void);
void M_DrawWeapons(void);
static void M_DrawMenuString(int,int,int);
static void M_DrawStringCentered(int,int,int,const char*);
void M_DrawStatusHUD(void);
void M_DrawExtHelp(void);
void M_DrawAutoMap(void);
void M_DrawEnemy(void);
void M_DrawMessages(void);
void M_DrawChatStrings(void);
void M_Compat(int);       // killough 10/98
void M_ChangeDemoSmoothTurns(void);
void M_ChangeFramerate(void);
void M_ChangeMouseLook(void);
void M_ChangeMaxViewPitch(void);
void M_General(int);      // killough 10/98
void M_DrawCompat(void);  // killough 10/98
void M_DrawGeneral(void); // killough 10/98

menu_t NewDef;                                              // phares 5/04/98

// end of prototypes added to support Setup Menus and Extended HELP screens

/////////////////////////////////////////////////////////////////////////////
//
// DOOM MENUS
//

/////////////////////////////
//
// MAIN MENU
//

// main_e provides numerical values for which Big Font screen you're on

enum
{
  newgame = 0,
  loadgame,
  savegame,
  options,
  readthis,
  quitdoom,
  main_end
} main_e;

//
// MainMenu is the definition of what the main menu Screen should look
// like. Each entry shows that the cursor can land on each item (1), the
// built-in graphic lump (i.e. "M_NGAME") that should be displayed,
// the program which takes over when an item is selected, and the hotkey
// associated with the item.
//

menuitem_t MainMenu[]=
{
  {1,"M_NGAME", M_NewGame, 'n', "New Game"},
  {1,"M_OPTION",M_Options, 'o', "Options"},
  {1,"M_LOADG", M_LoadGame,'l', "Load Game"},
  {1,"M_SAVEG", M_SaveGame,'s', "Save Game"},
  // Another hickup with Special edition.
  {1,"M_RDTHIS",M_ReadThis,'r', "Read This!"},
  {1,"M_QUITG", M_QuitDOOM,'q', "Quit Game"}
};

menu_t MainDef =
{
  main_end,       // number of menu items
  NULL,           // previous menu screen
  MainMenu,       // table that defines menu items
  M_DrawMainMenu, // drawing routine
  97,64,          // initial cursor position
  0               // last menu item the user was on
};

//
// M_DrawMainMenu
//

void M_DrawMainMenu(void)
{
  // CPhipps - patch drawing updated
  V_DrawNamePatch(94, 2, 0, "M_DOOM", CR_DEFAULT, VPT_STRETCH);
}

/////////////////////////////
//
// Read This! MENU 1 & 2
//

// There are no menu items on the Read This! screens, so read_e just
// provides a placeholder to maintain structure.

enum
{
  rdthsempty1,
  read1_end
} read_e;

enum
{
  rdthsempty2,
  read2_end
} read_e2;

enum               // killough 10/98
{
  helpempty,
  help_end
} help_e;


// The definitions of the Read This! screens

menuitem_t ReadMenu1[] =
{
  {1,"",M_ReadThis2,0,NULL}
};

menuitem_t ReadMenu2[]=
{
  {1,"",M_FinishReadThis,0,NULL}
};

menuitem_t HelpMenu[]=    // killough 10/98
{
  {1,"",M_FinishHelp,0,NULL}
};

menu_t ReadDef1 =
{
  read1_end,
  &MainDef,
  ReadMenu1,
  M_DrawReadThis1,
  330,175,
  //280,185,              // killough 2/21/98: fix help screens
  0
};

menu_t ReadDef2 =
{
  read2_end,
  &ReadDef1,
  ReadMenu2,
  M_DrawReadThis2,
  330,175,
  0
};

menu_t HelpDef =           // killough 10/98
{
  help_end,
  &HelpDef,
  HelpMenu,
  M_DrawHelp,
  330,175,
  0
};

//
// M_ReadThis
//

void M_ReadThis(int choice)
{
  M_SetupNextMenu(&ReadDef1);
}

void M_ReadThis2(int choice)
{
  M_SetupNextMenu(&ReadDef2);
}

void M_FinishReadThis(int choice)
{
  M_SetupNextMenu(&MainDef);
}

void M_FinishHelp(int choice)        // killough 10/98
{
  M_SetupNextMenu(&MainDef);
}

//
// Read This Menus
// Had a "quick hack to fix romero bug"
//
// killough 10/98: updated with new screens

void M_DrawReadThis1(void)
{
  inhelpscreens = TRUE;
  if (gamemode == shareware)
    V_DrawNamePatch(0, 0, 0, "HELP2", CR_DEFAULT, VPT_STRETCH);
  else
    M_DrawCredits();
}

//
// Read This Menus - optional second page.
//
// killough 10/98: updated with new screens

void M_DrawReadThis2(void)
{
  inhelpscreens = TRUE;
  if (gamemode == shareware)
    M_DrawCredits();
  else
    V_DrawNamePatch(0, 0, 0, "CREDIT", CR_DEFAULT, VPT_STRETCH);
}

/////////////////////////////
//
// EPISODE SELECT
//

// The definitions of the Episodes menu

menuitem_t EpisodeMenu[]=
{
  {1,"M_EPI1", M_Episode,'k',"Episode 1"},
  {1,"M_EPI2", M_Episode,'t',"Episode 2"},
  {1,"M_EPI3", M_Episode,'i',"Episode 3"},
  {1,"M_EPI4", M_Episode,'t',"Episode 4"},
  {1,"M_EPI5", M_Episode,'s',"Episode 5"},
  // Some extra empty episodes for extensibility through UMAPINFO
  {1,"M_EPI6", M_Episode,'6',"Episode 6"},
  {1,"M_EPI7", M_Episode,'7',"Episode 7"},
  {1,"M_EPI8", M_Episode,'8',"Episode 8"}
};

menu_t EpiDef =
{
  0,             // # of menu items ( will be set in M_Init )
  &MainDef,      // previous menu
  EpisodeMenu,   // menuitem_t ->
  M_DrawEpisode, // drawing routine ->
  48,63,         // x,y
  0              // lastOn
};

// This is for customized episode menus
int EpiCustom;
short EpiMenuEpi[8], EpiMenuMap[8];

//
//    M_Episode
//
int epi;

void M_AddEpisode(const char *map, char *def)
{
  if (!EpiCustom) {
     EpiCustom = true;
     // No more than 4 Eps expected when having UMAPINFO (prevent SIGILv1.2 from showing twice)
     if (EpiDef.numitems > 4)
        EpiDef.numitems = 4;
  }
  if (*def == '-')	// means 'clear'
  {
    EpiDef.numitems = 0;
  }
  else
  {
    int episodenum, mapnum;
    const char *gfx = strtok(def, "\n");
    const char *txt = strtok(NULL, "\n");
    const char *alpha = strtok(NULL, "\n");
    if (EpiDef.numitems >= 8)
       return;
    G_ValidateMapName(map, &episodenum, &mapnum);
    EpiMenuEpi[EpiDef.numitems] = episodenum;
    EpiMenuMap[EpiDef.numitems] = mapnum;
    strncpy(EpisodeMenu[EpiDef.numitems].name, gfx, 8);
    EpisodeMenu[EpiDef.numitems].name[8] = 0;
    EpisodeMenu[EpiDef.numitems].alttext = txt;
    EpisodeMenu[EpiDef.numitems].alphaKey = alpha ? *alpha : 0;
    EpiDef.numitems++;
  }
  if (EpiDef.numitems <= 4)
  {
    EpiDef.y = 63;
  }
  else
  {
    EpiDef.y = 63 - (EpiDef.numitems - 4) * (LINEHEIGHT / 2);
  }
}

void M_DrawEpisode(void)
{
  // CPhipps - patch drawing updated
  V_DrawNamePatch(54, EpiDef.y - 25, 0, "M_EPISOD", CR_DEFAULT, VPT_STRETCH);
}

void M_Episode(int choice)
{
  if ( (gamemode == shareware) && choice) {
    M_StartMessage(s_SWSTRING,NULL,FALSE); // Ty 03/27/98 - externalized
    M_SetupNextMenu(&ReadDef1);
    return;
  }

  epi = choice;
  M_SetupNextMenu(&NewDef);
}

/////////////////////////////
//
// NEW GAME
//

// numerical values for the New Game menu items

enum
{
  killthings,
  toorough,
  hurtme,
  violence,
  nightmare,
  newg_end
} newgame_e;

// The definitions of the New Game menu

menuitem_t NewGameMenu[]=
{
  {1,"M_JKILL", M_ChooseSkill, 'i', "I'm too young to die."},
  {1,"M_ROUGH", M_ChooseSkill, 'h', "Hey, not too rough."},
  {1,"M_HURT",  M_ChooseSkill, 'h', "Hurt me plenty."},
  {1,"M_ULTRA", M_ChooseSkill, 'u', "Ultra-Violence."},
  {1,"M_NMARE", M_ChooseSkill, 'n', "Nightmare!"}
};

menu_t NewDef =
{
  newg_end,       // # of menu items
  &EpiDef,        // previous menu
  NewGameMenu,    // menuitem_t ->
  M_DrawNewGame,  // drawing routine ->
  48,63,          // x,y
  hurtme          // lastOn
};

//
// M_NewGame
//

void M_DrawNewGame(void)
{
  // CPhipps - patch drawing updated
  V_DrawNamePatch(96, 14, 0, "M_NEWG", CR_DEFAULT, VPT_STRETCH);
  V_DrawNamePatch(54, 38, 0, "M_SKILL",CR_DEFAULT, VPT_STRETCH);
}

/* cph - make `New Game' restart the level in a netgame */
static void M_RestartLevelResponse(int ch)
{
  if (ch != 'y')
    return;

  currentMenu->lastOn = itemOn;
  M_ClearMenus ();
  G_RestartLevel ();
}

void M_NewGame(int choice)
{
  if (netgame && !demoplayback) {
    if (compatibility_level < lxdoom_1_compatibility)
      M_StartMessage(s_NEWGAME,NULL,FALSE); // Ty 03/27/98 - externalized
    else // CPhipps - query restarting the level
      M_RestartLevelResponse('y');
    return;
  }

  if ( EpiDef.numitems == 0 )
    M_SetupNextMenu(&NewDef);
  else
    M_SetupNextMenu(&EpiDef);
}

// CPhipps - static
static void M_VerifyNightmare(int ch)
{
  if (ch != 'y')
    return;

  G_DeferedInitNew(nightmare,epi+1,1);
  M_ClearMenus ();
}

void M_ChooseSkill(int choice)
{
  if (choice == nightmare)
    {   // Ty 03/27/98 - externalized
      M_VerifyNightmare('y');
      return;
    }

  if (!EpiCustom) G_DeferedInitNew(choice,epi+1,1);
  else G_DeferedInitNew(choice, EpiMenuEpi[epi], EpiMenuMap[epi]);
  M_ClearMenus ();
}

/////////////////////////////
//
// LOAD GAME MENU
//

// numerical values for the Load Game slots

enum
{
  load1,
  load2,
  load3,
  load4,
  load5,
  load6,
  load7, //jff 3/15/98 extend number of slots
  load8,
  load_end
} load_e;

// The definitions of the Load Game screen

menuitem_t LoadMenue[]=
{
  {1,"", M_LoadSelect,'1',NULL},
  {1,"", M_LoadSelect,'2',NULL},
  {1,"", M_LoadSelect,'3',NULL},
  {1,"", M_LoadSelect,'4',NULL},
  {1,"", M_LoadSelect,'5',NULL},
  {1,"", M_LoadSelect,'6',NULL},
  {1,"", M_LoadSelect,'7',NULL}, //jff 3/15/98 extend number of slots
  {1,"", M_LoadSelect,'8',NULL},
};

menu_t LoadDef =
{
  load_end,
  &MainDef,
  LoadMenue,
  M_DrawLoad,
  80,34, //jff 3/15/98 move menu up
  0
};

#define LOADGRAPHIC_Y 8

//
// M_LoadGame & Cie.
//

void M_DrawLoad(void)
{
  int i;

  //jff 3/15/98 use symbolic load position
  // CPhipps - patch drawing updated
  V_DrawNamePatch(72 ,LOADGRAPHIC_Y, 0, "M_LOADG", CR_DEFAULT, VPT_STRETCH);
  for (i = 0 ; i < load_end ; i++) {
    M_DrawSaveLoadBorder(LoadDef.x,LoadDef.y+LINEHEIGHT*i);
    M_WriteText(LoadDef.x,LoadDef.y+LINEHEIGHT*i,savegamestrings[i], CR_DEFAULT);
  }
}

//
// Draw border for the savegame description
//

void M_DrawSaveLoadBorder(int x,int y)
{
  int i;

  V_DrawNamePatch(x-8, y+7, 0, "M_LSLEFT", CR_DEFAULT, VPT_STRETCH);

  for (i = 0 ; i < 24 ; i++)
    {
      V_DrawNamePatch(x, y+7, 0, "M_LSCNTR", CR_DEFAULT, VPT_STRETCH);
      x += 8;
    }

  V_DrawNamePatch(x, y+7, 0, "M_LSRGHT", CR_DEFAULT, VPT_STRETCH);
}

//
// User wants to load this game
//

void M_LoadSelect(int choice)
{
  // CPhipps - Modified so savegame filename is worked out only internal
  //  to g_game.c, this only passes the slot.

  G_LoadGame(choice, FALSE); // killough 3/16/98, 5/15/98: add slot, cmd

  M_ClearMenus ();
}

//
// killough 5/15/98: add forced loadgames
//

static char *forced_loadgame_message;

static void M_VerifyForcedLoadGame(int ch)
{
  if (ch=='y')
    G_ForcedLoadGame();
  free(forced_loadgame_message);    // free the message strdup()'ed below
  M_ClearMenus();
}

void M_ForcedLoadGame(const char *msg)
{
  forced_loadgame_message = strdup(msg); // free()'d above
  M_StartMessage(forced_loadgame_message, M_VerifyForcedLoadGame, TRUE);
}

//
// Selected from DOOM menu
//

void M_LoadGame (int choice)
{
  M_SetupNextMenu(&LoadDef);
  M_ReadSaveStrings();
}

/////////////////////////////
//
// SAVE GAME MENU
//

// The definitions of the Save Game screen

menuitem_t SaveMenu[]=
{
  {1,"", M_SaveSelect,'1',NULL},
  {1,"", M_SaveSelect,'2',NULL},
  {1,"", M_SaveSelect,'3',NULL},
  {1,"", M_SaveSelect,'4',NULL},
  {1,"", M_SaveSelect,'5',NULL},
  {1,"", M_SaveSelect,'6',NULL},
  {1,"", M_SaveSelect,'7',NULL}, //jff 3/15/98 extend number of slots
  {1,"", M_SaveSelect,'8',NULL},
};

menu_t SaveDef =
{
  load_end, // same number of slots as the Load Game screen
  &MainDef,
  SaveMenu,
  M_DrawSave,
  80,34, //jff 3/15/98 move menu up
  0
};

//
// M_ReadSaveStrings
//  read the strings from the savegame files
//
void M_ReadSaveStrings(void)
{
  int i;

  for (i = 0 ; i < load_end ; i++) {
    char name[PATH_MAX+1];    // killough 3/22/98
    RFILE *fp;

    /* killough 3/22/98
     * cph - add not-demoplayback parameter */
    G_SaveGameName(name,sizeof(name),i,FALSE);
    fp = filestream_open(name,
		    RETRO_VFS_FILE_ACCESS_READ,
		    RETRO_VFS_FILE_ACCESS_HINT_NONE);

    if (!fp) {   // Ty 03/27/98 - externalized:
      strcpy(&savegamestrings[i][0],s_EMPTYSTRING);
      LoadMenue[i].status = 0;
      continue;
    }
    if ( rfread(&savegamestrings[i], SAVESTRINGSIZE, 1, fp) != 1)
      I_Error("M_ReadSaveStrings: can't read savegame file '%s'", name);
    filestream_close(fp);
    LoadMenue[i].status = 1;
  }
}

//
//  M_SaveGame & Cie.
//
void M_DrawSave(void)
{
  int i;

  //jff 3/15/98 use symbolic load position
  // CPhipps - patch drawing updated
  V_DrawNamePatch(72, LOADGRAPHIC_Y, 0, "M_SAVEG", CR_DEFAULT, VPT_STRETCH);
  for (i = 0 ; i < load_end ; i++)
    {
    M_DrawSaveLoadBorder(LoadDef.x,LoadDef.y+LINEHEIGHT*i);
    M_WriteText(LoadDef.x,LoadDef.y+LINEHEIGHT*i,savegamestrings[i], CR_DEFAULT);
    }

  if (saveStringEnter)
    {
    i = M_StringWidth(savegamestrings[saveSlot]);
    M_WriteText(LoadDef.x + i,LoadDef.y+LINEHEIGHT*saveSlot,"_", CR_DEFAULT);
    }
}

//
// M_Responder calls this when user is finished
//
static void M_DoSave(int slot)
{
  G_SaveGame (slot,savegamestrings[slot]);
  M_ClearMenus ();

  // PICK QUICKSAVE SLOT YET?
  if (quickSaveSlot == -2)
    quickSaveSlot = slot;
}

//
// User wants to save. Start string input for M_Responder
//
void M_SaveSelect(int choice)
{
  // we are going to be intercepting all chars
  saveStringEnter = 1;

  saveSlot = choice;
  strcpy(saveOldString,savegamestrings[choice]);
  if (!strcmp(saveOldString, s_EMPTYSTRING))
  {
     savegamestrings[choice][0] = 'S';
     savegamestrings[choice][1] = 'A';
     savegamestrings[choice][2] = 'V';
     savegamestrings[choice][3] = 'E';
     savegamestrings[choice][4] = '0' + choice;
     savegamestrings[choice][5] = 0;
   }
   saveCharIndex = strlen(savegamestrings[choice]);
}

//
// Selected from DOOM menu
//
void M_SaveGame (int choice)
{
  // killough 10/6/98: allow savegames during single-player demo playback
  if (!usergame && (!demoplayback || netgame))
    {
    M_StartMessage(s_SAVEDEAD,NULL,FALSE); // Ty 03/27/98 - externalized
    return;
    }

  if (gamestate != GS_LEVEL)
    return;

  M_SetupNextMenu(&SaveDef);
  M_ReadSaveStrings();
}

/////////////////////////////
//
// OPTIONS MENU
//

// numerical values for the Options menu items

enum
{
  general, // killough 10/98
  // killough 4/6/98: move setup to be a sub-menu of OPTIONs
  setup,                                                    // phares 3/21/98
  endgame,
  messages,
  /*    detail, obsolete -- killough */
  scrnsize,
  mousesens,
  /* option_empty2, submenu now -- killough */
  soundvol,
  opt_end
} options_e;

// The definitions of the Options menu

menuitem_t OptionsMenu[]=
{
  // killough 4/6/98: move setup to be a sub-menu of OPTIONs
  {1,"M_GENERL", M_General, 'g', "GENERAL"},      // killough 10/98
  {1,"M_SETUP",  M_Setup,   's', "SETUP"},        // phares 3/21/98
  {1,"M_ENDGAM", M_EndGame,'e',  "END GAME"},
  {1,"M_MESSG",  M_ChangeMessages,'m', "MESSAGES"},
  {2,"M_SCRNSZ", M_SizeDisplay,'s', "SCREEN SIZE"},
  {1,"M_MSENS",  M_ChangeSensitivity,'m', "MOUSE SENSITIVITY"},
  {1,"M_SVOL",   M_Sound,'s', "SOUND VOLUME"},
};

menu_t OptionsDef =
{
  opt_end,
  &MainDef,
  OptionsMenu,
  M_DrawOptions,
  60,37,
  0
};

//
// M_Options
//
char detailNames[2][9] = {"M_GDHIGH","M_GDLOW"};
char msgNames[2][9]  = {"M_MSGOFF","M_MSGON"};


void M_DrawOptions(void)
{
  // CPhipps - patch drawing updated
  // proff/nicolas 09/20/98 -- changed for hi-res
  V_DrawNamePatch(108, 15, 0, "M_OPTTTL", CR_DEFAULT, VPT_STRETCH);

  V_DrawNamePatch(OptionsDef.x + 120, OptionsDef.y+LINEHEIGHT*messages, 0,
      msgNames[showMessages], CR_DEFAULT, VPT_STRETCH);

  V_DrawNamePatch(OptionsDef.x + 150, OptionsDef.y + (LINEHEIGHT*4), 0,
      detailNames[!screenblocks], CR_DEFAULT, VPT_STRETCH);
}

void M_Options(int choice)
{
  M_SetupNextMenu(&OptionsDef);
}

/////////////////////////////
//
// M_QuitDOOM
//
int quitsounds[8] =
{
  sfx_pldeth,
  sfx_dmpain,
  sfx_popain,
  sfx_slop,
  sfx_telept,
  sfx_posit1,
  sfx_posit3,
  sfx_sgtatk
};

int quitsounds2[8] =
{
  sfx_vilact,
  sfx_getpow,
  sfx_boscub,
  sfx_slop,
  sfx_skeswg,
  sfx_kntdth,
  sfx_bspact,
  sfx_sgtatk
};

dbool quit_pressed = false;

static void M_QuitResponse(int ch)
{
  if (ch != 'y')
    return;
#ifndef __LIBRETRO__
  if ((!netgame || demoplayback) // killough 12/98
      && !nosfxparm && snd_card) // avoid delay if no sound card
  {
    int i;

    if (gamemode == commercial)
      S_StartSound(NULL,quitsounds2[(gametic>>2)&7]);
    else
      S_StartSound(NULL,quitsounds[(gametic>>2)&7]);

    // wait till all sounds stopped or 3 seconds are over
    i = 30;
    while (i>0) {
      I_uSleep(100000); // CPhipps - don't thrash cpu in this loop
      if (!I_AnySoundStillPlaying())
        break;
      i--;
    }
  }
#endif

  quit_pressed = true;
}

void M_QuitDOOM(int choice)
{
  // We pick index 0 which is language sensitive,
  // or one at random, between 1 and maximum number.
  // Ty 03/27/98 - externalized DOSY as a string s_DOSY that's in the sprintf
  has_exited = 0;
  M_QuitResponse('y');
}

/////////////////////////////
//
// SOUND VOLUME MENU
//

// numerical values for the Sound Volume menu items
// The 'empty' slots are where the sliding scales appear.

enum
{
  sfx_vol,
  sfx_empty1,
  music_vol,
  sfx_empty2,
  sound_end
} sound_e;

// The definitions of the Sound Volume menu

menuitem_t SoundMenu[]=
{
  {2,"M_SFXVOL",M_SfxVol,'s',"Sfx Volume"},
  {-1,"",NULL,0,NULL},
  {2,"M_MUSVOL",M_MusicVol,'m', "Music Volume"},
  {-1,"",NULL,0,NULL}
};

menu_t SoundDef =
{
  sound_end,
  &OptionsDef,
  SoundMenu,
  M_DrawSound,
  80,64,
  0
};

//
// Change Sfx & Music volumes
//

void M_DrawSound(void)
{
  // CPhipps - patch drawing updated
  V_DrawNamePatch(60, 38, 0, "M_SVOL", CR_DEFAULT, VPT_STRETCH);

  M_DrawThermo(SoundDef.x,SoundDef.y+LINEHEIGHT*(sfx_vol+1),16,snd_SfxVolume);

  M_DrawThermo(SoundDef.x,SoundDef.y+LINEHEIGHT*(music_vol+1),16,snd_MusicVolume);
}

void M_Sound(int choice)
{
  M_SetupNextMenu(&SoundDef);
}

void M_SfxVol(int choice)
{
  switch(choice)
    {
    case 0:
      if (snd_SfxVolume)
        snd_SfxVolume--;
      break;
    case 1:
      if (snd_SfxVolume < 15)
        snd_SfxVolume++;
      break;
    }

  S_SetSfxVolume(snd_SfxVolume /* *8 */);
}

void M_MusicVol(int choice)
{
  switch(choice)
    {
    case 0:
      if (snd_MusicVolume)
        snd_MusicVolume--;
      break;
    case 1:
      if (snd_MusicVolume < 15)
        snd_MusicVolume++;
      break;
    }

  S_SetMusicVolume(snd_MusicVolume /* *8 */);
}

/////////////////////////////
//
// MOUSE SENSITIVITY MENU -- killough
//

// numerical values for the Mouse Sensitivity menu items
// The 'empty' slots are where the sliding scales appear.

enum
{
  mouse_horiz,
  mouse_empty1,
  mouse_vert,
  mouse_empty2,
  mouse_end
} mouse_e;

// The definitions of the Mouse Sensitivity menu

menuitem_t MouseMenu[]=
{
  {2,"M_HORSEN",M_MouseHoriz,'h', "HORIZONTAL"},
  {-1,"",NULL,0,NULL},
  {2,"M_VERSEN",M_MouseVert,'v', "VERTICAL"},
  {-1,"",NULL,0,NULL}
};

menu_t MouseDef =
{
  mouse_end,
  &OptionsDef,
  MouseMenu,
  M_DrawMouse,
  60,64,
  0
};


// I'm using a scale of 100 since I don't know what's normal -- killough.

#define MOUSE_SENS_MAX 100

//
// Change Mouse Sensitivities -- killough
//

void M_DrawMouse(void)
{
  int mhmx,mvmx; /* jff 4/3/98 clamp drawn position    99max mead */

  // CPhipps - patch drawing updated
  V_DrawNamePatch(60, 38, 0, "M_MSENS", CR_DEFAULT, VPT_STRETCH);

  //jff 4/3/98 clamp horizontal sensitivity display
  mhmx = mouseSensitivity_horiz>99? 99 : mouseSensitivity_horiz; /*mead*/
  M_DrawThermo(MouseDef.x,MouseDef.y+LINEHEIGHT*(mouse_horiz+1),100,mhmx);
  //jff 4/3/98 clamp vertical sensitivity display
  mvmx = mouseSensitivity_vert>99? 99 : mouseSensitivity_vert; /*mead*/
  M_DrawThermo(MouseDef.x,MouseDef.y+LINEHEIGHT*(mouse_vert+1),100,mvmx);
}

void M_ChangeSensitivity(int choice)
{
  M_SetupNextMenu(&MouseDef);      // killough

  //  switch(choice)
  //      {
  //    case 0:
  //      if (mouseSensitivity)
  //        mouseSensitivity--;
  //      break;
  //    case 1:
  //      if (mouseSensitivity < 9)
  //        mouseSensitivity++;
  //      break;
  //      }
}

void M_MouseHoriz(int choice)
{
  M_Mouse(choice, &mouseSensitivity_horiz);
}

void M_MouseVert(int choice)
{
  M_Mouse(choice, &mouseSensitivity_vert);
}

void M_Mouse(int choice, int *sens)
{
  switch(choice)
    {
    case 0:
      if (*sens)
        --*sens;
      break;
    case 1:
      if (*sens < 99)
        ++*sens;              /*mead*/
      break;
    }
}

/////////////////////////////
//
//    M_QuickSave
//

char tempstring[80];

void M_QuickSave(void)
{
  if (!usergame && (!demoplayback || netgame)) { /* killough 10/98 */
    S_StartSound(NULL,sfx_oof);
    return;
  }

  if (gamestate != GS_LEVEL)
    return;

  if (quickSaveSlot < 0) {
    M_StartControlPanel();
    M_ReadSaveStrings();
    M_SetupNextMenu(&SaveDef);
    quickSaveSlot = -2; // means to pick a slot now
    return;
  }
  sprintf(tempstring,s_QSPROMPT,savegamestrings[quickSaveSlot]); // Ty 03/27/98 - externalized
  M_DoSave(quickSaveSlot);
  S_StartSound(NULL,sfx_swtchx);
}

/////////////////////////////
//
// M_QuickLoad
//

static void M_QuickLoadResponse(int ch)
{
  if (ch == 'y') {
    M_LoadSelect(quickSaveSlot);
    S_StartSound(NULL,sfx_swtchx);
  }
}

void M_QuickLoad(void)
{
  // cph - removed restriction against quickload in a netgame

  if (quickSaveSlot < 0) {
    M_StartMessage(s_QSAVESPOT,NULL,FALSE); // Ty 03/27/98 - externalized
    return;
  }
  sprintf(tempstring,s_QLPROMPT,savegamestrings[quickSaveSlot]); // Ty 03/27/98 - externalized
  M_QuickLoadResponse('y');
}

/////////////////////////////
//
// M_EndGame
//

static void M_EndGameResponse(int ch)
{
  if (ch != 'y')
    return;

  currentMenu->lastOn = itemOn;
  M_ClearMenus ();
  D_StartTitle ();
}

void M_EndGame(int choice)
{
  if (netgame)
    {
    M_StartMessage(s_NETEND,NULL,FALSE); // Ty 03/27/98 - externalized
    return;
    }
  M_EndGameResponse('y');
}

/////////////////////////////
//
//    Toggle messages on/off
//

void M_ChangeMessages(int choice)
{
  (void)choice; // unused, but needed in method signature due to menu logic

  showMessages = 1 - showMessages;

  if (!showMessages)
    players[consoleplayer].message = s_MSGOFF; // Ty 03/27/98 - externalized
  else
    players[consoleplayer].message = s_MSGON ; // Ty 03/27/98 - externalized

  message_dontfuckwithme = TRUE;
}

/////////////////////////////
//
// CHANGE DISPLAY SIZE
//
//

void M_SizeDisplay(int choice)
{
  if (screenblocks == choice && choice == 1) {
    // If it's already on full screen, cycle the hud_mode instead
    hud_mode = (hud_mode>1)? 0 : hud_mode+1;
  } else {
    screenblocks = choice;
    R_SetViewSize (screenblocks);
  }
}

//
// End of Original Menus
//
/////////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////////
//
// SETUP MENU (phares)
//
// We've added a set of Setup Screens from which you can configure a number
// of variables w/o having to restart the game. There are 7 screens:
//
//    Key Bindings
//    Weapons
//    Status Bar / HUD
//    Automap
//    Enemies
//    Messages
//    Chat Strings
//
// killough 10/98: added Compatibility and General menus
//

/////////////////////////////
//
// Some utility macros for initializing setup_menu_t items

#define SETUP_MENU(T,F,G,X,Y,V)     {T        ,F                         ,G     ,X,Y,V     ,0,0,NULL,NULL}
#define SETUP_MENU_KEY(T,G,X,Y,K,M) {T        ,S_KEY                     ,G     ,X,Y,{K}   ,M,0,NULL,NULL}
#define SETUP_MENU_TITLE(T,X,Y)     {T        ,S_SKIP|S_TITLE            ,m_null,X,Y,{NULL},0,0,NULL,NULL}
#define SETUP_MENU_CREDIT(T,X,Y)    {T        ,S_SKIP|S_CREDIT|S_LEFTJUST,m_null,X,Y,{NULL},0,0,NULL,NULL}
#define SETUP_MENU_PREV(G,X,Y)      {"<- PREV",S_SKIP|S_PREV             ,m_null,X,Y,{G}   ,0,0,NULL,NULL}
#define SETUP_MENU_NEXT(G,X,Y)      {"NEXT ->",S_SKIP|S_NEXT             ,m_null,X,Y,{G}   ,0,0,NULL,NULL}
#define SETUP_MENU_RESET            {NULL     ,S_RESET     ,m_null,X_BUTTON,Y_BUTTON,{NULL},0,0,NULL,NULL}
#define SETUP_MENU_END              {NULL     ,S_SKIP|S_END              ,m_null,0,0,{NULL},0,0,NULL,NULL}

/////////////////////////////
//
// dbools for setup screens
// these tell you what state the setup screens are in, and whether any of
// the overlay screens (automap colors, reset button message) should be
// displayed

dbool setup_active      = FALSE; // in one of the setup screens
dbool set_keybnd_active = FALSE; // in key binding setup screens
dbool set_weapon_active = FALSE; // in weapons setup screen
dbool set_status_active = FALSE; // in status bar/hud setup screen
dbool set_auto_active   = FALSE; // in automap setup screen
dbool set_enemy_active  = FALSE; // in enemies setup screen
dbool set_mess_active   = FALSE; // in messages setup screen
dbool set_chat_active   = FALSE; // in chat string setup screen
dbool setup_select      = FALSE; // changing an item
dbool setup_gather      = FALSE; // gathering keys for value
dbool colorbox_active   = FALSE; // color palette being shown
dbool default_verify    = FALSE; // verify reset defaults decision
dbool set_general_active = FALSE;
dbool set_compat_active = FALSE;

/////////////////////////////
//
// set_menu_itemon is an index that starts at zero, and tells you which
// item on the current screen the cursor is sitting on.
//
// current_setup_menu is a pointer to the current setup menu table.

int set_menu_itemon; // which setup item is selected?   // phares 3/98
setup_menu_t* current_setup_menu; // points to current setup menu table

/////////////////////////////
//
// The menu_buffer is used to construct strings for display on the screen.

static char menu_buffer[64];

/////////////////////////////
//
// The setup_e enum is used to provide a unique number for each group of Setup
// Screens.

enum
{
  set_compat,
  set_key_bindings,
  set_weapons,
  set_statbar,
  set_automap,
  set_enemy,
  set_messages,
  set_chatstrings,
  set_setup_end
} setup_e;

int setup_screen; // the current setup screen. takes values from setup_e

/////////////////////////////
//
// SetupMenu is the definition of what the main Setup Screen should look
// like. Each entry shows that the cursor can land on each item (1), the
// built-in graphic lump (i.e. "M_KEYBND") that should be displayed,
// the program which takes over when an item is selected, and the hotkey
// associated with the item.

menuitem_t SetupMenu[]=
{
  {1,"M_COMPAT",M_Compat,     'p', "DOOM COMPATIBILITY"},
  {1,"M_KEYBND",M_KeyBindings,'k', "KEY BINDINGS"},
  {1,"M_WEAP"  ,M_Weapons,    'w', "WEAPONS"},
  {1,"M_STAT"  ,M_StatusBar,  's', "STATUS BAR / HUD"},
  {1,"M_AUTO"  ,M_Automap,    'a', "AUTOMAP"},
  {1,"M_ENEM"  ,M_Enemy,      'e', "ENEMIES"},
  {1,"M_MESS"  ,M_Messages,   'm', "MESSAGES"},
  {1,"M_CHAT"  ,M_ChatStrings,'c', "CHAT STRINGS"},
};

/////////////////////////////
//
// M_DoNothing does just that: nothing. Just a placeholder.

static void M_DoNothing(int choice)
{
}

/////////////////////////////
//
// Items needed to satisfy the 'Big Font' menu structures:
//
// the generic_setup_e enum mimics the 'Big Font' menu structures, but
// means nothing to the Setup Menus.

enum
{
  generic_setupempty1,
  generic_setup_end
} generic_setup_e;

// Generic_Setup is a do-nothing definition that the mainstream Menu code
// can understand, while the Setup Menu code is working. Another placeholder.

menuitem_t Generic_Setup[] =
{
  {1,"",M_DoNothing,0,NULL}
};

/////////////////////////////
//
// SetupDef is the menu definition that the mainstream Menu code understands.
// This is used by M_Setup (below) to define what is drawn and what is done
// with the main Setup screen.

menu_t  SetupDef =
{
  set_setup_end, // number of Setup Menu items (Key Bindings, etc.)
  &OptionsDef,   // menu to return to when BACKSPACE is hit on this menu
  SetupMenu,     // definition of items to show on the Setup Screen
  M_DrawSetup,   // program that draws the Setup Screen
  59,37,         // x,y position of the skull (modified when the skull is
                 // drawn). The skull is parked on the upper-left corner
                 // of the Setup screens, since it isn't needed as a cursor
  0              // last item the user was on for this menu
};

/////////////////////////////
//
// Here are the definitions of the individual Setup Menu screens. They
// follow the format of the 'Big Font' menu structures. See the comments
// for SetupDef (above) to help understand what each of these says.

menu_t KeybndDef =
{
  generic_setup_end,
  &SetupDef,
  Generic_Setup,
  M_DrawKeybnd,
  34,5,      // skull drawn here
  0
};

menu_t WeaponDef =
{
  generic_setup_end,
  &SetupDef,
  Generic_Setup,
  M_DrawWeapons,
  34,5,      // skull drawn here
  0
};

menu_t StatusHUDDef =
{
  generic_setup_end,
  &SetupDef,
  Generic_Setup,
  M_DrawStatusHUD,
  34,5,      // skull drawn here
  0
};

menu_t AutoMapDef =
{
  generic_setup_end,
  &SetupDef,
  Generic_Setup,
  M_DrawAutoMap,
  34,5,      // skull drawn here
  0
};

menu_t EnemyDef =                                           // phares 4/08/98
{
  generic_setup_end,
  &SetupDef,
  Generic_Setup,
  M_DrawEnemy,
  34,5,      // skull drawn here
  0
};

menu_t MessageDef =                                         // phares 4/08/98
{
  generic_setup_end,
  &SetupDef,
  Generic_Setup,
  M_DrawMessages,
  34,5,      // skull drawn here
  0
};

menu_t ChatStrDef =                                         // phares 4/10/98
{
  generic_setup_end,
  &SetupDef,
  Generic_Setup,
  M_DrawChatStrings,
  34,5,      // skull drawn here
  0
};

menu_t GeneralDef =                                           // killough 10/98
{
  generic_setup_end,
  &OptionsDef,
  Generic_Setup,
  M_DrawGeneral,
  34,5,      // skull drawn here
  0
};

menu_t CompatDef =                                           // killough 10/98
{
  generic_setup_end,
  &SetupDef,
  Generic_Setup,
  M_DrawCompat,
  34,5,      // skull drawn here
  0
};

/////////////////////////////
//
// Draws the Title for the main Setup screen

void M_DrawSetup(void)
{
  // CPhipps - patch drawing updated
  M_DrawTitle(124, 15, "M_SETUP", CR_DEFAULT, "SETUP", CR_GOLD);
}

/////////////////////////////
//
// Uses the SetupDef structure to draw the menu items for the main
// Setup screen

void M_Setup(int choice)
{
  M_SetupNextMenu(&SetupDef);
}

/////////////////////////////
//
// Data that's used by the Setup screen code
//
// Establish the message colors to be used

#define CR_TITLE  CR_GOLD
#define CR_SET    CR_GREEN
#define CR_ITEM   CR_RED
#define CR_HILITE CR_LIGHT
#define CR_SELECT CR_GRAY

// Data used by the Automap color selection code

#define CHIP_SIZE 7 // size of color block for colored items

#define COLORPALXORIG ((320 - 16*(CHIP_SIZE+1))/2)
#define COLORPALYORIG ((200 - 16*(CHIP_SIZE+1))/2)

#define PAL_BLACK   0
#define PAL_WHITE   4

// Data used by the Chat String editing code

#define CHAT_STRING_BFR_SIZE 128

// chat strings must fit in this screen space
// killough 10/98: reduced, for more general uses
#define MAXCHATWIDTH         272

int   chat_index;
char* chat_string_buffer; // points to new chat strings while editing

/////////////////////////////
//
// phares 4/17/98:
// Added 'Reset to Defaults' Button to Setup Menu screens
// This is a small button that sits in the upper-right-hand corner of
// the first screen for each group. It blinks when selected, thus the
// two patches, which it toggles back and forth.

char ResetButtonName[2][8] = {"M_BUTT1","M_BUTT2"};

/////////////////////////////
//
// phares 4/18/98:
// Consolidate Item drawing code
//
// M_DrawItem draws the description of the provided item (the left-hand
// part). A different color is used for the text depending on whether the
// item is selected or not, or whether it's about to change.

// CPhipps - static, hanging else removed, const parameter
static void M_DrawItem(const setup_menu_t* s)
{
  int x = s->m_x;
  int y = s->m_y;
  int flags = s->m_flags;
  if (flags & S_RESET)

    // This item is the reset button
    // Draw the 'off' version if this isn't the current menu item
    // Draw the blinking version in tune with the blinking skull otherwise

    // proff/nicolas 09/20/98 -- changed for hi-res
    // CPhipps - Patch drawing updated, reformatted

    V_DrawNamePatch(x, y, 0, ResetButtonName[(flags & (S_HILITE|S_SELECT)) ? whichSkull : 0],
        CR_DEFAULT, VPT_STRETCH);

  else { // Draw the item string
    char *p, *t;
    int w = 0;
    int color =
  flags & S_SELECT ? CR_SELECT :
  flags & S_HILITE ? CR_HILITE :
  flags & (S_TITLE|S_NEXT|S_PREV) ? CR_TITLE : CR_ITEM; // killough 10/98

    /* killough 10/98:
     * Enhance to support multiline text separated by newlines.
     * This supports multiline items on horizontally-crowded menus.
     */

    for (p = t = strdup(s->m_text); (p = strtok(p,"\n")); y += 8, p = NULL)
      {      /* killough 10/98: support left-justification: */
  strcpy(menu_buffer,p);
  if (!(flags & S_LEFTJUST))
    w = M_GetPixelWidth(menu_buffer) + 4;
  M_DrawMenuString(x - w, y ,color);
      }
    free(t);
  }
}

// If a number item is being changed, allow up to N keystrokes to 'gather'
// the value. Gather_count tells you how many you have so far. The legality
// of what is gathered is determined by the low/high settings for the item.

#define MAXGATHER 5
int  gather_count;
char gather_buffer[MAXGATHER+1];  // killough 10/98: make input character-based

/////////////////////////////
//
// phares 4/18/98:
// Consolidate Item Setting drawing code
//
// M_DrawSetting draws the setting of the provided item (the right-hand
// part. It determines the text color based on whether the item is
// selected or being changed. Then, depending on the type of item, it
// displays the appropriate setting value: yes/no, a key binding, a number,
// a paint chip, etc.

static void M_DrawSetting(const setup_menu_t* s)
{
  int x = s->m_x, y = s->m_y, flags = s->m_flags, color;

  // Determine color of the text. This may or may not be used later,
  // depending on whether the item is a text string or not.

  color = flags & S_SELECT ? CR_SELECT : flags & S_HILITE ? CR_HILITE : CR_SET;

  // Is the item a YES/NO item?

  if (flags & S_YESNO) {
    strcpy(menu_buffer,*s->var.def->location.pi ? "YES" : "NO");
    M_DrawMenuString(x,y,color);
    return;
  }

  // Is the item a simple number?

  if (flags & S_NUM) {
    // killough 10/98: We must draw differently for items being gathered.
    if (flags & (S_HILITE|S_SELECT) && setup_gather) {
      gather_buffer[gather_count] = 0;
      strcpy(menu_buffer, gather_buffer);
    }
    else
      sprintf(menu_buffer,"%d",*s->var.def->location.pi);
    M_DrawMenuString(x,y,color);
    return;
  }

  // Is the item a key binding?

  if (flags & S_KEY) { // Key Binding
    int *key = s->var.m_key;

    // Draw the key bound to the action

    if (key) {
      M_GetKeyString(*key,0); // string to display
      if (key == &key_up || key == &key_down || key == &key_speed ||
         key == &key_fire || key == &key_strafe || key == &key_use)
  {
    if (s->m_mouse && *s->m_mouse != -1)
      sprintf(menu_buffer+strlen(menu_buffer), "/MB%d",
        *s->m_mouse+1);
  }
      M_DrawMenuString(x,y,color);
    }
    return;
  }

  // Is the item a weapon number?
  // OR, Is the item a colored text string from the Automap?
  //
  // killough 10/98: removed special code, since the rest of the engine
  // already takes care of it, and this code prevented the user from setting
  // their overall weapons preferences while playing Doom 1.
  //
  // killough 11/98: consolidated weapons code with color range code

  if (flags & (S_WEAP|S_CRITEM)) // weapon number or color range
    {
      sprintf(menu_buffer,"%d", *s->var.def->location.pi);
      M_DrawMenuString(x,y, flags & S_CRITEM ? *s->var.def->location.pi : color);
      return;
    }

  // Is the item a paint chip?

  if (flags & S_COLOR) // Automap paint chip
    {
      int ch;

      ch = *s->var.def->location.pi;
      // proff 12/6/98: Drawing of colorchips completly changed for hi-res, it now uses a patch
      // draw the paint chip
      V_FillRect(x*SCREENWIDTH/320, (y-1)*SCREENHEIGHT/200,
                    8*SCREENWIDTH/320, 8*SCREENHEIGHT/200,
                 PAL_BLACK);
      V_FillRect((x+1)*SCREENWIDTH/320, y*SCREENHEIGHT/200,
                        6*SCREENWIDTH/320, 6*SCREENHEIGHT/200,
                 (uint8_t)ch);

      if (!ch) // don't show this item in automap mode
  V_DrawNamePatch(x+1,y,0,"M_PALNO", CR_DEFAULT, VPT_STRETCH);
      return;
    }

  // Is the item a chat string?
  // killough 10/98: or a filename?

  if (flags & S_STRING) {
    /* cph - cast to char* as it's really a Z_Strdup'd string (see m_misc.h) */
    union { const char **c; char **s; } u; // type punning via unions
    char *text;

    u.c = s->var.def->location.ppsz;
    text = *(u.s);

    // Are we editing this string? If so, display a cursor under
    // the correct character.

    if (setup_select && (s->m_flags & (S_HILITE|S_SELECT))) {
      int cursor_start, char_width;
      char c[2];

      // If the string is too wide for the screen, trim it back,
      // one char at a time until it fits. This should only occur
      // while you're editing the string.

      while (M_GetPixelWidth(text) >= MAXCHATWIDTH) {
  int len = strlen(text);
  text[--len] = 0;
  if (chat_index > len)
    chat_index--;
      }

      // Find the distance from the beginning of the string to
      // where the cursor should be drawn, plus the width of
      // the char the cursor is under..

      *c = text[chat_index]; // hold temporarily
      c[1] = 0;
      char_width = M_GetPixelWidth(c);
      if (char_width == 1)
  char_width = 7; // default for end of line
      text[chat_index] = 0; // NULL to get cursor position
      cursor_start = M_GetPixelWidth(text);
      text[chat_index] = *c; // replace stored char

      // Now draw the cursor
      // proff 12/6/98: Drawing of cursor changed for hi-res
      V_FillRect(((x+cursor_start-1)*SCREENWIDTH)/320, (y*SCREENHEIGHT)/200,
      (char_width*SCREENWIDTH)/320, 9*SCREENHEIGHT/200, PAL_WHITE);
    }

    // Draw the setting for the item

    strcpy(menu_buffer,text);
    M_DrawMenuString(x,y,color);
    return;
  }

  // Is the item a selection of choices?

  if (flags & S_CHOICE) {
    if (s->var.def->type == def_int) {
      if (s->selectstrings == NULL) {
        sprintf(menu_buffer,"%d",*s->var.def->location.pi);
      } else {
        strcpy(menu_buffer,s->selectstrings[*s->var.def->location.pi]);
      }
    }

    if (s->var.def->type == def_str) {
      sprintf(menu_buffer,"%s", *s->var.def->location.ppsz);
    }

    M_DrawMenuString(x,y,color);
    return;
  }
}

/////////////////////////////
//
// M_DrawScreenItems takes the data for each menu item and gives it to
// the drawing routines above.

// CPhipps - static, const parameter, formatting
static void M_DrawScreenItems(const setup_menu_t* src)
{
  if (print_warning_about_changes > 0) { /* killough 8/15/98: print warning */
    if (warning_about_changes & S_BADVAL) {
  strcpy(menu_buffer, "Value out of Range");
  M_DrawMenuString(100,176,CR_RED);
    } else if (warning_about_changes & S_PRGWARN) {
        strcpy(menu_buffer, "Warning: Program must be restarted to see changes");
  M_DrawMenuString(3, 176, CR_RED);
    } else if (warning_about_changes & S_BADVID) {
        strcpy(menu_buffer, "Video mode not supported");
  M_DrawMenuString(80,176,CR_RED);
    } else {
  strcpy(menu_buffer, "Warning: Changes are pending until next game");
        M_DrawMenuString(18,184,CR_RED);
    }
  }

  while (!(src->m_flags & S_END)) {

    // See if we're to draw the item description (left-hand part)

    if (src->m_flags & S_SHOWDESC)
      M_DrawItem(src);

    // See if we're to draw the setting (right-hand part)

    if (src->m_flags & S_SHOWSET)
      M_DrawSetting(src);
    src++;
  }
}

/////////////////////////////
//
// Data used to draw the "are you sure?" dialogue box when resetting
// to defaults.

#define VERIFYBOXXORG 66
#define VERIFYBOXYORG 88
#define PAL_GRAY1  91
#define PAL_GRAY2  98
#define PAL_GRAY3 105

// And the routine to draw it.

static void M_DrawDefVerify(void)
{
  // Dialog background will use a patch if available, otherwise draw a black box
  int lump = W_CheckNumForName("M_VBOX");
  if ( lump != -1 )
    V_DrawNumPatch(VERIFYBOXXORG,VERIFYBOXYORG,0,lump,CR_DEFAULT,VPT_STRETCH);
  else {
    fline_t boxdiag = {
      { VERIFYBOXXORG, 	VERIFYBOXYORG },
      { VERIFYBOXXORG+187,	VERIFYBOXYORG+23 },
    };
    V_DrawBox(&boxdiag, 0);
  }

  // The blinking messages is keyed off of the blinking of the
  // cursor skull.
  if (whichSkull) { // blink the text
    strcpy(menu_buffer,"Reset to defaults? (Y or N)");
    M_DrawMenuString(VERIFYBOXXORG+8,VERIFYBOXYORG+8,CR_RED);
  }
}


/////////////////////////////
//
// phares 4/18/98:
// M_DrawInstructions writes the instruction text just below the screen title
//
// cph 2006/08/06 - go back to the Boom version, and then clean up by using
// M_DrawStringCentered (much better than all those magic 'x' valies!)

static void M_DrawInstructions(void)
{
  int flags = current_setup_menu[set_menu_itemon].m_flags;

  // There are different instruction messages depending on whether you
  // are changing an item or just sitting on it.

  if (setup_select) {
    switch (flags & (S_KEY | S_YESNO | S_WEAP | S_NUM | S_COLOR | S_CRITEM | S_CHAT | S_RESET | S_FILE | S_CHOICE)) {
      case S_KEY:
        // See if a joystick or mouse button setting is allowed for
        // this item.
        if (current_setup_menu[set_menu_itemon].m_mouse)
          M_DrawStringCentered(160, 20, CR_SELECT, "Press key or button for this action");
        else
          M_DrawStringCentered(160, 20, CR_SELECT, "Press key for this action");
        break;

    case S_YESNO:
      M_DrawStringCentered(160, 20, CR_SELECT, "Press ENTER key to toggle");
      break;
    case S_WEAP:
      M_DrawStringCentered(160, 20, CR_SELECT, "Enter weapon number");
      break;
    case S_NUM:
      M_DrawStringCentered(160, 20, CR_SELECT, "Enter value. Press ENTER when finished.");
      break;
    case S_COLOR:
      M_DrawStringCentered(160, 20, CR_SELECT, "Select color and press enter");
      break;
    case S_CRITEM:
      M_DrawStringCentered(160, 20, CR_SELECT, "Enter value");
      break;
    case S_CHAT:
      M_DrawStringCentered(160, 20, CR_SELECT, "Type/edit chat string and Press ENTER");
      break;
    case S_FILE:
      M_DrawStringCentered(160, 20, CR_SELECT, "Type/edit filename and Press ENTER");
      break;
    case S_CHOICE:
      M_DrawStringCentered(160, 20, CR_SELECT, "Press left or right to choose");
      break;
    case S_RESET:
      break;
    }
  } else {
    if (flags & S_RESET)
      M_DrawStringCentered(160, 20, CR_HILITE, "Press ENTER key to reset to defaults");
    else
      M_DrawStringCentered(160, 20, CR_HILITE, "Press Enter to Change");
  }
}


/////////////////////////////
//
// The Key Binding Screen tables.

#define KB_X  160
#define KB_PREV  57
#define KB_NEXT 310
#define KB_Y   31

// phares 4/16/98:
// X,Y position of reset button. This is the same for every screen, and is
// only defined once here.

#define X_BUTTON 300
#define Y_BUTTON   3

// Definitions of the (in this case) four key binding screens.

setup_menu_t keys_settings1[];
setup_menu_t keys_settings2[];
setup_menu_t keys_settings3[];
setup_menu_t keys_settings4[];

// The table which gets you from one screen table to the next.

setup_menu_t* keys_settings[] =
{
  keys_settings1,
  keys_settings2,
  keys_settings3,
  keys_settings4,
  NULL
};

int mult_screens_index; // the index of the current screen in a set

// Here's an example from this first screen, with explanations.
//
//  {
//  "STRAFE",      // The description of the item ('strafe' key)
//  S_KEY,         // This is a key binding item
//  m_scrn,        // It belongs to the m_scrn group. Its key cannot be
//                 // bound to two items in this group.
//  KB_X,          // The X offset of the start of the right-hand side
//  KB_Y+ 8*8,     // The Y offset of the start of the right-hand side.
//                 // Always given in multiples off a baseline.
//  &key_strafe,   // The variable that holds the key value bound to this
//                    OR a string that holds the config variable name.
//                    OR a pointer to another setup_menu
//  &mousebstrafe, // The variable that holds the mouse button bound to
                   // this. If zero, no mouse button can be bound here.
//  }

// The first Key Binding screen table.
// Note that the Y values are ascending. If you need to add something to
// this table, (well, this one's not a good example, because it's full)
// you need to make sure the Y values still make sense so everything gets
// displayed.
//
// Note also that the first screen of each set has a line for the reset
// button. If there is more than one screen in a set, the others don't get
// the reset button.
//
// Note also that this screen has a "NEXT ->" line. This acts like an
// item, in that 'activating' it moves you along to the next screen. If
// there's a "<- PREV" item on a screen, it behaves similarly, moving you
// to the previous screen. If you leave these off, you can't move from
// screen to screen.

setup_menu_t keys_settings1[] =  // Key Binding screen strings
{
  SETUP_MENU_TITLE("MOVEMENT"         ,KB_X,KB_Y),
  SETUP_MENU_KEY("FORWARD"     ,m_scrn,KB_X,KB_Y+1*8,&key_up         ,&mousebforward),
  SETUP_MENU_KEY("BACKWARD"    ,m_scrn,KB_X,KB_Y+2*8,&key_down       ,&mousebbackward),
  SETUP_MENU_KEY("TURN LEFT"   ,m_scrn,KB_X,KB_Y+3*8,&key_left       ,0),
  SETUP_MENU_KEY("TURN RIGHT"  ,m_scrn,KB_X,KB_Y+4*8,&key_right      ,0),
  SETUP_MENU_KEY("RUN"         ,m_scrn,KB_X,KB_Y+5*8,&key_speed      ,0),
  SETUP_MENU_KEY("STRAFE LEFT" ,m_scrn,KB_X,KB_Y+6*8,&key_strafeleft ,0),
  SETUP_MENU_KEY("STRAFE RIGHT",m_scrn,KB_X,KB_Y+7*8,&key_straferight,0),
  SETUP_MENU_KEY("STRAFE"      ,m_scrn,KB_X,KB_Y+8*8,&key_strafe     ,&mousebstrafe),
  SETUP_MENU_KEY("AUTORUN"     ,m_scrn,KB_X,KB_Y+9*8,&key_autorun    ,0),
  SETUP_MENU_KEY("180 TURN"    ,m_scrn,KB_X,KB_Y+10*8,&key_reverse   ,0),
  SETUP_MENU_KEY("USE"         ,m_scrn,KB_X,KB_Y+11*8,&key_use       ,&mousebforward),

  SETUP_MENU_TITLE("MENUS"           ,KB_X,KB_Y+12*8),
  SETUP_MENU_KEY("NEXT ITEM"  ,m_menu,KB_X,KB_Y+13*8,&key_menu_down     ,0),
  SETUP_MENU_KEY("PREV ITEM"  ,m_menu,KB_X,KB_Y+14*8,&key_menu_up       ,0),
  SETUP_MENU_KEY("LEFT"       ,m_menu,KB_X,KB_Y+15*8,&key_menu_left     ,0),
  SETUP_MENU_KEY("RIGHT"      ,m_menu,KB_X,KB_Y+16*8,&key_menu_right    ,0),
  SETUP_MENU_KEY("BACKSPACE"  ,m_menu,KB_X,KB_Y+17*8,&key_menu_backspace,0),
  SETUP_MENU_KEY("SELECT ITEM",m_menu,KB_X,KB_Y+18*8,&key_menu_enter    ,0),
  SETUP_MENU_KEY("EXIT"       ,m_menu,KB_X,KB_Y+19*8,&key_menu_escape   ,0),

  // Button for resetting to defaults
  SETUP_MENU_RESET,

  SETUP_MENU_NEXT(keys_settings2, KB_NEXT, KB_Y+20*8),

  // Final entry
  SETUP_MENU_END
};

setup_menu_t keys_settings2[] =  // Key Binding screen strings
{
   SETUP_MENU_TITLE("SCREEN"      ,KB_X,KB_Y),

  // phares 4/13/98:
  // key_help and key_escape can no longer be rebound. This keeps the
  // player from getting themselves in a bind where they can't remember how
  // to get to the menus, and can't remember how to get to the help screen
  // to give them a clue as to how to get to the menus. :)

  // Also, the keys assigned to these functions cannot be bound to other
  // functions. Introduce an S_KEEP flag to show that you cannot swap this
  // key with other keys in the same 'group'. (m_scrn, etc.)

  SETUP_MENU("HELP",S_SKIP|S_KEEP,m_scrn,0,0,{&key_help}),
  SETUP_MENU("MENU",S_SKIP|S_KEEP,m_scrn,0,0,{&key_escape}),

  // killough 10/98: hotkey for entering setup menu:
  SETUP_MENU_KEY("SETUP"      ,m_scrn,KB_X,KB_Y+ 1*8,&key_setup,0),
  SETUP_MENU_KEY("PAUSE"      ,m_scrn,KB_X,KB_Y+ 2*8,&key_pause,0),
  SETUP_MENU_KEY("AUTOMAP"    ,m_scrn,KB_X,KB_Y+ 3*8,&key_map,0),
  SETUP_MENU_KEY("VOLUME"     ,m_scrn,KB_X,KB_Y+ 4*8,&key_soundvolume,0),
  SETUP_MENU_KEY("HUD"        ,m_scrn,KB_X,KB_Y+ 5*8,&key_hud,0),
  SETUP_MENU_KEY("MESSAGES"   ,m_scrn,KB_X,KB_Y+ 6*8,&key_messages,0),
  SETUP_MENU_KEY("GAMMA FIX"  ,m_scrn,KB_X,KB_Y+ 7*8,&key_gamma,0),
  SETUP_MENU_KEY("SPY"        ,m_scrn,KB_X,KB_Y+ 8*8,&key_spy,0),
  SETUP_MENU_KEY("LARGER VIEW",m_scrn,KB_X,KB_Y+ 9*8,&key_zoomin,0),
  SETUP_MENU_KEY("SMALLER VIEW",m_scrn,KB_X,KB_Y+10*8,&key_zoomout,0),
  SETUP_MENU_KEY("SCREENSHOT" ,m_scrn,KB_X,KB_Y+11*8,&key_screenshot,0),

  SETUP_MENU_TITLE("GAME"            ,KB_X,KB_Y+12*8),
  SETUP_MENU_KEY("SAVE"       ,m_scrn,KB_X,KB_Y+13*8,&key_savegame,0),
  SETUP_MENU_KEY("LOAD"       ,m_scrn,KB_X,KB_Y+14*8,&key_loadgame,0),
  SETUP_MENU_KEY("QUICKSAVE"  ,m_scrn,KB_X,KB_Y+15*8,&key_quicksave,0),
  SETUP_MENU_KEY("QUICKLOAD"  ,m_scrn,KB_X,KB_Y+16*8,&key_quickload,0),
  SETUP_MENU_KEY("END GAME"   ,m_scrn,KB_X,KB_Y+17*8,&key_endgame,0),
  SETUP_MENU_KEY("QUIT"       ,m_scrn,KB_X,KB_Y+18*8,&key_quit,0),

  SETUP_MENU_PREV(keys_settings1,KB_PREV,KB_Y+20*8),
  SETUP_MENU_NEXT(keys_settings3,KB_NEXT,KB_Y+20*8),

  // Final entry
  SETUP_MENU_END
};

setup_menu_t keys_settings3[] =  // Key Binding screen strings
{
  SETUP_MENU_TITLE("WEAPONS"      ,KB_X,KB_Y),
  SETUP_MENU_KEY("FIST"    ,m_scrn,KB_X,KB_Y+ 1*8,&key_weapon1,0),
  SETUP_MENU_KEY("PISTOL"  ,m_scrn,KB_X,KB_Y+ 2*8,&key_weapon2,0),
  SETUP_MENU_KEY("SHOTGUN" ,m_scrn,KB_X,KB_Y+ 3*8,&key_weapon3,0),
  SETUP_MENU_KEY("CHAINGUN",m_scrn,KB_X,KB_Y+ 4*8,&key_weapon4,0),
  SETUP_MENU_KEY("ROCKET"  ,m_scrn,KB_X,KB_Y+ 5*8,&key_weapon5,0),
  SETUP_MENU_KEY("PLASMA"  ,m_scrn,KB_X,KB_Y+ 6*8,&key_weapon6,0),
  SETUP_MENU_KEY("BFG"     ,m_scrn,KB_X,KB_Y+ 7*8,&key_weapon7,0),
  SETUP_MENU_KEY("CHAINSAW",m_scrn,KB_X,KB_Y+ 8*8,&key_weapon8,0),
  SETUP_MENU_KEY("SSG"     ,m_scrn,KB_X,KB_Y+ 9*8,&key_weapon9,0),
  SETUP_MENU_KEY("BEST"    ,m_scrn,KB_X,KB_Y+10*8,&key_weapontoggle,0),
  SETUP_MENU_KEY("FIRE"    ,m_scrn,KB_X,KB_Y+11*8,&key_fire,&mousebfire),
  SETUP_MENU_KEY("NEXT"    ,m_scrn,KB_X,KB_Y+12*8,&key_weaponcycleup,0),
  SETUP_MENU_KEY("PREV"    ,m_scrn,KB_X,KB_Y+13*8,&key_weaponcycledown,0),

  SETUP_MENU_PREV(keys_settings2,KB_PREV,KB_Y+20*8),
  SETUP_MENU_NEXT(keys_settings4,KB_NEXT,KB_Y+20*8),

  // Final entry
  SETUP_MENU_END
};

setup_menu_t keys_settings4[] =  // Key Binding screen strings
{
  SETUP_MENU_TITLE("AUTOMAP"        ,KB_X,KB_Y),
  SETUP_MENU_KEY("FOLLOW"    ,m_map ,KB_X,KB_Y+ 1*8,&key_map_follow,0),
  SETUP_MENU_KEY("ZOOM IN"   ,m_map ,KB_X,KB_Y+ 2*8,&key_map_zoomin,0),
  SETUP_MENU_KEY("ZOOM OUT"  ,m_map ,KB_X,KB_Y+ 3*8,&key_map_zoomout,0),
  SETUP_MENU_KEY("SHIFT UP"  ,m_map ,KB_X,KB_Y+ 4*8,&key_map_up,0),
  SETUP_MENU_KEY("SHIFT DOWN",m_map ,KB_X,KB_Y+ 5*8,&key_map_down,0),
  SETUP_MENU_KEY("SHIFT LEFT",m_map ,KB_X,KB_Y+ 6*8,&key_map_left,0),
  SETUP_MENU_KEY("SHIFT RIGHT",m_map ,KB_X,KB_Y+ 7*8,&key_map_right,0),
  SETUP_MENU_KEY("MARK PLACE",m_map ,KB_X,KB_Y+ 8*8,&key_map_mark,0),
  SETUP_MENU_KEY("CLEAR MARKS",m_map ,KB_X,KB_Y+ 9*8,&key_map_clear,0),
  SETUP_MENU_KEY("FULL/ZOOM" ,m_map ,KB_X,KB_Y+10*8,&key_map_gobig,0),
  SETUP_MENU_KEY("GRID"      ,m_map ,KB_X,KB_Y+11*8,&key_map_grid,0),

  SETUP_MENU_TITLE("CHATTING"       ,KB_X,KB_Y+12*8),
  SETUP_MENU_KEY("BEGIN CHAT",m_scrn,KB_X,KB_Y+13*8,&key_chat,0),
  SETUP_MENU_KEY("PLAYER 1"  ,m_scrn,KB_X,KB_Y+14*8,&destination_keys[0],0),
  SETUP_MENU_KEY("PLAYER 2"  ,m_scrn,KB_X,KB_Y+15*8,&destination_keys[1],0),
  SETUP_MENU_KEY("PLAYER 3"  ,m_scrn,KB_X,KB_Y+16*8,&destination_keys[2],0),
  SETUP_MENU_KEY("PLAYER 4"  ,m_scrn,KB_X,KB_Y+17*8,&destination_keys[3],0),
  SETUP_MENU_KEY("BACKSPACE" ,m_scrn,KB_X,KB_Y+18*8,&key_backspace,0),
  SETUP_MENU_KEY("ENTER"     ,m_scrn,KB_X,KB_Y+19*8,&key_enter,0),

  SETUP_MENU_PREV(keys_settings3,KB_PREV,KB_Y+20*8),

  // Final entry
  SETUP_MENU_END
};

// Setting up for the Key Binding screen. Turn on flags, set pointers,
// locate the first item on the screen where the cursor is allowed to
// land.

void M_KeyBindings(int choice)
{
  M_SetupNextMenu(&KeybndDef);

  setup_active = TRUE;
  setup_screen = ss_keys;
  set_keybnd_active = TRUE;
  setup_select = FALSE;
  default_verify = FALSE;
  setup_gather = FALSE;
  mult_screens_index = 0;
  current_setup_menu = keys_settings[0];
  set_menu_itemon = 0;
  while (current_setup_menu[set_menu_itemon++].m_flags & S_SKIP);
  current_setup_menu[--set_menu_itemon].m_flags |= S_HILITE;
}

// The drawing part of the Key Bindings Setup initialization. Draw the
// background, title, instruction line, and items.

void M_DrawKeybnd(void)
{
  menuactive = mnact_full;

  // Set up the Key Binding screen

  M_DrawBackground("FLOOR4_6", 0); // Draw background

  // proff/nicolas 09/20/98 -- changed for hi-res
  M_DrawTitle(84, 2, "M_KEYBND", CR_DEFAULT, "KEY BINDINGS", CR_GOLD);
  M_DrawInstructions();
  M_DrawScreenItems(current_setup_menu);

  // If the Reset Button has been selected, an "Are you sure?" message
  // is overlayed across everything else.

  if (default_verify)
    M_DrawDefVerify();
}

/////////////////////////////
//
// The Weapon Screen tables.

#define WP_X 203
#define WP_Y  33

// There's only one weapon settings screen (for now). But since we're
// trying to fit a common description for screens, it gets a setup_menu_t,
// which only has one screen definition in it.
//
// Note that this screen has no PREV or NEXT items, since there are no
// neighboring screens.

enum {           // killough 10/98: enum for y-offset info
  weap_recoil,
  weap_bobbing,
  weap_bfg,
  weap_stub1,
  weap_pref1,
  weap_pref2,
  weap_pref3,
  weap_pref4,
  weap_pref5,
  weap_pref6,
  weap_pref7,
  weap_pref8,
  weap_pref9,
  weap_stub2,
  weap_toggle,
  weap_toggle2,
};

setup_menu_t weap_settings1[];

setup_menu_t* weap_settings[] =
{
  weap_settings1,
  NULL
};

setup_menu_t weap_settings1[] =  // Weapons Settings screen
{
  SETUP_MENU("ENABLE RECOIL", S_YESNO,m_null,WP_X, WP_Y+ weap_recoil*8, {"weapon_recoil"}),
  SETUP_MENU("ENABLE BOBBING",S_YESNO,m_null,WP_X, WP_Y+weap_bobbing*8, {"player_bobbing"}),

  SETUP_MENU("1ST CHOICE WEAPON",S_WEAP,m_null,WP_X,WP_Y+weap_pref1*8, {"weapon_choice_1"}),
  SETUP_MENU("2nd CHOICE WEAPON",S_WEAP,m_null,WP_X,WP_Y+weap_pref2*8, {"weapon_choice_2"}),
  SETUP_MENU("3rd CHOICE WEAPON",S_WEAP,m_null,WP_X,WP_Y+weap_pref3*8, {"weapon_choice_3"}),
  SETUP_MENU("4th CHOICE WEAPON",S_WEAP,m_null,WP_X,WP_Y+weap_pref4*8, {"weapon_choice_4"}),
  SETUP_MENU("5th CHOICE WEAPON",S_WEAP,m_null,WP_X,WP_Y+weap_pref5*8, {"weapon_choice_5"}),
  SETUP_MENU("6th CHOICE WEAPON",S_WEAP,m_null,WP_X,WP_Y+weap_pref6*8, {"weapon_choice_6"}),
  SETUP_MENU("7th CHOICE WEAPON",S_WEAP,m_null,WP_X,WP_Y+weap_pref7*8, {"weapon_choice_7"}),
  SETUP_MENU("8th CHOICE WEAPON",S_WEAP,m_null,WP_X,WP_Y+weap_pref8*8, {"weapon_choice_8"}),
  SETUP_MENU("9th CHOICE WEAPON",S_WEAP,m_null,WP_X,WP_Y+weap_pref9*8, {"weapon_choice_9"}),

  SETUP_MENU("Enable Fist/Chainsaw\n& SG/SSG toggle", S_YESNO, m_null, WP_X,
  WP_Y+ weap_toggle*8, {"doom_weapon_toggles"}),

  // Button for resetting to defaults
  SETUP_MENU_RESET,

  // Final entry
  SETUP_MENU_END
};

// Setting up for the Weapons screen. Turn on flags, set pointers,
// locate the first item on the screen where the cursor is allowed to
// land.

void M_Weapons(int choice)
{
  M_SetupNextMenu(&WeaponDef);

  setup_active = TRUE;
  setup_screen = ss_weap;
  set_weapon_active = TRUE;
  setup_select = FALSE;
  default_verify = FALSE;
  setup_gather = FALSE;
  mult_screens_index = 0;
  current_setup_menu = weap_settings[0];
  set_menu_itemon = 0;
  while (current_setup_menu[set_menu_itemon++].m_flags & S_SKIP);
  current_setup_menu[--set_menu_itemon].m_flags |= S_HILITE;
}


// The drawing part of the Weapons Setup initialization. Draw the
// background, title, instruction line, and items.

void M_DrawWeapons(void)
{
  menuactive = mnact_full;

  M_DrawBackground("FLOOR4_6", 0); // Draw background

  // proff/nicolas 09/20/98 -- changed for hi-res
  M_DrawTitle(109, 2, "M_WEAP", CR_DEFAULT, "WEAPONS", CR_GOLD);
  M_DrawInstructions();
  M_DrawScreenItems(current_setup_menu);

  // If the Reset Button has been selected, an "Are you sure?" message
  // is overlayed across everything else.

  if (default_verify)
    M_DrawDefVerify();
}

/////////////////////////////
//
// The Status Bar / HUD tables.

#define ST_X 203
#define ST_Y  31

// Screen table definitions

setup_menu_t stat_settings1[];

setup_menu_t* stat_settings[] =
{
  stat_settings1,
  NULL
};

static const char *hud_modes[] = {"OFF", "COMPACT", "DISTRIBUTED", NULL};

setup_menu_t stat_settings1[] =  // Status Bar and HUD Settings screen
{
  SETUP_MENU_TITLE("STATUS BAR"        ,ST_X,ST_Y+ 1*8 ),

  {"USE RED NUMBERS"   ,S_YESNO, m_null,ST_X,ST_Y+ 2*8, {"sts_always_red"}, 0, 0, NULL, NULL},
  {"GRAY %"            ,S_YESNO, m_null,ST_X,ST_Y+ 3*8, {"sts_pct_always_gray"}, 0, 0, NULL, NULL},
  {"SINGLE KEY DISPLAY",S_YESNO, m_null,ST_X,ST_Y+ 4*8, {"sts_traditional_keys"}, 0, 0, NULL, NULL},

  SETUP_MENU_TITLE("HEADS-UP DISPLAY"  ,ST_X,ST_Y+ 6*8),

  {"HUD DISPLAY MODE"  ,S_CHOICE    ,m_null,ST_X,ST_Y+ 7*8, {"hud_mode"}, 0, 0, NULL, hud_modes},
  {"SHOW KILL/ITEM/SECRET", S_YESNO ,m_null,ST_X,ST_Y+ 8*8, {"hud_showstats"}, 0, 0, NULL, NULL},
  {"SHOW WEAPONS"      ,S_YESNO     ,m_null,ST_X,ST_Y+ 9*8, {"hud_showweapons"}, 0, 0, NULL, NULL},
  {"SHOW KEYS"         ,S_YESNO     ,m_null,ST_X,ST_Y+10*8, {"hud_showkeys"}, 0, 0, NULL, NULL},
  {"HEALTH LOW/OK"     ,S_NUM       ,m_null,ST_X,ST_Y+11*8, {"health_red"}, 0, 0, NULL, NULL},
  {"HEALTH OK/GOOD"    ,S_NUM       ,m_null,ST_X,ST_Y+12*8, {"health_yellow"}, 0, 0, NULL, NULL},
  {"HEALTH GOOD/EXTRA" ,S_NUM       ,m_null,ST_X,ST_Y+13*8, {"health_green"}, 0, 0, NULL, NULL},
  {"ARMOR LOW/OK"      ,S_NUM       ,m_null,ST_X,ST_Y+14*8, {"armor_red"}, 0, 0, NULL, NULL},
  {"ARMOR OK/GOOD"     ,S_NUM       ,m_null,ST_X,ST_Y+15*8, {"armor_yellow"}, 0, 0, NULL, NULL},
  {"ARMOR GOOD/EXTRA"  ,S_NUM       ,m_null,ST_X,ST_Y+16*8, {"armor_green"}, 0, 0, NULL, NULL},
  {"AMMO LOW/OK"       ,S_NUM       ,m_null,ST_X,ST_Y+17*8, {"ammo_red"}, 0, 0, NULL, NULL},
  {"AMMO OK/GOOD"      ,S_NUM       ,m_null,ST_X,ST_Y+18*8, {"ammo_yellow"}, 0, 0, NULL, NULL},

  // Button for resetting to defaults
  SETUP_MENU_RESET,

  // Final entry
  SETUP_MENU_END
};

// Setting up for the Status Bar / HUD screen. Turn on flags, set pointers,
// locate the first item on the screen where the cursor is allowed to
// land.

void M_StatusBar(int choice)
{
  M_SetupNextMenu(&StatusHUDDef);

  setup_active = TRUE;
  setup_screen = ss_stat;
  set_status_active = TRUE;
  setup_select = FALSE;
  default_verify = FALSE;
  setup_gather = FALSE;
  mult_screens_index = 0;
  current_setup_menu = stat_settings[0];
  set_menu_itemon = 0;
  while (current_setup_menu[set_menu_itemon++].m_flags & S_SKIP);
  current_setup_menu[--set_menu_itemon].m_flags |= S_HILITE;
}


// The drawing part of the Status Bar / HUD Setup initialization. Draw the
// background, title, instruction line, and items.

void M_DrawStatusHUD(void)
{
  menuactive = mnact_full;

  M_DrawBackground("FLOOR4_6", 0); // Draw background

  // proff/nicolas 09/20/98 -- changed for hi-res
  M_DrawTitle(59, 2, "M_STAT", CR_DEFAULT, "STATUS BAR / HUD", CR_GOLD);
  M_DrawInstructions();
  M_DrawScreenItems(current_setup_menu);

  // If the Reset Button has been selected, an "Are you sure?" message
  // is overlayed across everything else.

  if (default_verify)
    M_DrawDefVerify();
}


/////////////////////////////
//
// The Automap tables.

#define AU_X    250
#define AU_Y     31
#define AU_PREV KB_PREV
#define AU_NEXT KB_NEXT

setup_menu_t auto_settings1[];
setup_menu_t auto_settings2[];

setup_menu_t* auto_settings[] =
{
  auto_settings1,
  auto_settings2,
  NULL
};

setup_menu_t auto_settings1[] =  // 1st AutoMap Settings screen
{
  SETUP_MENU("background", S_COLOR, m_null, AU_X, AU_Y, {"mapcolor_back"}),
  SETUP_MENU("grid lines", S_COLOR, m_null, AU_X, AU_Y + 1*8, {"mapcolor_grid"}),
  SETUP_MENU("normal 1s wall", S_COLOR, m_null,AU_X,AU_Y+ 2*8, {"mapcolor_wall"}),
  SETUP_MENU("line at floor height change", S_COLOR, m_null, AU_X, AU_Y+ 3*8, {"mapcolor_fchg"}),
  SETUP_MENU("line at ceiling height change"      ,S_COLOR,m_null,AU_X,AU_Y+ 4*8, {"mapcolor_cchg"}),
  SETUP_MENU("line at sector with floor = ceiling",S_COLOR,m_null,AU_X,AU_Y+ 5*8, {"mapcolor_clsd"}),
  SETUP_MENU("red key"                            ,S_COLOR,m_null,AU_X,AU_Y+ 6*8, {"mapcolor_rkey"}),
  SETUP_MENU("blue key"                           ,S_COLOR,m_null,AU_X,AU_Y+ 7*8, {"mapcolor_bkey"}),
  SETUP_MENU("yellow key"                         ,S_COLOR,m_null,AU_X,AU_Y+ 8*8, {"mapcolor_ykey"}),
  SETUP_MENU("red door"                           ,S_COLOR,m_null,AU_X,AU_Y+ 9*8, {"mapcolor_rdor"}),
  SETUP_MENU("blue door"                          ,S_COLOR,m_null,AU_X,AU_Y+10*8, {"mapcolor_bdor"}),
  SETUP_MENU("yellow door"                        ,S_COLOR,m_null,AU_X,AU_Y+11*8, {"mapcolor_ydor"}),

  SETUP_MENU("AUTOMAP LEVEL TITLE COLOR"      ,S_CRITEM,m_null,AU_X,AU_Y+13*8, {"hudcolor_titl"}),
  SETUP_MENU("AUTOMAP COORDINATES COLOR"      ,S_CRITEM,m_null,AU_X,AU_Y+14*8, {"hudcolor_xyco"}),

  SETUP_MENU("Show Secrets only after entering",S_YESNO,m_null,AU_X,AU_Y+15*8, {"map_secret_after"}),

  SETUP_MENU("Show coordinates of automap pointer",S_YESNO,m_null,AU_X,AU_Y+16*8, {"map_point_coord"}),

  // Button for resetting to defaults
  SETUP_MENU_RESET,

  SETUP_MENU_NEXT(auto_settings2, AU_NEXT, AU_Y+20*8),

  // Final entry
  SETUP_MENU_END
};

setup_menu_t auto_settings2[] =  // 2nd AutoMap Settings screen
{
  SETUP_MENU("teleporter line"                ,S_COLOR ,m_null,AU_X,AU_Y, {"mapcolor_tele"}),
  SETUP_MENU("secret sector boundary"         ,S_COLOR ,m_null,AU_X,AU_Y+ 1*8, {"mapcolor_secr"}),
  //jff 4/23/98 add exit line to automap
  SETUP_MENU("exit line"                      ,S_COLOR ,m_null,AU_X,AU_Y+ 2*8, {"mapcolor_exit"}),
  SETUP_MENU("computer map unseen line"       ,S_COLOR ,m_null,AU_X,AU_Y+ 3*8, {"mapcolor_unsn"}),
  SETUP_MENU("line w/no floor/ceiling changes",S_COLOR ,m_null,AU_X,AU_Y+ 4*8, {"mapcolor_flat"}),
  SETUP_MENU("general sprite"                 ,S_COLOR ,m_null,AU_X,AU_Y+ 5*8, {"mapcolor_sprt"}),
  SETUP_MENU("countable enemy sprite"         ,S_COLOR ,m_null,AU_X,AU_Y+ 6*8, {"mapcolor_enemy"}),  // cph 2006/06/30
  SETUP_MENU("countable item sprite"          ,S_COLOR ,m_null,AU_X,AU_Y+ 7*8, {"mapcolor_item"}),   // mead 3/4/2003
  SETUP_MENU("crosshair"                      ,S_COLOR ,m_null,AU_X,AU_Y+ 8*8, {"mapcolor_hair"}),
  SETUP_MENU("single player arrow"            ,S_COLOR ,m_null,AU_X,AU_Y+ 9*8, {"mapcolor_sngl"}),
  SETUP_MENU("your colour in multiplayer"     ,S_COLOR ,m_null,AU_X,AU_Y+10*8, {"mapcolor_me"}),

  SETUP_MENU("friends"                        ,S_COLOR ,m_null,AU_X,AU_Y+12*8, {"mapcolor_frnd"}),   // killough 8/8/98

  SETUP_MENU_PREV(auto_settings1, AU_PREV,AU_Y+20*8),

  // Final entry
  SETUP_MENU_END
};


// Setting up for the Automap screen. Turn on flags, set pointers,
// locate the first item on the screen where the cursor is allowed to
// land.

void M_Automap(int choice)
{
  M_SetupNextMenu(&AutoMapDef);

  setup_active = TRUE;
  setup_screen = ss_auto;
  set_auto_active = TRUE;
  setup_select = FALSE;
  colorbox_active = FALSE;
  default_verify = FALSE;
  setup_gather = FALSE;
  set_menu_itemon = 0;
  mult_screens_index = 0;
  current_setup_menu = auto_settings[0];
  while (current_setup_menu[set_menu_itemon++].m_flags & S_SKIP);
  current_setup_menu[--set_menu_itemon].m_flags |= S_HILITE;
}

// Data used by the color palette that is displayed for the player to
// select colors.

int color_palette_x; // X position of the cursor on the color palette
int color_palette_y; // Y position of the cursor on the color palette
uint8_t palette_background[16*(CHIP_SIZE+1)+8];

// M_DrawColPal() draws the color palette when the user needs to select a
// color.

// phares 4/1/98: now uses a single lump for the palette instead of
// building the image out of individual paint chips.

static void M_DrawColPal(void)
{
  int cpx, cpy;

  // Draw a background, border, and paint chips

  // proff/nicolas 09/20/98 -- changed for hi-res
  // CPhipps - patch drawing updated
  V_DrawNamePatch(COLORPALXORIG-5, COLORPALYORIG-5, 0, "M_COLORS", CR_DEFAULT, VPT_STRETCH);

  // Draw the cursor around the paint chip
  // (cpx,cpy) is the upper left-hand corner of the paint chip

  cpx = COLORPALXORIG+color_palette_x*(CHIP_SIZE+1)-1;
  cpy = COLORPALYORIG+color_palette_y*(CHIP_SIZE+1)-1;
  // proff 12/6/98: Drawing of colorchips completly changed for hi-res, it now uses a patch
  V_DrawNamePatch(cpx,cpy,0,"M_PALSEL",CR_DEFAULT,VPT_STRETCH); // PROFF_GL_FIX
}

// The drawing part of the Automap Setup initialization. Draw the
// background, title, instruction line, and items.

void M_DrawAutoMap(void)
{
  menuactive = mnact_full;

  M_DrawBackground("FLOOR4_6", 0); // Draw background

  // CPhipps - patch drawing updated
  M_DrawTitle(109, 2, "M_AUTO", CR_DEFAULT, "AUTOMAP", CR_GOLD);
  M_DrawInstructions();
  M_DrawScreenItems(current_setup_menu);

  // If a color is being selected, need to show color paint chips

  if (colorbox_active)
    M_DrawColPal();

  // If the Reset Button has been selected, an "Are you sure?" message
  // is overlayed across everything else.

  else if (default_verify)
    M_DrawDefVerify();
}


/////////////////////////////
//
// The Enemies table.

#define E_X 250
#define E_Y  31

setup_menu_t enem_settings1[];

setup_menu_t* enem_settings[] =
{
  enem_settings1,
  NULL
};

enum {
  enem_infighting,

  enem_remember = 1,

  enem_backing,
  enem_monkeys,
  enem_avoid_hazards,
  enem_friction,
  enem_help_friends,

  enem_distfriend,

  enem_end
};

setup_menu_t enem_settings1[] =  // Enemy Settings screen
{
  // killough 7/19/98
  SETUP_MENU("Monster Infighting When Provoked",S_YESNO,m_null,E_X,E_Y+ enem_infighting*8, {"monster_infighting"}),

  SETUP_MENU("Remember Previous Enemy",S_YESNO,m_null,E_X,E_Y+ enem_remember*8, {"monsters_remember"}),

  // killough 9/8/98
  SETUP_MENU("Monster Backing Out",S_YESNO,m_null,E_X,E_Y+ enem_backing*8, {"monster_backing"}),

  SETUP_MENU("Climb Steep Stairs", S_YESNO,m_null,E_X,E_Y+enem_monkeys*8, {"monkeys"}),

  // killough 9/9/98
  SETUP_MENU("Intelligently Avoid Hazards",S_YESNO,m_null,E_X,E_Y+ enem_avoid_hazards*8, {"monster_avoid_hazards"}),

  // killough 10/98
  SETUP_MENU("Affected by Friction",S_YESNO,m_null,E_X,E_Y+ enem_friction*8, {"monster_friction"}),

  SETUP_MENU("Rescue Dying Friends",S_YESNO,m_null,E_X,E_Y+ enem_help_friends*8, {"help_friends"}),

  // Button for resetting to defaults
  SETUP_MENU_RESET,

  // Final entry
  SETUP_MENU_END
};

/////////////////////////////

// Setting up for the Enemies screen. Turn on flags, set pointers,
// locate the first item on the screen where the cursor is allowed to
// land.

void M_Enemy(int choice)
{
  M_SetupNextMenu(&EnemyDef);

  setup_active = TRUE;
  setup_screen = ss_enem;
  set_enemy_active = TRUE;
  setup_select = FALSE;
  default_verify = FALSE;
  setup_gather = FALSE;
  mult_screens_index = 0;
  current_setup_menu = enem_settings[0];
  set_menu_itemon = 0;
  while (current_setup_menu[set_menu_itemon++].m_flags & S_SKIP);
  current_setup_menu[--set_menu_itemon].m_flags |= S_HILITE;
}

// The drawing part of the Enemies Setup initialization. Draw the
// background, title, instruction line, and items.

void M_DrawEnemy(void)
{
  menuactive = mnact_full;

  M_DrawBackground("FLOOR4_6", 0); // Draw background

  // proff/nicolas 09/20/98 -- changed for hi-res
  M_DrawTitle(114, 2, "M_ENEM", CR_DEFAULT, "ENEMIES", CR_GOLD);
  M_DrawInstructions();
  M_DrawScreenItems(current_setup_menu);

  // If the Reset Button has been selected, an "Are you sure?" message
  // is overlayed across everything else.

  if (default_verify)
    M_DrawDefVerify();
}


/////////////////////////////
//
// The General table.
// killough 10/10/98

extern int detect_voices, tran_filter_pct;

setup_menu_t gen_settings1[], gen_settings2[], gen_settings3[];

setup_menu_t* gen_settings[] =
{
  gen_settings1,
  gen_settings2,
  gen_settings3,
  NULL
};

enum {
  general_title_video,
  general_fps,
  general_gamma,

  general_title_sound,
  general_sndchan,
  general_pitch,
  general_mus_external,

  general_title_freelook,
  general_mouselook,
  general_mouseinvert,
  general_maxviewpitch,
};

#define G_X 250
#define G_YA  44
#define G_YA2 (G_YA + 16)
#define G_YA3 (G_YA2 + 16)
#define GF_X 76

static const char *framerates[] = {"35fps", "40fps", "50fps", "60fps", "70fps", "72fps", "75fps", "90fps", "100fps", "119fps", "120fps", "140fps", "144fps", "240fps", "244fps", "300fps", "360fps", NULL};
static const char *gamma_lvls[] = {"OFF", "Lv. 1", "Lv. 2", "Lv. 3", "Lv. 4", NULL};
static const char *mus_external_opts[] = {"Never", "Always", "Only IWAD", NULL};

setup_menu_t gen_settings1[] = { // General Settings screen1

  SETUP_MENU_TITLE("Video", G_X, G_YA - 2),

  {"Framerate", S_CHOICE, m_null, G_X,
  G_YA + general_fps*8, {"uncapped_framerate"}, 0, 0, M_ChangeFramerate, framerates},

  {"Gamma Correction", S_CHOICE, m_null, G_X,
  G_YA + general_gamma*8, {"usegamma"}, 0, 0, NULL, gamma_lvls},


  SETUP_MENU_TITLE("Sound & Music", G_X, G_YA2 + general_title_sound*8 - 2),

  {"Number of Sound Channels", S_NUM|S_PRGWARN, m_null, G_X,
   G_YA2 + general_sndchan*8, {"snd_channels"}, 0, 0, NULL, NULL},

  {"Enable v1.1 Pitch Effects", S_YESNO, m_null, G_X,
   G_YA2 + general_pitch*8, {"pitched_sounds"}, 0, 0, NULL, NULL},

  {"Play external MP3 files", S_CHOICE, m_null, G_X,
   G_YA2 + general_mus_external*8, {"mus_load_external"}, 0, 0, NULL, mus_external_opts},


  SETUP_MENU_TITLE("Freelook", G_X, G_YA3 + general_title_freelook*8 - 2),

  {"Enable Vertical Mouse Look", S_YESNO, m_null, G_X,
   G_YA3 + general_mouselook*8, {"movement_mouselook"}, 0, 0, M_ChangeMouseLook, NULL},

  {"Invert Vertical Mouse", S_YESNO, m_null, G_X,
   G_YA3 + general_mouseinvert*8, {"movement_mouseinvert"}, 0, 0, NULL, NULL},

  {"Maximum Vertical Pitch", S_NUM, m_null, G_X,
   G_YA3 + general_maxviewpitch*8, {"movement_maxviewpitch"}, 0, 0, M_ChangeMaxViewPitch, NULL},

  // Button for resetting to defaults
  SETUP_MENU_RESET,

  SETUP_MENU_NEXT(gen_settings2, KB_NEXT,KB_Y+20*8),

  // Final entry
  SETUP_MENU_END
};

enum {
  general_title_preload,
  general_wad1,
  general_wad2,
  general_deh1,
  general_deh2,

  general_title_misc,
  general_corpse,
  general_smooth,
  general_smoothfactor,
  general_defskill,
};


#define G_YB  60
#define G_YB1 (G_YB+20)

static const char *gen_skillstrings[] = {
  // Dummy first option because defaultskill is 1-based
  "", "ITYTD", "HNTR", "HMP", "UV", "NM", NULL
};

setup_menu_t gen_settings2[] = { // General Settings screen2

  SETUP_MENU_TITLE("Files Preloaded at Game Startup", G_X, G_YB - 2),

  {"WAD # 1",     S_FILE, m_null, GF_X, G_YB + general_wad1*8, {"wadfile_1"}, 0,0,NULL,NULL},
  {"WAD #2",      S_FILE, m_null, GF_X, G_YB + general_wad2*8, {"wadfile_2"}, 0,0,NULL,NULL},
  {"DEH/BEX # 1", S_FILE, m_null, GF_X, G_YB + general_deh1*8, {"dehfile_1"}, 0,0,NULL,NULL},
  {"DEH/BEX #2",  S_FILE, m_null, GF_X, G_YB + general_deh2*8, {"dehfile_2"}, 0,0,NULL,NULL},

  SETUP_MENU_TITLE("Miscellaneous", G_X, G_YB1 + general_title_misc*8 - 2),

  {"Maximum number of player corpses", S_NUM|S_PRGWARN, m_null, G_X,
   G_YB1 + general_corpse*8, {"max_player_corpse"}, 0, 0, NULL, NULL},

  {"Smooth Demo Playback", S_YESNO, m_null, G_X,
   G_YB1 + general_smooth*8, {"demo_smoothturns"}, 0, 0, M_ChangeDemoSmoothTurns, NULL},

  {"Smooth Demo Playback Factor", S_NUM, m_null, G_X,
   G_YB1 + general_smoothfactor*8, {"demo_smoothturnsfactor"}, 0, 0, M_ChangeDemoSmoothTurns, NULL},

  {"Default skill level", S_CHOICE, m_null, G_X,
    G_YB1 + general_defskill*8, {"default_skill"}, 0, 0, NULL, gen_skillstrings},

  SETUP_MENU_PREV(gen_settings1, KB_PREV, KB_Y+20*8),
  SETUP_MENU_NEXT(gen_settings3, KB_NEXT, KB_Y+20*8),

  // Final entry
  SETUP_MENU_END
};

enum {
  general_title_display,
  general_filterwall,
  general_filterfloor,
  general_filtersprite,
  general_filterpatch,
  general_filterz,
  general_spriteedges,
  general_patchedges,
  general_hom,
  general_skystretch,
  general_wigglefix,
  general_menubg,
};


#define G_YC  44
#define G_YC2 (G_YC+6)
#define G_YC3 (G_YC2+6)

static const char *renderfilters[] = {"none", "point", "linear", "rounded", NULL};
static const char *edgetypes[] = {"jagged", "sloped", NULL};

setup_menu_t gen_settings3[] = { // General Settings screen2

  SETUP_MENU_TITLE("Display options",    G_X, G_YC - 2),

  {"Filter for walls", S_CHOICE, m_null, G_X,
   G_YC + general_filterwall*8, {"filter_wall"}, 0, 0, NULL, renderfilters},

  {"Filter for floors/ceilings", S_CHOICE, m_null, G_X,
   G_YC + general_filterfloor*8, {"filter_floor"}, 0, 0, NULL, renderfilters},

  {"Filter for sprites", S_CHOICE, m_null, G_X,
   G_YC + general_filtersprite*8, {"filter_sprite"}, 0, 0, NULL, renderfilters},

  {"Filter for patches", S_CHOICE, m_null, G_X,
   G_YC + general_filterpatch*8, {"filter_patch"}, 0, 0, NULL, renderfilters},

  {"Filter for lighting", S_CHOICE, m_null, G_X,
   G_YC + general_filterz*8, {"filter_z"}, 0, 0, NULL, renderfilters},

  {"Drawing of sprite edges", S_CHOICE, m_null, G_X,
   G_YC + general_spriteedges*8, {"sprite_edges"}, 0, 0, NULL, edgetypes},

  {"Drawing of patch edges", S_CHOICE, m_null, G_X,
   G_YC + general_patchedges*8, {"patch_edges"}, 0, 0, NULL, edgetypes},

  {"Flashing HOM indicator", S_YESNO, m_null, G_X,
   G_YC2 + general_hom*8, {"flashing_hom"}, 0, 0, NULL, NULL},

  {"Stretch sky on freelook", S_YESNO, m_null, G_X,
   G_YC2 + general_skystretch*8, {"render_stretchsky"}, 0, 0, M_ChangeMouseLook, NULL},

  {"Wiggle geometry fix", S_YESNO, m_null, G_X,
   G_YC2 + general_wigglefix*8, {"r_wiggle_fix"}, 0, 0, NULL, NULL},

  {"Fullscreen menu background", S_YESNO, m_null, G_X,
   G_YC3 + general_menubg*8, {"menu_background"}, 0, 0, NULL, NULL},

  SETUP_MENU_PREV(gen_settings2, KB_PREV, KB_Y+20*8),

  // Final entry
  SETUP_MENU_END
};

void M_ChangeDemoSmoothTurns(void)
{
  if (demo_smoothturns)
    gen_settings2[general_smoothfactor].m_flags &= ~(S_SKIP|S_SELECT);
  else
    gen_settings2[general_smoothfactor].m_flags |= (S_SKIP|S_SELECT);

  R_SmoothPlaying_Reset(NULL);
}

void M_ChangeFramerate(void)
{
  R_InitInterpolation();
  G_ScaleMovementToFramerate();
}

void M_ChangeMouseLook(void)
{
  if (movement_mouselook) {
    gen_settings1[general_mouseinvert].m_flags  &= ~(S_SKIP|S_SELECT);
    gen_settings1[general_maxviewpitch].m_flags &= ~(S_SKIP|S_SELECT);
    gen_settings3[general_skystretch].m_flags   &= ~(S_SKIP|S_SELECT);
  } else {
    gen_settings1[general_mouseinvert].m_flags  |= (S_SKIP|S_SELECT);
    gen_settings1[general_maxviewpitch].m_flags |= (S_SKIP|S_SELECT);
    gen_settings3[general_skystretch].m_flags   |= (S_SKIP|S_SELECT);
  }
  R_InitSkyMap();
  viewpitch = 0;
}

void M_ChangeMaxViewPitch(void)
{
  angle_t angle = (angle_t)((float)movement_maxviewpitch / 45.0f * ANG45);

  viewpitch_max = (+angle - (1<<ANGLETOFINESHIFT));
  viewpitch_min = (-angle + (1<<ANGLETOFINESHIFT));

  viewpitch = 0;
}

// Setting up for the General screen. Turn on flags, set pointers,
// locate the first item on the screen where the cursor is allowed to
// land.

void M_General(int choice)
{
  M_SetupNextMenu(&GeneralDef);

  setup_active = TRUE;
  setup_screen = ss_gen;
  set_general_active = TRUE;
  setup_select = FALSE;
  default_verify = FALSE;
  setup_gather = FALSE;
  mult_screens_index = 0;
  current_setup_menu = gen_settings[0];
  set_menu_itemon = 0;
  while (current_setup_menu[set_menu_itemon++].m_flags & S_SKIP);
  current_setup_menu[--set_menu_itemon].m_flags |= S_HILITE;
}

// The drawing part of the General Setup initialization. Draw the
// background, title, instruction line, and items.

void M_DrawGeneral(void)
{
  menuactive = mnact_full;

  M_DrawBackground("FLOOR4_6", 0); // Draw background

  // proff/nicolas 09/20/98 -- changed for hi-res
  M_DrawTitle(114, 2, "M_GENERL", CR_DEFAULT, "GENERAL", CR_GOLD);
  M_DrawInstructions();
  M_DrawScreenItems(current_setup_menu);

  // If the Reset Button has been selected, an "Are you sure?" message
  // is overlayed across everything else.

  if (default_verify)
    M_DrawDefVerify();
}

/////////////////////////////
//
// The Compatibility table.
// killough 10/10/98

#define C_X  284
#define C_Y  32
#define COMP_SPC 12
#define C_NEXTPREV 131

setup_menu_t comp_settings1[], comp_settings2[], comp_settings3[];

setup_menu_t* comp_settings[] =
{
  comp_settings1,
  comp_settings2,
  comp_settings3,
  NULL
};

enum
{
  compat_telefrag,
  compat_dropoff,
  compat_falloff,
  compat_staylift,
  compat_doorstuck,
  compat_pursuit,
  compat_vile,
  compat_pain,
  compat_skull,
  compat_blazing,
  compat_doorlight = 0,
  compat_god,
  compat_infcheat,
  compat_zombie,
  compat_skymap,
  compat_stairs,
  compat_floors,
  compat_moveblock,
  compat_model,
  compat_zerotags,
  compat_666 = 0,
  compat_soul,
  compat_maskedanim,
  compat_sound
};

setup_menu_t comp_settings1[] =  // Compatibility Settings screen #1
{
  {"Any monster can telefrag on MAP30", S_YESNO, m_null, C_X,
   C_Y + compat_telefrag * COMP_SPC, {"comp_telefrag"}, 0, 0, NULL, NULL},

  {"Some objects never hang over tall ledges", S_YESNO, m_null, C_X,
   C_Y + compat_dropoff * COMP_SPC, {"comp_dropoff"}, 0, 0, NULL, NULL},

  {"Objects don't fall under their own weight", S_YESNO, m_null, C_X,
   C_Y + compat_falloff * COMP_SPC, {"comp_falloff"}, 0, 0, NULL, NULL},

  {"Monsters randomly walk off of moving lifts", S_YESNO, m_null, C_X,
   C_Y + compat_staylift * COMP_SPC, {"comp_staylift"}, 0, 0, NULL, NULL},

  {"Monsters get stuck on doortracks", S_YESNO, m_null, C_X,
   C_Y + compat_doorstuck * COMP_SPC, {"comp_doorstuck"}, 0, 0, NULL, NULL},

  {"Monsters don't give up pursuit of targets", S_YESNO, m_null, C_X,
   C_Y + compat_pursuit * COMP_SPC, {"comp_pursuit"}, 0, 0, NULL, NULL},

  {"Arch-Vile resurrects invincible ghosts", S_YESNO, m_null, C_X,
   C_Y + compat_vile * COMP_SPC, {"comp_vile"}, 0, 0, NULL, NULL},

  {"Pain Elementals limited to 21 lost souls", S_YESNO, m_null, C_X,
   C_Y + compat_pain * COMP_SPC, {"comp_pain"}, 0, 0, NULL, NULL},

  {"Lost souls get stuck behind walls", S_YESNO, m_null, C_X,
   C_Y + compat_skull * COMP_SPC, {"comp_skull"}, 0, 0, NULL, NULL},

  {"Blazing doors make double closing sounds", S_YESNO, m_null, C_X,
   C_Y + compat_blazing * COMP_SPC, {"comp_blazing"}, 0, 0, NULL, NULL},

  // Button for resetting to defaults
  SETUP_MENU_RESET,

  SETUP_MENU_NEXT(comp_settings2, KB_NEXT, C_Y+C_NEXTPREV),

  // Final entry
  SETUP_MENU_END
};

setup_menu_t comp_settings2[] =  // Compatibility Settings screen #2
{
  {"Tagged doors don't trigger special lighting", S_YESNO, m_null, C_X,
   C_Y + compat_doorlight * COMP_SPC, {"comp_doorlight"}, 0, 0, NULL, NULL},

  {"God mode isn't absolute", S_YESNO, m_null, C_X,
   C_Y + compat_god * COMP_SPC, {"comp_god"}, 0, 0, NULL, NULL},

  {"Powerup cheats are not infinite duration", S_YESNO, m_null, C_X,
   C_Y + compat_infcheat * COMP_SPC, {"comp_infcheat"}, 0, 0, NULL, NULL},

  {"Dead players can exit levels", S_YESNO, m_null, C_X,
   C_Y + compat_zombie * COMP_SPC, {"comp_zombie"}, 0, 0, NULL, NULL},

  {"Sky is unaffected by invulnerability", S_YESNO, m_null, C_X,
   C_Y + compat_skymap * COMP_SPC, {"comp_skymap"}, 0, 0, NULL, NULL},

  {"Use exactly Doom's stairbuilding method", S_YESNO, m_null, C_X,
   C_Y + compat_stairs * COMP_SPC, {"comp_stairs"}, 0, 0, NULL, NULL},

  {"Use exactly Doom's floor motion behavior", S_YESNO, m_null, C_X,
   C_Y + compat_floors * COMP_SPC, {"comp_floors"}, 0, 0, NULL, NULL},

  {"Use exactly Doom's movement clipping code", S_YESNO, m_null, C_X,
   C_Y + compat_moveblock * COMP_SPC, {"comp_moveblock"}, 0, 0, NULL, NULL},

  {"Use exactly Doom's linedef trigger model", S_YESNO, m_null, C_X,
   C_Y + compat_model * COMP_SPC, {"comp_model"}, 0, 0, NULL, NULL},

  {"Linedef effects work with sector tag = 0", S_YESNO, m_null, C_X,
   C_Y + compat_zerotags * COMP_SPC, {"comp_zerotags"}, 0, 0, NULL, NULL},

  SETUP_MENU_PREV(comp_settings1, KB_PREV, C_Y+C_NEXTPREV),
  SETUP_MENU_NEXT(comp_settings3, KB_NEXT, C_Y+C_NEXTPREV),

  // Final entry
  SETUP_MENU_END
};

setup_menu_t comp_settings3[] =  // Compatibility Settings screen #2
{
  {"All boss types can trigger tag 666 at ExM8", S_YESNO, m_null, C_X,
   C_Y + compat_666 * COMP_SPC, {"comp_666"}, 0, 0, NULL, NULL},

  {"Lost souls don't bounce off flat surfaces", S_YESNO, m_null, C_X,
   C_Y + compat_soul * COMP_SPC, {"comp_soul"}, 0, 0, NULL, NULL},

  {"2S middle textures do not animate", S_YESNO, m_null, C_X,
   C_Y + compat_maskedanim * COMP_SPC, {"comp_maskedanim"}, 0, 0, NULL, NULL},

  {"Use exactly Doom's sound code behavior", S_YESNO, m_null, C_X,
   C_Y + compat_sound * COMP_SPC, {"comp_sound"}, 0, 0, NULL, NULL},

  SETUP_MENU_PREV(comp_settings2, KB_PREV, C_Y+C_NEXTPREV),

  // Final entry
  SETUP_MENU_END
};

// Setting up for the Compatibility screen. Turn on flags, set pointers,
// locate the first item on the screen where the cursor is allowed to
// land.

void M_Compat(int choice)
{
  M_SetupNextMenu(&CompatDef);

  setup_active = TRUE;
  setup_screen = ss_comp;
  set_general_active = TRUE;
  setup_select = FALSE;
  default_verify = FALSE;
  setup_gather = FALSE;
  mult_screens_index = 0;
  current_setup_menu = comp_settings[0];
  set_menu_itemon = 0;
  while (current_setup_menu[set_menu_itemon++].m_flags & S_SKIP);
  current_setup_menu[--set_menu_itemon].m_flags |= S_HILITE;
}

// The drawing part of the Compatibility Setup initialization. Draw the
// background, title, instruction line, and items.

void M_DrawCompat(void)
{
  menuactive = mnact_full;

  M_DrawBackground("FLOOR4_6", 0); // Draw background

  M_DrawTitle(52, 2, "M_COMPAT", CR_DEFAULT, "DOOM COMPATIBILITY", CR_GOLD);
  M_DrawInstructions();
  M_DrawScreenItems(current_setup_menu);

  // If the Reset Button has been selected, an "Are you sure?" message
  // is overlayed across everything else.

  if (default_verify)
    M_DrawDefVerify();
}

/////////////////////////////
//
// The Messages table.

#define M_X 230
#define M_Y  39

// killough 11/98: enumerated

enum {
  mess_color_play,
  mess_timer,
  mess_color_chat,
  mess_chat_timer,
  mess_color_review,
  mess_timed,
  mess_hud_timer,
  mess_lines,
  mess_scrollup,
  mess_background,
};

setup_menu_t mess_settings1[];

setup_menu_t* mess_settings[] =
{
  mess_settings1,
  NULL
};

setup_menu_t mess_settings1[] =  // Messages screen
{
  {"Message Color During Play", S_CRITEM, m_null, M_X,
   M_Y + mess_color_play*8, {"hudcolor_mesg"}, 0, 0, NULL, NULL},

#if 0
  {"Message Duration During Play (ms)", S_NUM, m_null, M_X,
   M_Y  + mess_timer*8, {"message_timer"}, 0, 0, NULL, NULL},
#endif

  {"Chat Message Color", S_CRITEM, m_null, M_X,
   M_Y + mess_color_chat*8, {"hudcolor_chat"}, 0, 0, NULL, NULL},

#if 0
  {"Chat Message Duration (ms)", S_NUM, m_null, M_X,
   M_Y  + mess_chat_timer*8, {"chat_msg_timer"}, 0, 0, NULL, NULL},
#endif

  {"Message Review Color", S_CRITEM, m_null, M_X,
   M_Y + mess_color_review*8, {"hudcolor_list"}, 0, 0, NULL, NULL},

#if 0
  {"Message Listing Review is Temporary",  S_YESNO,  m_null,  M_X,
   M_Y + mess_timed*8, {"hud_msg_timed"}, 0, 0, NULL, NULL},

  {"Message Review Duration (ms)", S_NUM, m_null, M_X,
   M_Y  + mess_hud_timer*8, {"hud_msg_timer"}, 0, 0, NULL, NULL},
#endif

  {"Number of Review Message Lines", S_NUM, m_null,  M_X,
   M_Y + mess_lines*8, {"hud_msg_lines"}, 0, 0, NULL, NULL},

#if 0
  {"Message Listing Scrolls Upwards",  S_YESNO,  m_null,  M_X,
   M_Y + mess_scrollup*8, {"hud_msg_scrollup"}, 0, 0, NULL, NULL},
#endif

  {"Message Background",  S_YESNO,  m_null,  M_X,
   M_Y + mess_background*8, {"hud_list_bgon"}, 0, 0, NULL, NULL},

  // Button for resetting to defaults
  SETUP_MENU_RESET,

  // Final entry
  SETUP_MENU_END
};


// Setting up for the Messages screen. Turn on flags, set pointers,
// locate the first item on the screen where the cursor is allowed to
// land.

void M_Messages(int choice)
{
  M_SetupNextMenu(&MessageDef);

  setup_active = TRUE;
  setup_screen = ss_mess;
  set_mess_active = TRUE;
  setup_select = FALSE;
  default_verify = FALSE;
  setup_gather = FALSE;
  mult_screens_index = 0;
  current_setup_menu = mess_settings[0];
  set_menu_itemon = 0;
  while (current_setup_menu[set_menu_itemon++].m_flags & S_SKIP);
  current_setup_menu[--set_menu_itemon].m_flags |= S_HILITE;
}


// The drawing part of the Messages Setup initialization. Draw the
// background, title, instruction line, and items.

void M_DrawMessages(void)
{
  menuactive = mnact_full;

  M_DrawBackground("FLOOR4_6", 0); // Draw background

  // CPhipps - patch drawing updated
  M_DrawTitle(103, 2, "M_MESS", CR_DEFAULT, "MESSAGES", CR_GOLD);
  M_DrawInstructions();
  M_DrawScreenItems(current_setup_menu);
  if (default_verify)
    M_DrawDefVerify();
}


/////////////////////////////
//
// The Chat Strings table.

#define CS_X 20
#define CS_Y (31+8)

setup_menu_t chat_settings1[];

setup_menu_t* chat_settings[] =
{
  chat_settings1,
  NULL
};

setup_menu_t chat_settings1[] =  // Chat Strings screen
{
  {"1",S_CHAT,m_null,CS_X,CS_Y+ 1*8, {"chatmacro1"},0,0,NULL,NULL},
  {"2",S_CHAT,m_null,CS_X,CS_Y+ 2*8, {"chatmacro2"},0,0,NULL,NULL},
  {"3",S_CHAT,m_null,CS_X,CS_Y+ 3*8, {"chatmacro3"},0,0,NULL,NULL},
  {"4",S_CHAT,m_null,CS_X,CS_Y+ 4*8, {"chatmacro4"},0,0,NULL,NULL},
  {"5",S_CHAT,m_null,CS_X,CS_Y+ 5*8, {"chatmacro5"},0,0,NULL,NULL},
  {"6",S_CHAT,m_null,CS_X,CS_Y+ 6*8, {"chatmacro6"},0,0,NULL,NULL},
  {"7",S_CHAT,m_null,CS_X,CS_Y+ 7*8, {"chatmacro7"},0,0,NULL,NULL},
  {"8",S_CHAT,m_null,CS_X,CS_Y+ 8*8, {"chatmacro8"},0,0,NULL,NULL},
  {"9",S_CHAT,m_null,CS_X,CS_Y+ 9*8, {"chatmacro9"},0,0,NULL,NULL},
  {"0",S_CHAT,m_null,CS_X,CS_Y+10*8, {"chatmacro0"},0,0,NULL,NULL},

  // Button for resetting to defaults
  SETUP_MENU_RESET,

  // Final entry
  SETUP_MENU_END
};

// Setting up for the Chat Strings screen. Turn on flags, set pointers,
// locate the first item on the screen where the cursor is allowed to
// land.

void M_ChatStrings(int choice)
{
  M_SetupNextMenu(&ChatStrDef);
  setup_active = TRUE;
  setup_screen = ss_chat;
  set_chat_active = TRUE;
  setup_select = FALSE;
  default_verify = FALSE;
  setup_gather = FALSE;
  mult_screens_index = 0;
  current_setup_menu = chat_settings[0];
  set_menu_itemon = 0;
  while (current_setup_menu[set_menu_itemon++].m_flags & S_SKIP);
  current_setup_menu[--set_menu_itemon].m_flags |= S_HILITE;
}

// The drawing part of the Chat Strings Setup initialization. Draw the
// background, title, instruction line, and items.

void M_DrawChatStrings(void)
{
  menuactive = mnact_full;

  M_DrawBackground("FLOOR4_6", 0); // Draw background

  // CPhipps - patch drawing updated
  M_DrawTitle(83, 2, "M_CHAT", CR_DEFAULT, "CHAT STRINGS", CR_GOLD);
  M_DrawInstructions();
  M_DrawScreenItems(current_setup_menu);

  // If the Reset Button has been selected, an "Are you sure?" message
  // is overlayed across everything else.

  if (default_verify)
    M_DrawDefVerify();
}

/////////////////////////////
//
// General routines used by the Setup screens.
//

static dbool shiftdown = FALSE; // phares 4/10/98: SHIFT key down or not

// phares 4/17/98:
// M_SelectDone() gets called when you have finished entering your
// Setup Menu item change.

static void M_SelectDone(setup_menu_t* ptr)
{
  ptr->m_flags &= ~S_SELECT;
  ptr->m_flags |= S_HILITE;
  S_StartSound(NULL,sfx_itemup);
  setup_select = FALSE;
  colorbox_active = FALSE;
  if (print_warning_about_changes)     // killough 8/15/98
    print_warning_about_changes--;
}

// phares 4/21/98:
// Array of setup screens used by M_ResetDefaults()

static setup_menu_t **setup_screens[] =
{
  keys_settings,
  weap_settings,
  stat_settings,
  auto_settings,
  enem_settings,
  mess_settings,
  chat_settings,
  gen_settings,      // killough 10/98
  comp_settings,
};

// phares 4/19/98:
// M_ResetDefaults() resets all values for a setup screen to default values
//
// killough 10/98: rewritten to fix bugs and warn about pending changes

static void M_ResetDefaults(void)
{
  int i; //e6y

  default_t *dp;
  int warn = 0;

  // Look through the defaults table and reset every variable that
  // belongs to the group we're interested in.
  //
  // killough: However, only reset variables whose field in the
  // current setup screen is the same as in the defaults table.
  // i.e. only reset variables really in the current setup screen.

  // e6y
  // Fixed crash while trying to read data past array end
  // All previous versions of prboom worked only by a lucky accident
  // old code: for (dp = defaults; dp->name; dp++)
  for (i = 0; i < numdefaults ; i++)
  {
    dp = &defaults[i];

    if (dp->setupscreen == setup_screen)
      {
  setup_menu_t **l, *p;
  for (l = setup_screens[setup_screen-1]; *l; l++)
    for (p = *l; !(p->m_flags & S_END); p++)
      if (p->m_flags & S_HASDEFPTR ? p->var.def == dp :
    p->var.m_key == dp->location.pi ||
    p->m_mouse == dp->location.pi)
        {
    if (IS_STRING(*dp))
    {
      union { const char **c; char **s; } u; // type punning via unions

      u.c = dp->location.ppsz;
      free(*(u.s));
      *(u.c) = strdup(dp->defaultvalue.psz);
    }
    else
      *dp->location.pi = dp->defaultvalue.i;

#if 0
    if (p->m_flags & (S_LEVWARN | S_PRGWARN))
      warn |= p->m_flags & (S_LEVWARN | S_PRGWARN);
    else
      if (dp->current)
        if (allow_changes())
          *dp->current = *dp->location.pi;
        else
          warn |= S_LEVWARN;
#endif
    if (p->action)
      p->action();

    goto end;
        }
      end:;
      }
  }

  if (warn)
    warn_about_changes(warn);
}

//
// M_InitDefaults()
//
// killough 11/98:
//
// This function converts all setup menu entries consisting of cfg
// variable names, into pointers to the corresponding default[]
// array entry. var.name becomes converted to var.def.
//

static void M_InitDefaults(void)
{
  setup_menu_t *const *p, *t;
  default_t *dp;
  int i;
  for (i = 0; i < ss_max-1; i++)
    for (p = setup_screens[i]; *p; p++)
      for (t = *p; !(t->m_flags & S_END); t++)
  if (t->m_flags & S_HASDEFPTR) {
    if (!(dp = M_LookupDefault(t->var.name)))
      I_Error("M_InitDefaults: Couldn't find config variable %s", t->var.name);
    else
      (t->var.def = dp)->setup_menu = t;
  }
}

//
// End of Setup Screens.
//
/////////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////////
//
// Start of Extended HELP screens               // phares 3/30/98
//
// The wad designer can define a set of extended HELP screens for their own
// information display. These screens should be 320x200 graphic lumps
// defined in a separate wad. They should be named "HELP01" through "HELP99".
// "HELP01" is shown after the regular BOOM Dynamic HELP screen, and ENTER
// and BACKSPACE keys move the player through the HELP set.
//
// Rather than define a set of menu definitions for each of the possible
// HELP screens, one definition is used, and is altered on the fly
// depending on what HELPnn lumps the game finds.

// phares 3/30/98:
// Extended Help Screen variables

int extended_help_count;   // number of user-defined help screens found
int extended_help_index;   // index of current extended help screen

menuitem_t ExtHelpMenu[] =
{
   {1,"",M_ExtHelpNextScreen,0,NULL}
};

menu_t ExtHelpDef =
{
  1,             // # of menu items
  &ReadDef1,     // previous menu
  ExtHelpMenu,   // menuitem_t ->
  M_DrawExtHelp, // drawing routine ->
  330,181,       // x,y
  0              // lastOn
};

// M_ExtHelpNextScreen establishes the number of the next HELP screen in
// the series.

void M_ExtHelpNextScreen(int choice)
{
  (void)choice; // unused, but needed in method signature due to menu logic

  if (++extended_help_index > extended_help_count)
    {

      // when finished with extended help screens, return to Main Menu

      extended_help_index = 1;
      M_SetupNextMenu(&MainDef);
    }
}

// phares 3/30/98:
// Routine to look for HELPnn screens and create a menu
// definition structure that defines extended help screens.

void M_InitExtendedHelp(void)

{
  int index,i;
  char namebfr[] = { "HELPnn"} ;

  extended_help_count = 0;
  for (index = 1 ; index < 100 ; index++) {
    namebfr[4] = index/10 + 0x30;
    namebfr[5] = index%10 + 0x30;
    i = W_CheckNumForName(namebfr);
    if (i == -1) {
      if (extended_help_count) {
        if (gamemode == commercial) {
          ExtHelpDef.prevMenu  = &ReadDef1; /* previous menu */
          ReadMenu1[0].routine = M_ExtHelp;
  } else {
          ExtHelpDef.prevMenu  = &ReadDef2; /* previous menu */
          ReadMenu2[0].routine = M_ExtHelp;
  }
      }
      return;
    }
    extended_help_count++;
  }

}

// Initialization for the extended HELP screens.

void M_ExtHelp(int choice)
{
  (void)choice; // unused, but needed in method signature due to menu logic
  extended_help_index = 1; // Start with first extended help screen
  M_SetupNextMenu(&ExtHelpDef);
}

// Initialize the drawing part of the extended HELP screens.

void M_DrawExtHelp(void)
{
  char namebfr[10] = { "HELPnn" }; // CPhipps - make it local & writable

  inhelpscreens = TRUE;              // killough 5/1/98
  namebfr[4] = extended_help_index/10 + 0x30;
  namebfr[5] = extended_help_index%10 + 0x30;
  // CPhipps - patch drawing updated
  V_DrawNamePatch(0, 0, 0, namebfr, CR_DEFAULT, VPT_STRETCH);
}

//
// End of Extended HELP screens               // phares 3/30/98
//
////////////////////////////////////////////////////////////////////////////

////////////////////////////////////////////////////////////////////////////
//
// Dynamic HELP screen                     // phares 3/2/98
//
// Rather than providing the static HELP screens from DOOM and its versions,
// BOOM provides the player with a dynamic HELP screen that displays the
// current settings of major key bindings.
//
// The Dynamic HELP screen is defined in a manner similar to that used for
// the Setup Screens above.
//
// M_GetKeyString finds the correct string to represent the key binding
// for the current item being drawn.

int M_GetKeyString(int c,int offset)
{
  const char* s;

  if (c >= 33 && c <= 126) {

    // The '=', ',', and '.' keys originally meant the shifted
    // versions of those keys, but w/o having to shift them in
    // the game. Any actions that are mapped to these keys will
    // still mean their shifted versions. Could be changed later
    // if someone can come up with a better way to deal with them.

    if (c == '=')      // probably means the '+' key?
      c = '+';
    else if (c == ',') // probably means the '<' key?
      c = '<';
    else if (c == '.') // probably means the '>' key?
      c = '>';
    menu_buffer[offset++] = c; // Just insert the ascii key
    menu_buffer[offset] = 0;

  } else {

    // Retrieve 4-letter (max) string representing the key

    // cph - Keypad keys, general code reorganisation to
    //  make this smaller and neater.
    if ((0x100 <= c) && (c < 0x200)) {
      if (c == KEYD_KEYPADENTER)
  s = "PADE";
      else {
  strcpy(&menu_buffer[offset], "PAD");
  offset+=4;
  menu_buffer[offset-1] = c & 0xff;
  menu_buffer[offset] = 0;
      }
    } else if ((KEYD_F1 <= c) && (c < KEYD_F10)) {
      menu_buffer[offset++] = 'F';
      menu_buffer[offset++] = '1' + c - KEYD_F1;
      menu_buffer[offset]   = 0;
    } else {
      switch(c) {
      case KEYD_TAB:      s = "TAB";  break;
      case KEYD_ENTER:      s = "ENTR"; break;
      case KEYD_ESCAPE:     s = "ESC";  break;
      case KEYD_SPACEBAR:   s = "SPAC"; break;
      case KEYD_BACKSPACE:  s = "BACK"; break;
      case KEYD_RCTRL:      s = "CTRL"; break;
      case KEYD_LEFTARROW:  s = "LARR"; break;
      case KEYD_UPARROW:    s = "UARR"; break;
      case KEYD_RIGHTARROW: s = "RARR"; break;
      case KEYD_DOWNARROW:  s = "DARR"; break;
      case KEYD_RSHIFT:     s = "SHFT"; break;
      case KEYD_RALT:       s = "ALT";  break;
      case KEYD_CAPSLOCK:   s = "CAPS"; break;
      case KEYD_SCROLLLOCK: s = "SCRL"; break;
      case KEYD_HOME:       s = "HOME"; break;
      case KEYD_PAGEUP:     s = "PGUP"; break;
      case KEYD_END:        s = "END";  break;
      case KEYD_PAGEDOWN:   s = "PGDN"; break;
      case KEYD_INSERT:     s = "INST"; break;
      case KEYD_DEL:        s = "DEL"; break;
      case KEYD_F10:        s = "F10";  break;
      case KEYD_F11:        s = "F11";  break;
      case KEYD_F12:        s = "F12";  break;
      case KEYD_PAUSE:      s = "PAUS"; break;
      default:              s = "JUNK"; break;
      }

      if (s) { // cph - Slight code change
  strcpy(&menu_buffer[offset],s); // string to display
  offset += strlen(s);
      }
    }
  }
  return offset;
}

//
// The Dynamic HELP screen table.

#define KT_X1 283
#define KT_X2 172
#define KT_X3  87

#define KT_Y1   2
#define KT_Y2 118
#define KT_Y3 102

setup_menu_t helpstrings[] =  // HELP screen strings
{
  SETUP_MENU_TITLE("SCREEN"           ,KT_X1,KT_Y1),
  SETUP_MENU_KEY("HELP"        ,m_null,KT_X1,KT_Y1+ 1*8,&key_help,0),
  SETUP_MENU_KEY("MENU"        ,m_null,KT_X1,KT_Y1+ 2*8,&key_escape,0),
  SETUP_MENU_KEY("SETUP"       ,m_null,KT_X1,KT_Y1+ 3*8,&key_setup,0),
  SETUP_MENU_KEY("PAUSE"       ,m_null,KT_X1,KT_Y1+ 4*8,&key_pause,0),
  SETUP_MENU_KEY("AUTOMAP"     ,m_null,KT_X1,KT_Y1+ 5*8,&key_map,0),
  SETUP_MENU_KEY("SOUND VOLUME",m_null,KT_X1,KT_Y1+ 6*8,&key_soundvolume,0),
  SETUP_MENU_KEY("HUD"         ,m_null,KT_X1,KT_Y1+ 7*8,&key_hud,0),
  SETUP_MENU_KEY("MESSAGES"    ,m_null,KT_X1,KT_Y1+ 8*8,&key_messages,0),
  SETUP_MENU_KEY("GAMMA FIX"   ,m_null,KT_X1,KT_Y1+ 9*8,&key_gamma,0),
  SETUP_MENU_KEY("SPY"         ,m_null,KT_X1,KT_Y1+10*8,&key_spy,0),
  SETUP_MENU_KEY("LARGER VIEW" ,m_null,KT_X1,KT_Y1+11*8,&key_zoomin,0),
  SETUP_MENU_KEY("SMALLER VIEW",m_null,KT_X1,KT_Y1+12*8,&key_zoomout,0),
  SETUP_MENU_KEY("SCREENSHOT"  ,m_null,KT_X1,KT_Y1+13*8,&key_screenshot,0),

  SETUP_MENU_TITLE("AUTOMAP"          ,KT_X1,KT_Y2),
  SETUP_MENU_KEY("FOLLOW MODE" ,m_null,KT_X1,KT_Y2+ 1*8,&key_map_follow,0),
  SETUP_MENU_KEY("ZOOM IN"     ,m_null,KT_X1,KT_Y2+ 2*8,&key_map_zoomin,0),
  SETUP_MENU_KEY("ZOOM OUT"    ,m_null,KT_X1,KT_Y2+ 3*8,&key_map_zoomout,0),
  SETUP_MENU_KEY("MARK PLACE"  ,m_null,KT_X1,KT_Y2+ 4*8,&key_map_mark,0),
  SETUP_MENU_KEY("CLEAR MARKS" ,m_null,KT_X1,KT_Y2+ 5*8,&key_map_clear,0),
  SETUP_MENU_KEY("FULL/ZOOM"   ,m_null,KT_X1,KT_Y2+ 6*8,&key_map_gobig,0),
  SETUP_MENU_KEY("GRID"        ,m_null,KT_X1,KT_Y2+ 7*8,&key_map_grid,0),

  SETUP_MENU_TITLE("WEAPONS"          ,KT_X3,KT_Y1),
  SETUP_MENU_KEY("FIST"        ,m_null,KT_X3,KT_Y1+ 1*8,&key_weapon1,0),
  SETUP_MENU_KEY("PISTOL"      ,m_null,KT_X3,KT_Y1+ 2*8,&key_weapon2,0),
  SETUP_MENU_KEY("SHOTGUN"     ,m_null,KT_X3,KT_Y1+ 3*8,&key_weapon3,0),
  SETUP_MENU_KEY("CHAINGUN"    ,m_null,KT_X3,KT_Y1+ 4*8,&key_weapon4,0),
  SETUP_MENU_KEY("ROCKET"      ,m_null,KT_X3,KT_Y1+ 5*8,&key_weapon5,0),
  SETUP_MENU_KEY("PLASMA"      ,m_null,KT_X3,KT_Y1+ 6*8,&key_weapon6,0),
  SETUP_MENU_KEY("BFG 9000"    ,m_null,KT_X3,KT_Y1+ 7*8,&key_weapon7,0),
  SETUP_MENU_KEY("CHAINSAW"    ,m_null,KT_X3,KT_Y1+ 8*8,&key_weapon8,0),
  SETUP_MENU_KEY("SSG"         ,m_null,KT_X3,KT_Y1+ 9*8,&key_weapon9,0),
  SETUP_MENU_KEY("BEST"        ,m_null,KT_X3,KT_Y1+10*8,&key_weapontoggle,0),
  SETUP_MENU_KEY("FIRE"        ,m_null,KT_X3,KT_Y1+11*8,&key_fire,&mousebfire),

  SETUP_MENU_TITLE("MOVEMENT"         ,KT_X3,KT_Y3),
  SETUP_MENU_KEY("FORWARD"     ,m_null,KT_X3,KT_Y3+ 1*8,&key_up,&mousebforward),
  SETUP_MENU_KEY("BACKWARD"    ,m_null,KT_X3,KT_Y3+ 2*8,&key_down,&mousebbackward),
  SETUP_MENU_KEY("TURN LEFT"   ,m_null,KT_X3,KT_Y3+ 3*8,&key_left,0),
  SETUP_MENU_KEY("TURN RIGHT"  ,m_null,KT_X3,KT_Y3+ 4*8,&key_right,0),
  SETUP_MENU_KEY("RUN"         ,m_null,KT_X3,KT_Y3+ 5*8,&key_speed,0),
  SETUP_MENU_KEY("STRAFE LEFT" ,m_null,KT_X3,KT_Y3+ 6*8,&key_strafeleft,0),
  SETUP_MENU_KEY("STRAFE RIGHT",m_null,KT_X3,KT_Y3+ 7*8,&key_straferight,0),
  SETUP_MENU_KEY("STRAFE"      ,m_null,KT_X3,KT_Y3+ 8*8,&key_strafe,&mousebstrafe),
  SETUP_MENU_KEY("AUTORUN"     ,m_null,KT_X3,KT_Y3+ 9*8,&key_autorun,0),
  SETUP_MENU_KEY("180 TURN"    ,m_null,KT_X3,KT_Y3+10*8,&key_reverse,0),
  SETUP_MENU_KEY("USE"         ,m_null,KT_X3,KT_Y3+11*8,&key_use,&mousebforward),

  SETUP_MENU_TITLE("GAME"             ,KT_X2,KT_Y1),
  SETUP_MENU_KEY("SAVE"        ,m_null,KT_X2,KT_Y1+ 1*8,&key_savegame,0),
  SETUP_MENU_KEY("LOAD"        ,m_null,KT_X2,KT_Y1+ 2*8,&key_loadgame,0),
  SETUP_MENU_KEY("QUICKSAVE"   ,m_null,KT_X2,KT_Y1+ 3*8,&key_quicksave,0),
  SETUP_MENU_KEY("END GAME"    ,m_null,KT_X2,KT_Y1+ 4*8,&key_endgame,0),
  SETUP_MENU_KEY("QUICKLOAD"   ,m_null,KT_X2,KT_Y1+ 5*8,&key_quickload,0),
  SETUP_MENU_KEY("QUIT"        ,m_null,KT_X2,KT_Y1+ 6*8,&key_quit,0),

  // Final entry

  SETUP_MENU_END
};

#define SPACEWIDTH 4

/* cph 2006/08/06
 * M_DrawString() is the old M_DrawMenuString, except that it is not tied to
 * menu_buffer - no reason to force all the callers to write into one array! */

static void M_DrawString(int cx, int cy, int color, const char* ch)
{
  int   w;
  int   c;

  while (*ch) {
    c = *ch++;         // get next char
    c = toupper(c) - HU_FONTSTART;
    if (c < 0 || c> HU_FONTSIZE)
      {
      cx += SPACEWIDTH;    // space
      continue;
      }
    w = hu_font[c].width;
    if (cx + w > 320)
      break;

    // V_DrawpatchTranslated() will draw the string in the
    // desired color, colrngs[color]

    // CPhipps - patch drawing updated
    V_DrawNumPatch(cx, cy, 0, hu_font[c].lumpnum, color, VPT_STRETCH | VPT_TRANS);
    // The screen is cramped, so trim one unit from each
    // character so they butt up against each other.
    cx += w - 1;
  }
}

// M_DrawMenuString() draws the string in menu_buffer[]

static void M_DrawMenuString(int cx, int cy, int color)
{
    M_DrawString(cx, cy, color, menu_buffer);
}

// M_GetPixelWidth() returns the number of pixels in the width of
// the string, NOT the number of chars in the string.

static int M_GetPixelWidth(const char* ch)
{
  int len = 0;
  int c;

  while (*ch) {
    c = *ch++;    // pick up next char
    c = toupper(c) - HU_FONTSTART;
    if (c < 0 || c > HU_FONTSIZE)
      {
      len += SPACEWIDTH;   // space
      continue;
      }
    len += hu_font[c].width;
    len--; // adjust so everything fits
  }
  len++; // replace what you took away on the last char only
  return len;
}

static void M_DrawStringCentered(int cx, int cy, int color, const char* ch)
{
    M_DrawString(cx - M_GetPixelWidth(ch)/2, cy, color, ch);
}

//
// M_DrawHelp
//
// This displays the help screen

void M_DrawHelp (void)
{
  menuactive = mnact_full;

  M_DrawBackground("FLOOR4_6", 0);

  M_DrawScreenItems(helpstrings);
}

//
// End of Dynamic HELP screen                // phares 3/2/98
//
////////////////////////////////////////////////////////////////////////////

enum {
  prog,
  prog_stub,
  prog_stub1,
  prog_stub2,
  adcr
};

enum {
  cr_prog=0,
  cr_adcr=2,
};

#define CR_S 9
#define CR_X 20
#define CR_X2 50
#define CR_Y 32
#define CR_SH 9

setup_menu_t cred_settings[]={

  SETUP_MENU_CREDIT("Programmers", CR_X, CR_Y + CR_S*prog + CR_SH*cr_prog),
  SETUP_MENU_CREDIT("Florian 'Proff' Schulze", CR_X2, CR_Y + CR_S*(prog+1) + CR_SH*cr_prog),
  SETUP_MENU_CREDIT("Colin Phipps", CR_X2, CR_Y + CR_S*(prog+2) + CR_SH*cr_prog),
  SETUP_MENU_CREDIT("Neil Stevens", CR_X2, CR_Y + CR_S*(prog+3) + CR_SH*cr_prog),
  SETUP_MENU_CREDIT("Andrey Budko", CR_X2, CR_Y + CR_S*(prog+4) + CR_SH*cr_prog),

  SETUP_MENU_CREDIT("Additional Credit To", CR_X, CR_Y + CR_S*adcr + CR_SH*cr_adcr),
  SETUP_MENU_CREDIT("id Software for DOOM", CR_X2, CR_Y + CR_S*(adcr+1)+CR_SH*cr_adcr),
  SETUP_MENU_CREDIT("TeamTNT for BOOM", CR_X2, CR_Y + CR_S*(adcr+2)+CR_SH*cr_adcr),
  SETUP_MENU_CREDIT("Lee Killough for MBF", CR_X2, CR_Y + CR_S*(adcr+3)+CR_SH*cr_adcr),
  SETUP_MENU_CREDIT("The DOSDoom-Team for DOSDOOM", CR_X2, CR_Y + CR_S*(adcr+4)+CR_SH*cr_adcr),
  SETUP_MENU_CREDIT("Randy Heit for ZDOOM", CR_X2, CR_Y + CR_S*(adcr+5)+CR_SH*cr_adcr),
  SETUP_MENU_CREDIT("Michael 'Kodak' Ryssen for DOOMGL", CR_X2, CR_Y + CR_S*(adcr+6)+CR_SH*cr_adcr),
  SETUP_MENU_CREDIT("Jess Haas for lSDLDoom", CR_X2, CR_Y + CR_S*(adcr+7) + CR_SH*cr_adcr),
  SETUP_MENU_CREDIT("all others who helped (see AUTHORS file)", CR_X2, CR_Y + CR_S*(adcr+8)+CR_SH*cr_adcr),

  SETUP_MENU_END
};

void M_DrawCredits(void)     // killough 10/98: credit screen
{
  inhelpscreens = TRUE;
  // Use V_DrawBackground here deliberately to force drawing a background
  V_DrawBackground(gamemode==shareware ? "CEIL5_1" : "MFLR8_4", 0);
  M_DrawTitle(115, 9, "PRBOOM", CR_GOLD,
              PACKAGE_NAME " v" PACKAGE_VERSION, CR_GOLD);
  M_DrawScreenItems(cred_settings);
}

static int M_IndexInChoices(const char *str, const char **choices) {
  int i = 0;

  while (*choices != NULL) {
    if (!strcmp(str, *choices))
      return i;
    i++;
    choices++;
  }
  return 0;
}

/////////////////////////////////////////////////////////////////////////////
//
// M_Responder
//
// Examines incoming keystrokes and button pushes and determines some
// action based on the state of the system.
//

dbool M_Responder (event_t* ev) {
  int    ch;
  int    i;

  ch = -1; // will be changed to a legit char if we're going to use it here


      // Process keyboard input

      if (ev->type == ev_keydown)
        {
        ch = ev->data1;               // phares 4/11/98:
        if (ch == KEYD_RSHIFT)        // For chat string processing, need
          shiftdown = TRUE;           // to know when shift key is up or
        }                             // down so you can get at the !,#,
      else if (ev->type == ev_keyup)  // etc. keys. Keydowns are allowed
        if (ev->data1 == KEYD_RSHIFT) // past this point, but keyups aren't
          shiftdown = FALSE;          // so we need to note the difference

  if (ch == -1)
    return FALSE; // we can't use the event here

  // Save Game string input

   if (saveStringEnter) {
      if (ch == KEYD_ESCAPE)                    // phares 3/7/98
      {
         saveStringEnter = 0;
         strcpy(&savegamestrings[saveSlot][0],saveOldString);
      }

      else if (ch == key_fire || ch == KEYD_ENTER) // phares 3/7/98
      {
         saveStringEnter = 0;
         if (savegamestrings[saveSlot][0])
            M_DoSave(saveSlot);
      }

      else
      {
         ch = toupper(ch);

         if (ch >= 32 && ch <= 127 &&
               saveCharIndex < SAVESTRINGSIZE-1 &&
               M_StringWidth(savegamestrings[saveSlot]) < (SAVESTRINGSIZE-2)*8)
         {
            if (ch == KEYD_BACKSPACE && saveCharIndex > 0)
            {
               savegamestrings[saveSlot][saveCharIndex] = 0;
               (void) savegamestrings[saveSlot][saveCharIndex--];
               savegamestrings[saveSlot][saveCharIndex] = 0;
            }
            else if (ch != KEYD_BACKSPACE)
            {
               savegamestrings[saveSlot][saveCharIndex++] = ch;
               savegamestrings[saveSlot][saveCharIndex] = 0;
            }

         }
      }
      return TRUE;
  }

  // Take care of any messages that need input

  if (messageToPrint) {
    if (messageNeedsInput == TRUE &&
  !(ch == ' ' || ch == 'n' || ch == 'y' || ch == key_escape)) // phares
      return FALSE;

    menuactive = messageLastMenuActive;
    messageToPrint = 0;
    if (messageRoutine)
      messageRoutine(ch);

    menuactive = mnact_inactive;
    S_StartSound(NULL,sfx_swtchx);
    return TRUE;
  }

  // If there is no active menu displayed...

  if (!menuactive) {                                           // phares
    if (ch == key_autorun)      // Autorun                          //  V
      {
      autorun = !autorun;
      return TRUE;
      }

    if (ch == key_help)      // Help key
      {
      M_StartControlPanel ();

      currentMenu = &HelpDef;         // killough 10/98: new help screen

      itemOn = 0;
      S_StartSound(NULL,sfx_swtchn);
      return TRUE;
      }

    if (ch == key_savegame)     // Save Game
      {
      M_StartControlPanel();
      S_StartSound(NULL,sfx_swtchn);
      M_SaveGame(0);
      return TRUE;
      }

    if (ch == key_loadgame)     // Load Game
      {
      M_StartControlPanel();
      S_StartSound(NULL,sfx_swtchn);
      M_LoadGame(0);
      return TRUE;
      }

    if (ch == key_soundvolume)      // Sound Volume
      {
      M_StartControlPanel ();
      currentMenu = &SoundDef;
      itemOn = sfx_vol;
      S_StartSound(NULL,sfx_swtchn);
      return TRUE;
      }

    if (ch == key_quicksave)      // Quicksave
      {
      S_StartSound(NULL,sfx_swtchn);
      M_QuickSave();
      return TRUE;
      }

    if (ch == key_endgame)      // End game
      {
      S_StartSound(NULL,sfx_swtchn);
      M_EndGame(0);
      return TRUE;
      }

    if (ch == key_messages)      // Toggle messages
      {
      M_ChangeMessages(0);
      S_StartSound(NULL,sfx_swtchn);
      return TRUE;
      }

    if (ch == key_quickload)      // Quickload
      {
      S_StartSound(NULL,sfx_swtchn);
      M_QuickLoad();
      return TRUE;
      }

    if (ch == key_quit)       // Quit DOOM
      {
      S_StartSound(NULL,sfx_swtchn);
      has_exited = TRUE;
      return TRUE;
      }

    if (ch == key_gamma)       // gamma toggle
      {
      usegamma++;
      if (usegamma > 4)
  usegamma = 0;
      players[consoleplayer].message =
  usegamma == 0 ? s_GAMMALVL0 :
  usegamma == 1 ? s_GAMMALVL1 :
  usegamma == 2 ? s_GAMMALVL2 :
  usegamma == 3 ? s_GAMMALVL3 :
  s_GAMMALVL4;
      V_SetPalette(0);
      return TRUE;
      }


    if (ch == key_zoomout)     // zoom out
      {
      if ((automapmode & am_active) || chat_on)
        return FALSE;
      M_SizeDisplay(0);
      S_StartSound(NULL,sfx_stnmov);
      return TRUE;
      }

    if (ch == key_zoomin || ch == key_hud)     // zoom in
      {                                 // jff 2/23/98
      if ((automapmode & am_active) || chat_on)     // allow
        return FALSE;                   // key_hud==key_zoomin
      M_SizeDisplay(1);                                             //  ^
      S_StartSound(NULL,sfx_stnmov);                                //  |
      return TRUE;                                                  // phares
      }

    /* killough 10/98: allow key shortcut into Setup menu */
    if (ch == key_setup) {
      M_StartControlPanel();
      S_StartSound(NULL,sfx_swtchn);
      M_SetupNextMenu(&SetupDef);
      return TRUE;
    }
  }
  // Pop-up Main menu?

  if (!menuactive)
    {
    if (ch == key_escape)                                     // phares
      {
      M_StartControlPanel ();
      S_StartSound(NULL,sfx_swtchn);
      return TRUE;
      }
    return FALSE;
    }

  // phares 3/26/98 - 4/11/98:
  // Setup screen key processing

  if (setup_active) {
    setup_menu_t* ptr1= current_setup_menu + set_menu_itemon;
    setup_menu_t* ptr2 = NULL;

    // phares 4/19/98:
    // Catch the response to the 'reset to default?' verification
    // screen

    if (default_verify) {
      if (toupper(ch) == 'Y'
		  || ch == key_menu_enter) {
        M_ResetDefaults();
        default_verify = FALSE;
        M_SelectDone(ptr1);
      }
      else if (toupper(ch) == 'N'
               || ch == key_menu_escape
               || ch == key_menu_backspace) {
        default_verify = FALSE;
        M_SelectDone(ptr1);
      }
      return TRUE;
    }

      // Common processing for some items

      if (setup_select) { // changing an entry

  if (ptr1->m_flags & S_YESNO) // yes or no setting?
    {
    if (ch == key_menu_enter) {
      *ptr1->var.def->location.pi = !*ptr1->var.def->location.pi; // killough 8/15/98

      // phares 4/14/98:
      // If not in demoplayback, or netgame,
      // and there's a second variable in var2, set that
      // as well

      // killough 8/15/98: add warning messages

      if (ptr1->m_flags & (S_LEVWARN | S_PRGWARN))
        warn_about_changes(ptr1->m_flags &    // killough 10/98
         (S_LEVWARN | S_PRGWARN));
      else
        M_UpdateCurrent(ptr1->var.def);

      if (ptr1->action)      // killough 10/98
        ptr1->action();
    }
    M_SelectDone(ptr1);                           // phares 4/17/98
    return TRUE;
    }

  if (ptr1->m_flags & S_CRITEM)
    {
    if (ch != key_menu_enter)
      {
      ch -= 0x30; // out of ascii
      if (ch < 0 || ch > 9)
        return TRUE; // ignore
      *ptr1->var.def->location.pi = ch;
      }
    if (ptr1->action)      // killough 10/98
      ptr1->action();
    M_SelectDone(ptr1);                      // phares 4/17/98
    return TRUE;
    }

  if (ptr1->m_flags & S_NUM) // number?
    {
      if (setup_gather) { // gathering keys for a value?
        /* killough 10/98: Allow negatives, and use a more
         * friendly input method (e.g. don't clear value early,
         * allow backspace, and return to original value if bad
         * value is entered).
         */
        if (ch == key_menu_enter) {
    if (gather_count) {     // Any input?
      int value;

      gather_buffer[gather_count] = 0;
      value = atoi(gather_buffer);  // Integer value

      if ((ptr1->var.def->minvalue != UL &&
           value < ptr1->var.def->minvalue) ||
          (ptr1->var.def->maxvalue != UL &&
           value > ptr1->var.def->maxvalue))
        warn_about_changes(S_BADVAL);
      else {
        *ptr1->var.def->location.pi = value;

        /* killough 8/9/98: fix numeric vars
         * killough 8/15/98: add warning message
         */
        if (ptr1->m_flags & (S_LEVWARN | S_PRGWARN))
          warn_about_changes(ptr1->m_flags &
           (S_LEVWARN | S_PRGWARN));
        else
          M_UpdateCurrent(ptr1->var.def);

        if (ptr1->action)      // killough 10/98
          ptr1->action();
      }
    }
    M_SelectDone(ptr1);     // phares 4/17/98
    setup_gather = FALSE; // finished gathering keys
    return TRUE;
        }

        if (ch == key_menu_backspace && gather_count) {
    gather_count--;
    return TRUE;
        }

        if (gather_count >= MAXGATHER)
    return TRUE;

        if (!isdigit(ch) && ch != '-')
    return TRUE; // ignore

        /* killough 10/98: character-based numerical input */
        gather_buffer[gather_count++] = ch;
      }
      return TRUE;
    }

  if (ptr1->m_flags & S_CHOICE) // selection of choices?
    {
    if (ch == key_menu_left) {
      if (ptr1->var.def->type == def_int) {
        int value = *ptr1->var.def->location.pi;

        value = value - 1;
        if ((ptr1->var.def->minvalue != UL &&
             value < ptr1->var.def->minvalue))
          value = ptr1->var.def->minvalue;
        if ((ptr1->var.def->maxvalue != UL &&
             value > ptr1->var.def->maxvalue))
          value = ptr1->var.def->maxvalue;
        if (*ptr1->var.def->location.pi != value)
          S_StartSound(NULL,sfx_pstop);
        *ptr1->var.def->location.pi = value;
      }
      if (ptr1->var.def->type == def_str) {
        int old_value, value;

        old_value = M_IndexInChoices(*ptr1->var.def->location.ppsz,
                                     ptr1->selectstrings);
        value = old_value - 1;
        if (value < 0)
          value = 0;
        if (old_value != value)
          S_StartSound(NULL,sfx_pstop);
        *ptr1->var.def->location.ppsz = ptr1->selectstrings[value];
      }
    }
    if (ch == key_menu_right) {
      if (ptr1->var.def->type == def_int) {
        int value = *ptr1->var.def->location.pi;

        value = value + 1;
        if ((ptr1->var.def->minvalue != UL &&
             value < ptr1->var.def->minvalue))
          value = ptr1->var.def->minvalue;
        if ((ptr1->var.def->maxvalue != UL &&
             value > ptr1->var.def->maxvalue))
          value = ptr1->var.def->maxvalue;
        if (*ptr1->var.def->location.pi != value)
          S_StartSound(NULL,sfx_pstop);
        *ptr1->var.def->location.pi = value;
      }
      if (ptr1->var.def->type == def_str) {
        int old_value, value;

        old_value = M_IndexInChoices(*ptr1->var.def->location.ppsz,
                                     ptr1->selectstrings);
        value = old_value + 1;
        if (ptr1->selectstrings[value] == NULL)
          value = old_value;
        if (old_value != value)
          S_StartSound(NULL,sfx_pstop);
        *ptr1->var.def->location.ppsz = ptr1->selectstrings[value];
      }
    }
    if ((ch == key_menu_enter) ||
       (ch == key_menu_escape) || (ch == key_fire)) {
      // phares 4/14/98:
      // If not in demoplayback, or netgame,
      // and there's a second variable in var2, set that
      // as well

      // killough 8/15/98: add warning messages

      if (ptr1->m_flags & (S_LEVWARN | S_PRGWARN))
        warn_about_changes(ptr1->m_flags &    // killough 10/98
         (S_LEVWARN | S_PRGWARN));
      else
        M_UpdateCurrent(ptr1->var.def);

      if (ptr1->action)      // killough 10/98
        ptr1->action();
      M_SelectDone(ptr1);                           // phares 4/17/98
    }
    return TRUE;
    }

  if(ch == key_menu_escape) // Exit key = no change
  {
    M_SelectDone(ptr1);                           // phares 4/17/98
    setup_gather = FALSE;   // finished gathering keys, if any
    return TRUE;
  }

      }

      // Key Bindings

      if (set_keybnd_active) // on a key binding setup screen
  if (setup_select)    // incoming key or button gets bound
    {
      if (ev->type == ev_mouse)
        {
    unsigned int i,oldbutton,group;
    dbool search = TRUE;

    if (!ptr1->m_mouse)
      return TRUE; // not a legal action here (yet)

    // see if the button is already bound elsewhere. if so, you
    // have to swap bindings so the action where it's currently
    // bound doesn't go dead. Since there is more than one
    // keybinding screen, you have to search all of them for
    // any duplicates. You're only interested in the items
    // that belong to the same group as the one you're changing.

    oldbutton = *ptr1->m_mouse;
    group  = ptr1->m_group;
    if (ev->data1 & 1)
      ch = 0;
    else if (ev->data1 & 2)
      ch = 1;
    else if (ev->data1 & 4)
      ch = 2;
    else
      return TRUE;
    for (i = 0 ; keys_settings[i] && search ; i++)
      for (ptr2 = keys_settings[i] ; !(ptr2->m_flags & S_END) ; ptr2++)
        if (ptr2->m_group == group && ptr1 != ptr2)
          if (ptr2->m_flags & S_KEY && ptr2->m_mouse)
      if (*ptr2->m_mouse == ch)
        {
          *ptr2->m_mouse = oldbutton;
          search = FALSE;
          break;
        }
    *ptr1->m_mouse = ch;
        }
      else  // keyboard key
        {
    unsigned int i,oldkey,group;
    dbool search = TRUE;

    // see if 'ch' is already bound elsewhere. if so, you have
    // to swap bindings so the action where it's currently
    // bound doesn't go dead. Since there is more than one
    // keybinding screen, you have to search all of them for
    // any duplicates. You're only interested in the items
    // that belong to the same group as the one you're changing.

    // if you find that you're trying to swap with an action
    // that has S_KEEP set, you can't bind ch; it's already
    // bound to that S_KEEP action, and that action has to
    // keep that key.

    oldkey = *ptr1->var.m_key;
    group  = ptr1->m_group;
    for (i = 0 ; keys_settings[i] && search ; i++)
      for (ptr2 = keys_settings[i] ; !(ptr2->m_flags & S_END) ; ptr2++)
        if (ptr2->m_flags & (S_KEY|S_KEEP) &&
      ptr2->m_group == group &&
      ptr1 != ptr2)
          if (*ptr2->var.m_key == ch)
      {
        if (ptr2->m_flags & S_KEEP)
          return TRUE; // can't have it!
        *ptr2->var.m_key = oldkey;
        search = FALSE;
        break;
      }
    *ptr1->var.m_key = ch;
        }

      M_SelectDone(ptr1);       // phares 4/17/98
      return TRUE;
    }

      // Weapons

      if (set_weapon_active) // on the weapons setup screen
  if (setup_select) // changing an entry
    {
      if (ch != key_menu_enter)
        {
    ch -= '0'; // out of ascii
    if (ch < 1 || ch > 9)
      return TRUE; // ignore

    // Plasma and BFG don't exist in shareware
    // killough 10/98: allow it anyway, since this
    // isn't the game itself, just setting preferences

    // see if 'ch' is already assigned elsewhere. if so,
    // you have to swap assignments.

    // killough 11/98: simplified

    for (i = 0; (ptr2 = weap_settings[i]); i++)
      for (; !(ptr2->m_flags & S_END); ptr2++)
        if (ptr2->m_flags & S_WEAP &&
      *ptr2->var.def->location.pi == ch && ptr1 != ptr2)
          {
      *ptr2->var.def->location.pi = *ptr1->var.def->location.pi;
      goto end;
          }
        end:
    *ptr1->var.def->location.pi = ch;
        }

      M_SelectDone(ptr1);       // phares 4/17/98
      return TRUE;
    }

      // Automap

      if (set_auto_active) // on the automap setup screen
  if (setup_select) // incoming key
    {
      if (ch == key_menu_down)
        {
    if (++color_palette_y == 16)
      color_palette_y = 0;
    S_StartSound(NULL,sfx_itemup);
    return TRUE;
        }

      if (ch == key_menu_up)
        {
    if (--color_palette_y < 0)
      color_palette_y = 15;
    S_StartSound(NULL,sfx_itemup);
    return TRUE;
        }

      if (ch == key_menu_left)
        {
    if (--color_palette_x < 0)
      color_palette_x = 15;
    S_StartSound(NULL,sfx_itemup);
    return TRUE;
        }

      if (ch == key_menu_right)
        {
    if (++color_palette_x == 16)
      color_palette_x = 0;
    S_StartSound(NULL,sfx_itemup);
    return TRUE;
        }

      if (ch == key_menu_enter || ch == key_fire)
        {
    *ptr1->var.def->location.pi = color_palette_x + 16*color_palette_y;
    M_SelectDone(ptr1);                         // phares 4/17/98
    colorbox_active = FALSE;
    return TRUE;
        }
    }

      // killough 10/98: consolidate handling into one place:
      if (setup_select &&
    set_enemy_active | set_general_active | set_chat_active |
    set_mess_active | set_status_active | set_compat_active)
  {
    if (ptr1->m_flags & S_STRING) // creating/editing a string?
      {
        if (ch == key_menu_backspace) // backspace and DEL
    {
      if (chat_string_buffer[chat_index] == 0)
        {
          if (chat_index > 0)
      chat_string_buffer[--chat_index] = 0;
        }
      // shift the remainder of the text one char left
      else
        strcpy(&chat_string_buffer[chat_index],
         &chat_string_buffer[chat_index+1]);
    }
        else if (ch == key_menu_left) // move cursor left
    {
      if (chat_index > 0)
        chat_index--;
    }
        else if (ch == key_menu_right) // move cursor right
    {
      if (chat_string_buffer[chat_index] != 0)
        chat_index++;
    }
        else if ((ch == key_menu_enter) ||
           (ch == key_menu_escape) ||(ch == key_fire))
    {
      *ptr1->var.def->location.ppsz = chat_string_buffer;
      M_SelectDone(ptr1);   // phares 4/17/98
    }

        // Adding a char to the text. Has to be a printable
        // char, and you can't overrun the buffer. If the
        // chat string gets larger than what the screen can hold,
        // it is dealt with when the string is drawn (above).

        else if ((ch >= 32) && (ch <= 126))
    if ((chat_index+1) < CHAT_STRING_BFR_SIZE)
      {
        if (shiftdown)
          ch = shiftxform[ch];
        if (chat_string_buffer[chat_index] == 0)
          {
      chat_string_buffer[chat_index++] = ch;
      chat_string_buffer[chat_index] = 0;
          }
        else
          chat_string_buffer[chat_index++] = ch;
      }
        return TRUE;
      }

    M_SelectDone(ptr1);       // phares 4/17/98
    return TRUE;
  }

      // Not changing any items on the Setup screens. See if we're
      // navigating the Setup menus or selecting an item to change.

      if (ch == key_menu_down)
  {
    ptr1->m_flags &= ~S_HILITE;     // phares 4/17/98
    do
      if (ptr1->m_flags & S_END)
        {
    set_menu_itemon = 0;
    ptr1 = current_setup_menu;
        }
      else
        {
    set_menu_itemon++;
    ptr1++;
        }
    while (ptr1->m_flags & S_SKIP);
    M_SelectDone(ptr1);         // phares 4/17/98
    return TRUE;
  }

      if (ch == key_menu_up)
  {
    ptr1->m_flags &= ~S_HILITE;     // phares 4/17/98
    do
      {
        if (set_menu_itemon == 0)
    do
      set_menu_itemon++;
    while(!((current_setup_menu + set_menu_itemon)->m_flags & S_END));
        set_menu_itemon--;
      }
    while((current_setup_menu + set_menu_itemon)->m_flags & S_SKIP);
    M_SelectDone(current_setup_menu + set_menu_itemon);         // phares 4/17/98
    return TRUE;
  }

      if (ch == key_menu_enter || ch == key_fire)
  {
    int flags = ptr1->m_flags;

    // You've selected an item to change. Highlight it, post a new
    // message about what to do, and get ready to process the
    // change.
    //
    // killough 10/98: use friendlier char-based input buffer

    if (flags & S_NUM)
      {
        setup_gather = TRUE;
        print_warning_about_changes = FALSE;
        gather_count = 0;
      }
    else if (flags & S_COLOR)
      {
        int color = *ptr1->var.def->location.pi;

        if (color < 0 || color > 255) // range check the value
    color = 0;        // 'no show' if invalid

        color_palette_x = *ptr1->var.def->location.pi & 15;
        color_palette_y = *ptr1->var.def->location.pi >> 4;
        colorbox_active = TRUE;
      }
    else if (flags & S_STRING)
      {
        // copy chat string into working buffer; trim if needed.
        // free the old chat string memory and replace it with
        // the (possibly larger) new memory for editing purposes
        //
        // killough 10/98: fix bugs, simplify

        chat_string_buffer = malloc(CHAT_STRING_BFR_SIZE);
        strncpy(chat_string_buffer,
          *ptr1->var.def->location.ppsz, CHAT_STRING_BFR_SIZE);

        // guarantee null delimiter
        chat_string_buffer[CHAT_STRING_BFR_SIZE-1] = 0;

        // set chat table pointer to working buffer
        // and free old string's memory.
        {
          union { const char **c; char **s; } u; // type punning via unions

          u.c = ptr1->var.def->location.ppsz;
          free(*(u.s));
          *(u.c) = chat_string_buffer;
        }
        chat_index = 0; // current cursor position in chat_string_buffer
      }
    else if (flags & S_RESET)
      default_verify = TRUE;

    ptr1->m_flags |= S_SELECT;
    setup_select = TRUE;
    S_StartSound(NULL,sfx_itemup);
    return TRUE;
  }

      if ((ch == key_menu_escape) || (ch == key_menu_backspace))
  {
    if (ch == key_menu_escape) // Clear all menus
      M_ClearMenus();
    else // key_menu_backspace = return to Setup Menu
      if (currentMenu->prevMenu)
        {
    currentMenu = currentMenu->prevMenu;
    itemOn = currentMenu->lastOn;
    S_StartSound(NULL,sfx_swtchn);
        }
    ptr1->m_flags &= ~(S_HILITE|S_SELECT);// phares 4/19/98
    setup_active = FALSE;
    set_keybnd_active = FALSE;
    set_weapon_active = FALSE;
    set_status_active = FALSE;
    set_auto_active = FALSE;
    set_enemy_active = FALSE;
    set_mess_active = FALSE;
    set_chat_active = FALSE;
    colorbox_active = FALSE;
    default_verify = FALSE;       // phares 4/19/98
    set_general_active = FALSE;    // killough 10/98
          set_compat_active = FALSE;    // killough 10/98
    HU_Start();    // catch any message changes // phares 4/19/98
    S_StartSound(NULL,sfx_swtchx);
    return TRUE;
  }

      // Some setup screens may have multiple screens.
      // When there are multiple screens, m_prev and m_next items need to
      // be placed on the appropriate screen tables so the user can
      // move among the screens using the left and right arrow keys.
      // The m_var1 field contains a pointer to the appropriate screen
      // to move to.

      if (ch == key_menu_left)
  {
    ptr2 = ptr1;
    do
      {
        ptr2++;
        if (ptr2->m_flags & S_PREV)
    {
      ptr1->m_flags &= ~S_HILITE;
      mult_screens_index--;
      current_setup_menu = ptr2->var.menu;
      set_menu_itemon = 0;
      print_warning_about_changes = FALSE; // killough 10/98
      while (current_setup_menu[set_menu_itemon++].m_flags&S_SKIP);
      current_setup_menu[--set_menu_itemon].m_flags |= S_HILITE;
      S_StartSound(NULL,sfx_pstop);  // killough 10/98
      return TRUE;
    }
      }
    while (!(ptr2->m_flags & S_END));
  }

      if (ch == key_menu_right)
  {
    ptr2 = ptr1;
    do
      {
        ptr2++;
        if (ptr2->m_flags & S_NEXT)
    {
      ptr1->m_flags &= ~S_HILITE;
      mult_screens_index++;
      current_setup_menu = ptr2->var.menu;
      set_menu_itemon = 0;
      print_warning_about_changes = FALSE; // killough 10/98
      while (current_setup_menu[set_menu_itemon++].m_flags&S_SKIP);
      current_setup_menu[--set_menu_itemon].m_flags |= S_HILITE;
      S_StartSound(NULL,sfx_pstop);  // killough 10/98
      return TRUE;
    }
      }
    while (!(ptr2->m_flags & S_END));
  }

    } // End of Setup Screen processing

  // From here on, these navigation keys are used on the BIG FONT menus
  // like the Main Menu.

  if (ch == key_menu_down)                             // phares 3/7/98
    {
      do
  {
    if (itemOn+1 > currentMenu->numitems-1)
      itemOn = 0;
    else
      itemOn++;
    S_StartSound(NULL,sfx_pstop);
  }
      while(currentMenu->menuitems[itemOn].status==-1);
      return TRUE;
    }

  if (ch == key_menu_up)                               // phares 3/7/98
    {
      do
  {
    if (!itemOn)
      itemOn = currentMenu->numitems-1;
    else
      itemOn--;
    S_StartSound(NULL,sfx_pstop);
  }
      while(currentMenu->menuitems[itemOn].status==-1);
      return TRUE;
    }

  if (ch == key_menu_left)                             // phares 3/7/98
    {
      if (currentMenu->menuitems[itemOn].routine &&
    currentMenu->menuitems[itemOn].status == 2)
  {
    S_StartSound(NULL,sfx_stnmov);
    currentMenu->menuitems[itemOn].routine(0);
  }
      return TRUE;
    }

  if (ch == key_menu_right)                            // phares 3/7/98
    {
      if (currentMenu->menuitems[itemOn].routine &&
    currentMenu->menuitems[itemOn].status == 2)
  {
    S_StartSound(NULL,sfx_stnmov);
    currentMenu->menuitems[itemOn].routine(1);
  }
      return TRUE;
    }

  if (ch == key_menu_enter || ch == key_fire)                            // phares 3/7/98
    {
      if (currentMenu->menuitems[itemOn].routine &&
    currentMenu->menuitems[itemOn].status)
  {
    currentMenu->lastOn = itemOn;
    if (currentMenu->menuitems[itemOn].status == 2)
      {
        currentMenu->menuitems[itemOn].routine(1);   // right arrow
        S_StartSound(NULL,sfx_stnmov);
      }
    else
      {
        currentMenu->menuitems[itemOn].routine(itemOn);
        S_StartSound(NULL,sfx_pistol);
      }
  }
      //jff 3/24/98 remember last skill selected
      // killough 10/98 moved to skill-specific functions
      return TRUE;
    }

  if (ch == key_menu_escape)                           // phares 3/7/98
    {
      currentMenu->lastOn = itemOn;
      M_ClearMenus ();
      S_StartSound(NULL,sfx_swtchx);
      return TRUE;
    }

  if (ch == key_menu_backspace)                        // phares 3/7/98
    {
      currentMenu->lastOn = itemOn;

      // phares 3/30/98:
      // add checks to see if you're in the extended help screens
      // if so, stay with the same menu definition, but bump the
      // index back one. if the index bumps back far enough ( == 0)
      // then you can return to the Read_Thisn menu definitions

      if (currentMenu->prevMenu)
  {
    if (currentMenu == &ExtHelpDef)
      {
        if (--extended_help_index == 0)
    {
      currentMenu = currentMenu->prevMenu;
      extended_help_index = 1; // reset
    }
      }
    else
      currentMenu = currentMenu->prevMenu;
    itemOn = currentMenu->lastOn;
    S_StartSound(NULL,sfx_swtchn);
  }
      return TRUE;
    }

  else
    {
      for (i = itemOn+1;i < currentMenu->numitems;i++)
  if (currentMenu->menuitems[i].alphaKey == ch)
    {
      itemOn = i;
      S_StartSound(NULL,sfx_pstop);
      return TRUE;
    }
      for (i = 0;i <= itemOn;i++)
  if (currentMenu->menuitems[i].alphaKey == ch)
    {
      itemOn = i;
      S_StartSound(NULL,sfx_pstop);
      return TRUE;
    }
    }
  return FALSE;
}

//
// End of M_Responder
//
/////////////////////////////////////////////////////////////////////////////

/////////////////////////////////////////////////////////////////////////////
//
// General Routines
//
// This displays the Main menu and gets the menu screens rolling.
// Plus a variety of routines that control the Big Font menu display.
// Plus some initialization for game-dependant situations.

void M_StartControlPanel (void)
{
  // intro might call this repeatedly

  if (menuactive)
    return;

  //jff 3/24/98 make default skill menu choice follow -skill or defaultskill
  //from command line or config file
  //
  // killough 10/98:
  // Fix to make "always floating" with menu selections, and to always follow
  // defaultskill, instead of -skill.

  NewDef.lastOn = defaultskill - 1;

  default_verify = 0;                  // killough 10/98
  menuactive = mnact_float;
  currentMenu = &MainDef;         // JDC
  itemOn = currentMenu->lastOn;   // JDC
  print_warning_about_changes = FALSE;   // killough 11/98
}

//
// M_Drawer
// Called after the view has been rendered,
// but before it has been blitted.
//
// killough 9/29/98: Significantly reformatted source
//

void M_Drawer (void)
{
  inhelpscreens = FALSE;

  // Horiz. & Vertically center string and print it.
  // killough 9/29/98: simplified code, removed 40-character width limit
  if (messageToPrint)
    {
      /* cph - strdup string to writable memory */
      char *ms = strdup(messageString);
      char *p = ms;

      int y = 100 - M_StringHeight(messageString)/2;
      while (*p)
      {
        char *string = p, c;
        while ((c = *p) && *p != '\n')
          p++;
        *p = 0;
        M_WriteText(160 - M_StringWidth(string)/2, y, string, CR_DEFAULT);
        y += hu_font[0].height;
        if ((*p = c))
          p++;
      }
      free(ms);
    }
  else
    if (menuactive)
      {
  int x,y,max,i;
  int lumps_missing = 0;

  menuactive = mnact_float; // Boom-style menu drawers will set mnact_full

  if (currentMenu->routine)
    currentMenu->routine();     // call Draw routine

  // DRAW MENU

  x = currentMenu->x;
  y = currentMenu->y;
  max = currentMenu->numitems;

  for (i = 0; i < max; i++)
    if (currentMenu->menuitems[i].name[0])
      if (W_CheckNumForName(currentMenu->menuitems[i].name) < 0)
        lumps_missing++;

  if (lumps_missing == 0)
    for (i=0;i<max;i++)
    {
      if (currentMenu->menuitems[i].name[0])
        V_DrawNamePatch(x,y,0,currentMenu->menuitems[i].name,
            CR_DEFAULT, VPT_STRETCH);
      y += LINEHEIGHT;
    }
  else
    for (i = 0; i < max; i++)
    {
      const char *alttext = currentMenu->menuitems[i].alttext;
      if (alttext)
        M_WriteText(x, y+8-(M_StringHeight(alttext)/2), alttext, CR_DEFAULT);
      y += LINEHEIGHT;
    }

  // DRAW SKULL

  // CPhipps - patch drawing updated
  V_DrawNamePatch(x + SKULLXOFF, currentMenu->y - 5 + itemOn*LINEHEIGHT,0,
      skullName[whichSkull], CR_DEFAULT, VPT_STRETCH);
      }
}

//
// M_ClearMenus
//
// Called when leaving the menu screens for the real world

void M_ClearMenus (void)
{
  menuactive = mnact_inactive;
  print_warning_about_changes = 0;     // killough 8/15/98
  default_verify = 0;                  // killough 10/98

  // Have to call this here to ensure that any changes to the
  // gamma correction level are applied immediately...
  V_SetPalette(0);

  // if (!netgame && usergame && paused)
  //     sendpause = TRUE;
}

//
// M_SetupNextMenu
//
void M_SetupNextMenu(menu_t *menudef)
{
  currentMenu = menudef;
  itemOn = currentMenu->lastOn;
}

/////////////////////////////
//
// M_Ticker
//
void M_Ticker (void)
{
  if (gametic % 8 == 0)
    {
      whichSkull ^= 1;
    }
}

/////////////////////////////
//
// Message Routines
//

void M_StartMessage (const char* string,void* routine,dbool input)
{
  messageLastMenuActive = menuactive;
  messageToPrint = 1;
  messageString = string;
  messageRoutine = routine;
  messageNeedsInput = input;
  menuactive = mnact_float;
  return;
}

void M_StopMessage(void)
{
  menuactive = messageLastMenuActive;
  messageToPrint = 0;
}

/////////////////////////////
//
// Thermometer Routines
//

//
// M_DrawThermo draws the thermometer graphic for Mouse Sensitivity,
// Sound Volume, etc.
//
// proff/nicolas 09/20/98 -- changed for hi-res
// CPhipps - patch drawing updated
//
void M_DrawThermo(int x,int y,int thermWidth,int thermDot )
{
  int          xx;
  int           i;
  /*
   * Modification By Barry Mead to allow the Thermometer to have vastly
   * larger ranges. (the thermWidth parameter can now have a value as
   * large as 200.      Modified 1-9-2000  Originally I used it to make
   * the sensitivity range for the mouse better. It could however also
   * be used to improve the dynamic range of music and sound affect
   * volume controls for example.
   */
  int horizScaler; //Used to allow more thermo range for mouse sensitivity.
  thermWidth = (thermWidth > 200) ? 200 : thermWidth; //Clamp to 200 max
  horizScaler = (thermWidth > 23) ? (200 / thermWidth) : 8; //Dynamic range
  xx = x;
  V_DrawNamePatch(xx, y, 0, "M_THERML", CR_DEFAULT, VPT_STRETCH);
  xx += 8;
  for (i=0;i<thermWidth;i++)
    {
    V_DrawNamePatch(xx, y, 0, "M_THERMM", CR_DEFAULT, VPT_STRETCH);
    xx += horizScaler;
    }

  xx += (8 - horizScaler);  /* make the right end look even */

  V_DrawNamePatch(xx, y, 0, "M_THERMR", CR_DEFAULT, VPT_STRETCH);
  V_DrawNamePatch((x+8)+thermDot*horizScaler,y,0,"M_THERMO",CR_DEFAULT,VPT_STRETCH);
}

//
// Draw an empty cell in the thermometer
//

void M_DrawEmptyCell (menu_t* menu,int item)
{
  // CPhipps - patch drawing updated
  V_DrawNamePatch(menu->x - 10, menu->y+item*LINEHEIGHT - 1, 0,
      "M_CELL1", CR_DEFAULT, VPT_STRETCH);
}

//
// Draw a full cell in the thermometer
//

void M_DrawSelCell (menu_t* menu,int item)
{
  // CPhipps - patch drawing updated
  V_DrawNamePatch(menu->x - 10, menu->y+item*LINEHEIGHT - 1, 0,
      "M_CELL2", CR_DEFAULT, VPT_STRETCH);
}

/////////////////////////////
//
// String-drawing Routines
//

//
// Find string width from hu_font chars
//

int M_StringWidth(const char* string)
{
  int i, c, w = 0;
  for (i = 0;(size_t)i < strlen(string);i++)
    w += (c = toupper(string[i]) - HU_FONTSTART) < 0 || c >= HU_FONTSIZE ?
      4 : hu_font[c].width;
  return w;
}

//
//    Find string height from hu_font chars
//

int M_StringHeight(const char* string)
{
  int i, h, height = h = hu_font[0].height;
  for (i = 0;string[i];i++)            // killough 1/31/98
    if (string[i] == '\n')
      h += height;
  return h;
}

//
//    Write a string using the hu_font
//
void M_WriteText (int x,int y, const char* string, int cm)
{
  int   w;
  const char* ch;
  int   c;
  int   cx;
  int   cy;
  int   flags;

  ch = string;
  cx = x;
  cy = y;

  flags = VPT_STRETCH;
  if (cm != CR_DEFAULT)
    flags |= VPT_TRANS;

  while(1) {
    c = *ch++;
    if (!c)
      break;
    if (c == '\n') {
      cx = x;
      cy += 12;
      continue;
    }

    c = toupper(c) - HU_FONTSTART;
    if (c < 0 || c>= HU_FONTSIZE) {
      cx += 4;
      continue;
    }

    w = hu_font[c].width;
    if (cx+w > SCREENWIDTH)
      break;
    // proff/nicolas 09/20/98 -- changed for hi-res
    // CPhipps - patch drawing updated
    V_DrawNumPatch(cx, cy, 0, hu_font[c].lumpnum, cm, flags);
    cx+=w;
  }
}

void M_DrawTitle(int x, int y, const char *patch, int cm,
                 const char *alttext, int altcm)
{
  int lumpnum = W_CheckNumForName(patch);

  if (lumpnum >= 0)
  {
    int flags = VPT_STRETCH;
    if (cm != CR_DEFAULT)
      flags |= VPT_TRANS;
    V_DrawNumPatch(x, y, 0, lumpnum, cm, flags);
  }
  else
  {
    // patch doesn't exist, draw some text in place of it
    M_WriteText(160-(M_StringWidth(alttext)/2),
                y+8-(M_StringHeight(alttext)/2), // assumes patch height 16
                alttext, altcm);
  }
}

/////////////////////////////
//
// Initialization Routines to take care of one-time setup
//

// phares 4/08/98:
// M_InitHelpScreen() clears the weapons from the HELP
// screen that don't exist in this version of the game.

void M_InitHelpScreen(void)
{
  setup_menu_t* src;

  src = helpstrings;
  while (!(src->m_flags & S_END)) {

    if ((strncmp(src->m_text,"PLASMA",6) == 0) && (gamemode == shareware))
      src->m_flags = S_SKIP; // Don't show setting or item
    if ((strncmp(src->m_text,"BFG",3) == 0) && (gamemode == shareware))
      src->m_flags = S_SKIP; // Don't show setting or item
    if ((strncmp(src->m_text,"SSG",3) == 0) && (gamemode != commercial))
      src->m_flags = S_SKIP; // Don't show setting or item
    src++;
  }
}

//
// M_Init
//
void M_Init(void)
{
  M_InitDefaults();                // killough 11/98
  currentMenu = &MainDef;
  menuactive = mnact_inactive;
  itemOn = currentMenu->lastOn;
  whichSkull = 0;
  messageToPrint = 0;
  messageString = NULL;
  messageLastMenuActive = menuactive;
  quickSaveSlot = -1;

  // Here we could catch other version dependencies,
  //  like HELP1/2, and four episodes.

  switch(gamemode)
    {
    case commercial:
      // This is used because DOOM 2 had only one HELP
      //  page. I use CREDIT as second page now, but
      //  kept this hack for educational purposes.
      MainMenu[readthis] = MainMenu[quitdoom];
      MainDef.numitems--;
      MainDef.y += 8;
      NewDef.prevMenu = &MainDef;
      ReadDef1.routine = M_DrawReadThis1;
      ReadDef1.x = 330;
      ReadDef1.y = 165;
      ReadMenu1[0].routine = M_FinishReadThis;
      EpiDef.numitems = 0;
      break;
    case registered:
      // Episode 2 and 3 are handled,
      //  branching to an ad screen.

      // killough 2/21/98: Fix registered Doom help screen
      // killough 10/98: moved to second screen, moved up to the top
      ReadDef2.y = 15;

      // fall through
    case retail:
      // Check until which episode are there maps available
      for(EpiDef.numitems = 0; EpiDef.numitems < MAX_EPISODE_NUM; EpiDef.numitems++) {
        char mapname[9];
        sprintf(mapname, "E%uM1", EpiDef.numitems + 1);
        if (W_CheckNumForName(mapname) == -1) {
           break;
        }
      }
      break;
    case shareware:
      // Shareware version should only have 3 entries
      EpiDef.numitems = 3;
      break;
    default:
      break;
    }

  M_InitHelpScreen();   // init the help screen       // phares 4/08/98
  M_InitExtendedHelp(); // init extended help screens // phares 3/30/98

  M_ChangeDemoSmoothTurns();
  M_ChangeFramerate();
  M_ChangeMouseLook();
  M_ChangeMaxViewPitch();

  // Check if M_BUTT1 / M_BUTT2 exists, use fallback otherwise
  if ( W_CheckNumForName(ResetButtonName[0]) == -1 ||  W_CheckNumForName(ResetButtonName[1]) == -1 )
  {
    strcpy(ResetButtonName[0], "WARNB0");
    strcpy(ResetButtonName[1], "WARNA0");
  }
}

//
// End of General Routines
//
/////////////////////////////////////////////////////////////////////////////
