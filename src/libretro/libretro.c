#define LIBRETRO_CORE 1
#include "config.h"
#include "libretro.h"

/* prboom includes */

#include "d_main.h"
#include "lprintf.h"
#include "doomtype.h"
#include "doomdef.h"
#include "lprintf.h"
#include "m_fixed.h"
#include "r_fps.h"
#include "m_argv.h"
#include "m_misc.h"
#include "i_system.h"
#include "i_sound.h"
#include "v_video.h"
#include "st_stuff.h"
#include "z_zone.h"
#include "m_swap.h"
#include "w_wad.h"
#include "r_draw.h"

#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

/* PS3 values for i_sound.h - check if correct for libretro */
#define SAMPLECOUNT		(32000 / 35)
#define NUM_CHANNELS		32
#define BUFMUL                  4
#define MIXBUFFERSIZE		(SAMPLECOUNT*BUFMUL)
#define MAX_CHANNELS    32

//i_system
int ms_to_next_tick;

//i_video
static unsigned char screen_buf[2 * 320 * 200];

//i_sound
int 		lengths[NUMSFX];
int snd_card = 1;
int mus_card = 0;
int snd_samplerate= 32000;
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

//forward decls
extern void D_DoomMainSetup(void);
extern void D_DoomLoop(void);
extern void M_QuitDOOM(int choice);
void I_PreInitGraphics(void);
void D_DoomDeinit(void);
void I_SetRes(void);
extern int gametic;
extern int snd_SfxVolume;
extern int snd_MusicVolume;

void retro_init(void)
{
}

void retro_deinit(void)
{
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
   info->valid_extensions = ".WAD|.wad|.IWAD|.iwad"; // Anything is fine, we don't care.
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   info->timing = (struct retro_system_timing) {
      .fps = 35.0,
      .sample_rate = 32000.0,
   };

   info->geometry = (struct retro_game_geometry) {
      .base_width   = 320,
      .base_height  = 200,
      .max_width    = 320,
      .max_height   = 200,
      .aspect_ratio = 4.0 / 3.0,
   };
}

static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_environment_t environ_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

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
}

void retro_run(void)
{
   D_DoomLoop();
   I_UpdateSound();
}

static void extract_directory(char *buf, const char *path, size_t size)
{
   strncpy(buf, path, size - 1);
   buf[size - 1] = '\0';

   char *base = strrchr(buf, '/');
   if (!base)
      base = strrchr(buf, '\\');

   if (base)
      *base = '\0';
   else
      buf[0] = '\0';
}

bool retro_load_game(const struct retro_game_info *info)
{
   int argc = 0;
   char vbuf[200];
   char *argv[32] = {NULL};

   extract_directory(g_wad_dir, info->path, sizeof(g_wad_dir));

   argv[argc++] = strdup("prboom");
   if(info->path)
   {
      argv[argc++] = strdup("-iwad");
      argv[argc++] = strdup(info->path);
   }

   myargc = argc;
   myargv = argv;

  /* Version info */
  lprintf(LO_INFO,"\n");
  lprintf(LO_INFO,"%s\n",I_GetVersionString(vbuf,200));

  Z_Init(); /* 1/18/98 killough: start up memory stuff first */

  /* cphipps - call to video specific startup code */
  I_PreInitGraphics();

  D_DoomMainSetup();
  return true;
}


void retro_unload_game(void)
{
   D_DoomDeinit();
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
   static bool old_input[14];
   bool new_input[14];

   input_poll_cb();

   for(unsigned i = 0; i < 14; i++)
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
  lprintf(LO_INFO, "I_UpdateVideoMode: %dx%d\n", SCREENWIDTH, SCREENHEIGHT);

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

    lprintf(LO_INFO, "I_InitGraphics: %dx%d\n", SCREENWIDTH, SCREENHEIGHT);

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

  lprintf(LO_INFO,"I_SetRes: Using resolution %dx%d\n", SCREENWIDTH, SCREENHEIGHT);
}

/* i_system - i_main */

static struct timeval start;
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

static uint32_t GetTicks(void)
{
   uint32_t ticks;
   struct timeval now;
   gettimeofday(&now, NULL);
   ticks = (now.tv_sec - start.tv_sec) * 1000 + (now.tv_usec - start.tv_usec) / 1000;
   return (ticks);
}

static void Delay(uint32_t ms)
{
    int was_error;

    struct timeval tv;
    uint32_t then, now, elapsed;

    /* Set the timeout interval */
    then = GetTicks();
    do {
        errno = 0;

        /* Calculate the time interval left (in case of interrupt) */
        now = GetTicks();
        elapsed = (now - then);
        then = now;
        if (elapsed >= ms) {
            break;
        }
        ms -= elapsed;
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;

        was_error = select(0, NULL, NULL, NULL, &tv);
    }
    while (was_error && (errno == EINTR));
}

void I_uSleep(unsigned long usecs)
{
    Delay(usecs/1000);
}


static uint32_t StartTicks(void)
{
   gettimeofday(&start, NULL);
}



int I_GetTime_RealTime (void)
{
    struct timeval	tp;
    struct timezone	tzp;
    int			newtics;
    static int		basetime=0;
  
    gettimeofday(&tp, &tzp);
    if (!basetime)
	basetime = tp.tv_sec;
    newtics = (tp.tv_sec-basetime)*TICRATE + tp.tv_usec*TICRATE/1000000;
    return newtics;
}

/*
 * I_GetRandomTimeSeed
 *
 * CPhipps - extracted from G_ReloadDefaults because it is O/S based
 */
unsigned long I_GetRandomTimeSeed(void)
{
  /* killough 3/26/98: shuffle random seed, use the clock */ 
  struct timeval tv;
  struct timezone tz;
  gettimeofday(&tv,&tz);
  return (tv.tv_sec*1000ul + tv.tv_usec/1000ul);
}

/* cphipps - I_GetVersionString
 * Returns a version string in the given buffer
 */
const char* I_GetVersionString(char* buf, size_t sz)
{
#ifdef HAVE_SNPRINTF
  snprintf(buf,sz,"%s v%s (http://prboom.sourceforge.net/)",PACKAGE,VERSION);
#else
  sprintf(buf,"%s v%s (http://prboom.sourceforge.net/)",PACKAGE,VERSION);
#endif
  return buf;
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

fixed_t I_GetTimeFrac (void)
{
  unsigned long now;
  fixed_t frac;

  now = GetTicks();

  if (tic_vars.step == 0)
    return FRACUNIT;
  else
  {
    frac = (fixed_t)((now - tic_vars.start) * FRACUNIT / tic_vars.step);
    if (frac < 0)
      frac = 0;
    if (frac > FRACUNIT)
      frac = FRACUNIT;
    return frac;
  }
}

void I_GetTime_SaveMS(void)
{
  if (!movement_smooth)
    return;

  tic_vars.start = GetTicks();
  tic_vars.next = (unsigned int) ((tic_vars.start * tic_vars.msec + 1.0f) / tic_vars.msec);
  tic_vars.step = tic_vars.next - tic_vars.start;
}

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
  return ( (dn[strlen(dn)-1] == '/'));
}

/*
 * I_FindFile
 *
 * proff_fs 2002-07-04 - moved to i_system
 *
 * cphipps 19/1999 - writen to unify the logic in FindIWADFile and the WAD
 *      autoloading code.
 * Searches the standard dirs for a named WAD file
 * The dirs are listed at the start of the function
 */

char* I_FindFile(const char* wfname, const char* ext)
{
lprintf(LO_ALWAYS, "wfname: %s\n", wfname);
  // lookup table of directories to search
  static const struct {
    const char *dir; // directory
    const char *sub; // subdirectory
    const char *env; // environment variable
    const char *(*func)(void); // for I_DoomExeDir
  } search[] = {
    {g_wad_dir},
  };

  int   i;
  /* Precalculate a length we will need in the loop */
  size_t  pl = strlen(wfname) + strlen(ext) + 4;

  for (i = 0; i < sizeof(search)/sizeof(*search); i++) {
    char  * p;
    const char  * d = NULL;
    const char  * s = NULL;
    /* Each entry in the switch sets d to the directory to look in,
     * and optionally s to a subdirectory of d */
    // switch replaced with lookup table
    d = g_wad_dir;
    lprintf(LO_ALWAYS, "d: %s\n", d);
    s = search[i].sub;

    p = malloc((d ? strlen(d) : 0) + (s ? strlen(s) : 0) + pl);
    sprintf(p, wfname);
    lprintf(LO_ALWAYS, "p: %s\n", p);

    if (access(p,F_OK))
      strcat(p, ext);
    if (!access(p,F_OK)) {
      lprintf(LO_INFO, " found %s\n", p);
      return p;
    }
    free(p);
  }
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

static int I_GetTime_Error(void)
{
  I_Error("I_GetTime_Error: GetTime() used before initialization");
  return 0;
}

int (*I_GetTime)(void) = I_GetTime_Error;

void I_Init(void)
{
  I_GetTime = I_GetTime_RealTime;

  {
    /* killough 2/21/98: avoid sound initialization if no sound & no music */
    if (!(nomusicparm && nosfxparm))
      I_InitSound();
  }

  R_InitInterpolation();
}

// killough 2/22/98: Add support for ENDBOOM, which is PC-specific

static void PrintVer(void)
{
  char vbuf[200];
  lprintf(LO_INFO,"%s\n",I_GetVersionString(vbuf,200));
}

extern void D_Doom_Deinit(void);

/* i_sound */
static void I_SndMixResetChannel (int channum)
{
    memset (&channels[channum], 0, sizeof(channel_t));

    return;
}


//
// This function loads the sound data from the WAD lump
// for a single sound effect.
//
static void* I_SndLoadSample (const char* sfxname, int* len)
{
    int i;
    int sfxlump_num, sfxlump_len;
    char sfxlump_name[20];
    byte *sfxlump_data, *sfxlump_sound;
    byte *padded_sfx_data;
    uint16_t orig_rate;
    int padded_sfx_len;
    float times;
    int x;
    
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
    
    padded_sfx_len = ((sfxlump_len*ceil(times) + (SAMPLECOUNT-1)) / SAMPLECOUNT) * SAMPLECOUNT;
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
    return;
}

void I_SetMusicVolume(int volume)
{
    snd_MusicVolume = volume;
    return;
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
    
    //sys_lwmutex_lock (&chanmutex, 0);

    for (i=0; i<NUM_CHANNELS; i++)
    {
        if (channels[i].handle==handle)
        {
            I_SndMixResetChannel(i);
            //sys_lwmutex_unlock (&chanmutex);
            return;
        }
    }
    
    //sys_lwmutex_unlock (&chanmutex);
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
    int	i;
    
    int	oldestslot, oldesttics;
    int	slot;

    int	rightvol;
    int	leftvol;

    // this effect was not loaded.
    if (!S_sfx[id].data)
        return -1;

    //sys_lwmutex_lock (&chanmutex, 0);

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

    //sys_lwmutex_unlock (&chanmutex);

    return currenthandle;
}


boolean I_SoundIsPlaying (int handle)
{
    int i;
    
    //sys_lwmutex_lock (&chanmutex, 0);

    for (i=0; i<NUM_CHANNELS; i++)
    {
        if (channels[i].handle==handle)
        {
            //sys_lwmutex_unlock (&chanmutex);
            return 1;
        }
    }

    //sys_lwmutex_unlock (&chanmutex);
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

void I_UpdateSound(void)
{
    // Mix current sound data. Data, from raw sound, for right and left.
    byte sample;
    int dl, dr;
    int frames;

    // Pointers in global mixbuffer, left, right, end.
    int16_t *leftout, *rightout, *leftend;

    // Step in mixbuffer, left and right, thus two.
    int step;

    // Mixing channel index.
    int chan;

    // Left and right channel are in global mixbuffer, alternating.
    leftout = mixbuffer;
    rightout = mixbuffer+1;
    step = 2;

    // Determine end, for left channel only (right channel is implicit).
    leftend = mixbuffer + SAMPLECOUNT*step;

    // Mix sounds into the mixing buffer.
    // Loop over step*SAMPLECOUNT, that is 512 values for two channels.

    while (leftout <= leftend)
    {
        // Reset left/right value.
	dl = 0;
	dr = 0;


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
    }

    //fprintf(stderr, "AUDIO!\n");
    audio_batch_cb(mixbuffer, SAMPLECOUNT);

    return;
}

#if 0
static uint32_t playOneBlock(u64 *readIndex, float *audioDataStart)
{
    static uint64_t audio_block_index=1;
    uint64_t current_block = *readIndex;
    float *buf;

    if (audio_block_index == current_block)
        return 0;

    buf = audioDataStart + 2 /*channelcount*/ * AUDIO_BLOCK_SAMPLES * audio_block_index;

    I_UpdateSound();

    for (int i = 0; i < SAMPLECOUNT*2; i++)
        buf[i] = (float)mixbuffer[i]/32767.0f;

    audio_block_index = (audio_block_index + 1) % AUDIO_BLOCK_8;

    return 1;
}

static void mix_thread_func (uint64_t arg)
{
    for (;;)
    {
        usleep (20);

        sys_lwmutex_lock (&chanmutex, 0);

        playOneBlock((u64*)(u64)ps3_audio_port_cfg.readIndex,
                     (float*)(u64)ps3_audio_port_cfg.audioDataStart);

        sys_lwmutex_unlock (&chanmutex);
    }
 
    return;
}
#endif

void I_UpdateSoundParams (int handle, int vol, int sep, int pitch)
{
    int rightvol;
    int	leftvol;
    int i;

    // sys_lwmutex_lock (&chanmutex, 0);
    
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

            //sys_lwmutex_unlock (&chanmutex);
            return;
        }
    }
 
    //sys_lwmutex_unlock (&chanmutex);
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
  
    printf ("I_InitSound: \n");

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
//
void I_InitMusic(void)		{ }
void I_ShutdownMusic(void)	{ }

static int	looping=0;
static int	musicdies=-1;

void I_PlaySong(int handle, int looping)
{
  // UNUSED.
  handle = looping = 0;
  musicdies = gametic + TICRATE*30;
}

void I_PauseSong (int handle)
{
  // UNUSED.
  handle = 0;
}

void I_ResumeSong (int handle)
{
  // UNUSED.
  handle = 0;
}

void I_StopSong(int handle)
{
  // UNUSED.
  handle = 0;
  
  looping = 0;
  musicdies = 0;
}

void I_UnRegisterSong(int handle)
{
  // UNUSED.
  handle = 0;
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

int I_RegisterMusic( const char* filename, musicinfo_t *song )
{
  return 1;
}
