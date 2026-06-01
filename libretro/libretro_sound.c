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
#include "../src/dsda_hacked.h"
#include "../src/musicplayer.h"
#include "../src/flplayer.h"
#include "../src/oplplayer.h"
#include "../src/madplayer.h"

#include "../src/lprintf.h"
#include "../src/doomdef.h"
#include "../src/r_fps.h"
#include "../src/m_misc.h"
#include "../src/m_swap.h"
#include "../src/w_wad.h"
#include "../src/z_zone.h"

#include "../src/mus2mid.h"

#define SAMPLERATE    		(4 * 11025)
#define SAMPLECOUNT_35		(SAMPLERATE / 35)
#define NUM_CHANNELS		32
#define BUFMUL           4
#define MIXBUFFERSIZE   (SAMPLECOUNT_35*BUFMUL)
#define MAX_CHANNELS    32

static const void *music_handle;
static void *song_data;

/* Generic music save-state: counter of samples rendered since the
 * current song started.  Tracked by the wrapper, NOT by the backend,
 * so it's available for every backend (including ones with no
 * backend-level serialize/unserialize, e.g. mp_player).  Reset to
 * zero in I_PlaySong; advanced inside the render call site. */
static uint64_t music_samples_played;

/* Last looping flag passed to I_PlaySong, captured so the generic
 * restore path can call current_player->play() again with the same
 * argument it was given originally.  Separate from the (latently
 * buggy) file-scope `looping` static which is read by
 * I_QrySongPlaying -- leaving that alone here. */
static int music_last_looping;

extern retro_audio_sample_batch_t audio_batch_cb;
extern retro_log_printf_t log_cb;
extern int gametic;
extern int snd_SfxVolume;
extern int snd_MusicVolume;

int *lengths = NULL;
static int lengths_size = 0;
int snd_card = 1;
int mus_card = 0;

/* The global mixing buffer.
 * Basically, samples from all active internal channels
 *  are modifed and added, and stored in the buffer
 *  that is submitted to the audio device. */
int16_t mixbuffer[MIXBUFFERSIZE];

typedef struct
{
    uint8_t *snd_start_ptr, *snd_end_ptr;
    unsigned int starttic;
    int sfxid;
    int *leftvol, *rightvol;
    int handle;
} channel_t;

// list of possible music players
static const music_player_t *music_players[] =
{ // until some ui work is done, the order these appear is the autodetect order.
 #ifdef HAVE_LIBFLUIDSYNTH
  &fl_player, // flplayer.h
#endif
  &opl_synth_player, // oplplayer.h
#ifdef HAVE_LIBMAD
  &mp_player, // madplayer.h
#endif
  NULL
};
#define NUM_MUS_PLAYERS ((int)(sizeof (music_players) / sizeof (music_player_t *) - 1))

/* Music player currently used */
music_player_t* current_player = NULL;

static channel_t channels[NUM_CHANNELS];

int vol_lookup[128*256];

/* i_sound */

/* Load a single sound effect from its DS<name> WAD lump and resample
 * it to the libretro output rate.
 *
 * DMX SFX format: 8-byte header (uint16 type=3, uint16 sample_rate,
 * uint32 length) followed by `length` unsigned 8-bit PCM samples.
 *
 * Resampling here is nearest-neighbour with a 16.16 fixed-point step
 * accumulator -- one add and one shift per output sample, no float
 * division and no per-sample floor() call.  The previous version
 * computed (float)i/times + R_floor() per sample, which was both
 * slower and used custom-rolled R_ceil/R_floor functions that did
 * IEEE-754 bit hacks the hardware can do for free.
 *
 * The output is no longer padded to a multiple of SAMPLECOUNT_35 --
 * that padding (up to ~1259 silent bytes per loaded SFX) was a hold-
 * over from a buffering scheme the libretro mixer doesn't use; the
 * mix loop terminates each channel via the snd_start_ptr/snd_end_ptr
 * comparison. */
static void* I_SndLoadSample(const char* sfxname, int* len)
{
    int             out_i, sfxlump_num, sfxlump_len, out_len;
    char            sfxlump_name[20];
    const uint8_t  *sfxlump_data, *sfxlump_sound;
    uint8_t        *out_data;
    uint16_t        orig_rate;
    unsigned int    step_fx;     /* 16.16 input step per output sample */
    unsigned int    pos_fx;      /* 16.16 input position accumulator */

    sprintf (sfxlump_name, "DS%s", sfxname);

    /* check if the sound lump exists */
    if (W_CheckNumForName(sfxlump_name) == -1)
        return 0;

    sfxlump_num = W_GetNumForName (sfxlump_name);
    sfxlump_len = W_LumpLength (sfxlump_num);

    /* DMX header is 8 bytes; need at least one PCM byte after it. */
    if (sfxlump_len < 9)
        return 0;

    sfxlump_data    = W_CacheLumpNum (sfxlump_num);
    sfxlump_sound   = sfxlump_data + 8;
    sfxlump_len    -= 8;

    /* Get original sample rate from DMX header (offset 2, little-
     * endian uint16). */
    memcpy(&orig_rate, sfxlump_data+2, 2);
    orig_rate       = SHORT (orig_rate);
    if (orig_rate == 0)
        orig_rate = 11025;  /* defensive: malformed lump */

    /* Output length: input_samples * (SAMPLERATE / orig_rate),
     * rounded up so we don't drop the tail.  Computed in integer
     * math via a 64-bit intermediate: (sfxlump_len * SAMPLERATE)
     * overflows int32 once sfxlump_len passes ~48 KB, which can
     * happen for the longer DMX sound lumps (e.g. DSBAREXP).  Use
     * uint64_t to keep the computation clean and let the result
     * fit back into int (output is at most ~4x input length so
     * always fits int32). */
    out_len = (int)(((uint64_t)sfxlump_len * (uint64_t)SAMPLERATE +
                     (uint64_t)orig_rate - 1) /
                    (uint64_t)orig_rate);
    out_data = (uint8_t*)malloc(out_len);
    if (!out_data)
    {
        W_UnlockLumpNum (sfxlump_num);
        return 0;
    }

    /* 16.16 fixed-point step: orig_rate / SAMPLERATE input samples
     * per output sample.  E.g. for 11025 -> 44100 this is 0.25,
     * encoded as (1 << 14) = 16384. */
    step_fx = ((unsigned int)orig_rate << 16) / (unsigned int)SAMPLERATE;
    pos_fx  = 0;

    /* Linear interpolation between adjacent input samples.  The DMX
     * SFX are 8-bit PCM at 11025/22050 Hz; upsampling to 44100 by
     * nearest-neighbour (sample-and-hold) was cheap but turned each
     * input sample into a 2x/4x staircase, audibly aliased/harsh.
     * Interpolating between sfxlump_sound[x] and [x+1] by the 16.16
     * fractional position removes the staircase for the same single
     * pass.  This is load-time work (once per distinct SFX lump), so
     * the extra multiply/add per output sample is irrelevant.  Samples
     * are unsigned 8-bit; blend in that domain and store back as u8. */
    for (out_i = 0; out_i < out_len; out_i++)
    {
        unsigned int x    = pos_fx >> 16;
        unsigned int frac = pos_fx & 0xffff;

        if (x + 1 < (unsigned int)sfxlump_len)
        {
            unsigned int a = sfxlump_sound[x];
            unsigned int b = sfxlump_sound[x + 1];
            out_data[out_i] =
                (uint8_t)((a * (0x10000 - frac) + b * frac) >> 16);
        }
        else if (x < (unsigned int)sfxlump_len)
            out_data[out_i] = sfxlump_sound[x]; /* last sample: hold */
        else
            out_data[out_i] = 128;              /* past end: DC silence */

        pos_fx += step_fx;
    }

    /* Release the cached lump back to the zone via the cache's
     * lock-count mechanism.  The previous Z_Free here freed the
     * lump's memory directly, leaving cachelump[sfxlump_num].cache
     * pointing at freed memory and the lock count non-zero -- so
     * the next W_CacheLumpNum on the same lump (e.g. on the next
     * S_Init / content load) returned the dangling pointer
     * instead of re-reading.  Use the proper unlock path. */
    W_UnlockLumpNum (sfxlump_num);

    *len = out_len;
    return (void *)(out_data);
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
   for (i = 0; i < NUM_CHANNELS; i++)
      memset(&channels[i], 0, sizeof(channel_t));

   /* Generates volume lookup tables which also turn the unsigned
    * samples into signed samples. */
   for (i = 0; i < 128; i++)
   {
      for (j = 0; j < 256; j++)
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
   if (current_player)
      current_player->setvolume(volume);
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

    for (i = 0; i < NUM_CHANNELS; i++)
    {
        if (channels[i].handle == handle)
        {
            memset(&channels[i], 0, sizeof(channel_t));
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
    unsigned int i, oldestslot, oldesttics;
    int slot, rightvol, leftvol;

    // this effect was not loaded.
    if (!S_sfx[id].data)
        return -1;

    // Loop all channels to find a free slot.
    slot       = -1;
    oldesttics = gametic;
    oldestslot = 0;

    for (i = 0; i < NUM_CHANNELS; i++)
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
    channels[slot].snd_end_ptr   = channels[slot].snd_start_ptr + lengths[id];

    // Save starting gametic.
    channels[slot].starttic      = gametic;

    sep += 1;

    // Per left/right channel.
    //  x^2 seperation,
    //  adjust volume properly.
    leftvol  = vol - ((vol*sep*sep) >> 16); ///(256*256);
    sep     -= 257;
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

dbool   I_SoundIsPlaying (int handle)
{
    int i;

    for (i = 0; i < NUM_CHANNELS; i++)
    {
        if (channels[i].handle==handle)
            return 1;
    }

    return 0;
}

/* Mix one frame's worth of audio and submit it to libretro.
 *
 * Determinism: called exactly once per retro_run.  The number of
 * frames produced is fixed by tic_vars.sample_step (audio rate /
 * video fps), so each retro_run call submits the same-sized audio
 * batch every time -- no rate jitter, no underrun catch-up.
 *
 * Pipeline:
 *   1. Music (if any) renders int16 stereo straight into mixbuffer;
 *      otherwise mixbuffer gets zeroed.
 *   2. For each active SFX channel, accumulate its volume-scaled
 *      contribution into 32-bit dl/dr, then clamp to int16 and
 *      write back to mixbuffer.
 *   3. One audio_batch_cb submission for the whole frame.
 *
 * Optimisations vs the previous version:
 *   - No separate mad_audio_buf; music renders directly into the
 *     output buffer.  Saves a 5 KB stack alloc + memset + read-back.
 *   - Active channels collected into a compact list once per call,
 *     so the inner per-sample loop iterates ~8 entries (typical
 *     gameplay) instead of 32.
 *   - Channel-end handling moved out of the inner loop: when a
 *     channel runs dry mid-frame, NULL its start_ptr inline; no
 *     more 48-byte memset per channel completion.
 *   - Off-by-one fixed (was writing out_frames+1 frames into a
 *     buffer the submit loop only read out_frames from).
 *   - Single audio_batch_cb call (libretro guarantees full-batch
 *     consumption for the standard frontends; the retry loop
 *     guarded a non-issue).
 */
void I_UpdateSound(void)
{
   /* Compact list of currently-playing SFX channels, rebuilt each
    * call.  Pointer-based so the inner loop has one indirection
    * fewer than indexing channels[] by integer. */
   channel_t *active[NUM_CHANNELS];
   int        n_active = 0;
   int        i;
   int        out_frames;
   int16_t   *out;

   /* sample_step is 16.16 fixed-point samples-per-frame.  Carry the
    * fractional remainder across frames so the emitted frame count is
    * floor or floor+1 each call and the long-run average equals
    * sample_rate/fps exactly -- otherwise the dropped fraction makes the
    * core under-produce at any fps that doesn't divide the sample rate,
    * engaging the frontend resampler and drifting A/V sync. */
   if (tic_vars.sample_step)
   {
      static fixed_t sample_acc = 0;
      sample_acc += tic_vars.sample_step;
      out_frames  = sample_acc >> FRACBITS;
      sample_acc &= (FRACUNIT - 1);
   }
   else
      out_frames = SAMPLECOUNT_35;

   /* Step 1: music or silence into mixbuffer. */
#ifdef MUSIC_SUPPORT
   if (music_handle && current_player)
   {
      current_player->render(mixbuffer, out_frames);
      music_samples_played += (uint64_t)out_frames;
   }
   else
#endif
      memset(mixbuffer, 0, (size_t)out_frames * 2 * sizeof(int16_t));

   /* Step 2: gather active SFX channels. */
   for (i = 0; i < NUM_CHANNELS; i++)
   {
      if (channels[i].snd_start_ptr)
         active[n_active++] = &channels[i];
   }

   /* Step 3: per-sample mix.  When there are no SFX active we can
    * skip the mix loop entirely -- the mixbuffer already holds
    * music (or silence) and is ready to submit. */
   if (n_active > 0)
   {
      /* Determine the longest contiguous prefix of frames during
       * which all active channels still have data.  After this
       * boundary, at least one channel runs out -- we re-collect
       * the survivors and mix the remainder.  This lets the inner
       * loop run without any per-sample end-of-channel check
       * (which becomes the dominant cost when many channels are
       * active). */
      int frames_left = out_frames;

      out = mixbuffer;

      while (frames_left > 0 && n_active > 0)
      {
         int chunk = frames_left;
         int j;
         int16_t *chunk_end;

         /* Find the smallest "remaining samples" across active
          * channels; that's our chunk length.  remaining = (end -
          * start) bytes, one byte per output sample. */
         for (j = 0; j < n_active; j++)
         {
            int rem = (int)(active[j]->snd_end_ptr -
                            active[j]->snd_start_ptr);
            if (rem < chunk)
               chunk = rem;
         }
         if (chunk <= 0)
            chunk = 1;  /* defensive; shouldn't happen */

         chunk_end = out + chunk * 2;

         /* Inner loop: mix `chunk` samples with no end-of-channel
          * checks. */
         while (out < chunk_end)
         {
            int dl = out[0];
            int dr = out[1];

            for (j = 0; j < n_active; j++)
            {
               channel_t *c = active[j];
               uint8_t    s = *c->snd_start_ptr++;
               dl += c->leftvol[s];
               dr += c->rightvol[s];
            }

            /* Clamp to int16 range.  GCC and Clang both compile
             * this to a pair of conditional moves. */
            if      (dl >  0x7fff) dl =  0x7fff;
            else if (dl < -0x8000) dl = -0x8000;
            if      (dr >  0x7fff) dr =  0x7fff;
            else if (dr < -0x8000) dr = -0x8000;

            out[0] = (int16_t)dl;
            out[1] = (int16_t)dr;
            out   += 2;
         }

         frames_left -= chunk;

         /* Reap any channels that just hit end-of-data and rebuild
          * the active list in place. */
         {
            int dst = 0;
            for (j = 0; j < n_active; j++)
            {
               if (active[j]->snd_start_ptr >= active[j]->snd_end_ptr)
                  memset(active[j], 0, sizeof(channel_t));
               else
                  active[dst++] = active[j];
            }
            n_active = dst;
         }
      }

      /* If we exited because n_active reached 0 with frames left,
       * the rest of mixbuffer already holds music/silence -- no
       * further work needed. */
   }

   /* Step 4: hand off to libretro. */
   audio_batch_cb(mixbuffer, out_frames);
}

void I_UpdateSoundParams (int handle, int vol, int sep, int pitch)
{
   int rightvol, leftvol, i;

   for (i = 0; i < NUM_CHANNELS; i++)
   {
      if (channels[i].handle==handle)
      {
         sep     += 1;

         leftvol  = vol - ((vol*sep*sep) >> 16); ///(256*256);
         sep     -= 257;
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
   int i;

   for(i = 0; i < num_sfx; i++)
   {
      if (!S_sfx[i].link)
      {
         free(S_sfx[i].data);
         S_sfx[i].data = NULL;
      }
   }
}

void I_InitSound(void)
{
  int i;

  /* lengths[] tracks the (growable) sfx count; reallocate to cover any
   * dsdhacked-added sounds. */
  if (lengths_size < num_sfx)
  {
    lengths = (int*)realloc(lengths, num_sfx * sizeof(int));
    lengths_size = num_sfx;
  }
  memset(lengths, 0, sizeof(int) * num_sfx);

  for (i = 1; i < num_sfx; i++)
  {
     // Alias? Example is the chaingun sound linked to pistol.
     if (!S_sfx[i].link) // Load data from WAD file.
        S_sfx[i].data = I_SndLoadSample( S_sfx[i].name, &lengths[i] );
     else // Previously loaded already?
     {
        S_sfx[i].data = S_sfx[i].link->data;
        /* link - S_sfx is already an element index (pointer subtraction of
         * sfxinfo_t*); do not divide by sizeof again. */
        lengths[i]    = lengths[S_sfx[i].link - S_sfx];
     }
  }

  I_SetChannels();

  if (log_cb)
    log_cb(RETRO_LOG_INFO, "I_InitSound: \n");
}

/* MUSIC API */

static int	looping=0;
static int	musicdies=-1;

void I_PlaySong(int handle, int looping)
{
  (void)handle;
  musicdies              = gametic + TICRATE * 30;
  music_last_looping     = looping;
  music_samples_played   = 0;

#ifdef MUSIC_SUPPORT
  if (current_player)
  {
     current_player->play(music_handle, looping);
     current_player->setvolume(snd_MusicVolume);
  }
#endif
}

void I_PauseSong (int handle)
{
   (void)handle;
   if (current_player)
      current_player->pause();
}

void I_ResumeSong (int handle)
{
   (void)handle;
#ifdef MUSIC_SUPPORT
   if (current_player)
      current_player->resume();
#endif
}

void I_StopSong(int handle)
{
   (void)handle;

   looping   = 0;
   musicdies = 0;

   if (current_player)
      current_player->stop();
}

void I_UnRegisterSong(int handle)
{
   (void)handle;

#ifdef MUSIC_SUPPORT
  if (current_player)
    current_player->stop();

   free(song_data);
   music_handle = NULL;
   song_data    = NULL;
#endif
}

/* ------------------------------------------------------------------ */
/* Save-state hooks for music position.                               */
/*                                                                    */
/* Each music backend may optionally implement serialize/unserialize  */
/* on its music_player_t vtable.  These wrappers dispatch to whichever*/
/* backend is currently active and return 0 when no playback is in    */
/* progress or the backend doesn't implement state save.  The libretro*/
/* layer treats a 0 result as "no music state to record" -- a save    */
/* state with zero music payload restores to "music keeps running"    */
/* (status quo behaviour before this hook existed), so absence is     */
/* safe.                                                              */

/* Generic music-state wire format -- the cross-backend layer.
 *
 * Every save written by this wrapper starts with a fixed 16-byte
 * GMUS header that records only the cross-backend-meaningful state:
 * the rendered-sample count since I_PlaySong.  Any backend-specific
 * fast-path payload (e.g. 'OPLS' track positions, 'FLPS' eventpos)
 * is appended after the header.  Layout:
 *
 *   uint32_t  magic    = 'GMUS'
 *   uint32_t  version  = 1
 *   uint64_t  samples_played
 *   [optional backend payload, variable length]
 *
 * The GMUS prefix is what makes cross-backend save/load work: a
 * state written while the OPL backend was active still carries the
 * same opening header as a state written while MP3 was active, so
 * the reader can always extract the sample count.  The reader tries
 * the active backend's fast path on the appended payload first and
 * only falls through to render-replay (stop, restart, render() into
 * a throwaway buffer until samples_played catches up) if either:
 *
 *   - the active backend has no unserialize (e.g. mp_player), or
 *   - the active backend rejected the appended payload because it
 *     was written by a different backend (OPLS appended payload,
 *     read while fl_player or mp_player is active, or vice versa).
 *
 * Same-backend save/load is unchanged from the pre-wrapper world:
 * 16 bytes of GMUS overhead, then the backend's existing fast-path
 * unserialize takes over.  Cross-backend save/load is slower (the
 * render-replay walk is O(samples_played)) but correct -- the song
 * resumes at the right position instead of drifting past it.  The
 * cross-backend path is also the only available option for backends
 * whose internal state cannot be event-replayed, MP3 being the
 * motivating case (libmad's decoder state is opaque, so re-decoding
 * from the start to the saved sample count is the only way to
 * resume cleanly).
 */
#define MUSIC_GENERIC_MAGIC     0x474D5553u   /* 'GMUS' */
#define MUSIC_GENERIC_VERSION   1u
#define MUSIC_GENERIC_HDR_SIZE  16u

size_t I_MusicSerializeMaxSize(void)
{
#ifdef MUSIC_SUPPORT
   size_t total = MUSIC_GENERIC_HDR_SIZE;
   if (current_player && current_player->serialize)
   {
      size_t n = current_player->serialize(NULL, 0);
      if (n > 0)
         total += n;
   }
   return total;
#else
   return 0;
#endif
}

size_t I_MusicSerialize(void *dest, size_t cap)
{
#ifdef MUSIC_SUPPORT
   uint8_t *p = (uint8_t*)dest;
   uint32_t magic;
   uint32_t version;
   size_t   total = MUSIC_GENERIC_HDR_SIZE;

   if (!music_handle || !current_player)
      return 0;
   if (cap < total)
      return 0;

   /* GMUS header first -- the cross-backend portion every reader
    * can extract regardless of which backend was active at save
    * time.  16 bytes. */
   magic   = MUSIC_GENERIC_MAGIC;
   version = MUSIC_GENERIC_VERSION;
   memcpy(p + 0, &magic,                4);
   memcpy(p + 4, &version,              4);
   memcpy(p + 8, &music_samples_played, 8);

   /* Then the backend's fast-path payload, if any.  It writes into
    * the buffer immediately after the GMUS header; on load the
    * wrapper hands the backend exactly this slice.  A backend that
    * declines (no song active, no fast-path implemented, or its
    * payload wouldn't fit in the remaining cap) leaves the result
    * at just the 16-byte GMUS header -- a valid state on its own,
    * restored via render-replay. */
   if (current_player->serialize)
   {
      size_t n = current_player->serialize(p + total, cap - total);
      if (n > 0)
         total += n;
   }
   return total;
#else
   (void)dest; (void)cap;
   return 0;
#endif
}

int I_MusicUnserialize(const void *src, size_t size)
{
   if (size == 0)
      return 1;  /* nothing to restore is success */

#ifdef MUSIC_SUPPORT
   {
      const uint8_t *p = (const uint8_t*)src;
      uint32_t magic;
      uint32_t version;
      uint64_t target;
      short    tmpbuf[1024];   /* 512 stereo frames per replay chunk */

      if (!music_handle || !current_player)
         return 0;
      if (size < MUSIC_GENERIC_HDR_SIZE)
         return 0;

      /* GMUS header is mandatory: any save written by this wrapper
       * carries it regardless of which backend was active.  Reading
       * it gives us the sample count we'll need if the backend's
       * fast path declines (cross-backend save/load, or a backend
       * with no unserialize at all). */
      memcpy(&magic,   p + 0, 4);
      memcpy(&version, p + 4, 4);
      memcpy(&target,  p + 8, 8);
      if (magic   != MUSIC_GENERIC_MAGIC)   return 0;
      if (version != MUSIC_GENERIC_VERSION) return 0;

      /* Try the backend's fast path on the appended payload.  Same-
       * backend save/load lands here and event-replays in a few ms,
       * unchanged from the pre-wrapper world.  Cross-backend save/
       * load (e.g. OPLS payload, fl_player active) gets a rejection
       * from the backend's magic check and falls through below. */
      if (current_player->unserialize && size > MUSIC_GENERIC_HDR_SIZE)
      {
         if (current_player->unserialize(p + MUSIC_GENERIC_HDR_SIZE,
                                          size - MUSIC_GENERIC_HDR_SIZE) == 1)
            return 1;
      }

      /* Render-replay fallback using the GMUS sample count.  This
       * is the cross-backend path and the only path for backends
       * with no unserialize (mp_player).  Runahead's common case
       * (save and restore at the same sample position) short-circuits
       * here as a no-op, preserving the in-flight synth state. */
      if (target == music_samples_played)
         return 1;

      current_player->stop();
      current_player->play(music_handle, music_last_looping);
      music_samples_played = 0;

      while (music_samples_played < target)
      {
         uint64_t want  = target - music_samples_played;
         unsigned chunk = (want > 512) ? 512u : (unsigned)want;
         current_player->render(tmpbuf, chunk);
         music_samples_played += chunk;
      }
      return 1;
   }
#else
   (void)src;
   return 0;
#endif
}

int I_RegisterSong(const void* data, size_t len)
{
#if defined(MUSIC_SUPPORT)
  music_player_t *chosen_midi = NULL;
#endif
  music_handle = NULL;

#if defined(MUSIC_SUPPORT)
  /* Pick the MIDI player according to the user's "MIDI Hardware"
   * menu setting (defaults table in m_misc.c, midi_player_opts in
   * m_menu.c).  Non-MIDI inputs (e.g. MP3 lumps via mp_player)
   * are unaffected -- they go through the autodetect loop below
   * regardless of the MIDI choice. */
  switch (midi_player)
  {
     case 0: /* Off: no MIDI playback */
        chosen_midi = NULL;
        break;
     case 1: /* Adlib (OPL) */
        chosen_midi = (music_player_t *)&opl_synth_player;
        break;
#ifdef HAVE_LIBFLUIDSYNTH
     case 2: /* Fluidsynth */
        chosen_midi = (music_player_t *)&fl_player;
        break;
#endif
     default:
        chosen_midi = (music_player_t *)&opl_synth_player;
        break;
  }

  /* Now you can hear title music in deca.wad
   * http://www.doomworld.com/idgames/index.php?id=8808
   * Ability to use mp3 and ogg as inwad lump */

  if (len > 4 && memcmp(data, "MUS", 3) != 0)
  {
     /* The header has no MUS signature
      * Let's try to load this song with the music players */
     int i;
     for (i = 0; i < NUM_MUS_PLAYERS; i++)
     {
        const music_player_t *p = music_players[i];

        /* Skip MIDI players the user did not select.  Non-MIDI
         * players (e.g. mp_player) are not in this skip set, so
         * MP3-as-music streams keep working under any midi_player
         * value -- including "Off". */
        if (p == &opl_synth_player && chosen_midi != (music_player_t *)&opl_synth_player)
           continue;
#ifdef HAVE_LIBFLUIDSYNTH
        if (p == &fl_player && chosen_midi != (music_player_t *)&fl_player)
           continue;
#endif

        music_handle = p->registersong(data, len);
        if (music_handle)
        {
           current_player = (music_player_t *)p;
           break;
        }
     }
  }

  /* e6y: from Chocolate-Doom
   * Assume a MUS file and try to convert -- but only if the user
   * has a MIDI player selected.  Under "Off", we skip the whole
   * mus2mid path; the song fails to load and I_PlaySong's
   * current_player guard turns playback into a no-op. */
  if (!music_handle && chosen_midi)
  {
     int result;
     MEMFILE *instream  = mem_fopen_read(data, len);
     MEMFILE *outstream = mem_fopen_write();

     /* e6y: from chocolate-doom
      * New mus -> mid conversion code thanks to Ben Ryves <benryves@benryves.com>
      * This plays back a lot of music closer to Vanilla Doom - eg. tnt.wad map02 */
     result = mus2mid(instream, outstream);

     if (result != 0)
     {
        size_t muslen = len;
        const unsigned char *musptr = data;

        // haleyjd 04/04/10: scan forward for a MUS header. Evidently DMX was
        // capable of doing this, and would skip over any intervening data. That,
        // or DMX doesn't use the MUS header at all somehow.
        while (musptr < (const unsigned char*)data + len - sizeof(musheader))
        {
           // if we found a likely header start, reset the mus pointer to that location,
           // otherwise just leave it alone and pray.
           if (!strncmp((const char*)musptr, "MUS\x1a", 4))
           {
              mem_fclose(instream);
              instream = mem_fopen_read(musptr, muslen);
              result   = mus2mid(instream, outstream);
              break;
           }

           musptr++;
           muslen--;
        }
     }

     if (result == 0)
     {
        void *outbuf;
        size_t outbuf_len;
        mem_get_buf(outstream, &outbuf, &outbuf_len);
        music_handle = chosen_midi->registersong(outbuf, outbuf_len);
        if(music_handle)
           current_player = chosen_midi;
     }

     mem_fclose(instream);
     mem_fclose(outstream);
  }

  /* Failed to load */
  if (!music_handle)
     lprintf(LO_ERROR, "I_RegisterSong: couldn't load music song.\n");
#endif

  return !!music_handle;
}

// Is the song playing?
int I_QrySongPlaying(int handle)
{
  handle = 0;
  (void)handle;
  return looping || musicdies > gametic;
}

/* Is the currently registered music being decoded by mp_player
 * (an MP3 stream)?  Used by S_RestartMusic to decide whether a
 * MIDI-hardware change should trigger a restart -- mp_player is
 * not a MIDI player and is unaffected by the midi_player setting,
 * so its tracks must not be torn down. */
int I_MusicIsMP3(void)
{
#if defined(MUSIC_SUPPORT) && defined(HAVE_LIBMAD)
   return current_player == (music_player_t *)&mp_player;
#else
   return 0;
#endif
}

// try register external music file (not in WAD)
int I_RegisterMusicFile( const char* filename, musicinfo_t *song )
{
#ifdef MUSIC_SUPPORT
  int len = M_ReadFile(filename, (uint8_t**) &song_data);
  if (len == -1)
  {
     if (log_cb)
        log_cb(RETRO_LOG_WARN, "Couldn't read %s\n", filename);
     return 1;
  }

  if (!I_RegisterSong(song_data, len))
  {
     free(song_data);
     song_data = NULL;
     if (log_cb)
        log_cb(RETRO_LOG_WARN, "Couldn't load music from %s\n", filename);
     return 1;
  }

  song->data    = 0;
  song->handle  = 0;
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
      sin = realloc(sin, (nreq + 1) * 4);
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

void I_InitMusic(void)
{
   int i;
   for (i = 0; music_players[i]; i++)
      music_players[i]->init (SAMPLERATE);
}

void I_ShutdownMusic(void)
{
   int i;
   for (i = 0; music_players[i]; i++)
      music_players[i]->shutdown ();
}
