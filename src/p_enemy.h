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
 *      Enemy thinking, AI.
 *      Action Pointer Functions
 *      that are associated with states/frames.
 *
 *-----------------------------------------------------------------------------*/

#ifndef __P_ENEMY__
#define __P_ENEMY__

/* Hexen corpse queue (p_enemy.c), archived across hub travel. */
#define CORPSEQUEUESIZE 64
extern struct mobj_s *corpseQueue[CORPSEQUEUESIZE];
extern int corpseQueueSlot;

#include "p_mobj.h"

void P_NoiseAlert (mobj_t *target, mobj_t *emmiter);
void P_SpawnBrainTargets(void); /* killough 3/26/98: spawn icon landings */

extern struct brain_s {         /* killough 3/26/98: global state of boss brain */
  int easy, targeton;
} brain;

// ********************************************************************
// Function addresses or Code Pointers
// ********************************************************************
// These function addresses are the Code Pointers that have been
// modified for years by Dehacked enthusiasts.  The new BEX format
// allows more extensive changes (see d_deh.c)

// Doesn't work with g++, needs actionf_p1
void A_Explode(mobj_t *);
void A_Pain(mobj_t *);
void A_PlayerScream(mobj_t *);
void A_Fall(mobj_t *);
void A_XScream(mobj_t *);
dbool P_CheckMeleeRange(mobj_t *actor);
void A_Look(mobj_t *);
void A_Chase(mobj_t *);
void A_FaceTarget(mobj_t *);
/* Hexen monster codepointers. */
void A_EttinAttack(mobj_t *);
void A_LeafSpawn(mobj_t *);
void A_LeafThrust(mobj_t *);
void A_LeafCheck(mobj_t *);
dbool A_SinkMobj(mobj_t *);
dbool A_RaiseMobj(mobj_t *);
void P_SpawnDirt(mobj_t *, fixed_t);
void A_ThrustInitUp(mobj_t *);
void A_ThrustInitDn(mobj_t *);
void A_ThrustRaise(mobj_t *);
void A_ThrustLower(mobj_t *);
void A_ThrustBlock(mobj_t *);
void A_ThrustImpale(mobj_t *);
void A_SoAExplode(mobj_t *);
void A_PoisonBagInit(mobj_t *);
void A_PoisonBagCheck(mobj_t *);
void A_PoisonBagDamage(mobj_t *);
void A_CheckThrowBomb(mobj_t *);
void A_SmBounce(mobj_t *);
void A_BounceCheck(mobj_t *);
void A_NoGravity(mobj_t *);
void A_TeloSpawnA(mobj_t *);
void A_TeloSpawnB(mobj_t *);
void A_TeloSpawnC(mobj_t *);
void A_TeloSpawnD(mobj_t *);
void A_CheckTeleRing(mobj_t *);
void A_BellReset1(mobj_t *);
void A_BellReset2(mobj_t *);
void A_PotteryExplode(mobj_t *);
void A_PotteryChooseBit(mobj_t *);
void A_PotteryCheck(mobj_t *);
void A_QueueCorpse(mobj_t *);
void A_DemonDeath(mobj_t *);
void A_Demon2Death(mobj_t *);
void P_InitCreatureCorpseQueue(void);
void A_FogSpawn(mobj_t *);
void A_FogMove(mobj_t *);
void A_TreeDeath(mobj_t *);
void A_PoisonShroom(mobj_t *);
void A_FloatGib(mobj_t *);
void A_SinkGib(mobj_t *);
void A_DelayGib(mobj_t *);
void A_CorpseBloodDrip(mobj_t *);
void A_CorpseExplode(mobj_t *);
void A_FreezeDeath(mobj_t *);
void A_FreezeDeathChunks(mobj_t *);
void A_IceSetTics(mobj_t *);
void A_IceCheckHeadDone(mobj_t *);
void A_CentaurAttack(mobj_t *);
void A_CentaurDefend(mobj_t *);
void A_SetInvulnerable(mobj_t *);
void A_UnSetInvulnerable(mobj_t *);
void A_SetReflective(mobj_t *);
void A_UnSetReflective(mobj_t *);
void A_WraithMelee(mobj_t *);
void A_WraithMissile(mobj_t *);
void A_WraithInit(mobj_t *);
void A_WraithRaiseInit(mobj_t *);
void A_WraithRaise(mobj_t *);
void A_WraithFX2(mobj_t *);
void A_WraithFX3(mobj_t *);
void A_WraithFX4(mobj_t *);
void A_WraithLook(mobj_t *);
void A_WraithChase(mobj_t *);
void A_CentaurAttack2(mobj_t *);
void A_CentaurDropStuff(mobj_t *);
void A_SetShootable(mobj_t *);
void A_UnSetShootable(mobj_t *);
void A_SetAltShadow(mobj_t *);
void A_SpeedFade(mobj_t *);
void A_DropMace(mobj_t *);
void A_CheckFloor(mobj_t *);
void A_BridgeInit(mobj_t *);
void A_BridgeOrbit(mobj_t *);
void A_FlameCheck(mobj_t *);
void A_BatSpawnInit(mobj_t *);
void A_BatSpawn(mobj_t *);
void A_BatMove(mobj_t *);
void A_IceGuyLook(mobj_t *);
void A_IceGuyChase(mobj_t *);
void A_IceGuyAttack(mobj_t *);
void A_IceGuyMissilePuff(mobj_t *);
void A_IceGuyDie(mobj_t *);
void A_IceGuyMissileExplode(mobj_t *);
void A_SorcUpdateBallAngle(mobj_t *);
void A_AccelBalls(mobj_t *);
void A_DecelBalls(mobj_t *);
void A_SpeedBalls(mobj_t *);
void A_SlowBalls(mobj_t *);
void A_StopBalls(mobj_t *);
void A_SorcOffense1(mobj_t *);
void A_SorcOffense2(mobj_t *);
void A_CastSorcererSpell(mobj_t *);
void A_SorcSpinBalls(mobj_t *);
void A_SorcBallOrbit(mobj_t *);
void A_SorcBossAttack(mobj_t *);
void A_SpawnFizzle(mobj_t *);
void A_SorcFX1Seek(mobj_t *);
void A_SorcFX2Split(mobj_t *);
void A_SorcFX2Orbit(mobj_t *);
void A_SpawnBishop(mobj_t *);
void A_SorcererBishopEntry(mobj_t *);
void A_SorcFX4Check(mobj_t *);
void A_SorcBallPop(mobj_t *);
void A_DemonAttack1(mobj_t *);
void A_DemonAttack2(mobj_t *);
void A_SerpentChase(mobj_t *);
void A_SerpentWalk(mobj_t *);
void A_SerpentHumpDecide(mobj_t *);
void A_SerpentCheckForAttack(mobj_t *);
void A_SerpentChooseAttack(mobj_t *);
void A_SerpentMeleeAttack(mobj_t *);
void A_SerpentMissileAttack(mobj_t *);
void A_SerpentHide(mobj_t *);
void A_SerpentUnHide(mobj_t *);
void A_SerpentRaiseHump(mobj_t *);
void A_SerpentLowerHump(mobj_t *);
void A_SerpentBirthScream(mobj_t *);
void A_SerpentDiveSound(mobj_t *);
void A_SerpentHeadPop(mobj_t *);
void A_SerpentHeadCheck(mobj_t *);
void A_SerpentSpawnGibs(mobj_t *);
void A_FiredSpawnRock(mobj_t *);
void A_FiredRocks(mobj_t *);
void A_FiredChase(mobj_t *);
void A_FiredAttack(mobj_t *);
void A_FiredSplotch(mobj_t *);
void A_BishopAttack(mobj_t *);
void A_BishopAttack2(mobj_t *);
void A_BishopChase(mobj_t *);
void A_BishopDecide(mobj_t *);
void A_BishopDoBlur(mobj_t *);
void A_BishopSpawnBlur(mobj_t *);
void A_BishopPuff(mobj_t *);
void A_BishopPainBlur(mobj_t *);
void A_BishopMissileSeek(mobj_t *);
void A_BishopMissileWeave(mobj_t *);
void A_MinotaurFade0(mobj_t *);
void A_MinotaurFade1(mobj_t *);
void A_MinotaurFade2(mobj_t *);
void A_MinotaurLook(mobj_t *);
void A_MinotaurRoam(mobj_t *);
void A_MinotaurChase(mobj_t *);
void A_SmokePuffExit(mobj_t *);
void A_Summon(mobj_t *);
void A_PosAttack(mobj_t *);
void A_Scream(mobj_t *);
void A_SPosAttack(mobj_t *);
void A_VileChase(mobj_t *);
/* MBF21 thing codepointers */
void A_SpawnObject(mobj_t *);
void A_MonsterProjectile(mobj_t *);
void A_MonsterMeleeAttack(mobj_t *);
void A_RadiusDamage(mobj_t *);
void A_NoiseAlert(mobj_t *);
void A_HealChase(mobj_t *);
void A_SeekTracer(mobj_t *);
void A_FindTracer(mobj_t *);
void A_ClearTracer(mobj_t *);
void A_JumpIfHealthBelow(mobj_t *);
void A_JumpIfTargetInSight(mobj_t *);
void A_JumpIfTargetCloser(mobj_t *);
void A_JumpIfTracerInSight(mobj_t *);
void A_JumpIfTracerCloser(mobj_t *);
void A_JumpIfFlagsSet(mobj_t *);
void A_AddFlags(mobj_t *);
void A_RemoveFlags(mobj_t *);
void A_VileStart(mobj_t *);
void A_VileTarget(mobj_t *);
void A_VileAttack(mobj_t *);
void A_StartFire(mobj_t *);
void A_Fire(mobj_t *);
void A_FireCrackle(mobj_t *);
void A_Tracer(mobj_t *);
void A_SkelWhoosh(mobj_t *);
void A_SkelFist(mobj_t *);
void A_SkelMissile(mobj_t *);
void A_FatRaise(mobj_t *);
void A_FatAttack1(mobj_t *);
void A_FatAttack2(mobj_t *);
void A_FatAttack3(mobj_t *);
void A_BossDeath(mobj_t *);
void A_CPosAttack(mobj_t *);
void A_CPosRefire(mobj_t *);
void A_TroopAttack(mobj_t *);
void A_SargAttack(mobj_t *);
void A_HeadAttack(mobj_t *);
void A_BruisAttack(mobj_t *);
void A_SkullAttack(mobj_t *);
void A_Metal(mobj_t *);
void A_SpidRefire(mobj_t *);
void A_BabyMetal(mobj_t *);
void A_BspiAttack(mobj_t *);
void A_Hoof(mobj_t *);
void A_CyberAttack(mobj_t *);
void A_PainAttack(mobj_t *);
void A_PainDie(mobj_t *);
void A_KeenDie(mobj_t *);
void A_BrainPain(mobj_t *);
void A_BrainScream(mobj_t *);
void A_BrainDie(mobj_t *);
void A_BrainAwake(mobj_t *);
void A_BrainSpit(mobj_t *);
void A_SpawnSound(mobj_t *);
void A_SpawnFly(mobj_t *);
void A_BrainExplode(mobj_t *);
void A_Die(mobj_t *);
void A_Detonate(mobj_t *);        /* killough 8/9/98: detonate a bomb or other device */
void A_Mushroom(mobj_t *);        /* killough 10/98: mushroom effect */
void A_Spawn(mobj_t *);           // killough 11/98
void A_Turn(mobj_t *);            // killough 11/98
void A_Face(mobj_t *);            // killough 11/98
void A_Scratch(mobj_t *);         // killough 11/98
void A_PlaySound(mobj_t *);       // killough 11/98
void A_RandomJump(mobj_t *);      // killough 11/98
void A_LineEffect(mobj_t *);      // killough 11/98

void A_BetaSkullAttack(void *); // killough 10/98: beta lost souls attacked different
void A_Stop(void *);

#endif // __P_ENEMY__
