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
#include "p_pspr.h"
#include "d_items.h"
#include "sounds.h"
#include "p_mobj.h"
#include "p_zacs.h"
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
  char sprite[5];               /* 4-char sprite of a "SPRT F -1" spawn */
  int  frame;                   /* 0-based frame letter, FF_FULLBRIGHT or'd */
  int  spawn_static;            /* spawn state parsed and tics == -1 */

  /* animated Spawn sequence (no action functions): a chain of frames the
   * registrar turns into looping/terminating states.  Captured only for a
   * plain "SPRITE <letters> <tics>" sequence ending in loop/stop.  A small,
   * safe subset of per-frame action functions is also captured (act != 0)
   * and wired onto the registered state. */
#define MAX_SPAWN_FRAMES 32
  struct { short frame; short tics; short act; short snd; } seq[MAX_SPAWN_FRAMES];
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
  struct { short flow; short target; short jtoff; short has_jump;
           char tname[24]; char jtname[24]; dexpr_t jcond; }
        seqflow[MAX_SPAWN_FRAMES];
  /* Labels encountered in the state block, mapped to the frame index where
   * each begins.  A multi-label actor (Spawn + InitLoop + Idle + ...) records
   * every label so goto/A_JumpIf targets resolve to a frame. */
#define MAX_SEQ_LABELS 16
  struct { char name[24]; short frame; } seqlabel[MAX_SEQ_LABELS];
  int  num_seqlabels;
  int  multi_state;            /* 1 once a non-Spawn label/flow appears */
  char seq_sprite[5];           /* sprite the sequence uses (one sprite)  */
  int  seq_len;                 /* number of frames captured              */
  int  seq_loops;              /* 1 = loop back to frame 0, 0 = stop      */
  int  translucent;             /* RenderStyle Translucent / Add          */
  int  alpha;                   /* 16.16 alpha, FRACUNIT if unset         */
  int  damage;                  /* DECORATE Damage property (default 0)    */

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
} decorate_actor_t;

static decorate_actor_t actors[MAX_DECORATE_ACTORS];
static int num_actors;

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
  DA_CHANGEFLAG        /* A_ChangeFlag("FLAG", bool) -> set/clear a flag */
};

/* Sound names captured for DA_PLAYSOUND frames, resolved to sfx slots at
 * registration time (the sound tables are not grown during the parse). */
#define MAX_DECORATE_SOUNDS 256
static char decorate_sounds[MAX_DECORATE_SOUNDS][32];
static int  num_decorate_sounds;

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
  WA_BASEFIRE          /* delegate to the replaced weapon's native attack */
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
  /* A_PlaySound is deliberately NOT mapped: it is an mobj action
   * (void A_PlaySound(mobj_t*)) reading its sound from state->misc1, but the
   * engine's weapon path dispatches actions as (player,psp) and uses misc1/2
   * as sprite offsets.  Wiring it here would mis-call and shift the gun
   * sprite, so weapon-frame sounds are simply skipped. */
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
  a->alpha = FRACUNIT;
  a->wpn_slot = -1;

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

  /* append the parent's captured frames after the child's */
  base = c->seq_len;
  if (base + pp->seq_len > MAX_SPAWN_FRAMES)
    return;                 /* too large to merge; leave child as-is */
  for (f = 0; f < pp->seq_len; f++)
  {
    c->seq[base + f]   = pp->seq[f];
    c->seqop[base + f] = pp->seqop[f];
    c->seqflow[base + f] = pp->seqflow[f];
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
    else if (!strcasecmp(word, "Damage"))
    {
      p = skip_space(p, end);
      p = read_word(p, end, word, sizeof(word));
      a->damage = atoi(word);
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
          fr[0] == '#' || fr[0] == '-') && !strcmp(tics, "-1"))
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
               (tics[0] >= '0' && tics[0] <= '9') &&
               (a->seq_len == 0 ||
                !strncasecmp(a->seq_sprite, nspr, 4)))
      {
        /* animated frames: one entry per frame letter, all this sprite.
         * Only a single sprite per Spawn sequence is supported. */
        int t = atoi(tics);
        int bright = 0;
        short act = DA_NONE, snd = -1;
        struct { short uvslot; short flag; short fval; dexpr_t idx; dexpr_t val; }
              opstage;
        struct { short has_jump; short jtoff; char tname[24]; dexpr_t jcond; }
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
          else if (!strcasecmp(fn, "A_ActiveSound"))   act = DA_ACTIVESOUND;
          else if (!strcasecmp(fn, "A_NoBlocking") ||
                   !strcasecmp(fn, "A_Fall"))          act = DA_NOBLOCKING;
          else if (!strcasecmp(fn, "A_FaceTarget"))    act = DA_FACETARGET;
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
                   !strcasecmp(fn, "A_JumpIf"))
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
          }
        }
        if (a->seq_len == 0)
        {
          memcpy(a->seq_sprite, nspr, 4);
          a->seq_sprite[4] = 0;
        }
        if (t < 0) t = 0;
        if (t > 32767) t = 32767;
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
          if (fi == 0 && flowstage.has_jump)
          {
            a->seqflow[a->seq_len].has_jump = 1;
            a->seqflow[a->seq_len].jtoff    = flowstage.jtoff;
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
static int decorate_resolve_sfx(const char *name)
{
  int i;
  const char *stem;
  sfxinfo_t *sfx;
  char *copy;

  if (!name || !name[0])
    return 0;
  /* the bound lump may already carry a "ds" prefix; index by the stem */
  stem = ((name[0] == 'd' || name[0] == 'D') &&
          (name[1] == 's' || name[1] == 'S')) ? name + 2 : name;

  for (i = 1; i < num_sfx; i++)
    if (S_sfx[i].name && !strcasecmp(S_sfx[i].name, stem))
      return i;

  i   = num_sfx;                 /* grow one slot */
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
    state_t *state;
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
      int f;
      for (f = 0; f < nframes; f++)
      {
        int cur  = st_base + count + f;
        int last = (f == nframes - 1);
        state = dsda_GetState(cur);
        state->sprite = sp;
        if (a->spawn_static)
        {
          state->frame = a->frame;
          state->tics  = -1;
          state->nextstate = cur;
        }
        else
        {
          state->frame = a->seq[f].frame;
          state->tics  = a->seq[f].tics;
          if (a->multi_state)
          {
            /* honour this frame's recorded flow: loop back to the enclosing
             * label, freeze, wait (re-run), or goto another label; a frame
             * with no terminator falls through to the next. */
            int flow = a->seqflow[f].flow;
            if (flow == SEQF_LOOP)
            {
              /* loop back to the first frame of the label whose run contains
               * f (the nearest label at or before f) */
              int li, lab_frame = 0;
              for (li = 0; li < a->num_seqlabels; li++)
                if (a->seqlabel[li].frame <= f &&
                    a->seqlabel[li].frame >= lab_frame)
                  lab_frame = a->seqlabel[li].frame;
              state->nextstate = st_base + count + lab_frame;
            }
            else if (flow == SEQF_STOP)
              state->nextstate = cur;
            else if (flow == SEQF_WAIT)
              state->nextstate = cur;
            else if (flow == SEQF_GOTO)
            {
              int li = decorate_label_index(a, a->seqflow[f].tname);
              state->nextstate = (li >= 0)
                ? st_base + count + a->seqlabel[li].frame : cur;
            }
            else
              state->nextstate = last ? cur : cur + 1;
          }
          else
          state->nextstate = last
            ? (a->seq_loops ? first : cur)   /* loop back or freeze */
            : cur + 1;

          /* wire the safe per-frame action, if any, onto this state */
          switch (a->seq[f].act)
          {
            case DA_PLAYSOUND:
              state->action.arg0 = (arg0_t)A_PlaySound;
              state->misc1 = (a->seq[f].snd >= 0)
                ? decorate_resolve_sfx(decorate_sounds[a->seq[f].snd]) : 0;
              state->misc2 = 0;          /* positional (not full-volume) */
              break;
            case DA_ACTIVESOUND:
              /* no dedicated active-sound pointer here; emit the named or
               * (absent a name) the generic sound through A_PlaySound */
              state->action.arg0 = (arg0_t)A_PlaySound;
              state->misc1 = (a->seq[f].snd >= 0)
                ? decorate_resolve_sfx(decorate_sounds[a->seq[f].snd]) : 0;
              state->misc2 = 0;
              break;
            case DA_SCREAM:
              state->action.arg0 = (arg0_t)A_Scream;
              break;
            case DA_NOBLOCKING:
              state->action.arg0 = (arg0_t)A_Fall;
              break;
            case DA_FACETARGET:
              state->action.arg0 = (arg0_t)A_FaceTarget;
              break;
            case DA_ACS_NAMED:
              /* start a named ACS script when this frame runs.  The script
               * name is interned into a persistent table; its index rides in
               * misc1 (free on mobj states -- only weapon psprites use it). */
              if (a->seq[f].snd >= 0)
              {
                state->action.arg0 = (arg0_t)A_DecorateACSNamed;
                state->misc1 = decorate_intern_acsname(
                                 decorate_acsnames[a->seq[f].snd]);
              }
              break;
            case DA_SETUSERVAR:
            case DA_SETUSERARRAY:
              /* write a user variable when this frame runs.  misc1 holds the
               * target base slot; args[0..4] encode the value expression
               * (ka,va,op,kb,vb) and, for arrays, args[5..7]+misc2 encode the
               * index expression. */
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

          /* A_JumpIf(cond, target): a conditional jump overrides this frame's
           * action wiring.  Encode the condition in args[0..4], the resolved
           * jump-target state in args[5], and run A_DecorateJumpIf which sets
           * the mobj's state to the target when the condition is true. */
          if (a->seqflow[f].has_jump)
          {
            int tgt = cur;          /* default: no movement (cond false path) */
            if (a->seqflow[f].jtoff >= 0)
              tgt = cur + a->seqflow[f].jtoff;   /* offset N: Nth state after this one */
            else
            {
              int li = decorate_label_index(a, a->seqflow[f].jtname);
              if (li >= 0)
                tgt = st_base + count + a->seqlabel[li].frame;
            }
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
      st = first;
    }

    mt = mt_base + count_mt;
    info = dsda_GetMobjInfo(mt);
    info->doomednum   = a->doomednum;

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
    count    += nframes;        /* states consumed */
    count_mt += 1;              /* one mobj type   */
  }

  if (count_mt)
    lprintf(LO_INFO, "U_RegisterDecorateThings: %d decorations (%d states)\n",
            count_mt, count);
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
  if (!a->wst[wi].present || n <= 0)
    return -1;

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
      case WA_BASEFIRE: st->action.arg0 = weapon_base_fire(slot);break;
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
     * ready loop, or the weapon would freeze after one shot).  A goto whose
     * target we did not build leaves the terminal state frozen. */
    for (wi = 0; wi < WST_COUNT; wi++)
    {
      int base = first_of[wi];
      int dst;
      if (base < 0 || !a->wst[wi].present || a->wst[wi].len <= 0)
        continue;
      if (a->wst[wi].goto_dst < 0)
        continue;
      dst = first_of[a->wst[wi].goto_dst];
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
