#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif
#include <errno.h>

#include <libretro.h>
#include <file/file_path.h>

#if _MSC_VER
#include <compat/msvc.h>
#endif

#ifdef _WIN32
   #define DIR_SLASH '\\'
#else
   #define DIR_SLASH '/'
#endif

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
#include "../src/r_fps.h"
#include "../src/lprintf.h"
#include "../src/doomstat.h"
#include "../src/m_cheat.h"
#include "../src/g_game.h"

//i_system
int ms_to_next_tick;
int mus_opl_gain = 250; // fine tune OPL output level

int SCREENWIDTH  = 320;
int SCREENHEIGHT = 200;

//i_video
static unsigned char *screen_buf;

/* libretro */
static char g_wad_dir[1024];
static char g_basename[1024];
static char g_save_dir[1024];

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

#define MAX_PADS 1
static unsigned doom_devices[1];

/* Whether mouse active when using Gamepad */
boolean mouse_on;
/* Whether to search for IWADs on parent folders recursively */
boolean find_recursive_on;

// System analog stick range is -0x8000 to 0x8000
#define ANALOG_RANGE 0x8000
// This is scaled by in-game 'mouse sensitivity' option, so just choose a value
// that has acceptable performance at the default sensitivity value
// (i.e. user can easily change mouse speed, so absolute value here is not critical)
#define ANALOG_MOUSE_SPEED 128
// Default deadzone: 15%
static int analog_deadzone = (int)(0.15f * ANALOG_RANGE);

#define RETROPAD_CLASSIC RETRO_DEVICE_JOYPAD
#define RETROPAD_MODERN  RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_ANALOG, 2)

// (number of gamepad buttons) + 1
#define MAX_BUTTON_BINDS 17

typedef struct {
	int* gamekey;
	int* menukey;
} action_lut_t;

typedef struct {
	struct retro_input_descriptor desc[MAX_BUTTON_BINDS];
	action_lut_t action_lut[MAX_BUTTON_BINDS];
	unsigned num_buttons;
} gamepad_layout_t;

static gamepad_layout_t gp_classic = { // Based on PS1 Doom Port!
	{
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "Strafe" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "Use" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Fire" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Run" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "Strafe Left" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "Strafe Right" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,     "Previous Weapon" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "Next Weapon" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,     "Toggle Run" }, // added in case anyone want to use toggle run instead of press to run with classic layout
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,     "180 Turn" }, // added in case anyone want it in classic layout
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Show/Hide Map" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Show/Hide Menu" },
		{ 0 },
	},
	{	// gamekey,             menukey
		{ &key_strafe,          &key_menu_backspace }, // RETRO_DEVICE_ID_JOYPAD_B
		{ &key_speed,           &key_menu_backspace }, // RETRO DEVICE_ID_JOYPAD_Y
		{ &key_map,             &key_menu_backspace }, // RETRO_DEVICE_ID_JOYPAD_SELECT
		{ &key_menu_escape,     &key_menu_escape },    // RETRO_DEVICE_ID_JOYPAD_START
		{ &key_up,              &key_menu_up },        // RETRO_DEVICE_ID_JOYPAD_UP
		{ &key_down,            &key_menu_down },      // RETRO_DEVICE_ID_JOYPAD_DOWN
		{ &key_left,            &key_menu_left },      // RETRO_DEVICE_ID_JOYPAD_LEFT
		{ &key_right,           &key_menu_right },     // RETRO_DEVICE_ID_JOYPAD_RIGHT
		{ &key_use,             &key_menu_enter },     // RETRO_DEVICE_ID_JOYPAD_A
		{ &key_fire,            &key_menu_enter },     // RETRO_DEVICE_ID_JOYPAD_X
		{ &key_strafeleft,      &key_menu_left },      // RETRO_DEVICE_ID_JOYPAD_L1
		{ &key_straferight,     &key_menu_right },     // RETRO_DEVICE_ID_JOYPAD_R1
		{ &key_weaponcycledown, &key_menu_backspace }, // RETRO_DEVICE_ID_JOYPAD_L2
		{ &key_weaponcycleup,   &key_menu_enter },     // RETRO_DEVICE_ID_JOYPAD_R2
		{ &key_autorun,         &key_menu_enter },     // RETRO_DEVICE_ID_JOYPAD_L3
		{ &key_reverse,         &key_menu_backspace }, // RETRO_DEVICE_ID_JOYPAD_R3
	},
	16,
};

static gamepad_layout_t gp_modern = { // Based on Original XBOX Doom 3 Collection
	{
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "Show Last Message" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "Next Weapon" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Previous Weapon" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Use" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "Quick Load" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "Quick Save" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,     "Run" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "Fire" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,     "Toggle Run" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,     "180 Turn" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Show/Hide Map" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Show/Hide Menu" },
		{ 0 },
	},
	{	// gamekey,             menukey
		{ &key_enter,           &key_menu_backspace }, // RETRO_DEVICE_ID_JOYPAD_B
		{ &key_use,             &key_menu_enter },     // RETRO DEVICE_ID_JOYPAD_Y
		{ &key_map,             &key_menu_backspace }, // RETRO_DEVICE_ID_JOYPAD_SELECT
		{ &key_menu_escape,     &key_menu_escape },    // RETRO_DEVICE_ID_JOYPAD_START
		{ &key_map_up,          &key_menu_up },        // RETRO_DEVICE_ID_JOYPAD_UP
		{ &key_map_down,        &key_menu_down },      // RETRO_DEVICE_ID_JOYPAD_DOWN
		{ &key_map_left,        &key_menu_left },      // RETRO_DEVICE_ID_JOYPAD_LEFT
		{ &key_map_right,       &key_menu_right },     // RETRO_DEVICE_ID_JOYPAD_RIGHT
		{ &key_weaponcycleup,   &key_menu_enter },     // RETRO_DEVICE_ID_JOYPAD_A
		{ &key_weaponcycledown, &key_menu_backspace }, // RETRO_DEVICE_ID_JOYPAD_X
		{ &key_quickload,       &key_menu_left },      // RETRO_DEVICE_ID_JOYPAD_L1
		{ &key_quicksave,       &key_menu_right },     // RETRO_DEVICE_ID_JOYPAD_R1
		{ &key_speed,           &key_menu_backspace }, // RETRO_DEVICE_ID_JOYPAD_L2
		{ &key_fire,            &key_menu_enter },     // RETRO_DEVICE_ID_JOYPAD_R2
		{ &key_autorun,         &key_menu_enter },     // RETRO_DEVICE_ID_JOYPAD_L3
		{ &key_reverse,         &key_menu_backspace }, // RETRO_DEVICE_ID_JOYPAD_R3
	},
	16,
};

char* FindFileInDir(const char* dir, const char* wfname, const char* ext);

static void check_system_specs(void)
{
   unsigned level = 4;
   environ_cb(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL, &level);
}

static bool libretro_supports_bitmasks = false;

void retro_init(void)
{
   enum retro_pixel_format rgb565;
   struct retro_log_callback log;

   if(environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = NULL;

   rgb565 = RETRO_PIXEL_FORMAT_RGB565;
   if(environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &rgb565) && log_cb)
      log_cb(RETRO_LOG_INFO, "Frontend supports RGB565 - will use that instead of XRGB1555.\n");

   if (environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL))
      libretro_supports_bitmasks = true;

   check_system_specs();
}

void retro_deinit(void)
{
   D_DoomDeinit();
   libretro_supports_bitmasks = false;
}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
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
   info->valid_extensions = "wad|iwad|pwad|lmp";
   info->block_extract = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
  switch(movement_smooth)
  {
    case 0:
      info->timing.fps = 35.0;
      break;
    case 1:
      info->timing.fps = 40.0;
      break;
    case 2:
      info->timing.fps = 50.0;
      break;
    case 3:
      info->timing.fps = 60.0;
      break;
    case 4:
      info->timing.fps = 70.0;
      break;
    case 5:
      info->timing.fps = 72.0;
      break;
    case 6:
      info->timing.fps = 75.0;
      break;
    case 7:
      info->timing.fps = 100.0;
      break;
    case 8:
      info->timing.fps = 119.0;
      break;
    case 9:
      info->timing.fps = 120.0;
      break;
    case 10:
      info->timing.fps = 140.0;
      break;
    case 11:
      info->timing.fps = 144.0;
      break;
    default:
      info->timing.fps = TICRATE;
      break;
  }
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
			"Internal resolution (restart); 320x200|640x400|960x600|1280x800|1600x1000|1920x1200|2240x1400|2560x1600" },
		{ "prboom-mouse_on", "Mouse active when using Gamepad; disabled|enabled" },
		{ "prboom-find_recursive_on", "Look on parent folders for IWADs; enabled|disabled" },
		{ "prboom-analog_deadzone", "Analog Deadzone (percent); 15|20|25|30|0|5|10" },
		{ NULL, NULL },
	};

   static const struct retro_controller_description port[] = {
		{ "Gamepad Modern", RETROPAD_MODERN },
		{ "Gamepad Classic", RETROPAD_CLASSIC },
		{ "RetroKeyboard/Mouse", RETRO_DEVICE_KEYBOARD },
		{ 0 },
   };

	static const struct retro_controller_info ports[] = {
		{ port, 3 },
		{ NULL, 0 },
	};

	environ_cb = cb;

	cb(RETRO_ENVIRONMENT_SET_VARIABLES, variables);
	cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
	if (port)
		return;

	switch (device)
	{
		case RETROPAD_CLASSIC:
			doom_devices[port] = RETROPAD_CLASSIC;
			environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, gp_classic.desc);
			break;
		case RETROPAD_MODERN:
			doom_devices[port] = RETROPAD_MODERN;
			environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, gp_modern.desc);
			break;
		case RETRO_DEVICE_KEYBOARD:
			doom_devices[port] = RETRO_DEVICE_KEYBOARD;
			// Input descriptors are irrelevant in this case, but don't want
			// to leave undefined...
			environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, gp_classic.desc);
			break;
		default:
			if (log_cb)
				log_cb(RETRO_LOG_ERROR, "[libretro]: Invalid device, setting type to RETROPAD_CLASSIC ...\n");
			doom_devices[port] = RETROPAD_CLASSIC;
			environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, gp_classic.desc);
	}
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

   var.key = "prboom-mouse_on";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "enabled"))
         mouse_on = true;
      else
         mouse_on = false;
   }

   var.key = "prboom-find_recursive_on";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "enabled"))
         find_recursive_on = true;
      else
         find_recursive_on = false;
   }

   var.key = "prboom-analog_deadzone";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
		analog_deadzone = (int)(atoi(var.value) * 0.01f * ANALOG_RANGE);
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

static char* remove_extension(char *buf, const char *path, size_t size)
{
  char *base;
  strncpy(buf, path, size - 1);
  buf[size - 1] = '\0';

  base = strrchr(buf, '.');

  if (base)
     *base = '\0';

  return base+1;
}

static wadinfo_t get_wadinfo(const char *path)
{
   FILE* fp = fopen(path, "rb");
   wadinfo_t header;
   if (fp != NULL)
   {
      fread(&header, sizeof(header), 1, fp);
      fclose(fp);
   }
   else
      memset(&header, 0, sizeof(header));
   return header;
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

   update_variables(true);

   argv[argc++] = strdup("prboom");
   if(info->path)
   {
      wadinfo_t header;
      char *deh, *extension, *baseconfig;

      char name_without_ext[4096];
      bool use_external_savedir = false;
      const char *base_save_dir = NULL;

      extract_directory(g_wad_dir, info->path, sizeof(g_wad_dir));
      extract_basename(g_basename, info->path, sizeof(g_basename));
      extension = remove_extension(name_without_ext, g_basename, sizeof(name_without_ext));

      if(strcasecmp(extension,"lmp") == 0)
      {
        // Play as a demo file lump
        argv[argc++] = strdup("-playdemo");
        argv[argc++] = strdup(info->path);
      }
      else
      {
        header = get_wadinfo(info->path);
        if(header.identification == NULL)
        {
          I_Error("retro_load_game: couldn't read WAD header from '%s'", info->path);
          goto failed;
        }
        if(!strncmp(header.identification, "IWAD", 4))
        {
          argv[argc++] = strdup("-iwad");
          argv[argc++] = strdup(g_basename);
        }
        else if(!strncmp(header.identification, "PWAD", 4))
        {
          argv[argc++] = strdup("-file");
          argv[argc++] = strdup(info->path);
        }
        else
        {
          I_Error("retro_load_game: invalid WAD header '%s'", header.identification);
          goto failed;
        }

        /* Check for DEH or BEX files */
        if((deh = FindFileInDir(g_wad_dir, name_without_ext, ".deh"))
           || (deh = FindFileInDir(g_wad_dir, name_without_ext, ".bex")))
        {
          argv[argc++] = "-deh";
          argv[argc++] = deh;
        };

        if((baseconfig = FindFileInDir(g_wad_dir, name_without_ext, ".prboom.cfg"))
         || (baseconfig = I_FindFile("prboom.cfg", NULL)))
        {
          argv[argc++] = "-baseconfig";
          argv[argc++] = baseconfig;
        }
      }

      // Get save directory
      if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &base_save_dir) && base_save_dir)
		{
			if (strlen(base_save_dir) > 0)
			{
				// > Build save path
				snprintf(g_save_dir, sizeof(g_save_dir), "%s%c%s", base_save_dir, DIR_SLASH, name_without_ext);
				use_external_savedir = true;

				// > Create save directory, if required
				if (!path_is_directory(g_save_dir))
				{
					use_external_savedir = path_mkdir(g_save_dir);
				}
			}
		}
      if (!use_external_savedir)
		{
			// > Use WAD directory fallback...
			snprintf(g_save_dir, sizeof(g_save_dir), "%s", g_wad_dir);
		}
   }

   myargc = argc;
   myargv = (const char **) argv;

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
   int i;
   (void)index;(void)enabled;
   if(code)
      for (i=0; code[i] != '\0'; i++)
         M_FindCheats(code[i]);
}

static action_lut_t left_analog_lut[] = {
   { &key_straferight, &key_menu_right }, /* RETRO_DEVICE_INDEX_ANALOG_LEFT +X */
   { &key_strafeleft,  &key_menu_left },  /* RETRO_DEVICE_INDEX_ANALOG_LEFT -X */
   { &key_down,        &key_menu_down },  /* RETRO_DEVICE_INDEX_ANALOG_LEFT +Y */
   { &key_up,          &key_menu_up },    /* RETRO_DEVICE_INDEX_ANALOG_LEFT -Y */
};

static action_lut_t mw_lut[] = {
   { &key_weaponcycleup,   &key_menu_up },   /* RETRO_DEVICE_ID_MOUSE_WHEELUP */
   { &key_weaponcycledown, &key_menu_down }, /* RETRO_DEVICE_ID_MOUSE_WHEELDOWN */
};

static int keyboard_lut[117][2] = {
   {RETROK_BACKSPACE      ,KEYD_BACKSPACE},
   {RETROK_TAB            ,KEYD_TAB},
   {RETROK_CLEAR          ,},
   {RETROK_RETURN         ,KEYD_ENTER},
   {RETROK_PAUSE          ,KEYD_PAUSE},
   {RETROK_ESCAPE         ,KEYD_ESCAPE},
   {RETROK_SPACE          ,KEYD_SPACEBAR},
   {RETROK_EXCLAIM        ,'!'},
   {RETROK_QUOTEDBL       ,'"'},
   {RETROK_HASH           ,'#'},
   {RETROK_DOLLAR         ,'$'},
   {RETROK_AMPERSAND      ,'&'},
   {RETROK_QUOTE          ,'\''},
   {RETROK_LEFTPAREN      ,'('},
   {RETROK_RIGHTPAREN     ,')'},
   {RETROK_ASTERISK       ,'*'},
   {RETROK_PLUS           ,'+'},
   {RETROK_COMMA          ,','},
   {RETROK_MINUS          ,KEYD_MINUS},
   {RETROK_PERIOD         ,'.'},
   {RETROK_SLASH          ,'/'},
   {RETROK_0              ,'0'},
   {RETROK_1              ,'1'},
   {RETROK_2              ,'2'},
   {RETROK_3              ,'3'},
   {RETROK_4              ,'4'},
   {RETROK_5              ,'5'},
   {RETROK_6              ,'6'},
   {RETROK_7              ,'7'},
   {RETROK_8              ,'8'},
   {RETROK_9              ,'9'},
   {RETROK_COLON          ,':'},
   {RETROK_SEMICOLON      ,';'},
   {RETROK_LESS           ,'<'},
   {RETROK_EQUALS         ,KEYD_EQUALS},
   {RETROK_GREATER        ,'>'},
   {RETROK_QUESTION       ,'?'},
   {RETROK_AT             ,'@'},
   {RETROK_LEFTBRACKET    ,'['},
   {RETROK_BACKSLASH      ,'\\'},
   {RETROK_RIGHTBRACKET   ,']'},
   {RETROK_CARET          ,'^'},
   {RETROK_UNDERSCORE     ,'_'},
   {RETROK_BACKQUOTE      ,'`'},
   {RETROK_a              ,'a'},
   {RETROK_b              ,'b'},
   {RETROK_c              ,'c'},
   {RETROK_d              ,'d'},
   {RETROK_e              ,'e'},
   {RETROK_f              ,'f'},
   {RETROK_g              ,'g'},
   {RETROK_h              ,'h'},
   {RETROK_i              ,'i'},
   {RETROK_j              ,'j'},
   {RETROK_k              ,'k'},
   {RETROK_l              ,'l'},
   {RETROK_m              ,'m'},
   {RETROK_n              ,'n'},
   {RETROK_o              ,'o'},
   {RETROK_p              ,'p'},
   {RETROK_q              ,'q'},
   {RETROK_r              ,'r'},
   {RETROK_s              ,'s'},
   {RETROK_t              ,'t'},
   {RETROK_u              ,'u'},
   {RETROK_v              ,'v'},
   {RETROK_w              ,'w'},
   {RETROK_x              ,'x'},
   {RETROK_y              ,'y'},
   {RETROK_z              ,'z'},
   {RETROK_DELETE         ,KEYD_DEL},

   {RETROK_KP0            ,KEYD_KEYPAD0},
   {RETROK_KP1            ,KEYD_KEYPAD1},
   {RETROK_KP2            ,KEYD_KEYPAD2},
   {RETROK_KP3            ,KEYD_KEYPAD3},
   {RETROK_KP4            ,KEYD_KEYPAD4},
   {RETROK_KP5            ,KEYD_KEYPAD5},
   {RETROK_KP6            ,KEYD_KEYPAD6},
   {RETROK_KP7            ,KEYD_KEYPAD7},
   {RETROK_KP8            ,KEYD_KEYPAD8},
   {RETROK_KP9            ,KEYD_KEYPAD9},
   {RETROK_KP_PERIOD      ,KEYD_KEYPADPERIOD},
   {RETROK_KP_DIVIDE      ,KEYD_KEYPADDIVIDE},
   {RETROK_KP_MULTIPLY    ,KEYD_KEYPADMULTIPLY},
   {RETROK_KP_MINUS       ,KEYD_KEYPADMINUS},
   {RETROK_KP_PLUS        ,KEYD_KEYPADPLUS},
   {RETROK_KP_ENTER       ,KEYD_KEYPADENTER},

   {RETROK_UP             ,KEYD_UPARROW},
   {RETROK_DOWN           ,KEYD_DOWNARROW},
   {RETROK_RIGHT          ,KEYD_RIGHTARROW},
   {RETROK_LEFT           ,KEYD_LEFTARROW},
   {RETROK_INSERT         ,KEYD_INSERT},
   {RETROK_HOME           ,KEYD_HOME},
   {RETROK_END            ,KEYD_END},
   {RETROK_PAGEUP         ,KEYD_PAGEUP},
   {RETROK_PAGEDOWN       ,KEYD_PAGEDOWN},

   {RETROK_F1             ,KEYD_F1},
   {RETROK_F2             ,KEYD_F2},
   {RETROK_F3             ,KEYD_F3},
   {RETROK_F4             ,KEYD_F4},
   {RETROK_F5             ,KEYD_F5},
   {RETROK_F6             ,KEYD_F6},
   {RETROK_F7             ,KEYD_F7},
   {RETROK_F8             ,KEYD_F8},
   {RETROK_F9             ,KEYD_F9},
   {RETROK_F10            ,KEYD_F10},
   {RETROK_F11            ,KEYD_F11},
   {RETROK_F12            ,KEYD_F12},

   {RETROK_NUMLOCK        ,KEYD_NUMLOCK},
   {RETROK_CAPSLOCK       ,KEYD_CAPSLOCK},
   {RETROK_SCROLLOCK      ,KEYD_SCROLLLOCK},
   {RETROK_RSHIFT         ,KEYD_RSHIFT},
   {RETROK_LSHIFT         ,KEYD_RSHIFT},
   {RETROK_RCTRL          ,KEYD_RCTRL},
   {RETROK_LCTRL          ,KEYD_RCTRL},
   {RETROK_RALT           ,KEYD_RALT},
   {RETROK_LALT           ,KEYD_LALT}
};

// Produces a pulse train with average on-time fraction amplitude/pwm_period.
// (https://www.embeddedrelated.com/showarticle/107.php)
// > Now that we process input once per game tic, the period here must
//   match the internal tic rate (corresponding to default 35fps)
static const int pwm_period = TICRATE;
static bool synthetic_pwm(int amplitude, int* modulation_state)
{
	*modulation_state += amplitude;
	if (*modulation_state < pwm_period)
		return false;
	*modulation_state -= pwm_period;
	return true;
}

static void process_gamepad_buttons(int16_t ret, unsigned num_buttons, action_lut_t action_lut[])
{
   unsigned i;
   static bool old_input[MAX_BUTTON_BINDS];
   bool new_input[MAX_BUTTON_BINDS];

   for (i = 0; i < num_buttons; i++)
   {
      event_t event = {0};
      new_input[i]  = ret & (1 << i);

      if(new_input[i] && !old_input[i])
      {
         event.type = ev_keydown;
         event.data1 = *((menuactive)? action_lut[i].menukey : action_lut[i].gamekey);
      }

      if(!new_input[i] && old_input[i])
      {
         event.type = ev_keyup;
         event.data1 = *((menuactive)? action_lut[i].menukey : action_lut[i].gamekey);
      }

      if(event.type == ev_keydown || event.type == ev_keyup)
         D_PostEvent(&event);

      old_input[i] = new_input[i];
   }
}

static void process_gamepad_left_analog(void)
{
	unsigned i;
	static bool old_input_analog_l[4];
	bool new_input_analog_l[4];
	int analog_l_amplitude[4];
	static int analog_l_modulation_state[4];
	int lsx, lsy, analog_range;

	// Only run once per game tic
	if(tic_vars.frac < FRACUNIT) return;
	// Increase range on menu
	analog_range = (menuactive)? ANALOG_RANGE*8 : ANALOG_RANGE;

	lsx = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
	lsy = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);

	// Get movement 'amplitude' on each axis
	// > x-axis
	analog_l_amplitude[0] = 0;
	analog_l_amplitude[1] = 0;
	if (lsx > analog_deadzone)
	{
		// Add '1' to deal with float->int rounding accuracy loss...
		// (Similarly, subtract '1' when lsx is negative...)
		analog_l_amplitude[0] = 1 + pwm_period * (lsx - analog_deadzone) / (analog_range - analog_deadzone);
	}
	if (lsx < -analog_deadzone)
		analog_l_amplitude[1] = -1 * (-1 + pwm_period * (lsx + analog_deadzone) / (analog_range - analog_deadzone));
	// > y-axis
	analog_l_amplitude[2] = 0;
	analog_l_amplitude[3] = 0;
	if (lsy > analog_deadzone)
		analog_l_amplitude[2] = 1 + pwm_period * (lsy - analog_deadzone) / (analog_range - analog_deadzone);
	if (lsy < -analog_deadzone)
		analog_l_amplitude[3] = -1 * (-1 + pwm_period * (lsy + analog_deadzone) / (analog_range - analog_deadzone));

	for (i = 0; i < 4; i++)
	{
		event_t event = {0};

		new_input_analog_l[i] = synthetic_pwm(analog_l_amplitude[i], &analog_l_modulation_state[i]);

		if(new_input_analog_l[i] && !old_input_analog_l[i])
		{
			event.type = ev_keydown;
			event.data1 = *((menuactive)? left_analog_lut[i].menukey : left_analog_lut[i].gamekey);
		}

		if(!new_input_analog_l[i] && old_input_analog_l[i])
		{
			event.type = ev_keyup;
			event.data1 = *((menuactive)? left_analog_lut[i].menukey : left_analog_lut[i].gamekey);;
		}

		if(event.type == ev_keydown || event.type == ev_keyup)
			D_PostEvent(&event);

		old_input_analog_l[i] = new_input_analog_l[i];
	}
}

static void process_gamepad_right_analog(bool pressed_y, bool pressed_l2)
{
	extern int autorun;
	int rsx               = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
	int rsy               = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);
	bool run_key          = false;;
	int analog_turn_speed = 0;
	event_t event_mouse   = {0};

	/* retrieve run key status */
	if (doom_devices[0] == RETROPAD_CLASSIC)
		run_key            = pressed_y;
	else if (doom_devices[0] == RETROPAD_MODERN)
		run_key            = pressed_l2;

	/* make the run key invert autorun behavior if both are active */
	if ((autorun && !run_key) || (!autorun && run_key))
		analog_turn_speed = 2;
	else
		analog_turn_speed = 1;

	if (rsx < -analog_deadzone || rsx > analog_deadzone)
	{
		if (rsx > analog_deadzone)
			rsx = rsx - analog_deadzone;
		if (rsx < -analog_deadzone)
			rsx = rsx + analog_deadzone;
		event_mouse.type = ev_mouse;
		event_mouse.data2 = ANALOG_MOUSE_SPEED * rsx / (ANALOG_RANGE - analog_deadzone)
                         * analog_turn_speed * TICRATE / (float)tic_vars.fps;
	}

	if (rsy < -analog_deadzone || rsy > analog_deadzone)
	{
		if (rsy > analog_deadzone)
			rsy = rsy - analog_deadzone;
		if (rsy < -analog_deadzone)
			rsy = rsy + analog_deadzone;
		event_mouse.type = ev_mouse;
		event_mouse.data3 = ANALOG_MOUSE_SPEED * rsy / (ANALOG_RANGE - analog_deadzone)
                         * analog_turn_speed * TICRATE / (float)tic_vars.fps;
	}

	if(event_mouse.type == ev_mouse)
		D_PostEvent(&event_mouse);
}

void I_StartTic (void)
{
   int port;
   unsigned i;
   static int cur_mx;
   static int cur_my;
   int mx, my;
   static bool old_input_kb[117];
   bool new_input_kb[117];
   int16_t ret = 0;

   input_poll_cb();

   for (port = 0; port < MAX_PADS; port++)
   {
      if (!input_state_cb)
         break;

      switch (doom_devices[port])
      {
		case RETROPAD_CLASSIC:
         if (libretro_supports_bitmasks)
            ret = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
         else
         {
            unsigned i;
            for (i = 0; i < gp_classic.num_buttons; i++)
               if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, i))
                  ret |= (1 << i);
         }
			process_gamepad_buttons(ret, gp_classic.num_buttons, gp_classic.action_lut);
			break;
		case RETROPAD_MODERN:
         if (libretro_supports_bitmasks)
            ret = input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
         else
         {
            unsigned i;
            for (i = 0; i < gp_modern.num_buttons; i++)
               if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, i))
                  ret |= (1 << i);
         }
			process_gamepad_buttons(ret, gp_modern.num_buttons, gp_modern.action_lut);
			process_gamepad_left_analog();
			process_gamepad_right_analog(ret & (1 << RETRO_DEVICE_ID_JOYPAD_Y), ret & (1 << RETRO_DEVICE_ID_JOYPAD_L2));
			break;
      case RETRO_DEVICE_KEYBOARD:
         {
            /* Keyboard Input */

            for(i = 0; i < 117; i++)
            {
               event_t event = {0};
               new_input_kb[i] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, keyboard_lut[i][0]);

               if(new_input_kb[i] && !old_input_kb[i])
               {
                  event.type = ev_keydown;
                  event.data1 = keyboard_lut[i][1];
               }

               if(!new_input_kb[i] && old_input_kb[i])
               {
                  event.type = ev_keyup;
                  event.data1 = keyboard_lut[i][1];
               }

               if(event.type == ev_keydown || event.type == ev_keyup)
                  D_PostEvent(&event);

               old_input_kb[i] = new_input_kb[i];
            }
         }
         break;
      }
   }

   if (mouse_on || doom_devices[0] == RETRO_DEVICE_KEYBOARD)
   {
      /* Mouse Input */
      static bool old_input_mw[2];
      static bool new_input_mw[2];

      event_t event_mouse = {0};
      event_mouse.type = ev_mouse;

      mx = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
      my = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);

      if (mx != cur_mx || my != cur_my)
      {
         event_mouse.data2 = mx * 4;
         event_mouse.data3 = my * 4;
         cur_mx = mx;
         cur_my = my;
      }

      if (input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT))
         event_mouse.data1 = 1;
      if (input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT))
         event_mouse.data1 = 2;
      if (input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT) &&
            input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT))
         event_mouse.data1 = 3;

      D_PostEvent(&event_mouse);

      /* Mouse Wheel */

      for (i = 0; i < 2; i++)
      {
         event_t event_mw = {0};
         if (i == 0)
            new_input_mw[i] = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_WHEELUP);
         if (i == 1)
            new_input_mw[i] = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_WHEELDOWN);

         if (new_input_mw[i] && !old_input_mw[i])
         {
            event_mw.type = ev_keydown;
            event_mw.data1 = *((menuactive)? mw_lut[i].menukey : mw_lut[i].gamekey);
         }

         if (!new_input_mw[i] && old_input_mw[i])
         {
            event_mw.type = ev_keyup;
            event_mw.data1 = *((menuactive)? mw_lut[i].menukey : mw_lut[i].gamekey);
         }

         if (event_mw.type == ev_keydown || event_mw.type == ev_keyup)
            D_PostEvent(&event_mw);

         old_input_mw[i] = new_input_mw[i];
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
   snprintf(buf,sz,"signal %d",signum);
   return buf;
}

#else

const char *I_DoomExeDir(void)
{
   return g_save_dir;
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

/**
 * FindFileInDir
 **
 * Checks if directory contains a file with given name and extension.
 * returns the path to the file if it exists and is readable or NULL otherwise
 */
char* FindFileInDir(const char* dir, const char* wfname, const char* ext)
{
   FILE * file;
   char * p;
   /* Precalculate a length we will need in the loop */
   size_t pl = strlen(wfname) + (ext ? strlen(ext) : 0) + 4;

   if( dir == NULL ) {
      p = malloc(pl);
      sprintf(p, "%s", wfname);
   }
   else {
     p = malloc(strlen(dir) + pl);
     sprintf(p, "%s%c%s", dir, DIR_SLASH, wfname);
   }

   if (ext && ext[0] != '\0')
   {
      strcat(p, ext);
   }
   file = fopen(p, "rb");

   if (file)
   {
      if (log_cb)
         log_cb(RETRO_LOG_INFO, "FindFileInDir: found %s\n", p);
      fclose(file);
      return p;
   }
   else if (log_cb)
      log_cb(RETRO_LOG_DEBUG, "FindFileInDir: not found %s in %s\n", wfname, dir);

   free(p);
   return NULL;
}

/*
 * I_FindFile
 **
 * Given a file name, search for it in g_wad_dir first, then the system folder
 * and then scan the parent folders of g_wad_dir.
 */
char* I_FindFile(const char* wfname, const char* ext)
{
   char *p, *dir, *system_dir, *prboom_system_dir;
   int i;

   // First, check on WAD directory
   if ((p = FindFileInDir(g_wad_dir, wfname, ext)) == NULL)
   {
     // Then check on system dir (both under prboom subfolder and directly)
     environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir);
     if (system_dir)
     {
       prboom_system_dir = malloc(strlen(system_dir) + 8);
       sprintf(prboom_system_dir, "%s%c%s", system_dir, DIR_SLASH, "prboom");
       p = FindFileInDir(prboom_system_dir, wfname, ext);
       free(prboom_system_dir);
       if(p == NULL)
         p = FindFileInDir(system_dir, wfname, ext);
     }

     // If not found, check on parent folders recursively (if configured to do so)
     if ( p == NULL && find_recursive_on)
     {
       dir = malloc(strlen(g_wad_dir) + 1);
       strcpy(dir, g_wad_dir);
       for (i = strlen(dir)-1; i > 1; dir[i--] = '\0')
       {
         if((dir[i] == '/' || dir[i] == '\\')
           && dir[i-1] != dir[i])
         {
           dir[i] = '\0'; // remove leading slash
           p = FindFileInDir(dir, wfname, ext);
           if(p != NULL) break;
         }
       }
       free(dir);
     }
   }
   return p;
}

#endif


void I_Init(void)
{
   int i;

   if (!nosfxparm)
      I_InitSound();

   if (!nomusicparm)
     I_InitMusic();

   for (i = 0; i < MAX_PADS; i++)
      doom_devices[i] = RETRO_DEVICE_JOYPAD;

   R_InitInterpolation();
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

void R_InitInterpolation(void)
{
  struct retro_system_av_info info;
  retro_get_system_av_info(&info);
  if(tic_vars.fps != info.timing.fps) {
    // Only update av_info if changed and it's not the first run
    if(tic_vars.fps)
        environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &info);

    tic_vars.fps = info.timing.fps;
    tic_vars.frac_step = FRACUNIT * TICRATE / tic_vars.fps;
    tic_vars.sample_step = info.timing.sample_rate / tic_vars.fps;
  }
  tic_vars.frac = FRACUNIT;
}
