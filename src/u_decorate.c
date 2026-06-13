/* u_decorate.c: DECORATE doomednum aliasing.
 *
 * ZDoom-targeted wads define new actors in a DECORATE lump this engine
 * cannot execute.  Many of those actors are reskinned base-game monsters:
 * they inherit (": Parent") or stand in for ("replaces Target") a class
 * that occupies a known editor number, and only carry a new doomednum so
 * maps can place the variant.  chex3.wad's Larva (9050) inherits
 * FlemoidusStridicus, which replaces FlemoidusCycloptisCommonus -- ZDoom's
 * chex name for editor number 3002 -- so the engine can spawn it as the
 * 3002 monster and the wad's sprite replacements supply the look.
 *
 * Only the actor HEADERS are parsed (name, parent, replaces, doomednum);
 * bodies are skipped by brace counting.  An unknown editor number is
 * resolved by walking parent-then-replaces links until reaching either a
 * DECORATE actor that has its own editor number or a base-game class name
 * with a known one.  Actors that root in classes without editor numbers
 * (brand-new decorations with their own sprites) cannot be aliased and
 * stay unspawned.
 *
 * U_IsInertZDoomThing covers ZDoom editor-only map things (particle
 * fountains, interpolation points, camera/view stacks, editor cameras)
 * so they are skipped without the unknown-thing message. */

#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "doomstat.h"
#include "doomdef.h"
#include "info.h"
#include "m_fixed.h"
#include "w_wad.h"
#include "w_pk3.h"
#include "lprintf.h"
#include "dsda_hacked.h"
#include "p_enemy.h"
#include "s_sound.h"
#include "p_inter.h"
#include "r_main.h"
#include "p_pspr.h"
#include "d_items.h"
#include "sounds.h"
#include "p_mobj.h"
#include "p_zacs.h"
#include "m_random.h"
#include "u_decorate.h"

#define MAX_DECORATE_ACTORS 1024
#define MAX_NAME 64

/* A DECORATE expression operand: a constant, a user-variable slot, or the
 * actor's "damage" property.  An expression is "A <op> B"; when op is
 * EXOP_NONE only A is used. */
enum { EXK_CONST = 0, EXK_UVAR, EXK_DAMAGE };
enum { EXOP_NONE = 0, EXOP_ADD, EXOP_SUB, EXOP_LT, EXOP_LE, EXOP_GT,
       EXOP_GE, EXOP_EQ, EXOP_NE };
typedef struct {
  short ka, va;        /* operand A kind / value (slot or constant) */
  short op;            /* EXOP_* */
  short kb, vb;        /* operand B kind / value */
} dexpr_t;

typedef struct
{
  char name[MAX_NAME];
  char parent[MAX_NAME];
  char replaces[MAX_NAME];
  int  doomednum;               /* -1 if none */

  /* simple-prop registration (static single-frame actors only) */
  int  radius, height;          /* map units; -1 if unspecified */
  int  solid, nogravity, spawnceiling;
  /* A custom DECORATE monster (the "Monster" keyword with its own behaviour
   * states) is brought to life with AI.  It is deliberately NOT made solid:
   * a solid monster placed overlapping another actor in the map wedges both
   * (P_Move fails in every direction), so these stay non-solid and simply
   * pass through, while still being shootable and counting as a kill. */
  int  is_monster;              /* the "Monster" keyword was seen           */
  int  health;                  /* spawnhealth, -1 if unset                 */
  int  speed;                   /* chase speed, -1 if unset                 */
  int  painchance;              /* 0-255, -1 if unset                       */
  int  mass;                    /* -1 if unset                              */
  int  meleedamage;             /* A_MeleeAttack/A_ComboAttack, -1 if unset */
  int  m_float;                 /* +FLOAT/+NOGRAVITY flying                  */
  int  m_nocountkill;           /* -COUNTKILL                               */
  int  is_pickup;               /* parent chain is an Inventory/pickup base */
  int  m_countitem;             /* +COUNTITEM                               */
  char sprite[5];               /* 4-char sprite of a "SPRT F -1" spawn */
  int  frame;                   /* 0-based frame letter, FF_FULLBRIGHT or'd */
  int  spawn_static;            /* spawn state parsed and tics == -1 */
  int  spawnitem_run;           /* consecutive A_SpawnItemEx frames captured */

  /* animated Spawn sequence (no action functions): a chain of frames the
   * registrar turns into looping/terminating states.  Captured only for a
   * plain "SPRITE <letters> <tics>" sequence ending in loop/stop.  A small,
   * safe subset of per-frame action functions is also captured (act != 0)
   * and wired onto the registered state. */
/* The captured frame budget per actor.  ZDoom "scene" actors (the follow-on
 * actors whose many SexScene_* labels each hold an animation loop) expand to
 * well over a hundred frames once multi-letter sprite runs (e.g. "SXT2 ABCDEF
 * 4" = six frames) are counted -- the largest is ~390 frames across 27 labels.
 * Anything past the cap collapsed onto the last kept frame, so labels beyond it
 * played a static/wrong frame.  A cap that covers the whole sequence lets
 * SetActorState land on the right animation.  Each frame costs ~126 bytes
 * across the parallel arrays times MAX_DECORATE_ACTORS, so low-memory targets
 * (Wii, handhelds) keep the original small cap and forgo the long scenes. */
#ifdef MEMORY_LOW
#define MAX_SPAWN_FRAMES 32
#else
#define MAX_SPAWN_FRAMES 400
#endif
  struct { short frame; short tics; short act; short snd; char spr[5]; } seq[MAX_SPAWN_FRAMES];
  /* Parallel to seq[]: operands for the user-variable / flag actions.  uvslot
   * is the target scalar/array base slot (or flag id for DA_CHANGEFLAG); idx
   * holds an array index expression operand; val holds the value expression;
   * for DA_CHANGEFLAG, fval is the boolean. */
  struct { short uvslot; short flag; short fval; dexpr_t idx; dexpr_t val; }
        seqop[MAX_SPAWN_FRAMES];
  /* Parallel to seq[]: control flow.  flow is one of SEQF_*; for SEQF_GOTO and
   * an A_JumpIf frame, target names the label this frame jumps to (interned in
   * seqlabel[]) or, when >= 0, jtoff is a numeric "jump N states forward".
   * jcond is the A_JumpIf condition expression. */
#define SEQF_NEXT 0           /* fall through to the next frame */
#define SEQF_LOOP 1           /* loop back to the current label's first frame */
#define SEQF_STOP 2           /* freeze on this frame */
#define SEQF_WAIT 3           /* stay on this frame (re-run its action) */
#define SEQF_GOTO 4           /* jump to label seqlabel[target] */
  struct { short flow; short target; short jtoff; short has_jump; short jchance;
           char tname[24]; char jtname[24]; dexpr_t jcond; }
        seqflow[MAX_SPAWN_FRAMES];
  /* Labels encountered in the state block, mapped to the frame index where
   * each begins.  A multi-label actor (Spawn + InitLoop + Idle + ...) records
   * every label so goto/A_JumpIf targets resolve to a frame. */
#define MAX_SEQ_LABELS 48
  struct { char name[24]; short frame; } seqlabel[MAX_SEQ_LABELS];
  int  num_seqlabels;
  int  multi_state;            /* 1 once a non-Spawn label/flow appears */
  char seq_sprite[5];           /* sprite the sequence uses (one sprite)  */
  int  seq_len;                 /* number of frames captured              */
  int  seq_loops;              /* 1 = loop back to frame 0, 0 = stop      */
  int  translucent;             /* RenderStyle Translucent / Add          */
  int  alpha;                   /* 16.16 alpha, FRACUNIT if unset         */
  int  damage;                  /* DECORATE Damage property (default 0)    */
  char seesound[32];            /* DECORATE sound properties (empty if   */
  char painsound[32];           /* unset; override the cloned base sounds) */
  char deathsound[32];
  char attacksound[32];
  char activesound[32];

  /* DECORATE weapon (": <Base>" / "replaces <Base>" rooted in a known
   * weapon slot).  Each of the five engine weapon states is captured as a
   * frame sequence with per-frame weapon actions; the registrar builds the
   * chains and repoints weaponinfo[slot].  wpn_slot < 0 => not a weapon. */
  int  wpn_slot;                /* weapontype_t of the replaced weapon     */
#define WST_READY  0
#define WST_SELECT 1
#define WST_DESEL  2
#define WST_FIRE   3
#define WST_FLASH  4
#define WST_COUNT  5
#define MAX_WSTATE_FRAMES 24
  struct {
    struct { char spr[5]; short frame; short tics; short act; short snd; }
         fr[MAX_WSTATE_FRAMES];
    int  len;
    int  loops;                 /* terminator: 1 loop, 0 stop/goto         */
    int  present;
    int  goto_dst;              /* WST_* the block's "goto" targets, or -1  */
  } wst[WST_COUNT];

  /* DECORATE user variables this actor declares ("var int name;" or
   * "var int name[N];").  Each maps a name to a base slot and length in the
   * actor's per-instance user-var array; the ACS user-variable builtins
   * resolve a name to its slot through this table. */
#define MAX_USERVARS 16
  struct { char name[28]; short base; short len; } uvar[MAX_USERVARS];
  int  num_uvars;
  int  uvar_slots;              /* total int slots the actor needs */
  int  inherited;              /* 1 once parent state/vars merged in       */
  int  use_special;            /* 1 if +USESPECIAL: usable -> Active state  */
  int  uses_blue_puff;         /* 1 if a fire frame named BlueLaserPuff      */
  int  xlat_id;                /* DECORATE Translation: index into the
                                * built-translation table, or -1 if none    */
  char xlat_raw[160];          /* raw Translation property text, captured
                                * before the table is built at registration */
} decorate_actor_t;

static decorate_actor_t actors[MAX_DECORATE_ACTORS];
static int num_actors;

/* Built DECORATE colour-remap tables.  Each entry is a 256-byte palette
 * translation (identity except for the remapped ranges).  Actors reference one
 * by index (decorate_actor_t.xlat_id); the spawned mobj carries a pointer to
 * the bytes so the sprite renderer can swap colours.  Built once at
 * registration, after the palette is available, and never freed. */
#define MAX_DECORATE_XLATS 64
static uint8_t decorate_xlats[MAX_DECORATE_XLATS][256];
static char    decorate_xlat_src[MAX_DECORATE_XLATS][160];
static int     num_decorate_xlats;

/* Parse a ZDoom translation string ("a:b=c:d, e:f=g:h, ...") into `tbl`,
 * which must already be identity-initialised.  Each clause remaps source
 * palette indices a..b linearly onto destination indices c..d. */
#define DXLAT_CLAMP(v) ((v) < 0 ? 0 : (v) > 255 ? 255 : (v))
static void decorate_parse_translation(const char *src, uint8_t *tbl)
{
  const char *p = src;
  while (*p)
  {
    int a, b, c, d, i;
    /* skip separators and optional surrounding quotes */
    while (*p == ' ' || *p == '\t' || *p == ',' || *p == '"') p++;
    if (!*p) break;
    if (sscanf(p, "%d:%d=%d:%d", &a, &b, &c, &d) == 4)
    {
      int span_s;
      a = DXLAT_CLAMP(a);
      b = DXLAT_CLAMP(b);
      c = DXLAT_CLAMP(c);
      d = DXLAT_CLAMP(d);
      span_s = b - a;
      if (span_s < 0) span_s = -span_s;     /* iterate magnitude */
      for (i = 0; i <= span_s; i++)
      {
        int s = (b >= a) ? (a + i) : (a - i);
        int t = (span_s == 0) ? c
                              : c + (int)((long)(d - c) * i / span_s);
        t = DXLAT_CLAMP(t);
        tbl[s & 0xff] = (uint8_t)t;
      }
    }
    /* advance to the next clause (past this one's comma) */
    while (*p && *p != ',') p++;
  }
}

/* Resolve a raw Translation string to a built-table index, building it once
 * and de-duplicating identical strings.  Returns -1 on empty/overflow. */
static int decorate_translation_id(const char *raw)
{
  int i;
  if (!raw || !raw[0])
    return -1;
  for (i = 0; i < num_decorate_xlats; i++)
    if (!strcmp(decorate_xlat_src[i], raw))
      return i;
  if (num_decorate_xlats >= MAX_DECORATE_XLATS)
    return -1;
  i = num_decorate_xlats++;
  { int k; for (k = 0; k < 256; k++) decorate_xlats[i][k] = (uint8_t)k; }
  decorate_parse_translation(raw, decorate_xlats[i]);
  {
    size_t rl = strlen(raw);
    if (rl > sizeof(decorate_xlat_src[i]) - 1)
      rl = sizeof(decorate_xlat_src[i]) - 1;
    memcpy(decorate_xlat_src[i], raw, rl);
    decorate_xlat_src[i][rl] = 0;
  }
  return i;
}

/* Public: the 256-byte remap table for a built id, or NULL. */
const uint8_t *U_DecorateTranslation(int xlat_id)
{
  if (xlat_id < 0 || xlat_id >= num_decorate_xlats)
    return NULL;
  return decorate_xlats[xlat_id];
}

/* True if `p` points at one of the built 256-byte translation tables.  The
 * sprite renderer validates a vissprite's translation pointer with this so a
 * stale or wild value can never reach the column drawer. */
int U_DecorateTranslationOK(const uint8_t *p)
{
  const uint8_t *base = (const uint8_t *)decorate_xlats;
  if (!p || num_decorate_xlats <= 0)
    return 0;
  return p >= base &&
         p <  base + (size_t)num_decorate_xlats * 256 &&
         ((size_t)(p - base) % 256) == 0;   /* must be a table start */
}

/* Per-mobjtype user-variable map, filled at registration so the ACS
 * user-variable builtins can resolve (actor type, name) to a storage slot.
 * Keyed by mobjtype; only DECORATE-registered actors appear here. */
typedef struct {
  int   type;                   /* mobjtype this map describes */
  int   slots;                  /* total int slots the type needs */
  int   num;                    /* number of named variables */
  struct { char name[28]; short base; short len; } var[MAX_USERVARS];
} uvarmap_t;
#define MAX_UVARMAPS MAX_DECORATE_ACTORS
static uvarmap_t uvarmaps[MAX_UVARMAPS];
static int num_uvarmaps;

static uvarmap_t *uvarmap_for_type(int type)
{
  int i;
  for (i = 0; i < num_uvarmaps; i++)
    if (uvarmaps[i].type == type)
      return &uvarmaps[i];
  return NULL;
}

/* Public: total user-var slots a mobjtype declares (0 if none). */
int U_DecorateUserVarCount(int type)
{
  uvarmap_t *m = uvarmap_for_type(type);
  return m ? m->slots : 0;
}

/* Public: resolve a user-var name on a mobjtype to its base slot and length.
 * Returns 1 and fills the base and len out-params on success, 0 if the name
 * is not declared. */
int U_DecorateUserVarSlot(int type, const char *name, int *base, int *len)
{
  uvarmap_t *m = uvarmap_for_type(type);
  int i;
  if (!m || !name)
    return 0;
  for (i = 0; i < m->num; i++)
    if (!strcasecmp(m->var[i].name, name))
    {
      if (base) *base = m->var[i].base;
      if (len)  *len  = m->var[i].len;
      return 1;
    }
  return 0;
}

/* Use-activatable decorations (+USESPECIAL): the state a thing of this type
 * enters when the player presses use on it (its "Active" label resolved to a
 * concrete state index).  Registered at U_RegisterDecorateThings time. */
typedef struct { int type; int activestate; } useact_t;
#define MAX_USEACTS MAX_DECORATE_ACTORS
static useact_t useacts[MAX_USEACTS];
static int num_useacts;

/* Map a registered mobjtype back to the DECORATE actor it was built from and
 * the state-table slot its first frame landed at, so a state label ("Missile",
 * "SexScene_1_1", ...) can be resolved to a concrete state index at runtime.
 * Populated as each actor is registered; queried by U_DecorateStateForType. */
typedef struct { int type; int base; const decorate_actor_t *actor; } statemap_t;
#define MAX_STATEMAPS MAX_DECORATE_ACTORS
static statemap_t statemaps[MAX_STATEMAPS];
static int num_statemaps;

/* Public: the Active state a use-activatable mobjtype enters, or -1. */
int U_DecorateActiveState(int type)
{
  int i;
  for (i = 0; i < num_useacts; i++)
    if (useacts[i].type == type)
      return useacts[i].activestate;
  return -1;
}

/* Safe per-frame DECORATE actions the registrar can wire onto a decoration
 * state.  Kept deliberately small: only self-contained codepointers that the
 * engine already implements and that are harmless on a decoration's own
 * thinker.  DA_NONE leaves the state actionless. */
enum {
  DA_NONE = 0,
  DA_PLAYSOUND,        /* A_PlaySound / A_StartSound("name") -> A_PlaySound */
  DA_SCREAM,           /* A_Scream  (death sound) */
  DA_ACTIVESOUND,      /* A_ActiveSound -> active sound via A_PlaySound */
  DA_NOBLOCKING,       /* A_NoBlocking / A_Fall -> clears MF_SOLID */
  DA_FACETARGET,       /* A_FaceTarget (harmless no-op without a target) */
  DA_ACS_NAMED,        /* ACS_NamedExecuteAlways("name") -> start named script */
  DA_SETUSERVAR,       /* A_SetUserVar(name, expr) -> set scalar user var */
  DA_SETUSERARRAY,     /* A_SetUserArray(name, idxexpr, expr) -> set element */
  DA_CHANGEFLAG,       /* A_ChangeFlag("FLAG", bool) -> set/clear a flag */
  DA_SPAWNITEM,        /* A_SpawnItemEx("Class", ...) -> spawn a decoration */
  DA_FADEOUT,          /* A_FadeOut(reduce) -> fade the actor out and remove it */
  DA_CUSTOMMISSILE,    /* A_CustomMissile("Class",...) -> fire a projectile */
  DA_CUSTOMBULLET,     /* A_CustomBulletAttack(...) -> hitscan burst at target */
  DA_SPIDREFIRE,       /* A_SpidRefire -> keep firing or break off to See */
  DA_LOOK,             /* A_Look  -> wake on a target */
  DA_CHASE,            /* A_Chase -> pursue the target / enter attack */
  DA_FASTCHASE,        /* A_FastChase */
  DA_PAIN,             /* A_Pain  -> pain sound */
  DA_XSCREAM,          /* A_XScream -> gib death sound */
  DA_BOSSDEATH,        /* A_BossDeath */
  DA_CPOSREFIRE,       /* A_CPosRefire */
  DA_SKULLATTACK,      /* A_SkullAttack */
  DA_EXPLODE,          /* A_Explode */
  DA_MELEEATTACK,      /* A_MeleeAttack -> melee for MeleeDamage */
  DA_MISSILEATTACK,    /* A_MissileAttack -> fire the actor's projectile */
  DA_COMBOATTACK       /* A_ComboAttack -> melee if close, else missile */
};

/* Sound names captured for DA_PLAYSOUND frames, resolved to sfx slots at
 * registration time (the sound tables are not grown during the parse). */
#define MAX_DECORATE_SOUNDS 256
static char decorate_sounds[MAX_DECORATE_SOUNDS][32];
static int  num_decorate_sounds;

/* The mobjtype a DECORATE actor "replaces BulletPuff" registered to, or -1.
 * P_SpawnPuff emits this in place of MT_PUFF so the mod's laser-puff sprite is
 * shown on hitscan impacts. */
static int decorate_bulletpuff_type = -1;
/* The blue laser puff (named per-shot by the shotgun/SSG A_FireBullets, with no
 * "replaces" of its own) and a one-shot override the weapon-fire path raises
 * just before delegating to the base hitscan attack, so a shot that asked for a
 * specific puff gets it instead of the global BulletPuff replacement. */
static int decorate_bluepuff_type = -1;
static int decorate_puff_override = -1;

int U_DecoratePuffReplacement(void)
{
  /* an active per-shot override wins over the global "replaces BulletPuff" */
  if (decorate_puff_override >= 0)
    return decorate_puff_override;
  return decorate_bulletpuff_type;
}

/* Raise (mt>=0) or clear (-1) the per-shot puff override.  Set from the weapon
 * codepointer around a base-fire delegation that named a non-default puff. */
void U_DecorateSetPuffOverride(int mt)
{
  decorate_puff_override = mt;
}

int U_DecorateBluePuffType(void)
{
  return decorate_bluepuff_type;
}

/* Weapon-frame sounds (A_PlaySound on a pspr state): the sfx id a given weapon
 * state should play, keyed by the state index.  A weapon state cannot stash the
 * id in misc1/misc2 (the gun-sprite offset), so the weapon-safe codepointer
 * A_DecorateWeaponSound looks it up here.  Kept as a small open-addressed map so
 * it costs nothing for the vast majority of states that have no sound. */
#define MAX_WEAPON_SOUNDS 128
static struct { int state; int sfx; } decorate_wsnd[MAX_WEAPON_SOUNDS];
static int  num_decorate_wsnd;

static void decorate_set_weapon_sound(int state, int sfx)
{
  if (num_decorate_wsnd < MAX_WEAPON_SOUNDS)
  {
    decorate_wsnd[num_decorate_wsnd].state = state;
    decorate_wsnd[num_decorate_wsnd].sfx   = sfx;
    num_decorate_wsnd++;
  }
}

int U_DecorateWeaponSound(int state)
{
  int i;
  for (i = 0; i < num_decorate_wsnd; i++)
    if (decorate_wsnd[i].state == state)
      return decorate_wsnd[i].sfx;
  return 0;
}


/* Class names captured from A_CustomMissile("Class", ...) frames, so the fired
 * imp ball can inherit that class's DECORATE Translation/Scale.  Each frame
 * stores an index into this table (or -1) in its parallel seqop.uvslot. */
#define MAX_DECORATE_CMISS 64
static char decorate_cmiss_class[MAX_DECORATE_CMISS][32];
static int  num_decorate_cmiss;

/* Class names captured for A_SpawnItemEx frames; resolved to a mobjtype at
 * registration (the spawned actor must itself be a registered decoration). */
#define MAX_DECORATE_SPAWNS 128
static char decorate_spawns[MAX_DECORATE_SPAWNS][32];
static int  num_decorate_spawns;

/* Max consecutive A_SpawnItemEx emitter lines captured from one run: two
 * (each a six-frame STTR cycle here) is enough that items appear and the
 * pose animates, without letting a long repeated emitter crowd out the
 * actor's later states. */
#define DECORATE_SPAWNITEM_CAP 2

/* Script names captured for DA_ACS_NAMED frames.  At registration the name is
 * interned into the engine string pool so the runtime action can hand its
 * index to the ACS_NamedExecuteAlways line special. */
#define MAX_DECORATE_ACSNAMES 128
static char decorate_acsnames[MAX_DECORATE_ACSNAMES][32];
static int  num_decorate_acsnames;

/* Interned script names that persist past the parse: a registered state's
 * misc1 holds an index into this table, and the runtime action reads the
 * name back to start the script.  Kept as plain static storage (the names
 * are a handful of short identifiers fixed at load). */
#define MAX_INTERNED_ACSNAMES 128
static char interned_acsnames[MAX_INTERNED_ACSNAMES][32];
static int  num_interned_acsnames;

static int decorate_intern_acsname(const char *name)
{
  int i;
  for (i = 0; i < num_interned_acsnames; i++)
    if (!strcasecmp(interned_acsnames[i], name))
      return i;
  if (num_interned_acsnames >= MAX_INTERNED_ACSNAMES)
    return 0;
  {
    int n = 0;
    while (name[n] && n < 31) { interned_acsnames[num_interned_acsnames][n] = name[n]; n++; }
    interned_acsnames[num_interned_acsnames][n] = 0;
  }
  return num_interned_acsnames++;
}

/* Runtime action for a DECORATE frame carrying ACS_NamedExecuteAlways("x"):
 * start the named script with this actor as the activator.  misc1 indexes the
 * interned name table.
 *
 * A use-activated story actor runs its script as itself (the activator is the
 * actor, not the player): the script's own body switches the activator to the
 * actor's target with SetActivatorToTarget and reads its per-actor state by
 * TID, so it must see the actor first.  The use handler still records the
 * using player as the actor's target, which is what that in-script switch
 * resolves to. */
void A_DecorateACSNamed(mobj_t *mo)
{
  int idx;
  if (!mo || !mo->state)
    return;
  idx = (int)mo->state->misc1;
  if (idx < 0 || idx >= num_interned_acsnames)
    return;
  /* A use-activated story actor starts its conversation script with this
   * action.  The player is held frozen for the duration of the conversation,
   * and the dialogue itself consumes the use key to advance.  Our use handler
   * still lets a held or repeated use re-activate the actor, which would start
   * a second, overlapping copy of the conversation each press -- the lines
   * bounce and replay and the dialogue never settles.  The conversation script
   * only sets the player's freeze a few tics in, so guarding on the freeze
   * alone leaves a window where a mashed use stacks a duplicate controller; if
   * the named conversation script is already running, do not start another. */
  if (Z_ACSNamedRunning(interned_acsnames[idx]))
    return;
  if (mo->target && mo->target->player &&
      (mo->target->player->cheats & CF_TOTALLYFROZEN))
    return;
  Z_ACSStartNamedStr(interned_acsnames[idx], NULL, 0, mo, true);
}

/* Death action for the hdoom replacement monsters.  Their DECORATE Death
 * chain is entirely ACS-driven: roll the cross-death chance, and if it hits
 * and the gate cvar is set, hand off to the SpawnSexActor script (which reads
 * the dying actor's class and position to spawn its replacement).  The actor
 * then falls through to its normal death animation, which serves as the
 * "Faint" sequence.  Replicates:
 *   A_Jump(CallACS("GetXDeathChance"), "XDeath")
 *   XDeath: A_JumpIf(GetDoXDeath, SpawnSexActor)
 * Run synchronously here so the spawn happens on the death tic. */
void A_HDoomDeath(mobj_t *mo)
{
  int chance;
  if (!mo)
    return;
  chance = Z_ACSCallNamedSync("GetXDeathChance", mo);
  if (chance > 0 && P_Random(pr_misc) < chance &&
      Z_ACSCallNamedSync("GetDoXDeath", mo))
    Z_ACSCallNamedSync("SpawnSexActor", mo);
}

/* Evaluate one DECORATE operand against a mobj. */
static int decorate_eval_operand(mobj_t *mo, int kind, int val)
{
  switch (kind)
  {
    case EXK_UVAR:   return (mo->user_vars) ? mo->user_vars[val] : 0;
    case EXK_DAMAGE: return mo->info ? mo->info->damage : 0;
    default:         return val;            /* EXK_CONST */
  }
}

/* Evaluate a (kindA,valA,op,kindB,valB) expression against a mobj. */
static int decorate_eval_expr(mobj_t *mo, int ka, int va, int op,
                              int kb, int vb)
{
  int A = decorate_eval_operand(mo, ka, va);
  int B;
  if (op == EXOP_NONE)
    return A;
  B = decorate_eval_operand(mo, kb, vb);
  switch (op)
  {
    case EXOP_ADD: return A + B;
    case EXOP_SUB: return A - B;
    case EXOP_LT:  return A <  B;
    case EXOP_LE:  return A <= B;
    case EXOP_GT:  return A >  B;
    case EXOP_GE:  return A >= B;
    case EXOP_EQ:  return A == B;
    case EXOP_NE:  return A != B;
    default:       return A;
  }
}

/* Ensure a mobj has user-var storage allocated for its type. */
static int *decorate_uservars(mobj_t *mo)
{
  int total;
  if (mo->user_vars)
    return mo->user_vars;
  total = U_DecorateUserVarCount(mo->type);
  if (total <= 0)
    return NULL;
  mo->user_vars = Z_Malloc(total * sizeof(int), PU_LEVEL, NULL);
  memset(mo->user_vars, 0, total * sizeof(int));
  return mo->user_vars;
}

/* A_SetUserVar(name, expr): write the value expression to the scalar slot in
 * misc1.  Operands are encoded in args[0..4]. */
void A_DecorateSetUserVar(mobj_t *mo)
{
  state_t *st;
  int *uv;
  if (!mo || !mo->state)
    return;
  st = mo->state;
  uv = decorate_uservars(mo);
  if (!uv)
    return;
  uv[(int)st->misc1] = decorate_eval_expr(mo, (int)st->args[0],
      (int)st->args[1], (int)st->args[2], (int)st->args[3], (int)st->args[4]);
}

/* A_SpawnItemEx("Class", ...): spawn the resolved mobjtype (misc1) at the
 * actor's position.  The offset/velocity/flag arguments of the DECORATE call
 * are not modelled; the spawned decoration appears on the actor.  A negative
 * misc1 means the class did not resolve to a registered type -- skip. */
void A_DecorateSpawnItem(mobj_t *mo)
{
  int type;
  mobj_t *it;
  fixed_t ox, oy;
  if (!mo || !mo->state)
    return;
  type = (int)mo->state->misc1;
  if (type <= 0 || type >= num_mobj_types)
    return;
  /* The emitter calls spawn these decorations around the actor's upper body
   * with a small random horizontal scatter and a gentle upward drift (the
   * DECORATE call passes xy offsets and a positive z velocity), so they hover
   * and rise rather than sit on the floor.  The spawned class itself is
   * NOGRAVITY; give it the rising motion here. */
  ox = ((P_Random(pr_misc) - 128) * (20 * FRACUNIT)) / 128;
  oy = ((P_Random(pr_misc) - 128) * (20 * FRACUNIT)) / 128;
  it = P_SpawnMobj(mo->x + ox, mo->y + oy,
                   mo->z + mo->height - (8 * FRACUNIT), (mobjtype_t)type);
  if (it)
    it->momz = (FRACUNIT + (P_Random(pr_misc) % (3 * FRACUNIT)));
}

/* A_FadeOut(reduce): in ZDoom this lowers the actor's alpha by `reduce` each
 * call and removes it once fully transparent.  This engine has no graduated
 * per-actor alpha (only an on/off translucency), so approximate: draw the
 * actor translucent immediately, count the fade calls (misc1 holds 1/reduce,
 * the number of steps to reach zero), and remove the actor when the steps are
 * spent.  A short-lived decoration (a spawned heart, a puff) then fades to
 * translucent and disappears instead of lingering forever. */
void A_DecorateFadeOut(mobj_t *mo)
{
  int steps;
  if (!mo || !mo->state)
    return;
  mo->flags |= MF_TRANSLUCENT;
  steps = (int)mo->state->misc1;
  if (steps <= 0)
    steps = 8;                    /* sensible default if no rate was parsed */
  if (mo->special1.i <= 0)
    mo->special1.i = steps;
  if (--mo->special1.i <= 0)
    P_RemoveMobj(mo);
}

/* A_CustomMissile("Class", height, spawnofs, angle, ...): fire a projectile at
 * the actor's current target.  This engine has no general per-class spawning
 * for arbitrary DECORATE projectiles, so it fires the Doom imp fireball
 * (MT_TROOPSHOT); the mod's custom projectiles derive from the imp ball and
 * read as the same shot.  misc1 carries the angle offset in degrees, so a
 * burst of several calls at different angles fans into a spread.  With no
 * target the call is a harmless no-op, matching ZDoom. */
void A_DecorateCustomMissile(mobj_t *mo)
{
  int deg;
  const uint8_t *xlat = NULL;
  mobj_t *shot = NULL;
  if (!mo || !mo->state || !mo->target)
    return;
  A_FaceTarget(mo);
  deg = (int)mo->state->misc1;
  /* misc2, when nonzero, is (translation id + 1) of the named projectile
   * class; recolour the fired imp ball to match the mod's custom shot. */
  if (mo->state->misc2 > 0)
    xlat = U_DecorateTranslation((int)mo->state->misc2 - 1);
  if (deg == 0)
    shot = P_SpawnMissile(mo, mo->target, MT_TROOPSHOT);
  else
  {
    /* aim from the actor toward the target, offset by the requested angle:
     * ANG1 is the binary-angle measure of one degree, so multiplying gives the
     * offset directly; unsigned wraparound handles a negative offset. */
    angle_t base = R_PointToAngle2(mo->x, mo->y, mo->target->x, mo->target->y);
    angle_t off  = (angle_t)deg * ANG1;
    shot = P_SpawnMissileAngle(mo, MT_TROOPSHOT, base + off, 0);
  }
  if (shot && xlat)
    shot->translation = xlat;
}

/* A_MeleeAttack: deal the actor's MeleeDamage (carried in state->misc1, with a
 * stock-melee 3d8 default) when the target is in melee range. */
void A_DecorateMeleeAttack(mobj_t *mo)
{
  if (!mo || !mo->target)
    return;
  A_FaceTarget(mo);
  if (P_CheckMeleeRange(mo))
  {
    int dmg = (mo->state ? (int)mo->state->misc1 : 0);
    if (mo->info->attacksound)
      S_StartSound(mo, mo->info->attacksound);
    if (dmg <= 0)
      dmg = (P_Random(pr_troopattack) % 8 + 1) * 3;
    P_DamageMobj(mo->target, mo, mo, dmg);
  }
}

/* A_MissileAttack: fire a projectile at the target.  This engine has no
 * per-class projectile spawning, so it fires the generic imp-ball shot the
 * custom projectiles derive from (as A_CustomMissile does). */
void A_DecorateMissileAttack(mobj_t *mo)
{
  if (!mo || !mo->target)
    return;
  A_FaceTarget(mo);
  P_SpawnMissile(mo, mo->target, MT_TROOPSHOT);
}

/* A_ComboAttack: melee when the target is in range, otherwise a missile. */
void A_DecorateComboAttack(mobj_t *mo)
{
  if (!mo || !mo->target)
    return;
  A_FaceTarget(mo);
  if (P_CheckMeleeRange(mo))
  {
    int dmg = (mo->state ? (int)mo->state->misc1 : 0);
    if (mo->info->attacksound)
      S_StartSound(mo, mo->info->attacksound);
    if (dmg <= 0)
      dmg = (P_Random(pr_troopattack) % 8 + 1) * 3;
    P_DamageMobj(mo->target, mo, mo, dmg);
  }
  else
    P_SpawnMissile(mo, mo->target, MT_TROOPSHOT);
}

/* A_SetUserArray(name, idxexpr, expr): write the value expression to element
 * (index expression) of the array based at misc1.  The value expression is
 * args[0..4]; the index expression is args[5..7]+misc2 (low byte = kindB,
 * high byte = valB). */
void A_DecorateSetUserArray(mobj_t *mo)
{
  state_t *st;
  int *uv, base, idx, total, ikb, ivb;
  if (!mo || !mo->state)
    return;
  st = mo->state;
  uv = decorate_uservars(mo);
  if (!uv)
    return;
  base = (int)st->misc1;
  ikb  = (int)st->misc2 & 0xFF;
  ivb  = ((int)st->misc2 >> 8) & 0xFF;
  idx  = decorate_eval_expr(mo, (int)st->args[5], (int)st->args[6],
                            (int)st->args[7], ikb, ivb);
  total = U_DecorateUserVarCount(mo->type);
  if (idx < 0) idx = 0;
  if (base + idx >= total) idx = (total - 1) - base;
  if (idx < 0) return;
  uv[base + idx] = decorate_eval_expr(mo, (int)st->args[0], (int)st->args[1],
      (int)st->args[2], (int)st->args[3], (int)st->args[4]);
}

/* A_JumpIf(cond, target): if the condition evaluates non-zero, jump to the
 * target state encoded in args[5]; otherwise fall through.  The condition is
 * args[0..4].  Uses P_SetMobjState so the destination's own action runs. */
void A_DecorateJumpIf(mobj_t *mo)
{
  state_t *st;
  int cond, tgt;
  if (!mo || !mo->state)
    return;
  st = mo->state;
  cond = decorate_eval_expr(mo, (int)st->args[0], (int)st->args[1],
                            (int)st->args[2], (int)st->args[3],
                            (int)st->args[4]);
  if (!cond)
    return;
  tgt = (int)st->args[5];
  if (tgt >= 0 && tgt < num_states)
    P_SetMobjState(mo, (statenum_t)tgt);
}

/* A_Jump(chance, target): unconditional probabilistic jump.  With chance/256
 * probability jump to the target state in args[5]; otherwise fall through.  Used
 * by the scene state machines, notably A_Jump(256, "Finish") as the guaranteed
 * exit from a sex act back to the death/finish sprite. */
void A_DecorateJump(mobj_t *mo)
{
  int chance, tgt;
  if (!mo || !mo->state)
    return;
  chance = (int)mo->state->args[0];
  if (chance < 256 && P_Random(pr_randomjump) >= chance)
    return;
  tgt = (int)mo->state->args[5];
  if (tgt >= 0 && tgt < num_states)
    P_SetMobjState(mo, (statenum_t)tgt);
}

/* Safe weapon-state (pspr) actions: the structural codepointers that make a
 * custom weapon select, ready, fire-cycle, lower/raise and flash.  Firing
 * damage is delegated to the replaced base weapon's native attack so no
 * ZDoom-only hitscan/puff semantics are reinvented.  WA_NONE leaves the
 * weapon state actionless. */
enum {
  WA_NONE = 0,
  WA_READY,            /* A_WeaponReady */
  WA_RAISE,            /* A_Raise */
  WA_LOWER,            /* A_Lower */
  WA_GUNFLASH,         /* A_GunFlash */
  WA_REFIRE,           /* A_ReFire */
  WA_BASEFIRE,         /* delegate to the replaced weapon's native attack */
  WA_PLAYSOUND         /* A_PlaySound("name") -> play on the player via a
                        * weapon-safe codepointer (the sfx id rides in a side
                        * table keyed by state, never in misc1/2) */
};

/* Map a base-weapon class name to its engine slot, or -1.  Only the Doom
 * weapon classes are recognised; a custom weapon must inherit from or
 * replace one of these to occupy a usable slot. */
static int weapon_base_slot(const char *name)
{
  static const struct { const char *n; int slot; } tbl[] = {
    { "Fist",          WP_FIST },
    { "Chainsaw",      WP_CHAINSAW },
    { "Pistol",        WP_PISTOL },
    { "Shotgun",       WP_SHOTGUN },
    { "SuperShotgun",  WP_SUPERSHOTGUN },
    { "Chaingun",      WP_CHAINGUN },
    { "RocketLauncher",WP_MISSILE },
    { "PlasmaRifle",   WP_PLASMA },
    { "BFG9000",       WP_BFG }
  };
  int i;
  for (i = 0; i < (int)(sizeof(tbl) / sizeof(tbl[0])); i++)
    if (!strcasecmp(tbl[i].n, name))
      return tbl[i].slot;
  return -1;
}

/* The base weapon's native fire action, for WA_BASEFIRE delegation. */
static arg0_t weapon_base_fire(int slot)
{
  switch (slot)
  {
    case WP_FIST:         return (arg0_t)A_Punch;
    case WP_CHAINSAW:     return (arg0_t)A_Saw;
    case WP_PISTOL:       return (arg0_t)A_FirePistol;
    case WP_SHOTGUN:      return (arg0_t)A_FireShotgun;
    case WP_SUPERSHOTGUN: return (arg0_t)A_FireShotgun2;
    case WP_CHAINGUN:     return (arg0_t)A_FireCGun;
    case WP_MISSILE:      return (arg0_t)A_FireMissile;
    case WP_PLASMA:       return (arg0_t)A_FirePlasma;
    case WP_BFG:          return (arg0_t)A_FireBFG;
    default:              return (arg0_t)NULL;
  }
}

/* As weapon_base_fire, but for a weapon that supplies its own firing sound:
 * the stock sound the base attack would play is suppressed for the shot.  Only
 * the hitscan weapons play a stock sound; the rest fall back to the plain
 * delegate. */
static arg0_t weapon_base_fire_quiet(int slot)
{
  switch (slot)
  {
    case WP_PISTOL:       return (arg0_t)A_DecorateFirePistolQuiet;
    case WP_SHOTGUN:      return (arg0_t)A_DecorateFireShotgunQuiet;
    case WP_SUPERSHOTGUN: return (arg0_t)A_DecorateFireShotgun2Quiet;
    case WP_CHAINGUN:     return (arg0_t)A_DecorateFireCGunQuiet;
    default:              return weapon_base_fire(slot);
  }
}

/* As weapon_base_fire_quiet, but for a hitscan weapon that named BlueLaserPuff
 * in A_FireBullets: in addition to squelching the stock sound, the wrapper
 * raises the blue-puff override around the shot so the impacts show the blue
 * puff sprite.  Only the shotgun and SSG do this; others fall back to quiet. */
static arg0_t weapon_base_fire_bluepuff(int slot)
{
  switch (slot)
  {
    case WP_SHOTGUN:      return (arg0_t)A_DecorateFireShotgunBluePuff;
    case WP_SUPERSHOTGUN: return (arg0_t)A_DecorateFireShotgun2BluePuff;
    default:              return weapon_base_fire_quiet(slot);
  }
}

/* every 4-char sprite name appearing on a state line anywhere in the
 * DECORATE lump; R_InitSpriteDefs only unifies a sprite's art when the
 * lump actually redefines that sprite's sequence */
#define MAX_DECORATE_SPRITES 1024
static char sprite_names[MAX_DECORATE_SPRITES][5];
static int num_sprite_names;
static int parsed;              /* one-shot lazy parse */

static void parse_body(decorate_actor_t *a, const char *p, const char *end);
static const char *skip_space(const char *p, const char *end);
static const char *read_word(const char *p, const char *end,
                             char *out, size_t outsz);

/* Identify a weapon-state label.  Returns the WST_* index or -1.  ZDoom
 * weapons name many states; only the five the engine's weaponinfo can hold
 * are captured, plus the common aliases. */
static int weapon_state_label(const char *w)
{
  if (!strcasecmp(w, "Ready"))                            return WST_READY;
  if (!strcasecmp(w, "Select")  || !strcasecmp(w, "Up"))  return WST_SELECT;
  if (!strcasecmp(w, "Deselect")|| !strcasecmp(w, "Down"))return WST_DESEL;
  if (!strcasecmp(w, "Fire"))                             return WST_FIRE;
  if (!strcasecmp(w, "Flash"))                            return WST_FLASH;
  return -1;
}

/* Map a weapon-state action name to WA_*.  Unsupported actions become
 * WA_NONE (inert).  Firing actions (A_FireBullets/A_FireCGun/...) collapse to
 * WA_BASEFIRE: the registered chain delegates damage to the replaced base
 * weapon's native attack, so no ZDoom-only hitscan/puff is reinvented. */
static int weapon_action_of(const char *fn)
{
  if (!strcasecmp(fn, "A_WeaponReady"))  return WA_READY;
  if (!strcasecmp(fn, "A_Raise"))        return WA_RAISE;
  if (!strcasecmp(fn, "A_Lower"))        return WA_LOWER;
  if (!strcasecmp(fn, "A_GunFlash"))     return WA_GUNFLASH;
  if (!strcasecmp(fn, "A_ReFire") ||
      !strcasecmp(fn, "A_Refire"))       return WA_REFIRE;
  if (!strncasecmp(fn, "A_Fire", 6) ||
      !strcasecmp(fn, "A_Saw") ||
      !strcasecmp(fn, "A_Punch") ||
      !strcasecmp(fn, "A_CustomPunch") ||
      !strcasecmp(fn, "A_BFGsound"))     return WA_BASEFIRE;
  /* A_PlaySound / A_StartSound on a weapon frame play the sound on the player.
   * The mobj A_PlaySound reads its sfx from state->misc1, but the weapon path
   * uses misc1/misc2 as gun-sprite offsets, so a weapon-safe codepointer is
   * wired instead (build_weapon_chain) and the resolved sfx id is carried in a
   * side table keyed by the state, leaving misc1/misc2 zero. */
  if (!strcasecmp(fn, "A_PlaySound") ||
      !strcasecmp(fn, "A_StartSound")) return WA_PLAYSOUND;
  return WA_NONE;
}

/* Capture the frames of one weapon-state block into a->wst[idx].  p points
 * just past the label colon; advances until the block's terminator
 * (loop/stop/goto/wait/fail) or the next label, which it does not consume.
 * Each line is "SPR <letters> <tics> [bright] [A_action[(args)]]"; the "####"
 * / "----" sprite placeholder and "#"/"-" frame placeholder are honoured by
 * leaving the slot's sprite/frame unchanged from the previous frame. */
static const char *parse_weapon_state_block(decorate_actor_t *a, int idx,
                                            const char *p, const char *end)
{
  char prev_spr[5] = "----";
  short prev_frame = 0;

  a->wst[idx].present = 1;
  a->wst[idx].len     = 0;
  a->wst[idx].loops   = 0;
  a->wst[idx].goto_dst = -1;

  while (p < end)
  {
    char spr[MAX_NAME], fr[MAX_NAME], tics[MAX_NAME];
    const char *save = p;
    p = skip_space(p, end);
    if (p >= end) break;
    if (*p == '\n' || *p == '{' || *p == '}') { p++; continue; }

    p = read_word(p, end, spr, sizeof(spr));
    if (!spr[0]) { p++; continue; }

    /* DECORATE quotes the keep-sprite placeholder ("####"/"----") and
     * occasionally the frame; strip surrounding quotes so the placeholder
     * is seen as a 4-char token */
    if (spr[0] == '"')
    {
      size_t L = strlen(spr);
      if (L >= 2 && spr[L - 1] == '"')
      {
        memmove(spr, spr + 1, L - 2);
        spr[L - 2] = 0;
      }
    }

    /* a new label ("Fire:", "Hold:") ends this block; leave it unconsumed */
    if (p < end && *p == ':')
    {
      p = save;
      break;
    }
    /* block terminators */
    if (!strcasecmp(spr, "loop") || !strcasecmp(spr, "wait"))
    { a->wst[idx].loops = 1; break; }
    if (!strcasecmp(spr, "stop") || !strcasecmp(spr, "fail"))
    { a->wst[idx].loops = 0; break; }
    if (!strcasecmp(spr, "goto"))
    {
      /* "goto Label": record which of our captured states it targets so the
       * registrar can thread the terminal nextstate there (commonly Ready).
       * A goto to a state we do not model leaves goto_dst -1 -> freeze. */
      char dst[MAX_NAME];
      a->wst[idx].loops = 0;
      p = skip_space(p, end);
      p = read_word(p, end, dst, sizeof(dst));
      a->wst[idx].goto_dst = weapon_state_label(dst);
      break;
    }

    /* sprite must be 4 chars (or the "####"/"----" keep placeholder) */
    if (strlen(spr) != 4)
      continue;

    p = skip_space(p, end);
    p = read_word(p, end, fr, sizeof(fr));
    if (fr[0] == '"')
    {
      size_t L = strlen(fr);
      if (L >= 2 && fr[L - 1] == '"') { memmove(fr, fr + 1, L - 2); fr[L - 2] = 0; }
    }
    p = skip_space(p, end);
    p = read_word(p, end, tics, sizeof(tics));
    if (!fr[0] || !(tics[0] == '-' || (tics[0] >= '0' && tics[0] <= '9')))
      continue;                            /* not a state line */

    {
      int  t = atoi(tics);
      int  bright = 0, act = WA_NONE;
      short snd = -1;
      char b[MAX_NAME];
      const char *r = skip_space(p, end);
      size_t fi;
      const char *fr_ptr;
      char use_spr[5];
      int  keep_spr = (spr[0] == '#' || spr[0] == '-');

      r = read_word(r, end, b, sizeof(b));
      if (!strcasecmp(b, "bright"))
      {
        bright = FF_FULLBRIGHT;
        r = skip_space(r, end);
        r = read_word(r, end, b, sizeof(b));
      }
      if (b[0] == 'A' && b[1] == '_')
      {
        char fn[MAX_NAME];
        size_t bi = 0;
        while (b[bi] && b[bi] != '(') { fn[bi] = b[bi]; bi++; }
        fn[bi] = 0;
        act = weapon_action_of(fn);

        /* A_FireBullets(..., "BlueLaserPuff"): the shotgun and SSG name the blue
         * laser puff per shot rather than via a global "replaces BulletPuff".
         * The puff name is a later argument, past where read_word stopped, so
         * scan the rest of the action line (up to the newline) for it.  The
         * hitscan damage is delegated to the base weapon attack, which spawns
         * the global puff; note here that this weapon wants the blue puff so the
         * chain can raise the per-shot override around its fire. */
        if (act == WA_BASEFIRE)
        {
          const char *ls = r, *le = r;
          while (le < end && *le != '\n') le++;
          {
            const char *q;
            for (q = ls; q + 13 <= le; q++)
              if (!strncasecmp(q, "BlueLaserPuff", 13))
              { a->uses_blue_puff = 1; break; }
          }
        }

        /* A_PlaySound("name", ...): pull the quoted sound name out of the call
         * and register it in the shared sound table; the frame carries the
         * table index in snd, resolved to an sfx id at chain-build time. */
        if (act == WA_PLAYSOUND)
        {
          char arg[32];
          const char *ar = b + bi;
          int an = 0;
          while (*ar && *ar != '"') ar++;
          if (*ar == '"')
          {
            ar++;
            while (*ar && *ar != '"' && an < 31) arg[an++] = *ar++;
          }
          arg[an] = 0;
          if (arg[0] && num_decorate_sounds < MAX_DECORATE_SOUNDS)
          {
            int s;
            snd = -1;
            for (s = 0; s < num_decorate_sounds; s++)
              if (!strcasecmp(decorate_sounds[s], arg)) { snd = (short)s; break; }
            if (snd < 0)
            {
              int cn = 0;
              while (arg[cn] && cn < 31)
              { decorate_sounds[num_decorate_sounds][cn] = arg[cn]; cn++; }
              decorate_sounds[num_decorate_sounds][cn] = 0;
              snd = (short)num_decorate_sounds++;
            }
          }
          else
            act = WA_NONE;          /* nameless/overflow: leave inert */
        }
      }

      if (keep_spr) memcpy(use_spr, prev_spr, 5);
      else { memcpy(use_spr, spr, 4); use_spr[4] = 0; }
      if (t < 0) t = 0;
      if (t > 32767) t = 32767;

      /* one entry per frame letter; the action fires on the first */
      for (fi = 0, fr_ptr = fr; fr_ptr[fi]; fi++)
      {
        short fval;
        if (a->wst[idx].len >= MAX_WSTATE_FRAMES) break;
        if (fr_ptr[fi] == '#' || fr_ptr[fi] == '-')
          fval = prev_frame;             /* keep current frame */
        else if (fr_ptr[fi] >= 'A' && fr_ptr[fi] <= '_')
          fval = (short)(fr_ptr[fi] - 'A');
        else
          continue;
        {
          int k = a->wst[idx].len++;
          memcpy(a->wst[idx].fr[k].spr, use_spr, 5);
          a->wst[idx].fr[k].frame = (short)(fval | bright);
          a->wst[idx].fr[k].tics  = (short)t;
          a->wst[idx].fr[k].act   = (short)((fi == 0) ? act : WA_NONE);
          a->wst[idx].fr[k].snd   = (short)((fi == 0) ? snd : -1);
          prev_frame = fval;
          memcpy(prev_spr, use_spr, 5);
        }
      }
      p = r;
    }
  }
  return p;
}

/* ZDoom class names with fixed editor numbers, for chains that leave the
 * DECORATE lump.  Doom monsters plus ZDoom's Chex Quest class names (from
 * gzdoom's mapinfo/chex.txt DoomEdNums). */
static const struct { const char *name; int dn; } base_classes[] =
{
  { "ZombieMan",                  3004 },
  { "ShotgunGuy",                 9    },
  { "ChaingunGuy",                65   },
  { "DoomImp",                    3001 },
  { "Demon",                      3002 },
  { "Spectre",                    58   },
  { "LostSoul",                   3006 },
  { "Cacodemon",                  3005 },
  { "HellKnight",                 69   },
  { "BaronOfHell",                3003 },
  { "Arachnotron",                68   },
  { "PainElemental",              71   },
  { "Revenant",                   66   },
  { "Fatso",                      67   },
  { "Archvile",                   64   },
  { "SpiderMastermind",           7    },
  { "Cyberdemon",                 16   },
  { "WolfensteinSS",              84   },
  { "FlemoidusCommonus",          3004 },
  { "FlemoidusBipedicus",         9    },
  { "ArmoredFlemoidusBipedicus",  3001 },
  { "FlemoidusCycloptisCommonus", 3002 },
  { "Flembrane",                  3003 },
  { "ChexSoul",                   3006 },
  { NULL, 0 }
};

static const char *skip_space(const char *p, const char *end)
{
  while (p < end && (*p == ' ' || *p == '\t' || *p == '\r'))
    p++;
  return p;
}

static const char *read_word(const char *p, const char *end,
                             char *out, size_t outsz)
{
  size_t n = 0;
  while (p < end && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' &&
         *p != ':' && *p != '{')
  {
    if (n + 1 < outsz)
      out[n++] = *p;
    p++;
  }
  out[n] = 0;
  return p;
}

/* Resolve a user-var name declared on the actor being parsed to its base
 * slot, or -1.  Used while parsing A_SetUserVar/A_SetUserArray operands. */
static int decorate_uvar_slot(const decorate_actor_t *a, const char *name)
{
  int i;
  for (i = 0; i < a->num_uvars; i++)
    if (!strcasecmp(a->uvar[i].name, name))
      return a->uvar[i].base;
  return -1;
}

/* Record a state label at the current frame position (idempotent per name). */
static void decorate_add_label(decorate_actor_t *a, const char *name, int frame)
{
  int i;
  for (i = 0; i < a->num_seqlabels; i++)
    if (!strcasecmp(a->seqlabel[i].name, name))
      return;
  if (a->num_seqlabels >= MAX_SEQ_LABELS)
    return;
  {
    int n = 0;
    while (name[n] && n < 23) { a->seqlabel[a->num_seqlabels].name[n] = name[n]; n++; }
    a->seqlabel[a->num_seqlabels].name[n] = 0;
  }
  a->seqlabel[a->num_seqlabels].frame = (short)frame;
  a->num_seqlabels++;
}

/* Resolve a label name to the seqlabel[] index, or -1. */
static int decorate_label_index(const decorate_actor_t *a, const char *name)
{
  int i;
  for (i = 0; i < a->num_seqlabels; i++)
    if (!strcasecmp(a->seqlabel[i].name, name))
      return i;
  return -1;
}

/* Record that mobjtype `type` was built from actor `a` with its first frame at
 * state-table slot `base`, so labels can be resolved to states at runtime. */
static void decorate_record_statemap(int type, int base,
                                     const decorate_actor_t *a)
{
  if (num_statemaps < MAX_STATEMAPS && a)
  {
    statemaps[num_statemaps].type  = type;
    statemaps[num_statemaps].base  = base;
    statemaps[num_statemaps].actor = a;
    num_statemaps++;
  }
}

/* Public: resolve a DECORATE state label ("Missile", "SexScene_1_1", ...) on a
 * registered mobjtype to a concrete state index, or -1 if the type was not
 * built from DECORATE or has no such label.  Used by the ACS SetActorState
 * pcode to drive an actor to a named state. */
int U_DecorateStateForType(int type, const char *label)
{
  int i;
  if (!label || !label[0])
    return -1;
  for (i = 0; i < num_statemaps; i++)
    if (statemaps[i].type == type)
    {
      int li = decorate_label_index(statemaps[i].actor, label);
      if (li >= 0)
        return statemaps[i].base + statemaps[i].actor->seqlabel[li].frame;
      return -1;
    }
  return -1;
}

/* Parse a single token of a DECORATE expression into an operand kind/value:
 * a "var int" name, the "damage" property, "true"/"false", or an integer. */
static void parse_operand(const decorate_actor_t *a, const char *tok,
                          short *kind, short *val)
{
  char t[40];
  size_t n = 0;
  int slot;
  while (*tok == ' ' || *tok == '\t') tok++;
  while (tok[n] && n < sizeof(t) - 1) { t[n] = tok[n]; n++; }
  while (n > 0 && (t[n - 1] == ' ' || t[n - 1] == '\t')) n--;
  t[n] = 0;
  if (!strcasecmp(t, "true"))        { *kind = EXK_CONST; *val = 1; return; }
  if (!strcasecmp(t, "false"))       { *kind = EXK_CONST; *val = 0; return; }
  if (!strcasecmp(t, "damage"))      { *kind = EXK_DAMAGE; *val = 0; return; }
  slot = decorate_uvar_slot(a, t);
  if (slot >= 0)                     { *kind = EXK_UVAR; *val = (short)slot; return; }
  *kind = EXK_CONST; *val = (short)atoi(t);
}

/* Parse a DECORATE expression "A", "A + B", "A < B", etc. into a dexpr_t.
 * Only the small operator set the VN scripts use is recognised; an
 * unrecognised form degrades to operand A alone. */
static void parse_dexpr(const decorate_actor_t *a, const char *s, dexpr_t *e)
{
  char A[40], B[40];
  short op = EXOP_NONE;
  const char *opp = NULL, *q;
  size_t n;
  static const struct { const char *t; short op; } ops[] = {
    { "<=", EXOP_LE }, { ">=", EXOP_GE }, { "==", EXOP_EQ },
    { "!=", EXOP_NE }, { "+",  EXOP_ADD }, { "-",  EXOP_SUB },
    { "<",  EXOP_LT }, { ">",  EXOP_GT }
  };
  int i;
  memset(e, 0, sizeof(*e));
  for (i = 0; i < (int)(sizeof(ops) / sizeof(ops[0])); i++)
  {
    const char *f = strstr(s, ops[i].t);
    if (f) { opp = f; op = ops[i].op; break; }
  }
  if (opp)
  {
    int ol = (op == EXOP_ADD || op == EXOP_SUB ||
              op == EXOP_LT  || op == EXOP_GT) ? 1 : 2;
    n = (size_t)(opp - s); if (n >= sizeof(A)) n = sizeof(A) - 1;
    memcpy(A, s, n); A[n] = 0;
    q = opp + ol;
    n = 0; while (q[n] && n < sizeof(B) - 1) n++;
    memcpy(B, q, n); B[n] = 0;
    parse_operand(a, A, &e->ka, &e->va);
    parse_operand(a, B, &e->kb, &e->vb);
    e->op = op;
  }
  else
  {
    parse_operand(a, s, &e->ka, &e->va);
    e->op = EXOP_NONE;
  }
}

/* Copy the parenthesised argument list of an action starting at `start`
 * (which points at or before the '(') into out, dropping the outer parens
 * and any internal whitespace runs collapsed to single spaces.  Returns the
 * position just past the closing ')'. */
static const char *read_paren_args(const char *start, const char *end,
                                   char *out, size_t outsz)
{
  const char *p = start;
  size_t n = 0;
  int depth = 0;
  while (p < end && *p != '(') p++;
  if (p < end && *p == '(') { depth = 1; p++; }
  while (p < end && depth > 0)
  {
    char c = *p++;
    if (c == '(') depth++;
    else if (c == ')') { depth--; if (depth == 0) break; }
    if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    if (n + 1 < outsz) out[n++] = c;
  }
  out[n] = 0;
  return p;
}

/* Split a top-level comma-separated argument string into up to maxf fields
 * (quotes and nested parens are not split inside).  Returns the count. */
static int split_args(const char *s, char fields[][48], int maxf)
{
  int nf = 0, depth = 0, inq = 0;
  size_t k = 0;
  const char *p = s;
  if (maxf <= 0) return 0;
  fields[0][0] = 0;
  for (; *p; p++)
  {
    if (*p == '"') inq = !inq;
    if (!inq && *p == '(') depth++;
    else if (!inq && *p == ')') { if (depth) depth--; }
    if (!inq && depth == 0 && *p == ',')
    {
      fields[nf][k] = 0;
      if (++nf >= maxf) return nf;
      k = 0; fields[nf][0] = 0;
      continue;
    }
    if (k + 1 < 48) fields[nf][k++] = *p;
  }
  fields[nf][k] = 0;
  return nf + 1;
}

/* Trim leading/trailing spaces and surrounding quotes in place. */
static void trim_arg(char *s)
{
  char *a = s, *b;
  while (*a == ' ' || *a == '\t') a++;
  b = a + strlen(a);
  while (b > a && (b[-1] == ' ' || b[-1] == '\t')) b--;
  *b = 0;
  if (a[0] == '"' && b > a + 1 && b[-1] == '"') { a++; b[-1] = 0; }
  if (a != s) memmove(s, a, strlen(a) + 1);
}

/* Parse one "actor ..." header line starting at p (just past "actor"). */
static void parse_header(const char *p, const char *end)
{
  decorate_actor_t *a;
  char word[MAX_NAME];

  if (num_actors == MAX_DECORATE_ACTORS)
    return;
  a = &actors[num_actors];
  memset(a, 0, sizeof(*a));
  a->doomednum = -1;
  a->radius = a->height = -1;
  a->health = a->speed = a->painchance = a->mass = a->meleedamage = -1;
  a->alpha = FRACUNIT;
  a->wpn_slot = -1;
  a->xlat_id = -1;

  p = skip_space(p, end);
  p = read_word(p, end, a->name, sizeof(a->name));
  if (!a->name[0])
    return;

  while (p < end && *p != '\n' && *p != '{')
  {
    p = skip_space(p, end);
    if (p < end && *p == ':')
    {
      p = skip_space(p + 1, end);
      p = read_word(p, end, a->parent, sizeof(a->parent));
      continue;
    }
    p = read_word(p, end, word, sizeof(word));
    if (!word[0])
      break;
    if (!strcasecmp(word, "replaces"))
    {
      p = skip_space(p, end);
      p = read_word(p, end, a->replaces, sizeof(a->replaces));
    }
    else if (word[0] >= '0' && word[0] <= '9')
      a->doomednum = atoi(word);
    /* anything else ("native", editor comments) is ignored */
  }

  /* A custom weapon roots in a base weapon class through ": Base" or
   * "replaces Base"; record the slot it should take over.  "replaces" wins
   * (it names the weapon actually displaced) but the parent is the usual
   * carrier in this mod ("HPistol : Pistol replaces Pistol"). */
  if (a->replaces[0])
    a->wpn_slot = weapon_base_slot(a->replaces);
  if (a->wpn_slot < 0 && a->parent[0])
    a->wpn_slot = weapon_base_slot(a->parent);

  num_actors++;
}

/* Lump name a pk3 gives an included file: basename after the last slash, up
 * to the first '.', uppercased, max 8 chars (mirrors pk3_lump_name). */
static void decorate_include_name(char out[9], const char *path, size_t plen)
{
  const char *base = path;
  size_t i, n = 0;
  for (i = 0; i < plen; i++)
    if (path[i] == '/' || path[i] == '\\')
      base = path + i + 1;
  for (i = (size_t)(base - path); i < plen && path[i] != '.' && n < 8; i++)
  {
    char c = path[i];
    out[n++] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
  }
  out[n] = '\0';
}

/* Does this lump's content look like DECORATE (a top-level "actor" keyword)?
 * A PK3 flattens "decorate/vn/aosoth.txt" and "vns/aosoth.vns" to the same
 * 8-char lump name AOSOTH, so an #include must pick the one that is actually
 * DECORATE rather than whatever same-named lump the name table returns first. */
static int lump_is_decorate(int lump)
{
  size_t len, i;
  const char *txt;
  int   result = 0;
  if (lump < 0)
    return 0;
  len = W_LumpLength(lump);
  txt = W_CacheLumpNum(lump);
  if (!txt)
    return 0;
  for (i = 0; i + 5 < len && i < 4096; i++)
  {
    char c = txt[i];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
      continue;
    if (c == '/' )                  /* skip a // or / * comment line/block */
    {
      while (i < len && txt[i] != '\n') i++;
      continue;
    }
    if ((c == 'a' || c == 'A') && !strncasecmp(txt + i, "actor", 5) &&
        (txt[i + 5] == ' ' || txt[i + 5] == '\t'))
      result = 1;
    break;                          /* first real token decides */
  }
  W_UnlockLumpNum(lump);
  return result;
}

/* Resolve an #include lump name to the DECORATE lump that bears it, choosing
 * by content among same-named lumps (the name table alone is ambiguous in a
 * PK3).  Falls back to the first match if none looks like DECORATE. */
static int decorate_resolve_include(const char *nm, const char *fullpath)
{
  int first, l;
  /* Prefer an exact archive-path match: the basename truncates to eight
   * characters, so different folders sharing a basename (decorate/monster/
   * imp.txt vs decorate/sex/imp.txt) collide under the lump name "IMP" and a
   * name lookup would grab whichever was emitted last.  The full path picks
   * the intended member. */
  if (fullpath)
  {
    int p = W_PK3LumpForPath(fullpath);
    if (p >= 0)
      return p;
  }
  first = (W_CheckNumForName)(nm, ns_global);
  if (first < 0)
    return -1;
  for (l = first; l >= 0; l = (W_FindNumFromName)(nm, ns_global, l))
    if (lump_is_decorate(l))
      return l;
  return first;
}

#define DECORATE_MAX_LUMPS 128
static int parsed_lumps[DECORATE_MAX_LUMPS];
static int num_parsed_lumps;

/* Parse one DECORATE lump's actor definitions, following any #include
 * directives to the lumps the archive synthesised for them.  ZDoom packs
 * (ZDCMP2 etc.) keep a near-empty top-level DECORATE that only #includes the
 * real actor files under actors/..., so without this every custom prop --
 * palms, lamps, corpses, glass -- reads as an unknown thing and never spawns. */
static void parse_decorate_lump(int lump, int incdepth)
{
  size_t      len, i;
  const char *txt;
  int         depth = 0, k;
  int         incs[DECORATE_MAX_LUMPS];
  int         ninc = 0;

  if (lump < 0 || incdepth > 16)
    return;
  for (k = 0; k < num_parsed_lumps; k++)
    if (parsed_lumps[k] == lump)
      return;                         /* already parsed (cycle or dup) */
  if (num_parsed_lumps < DECORATE_MAX_LUMPS)
    parsed_lumps[num_parsed_lumps++] = lump;

  len = W_LumpLength(lump);
  txt = W_CacheLumpNum(lump);

  for (i = 0; i < len; i++)
  {
    char c = txt[i];
    if (c == '{')
      depth++;
    else if (c == '}')
    {
      if (depth > 0)
        depth--;
    }
    else if (depth == 0 && c == '#' && i + 8 < len &&
             !strncasecmp(txt + i, "#include", 8) &&
             (i == 0 || txt[i - 1] == '\n' || txt[i - 1] == '\r'))
    {
      size_t j = i + 8;
      while (j < len && txt[j] != '"' && txt[j] != '\n')
        j++;
      if (j < len && txt[j] == '"')
      {
        size_t s = j + 1, e = j + 1;
        while (e < len && txt[e] != '"' && txt[e] != '\n')
          e++;
        if (e < len && txt[e] == '"')
        {
          char nm[9];
          char fullp[192];
          size_t pn = e - s;
          int  inc;
          if (pn >= sizeof(fullp))
            pn = sizeof(fullp) - 1;
          memcpy(fullp, txt + s, pn);
          fullp[pn] = 0;
          decorate_include_name(nm, txt + s, e - s);
          inc = decorate_resolve_include(nm, fullp);
          if (inc >= 0 && ninc < DECORATE_MAX_LUMPS)
            incs[ninc++] = inc;
          j = e;
        }
      }
      i = j;
      continue;
    }
    else if (depth == 0 && (c == 'a' || c == 'A') && i + 6 < len &&
             !strncasecmp(txt + i, "actor", 5) &&
             (txt[i + 5] == ' ' || txt[i + 5] == '\t') &&
             (i == 0 || txt[i - 1] == '\n'))
    {
      size_t eol = i + 5;
      size_t body, bend;
      int    bdepth = 0;
      while (eol < len && txt[eol] != '\n' && txt[eol] != '{')
        eol++;
      parse_header(txt + i + 5, txt + eol);
      /* locate the body braces and parse the new actor's properties */
      body = eol;
      while (body < len && txt[body] != '{')
        body++;
      bend = body;
      while (bend < len)
      {
        if (txt[bend] == '{')
          bdepth++;
        else if (txt[bend] == '}' && --bdepth == 0)
          break;
        bend++;
      }
      if (num_actors > 0 && body < len && bend > body)
        parse_body(&actors[num_actors - 1], txt + body + 1, txt + bend);
      i = bend;
      depth = 0;
      continue;
    }
  }
  W_UnlockLumpNum(lump);

  /* recurse after unlocking, so at most incdepth lumps are cached at once */
  for (k = 0; k < ninc; k++)
    parse_decorate_lump(incs[k], incdepth + 1);
}

/* Mutable actor lookup by name (parse-time), or NULL. */
static decorate_actor_t *find_actor_mut(const char *name)
{
  int i;
  for (i = 0; i < num_actors; i++)
    if (!strcasecmp(actors[i].name, name))
      return &actors[i];
  return NULL;
}

/* DECORATE single inheritance: a child ": Parent" inherits the parent's user
 * variables, its Damage, and its whole state machine.  The parent's frames are
 * appended after the child's own and every parent label is recorded twice --
 * under its plain name (so a label the child does not define resolves to the
 * parent's) and under "Super::<name>" (so the child's "goto Super::Spawn"
 * reaches the parent's chain).  Parent-internal flow/jump targets are shifted
 * by the child's frame count so they keep pointing at the right frames.  Only
 * a single DECORATE-defined parent level is merged, which is what the content
 * here needs; engine base classes (no captured frames) contribute nothing. */
static void inherit_one(decorate_actor_t *c)
{
  decorate_actor_t *pp = find_actor_mut(c->parent);
  int base, u, l, f, li;
  if (!pp || !pp->name[0])
    return;
  /* recurse so a grandparent is merged into the parent first */
  if (pp->parent[0] && !pp->inherited)
    inherit_one(pp);

  /* user variables: inherit any the child does not itself declare, keeping
   * the child's slot layout first and appending the parent's */
  for (u = 0; u < pp->num_uvars; u++)
  {
    if (decorate_uvar_slot(c, pp->uvar[u].name) >= 0)
      continue;
    if (c->num_uvars >= MAX_USERVARS)
      break;
    {
      int k = c->num_uvars++;
      memcpy(c->uvar[k].name, pp->uvar[u].name, sizeof(c->uvar[k].name));
      c->uvar[k].base = (short)c->uvar_slots;
      c->uvar[k].len  = pp->uvar[u].len;
      c->uvar_slots  += pp->uvar[u].len;
    }
  }

  if (c->damage == 0 && pp->damage != 0)
    c->damage = pp->damage;
  if (pp->use_special)
    c->use_special = 1;

  /* inherit any sound the child did not set itself: ImpEncounter2..4 derive
   * from ImpEncounter1 (which defines the see/pain/death sounds) and add no
   * sounds of their own, so without this they would keep the stock sounds of
   * the monster they replace instead of the mod's. */
  if (!c->seesound[0]    && pp->seesound[0])
    memcpy(c->seesound,    pp->seesound,    sizeof(c->seesound));
  if (!c->painsound[0]   && pp->painsound[0])
    memcpy(c->painsound,   pp->painsound,   sizeof(c->painsound));
  if (!c->deathsound[0]  && pp->deathsound[0])
    memcpy(c->deathsound,  pp->deathsound,  sizeof(c->deathsound));
  if (!c->attacksound[0] && pp->attacksound[0])
    memcpy(c->attacksound, pp->attacksound, sizeof(c->attacksound));
  if (!c->activesound[0] && pp->activesound[0])
    memcpy(c->activesound, pp->activesound, sizeof(c->activesound));

  /* append the parent's captured frames after the child's.  The parent's
   * states are essential -- the child's Spawn jumps into them ("goto TooLate")
   * and the use action runs the parent's Active label -- so when the merge
   * would overflow, drop frames from the tail of the child's own sequence
   * (its later, optional states such as the interactive scenes) to make room
   * rather than skipping the merge and losing the parent states entirely. */
  base = c->seq_len;
  if (base + pp->seq_len > MAX_SPAWN_FRAMES)
  {
    int room = MAX_SPAWN_FRAMES - pp->seq_len;
    int l2;
    if (room < 1)
      return;               /* parent alone does not fit; nothing sensible */

    /* The parent frames do not fit after the whole child sequence.  Two very
     * different kinds of child reach here:
     *
     *  - A map-replacement monster variant (it carries a "replaces" target)
     *    redefines its own full state machine -- Spawn/See/Faint/... -- and
     *    builds its death from its own Faint; it needs none of the parent's
     *    frames, and trimming its sequence would corrupt its own reachable
     *    states.  Keep it whole and append no parent frames; bind the parent
     *    labels to the child's own states so a "goto Super::X" still resolves.
     *
     *  - A spawned follow-on actor (no "replaces") relies on the parent's base
     *    states (its Active/TooLate, reached from the child's own Spawn), and
     *    its own trailing frames are optional.  Trim that tail to make room and
     *    append the parent. */
    if (c->replaces[0])
    {
      for (l = 0; l < pp->num_seqlabels; l++)
      {
        char sup[32];
        int ci = decorate_label_index(c, pp->seqlabel[l].name);
        int frame = (ci >= 0) ? c->seqlabel[ci].frame
                              : (c->seq_len > 0 ? c->seq_len - 1 : 0);
        const char *pre = "Super::";
        int nn = 0, mm = 0;
        if (ci < 0)
          decorate_add_label(c, pp->seqlabel[l].name, frame);
        while (pre[nn]) { sup[mm++] = pre[nn++]; }
        nn = 0;
        while (pp->seqlabel[l].name[nn] && mm < 31)
        { sup[mm++] = pp->seqlabel[l].name[nn++]; }
        sup[mm] = 0;
        if (decorate_label_index(c, sup) < 0)
          decorate_add_label(c, sup, frame);
      }
      if (pp->multi_state)
        c->multi_state = 1;
      c->inherited = 1;
      return;
    }

    base = room;            /* keep the child's first `room` frames */
    for (l2 = 0; l2 < c->num_seqlabels; l2++)
      if (c->seqlabel[l2].frame >= base)
        c->seqlabel[l2].frame = (short)(base - 1);
  }
  for (f = 0; f < pp->seq_len; f++)
  {
    c->seq[base + f]   = pp->seq[f];
    c->seqop[base + f] = pp->seqop[f];
    c->seqflow[base + f] = pp->seqflow[f];
    /* The parent's frames may use the "####"/"----" keep-sprite placeholder
     * (StoryCharacter / SexActor base states are entirely "####"); ZDoom keeps
     * the sprite the actor already displays.  When such a frame is appended
     * after the child's own real spawn sprite (e.g. Lilitu's "TROO A" then
     * "goto Super::Spawn" into the parent's "####" Idle loop), carry the most
     * recent real sprite forward instead of leaving the literal placeholder,
     * which has no patches and renders the actor invisible. */
    if (c->seq[base + f].spr[0] == '#' || c->seq[base + f].spr[0] == '-')
    {
      if (base + f > 0)
        memcpy(c->seq[base + f].spr, c->seq[base + f - 1].spr, 5);
      else if (c->seq_sprite[0] &&
               c->seq_sprite[0] != '#' && c->seq_sprite[0] != '-')
        memcpy(c->seq[base + f].spr, c->seq_sprite, 4);
    }
    /* shift a numeric/relative goto's resolved-later targets: the parent's
     * label-name targets resolve through the merged label table, so only the
     * label frames themselves need the base offset (done below). */
  }
  if (c->seq_sprite[0] == 0)
    memcpy(c->seq_sprite, pp->seq_sprite, sizeof(c->seq_sprite));
  c->seq_len = base + pp->seq_len;
  if (pp->multi_state)
    c->multi_state = 1;

  /* record parent labels, offset into the appended region, under both the
   * plain name (if the child lacks it) and the Super:: alias */
  for (l = 0; l < pp->num_seqlabels; l++)
  {
    char sup[32];
    int frame = base + pp->seqlabel[l].frame;
    if (decorate_label_index(c, pp->seqlabel[l].name) < 0)
      decorate_add_label(c, pp->seqlabel[l].name, frame);
    sup[0] = 0;
    {
      const char *pre = "Super::";
      int n = 0, m = 0;
      while (pre[n]) { sup[m++] = pre[n++]; }
      n = 0;
      while (pp->seqlabel[l].name[n] && m < 31)
      { sup[m++] = pp->seqlabel[l].name[n++]; }
      sup[m] = 0;
    }
    decorate_add_label(c, sup, frame);
  }

  /* fix the child's own "goto Super::X" terminators: a SEQF_GOTO whose target
   * name now exists in the label table needs no change (resolved by name at
   * registration).  Numeric A_JumpIf offsets in the parent region were copied
   * verbatim and remain relative, which is correct. */
  (void)li;
  c->inherited = 1;
}

static void inherit_from_parents(void)
{
  int i;
  for (i = 0; i < num_actors; i++)
    if (actors[i].parent[0] && !actors[i].inherited)
      inherit_one(&actors[i]);
}

static void parse_decorate(void)
{
  int lump;

  parsed = 1;
  num_decorate_sounds = 0;
  lump = (W_CheckNumForName)("DECORATE", ns_global);
  if (lump < 0)
    return;
  num_parsed_lumps = 0;
  parse_decorate_lump(lump, 0);

  if (num_actors)
    lprintf(LO_INFO, "U_ParseDecorate: %d actor headers\n", num_actors);

  inherit_from_parents();
}

static const decorate_actor_t *find_actor(const char *name)
{
  int i;
  for (i = 0; i < num_actors; i++)
    if (!strcasecmp(actors[i].name, name))
      return &actors[i];
  return NULL;
}

/* True when `name` is (or derives from) a ZDoom inventory/pickup base class.
 * Walks the DECORATE parent chain up to a known inventory root.  These actors
 * are collected by touching them; the engine marks them MF_SPECIAL so the
 * player can pick them up and so any thing-special they carry fires on pickup
 * (e.g. a logbook whose ACS_Execute raises a lift). */
static int class_is_inventory(const char *name)
{
  int hops = 0;
  while (name && name[0] && hops++ < 16)
  {
    const decorate_actor_t *p;
    if (!strcasecmp(name, "Inventory")       ||
        !strcasecmp(name, "FakeInventory")   ||
        !strcasecmp(name, "CustomInventory") ||
        !strcasecmp(name, "Health")          ||
        !strcasecmp(name, "Ammo")            ||
        !strcasecmp(name, "PuzzleItem")      ||
        !strcasecmp(name, "ScoreItem")       ||
        !strcasecmp(name, "DummyStrifeItem"))
      return 1;
    p = find_actor(name);
    if (!p || !p->parent[0])
      return 0;
    name = p->parent;
  }
  return 0;
}

/* Decide whether a DECORATE actor should be a touch-collected pickup
 * (MF_SPECIAL).  Inventory/pickup-derived non-monster actors are, EXCEPT
 * +USESPECIAL actors, which are use-activated interactive objects (NPCs,
 * conversation starters) that must persist and respond to use rather than be
 * collected and removed on touch.  Exposed as a named predicate so the rule is
 * explicit and can be regression-checked directly. */
static int decorate_is_touch_pickup(const decorate_actor_t *a)
{
  if (!a || a->is_monster || a->use_special)
    return 0;
  return a->is_pickup || class_is_inventory(a->parent);
}

#ifdef ACS_SELFTEST
/* Regression check for the touch-pickup classification.  A +USESPECIAL actor
 * (an interactive NPC / conversation starter), even one derived from
 * CustomInventory, must NOT be a touch-collected pickup -- otherwise walking
 * into it collects and removes it before it can be used, so its conversation
 * never triggers.  Returns the number of failed checks. */
int U_DecoratePickupSelfTest(void);
int U_DecoratePickupSelfTest(void)
{
  int fail = 0;
  decorate_actor_t a;

  /* A plain inventory item is a touch pickup. */
  memset(&a, 0, sizeof a);
  strncpy(a.parent, "Inventory", sizeof a.parent - 1);
  if (!decorate_is_touch_pickup(&a))
  { fail++; lprintf(LO_ERROR, "ACS-SELFTEST FAIL: inventory item should be a touch pickup\n"); }
  else lprintf(LO_INFO, "ACS-SELFTEST ok:   inventory item is a touch pickup\n");

  /* A +USESPECIAL CustomInventory actor is an interactive object, NOT a
   * touch pickup -- this is the conversation-starter case that regressed. */
  memset(&a, 0, sizeof a);
  strncpy(a.parent, "CustomInventory", sizeof a.parent - 1);
  a.use_special = 1;
  if (decorate_is_touch_pickup(&a))
  { fail++; lprintf(LO_ERROR, "ACS-SELFTEST FAIL: +USESPECIAL actor must not be a touch pickup\n"); }
  else lprintf(LO_INFO, "ACS-SELFTEST ok:   +USESPECIAL interactive actor is not consumed on touch\n");

  /* A monster is never a touch pickup regardless of parent. */
  memset(&a, 0, sizeof a);
  strncpy(a.parent, "Inventory", sizeof a.parent - 1);
  a.is_monster = 1;
  if (decorate_is_touch_pickup(&a))
  { fail++; lprintf(LO_ERROR, "ACS-SELFTEST FAIL: a monster must not be a touch pickup\n"); }
  else lprintf(LO_INFO, "ACS-SELFTEST ok:   a monster is not a touch pickup\n");

  return fail;
}
#endif /* ACS_SELFTEST */

static int base_class_doomednum(const char *name)
{
  int i;
  for (i = 0; base_classes[i].name; i++)
    if (!strcasecmp(base_classes[i].name, name))
      return base_classes[i].dn;
  return -1;
}

/* Resolve a class name to an editor number by walking parent-then-replaces
 * links.  `from` is the actor whose number we are resolving, so its own
 * doomednum (the unknown one) never terminates the walk. */
static int resolve_class(const char *name, const decorate_actor_t *from,
                         int depth)
{
  const decorate_actor_t *a;
  int dn;

  if (depth > 16)
    return -1;

  a = find_actor(name);
  if (!a)
    return base_class_doomednum(name);

  if (a != from && a->doomednum >= 0)
    return a->doomednum;
  if (a->parent[0])
  {
    dn = resolve_class(a->parent, from, depth + 1);
    if (dn >= 0)
      return dn;
  }
  if (a->replaces[0])
    return resolve_class(a->replaces, from, depth + 1);
  return -1;
}

/* Parse the body span of the most recently added actor for the few
 * properties simple static decorations use.  Anything beyond a single
 * "SPRT F -1" spawn frame leaves spawn_static unset and the actor is not
 * registered. */
static void parse_body(decorate_actor_t *a, const char *p, const char *end)
{
  char word[MAX_NAME];
  int  in_spawn = 0;

  while (p < end)
  {
    p = skip_space(p, end);
    if (p >= end)
      break;
    if (*p == '\n' || *p == '{' || *p == '}')
    {
      p++;
      continue;
    }
    p = read_word(p, end, word, sizeof(word));
    if (!word[0])
    {
      p++;
      continue;
    }

    /* read_word stops before ':': a state label arrives as the bare word
     * with the colon still unconsumed */
    if (p < end && *p == ':')
    {
      p++;
      in_spawn = !strcasecmp(word, "Spawn");
      /* a weapon actor's named state blocks are captured separately and
       * wired into weaponinfo at registration */
      if (a->wpn_slot >= 0)
      {
        int wi = weapon_state_label(word);
        if (wi >= 0)
        {
          p = parse_weapon_state_block(a, wi, p, end);
          in_spawn = 0;
          continue;
        }
      }
      /* Non-weapon decoration: record every state label against the current
       * frame position so goto / A_JumpIf targets resolve, and keep capturing
       * frames under it.  A label other than Spawn means this actor needs the
       * multi-label state machine rather than the flat Spawn path. */
      if (a->wpn_slot < 0 && !a->spawn_static)
      {
        decorate_add_label(a, word, a->seq_len);
        if (!in_spawn)
        {
          a->multi_state = 1;
          in_spawn = 1;       /* continue capturing frames under this label */
        }
      }
      continue;
    }

    if (!strcasecmp(word, "Radius"))
    {
      p = skip_space(p, end);
      p = read_word(p, end, word, sizeof(word));
      a->radius = atoi(word);
    }
    else if (!strcasecmp(word, "Health"))
    {
      p = skip_space(p, end);
      p = read_word(p, end, word, sizeof(word));
      a->health = atoi(word);
    }
    else if (!strcasecmp(word, "Speed"))
    {
      p = skip_space(p, end);
      p = read_word(p, end, word, sizeof(word));
      a->speed = atoi(word);
    }
    else if (!strcasecmp(word, "PainChance"))
    {
      p = skip_space(p, end);
      p = read_word(p, end, word, sizeof(word));
      a->painchance = atoi(word);
    }
    else if (!strcasecmp(word, "Mass"))
    {
      p = skip_space(p, end);
      p = read_word(p, end, word, sizeof(word));
      a->mass = atoi(word);
    }
    else if (!strcasecmp(word, "MeleeDamage"))
    {
      p = skip_space(p, end);
      p = read_word(p, end, word, sizeof(word));
      a->meleedamage = atoi(word);
    }
    else if (!strcasecmp(word, "Monster"))
      a->is_monster = 1;
    else if (!strcasecmp(word, "+FLOAT") || !strcasecmp(word, "+FLY"))
      a->m_float = 1;
    else if (!strcasecmp(word, "-COUNTKILL"))
      a->m_nocountkill = 1;
    else if (!strcasecmp(word, "+COUNTITEM"))
      a->m_countitem = 1;
    else if (!strcasecmp(word, "Damage"))
    {
      p = skip_space(p, end);
      p = read_word(p, end, word, sizeof(word));
      a->damage = atoi(word);
    }
    else if (!strcasecmp(word, "Translation"))
    {
      /* "Translation \"a:b=c:d\", \"e:f=g:h\", ...": a palette colour remap.
       * Capture the raw text to the end of the line; it is turned into a
       * 256-byte translation table at registration (the palette is loaded by
       * then).  Source ranges map onto destination ranges. */
      const char *ls = p;
      const char *le = ls;
      int n;
      while (le < end && *le != '\n' && *le != '\r') le++;
      n = (int)(le - ls);
      if (n > (int)sizeof(a->xlat_raw) - 1)
        n = (int)sizeof(a->xlat_raw) - 1;
      memcpy(a->xlat_raw, ls, (size_t)n);
      a->xlat_raw[n] = 0;
      p = le;
    }
    else if (!strcasecmp(word, "SeeSound")  ||
             !strcasecmp(word, "PainSound") ||
             !strcasecmp(word, "DeathSound")||
             !strcasecmp(word, "AttackSound") ||
             !strcasecmp(word, "ActiveSound"))
    {
      /* "<Kind>Sound \"logical/name\"": capture the logical sound name so the
       * monster registrar can override the cloned base sound.  The value is a
       * quoted string; read it and strip the surrounding quotes. */
      char snd[40];
      char *dst = (!strcasecmp(word, "SeeSound"))   ? a->seesound   :
                  (!strcasecmp(word, "PainSound"))  ? a->painsound  :
                  (!strcasecmp(word, "DeathSound")) ? a->deathsound :
                  (!strcasecmp(word, "AttackSound"))? a->attacksound:
                                                      a->activesound;
      p = skip_space(p, end);
      p = read_word(p, end, snd, sizeof(snd));
      {
        char *s = snd;
        int   n;
        if (*s == '"') s++;            /* drop leading quote */
        n = (int)strlen(s);
        if (n > 0 && s[n-1] == '"') s[n-1] = 0;   /* drop trailing quote */
        strncpy(dst, s, 31);
        dst[31] = 0;
      }
    }
    else if (!strcasecmp(word, "var"))
    {
      /* "var int name;" or "var int name[N];" -- assign the name a base
       * slot (and length) in this actor's user-var array. */
      char ty[MAX_NAME], nm[MAX_NAME];
      p = skip_space(p, end);
      p = read_word(p, end, ty, sizeof(ty));      /* type, e.g. "int" */
      p = skip_space(p, end);
      p = read_word(p, end, nm, sizeof(nm));       /* name (maybe name[N] */
      if (nm[0] && a->num_uvars < MAX_USERVARS)
      {
        int len = 1;
        char *br;
        char *sc = strchr(nm, ';');     /* "name;" -> drop the terminator */
        if (sc) *sc = 0;
        br = strchr(nm, '[');
        if (br) { len = atoi(br + 1); if (len < 1) len = 1; *br = 0; }
        else
        {
          /* "name [N]" with a space: peek for a bracketed count */
          const char *q = skip_space(p, end);
          if (q < end && *q == '[')
          {
            char cnt[MAX_NAME];
            q = read_word(q + 1, end, cnt, sizeof(cnt));
            len = atoi(cnt); if (len < 1) len = 1;
            p = q;
            while (p < end && *p != ']' && *p != '\n') p++;
            if (p < end && *p == ']') p++;
          }
        }
        {
          int k = a->num_uvars++;
          int n = 0;
          while (nm[n] && n < 27) { a->uvar[k].name[n] = nm[n]; n++; }
          a->uvar[k].name[n] = 0;
          a->uvar[k].base = (short)a->uvar_slots;
          a->uvar[k].len  = (short)len;
          a->uvar_slots  += len;
        }
      }
    }
    else if (!strcasecmp(word, "Height"))
    {
      p = skip_space(p, end);
      p = read_word(p, end, word, sizeof(word));
      a->height = atoi(word);
    }
    else if (!strcasecmp(word, "+SOLID"))
      a->solid = 1;
    else if (!strcasecmp(word, "+USESPECIAL"))
      a->use_special = 1;
    else if (!strcasecmp(word, "+NOGRAVITY"))
      a->nogravity = 1;
    else if (!strcasecmp(word, "+NOINTERACTION"))
      /* NOINTERACTION actors are frozen out of the physics/clip pipeline;
       * for the purposes here that means no gravity (they hover) -- enough
       * for decorative spawns like the hovering hearts not to fall. */
      a->nogravity = 1;
    else if (!strcasecmp(word, "+SPAWNCEILING"))
      a->spawnceiling = 1;
    else if (!strcasecmp(word, "RenderStyle"))
    {
      char rs[MAX_NAME];
      p = skip_space(p, end);
      p = read_word(p, end, rs, sizeof(rs));
      if (!strcasecmp(rs, "Translucent") || !strcasecmp(rs, "Add") ||
          !strcasecmp(rs, "Stencil")     || !strcasecmp(rs, "Shaded"))
        a->translucent = 1;
    }
    else if (!strcasecmp(word, "Alpha"))
    {
      char av[MAX_NAME];
      p = skip_space(p, end);
      p = read_word(p, end, av, sizeof(av));
      /* "0.4" -> 16.16; atof avoided (no float determinism worry here,
       * but keep it integer-parsed for consistency) */
      {
        int whole = 0, frac = 0, scale = 1, seen_dot = 0;
        const char *s = av;
        while (*s)
        {
          if (*s == '.') seen_dot = 1;
          else if (*s >= '0' && *s <= '9')
          {
            if (!seen_dot) whole = whole * 10 + (*s - '0');
            else { frac = frac * 10 + (*s - '0'); scale *= 10; }
          }
          s++;
        }
        a->alpha = whole * FRACUNIT + (scale > 1 ? (frac * FRACUNIT) / scale : 0);
        if (a->alpha > FRACUNIT) a->alpha = FRACUNIT;
        if (a->alpha < 0)        a->alpha = 0;
      }
    }
    else if (in_spawn && !a->spawn_static && a->seq_len > 0 &&
             (!strcasecmp(word, "loop") || !strcasecmp(word, "stop") ||
              !strcasecmp(word, "wait")))
    {
      /* terminator for an animated sequence ("loop"/"wait" repeat, "stop"
       * freezes on the last frame).  Record it both in seq_loops (the flat
       * single-Spawn path) and in this frame's seqflow: an actor can turn out
       * to be multi-state only after a later label, so the flow must be
       * captured now rather than gated on multi_state being set yet. */
      a->seq_loops = strcasecmp(word, "stop") != 0;
      {
        int lf = a->seq_len - 1;
        a->seqflow[lf].flow = !strcasecmp(word, "loop") ? SEQF_LOOP
                            : !strcasecmp(word, "wait") ? SEQF_WAIT
                            : SEQF_STOP;
      }
      /* Do not stop capturing here.  A terminator ends the *run* of frames
       * that flows into it, but in a multi-label actor the frames textually
       * after it (before the next label or the closing brace) are still real
       * states reached by an A_JumpIf / goto -- e.g. the StoryCharacter Active
       * block jumps past its own "wait" to a trailing frame.  Dropping them
       * left the jump pointing one past the actor's last state, into the next
       * actor's frames (the dialogue NPCs turned into the imp afterwards).
       * Keep capturing; a new label or "}" ends the block as before. */
    }
    else if (in_spawn && !a->spawn_static && a->seq_len > 0 &&
             !strcasecmp(word, "goto"))
    {
      /* "goto Label": the last captured frame jumps to the named label.  The
       * target is resolved to a state index at registration.  The label may
       * be scoped ("Super::Spawn"), so read past ':' which read_word stops on. */
      char dst[MAX_NAME];
      int lf = a->seq_len - 1;
      size_t dn = 0;
      p = skip_space(p, end);
      while (p < end && *p != ' ' && *p != '\t' && *p != '\r' &&
             *p != '\n' && *p != '{' && dn + 1 < sizeof(dst))
        dst[dn++] = *p++;
      dst[dn] = 0;
      a->seqflow[lf].flow   = SEQF_GOTO;
      {
        int k = 0;
        while (dst[k] && k < 23) { a->seqflow[lf].tname[k] = dst[k]; k++; }
        a->seqflow[lf].tname[k] = 0;
      }
      a->multi_state = 1;
      in_spawn = 0;
    }
    else if (strlen(word) == 4 ||
             (word[0] == '"' &&
              (word[1] == '#' || word[1] == '-')))
    {
      /* a state line is "SPRT ABCD 5 [action]": a 4-char word, a word of
       * frame letters, then a numeric tic count.  Record the sprite name;
       * properties never match (their value words are not all letters or
       * are not followed by a number).  ZDoom's quoted keep-sprite
       * placeholder ("####"/"----") is accepted too -- such an actor carries
       * no sprite of its own (a logic-only actor), so its frames render
       * nothing but still run their actions. */
      char fr[MAX_NAME], tics[MAX_NAME];
      char spr[8];
      int  keep_spr = 0;
      const char *q = skip_space(p, end);
      /* normalise the sprite word: strip surrounding quotes, detect keep */
      {
        const char *wp = word;
        int n = 0;
        if (wp[0] == '"') wp++;
        while (wp[n] && wp[n] != '"' && n < 4) { spr[n] = wp[n]; n++; }
        spr[n] = 0;
        if (spr[0] == '#' || spr[0] == '-') keep_spr = 1;
      }
      q = read_word(q, end, fr, sizeof(fr));
      /* the frame-letter word may be quoted as well */
      if (fr[0] == '"')
      {
        int n = 0; const char *fp = fr + 1;
        while (fp[n] && fp[n] != '"') { fr[n] = fp[n]; n++; }
        fr[n] = 0;
      }
      q = skip_space(q, end);
      q = read_word(q, end, tics, sizeof(tics));
      if (fr[0] && (tics[0] == '-' || (tics[0] >= '0' && tics[0] <= '9')))
      {
        size_t fi;
        int letters = 1;
        for (fi = 0; fr[fi]; fi++)
          if ((fr[fi] < 'A' || fr[fi] > '_') && fr[fi] != '#' && fr[fi] != '-')
            letters = 0;
        if (letters)
        {
          int k;
          if (!keep_spr)
          {
            for (k = 0; k < num_sprite_names; k++)
              if (!strncasecmp(sprite_names[k], spr, 4))
                break;
            if (k == num_sprite_names && k < MAX_DECORATE_SPRITES)
            {
              memcpy(sprite_names[k], spr, 4);
              sprite_names[k][4] = 0;
              num_sprite_names++;
            }
          }
        }
      }
      if (in_spawn && !a->spawn_static)
      {
      /* A Spawn-state line is "SPRT <frames> <tics> [BRIGHT] [action]".
       * Two shapes are captured (action functions are otherwise ignored):
       *   "SPRT F -1"     -> a single frozen frame (spawn_static)
       *   "SPRT ABCD 10"  -> an animated sequence, terminated by loop/stop
       * A quoted keep-sprite/frame placeholder ("####" # ...) parses as the
       * literal sprite word here -- such logic-only frames still run their
       * actions, and the sprite they name renders harmlessly if absent. */
      char fr[MAX_NAME], tics[MAX_NAME];
      char nspr[8];
      const char *q = skip_space(p, end);
      {
        const char *wp = word; int n = 0;
        if (wp[0] == '"') wp++;
        while (wp[n] && wp[n] != '"' && n < 4) { nspr[n] = wp[n]; n++; }
        nspr[n] = 0;
      }
      q = read_word(q, end, fr, sizeof(fr));
      if (fr[0] == '"')
      {
        int n = 0; const char *fp = fr + 1;
        while (fp[n] && fp[n] != '"') { fr[n] = fp[n]; n++; }
        fr[n] = 0;
      }
      q = skip_space(q, end);
      q = read_word(q, end, tics, sizeof(tics));
      if (strlen(fr) == 1 && ((fr[0] >= 'A' && fr[0] <= 'Z') ||
          fr[0] == '#' || fr[0] == '-') && !strcmp(tics, "-1") &&
          a->seq_len == 0 && !a->multi_state)
      {
        const char *r = skip_space(q, end);
        char b[MAX_NAME];
        memcpy(a->sprite, nspr, 4);
        a->sprite[4] = 0;
        a->frame = (fr[0] == '#' || fr[0] == '-') ? 0 : fr[0] - 'A';
        read_word(r, end, b, sizeof(b));
        if (!strcasecmp(b, "BRIGHT"))
          a->frame |= FF_FULLBRIGHT;
        a->spawn_static = 1;
        a->seq_len = 0;          /* a frozen frame supersedes any sequence */
        p = q;
      }
      else if (((fr[0] >= 'A' && fr[0] <= '_') ||
                fr[0] == '#' || fr[0] == '-') &&
               ((tics[0] >= '0' && tics[0] <= '9') ||
                (tics[0] == '-' && tics[1] == '1')))
      {
        /* animated frames: one entry per frame letter.  The sprite may change
         * between lines of one sequence (e.g. TROO ... then STTR ...), so the
         * sprite is recorded per frame rather than once for the whole run. */
        int t = atoi(tics);
        int bright = 0;
        short act = DA_NONE, snd = -1;
        short cmiss_id = -1;       /* A_CustomMissile class index, or -1 */
        struct { short uvslot; short flag; short fval; dexpr_t idx; dexpr_t val; }
              opstage;
        struct { short has_jump; short jtoff; short jchance; char tname[24]; dexpr_t jcond; }
              flowstage;
        const char *r = skip_space(q, end);
        const char *act_src;
        char b[MAX_NAME];
        size_t fi;
        memset(&opstage, 0, sizeof(opstage));
        memset(&flowstage, 0, sizeof(flowstage));
        flowstage.jtoff = -1;
        act_src = r;
        r = read_word(r, end, b, sizeof(b));
        if (!strcasecmp(b, "BRIGHT"))
        {
          bright = FF_FULLBRIGHT;
          r = skip_space(r, end);
          act_src = r;
          r = read_word(r, end, b, sizeof(b));   /* action may follow BRIGHT */
        }
        /* a safe, self-contained per-frame action.  Parameterised forms
         * (A_PlaySound) keep their first string argument; everything else is
         * left as DA_NONE so unsupported actions are simply inert. */
        if ((b[0] == 'A' && b[1] == '_') ||
            !strncasecmp(b, "ACS_", 4))
        {
          char fn[MAX_NAME];
          const char *ar;
          size_t bi = 0;
          /* split "A_PlaySound(\"x\"..." into name + first arg */
          while (b[bi] && b[bi] != '(') { fn[bi] = b[bi]; bi++; }
          fn[bi] = 0;
          if (!strcasecmp(fn, "A_PlaySound") ||
              !strcasecmp(fn, "A_PlaySoundEx") ||
              !strcasecmp(fn, "A_StartSound"))
          {
            char arg[32];
            /* find the opening quote either in b (glued) or the next word */
            ar = strchr(b, '"');
            if (!ar)
            {
              char nx[MAX_NAME];
              const char *r2 = skip_space(r, end);
              read_word(r2, end, nx, sizeof(nx));
              ar = strchr(nx, '"');
              if (ar) { int n = 0; ar++;
                while (*ar && *ar != '"' && n < 31) arg[n++] = *ar++;
                arg[n] = 0; }
              else arg[0] = 0;
            }
            else
            {
              int n = 0; ar++;
              while (*ar && *ar != '"' && n < 31) arg[n++] = *ar++;
              arg[n] = 0;
            }
            if (arg[0] && num_decorate_sounds < MAX_DECORATE_SOUNDS)
            {
              int s;
              snd = -1;
              for (s = 0; s < num_decorate_sounds; s++)
                if (!strcasecmp(decorate_sounds[s], arg)) { snd = (short)s; break; }
              if (snd < 0)
              {
                int cn = 0;
                while (arg[cn] && cn < 31)
                {
                  decorate_sounds[num_decorate_sounds][cn] = arg[cn];
                  cn++;
                }
                decorate_sounds[num_decorate_sounds][cn] = 0;
                snd = (short)num_decorate_sounds++;
              }
              act = DA_PLAYSOUND;
            }
          }
          else if (!strcasecmp(fn, "A_Scream"))        act = DA_SCREAM;
          else if (!strcasecmp(fn, "A_FadeOut"))
          {
            /* A_FadeOut(reduce): reduce is a fixed-point fraction of alpha to
             * drop per call (default 0.1).  Convert to a step count = 1/reduce
             * (rounded), stored in the frame's snd slot; the runtime action
             * removes the actor after that many calls. */
            const char *ar = strchr(b, '(');
            double red = 0.1;
            int steps;
            if (ar && ar[1] && ar[1] != ')')
              red = atof(ar + 1);
            if (red <= 0.0) red = 0.1;
            steps = (int)(1.0 / red + 0.5);
            if (steps < 1)  steps = 1;
            if (steps > 255) steps = 255;
            snd = (short)steps;
            act = DA_FADEOUT;
          }
          else if (!strcasecmp(fn, "A_CustomMissile"))
          {
            /* A_CustomMissile("Class", height, spawnofs, angle, ...): the
             * angle offset (4th argument, degrees) fans a multi-call burst,
             * and the projectile class name is captured so its DECORATE
             * Translation/Scale can be applied to the fired imp ball (the
             * mod recolours and shrinks it).  Walk the comma-separated args. */
            const char *ar = strchr(b, '(');
            int deg = 0, commas = 0;
            if (ar)
            {
              const char *q2 = ar + 1;
              /* first argument: the class name (may be quoted) */
              const char *cs = q2;
              const char *ce;
              while (*cs == ' ' || *cs == '\t' || *cs == '"') cs++;
              ce = cs;
              while (*ce && *ce != ',' && *ce != ')' && *ce != '"' &&
                     *ce != ' ' && *ce != '\t') ce++;
              {
                int cl = (int)(ce - cs);
                if (cl > 0 && cl < (int)sizeof(decorate_cmiss_class[0]) &&
                    num_decorate_cmiss < MAX_DECORATE_CMISS)
                {
                  memcpy(decorate_cmiss_class[num_decorate_cmiss], cs, (size_t)cl);
                  decorate_cmiss_class[num_decorate_cmiss][cl] = 0;
                  cmiss_id = (short)num_decorate_cmiss++;
                }
              }
              while (*q2 && *q2 != ')')
              {
                if (*q2 == ',')
                {
                  commas++;
                  if (commas == 3)      /* the angle argument follows */
                  {
                    deg = atoi(q2 + 1);
                    break;
                  }
                }
                q2++;
              }
            }
            if (deg < -360) deg = -360;
            if (deg > 360)  deg = 360;
            snd = (short)deg;
            act = DA_CUSTOMMISSILE;
          }
          else if (!strcasecmp(fn, "A_SpawnItemEx") ||
                   !strcasecmp(fn, "A_SpawnItem"))
          {
            /* first argument is the spawned class name, in quotes; capture it
             * and resolve to a mobjtype at registration.  The remaining
             * offset/velocity/flag arguments are not modelled -- the item is
             * spawned at the actor's position. */
            char arg[32];
            const char *ar2 = strchr(b, '"');
            arg[0] = 0;
            if (!ar2)
            {
              char nx[MAX_NAME];
              const char *r2 = skip_space(r, end);
              read_word(r2, end, nx, sizeof(nx));
              ar2 = strchr(nx, '"');
              if (ar2) { int nn = 0; ar2++;
                while (*ar2 && *ar2 != '"' && nn < 31) arg[nn++] = *ar2++;
                arg[nn] = 0; }
            }
            else
            {
              int nn = 0; ar2++;
              while (*ar2 && *ar2 != '"' && nn < 31) arg[nn++] = *ar2++;
              arg[nn] = 0;
            }
            if (arg[0] && num_decorate_spawns < MAX_DECORATE_SPAWNS)
            {
              int s2;
              snd = -1;
              for (s2 = 0; s2 < num_decorate_spawns; s2++)
                if (!strcasecmp(decorate_spawns[s2], arg))
                { snd = (short)s2; break; }
              if (snd < 0)
              {
                int cn = 0;
                while (arg[cn] && cn < 31)
                { decorate_spawns[num_decorate_spawns][cn] = arg[cn]; cn++; }
                decorate_spawns[num_decorate_spawns][cn] = 0;
                snd = (short)num_decorate_spawns++;
              }
              act = DA_SPAWNITEM;
            }
          }
          else if (!strcasecmp(fn, "A_ActiveSound"))   act = DA_ACTIVESOUND;
          else if (!strcasecmp(fn, "A_NoBlocking") ||
                   !strcasecmp(fn, "A_Fall"))          act = DA_NOBLOCKING;
          else if (!strcasecmp(fn, "A_FaceTarget"))    act = DA_FACETARGET;
          else if (!strcasecmp(fn, "A_Look"))          act = DA_LOOK;
          else if (!strcasecmp(fn, "A_Chase") ||
                   !strcasecmp(fn, "A_Wander"))         act = DA_CHASE;
          else if (!strcasecmp(fn, "A_FastChase"))     act = DA_FASTCHASE;
          else if (!strcasecmp(fn, "A_Pain"))          act = DA_PAIN;
          else if (!strcasecmp(fn, "A_XScream"))       act = DA_XSCREAM;
          else if (!strcasecmp(fn, "A_BossDeath"))     act = DA_BOSSDEATH;
          else if (!strcasecmp(fn, "A_CPosRefire"))    act = DA_CPOSREFIRE;
          else if (!strcasecmp(fn, "A_SkullAttack"))   act = DA_SKULLATTACK;
          else if (!strcasecmp(fn, "A_Explode"))       act = DA_EXPLODE;
          else if (!strcasecmp(fn, "A_MeleeAttack"))   act = DA_MELEEATTACK;
          else if (!strcasecmp(fn, "A_MissileAttack")) act = DA_MISSILEATTACK;
          else if (!strcasecmp(fn, "A_ComboAttack"))   act = DA_COMBOATTACK;
          else if (!strcasecmp(fn, "A_BasicAttack"))
          {
            /* A_BasicAttack(meleedamage, meleesound, missiletype, meleeheight):
             * melee when the target is in range, otherwise fire a missile --
             * exactly the combo-attack behaviour.  Capture the first argument
             * (melee damage) into the actor's melee damage when it has none of
             * its own so the melee branch deals the authored amount; the missile
             * branch fires the generic shot the combo path already uses.
             * Without this the frame was inert and the monster never attacked. */
            const char *ar = strchr(b, '(');
            if (ar && a->meleedamage < 0)
            {
              int d = atoi(ar + 1);
              if (d > 0)
                a->meleedamage = d;
            }
            act = DA_COMBOATTACK;
          }
          /* A_SentinelBob (hover wobble) and A_Set/UnSetFloorClip (corpse
           * sink) are cosmetic with no engine equivalent; leave as no-ops. */
          else if (!strcasecmp(fn, "A_CustomBulletAttack"))
            /* a hitscan burst at the target; the stock SpiderMastermind it
             * stands in for fires the same way (A_SPosAttack: 3 bullets) */
            act = DA_CUSTOMBULLET;
          else if (!strcasecmp(fn, "A_SpidRefire"))    act = DA_SPIDREFIRE;
          else if (!strcasecmp(fn, "ACS_NamedExecuteAlways") ||
                   !strcasecmp(fn, "ACS_NamedExecute"))
          {
            /* ACS_NamedExecuteAlways("Script", ...): capture the quoted
             * script name; the runtime action starts it by name.  The name
             * may be glued to b ("ACS_NamedExecuteAlways(\"x\"") or sit in
             * the following word. */
            char arg[32];
            const char *ar = strchr(b, '"');
            arg[0] = 0;
            if (!ar)
            {
              char nx[MAX_NAME];
              const char *r2 = skip_space(r, end);
              read_word(r2, end, nx, sizeof(nx));
              ar = strchr(nx, '"');
              if (ar) { int n = 0; ar++;
                while (*ar && *ar != '"' && n < 31) arg[n++] = *ar++;
                arg[n] = 0; }
            }
            else
            {
              int n = 0; ar++;
              while (*ar && *ar != '"' && n < 31) arg[n++] = *ar++;
              arg[n] = 0;
            }
            if (arg[0] && num_decorate_acsnames < MAX_DECORATE_ACSNAMES)
            {
              int s;
              snd = -1;
              for (s = 0; s < num_decorate_acsnames; s++)
                if (!strcasecmp(decorate_acsnames[s], arg))
                { snd = (short)s; break; }
              if (snd < 0)
              {
                int cn = 0;
                while (arg[cn] && cn < 31)
                { decorate_acsnames[num_decorate_acsnames][cn] = arg[cn]; cn++; }
                decorate_acsnames[num_decorate_acsnames][cn] = 0;
                snd = (short)num_decorate_acsnames++;
              }
              act = DA_ACS_NAMED;
            }
          }
          else if (!strcasecmp(fn, "A_SetUserVar") ||
                   !strcasecmp(fn, "A_SetUserArray") ||
                   !strcasecmp(fn, "A_JumpIf") ||
                   !strcasecmp(fn, "A_Jump"))
          {
            /* These carry a parenthesised argument list that may contain
             * spaces and an expression, so re-read the whole "(...)" from
             * the action's '(' rather than the space-delimited word. */
            char args[160];
            char fld[4][48];
            const char *lp = strchr(act_src, '(');
            int nf;
            read_paren_args((lp && lp < end) ? lp : act_src, end,
                            args, sizeof(args));
            nf = split_args(args, fld, 4);
            if (!strcasecmp(fn, "A_SetUserVar") && nf >= 2)
            {
              int slot;
              trim_arg(fld[0]); trim_arg(fld[1]);
              slot = decorate_uvar_slot(a, fld[0]);
              if (slot >= 0)
              {
                opstage.uvslot = (short)slot;
                parse_dexpr(a, fld[1], &opstage.val);
                act = DA_SETUSERVAR;
              }
            }
            else if (!strcasecmp(fn, "A_SetUserArray") && nf >= 3)
            {
              int slot;
              trim_arg(fld[0]); trim_arg(fld[1]); trim_arg(fld[2]);
              slot = decorate_uvar_slot(a, fld[0]);
              if (slot >= 0)
              {
                opstage.uvslot = (short)slot;
                parse_dexpr(a, fld[1], &opstage.idx);
                parse_dexpr(a, fld[2], &opstage.val);
                act = DA_SETUSERARRAY;
              }
            }
            else if (!strcasecmp(fn, "A_JumpIf") && nf >= 2)
            {
              /* A_JumpIf(cond, target): target is a label name or a numeric
               * "skip N states forward".  This is flow, not a DA_ action --
               * it rides in the frame's seqflow as a conditional jump. */
              trim_arg(fld[0]); trim_arg(fld[1]);
              parse_dexpr(a, fld[0], &flowstage.jcond);
              flowstage.has_jump = 1;
              if (fld[1][0] >= '0' && fld[1][0] <= '9')
                flowstage.jtoff = (short)atoi(fld[1]);
              else
              {
                int dn = 0;
                flowstage.jtoff = -1;
                while (fld[1][dn] && dn < 23)
                { flowstage.tname[dn] = fld[1][dn]; dn++; }
                flowstage.tname[dn] = 0;
              }
              a->multi_state = 1;
            }
            else if (!strcasecmp(fn, "A_Jump") && nf >= 2)
            {
              /* A_Jump(chance, target[, target2...]): with chance/256
               * probability jump to a target (only the first is modelled),
               * else fall through.  Rides the same seqflow jump slot as
               * A_JumpIf but with no condition -- jchance records the
               * probability and marks it unconditional.  The scene state
               * machines use A_Jump(256, "Finish") as the guaranteed exit out
               * of a sex act, so without this the actor never leaves the act
               * sprite. */
              trim_arg(fld[0]); trim_arg(fld[1]);
              flowstage.has_jump = 1;
              flowstage.jchance  = (short)atoi(fld[0]);
              if (flowstage.jchance < 1)   flowstage.jchance = 1;
              if (flowstage.jchance > 256) flowstage.jchance = 256;
              if (fld[1][0] >= '0' && fld[1][0] <= '9')
                flowstage.jtoff = (short)atoi(fld[1]);
              else
              {
                int dn = 0;
                flowstage.jtoff = -1;
                while (fld[1][dn] && dn < 23)
                { flowstage.tname[dn] = fld[1][dn]; dn++; }
                flowstage.tname[dn] = 0;
              }
              a->multi_state = 1;
            }
          }
        }
        if (a->seq_len == 0 && nspr[0] != '#' && nspr[0] != '-')
        {
          memcpy(a->seq_sprite, nspr, 4);
          a->seq_sprite[4] = 0;
        }
        if (t < 0) t = -1;            /* -1 = freeze on this frame */
        if (t > 32767) t = 32767;
        /* A spawn-item emitter is often written as the same line repeated
         * many times (ten identical "STTR ABCDEF A_SpawnItemEx(...)" lines
         * here = 60 frames) to drizzle items over time.  Capturing every one
         * blows the per-actor frame budget and crowds out the actor's later
         * states (its terminators and inherited use/activation chain).  Keep
         * a bounded run of them -- enough that items still appear and the
         * animation reads -- and drop the rest; the run resets on any other
         * frame so a later emitter elsewhere is unaffected. */
        if (act == DA_SPAWNITEM)
        {
          if (a->spawnitem_run >= DECORATE_SPAWNITEM_CAP)
          {
            p = q;
            while (p < end && *p != '\n')
              p++;
            continue;
          }
          a->spawnitem_run++;
        }
        else
          a->spawnitem_run = 0;
        for (fi = 0; fr[fi] && a->seq_len < MAX_SPAWN_FRAMES; fi++)
        {
          int frbits;
          if ((fr[fi] < 'A' || fr[fi] > '_') &&
              fr[fi] != '#' && fr[fi] != '-')
            continue;
          /* "#"/"-" keep the current frame; store 0 (a logic-only actor
           * with no sprite shows nothing regardless) */
          frbits = (fr[fi] == '#' || fr[fi] == '-') ? 0 : (fr[fi] - 'A');
          a->seq[a->seq_len].frame = (short)(frbits | bright);
          a->seq[a->seq_len].tics  = (short)t;
          /* The "####"/"----" sprite placeholder means "keep displaying the
           * sprite the actor already had" (ZDoom semantics) -- not "show
           * nothing".  Visual-novel and follow-on actors spawn on a real
           * sprite (e.g. Lilitu's "TROO A 1") and then jump into a Spawn/Idle
           * loop made entirely of "####" frames; rendering those as a literal
           * "####" sprite (which has no patches) is what made the actors
           * invisible.  Inherit the most recent real sprite instead so the
           * keep-frames carry it forward; the same goes for the "#"/"-" frame
           * placeholder, which keeps the previous frame index. */
          if (nspr[0] == '#' || nspr[0] == '-')
          {
            if (a->seq_len > 0)
            {
              memcpy(a->seq[a->seq_len].spr, a->seq[a->seq_len - 1].spr, 4);
              if (fr[fi] == '#' || fr[fi] == '-')
                a->seq[a->seq_len].frame =
                  (short)((a->seq[a->seq_len - 1].frame & FF_FRAMEMASK) | bright);
            }
            else
            {
              /* a keep placeholder as the very first frame has nothing to
               * inherit; fall back to the actor's own sprite if one was
               * captured (e.g. from a parent's Spawn), else leave it blank */
              if (a->seq_sprite[0] &&
                  a->seq_sprite[0] != '#' && a->seq_sprite[0] != '-')
                memcpy(a->seq[a->seq_len].spr, a->seq_sprite, 4);
              else
                memcpy(a->seq[a->seq_len].spr, nspr, 4);
            }
          }
          else
          {
            memcpy(a->seq[a->seq_len].spr, nspr, 4);
          }
          a->seq[a->seq_len].spr[4] = 0;
          /* the action fires on the first frame letter of the line, as in
           * DECORATE (a multi-letter line repeats the frames, action once) */
          a->seq[a->seq_len].act = (fi == 0) ? act : (short)DA_NONE;
          a->seq[a->seq_len].snd = (fi == 0) ? snd : (short)-1;
          if (fi == 0 &&
              (act == DA_SETUSERVAR || act == DA_SETUSERARRAY ||
               act == DA_CHANGEFLAG))
          {
            a->seqop[a->seq_len].uvslot = opstage.uvslot;
            a->seqop[a->seq_len].flag   = opstage.flag;
            a->seqop[a->seq_len].fval   = opstage.fval;
            a->seqop[a->seq_len].idx    = opstage.idx;
            a->seqop[a->seq_len].val    = opstage.val;
          }
          else if (fi == 0 && act == DA_CUSTOMMISSILE)
            a->seqop[a->seq_len].uvslot = cmiss_id;   /* projectile class id */
          if (fi == 0 && flowstage.has_jump)
          {
            a->seqflow[a->seq_len].has_jump = 1;
            a->seqflow[a->seq_len].jtoff    = flowstage.jtoff;
            a->seqflow[a->seq_len].jchance  = flowstage.jchance;
            a->seqflow[a->seq_len].jcond    = flowstage.jcond;
            memcpy(a->seqflow[a->seq_len].jtname, flowstage.tname,
                   sizeof(flowstage.tname));
          }
          a->seq_len++;
        }
        p = q;
      }
      }
    }
  }
}

static dbool engine_knows_doomednum(int dn)
{
  int i;
  for (i = 0; i < num_mobj_types; i++)
    if (mobjinfo[i].doomednum == dn)
      return true;
  return false;
}

/* Register static single-frame DECORATE decorations as real thing types
 * via the DSDHacked growable tables.  Must run before the first
 * P_FindDoomedNum call (its hash is built once) and before R_Init (the
 * sprite definitions are built once from sprnames); d_main calls this
 * right before R_Init, and only for the Doom game. */
/* Resolve a DECORATE A_PlaySound("name") logical name to a sfx index for
 * state->misc1.  ZDoom binds the logical name through SNDINFO to a lump;
 * lacking that table here, match an existing sfx by name, else create a
 * grown slot named for the stem so the engine's lazy "ds%s"/bare lump
 * lookup finds the sample.  Returns 0 (no sound) when the name is empty. */
/* ---- SNDINFO logical sound names ---------------------------------------
 *
 * ZDoom mods bind logical sound names to actual lumps in a SNDINFO lump:
 *   logical/name  lumpname           (a direct alias)
 *   $random logical/name { a b c }   (one of a set, chosen at play time)
 * DECORATE then names the logical sound (SeeSound "impse/see"), not the lump.
 * A $random sound resolves to a member chosen at random each time it is
 * looked up, matching ZDoom; a direct alias is a one-member set.  Parsed
 * once, lazily, into a name -> member-list table. */
#define SND_ALIAS_MAX_MEMBERS 12
typedef struct {
  char logical[40];
  char members[SND_ALIAS_MAX_MEMBERS][40];
  int  num_members;
} sndalias_t;
static sndalias_t *snd_aliases;
static int         snd_num_aliases;
static int         snd_aliases_parsed;

/* Maps a logical $random sfx id to the sfx ids of its member samples, so a
 * play of the logical id can be redirected to a random member at run time. */
typedef struct {
  int logical_id;
  int members[SND_ALIAS_MAX_MEMBERS];
  int num_members;
} sfxrandom_t;
#define MAX_SFX_RANDOM 256
static sfxrandom_t sfxrandom[MAX_SFX_RANDOM];
static int         num_sfxrandom;

static void decorate_record_sfxrandom(int logical_id, const int *ids, int n)
{
  sfxrandom_t *r;
  int k;
  if (n <= 0 || num_sfxrandom >= MAX_SFX_RANDOM)
    return;
  r = &sfxrandom[num_sfxrandom++];
  r->logical_id  = logical_id;
  r->num_members = (n > SND_ALIAS_MAX_MEMBERS) ? SND_ALIAS_MAX_MEMBERS : n;
  for (k = 0; k < r->num_members; k++)
    r->members[k] = ids[k];
}

static int sfxrandom_count(int logical_id)
{
  int i;
  for (i = 0; i < num_sfxrandom; i++)
    if (sfxrandom[i].logical_id == logical_id)
      return sfxrandom[i].num_members;
  return 0;
}

static int sfxrandom_member(int logical_id, int which)
{
  int i;
  for (i = 0; i < num_sfxrandom; i++)
    if (sfxrandom[i].logical_id == logical_id)
    {
      int n = sfxrandom[i].num_members;
      if (n <= 0)
        return logical_id;
      return sfxrandom[i].members[(which % n + n) % n];
    }
  return logical_id;
}

static void sndinfo_parse_lump_idx(int lump, int *cap)
{
  int len, i;
  const char *b;
  if (lump < 0)
    return;
  len = W_LumpLength(lump);
  b   = (const char *)W_CacheLumpNum(lump);
  if (!b || len <= 0)
    return;
  i = 0;
  while (i < len)
  {
    char tag[40];
    int  t = 0;
    while (i < len && (b[i]==' '||b[i]=='\t'||b[i]=='\r'||b[i]=='\n')) i++;
    if (i >= len) break;
    if (b[i]=='/' && i+1<len && b[i+1]=='/')   /* // comment */
    { while (i < len && b[i] != '\n') i++; continue; }
    if (b[i]==';')
    { while (i < len && b[i] != '\n') i++; continue; }
    while (i < len && b[i]>' ' && t < 39) tag[t++] = b[i++];
    tag[t] = 0;
    if (tag[0] == '$')
    {
      if (!strcasecmp(tag, "$random"))
      {
        char logical[40];
        int  n = 0;
        while (i < len && (b[i]==' '||b[i]=='\t')) i++;
        while (i < len && b[i]>' ' && n < 39) logical[n++] = b[i++];
        logical[n] = 0;
        /* find '{' (it may sit on the next line), then collect every member
         * token up to '}'.  Skip intervening whitespace and newlines, but a
         * comment line in between must not be mistaken for a brace. */
        while (i < len && b[i] != '{' &&
               (b[i]==' '||b[i]=='\t'||b[i]=='\r'||b[i]=='\n'))
          i++;
        if (i < len && b[i] == '{' && logical[0] && snd_num_aliases < *cap)
        {
          sndalias_t *al = &snd_aliases[snd_num_aliases];
          i++;
          strncpy(al->logical, logical, 39);
          al->logical[39] = 0;
          al->num_members = 0;
          for (;;)
          {
            char mem[40];
            int  m = 0;
            while (i < len && (b[i]==' '||b[i]=='\t'||b[i]=='\r'||b[i]=='\n')) i++;
            if (i >= len || b[i] == '}')
              break;
            while (i < len && b[i]>' ' && b[i] != '}' && m < 39) mem[m++] = b[i++];
            mem[m] = 0;
            if (m > 0 && al->num_members < SND_ALIAS_MAX_MEMBERS)
            {
              memcpy(al->members[al->num_members], mem, (size_t)m + 1);
              al->num_members++;
            }
          }
          while (i < len && b[i] != '}' && b[i] != '\n') i++;
          if (al->num_members > 0)
            snd_num_aliases++;
        }
      }
      else                                 /* other $directive: skip line */
        while (i < len && b[i] != '\n') i++;
    }
    else if (tag[0])
    {
      /* "logical lump" direct alias */
      char lmp[16];
      int n = 0;
      while (i < len && (b[i]==' '||b[i]=='\t')) i++;
      while (i < len && b[i]>' ' && n < 15) lmp[n++] = b[i++];
      lmp[n] = 0;
      if (n > 0 && snd_num_aliases < *cap)
      {
        sndalias_t *al = &snd_aliases[snd_num_aliases];
        strncpy(al->logical, tag, 39);
        al->logical[39] = 0;
        memcpy(al->members[0], lmp, (size_t)n + 1);
        al->num_members = 1;
        snd_num_aliases++;
      }
      while (i < len && b[i] != '\n') i++;
    }
  }
}

static void sndinfo_parse(void)
{
  /* The mod ships hundreds of sound aliases across its SNDINFO lumps (a direct
   * "<logical> <lump>" registry plus the "$random" companion).  The cap must
   * clear that total; 512 truncated the table mid-way, dropping later members
   * (e.g. trosee3..5) so those samples never resolved. */
  int cap = 2048, li;
  snd_aliases_parsed = 1;
  snd_aliases = malloc(sizeof(sndalias_t) * cap);
  if (!snd_aliases)
    return;
  /* The mod splits its sound table across more than one SNDINFO lump (a
   * plain "<lump> <lump>" registry plus a "$random <logical> { ... }"
   * companion), and all collapse to the lump name SNDINFO.  W_CheckNumForName
   * would see only the last; walk every lump named SNDINFO so both the direct
   * aliases and the logical-name sets are captured. */
  for (li = 0; li < numlumps; li++)
    if (!strncasecmp(lumpinfo[li].name, "SNDINFO", 8))
      sndinfo_parse_lump_idx(li, &cap);
  if (snd_num_aliases)
    lprintf(LO_INFO, "U_Decorate: %d SNDINFO sound alias(es)\n",
            snd_num_aliases);
}

/* Translate a DECORATE/logical sound name to the lump it ultimately names,
 * via SNDINFO; returns the input unchanged when there is no alias.  Aliases
 * chain (impse/see -> trosee1 -> TROOSEE1: a $random logical set whose members
 * are themselves "<name> <lump>" aliases), so follow the chain to its end,
 * bounded against a cycle. */
/* Number of $random members bound to a logical name (0 if not a set, or a
 * plain one-member alias counts as 1). */
static int sndinfo_member_count(const char *logical)
{
  int i;
  if (!snd_aliases_parsed)
    sndinfo_parse();
  if (!logical)
    return 0;
  for (i = 0; i < snd_num_aliases; i++)
    if (!strcasecmp(snd_aliases[i].logical, logical))
      return snd_aliases[i].num_members;
  return 0;
}

/* Translate a DECORATE/logical sound name to a lump it names, via SNDINFO;
 * returns the input unchanged when there is no alias.  `pick` selects which
 * member of a $random set to use (the caller passes a random index for
 * per-play variation, or 0 for a deterministic first member).  Aliases chain
 * (impse/see -> trosee1 -> TROOSEE1), so follow the chain to its end, bounded
 * against a cycle. */
static const char *sndinfo_lump_for_pick(const char *logical, int pick)
{
  int hops;
  if (!snd_aliases_parsed)
    sndinfo_parse();
  if (!logical)
    return logical;
  for (hops = 0; hops < 8; hops++)
  {
    int i, found = 0;
    for (i = 0; i < snd_num_aliases; i++)
      if (!strcasecmp(snd_aliases[i].logical, logical))
      {
        int m = snd_aliases[i].num_members;
        const char *next;
        if (m <= 0)
          return logical;
        next = snd_aliases[i].members[(m > 0) ? (pick % m) : 0];
        /* a self-alias (name -> name) is the terminal lump; stop */
        if (!strcasecmp(next, logical))
          return logical;
        logical = next;
        /* only the first hop honours the random pick; chained aliases take
         * their first member so a member name still resolves to its lump */
        pick = 0;
        found = 1;
        break;
      }
    if (!found)
      break;
  }
  return logical;
}

static const char *sndinfo_lump_for(const char *logical)
{
  return sndinfo_lump_for_pick(logical, 0);
}

/* Resolve a DECORATE A_PlaySound("name") logical name to a sfx index for
 * state->misc1.  ZDoom binds the logical name through SNDINFO to a lump;
 * translate the logical name to its lump first, then match an existing sfx by
 * name, else create a grown slot named for the stem so the engine's lazy
 * "ds%s"/bare lump lookup finds the sample.  Returns 0 (no sound) when the
 * name is empty. */
static int decorate_resolve_sfx(const char *name)
{
  int i;
  const char *stem;
  sfxinfo_t *sfx;
  char *copy;
  int   is_random, logical_id;
  /* Next free sfx slot.  dsda_GetSfx grows the table by doubling to fit an
   * index and leaves num_sfx at the inflated capacity, so re-reading num_sfx
   * as the next slot would leap past the just-created entry on every new
   * sound.  Seed the cursor from num_sfx the first time and advance it
   * ourselves; -1 means "not yet seeded". */
  static int sfx_cursor = -1;

  if (!name || !name[0])
    return 0;

  if (sfx_cursor < 0)
    sfx_cursor = num_sfx;

  /* A $random SNDINFO set (impse/see -> { trosee1 .. trosee5 }) must vary per
   * play.  Register a logical sfx that carries no sample of its own and, on
   * the side, the sfx id of each member sample; the sound system redirects a
   * play of the logical id to a random member id.  A single-member alias (or
   * a plain lump name) bakes straight to its lump as before. */
  is_random = sndinfo_member_count(name) > 1;

  if (!is_random)
  {
    name = sndinfo_lump_for(name);
    /* the bound lump may already carry a "ds" prefix; index by the stem */
    stem = ((name[0] == 'd' || name[0] == 'D') &&
            (name[1] == 's' || name[1] == 'S')) ? name + 2 : name;

    for (i = 1; i < sfx_cursor; i++)
      if (S_sfx[i].name && !strcasecmp(S_sfx[i].name, stem))
        return i;

    i   = sfx_cursor++;            /* grow one slot */
    sfx = dsda_GetSfx(i);          /* may move S_sfx; use the returned ptr */
    copy = malloc(strlen(stem) + 1);
    if (!copy)
      return 0;
    strcpy(copy, stem);
    sfx->name        = copy;
    sfx->singularity = false;
    sfx->priority    = 98;
    sfx->pitch       = -1;
    sfx->volume      = -1;
    return i;
  }

  /* random set: reuse an existing logical entry if already built */
  for (i = 1; i < sfx_cursor; i++)
    if (S_sfx[i].name && !strcasecmp(S_sfx[i].name, name))
      return i;

  /* logical entry first (no sample) */
  logical_id = sfx_cursor++;
  sfx = dsda_GetSfx(logical_id);
  copy = malloc(strlen(name) + 1);
  if (!copy)
    return 0;
  strcpy(copy, name);
  sfx->name        = copy;
  sfx->singularity = false;
  sfx->priority    = 98;
  sfx->pitch       = -1;
  sfx->volume      = -1;

  /* register each member as its own sfx and record the mapping */
  {
    int mc = sndinfo_member_count(name), k;
    int ids[SND_ALIAS_MAX_MEMBERS], nids = 0;
    for (k = 0; k < mc && k < SND_ALIAS_MAX_MEMBERS; k++)
    {
      const char *ml = sndinfo_lump_for_pick(name, k);
      const char *ms;
      int j, mid = -1;
      if (!ml)
        continue;
      ms = ((ml[0] == 'd' || ml[0] == 'D') &&
            (ml[1] == 's' || ml[1] == 'S')) ? ml + 2 : ml;
      for (j = 1; j < sfx_cursor; j++)
        if (S_sfx[j].name && !strcasecmp(S_sfx[j].name, ms))
        { mid = j; break; }
      if (mid < 0)
      {
        char *mc2 = malloc(strlen(ms) + 1);
        if (!mc2)
          continue;
        mid = sfx_cursor++;
        sfx = dsda_GetSfx(mid);
        strcpy(mc2, ms);
        sfx->name        = mc2;
        sfx->singularity = false;
        sfx->priority    = 98;
        sfx->pitch       = -1;
        sfx->volume      = -1;
      }
      ids[nids++] = mid;
    }
    decorate_record_sfxrandom(logical_id, ids, nids);
  }
  return logical_id;
}

/* Map a logical $random sfx id to one of its member ids at play time, or the
 * id itself when it is not a random set.  Returns a member chosen at random so
 * the sound varies between plays. */
int U_SoundRandomId(int id)
{
  int n = sfxrandom_count(id);
  if (n <= 0)
    return id;
  return sfxrandom_member(id, P_Random(pr_misc) % n);
}

static int decorate_sprite_index(const char *name, int *sp_next);

/* Resolve a DECORATE class name to a registered mobjtype via its actorname,
 * or -1.  Used to wire A_SpawnItemEx targets to the type they spawn. */
static int decorate_type_by_name(const char *name)
{
  int i;
  if (!name || !name[0])
    return -1;
  for (i = 0; i < num_mobj_types; i++)
    if (mobjinfo[i].actorname && !strcasecmp(mobjinfo[i].actorname, name))
      return i;
  return -1;
}

/* Build an actor's frame sequence into states [base, base+nframes), honouring
 * the recorded per-frame flow (loop / stop / wait / goto / A_Jump) and wiring
 * the safe per-frame actions (A_PlaySound, A_Scream, A_NoBlocking,
 * A_FaceTarget, ACS-named, user-var writes, conditional jumps).  Shared by the
 * doomednum decoration registrar and the ACS-spawnable actor registrar so
 * both build identical chains.  A static (single frozen frame) actor is one
 * entry that freezes on itself. */
static void decorate_build_states(const decorate_actor_t *a, int sp, int base,
                                  int nframes, int *sp_next,
                                  const mobjinfo_t *inh)
{
  int first = base;
  int f;
  for (f = 0; f < nframes; f++)
  {
    int cur  = base + f;
    int last = (f == nframes - 1);
    int fsp  = sp;
    state_t *state;
    /* a frame may carry its own sprite (a sequence that switches sprite mid
     * way, e.g. TROO -> STTR); resolve it, else fall back to the default */
    if (!a->spawn_static && a->seq[f].spr[0] && sp_next)
      fsp = decorate_sprite_index(a->seq[f].spr, sp_next);
    state = dsda_GetState(cur);
    state->sprite = fsp;
    if (a->spawn_static)
    {
      state->frame = a->frame;
      state->tics  = -1;
      state->nextstate = cur;
      continue;
    }
    state->frame = a->seq[f].frame;
    state->tics  = a->seq[f].tics;
    if (a->multi_state)
    {
      int flow = a->seqflow[f].flow;
      if (flow == SEQF_LOOP)
      {
        int li, lab_frame = 0;
        for (li = 0; li < a->num_seqlabels; li++)
          if (a->seqlabel[li].frame <= f &&
              a->seqlabel[li].frame >= lab_frame)
            lab_frame = a->seqlabel[li].frame;
        state->nextstate = base + lab_frame;
      }
      else if (flow == SEQF_STOP)
        state->nextstate = cur;
      else if (flow == SEQF_WAIT)
        state->nextstate = cur;
      else if (flow == SEQF_GOTO)
      {
        /* "goto Label" or "goto Label+N": split off a trailing "+N" relative
         * offset, then resolve the label.  A label the actor defines itself
         * resolves within its built block (e.g. the spider's "Missile+1" loops
         * back to the second Missile frame for rapid fire).  A label it does
         * not define is inherited from the stock monster it replaces (e.g. the
         * cyberdemon's "goto See" returns to the inherited chase states), so
         * fall back to that monster's corresponding state rather than leaving
         * the terminal frame self-looping, which would freeze the monster in
         * its attack and stop it moving. */
        const char *tn = a->seqflow[f].tname;
        char lbl[24];
        int off = 0, li, dst = cur, k = 0;
        const char *plus = NULL;
        while (tn[k] && tn[k] != '+' && k < 23) { lbl[k] = tn[k]; k++; }
        lbl[k] = 0;
        if (tn[k] == '+') plus = tn + k + 1;
        if (plus) off = atoi(plus);
        li = decorate_label_index(a, lbl);
        if (li >= 0)
          dst = base + a->seqlabel[li].frame + off;
        else if (inh)
        {
          int s = -1;
          if      (!strcasecmp(lbl, "See"))    s = inh->seestate;
          else if (!strcasecmp(lbl, "Missile"))s = inh->missilestate;
          else if (!strcasecmp(lbl, "Melee"))  s = inh->meleestate;
          else if (!strcasecmp(lbl, "Pain"))   s = inh->painstate;
          else if (!strcasecmp(lbl, "Death"))  s = inh->deathstate;
          else if (!strcasecmp(lbl, "XDeath")) s = inh->xdeathstate;
          else if (!strcasecmp(lbl, "Spawn"))  s = inh->spawnstate;
          else if (!strcasecmp(lbl, "Raise"))  s = inh->raisestate;
          if (s > 0)
            dst = s + off;
        }
        state->nextstate = dst;
      }
      else
        state->nextstate = last ? cur : cur + 1;
    }
    else
      state->nextstate = last
        ? (a->seq_loops ? first : cur)
        : cur + 1;

    switch (a->seq[f].act)
    {
      case DA_PLAYSOUND:
        state->action.arg0 = (arg0_t)A_PlaySound;
        state->misc1 = (a->seq[f].snd >= 0)
          ? decorate_resolve_sfx(decorate_sounds[a->seq[f].snd]) : 0;
        state->misc2 = 0;
        break;
      case DA_ACTIVESOUND:
        state->action.arg0 = (arg0_t)A_PlaySound;
        state->misc1 = (a->seq[f].snd >= 0)
          ? decorate_resolve_sfx(decorate_sounds[a->seq[f].snd]) : 0;
        state->misc2 = 0;
        break;
      case DA_SCREAM:
        state->action.arg0 = (arg0_t)A_Scream;
        break;
      case DA_SPAWNITEM:
        state->action.arg0 = (arg0_t)A_DecorateSpawnItem;
        state->misc1 = (a->seq[f].snd >= 0)
          ? decorate_type_by_name(decorate_spawns[a->seq[f].snd]) : 0;
        if (state->misc1 < 0) state->misc1 = 0;
        break;
      case DA_FADEOUT:
        state->action.arg0 = (arg0_t)A_DecorateFadeOut;
        state->misc1 = (a->seq[f].snd > 0) ? a->seq[f].snd : 8;
        break;
      case DA_CUSTOMMISSILE:
        state->action.arg0 = (arg0_t)A_DecorateCustomMissile;
        state->misc1 = a->seq[f].snd;   /* angle offset in degrees (may be <0) */
        /* misc2 carries (xlat_id+1) of the named projectile class so the
         * runtime can recolour the fired ball; 0 means "no translation". */
        state->misc2 = 0;
        {
          int cid = a->seqop[f].uvslot;   /* captured A_CustomMissile class id */
          if (cid >= 0 && cid < num_decorate_cmiss)
          {
            decorate_actor_t *cm = find_actor_mut(decorate_cmiss_class[cid]);
            if (cm)
            {
              if (cm->xlat_id < 0 && cm->xlat_raw[0])
                cm->xlat_id = decorate_translation_id(cm->xlat_raw);
              if (cm->xlat_id >= 0)
                state->misc2 = cm->xlat_id + 1;
            }
          }
        }
        break;
      case DA_NOBLOCKING:
        state->action.arg0 = (arg0_t)A_Fall;
        break;
      case DA_FACETARGET:
        state->action.arg0 = (arg0_t)A_FaceTarget;
        break;
      case DA_CUSTOMBULLET:
        /* hitscan burst at the target.  The stock SpiderMastermind this stands
         * in for fires its missile state through A_SPosAttack (3 bullets,
         * (rand%5+1)*3 damage), matching the mod's A_CustomBulletAttack burst;
         * reuse it so the variant actually shoots rather than looping inert. */
        state->action.arg0 = (arg0_t)A_SPosAttack;
        break;
      case DA_SPIDREFIRE:
        /* keep firing while the target is alive and in sight, else drop back to
         * the (inherited) See state -- without this the attack loop never
         * breaks and the monster is stuck firing in place. */
        state->action.arg0 = (arg0_t)A_SpidRefire;
        break;
      case DA_LOOK:
        state->action.arg0 = (arg0_t)A_Look;
        break;
      case DA_CHASE:
        state->action.arg0 = (arg0_t)A_Chase;
        break;
      case DA_FASTCHASE:
        state->action.arg0 = (arg0_t)A_FastChase;
        break;
      case DA_PAIN:
        state->action.arg0 = (arg0_t)A_Pain;
        break;
      case DA_XSCREAM:
        state->action.arg0 = (arg0_t)A_XScream;
        break;
      case DA_BOSSDEATH:
        state->action.arg0 = (arg0_t)A_BossDeath;
        break;
      case DA_CPOSREFIRE:
        state->action.arg0 = (arg0_t)A_CPosRefire;
        break;
      case DA_SKULLATTACK:
        state->action.arg0 = (arg0_t)A_SkullAttack;
        break;
      case DA_EXPLODE:
        state->action.arg0 = (arg0_t)A_Explode;
        break;
      case DA_MELEEATTACK:
        state->action.arg0 = (arg0_t)A_DecorateMeleeAttack;
        state->misc1 = (a->meleedamage >= 0) ? a->meleedamage : 0;
        break;
      case DA_MISSILEATTACK:
        state->action.arg0 = (arg0_t)A_DecorateMissileAttack;
        break;
      case DA_COMBOATTACK:
        state->action.arg0 = (arg0_t)A_DecorateComboAttack;
        state->misc1 = (a->meleedamage >= 0) ? a->meleedamage : 0;
        break;
      case DA_ACS_NAMED:
        if (a->seq[f].snd >= 0)
        {
          state->action.arg0 = (arg0_t)A_DecorateACSNamed;
          state->misc1 = decorate_intern_acsname(
                           decorate_acsnames[a->seq[f].snd]);
        }
        break;
      case DA_SETUSERVAR:
      case DA_SETUSERARRAY:
        state->action.arg0 = (arg0_t)
          ((a->seq[f].act == DA_SETUSERVAR)
             ? (void *)A_DecorateSetUserVar
             : (void *)A_DecorateSetUserArray);
        state->misc1   = a->seqop[f].uvslot;
        state->args[0] = a->seqop[f].val.ka;
        state->args[1] = a->seqop[f].val.va;
        state->args[2] = a->seqop[f].val.op;
        state->args[3] = a->seqop[f].val.kb;
        state->args[4] = a->seqop[f].val.vb;
        if (a->seq[f].act == DA_SETUSERARRAY)
        {
          state->args[5] = a->seqop[f].idx.ka;
          state->args[6] = a->seqop[f].idx.va;
          state->args[7] = a->seqop[f].idx.op;
          state->misc2   = (a->seqop[f].idx.kb |
                            (a->seqop[f].idx.vb << 8));
        }
        break;
      default:
        break;
    }

    if (a->seqflow[f].has_jump)
    {
      int tgt = cur;
      if (a->seqflow[f].jtoff >= 0)
        tgt = cur + a->seqflow[f].jtoff;
      else
      {
        int li = decorate_label_index(a, a->seqflow[f].jtname);
        if (li >= 0)
          tgt = base + a->seqlabel[li].frame;
      }
      if (a->seqflow[f].jchance > 0)
      {
        /* A_Jump(chance, target): unconditional probabilistic jump.  The
         * chance (1..256) rides in args[0]; A_DecorateJump rolls against it. */
        state->action.arg0 = (arg0_t)A_DecorateJump;
        state->args[0] = a->seqflow[f].jchance;
        state->args[5] = tgt;
      }
      else
      {
        state->action.arg0 = (arg0_t)A_DecorateJumpIf;
        state->args[0] = a->seqflow[f].jcond.ka;
        state->args[1] = a->seqflow[f].jcond.va;
        state->args[2] = a->seqflow[f].jcond.op;
        state->args[3] = a->seqflow[f].jcond.kb;
        state->args[4] = a->seqflow[f].jcond.vb;
        state->args[5] = tgt;
      }
    }
  }
}

void U_RegisterDecorateThings(void)
{
  int i, count = 0, count_mt = 0;
  /* The dsda tables double when their end is touched: allocate
   * sequentially from the counts captured here so ten registrations cost
   * one doubling, not ten. */
  int st_base = num_states;
  int mt_base = num_mobj_types;
  int sp_next = num_sprites;

  if (!parsed)
    parse_decorate();

  for (i = 0; i < num_actors; i++)
  {
    decorate_actor_t *a = &actors[i];
    int st, mt, sp, k;
    mobjinfo_t *info;
    int nframes;
    const char *spr;

    if (a->doomednum < 0)
      continue;
    if (!a->spawn_static && a->seq_len <= 0)
      continue;                 /* nothing renderable captured            */
    if (engine_knows_doomednum(a->doomednum))
      continue;
    if (resolve_class(a->name, a, 0) >= 0)
      continue;                 /* monsters etc. handled by aliasing      */

    /* a frozen single frame is a one-entry sequence */
    nframes = a->spawn_static ? 1 : a->seq_len;
    spr     = a->spawn_static ? a->sprite : a->seq_sprite;

    sp = -1;
    for (k = 0; k < sp_next; k++)
      if (sprnames[k] && !strncasecmp(sprnames[k], spr, 4))
      {
        sp = k;
        break;
      }
    if (sp < 0)
    {
      sp = sp_next++;
      *dsda_GetSprite(sp) = strdup(spr);
    }

    /* one state per frame; nextstate chains forward, the last frame loops
     * to the first (animated loop) or freezes on itself (static / stop) */
    {
      int first = st_base + count;
      decorate_build_states(a, sp, first, nframes, &sp_next, NULL);
      st = first;
    }

    mt = mt_base + count_mt;
    info = dsda_GetMobjInfo(mt);
    info->doomednum   = a->doomednum;

    /* Carry the class name so ACS Spawn/SpawnSpot, which look an actor up by
     * name (zacs_actor_type), can find it.  Without this a decoration that the
     * scripts spawn by name -- e.g. an objective marker or a portal effect --
     * fails to resolve and silently never appears. */
    if (a->name[0] && !info->actorname)
    {
      char *nm = malloc(strlen(a->name) + 1);
      if (nm) { strcpy(nm, a->name); info->actorname = nm; }
    }

    /* publish this actor's user-variable map under its mobjtype so the ACS
     * user-variable builtins can resolve names to slots at runtime */
    if (a->num_uvars > 0 && num_uvarmaps < MAX_UVARMAPS)
    {
      uvarmap_t *m = &uvarmaps[num_uvarmaps++];
      int u;
      m->type  = mt;
      m->slots = a->uvar_slots;
      m->num   = a->num_uvars;
      for (u = 0; u < a->num_uvars; u++)
      {
        memcpy(m->var[u].name, a->uvar[u].name, sizeof(m->var[u].name));
        m->var[u].base = a->uvar[u].base;
        m->var[u].len  = a->uvar[u].len;
      }
    }

    /* +USESPECIAL: record the state its "Active" label resolves to, so the
     * use-trace can switch a used thing of this type into it. */
    if (a->use_special && !a->spawn_static && num_useacts < MAX_USEACTS)
    {
      int li = decorate_label_index(a, "Active");
      if (li >= 0)
      {
        useacts[num_useacts].type        = mt;
        useacts[num_useacts].activestate = st_base + count + a->seqlabel[li].frame;
        num_useacts++;
      }
    }
    info->spawnstate  = st;
    info->spawnhealth = 1000;
    info->mass        = 100;
    info->damage      = a->damage;
    info->radius      = (a->radius >= 0 ? a->radius : 20) * FRACUNIT;
    info->height      = (a->height >= 0 ? a->height : 16) * FRACUNIT;
    info->flags       = (a->solid ? MF_SOLID : 0) |
                        (a->nogravity ? MF_NOGRAVITY : 0) |
                        (a->spawnceiling ? (MF_SPAWNCEILING | MF_NOGRAVITY)
                                         : 0) |
                        ((a->translucent || a->alpha < FRACUNIT)
                                         ? MF_TRANSLUCENT : 0);

    /* A DECORATE inventory/pickup actor (a logbook, keycard, ammo token, ...)
     * is collected by touching it, so it must be MF_SPECIAL or the player
     * walks straight through and P_TouchSpecialThing never runs -- which also
     * means any thing-special the map gave the pickup (commonly an ACS_Execute
     * that opens a door or raises a lift) never fires.  Flag it here; the
     * touch handler grants it, fires its special, and removes it.
     *
     * A +USESPECIAL actor is the opposite kind of object: it is activated by
     * the player pressing use, not by being walked into, and it is meant to
     * persist and keep responding (an interactive NPC or a conversation
     * starter, often itself derived from CustomInventory).  Marking such an
     * actor MF_SPECIAL would let the touch handler collect and remove it before
     * it could ever be used, so its scene/conversation could not be triggered.
     * Exclude use-activated actors from touch-pickup flagging. */
    if (decorate_is_touch_pickup(a))
    {
      info->flags |= MF_SPECIAL;
      if (a->m_countitem)
        info->flags |= MF_COUNTITEM;
    }

    /* A custom DECORATE monster comes alive with AI: wire each behaviour label
     * to its mobjinfo state so the chase/attack/pain/death machinery drives it,
     * make it shootable and (unless told otherwise) count it as a kill, and
     * fill in health/speed/painchance/mass.  Crucially it is left NON-solid:
     * a solid monster placed overlapping another actor in the map wedges both
     * permanently (P_Move fails in every direction).  A non-solid monster moves
     * through other things instead of jamming against them, while still chasing,
     * attacking, being shot, and dying. */
    if (a->is_monster)
    {
      int base = st_base + count;
      int li;
      if ((li = decorate_label_index(a, "See"))     >= 0) info->seestate    = base + a->seqlabel[li].frame;
      if ((li = decorate_label_index(a, "Pain"))    >= 0) info->painstate    = base + a->seqlabel[li].frame;
      if ((li = decorate_label_index(a, "Melee"))   >= 0) info->meleestate   = base + a->seqlabel[li].frame;
      if ((li = decorate_label_index(a, "Missile")) >= 0) info->missilestate = base + a->seqlabel[li].frame;
      if ((li = decorate_label_index(a, "Death"))   >= 0) info->deathstate   = base + a->seqlabel[li].frame;
      if ((li = decorate_label_index(a, "XDeath"))  >= 0) info->xdeathstate  = base + a->seqlabel[li].frame;
      if ((li = decorate_label_index(a, "Raise"))   >= 0) info->raisestate   = base + a->seqlabel[li].frame;

      info->spawnhealth  = (a->health >= 0)     ? a->health     : 1000;
      info->speed        = (a->speed  >= 0)     ? a->speed      : 8;
      info->painchance   = (a->painchance >= 0) ? a->painchance : 0;
      info->mass         = (a->mass   >= 0)     ? a->mass       : 100;
      info->reactiontime = 8;
      info->radius       = (a->radius >= 0 ? a->radius : 20) * FRACUNIT;
      info->height       = (a->height >= 0 ? a->height : 56) * FRACUNIT;

      info->flags |= MF_SHOOTABLE;          /* can be hurt; deliberately NOT MF_SOLID */
      if (!a->m_nocountkill)
        info->flags |= MF_COUNTKILL;
      if (a->m_float)
        info->flags |= MF_FLOAT | MF_NOGRAVITY;

      if (a->seesound[0])    info->seesound    = decorate_resolve_sfx(a->seesound);
      if (a->painsound[0])   info->painsound   = decorate_resolve_sfx(a->painsound);
      if (a->deathsound[0])  info->deathsound  = decorate_resolve_sfx(a->deathsound);
      if (a->attacksound[0]) info->attacksound = decorate_resolve_sfx(a->attacksound);
      if (a->activesound[0]) info->activesound = decorate_resolve_sfx(a->activesound);
    }
    count    += nframes;        /* states consumed */
    count_mt += 1;              /* one mobj type   */
  }

  if (count_mt)
    lprintf(LO_INFO, "U_RegisterDecorateThings: %d decorations (%d states)\n",
            count_mt, count);
}

/* ---- monster replacement (DECORATE "replaces") --------------------------
 *
 * hdoom-style mods stand a custom monster in for a stock one with
 * "actor ImpEncounter1 : HDoomMonster replaces DoomImp".  The custom actor
 * carries no editor number of its own; it takes over the editor number of the
 * class it replaces, so every map placement of that stock thing should spawn
 * the replacement instead.  Most replacements reuse the stock monster's sprite
 * name (the mod overrides the sprite graphics), so cloning the stock monster's
 * mobjinfo already yields the right look and the stock AI; the few that use a
 * fresh sprite (the zombie family's PTRO/STRO/CTRO) get the cloned state chain
 * re-skinned to the new sprite.  A redirect table maps the replaced editor
 * number to the clone so P_SpawnMapThing diverts the spawn. */

typedef struct { int doomednum; int mobjtype; } decorate_repl_t;
static decorate_repl_t decorate_repls[MAX_DECORATE_ACTORS];
static int             num_decorate_repls;

/* The mobjtype whose editor number is dn, or -1. */
static int mobjtype_for_doomednum(int dn)
{
  int i;
  if (dn < 0)
    return -1;
  for (i = 0; i < num_mobj_types; i++)
    if (mobjinfo[i].doomednum == dn)
      return i;
  return -1;
}

/* Clone the reachable state graph of a stock monster into fresh state slots,
 * substituting newspr for the sprite on every cloned frame, and rewrite the
 * mobjinfo's state entry points to the clones.  Only frames the monster
 * actually reaches through its labelled entry points are copied; the walk is
 * bounded by the stock state table so a malformed nextstate cannot run away.
 * `map` (length oldcount, all -1) memoises old-state -> new-state. */
static int reskin_clone_state(int old, int newspr, int *map, int oldcount,
                              int *next_slot)
{
  int neu;
  state_t *src, *dst;
  if (old <= 0 || old >= oldcount)        /* S_NULL or out of range: keep */
    return old;
  if (map[old] >= 0)
    return map[old];

  neu = (*next_slot)++;
  map[old] = neu;
  /* GetState may move the table; fetch dst by index after each recursion */
  dst = dsda_GetState(neu);
  src = &states[old];
  *dst = *src;
  dst->sprite = newspr;
  /* recurse into the successor, then re-fetch dst (table may have grown) */
  {
    int nn = reskin_clone_state(states[old].nextstate, newspr, map, oldcount,
                                next_slot);
    dsda_GetState(neu)->nextstate = nn;
  }
  return neu;
}

static void register_one_monster_repl(decorate_actor_t *a, int *sp_next,
                                      int *st_cursor, int orig_states,
                                      int *mt_cursor)
{
  int basedn = resolve_class(a->replaces, NULL, 0);
  int basemt = mobjtype_for_doomednum(basedn);
  int mt;
  mobjinfo_t *info;
  char basespr[5];

  if (basemt < 0)
    return;                      /* unknown stock class: leave it alone */

  /* clone the stock monster wholesale: flags, sounds, speed, health, the
   * whole state set.  This inherits working A_Look / A_Chase / attack AI.
   * mt advances through an explicit cursor: dsda_GetMobjInfo grows the table
   * by doubling to fit the index, so reading num_mobj_types back as the next
   * slot would leap to the inflated capacity each time. */
  mt   = (*mt_cursor)++;
  info = dsda_GetMobjInfo(mt);   /* grows the table to fit; cache after */
  *info = mobjinfo[basemt];
  info->doomednum = -1;          /* reached only through the redirect */
  /* A reskinned monster is placed by the mod the way it sits in the source
   * port it was authored for, where two monsters that start overlapping push
   * apart.  This engine has no such push: two solid monsters that overlap (or
   * a solid monster overlapping a solid prop) deadlock -- P_Move fails in every
   * direction, P_NewChaseDir sets DI_NODIR, and the trapped monster loops its
   * walk animation in place without ever moving.  Clear MF_SOLID so the reskin
   * cannot wedge: it still chases, attacks, is shot and dies, but passes
   * through other actors instead of jamming against them. */
  info->flags &= ~MF_SOLID;
  /* carry the replacement's own class name so ACS GetActorClass reports the
   * custom name (the death system builds "<Name>_Sex" from it), not the stock
   * class the clone was copied from.  Names are interned in the actor table,
   * which outlives registration, so a borrowed pointer is safe. */
  {
    char *nm = malloc(strlen(a->name) + 1);
    if (nm) { strcpy(nm, a->name); info->actorname = nm; }
  }

  /* property overrides the header captured (others stay at stock values) */
  if (a->radius >= 0) info->radius = a->radius * FRACUNIT;
  if (a->height >= 0) info->height = a->height * FRACUNIT;

  /* sound overrides: the clone inherited the stock monster's sounds, but the
   * replacement defines its own (SeeSound "impse/see" etc.).  Resolve each
   * through SNDINFO to a sfx slot and override; unset sounds keep the stock
   * value so a partly-specified actor still sounds reasonable. */
  if (a->seesound[0])    info->seesound    = decorate_resolve_sfx(a->seesound);
  if (a->painsound[0])   info->painsound   = decorate_resolve_sfx(a->painsound);
  if (a->deathsound[0])  info->deathsound  = decorate_resolve_sfx(a->deathsound);
  if (a->attacksound[0]) info->attacksound = decorate_resolve_sfx(a->attacksound);
  if (a->activesound[0]) info->activesound = decorate_resolve_sfx(a->activesound);

  /* if the replacement names a different spawn sprite than the stock
   * monster, re-skin its cloned states to that sprite */
  if (a->seq_sprite[0])
  {
    const char *bs = ((int)states[info->spawnstate].sprite < num_sprites)
                     ? sprnames[states[info->spawnstate].sprite] : NULL;
    memset(basespr, 0, sizeof(basespr));
    if (bs) memcpy(basespr, bs, 4);
    if (strncasecmp(basespr, a->seq_sprite, 4) != 0)
    {
      int newspr = decorate_sprite_index(a->seq_sprite, sp_next);
      /* Only stock-table states are cloned; map is sized to the original
       * engine state count and the bound check rejects anything at or beyond
       * it (already-cloned or out-of-range successors are left as-is). */
      int *map = malloc(sizeof(int) * orig_states);
      int k;
      if (map)
      {
        int sp0 = info->spawnstate,   se0 = info->seestate;
        int pa0 = info->painstate,    me0 = info->meleestate;
        int mi0 = info->missilestate, de0 = info->deathstate;
        int xd0 = info->xdeathstate,  ra0 = info->raisestate;
        for (k = 0; k < orig_states; k++) map[k] = -1;
        info->spawnstate   = reskin_clone_state(sp0, newspr, map, orig_states, st_cursor);
        info->seestate     = reskin_clone_state(se0, newspr, map, orig_states, st_cursor);
        info->painstate    = reskin_clone_state(pa0, newspr, map, orig_states, st_cursor);
        info->meleestate   = reskin_clone_state(me0, newspr, map, orig_states, st_cursor);
        info->missilestate = reskin_clone_state(mi0, newspr, map, orig_states, st_cursor);
        info->deathstate   = reskin_clone_state(de0, newspr, map, orig_states, st_cursor);
        info->xdeathstate  = reskin_clone_state(xd0, newspr, map, orig_states, st_cursor);
        info->raisestate   = reskin_clone_state(ra0, newspr, map, orig_states, st_cursor);
        free(map);
      }
    }
  }

  /* Prepend a death action that runs the hdoom ACS death decision before the
   * death animation plays.  A fresh state with a zero-tic TNT1 frame runs
   * A_HDoomDeath, then falls through (nextstate) to the death animation.
   *
   * The mod defines its own death animation under a "Faint" label in its
   * DECORATE (different frames and timing from the stock monster it reskins),
   * so build that captured sequence and fall through into it instead of the
   * reskinned stock death.  Fall back to the reskinned stock death only when
   * the actor declares no Faint sequence.  Point both the normal and gib
   * death entries at the action so either kind of kill triggers it. */
  {
    int faint_li  = decorate_label_index(a, "Faint");
    int miss_li   = decorate_label_index(a, "Missile");
    int death_li  = decorate_label_index(a, "Death");
    int xdeath_li = decorate_label_index(a, "XDeath");
    int faint_state  = -1;
    int miss_state   = -1;
    int death_state  = -1;
    int xdeath_state = -1;
    if ((faint_li >= 0 || miss_li >= 0 || death_li >= 0 || xdeath_li >= 0) &&
        a->seq_len > 0)
    {
      int fsp  = decorate_sprite_index(a->seq_sprite[0] ? a->seq_sprite
                                                        : a->seq[0].spr, sp_next);
      int fbase = *st_cursor;
      dsda_GetState(fbase + a->seq_len - 1);  /* grow before building */
      info = dsda_GetMobjInfo(mt);            /* re-cache after possible move */
      decorate_build_states(a, fsp, fbase, a->seq_len, sp_next, info);
      *st_cursor += a->seq_len;
      decorate_record_statemap(mt, fbase, a);
      if (faint_li >= 0)
        faint_state = fbase + a->seqlabel[faint_li].frame;
      if (miss_li >= 0)
        miss_state = fbase + a->seqlabel[miss_li].frame;
      if (death_li >= 0)
        death_state = fbase + a->seqlabel[death_li].frame;
      if (xdeath_li >= 0)
        xdeath_state = fbase + a->seqlabel[xdeath_li].frame;
    }

    /* A replacement reskins the stock monster it stands in for, inheriting
     * that monster's attack -- so a variant standing in for a hitscan zombie
     * would fire hitscan rather than the projectile its own DECORATE Missile
     * state casts via A_CustomMissile.  When the actor defines its own Missile
     * sequence, point the missile state at that built sequence so the variant
     * uses its intended projectile attack; the cloned See/Chase AI still
     * drives into it through info->missilestate. */
    if (miss_state > 0)
      info->missilestate = miss_state;

    /* A variant that restates its own Death/XDeath (a custom gib animation
     * with its own frames, A_Jump between two death sequences, etc.) was left
     * playing the reskinned stock death because its captured death block was
     * never built or wired -- so the intended animation never ran.  Point the
     * death states at the built sequence so the variant plays its own death.
     * This is independent of the hdoom Faint/A_HDoomDeath ACS hook below. */
    if (death_state > 0)
      info->deathstate = death_state;
    if (xdeath_state > 0)
      info->xdeathstate = xdeath_state;

    /* hdoom drives its death through an ACS hook under a "Faint" label: prepend
     * a zero-tic A_HDoomDeath that falls through to the death animation.  Only
     * do this when the actor actually uses that hook (a Faint label); a variant
     * whose death is a plain DECORATE animation must not be rerouted. */
    if (faint_state > 0)
    {
      int ds   = (*st_cursor)++;
      state_t *st;
      dsda_GetState(ds);            /* grow the table to fit before caching */
      info = dsda_GetMobjInfo(mt);  /* re-cache: GetState may have moved tables */
      st = dsda_GetState(ds);
      st->sprite      = SPR_TNT1;
      st->frame       = 0;
      st->tics        = 0;
      st->action.arg1 = (arg1_t)A_HDoomDeath;
      st->nextstate   = faint_state;
      st->misc1 = st->misc2 = 0;
      info->deathstate  = ds;
      info->xdeathstate = ds;
    }
  }

  if (num_decorate_repls < MAX_DECORATE_ACTORS)
  {
    decorate_repls[num_decorate_repls].doomednum = basedn;
    decorate_repls[num_decorate_repls].mobjtype  = mt;
    num_decorate_repls++;
  }
}

void U_RegisterDecorateMonsters(void)
{
  int i, sp_next = num_sprites, n = 0;
  int orig_states = num_states;  /* the stock state count before any growth */
  int st_cursor   = num_states;  /* next free state slot (advances as we clone) */
  int mt_cursor   = num_mobj_types; /* next free mobjtype slot */

  if (!parsed)
    parse_decorate();

  num_decorate_repls = 0;
  for (i = 0; i < num_actors; i++)
  {
    decorate_actor_t *a = &actors[i];
    int basedn, basemt;
    if (!a->replaces[0])
      continue;
    /* monsters only: the replaced class must be a known stock monster with a
     * live mobjtype.  Weapons/ammo/blood/puff are handled elsewhere. */
    basedn = resolve_class(a->replaces, NULL, 0);
    basemt = mobjtype_for_doomednum(basedn);
    if (basemt < 0)
      continue;
    if (!(mobjinfo[basemt].flags & MF_COUNTKILL) &&
        !(mobjinfo[basemt].flags & MF_SHOOTABLE))
      continue;                  /* not a monster */
    register_one_monster_repl(a, &sp_next, &st_cursor, orig_states, &mt_cursor);
    n++;
  }

  if (n)
    lprintf(LO_INFO, "U_RegisterDecorateMonsters: %d monster replacement(s)\n",
            n);
}

/* The replacement mobjtype for a stock editor number, or -1. */
int U_DecorateReplacementType(int doomednum)
{
  int i;
  for (i = 0; i < num_decorate_repls; i++)
    if (decorate_repls[i].doomednum == doomednum)
      return decorate_repls[i].mobjtype;
  return -1;
}

/* True if an actor's parent chain roots in SexActor: these are the post-death
 * follow-on forms the death system's spawn script instantiates by name. */
static int is_sexactor_derived(const decorate_actor_t *a)
{
  const decorate_actor_t *cur = a;
  int depth = 0;
  while (cur && cur->parent[0] && depth < 16)
  {
    if (!strcasecmp(cur->parent, "SexActor"))
      return 1;
    cur = find_actor(cur->parent);
    depth++;
  }
  return 0;
}

/* Register the SexActor-derived actors as spawnable mobjtypes.  They carry no
 * editor number -- the death system spawns them by class name through the ACS
 * SpawnForced path -- so give each its actorname (so zacs_actor_type resolves
 * it) and build its state chain via the shared builder.  The actor's own Spawn
 * label is its spawn entry point. */
/* Register one parsed actor as a spawn-only mobjtype: build its Spawn-state
 * chain and fill a minimal mobjinfo (no editor number; resolved by name).
 * Returns the new mobjtype, or -1 if it has no usable sequence. */
static int register_spawnonly_actor(decorate_actor_t *a, int *st_cursor,
                                    int *mt_cursor, int *sp_next)
{
  int mt, sp, base, spawn_label;
  mobjinfo_t *info;
  char *nm;

  if (a->seq_len <= 0)
    return -1;

  sp = decorate_sprite_index(a->seq_sprite[0] ? a->seq_sprite
                                              : a->seq[0].spr, sp_next);
  base = *st_cursor;
  dsda_GetState(base + a->seq_len - 1);   /* grow to fit before building */
  decorate_build_states(a, sp, base, a->seq_len, sp_next, NULL);
  *st_cursor += a->seq_len;

  spawn_label = decorate_label_index(a, "Spawn");

  mt   = (*mt_cursor)++;
  info = dsda_GetMobjInfo(mt);
  memset(info, 0, sizeof(*info));
  info->doomednum   = -1;
  decorate_record_statemap(mt, base, a);
  info->spawnstate  = base +
    (spawn_label >= 0 ? a->seqlabel[spawn_label].frame : 0);
  info->spawnhealth = 1000;
  info->mass        = 100;
  info->damage      = a->damage;
  info->radius      = (a->radius >= 0 ? a->radius : 20) * FRACUNIT;
  info->height      = (a->height >= 0 ? a->height : 16) * FRACUNIT;
  info->seestate = info->painstate = info->meleestate =
    info->missilestate = info->deathstate = info->xdeathstate =
    info->raisestate = 0;
  info->flags = (a->solid ? MF_SOLID : 0) |
                (a->nogravity ? MF_NOGRAVITY : 0) |
                (a->spawnceiling ? (MF_SPAWNCEILING | MF_NOGRAVITY) : 0) |
                ((a->translucent || a->alpha < FRACUNIT)
                                 ? MF_TRANSLUCENT : 0);

  nm = malloc(strlen(a->name) + 1);
  if (nm) { strcpy(nm, a->name); info->actorname = nm; }

  if (a->num_uvars > 0 && num_uvarmaps < MAX_UVARMAPS)
  {
    uvarmap_t *m = &uvarmaps[num_uvarmaps++];
    int u;
    m->type  = mt;
    m->slots = a->uvar_slots;
    m->num   = a->num_uvars;
    for (u = 0; u < a->num_uvars; u++)
    {
      memcpy(m->var[u].name, a->uvar[u].name, sizeof(m->var[u].name));
      m->var[u].base = a->uvar[u].base;
      m->var[u].len  = a->uvar[u].len;
    }
  }

  /* +USESPECIAL: record the state its "Active" label resolves to so the
   * use-trace can switch a used actor of this type into it (the follow-on
   * actors are used to start their interaction). */
  if (a->use_special && num_useacts < MAX_USEACTS)
  {
    int li = decorate_label_index(a, "Active");
    if (li >= 0)
    {
      useacts[num_useacts].type        = mt;
      useacts[num_useacts].activestate = base + a->seqlabel[li].frame;
      num_useacts++;
    }
  }
  return mt;
}

void U_RegisterDecorateSexActors(void)
{
  int i, s, sp_next = num_sprites, n = 0;
  int st_cursor = num_states;
  int mt_cursor = num_mobj_types;

  if (!parsed)
    parse_decorate();

  /* First register any class named by an A_SpawnItemEx frame that is not
   * already a registered type (e.g. PrettyHeart), so the spawn action can
   * resolve its target.  These are plain decorations with no editor number. */
  for (s = 0; s < num_decorate_spawns; s++)
  {
    decorate_actor_t *t;
    if (decorate_type_by_name(decorate_spawns[s]) >= 0)
      continue;
    t = find_actor_mut(decorate_spawns[s]);
    if (t && t->seq_len > 0)
      register_spawnonly_actor(t, &st_cursor, &mt_cursor, &sp_next);
  }

  for (i = 0; i < num_actors; i++)
  {
    decorate_actor_t *a = &actors[i];
    if (!is_sexactor_derived(a) || a->seq_len <= 0)
      continue;
    if (register_spawnonly_actor(a, &st_cursor, &mt_cursor, &sp_next) >= 0)
      n++;
  }

  /* Register the DECORATE bullet-puff replacement (RedLaserPuff "replaces
   * BulletPuff" in this mod) as a spawnable type and record it so P_SpawnPuff
   * emits the mod's laser puff sprite instead of the stock Doom puff.  The puff
   * carries no editor number, so it is otherwise never registered.  The blue
   * laser puff is named per-shot by the shotgun/SSG via A_FireBullets but has no
   * "replaces" of its own, so it is registered here by name too and recorded
   * separately; the weapon-fire path selects it when a frame asked for it. */
  for (i = 0; i < num_actors; i++)
  {
    decorate_actor_t *a = &actors[i];
    if (a->seq_len <= 0)
      continue;
    if (a->replaces[0] && !strcasecmp(a->replaces, "BulletPuff"))
    {
      int mt = register_spawnonly_actor(a, &st_cursor, &mt_cursor, &sp_next);
      if (mt >= 0)
        decorate_bulletpuff_type = mt;
    }
    else if (a->name[0] && !strcasecmp(a->name, "BlueLaserPuff"))
    {
      int mt = register_spawnonly_actor(a, &st_cursor, &mt_cursor, &sp_next);
      if (mt >= 0)
        decorate_bluepuff_type = mt;
    }
  }

  /* A DECORATE actor that "replaces BFGBall" (the mod's PoetryProjectile, with
   * its own PBPR flight sprite and PBPE explosion) re-skins the BFG ball: the
   * weapon fires the stock MT_BFG, so without this the custom projectile sprite
   * never shows.  Re-point the BFG shot's flight and land states to the
   * replacement's Spawn and Death sprites.  Only the player's BFG fires MT_BFG
   * here, and vanilla has no DECORATE, so the demo hash is unchanged.  The
   * stock states (2 flight + 6 land) line up with the replacement's frames. */
  for (i = 0; i < num_actors; i++)
  {
    decorate_actor_t *a = &actors[i];
    if (a->seq_len <= 0 || !a->replaces[0] ||
        strcasecmp(a->replaces, "BFGBall"))
      continue;
    {
      /* the replacement's Spawn label is its first sequence frame; find the
       * Death label for the explosion sprite. */
      int spawn_spr = -1, death_spr = -1, li;
      if (a->seq[0].spr[0])
        spawn_spr = decorate_sprite_index(a->seq[0].spr, &sp_next);
      for (li = 0; li < a->num_seqlabels; li++)
        if (!strcasecmp(a->seqlabel[li].name, "Death"))
        {
          int fr = a->seqlabel[li].frame;
          if (fr >= 0 && fr < a->seq_len && a->seq[fr].spr[0])
            death_spr = decorate_sprite_index(a->seq[fr].spr, &sp_next);
          break;
        }
      if (spawn_spr >= 0)
      {
        dsda_GetState(S_BFGSHOT)->sprite  = spawn_spr;
        dsda_GetState(S_BFGSHOT2)->sprite = spawn_spr;
      }
      if (death_spr >= 0)
      {
        statenum_t ds[6];
        int d;
        ds[0] = S_BFGLAND;  ds[1] = S_BFGLAND2; ds[2] = S_BFGLAND3;
        ds[3] = S_BFGLAND4; ds[4] = S_BFGLAND5; ds[5] = S_BFGLAND6;
        for (d = 0; d < 6; d++)
          dsda_GetState(ds[d])->sprite = death_spr;
      }
    }
    break;
  }

  /* Some non-monster actors are reachable for map placement only through
   * doomednum aliasing or carry no editor number at all, so neither the
   * decoration registrar nor the alias path gives them an actorname.  When
   * the scripts spawn such an actor by name -- a temporary teleport-portal
   * effect, a spawned prop or marker -- zacs_actor_type cannot resolve it and
   * the spawn silently fails.  Register any remaining named non-monster actor
   * that has a usable Spawn sequence as a spawn-only type so ACS can find it
   * by name.  Monsters are deliberately excluded: register_spawnonly_actor
   * builds only a minimal Spawn-state mobjinfo, which is right for an inert
   * effect but wrong for a full monster (its See/Melee/Missile/Death chains
   * are not wired); those are reached through the monster-replacement and
   * alias paths instead.  Anything already resolvable by name is skipped, so
   * this only fills genuine gaps, and runs last so the puff/BFG passes still
   * see their targets unregistered. */
  for (i = 0; i < num_actors; i++)
  {
    decorate_actor_t *a = &actors[i];
    if (!a->name[0] || a->seq_len <= 0 || a->is_monster)
      continue;
    if (decorate_type_by_name(a->name) >= 0)
      continue;
    if (register_spawnonly_actor(a, &st_cursor, &mt_cursor, &sp_next) >= 0)
      n++;
  }

  if (n)
    lprintf(LO_INFO, "U_RegisterDecorateSexActors: %d actor(s)\n", n);
}

/* Resolve a 4-char sprite name to a sprite index, growing the table if the
 * name is new.  *sp_next tracks the next free slot across calls. */
static int decorate_sprite_index(const char *name, int *sp_next)
{
  int k;
  for (k = 0; k < *sp_next; k++)
    if (sprnames[k] && !strncasecmp(sprnames[k], name, 4))
      return k;
  k = (*sp_next)++;
  *dsda_GetSprite(k) = strdup(name);
  return k;
}

/* Build one weapon-state chain from a captured block; returns the index of
 * the first state, or -1 if the block is empty.  base_fire is appended to a
 * Fire chain's first action-less firing frame via WA_BASEFIRE wiring done by
 * the caller through the per-frame act codes. */
static int build_weapon_chain(decorate_actor_t *a, int wi, int slot,
                              int *st_cursor, int *sp_next)
{
  int n = a->wst[wi].len, f, first;
  int has_sound = 0;
  if (!a->wst[wi].present || n <= 0)
    return -1;

  /* Does this chain supply its own firing sound?  If so the base-fire
   * delegation must not also play the stock sound. */
  for (f = 0; f < n; f++)
    if (a->wst[wi].fr[f].act == WA_PLAYSOUND)
    { has_sound = 1; break; }

  first = *st_cursor;
  for (f = 0; f < n; f++)
  {
    int cur  = (*st_cursor)++;
    int last = (f == n - 1);
    int spr  = decorate_sprite_index(a->wst[wi].fr[f].spr, sp_next);
    /* sprite table resolved (and possibly grown) above; only now take the
     * state pointer, which dsda_GetState may itself move -- nothing
     * dereferences a stale states[] pointer across these calls.  Weapon
     * states never set misc1/misc2: the engine uses them as gun-sprite
     * offsets, so they must stay zero. */
    state_t *st = dsda_GetState(cur);
    st->sprite    = spr;
    st->frame     = a->wst[wi].fr[f].frame;
    st->tics      = a->wst[wi].fr[f].tics;
    st->nextstate = last ? (a->wst[wi].loops ? first : cur) : cur + 1;
    st->misc1 = st->misc2 = 0;
    st->action.arg0 = (arg0_t)NULL;

    switch (a->wst[wi].fr[f].act)
    {
      case WA_READY:    st->action.arg0 = (arg0_t)A_WeaponReady; break;
      case WA_RAISE:    st->action.arg0 = (arg0_t)A_Raise;       break;
      case WA_LOWER:    st->action.arg0 = (arg0_t)A_Lower;       break;
      case WA_GUNFLASH: st->action.arg0 = (arg0_t)A_GunFlash;    break;
      case WA_REFIRE:   st->action.arg0 = (arg0_t)A_ReFire;      break;
      case WA_BASEFIRE:
        /* a weapon that named the blue laser puff uses the blue-puff wrapper
         * (which also squelches the stock sound); one that supplies its own
         * sound but the default puff uses the quiet wrapper; otherwise the
         * plain base attack.  The blue puff itself is registered later (with the
         * scene actors), so the wrapper resolves its type at fire time -- route
         * on the parse-time flag alone here. */
        if (a->uses_blue_puff)
          st->action.arg0 = weapon_base_fire_bluepuff(slot);
        else
          st->action.arg0 = has_sound ? weapon_base_fire_quiet(slot)
                                       : weapon_base_fire(slot);
        break;
      case WA_PLAYSOUND:
        /* Resolve the captured sound name to an sfx id (a $random set yields a
         * logical id the sound system varies per play) and record it against
         * this state; the codepointer reads it back at fire time.  misc1/2 stay
         * zero so the gun sprite is not offset. */
        if (a->wst[wi].fr[f].snd >= 0)
        {
          int sfx = decorate_resolve_sfx(decorate_sounds[a->wst[wi].fr[f].snd]);
          if (sfx > 0)
          {
            decorate_set_weapon_sound(cur, sfx);
            st->action.arg0 = (arg0_t)A_DecorateWeaponSound;
          }
        }
        break;
      default: break;
    }
  }
  return first;
}

void U_RegisterDecorateWeapons(void)
{
  int i, nweap = 0;
  int sp_next   = num_sprites;
  int st_start  = num_states;
  int st_cursor = num_states;

  if (!parsed)
    parse_decorate();

  for (i = 0; i < num_actors; i++)
  {
    decorate_actor_t *a = &actors[i];
    int slot = a->wpn_slot;
    int ready, up, down, atk, flash;
    int first_of[WST_COUNT];
    int wi;

    if (slot < 0 || slot >= NUMWEAPONS)
      continue;
    /* require at least the structural states to consider it a real custom
     * weapon; a bare "replaces" with no states leaves the base intact */
    if (!a->wst[WST_READY].present && !a->wst[WST_FIRE].present)
      continue;

    ready = build_weapon_chain(a, WST_READY,  slot, &st_cursor, &sp_next);
    up    = build_weapon_chain(a, WST_SELECT, slot, &st_cursor, &sp_next);
    down  = build_weapon_chain(a, WST_DESEL,  slot, &st_cursor, &sp_next);
    atk   = build_weapon_chain(a, WST_FIRE,   slot, &st_cursor, &sp_next);
    flash = build_weapon_chain(a, WST_FLASH,  slot, &st_cursor, &sp_next);

    first_of[WST_READY]  = ready;
    first_of[WST_SELECT] = up;
    first_of[WST_DESEL]  = down;
    first_of[WST_FIRE]   = atk;
    first_of[WST_FLASH]  = flash;

    /* thread each block's "goto Label" terminator to the first state of the
     * target chain (e.g. a Fire block ending "goto Ready" must return to the
     * ready loop, or the weapon would freeze after one shot).  When the actor
     * did not provide the target block itself (a common case: a custom Fire
     * state that ends "goto Ready" while inheriting the stock Ready state), the
     * built chain has no first state for it, so fall back to the base weapon's
     * inherited state for that label.  Without this the terminal state keeps
     * its self-loop and the weapon freezes after one shot. */
    for (wi = 0; wi < WST_COUNT; wi++)
    {
      int base = first_of[wi];
      int dst;
      if (base < 0 || !a->wst[wi].present || a->wst[wi].len <= 0)
        continue;
      if (a->wst[wi].goto_dst < 0)
        continue;
      dst = first_of[a->wst[wi].goto_dst];
      if (dst < 0)
      {
        /* target block not built here -- use the base weapon's state */
        switch (a->wst[wi].goto_dst)
        {
          case WST_READY:  dst = weaponinfo[slot].readystate;  break;
          case WST_SELECT: dst = weaponinfo[slot].upstate;     break;
          case WST_DESEL:  dst = weaponinfo[slot].downstate;   break;
          case WST_FIRE:   dst = weaponinfo[slot].atkstate;    break;
          case WST_FLASH:  dst = weaponinfo[slot].flashstate;  break;
          default: break;
        }
      }
      if (dst >= 0)
        dsda_GetState(base + a->wst[wi].len - 1)->nextstate = dst;
    }

    /* repoint only the states the DECORATE actually provided; leave the
     * base weapon's own state for any the actor omitted */
    if (ready >= 0) weaponinfo[slot].readystate = ready;
    if (up    >= 0) weaponinfo[slot].upstate    = up;
    if (down  >= 0) weaponinfo[slot].downstate  = down;
    if (atk   >= 0) weaponinfo[slot].atkstate   = atk;
    if (flash >= 0) weaponinfo[slot].flashstate = flash;

    nweap++;
  }

  if (nweap)
    lprintf(LO_INFO, "U_RegisterDecorateWeapons: %d weapon(s) "
            "(%d states)\n", nweap, st_cursor - st_start);
}

/* does the DECORATE lump redefine this sprite's state sequence? */
dbool U_DecorateMentionsSprite(const char *name)
{
  int i;
  if (!parsed)
    parse_decorate();
  for (i = 0; i < num_sprite_names; i++)
    if (!strncasecmp(sprite_names[i], name, 4))
      return true;
  return false;
}

int U_DecorateAliasDoomedNum(int doomednum)
{
  int i;

  if (!parsed)
    parse_decorate();

  for (i = 0; i < num_actors; i++)
    if (actors[i].doomednum == doomednum)
    {
      int dn = resolve_class(actors[i].name, &actors[i], 0);
      if (dn >= 0 && dn != doomednum)
        lprintf(LO_INFO, "U_DecorateAliasDoomedNum: %s %d -> %d\n",
                actors[i].name, doomednum, dn);
      return (dn != doomednum) ? dn : -1;
    }
  return -1;
}

dbool U_IsInertZDoomThing(int doomednum)
{
  return (doomednum >= 9027 && doomednum <= 9033) ||  /* particle fountains */
         (doomednum >= 9070 && doomednum <= 9078) ||  /* interp/camera/stacks */
         (doomednum >= 32000 && doomednum <= 32003) ||/* editor cameras */
         doomednum == 9024 ||                         /* patrol point */
         doomednum == 9025 ||                         /* security camera */
         doomednum == 9026 ||                         /* spark */
         (doomednum >= 9080 && doomednum <= 9082) ||  /* sky viewpoint/picker/silencer */
         (doomednum >= 9800 && doomednum <= 9859) ||  /* GZDoom dynamic lights */
         (doomednum >= 14001 && doomednum <= 14067) ||/* ambient sounds */
         (doomednum >= 1400 && doomednum <= 1410);    /* sound sequence overrides */
}

/* Spawnable ZDoom utility markers: invisible, intangible mobjs whose
 * whole purpose is to carry a TID -- ACS SpawnSpot anchors and teleport
 * destinations.  Cloned from MT_TELEPORTMAN (S_NULL spawn state,
 * NOBLOCKMAP|NOSECTOR), with actor names so ACS ThingCountName can see
 * them. */
void U_RegisterZDoomUtilityThings(void)
{
  static const struct { int ednum; const char *name; } spots[] =
  {
    { 9001, "MapSpot" },
    { 9013, "MapSpotGravity" },
    { 9043, "TeleportDest3" },
    { 9044, "TeleportDest2" },
  };
  size_t i;

  int base = num_mobj_types;

  /* one growth covers all entries (the table doubles when touched) */
  dsda_GetMobjInfo(base + (int)(sizeof(spots) / sizeof(spots[0])) - 1);
  for (i = 0; i < sizeof(spots) / sizeof(spots[0]); i++)
  {
    mobjinfo_t *info = &mobjinfo[base + (int)i];
    *info = mobjinfo[MT_TELEPORTMAN];
    info->doomednum = spots[i].ednum;
    info->actorname = spots[i].name;
  }
}
