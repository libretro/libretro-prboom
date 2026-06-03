/* Heretic codepointer support helpers, adapted from dsda-doom to this
 * core.  Where dsda relies on subsystems this core does not have (the
 * skill_info health-scaling table, the terrain/floor-type splash system,
 * Hexen branches, and the boss-spot list), the helper is reduced to the
 * behaviour Heretic needs without them.  Inert for Doom. */

#include <stdlib.h>
#include "doomtype.h"
#include "doomdef.h"
#include "doomstat.h"
#include "m_fixed.h"
#include "tables.h"
#include "m_random.h"
#include "p_mobj.h"
#include "p_maputl.h"
#include "p_map.h"
#include "p_tick.h"
#include "s_sound.h"
#include "sounds.h"
#include "info.h"
#include "r_main.h"
#include "p_enemy.h"
#include "p_spec.h"
#include "p_inter.h"
#include "heretic/p_action.h"

extern int bodyqueslot, bodyquesize;
extern mobj_t **bodyque;

/* Floor-type return values used by P_HitFloor (terrain splashes are not
 * implemented here, so it always reports a solid floor). */
/* Heretic ammo types (separate space from this core's Doom AM_* enum). */

/* Heretic per-shot ammo costs. */
#define USE_GWND_AMMO_1 1
#define USE_GWND_AMMO_2 1
#define USE_CBOW_AMMO_1 1
#define USE_CBOW_AMMO_2 1
#define USE_BLSR_AMMO_1 1
#define USE_BLSR_AMMO_2 5
#define USE_SKRD_AMMO_1 1
#define USE_SKRD_AMMO_2 5
#define USE_PHRD_AMMO_1 1
#define USE_PHRD_AMMO_2 1
#define USE_MACE_AMMO_1 1
#define USE_MACE_AMMO_2 5


#define TELEFOGHEIGHT (32*FRACUNIT)

/* Heretic game-parameter constants.  dsda carries these as run-time
 * "g_*" globals selected per game; since these codepointers are Heretic-
 * only, the Heretic values are inlined here as constants. */
#define g_skullpop_mt        HERETIC_MT_BLOODYSKULL
#define g_sfx_respawn        heretic_sfx_respawn
/* The Minotaur codepointers below are shared between the Heretic Maulotaur
 * and the Hexen Dark Servant minotaur, so unlike the rest of this file they
 * must honour the real 'hexen' flag (the file-wide "#define hexen 0" is
 * #undef'd around the Minotaur block and restored after it).  Each selector
 * resolves to the Hexen value when running Hexen and the Heretic value
 * otherwise, matching dsda-doom's per-game g_mntr_* globals. */
#define g_mntr_charge_puff   (hexen ? HEXEN_MT_PUNCHPUFF : HERETIC_MT_PHOENIXPUFF)
#define g_mntr_charge_rng    (hexen ? 230 : 150)
#define g_mntr_charge_speed  (hexen ? (23 * FRACUNIT) : (13 * FRACUNIT))
#define g_mntr_charge_state  (hexen ? HEXEN_S_MNTR_ATK4_1 : HERETIC_S_MNTR_ATK4_1)
#define g_mntr_decide_range  (hexen ? 16 : 8)
#define g_mntr_fire          (hexen ? HEXEN_MT_MNTRFX3 : HERETIC_MT_MNTRFX3)
#define g_mntr_fire_rng      (hexen ? 100 : 220)
#define g_mntr_fire_state    (hexen ? HEXEN_S_MNTR_ATK3_1 : HERETIC_S_MNTR_ATK3_1)
#define g_mntr_atk1_sfx      (hexen ? hexen_sfx_maulator_hammer_swing : heretic_sfx_stfpow)
#define g_mntr_atk2_sfx      (hexen ? hexen_sfx_maulator_hammer_swing : heretic_sfx_minat2)
#define g_mntr_atk2_dice     (hexen ? 3 : 5)
#define g_mntr_atk2_missile  (hexen ? HEXEN_MT_MNTRFX1 : HERETIC_MT_MNTRFX1)
#define g_mntr_atk3_sfx      (hexen ? hexen_sfx_maulator_hammer_hit : heretic_sfx_minat1)
#define g_mntr_atk3_dice     (hexen ? 3 : 5)
#define g_mntr_atk3_missile  (hexen ? HEXEN_MT_MNTRFX2 : HERETIC_MT_MNTRFX2)
#define g_mntr_atk3_state    (hexen ? HEXEN_S_MNTR_ATK3_4 : HERETIC_S_MNTR_ATK3_4)

/* Heretic state aliases (game-agnostic in dsda; constant here). */
#define g_s_bloodyskullx1    HERETIC_S_BLOODYSKULLX1
#define g_s_bloodyskullx2    HERETIC_S_BLOODYSKULLX2
#define g_s_play_fdth20      HERETIC_S_PLAY_FDTH20

/* One degree in angle_t units (dsda's ANG1_X). */
#define ANG1_X 0x01000000
#define HITDICE(a) ((1 + (P_Random(pr_heretic) & 7)) * (a))
#define MAGIC_JUNK 1234
#define MAX_GEN_PODS 16
#define BF_DAMAGESOURCE 0x01

/* Boss-spot list is not present in this core; no spots => count 0. */
#define BossSpotCount 0

/* This core is Heretic-only; the Hexen branches in the ported codepointers
 * are dead.  Define 'hexen' as 0 so they fold out, and alias the Hexen-only
 * identifiers that appear inside those branches to keep them compilable. */
#define hexen 0
#define hexen_sfx_drip                       0
#define hexen_sfx_earthquake                 0
#define hexen_sfx_wind                       0
#define hexen_sfx_serpentfx_continuous       0
#define hexen_sfx_fighter_hammer_continuous  0


int P_SubRandom(void)
{
  int r = P_Random(pr_heretic);
  return r - P_Random(pr_heretic);
}

void S_StartMobjSound(mobj_t *mobj, int sfx_id)
{
  /* dsda also suppresses sound in SECF_SILENT sectors; this core has no
   * such sector flag, so just play the sound. */
  S_StartSound(mobj, sfx_id);
}

void S_StartVoidSound(int sfx_id)
{
  S_StartSound(NULL, sfx_id);
}

dbool P_SetMobjStateNF(mobj_t *mobj, statenum_t state)
{
  state_t *st;

  if (state == S_NULL)
  {
    mobj->state = NULL;
    P_RemoveMobj(mobj);
    return false;
  }
  st = &states[state];
  mobj->state = st;
  mobj->tics = st->tics;
  mobj->sprite = st->sprite;
  mobj->frame = st->frame;
  return true;
}

int P_MobjSpawnHealth(const mobj_t *mobj)
{
  /* dsda scales by skill_info health factors; this core has no such table,
   * so use the type's base spawn health. */
  return mobj->info->spawnhealth;
}

void P_DropItem(mobj_t *source, mobjtype_t type, int special, int chance)
{
  mobj_t *mo;

  if (P_Random(pr_heretic) > chance)
    return;

  mo = P_SpawnMobj(source->x, source->y,
                   source->z + (source->height >> 1), type);
  mo->momx = P_SubRandom() << 8;
  mo->momy = P_SubRandom() << 8;
  mo->momz = FRACUNIT * 5 + (P_Random(pr_heretic) << 10);
  mo->flags |= MF_DROPPED;
  mo->health = special;
}

int P_HitFloor(mobj_t *thing)
{
  /* Report the floor terrain type so callers can take liquid-specific
   * behaviour (the Serpent's surface/dive logic, fire/ice monsters, sprite
   * floorclipping).  Terrain splash effect mobjs are not spawned yet. */
  return P_GetThingFloorType(thing);
}

void P_Massacre(void)
{
  mobj_t *mo;
  thinker_t *think;

  for (think = thinkercap.next; think != &thinkercap; think = think->next)
  {
    if (think->function.arg0 != (arg0_t)P_MobjThinker)
      continue;
    mo = (mobj_t *) think;
    if ((mo->flags & MF_COUNTKILL) && (mo->health > 0))
    {
      mo->flags2 &= ~MF2_INVULNERABLE;
      P_DamageMobj(mo, NULL, NULL, 10000);
    }
  }
}

void P_DSparilTeleport(mobj_t *actor)
{
  /* The boss-spot list is not present in this core yet; without spots the
   * teleport is a no-op (matches dsda's "no spots" early return). */
  (void)actor;
}

dbool P_UpdateChicken(mobj_t *actor, int tics)
{
  mobj_t *fog;
  fixed_t x, y, z;
  mobjtype_t moType;
  mobj_t *mo;
  mobj_t oldChicken;

  actor->special1.i -= tics;
  if (actor->special1.i > 0)
    return false;

  moType = actor->special2.i;
  x = actor->x;
  y = actor->y;
  z = actor->z;
  oldChicken = *actor;
  P_SetMobjState(actor, HERETIC_S_FREETARGMOBJ);
  mo = P_SpawnMobj(x, y, z, moType);
  if (P_TestMobjLocation(mo) == false)
  {                             /* Didn't fit */
    P_RemoveMobj(mo);
    mo = P_SpawnMobj(x, y, z, HERETIC_MT_CHICKEN);
    mo->angle = oldChicken.angle;
    mo->flags = oldChicken.flags;
    mo->health = oldChicken.health;
    P_SetTarget(&mo->target, oldChicken.target);
    mo->special1.i = 5 * 35;    /* Next try in 5 seconds */
    mo->special2.i = moType;
    return false;
  }
  mo->angle = oldChicken.angle;
  P_SetTarget(&mo->target, oldChicken.target);
  fog = P_SpawnMobj(x, y, z + TELEFOGHEIGHT, HERETIC_MT_TFOG);
  S_StartMobjSound(fog, heretic_sfx_telept);
  return true;
}

dbool P_TestMobjLocation(mobj_t *mobj)
{
  int flags;

  flags = mobj->flags;
  mobj->flags &= ~MF_PICKUP;
  if (P_CheckPosition(mobj, mobj->x, mobj->y))
  {                             /* XY is ok, now check Z */
    mobj->flags = flags;
    if ((mobj->z < mobj->floorz)
        || (mobj->z + mobj->height > mobj->ceilingz))
      return false;             /* Bad Z */
    return true;
  }
  mobj->flags = flags;
  return false;
}

/* ===== Heretic codepointers (ported from dsda-doom) ===== */
void A_AccTeleGlitter(mobj_t * actor)
{
    if (++actor->health > 35)
    {
        actor->momz += actor->momz / 2;
    }
}


void A_AddPlayerCorpse(mobj_t * actor)
{
  if (bodyquesize > 0)
  {
    static int queuesize;

    if (queuesize < bodyquesize)
  	{
  	  bodyque = realloc(bodyque, bodyquesize * sizeof(*bodyque));
  	  memset(bodyque + queuesize, 0, (bodyquesize - queuesize) * sizeof(*bodyque));
  	  queuesize = bodyquesize;
  	}

    if (bodyqueslot >= bodyquesize)
  	  P_RemoveMobj(bodyque[bodyqueslot % bodyquesize]);

    bodyque[bodyqueslot++ % bodyquesize] = actor;
  }
  else if (!bodyquesize)
    P_RemoveMobj(actor);
}


void A_AddPlayerRain(mobj_t * actor)
{
    int playerNum;
    player_t *player;

    playerNum = netgame ? actor->special2.i : 0;
    if (!playeringame[playerNum])
    {                           // Player left the game
        return;
    }
    player = &players[playerNum];
    if (player->health <= 0)
    {                           // Player is dead
        return;
    }
    if (player->rain1 && player->rain2)
    {                           // Terminate an active rain
        if (player->rain1->health < player->rain2->health)
        {
            if (player->rain1->health > 16)
            {
                player->rain1->health = 16;
            }
            player->rain1 = NULL;
        }
        else
        {
            if (player->rain2->health > 16)
            {
                player->rain2->health = 16;
            }
            player->rain2 = NULL;
        }
    }
    // Add rain mobj to list
    if (player->rain1)
    {
        player->rain2 = actor;
    }
    else
    {
        player->rain1 = actor;
    }
}


void A_BeastAttack(mobj_t * actor)
{
    if (!actor->target)
    {
        return;
    }
    S_StartMobjSound(actor, actor->info->attacksound);
    if (P_CheckMeleeRange(actor))
    {
        P_DamageMobj(actor->target, actor, actor, HITDICE(3));
        return;
    }
    P_SpawnMissile(actor, actor->target, HERETIC_MT_BEASTBALL);
}


void A_BeastPuff(mobj_t * actor)
{
    if (P_Random(pr_heretic) > 64)
    {
        int r1,r2,r3;
        r1 = P_SubRandom();
        r2 = P_SubRandom();
        r3 = P_SubRandom();
        P_SpawnMobj(actor->x + (r3 << 10),
                    actor->y + (r2 << 10),
                    actor->z + (r1 << 10), HERETIC_MT_PUFFY);
    }
}


void A_BlueSpark(mobj_t * actor)
{
    int i;
    mobj_t *mo;

    for (i = 0; i < 2; i++)
    {
        mo = P_SpawnMobj(actor->x, actor->y, actor->z, HERETIC_MT_SOR2FXSPARK);
        mo->momx = P_SubRandom() << 9;
        mo->momy = P_SubRandom() << 9;
        mo->momz = FRACUNIT + (P_Random(pr_heretic) << 8);
    }
}


void A_BoltSpark(mobj_t * bolt)
{
    mobj_t *spark;

    if (P_Random(pr_heretic) > 50)
    {
        spark = P_SpawnMobj(bolt->x, bolt->y, bolt->z, HERETIC_MT_CRBOWFX4);
        spark->x += P_SubRandom() << 10;
        spark->y += P_SubRandom() << 10;
    }
}


void A_CheckBurnGone(mobj_t * actor)
{
    if (actor->special2.i == 666)
    {
        P_SetMobjState(actor, g_s_play_fdth20);
    }
}


void A_CheckSkullDone(mobj_t * actor)
{
    if (actor->special2.i == 666)
    {
        P_SetMobjState(actor, g_s_bloodyskullx2);
    }
}


void A_CheckSkullFloor(mobj_t * actor)
{
    if (actor->z <= actor->floorz)
    {
        P_SetMobjState(actor, g_s_bloodyskullx1);
        if (hexen)
          S_StartMobjSound(actor, hexen_sfx_drip);
    }
}


void A_ChicAttack(mobj_t * actor)
{
    if (P_UpdateChicken(actor, 18))
    {
        return;
    }
    if (!actor->target)
    {
        return;
    }
    if (P_CheckMeleeRange(actor))
    {
        P_DamageMobj(actor->target, actor, actor, 1 + (P_Random(pr_heretic) & 1));
    }
}


void A_ChicChase(mobj_t * actor)
{
    if (P_UpdateChicken(actor, 3))
    {
        return;
    }
    A_Chase(actor);
}


void A_ChicLook(mobj_t * actor)
{
    if (P_UpdateChicken(actor, 10))
    {
        return;
    }
    A_Look(actor);
}


void A_ChicPain(mobj_t * actor)
{
    if (P_UpdateChicken(actor, 10))
    {
        return;
    }
    S_StartMobjSound(actor, actor->info->painsound);
}


void A_ClinkAttack(mobj_t * actor)
{
    int damage;

    if (!actor->target)
    {
        return;
    }
    S_StartMobjSound(actor, actor->info->attacksound);
    if (P_CheckMeleeRange(actor))
    {
        damage = ((P_Random(pr_heretic) % 7) + 3);
        P_DamageMobj(actor->target, actor, actor, damage);
    }
}


void A_DeathBallImpact(mobj_t * ball)
{
    int i;
    mobj_t *target;
    angle_t angle;
    dbool newAngle;

    if ((ball->z <= ball->floorz) && (P_HitFloor(ball) != FLOOR_SOLID))
    {                           // Landed in some sort of liquid
        P_RemoveMobj(ball);
        return;
    }
    if ((ball->z <= ball->floorz) && ball->momz)
    {                           // Bounce
        newAngle = false;
        target = (mobj_t *) ball->special1.m;
        if (target)
        {
            if (!(target->flags & MF_SHOOTABLE))
            {                   // Target died
                P_SetTarget(&ball->special1.m, NULL);
            }
            else
            {                   // Seek
                angle = R_PointToAngle2(ball->x, ball->y,
                                        target->x, target->y);
                newAngle = true;
            }
        }
        else
        {                       // Find new target
            angle = 0;
            for (i = 0; i < 16; i++)
            {
                P_AimLineAttack(ball, angle, 10 * 64 * FRACUNIT, 0);
                if (linetarget && ball->target != linetarget)
                {
                    P_SetTarget(&ball->special1.m, linetarget);
                    angle = R_PointToAngle2(ball->x, ball->y,
                                            linetarget->x, linetarget->y);
                    newAngle = true;
                    break;
                }
                angle += ANG45 / 2;
            }
        }
        if (newAngle)
        {
            ball->angle = angle;
            angle >>= ANGLETOFINESHIFT;
            ball->momx = FixedMul(ball->info->speed, finecosine[angle]);
            ball->momy = FixedMul(ball->info->speed, finesine[angle]);
        }
        P_SetMobjState(ball, ball->info->spawnstate);
        S_StartMobjSound(ball, heretic_sfx_pstop);
    }
    else
    {                           // Explode
        ball->flags |= MF_NOGRAVITY;
        ball->flags2 &= ~MF2_LOGRAV;
        S_StartMobjSound(ball, heretic_sfx_phohit);
    }
}


void A_DripBlood(mobj_t * actor)
{
    mobj_t *mo;
    int r1,r2;

    r1 = P_SubRandom();
    r2 = P_SubRandom();

    mo = P_SpawnMobj(actor->x + (r2 << 11),
                     actor->y + (r1 << 11), actor->z,
                     HERETIC_MT_BLOOD);
    mo->momx = P_SubRandom() << 10;
    mo->momy = P_SubRandom() << 10;
    mo->flags2 |= MF2_LOGRAV;
}


void A_ESound(mobj_t * mo)
{
    int sound = heretic_sfx_None;

    switch (mo->type)
    {
        case HERETIC_MT_SOUNDWATERFALL:
            sound = heretic_sfx_waterfl;
            break;
        case HERETIC_MT_SOUNDWIND:
            sound = heretic_sfx_wind;
            break;
        default:
            break;
    }
    S_StartMobjSound(mo, sound);
}


void A_Feathers(mobj_t * actor)
{
    int i;
    int count;
    mobj_t *mo;

    if (actor->health > 0)
    {                           // Pain
        count = P_Random(pr_heretic) < 32 ? 2 : 1;
    }
    else
    {                           // Death
        count = 5 + (P_Random(pr_heretic) & 3);
    }
    for (i = 0; i < count; i++)
    {
        mo = P_SpawnMobj(actor->x, actor->y, actor->z + 20 * FRACUNIT,
                         HERETIC_MT_FEATHER);
        P_SetTarget(&mo->target, actor);
        mo->momx = P_SubRandom() << 8;
        mo->momy = P_SubRandom() << 8;
        mo->momz = FRACUNIT + (P_Random(pr_heretic) << 9);
        P_SetMobjState(mo, HERETIC_S_FEATHER1 + (P_Random(pr_heretic) & 7));
    }
}


void A_FlameEnd(mobj_t * actor)
{
    actor->momz += (fixed_t)(1.5 * FRACUNIT);
}


void A_FlameSnd(mobj_t * actor)
{
    S_StartMobjSound(actor, heretic_sfx_hedat1);    // Burn sound
}


void A_FloatPuff(mobj_t * puff)
{
    puff->momz += (fixed_t)(1.8 * FRACUNIT);
}


void A_FreeTargMobj(mobj_t * mo)
{
    mo->momx = mo->momy = mo->momz = 0;
    mo->z = mo->ceilingz + 4 * FRACUNIT;
    mo->flags &= ~(MF_SHOOTABLE | MF_FLOAT | MF_SKULLFLY | MF_SOLID);
    mo->flags |= MF_CORPSE | MF_DROPOFF | MF_NOGRAVITY;
    mo->flags2 &= ~(MF2_PASSMOBJ | MF2_LOGRAV);
    mo->player = NULL;

    // hexen_note: can we do this in heretic too?
    if (hexen)
    {
      mo->flags &= ~(MF_COUNTKILL);
      mo->flags2 |= MF2_DONTDRAW;
      mo->health = -1000;         // Don't resurrect
    }
}


void A_GenWizard(mobj_t * actor)
{
    mobj_t *mo;
    mobj_t *fog;

    mo = P_SpawnMobj(actor->x, actor->y,
                     actor->z - mobjinfo[HERETIC_MT_WIZARD].height / 2, HERETIC_MT_WIZARD);
    if (P_TestMobjLocation(mo) == false)
    {                           // Didn't fit
        P_RemoveMobj(mo);
        return;
    }
    actor->momx = actor->momy = actor->momz = 0;
    P_SetMobjState(actor, mobjinfo[actor->type].deathstate);
    actor->flags &= ~MF_MISSILE;
    fog = P_SpawnMobj(actor->x, actor->y, actor->z, HERETIC_MT_TFOG);
    S_StartMobjSound(fog, heretic_sfx_telept);
}


void A_GhostOff(mobj_t * actor)
{
    actor->flags &= ~MF_SHADOW;
}


void A_HeadFireGrow(mobj_t * fire)
{
    fire->health--;
    fire->z += 9 * FRACUNIT;
    if (fire->health == 0)
    {
        fire->damage = fire->info->damage;
        P_SetMobjState(fire, HERETIC_S_HEADFX3_4);
    }
}


void A_HeadIceImpact(mobj_t * ice)
{
    unsigned int i;
    angle_t angle;
    mobj_t *shard;

    for (i = 0; i < 8; i++)
    {
        shard = P_SpawnMobj(ice->x, ice->y, ice->z, HERETIC_MT_HEADFX2);
        angle = i * ANG45;
        P_SetTarget(&shard->target, ice->target);
        shard->angle = angle;
        angle >>= ANGLETOFINESHIFT;
        shard->momx = FixedMul(shard->info->speed, finecosine[angle]);
        shard->momy = FixedMul(shard->info->speed, finesine[angle]);
        shard->momz = (fixed_t)(-.6 * FRACUNIT);
        P_CheckMissileSpawn(shard);
    }
}


void A_HideInCeiling(mobj_t * actor)
{
    actor->z = actor->ceilingz + 4 * FRACUNIT;
}


void A_HideThing(mobj_t * actor)
{
    //P_UnsetThingPosition(actor);
    actor->flags2 |= MF2_DONTDRAW;
}


void A_ImpDeath(mobj_t * actor)
{
    actor->flags &= ~MF_SOLID;
    actor->flags2 |= MF2_FOOTCLIP;
    if (actor->z <= actor->floorz)
    {
        P_SetMobjState(actor, HERETIC_S_IMP_CRASH1);
    }
}


void A_ImpExplode(mobj_t * actor)
{
    mobj_t *mo;

    mo = P_SpawnMobj(actor->x, actor->y, actor->z, HERETIC_MT_IMPCHUNK1);
    mo->momx = P_SubRandom() << 10;
    mo->momy = P_SubRandom() << 10;
    mo->momz = 9 * FRACUNIT;
    mo = P_SpawnMobj(actor->x, actor->y, actor->z, HERETIC_MT_IMPCHUNK2);
    mo->momx = P_SubRandom() << 10;
    mo->momy = P_SubRandom() << 10;
    mo->momz = 9 * FRACUNIT;
    if (actor->special1.i == 666)
    {                           // Extreme death crash
        P_SetMobjState(actor, HERETIC_S_IMP_XCRASH1);
    }
}


void A_ImpMeAttack(mobj_t * actor)
{
    if (!actor->target)
    {
        return;
    }
    S_StartMobjSound(actor, actor->info->attacksound);
    if (P_CheckMeleeRange(actor))
    {
        P_DamageMobj(actor->target, actor, actor, 5 + (P_Random(pr_heretic) & 7));
    }
}


void A_ImpMsAttack(mobj_t * actor)
{
    mobj_t *dest;
    angle_t an;
    int dist;

    if (!actor->target || P_Random(pr_heretic) > 64)
    {
        P_SetMobjState(actor, actor->info->seestate);
        return;
    }
    dest = actor->target;
    actor->flags |= MF_SKULLFLY;
    S_StartMobjSound(actor, actor->info->attacksound);
    A_FaceTarget(actor);
    an = actor->angle >> ANGLETOFINESHIFT;
    actor->momx = FixedMul(12 * FRACUNIT, finecosine[an]);
    actor->momy = FixedMul(12 * FRACUNIT, finesine[an]);
    dist = P_AproxDistance(dest->x - actor->x, dest->y - actor->y);
    dist = dist / (12 * FRACUNIT);
    if (dist < 1)
    {
        dist = 1;
    }
    actor->momz = (dest->z + (dest->height >> 1) - actor->z) / dist;
}


void A_ImpMsAttack2(mobj_t * actor)
{
    if (!actor->target)
    {
        return;
    }
    S_StartMobjSound(actor, actor->info->attacksound);
    if (P_CheckMeleeRange(actor))
    {
        P_DamageMobj(actor->target, actor, actor, 5 + (P_Random(pr_heretic) & 7));
        return;
    }
    P_SpawnMissile(actor, actor->target, HERETIC_MT_IMPBALL);
}


void A_ImpXDeath1(mobj_t * actor)
{
    actor->flags &= ~MF_SOLID;
    actor->flags |= MF_NOGRAVITY;
    actor->flags2 |= MF2_FOOTCLIP;
    actor->special1.i = 666;      // Flag the crash routine
}


void A_ImpXDeath2(mobj_t * actor)
{
    actor->flags &= ~MF_NOGRAVITY;
    if (actor->z <= actor->floorz)
    {
        P_SetMobjState(actor, HERETIC_S_IMP_CRASH1);
    }
}


void A_InitKeyGizmo(mobj_t * gizmo)
{
    mobj_t *mo;
    statenum_t state = S_NULL;

    switch (gizmo->type)
    {
        case HERETIC_MT_KEYGIZMOBLUE:
            state = HERETIC_S_KGZ_BLUEFLOAT1;
            break;
        case HERETIC_MT_KEYGIZMOGREEN:
            state = HERETIC_S_KGZ_GREENFLOAT1;
            break;
        case HERETIC_MT_KEYGIZMOYELLOW:
            state = HERETIC_S_KGZ_YELLOWFLOAT1;
            break;
        default:
            break;
    }
    mo = P_SpawnMobj(gizmo->x, gizmo->y, gizmo->z + 60 * FRACUNIT,
                     HERETIC_MT_KEYGIZMOFLOAT);
    P_SetMobjState(mo, state);
}


void A_KnightAttack(mobj_t * actor)
{
    if (!actor->target)
    {
        return;
    }
    if (P_CheckMeleeRange(actor))
    {
        P_DamageMobj(actor->target, actor, actor, HITDICE(3));
        S_StartMobjSound(actor, heretic_sfx_kgtat2);
        return;
    }
    // Throw axe
    S_StartMobjSound(actor, actor->info->attacksound);
    if (actor->type == HERETIC_MT_KNIGHTGHOST || P_Random(pr_heretic) < 40)
    {                           // Red axe
        P_SpawnMissile(actor, actor->target, HERETIC_MT_REDAXE);
        return;
    }
    // Green axe
    P_SpawnMissile(actor, actor->target, HERETIC_MT_KNIGHTAXE);
}


void A_MaceBallImpact(mobj_t * ball)
{
    if ((ball->z <= ball->floorz) && (P_HitFloor(ball) != FLOOR_SOLID))
    {                           // Landed in some sort of liquid
        P_RemoveMobj(ball);
        return;
    }
    if ((ball->health != MAGIC_JUNK) && (ball->z <= ball->floorz)
        && ball->momz)
    {                           // Bounce
        ball->health = MAGIC_JUNK;
        ball->momz = (ball->momz * 192) >> 8;
        ball->flags2 &= ~MF2_FLOORBOUNCE;
        P_SetMobjState(ball, ball->info->spawnstate);
        S_StartMobjSound(ball, heretic_sfx_bounce);
    }
    else
    {                           // Explode
        ball->flags |= MF_NOGRAVITY;
        ball->flags2 &= ~MF2_LOGRAV;
        S_StartMobjSound(ball, heretic_sfx_lobhit);
    }
}


void A_MaceBallImpact2(mobj_t * ball)
{
    mobj_t *tiny;
    angle_t angle;

    if ((ball->z <= ball->floorz) && (P_HitFloor(ball) != FLOOR_SOLID))
    {                           // Landed in some sort of liquid
        P_RemoveMobj(ball);
        return;
    }
    if ((ball->z != ball->floorz) || (ball->momz < 2 * FRACUNIT))
    {                           // Explode
        ball->momx = ball->momy = ball->momz = 0;
        ball->flags |= MF_NOGRAVITY;
        ball->flags2 &= ~(MF2_LOGRAV | MF2_FLOORBOUNCE);
    }
    else
    {                           // Bounce
        ball->momz = (ball->momz * 192) >> 8;
        P_SetMobjState(ball, ball->info->spawnstate);

        tiny = P_SpawnMobj(ball->x, ball->y, ball->z, HERETIC_MT_MACEFX3);
        angle = ball->angle + ANG90;
        P_SetTarget(&tiny->target, ball->target);
        tiny->angle = angle;
        angle >>= ANGLETOFINESHIFT;
        tiny->momx = (ball->momx >> 1) + FixedMul(ball->momz - FRACUNIT,
                                                  finecosine[angle]);
        tiny->momy = (ball->momy >> 1) + FixedMul(ball->momz - FRACUNIT,
                                                  finesine[angle]);
        tiny->momz = ball->momz;
        P_CheckMissileSpawn(tiny);

        tiny = P_SpawnMobj(ball->x, ball->y, ball->z, HERETIC_MT_MACEFX3);
        angle = ball->angle - ANG90;
        P_SetTarget(&tiny->target, ball->target);
        tiny->angle = angle;
        angle >>= ANGLETOFINESHIFT;
        tiny->momx = (ball->momx >> 1) + FixedMul(ball->momz - FRACUNIT,
                                                  finecosine[angle]);
        tiny->momy = (ball->momy >> 1) + FixedMul(ball->momz - FRACUNIT,
                                                  finesine[angle]);
        tiny->momz = ball->momz;
        P_CheckMissileSpawn(tiny);
    }
}


void A_MacePL1Check(mobj_t * ball)
{
    angle_t angle;

    if (ball->special1.i == 0)
    {
        return;
    }
    ball->special1.i -= 4;
    if (ball->special1.i > 0)
    {
        return;
    }
    ball->special1.i = 0;
    ball->flags2 |= MF2_LOGRAV;
    angle = ball->angle >> ANGLETOFINESHIFT;
    ball->momx = FixedMul(7 * FRACUNIT, finecosine[angle]);
    ball->momy = FixedMul(7 * FRACUNIT, finesine[angle]);
    ball->momz -= ball->momz >> 1;
}


void A_MakePod(mobj_t * actor)
{
    mobj_t *mo;
    fixed_t x;
    fixed_t y;

    if (actor->special1.i == MAX_GEN_PODS)
    {                           // Too many generated pods
        return;
    }
    x = actor->x;
    y = actor->y;
    mo = P_SpawnMobj(x, y, ONFLOORZ, HERETIC_MT_POD);
    if (P_CheckPosition(mo, x, y) == false)
    {                           // Didn't fit
        P_RemoveMobj(mo);
        return;
    }
    P_SetMobjState(mo, HERETIC_S_POD_GROW1);
    P_ThrustMobj(mo, P_Random(pr_heretic) << 24, (fixed_t) (4.5 * FRACUNIT));
    S_StartMobjSound(mo, heretic_sfx_newpod);
    actor->special1.i++;          // Increment generated pod count
    P_SetTarget(&mo->special2.m, actor);       // Link the generator to the pod
    return;
}


/* --- Minotaur block: honour the real 'hexen' flag (see g_mntr_* above) --- */
#undef hexen

void A_MinotaurAtk1(mobj_t * actor)
{
    player_t *player;

    if (!actor->target)
    {
        return;
    }
    S_StartMobjSound(actor, g_mntr_atk1_sfx);
    if (P_CheckMeleeRange(actor))
    {
        P_DamageMobj(actor->target, actor, actor, HITDICE(4));
        if (heretic && (player = actor->target->player) != NULL)
        {                       // Squish the player
            player->deltaviewheight = -16 * FRACUNIT;
        }
    }
}


void A_MinotaurAtk2(mobj_t * actor)
{
    mobj_t *mo;
    angle_t angle;
    fixed_t momz;

    if (!actor->target)
    {
        return;
    }
    S_StartMobjSound(actor, g_mntr_atk2_sfx);
    if (P_CheckMeleeRange(actor))
    {
        P_DamageMobj(actor->target, actor, actor, HITDICE(g_mntr_atk2_dice));
        return;
    }
    mo = P_SpawnMissile(actor, actor->target, g_mntr_atk2_missile);
    if (mo)
    {
        if (heretic)
          S_StartMobjSound(mo, g_mntr_atk2_sfx);
        momz = mo->momz;
        angle = mo->angle;
        P_SpawnMissileAngle(actor, g_mntr_atk2_missile, angle - (ANG45 / 8), momz);
        P_SpawnMissileAngle(actor, g_mntr_atk2_missile, angle + (ANG45 / 8), momz);
        P_SpawnMissileAngle(actor, g_mntr_atk2_missile, angle - (ANG45 / 16), momz);
        P_SpawnMissileAngle(actor, g_mntr_atk2_missile, angle + (ANG45 / 16), momz);
    }
}


void A_MinotaurAtk3(mobj_t * actor)
{
    mobj_t *mo;
    player_t *player;

    if (!actor->target)
    {
        return;
    }
    if (P_CheckMeleeRange(actor))
    {
        P_DamageMobj(actor->target, actor, actor, HITDICE(g_mntr_atk3_dice));
        if ((player = actor->target->player) != NULL)
        {                       // Squish the player
            player->deltaviewheight = -16 * FRACUNIT;
        }
    }
    else
    {
        mo = P_SpawnMissile(actor, actor->target, g_mntr_atk3_missile);
        if (mo != NULL)
        {
            S_StartMobjSound(mo, g_mntr_atk3_sfx);
        }
    }
    if (P_Random(pr_heretic) < 192 && actor->special2.i == 0)
    {
        P_SetMobjState(actor, g_mntr_atk3_state);
        actor->special2.i = 1;
    }
}


void A_MinotaurCharge(mobj_t * actor)
{
    mobj_t *puff;

    if (hexen && !actor->target)
      return;

    if (hexen ? actor->special_args[4] > 0 : actor->special1.i)
    {
        puff = P_SpawnMobj(actor->x, actor->y, actor->z, g_mntr_charge_puff);
        puff->momz = 2 * FRACUNIT;
        if (hexen)
          actor->special_args[4]--;
        else
          actor->special1.i--;
    }
    else
    {
        actor->flags &= ~MF_SKULLFLY;
        P_SetMobjState(actor, actor->info->seestate);
    }
}


void A_MinotaurDecide(mobj_t * actor)
{
    angle_t angle;
    mobj_t *target;
    int dist;

    target = actor->target;
    if (!target)
    {
        return;
    }
    if (heretic)
      S_StartMobjSound(actor, heretic_sfx_minsit);
    dist = P_AproxDistance(actor->x - target->x, actor->y - target->y);
    if (target->z + target->height > actor->z
        && target->z + target->height < actor->z + actor->height
        && dist < g_mntr_decide_range * 64 * FRACUNIT
        && dist > 1 * 64 * FRACUNIT && P_Random(pr_heretic) < g_mntr_charge_rng)
    {                           // Charge attack
        // Don't call the state function right away
        P_SetMobjStateNF(actor, g_mntr_charge_state);
        actor->flags |= MF_SKULLFLY;
        A_FaceTarget(actor);
        angle = actor->angle >> ANGLETOFINESHIFT;
        actor->momx = FixedMul(g_mntr_charge_speed, finecosine[angle]);
        actor->momy = FixedMul(g_mntr_charge_speed, finesine[angle]);
        // Charge duration
        if (hexen)
          actor->special_args[4] = 35 / 2;
        else
          actor->special1.i = 35 / 2;
    }
    else if (target->z == target->floorz
             && dist < 9 * 64 * FRACUNIT && P_Random(pr_heretic) < g_mntr_fire_rng)
    {                           // Floor fire attack
        P_SetMobjState(actor, g_mntr_fire_state);
        actor->special2.i = 0;
    }
    else
    {                           // Swing attack
        A_FaceTarget(actor);
        // Don't need to call P_SetMobjState because the current state
        // falls through to the swing attack
    }
}


void A_MntrFloorFire(mobj_t * actor)
{
    mobj_t *mo;
    int r1, r2;

    r1 = P_SubRandom();
    r2 = P_SubRandom();

    actor->z = actor->floorz;
    mo = P_SpawnMobj(actor->x + (r2 << 10),
                     actor->y + (r1 << 10), ONFLOORZ,
                     g_mntr_fire);
    P_SetTarget(&mo->target, actor->target);
    mo->momx = 1;               // Force block checking
    P_CheckMissileSpawn(mo);
}

/* --- end Minotaur block: restore the file-wide hexen fold-out --- */
#define hexen 0


void A_MummyAttack(mobj_t * actor)
{
    if (!actor->target)
    {
        return;
    }
    S_StartMobjSound(actor, actor->info->attacksound);
    if (P_CheckMeleeRange(actor))
    {
        P_DamageMobj(actor->target, actor, actor, HITDICE(2));
        S_StartMobjSound(actor, heretic_sfx_mumat2);
        return;
    }
    S_StartMobjSound(actor, heretic_sfx_mumat1);
}


void A_MummyAttack2(mobj_t * actor)
{
    mobj_t *mo;

    if (!actor->target)
    {
        return;
    }

    if (P_CheckMeleeRange(actor))
    {
        P_DamageMobj(actor->target, actor, actor, HITDICE(2));
        return;
    }
    mo = P_SpawnMissile(actor, actor->target, HERETIC_MT_MUMMYFX1);

    if (mo != NULL)
    {
        P_SetTarget(&mo->special1.m, actor->target);
    }
}


void A_MummyFX1Seek(mobj_t * actor)
{
    P_SeekerMissile(actor, &actor->special1.m, ANG1_X * 10, ANG1_X * 20, false);
}


void A_MummySoul(mobj_t * mummy)
{
    mobj_t *mo;

    mo = P_SpawnMobj(mummy->x, mummy->y, mummy->z + 10 * FRACUNIT,
                     HERETIC_MT_MUMMYSOUL);
    mo->momz = FRACUNIT;
}


void A_NoBlocking(mobj_t * actor)
{
    actor->flags &= ~MF_SOLID;

    if (hexen)
      return;

    // Check for monsters dropping things
    switch (actor->type)
    {
        case HERETIC_MT_MUMMY:
        case HERETIC_MT_MUMMYLEADER:
        case HERETIC_MT_MUMMYGHOST:
        case HERETIC_MT_MUMMYLEADERGHOST:
            P_DropItem(actor, HERETIC_MT_AMGWNDWIMPY, 3, 84);
            break;
        case HERETIC_MT_KNIGHT:
        case HERETIC_MT_KNIGHTGHOST:
            P_DropItem(actor, HERETIC_MT_AMCBOWWIMPY, 5, 84);
            break;
        case HERETIC_MT_WIZARD:
            P_DropItem(actor, HERETIC_MT_AMBLSRWIMPY, 10, 84);
            P_DropItem(actor, HERETIC_MT_ARTITOMEOFPOWER, 0, 4);
            break;
        case HERETIC_MT_HEAD:
            P_DropItem(actor, HERETIC_MT_AMBLSRWIMPY, 10, 84);
            P_DropItem(actor, HERETIC_MT_ARTIEGG, 0, 51);
            break;
        case HERETIC_MT_BEAST:
            P_DropItem(actor, HERETIC_MT_AMCBOWWIMPY, 10, 84);
            break;
        case HERETIC_MT_CLINK:
            P_DropItem(actor, HERETIC_MT_AMSKRDWIMPY, 20, 84);
            break;
        case HERETIC_MT_SNAKE:
            P_DropItem(actor, HERETIC_MT_AMPHRDWIMPY, 5, 84);
            break;
        case HERETIC_MT_MINOTAUR:
            P_DropItem(actor, HERETIC_MT_ARTISUPERHEAL, 0, 51);
            P_DropItem(actor, HERETIC_MT_AMPHRDWIMPY, 10, 84);
            break;
        default:
            break;
    }
}


void A_PhoenixPuff(mobj_t * actor)
{
    mobj_t *puff;
    angle_t angle;

    P_SeekerMissile(actor, &actor->special1.m, ANG1_X * 5, ANG1_X * 10, false);
    puff = P_SpawnMobj(actor->x, actor->y, actor->z, HERETIC_MT_PHOENIXPUFF);
    angle = actor->angle + ANG90;
    angle >>= ANGLETOFINESHIFT;
    puff->momx = FixedMul((fixed_t)(FRACUNIT * 1.3), finecosine[angle]);
    puff->momy = FixedMul((fixed_t)(FRACUNIT * 1.3), finesine[angle]);
    puff->momz = 0;
    puff = P_SpawnMobj(actor->x, actor->y, actor->z, HERETIC_MT_PHOENIXPUFF);
    angle = actor->angle - ANG90;
    angle >>= ANGLETOFINESHIFT;
    puff->momx = FixedMul((fixed_t)(FRACUNIT * 1.3), finecosine[angle]);
    puff->momy = FixedMul((fixed_t)(FRACUNIT * 1.3), finesine[angle]);
    puff->momz = 0;
}


void A_PodPain(mobj_t * actor)
{
    int i;
    int count;
    int chance;
    mobj_t *goo;

    chance = P_Random(pr_heretic);
    if (chance < 128)
    {
        return;
    }
    count = chance > 240 ? 2 : 1;
    for (i = 0; i < count; i++)
    {
        goo = P_SpawnMobj(actor->x, actor->y,
                          actor->z + 48 * FRACUNIT, HERETIC_MT_PODGOO);
        P_SetTarget(&goo->target, actor);
        goo->momx = P_SubRandom() << 9;
        goo->momy = P_SubRandom() << 9;
        goo->momz = FRACUNIT / 2 + (P_Random(pr_heretic) << 9);
    }
}


void A_RainImpact(mobj_t * actor)
{
    if (actor->z > actor->floorz)
    {
        P_SetMobjState(actor, HERETIC_S_RAINAIRXPLR1_1 + actor->special2.i);
    }
    else if (P_Random(pr_heretic) < 40)
    {
        P_HitFloor(actor);
    }
}


void A_RemovePod(mobj_t * actor)
{
    mobj_t *mo;

    if (actor->special2.m)
    {
        mo = (mobj_t *) actor->special2.m;
        if (mo->special1.i > 0)
        {
            mo->special1.i--;
        }
    }
}


void A_RemovedPhoenixFunc(mobj_t *actor)
{
    return;
}


void A_SkullPop(mobj_t *actor)
{
  mobj_t *mo;
  player_t *player;

  if (hexen && !actor->player)
  {
    return;
  }

  actor->flags &= ~MF_SOLID;
  mo = P_SpawnMobj(actor->x, actor->y, actor->z + 48 * FRACUNIT, g_skullpop_mt);
  //mo->target = actor;
  mo->momx = P_SubRandom() << 9;
  mo->momy = P_SubRandom() << 9;
  mo->momz = FRACUNIT * 2 + (P_Random(pr_heretic) << 6);
  // Attach player mobj to bloody skull
  player = actor->player;
  actor->player = NULL;
  if (hexen)
    actor->special1.i = player->pclass;
  mo->player = player;
  mo->health = actor->health;
  mo->angle = actor->angle;
  mo->pitch = 0;
  if (player)
  {
    player->mo = mo;
    player->lookdir = 0;
    player->damagecount = 32;
  }
}


void A_SkullRodPL2Seek(mobj_t * actor)
{
    P_SeekerMissile(actor, &actor->special1.m, ANG1_X * 10, ANG1_X * 30, false);
}


void A_SkullRodStorm(mobj_t * actor)
{
    fixed_t x;
    fixed_t y;
    mobj_t *mo;
    int playerNum;
    player_t *player;

    if (actor->health-- == 0)
    {
        P_SetMobjState(actor, S_NULL);
        playerNum = netgame ? actor->special2.i : 0;
        if (!playeringame[playerNum])
        {                       // Player left the game
            return;
        }
        player = &players[playerNum];
        if (player->health <= 0)
        {                       // Player is dead
            return;
        }
        if (player->rain1 == actor)
        {
            player->rain1 = NULL;
        }
        else if (player->rain2 == actor)
        {
            player->rain2 = NULL;
        }
        return;
    }
    if (P_Random(pr_heretic) < 25)
    {                           // Fudge rain frequency
        return;
    }
    x = actor->x + ((P_Random(pr_heretic) & 127) - 64) * FRACUNIT;
    y = actor->y + ((P_Random(pr_heretic) & 127) - 64) * FRACUNIT;
    mo = P_SpawnMobj(x, y, ONCEILINGZ, HERETIC_MT_RAINPLR1 + actor->special2.i);
    P_SetTarget(&mo->target, actor->target);
    mo->momx = 1;               // Force collision detection
    mo->momz = -mo->info->speed;
    mo->special2.i = actor->special2.i;     // Transfer player number
    P_CheckMissileSpawn(mo);
    if (!(actor->special1.i & 31))
    {
        S_StartMobjSound(actor, heretic_sfx_ramrain);
    }
    actor->special1.i++;
}


void A_SnakeAttack(mobj_t * actor)
{
    if (!actor->target)
    {
        P_SetMobjState(actor, HERETIC_S_SNAKE_WALK1);
        return;
    }
    S_StartMobjSound(actor, actor->info->attacksound);
    A_FaceTarget(actor);
    P_SpawnMissile(actor, actor->target, HERETIC_MT_SNAKEPRO_A);
}


void A_SnakeAttack2(mobj_t * actor)
{
    if (!actor->target)
    {
        P_SetMobjState(actor, HERETIC_S_SNAKE_WALK1);
        return;
    }
    S_StartMobjSound(actor, actor->info->attacksound);
    A_FaceTarget(actor);
    P_SpawnMissile(actor, actor->target, HERETIC_MT_SNAKEPRO_B);
}


void A_Sor1Chase(mobj_t * actor)
{
    if (actor->special1.i)
    {
        actor->special1.i--;
        actor->tics -= 3;
    }
    A_Chase(actor);
}


void A_Sor1Pain(mobj_t * actor)
{
    actor->special1.i = 20;       // Number of steps to walk fast
    A_Pain(actor);
}


void A_Sor2DthInit(mobj_t * actor)
{
    actor->special1.i = 7;        // Animation loop counter
    P_Massacre();               // Kill monsters early
}


void A_Sor2DthLoop(mobj_t * actor)
{
    if (--actor->special1.i)
    {                           // Need to loop
        P_SetMobjState(actor, HERETIC_S_SOR2_DIE4);
    }
}


void A_SorDBon(mobj_t * actor)
{
    S_StartVoidSound(heretic_sfx_sordbon);
}


void A_SorDExp(mobj_t * actor)
{
    S_StartVoidSound(heretic_sfx_sordexp);
}


void A_SorDSph(mobj_t * actor)
{
    S_StartVoidSound(heretic_sfx_sordsph);
}


void A_SorRise(mobj_t * actor)
{
    S_StartVoidSound(heretic_sfx_sorrise);
}


void A_SorSightSnd(mobj_t * actor)
{
    S_StartVoidSound(heretic_sfx_sorsit);
}


void A_SorZap(mobj_t * actor)
{
    S_StartVoidSound(heretic_sfx_sorzap);
}


void A_SorcererRise(mobj_t * actor)
{
    mobj_t *mo;

    actor->flags &= ~MF_SOLID;
    mo = P_SpawnMobj(actor->x, actor->y, actor->z, HERETIC_MT_SORCERER2);
    P_SetMobjState(mo, HERETIC_S_SOR2_RISE1);
    mo->angle = actor->angle;
    P_SetTarget(&mo->target, actor->target);
}


void A_SpawnRippers(mobj_t * actor)
{
    unsigned int i;
    angle_t angle;
    mobj_t *ripper;

    for (i = 0; i < 8; i++)
    {
        ripper = P_SpawnMobj(actor->x, actor->y, actor->z, HERETIC_MT_RIPPER);
        angle = i * ANG45;
        P_SetTarget(&ripper->target, actor->target);
        ripper->angle = angle;
        angle >>= ANGLETOFINESHIFT;
        ripper->momx = FixedMul(ripper->info->speed, finecosine[angle]);
        ripper->momy = FixedMul(ripper->info->speed, finesine[angle]);
        P_CheckMissileSpawn(ripper);
    }
}


void A_SpawnTeleGlitter(mobj_t * actor)
{
    mobj_t *mo;
    int r1, r2;

    r1 = P_Random(pr_heretic);
    r2 = P_Random(pr_heretic);
    mo = P_SpawnMobj(actor->x + ((r2 & 31) - 16) * FRACUNIT,
                     actor->y + ((r1 & 31) - 16) * FRACUNIT,
                     actor->subsector->sector->floorheight, HERETIC_MT_TELEGLITTER);
    mo->momz = FRACUNIT / 4;
}


void A_SpawnTeleGlitter2(mobj_t * actor)
{
    mobj_t *mo;
    int r1, r2;

    r1 = P_Random(pr_heretic);
    r2 = P_Random(pr_heretic);
    mo = P_SpawnMobj(actor->x + ((r2 & 31) - 16) * FRACUNIT,
                     actor->y + ((r1 & 31) - 16) * FRACUNIT,
                     actor->subsector->sector->floorheight, HERETIC_MT_TELEGLITTER2);
    mo->momz = FRACUNIT / 4;
}


void A_Srcr1Attack(mobj_t * actor)
{
    mobj_t *mo;
    fixed_t momz;
    angle_t angle;

    if (!actor->target)
    {
        return;
    }
    S_StartMobjSound(actor, actor->info->attacksound);
    if (P_CheckMeleeRange(actor))
    {
        P_DamageMobj(actor->target, actor, actor, HITDICE(8));
        return;
    }
    if (actor->health > (P_MobjSpawnHealth(actor) / 3) * 2)
    {                           // Spit one fireball
        P_SpawnMissile(actor, actor->target, HERETIC_MT_SRCRFX1);
    }
    else
    {                           // Spit three fireballs
        mo = P_SpawnMissile(actor, actor->target, HERETIC_MT_SRCRFX1);
        if (mo)
        {
            momz = mo->momz;
            angle = mo->angle;
            P_SpawnMissileAngle(actor, HERETIC_MT_SRCRFX1, angle - ANG1_X * 3, momz);
            P_SpawnMissileAngle(actor, HERETIC_MT_SRCRFX1, angle + ANG1_X * 3, momz);
        }
        if (actor->health < P_MobjSpawnHealth(actor) / 3)
        {                       // Maybe attack again
            if (actor->special1.i)
            {                   // Just attacked, so don't attack again
                actor->special1.i = 0;
            }
            else
            {                   // Set state to attack again
                actor->special1.i = 1;
                P_SetMobjState(actor, HERETIC_S_SRCR1_ATK4);
            }
        }
    }
}


void A_Srcr2Attack(mobj_t * actor)
{
    int chance;

    if (!actor->target)
    {
        return;
    }
    S_StartVoidSound(actor->info->attacksound);
    if (P_CheckMeleeRange(actor))
    {
        P_DamageMobj(actor->target, actor, actor, HITDICE(20));
        return;
    }
    chance = actor->health < P_MobjSpawnHealth(actor) / 2 ? 96 : 48;
    if (P_Random(pr_heretic) < chance)
    {                           // Wizard spawners
        P_SpawnMissileAngle(actor, HERETIC_MT_SOR2FX2,
                            actor->angle - ANG45, FRACUNIT / 2);
        P_SpawnMissileAngle(actor, HERETIC_MT_SOR2FX2,
                            actor->angle + ANG45, FRACUNIT / 2);
    }
    else
    {                           // Blue bolt
        P_SpawnMissile(actor, actor->target, HERETIC_MT_SOR2FX1);
    }
}


void A_Srcr2Decide(mobj_t * actor)
{
    static int chance[] = {
        192, 120, 120, 120, 64, 64, 32, 16, 0
    };

    if (!BossSpotCount)
    {                           // No spots
        return;
    }
    if (P_Random(pr_heretic) < chance[actor->health / (P_MobjSpawnHealth(actor) / 8)])
    {
        P_DSparilTeleport(actor);
    }
}


void A_UnHideThing(mobj_t * actor)
{
    //P_SetThingPosition(actor);
    actor->flags2 &= ~MF2_DONTDRAW;
}


void A_VolcBallImpact(mobj_t * ball)
{
    unsigned int i;
    mobj_t *tiny;
    angle_t angle;

    if (ball->z <= ball->floorz)
    {
        ball->flags |= MF_NOGRAVITY;
        ball->flags2 &= ~MF2_LOGRAV;
        ball->z += 28 * FRACUNIT;
        //ball->momz = 3*FRACUNIT;
    }
    P_RadiusAttackEx(ball, ball->target, 25, 25);
    for (i = 0; i < 4; i++)
    {
        tiny = P_SpawnMobj(ball->x, ball->y, ball->z, HERETIC_MT_VOLCANOTBLAST);
        P_SetTarget(&tiny->target, ball);
        angle = i * ANG90;
        tiny->angle = angle;
        angle >>= ANGLETOFINESHIFT;
        tiny->momx = FixedMul((fixed_t)(FRACUNIT * .7), finecosine[angle]);
        tiny->momy = FixedMul((fixed_t)(FRACUNIT * .7), finesine[angle]);
        tiny->momz = FRACUNIT + (P_Random(pr_heretic) << 9);
        P_CheckMissileSpawn(tiny);
    }
}


void A_VolcanoBlast(mobj_t * volcano)
{
    int i;
    int count;
    mobj_t *blast;
    angle_t angle;

    count = 1 + (P_Random(pr_heretic) % 3);
    for (i = 0; i < count; i++)
    {
        blast = P_SpawnMobj(volcano->x, volcano->y, volcano->z + 44 * FRACUNIT, HERETIC_MT_VOLCANOBLAST);
        P_SetTarget(&blast->target, volcano);
        angle = P_Random(pr_heretic) << 24;
        blast->angle = angle;
        angle >>= ANGLETOFINESHIFT;
        blast->momx = FixedMul(1 * FRACUNIT, finecosine[angle]);
        blast->momy = FixedMul(1 * FRACUNIT, finesine[angle]);
        blast->momz = (fixed_t)(2.5 * FRACUNIT) + (P_Random(pr_heretic) << 10);
        S_StartMobjSound(blast, heretic_sfx_volsht);
        P_CheckMissileSpawn(blast);
    }
}


void A_VolcanoSet(mobj_t * volcano)
{
    volcano->tics = 105 + (P_Random(pr_heretic) & 127);
}


void A_WhirlwindSeek(mobj_t * actor)
{
    actor->health -= 3;
    if (actor->health < 0)
    {
        actor->momx = actor->momy = actor->momz = 0;
        P_SetMobjState(actor, mobjinfo[actor->type].deathstate);
        actor->flags &= ~MF_MISSILE;
        return;
    }
    if ((actor->special2.i -= 3) < 0)
    {
        actor->special2.i = 58 + (P_Random(pr_heretic) & 31);
        S_StartMobjSound(actor, heretic_sfx_hedat3);
    }
    if (actor->special1.m
        && (((mobj_t *) (actor->special1.m))->flags & MF_SHADOW))
    {
        return;
    }
    P_SeekerMissile(actor, &actor->special1.m, ANG1_X * 10, ANG1_X * 30, false);
}


void A_WizAtk1(mobj_t * actor)
{
    A_FaceTarget(actor);
    actor->flags &= ~MF_SHADOW;
}


void A_WizAtk2(mobj_t * actor)
{
    A_FaceTarget(actor);
    actor->flags |= MF_SHADOW;
}


void A_WizAtk3(mobj_t * actor)
{
    mobj_t *mo;
    angle_t angle;
    fixed_t momz;

    actor->flags &= ~MF_SHADOW;
    if (!actor->target)
    {
        return;
    }
    S_StartMobjSound(actor, actor->info->attacksound);
    if (P_CheckMeleeRange(actor))
    {
        P_DamageMobj(actor->target, actor, actor, HITDICE(4));
        return;
    }
    mo = P_SpawnMissile(actor, actor->target, HERETIC_MT_WIZFX1);
    if (mo)
    {
        momz = mo->momz;
        angle = mo->angle;
        P_SpawnMissileAngle(actor, HERETIC_MT_WIZFX1, angle - (ANG45 / 8), momz);
        P_SpawnMissileAngle(actor, HERETIC_MT_WIZFX1, angle + (ANG45 / 8), momz);
    }
}


void A_RestoreArtifact(mobj_t * arti)
{
    arti->flags |= MF_SPECIAL;
    P_SetMobjState(arti, arti->info->spawnstate);
    S_StartMobjSound(arti, g_sfx_respawn);
}


void A_RestoreSpecialThing1(mobj_t * thing)
{
    if (thing->type == HERETIC_MT_WMACE)
    {                           // Do random mace placement
        P_RepositionMace(thing);
    }
    thing->flags2 &= ~MF2_DONTDRAW;
    S_StartMobjSound(thing, g_sfx_respawn);
}


void A_RestoreSpecialThing2(mobj_t * thing)
{
    thing->flags |= MF_SPECIAL;
    P_SetMobjState(thing, thing->info->spawnstate);
}


void A_ContMobjSound(mobj_t * actor)
{
    switch (actor->type)
    {
        case HERETIC_MT_KNIGHTAXE:
            S_StartMobjSound(actor, heretic_sfx_kgtatk);
            break;
        case HERETIC_MT_MUMMYFX1:
            S_StartMobjSound(actor, heretic_sfx_mumhed);
            break;
        default:
            break;
    }
}