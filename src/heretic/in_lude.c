/* Heretic intermission ("in_lude").  Ported from the Raven/dsda-doom
 * implementation and adapted to this port's primitives.  Heretic's
 * intermission is a self-contained state machine completely unlike Doom's
 * WI_* code, so when the Doom intermission ran for Heretic it never advanced
 * and the game locked up on level completion.  This drives the stats screen
 * and the "you are here" map, then calls G_WorldDone to continue.
 *
 * Inert for Doom: WI_* dispatches here only when the heretic flag is set. */

#include "doomstat.h"
#include "d_event.h"
#include "g_game.h"
#include "i_system.h"
#include "m_random.h"
#include "p_tick.h"
#include "r_defs.h"
#include "r_main.h"
#include "s_sound.h"
#include "sounds.h"
#include "v_video.h"
#include "w_wad.h"
#include "lprintf.h"
#include "am_map.h"
#include "wi_stuff.h"
#include "in_lude.h"

typedef enum
{
  IN_SINGLE,
  IN_COOPERATIVE,
  IN_DEATHMATCH
} in_gametype_t;

/* The fork has no Heretic status bar object to restart; the status bar is
 * rebuilt by the normal level-start path, so this is a no-op shim. */
static void SB_Start(void) {}

static int FontBLump;        /* lump of FONTB '!' (char 33) */
static int FontBNumbers[10]; /* FONTB16.. digit lumps */
static int FontALump;        /* lump of FONTA '!' (char 33) */

static wbstartstruct_t *wbs;
static int prevmap;
static int nextmap;
static dbool intermission;
static dbool skipintermission;
static dbool finalintermission;
static int interstate = 0;
static int intertime = -1;
static int oldintertime = 0;
static in_gametype_t gametype;
static int cnt;

static int hours, minutes, seconds;
static int totalHours, totalMinutes, totalSeconds;

/* "You are here" marker positions per episode (E1..E3), 9 maps each. */
static const struct { int x, y; } YAHspot[3][9] = {
  { {172,78},{86,90},{73,66},{159,95},{148,126},{132,54},{131,74},{208,138},{52,101} },
  { {218,57},{137,81},{155,124},{171,68},{250,86},{136,98},{203,90},{220,140},{279,106} },
  { {86,99},{124,103},{154,79},{202,83},{178,59},{142,58},{219,66},{247,57},{107,80} }
};

/* Heretic episode/level names (E1..E3, the registered set). */
static const char *const HereticLevelNames[] = {
  "THE DOCKS","THE DUNGEONS","THE GATEHOUSE","THE GUARD TOWER",
  "THE CITADEL","THE CATHEDRAL","THE CRYPTS","HELL'S MAW","THE GRAVEYARD",
  "THE CRATER","THE LAVA PITS","THE RIVER OF FIRE","THE ICE GROTTO",
  "THE CATACOMBS","THE LABYRINTH","THE GREAT HALL","THE PORTALS OF CHAOS","THE GLACIER",
  "THE STOREHOUSE","THE CESSPOOL","THE CONFLUENCE","THE AZURE FORTRESS",
  "THE OPHIDIAN LAIR","THE HALLS OF FEAR","THE CHASM","THE ORACLE","THE STRONGHOLD"
};

static const char *NameForMap(int map)
{
  int idx = (gameepisode - 1) * 9 + map - 1;
  if (gameepisode < 1 || gameepisode > 3 || map < 1 || map > 9)
    return "";
  return HereticLevelNames[idx];
}

/* --- local FONTA/FONTB text helpers (the fork lacks MN_DrText*) --- */

static int IN_TextBWidth(const char *text)
{
  char c;
  int width = 0;
  while ((c = *text++) != 0)
  {
    if (c < 33)
      width += 8;
    else
      width += R_NumPatchWidth(FontBLump + c - 33) - 1;
  }
  return width;
}

static int IN_TextAWidth(const char *text)
{
  char c;
  int width = 0;
  while ((c = *text++) != 0)
  {
    if (c < 33)
      width += 5;
    else
      width += R_NumPatchWidth(FontALump + c - 33) - 1;
  }
  return width;
}

static void IN_DrTextB(const char *text, int x, int y)
{
  char c;
  while ((c = *text++) != 0)
  {
    if (c < 33)
      x += 8;
    else
    {
      int lump = FontBLump + c - 33;
      V_DrawNumPatch(x, y, 0, lump, CR_DEFAULT, VPT_STRETCH);
      x += R_NumPatchWidth(lump) - 1;
    }
  }
}

static void IN_DrTextA(const char *text, int x, int y)
{
  char c;
  while ((c = *text++) != 0)
  {
    if (c < 33)
      x += 5;
    else
    {
      int lump = FontALump + c - 33;
      V_DrawNumPatch(x, y, 0, lump, CR_DEFAULT, VPT_STRETCH);
      x += R_NumPatchWidth(lump) - 1;
    }
  }
}

static void IN_DrawNumber(int val, int x, int y, int digits)
{
  int lump, xpos, oldval, realdigits;

  oldval = val;
  xpos = x;
  realdigits = 1;
  if (val < 0)
    val = 0;
  if (val > 9)  { realdigits++; if (digits < realdigits) { realdigits = digits; val = 9; } }
  if (val > 99) { realdigits++; if (digits < realdigits) { realdigits = digits; val = 99; } }
  if (val > 999){ realdigits++; if (digits < realdigits) { realdigits = digits; val = 999; } }

  if (digits == 4)
  {
    lump = FontBNumbers[val / 1000];
    V_DrawNumPatch(xpos + 6 - R_NumPatchWidth(lump) / 2 - 12, y, 0, lump, CR_DEFAULT, VPT_STRETCH);
  }
  if (digits > 2)
  {
    if (realdigits > 2)
    {
      lump = FontBNumbers[val / 100];
      V_DrawNumPatch(xpos + 6 - R_NumPatchWidth(lump) / 2, y, 0, lump, CR_DEFAULT, VPT_STRETCH);
    }
    xpos += 12;
  }
  val = val % 100;
  if (digits > 1)
  {
    if (val > 9)
    {
      lump = FontBNumbers[val / 10];
      V_DrawNumPatch(xpos + 6 - R_NumPatchWidth(lump) / 2, y, 0, lump, CR_DEFAULT, VPT_STRETCH);
    }
    else if (digits == 2 || oldval > 99)
      V_DrawNumPatch(xpos, y, 0, FontBNumbers[0], CR_DEFAULT, VPT_STRETCH);
    xpos += 12;
  }
  val = val % 10;
  lump = FontBNumbers[val];
  V_DrawNumPatch(xpos + 6 - R_NumPatchWidth(lump) / 2, y, 0, lump, CR_DEFAULT, VPT_STRETCH);
}

static void IN_DrawTime(int x, int y, int h, int m, int s)
{
  if (h)
  {
    IN_DrawNumber(h, x, y, 2);
    IN_DrTextB(":", x + 26, y);
  }
  IN_DrawNumber(m, x + 34, y, 2);
  IN_DrTextB(":", x + 60, y);
  IN_DrawNumber(s, x + 68, y, 2);
}

/* --- background / map --- */

static void IN_DrawInterpic(void)
{
  char name[9];
  if (gameepisode < 1 || gameepisode > 3)
    return;
  sprintf(name, "MAPE%d", gameepisode);
  V_DrawNamePatch(0, 0, 0, name, CR_DEFAULT, VPT_STRETCH);
}

static void IN_DrawStatBack(void)
{
  V_DrawBackground("FLOOR16", 0);
}

static void IN_DrawBeenThere(int i)
{
  V_DrawNamePatch(YAHspot[gameepisode - 1][i].x, YAHspot[gameepisode - 1][i].y,
                  0, "IN_X", CR_DEFAULT, VPT_STRETCH);
}

static void IN_DrawGoingThere(int i)
{
  V_DrawNamePatch(YAHspot[gameepisode - 1][i].x, YAHspot[gameepisode - 1][i].y,
                  0, "IN_YAH", CR_DEFAULT, VPT_STRETCH);
}

/* --- init --- */

static void IN_InitLumps(void)
{
  int i, base;
  base = W_GetNumForName("FONTB16");
  for (i = 0; i < 10; i++)
    FontBNumbers[i] = base + i;
  FontBLump = W_GetNumForName("FONTB_S") + 1;
  FontALump = W_GetNumForName("FONTA_S") + 1;
}

static void IN_InitVariables(wbstartstruct_t *wbstartstruct)
{
  wbs = wbstartstruct;
  prevmap = wbs->last + 1;
  nextmap = wbs->next + 1;
  finalintermission = (prevmap == 8);
}

static void IN_InitStats(void)
{
  int count;

  gametype = netgame ? (deathmatch ? IN_DEATHMATCH : IN_COOPERATIVE) : IN_SINGLE;

  count = leveltime / 35;
  hours = count / 3600; count -= hours * 3600;
  minutes = count / 60;  count -= minutes * 60;
  seconds = count;

  count = wbs->totaltimes / 35;
  totalHours = count / 3600; count -= totalHours * 3600;
  totalMinutes = count / 60;  count -= totalMinutes * 60;
  totalSeconds = count;
}

static void IN_Stop(void)
{
  intermission = false;
  SB_Start();
}

static void IN_WaitStop(void)
{
  if (!--cnt)
  {
    IN_Stop();
    G_WorldDone();
  }
}

static void IN_CheckForSkip(void)
{
  int i;
  player_t *player;

  for (i = 0, player = players; i < MAXPLAYERS; i++, player++)
  {
    if (playeringame[i])
    {
      if (player->cmd.buttons & BT_ATTACK)
      {
        if (!(player->cmd.buttons & BT_SPECIAL))
          skipintermission = 1;
      }
      if (player->cmd.buttons & BT_USE)
      {
        if (!(player->cmd.buttons & BT_SPECIAL))
          skipintermission = 1;
      }
    }
  }
}

/* --- public entry points (called from WI_* when heretic) --- */

void IN_Start(wbstartstruct_t *wbstartstruct)
{
  V_SetPalette(0);
  IN_InitVariables(wbstartstruct);
  IN_InitLumps();
  IN_InitStats();
  intermission = true;
  interstate = -1;
  skipintermission = false;
  intertime = 0;
  oldintertime = 0;
  AM_Stop();
  /* Heretic intermission music (mus_intr) is only defined in HEXEN builds;
   * leave the current music playing rather than reference a missing enum. */
}

void IN_Ticker(void)
{
  if (!intermission)
    return;
  if (interstate == 3)
  {
    IN_WaitStop();
    return;
  }
  IN_CheckForSkip();
  intertime++;
  if (oldintertime < intertime)
  {
    interstate++;
    if (interstate >= 1 && finalintermission)
    {
      IN_Stop();
      G_WorldDone();
      return;
    }
    switch (interstate)
    {
      case 0:  oldintertime = intertime + 300; break;
      case 1:  oldintertime = intertime + 200; break;
      case 2:  oldintertime = INT_MAX;         break;
      case 3:  cnt = 10;                       break;
      default: break;
    }
  }
  if (skipintermission)
  {
    if (interstate == 0 && intertime < 150)
    {
      intertime = 150;
      skipintermission = false;
      return;
    }
    else if (finalintermission)
    {
      IN_Stop();
      G_WorldDone();
      return;
    }
    else if (interstate < 2)
    {
      interstate = 2;
      skipintermission = false;
      S_StartSound(NULL, heretic_sfx_dorcls);
      return;
    }
    interstate = 3;
    cnt = 10;
    skipintermission = false;
    S_StartSound(NULL, heretic_sfx_dorcls);
  }
}

static void IN_DrawSingleStats(void)
{
  static int sounds;
  const char *prev_level_name = NameForMap(prevmap);
  int x;

  IN_DrTextB("KILLS",   50, 65);
  IN_DrTextB("ITEMS",   50, 90);
  IN_DrTextB("SECRETS", 50, 115);

  x = 160 - IN_TextBWidth(prev_level_name) / 2;
  IN_DrTextB(prev_level_name, x, 3);
  x = 160 - IN_TextAWidth("FINISHED") / 2;
  IN_DrTextA("FINISHED", x, 25);

  if (intertime < 30) { sounds = 0; return; }
  if (sounds < 1 && intertime >= 30)
    { S_StartSound(NULL, heretic_sfx_dorcls); sounds++; }
  IN_DrawNumber(players[consoleplayer].killcount, 200, 65, 3);
  V_DrawNamePatch(237, 65, 0, "FONTB15", CR_DEFAULT, VPT_STRETCH);
  IN_DrawNumber(totalkills, 248, 65, 3);
  if (intertime < 60) return;
  if (sounds < 2 && intertime >= 60)
    { S_StartSound(NULL, heretic_sfx_dorcls); sounds++; }
  IN_DrawNumber(players[consoleplayer].itemcount, 200, 90, 3);
  V_DrawNamePatch(237, 90, 0, "FONTB15", CR_DEFAULT, VPT_STRETCH);
  IN_DrawNumber(totalitems, 248, 90, 3);
  if (intertime < 90) return;
  if (sounds < 3 && intertime >= 90)
    { S_StartSound(NULL, heretic_sfx_dorcls); sounds++; }
  IN_DrawNumber(players[consoleplayer].secretcount, 200, 115, 3);
  V_DrawNamePatch(237, 115, 0, "FONTB15", CR_DEFAULT, VPT_STRETCH);
  IN_DrawNumber(totalsecret, 248, 115, 3);
  if (intertime < 150) return;
  if (sounds < 4 && intertime >= 150)
    { S_StartSound(NULL, heretic_sfx_dorcls); sounds++; }

  IN_DrTextB("TIME", 85, 150);
  IN_DrawTime(155, 150, hours, minutes, seconds);
  IN_DrTextB("TOTAL", 85, 170);
  IN_DrawTime(155, 170, totalHours, totalMinutes, totalSeconds);
}

static void IN_DrawOldLevel(void)
{
  const char *level_name = NameForMap(prevmap);
  int i, x;

  x = 160 - IN_TextBWidth(level_name) / 2;
  IN_DrTextB(level_name, x, 3);
  x = 160 - IN_TextAWidth("FINISHED") / 2;
  IN_DrTextA("FINISHED", x, 25);

  for (i = 0; i < prevmap - 1; i++)
    IN_DrawBeenThere(i);
  if (players[consoleplayer].didsecret)
    IN_DrawBeenThere(8);
  if (!(intertime & 16))
    IN_DrawBeenThere(prevmap - 1);
}

static void IN_DrawYAH(void)
{
  const char *level_name = NameForMap(nextmap);
  int i, x;

  x = 160 - IN_TextAWidth("NOW ENTERING:") / 2;
  IN_DrTextA("NOW ENTERING:", x, 10);
  x = 160 - IN_TextBWidth(level_name) / 2;
  IN_DrTextB(level_name, x, 20);

  if (prevmap == 9)
    prevmap = nextmap - 1;
  for (i = 0; i < prevmap; i++)
    IN_DrawBeenThere(i);
  if (players[consoleplayer].didsecret)
    IN_DrawBeenThere(8);
  if (!(intertime & 16) || interstate == 3)
    IN_DrawGoingThere(nextmap - 1);
}

void IN_Drawer(void)
{
  static int oldinterstate;

  if (!intermission)
    return;
  if (interstate == 3)
    return;

  if (oldinterstate != 2 && interstate == 2)
    S_StartSound(NULL, heretic_sfx_pstop);
  oldinterstate = interstate;

  switch (interstate)
  {
    case -1:
    case 0:
      IN_DrawStatBack();
      IN_DrawSingleStats();
      break;
    case 1:
      if (gameepisode < 4)
      {
        IN_DrawInterpic();
        IN_DrawOldLevel();
      }
      break;
    case 2:
      if (gameepisode < 4)
      {
        IN_DrawInterpic();
        IN_DrawYAH();
      }
      break;
    case 3:
      if (gameepisode < 4)
        IN_DrawInterpic();
      break;
    default:
      break;
  }
}
