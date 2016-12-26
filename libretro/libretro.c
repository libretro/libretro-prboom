#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif
#include <errno.h>

#include "libretro.h"

/* prboom includes */

#include "../src/d_main.h"
#include "../src/m_fixed.h"
#include "../src/m_argv.h"
#include "../src/i_system.h"
#include "../src/i_sound.h"
#include "../src/v_video.h"
#include "../src/st_stuff.h"
#include "../src/w_wad.h"
#include "../src/r_draw.h"
#include "../src/lprintf.h"

void I_MPPlayer_Init(void);
void I_MPPlayer_Free(void);

//i_system
int ms_to_next_tick;
int mus_opl_gain = 50; // NSM  fine tune OPL output level

int SCREENWIDTH  = 320;
int SCREENHEIGHT = 200;

//i_video
static unsigned char *screen_buf;

/* libretro */
static char g_wad_dir[1024];
static char g_basename[1024];

//forward decls
bool D_DoomMainSetup(void);
void D_DoomLoop(void);
void M_QuitDOOM(int choice);
void D_DoomDeinit(void);
void I_SetRes(void);
void I_UpdateSound(void);
void M_EndGame(int choice);

retro_log_printf_t log_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
retro_audio_sample_batch_t audio_batch_cb;
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

   I_MPPlayer_Init();

   rgb565 = RETRO_PIXEL_FORMAT_RGB565;
   if(environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &rgb565) && log_cb)
      log_cb(RETRO_LOG_INFO, "Frontend supports RGB565 - will use that instead of XRGB1555.\n");

   check_system_specs();
}

void retro_deinit(void)
{
   I_MPPlayer_Free();
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
   info->library_name     = "PrBoom";
#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif
   info->library_version  = "v2.5.0" GIT_VERSION;
   info->need_fullpath    = true;
   info->valid_extensions = "wad|iwad";
   info->block_extract = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   info->timing.fps = 60.0;
   info->timing.sample_rate = 44100.0;
   info->geometry.base_width = SCREENWIDTH;
   info->geometry.base_height = SCREENHEIGHT;
   info->geometry.max_width = SCREENWIDTH;
   info->geometry.max_height = SCREENHEIGHT;
   info->geometry.aspect_ratio = 4.0 / 3.0;
}


void retro_set_environment(retro_environment_t cb)
{
   struct retro_variable variables[] = {
      { "prboom-resolution",
         "Internal resolution; 320x200|640x400|960x600|1280x800|1600x1000|1920x1200" },
      { NULL, NULL },
   };

   environ_cb = cb;

   cb(RETRO_ENVIRONMENT_SET_VARIABLES, variables);
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

extern boolean quit_pressed;

static void update_variables(bool startup)
{
   struct retro_variable var;
   
   if (startup)
   {
      var.key = "prboom-resolution";
      var.value = NULL;

      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      {
         char *pch;
         char str[100];
         snprintf(str, sizeof(str), "%s", var.value);

         pch = strtok(str, "x");
         if (pch)
            SCREENWIDTH = strtoul(pch, NULL, 0);
         pch = strtok(NULL, "x");
         if (pch)
            SCREENHEIGHT = strtoul(pch, NULL, 0);

         if (log_cb)
            log_cb(RETRO_LOG_INFO, "Got size: %u x %u.\n", SCREENWIDTH, SCREENHEIGHT);
      }
      else
      {
         SCREENWIDTH = 320;
         SCREENHEIGHT = 200;
      }
   }
}

void I_SafeExit(int rc);

void retro_run(void)
{
   bool updated = false;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      update_variables(false);
   if (quit_pressed)
   {
      environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
      I_SafeExit(1);
   }

   D_DoomLoop();
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

bool I_PreInitGraphics(void)
{
   screen_buf = malloc(SURFACE_PIXEL_DEPTH * SCREENWIDTH * SCREENHEIGHT);
   return true;
}


bool retro_load_game(const struct retro_game_info *info)
{
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

   update_variables(true);


   argv[argc++] = strdup("prboom");
   if(info->path)
   {
      extract_directory(g_wad_dir, info->path, sizeof(g_wad_dir));
      extract_basename(g_basename, info->path, sizeof(g_basename));
      argv[argc++] = strdup("-iwad");
      argv[argc++] = strdup(g_basename);
   }

   myargc = argc;
   myargv = argv;

   if (!Z_Init()) /* 1/18/98 killough: start up memory stuff first */
      goto failed;

   /* cphipps - call to video specific startup code */
   if (!I_PreInitGraphics())
      goto failed;

   if (!D_DoomMainSetup())
      goto failed;

   return true;

failed:
   {
      struct retro_message msg;
      char msg_local[256];

      snprintf(msg_local, sizeof(msg_local),
            "ROM loading failed...");
      msg.msg    = msg_local;
      msg.frames = 360;
      if (environ_cb)
         environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, (void*)&msg);
   }
   if (screen_buf)
      free(screen_buf);
   I_SafeExit(-1);
   return false;
}


void retro_unload_game(void)
{
   if (screen_buf)
      free(screen_buf);
   screen_buf = NULL;
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

static int left_analog_lut[] = { 
        '.',               /* RETRO_DEVICE_ID_JOYPAD_R1 */
        ',',               /* RETRO_DEVICE_ID_JOYPAD_L1 */
        KEYD_DOWNARROW,    /* RETRO_DEVICE_ID_JOYPAD_DOWN */
        KEYD_UPARROW,      /* RETRO_DEVICE_ID_JOYPAD_UP */
        KEYD_RIGHTARROW,   /* RETRO_DEVICE_ID_JOYPAD_RIGHT */
        KEYD_LEFTARROW     /* RETRO_DEVICE_ID_JOYPAD_LEFT */
};

#define ANALOG_THRESHOLD 4096

void I_StartTic (void)
{
   unsigned i;
   static bool old_input[20];
   bool new_input[20];
   static bool old_input_analog_l[6];
   bool new_input_analog_l[6];

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

   {
      int lsx = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                     RETRO_DEVICE_ID_ANALOG_X);
      int lsy = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
                     RETRO_DEVICE_ID_ANALOG_Y);
      int rsx = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
                     RETRO_DEVICE_ID_ANALOG_X);
      int rsy = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
                     RETRO_DEVICE_ID_ANALOG_Y);

      if (lsx > ANALOG_THRESHOLD)
         new_input_analog_l[0] = true;
      else
         new_input_analog_l[0] = false;

      if (lsx < -ANALOG_THRESHOLD)
         new_input_analog_l[1] = true;
      else
         new_input_analog_l[1] = false;

      if (lsy > ANALOG_THRESHOLD)
         new_input_analog_l[2] = true;
      else
         new_input_analog_l[2] = false;

      if (lsy < -ANALOG_THRESHOLD)
         new_input_analog_l[3] = true;
      else
         new_input_analog_l[3] = false;

      if (rsx > ANALOG_THRESHOLD)
         new_input_analog_l[4] = true;
      else
         new_input_analog_l[4] = false;

      if (rsx < -ANALOG_THRESHOLD)
         new_input_analog_l[5] = true;
      else
         new_input_analog_l[5] = false;

      for (i = 0; i < 6; i++)
      {
         event_t event = {0};
         if(new_input_analog_l[i] && !old_input_analog_l[i])
         {
            event.type = ev_keydown;
            event.data1 = left_analog_lut[i];
         }

         if(!new_input_analog_l[i] && old_input_analog_l[i])
         {
            event.type = ev_keyup;
            event.data1 = left_analog_lut[i];
         }

         if(event.type == ev_keydown || event.type == ev_keyup)
            D_PostEvent(&event);

         old_input_analog_l[i] = new_input_analog_l[i];
      }
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
  char  * p;
#ifdef _WIN32
  char slash = '\\';
#else
  char slash = '/';
#endif

  /* Precalculate a length we will need in the loop */
  size_t pl = strlen(wfname) + strlen(ext) + 4;

  if (log_cb)
     log_cb(RETRO_LOG_INFO, "wfname: [%s], g_wad_dir: [%s]\n", wfname, g_wad_dir);

  p = malloc(strlen(g_wad_dir) + pl);
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


void I_Init(void)
{
  /* killough 2/21/98: avoid sound initialization if no sound & no music */
  if (!(nomusicparm && nosfxparm))
    I_InitSound();

#ifndef __LIBRETRO__
  R_InitInterpolation();
#endif
}

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

void I_Read(int fd, void* vbuf, size_t sz)
{
   unsigned char* buf = vbuf;

   while (sz)
   {
      int rc = read(fd,buf,sz);
      if (rc <= 0)
         I_Error("I_Read: read failed: %s", rc ? strerror(errno) : "EOF");
      sz  -= rc;
      buf += rc;
   }
}
