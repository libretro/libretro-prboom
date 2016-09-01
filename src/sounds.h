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
 *      Created by the sound utility written by Dave Taylor.
 *      Kept as a sample, DOOM2 sounds. Frozen.
 *
 *-----------------------------------------------------------------------------*/

#ifndef __SOUNDS__
#define __SOUNDS__

//
// SoundFX struct.
//

struct sfxinfo_struct;

typedef struct sfxinfo_struct sfxinfo_t;

struct sfxinfo_struct {

  // up to 6-character name
  const char *name; // CPhipps - const

  // Sfx singularity (only one at a time)
  int singularity;

  // Sfx priority
  int priority;

  // referenced sound if a link
  sfxinfo_t *link;

  // pitch if a link
  int pitch;

  // volume if a link
  int volume;

  // sound data
  void *data;

  // this is checked every second to see if sound
  // can be thrown out (if 0, then decrement, if -1,
  // then throw out, if > 0, then it is in use)
  int usefulness;

  // lump number of sfx
  int lumpnum;
};

//
// MusicInfo struct.
//

typedef struct {
  // up to 6-character name
  const char *name; // CPhipps - const

  // lump number of music
  int lumpnum;

  /* music data - cphipps 4/11 made const void* */
  const void *data;

  // music handle once registered
  int handle;
} musicinfo_t;

// the complete set of sound effects
extern sfxinfo_t    S_sfx[];

// the complete set of music
extern musicinfo_t  S_music[];

/* Identifiers for all music in game. */

typedef enum
{
#ifdef HEXEN
   mus_e1m1,
   mus_e1m2,
   mus_e1m3,
   mus_e1m4,
   mus_e1m5,
   mus_e1m6,
   mus_e1m7,
   mus_e1m8,
   mus_e1m9,
   mus_e2m1,
   mus_e2m2,
   mus_e2m3,
   mus_e2m4,
   mus_e2m5,
   mus_e2m6,
   mus_e2m7,
   mus_e2m8,
   mus_e2m9,
   mus_e3m1,
   mus_e3m2,
   mus_e3m3,
   mus_e3m4,
   mus_e3m5,
   mus_e3m6,
   mus_e3m7,
   mus_e3m8,
   mus_e3m9,
   mus_e4m1,
   mus_titl,
   mus_intr,
   mus_cptd,
#else
   mus_None,
   mus_e1m1,
   mus_e1m2,
   mus_e1m3,
   mus_e1m4,
   mus_e1m5,
   mus_e1m6,
   mus_e1m7,
   mus_e1m8,
   mus_e1m9,
   mus_e2m1,
   mus_e2m2,
   mus_e2m3,
   mus_e2m4,
   mus_e2m5,
   mus_e2m6,
   mus_e2m7,
   mus_e2m8,
   mus_e2m9,
   mus_e3m1,
   mus_e3m2,
   mus_e3m3,
   mus_e3m4,
   mus_e3m5,
   mus_e3m6,
   mus_e3m7,
   mus_e3m8,
   mus_e3m9,
   mus_inter,
   mus_intro,
   mus_bunny,
   mus_victor,
   mus_introa,
   mus_runnin,
   mus_stalks,
   mus_countd,
   mus_betwee,
   mus_doom,
   mus_the_da,
   mus_shawn,
   mus_ddtblu,
   mus_in_cit,
   mus_dead,
   mus_stlks2,
   mus_theda2,
   mus_doom2,
   mus_ddtbl2,
   mus_runni2,
   mus_dead2,
   mus_stlks3,
   mus_romero,
   mus_shawn2,
   mus_messag,
   mus_count2,
   mus_ddtbl3,
   mus_ampie,
   mus_theda3,
   mus_adrian,
   mus_messg2,
   mus_romer2,
   mus_tense,
   mus_shawn3,
   mus_openin,
   mus_evil,
   mus_ultima,
   mus_read_m,
   mus_dm2ttl,
   mus_dm2int,
#endif
   NUMMUSIC
} musicenum_t;

/* Identifiers for all sfx in game. */

typedef enum
{
#ifdef HEXEN
   SFX_NONE,
   SFX_PLAYER_FIGHTER_NORMAL_DEATH,		// class specific death screams
   SFX_PLAYER_FIGHTER_CRAZY_DEATH,
   SFX_PLAYER_FIGHTER_EXTREME1_DEATH,
   SFX_PLAYER_FIGHTER_EXTREME2_DEATH,
   SFX_PLAYER_FIGHTER_EXTREME3_DEATH,
   SFX_PLAYER_FIGHTER_BURN_DEATH,
   SFX_PLAYER_CLERIC_NORMAL_DEATH,
   SFX_PLAYER_CLERIC_CRAZY_DEATH,
   SFX_PLAYER_CLERIC_EXTREME1_DEATH,
   SFX_PLAYER_CLERIC_EXTREME2_DEATH,
   SFX_PLAYER_CLERIC_EXTREME3_DEATH,
   SFX_PLAYER_CLERIC_BURN_DEATH,
   SFX_PLAYER_MAGE_NORMAL_DEATH,
   SFX_PLAYER_MAGE_CRAZY_DEATH,
   SFX_PLAYER_MAGE_EXTREME1_DEATH,
   SFX_PLAYER_MAGE_EXTREME2_DEATH,
   SFX_PLAYER_MAGE_EXTREME3_DEATH,
   SFX_PLAYER_MAGE_BURN_DEATH,
   SFX_PLAYER_FIGHTER_PAIN,
   SFX_PLAYER_CLERIC_PAIN,
   SFX_PLAYER_MAGE_PAIN,
   SFX_PLAYER_FIGHTER_GRUNT,
   SFX_PLAYER_CLERIC_GRUNT,
   SFX_PLAYER_MAGE_GRUNT,
   SFX_PLAYER_LAND,
   SFX_PLAYER_POISONCOUGH,
   SFX_PLAYER_FIGHTER_FALLING_SCREAM,	// class specific falling screams
   SFX_PLAYER_CLERIC_FALLING_SCREAM,
   SFX_PLAYER_MAGE_FALLING_SCREAM,
   SFX_PLAYER_FALLING_SPLAT,
   SFX_PLAYER_FIGHTER_FAILED_USE,
   SFX_PLAYER_CLERIC_FAILED_USE,
   SFX_PLAYER_MAGE_FAILED_USE,
   SFX_PLATFORM_START,
   SFX_PLATFORM_STARTMETAL,
   SFX_PLATFORM_STOP,
   SFX_STONE_MOVE,
   SFX_METAL_MOVE,
   SFX_DOOR_OPEN,
   SFX_DOOR_LOCKED,
   SFX_DOOR_METAL_OPEN,
   SFX_DOOR_METAL_CLOSE,
   SFX_DOOR_LIGHT_CLOSE,
   SFX_DOOR_HEAVY_CLOSE,
   SFX_DOOR_CREAK,
   SFX_PICKUP_WEAPON,
   SFX_PICKUP_ARTIFACT,
   SFX_PICKUP_KEY,
   SFX_PICKUP_ITEM,
   SFX_PICKUP_PIECE,
   SFX_WEAPON_BUILD,
   SFX_ARTIFACT_USE,
   SFX_ARTIFACT_BLAST,
   SFX_TELEPORT,
   SFX_THUNDER_CRASH,
   SFX_FIGHTER_PUNCH_MISS,
   SFX_FIGHTER_PUNCH_HITTHING,
   SFX_FIGHTER_PUNCH_HITWALL,
   SFX_FIGHTER_GRUNT,	
   SFX_FIGHTER_AXE_HITTHING,	
   SFX_FIGHTER_HAMMER_MISS,
   SFX_FIGHTER_HAMMER_HITTHING,
   SFX_FIGHTER_HAMMER_HITWALL,
   SFX_FIGHTER_HAMMER_CONTINUOUS,
   SFX_FIGHTER_HAMMER_EXPLODE,
   SFX_FIGHTER_SWORD_FIRE,
   SFX_FIGHTER_SWORD_EXPLODE,
   SFX_CLERIC_CSTAFF_FIRE,
   SFX_CLERIC_CSTAFF_EXPLODE,
   SFX_CLERIC_CSTAFF_HITTHING,
   SFX_CLERIC_FLAME_FIRE,
   SFX_CLERIC_FLAME_EXPLODE,
   SFX_CLERIC_FLAME_CIRCLE,
   SFX_MAGE_WAND_FIRE,
   SFX_MAGE_LIGHTNING_FIRE,
   SFX_MAGE_LIGHTNING_ZAP,
   SFX_MAGE_LIGHTNING_CONTINUOUS,
   SFX_MAGE_LIGHTNING_READY,
   SFX_MAGE_SHARDS_FIRE,
   SFX_MAGE_SHARDS_EXPLODE,
   SFX_MAGE_STAFF_FIRE,
   SFX_MAGE_STAFF_EXPLODE,
   SFX_SWITCH1,
   SFX_SWITCH2,
   SFX_SERPENT_SIGHT,
   SFX_SERPENT_ACTIVE,
   SFX_SERPENT_PAIN,
   SFX_SERPENT_ATTACK,
   SFX_SERPENT_MELEEHIT,
   SFX_SERPENT_DEATH,
   SFX_SERPENT_BIRTH,
   SFX_SERPENTFX_CONTINUOUS,
   SFX_SERPENTFX_HIT,
   SFX_POTTERY_EXPLODE,
   SFX_DRIP,
   SFX_CENTAUR_SIGHT,
   SFX_CENTAUR_ACTIVE,
   SFX_CENTAUR_PAIN,
   SFX_CENTAUR_ATTACK,
   SFX_CENTAUR_DEATH,
   SFX_CENTAURLEADER_ATTACK,
   SFX_CENTAUR_MISSILE_EXPLODE,
   SFX_WIND,
   SFX_BISHOP_SIGHT,
   SFX_BISHOP_ACTIVE,
   SFX_BISHOP_PAIN,
   SFX_BISHOP_ATTACK,
   SFX_BISHOP_DEATH,
   SFX_BISHOP_MISSILE_EXPLODE,
   SFX_BISHOP_BLUR,
   SFX_DEMON_SIGHT,
   SFX_DEMON_ACTIVE,
   SFX_DEMON_PAIN,
   SFX_DEMON_ATTACK,
   SFX_DEMON_MISSILE_FIRE,
   SFX_DEMON_MISSILE_EXPLODE,
   SFX_DEMON_DEATH,
   SFX_WRAITH_SIGHT,
   SFX_WRAITH_ACTIVE,
   SFX_WRAITH_PAIN,
   SFX_WRAITH_ATTACK,
   SFX_WRAITH_MISSILE_FIRE,
   SFX_WRAITH_MISSILE_EXPLODE,
   SFX_WRAITH_DEATH,
   SFX_PIG_ACTIVE1,
   SFX_PIG_ACTIVE2,
   SFX_PIG_PAIN,
   SFX_PIG_ATTACK,
   SFX_PIG_DEATH,
   SFX_MAULATOR_SIGHT,
   SFX_MAULATOR_ACTIVE,
   SFX_MAULATOR_PAIN,
   SFX_MAULATOR_HAMMER_SWING,
   SFX_MAULATOR_HAMMER_HIT,
   SFX_MAULATOR_MISSILE_HIT,
   SFX_MAULATOR_DEATH,
   SFX_FREEZE_DEATH,
   SFX_FREEZE_SHATTER,
   SFX_ETTIN_SIGHT,
   SFX_ETTIN_ACTIVE,
   SFX_ETTIN_PAIN,
   SFX_ETTIN_ATTACK,
   SFX_ETTIN_DEATH,
   SFX_FIRED_SPAWN,
   SFX_FIRED_ACTIVE,
   SFX_FIRED_PAIN,
   SFX_FIRED_ATTACK,
   SFX_FIRED_MISSILE_HIT,
   SFX_FIRED_DEATH,
   SFX_ICEGUY_SIGHT,
   SFX_ICEGUY_ACTIVE,
   SFX_ICEGUY_ATTACK,
   SFX_ICEGUY_FX_EXPLODE,
   SFX_SORCERER_SIGHT,
   SFX_SORCERER_ACTIVE,
   SFX_SORCERER_PAIN,
   SFX_SORCERER_SPELLCAST,
   SFX_SORCERER_BALLWOOSH,
   SFX_SORCERER_DEATHSCREAM,
   SFX_SORCERER_BISHOPSPAWN,
   SFX_SORCERER_BALLPOP,
   SFX_SORCERER_BALLBOUNCE,
   SFX_SORCERER_BALLEXPLODE,
   SFX_SORCERER_BIGBALLEXPLODE,
   SFX_SORCERER_HEADSCREAM,
   SFX_DRAGON_SIGHT,
   SFX_DRAGON_ACTIVE,
   SFX_DRAGON_WINGFLAP,
   SFX_DRAGON_ATTACK,
   SFX_DRAGON_PAIN,
   SFX_DRAGON_DEATH,
   SFX_DRAGON_FIREBALL_EXPLODE,
   SFX_KORAX_SIGHT,
   SFX_KORAX_ACTIVE,
   SFX_KORAX_PAIN,
   SFX_KORAX_ATTACK,
   SFX_KORAX_COMMAND,
   SFX_KORAX_DEATH,
   SFX_KORAX_STEP,
   SFX_THRUSTSPIKE_RAISE,
   SFX_THRUSTSPIKE_LOWER,
   SFX_STAINEDGLASS_SHATTER,
   SFX_FLECHETTE_BOUNCE,
   SFX_FLECHETTE_EXPLODE,
   SFX_LAVA_MOVE,
   SFX_WATER_MOVE,
   SFX_ICE_STARTMOVE,
   SFX_EARTH_STARTMOVE,
   SFX_WATER_SPLASH,
   SFX_LAVA_SIZZLE,
   SFX_SLUDGE_GLOOP,
   SFX_CHOLY_FIRE,
   SFX_SPIRIT_ACTIVE,
   SFX_SPIRIT_ATTACK,
   SFX_SPIRIT_DIE,
   SFX_VALVE_TURN,
   SFX_ROPE_PULL,
   SFX_FLY_BUZZ,
   SFX_IGNITE,
   SFX_PUZZLE_SUCCESS,
   SFX_PUZZLE_FAIL_FIGHTER,
   SFX_PUZZLE_FAIL_CLERIC,
   SFX_PUZZLE_FAIL_MAGE,
   SFX_EARTHQUAKE,
   SFX_BELLRING,
   SFX_TREE_BREAK,
   SFX_TREE_EXPLODE,
   SFX_SUITOFARMOR_BREAK,
   SFX_POISONSHROOM_PAIN,
   SFX_POISONSHROOM_DEATH,
   SFX_AMBIENT1,
   SFX_AMBIENT2,
   SFX_AMBIENT3,
   SFX_AMBIENT4,
   SFX_AMBIENT5,
   SFX_AMBIENT6,
   SFX_AMBIENT7,
   SFX_AMBIENT8,
   SFX_AMBIENT9,
   SFX_AMBIENT10,
   SFX_AMBIENT11,
   SFX_AMBIENT12,
   SFX_AMBIENT13,
   SFX_AMBIENT14,
   SFX_AMBIENT15,
   SFX_STARTUP_TICK,
   SFX_SWITCH_OTHERLEVEL,
   SFX_RESPAWN,
   SFX_KORAX_VOICE_1,
   SFX_KORAX_VOICE_2,
   SFX_KORAX_VOICE_3,
   SFX_KORAX_VOICE_4,
   SFX_KORAX_VOICE_5,
   SFX_KORAX_VOICE_6,
   SFX_KORAX_VOICE_7,
   SFX_KORAX_VOICE_8,
   SFX_KORAX_VOICE_9,
   SFX_BAT_SCREAM,
   SFX_CHAT,
   SFX_MENU_MOVE,
   SFX_CLOCK_TICK,
   SFX_FIREBALL,
   SFX_PUPPYBEAT,
   SFX_MYSTICINCANT,
#else
   sfx_None,
   sfx_pistol,
   sfx_shotgn,
   sfx_sgcock,
   sfx_dshtgn,
   sfx_dbopn,
   sfx_dbcls,
   sfx_dbload,
   sfx_plasma,
   sfx_bfg,
   sfx_sawup,
   sfx_sawidl,
   sfx_sawful,
   sfx_sawhit,
   sfx_rlaunc,
   sfx_rxplod,
   sfx_firsht,
   sfx_firxpl,
   sfx_pstart,
   sfx_pstop,
   sfx_doropn,
   sfx_dorcls,
   sfx_stnmov,
   sfx_swtchn,
   sfx_swtchx,
   sfx_plpain,
   sfx_dmpain,
   sfx_popain,
   sfx_vipain,
   sfx_mnpain,
   sfx_pepain,
   sfx_slop,
   sfx_itemup,
   sfx_wpnup,
   sfx_oof,
   sfx_telept,
   sfx_posit1,
   sfx_posit2,
   sfx_posit3,
   sfx_bgsit1,
   sfx_bgsit2,
   sfx_sgtsit,
   sfx_cacsit,
   sfx_brssit,
   sfx_cybsit,
   sfx_spisit,
   sfx_bspsit,
   sfx_kntsit,
   sfx_vilsit,
   sfx_mansit,
   sfx_pesit,
   sfx_sklatk,
   sfx_sgtatk,
   sfx_skepch,
   sfx_vilatk,
   sfx_claw,
   sfx_skeswg,
   sfx_pldeth,
   sfx_pdiehi,
   sfx_podth1,
   sfx_podth2,
   sfx_podth3,
   sfx_bgdth1,
   sfx_bgdth2,
   sfx_sgtdth,
   sfx_cacdth,
   sfx_skldth,
   sfx_brsdth,
   sfx_cybdth,
   sfx_spidth,
   sfx_bspdth,
   sfx_vildth,
   sfx_kntdth,
   sfx_pedth,
   sfx_skedth,
   sfx_posact,
   sfx_bgact,
   sfx_dmact,
   sfx_bspact,
   sfx_bspwlk,
   sfx_vilact,
   sfx_noway,
   sfx_barexp,
   sfx_punch,
   sfx_hoof,
   sfx_metal,
   sfx_chgun,
   sfx_tink,
   sfx_bdopn,
   sfx_bdcls,
   sfx_itmbk,
   sfx_flame,
   sfx_flamst,
   sfx_getpow,
   sfx_bospit,
   sfx_boscub,
   sfx_bossit,
   sfx_bospn,
   sfx_bosdth,
   sfx_manatk,
   sfx_mandth,
   sfx_sssit,
   sfx_ssdth,
   sfx_keenpn,
   sfx_keendt,
   sfx_skeact,
   sfx_skesit,
   sfx_skeatk,
   sfx_radio,
#endif
   NUMSFX
} sfxenum_t;

#endif
