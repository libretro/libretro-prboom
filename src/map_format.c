/* Emacs style mode select   -*- C -*-
 *-----------------------------------------------------------------------------
 *
 *  Map format abstraction (reduced) - implementation.
 *
 *  See map_format.h.  The Doom descriptor points every dispatcher at the
 *  engine's existing functions, so dispatching through map_format is
 *  behaviourally identical to calling them directly.
 *
 *-----------------------------------------------------------------------------
 */

#include "doomstat.h"
#include "p_spec.h"
#include "map_format.h"
#include "hexen/p_spec_hexen.h"

map_format_t map_format;

static const map_format_t doom_map_format =
{
  false,                      /* zdoom    */
  false,                      /* hexen    */
  false,                      /* polyobjs */
  false,                      /* acs      */
  false,                      /* sndseq   */
  false,                      /* animdefs */
  false,                      /* doublesky*/
  P_CrossSpecialLine,
  P_ShootSpecialLine,
  P_PlayerInSpecialSector,
  NULL,                       /* execute_line_special (Doom: none) */
  VF_DOOM
};

/* Hexen map format.  The format flags drive the Hexen-sized linedef/thing
 * parsing in p_setup; polyobjs/acs/sndseq/animdefs are declared here but
 * their handlers are added in later commits.  The per-line trigger
 * dispatchers still point at the Doom functions for now -- Hexen's scripted
 * line specials land with the specials layer -- so a Hexen map currently
 * loads and renders with Doom-style line activation. */
static const map_format_t hexen_map_format =
{
  false,                      /* zdoom    */
  true,                       /* hexen    */
  true,                       /* polyobjs */
  true,                       /* acs      */
  true,                       /* sndseq   */
  true,                       /* animdefs */
  false,                      /* doublesky*/
  P_CrossHexenSpecialLine,
  P_ShootHexenSpecialLine,
  P_PlayerInSpecialSector,
  P_ExecuteHexenLineSpecial,
  VF_HEXEN
};

void P_ApplyMapFormat(void)
{
  if (hexen)
    map_format = hexen_map_format;
  else
    map_format = doom_map_format;
}

/* ZDoom 'Doom-in-Hexen': Hexen-sized THINGS/LINEDEFS records in a Doom-game
 * map (detected by a BEHAVIOR lump).  The hexen format flag drives the
 * Hexen-stride parsing in p_setup and the positive-bit thing filtering in
 * P_SpawnMapThing; zdoom marks the specials as ZDoom-numbered.
 *
 * The trigger dispatchers are inert stubs for now: ZDoom action specials
 * share Hexen's special+args encoding but not its numbering, and the Doom
 * dispatchers would misinterpret the numbers as Doom line types.  The
 * specials translation layer lands in a later commit; until then such maps
 * load and are walkable, but lines and scripted sectors do nothing. */

static void P_InertCrossSpecialLine(line_t *line, int side, mobj_t *thing)
{
  (void)line; (void)side; (void)thing;
}

static void P_InertShootSpecialLine(mobj_t *thing, line_t *line)
{
  (void)thing; (void)line;
}

static void P_InertPlayerInSpecialSector(player_t *player)
{
  (void)player;
}

static const map_format_t zdoom_in_doom_map_format =
{
  true,                       /* zdoom    */
  true,                       /* hexen    */
  false,                      /* polyobjs */
  false,                      /* acs      */
  false,                      /* sndseq   */
  false,                      /* animdefs */
  false,                      /* doublesky*/
  P_InertCrossSpecialLine,
  P_InertShootSpecialLine,
  P_InertPlayerInSpecialSector,
  NULL,                       /* execute_line_special (not yet) */
  VF_DOOM
};

void P_ApplyZDoomInDoomMapFormat(void)
{
  map_format = zdoom_in_doom_map_format;
}
