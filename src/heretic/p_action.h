/* Heretic codepointer declarations.  Bodies (stubs for now) live in
 * heretic/info.c; real implementations land in a later commit. */
#ifndef __HERETIC_P_ACTION__
#define __HERETIC_P_ACTION__

#include "p_mobj.h"
#include "d_player.h"
#include "p_pspr.h"

void A_AccTeleGlitter(mobj_t *actor);
void A_AddPlayerCorpse(mobj_t *actor);
void A_AddPlayerRain(mobj_t *actor);
void A_BeakAttackPL1(player_t *player, pspdef_t *psp);
void A_BeakAttackPL2(player_t *player, pspdef_t *psp);
void A_BeakRaise(player_t *player, pspdef_t *psp);
void A_BeakReady(player_t *player, pspdef_t *psp);
void A_BeastAttack(mobj_t *actor);
void A_BeastPuff(mobj_t *actor);
void A_BlueSpark(mobj_t *actor);
void A_BoltSpark(mobj_t *actor);
void A_CheckBurnGone(mobj_t *actor);
void A_CheckSkullDone(mobj_t *actor);
void A_CheckSkullFloor(mobj_t *actor);
void A_ChicAttack(mobj_t *actor);
void A_ChicChase(mobj_t *actor);
void A_ChicLook(mobj_t *actor);
void A_ChicPain(mobj_t *actor);
void A_ClinkAttack(mobj_t *actor);
void A_ContMobjSound(mobj_t *actor);
void A_DeathBallImpact(mobj_t *actor);
void A_DripBlood(mobj_t *actor);
void A_ESound(mobj_t *actor);
void A_Feathers(mobj_t *actor);
void A_FireBlasterPL1(player_t *player, pspdef_t *psp);
void A_FireBlasterPL2(player_t *player, pspdef_t *psp);
void A_FireCrossbowPL1(player_t *player, pspdef_t *psp);
void A_FireCrossbowPL2(player_t *player, pspdef_t *psp);
void A_FireGoldWandPL1(player_t *player, pspdef_t *psp);
void A_FireGoldWandPL2(player_t *player, pspdef_t *psp);
void A_FireMacePL1(player_t *player, pspdef_t *psp);
void A_FireMacePL2(player_t *player, pspdef_t *psp);
void A_FirePhoenixPL1(player_t *player, pspdef_t *psp);
void A_FirePhoenixPL2(player_t *player, pspdef_t *psp);
void A_FireSkullRodPL1(player_t *player, pspdef_t *psp);
void A_FireSkullRodPL2(player_t *player, pspdef_t *psp);
void A_FlameEnd(mobj_t *actor);
void A_FlameSnd(mobj_t *actor);
void A_FloatPuff(mobj_t *actor);
void A_FreeTargMobj(mobj_t *actor);
void A_GauntletAttack(player_t *player, pspdef_t *psp);
void A_GenWizard(mobj_t *actor);
void A_GhostOff(mobj_t *actor);
void A_HeadFireGrow(mobj_t *actor);
void A_HeadIceImpact(mobj_t *actor);
void A_HideInCeiling(mobj_t *actor);
void A_HideThing(mobj_t *actor);
void A_ImpDeath(mobj_t *actor);
void A_ImpExplode(mobj_t *actor);
void A_ImpMeAttack(mobj_t *actor);
void A_ImpMsAttack(mobj_t *actor);
void A_ImpMsAttack2(mobj_t *actor);
void A_ImpXDeath1(mobj_t *actor);
void A_ImpXDeath2(mobj_t *actor);
void A_InitKeyGizmo(mobj_t *actor);
void A_InitPhoenixPL2(player_t *player, pspdef_t *psp);
void A_KnightAttack(mobj_t *actor);
void A_MaceBallImpact(mobj_t *actor);
void A_MaceBallImpact2(mobj_t *actor);
void A_MacePL1Check(mobj_t *actor);
void A_MakePod(mobj_t *actor);
void A_MinotaurAtk1(mobj_t *actor);
void A_MinotaurAtk2(mobj_t *actor);
void A_MinotaurAtk3(mobj_t *actor);
void A_MinotaurCharge(mobj_t *actor);
void A_MinotaurDecide(mobj_t *actor);
void A_MntrFloorFire(mobj_t *actor);
void A_MummyAttack(mobj_t *actor);
void A_MummyAttack2(mobj_t *actor);
void A_MummyFX1Seek(mobj_t *actor);
void A_MummySoul(mobj_t *actor);
void A_NoBlocking(mobj_t *actor);
void A_PhoenixPuff(mobj_t *actor);
void A_PodPain(mobj_t *actor);
void A_RainImpact(mobj_t *actor);
void A_RemovePod(mobj_t *actor);
void A_RemovedPhoenixFunc(mobj_t *actor);
void A_RestoreArtifact(mobj_t *actor);
void A_RestoreSpecialThing1(mobj_t *actor);
void A_RestoreSpecialThing2(mobj_t *actor);
void A_ShutdownPhoenixPL2(player_t *player, pspdef_t *psp);
void A_SkullPop(mobj_t *actor);
void A_SkullRodPL2Seek(mobj_t *actor);
void A_SkullRodStorm(mobj_t *actor);
void A_SnakeAttack(mobj_t *actor);
void A_SnakeAttack2(mobj_t *actor);
void A_Sor1Chase(mobj_t *actor);
void A_Sor1Pain(mobj_t *actor);
void A_Sor2DthInit(mobj_t *actor);
void A_Sor2DthLoop(mobj_t *actor);
void A_SorDBon(mobj_t *actor);
void A_SorDExp(mobj_t *actor);
void A_SorDSph(mobj_t *actor);
void A_SorRise(mobj_t *actor);
void A_SorSightSnd(mobj_t *actor);
void A_SorZap(mobj_t *actor);
void A_SorcererRise(mobj_t *actor);
void A_SpawnRippers(mobj_t *actor);
void A_SpawnTeleGlitter(mobj_t *actor);
void A_SpawnTeleGlitter2(mobj_t *actor);
void A_Srcr1Attack(mobj_t *actor);
void A_Srcr2Attack(mobj_t *actor);
void A_Srcr2Decide(mobj_t *actor);
void A_StaffAttackPL1(player_t *player, pspdef_t *psp);
void A_StaffAttackPL2(player_t *player, pspdef_t *psp);
void A_UnHideThing(mobj_t *actor);
void A_VolcBallImpact(mobj_t *actor);
void A_VolcanoBlast(mobj_t *actor);
void A_VolcanoSet(mobj_t *actor);
void A_WhirlwindSeek(mobj_t *actor);
void A_WizAtk1(mobj_t *actor);
void A_WizAtk2(mobj_t *actor);
void A_WizAtk3(mobj_t *actor);


/* Heretic codepointer support helpers (heretic/p_action.c). */
int  P_SubRandom(void);
void S_StartMobjSound(mobj_t *mobj, int sfx_id);
void S_StartVoidSound(int sfx_id);
dbool P_SetMobjStateNF(mobj_t *mobj, statenum_t state);
int  P_MobjSpawnHealth(const mobj_t *mobj);
void P_DropItem(mobj_t *source, mobjtype_t type, int special, int chance);
int  P_HitFloor(mobj_t *thing);
void P_Massacre(void);
void P_DSparilTeleport(mobj_t *actor);
dbool P_UpdateChicken(mobj_t *actor, int tics);
dbool P_TestMobjLocation(mobj_t *mobj);
void P_RepositionMace(mobj_t *mo);

int P_GetPlayerNum(player_t *player);
void P_ThrustMobj(mobj_t *mo, angle_t angle, fixed_t move);
dbool P_SeekerMissile(mobj_t *actor, mobj_t **seekTarget, angle_t thresh, angle_t turnMax, dbool seekcenter);
mobj_t *P_SPMAngle(mobj_t *source, mobjtype_t type, angle_t angle);
mobj_t *P_SpawnMissileAngle(mobj_t *source, mobjtype_t type, angle_t angle, fixed_t momz);

#endif
