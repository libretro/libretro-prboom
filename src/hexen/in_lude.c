/* Hexen intermission: the cluster-transition text screens (CLUSxMSG typed in
 * FONTA over INTERPIC with the hub theme) and the deathmatch frag tally.
 * Ported from the vanilla/dsda-doom interlude; same-cluster hub travel never
 * comes through here (G_DoCompleted loads the next map directly).  Like the
 * fork's heretic interlude, the menu text helpers are local since the fork
 * has no MN_DrTextA family. */

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "doomstat.h"
#include "d_event.h"
#include "s_sound.h"
#include "sounds.h"
#include "v_video.h"
#include "w_wad.h"
#include "g_game.h"
#include "r_patch.h"
#include "lprintf.h"

#include "hexen/p_mapinfo.h"
#include "hexen/sn_sonix.h"
#include "hexen/in_lude.h"

#define HEXEN_IN_TEXTSPEED 3
#define HEXEN_IN_TEXTWAIT  140

#define MAX_INTRMSN_MESSAGE_SIZE 1024

typedef enum
{
  SINGLE,
  COOPERATIVE,
  DEATHMATCH
} gametype_t;

static dbool intermission;
static dbool skipintermission;
static int interstate;
static int intertime = -1;
static gametype_t gametype;
static int cnt;
static int slaughterboy;        /* in DM, the player(s) with the most kills */
static int FontABaseLump;
static int FontAYBaseLump;

static signed int totalFrags[MAXPLAYERS];

static int HubCount;
static char *HubText;
static char ClusterMessage[MAX_INTRMSN_MESSAGE_SIZE];

/* --- local FONTA text helpers (the fork lacks MN_DrText*) --- */

static int IN_TextAWidth(const char *text)
{
  int width = 0;
  int c;

  while ((c = *text++))
  {
    c = toupper(c);
    if (c < 33)
      width += 5;
    else
      width += R_NumPatchWidth(FontABaseLump + c - 33) - 1;
  }
  return width;
}

static void IN_DrTextABase(const char *text, int x, int y, int base)
{
  int c;

  while ((c = *text++))
  {
    c = toupper(c);
    if (c < 33)
    {
      x += 5;
      continue;
    }
    V_DrawNumPatch(x, y, 0, base + c - 33, CR_DEFAULT, VPT_STRETCH);
    x += R_NumPatchWidth(base + c - 33) - 1;
  }
}

static void IN_DrTextA(const char *text, int x, int y)
{
  IN_DrTextABase(text, x, y, FontABaseLump);
}

static void IN_DrTextAYellow(const char *text, int x, int y)
{
  IN_DrTextABase(text, x, y, FontAYBaseLump);
}

/* --- intermission proper --- */

static void Stop(void)
{
  intermission = FALSE;
}

static void WaitStop(void)
{
  if (!--cnt)
  {
    Stop();
    G_WorldDone();
  }
}

static const char *ClusMsgLumpNames[] =
{
  "clus1msg",
  "clus2msg",
  "clus3msg",
  "clus4msg",
  "clus5msg"
};

static void InitStats(void)
{
  int i;
  int j;
  int oldCluster;
  signed int slaughterfrags;
  int slaughtercount;
  int playercount;

  if (!deathmatch)
  {
    gametype = SINGLE;
    HubCount = 0;
    oldCluster = P_GetMapCluster(gamemap);
    if (oldCluster != P_GetMapCluster(P_TranslateMapWarp(G_GetLeaveMap())))
    {
      if (oldCluster >= 1 && oldCluster <= 5)
      {
        /* Guard with a presence check rather than erroring out: expansion
         * wads can route clusters whose message lump is absent. */
        int msgLump = W_CheckNumForName(ClusMsgLumpNames[oldCluster - 1]);

        if (msgLump >= 0)
        {
          int msgSize = W_LumpLength(msgLump);

          if (msgSize >= MAX_INTRMSN_MESSAGE_SIZE)
            I_Error("Cluster message too long (%s)",
                    ClusMsgLumpNames[oldCluster - 1]);
          memcpy(ClusterMessage, W_CacheLumpNum(msgLump), msgSize);
          W_UnlockLumpNum(msgLump);
          ClusterMessage[msgSize] = '\0';
          HubText = ClusterMessage;
          HubCount = strlen(HubText) * HEXEN_IN_TEXTSPEED + HEXEN_IN_TEXTWAIT;
          S_ChangeMusicByName("hub", TRUE);
        }
      }
    }
  }
  else
  {
    gametype = DEATHMATCH;
    slaughterboy = 0;
    slaughterfrags = -9999;
    playercount = 0;
    slaughtercount = 0;
    for (i = 0; i < MAXPLAYERS; i++)
    {
      totalFrags[i] = 0;
      if (playeringame[i])
      {
        playercount++;
        for (j = 0; j < MAXPLAYERS; j++)
        {
          if (playeringame[j])
            totalFrags[i] += players[i].frags[j];
        }
      }
      if (totalFrags[i] > slaughterfrags)
      {
        slaughterboy = 1 << i;
        slaughterfrags = totalFrags[i];
        slaughtercount = 1;
      }
      else if (totalFrags[i] == slaughterfrags)
      {
        slaughterboy |= 1 << i;
        slaughtercount++;
      }
    }
    if (playercount == slaughtercount)
      slaughterboy = 0;         /* everyone is equal: no flashing winner */
    S_ChangeMusicByName("hub", TRUE);
  }
}

static void LoadPics(void)
{
  if (HubCount || gametype == DEATHMATCH)
  {
    FontABaseLump  = W_GetNumForName("FONTA_S") + 1;
    FontAYBaseLump = W_GetNumForName("FONTAY_S") + 1;
  }
}

void Hexen_IN_Start(wbstartstruct_t *wbstartstruct)
{
  (void) wbstartstruct;

  V_SetPalette(0);
  InitStats();
  LoadPics();
  intermission = TRUE;
  interstate = 0;
  skipintermission = FALSE;
  intertime = 0;
  automapmode &= ~am_active;
  SN_StopAllSequences();
}

static void CheckForSkip(void)
{
  int i;
  player_t *player;
  static dbool triedToSkip;

  for (i = 0, player = players; i < MAXPLAYERS; i++, player++)
  {
    if (playeringame[i])
    {
      if (player->cmd.buttons & BT_ATTACK)
      {
        if (!player->attackdown)
          skipintermission = 1;
        player->attackdown = true;
      }
      else
        player->attackdown = false;
      if (player->cmd.buttons & BT_USE)
      {
        if (!player->usedown)
          skipintermission = 1;
        player->usedown = true;
      }
      else
        player->usedown = false;
    }
  }
  if (deathmatch && intertime < 140)
  {                             /* wait 4 seconds before allowing a skip */
    if (skipintermission == 1)
    {
      triedToSkip = TRUE;
      skipintermission = 0;
    }
  }
  else
  {
    if (triedToSkip)
    {
      skipintermission = 1;
      triedToSkip = FALSE;
    }
  }
}

void Hexen_IN_Ticker(void)
{
  if (!intermission)
    return;
  if (interstate)
  {
    WaitStop();
    return;
  }
  skipintermission = FALSE;
  CheckForSkip();
  intertime++;
  if (skipintermission || (gametype == SINGLE && !HubCount))
  {
    interstate = 1;
    cnt = 10;
    skipintermission = FALSE;
  }
}

/* --- drawers --- */

#define TALLY_EFFECT_TICKS 20
#define TALLY_FINAL_X_DELTA (23*FRACUNIT)
#define TALLY_FINAL_Y_DELTA (13*FRACUNIT)
#define TALLY_START_XPOS (178*FRACUNIT)
#define TALLY_STOP_XPOS (90*FRACUNIT)
#define TALLY_START_YPOS (132*FRACUNIT)
#define TALLY_STOP_YPOS (83*FRACUNIT)
#define TALLY_TOP_X 85
#define TALLY_TOP_Y 9
#define TALLY_LEFT_X 7
#define TALLY_LEFT_Y 71
#define TALLY_TOTALS_X 291

static void DrNumberBase(int val, int x, int y, int wrapThresh, dbool bold)
{
  char buff[8] = "XX";

  if (!(val < -9 && wrapThresh < 1000))
    snprintf(buff, sizeof(buff), "%d", val >= wrapThresh ? val % wrapThresh : val);
  if (bold)
    IN_DrTextAYellow(buff, x - IN_TextAWidth(buff) / 2, y);
  else
    IN_DrTextA(buff, x - IN_TextAWidth(buff) / 2, y);
}

static void DrDeathTally(void)
{
  int i, j;
  fixed_t xPos, yPos;
  fixed_t xDelta, yDelta;
  fixed_t xStart, scale;
  int x, y;
  dbool bold;
  static dbool showTotals;
  int temp;

  V_DrawNamePatch(TALLY_TOP_X, TALLY_TOP_Y, 0, "tallytop", CR_DEFAULT, VPT_STRETCH);
  V_DrawNamePatch(TALLY_LEFT_X, TALLY_LEFT_Y, 0, "tallylft", CR_DEFAULT, VPT_STRETCH);
  if (intertime < TALLY_EFFECT_TICKS)
  {
    showTotals = FALSE;
    scale = (intertime * FRACUNIT) / TALLY_EFFECT_TICKS;
    xDelta = FixedMul(scale, TALLY_FINAL_X_DELTA);
    yDelta = FixedMul(scale, TALLY_FINAL_Y_DELTA);
    xStart = TALLY_START_XPOS - FixedMul(scale,
                                         TALLY_START_XPOS - TALLY_STOP_XPOS);
    yPos = TALLY_START_YPOS - FixedMul(scale,
                                       TALLY_START_YPOS - TALLY_STOP_YPOS);
  }
  else
  {
    xDelta = TALLY_FINAL_X_DELTA;
    yDelta = TALLY_FINAL_Y_DELTA;
    xStart = TALLY_STOP_XPOS;
    yPos = TALLY_STOP_YPOS;
  }
  if (intertime >= TALLY_EFFECT_TICKS && showTotals == FALSE)
  {
    showTotals = TRUE;
    S_StartSound(NULL, hexen_sfx_platform_stop);
  }
  y = yPos >> FRACBITS;
  for (i = 0; i < MAXPLAYERS; i++)
  {
    xPos = xStart;
    for (j = 0; j < MAXPLAYERS; j++, xPos += xDelta)
    {
      x = xPos >> FRACBITS;
      bold = (i == consoleplayer || j == consoleplayer);
      if (playeringame[i] && playeringame[j])
        DrNumberBase(players[i].frags[j], x, y, 100, bold);
      else
      {
        temp = IN_TextAWidth("--") / 2;
        if (bold)
          IN_DrTextAYellow("--", x - temp, y);
        else
          IN_DrTextA("--", x - temp, y);
      }
    }
    if (showTotals && playeringame[i]
        && !((slaughterboy & (1 << i)) && !(intertime & 16)))
      DrNumberBase(totalFrags[i], TALLY_TOTALS_X, y, 1000, FALSE);
    yPos += yDelta;
    y = yPos >> FRACBITS;
  }
}

static void DrawHubText(void)
{
  int count;
  char *ch;
  int c;
  int cx, cy;
  int lump;
  int width;

  cy = 5;
  cx = 10;
  ch = HubText;
  count = (intertime - 10) / HEXEN_IN_TEXTSPEED;
  if (count < 0)
    count = 0;
  for (; count; count--)
  {
    c = *ch++;
    if (!c)
      break;
    if (c == '\n')
    {
      cx = 10;
      cy += 9;
      continue;
    }
    if (c < 32)
      continue;
    c = toupper(c);
    if (c == 32)
    {
      cx += 5;
      continue;
    }
    lump = FontABaseLump + c - 33;
    width = R_NumPatchWidth(lump);
    if (cx + width > SCREENWIDTH)
      break;
    V_DrawNumPatch(cx, cy, 0, lump, CR_DEFAULT, VPT_STRETCH);
    cx += width;
  }
}

void Hexen_IN_Drawer(void)
{
  if (!intermission)
    return;
  if (interstate)
    return;

  V_DrawRawScreen("INTERPIC");

  if (gametype == SINGLE)
  {
    if (HubCount)
      DrawHubText();
  }
  else
    DrDeathTally();
}
