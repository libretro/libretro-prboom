#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

#include "libretro.h"

#include "../src/i_sound.h"
#include "../src/musicplayer.h"
#include "../src/oplplayer.h"
#include "../src/madplayer.h"

#include "../src/lprintf.h"
#include "../src/doomdef.h"
#include "../src/r_fps.h"
#include "../src/m_misc.h"
#include "../src/m_swap.h"
#include "../src/w_wad.h"
#include "../src/z_zone.h"

#define SAMPLECOUNT_35		((4 * 11025) / 35)
#define SAMPLECOUNT_40		((4 * 11025) / 40)
#define SAMPLECOUNT_50		((4 * 11025) / 50)
#define SAMPLECOUNT_60		((4 * 11025) / 60)
#define NUM_CHANNELS		32
#define BUFMUL           4
#define MIXBUFFERSIZE   (SAMPLECOUNT_35*BUFMUL)
#define MAX_CHANNELS    32

#if 0
#define MIDI_SUPPORT
#endif

extern retro_audio_sample_batch_t audio_batch_cb;
extern retro_log_printf_t log_cb;
extern int gametic;
extern int snd_SfxVolume;
extern int snd_MusicVolume;

int 		lengths[NUMSFX];
int snd_card = 1;
int mus_card = 0;
int snd_samplerate= 11025;

typedef struct
{
   // SFX id of the playing sound effect.
   // Used to catch duplicates (like chainsaw).
   int id;
   // The channel step amount...
   unsigned int step;
   // ... and a 0.16 bit remainder of last step.
   unsigned int stepremainder;
   unsigned int samplerate;
   // The channel data pointers, start and end.
   const unsigned char* data;
   const unsigned char* enddata;
   // Hardware left and right channel volume lookup.
   int *leftvol_lookup;
   int *rightvol_lookup;
} channel_info_t;

channel_info_t channelinfo[MAX_CHANNELS];

// The global mixing buffer.
// Basically, samples from all active internal channels
//  are modifed and added, and stored in the buffer
//  that is submitted to the audio device.
int16_t mixbuffer[MIXBUFFERSIZE];

typedef struct
{
    uint8_t *snd_start_ptr, *snd_end_ptr;
    unsigned int starttic;
    int sfxid;
    int *leftvol, *rightvol;
    int handle;
} channel_t;

static channel_t channels[NUM_CHANNELS];

int		vol_lookup[128*256];

static double R_ceil (double x)
{
   if (x > LONG_MAX)
      return x; /* big floats are all ints */
   return ((long)(x+(0.99999999999999997)));
}

static double R_floor(double x)
{
   double y;
   union {double f; uint64_t i;} u = {x};
   int e = u.i >> 52 & 0x7ff;

   if (e >= 0x3ff+52 || x == 0)
      return x;
   /* y = int(x) - x, where int(x) is an integer neighbor of x */
   if (u.i >> 63)
      y = (double)(x - 0x1p52) + 0x1p52 - x;
   else
      y = (double)(x + 0x1p52) - 0x1p52 - x;
   /* special case because of non-nearest rounding modes */
   if (e <= 0x3ff-1)
      return u.i >> 63 ? -1 : 0;
   if (y > 0)
      return x + y - 1;
   return x + y;
}

/* i_sound */
static void I_SndMixResetChannel (int channum)
{
   memset (&channels[channum], 0, sizeof(channel_t));
}

/* This function loads the sound data from the WAD lump
 * for a single sound effect. */
static void* I_SndLoadSample (const char* sfxname, int* len)
{
    int i, x, padded_sfx_len, sfxlump_num, sfxlump_len;
    char sfxlump_name[20];
    uint8_t *sfxlump_data, *sfxlump_sound, *padded_sfx_data;
    uint16_t orig_rate;
    float times;
    
    sprintf (sfxlump_name, "DS%s", sfxname);
    
    // check if the sound lump exists
    if (W_CheckNumForName(sfxlump_name) == -1)
        return 0;
        
    sfxlump_num = W_GetNumForName (sfxlump_name);
    sfxlump_len = W_LumpLength (sfxlump_num);
    
    // if it's not at least 9 bytes (8 byte header + at least 1 sample), it's
    // not in the correct format
    if (sfxlump_len < 9)
        return 0;
    
    // load it
    sfxlump_data = W_CacheLumpNum (sfxlump_num);
    sfxlump_sound = sfxlump_data + 8;
    sfxlump_len -= 8;
    
    // get original sample rate from DMX header
    memcpy (&orig_rate, sfxlump_data+2, 2);
    orig_rate = SHORT (orig_rate);
    
    times = 48000.0f / (float)orig_rate;
    
    padded_sfx_len = ((sfxlump_len*R_ceil(times) + (SAMPLECOUNT_35-1)) / SAMPLECOUNT_35) * SAMPLECOUNT_35;
    padded_sfx_data = (uint8_t*)malloc(padded_sfx_len);
    
    for (i=0; i < padded_sfx_len; i++)
    {
        x = R_floor ((float)i/times);
        
        if (x < sfxlump_len) // 8 was already subtracted
            padded_sfx_data[i] = sfxlump_sound[x];
        else
            padded_sfx_data[i] = 128; // fill the rest with silence
    }
        
    Z_Free (sfxlump_data); //  free original lump

    *len = padded_sfx_len;
    return (void *)(padded_sfx_data);
}


//
// SFX API
// Note: this was called by S_Init.
// However, whatever they did in the
// old DPMS based DOS version, this
// were simply dummies in the Linux
// version.
// See soundserver initdata().
//

void I_SetChannels(void)
{
   /* Init internal lookups (raw data, mixing buffer, channels).
    * This function sets up internal lookups used during
    * the mixing process.
    */

   int i, j;

   /* Okay, reset internal mixing channels to zero. */
   for (i=0; i<NUM_CHANNELS; i++)
      I_SndMixResetChannel(i);

   /* Generates volume lookup tables which also turn the unsigned
    * samples into signed samples. */
   for (i=0 ; i<128 ; i++)
   {
      for (j=0 ; j<256 ; j++)
         vol_lookup[i*256+j] = (i*(j-128)*256)/127;
   }
}	

 
void I_SetSfxVolume(int volume)
{
    snd_SfxVolume = volume;
}

void I_SetMusicVolume(int volume)
{
    snd_MusicVolume = volume;
#ifdef MUSIC_SUPPORT
#ifdef MIDI_SUPPORT
    opl_synth_player.setvolume(volume);
#else
    mp_player.setvolume(volume);
#endif
#endif
}

/* Retrieve the raw data lump index
 * for a given SFX name. */
int I_GetSfxLumpNum(sfxinfo_t* sfx)
{
    char namebuf[9];

    sprintf(namebuf, "ds%s", sfx->name);
    return W_GetNumForName(namebuf);
}

void I_StopSound (int handle)
{
    int i;
    
    for (i=0; i<NUM_CHANNELS; i++)
    {
        if (channels[i].handle==handle)
        {
            I_SndMixResetChannel(i);
            return;
        }
    }
}

//
// Starting a sound means adding it
//  to the current list of active sounds
//  in the internal channels.
// As the SFX info struct contains
//  e.g. a pointer to the raw data,
//  it is ignored.
// As our sound handling does not handle
//  priority, it is ignored.
// Pitching (that is, increased speed of playback)
//  is set, but currently not used by mixing.
//
static int currenthandle = 0;

int I_StartSound (int id, int channel, int vol, int sep, int pitch, int priority)
{
    int	i, oldestslot, oldesttics, slot, rightvol, leftvol;

    // this effect was not loaded.
    if (!S_sfx[id].data)
        return -1;

    // Loop all channels to find a free slot.
    slot = -1;
    oldesttics = gametic;
    oldestslot = 0;

    for (i=0; i<NUM_CHANNELS; i++)
    {
	if (!channels[i].snd_start_ptr)  // not playing
        {
            slot = i;
            break;
        }
        
        if (channels[i].starttic < oldesttics)
        {
            oldesttics = channels[i].starttic;
            oldestslot = i;
        }
    }
    
    // No free slots, so replace the oldest sound still playing.
    if (slot == -1)
        slot = oldestslot;
    
    channels[slot].handle = ++currenthandle;
    
    // Set pointers to raw sound data start & end.
    channels[slot].snd_start_ptr = (uint8_t*)S_sfx[id].data;
    channels[slot].snd_end_ptr = channels[slot].snd_start_ptr + lengths[id];

    // Save starting gametic.
    channels[slot].starttic = gametic;

    sep += 1;

    // Per left/right channel.
    //  x^2 seperation,
    //  adjust volume properly.
    leftvol = vol - ((vol*sep*sep) >> 16); ///(256*256);
    sep -= 257;
    rightvol = vol - ((vol*sep*sep) >> 16);	

    // Sanity check, clamp volume.
    if (rightvol < 0 || rightvol > 127)
	I_Error("addsfx: rightvol out of bounds");
    
    if (leftvol < 0 || leftvol > 127)
	I_Error("addsfx: leftvol out of bounds");
    
    // Get the proper lookup table piece
    //  for this volume level???
    channels[slot].leftvol = &vol_lookup[leftvol*256];
    channels[slot].rightvol = &vol_lookup[rightvol*256];

    // Preserve sound SFX id,
    //  e.g. for avoiding duplicates of chainsaw.
    channels[slot].sfxid = id;

    return currenthandle;
}


boolean I_SoundIsPlaying (int handle)
{
    int i;
    
    for (i=0; i<NUM_CHANNELS; i++)
    {
        if (channels[i].handle==handle)
            return 1;
    }

    return 0;
}

//
// This function loops all active (internal) sound
//  channels, retrieves a given number of samples
//  from the raw sound data, modifies it according
//  to the current (internal) channel parameters,
//  mixes the per channel samples into the global
//  mixbuffer, clamping it to the allowed range,
//  and sets up everything for transferring the
//  contents of the mixbuffer to the (two)
//  hardware channels (left and right, that is).
//
// This function currently supports only 16bit.
//

static const void *music_handle;
static void *song_data;

void I_UpdateSound(void)
{
   // Mix current sound data. Data, from raw sound, for right and left.
   uint8_t sample;
   int dl, dr, frames, out_frames, step, chan;
   int16_t mad_audio_buf[SAMPLECOUNT_35 * 2];

   // Pointers in global mixbuffer, left, right, end.
   int16_t *leftout, *rightout, *leftend;

   // Left and right channel are in global mixbuffer, alternating.
   leftout = mixbuffer;
   rightout = mixbuffer+1;
   step = 2;
   frames = 0;

   switch(movement_smooth)
   {
      case 0:
         out_frames = SAMPLECOUNT_35;
         break;
      case 1:
         out_frames = SAMPLECOUNT_40;
         break;
      case 2:
         out_frames = SAMPLECOUNT_50;
         break;
      case 3:
         out_frames = SAMPLECOUNT_60;
         break;
      default:
         out_frames = SAMPLECOUNT_35;
         break;
   }

#ifdef MUSIC_SUPPORT
   if (music_handle)
   {
#ifdef MIDI_SUPPORT
      opl_synth_player.render(mad_audio_buf, out_frames);
#else
      mp_player.render(mad_audio_buf, out_frames);
#endif
   }
   else
#endif
      memset(mad_audio_buf, 0, out_frames * 4);

   // Determine end, for left channel only (right channel is implicit).
   leftend = mixbuffer + out_frames * step;

   // Mix sounds into the mixing buffer.
   // Loop over step*SAMPLECOUNT, that is 512 values for two channels.

   while (leftout <= leftend)
   {
      // Reset left/right value.
      dl = mad_audio_buf[frames * 2 + 0];
      dr = mad_audio_buf[frames * 2 + 1];


      for (chan=0; chan<NUM_CHANNELS; chan++)
      {
         // Check channel, if active.
         if (channels[chan].snd_start_ptr)
         {
            // Get the raw data from the channel. 
            sample = *channels[chan].snd_start_ptr;

            // Add left and right part for this channel (sound) to the
            // current data. Adjust volume accordingly.
            dl += channels[chan].leftvol[sample];
            dr += channels[chan].rightvol[sample];

            channels[chan].snd_start_ptr++;

            if (!(channels[chan].snd_start_ptr < channels[chan].snd_end_ptr))
               I_SndMixResetChannel (chan);
         }
      }

      // Clamp to range. Left hardware channel.
      // Has been char instead of short.
      // if (dl > 127) *leftout = 127;
      // else if (dl < -128) *leftout = -128;
      // else *leftout = dl;

      if (dl > 0x7fff)
         *leftout = 0x7fff;
      else if (dl < -0x8000)
         *leftout = -0x8000;
      else
         *leftout = dl;

      // Same for right hardware channel.
      if (dr > 0x7fff)
         *rightout = 0x7fff;
      else if (dr < -0x8000)
         *rightout = -0x8000;
      else
         *rightout = dr;

      // Increment current pointers in mixbuffer.
      leftout += step;
      rightout += step;
      frames++;
   }

   for (frames = 0; frames < out_frames; )
      frames += audio_batch_cb(mixbuffer + (frames << 1), out_frames - frames);
}

void I_UpdateSoundParams (int handle, int vol, int sep, int pitch)
{
   int rightvol, leftvol, i;

   for (i=0; i<NUM_CHANNELS; i++)
   {
      if (channels[i].handle==handle)
      {
         sep += 1;

         leftvol = vol - ((vol*sep*sep) >> 16); ///(256*256);
         sep -= 257;
         rightvol = vol - ((vol*sep*sep) >> 16);	

         if (rightvol < 0 || rightvol > 127)
            I_Error("I_UpdateSoundParams: rightvol out of bounds.");

         if (leftvol < 0 || leftvol > 127)
            I_Error("I_UpdateSoundParams: leftvol out of bounds.");

         channels[i].leftvol = &vol_lookup[leftvol*256];
         channels[i].rightvol = &vol_lookup[rightvol*256];
         return;
      }
   }
}


void I_ShutdownSound(void)
{    
}


void I_InitSound(void)
{
    int i;
    
    memset (&lengths, 0, sizeof(int)*NUMSFX);
    for (i=1 ; i<NUMSFX ; i++)
    { 
        // Alias? Example is the chaingun sound linked to pistol.
        if (!S_sfx[i].link)
        {
            // Load data from WAD file.
            S_sfx[i].data = I_SndLoadSample( S_sfx[i].name, &lengths[i] );
        }	
        else
        {
            // Previously loaded already?
            S_sfx[i].data = S_sfx[i].link->data;
            lengths[i] = lengths[(S_sfx[i].link - S_sfx)/sizeof(sfxinfo_t)];
        }
    }

    I_SetChannels();
  
    if (log_cb)
       log_cb(RETRO_LOG_INFO, "I_InitSound: \n");
}

boolean I_AnySoundStillPlaying(void)
{
  boolean result = false;
  int i;

  for (i=0; i<MAX_CHANNELS; i++)
    result |= channelinfo[i].data != NULL;

  return result;
}

//
// MUSIC API.
// Still no music done.
// Remains. Dummies.

void I_InitMusic(void)		{ }
void I_ShutdownMusic(void)	{ }

static int	looping=0;
static int	musicdies=-1;

void I_PlaySong(int handle, int looping)
{
  handle = 0;
  musicdies = gametic + TICRATE*30; // ?

  (void)handle;

#ifdef MUSIC_SUPPORT
#ifdef MIDI_SUPPORT
  opl_synth_player.play(music_handle, looping);
  opl_synth_player.setvolume(snd_MusicVolume);
#else
  mp_player.play(music_handle, looping);
  mp_player.setvolume(snd_MusicVolume);
#endif
#endif
}

void I_PauseSong (int handle)
{
  handle = 0;
  (void)handle;

#ifdef MUSIC_SUPPORT
#ifdef MIDI_SUPPORT
  opl_synth_player.pause();
#else
  mp_player.pause();
#endif
#endif
}

void I_ResumeSong (int handle)
{
  handle = 0;
  (void)handle;

#ifdef MUSIC_SUPPORT
#ifdef MIDI_SUPPORT
  opl_synth_player.resume();
#else
  mp_player.resume();
#endif
#endif
}

void I_StopSong(int handle)
{
  handle    = 0;
  looping   = 0;
  musicdies = 0;

  (void)handle;

#ifdef MUSIC_SUPPORT
#ifdef MIDI_SUPPORT
  opl_synth_player.stop();
#else
  mp_player.stop();
#endif
#endif
}

void I_UnRegisterSong(int handle)
{
  handle = 0;
  (void)handle;

#ifdef MUSIC_SUPPORT
#ifdef MIDI_SUPPORT
  opl_synth_player.unregistersong(music_handle);
#else
  mp_player.unregistersong(music_handle);
#endif
  music_handle = NULL;
  free(song_data);
  song_data = NULL;
#endif
}

int I_RegisterSong(const void* data, size_t len)
{
  return 1;
}

// Is the song playing?
int I_QrySongPlaying(int handle)
{
  handle = 0;
  (void)handle;
  return looping || musicdies > gametic;
}

#ifdef MUSIC_SUPPORT
static int RegisterSong(const void *data, size_t len)
{
#ifdef MIDI_SUPPORT
   music_handle = opl_synth_player.registersong(data, len);
#else
   music_handle = mp_player.registersong(data, len);
#endif
   return !!music_handle;
}
#endif

int I_RegisterMusic( const char* filename, musicinfo_t *song )
{
  int len;

#ifdef MUSIC_SUPPORT
  if (log_cb)
     log_cb(RETRO_LOG_INFO, "RegisterMusic: %s\n", filename);

  len = M_ReadFile(filename, (uint8_t**) &song_data);
  if (len == -1)
  {
     if (log_cb)
        log_cb(RETRO_LOG_WARN, "Couldn't read %s\n", filename);
     return 1;
  }

  if (!RegisterSong(song_data, len))
  {
     free(song_data);
     song_data = NULL;
     if (log_cb)
        log_cb(RETRO_LOG_WARN, "Couldn't load music from %s\n", filename);
     return 1;
  }

  song->data = 0;
  song->handle = 0;
  song->lumpnum = 0;
#endif
  return 0;
}

/* NSM helper routine for some of the streaming audio.
 * Assumes 16bit signed interleaved stereo. */
void I_ResampleStream (void *dest, unsigned nsamp, void (*proc)(void *dest, unsigned nsamp),
      unsigned sratein, unsigned srateout)
{
  unsigned i;
  int                   j   = 0;
  int16_t           *sout   = dest;
  static int16_t     *sin   = NULL;
  static unsigned sinsamp   = 0;
  static unsigned remainder = 0;
  unsigned step             = (sratein << 16) / (unsigned) srateout;
  unsigned nreq             = (step * nsamp + remainder) >> 16;

  if (nreq > sinsamp)
  {
    sin = realloc (sin, (nreq + 1) * 4);
    if (!sinsamp) // avoid pop when first starting stream
      sin[0] = sin[1] = 0;
    sinsamp = nreq;
  }

  proc (sin + 2, nreq);

  for (i = 0; i < nsamp; i++)
  {
    *sout++ = ((unsigned) sin[j + 0] * (0x10000 - remainder) +
               (unsigned) sin[j + 2] * remainder) >> 16;
    *sout++ = ((unsigned) sin[j + 1] * (0x10000 - remainder) +
               (unsigned) sin[j + 3] * remainder) >> 16;
    remainder += step;
    j += remainder >> 16 << 1;
    remainder &= 0xffff;
  }
  sin[0] = sin[nreq * 2];
  sin[1] = sin[nreq * 2 + 1];
}  

void I_MPPlayer_Init(void)
{
#ifdef MUSIC_SUPPORT
#ifdef MIDI_SUPPORT
   opl_synth_player.init(44100);
#else
   mp_player.init(44100);
#endif
#endif
}

void I_MPPlayer_Free(void)
{
#ifdef MUSIC_SUPPORT
#ifdef MIDI_SUPPORT
   opl_synth_player.shutdown();
#else
   mp_player.shutdown();
#endif
#endif
}
