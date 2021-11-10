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

#ifdef USE_SDL_NET
 #include "SDL.h"
#endif

#include "doomtype.h"
#include "doomstat.h"
#include "d_net.h"
#include "z_zone.h"

#include "d_main.h"
#include "g_game.h"
#include "m_menu.h"

#include "protocol.h"
#include "i_network.h"
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

#ifdef HAVE_NET
static dbool     server;

static int       remotetic; // Tic expected from the remote
static int       remotesend; // Tic expected by the remote

static unsigned          numqueuedpackets;
static packet_header_t** queuedpacket;

static int xtratics = 0;

static dbool   isExtraDDisplay = FALSE;

void D_CheckNetGame(void)
{
  packet_header_t *packet = Z_Malloc(sizeof(packet_header_t)+1, PU_STATIC, NULL);

  if (server) {
    lprintf(LO_INFO, "D_CheckNetGame: waiting for server to signal game start\n");
    do {
      while (!I_GetPacket(packet, sizeof(packet_header_t)+1)) {
        packet_set(packet, PKT_GO, 0);
	*(uint8_t*)(packet+1) = consoleplayer;
	I_SendPacket(packet, sizeof(packet_header_t)+1);
	I_uSleep(100000);
      }
    } while (packet->type != PKT_GO);
  }
  Z_Free(packet);
}

dbool   D_NetGetWad(const char* name)
{
#if defined(HAVE_WAIT_H)
  size_t psize = sizeof(packet_header_t) + strlen(name) + 500;
  packet_header_t *packet;
  dbool   done = FALSE;

  if (!server || strchr(name, '/')) return FALSE; // If it contains path info, reject

  do {
    // Send WAD request to remote
    packet = Z_Malloc(psize, PU_STATIC, NULL);
    packet_set(packet, PKT_WAD, 0);
    *(uint8_t*)(packet+1) = consoleplayer;
    strcpy(1+(uint8_t*)(packet+1), name);
    I_SendPacket(packet, sizeof(packet_header_t) + strlen(name) + 2);

    I_uSleep(10000);
  } while (!I_GetPacket(packet, psize) || (packet->type != PKT_WAD));
  Z_Free(packet);

  if (!strcasecmp((void*)(packet+1), name)) {
    pid_t pid;
    int   rv;
    uint8_t *p = (uint8_t*)(packet+1) + strlen(name) + 1;

    /* Automatic wad file retrieval using wget (supports http and ftp, using URLs)
     * Unix systems have all these commands handy, this kind of thing is easy
     * Any windo$e port will have some awkward work replacing these.
     */
    /* cph - caution here. This is data from an untrusted source.
     * Don't pass it via a shell. */
    if ((pid = fork()) == -1)
      perror("fork");
    else if (!pid) {
      /* Child chains to wget, does the download */
      execlp("wget", "wget", p, NULL);
    }
    /* This is the parent, i.e. main LxDoom process */
    wait(&rv);
    if (!(done = !access(name, R_OK))) {
      if (!strcmp(p+strlen(p)-4, ".zip")) {
  p = strrchr(p, '/')+1;
  if ((pid = fork()) == -1)
    perror("fork");
  else if (!pid) {
    /* Child executes decompressor */
    execlp("unzip", "unzip", p, name, NULL);
  }
  /* Parent waits for the file */
  wait(&rv);
  done = !!access(name, R_OK);
      }
      /* Add more decompression protocols here as desired */
    }
    Z_Free(buffer);
  }
  return done;
#else /* HAVE_WAIT_H */
  return FALSE;
#endif
}

void NetUpdate(void)
{
  static int lastmadetic;
  if (isExtraDDisplay)
    return;
  if (server) { // Receive network packets
    size_t recvlen;
    packet_header_t *packet = Z_Malloc(10000, PU_STATIC, NULL);
    while ((recvlen = I_GetPacket(packet, 10000))) {
      switch(packet->type) {
      case PKT_TICS:
  {
    uint8_t *p = (void*)(packet+1);
    int tics = *p++;
    unsigned long ptic = doom_ntohl(packet->tic);
    if (ptic > (unsigned)remotetic) { // Missed some
      packet_set(packet, PKT_RETRANS, remotetic);
      *(uint8_t*)(packet+1) = consoleplayer;
      I_SendPacket(packet, sizeof(*packet)+1);
    } else {
      if (ptic + tics <= (unsigned)remotetic) break; // Will not improve things
      remotetic = ptic;
      while (tics--) {
        int players = *p++;
        while (players--) {
            int n = *p++;
            RawToTic(&netcmds[n][remotetic%BACKUPTICS], p);
            p += sizeof(ticcmd_t);
        }
        remotetic++;
      }
    }
  }
  break;
      case PKT_RETRANS: // Resend request
          remotesend = doom_ntohl(packet->tic);
          break;
      case PKT_DOWN: // Server downed
  {
    int j;
    for (j=0; j<MAXPLAYERS; j++)
      if (j != consoleplayer) playeringame[j] = FALSE;
    server = FALSE;
    doom_printf("Server is down\nAll other players are no longer in the game\n");
  }
  break;
      case PKT_EXTRA: // Misc stuff
      case PKT_QUIT: // Player quit
  // Queue packet to be processed when its tic time is reached
  queuedpacket = Z_Realloc(queuedpacket, ++numqueuedpackets * sizeof *queuedpacket,
         PU_STATIC, NULL);
  queuedpacket[numqueuedpackets-1] = Z_Malloc(recvlen, PU_STATIC, NULL);
  memcpy(queuedpacket[numqueuedpackets-1], packet, recvlen);
  break;
      case PKT_BACKOFF:
        /* cph 2003-09-18 -
	 * The server sends this when we have got ahead of the other clients. We should
	 * stall the input side on this client, to allow other clients to catch up.
	 */
        lastmadetic++;
	break;
      default: // Other packet, unrecognised or redundant
  break;
      }
    }
    Z_Free(packet);
  }
  { // Build new ticcmds
    int newtics = I_GetTime() - lastmadetic;
    newtics = (newtics > 0 ? newtics : 0);
    lastmadetic += newtics;
    if (ffmap) newtics++;
    while (newtics--) {
      I_StartTic();
      if (maketic - gametic > BACKUPTICS/2) break;
      G_BuildTiccmd(&localcmds[maketic%BACKUPTICS]);
      maketic++;
    }
    if (server && maketic > remotesend) { // Send the tics to the server
      int sendtics;
      remotesend -= xtratics;
      if (remotesend < 0) remotesend = 0;
      sendtics = maketic - remotesend;
      {
  size_t pkt_size = sizeof(packet_header_t) + 2 + sendtics * sizeof(ticcmd_t);
  packet_header_t *packet = Z_Malloc(pkt_size, PU_STATIC, NULL);

  packet_set(packet, PKT_TICC, maketic - sendtics);
  *(uint8_t*)(packet+1) = sendtics;
  *(((uint8_t*)(packet+1))+1) = consoleplayer;
  {
    void *tic = ((uint8_t*)(packet+1)) +2;
    while (sendtics--) {
      TicToRaw(tic, &localcmds[remotesend++%BACKUPTICS]);
      tic = (uint8_t *)tic + sizeof(ticcmd_t);
    }
  }
  I_SendPacket(packet, pkt_size);
  Z_Free(packet);
      }
    }
  }
}

/* cph - data passed to this must be in the Doom (little-) endian */
void D_NetSendMisc(netmisctype_t type, size_t len, void* data)
{
  if (server) {
    size_t size = sizeof(packet_header_t) + 3*sizeof(int) + len;
    packet_header_t *packet = Z_Malloc(size, PU_STATIC, NULL);
    int *p = (void*)(packet+1);

    packet_set(packet, PKT_EXTRA, gametic);
    *p++ = LONG(type); *p++ = LONG(consoleplayer); *p++ = LONG(len);
    memcpy(p, data, len);
    I_SendPacket(packet, size);

    Z_Free(packet);
  }
}

static void CheckQueuedPackets(void)
{
  int i;
  for (i=0; (unsigned)i<numqueuedpackets; i++)
    if (doom_ntohl(queuedpacket[i]->tic) <= gametic)
      switch (queuedpacket[i]->type) {
      case PKT_QUIT: // Player quit the game
  {
    int pn = *(uint8_t*)(queuedpacket[i]+1);
    playeringame[pn] = FALSE;
    doom_printf("Player %d left the game\n", pn);
  }
  break;
      case PKT_EXTRA:
  {
    int *p = (int*)(queuedpacket[i]+1);
    size_t len = LONG(*(p+2));
    switch (LONG(*p)) {
    case nm_plcolour:
      G_ChangedPlayerColour(LONG(*(p+1)), LONG(*(p+3)));
      break;
    case nm_savegamename:
      if (len < SAVEDESCLEN) {
        memcpy(savedescription, p+3, len);
        // Force terminating 0 in case
        savedescription[len] = 0;
      }
      break;
    }
  }
  break;
      default: // Should not be queued
  break;
      }

  { // Requeue remaining packets
    int newnum = 0;
    packet_header_t **newqueue = NULL;

    for (i=0; (unsigned)i<numqueuedpackets; i++)
      if (doom_ntohl(queuedpacket[i]->tic) > gametic) {
  newqueue = Z_Realloc(newqueue, ++newnum * sizeof *newqueue,
           PU_STATIC, NULL);
  newqueue[newnum-1] = queuedpacket[i];
      } else Z_Free(queuedpacket[i]);

    Z_Free(queuedpacket);
    numqueuedpackets = newnum; queuedpacket = newqueue;
  }
}

void D_QuitNetGame (void)
{
  uint8_t buf[1 + sizeof(packet_header_t)];
  packet_header_t *packet = (void*)buf;
  int i;

  if (!server) return;
  buf[sizeof(packet_header_t)] = consoleplayer;
  packet_set(packet, PKT_QUIT, gametic);

  for (i=0; i<4; i++) {
    I_SendPacket(packet, 1 + sizeof(packet_header_t));
    I_uSleep(10000);
  }
}

void D_InitNetGame (void)
{
  int i;
  int numplayers = 1;

  i = M_CheckParm("-net");
  if (i && i < myargc-1) i++;

  if (!(netgame = server =  !!i)) {
    playeringame[consoleplayer = 0] = TRUE;
    // e6y
    // for play, recording or playback using "single-player coop" mode.
    // Equivalent to using prboom_server with -N 1
    netgame = M_CheckParm("-solo-net") || M_CheckParm("-net1");
  } else {
    // Get game info from server
    packet_header_t *packet = Z_Malloc(1000, PU_STATIC, NULL);
    struct setup_packet_s *sinfo = (void*)(packet+1);
  struct { packet_header_t head; short pn; } PACKEDATTR initpacket;

    I_InitNetwork();
  udp_socket = I_Socket(0);
  I_ConnectToServer(myargv[i]);

    do
    {
      do {
	// Send init packet
	initpacket.pn = doom_htons(wanted_player_number);
	packet_set(&initpacket.head, PKT_INIT, 0);
	I_SendPacket(&initpacket.head, sizeof(initpacket));
	I_WaitForPacket(5000);
      } while (!I_GetPacket(packet, 1000));
      if (packet->type == PKT_DOWN) I_Error("Server aborted the game");
    } while (packet->type != PKT_SETUP);

    // Get info from the setup packet
    consoleplayer = sinfo->yourplayer;
    compatibility_level = sinfo->complevel;
    G_Compatibility();
    startskill = sinfo->skill;
    deathmatch = sinfo->deathmatch;
    startmap = sinfo->level;
    startepisode = sinfo->episode;
    ticdup = sinfo->ticdup;
    xtratics = sinfo->extratic;
    G_ReadOptions(sinfo->game_options);

    lprintf(LO_INFO, "\tjoined game as player %d/%d; %d WADs specified\n",
      consoleplayer+1, numplayers = sinfo->players, sinfo->numwads);
    {
      char *p = sinfo->wadnames;
      int i = sinfo->numwads;

      while (i--) {
  D_AddFile(p, source_net);
  p += strlen(p) + 1;
      }
    }
    Z_Free(packet);
  }
  localcmds = netcmds[displayplayer = consoleplayer];
  for (i=0; i<numplayers; i++)
    playeringame[i] = TRUE;
  for (; i<MAXPLAYERS; i++)
    playeringame[i] = FALSE;
  if (!playeringame[consoleplayer]) I_Error("D_InitNetGame: consoleplayer not in game");
}

#if 0
void TryRunTics (void)
{
  int runtics;
  int entertime = I_GetTime();

  // Wait for tics to run
  while (1) {
    NetUpdate();
    runtics = (server ? remotetic : maketic) - gametic;
    if (!runtics) {
      if (!movement_smooth) {
        if (server)
          I_WaitForPacket(ms_to_next_tick);
        else
          I_uSleep(ms_to_next_tick*1000);
      }
      if (I_GetTime() - entertime > 10) {
        if (server) {
          char buf[sizeof(packet_header_t)+1];
          remotesend--;
          packet_set((packet_header_t *)buf, PKT_RETRANS, remotetic);
          buf[sizeof(buf)-1] = consoleplayer;
          I_SendPacket((packet_header_t *)buf, sizeof buf);
        }
        M_Ticker(); return;
      }
      {
        WasRenderedInTryRunTics = TRUE;
        if (movement_smooth && gamestate==wipegamestate)
        {
          isExtraDDisplay = TRUE;
          D_Display();
          isExtraDDisplay = FALSE;
        }
      }
    } else break;
  }

  while (runtics--) {
    if (server) CheckQueuedPackets();
    if (advancedemo)
      D_DoAdvanceDemo ();
    M_Ticker ();
    I_GetTime_SaveMS();
    G_Ticker ();
    gametic++;
    NetUpdate(); // Keep sending our tics to avoid stalling remote nodes
  }
}
#else
void TryRunTics(void) // Avoid sleeping/timer crap, just run it. (Themaister)
{
   int runtics = maketic - gametic;

   while (runtics--)
   {
      if (advancedemo)
         D_DoAdvanceDemo ();
      M_Ticker ();
      G_Ticker ();
      gametic++;
   }
}
#endif


#else
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

#endif
