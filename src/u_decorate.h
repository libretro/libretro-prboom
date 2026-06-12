/* u_decorate.h: DECORATE doomednum aliasing (see u_decorate.c). */

#ifndef U_DECORATE_H
#define U_DECORATE_H

#include "doomtype.h"

/* If `doomednum` belongs to a DECORATE actor whose inheritance/replaces
 * chain roots in a class with a known editor number, return that number;
 * otherwise -1. */
/* 256-byte palette remap table built from a DECORATE Translation property for
 * the given built-translation id, or NULL.  Used by the sprite renderer. */
const unsigned char *U_DecorateTranslation(int xlat_id);

/* True if the pointer is one of the built translation tables (a validity
 * guard for the sprite renderer). */
int U_DecorateTranslationOK(const unsigned char *p);

int U_DecorateAliasDoomedNum(int doomednum);

/* If `id` is a logical $random SNDINFO sound (more than one member), return a
 * randomly chosen member's sfx id (varies per call); otherwise return `id`
 * unchanged.  The sound system calls this so a $random sound varies between
 * plays the way it does in ZDoom. */
int U_SoundRandomId(int id);

/* true if the wad's DECORATE lump redefines the named sprite's state
 * sequence (r_things only unifies art for such sprites) */
dbool U_DecorateMentionsSprite(const char *name);

/* Register static single-frame DECORATE decorations as thing types via the
 * DSDHacked tables.  Call once at startup, after dehacked processing and
 * before R_Init; Doom game only. */
void U_RegisterDecorateThings(void);

/* Repoint Doom weapon slots to the state chains of DECORATE weapons that
 * inherit from / replace a base weapon class.  Call once at startup after
 * U_RegisterDecorateThings (shares the DSDHacked state/sprite growth) and
 * before R_Init; Doom game only. */
void U_RegisterDecorateWeapons(void);

/* Register DECORATE "replaces" monsters as mobjtypes cloned from the stock
 * class they replace, and expose the redirect so P_SpawnMapThing can spawn
 * the replacement in place of the stock editor number.  Call after
 * U_RegisterDecorateThings (shares the DSDHacked state/sprite growth). */
void U_RegisterDecorateMonsters(void);

/* Register the SexActor-derived actors as ACS-spawnable mobjtypes (the
 * death system spawns them by class name).  Call after the monster
 * replacements so the spawn names resolve. */
void U_RegisterDecorateSexActors(void);
int  U_DecorateReplacementType(int doomednum);

/* ZDoom editor-only map things (particle fountains, interpolation points,
 * camera/view stacks, editor cameras): skip without a warning. */
dbool U_IsInertZDoomThing(int doomednum);

/* Register spawnable ZDoom utility markers (MapSpot, TeleportDest2/3)
 * as dynamic mobj types; call once after dsda_InitTables. */
void U_RegisterZDoomUtilityThings(void);

/* DECORATE user variables: total slots a mobjtype declares, and name->slot
 * resolution for the ACS user-variable builtins.  U_DecorateUserVarSlot
 * returns 1 and fills the base and len out-params on success, 0 if the name
 * is undeclared. */
int U_DecorateUserVarCount(int type);
int U_DecorateUserVarSlot(int type, const char *name, int *base, int *len);

/* +USESPECIAL decorations: the Active state a use-activatable mobjtype enters
 * when the player presses use on it, or -1 if not use-activatable. */
int U_DecorateActiveState(int type);
int U_DecorateStateForType(int type, const char *label);

#endif
