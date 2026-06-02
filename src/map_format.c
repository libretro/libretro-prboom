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
  P_CrossSpecialLine,
  P_ShootSpecialLine,
  P_PlayerInSpecialSector,
  VF_HEXEN
};

void P_ApplyMapFormat(void)
{
  if (hexen)
    map_format = hexen_map_format;
  else
    map_format = doom_map_format;
}
