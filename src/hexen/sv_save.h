/* Emacs style mode select   -*- C -*-
 *-----------------------------------------------------------------------------
 *
 *  Hexen hub world-state persistence.
 *
 *  When the player leaves a map for another map in the same hub cluster,
 *  the departing map's world state (sectors, lines, mobjs, movers, scripts,
 *  polyobjects) is archived in memory; revisiting the map restores it, so
 *  solved puzzles, opened doors and slain monsters persist the way Raven's
 *  hub system intends.  Derived from Raven's sv_save.c via dsda-doom.
 *
 *-----------------------------------------------------------------------------
 */

#ifndef __SV_SAVE_H__
#define __SV_SAVE_H__

/* Travel to another map (warp number) at the given arrival position,
 * archiving the current map if the destination is in the same cluster and
 * restoring the destination from its archive when one exists. */
void SV_MapTeleport(int map, int position);

/* Forget all per-map archives (new game / new cluster). */
void SV_HubInit(void);

#endif
