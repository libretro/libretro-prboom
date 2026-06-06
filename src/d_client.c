/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2000 by
 *  Jess Haas, Nicolas Kalkhof, Colin Phipps, Florian Schulze
 *  Copyright 2005, 2006 by
 *  Florian Schulze, Colin Phipps, Neil Stevens, Andrey Budko
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 *  02111-1307, USA.
 *
 * DESCRIPTION:
 *    Network client. Passes information to/from server, staying
 *    synchronised.
 *    Contains the main wait loop, waiting for network input or
 *    time before doing the next tic.
 *    Rewritten for LxDoom, but based around bits of the old code.
 *
 *-----------------------------------------------------------------------------
 */

#include "config.h"
#include <sys/types.h>

#include "doomtype.h"
#include "doomstat.h"
#include "d_net.h"
#include "z_zone.h"

#include "d_main.h"
#include "g_game.h"
#include "m_menu.h"

#include "i_system.h"
#include "i_main.h"
#include "i_video.h"
#include "m_argv.h"
#include "r_fps.h"
#include "lprintf.h"

ticcmd_t         netcmds[MAXPLAYERS][BACKUPTICS];
static ticcmd_t* localcmds;
int maketic;
int ticdup = 1;
int              wanted_player_number;

doomcom_t*      doomcom;

void D_InitNetGame (void)
{
  int i;

  doomcom = Z_Malloc(sizeof *doomcom, PU_STATIC, NULL);
  doomcom->consoleplayer = 0;
  doomcom->numnodes = 0; doomcom->numplayers = 1;
  localcmds = netcmds[consoleplayer];
  netgame = (M_CheckParm("-solo-net") != 0) || (M_CheckParm("-net1") != 0);

  for (i=0; i<doomcom->numplayers; i++)
    playeringame[i] = TRUE;
  for (; i<MAXPLAYERS; i++)
    playeringame[i] = FALSE;

  consoleplayer = displayplayer = doomcom->consoleplayer;
}

void D_BuildNewTiccmds(void)
{
  I_StartTic();

  if (maketic <= gametic)
  {
	 // Create new ticcmds if running behind
	 G_BuildTiccmd(&localcmds[maketic % BACKUPTICS]);
	 maketic++;
  }
  else
  {
	 // Update latest ticcmd if running ahead
	 ticcmd_t prevcmd = localcmds[(maketic-1) % BACKUPTICS];
	 ticcmd_t *cmd = &localcmds[(maketic-1) % BACKUPTICS];

	 G_BuildTiccmd(cmd);
	 cmd->angleturn += prevcmd.angleturn;
	 cmd->buttons |= prevcmd.buttons;
  }
}

/* Net turn staged in local ticcmds the simulation has not consumed yet:
 * the exact view-angle delta the next tic(s) will apply.  R_SetupFrame
 * adds this to the rendered view angle when low-latency turning is on,
 * so the camera answers input at frame rate while the simulation and
 * any demo being recorded see byte-identical ticcmds.  Summing shorts
 * wraps exactly like the angle's top sixteen bits, so the preview
 * equals the sim's future result modulo full revolutions. */
int D_PendingLocalTurn(void)
{
  int t;
  signed short sum = 0;

  for (t = gametic; t < maketic; t++)
    sum += localcmds[t % BACKUPTICS].angleturn;
  return sum;
}

void TryRunTics(void)
{
  fixed_t overflow = 0;

  // Increment tic fraction
  tic_vars.frac += tic_vars.frac_step;
  if(tic_vars.frac > FRACUNIT) {
    overflow = tic_vars.frac - FRACUNIT;
    tic_vars.frac = FRACUNIT;
  }

  D_BuildNewTiccmds();

  if(tic_vars.frac == FRACUNIT) {
    tic_vars.frac = overflow;
    if (!paused) {
      if (advancedemo)
        D_DoAdvanceDemo ();
      G_Ticker ();
    }
    if (menuactive)
      M_Ticker ();
    gametic++;
  }
}

