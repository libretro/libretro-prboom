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
 * DESCRIPTION:  Platform-independent sound code
 *
 *-----------------------------------------------------------------------------*/

// killough 3/7/98: modified to allow arbitrary listeners in spy mode
// killough 5/2/98: reindented, removed useless code, beautified

#include "config.h"

#include "doomstat.h"
#include "s_sound.h"
#include "i_sound.h"
#include "i_system.h"
#include "d_main.h"
#include "r_main.h"
#include "m_random.h"
#include "w_wad.h"
#include "lprintf.h"
#include "dsda_hacked.h"

// when to clip out sounds
// Does not fit the large outdoor areas.
#define S_CLIPPING_DIST (1200<<FRACBITS)

// Distance tp origin when sounds should be maxed out.
// This should relate to movement clipping resolution
// (see BLOCKMAP handling).
// Originally: (200*0x10000).

#define S_CLOSE_DIST (160<<FRACBITS)
#define S_ATTENUATOR ((S_CLIPPING_DIST-S_CLOSE_DIST)>>FRACBITS)

// Adjustable by menu.
#define NORM_PITCH 128
#define NORM_PRIORITY 64
#define NORM_SEP 128
#define S_STEREO_SWING (96<<FRACBITS)

const char* S_music_files[NUMMUSIC]; // cournia - stores music file names
extern int mus_load_external; // value for the "Load external MP3 files" setting

typedef struct
{
  sfxinfo_t *sfxinfo;  // sound information (if null, channel avail.)
  void *origin;        // origin of sound
  int handle;          // handle of the sound being played
  int is_pickup;       // killough 4/25/98: whether sound is a player's weapon
} channel_t;

// the set of channels available
static channel_t *channels;

// These are not used, but should be (menu).
// Maximum volume of a sound effect.
// Internal default is max out of 0-15.
int snd_SfxVolume = 15;

// Maximum volume of music. Useless so far.
int snd_MusicVolume = 15;

// whether songs are mus_paused
static dbool   mus_paused;

// music currently being played
static musicinfo_t *mus_playing;

/* looping flag for mus_playing -- captured from the most recent
 * S_ChangeMusic / S_ChangeMusicByName / S_StartMusic call so that
 * S_RestartMusic can re-issue I_PlaySong with the correct value. */
static int mus_playing_looping;

// following is set
//  by the defaults code in M_misc:
// number of channels available
int default_numChannels;
int numChannels;

//jff 3/17/98 to keep track of last IDMUS specified music num
int idmusnum;

//
// Internals.
//

void S_StopChannel(int cnum);

// Will start a sound at a given volume.
static void S_StartSoundAtVolume(degenmobj_t *origin, int sound_id, int volume);

int S_AdjustSoundParams(mobj_t *listener, degenmobj_t *source,
                        int *vol, int *sep, int *pitch);

static int S_getChannel(void *origin, sfxinfo_t *sfxinfo, int is_pickup);

// Initializes sound stuff, including volume
// Sets channels, SFX and music volume,
//  allocates channel buffer, sets S_sfx lookup.
//

/* Per-map music lump names from the Hexen SNDINFO "$MAP n song" lines,
 * indexed by map number (1..98). */
static char hexen_map_song[99][16];

const char *S_HexenMapSong(int map)
{
  if (map < 1 || map > 98 || !hexen_map_song[map][0])
    return NULL;
  return hexen_map_song[map];
}

/* Hexen sounds are indirected through the SNDINFO lump: the S_sfx table
 * ships with each entry's *logical* name (e.g. "PlayerFighterGrunt"), and
 * SNDINFO maps that to the actual lump ("fgtgrunt").  Doom and Heretic name
 * their sfx lumps directly, so this step is Hexen-only.  Rewrite each
 * S_sfx[i].name from its logical tag to the mapped lump so the normal
 * I_GetSfxLumpNum lookup resolves it.  Lines are "<tag> <lump>"; the "$MAP n
 * song" directive records per-map music; other '$' lines and ';' comments
 * are skipped. */
void S_HexenLoadSndInfo(void)
{
  int         lump, len, i;
  const char *buf;
  char        tag[64], lmp[64];

  memset(hexen_map_song, 0, sizeof(hexen_map_song));

  lump = (W_CheckNumForName)("SNDINFO", ns_global);
  if (lump < 0)
    return;
  len = W_LumpLength(lump);
  buf = (const char *)W_CacheLumpNum(lump);
  if (!buf || len <= 0)
    return;

  i = 0;
  while (i < len)
  {
    int t = 0, l = 0;

    /* skip whitespace */
    while (i < len && (buf[i] == ' ' || buf[i] == '\t' ||
                       buf[i] == '\r' || buf[i] == '\n'))
      i++;
    if (i >= len)
      break;

    /* comment line */
    if (buf[i] == ';')
    {
      while (i < len && buf[i] != '\n')
        i++;
      continue;
    }

    /* first token */
    while (i < len && buf[i] != ' ' && buf[i] != '\t' &&
           buf[i] != '\r' && buf[i] != '\n' && t < (int)sizeof(tag) - 1)
      tag[t++] = buf[i++];
    tag[t] = '\0';

    /* '$' directive.  "$MAP n song" carries the per-map music lump; other
     * directives ($ARCHIVEPATH, ...) are ignored. */
    if (tag[0] == '$')
    {
      if (!strcasecmp(tag, "$MAP"))
      {
        char nbuf[16], sbuf[16];
        int  nn = 0, ss = 0, mapnum;
        while (i < len && (buf[i] == ' ' || buf[i] == '\t')) i++;
        while (i < len && buf[i] != ' ' && buf[i] != '\t' &&
               buf[i] != '\r' && buf[i] != '\n' && nn < (int)sizeof(nbuf)-1)
          nbuf[nn++] = buf[i++];
        nbuf[nn] = '\0';
        while (i < len && (buf[i] == ' ' || buf[i] == '\t')) i++;
        while (i < len && buf[i] != ' ' && buf[i] != '\t' &&
               buf[i] != '\r' && buf[i] != '\n' && ss < (int)sizeof(sbuf)-1)
          sbuf[ss++] = buf[i++];
        sbuf[ss] = '\0';
        mapnum = atoi(nbuf);
        if (mapnum >= 1 && mapnum <= 98 && sbuf[0])
        {
          size_t cl = strlen(sbuf);
          if (cl > sizeof(hexen_map_song[0]) - 1)
            cl = sizeof(hexen_map_song[0]) - 1;
          memcpy(hexen_map_song[mapnum], sbuf, cl);
          hexen_map_song[mapnum][cl] = '\0';
        }
      }
      while (i < len && buf[i] != '\n')
        i++;
      continue;
    }

    /* skip the gap to the second token */
    while (i < len && (buf[i] == ' ' || buf[i] == '\t'))
      i++;
    /* second token (the lump name) */
    while (i < len && buf[i] != ' ' && buf[i] != '\t' &&
           buf[i] != '\r' && buf[i] != '\n' && l < (int)sizeof(lmp) - 1)
      lmp[l++] = buf[i++];
    lmp[l] = '\0';

    if (!t || !l)
      continue;

    /* '?' means "use the default sound"; leave the entry as-is. */
    if (lmp[0] == '?')
      continue;

    /* match the tag against a sfx entry's logical name and repoint it. */
    {
      int s;
      for (s = 1; s < num_sfx; s++)
        if (S_sfx[s].name && !strcasecmp(S_sfx[s].name, tag))
        {
          S_sfx[s].name = strdup(lmp);
          break;
        }
    }
  }

  W_UnlockLumpNum(lump);
}

void S_Init(int sfxVolume, int musicVolume)
{
  //jff 1/22/98 skip sound init if sound not enabled
  numChannels = default_numChannels;
  if (!nosfxparm)
  {
    int i;

    lprintf(LO_CONFIRM, "S_Init: default sfx volume %d\n", sfxVolume);

    // Whatever these did with DMX, these are rather dummies now.
    I_SetChannels();

    S_SetSfxVolume(sfxVolume);

    // Allocating the internal channels for mixing
    // (the maximum numer of sounds rendered
    // simultaneously) within zone memory.
    // CPhipps - calloc
    channels =
      (channel_t *) calloc(numChannels,sizeof(channel_t));

    // Note that sounds have not been cached (yet).
    // DSDHacked: cover the runtime-grown table, not just the static seed.
    for (i=1 ; i<num_sfx ; i++)
      S_sfx[i].lumpnum = S_sfx[i].usefulness = -1;
  }

  // CPhipps - music init reformatted
  if (!nomusicparm) {
    S_SetMusicVolume(musicVolume);

    // no sounds are playing, and they are not mus_paused
    mus_paused = 0;
  }
}

/* S_Shutdown
 *
 * Tears down the per-session sound state established by S_Init.
 * Called from D_DoomDeinit.
 *
 *  - S_StopMusic: stops the current track, unlocks the music lump,
 *    and clears mus_playing (issue #53 from the audit -- mus_playing
 *    used to carry across sessions).
 *  - S_Stop: stops every active sound channel.
 *  - free(channels) + numChannels=0: releases the calloc'd channel
 *    buffer.  S_Init calloc'd this every call without freeing the
 *    previous allocation; the leak was numChannels * sizeof(channel_t)
 *    per content load.
 *  - Zero S_music[i].lumpnum: S_ChangeMusic caches lump numbers in
 *    S_music[i].lumpnum on first lookup ("if (!music->lumpnum)").
 *    Without invalidating the cache, a WAD swap to content with
 *    different music lumps would replay the previous WAD's lumpnums,
 *    yielding wrong music or out-of-range lookups.
 *
 * Safe to call when S_Init wasn't called (channels==NULL): the
 * NULL-guarded S_Stop loop short-circuits and free(NULL) is a no-op.
 */
void S_Shutdown(void)
{
   int i;

   if (!nomusicparm)
      S_StopMusic();

   if (!nosfxparm && channels)
      S_Stop();

   free(channels);
   channels = NULL;
   numChannels = 0;

   /* Invalidate cached music lump numbers so the next session
    * re-resolves them against whatever WAD is now loaded. */
   for (i = 0; i < NUMMUSIC; i++)
      S_music[i].lumpnum = 0;
}

void S_Stop(void)
{
  int cnum;

  //jff 1/22/98 skip sound init if sound not enabled
  if (!nosfxparm)
    for (cnum=0 ; cnum<numChannels ; cnum++)
      if (channels[cnum].sfxinfo)
        S_StopChannel(cnum);
}

//
// Per level startup code.
// Kills playing sounds at start of level,
//  determines music if any, changes music.
//
void S_Start(void)
{
  int mnum;

  // kill all playing sounds at start of level
  //  (trust me - a good idea)

  S_Stop();

  //jff 1/22/98 return if music is not enabled
  if (nomusicparm)
    return;

  // start new music for the level
  mus_paused = 0;

  if (gamemapinfo && gamemapinfo->music[0])
  {
    S_ChangeMusicByName(gamemapinfo->music, TRUE);
    return;
  }

  /* Hexen sets the per-map music by lump name through SNDINFO ("$MAP n
   * song"), which S_HexenLoadSndInfo recorded; the Doom episode/commercial
   * music numbering below does not apply. */
  if (hexen)
  {
    const char *song = S_HexenMapSong(gamemap);
    if (song)
      S_ChangeMusicByName((char *)song, TRUE);
    return;
  }

  if (idmusnum!=-1)
    mnum = idmusnum; //jff 3/17/98 reload IDMUS music if not -1
  else
    if (gamemode == commercial)
      mnum = mus_runnin + gamemap - 1;
    else
      mnum = mus_e1m1 + (gameepisode-1)*9 + gamemap-1;

  S_ChangeMusic(mnum, TRUE);
}

static void S_StartSoundAtVolume(degenmobj_t *origin, int sfx_id, int volume)
{
  int sep, pitch, priority, cnum, is_pickup;
  sfxinfo_t *sfx;

  //jff 1/22/98 return if sound is not enabled
  if (nosfxparm)
    return;

  is_pickup = sfx_id & PICKUP_SOUND || sfx_id == sfx_oof || (compatibility_level >= prboom_2_compatibility && sfx_id == sfx_noway); // killough 4/25/98
  sfx_id &= ~PICKUP_SOUND;

  // check for bogus sound #
  // DSDHacked: valid ids are 1..num_sfx-1 against the runtime-grown S_sfx
  // table, not the static NUMSFX seed.  I_Error is non-fatal in this build
  // (it logs and returns), so we must also bail out explicitly -- otherwise
  // execution falls through into the out-of-bounds S_sfx[sfx_id] below.
  //
  // In Raven games (Heretic/Hexen) sfx id 0 is the "None" sentinel: a number
  // of sound-origin and sequence code paths legitimately ask to play "no
  // sound".  Treat that as a quiet no-op rather than an error, which is what
  // those games expect; only genuinely out-of-range ids are reported.
  if (raven && sfx_id == 0)
    return;
  if (sfx_id < 1 || sfx_id >= num_sfx)
  {
    I_Error("S_StartSoundAtVolume: Bad sfx #: %d", sfx_id);
    return;
  }

  sfx = &S_sfx[sfx_id];

  // Initialize sound parameters
  if (sfx->link)
    {
      pitch = sfx->pitch;
      priority = sfx->priority;
      volume += sfx->volume;

      if (volume < 1)
        return;

      if (volume > snd_SfxVolume)
        volume = snd_SfxVolume;
    }
  else
    {
      pitch = NORM_PITCH;
      priority = NORM_PRIORITY;
    }

  // Check to see if it is audible, modify the params
  // killough 3/7/98, 4/25/98: code rearranged slightly

  if (!origin || origin == (degenmobj_t*)players[displayplayer].mo) {
    sep = NORM_SEP;
    volume *= 8;
  } else
    if (!S_AdjustSoundParams(players[displayplayer].mo, origin, &volume,
                             &sep, &pitch))
      return;
    else
      if ( origin->x == players[displayplayer].mo->x &&
           origin->y == players[displayplayer].mo->y)
        sep = NORM_SEP;

  /* hacks to vary the sfx pitches */
  if (raven)
  {
    /* Heretic applies a small symmetric pitch jitter to every sound;
     * Hexen only to sounds whose table entry asks for it (the pitch
     * field carries vanilla's changePitch).  Both consume two RNG
     * values per jittered sound (NORM_PITCH +/- up to 7).  The Doom
     * saw-pitch range check below is meaningless for raven sfx ids
     * and would consume the RNG on a different schedule. */
    if (heretic || sfx->pitch > 0)
      pitch = NORM_PITCH + (M_Random() & 7) - (M_Random() & 7);
    else
      pitch = NORM_PITCH;
  }
  else if (sfx_id >= sfx_sawup && sfx_id <= sfx_sawhit)
    pitch += 8 - (M_Random()&15);
  else
    if (sfx_id != sfx_itemup && sfx_id != sfx_tink)
      pitch += 16 - (M_Random()&31);

  if (pitch<0)
    pitch = 0;

  if (pitch>255)
    pitch = 255;

  // kill old sound
  for (cnum=0 ; cnum<numChannels ; cnum++)
    if (channels[cnum].sfxinfo && channels[cnum].origin == origin &&
        (comp[comp_sound] || channels[cnum].is_pickup == is_pickup))
      {
        S_StopChannel(cnum);
        break;
      }

  // try to find a channel
  cnum = S_getChannel(origin, sfx, is_pickup);

  if (cnum<0)
    return;

  // get lumpnum if necessary
  // killough 2/28/98: make missing sounds non-fatal
  if (sfx->lumpnum < 0 && (sfx->lumpnum = I_GetSfxLumpNum(sfx)) < 0)
    return;

  // increase the usefulness
  if (sfx->usefulness++ < 0)
    sfx->usefulness = 1;

  // Assigns the handle to one of the channels in the mix/output buffer.
  { // e6y: [Fix] Crash with zero-length sounds.
    int h = I_StartSound(sfx_id, cnum, volume, sep, pitch, priority);
    if (h != -1) channels[cnum].handle = h;
  }
}

void S_StartSound(void *origin, int sfx_id)
{
  S_StartSoundAtVolume(origin, sfx_id, snd_SfxVolume);
}

/* Heretic ambient sound sequences play at a script-chosen volume rather
 * than the global sfx volume. */
void S_StartAmbientSound(void *origin, int sfx_id, int volume)
{
  if (sfx_id == heretic_sfx_None || volume <= 0)
    return;
  S_StartSoundAtVolume((degenmobj_t *)origin, sfx_id, volume);
}

/* Hexen sound sequences need to know whether a particular sound is still
 * playing on a given origin, to chain or repeat sequence steps. */
dbool S_GetSoundPlayingInfo(void *origin, int sound_id)
{
  int        cnum;
  sfxinfo_t *sfx;

  if (nosfxparm)
    return false;

  sfx = &S_sfx[sound_id];
  for (cnum = 0; cnum < numChannels; cnum++)
  {
    channel_t *c = &channels[cnum];
    if (c->sfxinfo == sfx && c->origin == origin)
      if (I_SoundIsPlaying(c->handle))
        return true;
  }
  return false;
}

void S_StopSound(void *origin)
{
  int cnum;

  //jff 1/22/98 return if sound is not enabled
  if (nosfxparm)
    return;

  for (cnum=0 ; cnum<numChannels ; cnum++)
    if (channels[cnum].sfxinfo && channels[cnum].origin == origin)
      {
        S_StopChannel(cnum);
        break;
      }
}


//
// Stop and resume music, during game PAUSE.
//
void S_PauseSound(void)
{
  //jff 1/22/98 return if music is not enabled
  if (nomusicparm)
    return;

  if (mus_playing && !mus_paused)
    {
      I_PauseSong(mus_playing->handle);
      mus_paused = TRUE;
    }
}

void S_ResumeSound(void)
{
  //jff 1/22/98 return if music is not enabled
  if (nomusicparm)
    return;

  if (mus_playing && mus_paused)
    {
      I_ResumeSong(mus_playing->handle);
      mus_paused = FALSE;
    }
}


//
// Updates music & sounds
//
void S_UpdateSounds(void* listener_p)
{
  mobj_t *listener = (mobj_t*) listener_p;
  int cnum;

  //jff 1/22/98 return if sound is not enabled
  if (nosfxparm)
    return;

#ifdef UPDATE_MUSIC
  I_UpdateMusic();
#endif

  for (cnum=0 ; cnum<numChannels ; cnum++)
    {
      sfxinfo_t *sfx;
      channel_t *c = &channels[cnum];
      if ((sfx = c->sfxinfo))
        {
          if (I_SoundIsPlaying(c->handle))
            {
              // initialize parameters
              int volume = snd_SfxVolume;
              int pitch = NORM_PITCH;
              int sep = NORM_SEP;

              if (sfx->link)
                {
                  pitch = sfx->pitch;
                  volume += sfx->volume;
                  if (volume < 1)
                    {
                      S_StopChannel(cnum);
                      continue;
                    }
                  else
                    if (volume > snd_SfxVolume)
                      volume = snd_SfxVolume;
                }

              // check non-local sounds for distance clipping
              // or modify their params
              if (c->origin && listener_p != c->origin) { // killough 3/20/98
                if (!S_AdjustSoundParams(listener, c->origin,
                                         &volume, &sep, &pitch))
                  S_StopChannel(cnum);
                else
                  I_UpdateSoundParams(c->handle, volume, sep, pitch);
        }
            }
          else   // if channel is allocated but sound has stopped, free it
            S_StopChannel(cnum);
        }
    }
}



void S_SetMusicVolume(int volume)
{
  //jff 1/22/98 return if music is not enabled
  if (nomusicparm)
    return;
  if (volume < 0 || volume > 15)
    I_Error("S_SetMusicVolume: Attempt to set music volume at %d", volume);
  I_SetMusicVolume(volume);
  snd_MusicVolume = volume;
}



void S_SetSfxVolume(int volume)
{
  //jff 1/22/98 return if sound is not enabled
  if (nosfxparm)
    return;
  if (volume < 0 || volume > 127)
    I_Error("S_SetSfxVolume: Attempt to set sfx volume at %d", volume);
  snd_SfxVolume = volume;
}



// Starts some music with the music id found in sounds.h.
//
void S_StartMusic(int m_id)
{
  //jff 1/22/98 return if music is not enabled
  if (nomusicparm)
    return;
  S_ChangeMusic(m_id, FALSE);
}



void S_ChangeMusic(int musicnum, int looping)
{
  musicinfo_t *music;
  int music_file_failed; // cournia - if TRUE load the default MIDI music
  char* music_filename;  // cournia

  //jff 1/22/98 return if music is not enabled
  if (nomusicparm)
    return;

  /* DSDHacked: bound against the runtime-grown S_music table. */
  if (musicnum <= mus_None || musicnum >= num_music)
  {
    I_Error("S_ChangeMusic: Bad music number %d", musicnum);
    return;
  }

  music = &S_music[musicnum];

  if (mus_playing == music)
    return;

  // shutdown old music
  S_StopMusic();

  // get lumpnum if neccessary
  if (!music->lumpnum)
  {
    char namebuf[9];
    /* Doom music lumps are D_<name> (e.g. D_E1M1); Heretic uses MUS_<name>
     * (e.g. MUS_E1M1). */
    if (heretic)
    {
      /* The music table is Doom-shaped, so the non-level slots carry Doom
       * names (intro/inter/...).  Heretic's title, intermission and finale
       * tracks live under different lump names, so remap those few slots to
       * the Heretic lumps; ordinary level music (e1m1...) passes through. */
      const char *hname = music->name;
      if (!strcmp(hname, "intro") || !strcmp(hname, "dm2ttl"))
        hname = "titl";                 /* title screen   -> MUS_TITL */
      else if (!strcmp(hname, "inter") || !strcmp(hname, "dm2int"))
        hname = "intr";                 /* intermission   -> MUS_INTR */
      else if (!strcmp(hname, "victor") || !strcmp(hname, "read_m"))
        hname = "cptd";                 /* finale/ending  -> MUS_CPTD */
      snprintf(namebuf, sizeof(namebuf), "MUS_%s", hname);
    }
    else if (hexen)
    {
      /* Hexen's level music is set by lump name through SNDINFO (see
       * S_Start); only the non-level slots reach S_ChangeMusic by number.
       * Those Doom-shaped slot names map to Hexen's bare music lumps. */
      const char *hname = music->name;
      if (!strcmp(hname, "intro") || !strcmp(hname, "dm2ttl"))
        hname = "HEXEN";                /* title screen */
      else if (!strcmp(hname, "inter") || !strcmp(hname, "dm2int"))
        hname = "HUB";                  /* hub / intermission */
      else if (!strcmp(hname, "victor"))
        hname = "ORB";                  /* victory */
      else if (!strcmp(hname, "read_m"))
        hname = "HALL";                 /* finale */
      snprintf(namebuf, sizeof(namebuf), "%s", hname);
    }
    else
      snprintf(namebuf, sizeof(namebuf), "d_%s", music->name);
    music->lumpnum = W_CheckNumForName(namebuf);
  }
  if (music->lumpnum < 0) {
    I_Error("S_ChangeMusic: No valid music lump");
    return;
  }
  music_file_failed = 1;

  // Look for external music files (eg. mp3) according to the settings
  // 0 = never load external music files, 1 = always load it, 2 = only from iwads
  if ((mus_load_external == 2 && lumpinfo[music->lumpnum].source == source_iwad)
     || (mus_load_external == 1))
  {
    // cournia - check to see if we can play a higher quality music file
    //           rather than the default MIDI
    music_filename = I_FindFile(S_music_files[musicnum], NULL);
    if (music_filename)
    {
      lprintf(LO_INFO, "S_ChangeMusic: playing %s from file '%s'\n",
                       music->name, music_filename);
      music_file_failed = I_RegisterMusicFile(music_filename, music);
      free(music_filename);
    }
  }

  if (music_file_failed)
  {
    //cournia - could not load music file, play default MIDI music
    // load & register it
    lprintf(LO_INFO, "S_ChangeMusic: playing '%s'\n", music->name);
    music->data = W_CacheLumpNum(music->lumpnum);
    music->handle = I_RegisterSong(music->data, W_LumpLength(music->lumpnum));
  }

  // play it
  I_PlaySong(music->handle, looping);

  mus_playing = music;
  mus_playing_looping = looping;
}

void S_ChangeMusicByName(char* lumpname, int looping)
{
  if (nomusicparm)
    return;

  {
    // First find if the provided lump is in the list of customizable music
    // and can be played with S_ChangeMusic
    int i;
    char *musicname = lumpname+2; // skip  first 2 chars ("D_" prefix)
    for (i=1; i<NUMMUSIC; i++)
      if (!strncasecmp(musicname, S_music[i].name, 6))
      {
        S_ChangeMusic(i, looping);
        return;
      }
  }
  {
    // If the lump name does not correspond to any known music
    // attempt to play it as custom music (last in S_music array)

    musicinfo_t *music = &S_music[NUMMUSIC];
    int lumpnum = W_CheckNumForName(lumpname);

    if (lumpnum <= -1)
    {
       I_Error("S_ChangeMusicByName: invalid lump name '%s'", lumpname);
       return;
    }
    if(mus_playing && mus_playing->lumpnum == lumpnum)
      return;

    // shutdown old music
    S_StopMusic();

    // save lumpnum
    music->lumpnum = lumpnum;

    // load & register it
    music->data = W_CacheLumpNum(music->lumpnum);
    if (!music->data)
    {
      I_Error("S_ChangeMusicByName: invalid music lump '%s'", lumpname);
      return;
    }

    lprintf(LO_INFO, "S_ChangeMusicByName: playing '%s'\n", lumpname);
    music->handle = I_RegisterSong(music->data, W_LumpLength(music->lumpnum));
    I_PlaySong(music->handle, looping);

    mus_playing = music;
    mus_playing_looping = looping;
  }
}

void S_StopMusic(void)
{
  //jff 1/22/98 return if music is not enabled
  if (nomusicparm)
    return;

  if (mus_playing)
    {
      if (mus_paused)
        I_ResumeSong(mus_playing->handle);

      I_StopSong(mus_playing->handle);
      I_UnRegisterSong(mus_playing->handle);
      if (mus_playing->lumpnum >= 0)
  W_UnlockLumpNum(mus_playing->lumpnum); // cph - release the music data

      mus_playing->data = 0;
      mus_playing = 0;
      mus_playing_looping = 0;
    }
}

/* Re-registers and re-plays the currently playing track without
 * advancing it.  Used by the "MIDI Hardware" menu callback so a
 * change between Off / Adlib / Fluidsynth takes effect immediately
 * instead of waiting for the next S_ChangeMusic.
 *
 * Skipped when:
 *   - nothing is playing (mus_playing == NULL),
 *   - the current track is an MP3 stream (mp_player) -- the
 *     midi_player setting does not apply to non-MIDI playback,
 *   - the current track was loaded from an external music file
 *     via I_RegisterMusicFile (lumpnum was zeroed by that path,
 *     and the original filename is no longer recoverable here);
 *     such tracks are mp_player-backed in practice and would be
 *     filtered out by the MP3 check above, but the lumpnum guard
 *     is kept as a defensive belt-and-braces. */
void S_RestartMusic(void)
{
  musicinfo_t *m;
  int looping;
  int lumpnum;

  if (nomusicparm)
    return;
  if (!mus_playing)
    return;
  if (I_MusicIsMP3())
    return;

  m       = mus_playing;
  looping = mus_playing_looping;
  lumpnum = m->lumpnum;

  if (lumpnum <= 0)
    return; /* external file or never-cached, nothing safe to re-register */

  /* S_StopMusic clears mus_playing, mus_playing->data, and the
   * lump's cache lock.  We then re-cache and re-register on the
   * same musicinfo_t slot. */
  S_StopMusic();

  m->data   = W_CacheLumpNum(lumpnum);
  m->handle = I_RegisterSong(m->data, W_LumpLength(lumpnum));
  I_PlaySong(m->handle, looping);

  mus_playing         = m;
  mus_playing_looping = looping;
}



void S_StopChannel(int cnum)
{
  int i;
  channel_t *c = &channels[cnum];

  //jff 1/22/98 return if sound is not enabled
  if (nosfxparm)
    return;

  if (c->sfxinfo)
    {
      // stop the sound playing
      if (I_SoundIsPlaying(c->handle))
        I_StopSound(c->handle);

      // check to see
      //  if other channels are playing the sound
      for (i=0 ; i<numChannels ; i++)
        if (cnum != i && c->sfxinfo == channels[i].sfxinfo)
          break;

      // degrade usefulness of sound data
      c->sfxinfo->usefulness--;
      c->sfxinfo = 0;
    }
}

//
// Changes volume, stereo-separation, and pitch variables
//  from the norm of a sound effect to be played.
// If the sound is not audible, returns a 0.
// Otherwise, modifies parameters and returns 1.
//

int S_AdjustSoundParams(mobj_t *listener, degenmobj_t *source,
                        int *vol, int *sep, int *pitch)
{
  fixed_t adx, ady,approx_dist;
  angle_t angle;

  //jff 1/22/98 return if sound is not enabled
  if (nosfxparm)
    return 0;

  // e6y
  // Fix crash when the program wants to S_AdjustSoundParams() for player
  // which is not displayplayer and displayplayer was not spawned at the moment.
  // It happens in multiplayer demos only.
  //
  // Stack trace is:
  /* P_SetupLevel() \ P_LoadThings() \ P_SpawnMapThing() \ P_SpawnPlayer(players[0]) \
     P_SetupPsprites() \ P_BringUpWeapon() \ S_StartSound(players[0]->mo, sfx_sawup) \
     S_StartSoundAtVolume() \ S_AdjustSoundParams(players[displayplayer]->mo, ...); */
  // players[displayplayer]->mo is NULL
  //
  // There is no more crash on e1cmnet3.lmp between e1m2 and e1m3
  // http://competn.doom2.net/pub/compet-n/doom/coop/movies/e1cmnet3.zip
  if (!listener)
    return 0;

  // calculate the distance to sound origin
  //  and clip it if necessary
  adx = D_abs(listener->x - source->x);
  ady = D_abs(listener->y - source->y);

  // From _GG1_ p.428. Appox. eucledian distance fast.
  approx_dist = adx + ady - ((adx < ady ? adx : ady)>>1);

  if (!approx_dist)  // killough 11/98: handle zero-distance as special case
    {
      *sep = NORM_SEP;
      *vol = snd_SfxVolume;
      return *vol > 0;
    }

  if (approx_dist > S_CLIPPING_DIST)
    return 0;

  // angle of source to listener
  angle = R_PointToAngle2(listener->x, listener->y, source->x, source->y);

  if (angle <= listener->angle)
    angle += 0xffffffff;
  angle -= listener->angle;
  angle >>= ANGLETOFINESHIFT;

  // stereo separation
  *sep = 128 - (FixedMul(S_STEREO_SWING,finesine[angle])>>FRACBITS);

  // volume calculation
  if (approx_dist < S_CLOSE_DIST)
    *vol = snd_SfxVolume*8;
  else
    // distance effect
    *vol = (snd_SfxVolume * ((S_CLIPPING_DIST-approx_dist)>>FRACBITS) * 8)
      / S_ATTENUATOR;

  return (*vol > 0);
}

//
// S_getChannel :
//   If none available, return -1.  Otherwise channel #.
//
// killough 4/25/98: made static, added is_pickup argument

static int S_getChannel(void *origin, sfxinfo_t *sfxinfo, int is_pickup)
{
  // channel number to use
  int cnum;
  channel_t *c;

  //jff 1/22/98 return if sound is not enabled
  if (nosfxparm)
    return -1;

  // Find an open channel
  for (cnum=0; cnum<numChannels && channels[cnum].sfxinfo; cnum++)
    if (origin && channels[cnum].origin == origin &&
        channels[cnum].is_pickup == is_pickup)
      {
        S_StopChannel(cnum);
        break;
      }

    // None available
  if (cnum == numChannels)
    {      // Look for lower priority
      for (cnum=0 ; cnum<numChannels ; cnum++)
        if (channels[cnum].sfxinfo->priority >= sfxinfo->priority)
          break;
      if (cnum == numChannels)
        return -1;                  // No lower priority.  Sorry, Charlie.
      else
        S_StopChannel(cnum);        // Otherwise, kick out lower priority.
    }

  c = &channels[cnum];              // channel is decided to be cnum.
  c->sfxinfo = sfxinfo;
  c->origin = origin;
  c->is_pickup = is_pickup;         // killough 4/25/98
  return cnum;
}
