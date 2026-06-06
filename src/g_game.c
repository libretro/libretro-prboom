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
 * DESCRIPTION:  none
 *  The original Doom description was none, basically because this file
 *  has everything. This ties up the game logic, linking the menu and
 *  input code to the underlying game by creating & respawning players,
 *  building game tics, calling the underlying thing logic.
 *
 *-----------------------------------------------------------------------------
 */

#include <stdarg.h>
#include <stdlib.h>
#ifdef _MSC_VER
#define    F_OK    0    /* Check for file existence */
#define    W_OK    2    /* Check for write permission */
#define    R_OK    4    /* Check for read permission */
#include <io.h>
#include <compat/msvc.h>
#endif

#include <boolean.h>

#include "config.h"

#include "doomstat.h"
#include "d_net.h"
#include "f_finale.h"
#include "m_argv.h"
#include "m_misc.h"
#include "m_menu.h"
#include "m_random.h"
#include "p_setup.h"
#include "p_saveg.h"
#include "p_tick.h"
#include "p_map.h"
#include "p_user.h"
#include "d_main.h"
#include "wi_stuff.h"
#include "hu_stuff.h"
#include "st_stuff.h"
#include "am_map.h"
#include "w_wad.h"
#include "r_main.h"
#include "r_draw.h"
#include "p_map.h"
#include "s_sound.h"
#include "dstrings.h"
#include "sounds.h"
#include "r_data.h"
#include "r_sky.h"
#include "hexen/p_mapinfo.h"
#include "hexen/sv_save.h"
#include "hexen/p_acs.h"
#include "d_deh.h"              // Ty 3/27/98 deh declarations
#include "p_inter.h"
#include "g_game.h"
#include "dsda_hacked.h"
#include "lprintf.h"
#include "i_main.h"
#include "i_system.h"
#include "r_demo.h"
#include "r_fps.h"

#define SAVEGAMESIZE  0x20000
#define SAVESTRINGSIZE  24

static size_t   savegamesize = SAVEGAMESIZE; // killough
static dbool    netdemo;
static const uint8_t *demobuffer;   /* cph - only used for playback */
static int demolength; // check for overrun (missing DEMOMARKER)
#if 0
static FILE    *demofp; /* cph - record straight to file */
#endif

static const uint8_t *demo_p;
static short    consistancy[MAXPLAYERS][BACKUPTICS];

gameaction_t    gameaction;
gamestate_t     gamestate;
skill_t         gameskill;
dbool           respawnmonsters;
int             gameepisode;
int             gamemap;

/* Hexen hub travel: the player start position the next map is entered at
 * (args[0] of the matching player start), and the pending Teleport_NewMap
 * destination staged by G_Completed.  A LeaveMap of -1 means
 * Teleport_EndGame. */
int RebornPosition;
static int LeaveMap;
static int LeavePosition;
mapentry_t*     gamemapinfo;
dbool           paused;
// CPhipps - moved *_loadgame vars here
static dbool   forced_loadgame = FALSE;
static dbool   command_loadgame = FALSE;

dbool           usergame;      // ok to save / end game
dbool           deathmatch;    // only if started as net death
dbool           netgame;       // only TRUE if packets are broadcast
dbool           playeringame[MAXPLAYERS];
player_t        players[MAXPLAYERS];
/* Hexen: per-player chosen class (PCLASS_*); PCLASS_NULL for Doom/Heretic.
 * Copied into players[].class at spawn. */
pclass_t        PlayerClass[MAXPLAYERS];
int             consoleplayer; // player taking events and displaying
int             displayplayer; // view being displayed
int             gametic;
int             basetic;       /* killough 9/29/98: for demo sync */
int             totalkills, totallive, totalitems, totalsecret;    // for intermission
dbool           demoplayback;
int             demover;
wbstartstruct_t wminfo;               // parms for world map / intermission
dbool           haswolflevels = FALSE;// jff 4/18/98 wolf levels present
static uint8_t     *savebuffer;          // CPhipps - static
int             autorun = FALSE;      // always running?          // phares
int             totalleveltimes;      // CPhipps - total time for all completed levels
int		longtics;

//
// controls (have defaults)
//

int     key_right;
int     key_left;
int     key_up;
int     key_down;
int     key_menu_right;                                      // phares 3/7/98
int     key_menu_left;                                       //     |
int     key_menu_up;                                         //     V
int     key_menu_down;
int     key_menu_backspace;                                  //     ^
int     key_menu_escape;                                     //     |
int     key_menu_enter;                                      // phares 3/7/98
int     key_strafeleft;
int     key_straferight;
int     key_fire;
int     key_use;
int     key_use_artifact;
int     pending_artifact = 0;  /* Heretic: artifact staged for use this tic */
int     inventory = 0;         /* Heretic: inventory bar currently displayed */
int     inventoryTics = 0;     /* Heretic: tics until the bar auto-closes */
int     key_inv_left;
int     key_inv_right;
int     key_fly_up;
int     key_fly_down;
int     key_jump;
int     key_strafe;
int     key_speed;
int     key_escape = KEYD_ESCAPE;                           // phares 4/13/98
int     key_savegame;                                               // phares
int     key_loadgame;                                               //    |
int     key_autorun;                                                //    V
int     key_reverse;
int     key_zoomin;
int     key_zoomout;
int     key_chat;
int     key_backspace;
int     key_enter;
int     key_map_right;
int     key_map_left;
int     key_map_up;
int     key_map_down;
int     key_map_zoomin;
int     key_map_zoomout;
int     key_map;
int     key_map_gobig;
int     key_map_follow;
int     key_map_mark;
int     key_map_clear;
int     key_map_grid;
int     key_map_overlay; // cph - map overlay
int     key_map_rotate;  // cph - map rotation
int     key_help = KEYD_F1;                                 // phares 4/13/98
int     key_soundvolume;
int     key_hud;
int     key_quicksave;
int     key_endgame;
int     key_messages;
int     key_quickload;
int     key_quit;
int     key_gamma;
int     key_spy;
int     key_pause;
int     key_setup;
int     destination_keys[MAXPLAYERS];
int     key_weapontoggle;
int     key_weaponcycleup;
int     key_weaponcycledown;
int     key_weapon1;
int     key_weapon2;
int     key_weapon3;
int     key_weapon4;
int     key_weapon5;
int     key_weapon6;
int     key_weapon7;                                                //    ^
int     key_weapon8;                                                //    |
int     key_weapon9;                                                // phares

int     key_screenshot;             // killough 2/22/98: screenshot key
int     mousebfire;
int     mousebstrafe;
int     mousebforward;
int     mousebbackward;
int     mlooky;
/* Heretic keyboard-look: signed per-tic look step staged by the gamepad
 * right stick (when mouselook is off) and consumed into cmd->lookfly in
 * G_BuildTiccmd.  Negative = look down, positive = look up. */
int     gamepad_lookdelta;

#define MAXPLMOVE   (forwardmove[1])
#define TURBOTHRESHOLD  0x32
#define SLOWTURNTICS  6
#define QUICKREVERSE (short)32768 // 180 degree reverse                    // phares
#define NUMKEYS   512

fixed_t forwardmove[2] = {0x19, 0x32};
fixed_t sidemove[2]    = {0x18, 0x28};
fixed_t angleturn[3]   = {640, 1280, 320};  // + slow turn

// CPhipps - made lots of key/button state vars static
dbool   gamekeydown[NUMKEYS];
int     turnheld;       // for accelerative turning

static dbool   mousearray[4];
static dbool   *mousebuttons = &mousearray[1];    // allow [-1]

// mouse values are used once
int lowlatency_turning; /* config: apply pending mouse turn to the view
                         * every rendered frame (m_misc.c, general menu) */
static int   mousex;
static int   mousey;

// Game events info
static buttoncode_t special_event; // Event triggered by local player, to send
static uint8_t  savegameslot;         // Slot to load if gameaction == ga_loadgame
char         savedescription[SAVEDESCLEN];  // Description to save in savegame if gameaction == ga_savegame

//jff 3/24/98 define defaultskill here
int defaultskill;               //note 1-based

// killough 2/8/98: make corpse queue variable in size
int    bodyqueslot, bodyquesize;        // killough 2/8/98
mobj_t **bodyque = 0;                   // phares 8/10/98

static void G_DoSaveGame (dbool   menu);
static const uint8_t* G_ReadDemoHeader(const uint8_t* demo_p, size_t size, dbool   failonerror);
static mapentry_t *G_LookupMapinfo(int episode, int map);

//
// G_BuildTiccmd
// Builds a ticcmd from all of the available inputs
// or reads it from the demo buffer.
// If recording a demo, write it out
//
static INLINE signed char fudgef(signed char b)
{
  static int c;
  if (!b || !demo_compatibility || longtics) return b;
  if (++c & 0x1f) return b;
  b |= 1; if (b>2) b-=2;
  return b;
}

static INLINE signed short fudgea(signed short b)
{
  if (!b || !demo_compatibility || !longtics) return b;
  b |= 1; if (b>2) b-=2;
  return b;
}


void G_BuildTiccmd(ticcmd_t* cmd)
{
  dbool   strafe;
  int speed;
  int tspeed;
  int forward;
  int side;
  int newweapon   = WP_NOCHANGE;
  dbool   bstrafe = FALSE;
  /* cphipps - remove needless I_BaseTiccmd call, just set the ticcmd to zero */
  memset(cmd,0,sizeof*cmd);
  cmd->consistancy = consistancy[consoleplayer][maketic%BACKUPTICS];

  (void)bstrafe;

  strafe = gamekeydown[key_strafe] || mousebuttons[mousebstrafe];
  //e6y: the "RUN" key inverts the autorun state
  speed = (gamekeydown[key_speed] ? !autorun : autorun); // phares

  forward = side = 0;

    // use two stage accelerative turning
    // on the keyboard
  if (gamekeydown[key_right] || gamekeydown[key_left])
    turnheld += ticdup;
  else
    turnheld = 0;

  if (turnheld < SLOWTURNTICS)
    tspeed = 2;             // slow turn
  else
    tspeed = speed;

  // turn 180 degrees in one keystroke?                           // phares
                                                                  //    |
  if (gamekeydown[key_reverse])                                   //    V
    {
      cmd->angleturn += QUICKREVERSE;                             //    ^
      gamekeydown[key_reverse] = FALSE;                           //    |
    }                                                             // phares

  // let movement keys cancel each other out

  if (strafe)
    {
      if (gamekeydown[key_right])
        side += sidemove[speed];
      if (gamekeydown[key_left])
        side -= sidemove[speed];
    }
  else
    {
      if (gamekeydown[key_right])
        cmd->angleturn -= angleturn[tspeed];
      if (gamekeydown[key_left])
        cmd->angleturn += angleturn[tspeed];
    }

  if (gamekeydown[key_up])
    forward += forwardmove[speed];
  if (gamekeydown[key_down])
    forward -= forwardmove[speed];
  if (gamekeydown[key_straferight])
    side += sidemove[speed];
  if (gamekeydown[key_strafeleft])
    side -= sidemove[speed];

    // buttons
  cmd->chatchar = HU_dequeueChatChar();

  if (gamekeydown[key_fire] || mousebuttons[mousebfire])
    cmd->buttons |= BT_ATTACK;

  /* Use / open door.  In Hexen the spacebar is the jump key, so the use
   * action there comes from E (and the mouse); Heretic and Doom keep the
   * configured use key (spacebar by default). */
  if (mousebuttons[mousebforward] ||
      (raven && gamekeydown['e']) ||
      (!hexen && gamekeydown[key_use]))
    {
      cmd->buttons |= BT_USE;
    }

  /* Hexen jump: stage the request in the arti byte's top bit; P_MovePlayer
   * turns it into upward momentum when the player is on the ground. */
  if (hexen && gamekeydown[key_jump])
    cmd->arti |= AFLAG_JUMP;

  /* Heretic/Hexen inventory input: cycle the ready artifact with the inv keys
   * and use it with the use-artifact key. inv_ptr/curpos and readyArtifact
   * live in p_user.c; we just nudge the cursor and stage the artifact to use. */
  if (raven)
  {
    extern int inv_ptr, curpos;

    if (gamekeydown[key_inv_right])
    {
      gamekeydown[key_inv_right] = FALSE;
      if (players[consoleplayer].inventorySlotNum > 0)
      {
        inventoryTics = 5 * 35;        /* keep the bar up for 5s */
        if (!inventory)
          inventory = TRUE;            /* first press just opens the bar */
        else
        {
          inv_ptr++;
          if (inv_ptr >= players[consoleplayer].inventorySlotNum)
            inv_ptr = players[consoleplayer].inventorySlotNum - 1;
          else if (++curpos > 6)
            curpos = 6;
        }
        players[consoleplayer].readyArtifact =
          players[consoleplayer].inventory[inv_ptr].type;
      }
    }
    if (gamekeydown[key_inv_left])
    {
      gamekeydown[key_inv_left] = FALSE;
      if (players[consoleplayer].inventorySlotNum > 0)
      {
        inventoryTics = 5 * 35;
        if (!inventory)
          inventory = TRUE;
        else
        {
          inv_ptr--;
          if (inv_ptr < 0)
            inv_ptr = 0;
          else if (--curpos < 0)
            curpos = 0;
        }
        players[consoleplayer].readyArtifact =
          players[consoleplayer].inventory[inv_ptr].type;
      }
    }
    if (gamekeydown[key_use_artifact])
    {
      gamekeydown[key_use_artifact] = FALSE;
      /* If the inventory bar is open, the use key just closes it (selecting
       * the highlighted artifact as ready) rather than using immediately --
       * matching Heretic. Otherwise it uses the ready artifact. */
      if (inventory)
      {
        inventory = FALSE;
        players[consoleplayer].readyArtifact =
          players[consoleplayer].inventory[inv_ptr].type;
      }
      else
      {
        /* Single-player: route the artifact-use request through a dedicated
         * pending global rather than the ticcmd, whose ring-buffer slots are
         * reused across tics and would drop or duplicate the request. The
         * ticcmd arti field stays reserved for a future netgame/demo path. */
        pending_artifact = players[consoleplayer].readyArtifact;
      }
    }
  }

  // Toggle between the top 2 favorite weapons.                   // phares
  // If not currently aiming one of these, switch to              // phares
  // the favorite. Only switch if you possess the weapon.         // phares

  // killough 3/22/98:
  //
  // Perform automatic weapons switch here rather than in p_pspr.c,
  // except in demo_compatibility mode.
  //
  // killough 3/26/98, 4/2/98: fix autoswitch when no weapons are left

  if ((!demo_compatibility && players[consoleplayer].attackdown && // killough
       !P_CheckAmmo(&players[consoleplayer])) || gamekeydown[key_weapontoggle])
  {
	  newweapon = P_SwitchWeapon(&players[consoleplayer]);           // phares
  }
  else if (gamekeydown[key_weaponcycleup] || gamekeydown[key_weaponcycledown])
  {
	  static weapontype_t weapons_list[] = {WP_FIST, WP_CHAINSAW, WP_PISTOL, WP_SHOTGUN, WP_SUPERSHOTGUN, WP_CHAINGUN, WP_MISSILE, WP_PLASMA, WP_BFG};
	  int i;
	  weapontype_t origweapon = players[consoleplayer].readyweapon;
	  int oldweapon = 0;

	  for(i = 0; i != 9; i ++)
	  {
		  if(weapons_list[i] == players[consoleplayer].readyweapon)
		  {
			  oldweapon = i;
			  break;
		  }
	  }

	  for(i = 0; i != 9; i ++)
	  {
		  if(gamekeydown[key_weaponcycleup])
			  oldweapon ++;
		  else
			  oldweapon --;
		  if(oldweapon < 0)
			  oldweapon = 8;
		  if(oldweapon > 8)
			  oldweapon = 0;

		  // I SHOULD just write a P_CheckAmmo that takes the weapon as an argument... This will die if multithreaded...
		  players[consoleplayer].readyweapon = weapons_list[oldweapon];

		  if(players[consoleplayer].weaponowned[players[consoleplayer].readyweapon] && P_CheckAmmo(&players[consoleplayer]))
		  {
			  newweapon = players[consoleplayer].readyweapon;
			  break;
		  }
	  }
	  players[consoleplayer].readyweapon = origweapon;
  }
  else
  {                                 // phares 02/26/98: Added gamemode checks
	  newweapon =
		  gamekeydown[key_weapon1] ? WP_FIST :    // killough 5/2/98: reformatted
		  gamekeydown[key_weapon2] ? WP_PISTOL :
		  gamekeydown[key_weapon3] ? WP_SHOTGUN :
		  gamekeydown[key_weapon4] ? WP_CHAINGUN :
		  gamekeydown[key_weapon5] ? WP_MISSILE :
		  gamekeydown[key_weapon6] && gamemode != shareware ? WP_PLASMA :
		  gamekeydown[key_weapon7] && gamemode != shareware ? WP_BFG :
		  gamekeydown[key_weapon8] ? WP_CHAINSAW :
		  (!demo_compatibility && gamekeydown[key_weapon9] && gamemode == commercial) ? WP_SUPERSHOTGUN :
		  WP_NOCHANGE;

	  // killough 3/22/98: For network and demo consistency with the
	  // new weapons preferences, we must do the weapons switches here
	  // instead of in p_user.c. But for old demos we must do it in
	  // p_user.c according to the old rules. Therefore demo_compatibility
	  // determines where the weapons switch is made.

	  // killough 2/8/98:
	  // Allow user to switch to fist even if they have chainsaw.
	  // Switch to fist or chainsaw based on preferences.
	  // Switch to shotgun or SSG based on preferences.

	  if (!demo_compatibility)
	  {
		  const player_t *player = &players[consoleplayer];

		  // only select chainsaw from '1' if it's owned, it's
		  // not already in use, and the player prefers it or
		  // the fist is already in use, or the player does not
		  // have the berserker strength.

		  if (newweapon==WP_FIST && player->weaponowned[WP_CHAINSAW] &&
				  player->readyweapon!=WP_CHAINSAW &&
				  (player->readyweapon==WP_FIST ||
				   !player->powers[pw_strength] ||
				   P_WeaponPreferred(WP_CHAINSAW, WP_FIST)))
			  newweapon = WP_CHAINSAW;

		  // Select SSG from '3' only if it's owned and the player
		  // does not have a shotgun, or if the shotgun is already
		  // in use, or if the SSG is not already in use and the
		  // player prefers it.

		  if (newweapon == WP_SHOTGUN && gamemode == commercial &&
				  player->weaponowned[WP_SUPERSHOTGUN] &&
				  (!player->weaponowned[WP_SHOTGUN] ||
				   player->readyweapon == WP_SHOTGUN ||
				   (player->readyweapon != WP_SUPERSHOTGUN &&
				    P_WeaponPreferred(WP_SUPERSHOTGUN, WP_SHOTGUN))))
			  newweapon = WP_SUPERSHOTGUN;
	  }
	  // killough 2/8/98, 3/22/98 -- end of weapon selection changes
  }

  if (newweapon != WP_NOCHANGE)
    {
      cmd->buttons |= BT_CHANGE;
      cmd->buttons |= newweapon<<BT_WEAPONSHIFT;
    }

  // mouse
  if (mousebuttons[mousebbackward])
    forward -= forwardmove[speed];

  bstrafe = mousebuttons[mousebstrafe];
  if (strafe)
    side += mousex / 4;       /* mead  Don't want to strafe as fast as turns.*/
  else
    cmd->angleturn -= mousex; /* mead now have enough dynamic range 2-10-00 */

  // if mouselook enabled, set pitch without affecting the tic cmds
  if (movement_mouselook) {
     if (movement_mouseinvert)
        mlooky -= mousey;
     else
        mlooky += mousey;
  }
  else if (raven && gamepad_lookdelta)
  {
     /* Heretic keyboard look without mouselook: encode the staged look step
      * into the low nibble of cmd->lookfly (signed -8..+7), which
      * P_MovePlayer decodes into player->lookdir.  Clamp to the encodable
      * range and mask to the nibble so negative steps wrap correctly. */
     int look = gamepad_lookdelta;
     if (look > 7)
        look = 7;
     else if (look < -7)
        look = -7;
     cmd->lookfly = (uint8_t)((cmd->lookfly & 0xF0) | (look & 0x0F));
     gamepad_lookdelta = 0;
  }

  /* Heretic/Hexen flight: the fly up/down keys stage a vertical step into the
   * high nibble of cmd->lookfly (signed -8..+7), which P_MovePlayer applies to
   * player->flyheight while the flight power is active. */
  if (raven && (gamekeydown[key_fly_up] || gamekeydown[key_fly_down]))
  {
     int fly = 0;
     if (gamekeydown[key_fly_up])
        fly = 7;
     else if (gamekeydown[key_fly_down])
        fly = -7;
     cmd->lookfly = (uint8_t)((cmd->lookfly & 0x0F) | ((fly & 0x0F) << 4));
  }

  mousex = mousey = 0;

  if (forward > MAXPLMOVE)
    forward = MAXPLMOVE;
  else if (forward < -MAXPLMOVE)
    forward = -MAXPLMOVE;
  if (side > MAXPLMOVE)
    side = MAXPLMOVE;
  else if (side < -MAXPLMOVE)
    side = -MAXPLMOVE;

  cmd->forwardmove += fudgef((signed char)forward);
  cmd->sidemove += side;
  cmd->angleturn = fudgea(cmd->angleturn);

  // CPhipps - special events (game new/load/save/pause)
  if (special_event & BT_SPECIAL) {
    cmd->buttons = special_event;
    special_event = 0;
  }
}

/* Mouse turn accumulated since the last ticcmd, as a view angle delta.
 * R_SetupFrame adds this to the rendered view angle every frame, so the
 * camera answers the mouse at frame rate while the simulation still
 * consumes the identical counts at the next tic: G_BuildTiccmd folds
 * mousex into cmd->angleturn (a short, so it wraps exactly like the
 * angle's top sixteen bits) and zeroes the accumulator, at which point
 * the preview returns to zero in the same frame.  The ticcmd is never
 * altered, so demos record byte-identical with the feature on or off.
 *
 * Inert while strafing (the same condition under which G_BuildTiccmd
 * turns mousex into sidemove), during demo playback (mouse events are
 * not driving the player), and outside levels. */
dbool G_PendingTurnActive(void)
{
  return lowlatency_turning && !demoplayback && gamestate == GS_LEVEL &&
         !menuactive && !paused;
}

angle_t G_PendingTurn(void)
{
  int D_PendingLocalTurn(void);

  if (!G_PendingTurnActive())
    return 0;
  return (angle_t) D_PendingLocalTurn() << 16;
}

/* The freelook analogue of G_PendingTurn.  mlooky is the look input
 * G_BuildTiccmd has accumulated since P_SetPitch last consumed it, so
 * the pitch the next tic will commit is simply the latest pitch plus
 * that backlog, clamped as the tic will clamp it.
 *
 * The preview is only valid for pitch that mlooky itself drives, which
 * is why the caller must check G_PendingPitchActive first: under the
 * Heretic/Hexen keyboard- and gamepad-look path (mouselook off) the
 * pitch moves per tic from lookdir with no backlog to anchor on, so
 * overriding the renderer's interpolated pitch there would replace a
 * smooth ramp with raw 35Hz steps.  The same applies during the
 * post-teleport reaction pause and under the full-screen automap,
 * where P_SetPitch leaves mlooky unconsumed. */
dbool G_PendingPitchActive(const mobj_t *mo)
{
  return movement_mouselook && !mo->reactiontime &&
         !((automapmode & am_active) && !(automapmode & am_overlay));
}

angle_t G_PendingPitch(const mobj_t *mo)
{
  angle_t pitch = mo->pitch;

  if (!G_PendingPitchActive(mo))
    return pitch;
  pitch += (angle_t) (mlooky << 16);
  P_CheckPitch(&pitch);
  return pitch;
}

//
// G_CheckNumForLevel
//
// Returns the level lump number if it exists, -1 otherwise
//
int G_CheckNumForLevel(int episode, int map)
{
  char mapname[9];
  if (gamemode == commercial)
    sprintf(mapname, "MAP%.2d", map);
  else
    sprintf(mapname, "E%dM%d", episode, map);

  return W_CheckNumForName(mapname);
}

//
// G_RestartLevel
//

void G_RestartLevel(void)
{
  special_event = BT_SPECIAL | (BTS_RESTARTLEVEL & BT_SPECIALMASK);
}

#include "z_bmalloc.h"

/*
==============
=
= G_DoLoadLevel
=
==============
*/

static void G_DoLoadLevel (void)
{
  int i;

#if 0
  lprintf(LO_INFO, "------------------------------\n"
          "G_DoLoadLevel:  ===== Episode %d - Map %.2d =====\n",
          gameepisode, gamemap);
#endif

  /* Set the sky map for the episode.
   * First thing, we have a dummy sky texture name,
   *  a flat. The data is in the WAD only because
   *  we look for an actual index, instead of simply
   *  setting one.
   */
  /* Hexen's sky-flat lump is "F_SKY" (Doom and Heretic use "F_SKY1"); a
   * ceiling painted with the sky flat is what the renderer treats as open
   * sky, so getting this wrong leaves sky sectors drawing garbage. */
  skyflatnum = R_FlatNumForName ( hexen ? "F_SKY" : SKYFLATNAME );

  /* skytexture set through UMAPINFO */
  if (gamemapinfo && gamemapinfo->skytexture[0])
  {
    skytexture = R_TextureNumForName(gamemapinfo->skytexture);
  }
  /* Hexen picks the sky per map through its MAPINFO: a primary sky (Sky1),
   * an alternate sky (Sky2) used for the lightning flash and as the scrolling
   * backdrop when DoubleSky is set, and a horizontal scroll speed for each.
   * skytexture tracks Sky1 (the lightning code swaps it to Sky2 mid-flash). */
  else if (hexen)
  {
    Sky1Texture      = P_GetMapSky1Texture(gamemap);
    Sky2Texture      = P_GetMapSky2Texture(gamemap);
    Sky1ScrollDelta  = P_GetMapSky1ScrollDelta(gamemap);
    Sky2ScrollDelta  = P_GetMapSky2ScrollDelta(gamemap);
    Sky1ColumnOffset = 0;
    Sky2ColumnOffset = 0;
    DoubleSky        = P_GetMapDoubleSky(gamemap);
    skytexture       = Sky1Texture;
  }
  /* DOOM determines the sky texture to be used
   * depending on the current episode, and the game version.
   */
  else if (gamemode == commercial)
     // || gamemode == pack_tnt   //jff 3/27/98 sorry guys pack_tnt,pack_plut
     // || gamemode == pack_plut) //aren't gamemodes, this was matching retail
  {
    skytexture = R_TextureNumForName ("SKY3");
    if (gamemap < 12)
      skytexture = R_TextureNumForName ("SKY1");
    else
      if (gamemap < 21)
        skytexture = R_TextureNumForName ("SKY2");
  }
  else /* and lets not forget about DOOM, Ultimate DOOM, SIGIL & extra Eps */
  {
    // Each episode has its own sky, numbered after it
    char skyname[9];
    sprintf(skyname, "SKY%d", gameepisode);
    skytexture = R_CheckTextureNumForName(skyname);
    if (skytexture == -1)
      // default sky, in case of custom episodes with missing SKY
      skytexture = R_TextureNumForName ("SKY1");
  }

  /* cph 2006/07/31 - took out unused levelstarttic variable */

  if (!demo_compatibility && !mbf_features)   // killough 9/29/98
     basetic = gametic;

  if (wipegamestate == GS_LEVEL && (gameaction == ga_newgame || gameaction == ga_completed))
     wipegamestate = -1;             // force a wipe

  gamestate = GS_LEVEL;

  for (i=0 ; i<MAXPLAYERS ; i++)
  {
    if (playeringame[i] && players[i].playerstate == PST_DEAD)
      players[i].playerstate = PST_REBORN;
    memset (players[i].frags,0,sizeof(players[i].frags));
  }

  // initialize the msecnode_t freelist.                     phares 3/25/98
  // any nodes in the freelist are gone by now, cleared
  // by Z_FreeTags() when the previous level ended or player
  // died.

  {
    DECLARE_BLOCK_MEMORY_ALLOC_ZONE(secnodezone);
    NULL_BLOCK_MEMORY_ALLOC_ZONE(secnodezone);
    //extern msecnode_t *headsecnode; // phares 3/25/98
    //headsecnode = NULL;
  }

  P_SetupLevel (gameepisode, gamemap, 0, gameskill);

  {
    /* P_SetupLevel could not build a usable level (e.g. a UDMF map whose
     * node format we cannot decode).  Don't run or render it -- drop back
     * to the demo/title screen so the core stays responsive instead of
     * ticking and rendering a level with no player mobj or BSP. */
    extern dbool level_setup_failed;
    if (level_setup_failed)
    {
      gamestate = GS_DEMOSCREEN;
      gameaction = ga_nothing;
      return;
    }
  }

  if (!demoplayback) /* Don't switch views if playing a demo */
    displayplayer = consoleplayer;    /* view the guy you are playing */
  gameaction = ga_nothing;

  /* clear cmd building stuff */
  memset (gamekeydown, 0, sizeof(gamekeydown));
  mousex = mousey = 0;
  mlooky = 0;
  special_event = 0; paused = FALSE;
  memset (mousebuttons, 0, sizeof(*mousebuttons));

  // killough 5/13/98: in case netdemo has consoleplayer other than green
  ST_Start();
  HU_Start();
}


//
// G_Responder
// Get info needed to make ticcmd_ts for the players.
//

dbool   G_Responder (event_t* ev)
{
  // allow spy mode changes even during the demo
  // killough 2/22/98: even during DM demo
  //
  // killough 11/98: don't autorepeat spy mode switch

  if (ev->data1 == key_spy && netgame && (demoplayback || !deathmatch) &&
      gamestate == GS_LEVEL)
    {
      if (ev->type == ev_keyup)
  gamekeydown[key_spy] = FALSE;
      if (ev->type == ev_keydown && !gamekeydown[key_spy])
  {
    gamekeydown[key_spy] = TRUE;
    do                                          // spy mode
      if (++displayplayer >= MAXPLAYERS)
        displayplayer = 0;
    while (!playeringame[displayplayer] && displayplayer!=consoleplayer);

    ST_Start();    // killough 3/7/98: switch status bar views too
    HU_Start();
    S_UpdateSounds(players[displayplayer].mo);

    R_ActivateSectorInterpolations();
    R_SmoothPlaying_Reset(NULL);
  }
      return TRUE;
    }

  // any other key pops up menu if in demos
  //
  // killough 8/2/98: enable automap in -timedemo demos
  //
  // killough 9/29/98: make any key pop up menu regardless of
  // which kind of demo, and allow other events during playback

  if (gameaction == ga_nothing && (demoplayback || gamestate == GS_DEMOSCREEN))
    {
      // killough 9/29/98: allow user to pause demos during playback
      if (ev->type == ev_keydown && ev->data1 == key_pause)
  {
    if (paused ^= 2)
      S_PauseSound();
    else
      S_ResumeSound();
    return TRUE;
  }

      // killough 10/98:
      // Don't pop up menu, if paused in middle
      // of demo playback, or if automap active.
      // Don't suck up keys, which may be cheats

      return gamestate == GS_DEMOSCREEN &&
  !(paused & 2) && !(automapmode & am_active) &&
  ((ev->type == ev_keydown) ||
   (ev->type == ev_mouse && ev->data1)) ?
  M_StartControlPanel(), TRUE : FALSE;
    }

  if (gamestate == GS_FINALE && F_Responder(ev))
    return TRUE;  // finale ate the event

  switch (ev->type)
    {
    case ev_keydown:
      if (ev->data1 == key_pause)           // phares
        {
          special_event = BT_SPECIAL | (BTS_PAUSE & BT_SPECIALMASK);
          return TRUE;
        }
      if (ev->data1 <NUMKEYS)
        gamekeydown[ev->data1] = TRUE;
      return TRUE;    // eat key down events

    case ev_keyup:
      if (ev->data1 <NUMKEYS)
        gamekeydown[ev->data1] = FALSE;
      return FALSE;   // always let key up events filter down

    case ev_mouse:
      mousebuttons[0] = ev->data1 & 1;
      mousebuttons[1] = ev->data1 & 2;
      mousebuttons[2] = ev->data1 & 4;
      /*
       * bmead@surfree.com
       * Modified by Barry Mead after adding vastly more resolution
       * to the Mouse Sensitivity Slider in the options menu 1-9-2000
       * Removed the mouseSensitivity "*4" to allow more low end
       * sensitivity resolution especially for lsdoom users.
       */
      mousex += (ev->data2*(mouseSensitivity_horiz))/10;  /* killough */
      mousey += (ev->data3*(mouseSensitivity_vert))/10;  /*Mead rm *4 */
      return TRUE;    // eat events
    default:
      break;
    }
  return FALSE;
}

//
// G_Ticker
// Make ticcmd_ts for the players.
//

void G_Ticker (void)
{
  int i;
  static gamestate_t prevgamestate;

  // CPhipps - player colour changing
  if (!demoplayback && mapcolor_plyr[consoleplayer] != mapcolor_me) {
    G_ChangedPlayerColour(consoleplayer, mapcolor_me);
  }
  P_MapStart();
  // do player reborns if needed
  for (i=0 ; i<MAXPLAYERS ; i++)
    if (playeringame[i] && players[i].playerstate == PST_REBORN)
      G_DoReborn (i);
  P_MapEnd();

  // do things to change the game state
  while (gameaction != ga_nothing)
    {
      switch (gameaction)
        {
        case ga_loadlevel:
    // force players to be initialized on level reload
    for (i=0 ; i<MAXPLAYERS ; i++)
      players[i].playerstate = PST_REBORN;
          G_DoLoadLevel ();
          break;
        case ga_newgame:
          G_DoNewGame ();
          break;
        case ga_loadgame:
          G_DoLoadGame ();
          break;
        case ga_savegame:
          G_DoSaveGame (FALSE);
          break;
        case ga_playdemo:
          G_DoPlayDemo ();
          break;
        case ga_completed:
          G_DoCompleted ();
          break;
        case ga_victory:
          F_StartFinale ();
          break;
        case ga_worlddone:
          G_DoWorldDone ();
          break;
        case ga_nothing:
          break;
        }
    }

  if (paused & 2 || (!demoplayback && menuactive && !netgame))
    basetic++;  // For revenant tracers and RNG -- we must maintain sync
  else {
    // get commands, check consistancy, and build new consistancy check
    int buf = (gametic/ticdup)%BACKUPTICS;

    for (i=0 ; i<MAXPLAYERS ; i++) {
      if (playeringame[i])
        {
          ticcmd_t *cmd = &players[i].cmd;

          memcpy(cmd, &netcmds[i][buf], sizeof *cmd);

          if (demoplayback)
            G_ReadDemoTiccmd (cmd);

          // check for turbo cheats
          // killough 2/14/98, 2/20/98 -- only warn in netgames and demos

          if ((netgame || demoplayback) && cmd->forwardmove > TURBOTHRESHOLD &&
              !(gametic&31) && ((gametic>>5)&3) == i )
            {
        extern char *player_names[];
        /* cph - don't use sprintf, use doom_printf */
              doom_printf ("%s is turbo!", player_names[i]);
            }

          if (netgame && !netdemo && !(gametic%ticdup) )
            {
              if (gametic > BACKUPTICS
                  && consistancy[i][buf] != cmd->consistancy)
                I_Error("G_Ticker: Consistency failure (%i should be %i)",
            cmd->consistancy, consistancy[i][buf]);
              if (players[i].mo)
                consistancy[i][buf] = players[i].mo->x;
              else
                consistancy[i][buf] = 0; // killough 2/14/98
            }
        }
    }

    // check for special buttons
    for (i=0; i<MAXPLAYERS; i++) {
      if (playeringame[i])
        {
          if (players[i].cmd.buttons & BT_SPECIAL)
            {
              switch (players[i].cmd.buttons & BT_SPECIALMASK)
                {
                case BTS_PAUSE:
                  paused ^= 1;
                  if (paused)
                    S_PauseSound ();
                  else
                    S_ResumeSound ();
                  break;

                case BTS_SAVEGAME:
                  if (!savedescription[0])
                    strcpy(savedescription, "NET GAME");
                  savegameslot =
                    (players[i].cmd.buttons & BTS_SAVEMASK)>>BTS_SAVESHIFT;
                  gameaction = ga_savegame;
                  break;

      // CPhipps - remote loadgame request
                case BTS_LOADGAME:
                  savegameslot =
                    (players[i].cmd.buttons & BTS_SAVEMASK)>>BTS_SAVESHIFT;
                  gameaction = ga_loadgame;
      forced_loadgame = netgame; // Force if a netgame
      command_loadgame = FALSE;
                  break;

      // CPhipps - Restart the level
    case BTS_RESTARTLEVEL:
                  if (demoplayback || (compatibility_level < lxdoom_1_compatibility))
                    break;     // CPhipps - Ignore in demos or old games
      gameaction = ga_loadlevel;
      break;
                }
        players[i].cmd.buttons = 0;
            }
        }
    }
  }

  // cph - if the gamestate changed, we may need to clean up the old gamestate
  if (gamestate != prevgamestate) {
    switch (prevgamestate) {
    case GS_LEVEL:
      // This causes crashes at level end - Neil Stevens
      // The crash is because the sounds aren't stopped before freeing them
      // the following is a possible fix
      // This fix does avoid the crash wowever, with this fix in, the exit
      // switch sound is cut off
      // S_Stop();
      // Z_FreeTags(PU_LEVEL, PU_PURGELEVEL-1);
      break;
    case GS_INTERMISSION:
      WI_End();
    default:
      break;
    }
    prevgamestate = gamestate;
  }

  // e6y
  // do nothing if a pause has been pressed during playback
  // pausing during intermission can cause desynchs without that
  if (paused & 2 && gamestate != GS_LEVEL)
    return;

  // do main actions
  switch (gamestate)
    {
    case GS_LEVEL:
      P_Ticker ();
      ST_Ticker ();
      AM_Ticker ();
      HU_Ticker ();
      break;

    case GS_INTERMISSION:
      WI_Ticker ();
      break;

    case GS_FINALE:
      F_Ticker ();
      break;

    case GS_DEMOSCREEN:
      D_PageTicker ();
      break;

    default:
      break;
    }
}

//
// PLAYER STRUCTURE FUNCTIONS
// also see P_SpawnPlayer in P_Things
//

/*
====================
=
= G_PlayerFinishLevel
=
= Can when a player completes a level
====================
*/

static void G_PlayerFinishLevel(int player)
{
  player_t *p = &players[player];

  /* Heretic end-of-level housekeeping (vanilla G_PlayerFinishLevel):
   * artifact stacks shrink to one of each, any Wings of Wrath are used up
   * (flight does not carry between maps), and a morphed player reverts --
   * the pre-morph weapon rides in the chicken mobj's special1.  The rain
   * trackers point at PU_LEVEL mobjs and must not dangle into the next
   * map.  The morph restore applies to hexen's pig as well. */
  if (heretic)
  {
    int i;

    for (i = 0; i < p->inventorySlotNum; i++)
      p->inventory[i].count = 1;
    p->artifactCount = p->inventorySlotNum;

    if (!deathmatch)
      for (i = 0; i < 16; i++)
        P_PlayerUseArtifact(p, arti_fly);
  }

  if (raven && (p->chickenTics || p->morphTics) && p->mo)
  {
    p->readyweapon = p->mo->special1.i;       /* restore weapon */
    p->chickenTics = 0;
    p->morphTics = 0;
  }

  if (raven)
  {
    p->lookdir = 0;
    p->rain1 = NULL;
    p->rain2 = NULL;
    p->poisoncount = 0;
  }

  /* Hexen keys and flight are hub-scoped: both survive Teleport_NewMap
   * within a cluster.  Keys are stripped when the destination lies in a
   * different cluster (or the game is ending), matching G_PlayerExitMap;
   * flight carries within a cluster, while leaving the cluster burns any
   * banked flight artifacts (up to 25) before stripping the power. */
  {
    dbool different_cluster = (!hexen || LeaveMap == -1 ||
        P_GetMapCluster(gamemap) != P_GetMapCluster(P_TranslateMapWarp(LeaveMap)));
    int flight_carryover = 0;

    if (hexen && !deathmatch)
    {
      if (!different_cluster)
        flight_carryover = p->powers[pw_flight];
      else
      {
        int i;
        for (i = 0; i < 25; i++)
        {
          p->powers[pw_flight] = 0;
          P_PlayerUseArtifact(p, hexen_arti_fly);
        }
      }
    }

    memset(p->powers, 0, sizeof p->powers);
    p->powers[pw_flight] = flight_carryover;

    if (different_cluster)
      memset(p->cards, 0, sizeof p->cards);

  }

  p->mo = NULL;           // cph - this is allocated PU_LEVEL so it's gone
  p->extralight = 0;      /* cancel gun flashes */
  p->fixedcolormap = 0;   /* cancel ir gogles */
  p->damagecount = 0;     /* no palette changes */
  p->bonuscount = 0;
}

/* The player start to use for (re)spawning: the RebornPosition start on
 * Hexen maps when present, position 0 otherwise. */
static mapthing_t *G_PlayerStart(int playernum)
{
  if (hexen && RebornPosition > 0 && RebornPosition < MAX_PLAYER_STARTS &&
      playerstarts[RebornPosition][playernum].options)
    return &playerstarts[RebornPosition][playernum];
  return &playerstarts[0][playernum];
}

// CPhipps - G_SetPlayerColour
// Player colours stuff
//
// G_SetPlayerColour

#include "r_draw.h"

void G_ChangedPlayerColour(int pn, int cl)
{
  int i;

  if (!netgame) return;

  mapcolor_plyr[pn] = cl;

  // Rebuild colour translation tables accordingly
  R_InitTranslationTables();
  // Change translations on existing player mobj's
  for (i=0; i<MAXPLAYERS; i++) {
    if ((gamestate == GS_LEVEL) && playeringame[i] && (players[i].mo != NULL)) {
      players[i].mo->flags &= ~MF_TRANSLATION;
      players[i].mo->flags |= playernumtotrans[i] << MF_TRANSSHIFT;
    }
  }
}

/*
====================
=
= G_PlayerReborn
=
= Called after a player dies
= almost everything is cleared and initialized
====================
*/

void G_PlayerReborn (int player)
{
  player_t *p;
  int i;
  int frags[MAXPLAYERS];
  int killcount;
  int itemcount;
  int secretcount;

  memcpy (frags, players[player].frags, sizeof frags);
  killcount = players[player].killcount;
  itemcount = players[player].itemcount;
  secretcount = players[player].secretcount;

  p = &players[player];

  // killough 3/10/98,3/21/98: preserve cheats across idclev
  {
    int cheats = p->cheats;
    memset (p, 0, sizeof(*p));
    p->cheats = cheats;
  }

  memcpy(players[player].frags, frags, sizeof(players[player].frags));
  players[player].killcount = killcount;
  players[player].itemcount = itemcount;
  players[player].secretcount = secretcount;

  p->usedown = p->attackdown = true;  // don't do anything immediately
  p->playerstate = PST_LIVE;
  p->health = initial_health;  // Ty 03/12/98 - use dehacked values

  if (hexen)
  {
    /* Hexen player: class comes from the chosen PlayerClass[]; the player
     * starts with the first weapon (fists/equivalent) of that class and an
     * empty mana pool (mana is picked up in the level).  The per-class
     * weapon identity is resolved through WeaponInfo[slot][class]. */
    int j;
    p->class = PlayerClass[player];
    p->readyweapon = p->pendingweapon = WP_FIRST;
    p->weaponowned[WP_FIRST] = true;
    p->maxmana = MAX_MANA;
    for (j = 0; j < NUMMANA; j++)
      p->mana[j] = 0;
    return;
  }

  p->readyweapon = p->pendingweapon = WP_PISTOL;
  p->weaponowned[WP_FIST] = true;
  p->weaponowned[WP_PISTOL] = true;

  /* Heretic reuses the WP_FIST / WP_PISTOL slots for the staff and gold
   * wand (see heretic_weaponinfo). The gold wand uses am_goldwand ammo and
   * the player starts with 50; Doom starts with AM_CLIP bullets. */
  if (heretic)
    p->ammo[am_goldwand] = 50;
  else
    p->ammo[AM_CLIP] = initial_bullets; // Ty 03/12/98 - use dehacked values

  for (i=0 ; i<NUMAMMO ; i++)
    p->maxammo[i] = heretic ? heretic_maxammo[i] : maxammo[i];
}

/*
====================
=
= G_CheckSpot
=
= Returns false if the player cannot be respawned at the given mapthing_t spot
= because something is occupying it
====================
*/

static dbool   G_CheckSpot(int playernum, mapthing_t *mthing)
{
  fixed_t     x,y;
  subsector_t *ss;
  int         i;

  if (!players[playernum].mo)
  {
     /* first spawn of level, before corpses */
     for (i=0 ; i<playernum ; i++)
        if (players[i].mo->x == mthing->x << FRACBITS
              && players[i].mo->y == mthing->y << FRACBITS)
           return FALSE;
     return TRUE;
  }

  x = mthing->x << FRACBITS;
  y = mthing->y << FRACBITS;

  // killough 4/2/98: fix bug where P_CheckPosition() uses a non-solid
  // corpse to detect collisions with other players in DM starts
  //
  // Old code:
  // if (!P_CheckPosition (players[playernum].mo, x, y))
  //    return FALSE;

  players[playernum].mo->flags |=  MF_SOLID;
  i = P_CheckPosition(players[playernum].mo, x, y);
  players[playernum].mo->flags &= ~MF_SOLID;
  if (!i)
    return FALSE;

  // flush an old corpse if needed
  // killough 2/8/98: make corpse queue have an adjustable limit
  // killough 8/1/98: Fix bugs causing strange crashes

  if (bodyquesize > 0)
    {
      static int queuesize;
      if (queuesize < bodyquesize)
	{
	  bodyque = realloc(bodyque, bodyquesize*sizeof*bodyque);
	  memset(bodyque+queuesize, 0,
		 (bodyquesize-queuesize)*sizeof*bodyque);
	  queuesize = bodyquesize;
	}
      if (bodyqueslot >= bodyquesize)
	P_RemoveMobj(bodyque[bodyqueslot % bodyquesize]);
      bodyque[bodyqueslot++ % bodyquesize] = players[playernum].mo;
    }
  else
    if (!bodyquesize)
      P_RemoveMobj(players[playernum].mo);

  /* spawn a teleport fog */
  ss = R_PointInSubsector (x,y);
  {
     // Teleport fog at respawn point
    fixed_t xa,ya;
    int an;
    mobj_t      *mo;

/* BUG: an can end up negative, because mthing->angle is (signed) short.
 * We have to emulate original Doom's behaviour, deferencing past the start
 * of the array, into the previous array (finetangent) */
    an = ( ANG45 * ((signed)mthing->angle/45) ) >> ANGLETOFINESHIFT;
    xa = finecosine[an];
    ya = finesine[an];

    if (compatibility_level <= finaldoom_compatibility || compatibility_level == prboom_4_compatibility)
      switch (an) {
      case -4096: xa = finetangent[2048];   // finecosine[-4096]
          	ya = finetangent[0];      // finesine[-4096]
          	break;
      case -3072: xa = finetangent[3072];   // finecosine[-3072]
          	ya = finetangent[1024];   // finesine[-3072]
          	break;
      case -2048: xa = finesine[0];   // finecosine[-2048]
          	ya = finetangent[2048];   // finesine[-2048]
          	break;
      case -1024:	xa = finesine[1024];     // finecosine[-1024]
          	ya = finetangent[3072];  // finesine[-1024]
          	break;
      case 1024:
      case 2048:
      case 3072:
      case 4096:
      case 0:	break; /* correct angles set above */
      default:	I_Error("G_CheckSpot: unexpected angle %d\n",an);
      }

    mo = P_SpawnMobj(x+20*xa, y+20*ya, ss->sector->floorheight, MT_TFOG);

    if (players[consoleplayer].viewz != 1)
      S_StartSound(mo, sfx_telept);  // don't start sound on first frame
  }

  return TRUE;
}


/*
====================
=
= G_DeathMatchSpawnPlayer
=
= Spawns a player at one of the random death match spots
= called at level load and each death
====================
*/

void G_DeathMatchSpawnPlayer (int playernum)
{
   int i, j;
   int selections = deathmatch_p - deathmatchstarts;

   if (selections < MAXPLAYERS)
      I_Error("G_DeathMatchSpawnPlayer: Only %i deathmatch spots, %d required",
            selections, MAXPLAYERS);

   for (j=0 ; j<20 ; j++)
   {
      i = P_Random(pr_dmspawn) % selections;
      if (G_CheckSpot (playernum, &deathmatchstarts[i]) )
      {
         deathmatchstarts[i].type = playernum+1;
         P_SpawnPlayer (playernum, &deathmatchstarts[i]);
         return;
      }
   }

   /* no good spot, so the player will probably get stuck */
   P_SpawnPlayer (playernum, G_PlayerStart(playernum));
}

/*
====================
=
= G_DoReborn
=
====================
*/

void G_DoReborn (int playernum)
{
   int i;

   if (!netgame)
   {
      gameaction = ga_loadlevel;			/* reload the level from scratch  */
      return;
   }

   /*	respawn this player while the other players keep going */

   /* dissasociate the corpse */
   players[playernum].mo->player = NULL;

   /* spawn at random spot if in death match */
   if (deathmatch)
   {
      G_DeathMatchSpawnPlayer (playernum);
      return;
   }

   if (G_CheckSpot (playernum, G_PlayerStart(playernum)) )
   {
      P_SpawnPlayer (playernum, G_PlayerStart(playernum));
      return;
   }

   /* try to spawn at one of the other players spots */
   for (i=0 ; i<MAXPLAYERS ; i++)
   {
      if (G_CheckSpot (playernum, G_PlayerStart(i)) )
      {
         P_SpawnPlayer (playernum, G_PlayerStart(i));
         return;
      }
   }

   /* he's going to be inside something.  Too bad. */
   P_SpawnPlayer (playernum, G_PlayerStart(playernum));
}

// DOOM Par Times
// killough/BFG: Episode 4 (Thy Flesh Consumed) par times were absent
// from the original Ultimate DOOM release; the values below were added
// in DOOM 3 BFG Edition.  The table is [5][10] so gameepisode 4 (E4) is
// in bounds -- previously it was [4][10] and E4 read past the end.
int pars[5][10] = {
  {0},
  {0,30,75,120,90,165,180,180,30,165},
  {0,90,90,90,120,90,360,240,30,170},
  {0,90,45,90,150,90,90,165,30,135},
  {0,165,255,135,150,180,390,135,360,180}
};

// DOOM II Par Times
int cpars[32] = {
  30,90,120,120,90,150,120,120,270,90,  //  1-10
  210,150,150,150,210,150,420,150,210,150,  // 11-20
  240,150,180,150,150,300,330,420,300,180,  // 21-30
  120,30          // 31-32
};

dbool   secretexit;

/*
====================
=
= G_ExitLevel
=
====================
*/

void G_ExitLevel (void)
{
  secretexit = FALSE;
  gameaction = ga_completed;
}

/* Hexen Teleport_NewMap: stage the destination (a MAPINFO warp number and an
 * arrival start position) and complete the level.  map -1 ends the game. */
void G_Completed(int map, int position)
{
  secretexit = false;
  LeaveMap = map;
  LeavePosition = position;
  gameaction = ga_completed;
}

// Here's for the german edition.
// IF NO WOLF3D LEVELS, NO SECRET EXIT!

void G_SecretExitLevel (void)
{
  if (gamemode!=commercial || haswolflevels)
    secretexit = TRUE;
  else
    secretexit = FALSE;
  gameaction = ga_completed;
}

//
// G_DoCompleted
//

void G_DoCompleted (void)
{
  int i;

  gameaction = ga_nothing;

  for (i=0; i<MAXPLAYERS; i++)
    if (playeringame[i])
      G_PlayerFinishLevel(i);        // take away cards and stuff

  if (automapmode & am_active)
    AM_Stop();

  /* Hexen: hub travel within a cluster loads the next map directly; a
   * cluster change (or any deathmatch exit) goes through the interlude
   * first -- the CLUSxMSG text screen or the frag tally -- and the staged
   * destination is consumed by G_DoWorldDone when it ends. */
  if (hexen)
  {
    if (LeaveMap == -1)
    { /* Teleport_EndGame */
      gameaction = ga_victory;
      return;
    }
    wminfo.nextep = gameepisode - 1;
    wminfo.next = P_TranslateMapWarp(LeaveMap) - 1;
    RebornPosition = LeavePosition;
    if (!deathmatch &&
        P_GetMapCluster(gamemap) ==
        P_GetMapCluster(P_TranslateMapWarp(LeaveMap)))
    {
      gameaction = ga_worlddone;
      return;
    }
    gamestate = GS_INTERMISSION;
    automapmode &= ~am_active;
    WI_Start(&wminfo);
    return;
  }

  wminfo.lastmapinfo = gamemapinfo;
  wminfo.nextmapinfo = NULL;
  if (gamemapinfo)
  {
    const char *next = "";
    if (gamemapinfo->endpic[0])
    {
      gameaction = ga_victory;
      return;
    }
    if (secretexit)
       next = gamemapinfo->nextsecret;
    if (next[0] == 0)
       next = gamemapinfo->nextmap;
    if (next[0])
    {
      G_ValidateMapName(next, &wminfo.nextep, &wminfo.next);
      wminfo.nextep--;
      wminfo.next--;
      wminfo.didsecret = players[consoleplayer].didsecret;
      wminfo.partime = gamemapinfo->partime;
      goto frommapinfo;	// skip past the default setup.
    }
  }

  if (gamemode != commercial) // kilough 2/7/98
    switch(gamemap)
    {
      // cph - Remove ExM8 special case, so it gets summary screen displayed
      case 9:
        for (i=0 ; i<MAXPLAYERS ; i++)
          players[i].didsecret = TRUE;
        break;
    }

  wminfo.didsecret = players[consoleplayer].didsecret;
  wminfo.nextep = wminfo.epsd = gameepisode -1;
  wminfo.last = gamemap -1;

  // wminfo.next is 0 biased, unlike gamemap
  if (gamemode == commercial)
    {
      if (secretexit)
        switch(gamemap)
          {
          case 15:
            wminfo.next = 30; break;
          case 31:
            wminfo.next = 31; break;
          case 2:
            if (bfgedition)
               wminfo.next = 32;
            break;
          }
      else
        switch(gamemap)
          {
          case 31:
          case 32:
            wminfo.next = 15; break;
          case 33:
            wminfo.next = 2; break;
          default:
            wminfo.next = gamemap;
          }
    }
  else
    {
      if (secretexit)
        wminfo.next = 8;  // go to secret level
      else
        if (gamemap == 9)
          {
            // returning from secret level
            if (heretic)
              {
                /* vanilla heretic afterSecret: E1M9 returns to E1M7, the
                 * middle episodes to M5, episode 5 to M4 (0-biased here) */
                static const int after_secret[5] = { 6, 4, 4, 4, 3 };
                if (gameepisode >= 1 && gameepisode <= 5)
                  wminfo.next = after_secret[gameepisode - 1];
                else
                  wminfo.next = 0;
              }
            else
            switch (gameepisode)
              {
              case 1:
                wminfo.next = 3;
                break;
              case 2:
                wminfo.next = 5;
                break;
              case 3:
                wminfo.next = 6;
                break;
              case 4:
                wminfo.next = 2;
                break;
              }
          }
        else
          wminfo.next = gamemap;          // go to next level
    }

  /* Par lookups are bounds-guarded: the tables cover Doom's episodes and
   * maps only, and Heretic reaches episode 6 (no par times exist for it
   * at all; its intermission shows none). */
  wminfo.partime = 0;
  if ( gamemode == commercial )
  {
    if (gamemap >= 1 && gamemap <= 34)
      wminfo.partime = TICRATE*cpars[gamemap-1];
  }
  else if (!heretic && gameepisode >= 1 && gameepisode <= 4
           && gamemap >= 1 && gamemap <= 9)
    wminfo.partime = TICRATE*pars[gameepisode][gamemap];

frommapinfo:
  wminfo.nextmapinfo = G_LookupMapinfo(wminfo.nextep+1, wminfo.next+1);
  wminfo.maxkills = totalkills;
  wminfo.maxitems = totalitems;
  wminfo.maxsecret = totalsecret;
  wminfo.maxfrags = 0;
  wminfo.pnum = consoleplayer;

  for (i=0 ; i<MAXPLAYERS ; i++)
    {
      wminfo.plyr[i].in = playeringame[i];
      wminfo.plyr[i].skills = players[i].killcount;
      wminfo.plyr[i].sitems = players[i].itemcount;
      wminfo.plyr[i].ssecret = players[i].secretcount;
      wminfo.plyr[i].stime = leveltime;
      memcpy (wminfo.plyr[i].frags, players[i].frags,
              sizeof(wminfo.plyr[i].frags));
    }

  /* cph - modified so that only whole seconds are added to the totalleveltimes
   *  value; so our total is compatible with the "naive" total of just adding
   *  the times in seconds shown for each level. Also means our total time
   *  will agree with Compet-n.
   */
  wminfo.totaltimes = (totalleveltimes += (leveltime - leveltime%35));

  gamestate = GS_INTERMISSION;
  automapmode &= ~am_active;

  WI_Start (&wminfo);
}

//
// G_WorldDone
//

/* The staged Teleport_NewMap destination (warp number); the hexen
 * intermission needs it to pick the cluster message. */
int G_GetLeaveMap(void)
{
  return LeaveMap;
}

void G_WorldDone (void)
{
  gameaction = ga_worlddone;

  if (secretexit)
    players[consoleplayer].didsecret = TRUE;

  if (gamemapinfo)
  {
    if (gamemapinfo->intertextsecret && secretexit)
    {
      if (gamemapinfo->intertextsecret[0] != '-') // '-' means that any default intermission was cleared.
        F_StartFinale();
      return;
    }
    else if (gamemapinfo->intertext && !secretexit)
    {
      if (gamemapinfo->intertext[0] != '-') // '-' means that any default intermission was cleared.
        F_StartFinale();
      return;
    }
    else if (gamemapinfo->endpic[0] && gamemapinfo->nointermission)
    {
      // game ends without a status screen.
      gameaction = ga_victory;
      return;
    }
    // if nothing applied, use the defaults.
  }

  if (gamemode == commercial)
    {
      switch (gamemap)
        {
        case 15:
        case 31:
          if (!secretexit)
            break;
          // fall through
        case 6:
        case 11:
        case 20:
        case 30:
          F_StartFinale ();
          break;
        }
    }
  else if (gamemap == 8)
    gameaction = ga_victory; // cph - after ExM8 summary screen, show victory stuff
}

void G_DoWorldDone (void)
{
  idmusnum = -1;             //jff 3/17/98 allow new level's music to be loaded
  gamestate = GS_LEVEL;
  gameepisode = wminfo.nextep + 1;

  /* Hexen hub travel: archive the departing map and restore the
   * destination's archived state when revisiting (sv_save.c). */
  if (hexen)
  {
    SV_MapTeleport(wminfo.next + 1, RebornPosition);
    gameaction = ga_nothing;
    AM_clearMarks();
    return;
  }

  gamemap = wminfo.next + 1;
  gamemapinfo = G_LookupMapinfo(gameepisode, gamemap);
  G_DoLoadLevel();
  gameaction = ga_nothing;
  AM_clearMarks();           //jff 4/12/98 clear any marks on the automap
}

// killough 2/28/98: A ridiculously large number
// of players, the most you'll ever need in a demo
// or savegame. This is used to prevent problems, in
// case more players in a game are supported later.

#define MIN_MAXPLAYERS 32

extern dbool   setsizeneeded;

//CPhipps - savename variable redundant

/* killough 12/98:
 * This function returns a signature for the current wad.
 * It is used to distinguish between wads, for the purposes
 * of savegame compatibility warnings, and options lookups.
 */

static uint64_t G_UpdateSignature(uint64_t s, const char *name)
{
   int i, lump = W_CheckNumForName(name);
   if (lump != -1 && (i = lump+10) < numlumps)
      do
      {
         int size = W_LumpLength(i);
         const uint8_t *p = W_CacheLumpNum(i);
         while (size--)
            s <<= 1, s += *p++;
         W_UnlockLumpNum(i);
      }
      while (--i > lump);
   return s;
}

static uint64_t G_Signature(void)
{
  static uint64_t s = 0;
  static dbool   computed = FALSE;
  char name[9];
  int episode, map;

  if (!computed) {
   computed = TRUE;
   if (gamemode == commercial)
    for (map = haswolflevels ? 32 : 30; map; map--)
      sprintf(name, "map%02u", (unsigned)(map & 0xff)), s = G_UpdateSignature(s, name);
   else
    for (episode = gamemode==retail ? 4 :
     gamemode==shareware ? 1 : 3; episode; episode--)
      for (map = 9; map; map--)
  sprintf(name, "E%uM%u", (unsigned)(episode & 0xff), (unsigned)(map & 0xff)), s = G_UpdateSignature(s, name);
  }
  return s;
}

//
// killough 5/15/98: add forced loadgames, which allow user to override checks
//

void G_ForcedLoadGame(void)
{
  // CPhipps - net loadgames are always forced, so we only reach here
  //  in single player
  gameaction = ga_loadgame;
  forced_loadgame = TRUE;
}

// killough 3/16/98: add slot info
// killough 5/15/98: add command-line
void G_LoadGame(int slot, dbool   command)
{
  if (!demoplayback && !command) {
    // CPhipps - handle savegame filename in G_DoLoadGame
    //         - Delay load so it can be communicated in net game
    //         - store info in special_event
    special_event = BT_SPECIAL | (BTS_LOADGAME & BT_SPECIALMASK) |
      ((slot << BTS_SAVESHIFT) & BTS_SAVEMASK);
    forced_loadgame = netgame; // CPhipps - always force load netgames
  } else {
    // Do the old thing, immediate load
    gameaction = ga_loadgame;
    forced_loadgame = FALSE;
    savegameslot = slot;
    demoplayback = FALSE;
    // Don't stay in netgame state if loading single player save
    // while watching multiplayer demo
    netgame = FALSE;
  }
  command_loadgame = command;
  R_SmoothPlaying_Reset(NULL); // e6y
}

// killough 5/15/98:
// Consistency Error when attempting to load savegame.

static void G_LoadGameErr(const char *msg)
{
  Z_Free(savebuffer);                // Free the savegame buffer
  /* Null the globals here so callers (G_DoLoadGame's error paths)
   * can fall through to their trailing Z_Free(savebuffer) without
   * causing a double-free.  Matches the post-save pattern in
   * G_DoSaveGame / G_DoSaveGameToBuffer where savebuffer/save_p
   * are nulled together. */
  savebuffer = NULL;
  save_p     = NULL;
  M_ForcedLoadGame(msg);             // Print message asking for 'Y' to force
  if (command_loadgame)              // If this was a command-line -loadgame
    {
      D_StartTitle();                // Start the title screen
      gamestate = GS_DEMOSCREEN;     // And set the game state accordingly
    }
}

// CPhipps - size of version header
#define VERSIONSIZE   16

const char * comp_lev_str[MAX_COMPATIBILITY_LEVEL] =
{ "doom v1.2", "doom v1.666", "doom/doom2 v1.9", "ultimate doom", "final doom",
  "dosdoom compatibility", "tasdoom compatibility", "\"boom compatibility\"", "boom v2.01", "boom v2.02", "lxdoom v1.3.2+",
  "MBF", "PrBoom 2.03beta", "PrBoom v2.1.0-2.1.1", "PrBoom v2.1.2-v2.2.6",
  "PrBoom v2.3.x", "PrBoom 2.4.0", "Current PrBoom", "MBF21"  };

// comp_options_by_version removed - see G_Compatibility

static uint8_t map_old_comp_levels[] =
{ 0, 1, 2, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };

static const struct {
  int comp_level;
  const char* ver_printf;
  int version;
} version_headers[] = {
  /* cph - we don't need a new version_header for prboom_3_comp/v2.1.1, since
   *  the file format is unchanged. */
  { prboom_3_compatibility, "PrBoom %d", 210},
  { prboom_5_compatibility, "PrBoom %d", 211},
  { prboom_6_compatibility, "PrBoom %d", 212}
};

static const size_t num_version_headers = sizeof(version_headers) / sizeof(version_headers[0]);

//
// Load the game from the internal savebuffer
//
static int G_DoLoadGameFromSaveBuffer(int length)
{
  int isok;
  int  i;
  int savegame_compatibility = -1;

  gameaction = ga_nothing;

  save_p = savebuffer + SAVESTRINGSIZE;

  // CPhipps - read the description field, compare with supported ones
  for (i=0; (size_t)i<num_version_headers; i++) {
    char vcheck[VERSIONSIZE];
    // killough 2/22/98: "proprietary" version string :-)
    sprintf (vcheck, version_headers[i].ver_printf, version_headers[i].version);

    if (!strncmp((const char*)save_p, vcheck, VERSIONSIZE)) {
      savegame_compatibility = version_headers[i].comp_level;
      i = num_version_headers;
    }
  }
  if (savegame_compatibility == -1) {
    if (forced_loadgame) {
      savegame_compatibility = MAX_COMPATIBILITY_LEVEL-1;
    } else {
      return -2;
    }
  }

  save_p += VERSIONSIZE;

  /* Raven layout tag (see G_DoSaveGameToSaveBuffer).  Reject saves written by
   * an older build whose Heretic/Hexen struct layout differs, rather than
   * deserialising them misaligned and crashing.  Not forced-loadable: a
   * layout mismatch is unrecoverable, unlike a wad/checksum mismatch. */
  if (raven)
  {
    /* RVN2: the raven mobj record became the full struct (the legacy
     * truncated layout lost tid/special/damage/floorclip) and hexen saves
     * gained the world state (ACS, polyobjs, sound sequences).
     * RVN3 (hexen only): the hub map archives ride in the savegame.
     * RVN4 (hexen only): the map archives gained the vanilla sound-
     * sequence segment.
     * RVN5 (heretic only): the ambient sound cursor rides in the save. */
    char raven_magic[4] = { 'R','V','N','5' };
    if (hexen)
      raven_magic[3] = '4';
    if (memcmp(save_p, raven_magic, sizeof raven_magic))
      return -2;
    save_p += sizeof raven_magic;
  }

  // CPhipps - always check savegames even when forced,
  //  only print a warning if forced
  {  // killough 3/16/98: check lump name checksum (independent of order)
    uint64_t checksum = 0;

    checksum = G_Signature();

    if (memcmp(&checksum, save_p, sizeof checksum)) {
      if (!forced_loadgame) {
        return -3;
      } else
  lprintf(LO_WARN, "G_DoLoadGame: Incompatible savegame\n");
    }
    save_p += sizeof checksum;
   }

  save_p += strlen((const char*)save_p)+1;

  compatibility_level = (savegame_compatibility >= prboom_4_compatibility) ? *save_p : savegame_compatibility;
  if (savegame_compatibility < prboom_6_compatibility)
    compatibility_level = map_old_comp_levels[compatibility_level];
  save_p++;

  gameskill = *save_p++;
  gameepisode = *save_p++;
  gamemap = *save_p++;
  gamemapinfo = G_LookupMapinfo(gameepisode, gamemap);

  for (i=0 ; i<MAXPLAYERS ; i++)
    playeringame[i] = *save_p++;
  save_p += MIN_MAXPLAYERS-MAXPLAYERS;         // killough 2/28/98

  idmusnum = *save_p++;           // jff 3/17/98 restore idmus music
  if (idmusnum==255) idmusnum=-1; // jff 3/18/98 account for unsigned byte

  /* killough 3/1/98: Read game options
   * killough 11/98: move down to here
   */
  // Avoid assignment of const to non-const: add the difference
  // between the updated and original pointer onto the original
  save_p += (G_ReadOptions(save_p) - save_p);

  // load a base level
  G_InitNew (gameskill, gameepisode, gamemap);

  /* get the times - killough 11/98: save entire word */
  memcpy(&leveltime, save_p, sizeof leveltime);
  save_p += sizeof leveltime;

  /* cph - total episode time */
  if (compatibility_level >= prboom_2_compatibility) {
    memcpy(&totalleveltimes, save_p, sizeof totalleveltimes);
    save_p += sizeof totalleveltimes;
  }
  else totalleveltimes = 0;

  // killough 11/98: load revenant tracer state
  basetic = gametic - *save_p++;

  // dearchive all the modifications
  P_MapStart();
  P_UnArchiveACS ();   /* hexen world vars + deferred scripts (no-op otherwise) */
  P_UnArchivePlayers ();
  P_UnArchiveWorld ();
  P_UnArchivePolyobjs (); /* hexen (no-op otherwise) */
  P_UnArchiveThinkers ();
  if (hexen)
    P_CreateTIDList (); /* thing IDs hang off the freshly loaded mobjs */
  P_UnArchiveSpecials ();
  P_UnArchiveScripts (); /* hexen ACS script states + map vars (no-op otherwise) */
  P_UnArchiveSounds ();  /* hexen active sound sequences (no-op otherwise) */
  P_UnArchiveAmbientSound ();  /* heretic ambient cursor (no-op otherwise) */
  SV_UnArchiveMaps (); /* hexen hub map archives (no-op otherwise) */
  P_UnArchiveRNG ();    // killough 1/18/98: load RNG information
  P_UnArchiveMap ();    // killough 1/22/98: load automap information
  P_MapEnd();
  R_SmoothPlaying_Reset(NULL); // e6y

  isok = *save_p == 0xe6;
  if (!isok)
    I_Error ("G_DoLoadGame: Bad savegame");

  if (setsizeneeded)
    R_ExecuteSetViewSize ();

  return isok ? 0 : -1;
}

void G_DoLoadGame(void)
{
  int err;
  int  length;
  // CPhipps - do savegame filename stuff here
  char name[PATH_MAX+1];     // killough 3/22/98

  G_SaveGameName(name,sizeof(name),savegameslot, demoplayback);

  length = M_ReadFile(name, &savebuffer);
  if (length<=0)
    I_Error("Couldn't read file %s: %s", name, "(Unknown Error)");

  err = G_DoLoadGameFromSaveBuffer(length);
  if (err == -2) {
    G_LoadGameErr("Unrecognised savegame version!\nAre you sure? (y/n) ");
  }

  if (err == -3) {
    char *msg;
    uint64_t checksum = 0;

    checksum = G_Signature();
    msg      = malloc(strlen((const char*)save_p + sizeof checksum) + 128);
    strcpy(msg,"Incompatible Savegame!!!\n");
    if (save_p[sizeof checksum])
      strcat(strcat(msg,"Wads expected:\n\n"), (const char*)save_p + sizeof checksum);
    strcat(msg, "\nAre you sure?");
    G_LoadGameErr(msg);
    free(msg);
  }

  // done
  Z_Free (savebuffer);
  savebuffer = NULL;
  save_p     = NULL;
}

bool G_DoLoadGameFromBuffer(void *data, size_t length)
{
  int err;
  savebuffer = data;

  err = G_DoLoadGameFromSaveBuffer(length);

  // done
  savebuffer = NULL;

  return err == 0;
}

//
// G_SaveGame
// Called by the menu task.
// Description is a 24 byte text string
//

void G_SaveGame(int slot, char *description)
{
  strcpy(savedescription, description);
  if (demoplayback) {
    /* cph - We're doing a user-initiated save game while a demo is
     * running so, go outside normal mechanisms
     */
    savegameslot = slot;
    G_DoSaveGame(TRUE);
  }
  // CPhipps - store info in special_event
  special_event = BT_SPECIAL | (BTS_SAVEGAME & BT_SPECIALMASK) |
    ((slot << BTS_SAVESHIFT) & BTS_SAVEMASK);
}

// Check for overrun and realloc if necessary -- Lee Killough 1/22/98
void (CheckSaveGame)(size_t size, const char* file, int line)
{
  size_t pos = save_p - savebuffer;

  size += 1024;  // breathing room
  if (pos+size > savegamesize)
    save_p = (savebuffer = realloc(savebuffer,
           savegamesize += (size+1023) & ~1023)) + pos;
}

/* killough 3/22/98: form savegame name in one location
 * (previously code was scattered around in multiple places)
 * cph - Avoid possible buffer overflow problems by passing
 * size to this function and using snprintf */

void G_SaveGameName(char *name, size_t size, int slot, dbool   demoplayback)
{
  const char* sgn = demoplayback ? "demosav" : savegamename;
#ifdef _WIN32
  char slash = '\\';
#else
  char slash = '/';
#endif
  snprintf (name, size, "%s%c%s%d.dsg", basesavegame, slash, sgn, slot);
}

//
// Save the game state into the internal savebuffer
//
static int G_DoSaveGameToSaveBuffer() {
  char name2[VERSIONSIZE];
  char *description;
  int  length, i;

  description = savedescription;

  save_p = savebuffer = malloc(savegamesize);

  CheckSaveGame(SAVESTRINGSIZE+VERSIONSIZE+sizeof(uint64_t));
  memcpy (save_p, description, SAVESTRINGSIZE);
  save_p += SAVESTRINGSIZE;
  memset (name2,0,sizeof(name2));

  // CPhipps - scan for the version header
  for (i=0; (size_t)i<num_version_headers; i++)
    if (version_headers[i].comp_level == best_compatibility) {
      // killough 2/22/98: "proprietary" version string :-)
      sprintf (name2,version_headers[i].ver_printf,version_headers[i].version);
      memcpy (save_p, name2, VERSIONSIZE);
      i = num_version_headers+1;
    }

  save_p += VERSIONSIZE;

  /* Raven (Heretic/Hexen) layout tag.  The shared PrBoom version header only
   * identifies the file format, not the in-memory struct layout, and this
   * fork has grown mobj_t / player_t with Heretic/Hexen fields.  A savegame
   * written by an older build of this core therefore passes the version
   * check but deserialises with the wrong field offsets, desynchronising the
   * thinker/special stream and crashing in P_UnArchiveSpecials.  Stamp a
   * layout magic for raven games so an incompatible old save is rejected
   * cleanly instead.  Bump RAVEN_SAVE_MAGIC whenever a raven-affecting struct
   * layout changes.  Doom saves are untouched (the shared format is shared
   * with upstream). */
  if (raven)
  {
    char raven_magic[4] = { 'R','V','N','5' };
    if (hexen)
      raven_magic[3] = '4';
    CheckSaveGame(sizeof raven_magic);
    memcpy(save_p, raven_magic, sizeof raven_magic);
    save_p += sizeof raven_magic;
  }

  { /* killough 3/16/98, 12/98: store lump name checksum */
    uint64_t checksum = G_Signature();
    memcpy(save_p, &checksum, sizeof checksum);
    save_p += sizeof checksum;
  }

  // killough 3/16/98: store pwad filenames in savegame
  {
    // CPhipps - changed for new wadfiles handling
    size_t i;
    for (i = 0; i<numwadfiles; i++)
      {
        const char *const w = wadfiles[i].name;
        CheckSaveGame(strlen(w)+2);
        strcpy((char*)save_p, w);
        save_p += strlen((const char*)save_p);
        *save_p++ = '\n';
      }
    *save_p++ = 0;
  }

  CheckSaveGame(GAME_OPTION_SIZE+MIN_MAXPLAYERS+14);

  *save_p++ = compatibility_level;

  *save_p++ = gameskill;
  *save_p++ = gameepisode;
  *save_p++ = gamemap;

  for (i=0 ; i<MAXPLAYERS ; i++)
    *save_p++ = playeringame[i];

  for (;i<MIN_MAXPLAYERS;i++)         // killough 2/28/98
    *save_p++ = 0;

  *save_p++ = idmusnum;               // jff 3/17/98 save idmus state

  save_p = G_WriteOptions(save_p);    // killough 3/1/98: save game options

  /* cph - FIXME - endianness? */
  /* killough 11/98: save entire word */
  memcpy(save_p, &leveltime, sizeof leveltime);
  save_p += sizeof leveltime;

  /* cph - total episode time */
  if (compatibility_level >= prboom_2_compatibility) {
    memcpy(save_p, &totalleveltimes, sizeof totalleveltimes);
    save_p += sizeof totalleveltimes;
  }
  else totalleveltimes = 0;

  // killough 11/98: save revenant tracer state
  *save_p++ = (gametic-basetic) & 255;

  P_ArchiveACS();      /* hexen world vars + deferred scripts (no-op otherwise) */

  P_ArchivePlayers();

  // phares 9/13/98: Move mobj_t->index out of P_ArchiveThinkers so the
  // indices can be used by P_ArchiveWorld when the sectors are saved.
  // This is so we can save the index of the mobj_t of the thinker that
  // caused a sound, referenced by sector_t->soundtarget.
  P_ThinkerToIndex();

  P_ArchiveWorld();
  P_ArchivePolyobjs(); /* hexen (no-op otherwise) */
  P_ArchiveThinkers();

  // phares 9/13/98: Move index->mobj_t out of P_ArchiveThinkers, simply
  // for symmetry with the P_ThinkerToIndex call above.

  P_IndexToThinker();

  P_ArchiveSpecials();
  P_ArchiveScripts();  /* hexen ACS script states + map vars (no-op otherwise) */
  P_ArchiveSounds();   /* hexen active sound sequences (no-op otherwise) */
  P_ArchiveAmbientSound();   /* heretic ambient cursor (no-op otherwise) */
  SV_ArchiveMaps();    /* hexen hub map archives (no-op otherwise) */
  P_ArchiveRNG();    // killough 1/18/98: save RNG information
  P_ArchiveMap();    // killough 1/22/98: save automap information

  *save_p++ = 0xe6;   // consistancy marker

  length = save_p - savebuffer;

  return length;
}

static void G_DoSaveGame (dbool   menu)
{
  int length;
  char name[PATH_MAX+1];

  gameaction = ga_nothing; // cph - cancel savegame at top of this function,
    // in case later problems cause a premature exit

  G_SaveGameName(name,sizeof(name),savegameslot, demoplayback && !menu);

  length = G_DoSaveGameToSaveBuffer();

  doom_printf( "%s", M_WriteFile(name, savebuffer, length)
         ? s_GGSAVED /* Ty - externalised */
         : "Game save failed!"); // CPhipps - not externalised

  free(savebuffer);  // killough
  savebuffer = save_p = NULL;

  savedescription[0] = 0;
}

bool G_DoSaveGameToBuffer(void *buf, size_t size) {
  int length, ok;
  char description_saved[SAVEDESCLEN];
  // If no game is loaded we can't save
  if (thinkercap.next == NULL)
    return false;


  memcpy(description_saved, savedescription, SAVEDESCLEN);
  strcpy(savedescription, "BUFFER");

  length = G_DoSaveGameToSaveBuffer();

  ok = (length > 0 && (size_t) length <= size);

  if (ok) {
    memcpy(buf, savebuffer, length);
    memset(((char*)buf)+length, 0, size - length);
  }

  free(savebuffer);  // killough
  savebuffer = save_p = NULL;
  memcpy(savedescription, description_saved, SAVEDESCLEN);

  return ok;
}

static skill_t d_skill;
static int     d_episode;
static int     d_map;

void G_DeferedInitNew(skill_t skill, int episode, int map)
{
  d_skill = skill;
  d_episode = episode;
  d_map = map;
  gameaction = ga_newgame;
}

/* cph -
 * G_Compatibility
 *
 * Initialises the comp[] array based on the compatibility_level
 * For reference, MBF did:
 * for (i=0; i < COMP_TOTAL; i++)
 *   comp[i] = compatibility;
 *
 * Instead, we have a lookup table showing at what version a fix was
 *  introduced, and made optional (replaces comp_options_by_version)
 */

void G_Compatibility(void)
{
  static const struct {
    complevel_t fix; // level at which fix/change was introduced
    complevel_t opt; // level at which fix/change was made optional
  } levels[] = {
    // comp_telefrag - monsters used to telefrag only on MAP30, now they do it for spawners only
    { mbf_compatibility, mbf_compatibility },
    // comp_dropoff - MBF encourages things to drop off of overhangs
    { mbf_compatibility, mbf_compatibility },
    // comp_vile - original Doom archville bugs like ghosts
    { boom_compatibility, mbf_compatibility },
    // comp_pain - original Doom limits Pain Elementals from spawning too many skulls
    { boom_compatibility, mbf_compatibility },
    // comp_skull - original Doom let skulls be spit through walls by Pain Elementals
    { boom_compatibility, mbf_compatibility },
    // comp_blazing - original Doom duplicated blazing door sound
    { boom_compatibility, mbf_compatibility },
    // e6y: "Tagged doors don't trigger special lighting" handled wrong
    // http://sourceforge.net/tracker/index.php?func=detail&aid=1411400&group_id=148658&atid=772943
    // comp_doorlight - MBF made door lighting changes more gradual
    { boom_compatibility, mbf_compatibility },
    // comp_model - improvements to the game physics
    { boom_compatibility, mbf_compatibility },
    // comp_god - fixes to God mode
    { boom_compatibility, mbf_compatibility },
    // comp_falloff - MBF encourages things to drop off of overhangs
    { mbf_compatibility, mbf_compatibility },
    // comp_floors - fixes for moving floors bugs
    { boom_compatibility_compatibility, mbf_compatibility },
    // comp_skymap
    { boom_compatibility, mbf_compatibility },
    // comp_pursuit - MBF AI change, limited pursuit?
    { mbf_compatibility, mbf_compatibility },
    // comp_doorstuck - monsters stuck in doors fix
    { boom_202_compatibility, mbf_compatibility },
    // comp_staylift - MBF AI change, monsters try to stay on lifts
    { mbf_compatibility, mbf_compatibility },
    // comp_zombie - prevent dead players triggering stuff
    { lxdoom_1_compatibility, mbf_compatibility },
    // comp_stairs - see p_floor.c
    { boom_202_compatibility, mbf_compatibility },
    // comp_infcheat - FIXME
    { mbf_compatibility, mbf_compatibility },
    // comp_zerotags - allow zero tags in wads */
    { boom_compatibility, mbf_compatibility },
    // comp_moveblock - enables keygrab and mancubi shots going thru walls
    { lxdoom_1_compatibility, prboom_2_compatibility },
    // comp_respawn - objects which aren't on the map at game start respawn at (0,0)
    { prboom_2_compatibility, prboom_2_compatibility },
    // comp_sound - see s_sound.c
    { boom_compatibility_compatibility, prboom_3_compatibility },
    // comp_666 - enables tag 666 in non-ExM8 levels
    { ultdoom_compatibility, prboom_4_compatibility },
    // comp_soul - enables lost souls bouncing (see P_ZMovement)
    { prboom_4_compatibility, prboom_4_compatibility },
    // comp_maskedanim - 2s mid textures don't animate
    { doom_1666_compatibility, prboom_4_compatibility },
  };
  unsigned i;

  if (sizeof(levels)/sizeof(*levels) != COMP_NUM)
    I_Error("G_Compatibility: consistency error");

  for (i = 0; i < sizeof(levels)/sizeof(*levels); i++)
    if (compatibility_level < levels[i].opt)
      comp[i] = (compatibility_level < levels[i].fix);

  if (!mbf_features) {
    monster_infighting = 1;
    monster_backing = 0;
    monster_avoid_hazards = 0;
    monster_friction = 0;
    help_friends = 0;

    monkeys = 0;
  }

  if (demo_compatibility) {
    allow_pushers = 0;
    variable_friction = 0;
    monsters_remember = 0;
    weapon_recoil = 0;
    player_bobbing = 1;
  }
}

// killough 3/1/98: function to reload all the default parameter
// settings before a new game begins

void G_ReloadDefaults(void)
{
  // killough 3/1/98: Initialize options based on config file
  // (allows functions above to load different values for demos
  // and savegames without messing up defaults).

  weapon_recoil = default_weapon_recoil;    // weapon recoil

  player_bobbing = default_player_bobbing;  // whether player bobs or not

  /* cph 2007/06/31 - for some reason, the default_* of the next 2 vars was never implemented */
  variable_friction = default_variable_friction;
  allow_pushers     = default_allow_pushers;


  monsters_remember = default_monsters_remember;   // remember former enemies

  monster_infighting = default_monster_infighting; // killough 7/19/98


  distfriend = default_distfriend;                 // killough 8/8/98

  monster_backing = default_monster_backing;     // killough 9/8/98

  monster_avoid_hazards = default_monster_avoid_hazards; // killough 9/9/98

  monster_friction = default_monster_friction;     // killough 10/98

  help_friends = default_help_friends;             // killough 9/9/98

  monkeys = default_monkeys;

  // jff 1/24/98 reset play mode to command line spec'd version
  // killough 3/1/98: moved to here
  respawnparm = clrespawnparm;
  fastparm = clfastparm;
  nomonsters = clnomonsters;

  //jff 3/24/98 set startskill from defaultskill in config file, unless
  // it has already been set by a -skill parameter
  if (startskill==sk_none)
    startskill = (skill_t)(defaultskill-1);

  demoplayback = FALSE;
  netdemo = FALSE;

  // killough 2/21/98:
  memset(playeringame+1, 0, sizeof(*playeringame)*(MAXPLAYERS-1));

  consoleplayer = 0;

  compatibility_level = default_compatibility_level;
  {
    int i = M_CheckParm("-complevel");
    if (i && (1+i) < myargc) {
      int l = atoi(myargv[i+1]);;
      if (l >= -1) compatibility_level = l;
    }
  }
  if (compatibility_level == (unsigned) -1)
    compatibility_level = best_compatibility;

  if (mbf_features)
    memcpy(comp, default_comp, sizeof comp);
  G_Compatibility();

  // killough 3/31/98, 4/5/98: demo sync insurance
  demo_insurance = default_demo_insurance == 1;

  rngseed += I_GetRandomTimeSeed() + gametic; // CPhipps
}

void G_DoNewGame (void)
{
  /* a fresh game forgets any hub map archives */
  if (hexen)
    SV_HubInit();
  G_ReloadDefaults();            // killough 3/1/98
  netgame = FALSE;               // killough 3/29/98
  deathmatch = FALSE;
  G_InitNew (d_skill, d_episode, d_map);
  gameaction = ga_nothing;

  //jff 4/26/98 wake up the status bar in case were coming out of a DM demo
  ST_Start();
}

// killough 4/10/98: New function to fix bug which caused Doom
// lockups when idclev was used in conjunction with -fast.

void G_SetFastParms(int fast_pending)
{
  static int fast = 0;            // remembers fast state
  int i;
  if (fast != fast_pending) {     /* only change if necessary */
    /* MBF21: a thing with a "Fast speed" (altspeed) uses it under fast/
     * nightmare.  Swap speed<->altspeed on toggle.  altspeed is
     * NO_ALTSPEED for stock things, so this is inert for vanilla. */
    for (i=0; i<num_mobj_types; i++)
      if (mobjinfo[i].altspeed != NO_ALTSPEED)
      {
        int swap = mobjinfo[i].speed;
        mobjinfo[i].speed = mobjinfo[i].altspeed;
        mobjinfo[i].altspeed = swap;
      }
    if ((fast = fast_pending))
      {
        /* MBF21: halve the tics of every SKILL5FAST frame.  The demon's
         * default states carry the flag (set in D_BuildBEXTables), so this
         * reproduces the vanilla SARG behaviour for stock content while
         * also honouring the flag on MBF21 deh-modified frames. */
        for (i=0; i<num_states; i++)
          if ((states[i].flags & STATEF_SKILL5FAST) &&
              (states[i].tics != 1 || demo_compatibility)) // killough 4/10/98
            states[i].tics >>= 1;  // don't change 1->0 since it causes cycles
        mobjinfo[MT_BRUISERSHOT].speed = 20*FRACUNIT;
        mobjinfo[MT_HEADSHOT].speed = 20*FRACUNIT;
        mobjinfo[MT_TROOPSHOT].speed = 20*FRACUNIT;
      }
    else
      {
        for (i=0; i<num_states; i++)
          if (states[i].flags & STATEF_SKILL5FAST)
            states[i].tics <<= 1;
        mobjinfo[MT_BRUISERSHOT].speed = 15*FRACUNIT;
        mobjinfo[MT_HEADSHOT].speed = 10*FRACUNIT;
        mobjinfo[MT_TROOPSHOT].speed = 10*FRACUNIT;
      }
  }
}

// Since input is read on each frame, the speed of
// turns needs to be scaled. This is called whenever
// the framerate changes.
void G_ScaleMovementToFramerate (void)
{
  angleturn[0] = (320  * TICRATE) / tic_vars.fps;
  angleturn[1] = (640 * TICRATE) / tic_vars.fps;
  angleturn[2] = (160  * TICRATE) / tic_vars.fps;
}

//
//
//
mapentry_t *G_LookupMapinfo(int episode, int map)
{
  char lumpname[9];
  unsigned i;
  if (gamemode == commercial) snprintf(lumpname, 9, "MAP%02u", (unsigned)(map & 0xff));
  else snprintf(lumpname, 9, "E%uM%u", (unsigned)(episode & 0xff), (unsigned)(map & 0xff));
  for (i = 0; i < U_mapinfo.mapcount; i++)
  {
    if (!stricmp(lumpname, U_mapinfo.maps[i].mapname))
    {
      return &U_mapinfo.maps[i];
    }
  }
  return NULL;
}

//
//
//
mapentry_t *G_LookupMapinfoByName(const char *lumpname)
{
  unsigned i;
  for (i = 0; i < U_mapinfo.mapcount; i++)
  {
    if (!stricmp(lumpname, U_mapinfo.maps[i].mapname))
    {
      return &U_mapinfo.maps[i];
    }
  }
  return NULL;
}


int G_ValidateMapName(const char *mapname, int *pEpi, int *pMap)
{
  // Check if the given map name can be expressed as a gameepisode/gamemap pair and be reconstructed from it.
  char lumpname[9], mapuname[9];
  int epi = -1, map = -1;

  if (strlen(mapname) > 8) return 0;
  strncpy(mapuname, mapname, 8);
  mapuname[8] = 0;
  M_Strupr(mapuname);


  /* Hand-rolled parse to avoid sscanf (slow due to format-string
   * parsing overhead, plus internal malloc on musl/older Android/
   * console libcs).  Map names are short and the format is fully
   * constrained: "MAPnn" or "EnMn". */
  {
    const char *p = mapuname;
    char       *end;
    long        v;

    if (p[0] == 'M' && p[1] == 'A' && p[2] == 'P')
    {
      v = strtol(p + 3, &end, 10);
      if (end != p + 3)
      {
        map = (int)v;
        snprintf(lumpname, 9, "MAP%02u", (unsigned)(map & 0xff));
      }
      else return 0;
    }
    else if (p[0] == 'E')
    {
      char *map_start;
      v = strtol(p + 1, &end, 10);
      if (end == p + 1 || *end != 'M')
        return 0;
      epi = (int)v;
      map_start = end + 1;
      v = strtol(map_start, &end, 10);
      if (end == map_start)
        return 0;
      map = (int)v;
      snprintf(lumpname, 9, "E%uM%u", (unsigned)(epi & 0xff), (unsigned)(map & 0xff));
    }
    else return 0;
  }

  if (pEpi) *pEpi = epi;
  if (pMap) *pMap = map;
  return !strcmp(mapuname, lumpname);
}


//
// G_InitNew
// Can be called by the startup code or the menu task,
// consoleplayer, displayplayer, playeringame[] should be set.
//

void G_InitNew(skill_t skill, int episode, int map)
{
  int i;

  /* Hexen: a fresh game must not inherit ACS world variables or deferred
   * cross-map scripts from the previous one.  Hub travel re-enters here for
   * every map transition (sv_save.c), and that must NOT reset ACS world
   * state: world variables and the deferred-script store are exactly the
   * state that persists across maps within a hub. */
  if (hexen && !SV_IsHubTravel())
    P_ACSInitNewGame();

  if (paused)
    {
      paused = FALSE;
      S_ResumeSound();
    }

  if (skill > sk_nightmare)
    skill = sk_nightmare;

  if (episode < 1)
    episode = 1;

  if (heretic)
    {
      /* Heretic is mapped onto the Doom 'registered' gamemode, but it has
       * more than three episodes (the registered/extended set runs E1..E5,
       * and UMAPINFO or extra IWAD content can add more).  The plain Doom
       * clamp below would force any episode past 3 down to 3 -- which made
       * the 6-episode menu start episodes 3..6 all at E3M1.  Clamp instead
       * to the highest episode whose ExM1 map actually exists, mirroring
       * how the episode menu counts available episodes. */
      int last = 1;
      int e;
      for (e = 1; e <= MAX_EPISODE_NUM; e++)
        {
          char mapname[9];
          sprintf(mapname, "E%uM1", (unsigned)e);
          if (W_CheckNumForName(mapname) == -1)
            break;
          last = e;
        }
      if (episode > last)
        episode = last;
    }
  else
  if (gamemode == retail)
    {
      if (episode > MAX_EPISODE_NUM)
        episode = MAX_EPISODE_NUM;
    }
  else
    if (gamemode == shareware)
      {
        if (episode > 1)
          episode = 1; // only start episode 1 on shareware
      }
    else
      if (episode > 3)
        episode = 3;

  if (map < 1)
    map = 1;
  if (map > 9 && gamemode != commercial)
    map = 9;

  G_SetFastParms(fastparm || skill == sk_nightmare);  // killough 4/10/98

  M_ClearRandom();

  respawnmonsters = skill == sk_nightmare || respawnparm;

  // force players to be initialized upon first level load
  for (i=0 ; i<MAXPLAYERS ; i++)
    players[i].playerstate = PST_REBORN;

  usergame = TRUE;                // will be set FALSE if a demo
  paused = FALSE;
  automapmode &= ~am_active;
  gameepisode = episode;
  gamemap = map;
  gameskill = skill;
  gamemapinfo = G_LookupMapinfo(gameepisode, gamemap);

  totalleveltimes = 0; // cph

  //jff 4/16/98 force marks on automap cleared every new level start
  AM_clearMarks();

  G_DoLoadLevel ();
}

//
// DEMO RECORDING
//

#define DEMOMARKER    0x80

void G_ReadDemoTiccmd (ticcmd_t* cmd)
{
  unsigned char at = 0; // e6y: tasdoom stuff

  if (*demo_p == DEMOMARKER)
    G_CheckDemoStatus();      // end of demo data stream
  else if (demoplayback &&
           demo_p + (longtics?5:4) + (raven?2:0) > demobuffer + demolength)
  {
    lprintf(LO_WARN, "G_ReadDemoTiccmd: missing DEMOMARKER\n");
    G_CheckDemoStatus();
  }
  else
  {
    cmd->forwardmove = ((signed char)*demo_p++);
    cmd->sidemove = ((signed char)*demo_p++);
    if (!longtics) {
      cmd->angleturn = ((unsigned char)(at = *demo_p++))<<8;
    } else {
      unsigned int lowbyte = (unsigned char)*demo_p++;
      cmd->angleturn = (((signed int)(*demo_p++))<<8) + lowbyte;
    }
    cmd->buttons = (unsigned char)*demo_p++;
    if (raven)
    {
      /* Raven demo tics carry two extra bytes: look/fly and the artifact
       * used this tic. */
      cmd->lookfly = (unsigned char)*demo_p++;
      cmd->arti    = (unsigned char)*demo_p++;
    }
    else
    {
      /* arti/lookfly are not part of the Doom demo format; clear them so a
       * stale ring-buffer value can't leak into playback. */
      cmd->arti    = 0;
      cmd->lookfly = 0;
    }
    // e6y: ability to play tasdoom demos directly
    if (compatibility_level == tasdoom_compatibility)
    {
      signed char k = cmd->forwardmove;
      cmd->forwardmove = cmd->sidemove;
      cmd->sidemove = (signed char)at;
      cmd->angleturn = ((unsigned char)cmd->buttons)<<8;
      cmd->buttons = (uint8_t)k;
    }
  }
}

// These functions are used to read and write game-specific options in demos
// and savegames so that demo sync is preserved and savegame restoration is
// complete. Not all options (for example "compatibility"), however, should
// be loaded and saved here. It is extremely important to use the same
// positions as before for the variables, so if one becomes obsolete, the
// byte(s) should still be skipped over or padded with 0's.
// Lee Killough 3/1/98

extern int forceOldBsp;

uint8_t *G_WriteOptions(uint8_t *demo_p)
{
  uint8_t *target = demo_p + GAME_OPTION_SIZE;

  *demo_p++ = monsters_remember;  // part of monster AI

  *demo_p++ = variable_friction;  // ice & mud

  *demo_p++ = weapon_recoil;      // weapon recoil

  *demo_p++ = allow_pushers;      // MT_PUSH Things

  *demo_p++ = 0;

  *demo_p++ = player_bobbing;  // whether player bobs or not

  // killough 3/6/98: add parameters to savegame, move around some in demos
  *demo_p++ = respawnparm;
  *demo_p++ = fastparm;
  *demo_p++ = nomonsters;

  *demo_p++ = demo_insurance;        // killough 3/31/98

  // killough 3/26/98: Added rngseed. 3/31/98: moved here
  *demo_p++ = (uint8_t)((rngseed >> 24) & 0xff);
  *demo_p++ = (uint8_t)((rngseed >> 16) & 0xff);
  *demo_p++ = (uint8_t)((rngseed >>  8) & 0xff);
  *demo_p++ = (uint8_t)( rngseed        & 0xff);

  // Options new to v2.03 begin here

  *demo_p++ = monster_infighting;   // killough 7/19/98

  *demo_p++ = 0;

  *demo_p++ = 0;
  *demo_p++ = 0;

  *demo_p++ = (distfriend >> 8) & 0xff;  // killough 8/8/98
  *demo_p++ =  distfriend       & 0xff;  // killough 8/8/98

  *demo_p++ = monster_backing;         // killough 9/8/98

  *demo_p++ = monster_avoid_hazards;    // killough 9/9/98

  *demo_p++ = monster_friction;         // killough 10/98

  *demo_p++ = help_friends;             // killough 9/9/98

  *demo_p++ = 0;

  *demo_p++ = monkeys;

  {   // killough 10/98: a compatibility vector now
    int i;
    for (i=0; i < COMP_TOTAL; i++)
      *demo_p++ = comp[i] != 0;
  }

  *demo_p++ = (compatibility_level >= prboom_2_compatibility) && forceOldBsp; // cph 2002/07/20

  //----------------
  // Padding at end
  //----------------
  while (demo_p < target)
    *demo_p++ = 0;

  if (demo_p != target)
    I_Error("G_WriteOptions: GAME_OPTION_SIZE is too small");

  return target;
}

/* Same, but read instead of write
 * cph - const byte*'s
 */

const uint8_t *G_ReadOptions(const uint8_t *demo_p)
{
  const uint8_t *target = demo_p + GAME_OPTION_SIZE;

  monsters_remember = *demo_p++;

  variable_friction = *demo_p;  // ice & mud
  demo_p++;

  weapon_recoil = *demo_p;       // weapon recoil
  demo_p++;

  allow_pushers = *demo_p;      // MT_PUSH Things
  demo_p++;

  demo_p++;

  player_bobbing = *demo_p;     // whether player bobs or not
  demo_p++;

  // killough 3/6/98: add parameters to savegame, move from demo
  respawnparm = *demo_p++;
  fastparm = *demo_p++;
  nomonsters = *demo_p++;

  demo_insurance = *demo_p++;              // killough 3/31/98

  // killough 3/26/98: Added rngseed to demos; 3/31/98: moved here

  rngseed  = *demo_p++ & 0xff;
  rngseed <<= 8;
  rngseed += *demo_p++ & 0xff;
  rngseed <<= 8;
  rngseed += *demo_p++ & 0xff;
  rngseed <<= 8;
  rngseed += *demo_p++ & 0xff;

  // Options new to v2.03
  if (mbf_features)
    {
      monster_infighting = *demo_p++;   // killough 7/19/98

      demo_p++;

      demo_p += 2;

      distfriend = *demo_p++ << 8;      // killough 8/8/98
      distfriend+= *demo_p++;

      monster_backing = *demo_p++;     // killough 9/8/98

      monster_avoid_hazards = *demo_p++; // killough 9/9/98

      monster_friction = *demo_p++;      // killough 10/98

      help_friends = *demo_p++;          // killough 9/9/98

      demo_p++;

      monkeys = *demo_p++;

      {   // killough 10/98: a compatibility vector now
  int i;
  for (i=0; i < COMP_TOTAL; i++)
    comp[i] = *demo_p++;
      }

      forceOldBsp = *demo_p++; // cph 2002/07/20
    }
  else  /* defaults for versions <= 2.02 */
    {
      /* G_Compatibility will set these */
    }

  G_Compatibility();
  return target;
}

//
// G_PlayDemo
//

static const char *defdemoname;

void G_DeferedPlayDemo (const char* name)
{
  defdemoname = name;
  gameaction = ga_playdemo;
}

static int demolumpnum = -1;

static int G_GetOriginalDoomCompatLevel(int ver)
{
  {
    int lev;
    int i = M_CheckParm("-complevel");
    if (i && (i+1 < myargc))
    {
      lev = atoi(myargv[i+1]);
      if (lev>=0)
        return lev;
    }
  }
  if (ver < 107) return doom_1666_compatibility;
  if (gamemode == retail) return ultdoom_compatibility;
  if (gamemission >= pack_tnt) return finaldoom_compatibility;
  return doom2_19_compatibility;
}

//e6y: Check for overrun
static dbool   CheckForOverrun(const uint8_t *start_p, const uint8_t *current_p, size_t maxsize, size_t size, dbool   failonerror)
{
  size_t pos = current_p - start_p;
  if (pos + size > maxsize)
  {
    if (failonerror)
      I_Error("G_ReadDemoHeader: wrong demo header\n");
    /* I_Error is non-fatal in this core (it logs and returns), so we must
     * still report the overrun to the caller -- otherwise G_ReadDemoHeader
     * keeps reading past the end of the demo buffer and crashes. */
    return TRUE;
  }
  return FALSE;
}

static const uint8_t* G_ReadDemoHeader(const uint8_t *demo_p, size_t size, dbool   failonerror)
{
   skill_t skill;
   int i, episode, map;

   // e6y
   // The local variable should be used instead of demobuffer,
   // because demobuffer can be uninitialized
   const uint8_t *header_p = demo_p;

   const uint8_t *option_p = NULL;      /* killough 11/98 */

   (void)option_p;

   basetic = gametic;  // killough 9/29/98

   // killough 2/22/98, 2/28/98: autodetect old demos and act accordingly.
   // Old demos turn on demo_compatibility => compatibility; new demos load
   // compatibility flag, and other flags as well, as a part of the demo.

   /* Vanilla Hexen demos are versionless: skill, episode, map, then for
    * each of MAXPLAYERS a presence byte and a class byte.  The vvHeretic
    * extension bits some tools put on the first presence byte (0x20
    * -respawn, 0x10 -longtics, 0x02 -nomonsters) are honoured; retail
    * lumps carry a plain 1 there. */
   if (hexen)
   {
#define HEXEN_DEMO_MAXPLAYERS 8
      int i;
      int skill, episode, map;

      if (CheckForOverrun(header_p, demo_p, size, 3 + 2 * HEXEN_DEMO_MAXPLAYERS,
                          failonerror))
         return NULL;

      skill   = *demo_p++;
      episode = *demo_p++;
      map     = *demo_p++;

      longtics = 0;
      deathmatch = FALSE;
      consoleplayer = 0;
      respawnparm = fastparm = nomonsters = FALSE;
      if (*demo_p & 0x20)
         respawnparm = TRUE;
      if (*demo_p & 0x10)
         longtics = 1;
      if (*demo_p & 0x02)
         nomonsters = TRUE;

      for (i = 0; i < HEXEN_DEMO_MAXPLAYERS; i++)
      {
         /* the on-disk header always carries vanilla hexen's 8 player
          * slots, regardless of this engine's MAXPLAYERS */
         if (i < MAXPLAYERS)
         {
            playeringame[i] = (*demo_p++) != 0;
            PlayerClass[i]  = *demo_p++ + 1;
         }
         else
            demo_p += 2;
      }

      if (playeringame[1])
      {
         netgame = TRUE;
         netdemo = TRUE;
      }

      if (gameaction != ga_loadgame)
         G_InitNew(skill, episode, map);

      for (i = 0; i < MAXPLAYERS; i++)
         players[i].cheats = 0;

      return demo_p;
   }

   //e6y: check for overrun
   if (CheckForOverrun(header_p, demo_p, size, 1, failonerror))
      return NULL;

   demover = *demo_p++;
   longtics = 0;

   if (demover == 255) // UMAPINFO flag set
   {
     // Uses UMAPINFO. The real version will be in the second byte.
     // This prepended 255 is here to prevent non-UMAPINFO ports from recognizing the demo.
     demover = *demo_p++;
     if (!U_mapinfo.mapcount)
     {
       I_Error("UMAPINFO not loaded but trying to play a demo recorded with it");
       return NULL;
     }
   }
   else if (U_mapinfo.mapcount)
   {
     I_Error("UMAPINFO loaded but trying to play a demo recorded without it");
     return NULL;
   }

   // e6y
   // Handling of unrecognized demo formats
   // Versions up to 1.2 use a 7-byte header - first byte is a skill level.
   // Versions after 1.2 use a 13-byte header - first byte is a demoversion.
   // BOOM's demoversion starts from 200
   if (!((demover >=   0  && demover <=   4) ||
            (demover >= 104  && demover <= 111) ||
            (demover >= 200  && demover <= 214) ||
            (demover == 221)))
   {
      I_Error("G_ReadDemoHeader: Unknown demo format %d.", demover);
      return NULL; /* I_Error is non-fatal here; bail rather than parse garbage */
   }

   if (demover < 200)     // Autodetect old demos
   {
      if (demover >= 111) longtics = 1;

      // killough 3/2/98: force these variables to be 0 in demo_compatibility

      variable_friction = 0;

      weapon_recoil = 0;

      allow_pushers = 0;

      monster_infighting = 1;           // killough 7/19/98

      monster_backing = 0;              // killough 9/8/98

      monster_avoid_hazards = 0;        // killough 9/9/98

      monster_friction = 0;             // killough 10/98
      help_friends = 0;                 // killough 9/9/98
      monkeys = 0;

      // killough 3/6/98: rearrange to fix savegame bugs (moved fastparm,
      // respawnparm, nomonsters flags to G_LoadOptions()/G_SaveOptions())

      if ((skill=demover) >= 100)         // For demos from versions >= 1.4
      {
         //e6y: check for overrun
         if (CheckForOverrun(header_p, demo_p, size, 8, failonerror))
            return NULL;

         compatibility_level = G_GetOriginalDoomCompatLevel(demover);
         skill = *demo_p++;
         episode = *demo_p++;
         map = *demo_p++;
         deathmatch = *demo_p++;
         respawnparm = *demo_p++;
         fastparm = *demo_p++;
         nomonsters = *demo_p++;
         consoleplayer = *demo_p++;
      }
      else
      {
         //e6y: check for overrun
         if (CheckForOverrun(header_p, demo_p, size, 2, failonerror))
            return NULL;

         compatibility_level = doom_12_compatibility;
         episode = *demo_p++;
         map = *demo_p++;
         deathmatch = respawnparm = fastparm =
            nomonsters = consoleplayer = 0;
      }
      G_Compatibility();
   }
   else    // new versions of demos
   {
      demo_p += 6;               // skip signature;
      switch (demover) {
         case 200: /* BOOM */
         case 201:
            //e6y: check for overrun
            if (CheckForOverrun(header_p, demo_p, size, 1, failonerror))
               return NULL;

            if (!*demo_p++)
               compatibility_level = boom_201_compatibility;
            else
               compatibility_level = boom_compatibility_compatibility;
            break;
         case 202:
            //e6y: check for overrun
            if (CheckForOverrun(header_p, demo_p, size, 1, failonerror))
               return NULL;

            if (!*demo_p++)
               compatibility_level = boom_202_compatibility;
            else
               compatibility_level = boom_compatibility_compatibility;
            break;
         case 203:
            /* LxDoom or MBF - determine from signature
             * cph - load compatibility level */
            switch (*(header_p + 2)) {
               case 'B': /* LxDoom */
                  /* cph - DEMOSYNC - LxDoom demos recorded in compatibility modes support dropped */
                  compatibility_level = lxdoom_1_compatibility;
                  break;
               case 'M':
                  compatibility_level = mbf_compatibility;
                  demo_p++;
                  break;
            }
            break;
         case 210:
            compatibility_level = prboom_2_compatibility;
            demo_p++;
            break;
         case 211:
            compatibility_level = prboom_3_compatibility;
            demo_p++;
            break;
         case 212:
            compatibility_level = prboom_4_compatibility;
            demo_p++;
            break;
         case 213:
            compatibility_level = prboom_5_compatibility;
            demo_p++;
            break;
         case 214:
            compatibility_level = prboom_6_compatibility;
            longtics = 1;
            demo_p++;
            break;
         case 221:
            compatibility_level = mbf21_compatibility;
            longtics = 1;
            demo_p++;
            break;
      }
      //e6y: check for overrun
      if (CheckForOverrun(header_p, demo_p, size, 5, failonerror))
         return NULL;

      skill = *demo_p++;
      episode = *demo_p++;
      map = *demo_p++;
      deathmatch = *demo_p++;
      consoleplayer = *demo_p++;

      /* killough 11/98: save option pointer for below */
      if (mbf_features)
         option_p = demo_p;

      //e6y: check for overrun
      if (CheckForOverrun(header_p, demo_p, size, GAME_OPTION_SIZE, failonerror))
         return NULL;

      demo_p = G_ReadOptions(demo_p);  // killough 3/1/98: Read game options

      if (demover == 200)              // killough 6/3/98: partially fix v2.00 demos
         demo_p += 256-GAME_OPTION_SIZE;
   }

   if (sizeof(comp_lev_str)/sizeof(comp_lev_str[0]) != MAX_COMPATIBILITY_LEVEL)
   {
      I_Error("G_ReadDemoHeader: compatibility level strings incomplete");
      return NULL;
   }
   lprintf(LO_INFO, "G_DoPlayDemo: playing demo with %s compatibility\n",
         comp_lev_str[compatibility_level]);

   if (demo_compatibility)  // only 4 players can exist in old demos
   {
      //e6y: check for overrun
      if (CheckForOverrun(header_p, demo_p, size, 4, failonerror))
         return NULL;

      /* Heretic demos share the doom 1.2 header layout; the vvHeretic
       * extension bits some tools put on the first presence byte (0x20
       * -respawn, 0x10 -longtics, 0x02 -nomonsters) are honoured here.
       * Retail lumps carry a plain 1. */
      if (heretic)
      {
         if (*demo_p & 0x20)
            respawnparm = TRUE;
         if (*demo_p & 0x10)
            longtics = 1;
         if (*demo_p & 0x02)
            nomonsters = TRUE;
      }

      for (i=0; i<4; i++)  // intentionally hard-coded 4 -- killough
         playeringame[i] = *demo_p++;
      for (;i < MAXPLAYERS; i++)
         playeringame[i] = 0;
   }
   else
   {
      //e6y: check for overrun
      if (CheckForOverrun(header_p, demo_p, size, MAXPLAYERS, failonerror))
         return NULL;

      for (i=0 ; i < MAXPLAYERS; i++)
         playeringame[i] = *demo_p++;
      demo_p += MIN_MAXPLAYERS - MAXPLAYERS;
   }

   if (playeringame[1])
   {
      netgame = TRUE;
      netdemo = TRUE;
   }

   if (gameaction != ga_loadgame) { /* killough 12/98: support -loadgame */
      G_InitNew(skill, episode, map);
   }

   for (i=0; i<MAXPLAYERS;i++)         // killough 4/24/98
      players[i].cheats = 0;

   return demo_p;
}

void G_DoPlayDemo(void)
{
  char basename[9];

  ExtractFileBase(defdemoname,basename);           // killough
  basename[8] = 0;

  /* cph - store lump number for unlocking later */
  demolumpnum = W_GetNumForName(basename);
  demobuffer = W_CacheLumpNum(demolumpnum);
  demolength = W_LumpLength(demolumpnum);

  demo_p = G_ReadDemoHeader(demobuffer, demolength, TRUE);

  if (!demo_p)
  {
    /* Invalid / truncated demo header.  G_ReadDemoHeader has already
     * logged the reason.  Don't enter playback with a NULL demo pointer
     * (the ticker would dereference it); release the lump and advance the
     * title-screen demo loop to the next item instead. */
    if (demolumpnum != -1)
    {
      W_UnlockLumpNum(demolumpnum);
      demolumpnum = -1;
    }
    gameaction = ga_nothing;
    demoplayback = FALSE;
    D_AdvanceDemo();
    return;
  }

  gameaction = ga_nothing;
  usergame = FALSE;

  demoplayback = TRUE;
  R_SmoothPlaying_Reset(NULL); // e6y
}

/* G_CheckDemoStatus
 *
 * Called after a death or level completion to allow demos to be cleaned up
 * Returns TRUE if a new demo loop action will take place
 */
dbool   G_CheckDemoStatus (void)
{
  if (demoplayback)
  {
    if (demolumpnum != -1) {
      // cph - unlock the demo lump
      W_UnlockLumpNum(demolumpnum);
      demolumpnum = -1;
    }
    G_ReloadDefaults();    // killough 3/1/98
    netgame = FALSE;       // killough 3/29/98
    deathmatch = FALSE;
    D_AdvanceDemo ();
    return TRUE;
  }
  return FALSE;
}

// killough 1/22/98: this is a "Doom printf" for messages. I've gotten
// tired of using players->message=... and so I've added this dprintf.
//
// killough 3/6/98: Made limit static to allow z_zone functions to call
// this function, without calling realloc(), which seems to cause problems.

#define MAX_MESSAGE_SIZE 1024

// CPhipps - renamed to doom_printf to avoid name collision with glibc
void doom_printf(const char *s, ...)
{
  static char msg[MAX_MESSAGE_SIZE];
  va_list v;
  va_start(v,s);
#ifdef HAVE_VSNPRINTF
  vsnprintf(msg,sizeof(msg),s,v);        /* print message in buffer */
#else
  vsprintf(msg,s,v);
#endif
  va_end(v);
  players[consoleplayer].message = msg;  // set new message
}

/* G_Deinit
 *
 * Reset session-spanning g_game state.  Called from D_DoomDeinit.
 *
 * demolumpnum: cached lump index from G_DoPlayDemo's W_GetNumForName.
 * If a session ends mid-demo (without G_CheckDemoStatus running),
 * this index keeps pointing into the previous wad's lump table.  On
 * next session it would either index wrong content or feed
 * W_UnlockLumpNum a stale index that decrements an unrelated lump's
 * lock count.  Reset to -1 here; the underlying lump table is about
 * to be torn down by W_Exit anyway, so an explicit unlock isn't
 * needed (and would be unsafe with a possibly-stale index).
 *
 * forced_loadgame / command_loadgame: the load-game error paths
 * leave these in whichever state the last load attempt landed
 * them.  Clean them so a fresh session doesn't inherit a "forced"
 * or "from-command-line" flag that no longer applies.
 */
void G_Deinit(void)
{
   demolumpnum     = -1;
   forced_loadgame = FALSE;
   command_loadgame = FALSE;
}
