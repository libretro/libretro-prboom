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
 *      Created by a sound utility.
 *      Kept as a sample, DOOM2 sounds.
 *
 *-----------------------------------------------------------------------------*/

// killough 5/3/98: reformatted

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "doomtype.h"
#include "sounds.h"

//
// Information about all the music
//

musicinfo_t S_music[] = {
  { 0 },
  { "e1m1", 0 },
  { "e1m2", 0 },
  { "e1m3", 0 },
  { "e1m4", 0 },
  { "e1m5", 0 },
  { "e1m6", 0 },
  { "e1m7", 0 },
  { "e1m8", 0 },
  { "e1m9", 0 },
  { "e2m1", 0 },
  { "e2m2", 0 },
  { "e2m3", 0 },
  { "e2m4", 0 },
  { "e2m5", 0 },
  { "e2m6", 0 },
  { "e2m7", 0 },
  { "e2m8", 0 },
  { "e2m9", 0 },
  { "e3m1", 0 },
  { "e3m2", 0 },
  { "e3m3", 0 },
  { "e3m4", 0 },
  { "e3m5", 0 },
  { "e3m6", 0 },
  { "e3m7", 0 },
  { "e3m8", 0 },
  { "e3m9", 0 },
  { "inter", 0 },
  { "intro", 0 },
  { "bunny", 0 },
  { "victor", 0 },
  { "introa", 0 },
  { "runnin", 0 },
  { "stalks", 0 },
  { "countd", 0 },
  { "betwee", 0 },
  { "doom", 0 },
  { "the_da", 0 },
  { "shawn", 0 },
  { "ddtblu", 0 },
  { "in_cit", 0 },
  { "dead", 0 },
  { "stlks2", 0 },
  { "theda2", 0 },
  { "doom2", 0 },
  { "ddtbl2", 0 },
  { "runni2", 0 },
  { "dead2", 0 },
  { "stlks3", 0 },
  { "romero", 0 },
  { "shawn2", 0 },
  { "messag", 0 },
  { "count2", 0 },
  { "ddtbl3", 0 },
  { "ampie", 0 },
  { "theda3", 0 },
  { "adrian", 0 },
  { "messg2", 0 },
  { "romer2", 0 },
  { "tense", 0 },
  { "shawn3", 0 },
  { "openin", 0 },
  { "evil", 0 },
  { "ultima", 0 },
  { "read_m", 0 },
  { "dm2ttl", 0 },
  { "dm2int", 0 },
};


//
// Information about all the sfx
//

sfxinfo_t S_sfx[] = {
  // S_sfx[0] needs to be a dummy for odd reasons.
  { "none", FALSE,  0, 0, -1, -1, 0 },

  { "pistol", FALSE, 64, 0, -1, -1, 0 },
  { "shotgn", FALSE, 64, 0, -1, -1, 0 },
  { "sgcock", FALSE, 64, 0, -1, -1, 0 },
  { "dshtgn", FALSE, 64, 0, -1, -1, 0 },
  { "dbopn", FALSE, 64, 0, -1, -1, 0 },
  { "dbcls", FALSE, 64, 0, -1, -1, 0 },
  { "dbload", FALSE, 64, 0, -1, -1, 0 },
  { "plasma", FALSE, 64, 0, -1, -1, 0 },
  { "bfg", FALSE, 64, 0, -1, -1, 0 },
  { "sawup", FALSE, 64, 0, -1, -1, 0 },
  { "sawidl", FALSE, 118, 0, -1, -1, 0 },
  { "sawful", FALSE, 64, 0, -1, -1, 0 },
  { "sawhit", FALSE, 64, 0, -1, -1, 0 },
  { "rlaunc", FALSE, 64, 0, -1, -1, 0 },
  { "rxplod", FALSE, 70, 0, -1, -1, 0 },
  { "firsht", FALSE, 70, 0, -1, -1, 0 },
  { "firxpl", FALSE, 70, 0, -1, -1, 0 },
  { "pstart", FALSE, 100, 0, -1, -1, 0 },
  { "pstop", FALSE, 100, 0, -1, -1, 0 },
  { "doropn", FALSE, 100, 0, -1, -1, 0 },
  { "dorcls", FALSE, 100, 0, -1, -1, 0 },
  { "stnmov", FALSE, 119, 0, -1, -1, 0 },
  { "swtchn", FALSE, 78, 0, -1, -1, 0 },
  { "swtchx", FALSE, 78, 0, -1, -1, 0 },
  { "plpain", FALSE, 96, 0, -1, -1, 0 },
  { "dmpain", FALSE, 96, 0, -1, -1, 0 },
  { "popain", FALSE, 96, 0, -1, -1, 0 },
  { "vipain", FALSE, 96, 0, -1, -1, 0 },
  { "mnpain", FALSE, 96, 0, -1, -1, 0 },
  { "pepain", FALSE, 96, 0, -1, -1, 0 },
  { "slop", FALSE, 78, 0, -1, -1, 0 },
  { "itemup", TRUE, 78, 0, -1, -1, 0 },
  { "wpnup", TRUE, 78, 0, -1, -1, 0 },
  { "oof", FALSE, 96, 0, -1, -1, 0 },
  { "telept", FALSE, 32, 0, -1, -1, 0 },
  { "posit1", TRUE, 98, 0, -1, -1, 0 },
  { "posit2", TRUE, 98, 0, -1, -1, 0 },
  { "posit3", TRUE, 98, 0, -1, -1, 0 },
  { "bgsit1", TRUE, 98, 0, -1, -1, 0 },
  { "bgsit2", TRUE, 98, 0, -1, -1, 0 },
  { "sgtsit", TRUE, 98, 0, -1, -1, 0 },
  { "cacsit", TRUE, 98, 0, -1, -1, 0 },
  { "brssit", TRUE, 94, 0, -1, -1, 0 },
  { "cybsit", TRUE, 92, 0, -1, -1, 0 },
  { "spisit", TRUE, 90, 0, -1, -1, 0 },
  { "bspsit", TRUE, 90, 0, -1, -1, 0 },
  { "kntsit", TRUE, 90, 0, -1, -1, 0 },
  { "vilsit", TRUE, 90, 0, -1, -1, 0 },
  { "mansit", TRUE, 90, 0, -1, -1, 0 },
  { "pesit", TRUE, 90, 0, -1, -1, 0 },
  { "sklatk", FALSE, 70, 0, -1, -1, 0 },
  { "sgtatk", FALSE, 70, 0, -1, -1, 0 },
  { "skepch", FALSE, 70, 0, -1, -1, 0 },
  { "vilatk", FALSE, 70, 0, -1, -1, 0 },
  { "claw", FALSE, 70, 0, -1, -1, 0 },
  { "skeswg", FALSE, 70, 0, -1, -1, 0 },
  { "pldeth", FALSE, 32, 0, -1, -1, 0 },
  { "pdiehi", FALSE, 32, 0, -1, -1, 0 },
  { "podth1", FALSE, 70, 0, -1, -1, 0 },
  { "podth2", FALSE, 70, 0, -1, -1, 0 },
  { "podth3", FALSE, 70, 0, -1, -1, 0 },
  { "bgdth1", FALSE, 70, 0, -1, -1, 0 },
  { "bgdth2", FALSE, 70, 0, -1, -1, 0 },
  { "sgtdth", FALSE, 70, 0, -1, -1, 0 },
  { "cacdth", FALSE, 70, 0, -1, -1, 0 },
  { "skldth", FALSE, 70, 0, -1, -1, 0 },
  { "brsdth", FALSE, 32, 0, -1, -1, 0 },
  { "cybdth", FALSE, 32, 0, -1, -1, 0 },
  { "spidth", FALSE, 32, 0, -1, -1, 0 },
  { "bspdth", FALSE, 32, 0, -1, -1, 0 },
  { "vildth", FALSE, 32, 0, -1, -1, 0 },
  { "kntdth", FALSE, 32, 0, -1, -1, 0 },
  { "pedth", FALSE, 32, 0, -1, -1, 0 },
  { "skedth", FALSE, 32, 0, -1, -1, 0 },
  { "posact", TRUE, 120, 0, -1, -1, 0 },
  { "bgact", TRUE, 120, 0, -1, -1, 0 },
  { "dmact", TRUE, 120, 0, -1, -1, 0 },
  { "bspact", TRUE, 100, 0, -1, -1, 0 },
  { "bspwlk", TRUE, 100, 0, -1, -1, 0 },
  { "vilact", TRUE, 100, 0, -1, -1, 0 },
  { "noway", FALSE, 78, 0, -1, -1, 0 },
  { "barexp", FALSE, 60, 0, -1, -1, 0 },
  { "punch", FALSE, 64, 0, -1, -1, 0 },
  { "hoof", FALSE, 70, 0, -1, -1, 0 },
  { "metal", FALSE, 70, 0, -1, -1, 0 },
  { "chgun", FALSE, 64, &S_sfx[sfx_pistol], 150, 0, 0 },
  { "tink", FALSE, 60, 0, -1, -1, 0 },
  { "bdopn", FALSE, 100, 0, -1, -1, 0 },
  { "bdcls", FALSE, 100, 0, -1, -1, 0 },
  { "itmbk", FALSE, 100, 0, -1, -1, 0 },
  { "flame", FALSE, 32, 0, -1, -1, 0 },
  { "flamst", FALSE, 32, 0, -1, -1, 0 },
  { "getpow", FALSE, 60, 0, -1, -1, 0 },
  { "bospit", FALSE, 70, 0, -1, -1, 0 },
  { "boscub", FALSE, 70, 0, -1, -1, 0 },
  { "bossit", FALSE, 70, 0, -1, -1, 0 },
  { "bospn", FALSE, 70, 0, -1, -1, 0 },
  { "bosdth", FALSE, 70, 0, -1, -1, 0 },
  { "manatk", FALSE, 70, 0, -1, -1, 0 },
  { "mandth", FALSE, 70, 0, -1, -1, 0 },
  { "sssit", FALSE, 70, 0, -1, -1, 0 },
  { "ssdth", FALSE, 70, 0, -1, -1, 0 },
  { "keenpn", FALSE, 70, 0, -1, -1, 0 },
  { "keendt", FALSE, 70, 0, -1, -1, 0 },
  { "skeact", FALSE, 70, 0, -1, -1, 0 },
  { "skesit", FALSE, 70, 0, -1, -1, 0 },
  { "skeatk", FALSE, 70, 0, -1, -1, 0 },
  { "radio", FALSE, 60, 0, -1, -1, 0 },

};
