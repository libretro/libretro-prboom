/* Emacs style mode select   -*- C -*-
 *-----------------------------------------------------------------------------
 *
 *  Map format abstraction (reduced).
 *
 *  Adapted from dsda-doom's dsda/map_format by Ryan Krafnick.  dsda uses
 *  this descriptor to let one engine drive Doom, Heretic and Hexen maps:
 *  per-game behaviour is reached through function pointers selected at
 *  level load instead of hardcoded Doom logic.
 *
 *  This is a deliberately reduced version: it carries only the format
 *  booleans and the per-line-trigger dispatchers that this Doom-lineage
 *  core can map cleanly onto its existing p_spec functions, plus the
 *  thing-visibility mask.  The Hexen/ZDoom-only members (polyobject and
 *  thing_id management, the Hexen-sized mapthing, the scripted line
 *  specials and the per-game thinker dispatchers) are intentionally
 *  omitted; they can be added when Hexen support needs them.  The Doom
 *  descriptor's pointers resolve to the existing engine functions, so a
 *  Doom session behaves exactly as before -- the only change is one level
 *  of indirection that resolves to the same code.
 *
 *-----------------------------------------------------------------------------
 */

#ifndef __MAP_FORMAT__
#define __MAP_FORMAT__

#include "doomtype.h"
#include "r_defs.h"
#include "d_player.h"
#include "p_mobj.h"

/* Thing-visibility flags: a map thing is skipped if its game's bit is not
 * set in the active format's visibility mask. */
#define VF_DOOM    0x01
#define VF_HERETIC 0x02
#define VF_HEXEN   0x04

typedef struct
{
  dbool zdoom;
  dbool hexen;
  dbool polyobjs;
  dbool acs;
  dbool sndseq;
  dbool animdefs;
  dbool doublesky;
  /* Per-line / per-sector trigger dispatchers.  Signatures match this
   * core's existing p_spec functions (note: no trailing activation arg,
   * unlike dsda). */
  void (*cross_special_line)(line_t *line, int side, mobj_t *thing);
  void (*shoot_special_line)(mobj_t *thing, line_t *line);
  void (*player_in_special_sector)(player_t *player);
  /* Hexen scripted line-special executor: decodes a byte special + 5 args
   * and runs the corresponding action.  NULL for Doom/Heretic. */
  dbool (*execute_line_special)(int special, byte *args, line_t *line,
                                int side, mobj_t *mo);
  int visibility;
} map_format_t;

extern map_format_t map_format;

/* Install the descriptor for the current game.  Called at level setup.
 * For now only the Doom descriptor exists; Heretic/Hexen descriptors are
 * added with their handlers in later commits. */
void P_ApplyMapFormat(void);

#endif
