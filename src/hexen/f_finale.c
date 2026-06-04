/* Hexen game ending: the three WINxMSG text screens over the FINALE pics,
 * with the class-specific chess scene.  Ported from the vanilla/dsda-doom
 * Hexen finale; the original's 70-tic palette fades between pics are not
 * reproduced (the pics cut directly), matching dsda-doom's behaviour. */

#include <ctype.h>
#include <string.h>

#include "doomstat.h"
#include "w_wad.h"
#include "v_video.h"
#include "s_sound.h"
#include "sounds.h"
#include "r_patch.h"
#include "lprintf.h"

#include "g_game.h"

#include "hexen/f_finale.h"

#define HEXEN_TEXTSPEED 3
#define HEXEN_TEXTWAIT  250

#define MAX_FINALE_MESSAGE_SIZE 1024

static int HexFinaleStage;
static int HexFinaleCount;
static int HexFinaleEndCount;
static const char *HexFinaleLumpName;
static int FontABaseLump;
static char *HexFinaleText;
static char FinaleMessage[MAX_FINALE_MESSAGE_SIZE];

static char *GetFinaleText(int sequence)
{
  static const char *winMsgLumpNames[] =
  {
    "win1msg",
    "win2msg",
    "win3msg"
  };
  const char *msgLumpName;
  int msgSize;
  int msgLump;

  msgLumpName = winMsgLumpNames[sequence];
  msgLump = W_GetNumForName(msgLumpName);
  msgSize = W_LumpLength(msgLump);
  if (msgSize >= MAX_FINALE_MESSAGE_SIZE)
    I_Error("Finale message too long (%s)", msgLumpName);

  memcpy(FinaleMessage, W_CacheLumpNum(msgLump), msgSize);
  FinaleMessage[msgSize] = '\0';
  W_UnlockLumpNum(msgLump);
  return FinaleMessage;
}

void Hexen_F_StartFinale(void)
{
  gameaction = ga_nothing;
  gamestate = GS_FINALE;
  automapmode &= ~am_active;

  HexFinaleStage = 0;
  HexFinaleCount = 0;
  HexFinaleText = GetFinaleText(0);
  HexFinaleEndCount = 70;
  HexFinaleLumpName = "FINALE1";
  FontABaseLump = W_GetNumForName("FONTA_S") + 1;

  S_ChangeMusicByName("hall", FALSE);   /* don't loop the song */
}

dbool Hexen_F_Responder(event_t *event)
{
  (void) event;
  return FALSE;
}

void Hexen_F_Ticker(void)
{
  HexFinaleCount++;
  if (HexFinaleStage < 5 && HexFinaleCount >= HexFinaleEndCount)
  {
    HexFinaleCount = 0;
    HexFinaleStage++;
    switch (HexFinaleStage)
    {
      case 1:                   /* text 1 */
        HexFinaleEndCount = strlen(HexFinaleText) * HEXEN_TEXTSPEED + HEXEN_TEXTWAIT;
        break;
      case 2:                   /* pic 2, text 2 */
        HexFinaleText = GetFinaleText(1);
        HexFinaleEndCount = strlen(HexFinaleText) * HEXEN_TEXTSPEED + HEXEN_TEXTWAIT;
        HexFinaleLumpName = "FINALE2";
        S_ChangeMusicByName("orb", FALSE);
        break;
      case 3:                   /* pic 2 -- fade out */
        HexFinaleEndCount = 70;
        break;
      case 4:                   /* pic 3 -- fade in */
        HexFinaleLumpName = "FINALE3";
        HexFinaleEndCount = 71;
        S_ChangeMusicByName("chess", TRUE);
        break;
      case 5:                   /* pic 3, text 3 */
        HexFinaleText = GetFinaleText(2);
        break;
      default:
        break;
    }
    return;
  }
}

/* The chess scene's class overlay: all three heroes in netgames, the
 * Cleric or Mage patch in single player (the base pic shows the Fighter). */
static void DrawClassOverlay(void)
{
  if (netgame)
    V_DrawNamePatch(20, 0, 0, "chessall", CR_DEFAULT, VPT_STRETCH);
  else if (PlayerClass[consoleplayer] > 1)
    V_DrawNumPatch(60, 0, 0,
                   W_GetNumForName("chessc") + PlayerClass[consoleplayer] - 2,
                   CR_DEFAULT, VPT_STRETCH);
}

static void TextWrite(void)
{
  int count;
  char *ch;
  int c;
  int cx, cy;
  int lump;
  int width;

  V_DrawRawScreen(HexFinaleLumpName);
  if (HexFinaleStage == 5)
    DrawClassOverlay();

  /* draw the actual text */
  cy = (HexFinaleStage == 5) ? 135 : 5;
  cx = 20;
  ch = HexFinaleText;
  count = (HexFinaleCount - 10) / HEXEN_TEXTSPEED;
  if (count < 0)
    count = 0;
  for (; count; count--)
  {
    c = *ch++;
    if (!c)
      break;
    if (c == '\n')
    {
      cx = 20;
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

static void DrawPic(void)
{
  V_DrawRawScreen(HexFinaleLumpName);
  if (HexFinaleStage == 4 || HexFinaleStage == 5)
    DrawClassOverlay();
}

void Hexen_F_Drawer(void)
{
  switch (HexFinaleStage)
  {
    case 0:                     /* initial finale screen */
    case 3:                     /* between pics */
    case 4:                     /* chess screen */
      DrawPic();
      break;
    case 1:
    case 2:
    case 5:
      TextWrite();
      break;
    default:
      break;
  }
}
