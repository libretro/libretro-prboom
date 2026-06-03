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
 *      Status bar code.
 *      Does the face/direction indicator animatin.
 *      Does palette indicators as well (red pain/berserk, bright pickup)
 *
 *-----------------------------------------------------------------------------*/

#include "doomdef.h"
#include "doomstat.h"
#include "m_random.h"
#include "i_video.h"
#include "w_wad.h"
#include "st_stuff.h"
#include "st_lib.h"
#include "r_main.h"
#include "am_map.h"
#include "m_cheat.h"
#include "s_sound.h"
#include "sounds.h"
#include "dstrings.h"
#include "r_draw.h"

//
// STATUS BAR DATA
//

// Palette indices.
// For damage/bonus red-/gold-shifts
#define STARTREDPALS            1
#define STARTBONUSPALS          9
#define NUMREDPALS              8
#define NUMBONUSPALS            4
// Radiation suit, green shift.
#define RADIATIONPAL            13

// Location of status bar
#define ST_X                    0
#define ST_X2                   104

// proff 08/18/98: Changed for high-res
#define ST_FX (ST_X+143)
#define ST_FY (ST_Y+1)
//#define ST_FX 143
//#define ST_FY 169

// Should be set to patch width
//  for tall numbers later on
#define ST_TALLNUMWIDTH         (tallnum[0]->width)

// Number of status faces.
#define ST_NUMPAINFACES         5
#define ST_NUMSTRAIGHTFACES     3
#define ST_NUMTURNFACES         2
#define ST_NUMSPECIALFACES      3

#define ST_FACESTRIDE \
          (ST_NUMSTRAIGHTFACES+ST_NUMTURNFACES+ST_NUMSPECIALFACES)

#define ST_NUMEXTRAFACES        2

#define ST_NUMFACES \
          (ST_FACESTRIDE*ST_NUMPAINFACES+ST_NUMEXTRAFACES)

#define ST_TURNOFFSET           (ST_NUMSTRAIGHTFACES)
#define ST_OUCHOFFSET           (ST_TURNOFFSET + ST_NUMTURNFACES)
#define ST_EVILGRINOFFSET       (ST_OUCHOFFSET + 1)
#define ST_RAMPAGEOFFSET        (ST_EVILGRINOFFSET + 1)
#define ST_GODFACE              (ST_NUMPAINFACES*ST_FACESTRIDE)
#define ST_DEADFACE             (ST_GODFACE+1)

// proff 08/18/98: Changed for high-res
#define ST_FACESX (ST_X+143)
#define ST_FACESY (ST_Y)
//#define ST_FACESX 143
//#define ST_FACESY 168

#define ST_EVILGRINCOUNT        (2*TICRATE)
#define ST_STRAIGHTFACECOUNT    (TICRATE/2)
#define ST_TURNCOUNT            (1*TICRATE)
#define ST_OUCHCOUNT            (1*TICRATE)
#define ST_RAMPAGEDELAY         (2*TICRATE)

#define ST_MUCHPAIN             20

// Location and size of statistics,
//  justified according to widget type.
// Problem is, within which space? STbar? Screen?
// Note: this could be read in by a lump.
//       Problem is, is the stuff rendered
//       into a buffer,
//       or into the frame buffer?
// I dunno, why don't you go and find out!!!  killough

// AMMO number pos.
#define ST_AMMOWIDTH            3
// proff 08/18/98: Changed for high-res
#define ST_AMMOX (ST_X+44)
#define ST_AMMOY (ST_Y+3)
//#define ST_AMMOX 44
//#define ST_AMMOY 171

// HEALTH number pos.
#define ST_HEALTHWIDTH          3
// proff 08/18/98: Changed for high-res
#define ST_HEALTHX (ST_X+90)
#define ST_HEALTHY (ST_Y+3)
//#define ST_HEALTHX 90
//#define ST_HEALTHY 171

// Weapon pos.
// proff 08/18/98: Changed for high-res
#define ST_ARMSX (ST_X+111)
#define ST_ARMSY (ST_Y+4)
#define ST_ARMSBGX (ST_X+104)
#define ST_ARMSBGY (ST_Y)
//#define ST_ARMSX 111
//#define ST_ARMSY 172
//#define ST_ARMSBGX 104
//#define ST_ARMSBGY 168
#define ST_ARMSXSPACE 12
#define ST_ARMSYSPACE 10

// Frags pos.
// proff 08/18/98: Changed for high-res
#define ST_FRAGSX (ST_X+138)
#define ST_FRAGSY (ST_Y+3)
//#define ST_FRAGSX 138
//#define ST_FRAGSY 171
#define ST_FRAGSWIDTH 2

// ARMOR number pos.
#define ST_ARMORWIDTH 3
// proff 08/18/98: Changed for high-res
#define ST_ARMORX (ST_X+221)
#define ST_ARMORY (ST_Y+3)
//#define ST_ARMORX 221
//#define ST_ARMORY 171

// Key icon positions.
#define ST_KEY0WIDTH 8
#define ST_KEY0HEIGHT 5
// proff 08/18/98: Changed for high-res
#define ST_KEY0X (ST_X+239)
#define ST_KEY0Y (ST_Y+3)
//#define ST_KEY0X 239
//#define ST_KEY0Y 171
#define ST_KEY1WIDTH ST_KEY0WIDTH
// proff 08/18/98: Changed for high-res
#define ST_KEY1X (ST_X+239)
#define ST_KEY1Y (ST_Y+13)
//#define ST_KEY1X 239
//#define ST_KEY1Y 181
#define ST_KEY2WIDTH ST_KEY0WIDTH
// proff 08/18/98: Changed for high-res
#define ST_KEY2X (ST_X+239)
#define ST_KEY2Y (ST_Y+23)
//#define ST_KEY2X 239
//#define ST_KEY2Y 191

// Ammunition counter.
#define ST_AMMO0WIDTH 3
#define ST_AMMO0HEIGHT 6
// proff 08/18/98: Changed for high-res
#define ST_AMMO0X (ST_X+288)
#define ST_AMMO0Y (ST_Y+5)
//#define ST_AMMO0X 288
//#define ST_AMMO0Y 173
#define ST_AMMO1WIDTH ST_AMMO0WIDTH
// proff 08/18/98: Changed for high-res
#define ST_AMMO1X (ST_X+288)
#define ST_AMMO1Y (ST_Y+11)
//#define ST_AMMO1X 288
//#define ST_AMMO1Y 179
#define ST_AMMO2WIDTH ST_AMMO0WIDTH
// proff 08/18/98: Changed for high-res
#define ST_AMMO2X (ST_X+288)
#define ST_AMMO2Y (ST_Y+23)
//#define ST_AMMO2X 288
//#define ST_AMMO2Y 191
#define ST_AMMO3WIDTH ST_AMMO0WIDTH
// proff 08/18/98: Changed for high-res
#define ST_AMMO3X (ST_X+288)
#define ST_AMMO3Y (ST_Y+17)
//#define ST_AMMO3X 288
//#define ST_AMMO3Y 185

// Indicate maximum ammunition.
// Only needed because backpack exists.
#define ST_MAXAMMO0WIDTH 3
#define ST_MAXAMMO0HEIGHT 5
// proff 08/18/98: Changed for high-res
#define ST_MAXAMMO0X (ST_X+314)
#define ST_MAXAMMO0Y (ST_Y+5)
//#define ST_MAXAMMO0X 314
//#define ST_MAXAMMO0Y 173
#define ST_MAXAMMO1WIDTH ST_MAXAMMO0WIDTH
// proff 08/18/98: Changed for high-res
#define ST_MAXAMMO1X (ST_X+314)
#define ST_MAXAMMO1Y (ST_Y+11)
//#define ST_MAXAMMO1X 314
//#define ST_MAXAMMO1Y 179
#define ST_MAXAMMO2WIDTH ST_MAXAMMO0WIDTH
// proff 08/18/98: Changed for high-res
#define ST_MAXAMMO2X (ST_X+314)
#define ST_MAXAMMO2Y (ST_Y+23)
//#define ST_MAXAMMO2X 314
//#define ST_MAXAMMO2Y 191
#define ST_MAXAMMO3WIDTH ST_MAXAMMO0WIDTH
// proff 08/18/98: Changed for high-res
#define ST_MAXAMMO3X (ST_X+314)
#define ST_MAXAMMO3Y (ST_Y+17)
//#define ST_MAXAMMO3X 314
//#define ST_MAXAMMO3Y 185

// killough 2/8/98: weapon info position macros UNUSED, removed here

// main player in game
static player_t *plyr;

// ST_Start() has just been called
static dbool st_firsttime;

// CPhipps - no longer do direct PLAYPAL handling here

// used for timing
static unsigned int st_clock;

// used for making messages go away
static int st_msgcounter=0;

// used when in chat
static st_chatstateenum_t st_chatstate;

// whether in automap or first-person
static st_stateenum_t st_gamestate;

// whether left-side main status bar is active
static dbool st_statusbaron;

// whether status bar chat is active
static dbool st_chat;

// value of st_chat before message popped up
static dbool st_oldchat;

// whether chat window has the cursor on
static dbool st_cursoron;

// !deathmatch
static dbool st_notdeathmatch;

// !deathmatch && st_statusbaron
static dbool st_armson;

// !deathmatch
static dbool st_fragson;

// 0-9, tall numbers
static patchnum_t tallnum[10];

// tall % sign
static patchnum_t tallpercent;

// 0-9, short, yellow (,different!) numbers
static patchnum_t shortnum[10];

// 3 key-cards, 3 skulls, 3 card/skull combos
// jff 2/24/98 extend number of patches by three skull/card combos
static patchnum_t keys[NUMCARDS+3];

// face status patches
static patchnum_t faces[ST_NUMFACES];

// face background
static patchnum_t faceback; // CPhipps - single background, translated for different players

//e6y: status bar background
static patchnum_t stbarbg;

// main bar right
static patchnum_t armsbg;

// weapon ownership patches
static patchnum_t arms[6][2];

// ready-weapon widget
static st_number_t w_ready;

//jff 2/16/98 status color change levels
int ammo_red;      // ammo percent less than which status is red
int ammo_yellow;   // ammo percent less is yellow more green
int health_red;    // health amount less than which status is red
int health_yellow; // health amount less than which status is yellow
int health_green;  // health amount above is blue, below is green
int armor_red;     // armor amount less than which status is red
int armor_yellow;  // armor amount less than which status is yellow
int armor_green;   // armor amount above is blue, below is green

 // in deathmatch only, summary of frags stats
static st_number_t w_frags;

// health widget
static st_percent_t w_health;

// arms background
static st_binicon_t  w_armsbg;

// weapon ownership widgets
static st_multicon_t w_arms[6];

// face status widget
static st_multicon_t w_faces;

// keycard widgets
static st_multicon_t w_keyboxes[3];

// armor widget
static st_percent_t  w_armor;

// ammo widgets
static st_number_t   w_ammo[4];

// max ammo widgets
static st_number_t   w_maxammo[4];

 // number of frags so far in deathmatch
static int      st_fragscount;

// used to use appopriately pained face
static int      st_oldhealth = -1;

// used to track armour reduction
static int      st_oldarmour = -1;

// used for evil grin
static dbool  oldweaponsowned[NUMWEAPONS];

 // count until face changes
static int      st_facecount = 0;

// current face index, used by w_faces
static int      st_faceindex = 0;

// holds key-type for each key box on bar
static int      keyboxes[3];

// a random number per tick
static int      st_randomnumber;

/* Heretic health-chain animation: HealthMarker chases the real health so the
 * life gem glides rather than snapping, and ChainWiggle bobs the chain by a
 * pixel while it is moving. Updated once per tic in ST_Ticker. */
static int      HealthMarker;
static int      ChainWiggle;

extern char     *mapnames[];

//
// STATUS BAR CODE
//

static void ST_Stop(void);

static void ST_refreshBackground(void)
{
  int y=0;

  if (st_statusbaron)
    {
      /* The background patches (stbarbg, armsbg, and the netgame faceback)
       * are static art.  Re-stretching them from 320-space into the BG buffer
       * with VPT_STRETCH every frame is the dominant status-bar cost at high
       * internal resolutions (~1.1 ms/frame at 2560x1200); only the final
       * BG -> FG copy actually has to happen each frame (the libretro frontend
       * rotates framebuffers, so the visible buffer must be refilled).  Stretch
       * into BG once and rebuild only when something that affects the cached
       * pixels changes: the internal resolution, the arms-panel visibility, or
       * (netgame only) the displayed player's face background. */
      static int cached_w        = -1;
      static int cached_h        = -1;
      static int cached_armson   = -1;
      static int cached_netface   = -2;
      int        netface         = netgame ? displayplayer : -1;

      if (cached_w      != SCREENWIDTH || cached_h       != SCREENHEIGHT ||
          cached_armson != st_armson   || cached_netface != netface)
        {
          V_DrawNumPatch(ST_X, y, BG, stbarbg.lumpnum, CR_DEFAULT, VPT_STRETCH);
          if (st_armson)
            V_DrawNumPatch(ST_ARMSBGX, y, BG, armsbg.lumpnum, CR_DEFAULT, VPT_STRETCH);

          // killough 3/7/98: make face background change with displayplayer
          if (netgame)
            {
              V_DrawNumPatch(ST_FX, y, BG, faceback.lumpnum,
                 displayplayer ? CR_LIMIT+displayplayer : CR_DEFAULT,
                 displayplayer ? (VPT_TRANS | VPT_STRETCH) : VPT_STRETCH);
            }

          cached_w       = SCREENWIDTH;
          cached_h       = SCREENHEIGHT;
          cached_armson  = st_armson;
          cached_netface = netface;
        }

      V_CopyRect(ST_X, y, BG, ST_SCALED_WIDTH, ST_SCALED_HEIGHT, ST_X, ST_SCALED_Y, FG, VPT_NONE);
    }
}


// Respond to keyboard input events,
//  intercept cheats.
dbool ST_Responder(event_t *ev)
{
  // Filter automap on/off.
  if (ev->type == ev_keyup && (ev->data1 & 0xffff0000) == AM_MSGHEADER)
    {
      switch(ev->data1)
        {
        case AM_MSGENTERED:
          st_gamestate = AutomapState;
          st_firsttime = TRUE;
          break;

        case AM_MSGEXITED:
          st_gamestate = FirstPersonState;
          break;
        }
    }
  else  // if a user keypress...
    if (ev->type == ev_keydown)       // Try cheat responder in m_cheat.c
      return M_FindCheats(ev->data1); // killough 4/17/98, 5/2/98
  return FALSE;
}

static int ST_calcPainOffset(void)
{
  static int lastcalc;
  static int oldhealth = -1;
  int health = plyr->health > 100 ? 100 : plyr->health;

  if (health != oldhealth)
    {
      lastcalc = ST_FACESTRIDE * (((100 - health) * ST_NUMPAINFACES) / 101);
      oldhealth = health;
    }
  return lastcalc;
}

//
// This is a not-very-pretty routine which handles
//  the face states and their timing.
// the precedence of expressions is:
//  dead > evil grin > turned head > straight ahead
//

static void ST_updateFaceWidget(void)
{
  int         i;
  angle_t     badguyangle;
  angle_t     diffang;
  static int  lastattackdown = -1;
  static int  priority = 0;
  dbool     doevilgrin;

  if (priority < 10)
    {
      // dead
      if (!plyr->health)
        {
          priority = 9;
          st_faceindex = ST_DEADFACE;
          st_facecount = 1;
        }
    }

  if (priority < 9)
    {
      if (plyr->bonuscount)
        {
          // picking up bonus
          doevilgrin = FALSE;

          for (i=0;i<NUMWEAPONS;i++)
            {
              if (oldweaponsowned[i] != plyr->weaponowned[i])
                {
                  doevilgrin = TRUE;
                  oldweaponsowned[i] = plyr->weaponowned[i];
                }
            }
          if (doevilgrin)
            {
              // evil grin if just picked up weapon
              priority = 8;
              st_facecount = ST_EVILGRINCOUNT;
              st_faceindex = ST_calcPainOffset() + ST_EVILGRINOFFSET;
            }
        }

    }

  if (priority < 8)
    {
      if (plyr->damagecount && plyr->attacker && plyr->attacker != plyr->mo)
        {
          // being attacked
          priority = 7;

          // haleyjd 10/12/03: classic DOOM problem of missing OUCH face
          // was due to inversion of this test:
          // if(plyr->health - st_oldhealth > ST_MUCHPAIN)
          if(st_oldhealth - plyr->health > ST_MUCHPAIN)
            {
              st_facecount = ST_TURNCOUNT;
              st_faceindex = ST_calcPainOffset() + ST_OUCHOFFSET;
            }
          else
            {
              badguyangle = R_PointToAngle2(plyr->mo->x,
                                            plyr->mo->y,
                                            plyr->attacker->x,
                                            plyr->attacker->y);

              if (badguyangle > plyr->mo->angle)
                {
                  // whether right or left
                  diffang = badguyangle - plyr->mo->angle;
                  i = diffang > ANG180;
                }
              else
                {
                  // whether left or right
                  diffang = plyr->mo->angle - badguyangle;
                  i = diffang <= ANG180;
                } // confusing, aint it?


              st_facecount = ST_TURNCOUNT;
              st_faceindex = ST_calcPainOffset();

              if (diffang < ANG45)
                {
                  // head-on
                  st_faceindex += ST_RAMPAGEOFFSET;
                }
              else if (i)
                {
                  // turn face right
                  st_faceindex += ST_TURNOFFSET;
                }
              else
                {
                  // turn face left
                  st_faceindex += ST_TURNOFFSET+1;
                }
            }
        }
    }

  if (priority < 7)
    {
      // getting hurt because of your own damn stupidity
      if (plyr->damagecount)
        {
          // haleyjd 10/12/03: classic DOOM problem of missing OUCH face
          // was due to inversion of this test:
          // if(plyr->health - st_oldhealth > ST_MUCHPAIN)
          if(st_oldhealth - plyr->health > ST_MUCHPAIN)
            {
              priority = 7;
              st_facecount = ST_TURNCOUNT;
              st_faceindex = ST_calcPainOffset() + ST_OUCHOFFSET;
            }
          else
            {
              priority = 6;
              st_facecount = ST_TURNCOUNT;
              st_faceindex = ST_calcPainOffset() + ST_RAMPAGEOFFSET;
            }

        }

    }

  if (priority < 6)
    {
      // rapid firing
      if (plyr->attackdown)
        {
          if (lastattackdown==-1)
            lastattackdown = ST_RAMPAGEDELAY;
          else if (!--lastattackdown)
            {
              priority = 5;
              st_faceindex = ST_calcPainOffset() + ST_RAMPAGEOFFSET;
              st_facecount = 1;
              lastattackdown = 1;
            }
        }
      else
        lastattackdown = -1;

    }

  if (priority < 5)
    {
      // invulnerability
      if ((plyr->cheats & CF_GODMODE)
          || plyr->powers[pw_invulnerability])
        {
          priority = 4;

          st_faceindex = ST_GODFACE;
          st_facecount = 1;

        }

    }

  // look left or look right if the facecount has timed out
  if (!st_facecount)
    {
      st_faceindex = ST_calcPainOffset() + (st_randomnumber % 3);
      st_facecount = ST_STRAIGHTFACECOUNT;
      priority = 0;
    }

  st_facecount--;

}

int sts_traditional_keys; // killough 2/28/98: traditional status bar keys

static void ST_updateWidgets(void)
{
  static int  largeammo = 1994; // means "n/a"
  int         i;

  // must redirect the pointer if the ready weapon has changed.
  //  if (w_ready.data != plyr->readyweapon)
  //  {
  if (weaponinfo[plyr->readyweapon].ammo == AM_NOAMMO)
    w_ready.num = &largeammo;
  else
    w_ready.num = &plyr->ammo[weaponinfo[plyr->readyweapon].ammo];
  //{
  // static int tic=0;
  // static int dir=-1;
  // if (!(tic&15))
  //   plyr->ammo[weaponinfo[plyr->readyweapon].ammo]+=dir;
  // if (plyr->ammo[weaponinfo[plyr->readyweapon].ammo] == -100)
  //   dir = 1;
  // tic++;
  // }
  w_ready.data = plyr->readyweapon;

  // if (*w_ready.on)
  //  STlib_updateNum(&w_ready, TRUE);
  // refresh weapon change
  //  }

  // update keycard multiple widgets
  for (i=0;i<3;i++)
    {
      keyboxes[i] = plyr->cards[i] ? i : -1;

      //jff 2/24/98 select double key
      //killough 2/28/98: preserve traditional keys by config option

      if (plyr->cards[i+3])
        keyboxes[i] = keyboxes[i]==-1 || sts_traditional_keys ? i+3 : i+6;
    }

  // refresh everything if this is him coming back to life
  ST_updateFaceWidget();

  // used by the w_armsbg widget
  st_notdeathmatch = !deathmatch;

  // used by w_arms[] widgets
  st_armson = st_statusbaron && !deathmatch;

  // used by w_frags widget
  st_fragson = deathmatch && st_statusbaron;
  st_fragscount = 0;

  for (i=0 ; i<MAXPLAYERS ; i++)
    {
      if (i != displayplayer)            // killough 3/7/98
        st_fragscount += plyr->frags[i];
      else
        st_fragscount -= plyr->frags[i];
    }

  // get rid of chat window if up because of message
  if (!--st_msgcounter)
    st_chat = st_oldchat;

}

extern void retro_set_rumble_damage(int damage, float duration);

void ST_Ticker(void)
{
  st_clock++;
  st_randomnumber = M_Random();
  ST_updateWidgets();

  /* If player has taken damage, trigger a
   * rumble effect */
  if (st_oldhealth > plyr->health)
  {
    int damage = st_oldhealth - plyr->health;
    /* Add any armour damage */
    if (st_oldarmour > plyr->armorpoints)
      damage += st_oldarmour - plyr->armorpoints;

    retro_set_rumble_damage(damage, 333.3333f);
  }

  st_oldhealth = plyr->health;
  st_oldarmour = plyr->armorpoints;

  /* Heretic: glide the health gem toward the real health and wiggle the
   * chain while it moves. Mirrors the Raven SB_Ticker behaviour. */
  if (heretic)
  {
    extern int inventory, inventoryTics, inv_ptr;
    int curhealth = plyr->health;
    int delta;

    /* Auto-close the inventory bar after its display timer runs out,
     * leaving the highlighted artifact selected. */
    if (inventory && inventoryTics > 0 && !--inventoryTics)
    {
      inventory = FALSE;
      plyr->readyArtifact = plyr->inventory[inv_ptr].type;
    }

    if (curhealth < 0)
      curhealth = 0;

    if (leveltime & 1)
      ChainWiggle = P_Random(pr_heretic) & 1;

    if (curhealth < HealthMarker)
    {
      delta = (HealthMarker - curhealth) >> 2;
      if (delta < 1)
        delta = 1;
      else if (delta > 6)
        delta = 6;
      HealthMarker -= delta;
    }
    else if (curhealth > HealthMarker)
    {
      delta = (curhealth - HealthMarker) >> 2;
      if (delta < 1)
        delta = 1;
      else if (delta > 6)
        delta = 6;
      HealthMarker += delta;
    }
  }
}

int st_palette = 0;

static void ST_doPaletteStuff(void)
{
  int         palette;
  int cnt = plyr->damagecount;

  if (plyr->powers[pw_strength])
    {
      // slowly fade the berzerk out
      int bzc = 12 - (plyr->powers[pw_strength]>>6);
      if (bzc > cnt)
        cnt = bzc;
    }

  if (cnt)
    {
      palette = (cnt+7)>>3;
      if (palette >= NUMREDPALS)
        palette = NUMREDPALS-1;

      /* cph 2006/08/06 - if in the menu, reduce the red tint - navigating to
       * load a game can be tricky if the screen is all red */
      if (menuactive) palette >>=1;

      palette += STARTREDPALS;
    }
  else
    if (plyr->bonuscount)
      {
        palette = (plyr->bonuscount+7)>>3;
        if (palette >= NUMBONUSPALS)
          palette = NUMBONUSPALS-1;
        palette += STARTBONUSPALS;
      }
    else
      if (plyr->powers[pw_ironfeet] > 4*32 || plyr->powers[pw_ironfeet] & 8)
        palette = RADIATIONPAL;
      else
        palette = 0;

  if (palette != st_palette) {
    V_SetPalette(st_palette = palette); // CPhipps - use new palette function

    // have to redraw the entire status bar when the palette changes
    // in TRUEcolor modes - POPE
    st_firsttime = TRUE;
  }
}

static void ST_drawWidgets(dbool refresh)
{
  int i, ammolevel;

  // used by w_arms[] widgets
  st_armson = st_statusbaron && !deathmatch;

  // used by w_frags widget
  st_fragson = deathmatch && st_statusbaron;

  //jff 2/16/98 make color of ammo depend on amount
  //djsd 12/01/10 add empty, full.
  ammolevel = P_GetAmmoLevel(plyr, w_ready.data); // handles BFG/SSG
  if (ammolevel == 0)
    STlib_updateNum(&w_ready, CR_GRAY, refresh);
  else if (ammolevel >= 100)
    STlib_updateNum(&w_ready, CR_BLUE2, refresh);
  else if (ammolevel < ammo_red)
    STlib_updateNum(&w_ready, CR_RED, refresh);
  else if (ammolevel < ammo_yellow)
    STlib_updateNum(&w_ready, CR_GOLD, refresh);
  else
    STlib_updateNum(&w_ready, CR_GREEN, refresh);

  for (i=0;i<4;i++)
    {
      STlib_updateNum(&w_ammo[i], CR_DEFAULT, refresh);   //jff 2/16/98 no xlation
      STlib_updateNum(&w_maxammo[i], CR_DEFAULT, refresh);
    }

  //jff 2/16/98 make color of health depend on amount
  if (*w_health.n.num<health_red)
    STlib_updatePercent(&w_health, CR_RED, refresh);
  else if (*w_health.n.num<health_yellow)
    STlib_updatePercent(&w_health, CR_GOLD, refresh);
  else if (*w_health.n.num<=health_green)
    STlib_updatePercent(&w_health, CR_GREEN, refresh);
  else
    STlib_updatePercent(&w_health, CR_BLUE2, refresh); //killough 2/28/98

  //jff 2/16/98 make color of armor depend on amount
  if (*w_armor.n.num<armor_red)
    STlib_updatePercent(&w_armor, CR_RED, refresh);
  else if (*w_armor.n.num<armor_yellow)
    STlib_updatePercent(&w_armor, CR_GOLD, refresh);
  else if (*w_armor.n.num<=armor_green)
    STlib_updatePercent(&w_armor, CR_GREEN, refresh);
  else
    STlib_updatePercent(&w_armor, CR_BLUE2, refresh); //killough 2/28/98

  //e6y: moved to ST_refreshBackground() for correct single-pass stretching
  //STlib_updateBinIcon(&w_armsbg, refresh);

  for (i=0;i<6;i++)
    STlib_updateMultIcon(&w_arms[i], refresh);

  STlib_updateMultIcon(&w_faces, refresh);

  for (i=0;i<3;i++)
    STlib_updateMultIcon(&w_keyboxes[i], refresh);

  STlib_updateNum(&w_frags, CR_DEFAULT, refresh);

}



/*
 * Minimal Heretic status bar.
 *
 * Heretic's bar is a 42px-tall strip at the bottom built from its own
 * lumps (BARBACK background, STATBAR stat panel) with the big "IN" number
 * font for health and ammo. This is intentionally a reduced version: it
 * draws the bar frame and the health / ready-ammo readouts so the bottom
 * of the screen shows the real bar instead of framebuffer residue. The
 * artifact bar, life-gem chain, key/armor icons and the inventory are a
 * later, fuller sb_bar port.
 *
 * Coordinates are in the 320x200 base space (V_DrawNumPatch with
 * VPT_STRETCH maps them to the scaled screen). The bar top is at
 * y = 200 - 42 = 158.
 */

#define HST_Y        158   /* top of the 42px Heretic bar */

/* Draw a right-aligned up-to-3-digit number using the Heretic IN font
 * (each glyph 9px wide). x is the left edge of the hundreds glyph. */
static void ST_HereticDrawINumber(int val, int x, int y)
{
  char name[9];
  int  d;

  if (val < 0)
    val = 0;

  if (val > 99)
  {
    d = (val / 100) % 10;
    sprintf(name, "IN%d", d);
    if (W_CheckNumForName(name) >= 0)
      V_DrawNamePatch(x, y, FG, name, CR_DEFAULT, VPT_STRETCH);
  }
  if (val > 9)
  {
    d = (val / 10) % 10;
    sprintf(name, "IN%d", d);
    if (W_CheckNumForName(name) >= 0)
      V_DrawNamePatch(x + 9, y, FG, name, CR_DEFAULT, VPT_STRETCH);
  }
  d = val % 10;
  sprintf(name, "IN%d", d);
  if (W_CheckNumForName(name) >= 0)
    V_DrawNamePatch(x + 18, y, FG, name, CR_DEFAULT, VPT_STRETCH);
}

/* Draw a small two-digit number using the Heretic SMALLIN font (each glyph
 * 4px wide). Used for artifact counts. A count of 1 draws nothing, matching
 * the Raven HUD (a single item shows just the icon). */
static void ST_HereticDrawSmallNumber(int val, int x, int y)
{
  char name[9];
  int  d;

  if (val <= 1)
    return;
  if (val > 9)
  {
    d = (val / 10) % 10;
    sprintf(name, "SMALLIN%d", d);
    if (W_CheckNumForName(name) >= 0)
      V_DrawNamePatch(x, y, FG, name, CR_DEFAULT, VPT_STRETCH);
  }
  d = val % 10;
  sprintf(name, "SMALLIN%d", d);
  if (W_CheckNumForName(name) >= 0)
    V_DrawNamePatch(x + 4, y, FG, name, CR_DEFAULT, VPT_STRETCH);
}

/* Draw an artifact icon by lump name. The Heretic artifact graphics
 * (ARTIPTN2, ARTIINVU, ...) live in the sprite namespace (between the
 * S_START/S_END markers) because they double as the in-world pickup
 * sprites, so a plain W_CheckNumForName/W_GetNumForName -- which search
 * ns_global -- never finds them and the icon silently fails to draw.
 * Resolve in ns_sprites first, falling back to ns_global (ARTIBOX is a
 * global lump). */
static void ST_HereticDrawArtiIcon(const char *name, int x, int y)
{
  int lump = (W_CheckNumForName)(name, ns_sprites);
  if (lump < 0)
    lump = W_CheckNumForName(name);
  if (lump >= 0)
    V_DrawNumPatch(x, y, FG, lump, CR_DEFAULT, VPT_STRETCH);
}

/* Draw the full inventory bar (shown while the player is cycling artifacts):
 * the INVBAR row, up to 7 artifact icons with counts, and the selection box
 * over the current cursor position. Replaces the stat panel while open. */
static void ST_HereticDrawInvBar(player_t *plyr, const char *const *arti_icon)
{
  extern int inv_ptr, curpos;
  int i, base;

  if (W_CheckNumForName("INVBAR") >= 0)
    V_DrawNamePatch(34, 160, FG, "INVBAR", CR_DEFAULT, VPT_STRETCH);

  base = inv_ptr - curpos;
  if (base < 0)
    base = 0;

  for (i = 0; i < 7; i++)
  {
    int slot = base + i;
    if (slot < plyr->inventorySlotNum &&
        plyr->inventory[slot].type != arti_none)
    {
      int t = plyr->inventory[slot].type;
      if (t > 0 && t < NUMARTIFACTS)
        ST_HereticDrawArtiIcon(arti_icon[t], 50 + i * 31, 160);
      ST_HereticDrawSmallNumber(plyr->inventory[slot].count,
                                69 + i * 31, 182);
    }
  }

  if (W_CheckNumForName("SELECTBO") >= 0)
    V_DrawNamePatch(50 + curpos * 31, 189, FG, "SELECTBO",
                    CR_DEFAULT, VPT_STRETCH);
}

static void ST_HereticDrawer(void)
{
  /* Ammo-type icon next to the ammo count, indexed by ammotype_t
   * (am_goldwand=0 .. am_mace=5), matching the Raven artwork order. */
  static const char *const ammo_icon[HERETIC_NUMAMMO] = {
    "INAMGLD", "INAMBOW", "INAMBST", "INAMRAM", "INAMPNX", "INAMLOB"
  };
  /* Artifact sprite per arti type (arti_none=0 .. arti_teleport=10). */
  static const char *const arti_icon[NUMARTIFACTS] = {
    "ARTIBOX",  "ARTIINVU", "ARTIINVS", "ARTIPTN2", "ARTISPHL", "ARTIPWBK",
    "ARTITRCH", "ARTIFBMB", "ARTIEGGC", "ARTISOAR", "ARTIATLP"
  };
  extern int  inventory;
  player_t   *plyr = &players[displayplayer];
  ammotype_t  at;
  int         ammo;
  int         health, gempos, chainy;

  /* Background frame (always).
   *
   * BARBACK is a static 320x42 patch.  Re-stretching it from 320-space to the
   * full screen width with VPT_STRETCH every frame is by far the most
   * expensive part of the Heretic HUD at high internal resolutions (~1.4 ms
   * of a ~2.5 ms HUD pass at 2560x1200).  Since the art never changes, stretch
   * it once into the offscreen BG buffer and copy the bar region to the
   * visible FG screen each frame with V_CopyRect (a flat row copy, no
   * per-pixel scaling).  Re-stretch only when the internal resolution changes,
   * mirroring how the Doom status bar caches its background. */
  {
    static int   barback_cached_w = -1;
    static int   barback_cached_h = -1;
    static int   barback_lump     = -2; /* -2 = unresolved, -1 = absent */

    if (barback_lump == -2)
      barback_lump = W_CheckNumForName("BARBACK");

    if (barback_lump >= 0)
    {
      /* Scaled bar geometry. BARBACK is 320x42 in 320x200 space, so the bar
       * occupies screen rows [HST_Y_scaled .. SCREENHEIGHT). The BG cache
       * holds the stretched bar at its top (y=0). */
      int bar_dst_y     = (HST_Y * SCREENHEIGHT) / 200;
      int bar_scaled_h  = SCREENHEIGHT - bar_dst_y;

      if (barback_cached_w != SCREENWIDTH || barback_cached_h != SCREENHEIGHT)
      {
        /* Stretch BARBACK once into the top of the BG buffer (y=0). */
        V_DrawNumPatch(ST_X, 0, BG, barback_lump, CR_DEFAULT, VPT_STRETCH);
        barback_cached_w = SCREENWIDTH;
        barback_cached_h = SCREENHEIGHT;
      }

      /* Flat-copy the cached rows from BG (y=0) to the bar position on FG.
       * Coordinates are already in screen space, so no VPT_STRETCH. */
      V_CopyRect(0, 0, BG, SCREENWIDTH, bar_scaled_h, 0, bar_dst_y, FG, VPT_NONE);
    }
  }

  /* While the inventory bar is open it replaces the stat panel and its
   * widgets; the health chain below is still drawn either way. */
  if (inventory)
  {
    ST_HereticDrawInvBar(plyr, arti_icon);
  }
  else
  {
  /* Stat panel (x=34,y=160). */
  if (W_CheckNumForName("STATBAR") >= 0)
    V_DrawNamePatch(34, 160, FG, "STATBAR", CR_DEFAULT, VPT_STRETCH);

  /* Health number (x=57,y=170). */
  ST_HereticDrawINumber(plyr->health, 57, 170);

  /* Ready-weapon ammo count plus its type icon. The staff and gauntlets
   * use no ammo (AM_NOAMMO), so nothing is drawn for them. */
  at = weaponinfo[plyr->readyweapon].ammo;
  if (at != AM_NOAMMO)
  {
    ammo = plyr->ammo[at];
    ST_HereticDrawINumber(ammo, 109, 162);
    if ((int)at >= 0 && (int)at < HERETIC_NUMAMMO &&
        W_CheckNumForName(ammo_icon[at]) >= 0)
      V_DrawNamePatch(111, 172, FG, ammo_icon[at], CR_DEFAULT, VPT_STRETCH);
  }

  /* Keys: the three key icons stack at x=153. Heretic's blue/yellow/green
   * keys map onto the it_bluecard/it_yellowcard/it_redcard slots (green
   * reuses the red slot). */
  if (plyr->cards[it_yellowcard] && W_CheckNumForName("YKEYICON") >= 0)
    V_DrawNamePatch(153, 164, FG, "YKEYICON", CR_DEFAULT, VPT_STRETCH);
  if (plyr->cards[it_redcard] && W_CheckNumForName("GKEYICON") >= 0)
    V_DrawNamePatch(153, 172, FG, "GKEYICON", CR_DEFAULT, VPT_STRETCH);
  if (plyr->cards[it_bluecard] && W_CheckNumForName("BKEYICON") >= 0)
    V_DrawNamePatch(153, 180, FG, "BKEYICON", CR_DEFAULT, VPT_STRETCH);

  /* Armor amount (x=228,y=170). */
  ST_HereticDrawINumber(plyr->armorpoints, 224, 170);

  /* Ready-artifact box (x=180,y=160): the currently selected artifact, its
   * count, and the use-flash animation when one has just been used. */
  {
    extern int inv_ptr, ArtifactFlash;

    if (ArtifactFlash)
    {
      char fname[9];
      if (W_CheckNumForName("BLACKSQ") >= 0)
        V_DrawNamePatch(180, 161, FG, "BLACKSQ", CR_DEFAULT, VPT_STRETCH);
      /* USEARTIA..D are the flash frames; ArtifactFlash counts down 4->1. */
      sprintf(fname, "USEARTI%c", 'A' + (ArtifactFlash - 1));
      if (W_CheckNumForName(fname) >= 0)
        V_DrawNamePatch(182, 161, FG, fname, CR_DEFAULT, VPT_STRETCH);
      ArtifactFlash--;
    }
    else if (plyr->readyArtifact > 0 && plyr->readyArtifact < NUMARTIFACTS)
    {
      if (W_CheckNumForName("ARTIBOX") >= 0)
        V_DrawNamePatch(180, 161, FG, "ARTIBOX", CR_DEFAULT, VPT_STRETCH);
      ST_HereticDrawArtiIcon(arti_icon[plyr->readyArtifact], 179, 160);
      if (plyr->inventorySlotNum > 0)
        ST_HereticDrawSmallNumber(plyr->inventory[inv_ptr].count, 201, 182);
    }
  }
  } /* end stat-panel (inventory closed) */

  /* Health chain along the bottom: a gem slides between the two gargoyle
   * heads (LTFACE at the left -- the "demon's mouth" the chain runs from --
   * and RTFACE at the right) to show health 0..100. The gem follows the
   * smoothed HealthMarker so it glides, and the chain bobs by ChainWiggle
   * while the marker is still catching up to the real health. LIFEGEM2 is
   * the single-player red gem. Geometry follows the Raven HUD. */
  health = HealthMarker;
  if (health < 0)
    health = 0;
  else if (health > 100)
    health = 100;
  gempos = (health * 256) / 100;
  chainy = (HealthMarker == plyr->health) ? 191 : 191 + ChainWiggle;
  if (W_CheckNumForName("CHAIN") >= 0)
    V_DrawNamePatch(2 + (gempos % 17), chainy, FG, "CHAIN", CR_DEFAULT, VPT_STRETCH);
  if (W_CheckNumForName("LIFEGEM2") >= 0)
    V_DrawNamePatch(17 + gempos, chainy, FG, "LIFEGEM2", CR_DEFAULT, VPT_STRETCH);
  if (W_CheckNumForName("LTFACE") >= 0)
    V_DrawNamePatch(0, 190, FG, "LTFACE", CR_DEFAULT, VPT_STRETCH);
  if (W_CheckNumForName("RTFACE") >= 0)
    V_DrawNamePatch(276, 190, FG, "RTFACE", CR_DEFAULT, VPT_STRETCH);
}

/* Fullscreen overlay shown when the status bar is hidden: health at the
 * lower-left, the ready weapon's ammo and icon at the lower-right, and the
 * ready-artifact box in the corner -- all drawn directly over the play view
 * with no bar background. Uses 320x200 coordinates (VPT_STRETCH scales). */
static void ST_HereticFullscreenDrawer(void)
{
  static const char *const ammo_icon[HERETIC_NUMAMMO] = {
    "INAMGLD", "INAMBOW", "INAMBST", "INAMRAM", "INAMPNX", "INAMLOB"
  };
  static const char *const arti_icon[NUMARTIFACTS] = {
    "ARTIBOX",  "ARTIINVU", "ARTIINVS", "ARTIPTN2", "ARTISPHL", "ARTIPWBK",
    "ARTITRCH", "ARTIFBMB", "ARTIEGGC", "ARTISOAR", "ARTIATLP"
  };
  extern int  inv_ptr;
  player_t   *plyr = &players[displayplayer];
  ammotype_t  at;

  /* Health, lower-left. */
  ST_HereticDrawINumber(plyr->health, 5, 180);

  /* Ready-weapon ammo and its icon, lower-right. */
  at = weaponinfo[plyr->readyweapon].ammo;
  if (at != AM_NOAMMO)
  {
    ST_HereticDrawINumber(plyr->ammo[at], 274, 180);
    if ((int)at >= 0 && (int)at < HERETIC_NUMAMMO &&
        W_CheckNumForName(ammo_icon[at]) >= 0)
      V_DrawNamePatch(252, 180, FG, ammo_icon[at], CR_DEFAULT, VPT_STRETCH);
  }

  /* Ready-artifact box in the lower-right corner. */
  if (plyr->readyArtifact > 0 && plyr->readyArtifact < NUMARTIFACTS)
  {
    if (W_CheckNumForName("ARTIBOX") >= 0)
      V_DrawNamePatch(286, 170, FG, "ARTIBOX", CR_DEFAULT, VPT_STRETCH);
    /* Inset the icon by 1px inside the box, matching the stat-panel box
     * (box at x,y; icon at x-1,y-1) rather than drawing it flush. */
    ST_HereticDrawArtiIcon(arti_icon[plyr->readyArtifact], 285, 169);
    if (plyr->inventorySlotNum > 0)
      ST_HereticDrawSmallNumber(plyr->inventory[inv_ptr].count, 307, 192);
  }
}

/* Hexen artifact icon per hexen_arti_* type.  These graphics live in the
 * sprite namespace (they double as in-world pickup sprites), so
 * ST_HereticDrawArtiIcon resolves them via ns_sprites. */
static const char *const hexen_arti_icon[HEXEN_NUMARTIFACTS] = {
  "ARTIBOX",  "ARTIINVU", "ARTIPTN2", "ARTISPHL", "ARTIHRAD", "ARTISUMN",
  "ARTITRCH", "ARTIPORK", "ARTISOAR", "ARTIBLST", "ARTIPSBG", "ARTITELO",
  "ARTISPED", "ARTIBMAN", "ARTIBRAC", "ARTIATLP", "ARTISKLL", "ARTIBGEM",
  "ARTIGEMR", "ARTIGEMG", "ARTIGMG2", "ARTIGEMB", "ARTIGMB2", "ARTIBOK1",
  "ARTIBOK2", "ARTISKL2", "ARTIFWEP", "ARTICWEP", "ARTIMWEP", "ARTIGEAR",
  "ARTIGER2", "ARTIGER3", "ARTIGER4"
};

/* The full inventory bar for Hexen (shown while cycling artifacts).  Mirrors
 * ST_HereticDrawInvBar but uses the larger Hexen artifact set/bound and the
 * Hexen SELECTBOX lump name (SELECTBO). */
static void ST_HexenDrawInvBar(player_t *plyr)
{
  extern int inv_ptr, curpos;
  int i, base;

  if (W_CheckNumForName("INVBAR") >= 0)
    V_DrawNamePatch(38, 162, FG, "INVBAR", CR_DEFAULT, VPT_STRETCH);

  base = inv_ptr - curpos;
  if (base < 0)
    base = 0;

  for (i = 0; i < 7; i++)
  {
    int slot = base + i;
    if (slot < plyr->inventorySlotNum &&
        plyr->inventory[slot].type != arti_none)
    {
      int t = plyr->inventory[slot].type;
      if (t > 0 && t < HEXEN_NUMARTIFACTS)
        ST_HereticDrawArtiIcon(hexen_arti_icon[t], 50 + i * 31, 162);
      ST_HereticDrawSmallNumber(plyr->inventory[slot].count,
                                69 + i * 31, 185);
    }
  }

  if (W_CheckNumForName("SELECTBO") >= 0)
    V_DrawNamePatch(50 + curpos * 31, 189, FG, "SELECTBO",
                    CR_DEFAULT, VPT_STRETCH);
}

/* Hexen status bar.  Layout follows the Raven HUD (320x200 coords scaled by
 * VPT_STRETCH): the H2BAR frame, then either the inventory bar or the stat
 * panel with health, the two mana pools (count + bright/dim icon by ready
 * weapon), armor, the ready-artifact box, and the fourth-weapon piece
 * indicators.  The fork's player_t carries a single armorpoints value and a
 * single mana pair, so the per-armor-type sum and the vial fill-levels from
 * the original are simplified to those fields. */
static void ST_HexenDrawer(void)
{
  extern int inventory, inv_ptr, ArtifactFlash;
  player_t  *plyr = &players[displayplayer];
  int        m1, m2;

  /* H2BAR frame (320x65 at y=134).  Drawn directly each frame with
   * VPT_STRETCH; the status-bar BG cache buffer (screens[BG]) is sized for
   * the Doom/Heretic bar, so stretching the taller Hexen frame through it
   * would overrun it. */
  if (W_CheckNumForName("H2BAR") >= 0)
    V_DrawNamePatch(0, 134, FG, "H2BAR", CR_DEFAULT, VPT_STRETCH);

  if (inventory)
  {
    ST_HexenDrawInvBar(plyr);
  }
  else
  {
    /* Stat panel. */
    if (W_CheckNumForName("STATBAR") >= 0)
      V_DrawNamePatch(38, 162, FG, "STATBAR", CR_DEFAULT, VPT_STRETCH);

    /* Health (use the smoothed HealthMarker, clamped 0..100 like the bar). */
    {
      int h = HealthMarker;
      if (h < 0) h = 0; else if (h > 100) h = 100;
      ST_HereticDrawINumber(h, 40, 176);
    }

    /* Mana pools: count + bright/dim icon depending on whether the ready
     * weapon draws on that mana type. */
    m1 = plyr->mana[0];
    m2 = plyr->mana[1];
    ST_HereticDrawSmallNumber(m1, 79, 181);
    ST_HereticDrawSmallNumber(m2, 111, 181);
    if (W_CheckNumForName(m1 ? "MANABRT1" : "MANADIM1") >= 0)
      V_DrawNamePatch(77, 164, FG, m1 ? "MANABRT1" : "MANADIM1",
                      CR_DEFAULT, VPT_STRETCH);
    if (W_CheckNumForName(m2 ? "MANABRT2" : "MANADIM2") >= 0)
      V_DrawNamePatch(110, 164, FG, m2 ? "MANABRT2" : "MANADIM2",
                      CR_DEFAULT, VPT_STRETCH);
    if (W_CheckNumForName("MANAVL1") >= 0)
      V_DrawNamePatch(94, 164, FG, "MANAVL1", CR_DEFAULT, VPT_STRETCH);
    if (W_CheckNumForName("MANAVL2") >= 0)
      V_DrawNamePatch(102, 164, FG, "MANAVL2", CR_DEFAULT, VPT_STRETCH);

    /* Armor. */
    ST_HereticDrawINumber(plyr->armorpoints, 250, 176);

    /* Ready-artifact box, count, and the use-flash animation. */
    if (ArtifactFlash)
    {
      char fname[9];
      if (W_CheckNumForName("ARTICLS") >= 0)
        V_DrawNamePatch(144, 160, FG, "ARTICLS", CR_DEFAULT, VPT_STRETCH);
      sprintf(fname, "USEARTI%c", 'A' + (ArtifactFlash - 1));
      if (W_CheckNumForName(fname) >= 0)
        V_DrawNamePatch(148, 164, FG, fname, CR_DEFAULT, VPT_STRETCH);
      ArtifactFlash--;
    }
    else if (plyr->readyArtifact > 0 &&
             plyr->readyArtifact < HEXEN_NUMARTIFACTS)
    {
      if (W_CheckNumForName("ARTIBOX") >= 0)
        V_DrawNamePatch(143, 163, FG, "ARTIBOX", CR_DEFAULT, VPT_STRETCH);
      ST_HereticDrawArtiIcon(hexen_arti_icon[plyr->readyArtifact], 143, 163);
      if (plyr->inventorySlotNum > 0)
        ST_HereticDrawSmallNumber(plyr->inventory[inv_ptr].count, 162, 184);
    }

    /* Fourth-weapon piece indicators (class-specific lumps). */
    {
      const char *cls = (plyr->class == PCLASS_CLERIC) ? "C"
                      : (plyr->class == PCLASS_MAGE)   ? "M" : "F";
      char nm[9];
      if (plyr->pieces == (WPIECE1 | WPIECE2 | WPIECE3))
      {
        sprintf(nm, "WPFULL%c", (plyr->class == PCLASS_CLERIC) ? '1'
                              : (plyr->class == PCLASS_MAGE)   ? '2' : '0');
        if (W_CheckNumForName(nm) >= 0)
          V_DrawNamePatch(190, 162, FG, nm, CR_DEFAULT, VPT_STRETCH);
      }
      else
      {
        if (plyr->pieces & WPIECE1)
        {
          sprintf(nm, "WPIECE%c1", cls[0]);
          if (W_CheckNumForName(nm) >= 0)
            V_DrawNamePatch(190, 162, FG, nm, CR_DEFAULT, VPT_STRETCH);
        }
        if (plyr->pieces & WPIECE2)
        {
          sprintf(nm, "WPIECE%c2", cls[0]);
          if (W_CheckNumForName(nm) >= 0)
            V_DrawNamePatch(225, 162, FG, nm, CR_DEFAULT, VPT_STRETCH);
        }
        if (plyr->pieces & WPIECE3)
        {
          sprintf(nm, "WPIECE%c3", cls[0]);
          if (W_CheckNumForName(nm) >= 0)
            V_DrawNamePatch(234, 162, FG, nm, CR_DEFAULT, VPT_STRETCH);
        }
      }
    }
  }
}

void ST_Drawer(dbool statusbaron, dbool refresh, dbool fullmenu)
{
  /* cph - let status bar on be controlled
   * completely by the call from D_Display
   * proff - really do it
   */
  st_firsttime = st_firsttime || refresh || fullmenu;

  ST_doPaletteStuff();  // Do red-/gold-shifts from damage/items

  /* Hexen has its own status bar (its own lumps); draw it and return before
   * the Doom bar code, which has no Hexen graphics. */
  if (hexen)
  {
    if (statusbaron)
      ST_HexenDrawer();
    return;
  }

  /* The Doom status bar widgets/background are not loaded for Heretic
   * (see ST_loadGraphics). Draw the Heretic bar instead (its own lumps),
   * then return before the Doom bar code. */
  if (heretic)
  {
    if (statusbaron)
      ST_HereticDrawer();
    else
      ST_HereticFullscreenDrawer();
    return;
  }

  if (statusbaron) {
    if (st_firsttime)
    {
      /* If just after ST_Start(), refresh all */
      st_firsttime = FALSE;
      ST_refreshBackground(); // draw status bar background to off-screen buff
      if (!fullmenu)
        ST_drawWidgets(TRUE); // and refresh all widgets
    }
    else
    {
      /* Otherwise, update as little as possible */
      if (!fullmenu)
        ST_drawWidgets(FALSE); // update all widgets
    }
  }
}



//
// ST_loadGraphics
//
// CPhipps - Loads graphics needed for status bar if doload is TRUE,
//  unloads them otherwise
//
static void ST_loadGraphics(dbool doload)
{
  unsigned short i, facenum;
  char namebuf[9];
  // cph - macro that either acquires a pointer and lock for a lump, or
  // unlocks it. var is referenced exactly once in either case, so ++ in arg works

  /* Heretic and Hexen have no Doom status bar lumps (STBAR, STARMS, STF*
   * faces, STTNUM, ...). Requesting them would abort on the missing lump.
   * Both games draw their own status bar separately; skip the Doom graphics. */
  if (raven)
    return;

  // Load the numbers, tall and short
  for (i=0;i<10;i++)
    {
      sprintf(namebuf, "STTNUM%d", i);
      R_SetPatchNum(&tallnum[i],namebuf);
      sprintf(namebuf, "STYSNUM%d", i);
      R_SetPatchNum(&shortnum[i],namebuf);
    }

  // Load percent key.
  R_SetPatchNum(&tallpercent,"STTPRCNT");

  // key cards
  for (i=0;i<NUMCARDS;i++)
    {
      sprintf(namebuf, "STKEYS%d", i);
      R_SetPatchNum(&keys[i], namebuf);
    }
  // if there are graphics for the combined keycard+skullkeys, use them
  // otherwise fallback to skullkeys like in vanilla Doom.
  for (i=0;i<3;i++)
  {
    sprintf(namebuf, "STKEYS%d", NUMCARDS+i);
    if (W_CheckNumForName(namebuf) != -1)
      R_SetPatchNum(&keys[NUMCARDS+i], namebuf);
    else
      keys[NUMCARDS+i] = keys[3+i];
  }

  //e6y: status bar background
  R_SetPatchNum(&stbarbg, "STBAR");

  // arms background
  R_SetPatchNum(&armsbg, "STARMS");

  // arms ownership widgets
  for (i=0;i<6;i++)
    {
      sprintf(namebuf, "STGNUM%d", i+2);

      // gray #
      R_SetPatchNum(&arms[i][0], namebuf);

      // yellow #
      arms[i][1] = shortnum[i+2];
    }

  // face backgrounds for different color players
  // killough 3/7/98: add better support for spy mode by loading all
  // player face backgrounds and using displayplayer to choose them:
  R_SetPatchNum(&faceback, "STFB0");

  // face states
  facenum = 0;
  for (i=0;i<ST_NUMPAINFACES;i++)
    {
      int j;
      for (j=0;j<ST_NUMSTRAIGHTFACES;j++)
        {
          sprintf(namebuf, "STFST%d%d", i, j);
          R_SetPatchNum(&faces[facenum++], namebuf);
        }
      sprintf(namebuf, "STFTR%d0", i);        // turn right
      R_SetPatchNum(&faces[facenum++], namebuf);
      sprintf(namebuf, "STFTL%d0", i);        // turn left
      R_SetPatchNum(&faces[facenum++], namebuf);
      sprintf(namebuf, "STFOUCH%d", i);       // ouch!
      R_SetPatchNum(&faces[facenum++], namebuf);
      sprintf(namebuf, "STFEVL%d", i);        // evil grin ;)
      R_SetPatchNum(&faces[facenum++], namebuf);
      sprintf(namebuf, "STFKILL%d", i);       // pissed off
      R_SetPatchNum(&faces[facenum++], namebuf);
    }
  R_SetPatchNum(&faces[facenum++], "STFGOD0");
  R_SetPatchNum(&faces[facenum++], "STFDEAD0");
}

static void ST_loadData(void)
{
  ST_loadGraphics(TRUE);
}

static void ST_initData(void)
{
  int i;

  st_firsttime = TRUE;
  plyr = &players[displayplayer];            // killough 3/7/98

  /* Seed the Heretic health-chain marker so the gem starts at the current
   * health rather than gliding up from empty on every level load. */
  HealthMarker = plyr->health;
  ChainWiggle  = 0;

  st_clock = 0;
  st_chatstate = StartChatState;
  st_gamestate = FirstPersonState;

  st_statusbaron = TRUE;
  st_oldchat = st_chat = FALSE;
  st_cursoron = FALSE;

  st_faceindex = 0;
  st_palette = -1;

  st_oldhealth = -1;

  for (i=0;i<NUMWEAPONS;i++)
    oldweaponsowned[i] = plyr->weaponowned[i];

  for (i=0;i<3;i++)
    keyboxes[i] = -1;

  STlib_init();
}

static void ST_createWidgets(void)
{
  int i;

  // ready weapon ammo
  STlib_initNum(&w_ready,
                ST_AMMOX,
                ST_AMMOY,
                tallnum,
                &plyr->ammo[weaponinfo[plyr->readyweapon].ammo],
                &st_statusbaron,
                ST_AMMOWIDTH );

  // the last weapon type
  w_ready.data = plyr->readyweapon;

  // health percentage
  STlib_initPercent(&w_health,
                    ST_HEALTHX,
                    ST_HEALTHY,
                    tallnum,
                    &plyr->health,
                    &st_statusbaron,
                    &tallpercent);

  // arms background
  STlib_initBinIcon(&w_armsbg,
                    ST_ARMSBGX,
                    ST_ARMSBGY,
                    &armsbg,
                    &st_notdeathmatch,
                    &st_statusbaron);

  // weapons owned
  for(i=0;i<6;i++)
    {
      STlib_initMultIcon(&w_arms[i],
                         ST_ARMSX+(i%3)*ST_ARMSXSPACE,
                         ST_ARMSY+(i/3)*ST_ARMSYSPACE,
                         arms[i], (int *) &plyr->weaponowned[i+1],
                         &st_armson);
    }

  // frags sum
  STlib_initNum(&w_frags,
                ST_FRAGSX,
                ST_FRAGSY,
                tallnum,
                &st_fragscount,
                &st_fragson,
                ST_FRAGSWIDTH);

  // faces
  STlib_initMultIcon(&w_faces,
                     ST_FACESX,
                     ST_FACESY,
                     faces,
                     &st_faceindex,
                     &st_statusbaron);

  // armor percentage - should be colored later
  STlib_initPercent(&w_armor,
                    ST_ARMORX,
                    ST_ARMORY,
                    tallnum,
                    &plyr->armorpoints,
                    &st_statusbaron, &tallpercent);

  // keyboxes 0-2
  STlib_initMultIcon(&w_keyboxes[0],
                     ST_KEY0X,
                     ST_KEY0Y,
                     keys,
                     &keyboxes[0],
                     &st_statusbaron);

  STlib_initMultIcon(&w_keyboxes[1],
                     ST_KEY1X,
                     ST_KEY1Y,
                     keys,
                     &keyboxes[1],
                     &st_statusbaron);

  STlib_initMultIcon(&w_keyboxes[2],
                     ST_KEY2X,
                     ST_KEY2Y,
                     keys,
                     &keyboxes[2],
                     &st_statusbaron);

  // ammo count (all four kinds)
  STlib_initNum(&w_ammo[0],
                ST_AMMO0X,
                ST_AMMO0Y,
                shortnum,
                &plyr->ammo[0],
                &st_statusbaron,
                ST_AMMO0WIDTH);

  STlib_initNum(&w_ammo[1],
                ST_AMMO1X,
                ST_AMMO1Y,
                shortnum,
                &plyr->ammo[1],
                &st_statusbaron,
                ST_AMMO1WIDTH);

  STlib_initNum(&w_ammo[2],
                ST_AMMO2X,
                ST_AMMO2Y,
                shortnum,
                &plyr->ammo[2],
                &st_statusbaron,
                ST_AMMO2WIDTH);

  STlib_initNum(&w_ammo[3],
                ST_AMMO3X,
                ST_AMMO3Y,
                shortnum,
                &plyr->ammo[3],
                &st_statusbaron,
                ST_AMMO3WIDTH);

  // max ammo count (all four kinds)
  STlib_initNum(&w_maxammo[0],
                ST_MAXAMMO0X,
                ST_MAXAMMO0Y,
                shortnum,
                &plyr->maxammo[0],
                &st_statusbaron,
                ST_MAXAMMO0WIDTH);

  STlib_initNum(&w_maxammo[1],
                ST_MAXAMMO1X,
                ST_MAXAMMO1Y,
                shortnum,
                &plyr->maxammo[1],
                &st_statusbaron,
                ST_MAXAMMO1WIDTH);

  STlib_initNum(&w_maxammo[2],
                ST_MAXAMMO2X,
                ST_MAXAMMO2Y,
                shortnum,
                &plyr->maxammo[2],
                &st_statusbaron,
                ST_MAXAMMO2WIDTH);

  STlib_initNum(&w_maxammo[3],
                ST_MAXAMMO3X,
                ST_MAXAMMO3Y,
                shortnum,
                &plyr->maxammo[3],
                &st_statusbaron,
                ST_MAXAMMO3WIDTH);
}

static dbool st_stopped = TRUE;

void ST_Start(void)
{
  if (!st_stopped)
    ST_Stop();
  ST_initData();
  ST_createWidgets();
  st_stopped = FALSE;
}

static void ST_Stop(void)
{
  if (st_stopped)
    return;
  V_SetPalette(0);
  st_stopped = TRUE;
}

/*
====================
=
= ST_Init
=
= Locate and load all needed graphics
====================
*/
void ST_Init(void)
{
  ST_loadData();
}
