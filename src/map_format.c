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
  VF_DOOM
};

void P_ApplyMapFormat(void)
{
  /* Only the Doom format exists at this stage.  Heretic and Hexen
   * descriptors (and the game selection) land with their handlers in
   * later commits; until then every game uses Doom dispatch, which is
   * exactly the prior behaviour. */
  map_format = doom_map_format;
}
