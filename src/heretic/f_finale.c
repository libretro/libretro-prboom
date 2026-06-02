/* Heretic end-of-episode finale.  Ported from the Raven/dsda-doom
 * implementation and adapted to this port's primitives.
 *
 * The fork's F_StartFinale/F_Ticker/F_Drawer ran Doom's finale for Heretic,
 * which set a Doom background flat (FLOOR4_8, absent in Heretic) and Doom end
 * text, and never drew Heretic's raw-bitmap art screens (FINAL1/FINAL2/etc).
 * These routines run only when the heretic flag is set; F_* dispatches here.
 *
 * The text screen tiles a Heretic flat and writes the episode text in the
 * Heretic small font (FONTA).  The art screen is a raw 320x200 bitmap drawn
 * with V_DrawRawScreen (not the patch path), per episode. */

#include <ctype.h>
#include <string.h>

#include "doomstat.h"
#include "d_event.h"
#include "g_game.h"
#include "w_wad.h"
#include "v_video.h"
#include "r_main.h"
#include "m_menu.h"
#include "s_sound.h"
#include "sounds.h"
#include "f_finale.h"

#define HERETIC_TEXTSPEED  3
#define HERETIC_TEXTWAIT   250

/* Episode end text (registered episodes E1-E3 plus the extended E4-E5). */
#define HERETIC_E1TEXT \
"with the destruction of the iron\n" \
"liches and their minions, the last\n" \
"of the undead are cleared from this\n" \
"plane of existence.\n\n" \
"those creatures had to come from\n" \
"somewhere, though, and you have the\n" \
"sneaky suspicion that the fiery\n" \
"portal of hell's maw opens onto\n" \
"their home dimension.\n\n" \
"to make sure that more undead\n" \
"(or even worse things) don't come\n" \
"through, you'll have to seal hell's\n" \
"maw from the other side. of course\n" \
"this means you may get stuck in a\n" \
"very unfriendly world, but no one\n" \
"ever said being a Heretic was easy!"

#define HERETIC_E2TEXT \
"the mighty maulotaurs have proved\n" \
"to be no match for you, and as\n" \
"their steaming corpses slide to the\n" \
"ground you feel a sense of grim\n" \
"satisfaction that they have been\n" \
"destroyed.\n\n" \
"the gateways which they guarded\n" \
"have opened, revealing what you\n" \
"hope is the way home. but as you\n" \
"step through, mocking laughter\n" \
"rings in your ears.\n\n" \
"was some other force controlling\n" \
"the maulotaurs? could there be even\n" \
"more horrific beings through this\n" \
"gate? the sweep of a crystal dome\n" \
"overhead where the sky should be is\n" \
"certainly not a good sign...."

#define HERETIC_E3TEXT \
"the death of d'sparil has loosed\n" \
"the magical bonds holding his\n" \
"creatures on this plane, their\n" \
"dying screams overwhelming his own\n" \
"cries of agony.\n\n" \
"your oath of vengeance fulfilled,\n" \
"you enter the portal to your own\n" \
"world, mere moments before the dome\n" \
"shatters into a million pieces.\n\n" \
"but if d'sparil's power is broken\n" \
"forever, why don't you feel safe?\n" \
"was it that last shout just before\n" \
"his death, the one that sounded\n" \
"like a curse? or a summoning? you\n" \
"can't really be sure, but it might\n" \
"just have been a scream.\n\n" \
"then again, what about the other\n" \
"serpent riders?"

static int         finalestage; /* 0 = text, 1 = art screen */
static int         finalecount;
static const char *finaletext;
static const char *finaleflat;
static int         FontABaseLump;

void Heretic_F_StartFinale(void)
{
  gameaction = ga_nothing;
  gamestate  = GS_FINALE;
  automapmode &= ~am_active;

  switch (gameepisode)
  {
    case 1:
      finaleflat = "FLOOR25";
      finaletext = HERETIC_E1TEXT;
      break;
    case 2:
      finaleflat = "FLATHUH1";
      finaletext = HERETIC_E2TEXT;
      break;
    case 3:
      finaleflat = "FLTWAWA2";
      finaletext = HERETIC_E3TEXT;
      break;
    case 4:
      finaleflat = "FLOOR28";
      finaletext = HERETIC_E1TEXT;
      break;
    case 5:
      finaleflat = "FLOOR08";
      finaletext = HERETIC_E1TEXT;
      break;
    default:
      finaleflat = "FLOOR25";
      finaletext = HERETIC_E1TEXT;
      break;
  }

  finalestage   = 0;
  finalecount   = 0;
  FontABaseLump = W_GetNumForName("FONTA_S") + 1;
  /* Heretic ending music (mus_cptd) is only defined in HEXEN builds; leave
   * the current music rather than reference a missing enum. */
}

dbool Heretic_F_Responder(event_t *event)
{
  (void)event;
  return FALSE;
}

void Heretic_F_Ticker(void)
{
  finalecount++;
  if (!finalestage
      && finalecount > (int)strlen(finaletext) * HERETIC_TEXTSPEED
                       + HERETIC_TEXTWAIT)
  {
    finalecount = 0;
    finalestage = 1;
  }
}

static void Heretic_F_TextWrite(void)
{
  int         count;
  const char *ch;
  int         c;
  int         cx, cy;
  int         lump;
  int         width;

  /* erase the entire screen to a tiled background */
  V_DrawBackground(finaleflat, 0);

  cx = 20;
  cy = 5;
  ch = finaletext;

  count = (finalecount - 10) / HERETIC_TEXTSPEED;
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
    c = toupper(c);
    if (c < 33)
    {
      cx += 5;
      continue;
    }
    lump  = FontABaseLump + c - 33;
    width = R_NumPatchWidth(lump);
    if (cx + width > SCREENWIDTH)
      break;
    V_DrawNumPatch(cx, cy, 0, lump, CR_DEFAULT, VPT_STRETCH);
    cx += width;
  }
}

/* E3 demon scroll: the fork has no sectioned raw blit, so show the first
 * page, then switch to the second once the scroll period elapses. */
static void Heretic_F_DemonScroll(void)
{
  if (finalecount < 140)
    V_DrawRawScreen("FINAL1");
  else
    V_DrawRawScreen("FINAL2");
}

void Heretic_F_Drawer(void)
{
  if (!finalestage)
  {
    Heretic_F_TextWrite();
    return;
  }

  switch (gameepisode)
  {
    case 1:
      V_DrawRawScreen("CREDIT");
      break;
    case 2:
      V_DrawRawScreen("E2END");
      break;
    case 3:
      Heretic_F_DemonScroll();
      break;
    case 4:
    case 5:
    default:
      V_DrawRawScreen("CREDIT");
      break;
  }
}
