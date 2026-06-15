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

#include <formats/rwav.h>

/* stb_vorbis in-memory Ogg decoder (deps/stb/stb_vorbis_impl.c).  Declared
 * here rather than pulling the whole header; returns the sample count per
 * channel and a malloc'd interleaved int16 buffer, or a negative count on
 * error.  The decoder's public symbols are prb_-prefixed so they do not clash
 * with the stock stb_vorbis a statically linked RetroArch already exports. */
extern int prb_stb_vorbis_decode_memory(const unsigned char *mem, int len,
                                     int *channels, int *sample_rate,
                                     short **output);


#include "../src/i_sound.h"
#include "../src/doomstat.h"
#include "../src/dsda_hacked.h"
#include "../src/musicplayer.h"
#include "../src/flplayer.h"
#include "../src/oplplayer.h"
#include "../src/madplayer.h"
#include "../src/modplayer.h"
#include "../src/oggplayer.h"
#include "../src/libretro_midiout.h"

#include "../src/lprintf.h"
#include "../src/doomdef.h"
#include "../src/r_fps.h"
#include "../src/m_misc.h"
#include "../src/m_swap.h"
#include "../src/w_wad.h"
#include "../src/z_zone.h"

#include "../src/mus2mid.h"

/* The audio output rate is no longer a compile-time constant.  prboom's
 * music is synthesised in real time (OPL/Fluidsynth MIDI, the mod tracker,
 * Ogg/MP3 streams), so there is no single "native" rate the core must emit
 * at -- we can pick whichever supported rate best matches the host's audio
 * device and let the synths render straight to it.  Higher rates lower
 * latency, push aliasing images above the audible band, sidestep the
 * frontend resampler's low-pass smearing, and give the SFX/stream
 * resamplers finer time-domain resolution.
 *
 * snd_samplerate_output holds the rate currently in force (one of the
 * SND_SAMPLERATE_* values).  Everything that used to reference the old
 * SAMPLERATE macro now reads this variable through the alias below, so a
 * single assignment in I_SetSoundRate() retunes the whole pipeline.  It
 * defaults to 44100 so behaviour is unchanged until the core option is
 * read. */
#define SND_SAMPLERATE_32K      32000
#define SND_SAMPLERATE_44K      44100
#define SND_SAMPLERATE_48K      48000
#define SND_SAMPLERATE_96K      96000
#define SND_SAMPLERATE_MIN      SND_SAMPLERATE_32K
#define SND_SAMPLERATE_MAX      SND_SAMPLERATE_96K
#define SND_SAMPLERATE_DEFAULT  SND_SAMPLERATE_44K

int snd_samplerate_output = SND_SAMPLERATE_DEFAULT;

/* SAMPLERATE was a macro used throughout the mixer and loaders.  Keep the
 * name as a read-only alias of the runtime variable so the body of this
 * file needs no churn beyond the few spots that genuinely must size for
 * the worst case (the static mixbuffer) or re-init the synths. */
#define SAMPLERATE      (snd_samplerate_output)

/* SAMPLECOUNT_35 (samples per 35Hz tic) tracks the active rate and is only
 * the degenerate fallback when tic_vars.sample_step is 0.  The static
 * mixbuffer, however, must be sized for the worst case -- the highest
 * supported rate at the slowest tic rate -- so that switching up to 96 kHz
 * at runtime can never overrun it. */
#define SAMPLECOUNT_35      (SAMPLERATE / 35)
#define SAMPLECOUNT_35_MAX  (SND_SAMPLERATE_MAX / 35)
#define NUM_CHANNELS		32
#define BUFMUL           4
#define MIXBUFFERSIZE   (SAMPLECOUNT_35_MAX*BUFMUL)
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
/* Per-sfx 16.16 playback step at the output rate (orig_rate / SAMPLERATE),
 * recorded by the loader; samples are stored at their native rate and the
 * mixer advances through them at this step. */
static unsigned int *sfx_steps = NULL;
/* Per-sfx native (source) sample rate in Hz, retained alongside sfx_steps
 * so the step table can be recomputed for a new output rate without
 * reloading every lump (see I_RecalcSfxSteps).  Mirrors the link aliasing
 * that lengths/sfx_steps use. */
static unsigned int *sfx_orig_rate = NULL;
static int lengths_size = 0;
int snd_card = 1;
int mus_card = 0;

/* 16.16 resample step that plays a sound recorded at `rate` Hz at the
 * current output rate.  Clamped to a minimum of 1 so a (degenerate) zero
 * never stalls a channel forever. */
#define STEP_FROM_RATE(rate) \
   ((unsigned int)(((uint64_t)(rate) << 16) / (uint64_t)SAMPLERATE))

/* The global mixing buffer.
 * Basically, samples from all active internal channels
 *  are modifed and added, and stored in the buffer
 *  that is submitted to the audio device. */
int16_t mixbuffer[MIXBUFFERSIZE];

typedef struct
{
    int16_t *snd_start_ptr, *snd_end_ptr;
    unsigned int starttic;
    int sfxid;
    int leftvol, rightvol;   /* 0..127 per-channel volume scalars */
    int handle;
    /* Playback cursor: integer sample pointer plus a 16-bit fractional
     * accumulator, advanced by the 16.16 step (FRACUNIT plays at the
     * recorded rate; the raven games randomize it per sound -- vanilla
     * pitch jitter).  Splitting the position this way keeps it exact
     * for sounds of any length; a single 32-bit 16.16 position wraps
     * after 65536 samples (~1.5s) and the channel never ends. */
    int16_t *cur;
    unsigned int frac, step_fx;
} channel_t;

/* Playback-rate multiplier per vanilla pitch value: 2^((p-128)/64) in
 * 16.16, so 128 is unity and each +/-64 doubles or halves the rate.
 * Built once at init. */
static unsigned int steptable[256];

// list of possible music players
static const music_player_t *music_players[] =
{ // until some ui work is done, the order these appear is the autodetect order.
 #ifdef HAVE_LIBFLUIDSYNTH
  &fl_player, // flplayer.h
#endif
  &opl_synth_player, // oplplayer.h
  &libretro_midi_player, // libretro_midiout.h (raw MIDI to the frontend)
#ifdef HAVE_LIBMAD
  &mp_player, // madplayer.h
#endif
  &mod_player, // modplayer.h (ProTracker .MOD via pocketmod)
  &ogg_player, // oggplayer.h (Ogg Vorbis via stb_vorbis)
  NULL
};
#define NUM_MUS_PLAYERS ((int)(sizeof (music_players) / sizeof (music_player_t *) - 1))

/* Music player currently used */
music_player_t* current_player = NULL;

/* True once I_InitMusic has brought the synth backends up; gates the music
 * re-init path in I_SetSoundRate so a rate set before I_InitMusic (the
 * normal startup order: update_variables runs before I_Init) doesn't
 * shutdown/init backends that haven't been initialised yet, nor
 * double-initialise them ahead of I_InitMusic's own init pass. */
static int music_system_up = 0;

static channel_t channels[NUM_CHANNELS];


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
static void* I_SndLoadSample(const char* sfxname, int* len, unsigned int* step,
                             unsigned int* out_rate)
{
    int             out_i, sfxlump_num, sfxlump_len;
    char            sfxlump_name[20];
    const uint8_t  *sfxlump_data, *sfxlump_sound;
    int16_t        *out_data;
    uint16_t        orig_rate;

    /* Doom sfx lumps are DS<name> (DSPISTOL); Heretic uses the bare name
     * (GLDHIT, IMPAT1, ...). Build the right one. W_CheckNumForName
     * uppercases internally. */
    {
        extern dbool raven;   /* doomstat.h: heretic||hexen */
        if (raven)
            snprintf(sfxlump_name, sizeof(sfxlump_name), "%s", sfxname);
        else
            snprintf(sfxlump_name, sizeof(sfxlump_name), "DS%s", sfxname);
    }

    /* check if the sound lump exists */
    if (W_CheckNumForName(sfxlump_name) == -1)
    {
        /* ZDoom-based mods bind SNDINFO logical names directly to lumps
         * that do not follow Doom's DS<name> convention (e.g. a lump named
         * SOULSWA1, not DSSOULSWA1).  When the conventional name misses,
         * fall back to the bare sfx name so those lumps are reachable.
         * DS<name> keeps priority so an IWAD's real DSxxxx is never shadowed
         * by an unrelated bare lump of the same stem. */
        snprintf(sfxlump_name, sizeof(sfxlump_name), "%s", sfxname);
        if (W_CheckNumForName(sfxlump_name) == -1)
            return 0;
    }

    sfxlump_num = W_GetNumForName (sfxlump_name);
    sfxlump_len = W_LumpLength (sfxlump_num);

    /* DMX header is 8 bytes; need at least one PCM byte after it. */
    if (sfxlump_len < 9)
        return 0;

    sfxlump_data    = W_CacheLumpNum (sfxlump_num);

    /* Some ZDoom-based mods ship sound effects as RIFF/WAVE lumps rather
     * than the vanilla DMX format.  Detect the "RIFF" magic and decode
     * with RWAV (libretro-common formats/wav); everything else falls
     * through to the DMX path below unchanged.  Both paths produce the
     * same output contract: native-rate signed-16-bit mono PCM with one
     * extra trailing sample duplicated for the mixer's cur[1] fetch, and
     * a 16.16 *step computed from the source rate. */
    if (   sfxlump_len >= 12
        && sfxlump_data[0] == 'R' && sfxlump_data[1] == 'I'
        && sfxlump_data[2] == 'F' && sfxlump_data[3] == 'F')
    {
        rwav_t    wav;
        size_t    n;
        uint32_t  wav_rate;

        if (rwav_load(&wav, sfxlump_data, (size_t)sfxlump_len) != RWAV_ITERATE_DONE)
        {
            W_UnlockLumpNum (sfxlump_num);
            return 0;
        }

        if (wav.numsamples == 0 || wav.numchannels == 0)
        {
            rwav_free(&wav);
            W_UnlockLumpNum (sfxlump_num);
            return 0;
        }

        out_data = (int16_t*)malloc((wav.numsamples + 1) * sizeof(int16_t));
        if (!out_data)
        {
            rwav_free(&wav);
            W_UnlockLumpNum (sfxlump_num);
            return 0;
        }

        /* RWAV gives 8-bit unsigned or 16-bit signed PCM, interleaved per
         * channel.  Convert to signed-16-bit mono: 8-bit is centre-128
         * like DMX ((s - 128) << 8); multi-channel is averaged down. */
        if (wav.bitspersample == 8)
        {
            const uint8_t *src = (const uint8_t*)wav.samples;
            unsigned       ch  = wav.numchannels;
            for (n = 0; n < wav.numsamples; n++)
            {
                int acc = 0;
                unsigned c;
                for (c = 0; c < ch; c++)
                    acc += ((int)src[n * ch + c] - 128) << 8;
                out_data[n] = (int16_t)(acc / (int)ch);
            }
        }
        else /* 16-bit (rwav only ever yields 8 or 16) */
        {
            const int16_t *src = (const int16_t*)wav.samples;
            unsigned        ch = wav.numchannels;
            for (n = 0; n < wav.numsamples; n++)
            {
                int acc = 0;
                unsigned c;
                for (c = 0; c < ch; c++)
                    acc += src[n * ch + c];
                out_data[n] = (int16_t)(acc / (int)ch);
            }
        }
        out_data[wav.numsamples] = out_data[wav.numsamples - 1];

        wav_rate = wav.samplerate ? wav.samplerate : 11025;
        *step = STEP_FROM_RATE(wav_rate);
        if (out_rate) *out_rate = wav_rate;

        *len = (int)wav.numsamples;
        rwav_free(&wav);
        W_UnlockLumpNum (sfxlump_num);
        return (void *)(out_data);
    }

    /* Some ZDoom-based mods ship sound effects as Ogg Vorbis lumps.  Detect
     * the "OggS" magic and decode with stb_vorbis into the same output
     * contract as the WAV path: native-rate signed-16-bit mono PCM with one
     * duplicated trailing sample for the mixer's cur[1] fetch and a 16.16
     * *step from the source rate. */
    if (   sfxlump_len >= 4
        && sfxlump_data[0] == 'O' && sfxlump_data[1] == 'g'
        && sfxlump_data[2] == 'g' && sfxlump_data[3] == 'S')
    {
        short   *pcm    = NULL;
        int      channels = 0, rate = 0;
        int      samples;
        unsigned int ogg_rate;

        samples = prb_stb_vorbis_decode_memory(sfxlump_data, sfxlump_len,
                                           &channels, &rate, &pcm);
        if (samples <= 0 || !pcm || channels < 1)
        {
            if (pcm) free(pcm);
            W_UnlockLumpNum (sfxlump_num);
            return 0;
        }

        out_data = (int16_t*)malloc(((size_t)samples + 1) * sizeof(int16_t));
        if (!out_data)
        {
            free(pcm);
            W_UnlockLumpNum (sfxlump_num);
            return 0;
        }

        /* stb_vorbis yields interleaved signed-16 per channel; average to
         * mono to match the mixer's single-channel sample store. */
        if (channels == 1)
        {
            memcpy(out_data, pcm, (size_t)samples * sizeof(int16_t));
        }
        else
        {
            int i, c;
            for (i = 0; i < samples; i++)
            {
                int acc = 0;
                for (c = 0; c < channels; c++)
                    acc += pcm[i * channels + c];
                out_data[i] = (int16_t)(acc / channels);
            }
        }
        out_data[samples] = out_data[samples - 1];

        ogg_rate = rate ? (unsigned int)rate : 11025;
        *step = STEP_FROM_RATE(ogg_rate);
        if (out_rate) *out_rate = ogg_rate;

        *len = samples;
        free(pcm);
        W_UnlockLumpNum (sfxlump_num);
        return (void *)(out_data);
    }

    sfxlump_sound   = sfxlump_data + 8;
    sfxlump_len    -= 8;

    /* Get original sample rate from DMX header (offset 2, little-
     * endian uint16). */
    memcpy(&orig_rate, sfxlump_data+2, 2);
    orig_rate       = SHORT (orig_rate);
    if (orig_rate == 0)
        orig_rate = 11025;  /* defensive: malformed lump */

    /* Samples are kept at the lump's native rate; the mixer's per-channel
     * 16.16 stepping (added for the raven pitch jitter) resamples at mix
     * time, with linear interpolation between adjacent samples in the
     * inner loop.  Storing native-rate audio instead of upsampling to the
     * output rate at load cuts the resident sound data to a quarter
     * (11025 -> 44100 was a 4x expansion: ~28 MB for hexen's set) and
     * removes the per-lump resample pass from startup.
     *
     * Convert unsigned 8-bit PCM (centre 128) to signed 16-bit:
     * (s - 128) << 8.  One extra sample is appended duplicating the last,
     * so the mixer's interpolated fetch of cur[1] at the final sample
     * stays in bounds; the reported length excludes it. */
    out_data = (int16_t*)malloc(((size_t)sfxlump_len + 1) * sizeof(int16_t));
    if (!out_data)
    {
        W_UnlockLumpNum (sfxlump_num);
        return 0;
    }
    for (out_i = 0; out_i < sfxlump_len; out_i++)
        out_data[out_i] = (int16_t)(((int)sfxlump_sound[out_i] - 128) << 8);
    out_data[sfxlump_len] = out_data[sfxlump_len - 1];

    *step = STEP_FROM_RATE(orig_rate);
    if (out_rate) *out_rate = orig_rate;

    /* Release the cached lump back to the zone via the cache's
     * lock-count mechanism.  The previous Z_Free here freed the
     * lump's memory directly, leaving cachelump[sfxlump_num].cache
     * pointing at freed memory and the lock count non-zero -- so
     * the next W_CacheLumpNum on the same lump (e.g. on the next
     * S_Init / content load) returned the dangling pointer
     * instead of re-reading.  Use the proper unlock path. */
    W_UnlockLumpNum (sfxlump_num);

    *len = sfxlump_len;
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
   int i;

   {
      int p;
      double base = 1.0;
      /* steptable[p] = 2^((p-128)/64) * FRACUNIT without libm: walk the
       * ratio incrementally from the midpoint in both directions. */
      const double ratio = 1.0108892860517005; /* 2^(1/64) */
      steptable[128] = 1 << 16;
      for (p = 129; p < 256; p++)
      {
         base *= ratio;
         steptable[p] = (unsigned int)(base * 65536.0 + 0.5);
      }
      base = 1.0;
      for (p = 127; p >= 0; p--)
      {
         base /= ratio;
         steptable[p] = (unsigned int)(base * 65536.0 + 0.5);
      }
   }

   /* Init internal lookups (raw data, mixing buffer, channels).
    * This function sets up internal lookups used during
    * the mixing process.
    */

   /* Okay, reset internal mixing channels to zero. */
   for (i = 0; i < NUM_CHANNELS; i++)
      memset(&channels[i], 0, sizeof(channel_t));
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
    extern dbool raven;   /* doomstat.h: heretic||hexen */
    char namebuf[9];

    /* Doom names its sfx lumps DS<name> (e.g. DSPISTOL); Heretic and Hexen
     * use the bare lump name with no prefix (for Hexen the SNDINFO step has
     * already rewritten sfx->name to the real lump). W_GetNumForName
     * uppercases internally. */
    if (raven)
        snprintf(namebuf, sizeof(namebuf), "%s", sfx->name);
    else
        snprintf(namebuf, sizeof(namebuf), "ds%s", sfx->name);

    {
        int n = W_CheckNumForName(namebuf);
        if (n < 0 && !raven)
        {
            /* ZDoom mods may bind a logical name to a lump that lacks the
             * DS prefix; mirror I_SndLoadSample's bare-name fallback so this
             * presence check agrees with what the loader can actually read. */
            snprintf(namebuf, sizeof(namebuf), "%s", sfx->name);
            n = W_CheckNumForName(namebuf);
        }
        return n;
    }
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
    channels[slot].snd_start_ptr = (int16_t*)S_sfx[id].data;
    channels[slot].snd_end_ptr   = channels[slot].snd_start_ptr + lengths[id];
    channels[slot].cur           = channels[slot].snd_start_ptr;
    channels[slot].frac          = 0;
    /* Playback step = the sound's native-rate step times the pitch
     * multiplier.  The raven games honor vanilla's per-sound pitch
     * jitter always; doom only when the v1.1 pitch effects setting is
     * enabled, matching prboom's pitched_sounds.  The engine computes
     * (and draws RNG for) doom's pitch either way, exactly as vanilla
     * and prboom do, so demo sync is unaffected by the toggle. */
    channels[slot].step_fx       = (raven || pitched_sounds)
       ? (unsigned int)(((uint64_t)sfx_steps[id] * steptable[pitch & 0xff]) >> 16)
       : sfx_steps[id];
    if (!channels[slot].step_fx)
        channels[slot].step_fx = 1;   /* defensive: never a zero step */

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

    /* Store the per-channel volume scalars (0..127); the mixer scales
     * each 16-bit sample by these directly. */
    channels[slot].leftvol  = leftvol;
    channels[slot].rightvol = rightvol;

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
            channel_t *c = active[j];
            /* Remaining output frames before this channel's read cursor
             * crosses its sample end, at its playback step. */
            int64_t left_fx = (((int64_t)(c->snd_end_ptr - c->cur)) << 16) -
                              c->frac;
            int     rem     = (int)((left_fx + c->step_fx - 1) / c->step_fx);
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
               /* Linear interpolation between the sample under the read
                * cursor and its neighbor (the loader appends a sentinel
                * duplicate of the final sample, so cur[1] is always in
                * bounds).  The fraction is taken at 8 bits so the
                * multiply stays comfortably inside 32-bit arithmetic. */
               int        a = c->cur[0];
               int        s = a + ((((int)c->cur[1] - a) *
                                    (int)(c->frac >> 8)) >> 8);
               c->frac   += c->step_fx;
               c->cur    += c->frac >> 16;
               c->frac   &= 0xffff;
               /* s is signed 16-bit; leftvol/rightvol are 0..127.
                * (s * vol) >> 7 scales by vol/128, keeping full
                * precision and the int16 output range. */
               dl += (s * c->leftvol)  >> 7;
               dr += (s * c->rightvol) >> 7;
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
               if (active[j]->cur >= active[j]->snd_end_ptr)
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

         channels[i].leftvol  = leftvol;
         channels[i].rightvol = rightvol;
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

  /* Hexen indirects sfx through SNDINFO: the precache below loads each
   * sample by S_sfx[].name, so the logical names must already have been
   * rewritten to real lumps.  This runs before S_Init (which used to do it,
   * too late -- after the precache had already failed to NULL).  Also
   * records the per-map music. */
  {
    extern dbool hexen;          /* doomstat.h */
    extern void  S_HexenLoadSndInfo(void); /* s_sound.h */
    extern void  U_ZDoomLoadSndInfo(void); /* u_zsndinfo.h */
    if (hexen)
      S_HexenLoadSndInfo();
    else
      U_ZDoomLoadSndInfo();
  }

  /* lengths[] tracks the (growable) sfx count; reallocate to cover any
   * dsdhacked-added sounds. */
  if (lengths_size < num_sfx)
  {
    lengths = (int*)realloc(lengths, num_sfx * sizeof(int));
    sfx_steps = (unsigned int*)realloc(sfx_steps, num_sfx * sizeof(unsigned int));
    sfx_orig_rate = (unsigned int*)realloc(sfx_orig_rate, num_sfx * sizeof(unsigned int));
    lengths_size = num_sfx;
  }
  memset(lengths, 0, sizeof(int) * num_sfx);
  memset(sfx_steps, 0, sizeof(unsigned int) * num_sfx);
  memset(sfx_orig_rate, 0, sizeof(unsigned int) * num_sfx);

  for (i = 1; i < num_sfx; i++)
  {
     // Alias? Example is the chaingun sound linked to pistol.
     if (!S_sfx[i].link) // Load data from WAD file.
        S_sfx[i].data = I_SndLoadSample( S_sfx[i].name, &lengths[i],
                                         &sfx_steps[i], &sfx_orig_rate[i] );
     else // Previously loaded already?
     {
        S_sfx[i].data = S_sfx[i].link->data;
        /* link - S_sfx is already an element index (pointer subtraction of
         * sfxinfo_t*); do not divide by sizeof again. */
        lengths[i]       = lengths[S_sfx[i].link - S_sfx];
        sfx_steps[i]     = sfx_steps[S_sfx[i].link - S_sfx];
        sfx_orig_rate[i] = sfx_orig_rate[S_sfx[i].link - S_sfx];
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
     case 3: /* libretro raw MIDI out */
        chosen_midi = (music_player_t *)&libretro_midi_player;
        break;
#else
     case 2: /* libretro raw MIDI out (Fluidsynth not built) */
        chosen_midi = (music_player_t *)&libretro_midi_player;
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
        if (p == &libretro_midi_player && chosen_midi != (music_player_t *)&libretro_midi_player)
           continue;

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

/* The libretro raw-MIDI player declines to register a song while the
 * frontend's MIDI output is not yet available (e.g. the MIDI driver is
 * still coming up during the first frames after load).  That is exactly
 * when the very first track -- the title music -- is registered, so it
 * would stay silent forever: S_ChangeMusic latches mus_playing and never
 * retries.  This reports whether the user has the libretro MIDI player
 * selected and that output is now available, so the per-frame music
 * update can re-register a track that was deferred.  Returns 0 unless
 * the libretro player is the chosen MIDI backend and it is ready now. */
int I_MidiLibretroReady(void)
{
#if defined(MUSIC_SUPPORT)
   int is_libretro_selected;
#ifdef HAVE_LIBFLUIDSYNTH
   is_libretro_selected = (midi_player == 3);
#else
   is_libretro_selected = (midi_player == 2);
#endif
   if (!is_libretro_selected)
      return 0;
   return I_LibretroMidiAvailable();
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
   music_system_up = 1;
}

void I_ShutdownMusic(void)
{
   int i;
   for (i = 0; music_players[i]; i++)
      music_players[i]->shutdown ();
   music_system_up = 0;
}

/* Recompute every sfx's 16.16 playback step for the current output rate
 * from its retained native rate.  Cheap (one 64-bit divide per sfx, no
 * lump I/O) and safe to call at any time: a channel that is mid-playback
 * keeps its cur/frac cursor and simply advances at the new step from the
 * next mixed sample on.  Channels whose sfxid still resolves are also
 * retuned so an in-flight sound doesn't keep playing at the old pitch. */
static void I_RecalcSfxSteps(void)
{
   int i;

   if (!sfx_steps || !sfx_orig_rate)
      return;

   for (i = 0; i < num_sfx; i++)
   {
      if (sfx_orig_rate[i])
      {
         sfx_steps[i] = STEP_FROM_RATE(sfx_orig_rate[i]);
         if (!sfx_steps[i])
            sfx_steps[i] = 1;
      }
   }

   /* Retune any active channels.  step_fx may carry vanilla pitch jitter
    * (raven / pitched_sounds) baked in at I_StartSound time; we can't
    * recover the original pitch byte here, so recompute from the base
    * step only.  The audible effect of dropping a one-frame pitch jitter
    * across a rate switch is nil. */
   for (i = 0; i < NUM_CHANNELS; i++)
   {
      if (channels[i].snd_start_ptr)
      {
         unsigned int s = sfx_steps[channels[i].sfxid];
         channels[i].step_fx = s ? s : 1;
      }
   }
}

/* Set the desired audio output rate.  Clamps to a supported value.  If the
 * rate is unchanged this is a no-op.  Otherwise it retunes the SFX step
 * table in place and, if music is playing, re-inits the synth backends at
 * the new rate and resumes the current song at its saved sample position
 * (the same render-replay path the save-state code uses, which is the only
 * rate-agnostic way to resume the opaque synth/decoder state).  Returns the
 * rate in force afterwards. */
int I_SetSoundRate(int rate)
{
   int old_rate = snd_samplerate_output;

   /* Snap to the nearest supported rate so a caller passing a raw host
    * rate (e.g. 22050 or 192000) still lands on something we render at. */
   if      (rate <= (SND_SAMPLERATE_32K + SND_SAMPLERATE_44K) / 2)
      rate = SND_SAMPLERATE_32K;
   else if (rate <= (SND_SAMPLERATE_44K + SND_SAMPLERATE_48K) / 2)
      rate = SND_SAMPLERATE_44K;
   else if (rate <= (SND_SAMPLERATE_48K + SND_SAMPLERATE_96K) / 2)
      rate = SND_SAMPLERATE_48K;
   else
      rate = SND_SAMPLERATE_96K;

   if (rate == old_rate)
      return old_rate;

   snd_samplerate_output = rate;

   /* SFX: just retune; samples are stored at native rate. */
   I_RecalcSfxSteps();

#ifdef MUSIC_SUPPORT
   /* Music: re-init every synth backend at the new rate so their internal
    * rate-derived tables (OPL envelope/LFO scaling via Chip__Setup, the
    * fluidsynth synth.sample-rate, the stream resampler steps, etc.) are
    * rebuilt for the new rate.
    *
    * We deliberately do NOT resume the song from here.  Per-song,
    * rate-derived state lives in each backend's registersong() (the libretro
    * MIDI player's samples-per-MIDI-clock lm_spmc, fluidsynth's spmc, the
    * ogg resampler's ogg_step, OPL's per-track timing), so a bare
    * shutdown()/init()/play() would leave that state stale -> wrong tempo or
    * pitch.  The correct, backend-agnostic resume is a full re-registration
    * from the song lump, which the s_sound.c layer owns: the libretro layer
    * calls S_RestartMusic() after this returns.
    *
    * For backends whose rate state is fully re-derived per render call and
    * not at registersong (the MP3 resampler reads mp_samplerate_target live;
    * the mod renderer reads mod_rate live), S_RestartMusic's early-out for
    * non-lump tracks is harmless -- they simply keep decoding and pick up the
    * new rate on the next render.
    *
    * Skipped until I_InitMusic has run (normal startup order sets the rate
    * first), since I_InitMusic will init the backends at the new rate itself.
    *
    * NOTE: backends own the lifetime of the parsed song handle across
    * shutdown(); we leave music_handle/current_player untouched so the
    * non-lump backends above keep a valid handle.  S_RestartMusic replaces
    * both for the lump-backed case via I_UnRegisterSong + I_RegisterSong. */
   if (music_system_up)
   {
      int i;
      for (i = 0; music_players[i]; i++)
      {
         music_players[i]->shutdown();
         music_players[i]->init(SAMPLERATE);
      }
   }
#endif

   if (log_cb)
      log_cb(RETRO_LOG_INFO, "I_SetSoundRate: output rate %d Hz\n", rate);

   return rate;
}
