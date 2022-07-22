#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif
#include <errno.h>
#include <stdarg.h>

#include <libretro.h>
#include <file/file_path.h>
#include <streams/file_stream.h>
#include <array/rbuf.h>
#include <compat/strl.h>

#if _MSC_VER
#include <compat/msvc.h>
#endif

#ifdef _WIN32
   #define DIR_SLASH '\\'
#else
   #define DIR_SLASH '/'
#endif

#include "libretro_core_options.h"

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
#include "../src/wi_stuff.h"
#include "../src/p_tick.h"
#include "../src/z_zone.h"

/* Don't include file_stream_transforms.h but instead
just forward declare the prototype */
int64_t rfread(void* buffer,
   size_t elem_size, size_t elem_count, RFILE* stream);

//i_system
int ms_to_next_tick;
int mus_opl_gain = 250; // fine tune OPL output level

int SCREENWIDTH  = 320;
int SCREENHEIGHT = 200;

//i_video
static unsigned char *screen_buf = NULL;

/* libretro */
static char g_wad_dir[1024];
static char g_basename[1024];
static char g_save_dir[1024];

/* Cheat handling */
static bool cheats_enabled = false;
static bool cheats_pending = false;
static char **cheats_pending_list = NULL;

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

static void process_input(void);

#define MAX_PADS 1
static unsigned doom_devices[1];

/* Whether mouse active when using Gamepad */
dbool   mouse_on;
/* Whether to search for IWADs on parent folders recursively */
dbool   find_recursive_on;

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
	struct retro_input_descriptor desc[MAX_BUTTON_BINDS+4];
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
		{ 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Strafe" },
		{ 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Move" },
		{ 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Turn" },
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

static struct retro_rumble_interface rumble = {0};
static bool rumble_enabled                  = false;
static uint16_t rumble_damage_strength      = 0;
static int16_t rumble_damage_counter        = -1;
static uint16_t rumble_touch_strength       = 0;
static int16_t rumble_touch_counter         = -1;

void retro_set_rumble_damage(int damage, float duration)
{
   /* Rumble scales linearly from 0xFFF to 0xFFFF
    * as damage increases from 1 to 80 */
   int capped_damage = (damage < 80) ? damage : 80;
   uint16_t strength = 0;

   if (!rumble.set_rumble_state ||
       (!rumble_enabled && (capped_damage > 0)))
      return;

   if ((capped_damage > 0) && (duration > 0.0f))
   {
      strength = 0xFFF + (capped_damage * 0x300);
      rumble_damage_counter = (int16_t)((duration * (float)tic_vars.fps / 1000.0f) + 1.0f);
   }

   /* Return early if:
    * - strength and last set value are both zero
    * - strength is greater than zero but less than
    *   (or equal to) last set value */
   if (((strength == 0) && (rumble_damage_strength == 0)) ||
       ((strength > 0) && (strength <= rumble_damage_strength)))
      return;

   rumble.set_rumble_state(0, RETRO_RUMBLE_STRONG, strength);
   rumble_damage_strength = strength;
}

void retro_set_rumble_touch(unsigned intensity, float duration)
{
   /* Rumble scales linearly from 0x1FE to 0xFFFF
    * as intensity increases from 1 to 20 */
   unsigned capped_intensity = (intensity < 20) ? intensity : 20;
   uint16_t strength         = 0;

   if (!rumble.set_rumble_state ||
       (!rumble_enabled && (capped_intensity > 0)))
      return;

   if ((capped_intensity > 0) && (duration > 0.0f))
   {
      strength             = 0x1FE + (capped_intensity * 0xCB3);
      rumble_touch_counter = (int16_t)((duration * (float)tic_vars.fps / 1000.0f) + 1.0f);
   }

   /* Return early if strength matches last
    * set value */
   if (strength == rumble_touch_strength)
      return;

   rumble.set_rumble_state(0, RETRO_RUMBLE_WEAK, strength);
   rumble_touch_strength = strength;
}

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

   Z_Init(); /* 1/18/98 killough: start up memory stuff first */

   if(environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = NULL;

   rgb565 = RETRO_PIXEL_FORMAT_RGB565;
   if(environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &rgb565) && log_cb)
      log_cb(RETRO_LOG_DEBUG, "Frontend supports RGB565 - will use that instead of XRGB1555.\n");

   if (environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL))
      libretro_supports_bitmasks = true;

   check_system_specs();
}

void retro_deinit(void)
{
   D_DoomDeinit();

   if (screen_buf)
      free(screen_buf);
   screen_buf = NULL;

   cheats_enabled = false;
   cheats_pending = false;
   if (cheats_pending_list)
   {
      unsigned i;
      for (i = 0; i < RBUF_LEN(cheats_pending_list); i++)
      {
         if (cheats_pending_list[i])
            free(cheats_pending_list[i]);
         cheats_pending_list[i] = NULL;
      }
      RBUF_FREE(cheats_pending_list);
   }

   libretro_supports_bitmasks = false;

   retro_set_rumble_damage(0, 0.0f);
   retro_set_rumble_touch(0, 0.0f);

   memset(&rumble, 0, sizeof(struct retro_rumble_interface));
   rumble_enabled         = false;
   rumble_damage_strength = 0;
   rumble_damage_counter  = -1;
   rumble_touch_strength  = 0;
   rumble_touch_counter   = -1;

   /* Z_Close() must be the very last
    * function that is called, since
    * z_zone.h overrides malloc()/free()/etc.
    * (i.e. anything that calls free()
    * after Z_Close() will likely segfault) */
   Z_Close();
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
      info->timing.fps = 90.0;
      break;
    case 8:
      info->timing.fps = 100.0;
      break;
    case 9:
      info->timing.fps = 119.0;
      break;
    case 10:
      info->timing.fps = 120.0;
      break;
    case 11:
      info->timing.fps = 140.0;
      break;
    case 12:
      info->timing.fps = 144.0;
      break;
    case 13:
      info->timing.fps = 240.0;
      break;
    case 14:
      info->timing.fps = 244.0;
      break;
    case 15:
      info->timing.fps = 300.0;
      break;
    case 16:
      info->timing.fps = 360.0;
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
   struct retro_vfs_interface_info vfs_iface_info;
   static bool libretro_supports_option_categories = false;
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

   libretro_set_core_options(environ_cb,
         &libretro_supports_option_categories);

	cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);

   vfs_iface_info.required_interface_version = 1;
   vfs_iface_info.iface                      = NULL;
   if (cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs_iface_info))
      filestream_vfs_init(&vfs_iface_info);
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
				log_cb(RETRO_LOG_ERROR, "Invalid libretro controller device, using default: RETROPAD_CLASSIC\n");
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

extern dbool   quit_pressed;

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
         strlcpy(str, var.value, sizeof(str));

         pch = strtok(str, "x");
         if (pch)
            SCREENWIDTH = strtoul(pch, NULL, 0);
         pch = strtok(NULL, "x");
         if (pch)
            SCREENHEIGHT = strtoul(pch, NULL, 0);

         if (log_cb)
            log_cb(RETRO_LOG_DEBUG, "Got size: %u x %u.\n", SCREENWIDTH, SCREENHEIGHT);
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

   var.key = "prboom-rumble";
   var.value = NULL;
   rumble_enabled = false;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      if (!strcmp(var.value, "enabled"))
         rumble_enabled = true;

   if (!rumble_enabled)
   {
      retro_set_rumble_damage(0, 0.0f);
      retro_set_rumble_touch(0, 0.0f);
   }

   var.key = "prboom-analog_deadzone";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      analog_deadzone = (int)(atoi(var.value) * 0.01f * ANALOG_RANGE);

#if defined(MEMORY_LOW)
   var.key = "prboom-purge_limit";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      int purge_limit = atoi(var.value) * 1024 * 1024;
      Z_SetPurgeLimit(purge_limit);
   }
#endif
}

void I_SafeExit(int rc);

void retro_run(void)
{
   bool updated = false;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      update_variables(false);

   /* Check for pending cheats */
   if (cheats_pending && (gamestate == GS_LEVEL) && !demoplayback)
   {
      unsigned i, j;
      for (i = 0; i < RBUF_LEN(cheats_pending_list); i++)
      {
         const char *cheat_code = cheats_pending_list[i];
         if (cheat_code)
         {
            for (j = 0; cheat_code[j] != '\0'; j++)
               M_FindCheats(cheat_code[j]);

            free(cheats_pending_list[i]);
            cheats_pending_list[i] = NULL;
         }
      }
      RBUF_FREE(cheats_pending_list);
      cheats_pending = false;
   }

   if (quit_pressed)
   {
      environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
      I_SafeExit(1);
      return;
   }
   D_DoomLoop();
   I_UpdateSound();

   if (rumble_damage_counter > -1)
   {
      rumble_damage_counter--;

      if (rumble_damage_counter == 0)
         retro_set_rumble_damage(0, 0.0f);
   }

   if (rumble_touch_counter > -1)
   {
      rumble_touch_counter--;

      if (rumble_touch_counter == 0)
         retro_set_rumble_touch(0, 0.0f);
   }
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
  memcpy(buf, path, size - 1);
  buf[size - 1] = '\0';

  base = strrchr(buf, '.');

  if (base)
     *base = '\0';

  return base+1;
}

static wadinfo_t get_wadinfo(const char *path)
{
   wadinfo_t header;
   RFILE* fp = filestream_open(path,
		   RETRO_VFS_FILE_ACCESS_READ,
		   RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (fp)
   {
      if(rfread(&header, sizeof(header), 1, fp) != 1)
         I_Error("get_wadinfo: error reading file header");
      filestream_close(fp);
   }
   else
      memset(&header, 0, sizeof(header));
   return header;
}

bool I_PreInitGraphics(void)
{
   screen_buf = malloc(SURFACE_PIXEL_DEPTH * SCREENWIDTH * SCREENHEIGHT);
   return (screen_buf != NULL);
}

bool retro_load_game(const struct retro_game_info *info)
{
   unsigned i;
   int argc = 0;
   static char *argv[32] = {NULL};

   if (environ_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble))
   {
      if (log_cb)
         log_cb(RETRO_LOG_INFO, "Rumble environment supported.\n");
   }
   else if (log_cb)
      log_cb(RETRO_LOG_INFO, "Rumble environment not supported.\n");

   update_variables(true);

   argv[argc++] = strdup("prboom");
   if(info->path)
   {
      wadinfo_t header;
      char *deh, *extension, *baseconfig;

      char name_without_ext[1023];
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
        // header.identification is static array, always non-NULL, but it might be empty if it couldn't be read
        if(header.identification[0] == 0)
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
        else {
          I_Error("retro_load_game: invalid WAD header '%.*s'", 4, header.identification);
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
			if (base_save_dir && strlen(base_save_dir) > 0)
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
			strlcpy(g_save_dir, g_wad_dir, sizeof(g_save_dir));
		}
   }

#if DEBUG
   argv[argc++] = "-dehout";
   argv[argc++] = "-";
#endif

   myargc = argc;
   myargv = (const char **) argv;

   /* cphipps - call to video specific startup code */
   if (!I_PreInitGraphics())
      goto failed;

   if (!D_DoomMainSetup())
      goto failed;

   // Run few cycles to finish init.
   for (i = 0; i < 3; i++)
     D_DoomLoop();

   cheats_enabled      = true;
   cheats_pending      = false;
   cheats_pending_list = NULL;

   return true;

failed:
   {
      struct retro_message msg;
      char msg_local[256];

      strlcpy(msg_local, "ROM loading failed...", sizeof(msg_local));
      msg.msg    = msg_local;
      msg.frames = 360;
      if (environ_cb)
         environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, (void*)&msg);
   }
   if (screen_buf) {
      free(screen_buf);
      screen_buf = NULL;
   }
   I_SafeExit(-1);
   return false;
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

extern short itemOn;
extern short skullAnimCounter;
extern short whichSkull;
extern int set_menu_itemon;
extern enum menuactive_e menuactive;
typedef struct menu_s menu_t;

extern menu_t* currentMenu;

extern menu_t NewDef;
extern menu_t MainDef;
extern menu_t HelpDef;
extern menu_t ExtHelpDef;
extern menu_t SoundDef;
extern menu_t ReadDef1;
extern menu_t ReadDef2;
extern menu_t EpiDef;
extern menu_t LoadDef;
extern menu_t SaveDef;
extern menu_t OptionsDef;
extern menu_t MouseDef;
extern menu_t KeybndDef;
extern menu_t WeaponDef;
extern menu_t StatusHUDDef;
extern menu_t AutoMapDef;
extern menu_t EnemyDef;
extern menu_t GeneralDef;
extern menu_t CompatDef;
extern menu_t MessageDef;
extern menu_t ChatStrDef;
extern menu_t SetupDef;

static menu_t *menus[] = {
		   &MainDef,
		   &HelpDef,
		   &SoundDef,
		   &ExtHelpDef,
		   &ReadDef1,
		   &ReadDef2,
		   &NewDef,
		   &EpiDef,
		   &LoadDef,
		   &SaveDef,
		   &OptionsDef,
		   &MouseDef,
		   &KeybndDef,
		   &WeaponDef,
		   &StatusHUDDef,
		   &AutoMapDef,
		   &EnemyDef,
		   &GeneralDef,
		   &CompatDef,
		   &MessageDef,
		   &ChatStrDef,
		   &SetupDef
};

#define NUMKEYS 512
extern dbool   gamekeydown[NUMKEYS];
static bool old_input[MAX_BUTTON_BINDS];

struct extra_serialize {
  uint32_t extra_size;
  uint32_t gametic;
  fixed_t  gameticfrac;
  uint32_t gameaction;
  uint32_t turnheld;
  uint32_t gamestate;
  uint32_t FinaleStage;
  uint32_t FinaleCount;
  uint32_t set_menu_itemon;
  struct wi_state wi_state;
  short itemOn;
  short whichSkull;
  short currentMenu;
  uint8_t  autorun;
  uint8_t  gameless;
  uint8_t  menuactive;
  fixed_t  prevx;
  fixed_t  prevy;
  fixed_t  prevz;
  angle_t  prevangle;
  angle_t  prevpitch;
  uint8_t  old_input[MAX_BUTTON_BINDS];
  uint8_t  gamekeydown[NUMKEYS];
};

size_t retro_serialize_size(void)
{
  return sizeof(struct extra_serialize) + 0x30000;
}

bool retro_serialize(void *data_, size_t size)
{
  unsigned i;
  struct extra_serialize *extra = data_;

  if (gamestate == GS_LEVEL) {
    int ret = G_DoSaveGameToBuffer((char *) data_ + sizeof(*extra),
				   size - sizeof(*extra));
    if (!ret) {
      return false;
    }
    if (viewplayer && viewplayer->mo) {
      extra->prevx = viewplayer->mo->PrevX;
      extra->prevy = viewplayer->mo->PrevY;
      extra->prevz = viewplayer->prev_viewz;
      extra->prevangle = viewplayer->prev_viewangle;
      extra->prevpitch = viewplayer->prev_viewpitch;
    }
  }
  extra->gametic = gametic;
  extra->gameticfrac = tic_vars.frac;
  extra->gameaction = gameaction;
  extra->turnheld = turnheld;
  extra->extra_size = sizeof(*extra);
  extra->autorun = autorun;
  extra->gamestate = gamestate;
  extra->FinaleStage = FinaleStage;
  extra->FinaleCount = FinaleCount;
  extra->itemOn = itemOn;
  extra->whichSkull = whichSkull;
  extra->currentMenu = 0;
  extra->set_menu_itemon = set_menu_itemon;
  extra->menuactive = menuactive;
  for (i = 0; i < sizeof (menus) / sizeof(menus[0]); i++)
    if (menus[i] == currentMenu)
      extra->currentMenu = i;
  for (i = 0; i < NUMKEYS; i++)
    extra->gamekeydown[i] = gamekeydown[i];
  for (i = 0; i < MAX_BUTTON_BINDS; i++)
	extra->old_input[i] = old_input[i];
  WI_Save(&extra->wi_state);
  return true;
}

bool retro_unserialize(const void *data_, size_t size)
{
  const struct extra_serialize *extra = data_;
  int gameless = 0;
  if (extra->extra_size == sizeof(*extra))
    gameless = (extra->gamestate != GS_LEVEL);
  if (!gameless) {
    int ret = G_DoLoadGameFromBuffer((char *) data_ + extra->extra_size,
				     size - extra->extra_size);
    if (!ret)
      return false;

    if (viewplayer && viewplayer->mo) {
      viewplayer->mo->PrevX = extra->prevx;
      viewplayer->mo->PrevY = extra->prevy;
      viewplayer->prev_viewz = extra->prevz;
      viewplayer->prev_viewangle = extra->prevangle;
      viewplayer->prev_viewpitch = extra->prevpitch;
    }
  }
  if (extra->extra_size == sizeof(*extra))
    {
      unsigned i;
      gametic = maketic = extra->gametic;
      gameaction = extra->gameaction;
      turnheld = extra->turnheld;
      autorun = extra->autorun;
      gamestate = extra->gamestate;
      FinaleStage = extra->FinaleStage;
      FinaleCount = extra->FinaleCount;
      WI_Load(&extra->wi_state);
      itemOn = extra->itemOn;
      whichSkull = extra->whichSkull;
      currentMenu = menus[extra->currentMenu];
      set_menu_itemon = extra->set_menu_itemon;
      for (i = 0; i < NUMKEYS; i++)
	gamekeydown[i] = extra->gamekeydown[i];
      for (i = 0; i < MAX_BUTTON_BINDS; i++)
	old_input[i] = extra->old_input[i];
      menuactive = extra->menuactive;
      tic_vars.frac = extra->gameticfrac;
    }
  return true;
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
   unsigned i;
   (void)index;(void)enabled;

   if (!code || !cheats_enabled)
      return;

   /* Note: it is not currently possible to disable
    * cheats once active... */

   /* Check if cheat can be applied now */
   if ((gamestate == GS_LEVEL) && !demoplayback)
   {
      for (i = 0; code[i] != '\0'; i++)
         M_FindCheats(code[i]);
   }
   /* Current game state does not support cheat
    * activation - add code to pending list */
   else
   {
      bool code_found = false;

      for (i = 0; i < RBUF_LEN(cheats_pending_list); i++)
      {
         const char *existing_code = cheats_pending_list[i];
         if (existing_code && !strcmp(existing_code, code))
         {
            code_found = true;
            break;
         }
      }

      if (!code_found)
      {
         RBUF_PUSH(cheats_pending_list, strdup(code));
         cheats_pending = true;
      }
   }
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
	bool run_key          = false;
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

static void
process_input(void)
{
   int port;
   unsigned i;
   static int cur_mx;
   static int cur_my;
   int mx, my;
   static bool old_input_kb[117];
   bool new_input_kb[117];
   int16_t ret = 0;

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

void I_StartTic (void)
{
  if (!input_poll_cb)
    return;
  input_poll_cb();
  process_input();
}

static void I_UpdateVideoMode(void)
{
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
   if (!video_cb)
     return;
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
}

/* i_system - i_main */

static dbool   InDisplay = false;

dbool   I_StartDisplay(void)
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
dbool   HasTrailingSlash(const char* dn)
{
  size_t dn_len = strlen(dn);
  return ( dn && ((dn[dn_len - 1] == '/') || (dn[dn_len - 1] == '\\')));
}

/**
 * FindFileInDir
 **
 * Checks if directory contains a file with given name and extension.
 * returns the path to the file if it exists and is readable or NULL otherwise
 */
char* FindFileInDir(const char* dir, const char* wfname, const char* ext)
{
   char * p;
   /* Precalculate a length we will need in the loop */
   size_t pl = strlen(wfname) + (ext ? strlen(ext) : 0) + 4;

   if(!dir)
   {
      if (!(p = malloc(pl)))
         return NULL;
      sprintf(p, "%s", wfname);
   }
   else
   {
     if (!(p = malloc(strlen(dir) + pl)))
        return NULL;
     sprintf(p, "%s%c%s", dir, DIR_SLASH, wfname);
   }

   if (ext && ext[0] != '\0')
      strcat(p, ext);

   if (path_is_valid(p))
   {
      if (log_cb)
         log_cb(RETRO_LOG_DEBUG, "FindFileInDir: found %s\n", p);
      return p;
   }
   else if (log_cb)
      log_cb(RETRO_LOG_ERROR, "FindFileInDir: not found %s in %s\n", wfname, dir);

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
     if (system_dir && (prboom_system_dir = malloc(strlen(system_dir) + 8)))
     {
       sprintf(prboom_system_dir, "%s%c%s", system_dir, DIR_SLASH, "prboom");
       p = FindFileInDir(prboom_system_dir, wfname, ext);
       free(prboom_system_dir);
       if (!p)
         p = FindFileInDir(system_dir, wfname, ext);
     }

     // If not found, check on parent folders recursively (if configured to do so)
     if (!p && find_recursive_on)
     {
       if ((dir = malloc(strlen(g_wad_dir) + 1)) != NULL)
       {
         strcpy(dir, g_wad_dir);
         for (i = strlen(dir)-1; i > 1; dir[i--] = '\0')
         {
           if((dir[i] == '/' || dir[i] == '\\')
              && dir[i-1] != dir[i])
           {
             dir[i] = '\0'; // remove leading slash
             p = FindFileInDir(dir, wfname, ext);
             if(p != NULL)
                break;
           }
         }
         free(dir);
       }
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

void R_InitInterpolation(void)
{
  struct retro_system_av_info info;
  retro_get_system_av_info(&info);
  if(tic_vars.fps != info.timing.fps)
  {
     // Only update av_info if changed and it's not the first run
     if(tic_vars.fps)
        environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &info);

     tic_vars.fps = info.timing.fps;
     tic_vars.frac_step = FRACUNIT * TICRATE / tic_vars.fps;
     tic_vars.sample_step = info.timing.sample_rate / tic_vars.fps;

     if (log_cb)
        log_cb(RETRO_LOG_DEBUG, "R_InitInterpolation: Framerate set to %.2f FPS\n", info.timing.fps);
  }
  tic_vars.frac = FRACUNIT;
}

int lprintf(OutputLevels pri, const char *s, ...)
{
  va_list v;
  char msg[MAX_LOG_MESSAGE_SIZE];

  if (!log_cb)
     return 0;

  va_start(v,s);
#ifdef HAVE_VSNPRINTF
  vsnprintf(msg,sizeof(msg),s,v);
#else
  vsprintf(msg,s,v);
#endif
  va_end(v);

  if (log_cb)
  {
    enum retro_log_level lvl;
    switch(pri)
    {
       case LO_DEBUG:   
          lvl = RETRO_LOG_DEBUG;
          break;
       case LO_CONFIRM:
       case LO_INFO:
          lvl = RETRO_LOG_INFO;
          break;
       case LO_WARN:
          lvl = RETRO_LOG_WARN;
          break;
       case LO_ERROR:
       case LO_FATAL:
       default:
          lvl = RETRO_LOG_ERROR;
          break;
    }

    log_cb(lvl, "%s", msg);
  }

  return 0;
}
