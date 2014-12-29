#include "libretro.h"

/* prboom includes */

#include "../src/d_main.h"
#include "../src/doomdef.h"
#include "../src/m_fixed.h"
#include "../src/r_fps.h"
#include "../src/m_argv.h"
#include "../src/m_misc.h"
#include "../src/i_system.h"
#include "../src/i_sound.h"
#include "../src/v_video.h"
#include "../src/st_stuff.h"
#include "../src/z_zone.h"
#include "../src/m_swap.h"
#include "../src/w_wad.h"
#include "../src/r_draw.h"
#include "../src/musicplayer.h"
#include "../src/madplayer.h"

#include <sys/stat.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <math.h>
#include <errno.h>

/* PS3 values for i_sound.h - check if correct for libretro */
#define SAMPLECOUNT_35		((4 * 11025) / 35)
#define SAMPLECOUNT_40		((4 * 11025) / 40)
#define SAMPLECOUNT_50		((4 * 11025) / 50)
#define SAMPLECOUNT_60		((4 * 11025) / 60)
#define NUM_CHANNELS		32
#define BUFMUL                  4
#define MIXBUFFERSIZE		(SAMPLECOUNT_35*BUFMUL)
#define MAX_CHANNELS    32

//i_system
int ms_to_next_tick;

static bool use_audio_cb;

//i_video
static unsigned char screen_buf[2 * 320 * 200];

//i_sound
int 		lengths[NUMSFX];
int snd_card = 1;
int mus_card = 0;
int snd_samplerate= 11025;
int use_doublebuffer = 1;

typedef struct {
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
    byte *snd_start_ptr, *snd_end_ptr;
    unsigned int starttic;
    int sfxid;
    int *leftvol, *rightvol;
    int handle;
} channel_t;

static channel_t channels[NUM_CHANNELS];

int		vol_lookup[128*256];


/* libretro */
static char g_wad_dir[1024];
static char g_basename[1024];

//forward decls
extern void D_DoomMainSetup(void);
extern void D_DoomLoop(void);
extern void M_QuitDOOM(int choice);
void I_PreInitGraphics(void);
void D_DoomDeinit(void);
void I_SetRes(void);
void I_UpdateSound(void);
void M_EndGame(int choice);
extern int gametic;
extern int snd_SfxVolume;
extern int snd_MusicVolume;

static retro_log_printf_t log_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_environment_t environ_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

static void check_system_specs(void)
{
   unsigned level = 4;
   environ_cb(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL, &level);
}

void retro_init(void)
{
   enum retro_pixel_format rgb565;
   struct retro_log_callback log;

   if(environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = NULL;
#ifdef MUSIC_SUPPORT
   mp_player.init(44100);
#endif

#ifdef FRONTEND_SUPPORTS_RGB565
   rgb565 = RETRO_PIXEL_FORMAT_RGB565;
   if(environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &rgb565) && log_cb)
      log_cb(RETRO_LOG_INFO, "Frontend supports RGB565 - will use that instead of XRGB1555.\n");
#endif
   check_system_specs();
}

void retro_deinit(void)
{
#ifdef MUSIC_SUPPORT
   mp_player.shutdown();
#endif
   D_DoomDeinit();
}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
   (void)port;
   (void)device;
}

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "prboom";
   info->library_version  = "v2.5.0";
   info->need_fullpath    = false;
   info->valid_extensions = "wad|iwad";
   info->block_extract = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   info->timing.fps = 60.0;
   info->timing.sample_rate = 44100.0;
   info->geometry.base_width = 320;
   info->geometry.base_height = 200;
   info->geometry.max_width = 320;
   info->geometry.max_height = 200;
   info->geometry.aspect_ratio = 4.0 / 3.0;
}


void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
   audio_cb = cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
   input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
   input_state_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}

void retro_reset(void)
{
   M_EndGame(0);
}

void retro_shutdown_prboom(void)
{
   environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
}

static void update_variables(void)
{
}

void retro_run(void)
{
   bool updated = false;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      update_variables();

   D_DoomLoop();
   if (!use_audio_cb)
      I_UpdateSound();
}

static void extract_basename(char *buf, const char *path, size_t size)
{
   const char *base = strrchr(path, '/');
   if (!base)
      base = strrchr(path, '\\');
   if (!base)
      base = path;

   if (*base == '\\' || *base == '/')
      base++;

   strncpy(buf, base, size - 1);
   buf[size - 1] = '\0';
}

static void extract_directory(char *buf, const char *path, size_t size)
{
   char *base;
   strncpy(buf, path, size - 1);
   buf[size - 1] = '\0';

   base = strrchr(buf, '/');
   if (!base)
      base = strrchr(buf, '\\');

   if (base)
      *base = '\0';
   else
   {
      buf[0] = '.';
      buf[1] = '\0';
   }
}

static void audio_set_state(bool enable)
{
   (void)enable;
}

bool retro_load_game(const struct retro_game_info *info)
{
   struct retro_audio_callback cb = { I_UpdateSound, audio_set_state };
   int argc = 0;
   static char *argv[32] = {NULL};

   struct retro_input_descriptor desc[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Strafe" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Use" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Fire" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Run" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "Strafe Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "Strafe Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,     "Previous Weapon" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "Next Weapon" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,    "Show/Hide Map" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Settings" },

      { 0 },
   };

   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);

   update_variables();

   extract_directory(g_wad_dir, info->path, sizeof(g_wad_dir));
   extract_basename(g_basename, info->path, sizeof(g_basename));


   argv[argc++] = strdup("prboom");
   if(info->path)
   {
      argv[argc++] = strdup("-iwad");
      argv[argc++] = strdup(g_basename);
   }

   myargc = argc;
   myargv = argv;

  Z_Init(); /* 1/18/98 killough: start up memory stuff first */

  /* cphipps - call to video specific startup code */
  I_PreInitGraphics();

  D_DoomMainSetup();

  use_audio_cb = environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_CALLBACK, &cb);
  return true;
}


void retro_unload_game(void)
{
}

unsigned retro_get_region(void)
{
   return RETRO_REGION_NTSC;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
   (void)type;
   (void)info;
   (void)num;
   return false;
}

size_t retro_serialize_size(void)
{
   return 0;
}

bool retro_serialize(void *data_, size_t size)
{
   return false;
}

bool retro_unserialize(const void *data_, size_t size)
{
   return false;
}

void *retro_get_memory_data(unsigned id)
{
   (void)id;
   return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
   (void)id;
   return 0;
}

void retro_cheat_reset(void)
{}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   (void)index;
   (void)enabled;
   (void)code;
}

/* i_video */

static int action_lut[] = { 
	KEYD_RALT,         /* RETRO_DEVICE_ID_JOYPAD_B */
        KEYD_RSHIFT,       /* RETRO DEVICE_ID_JOYPAD_Y */
        KEYD_TAB,          /* RETRO_DEVICE_ID_JOYPAD_SELECT */
        KEYD_ESCAPE,       /* RETRO_DEVICE_ID_JOYPAD_START */
        KEYD_UPARROW,      /* RETRO_DEVICE_ID_JOYPAD_UP */
        KEYD_DOWNARROW,    /* RETRO_DEVICE_ID_JOYPAD_DOWN */
        KEYD_LEFTARROW,    /* RETRO_DEVICE_ID_JOYPAD_LEFT */
        KEYD_RIGHTARROW,   /* RETRO_DEVICE_ID_JOYPAD_RIGHT */
        KEYD_SPACEBAR,     /* RETRO_DEVICE_ID_JOYPAD_A */
        KEYD_RCTRL,        /* RETRO_DEVICE_ID_JOYPAD_X */
        ',',               /* RETRO_DEVICE_ID_JOYPAD_L1 */
        '.',               /* RETRO_DEVICE_ID_JOYPAD_R1 */
        'n',               /* RETRO_DEVICE_ID_JOYPAD_L2 */ 
        'm',               /* RETRO_DEVICE_ID_JOYPAD_R2 */
};

void I_StartTic (void)
{
   unsigned i;
   static bool old_input[20];
   bool new_input[20];
   event_t analog_event = {0};
   int analog_l_x, analog_l_y, analog_r_x;

   input_poll_cb();

   for(i = 0; i < 14; i++)
   {
      event_t event = {0};
      new_input[i] = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, i);

      if(new_input[i] && !old_input[i])
      {
         event.type = ev_keydown;
         event.data1 = action_lut[i];
      }

      if(!new_input[i] && old_input[i])
      {
         event.type = ev_keyup;
         event.data1 = action_lut[i];
      }

      if(event.type == ev_keydown || event.type == ev_keyup)
         D_PostEvent(&event);

      old_input[i] = new_input[i];
   }
}

static void I_UpdateVideoMode(void)
{
   if (log_cb)
      log_cb(RETRO_LOG_INFO, "I_UpdateVideoMode: %dx%d\n", SCREENWIDTH, SCREENHEIGHT);

  V_InitMode();
  V_DestroyUnusedTrueColorPalettes();
  V_FreeScreens();

  I_SetRes();

  screens[0].not_on_heap = true;
  screens[0].data = (unsigned char *)screen_buf;

  V_AllocScreens();

  R_InitBuffer(SCREENWIDTH, SCREENHEIGHT);
}

void I_FinishUpdate (void)
{
  video_cb(screen_buf, SCREENWIDTH, SCREENHEIGHT, SCREENPITCH);
}

void I_SetPalette (int pal)
{
}

void I_InitGraphics(void)
{
  static int    firsttime=1;

  if (firsttime)
  {
    firsttime = 0;

    if (log_cb)
       log_cb(RETRO_LOG_INFO, "I_InitGraphics: %dx%d\n", SCREENWIDTH, SCREENHEIGHT);

    /* Set the video mode */
    I_UpdateVideoMode();

  }
}

void I_PreInitGraphics(void)
{
}

void I_SetRes(void)
{
  int i;

  for (i=0; i<3; i++)
    screens[i].height = SCREENHEIGHT;

  screens[4].height = (ST_SCALED_HEIGHT+1);

  if (log_cb)
     log_cb(RETRO_LOG_INFO, "I_SetRes: Using resolution %dx%d\n", SCREENWIDTH, SCREENHEIGHT);
}

/* i_system - i_main */

static boolean InDisplay = false;

boolean I_StartDisplay(void)
{
  if (InDisplay)
    return false;

  InDisplay = true;
  return true;
}

void I_EndDisplay(void)
{
  InDisplay = false;
}

/*
 * I_GetRandomTimeSeed
 *
 */
unsigned long I_GetRandomTimeSeed(void)
{
	return 0;
}

#ifdef PRBOOM_SERVER

/* cphipps - I_SigString
 * Returns a string describing a signal number
 */
const char* I_SigString(char* buf, size_t sz, int signum)
{
#ifdef HAVE_SNPRINTF
    snprintf(buf,sz,"signal %d",signum);
#else
    sprintf(buf,"signal %d",signum);
#endif
  return buf;
}

#else

const char *I_DoomExeDir(void)
{
  return g_wad_dir;
}

/*
 * HasTrailingSlash
 *
 * cphipps - simple test for trailing slash on dir names
 */

boolean HasTrailingSlash(const char* dn)
{
  return ( (dn[strlen(dn)-1] == '/') || (dn[strlen(dn)-1] == '\\'));
}

/*
 * I_FindFile
 *
 * proff_fs 2002-07-04 - moved to i_system
 *
 * cphipps 19/1999 - writen to unify the logic in FindIWADFile and the WAD
 *      autoloading code.
 * Searches g_wad_dir for a named WAD file
 */

char* I_FindFile(const char* wfname, const char* ext)
{
  FILE *file;
  size_t pl;
  char  * p;
  char slash;

  /* Precalculate a length we will need in the loop */
  pl = strlen(wfname) + strlen(ext) + 4;

  if (log_cb)
     log_cb(RETRO_LOG_INFO, "wfname: [%s], g_wad_dir: [%s]\n", wfname, g_wad_dir);

  p = malloc(strlen(g_wad_dir) + pl);
#ifdef _WIN32
  slash = '\\';
#else
  slash = '/';
#endif
  if (log_cb)
     log_cb(RETRO_LOG_INFO, "%s%c%s\n", g_wad_dir, slash, wfname);
  sprintf(p, "%s%c%s", g_wad_dir, slash, wfname);

  file = fopen(p, "rb");
  if (!file)
  {
	  strcat(p, ext);
	  file = fopen(p, "rb");
  }

  if (file)
  {
     if (log_cb)
        log_cb(RETRO_LOG_INFO, " found %s\n", p);
	  fclose(file);
	  return p;
  }

  free(p);
  return NULL;
}

#endif


/*
 * I_Filelength
 *
 * Return length of an open file.
 */

int I_Filelength(int handle)
{
  struct stat   fileinfo;
  if (fstat(handle,&fileinfo) == -1)
    I_Error("I_Filelength: %s",strerror(errno));
  return fileinfo.st_size;
}

void I_Init(void)
{
  /* killough 2/21/98: avoid sound initialization if no sound & no music */
  if (!(nomusicparm && nosfxparm))
    I_InitSound();

#ifndef __LIBRETRO__
  R_InitInterpolation();
#endif
}

extern void D_Doom_Deinit(void);

/* i_sound */
static void I_SndMixResetChannel (int channum)
{
    memset (&channels[channum], 0, sizeof(channel_t));
}

//
// This function loads the sound data from the WAD lump
// for a single sound effect.
//
static void* I_SndLoadSample (const char* sfxname, int* len)
{
    int i, x, padded_sfx_len, sfxlump_num, sfxlump_len;
    char sfxlump_name[20];
    byte *sfxlump_data, *sfxlump_sound, *padded_sfx_data;
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
    
    padded_sfx_len = ((sfxlump_len*ceil(times) + (SAMPLECOUNT_35-1)) / SAMPLECOUNT_35) * SAMPLECOUNT_35;
    padded_sfx_data = (byte*)malloc(padded_sfx_len);
    
    for (i=0; i < padded_sfx_len; i++)
    {
        x = floor ((float)i/times);
        
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

void I_SetChannels()
{
    // Init internal lookups (raw data, mixing buffer, channels).
    // This function sets up internal lookups used during
    //  the mixing process. 
  
    int i, j;
  
    // Okay, reset internal mixing channels to zero.
    for (i=0; i<NUM_CHANNELS; i++)
        I_SndMixResetChannel(i);

    // Generates volume lookup tables which also turn the unsigned
    // samples into signed samples.
    for (i=0 ; i<128 ; i++)
    {
        for (j=0 ; j<256 ; j++)
            vol_lookup[i*256+j] = (i*(j-128)*256)/127;
    }
    
    return;
}	

 
void I_SetSfxVolume(int volume)
{
    snd_SfxVolume = volume;
}

void I_SetMusicVolume(int volume)
{
    snd_MusicVolume = volume;
#ifdef MUSIC_SUPPORT
    mp_player.setvolume(volume);
#endif
}

//
// Retrieve the raw data lump index
//  for a given SFX name.
//
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
    
    return;
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
    channels[slot].snd_start_ptr = (byte *)S_sfx[id].data;
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
   byte sample;
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
      mp_player.render(mad_audio_buf, out_frames);
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

   return;
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
 
    return;
}


void I_ShutdownSound(void)
{    
    return;
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

    return;
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
  // UNUSED.
  handle = 0;
  musicdies = gametic + TICRATE*30; // ?

#ifdef MUSIC_SUPPORT
  mp_player.play(music_handle, looping);
  mp_player.setvolume(snd_MusicVolume);
#endif
}

void I_PauseSong (int handle)
{
  // UNUSED.
  handle = 0;
#ifdef MUSIC_SUPPORT

  mp_player.pause();
#endif
}

void I_ResumeSong (int handle)
{
  // UNUSED.
  handle = 0;

#ifdef MUSIC_SUPPORT
  mp_player.resume();
#endif
}

void I_StopSong(int handle)
{
  // UNUSED.
  handle = 0;
  
  looping = 0;
  musicdies = 0;

#ifdef MUSIC_SUPPORT
  mp_player.stop();
#endif
}

void I_UnRegisterSong(int handle)
{
  // UNUSED.
  handle = 0;

#ifdef MUSIC_SUPPORT
  mp_player.unregistersong(music_handle);
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
  // UNUSED.
  handle = 0;
  return looping || musicdies > gametic;
}

#ifdef MUSIC_SUPPORT
static int RegisterSong(const void *data, size_t len)
{
   music_handle = mp_player.registersong(data, len);
   return !!music_handle;
}
#endif

int I_RegisterMusic( const char* filename, musicinfo_t *song )
{
  int len;

#ifdef MUSIC_SUPPORT
  if (log_cb)
     log_cb(RETRO_LOG_INFO, "RegisterMusic: %s\n", filename);

  len = M_ReadFile(filename, (byte **) &song_data);
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

// NSM helper routine for some of the streaming audio
void I_ResampleStream (void *dest, unsigned nsamp, void (*proc) (void *dest, unsigned nsamp), unsigned sratein, unsigned srateout)
{ // assumes 16 bit signed interleaved stereo
  
  unsigned i;
  int j = 0;
  
  short *sout = dest;
  
  static short *sin = NULL;
  static unsigned sinsamp = 0;

  static unsigned remainder = 0;
  unsigned step = (sratein << 16) / (unsigned) srateout;

  unsigned nreq = (step * nsamp + remainder) >> 16;

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

void I_Read(int fd, void* vbuf, size_t sz)
{
  unsigned char* buf = vbuf;

  while (sz) {
    int rc = read(fd,buf,sz);
    if (rc <= 0) {
      I_Error("I_Read: read failed: %s", rc ? strerror(errno) : "EOF");
    }
    sz -= rc; buf += rc;
  }
}
