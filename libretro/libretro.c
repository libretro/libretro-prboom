#ifndef PSX
#include <sys/stat.h>
#else
#include <psx.h>
#endif

#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif
#include <errno.h>
#include <stdarg.h>
#include <time.h>

#include <libretro.h>
#include <file/file_path.h>
#include <streams/file_stream.h>
#include <vfs/vfs_implementation.h>
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
#include "../src/dsda_hacked.h"
#include "../src/m_fixed.h"
#include "../src/m_argv.h"
#include "../src/i_system.h"
#include "../src/i_sound.h"
#include "../src/s_sound.h"
#include "../src/v_video.h"
#include "../src/st_stuff.h"
#include "../src/w_wad.h"
#include "../src/r_draw.h"
#include "../src/r_main.h"
#include "../src/doomdef.h"
#include "../src/r_filter.h"
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
char *rfgets(char *buffer, int maxCount, RFILE *stream);

/* i_system */
int ms_to_next_tick;
int mus_opl_gain = 250; /* fine tune OPL output level */

int SCREENPITCH;
int SCREENWIDTH  = 320;
int SCREENHEIGHT = 200;
/* Width a 4:3 buffer of the current height would have.  Set whenever
 * the resolution option is parsed; the aspect-ratio selector scales
 * SCREENWIDTH relative to this so the vertical FOV stays vanilla. */
static int base_width_43 = 320;

/* i_video */
static unsigned char *screen_buf = NULL;
static bool have_sw_fb           = false;
static bool sw_fb_checked        = false;

/* Direct-render state.  When the frontend's framebuffer pitch
 * matches our renderer's stride (SCREENWIDTH*SURFACE_PIXEL_DEPTH),
 * I_StartDisplay points screens[0].data at the frontend buffer so
 * the column drawers write directly into it.  This eliminates the
 * per-frame memcpy(fb.data, screen_buf, ...) of ~125 KB at
 * 320x200 RGB565, scaling linearly with resolution.
 *
 * direct_fb_data is non-NULL only between I_StartDisplay (or the
 * wipe-reentry equivalent) and I_FinishUpdate of the same frame. */
static unsigned char *direct_fb_data  = NULL;
static unsigned int   direct_fb_pitch = 0;

/* True only while we are inside retro_run.  retro_load_game
 * calls D_DoomLoop a few times during init, but the frontend's
 * video driver isn't fully wired up at that point and
 * GET_CURRENT_SOFTWARE_FRAMEBUFFER can crash with a nullptr deref
 * inside the frontend's video pipeline.  Skip SW FB acquisition
 * outside retro_run; render to screen_buf instead. */
static bool in_retro_run = false;

/* Set by the in-game Aspect Ratio menu item; consumed at a safe
 * point in retro_run (see I_SetAspectRatio / I_ApplyAspectRatio). */
static bool aspect_change_pending = false;

/* libretro */
static char g_wad_dir[1024];
static char g_basename[1024];
static char g_save_dir[1024];

/* Cheat handling */
static bool cheats_enabled = false;
static bool cheats_pending = false;
static char **cheats_pending_list = NULL;

/* forward decls */
bool D_DoomMainSetup(void);
void D_DoomLoop(void);
void M_QuitDOOM(int choice);
void D_DoomDeinit(void);
void I_SetRes(void);
void I_SetAspectRatio(void);
static void I_ApplyAspectRatio(void);
void I_UpdateSound(void);
void M_EndGame(int choice);

retro_log_printf_t log_cb;
static retro_perf_get_time_usec_t perf_get_time_usec_cb = NULL;
/* Optional raw-MIDI output interface from the frontend, used by the
 * "libretro" MIDI Hardware option (see I_LibretroMidi* in libretro_midiout.c).
 * NULL when the frontend exposes no MIDI interface or MIDI output is
 * disabled, in which case that music player declines to register. */
static struct retro_midi_interface midi_iface;
static bool                        midi_iface_valid = false;
static retro_video_refresh_t video_cb;
retro_audio_sample_batch_t audio_batch_cb;

/* Float audio output, negotiated once in retro_load_game via
 * RETRO_ENVIRONMENT_GET_AUDIO_SAMPLE_BATCH_FLOAT. use_float_output stays 0
 * (and audio_batch_cb_float NULL) on any frontend that doesn't support it,
 * leaving the int16 path byte-identical. Read by the mixer in
 * libretro_sound.c. */
retro_audio_sample_batch_float_t audio_batch_cb_float = NULL;
int use_float_output = 0;
static retro_environment_t environ_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

/* prboom's music is synthesised in real time, so the core can output at
 * whichever of its supported rates best fits the host instead of a fixed
 * 44100.  The chosen rate (set from the "Sound Samplerate (Hint)" core
 * option, resolved against the frontend's target rate when "Auto") feeds
 * info.timing.sample_rate and the sound layer's snd_samplerate_output. */
#ifndef RETRO_ENVIRONMENT_GET_TARGET_SAMPLE_RATE
/* Added to libretro.h after the copy bundled here; data is unsigned* (Hz).
 * Guarded so this still builds against the in-tree header and any newer
 * one that already defines it. */
#define RETRO_ENVIRONMENT_GET_TARGET_SAMPLE_RATE (81 | RETRO_ENVIRONMENT_EXPERIMENTAL)
#endif

/* Resolved output rate currently advertised to the frontend (Hz).  Kept in
 * sync with snd_samplerate_output; compared in R_InitInterpolation to
 * decide whether timing changed enough to warrant SET_SYSTEM_AV_INFO. */
static int audio_sample_rate = 44100;

static void process_input(void);

/* Backing storage for the const char **myargv that prboom's argv
 * machinery reads.  We populate this in retro_load_game and free
 * each heap-allocated slot in retro_unload_game (see
 * free_load_argv below).  Every populated slot is heap-allocated
 * so the cleanup is uniform.
 *
 * Sizing: an m3u playlist load (see issue #196) expands to
 * `prboom -iwad <path> -file <p1> <p2> ... -deh <d1> ... -baseconfig <c>`,
 * so the slot count grows with the playlist length.  64 covers up
 * to ~55 entries which is well past any realistic mod stack. */
static char *load_argv[64];

static void free_load_argv(void)
{
   unsigned i;
   for (i = 0; i < sizeof(load_argv)/sizeof(load_argv[0]); i++)
   {
      free(load_argv[i]);
      load_argv[i] = NULL;
   }
}

#define MAX_PADS 1
static unsigned doom_devices[1];

/* Whether mouse active when using Gamepad */
dbool   mouse_on;
extern int wall_decals_enabled;   /* src/r_decal.c -- frontend toggle */
/* Whether to search for IWADs on parent folders recursively */
dbool   find_recursive_on;
#ifdef HAVE_MMAP
/* Core option "prboom-mmap_wads" (default off): memory-map WAD files in
 * W_AddFile instead of reading them fully into RAM.  Read by src/w_wad.c. */
int     prboom_mmap_wads = 0;
#endif

// System analog stick range is -0x8000 to 0x8000
#define ANALOG_RANGE 0x8000
// This is scaled by in-game 'mouse sensitivity' option, so just choose a value
// that has acceptable performance at the default sensitivity value
// (i.e. user can easily change mouse speed, so absolute value here is not critical)
#define ANALOG_MOUSE_SPEED 128
/* Default deadzone: 15% */
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

/* Heretic "Gamepad Modern" layout.
 *
 * Heretic adds inventory and look controls on top of the Doom action set.
 * The engine bindings the core currently exposes are the shared movement /
 * fire / use / weapon-cycle / map / menu keys, so the inventory and look
 * actions are labelled per the intended Heretic scheme and mapped to the
 * nearest available engine action until dedicated Heretic binds
 * (key_invleft / key_invright / key_useartifact / key_lookcenter / jump)
 * are plumbed through. Movement / aim are on the analog sticks.
 *
 *   Left stick : Move / Strafe
 *   Right stick: Aim / Look (turn)
 *   R2         : Fire
 *   L2         : Use / activate selected inventory item
 *   R1 / L1    : Cycle inventory (right / left)
 *   X (Cross)  : Jump / confirm
 *   Square     : Interact / open doors
 *   Circle     : Run / alt-fire
 *   Triangle   : Look center
 *   Select     : Automap
 *   Start      : Pause menu
 */
static gamepad_layout_t gp_heretic_modern = {
	{
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "Run / Alt-Fire" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "Jump / Confirm" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Interact / Open Door" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Look Center" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "Cycle Inventory Left" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "Cycle Inventory Right" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,     "Use Inventory Item" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "Fire" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,     "Toggle Run" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,     "180 Turn" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Automap" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Pause Menu" },
		{ 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Strafe" },
		{ 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Move" },
		{ 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Look" },
		{ 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Look Up/Down" },
		{ 0 },
	},
	{	// gamekey,             menukey      (indexed by RETRO_DEVICE_ID_JOYPAD)
		{ &key_speed,           &key_menu_backspace }, // 0  B  : Run / Alt-Fire
		{ &key_use,             &key_menu_enter },     // 1  Y  : Look Center (stand-in: use)
		{ &key_map,             &key_menu_backspace }, // 2  SELECT: Automap
		{ &key_menu_escape,     &key_menu_escape },    // 3  START : Pause Menu
		{ &key_map_up,          &key_menu_up },        // 4  UP
		{ &key_map_down,        &key_menu_down },      // 5  DOWN
		{ &key_map_left,        &key_menu_left },      // 6  LEFT
		{ &key_map_right,       &key_menu_right },     // 7  RIGHT
		{ &key_menu_enter,      &key_menu_enter },     // 8  A  : Jump / Confirm (stand-in)
		{ &key_use,             &key_menu_backspace }, // 9  X  : Interact / Open Door
		{ &key_weaponcycledown, &key_menu_left },      // 10 L1 : Cycle Inventory Left
		{ &key_weaponcycleup,   &key_menu_right },     // 11 R1 : Cycle Inventory Right
		{ &key_use,             &key_menu_backspace }, // 12 L2 : Use Inventory Item (stand-in: use)
		{ &key_fire,            &key_menu_enter },     // 13 R2 : Fire
		{ &key_autorun,         &key_menu_enter },     // 14 L3 : Toggle Run
		{ &key_reverse,         &key_menu_backspace }, // 15 R3 : 180 Turn
	},
	16,
};

/* Heretic "Gamepad Classic" layout: the PS1-style Doom classic mapping
 * with Heretic-appropriate labels (the shoulder weapon-cycle buttons
 * double as inventory cycling). Same engine binds as Doom classic. */
static gamepad_layout_t gp_heretic_classic = {
	{
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "Strafe" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "Use / Open Door" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Fire" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Run" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "Strafe Left" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "Strafe Right" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,     "Cycle Inventory Left" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "Cycle Inventory Right" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,     "Toggle Run" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,     "180 Turn" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Automap" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Pause Menu" },
		{ 0 },
	},
	{	// gamekey,             menukey      (indexed by RETRO_DEVICE_ID_JOYPAD)
		{ &key_strafe,          &key_menu_backspace }, // 0  B
		{ &key_speed,           &key_menu_backspace }, // 1  Y
		{ &key_map,             &key_menu_backspace }, // 2  SELECT
		{ &key_menu_escape,     &key_menu_escape },    // 3  START
		{ &key_up,              &key_menu_up },        // 4  UP
		{ &key_down,            &key_menu_down },      // 5  DOWN
		{ &key_left,            &key_menu_left },      // 6  LEFT
		{ &key_right,           &key_menu_right },     // 7  RIGHT
		{ &key_use,             &key_menu_enter },     // 8  A
		{ &key_fire,            &key_menu_enter },     // 9  X
		{ &key_strafeleft,      &key_menu_left },      // 10 L1
		{ &key_straferight,     &key_menu_right },     // 11 R1
		{ &key_weaponcycledown, &key_menu_backspace }, // 12 L2 : Cycle Inventory Left
		{ &key_weaponcycleup,   &key_menu_enter },     // 13 R2 : Cycle Inventory Right
		{ &key_autorun,         &key_menu_enter },     // 14 L3
		{ &key_reverse,         &key_menu_backspace }, // 15 R3
	},
	16,
};

/* "Hexen Gamepad Modern" layout.
 *
 * Hexen on a modern pad: sticks move and turn, triggers run and fire, and
 * the Hexen-specific actions sit on real engine binds (key_jump,
 * key_use_artifact, key_inv_left / key_inv_right, key_fly_up /
 * key_fly_down) rather than stand-ins.  With only four weapons per class
 * the weapon cycle moves to the d-pad, freeing the shoulders for the
 * inventory and the face buttons for Jump and the artifact.
 *
 *   Left stick : Move / Strafe        Right stick: Turn
 *   R2 / L2    : Fire / Run           L1 / R1    : Cycle inventory
 *   A          : Jump                 Y          : Use / open door
 *   X          : Use inventory item   B          : Show last message
 *   D-pad Up/Down   : Fly up / down (with the Wings of Wrath)
 *   D-pad Left/Right: Previous / next weapon
 */
static gamepad_layout_t gp_hexen_modern = {
	{
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "Previous Weapon" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "Fly Up" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "Fly Down" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "Next Weapon" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "Show Last Message" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "Jump" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Use Inventory Item" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Use / Open Door" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "Cycle Inventory Left" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "Cycle Inventory Right" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,     "Run" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "Fire" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,     "Toggle Run" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,     "180 Turn" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Automap" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Pause Menu" },
		{ 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Strafe" },
		{ 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Move" },
		{ 0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Turn" },
		{ 0 },
	},
	{	// gamekey,             menukey      (indexed by RETRO_DEVICE_ID_JOYPAD)
		{ &key_enter,           &key_menu_backspace }, // 0  B  : Show Last Message
		{ &key_use,             &key_menu_enter },     // 1  Y  : Use / Open Door
		{ &key_map,             &key_menu_backspace }, // 2  SELECT: Automap
		{ &key_menu_escape,     &key_menu_escape },    // 3  START : Pause Menu
		{ &key_fly_up,          &key_menu_up },        // 4  UP : Fly Up
		{ &key_fly_down,        &key_menu_down },      // 5  DOWN : Fly Down
		{ &key_weaponcycledown, &key_menu_left },      // 6  LEFT : Previous Weapon
		{ &key_weaponcycleup,   &key_menu_right },     // 7  RIGHT: Next Weapon
		{ &key_jump,            &key_menu_enter },     // 8  A  : Jump
		{ &key_use_artifact,    &key_menu_backspace }, // 9  X  : Use Inventory Item
		{ &key_inv_left,        &key_menu_left },      // 10 L1 : Cycle Inventory Left
		{ &key_inv_right,       &key_menu_right },     // 11 R1 : Cycle Inventory Right
		{ &key_speed,           &key_menu_backspace }, // 12 L2 : Run
		{ &key_fire,            &key_menu_enter },     // 13 R2 : Fire
		{ &key_autorun,         &key_menu_enter },     // 14 L3 : Toggle Run
		{ &key_reverse,         &key_menu_backspace }, // 15 R3 : 180 Turn
	},
	16,
};

/* "Hexen Gamepad Classic" layout: the PS1-style classic mapping with the
 * Hexen actions on real binds.  Run becomes the L3 toggle so that Jump can
 * take the Y face button; the shoulders cycle the inventory, the triggers
 * cycle the four weapons, and the right stick click uses the selected
 * artifact. */
static gamepad_layout_t gp_hexen_classic = {
	{
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "Strafe" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "Use / Open Door" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Fire" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Jump" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "Cycle Inventory Left" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "Cycle Inventory Right" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,     "Previous Weapon" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "Next Weapon" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,     "Toggle Run" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,     "Use Inventory Item" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Automap" },
		{ 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Pause Menu" },
		{ 0 },
	},
	{	// gamekey,             menukey      (indexed by RETRO_DEVICE_ID_JOYPAD)
		{ &key_strafe,          &key_menu_backspace }, // 0  B  : Strafe
		{ &key_jump,            &key_menu_backspace }, // 1  Y  : Jump
		{ &key_map,             &key_menu_backspace }, // 2  SELECT: Automap
		{ &key_menu_escape,     &key_menu_escape },    // 3  START : Pause Menu
		{ &key_up,              &key_menu_up },        // 4  UP
		{ &key_down,            &key_menu_down },      // 5  DOWN
		{ &key_left,            &key_menu_left },      // 6  LEFT
		{ &key_right,           &key_menu_right },     // 7  RIGHT
		{ &key_use,             &key_menu_enter },     // 8  A  : Use / Open Door
		{ &key_fire,            &key_menu_enter },     // 9  X  : Fire
		{ &key_inv_left,        &key_menu_left },      // 10 L1 : Cycle Inventory Left
		{ &key_inv_right,       &key_menu_right },     // 11 R1 : Cycle Inventory Right
		{ &key_weaponcycledown, &key_menu_backspace }, // 12 L2 : Previous Weapon
		{ &key_weaponcycleup,   &key_menu_enter },     // 13 R2 : Next Weapon
		{ &key_autorun,         &key_menu_enter },     // 14 L3 : Toggle Run
		{ &key_use_artifact,    &key_menu_backspace }, // 15 R3 : Use Inventory Item
	},
	16,
};

/* Keyboard / mouse descriptors.  Unlike the gamepad layouts these document
 * the keyboard keys the Raven games bind: movement / fire / use plus the
 * inventory and flight keys.  Heretic uses spacebar for Use; Hexen reserves
 * spacebar for Jump (added separately) and uses E for Use. */
static const struct retro_input_descriptor kbd_heretic_desc[] = {
	{ 0, RETRO_DEVICE_KEYBOARD, 0, RETROK_UP,       "Move Forward" },
	{ 0, RETRO_DEVICE_KEYBOARD, 0, RETROK_DOWN,     "Move Backward" },
	{ 0, RETRO_DEVICE_KEYBOARD, 0, RETROK_LEFT,     "Turn Left" },
	{ 0, RETRO_DEVICE_KEYBOARD, 0, RETROK_RIGHT,    "Turn Right" },
	{ 0, RETRO_DEVICE_KEYBOARD, 0, RETROK_LCTRL,    "Fire" },
	{ 0, RETRO_DEVICE_KEYBOARD, 0, RETROK_SPACE,    "Use / Open Door" },
	{ 0, RETRO_DEVICE_KEYBOARD, 0, RETROK_e,        "Use / Open Door" },
	{ 0, RETRO_DEVICE_KEYBOARD, 0, RETROK_LSHIFT,   "Run" },
	{ 0, RETRO_DEVICE_KEYBOARD, 0, RETROK_LALT,     "Strafe" },
	{ 0, RETRO_DEVICE_KEYBOARD, 0, RETROK_q,        "Use Inventory Item" },
	{ 0, RETRO_DEVICE_KEYBOARD, 0, RETROK_c,        "Previous Inventory Item" },
	{ 0, RETRO_DEVICE_KEYBOARD, 0, RETROK_v,        "Next Inventory Item" },
	{ 0, RETRO_DEVICE_KEYBOARD, 0, RETROK_x,        "Fly Up" },
	{ 0, RETRO_DEVICE_KEYBOARD, 0, RETROK_z,        "Fly Down" },
	{ 0 },
};

static const struct retro_input_descriptor kbd_hexen_desc[] = {
	{ 0, RETRO_DEVICE_KEYBOARD, 0, RETROK_UP,       "Move Forward" },
	{ 0, RETRO_DEVICE_KEYBOARD, 0, RETROK_DOWN,     "Move Backward" },
	{ 0, RETRO_DEVICE_KEYBOARD, 0, RETROK_LEFT,     "Turn Left" },
	{ 0, RETRO_DEVICE_KEYBOARD, 0, RETROK_RIGHT,    "Turn Right" },
	{ 0, RETRO_DEVICE_KEYBOARD, 0, RETROK_LCTRL,    "Fire" },
	{ 0, RETRO_DEVICE_KEYBOARD, 0, RETROK_e,        "Use / Open Door" },
	{ 0, RETRO_DEVICE_KEYBOARD, 0, RETROK_SPACE,    "Jump" },
	{ 0, RETRO_DEVICE_KEYBOARD, 0, RETROK_LSHIFT,   "Run" },
	{ 0, RETRO_DEVICE_KEYBOARD, 0, RETROK_LALT,     "Strafe" },
	{ 0, RETRO_DEVICE_KEYBOARD, 0, RETROK_q,        "Use Inventory Item" },
	{ 0, RETRO_DEVICE_KEYBOARD, 0, RETROK_c,        "Previous Inventory Item" },
	{ 0, RETRO_DEVICE_KEYBOARD, 0, RETROK_v,        "Next Inventory Item" },
	{ 0, RETRO_DEVICE_KEYBOARD, 0, RETROK_x,        "Fly Up" },
	{ 0, RETRO_DEVICE_KEYBOARD, 0, RETROK_z,        "Fly Down" },
	{ 0 },
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

/**
 * FindFileInDir
 **
 * Checks if directory contains a file with given name and extension.
 * returns the path to the file if it exists and is readable or NULL otherwise
 *
 * On case-sensitive filesystems (Linux, *BSD, macOS HFS+ case-sensitive,
 * etc.) the literal-name path_is_valid() check fails when the on-disk
 * file uses different letter case than the caller asked for.  This
 * affects users a lot in practice: many distributions of the original
 * IWADs ship the file as DOOM.WAD / DOOM2.WAD / etc., while the
 * engine consistently asks lowercase -- standard_iwads[] entries are
 * all lowercase, hu_stuff and elsewhere look up "prboom.wad" in
 * lowercase, and the basename copied to -iwad from the libretro
 * frontend preserves whatever case the user-selected file had.  See
 * issues #186 ("WAD detection should not be case sensitive") and
 * #188 ("Doom1.wad megawads don't work" -- same root cause, exposed
 * via the IWAD-not-found path when a Doom 1 PWAD is loaded next to
 * an uppercase DOOM.WAD).
 *
 * After the literal lookup misses, scan `dir` once via the libretro
 * VFS dirent API and look for an entry whose name matches `wfname`
 * (+ optional ext) case-insensitively.  On a hit we rebuild the
 * result path with the actual on-disk filename so subsequent
 * filestream_open calls see a path that resolves.
 *
 * On Windows the literal-name path_is_valid() succeeds regardless
 * of casing (NTFS / FAT are case-insensitive at the OS layer), so
 * the fallback never fires there.  retro_vfs_opendir_impl handles
 * platform differences internally; this code path is
 * platform-portable. */
static char *find_in_dir_case_insensitive(const char *dir,
                                          const char *wfname,
                                          const char *ext)
{
   libretro_vfs_implementation_dir *dh;
   char *want;
   char *match = NULL;
   size_t want_len;

   if (!dir || !wfname)
      return NULL;
   dh = retro_vfs_opendir_impl(dir, false);
   if (!dh)
      return NULL;

   want_len = strlen(wfname) + (ext ? strlen(ext) : 0);
   want = malloc(want_len + 1);
   if (!want)
   {
      retro_vfs_closedir_impl(dh);
      return NULL;
   }
   strcpy(want, wfname);
   if (ext && *ext)
      strcat(want, ext);

   while (retro_vfs_readdir_impl(dh))
   {
      const char *de = retro_vfs_dirent_get_name_impl(dh);
      if (de && !strcasecmp(de, want))
      {
         match = malloc(strlen(dir) + 1 + strlen(de) + 1);
         if (match)
            sprintf(match, "%s%c%s", dir, DIR_SLASH, de);
         break;
      }
   }
   free(want);
   retro_vfs_closedir_impl(dh);
   return match;
}

/* Given an absolute or already-prefixed file path, return a strdup'd
 * version with the correct on-disk case for the final component,
 * even if the supplied case doesn't match.  Returns NULL if no
 * matching entry is in the directory.  Useful for resolving entries
 * a user typed into an m3u playlist (`doom2.wad`) when the on-disk
 * file is `DOOM2.WAD`: filestream_open won't tolerate the mismatch
 * on case-sensitive filesystems.
 *
 * If the supplied path already resolves verbatim, returns a plain
 * strdup of it -- no directory scan, no allocation churn beyond
 * the dup.  Only the case-mismatch path triggers an opendir + scan.
 *
 * Only the final component is case-folded; the directory portion
 * is taken as-is.  In practice the directory is one the frontend
 * handed us (already correctly cased) and only the basename came
 * from user-typed input. */
static char *canonicalize_path_casefold(const char *path)
{
   char *sep;
   char *dir;
   char *match;

   if (!path || !*path)
      return NULL;
   if (path_is_valid(path))
      return strdup(path);

   /* Split into dir + basename. */
   sep = strrchr(path, '/');
   if (!sep)
      sep = strrchr(path, '\\');
   if (!sep)
      return NULL;  /* no directory component to scan */

   dir = malloc((sep - path) + 1);
   if (!dir)
      return NULL;
   memcpy(dir, path, sep - path);
   dir[sep - path] = '\0';

   match = find_in_dir_case_insensitive(dir, sep + 1, NULL);
   free(dir);
   return match;
}

static char *FindFileInDir(const char* dir, const char* wfname, const char* ext)
{
   char * p;
   /* Precalculate a length we will need in the loop */
   size_t pl = strlen(wfname) + (ext ? strlen(ext) : 0) + 4;

   if(!dir)
   {
      if (!(p = malloc(pl)))
         return NULL;
      strcpy(p, wfname);
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

   /* Case-insensitive fallback for case-sensitive filesystems.  See
    * the comment on find_in_dir_case_insensitive() above.  Only
    * attempted when a real directory was supplied (the no-dir branch
    * above is for callers passing already-prefixed paths). */
   if (dir)
   {
      char *cif = find_in_dir_case_insensitive(dir, wfname, ext);
      if (cif)
      {
         free(p);
         if (log_cb)
            log_cb(RETRO_LOG_DEBUG,
                   "FindFileInDir: case-insensitive match %s for %s\n",
                   cif, wfname);
         return cif;
      }
   }

   if (log_cb)
      log_cb(RETRO_LOG_ERROR, "FindFileInDir: not found %s in %s\n", wfname, dir);

   free(p);
   return NULL;
}


static bool libretro_supports_bitmasks = false;

/* High-resolution wall-clock in microseconds for the optional render
 * profiler, sourced from the libretro perf interface.  Returns 0.0 if the
 * frontend did not provide a perf interface. */
double I_RenderProfileUsec(void)
{
   if (perf_get_time_usec_cb)
      return (double)perf_get_time_usec_cb();
#if defined(CLOCK_MONOTONIC) && !defined(_WIN32)
   {
      /* Fallback when the frontend exposes no perf interface (e.g. a
       * headless test harness): use the monotonic clock directly so the
       * compile-time render profiler still produces real numbers. */
      struct timespec ts;
      if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
         return (double)ts.tv_sec * 1000000.0 + (double)ts.tv_nsec / 1000.0;
   }
#endif
   return 0.0;
}

/* Raw-MIDI output bridge for the "libretro" MIDI Hardware player
 * (libretro_midiout.c).  These thin wrappers hide the frontend
 * retro_midi_interface so the player TU does not need libretro.h
 * globals.  I_LibretroMidiAvailable reports whether the frontend
 * gave us a usable, output-enabled MIDI interface; the player's
 * init/registersong decline when it returns 0. */
int I_LibretroMidiAvailable(void)
{
   if (!midi_iface_valid)
      return 0;
   /* output_enabled may be NULL on a partial interface, and may
    * toggle at runtime; treat a missing query as "no". */
   if (!midi_iface.write || !midi_iface.output_enabled)
      return 0;
   return midi_iface.output_enabled() ? 1 : 0;
}

/* Write one MIDI byte to the frontend.  delta_us is microseconds since
 * the previous write (the frontend uses it for output scheduling).
 * Returns 1 on success. */
int I_LibretroMidiWrite(unsigned char byte, unsigned delta_us)
{
   if (!midi_iface_valid || !midi_iface.write)
      return 0;
   return midi_iface.write(byte, (uint32_t)delta_us) ? 1 : 0;
}

/* Flush any buffered MIDI output.  Called once per rendered music
 * chunk after the player has emitted that chunk's events. */
void I_LibretroMidiFlush(void)
{
   if (midi_iface_valid && midi_iface.flush)
      midi_iface.flush();
}

/* Bounds for frontend-driven zone-cache sizing via
 * RETRO_ENVIRONMENT_GET_MEMORY_STATUS. The lump cache lives on top of the
 * WAD image (which is loaded separately with malloc), so we take only a
 * quarter of reported free memory, cap it at 1 GB -- ample for even the
 * largest PWADs, some of which exceed 600 MB -- and never drop below a
 * small floor. Non-MEMORY_LOW builds otherwise default to 0 (unlimited). */
#define ZONE_CAP_BYTES   (1024ULL * 1024ULL * 1024ULL)  /* 1 GB ceiling */
#define ZONE_FLOOR_BYTES (16ULL   * 1024ULL * 1024ULL)  /* sane minimum */

void retro_init(void)
{
   struct retro_log_callback log;
   unsigned level = 4;
   SCREENPITCH    = (SCREENWIDTH * SURFACE_PIXEL_DEPTH);

   Z_Init(); /* 1/18/98 killough: start up memory stuff first */

   if(environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = NULL;

#ifndef MEMORY_LOW
   {
      /* Size the zone-cache limit to the host machine when the frontend can
       * report its memory, rather than leaving it unbounded. A value of 0
       * (the compile-time default here) means "no limit"; any positive value
       * is the size at which Z_Malloc starts purging PU_CACHE. Frontends that
       * do not implement the query leave that default in place. */
      struct retro_memory_status memstat;
      memstat.free = memstat.total = 0;
      if (environ_cb(RETRO_ENVIRONMENT_GET_MEMORY_STATUS, &memstat) && memstat.free)
      {
         unsigned long long budget = memstat.free / 4;
         if (budget > ZONE_CAP_BYTES)
            budget = ZONE_CAP_BYTES;
         if (budget < ZONE_FLOOR_BYTES)
            budget = ZONE_FLOOR_BYTES;
         Z_SetHeapCap((int)budget);
         if (log_cb)
            log_cb(RETRO_LOG_INFO,
                   "Frontend reports %llu MB free; capping Doom zone cache at %llu MB.\n",
                   (unsigned long long)(memstat.free >> 20),
                   (unsigned long long)(budget >> 20));
      }
      else if (log_cb)
         log_cb(RETRO_LOG_INFO,
                "No memory-status query; Doom zone cache left unlimited (default).\n");
   }
#endif

   {
      /* Optional: high-resolution timer for the compile-time render
       * profiler (I_RenderProfileUsec).  Harmless if unsupported. */
      struct retro_perf_callback perf;
      if (environ_cb(RETRO_ENVIRONMENT_GET_PERF_INTERFACE, &perf))
         perf_get_time_usec_cb = perf.get_time_usec;
      else
         perf_get_time_usec_cb = NULL;
   }

   {
      /* Optional: raw-MIDI output interface for the "libretro" MIDI
       * Hardware option.  The frontend routes the bytes we write to a
       * real or virtual MIDI device.  Absent on most setups, in which
       * case the libretro MIDI player simply declines and the user
       * gets Adlib/Fluidsynth as before. */
      if (environ_cb(RETRO_ENVIRONMENT_GET_MIDI_INTERFACE, &midi_iface))
         midi_iface_valid = true;
      else
         midi_iface_valid = false;
   }

   /* Negotiate pixel format with the frontend.  The renderer is
    * hardcoded to write 16-bit pixels via VID_PAL16 -- the
    * surface depth is fixed at SURFACE_PIXEL_DEPTH=2 and every
    * draw column / span helper writes uint16_t directly.  We
    * cannot fall through to XRGB1555 silently if RGB565 is
    * rejected; the output would be unspecified-format garbage
    * (with the wrong red/blue channel widths and an alpha bit
    * smeared across the green LSB).  Log a hard error so the
    * user sees a visible diagnostic instead of a corrupted
    * screen. */
   {
      enum retro_pixel_format rgb565 = RETRO_PIXEL_FORMAT_RGB565;
      if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &rgb565))
      {
         if (log_cb)
            log_cb(RETRO_LOG_ERROR,
                   "Frontend rejected RGB565 pixel format -- this "
                   "core requires it.  Update RetroArch or your "
                   "frontend to a build that supports RGB565.\n");
      }
      else if (log_cb)
      {
         log_cb(RETRO_LOG_DEBUG,
                "Frontend accepted RGB565 pixel format.\n");
      }
   }

   if (environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL))
      libretro_supports_bitmasks = true;

   environ_cb(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL, &level);
}

void retro_deinit(void)
{
   libretro_supports_bitmasks = false;
   have_sw_fb                 = false;
   sw_fb_checked              = false;

   retro_set_rumble_damage(0, 0.0f);
   retro_set_rumble_touch(0, 0.0f);

   memset(&rumble, 0, sizeof(struct retro_rumble_interface));
   rumble_enabled         = false;
   rumble_damage_strength = 0;
   rumble_damage_counter  = -1;
   rumble_touch_strength  = 0;
   rumble_touch_counter   = -1;

   /* Free the dsdhacked growable tables and reset their globals to the
    * static seeds before the zone is torn down, so a subsequent content
    * load re-seeds cleanly instead of dangling. */
   dsda_FreeTables();

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
   info->valid_extensions = "wad|iwad|pwad|lmp|m3u|pk3|ipk3|zip";
   info->block_extract    = false;
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
      info->timing.fps = 105.0;
      break;
    case 10:
      info->timing.fps = 119.0;
      break;
    case 11:
      info->timing.fps = 120.0;
      break;
    case 12:
      info->timing.fps = 140.0;
      break;
    case 13:
      info->timing.fps = 144.0;
      break;
    case 14:
      info->timing.fps = 155.0;
      break;
    case 15:
      info->timing.fps = 160.0;
      break;
    case 16:
      info->timing.fps = 165.0;
      break;
    case 17:
      info->timing.fps = 175.0;
      break;
    case 18:
      info->timing.fps = 180.0;
      break;
    case 19:
      info->timing.fps = 200.0;
      break;
    case 20:
      info->timing.fps = 210.0;
      break;
    case 21:
      info->timing.fps = 240.0;
      break;
    case 22:
      info->timing.fps = 244.0;
      break;
    case 23:
      info->timing.fps = 245.0;
      break;
    case 24:
      info->timing.fps = 280.0;
      break;
    case 25:
      info->timing.fps = 300.0;
      break;
    case 26:
      info->timing.fps = 315.0;
      break;
    case 27:
      info->timing.fps = 320.0;
      break;
    case 28:
      info->timing.fps = 350.0;
      break;
    case 29:
      info->timing.fps = 360.0;
      break;
    case 30:
      info->timing.fps = 385.0;
      break;
    case 31:
      info->timing.fps = 420.0;
      break;
    case 32:
      info->timing.fps = 455.0;
      break;
    case 33:
      info->timing.fps = 480.0;
      break;
    case 34:
      info->timing.fps = 490.0;
      break;
    case 35:
      info->timing.fps = 540.0;
      break;
    default:
      info->timing.fps = TICRATE;
      break;
  }
  info->timing.sample_rate    = (double)audio_sample_rate;
  info->geometry.base_width   = SCREENWIDTH;
  info->geometry.base_height  = SCREENHEIGHT;
  info->geometry.max_width    = MAX_SCREENWIDTH;
  info->geometry.max_height   = MAX_SCREENHEIGHT;
  switch (render_aspect)
  {
    case 1:  info->geometry.aspect_ratio = 16.0 / 9.0;  break;
    case 2:  info->geometry.aspect_ratio = 16.0 / 10.0; break;
    case 3:  info->geometry.aspect_ratio = 32.0 / 9.0;  break;
    case 4:  info->geometry.aspect_ratio = 64.0 / 27.0; break;
    default: info->geometry.aspect_ratio = 4.0 / 3.0;   break;
  }
}


void retro_set_environment(retro_environment_t cb)
{
   struct retro_vfs_interface_info vfs_iface_info;
   static bool libretro_supports_option_categories = false;
   static const struct retro_controller_description port[] = {
		{ "Doom Gamepad Modern (OG Xbox Doom 3)", RETROPAD_MODERN },
		{ "Doom Gamepad Classic (PS1)", RETROPAD_CLASSIC },
		{ "Doom RetroKeyboard/Mouse", RETRO_DEVICE_KEYBOARD },
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
			{
				extern dbool heretic, hexen;
				environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS,
				           hexen   ? gp_hexen_classic.desc :
				           heretic ? gp_heretic_classic.desc : gp_classic.desc);
			}
			break;
		case RETROPAD_MODERN:
			doom_devices[port] = RETROPAD_MODERN;
			{
				extern dbool heretic, hexen;
				environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS,
				           hexen   ? gp_hexen_modern.desc :
				           heretic ? gp_heretic_modern.desc : gp_modern.desc);
			}
			break;
		case RETRO_DEVICE_KEYBOARD:
			doom_devices[port] = RETRO_DEVICE_KEYBOARD;
			{
				extern dbool heretic, hexen;
				if (hexen)
					environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS,
					           (void *)kbd_hexen_desc);
				else if (heretic)
					environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS,
					           (void *)kbd_heretic_desc);
				else
					environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS,
					           gp_classic.desc);
			}
			break;
		default:
			if (log_cb)
				log_cb(RETRO_LOG_ERROR, "Invalid libretro controller device, using default: RETROPAD_CLASSIC\n");
			doom_devices[port] = RETROPAD_CLASSIC;
			{
				extern dbool heretic, hexen;
				environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS,
				           hexen   ? gp_hexen_classic.desc :
				           heretic ? gp_heretic_classic.desc : gp_classic.desc);
			}
	}
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
   /* Stubbed -- this core uses retro_set_audio_sample_batch instead.
    * The libretro frontend always calls this setter, so we keep the
    * symbol but ignore the callback. */
   (void)cb;
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

/* Snap an arbitrary host rate to the nearest rate prboom renders at. */
static int prboom_nearest_supported_rate(unsigned host_rate)
{
   if      (host_rate <= (32000u + 44100u) / 2) return 32000;
   else if (host_rate <= (44100u + 48000u) / 2) return 44100;
   else if (host_rate <= (48000u + 96000u) / 2) return 48000;
   return 96000;
}

/* Resolve the "Sound Samplerate (Hint)" core option to a concrete rate and
 * apply it.  "auto" asks the frontend for its target rate via
 * RETRO_ENVIRONMENT_GET_TARGET_SAMPLE_RATE and snaps to the nearest
 * supported value (falling back to 44100 if the frontend doesn't implement
 * the call).  Updates audio_sample_rate and, when the sound system is
 * already up, retunes it through I_SetSoundRate.  The AV-info side effects
 * (timing.sample_rate, SET_SYSTEM_AV_INFO) are handled by
 * R_InitInterpolation, which the caller invokes after content is loaded. */
static void update_audio_samplerate(void)
{
   struct retro_variable var;
   int chosen = 44100;

   var.key   = "prboom-sound_samplerate";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "auto"))
      {
         unsigned host_rate = 0;
         if (environ_cb(RETRO_ENVIRONMENT_GET_TARGET_SAMPLE_RATE, &host_rate)
               && host_rate > 0)
            chosen = prboom_nearest_supported_rate(host_rate);
         else
            chosen = 44100;  /* frontend can't tell us; safe default */
      }
      else
         chosen = atoi(var.value);  /* "32000".."96000" */
   }

   audio_sample_rate = chosen;

   /* If the sound system is already running (option toggled in-game), apply
    * immediately; otherwise this just primes snd_samplerate_output for the
    * upcoming I_InitSound/I_InitMusic.  I_SetSoundRate clamps, no-ops when
    * unchanged, and re-tunes SFX + music as needed. */
   I_SetSoundRate(chosen);
}

static void update_variables(bool startup)
{
   struct retro_variable var;

#ifdef HAVE_MMAP
   var.key   = "prboom-mmap_wads";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      prboom_mmap_wads = !strcmp(var.value, "enabled");
#endif

   if (startup)
   {
      var.key = "prboom-resolution";
      var.value = NULL;

      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      {
         char str[100];
         char *sep;
         strlcpy(str, var.value, sizeof(str));

         /* Parse "WIDTHxHEIGHT" without strtok: split on the first 'x'. */
         sep = strchr(str, 'x');
         if (sep)
            *sep = '\0';
#ifdef PSX
          {
             SCREENWIDTH = (unsigned long)strtol(str, NULL, 0);
	     SCREENPITCH = (SCREENWIDTH * SURFACE_PIXEL_DEPTH);
          }
          if (sep)
             SCREENHEIGHT = (unsigned long)strtol(sep + 1, NULL, 0);
#else
         {
            SCREENWIDTH = strtoul(str, NULL, 0);
	    SCREENPITCH = (SCREENWIDTH * SURFACE_PIXEL_DEPTH);
         }
         if (sep)
            SCREENHEIGHT = strtoul(sep + 1, NULL, 0);
#endif

         if (log_cb)
            log_cb(RETRO_LOG_DEBUG, "Got size: %u x %u.\n", SCREENWIDTH, SCREENHEIGHT);
      }
      else
      {
         SCREENWIDTH  = 320;
         SCREENHEIGHT = 200;
	 SCREENPITCH  = (SCREENWIDTH * SURFACE_PIXEL_DEPTH);
      }

      /* The parsed width is the 4:3 reference for this height; the
       * aspect-ratio selector widens SCREENWIDTH relative to it.
       * render_aspect isn't known until the config is loaded, so the
       * initial widen happens later in retro_load_game. */
      base_width_43 = SCREENWIDTH;
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

   var.key = "prboom-wall_decals";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      wall_decals_enabled = !strcmp(var.value, "enabled") ? 1 : 0;

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

   /* Resolve and apply the audio output rate.  At startup this primes
    * snd_samplerate_output before I_InitSound/I_InitMusic.  When toggled
    * in-game, I_SetSoundRate retunes the live SFX pipeline and re-inits the
    * synth backends at the new rate; S_RestartMusic then re-registers the
    * current track so each backend rebuilds its per-song, rate-derived state
    * (tempo/clock scaling, resampler steps) correctly.  R_InitInterpolation
    * notices timing.sample_rate changed and pushes SET_SYSTEM_AV_INFO so the
    * frontend resampler retargets the new rate. */
   {
      int prev_rate = audio_sample_rate;
      update_audio_samplerate();
      if (!startup && audio_sample_rate != prev_rate)
      {
         S_RestartMusic();
         R_InitInterpolation();
      }
   }
}

void I_SafeExit(int rc);

void retro_run(void)
{
#ifdef ACS_SELFTEST
  /* Fire once the world is actually in play, so a real player actor exists for
   * the conversation trigger to act on (players[0].mo is NULL before that). */
  { static int acs_st_done = 0;
    extern int ZACS_ConversationSelfTest(void);
    if (!acs_st_done && gamestate == GS_LEVEL && players[0].mo)
    { acs_st_done = 1; ZACS_ConversationSelfTest(); }
  }
#endif

   bool updated = false;

   in_retro_run = true;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      update_variables(false);

   /* Apply a deferred aspect-ratio change here, before any rendering
    * this frame, so the framebuffer rebuild never races an in-flight
    * render or the menu draw that requested it. */
   if (aspect_change_pending)
      I_ApplyAspectRatio();

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

   in_retro_run = false;
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

/* True if the WAD at `path` contains a PLAYPAL lump.  PLAYPAL is the
 * master palette every playable Doom WAD needs; its presence in a
 * PWAD-headered file is a strong signal that the file is intended as
 * a standalone game.  chex.wad is the motivating case: it ships with
 * PWAD magic but contains its own complete lump set (palette,
 * colormap, textures, sprites, sounds, maps).  Without this
 * detection the libretro path routes chex.wad as -file, then
 * IdentifyVersion runs and errors out with "IWAD not found" because
 * no doom.wad is present alongside.  Reads only the lump directory
 * header, not lump data. */
static bool wad_contains_playpal(const char *path, const wadinfo_t *header)
{
   bool found = false;
   RFILE *fp;
   filelump_t lump;
   int i;
   int numlumps     = LONG(header->numlumps);
   int infotableofs = LONG(header->infotableofs);

   if (numlumps <= 0 || infotableofs <= 0)
      return false;
   fp = filestream_open(path,
         RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (!fp)
      return false;
   if (filestream_seek(fp, infotableofs, SEEK_SET) != 0)
   {
      filestream_close(fp);
      return false;
   }
   for (i = 0; i < numlumps; i++)
   {
      if (rfread(&lump, sizeof(lump), 1, fp) != 1)
         break;
      /* PLAYPAL is exactly 7 chars; byte 7 of the 8-byte name slot
       * is either NUL or a trailing space-pad depending on the
       * authoring tool, so don't require either. */
      if (!strncmp(lump.name, "PLAYPAL", 7))
      {
         found = true;
         break;
      }
   }
   filestream_close(fp);
   return found;
}

/* PWAD "map kind": which IWAD generation the PWAD's maps target.
 * The libretro front-end hands us a single file via retro_load_game.
 * For a genuine add-on PWAD we emit -file <path> and let the engine's
 * FindIWADFile pick an IWAD by iterating standard_iwads[] in d_main.c.
 * That list probes doom2.wad / doom2f.wad / plutonia.wad / tnt.wad /
 * freedoom2.wad BEFORE doom.wad / doomu.wad / freedoom1.wad / ..., so
 * a directory with both DOOM 1 and DOOM 2 IWADs side-by-side always
 * picks the commercial one.  When the user picks an episode-5 add-on
 * like SIGIL.WAD with a doom2.wad next to it, the engine boots in
 * commercial mode (MAP01 plays at game start, SIGIL's E5Mx maps are
 * unreachable, and SIGIL's missing textures cause render glitches /
 * crashes once any unmapped texture is referenced).
 *
 * Peek at the PWAD's lump directory: ExMy markers (E1..E5 maps) mean
 * "DOOM 1 add-on", MAPxx markers mean "DOOM 2 add-on".  When both
 * are present (rare, but some compatibility shims do it), prefer the
 * DOOM 2 reading; the commercial map set is the more permissive
 * gamemode and standalone E maps in a Doom 2 mod usually exist for
 * UMAPINFO override purposes rather than as the primary content. */
typedef enum
{
   PWAD_MAP_NONE,        /* no map markers (graphics / sound / DEH-only PWAD) */
   PWAD_MAP_DOOM1,       /* ExMy markers found */
   PWAD_MAP_DOOM2        /* MAPxx markers found */
} pwad_map_kind_t;

static pwad_map_kind_t scan_pwad_map_kind(const char *path,
                                          const wadinfo_t *header)
{
   RFILE *fp;
   filelump_t lump;
   int i;
   int numlumps     = LONG(header->numlumps);
   int infotableofs = LONG(header->infotableofs);
   bool saw_exmy    = false;
   bool saw_mapxx   = false;

   if (numlumps <= 0 || infotableofs <= 0)
      return PWAD_MAP_NONE;
   fp = filestream_open(path,
         RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (!fp)
      return PWAD_MAP_NONE;
   if (filestream_seek(fp, infotableofs, SEEK_SET) != 0)
   {
      filestream_close(fp);
      return PWAD_MAP_NONE;
   }
   for (i = 0; i < numlumps; i++)
   {
      const char *n;
      if (rfread(&lump, sizeof(lump), 1, fp) != 1)
         break;
      n = lump.name;
      /* "ExMy": e.g. E1M1..E4M9 plus the SIGIL E5Mx extension.  The
       * 8-byte lump name slot is NUL- or space-padded after the
       * 4 significant characters. */
      if ((n[0] == 'E' || n[0] == 'e') &&
          n[1] >= '1' && n[1] <= '9' &&
          (n[2] == 'M' || n[2] == 'm') &&
          n[3] >= '0' && n[3] <= '9' &&
          (n[4] == '\0' || n[4] == ' '))
         saw_exmy = true;
      /* "MAPxx": MAP01..MAP99 (and a few mods exceeding that). */
      else if ((n[0] == 'M' || n[0] == 'm') &&
               (n[1] == 'A' || n[1] == 'a') &&
               (n[2] == 'P' || n[2] == 'p') &&
               n[3] >= '0' && n[3] <= '9' &&
               n[4] >= '0' && n[4] <= '9' &&
               (n[5] == '\0' || n[5] == ' '))
         saw_mapxx = true;
   }
   filestream_close(fp);
   if (saw_mapxx)
      return PWAD_MAP_DOOM2;
   if (saw_exmy)
      return PWAD_MAP_DOOM1;
   return PWAD_MAP_NONE;
}

/* Probe the wad-dir / system-dir / system-dir/prboom hierarchy for an
 * IWAD that matches the requested map kind.  Returns a heap-allocated
 * absolute path on first hit, NULL otherwise.  Uses the case-
 * insensitive helper so a DOOM.WAD on a Linux ext4 matches the
 * lowercase candidate name.
 *
 * Candidate order within each kind mirrors d_main.c's standard_iwads[]
 * preference: official releases before freedoom fallbacks, common
 * names before rare ones. */
static char *find_iwad_for_kind(pwad_map_kind_t kind)
{
   static const char *const doom1_candidates[] = {
      "doom.wad", "doomu.wad", "freedoom1.wad", "freedoom.wad", "doom1.wad"
   };
   static const char *const doom2_candidates[] = {
      "doom2.wad", "doom2f.wad", "plutonia.wad", "tnt.wad", "freedoom2.wad"
   };
   const char *const *list;
   size_t list_n;
   size_t i;
   char *system_dir = NULL;
   char *prboom_subdir = NULL;
   char *hit;

   if (kind == PWAD_MAP_DOOM1)
   {
      list   = doom1_candidates;
      list_n = sizeof(doom1_candidates)/sizeof(doom1_candidates[0]);
   }
   else if (kind == PWAD_MAP_DOOM2)
   {
      list   = doom2_candidates;
      list_n = sizeof(doom2_candidates)/sizeof(doom2_candidates[0]);
   }
   else
      return NULL;

   environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir);
   if (system_dir && *system_dir)
   {
      prboom_subdir = malloc(strlen(system_dir) + 8);
      if (prboom_subdir)
         sprintf(prboom_subdir, "%s%c%s", system_dir, DIR_SLASH, "prboom");
   }

   for (i = 0; i < list_n; i++)
   {
      /* wad dir first -- the user-visible "load location" */
      if ((hit = FindFileInDir(g_wad_dir, list[i], NULL)))
         goto done;
      /* system_dir/prboom -- the canonical libretro IWAD stash */
      if (prboom_subdir && (hit = FindFileInDir(prboom_subdir, list[i], NULL)))
         goto done;
      /* system_dir directly -- some users drop IWADs here */
      if (system_dir && *system_dir && (hit = FindFileInDir(system_dir, list[i], NULL)))
         goto done;
   }
   hit = NULL;
done:
   free(prboom_subdir);
   return hit;
}

/* Look for a ZDoom base-resource archive (gzdoom.pk3, then zdoom.pk3) in
 * the same places find_iwad_for_kind searches: next to the loaded content,
 * then system_dir/prboom, then system_dir.  ZDoom-targeted pk3s name stock
 * assets (decal graphics, sounds, the TEXTURES base, ...) that live in this
 * archive; pairing it in lets those resolve without the user wiring up an
 * autoload slot.  Returns a heap path (caller frees) or NULL if none found. */
static char *find_base_resource(void)
{
   static const char *const candidates[] = { "gzdoom.pk3", "zdoom.pk3" };
   size_t i;
   char *system_dir = NULL;
   char *prboom_subdir = NULL;
   char *hit;

   environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir);
   if (system_dir && *system_dir)
   {
      prboom_subdir = malloc(strlen(system_dir) + 8);
      if (prboom_subdir)
         sprintf(prboom_subdir, "%s%c%s", system_dir, DIR_SLASH, "prboom");
   }

   for (i = 0; i < sizeof(candidates)/sizeof(candidates[0]); i++)
   {
      if ((hit = FindFileInDir(g_wad_dir, candidates[i], NULL)))
         goto done;
      if (prboom_subdir &&
          (hit = FindFileInDir(prboom_subdir, candidates[i], NULL)))
         goto done;
      if (system_dir && *system_dir &&
          (hit = FindFileInDir(system_dir, candidates[i], NULL)))
         goto done;
   }
   hit = NULL;
done:
   free(prboom_subdir);
   return hit;
}


/* Classification of a single playlist entry.  Used by the m3u
 * loader (issue #196) so each line of the playlist is routed to
 * the correct argv chunk (-iwad / -file / -deh).  Same rules the
 * single-file retro_load_game path uses, just factored out. */
typedef enum
{
   ENTRY_INVALID,
   ENTRY_IWAD,    /* primary IWAD: header == "IWAD", or "PWAD" + PLAYPAL */
   ENTRY_PWAD,    /* genuine add-on PWAD */
   ENTRY_DEH      /* DeHackEd / BEX patch */
} entry_kind_t;

static entry_kind_t classify_entry(const char *path)
{
   const char *ext;
   wadinfo_t header;

   ext = strrchr(path, '.');
   if (ext)
   {
      ext++;
      if (!strcasecmp(ext, "deh") || !strcasecmp(ext, "bex"))
         return ENTRY_DEH;
   }
   header = get_wadinfo(path);
   if (header.identification[0] == 0)
      return ENTRY_INVALID;
   if (!strncmp(header.identification, "IWAD", 4))
      return ENTRY_IWAD;
   if (!strncmp(header.identification, "PWAD", 4))
   {
      if (wad_contains_playpal(path, &header))
         return ENTRY_IWAD;
      return ENTRY_PWAD;
   }
   return ENTRY_INVALID;
}

/* Parse an m3u playlist of WAD / DEH / BEX entries.  One path per
 * line; blank lines and lines beginning with '#' are skipped.  Each
 * entry is resolved relative to the playlist's own directory unless
 * the line is an absolute path (leading '/' on POSIX, or 'X:' drive
 * prefix on Windows).
 *
 * On return, `paths[]` holds strdup'd resolved paths the caller
 * must free, `kinds[]` the corresponding classification, and the
 * function value is the number of entries kept (>= 0) or -1 if the
 * playlist could not be opened.  Unloadable / unrecognised lines
 * are logged and skipped, not failed.
 *
 * Motivating use case (issue #196): users want to stack mods like
 *   SCYTHE.WAD
 *   D4V.WAD
 *   D4V.deh
 * and have them load in the listed order.  The single-WAD libretro
 * entry point has no way to express this; an m3u playlist is the
 * standard libretro mechanism for multi-file content and keeps the
 * load order explicit and editable. */
static int parse_m3u_playlist(const char *m3u_path,
                              char **paths, entry_kind_t *kinds,
                              int max_entries)
{
   char m3u_dir[PATH_MAX + 1];
   char line[PATH_MAX + 16];
   /* `m3u_dir` (up to PATH_MAX) + separator + `line` text (up to
    * PATH_MAX + 15) gives a worst-case concatenation around
    * 2 * PATH_MAX.  Size with that headroom so the compiler can
    * see the snprintf below is non-truncating. */
   char resolved[2 * (PATH_MAX + 1) + 32];
   int n = 0;
   RFILE *fp;

   extract_directory(m3u_dir, m3u_path, sizeof(m3u_dir));
   fp = filestream_open(m3u_path,
         RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);
   if (!fp)
      return -1;

   while (rfgets(line, sizeof(line), fp) && n < max_entries)
   {
      char *p = line;
      size_t L;
      entry_kind_t k;

      /* Trim leading whitespace. */
      while (*p == ' ' || *p == '\t') p++;
      /* Trim trailing whitespace and line terminators. */
      L = strlen(p);
      while (L > 0 &&
             (p[L-1] == '\n' || p[L-1] == '\r' ||
              p[L-1] == ' '  || p[L-1] == '\t'))
         p[--L] = 0;
      if (L == 0 || *p == '#') continue;  /* blank or comment */

      /* Resolve relative to m3u dir.  Absolute paths pass through. */
      if (p[0] == '/' || p[0] == '\\' ||
          (p[0] && p[1] == ':'))  /* POSIX abs or DOS-style drive */
         snprintf(resolved, sizeof(resolved), "%s", p);
      else
         snprintf(resolved, sizeof(resolved), "%s%c%s",
                  m3u_dir, DIR_SLASH, p);

      /* Canonicalise letter case for the final component.  Many real
       * IWAD distributions ship DOOM.WAD / DOOM2.WAD uppercase, but
       * a user's m3u line might say `doom2.wad` -- on case-sensitive
       * filesystems filestream_open won't find the uppercase file
       * from the lowercase path.  See issues #186 / #188. */
      {
         char *fixed = canonicalize_path_casefold(resolved);
         if (fixed)
         {
            strncpy(resolved, fixed, sizeof(resolved) - 1);
            resolved[sizeof(resolved) - 1] = '\0';
            free(fixed);
         }
      }

      k = classify_entry(resolved);
      if (k == ENTRY_INVALID)
      {
         if (log_cb)
            log_cb(RETRO_LOG_WARN,
                   "m3u: skipping unrecognised / unreadable entry '%s'\n",
                   resolved);
         continue;
      }
      paths[n] = strdup(resolved);
      kinds[n] = k;
      n++;
   }
   filestream_close(fp);
   return n;
}

bool retro_load_game(const struct retro_game_info *info)
{
   unsigned i;
   int argc = 0;
   char **argv = load_argv;

   /* Free any leftover heap pointers from a previous session.
    * Belt-and-braces: retro_unload_game also frees, but if a
    * frontend re-calls retro_load_game without an intervening
    * unload (unusual but legal), we'd otherwise leak. */
   free_load_argv();

   if (environ_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble))
   {
      if (log_cb)
         log_cb(RETRO_LOG_INFO, "Rumble environment supported.\n");
   }
   else if (log_cb)
      log_cb(RETRO_LOG_INFO, "Rumble environment not supported.\n");

   update_variables(true);

   argv[argc++] = strdup("prboom");

   /* Compatibility level: if the user picked a specific complevel in the
    * core options, pass it as -complevel so it overrides the config and the
    * engine's automatic choice.  "-1" means "Default (Auto)" -> don't force.
    * This is how MBF21 WADs (complevel 21) get the MBF21 feature gate. */
   {
      struct retro_variable cvar;
      cvar.key = "prboom-complevel";
      cvar.value = NULL;
      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &cvar) && cvar.value &&
          strcmp(cvar.value, "-1") != 0)
      {
         argv[argc++] = strdup("-complevel");
         argv[argc++] = strdup(cvar.value);
      }
   }

   /* Hexen player class, from the core option.  d_main only consults -class
    * when hexen is set, so passing it for Doom/Heretic is harmless. */
   {
      struct retro_variable cvar;
      cvar.key = "prboom-class";
      cvar.value = NULL;
      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &cvar) && cvar.value)
      {
         argv[argc++] = strdup("-class");
         argv[argc++] = strdup(cvar.value);
      }
   }

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

#ifdef PSX
       if(strcmp(extension,"lmp") == 0)
#else
      if(strcasecmp(extension,"lmp") == 0)
#endif
      {
        // Play as a demo file lump
        argv[argc++] = strdup("-playdemo");
        argv[argc++] = strdup(info->path);
      }
      else if (strcasecmp(extension, "m3u") == 0)
      {
         /* M3U playlist: the user listed WAD / DEH / BEX files in
          * load order.  Parse them, find the IWAD, and stage the
          * rest as -file / -deh (issue #196).  The g_wad_dir was
          * already set to the m3u's directory so I_FindFile picks
          * up sibling lumps / configs from the same folder. */
         enum { M3U_MAX_ENTRIES = 32 };
         char *paths[M3U_MAX_ENTRIES];
         entry_kind_t kinds[M3U_MAX_ENTRIES];
         int n = parse_m3u_playlist(info->path, paths, kinds, M3U_MAX_ENTRIES);
         int j, iwad_idx = -1, file_emitted = 0, deh_emitted = 0;
         if (n < 0)
         {
            I_Error("retro_load_game: couldn't open m3u playlist '%s'",
                    info->path);
            goto failed;
         }
         for (j = 0; j < n; j++)
            if (kinds[j] == ENTRY_IWAD) { iwad_idx = j; break; }
         if (iwad_idx < 0)
         {
            for (j = 0; j < n; j++) free(paths[j]);
            I_Error("retro_load_game: m3u '%s' has no IWAD entry",
                    info->path);
            goto failed;
         }
         /* -iwad goes first.  Pass the full resolved path so the
          * engine doesn't fall through to standard_iwads[]; the
          * basename alone would be ambiguous when the playlist
          * points at a custom location. */
         argv[argc++] = strdup("-iwad");
         argv[argc++] = strdup(paths[iwad_idx]);
         /* Then -file with every PWAD (and any extra IWAD-shape
          * entries beyond the first) in playlist order. */
         for (j = 0; j < n; j++)
         {
            if (j == iwad_idx) continue;
            if (kinds[j] == ENTRY_PWAD || kinds[j] == ENTRY_IWAD)
            {
               if (!file_emitted)
               {
                  argv[argc++] = strdup("-file");
                  file_emitted = 1;
               }
               argv[argc++] = strdup(paths[j]);
            }
         }
         /* Then -deh with every DEH/BEX patch.  D_DoomMainSetup's
          * dehs[] collector picks them up after wad loading. */
         for (j = 0; j < n; j++)
         {
            if (kinds[j] == ENTRY_DEH)
            {
               if (!deh_emitted)
               {
                  argv[argc++] = strdup("-deh");
                  deh_emitted = 1;
               }
               argv[argc++] = strdup(paths[j]);
            }
         }
         /* baseconfig lookup still runs against the m3u dir, mirroring
          * the single-WAD path: a per-playlist <name>.prboom.cfg
          * takes priority, otherwise a generic prboom.cfg. */
         if((baseconfig = FindFileInDir(g_wad_dir, name_without_ext, ".prboom.cfg"))
               || (baseconfig = I_FindFile("prboom.cfg", NULL)))
         {
            argv[argc++] = strdup("-baseconfig");
            argv[argc++] = baseconfig;
         }
         for (j = 0; j < n; j++) free(paths[j]);
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
            /* Decide whether this PWAD is a genuine add-on (needs an
             * external IWAD) or a standalone game shipped with PWAD magic
             * but a complete lump set (chex.wad is the canonical example).
             *
             * A PLAYPAL lump alone is NOT sufficient to call something a
             * standalone IWAD: many add-on PWADs (notably DSDHacked total
             * conversions like Dwelling / vesper) bundle a custom PLAYPAL
             * while still depending on a real IWAD for the status bar,
             * textures, flats and sprites.  If we mis-route those to
             * -iwad, the engine never loads doom2.wad and dies later on a
             * missing IWAD lump (STKEYS*, STF*, MFLR8_4, ...).
             *
             * So: if the PWAD carries map markers AND we can actually find
             * an IWAD of the matching generation nearby, treat it as an
             * add-on and steer it onto that IWAD -- this takes priority
             * over the PLAYPAL heuristic.  Only when no suitable IWAD can
             * be found do we fall back to treating a PLAYPAL-bearing PWAD
             * as a standalone IWAD (preserving the chex.wad-with-no-doom
             * case). */
            pwad_map_kind_t kind = scan_pwad_map_kind(info->path, &header);
            char *iwad_match = (kind != PWAD_MAP_NONE)
                               ? find_iwad_for_kind(kind) : NULL;

            if (log_cb)
               log_cb(RETRO_LOG_INFO,
                      "retro_load_game: PWAD '%s' map kind=%s, "
                      "matching IWAD=%s\n", g_basename,
                      kind == PWAD_MAP_DOOM1 ? "ExMy(DOOM1)" :
                      kind == PWAD_MAP_DOOM2 ? "MAPxx(DOOM2)" : "none",
                      iwad_match ? iwad_match : "(none found)");

            /* If the matching-generation probe missed, try the other
             * generation too: a MAPxx probe can miss when only a Doom 1
             * IWAD is present and vice versa, and a kind==none scan (some
             * WADs hide their maps behind UMAPINFO or non-standard markers)
             * still needs a real IWAD if one exists.  Only fall back to the
             * PLAYPAL standalone-IWAD path when NO standard IWAD is findable
             * anywhere -- that is the genuine chex.wad case. */
            if (!iwad_match)
            {
               iwad_match = find_iwad_for_kind(PWAD_MAP_DOOM2);
               if (!iwad_match)
                  iwad_match = find_iwad_for_kind(PWAD_MAP_DOOM1);
            }

            if (iwad_match)
            {
               /* Add-on PWAD with a real IWAD available: pair them. */
               if (log_cb)
                  log_cb(RETRO_LOG_INFO,
                         "retro_load_game: steering PWAD '%s' toward "
                         "IWAD '%s'\n", g_basename, iwad_match);
               argv[argc++] = strdup("-iwad");
               argv[argc++] = iwad_match;  /* already heap from FindFileInDir */
               argv[argc++] = strdup("-file");
               argv[argc++] = strdup(info->path);
            }
            else if (wad_contains_playpal(info->path, &header))
            {
               /* No standard IWAD found anywhere, but the PWAD carries its
                * own palette: a genuine standalone game shipped with PWAD
                * magic (chex.wad and friends).  CheckIWAD accepts PWAD
                * magic, so -iwad routing works. */
               if (log_cb)
                  log_cb(RETRO_LOG_INFO,
                         "retro_load_game: no external IWAD found for '%s'; "
                         "it has a PLAYPAL, treating it as a standalone "
                         "IWAD\n", g_basename);
               argv[argc++] = strdup("-iwad");
               argv[argc++] = strdup(g_basename);
            }
            else
            {
               /* No IWAD found and no own palette.  Let the engine's
                * FindIWADFile auto-detect try; warn for the Doom 1 case. */
               if (kind == PWAD_MAP_DOOM1 && log_cb)
                  log_cb(RETRO_LOG_WARN,
                         "retro_load_game: PWAD '%s' has DOOM 1 (ExMy) "
                         "maps but no doom.wad / doomu.wad / freedoom1.wad / "
                         "freedoom.wad / doom1.wad found near the PWAD "
                         "or in the system directory.  Place a DOOM 1 "
                         "IWAD next to '%s' or use an m3u playlist to "
                         "name the IWAD explicitly.\n",
                         g_basename, g_basename);
               argv[argc++] = strdup("-file");
               argv[argc++] = strdup(info->path);
            }
         }
         else if(header.identification[0] == 'P' &&
                 header.identification[1] == 'K' &&
                 header.identification[2] == 0x03 &&
                 header.identification[3] == 0x04)
         {
            /* PK3/ZIP archive.  These are add-ons (no PLAYPAL routing
             * heuristic applies: standalone archives are ipk3 games this
             * core does not support yet), so pair them with whatever IWAD
             * is findable.  ZDoom-targeted archives overwhelmingly assume
             * doom2.wad, so prefer the DOOM2 generation. */
            char *iwad_match = find_iwad_for_kind(PWAD_MAP_DOOM2);
            if (!iwad_match)
               iwad_match = find_iwad_for_kind(PWAD_MAP_DOOM1);
            if (iwad_match)
            {
               if (log_cb)
                  log_cb(RETRO_LOG_INFO,
                         "retro_load_game: steering archive '%s' toward "
                         "IWAD '%s'\n", g_basename, iwad_match);
               argv[argc++] = strdup("-iwad");
               argv[argc++] = iwad_match;  /* heap from FindFileInDir */
               argv[argc++] = strdup("-file");
               /* A ZDoom base resource (gzdoom.pk3 / zdoom.pk3), if present
                * nearby, loads BEFORE the mod so its stock assets resolve;
                * the mod follows and overrides any it redefines (Doom load
                * order: later wins). */
               {
                  char *base = find_base_resource();
                  if (base)
                  {
                     if (log_cb)
                        log_cb(RETRO_LOG_INFO,
                               "retro_load_game: pairing base resource "
                               "'%s' with archive '%s'\n", base, g_basename);
                     argv[argc++] = base;  /* heap from FindFileInDir */
                  }
               }
               argv[argc++] = strdup(info->path);
            }
            else
            {
               if (log_cb)
                  log_cb(RETRO_LOG_WARN,
                         "retro_load_game: archive '%s' loaded with no "
                         "IWAD found nearby; the engine's auto-detect "
                         "will try, but a doom2.wad next to the archive "
                         "is the expected setup.\n", g_basename);
               argv[argc++] = strdup("-file");
               {
                  char *base = find_base_resource();
                  if (base)
                  {
                     if (log_cb)
                        log_cb(RETRO_LOG_INFO,
                               "retro_load_game: pairing base resource "
                               "'%s' with archive '%s'\n", base, g_basename);
                     argv[argc++] = base;  /* heap from FindFileInDir */
                  }
               }
               argv[argc++] = strdup(info->path);
            }
         }
         else
         {
            I_Error("retro_load_game: invalid WAD header '%.*s'", 4, header.identification);
            goto failed;
         }

         /* Check for DEH or BEX files */
         if((deh = FindFileInDir(g_wad_dir, name_without_ext, ".deh"))
               || (deh = FindFileInDir(g_wad_dir, name_without_ext, ".bex")))
         {
            /* strdup so every argv[] slot is uniformly heap-owned;
             * see retro_unload_game cleanup. */
            argv[argc++] = strdup("-deh");
            argv[argc++] = deh;  /* already heap from FindFileInDir */
         }

         if((baseconfig = FindFileInDir(g_wad_dir, name_without_ext, ".prboom.cfg"))
               || (baseconfig = I_FindFile("prboom.cfg", NULL)))
         {
            argv[argc++] = strdup("-baseconfig");
            argv[argc++] = baseconfig;  /* already heap */
         }
      }

      /* Get save directory */
      if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &base_save_dir) && base_save_dir)
      {
         if (base_save_dir && strlen(base_save_dir) > 0)
         {
            // > Build save path
            snprintf(g_save_dir, sizeof(g_save_dir), "%s%c%s", base_save_dir, DIR_SLASH, name_without_ext);
            use_external_savedir = true;

            // > Create save directory, if required
            if (!path_is_directory(g_save_dir))
               use_external_savedir = path_mkdir(g_save_dir);
         }
      }

       // > Use WAD directory fallback...
       if (!use_external_savedir)
          strlcpy(g_save_dir, g_wad_dir, sizeof(g_save_dir));
   }

#if DEBUG
   argv[argc++] = strdup("-dehout");
   argv[argc++] = strdup("-");
#endif

   myargc = argc;
   myargv = (const char **) argv;

   /* cphipps - call to video specific startup code.
    * Allocate the framebuffer at the worst-case widescreen size
    * (MAX_SCREENWIDTH*MAX_SCREENHEIGHT) so the Aspect Ratio selector
    * can widen SCREENWIDTH at runtime without ever reallocating. */
   screen_buf = (unsigned char*)malloc(SURFACE_PIXEL_DEPTH * MAX_SCREENWIDTH * MAX_SCREENHEIGHT);
   if (!screen_buf)
      goto failed;

   if (!D_DoomMainSetup())
      goto failed;

   /* Game type (Doom vs Heretic vs Hexen) is now known. The controller
    * info was registered with Doom device names in retro_set_environment
    * (before any game was loaded); for the Raven games, re-register with
    * game-appropriate names so the frontend's device list reflects the
    * Heretic / Hexen control schemes. */
   {
      extern dbool heretic, hexen;
      if (hexen)
      {
         static const struct retro_controller_description hexen_port[] = {
            { "Hexen Gamepad Modern", RETROPAD_MODERN },
            { "Hexen Gamepad Classic", RETROPAD_CLASSIC },
            { "Hexen RetroKeyboard/Mouse", RETRO_DEVICE_KEYBOARD },
            { 0 },
         };
         static const struct retro_controller_info hexen_ports[] = {
            { hexen_port, 3 },
            { NULL, 0 },
         };
         environ_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)hexen_ports);
         /* Refresh the active descriptors for the current device. */
         retro_set_controller_port_device(0, doom_devices[0]);
      }
      else if (heretic)
      {
         static const struct retro_controller_description heretic_port[] = {
            { "Heretic Gamepad Modern", RETROPAD_MODERN },
            { "Heretic Gamepad Classic", RETROPAD_CLASSIC },
            { "Heretic RetroKeyboard/Mouse", RETRO_DEVICE_KEYBOARD },
            { 0 },
         };
         static const struct retro_controller_info heretic_ports[] = {
            { heretic_port, 3 },
            { NULL, 0 },
         };
         environ_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)heretic_ports);
         /* Refresh the active descriptors for the current device. */
         retro_set_controller_port_device(0, doom_devices[0]);
      }
   }

   /* Config is now loaded, so render_aspect is valid.  Apply the
    * saved aspect ratio directly here: this runs before the main
    * loop starts, so there is no in-flight render to race -- widen
    * the buffer (no-op for 4:3) and rebuild the video mode before
    * the first frame is presented. */
   I_ApplyAspectRatio();

   // Run few cycles to finish init.
   for (i = 0; i < 3; i++)
     D_DoomLoop();

   cheats_enabled      = true;
   cheats_pending      = false;
   cheats_pending_list = NULL;

   /* Negotiate float audio output once, now that the game is loaded and
    * the audio path is up. If the frontend supports it we commit to float
    * for this game's lifetime; otherwise the int16 path is used unchanged.
    * (Contract: do this once per loaded game, never mix formats.) */
   use_float_output     = 0;
   audio_batch_cb_float = NULL;
   {
      struct retro_audio_sample_float_callback fcb;
      fcb.batch = NULL;
      if (environ_cb(RETRO_ENVIRONMENT_GET_AUDIO_SAMPLE_BATCH_FLOAT, &fcb)
            && fcb.batch)
      {
         audio_batch_cb_float = fcb.batch;
         use_float_output     = 1;
      }
   }

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
   if (screen_buf)
   {
      free(screen_buf);
      screen_buf = NULL;
   }
   /* Roll back any partial init D_DoomMainSetup did before
    * failing.  Critically: if IdentifyVersion ran (D_AddFile
    * appended an entry to wadfiles[]) but a later step failed,
    * the next retro_load_game would otherwise see the stale
    * wadfile entry, re-open it on top of the new IWAD, and
    * load both lump tables concatenated -- the same bug
    * W_ReleaseAllWads in D_DoomDeinit was added to prevent for
    * the success path.
    *
    * D_DoomDeinit's chain is null-safe in every step: P_Deinit's
    * Z_Free(NULL) is a no-op, the R_, V_, and S_ shutdowns guard
    * their pointers, M_SaveDefaults guards a NULL defaultfile,
    * and W_ReleaseAllWads handles wadfiles==NULL /
    * numwadfiles==0 gracefully.  Safe to call even if
    * D_DoomMainSetup failed before reaching M_LoadDefaults. */
   D_DoomDeinit();
   /* Release the strdup'd argv slots populated above before we
    * return false.  RetroArch does not call retro_unload_game
    * after retro_load_game returns false (nothing succeeded to
    * unload), so without this each failed load leaks ~6-12
    * strdups -- "prboom" plus -iwad/-file/-playdemo plus their
    * paths plus optional -deh/-baseconfig and their paths. */
   free_load_argv();
   myargc = 0;
   myargv = NULL;
   I_SafeExit(-1);
   return false;
}

void retro_unload_game(void)
{
   D_DoomDeinit();

   /* Drop the float-output negotiation; the frontend's batch pointer is
    * only valid until here, and the next game re-negotiates. */
   use_float_output     = 0;
   audio_batch_cb_float = NULL;

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

   if (screen_buf)
      free(screen_buf);
   screen_buf = NULL;

   /* Release the strdup'd argv slots from retro_load_game.
    * Without this, every content load leaks ~8-12 strdups
    * (prboom, -iwad/-file, the path, optionally -deh/-baseconfig
    * with their paths). */
   free_load_argv();

   myargc     = 0;
   myargv     = NULL;
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

/* Fixed-size region for music backend state.  Sized to cover the
 * worst case the current OPL backend can produce (~16 byte header +
 * 4 bytes per MIDI track, capped at 256 tracks); the few hundred
 * extra bytes are negligible even for runahead, which saves state
 * every frame and so is sensitive to size.  music_state_size records
 * how many bytes are actually valid -- 0 means "no music state". */
#define MUSIC_STATE_RESERVED 512

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
  uint32_t music_state_size;
  uint8_t  music_state[MUSIC_STATE_RESERVED];
};

size_t retro_serialize_size(void)
{
  /* Hexen savegames are much larger than Doom/Heretic ones: the maps carry
   * far more mobjs plus the ACS/polyobj/sound-sequence world state, and the
   * save now also embeds the hub map archives -- a retail map at skill 4
   * needs ~390KB on its own and a fully explored hub several times that. */
  size_t need = 0x30000;

  /* The old fixed 192KB Doom/Heretic budget is too small for large
   * megawad maps (nova4.wad MAP16 carries ~700 thinkers and its save
   * exceeds the cap), which made retro_serialize fail outright; under
   * run-ahead or rewind the frontend then restores a stale state every
   * frame and the world appears frozen -- monsters stuck on one frame,
   * death animations never advancing.  Size the budget from the live
   * world instead: mobj thinkers dominate a savegame, sector and line
   * deltas scale with the map, and the thinker term is doubled so
   * mid-level spawning (lost souls, arch-vile resurrections, the icon
   * of sin) has headroom.  The old constant stays as the floor so
   * small maps report a stable size. */
  if (!hexen && thinkercap.next != NULL)
  {
    size_t      nthinkers = 0;
    thinker_t  *th;
    for (th = thinkercap.next; th != &thinkercap; th = th->next)
      nthinkers++;
    {
      size_t est = nthinkers * (sizeof(mobj_t) + 64) * 2
                 + (size_t)numsectors * 64
                 + (size_t)numlines * 64
                 + 0x10000;
      if (est > need)
        need = est;
    }
  }

  return sizeof(struct extra_serialize) + (hexen ? 0x400000 : need);
}

bool retro_serialize(void *data_, size_t size)
{
  unsigned i;
  struct extra_serialize *extra = data_;

  if (gamestate == GS_LEVEL)
  {
     int ret = G_DoSaveGameToBuffer((char *) data_ + sizeof(*extra),
           size - sizeof(*extra));
     if (!ret)
        return false;

     if (viewplayer && viewplayer->mo)
     {
        extra->prevx = viewplayer->mo->PrevX;
        extra->prevy = viewplayer->mo->PrevY;
        extra->prevz = viewplayer->prev_viewz;
        extra->prevangle = viewplayer->prev_viewangle;
        extra->prevpitch = viewplayer->prev_viewpitch;
     }
  }

  extra->gametic     = gametic;
  extra->gameticfrac = tic_vars.frac;
  extra->gameaction  = gameaction;
  extra->turnheld    = turnheld;
  extra->extra_size  = sizeof(*extra);
  extra->autorun     = autorun;
  extra->gamestate   = gamestate;
  extra->FinaleStage = FinaleStage;
  extra->FinaleCount = FinaleCount;
  extra->itemOn      = itemOn;
  extra->whichSkull  = whichSkull;
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

  /* Record music backend's playback position so a later load resumes
   * the track from here instead of letting it continue past the load
   * point.  Backends that don't implement this return 0; we record
   * 0 and the load path becomes a no-op. */
  {
    size_t n = I_MusicSerialize(extra->music_state, sizeof extra->music_state);
    extra->music_state_size = (uint32_t)n;
  }
  return true;
}

bool retro_unserialize(const void *data_, size_t size)
{
  const struct extra_serialize *extra = data_;
  int gameless = 0;
  if (extra->extra_size == sizeof(*extra))
    gameless = (extra->gamestate != GS_LEVEL);
  if (!gameless)
  {
     int ret = G_DoLoadGameFromBuffer((char *) data_ + extra->extra_size,
           size - extra->extra_size);
     if (!ret)
        return false;

     if (viewplayer && viewplayer->mo)
     {
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

     /* Restore music backend's playback position.  Soft-fail: if the
      * backend can't restore (different song loaded, version mismatch,
      * or doesn't implement state restore), we just leave music
      * playing as-is -- the pre-hook behaviour. */
     if (extra->music_state_size > 0 &&
         extra->music_state_size <= sizeof extra->music_state)
        (void)I_MusicUnserialize(extra->music_state, extra->music_state_size);
  }

  return true;
}

void *retro_get_memory_data(unsigned id) { return NULL; }
size_t retro_get_memory_size(unsigned id) { return 0; }
void retro_cheat_reset(void) { }

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
   if (tic_vars.frac < FRACUNIT)
      return;
   // Increase range on menu
   analog_range = (menuactive)? ANALOG_RANGE*8 : ANALOG_RANGE;

   lsx = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
   lsy = input_state_cb(0, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);

   // Get movement 'amplitude' on each axis
   // > x-axis
   analog_l_amplitude[0] = 0;
   analog_l_amplitude[1] = 0;

   // Add '1' to deal with float->int rounding accuracy loss...
   // (Similarly, subtract '1' when lsx is negative...)
   if (lsx > analog_deadzone)
      analog_l_amplitude[0] = 1 + pwm_period * (lsx - analog_deadzone) / (analog_range - analog_deadzone);

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
         event.type  = ev_keydown;
         event.data1 = *((menuactive)? left_analog_lut[i].menukey : left_analog_lut[i].gamekey);
      }

      if(!new_input_analog_l[i] && old_input_analog_l[i])
      {
         event.type  = ev_keyup;
         event.data1 = *((menuactive)? left_analog_lut[i].menukey : left_analog_lut[i].gamekey);;
      }

      if(      event.type == ev_keydown 
            || event.type == ev_keyup)
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
		extern dbool raven;
		extern dbool movement_mouselook;
		extern int gamepad_lookdelta;

		if (rsy > analog_deadzone)
			rsy = rsy - analog_deadzone;
		if (rsy < -analog_deadzone)
			rsy = rsy + analog_deadzone;

		if (raven && !movement_mouselook)
		{
			/* Heretic keyboard-look path: stage a per-tic look step from
			 * the stick deflection.  Stick up (rsy < 0) looks up, so negate
			 * to match lookdir's +up convention.  Magnitude is 1 inside the
			 * deadzone-adjusted range and 2 near full deflection, mirroring
			 * the vanilla slow/fast look speeds. */
			int mag = (abs(rsy) > (ANALOG_RANGE - analog_deadzone) / 2) ? 2 : 1;
			gamepad_lookdelta = (rsy < 0) ? mag : -mag;
		}
		else
		{
			event_mouse.type = ev_mouse;
			event_mouse.data3 = ANALOG_MOUSE_SPEED * rsy / (ANALOG_RANGE - analog_deadzone)
                         * analog_turn_speed * TICRATE / (float)tic_vars.fps;
		}
	}

	if(event_mouse.type == ev_mouse)
		D_PostEvent(&event_mouse);
}

static void
process_input(void)
{
   int port;
   unsigned i;
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
         {
            extern dbool heretic, hexen;
            gamepad_layout_t *gp = hexen   ? &gp_hexen_classic :
                                   heretic ? &gp_heretic_classic : &gp_classic;
            process_gamepad_buttons(ret, gp->num_buttons, gp->action_lut);
         }
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
         {
            extern dbool heretic, hexen;
            gamepad_layout_t *gp = hexen   ? &gp_hexen_modern :
                                   heretic ? &gp_heretic_modern : &gp_modern;
            process_gamepad_buttons(ret, gp->num_buttons, gp->action_lut);
         }
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
               {
                  D_PostEvent(&event);

                  /* WASD movement: in addition to posting the primary
                   * letter event (which keeps 'w' / 'a' / 's' / 'd'
                   * usable for cheat-string entry and other text-input
                   * paths), emit a second event for these four keys
                   * that targets the configured movement bindings.
                   *
                   *   W -> key_up           (move forward, like Up)
                   *   S -> key_down         (move back,    like Down)
                   *   A -> key_strafeleft   (strafe left,  like ',')
                   *   D -> key_straferight  (strafe right, like '.')
                   *
                   * Targeting key_up / key_down / key_strafeleft /
                   * key_straferight rather than the hard-coded
                   * KEYD_UPARROW etc. means rebound movement keys
                   * continue to receive the WASD events.  Arrow-key
                   * defaults stay wired up through their own
                   * keyboard_lut entries, so both schemes work
                   * simultaneously without one suppressing the
                   * other. */
                  {
                     int sec = 0;
                     switch (keyboard_lut[i][0])
                     {
                        case RETROK_w: sec = key_up;          break;
                        case RETROK_s: sec = key_down;        break;
                        case RETROK_a: sec = key_strafeleft;  break;
                        case RETROK_d: sec = key_straferight; break;
                        default: break;
                     }
                     if (sec)
                     {
                        event_t e2;
                        e2.type  = event.type;
                        e2.data1 = sec;
                        e2.data2 = 0;
                        e2.data3 = 0;
                        D_PostEvent(&e2);
                     }
                  }
               }

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

      if (mx || my)
      {
         event_mouse.data2 = mx * 4;
         event_mouse.data3 = my * 4;
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

void I_StartTic(void)
{
   if (!input_poll_cb)
      return;
   input_poll_cb();
   process_input();
}

/* Map render_aspect (0=4:3 1=16:9 2=16:10 3=32:9) to a buffer width
 * for the current 4:3 base width.  Width scales as ratio/(4:3) and is
 * rounded DOWN to a multiple of 4, then clamped to MAX_SCREENWIDTH.
 * The multiple-of-4 requirement is hard: the software column drawer
 * batches four columns and quad-flushes them (R_FlushQuad16 writes
 * dest[0..3] per row), so a viewwidth that isn't a multiple of 4
 * makes the final batch write past the view edge and corrupts memory.
 * Every stock 4:3 resolution width is already a multiple of 4, so the
 * widened width must preserve that.  At 4:3 this returns
 * base_width_43 unchanged. */
static int I_AspectWidth(void)
{
   /* ratio = num/den, expressed against a 4:3 reference. */
   static const int num[5] = { 4, 16, 16, 32, 64 };
   static const int den[5] = { 3,  9, 10,  9, 27 };
   int idx = render_aspect;
   int w;

   if (idx < 0 || idx > 4)
      idx = 0;

   /* base_width_43 corresponds to 4:3, i.e. (height*4/3).  Scale to
    * the target ratio: w = base_width_43 * (num/den) / (4/3). */
   w = (int)(((int64_t)base_width_43 * num[idx] * 3) / (den[idx] * 4));

   w &= ~3; /* MUST be a multiple of 4 for the quad-column drawer */

   if (w < base_width_43)
      w = base_width_43;
   if (w > MAX_SCREENWIDTH)
      w = MAX_SCREENWIDTH;

   return w;
}

static void I_UpdateVideoMode(void)
{
   R_FilterInit();
   V_DestroyTrueColorPalette();
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

   if (direct_fb_data)
   {
      /* Direct-render: the renderer wrote pixels straight into
       * the frontend's buffer in place; just hand it back.  Per
       * libretro.h the pointer must match exactly what
       * GET_CURRENT_SOFTWARE_FRAMEBUFFER returned, hence the
       * stored pitch / pointer rather than recomputing. */
      video_cb(direct_fb_data, SCREENWIDTH, SCREENHEIGHT,
               direct_fb_pitch);
      /* Issue #183: snapshot the just-presented frame into the
       * persistent screen_buf.  D_Display's wipe_StartScreen runs
       * BEFORE the next I_StartDisplay rebinds screens[0] to a
       * fresh frontend FB; at that point screens[0].data is
       * screen_buf, so capturing from it gives the wipe the
       * correct previous-frame source.  Without this copy, under
       * direct-render screen_buf is never written -- the renderer
       * is bypassing it on every frame -- and wipe_StartScreen
       * (whether called here or after I_StartDisplay) sees
       * uninitialised buffer content.
       *
       * Cost: one read + one write of SCREENPITCH*SCREENHEIGHT
       * bytes per frame.  ~128 KB at 320x200 RGB565.  At 35 Hz
       * that's ~4.5 MB/s of cache-friendly streaming memcpy;
       * trivial on any platform that can run a software Doom
       * renderer at all. */
      memcpy(screen_buf, direct_fb_data,
             SCREENPITCH * SCREENHEIGHT);
      direct_fb_data  = NULL;
      direct_fb_pitch = 0;
      /* Restore screens[0] / drawvars to the heap fallback so any
       * code path that touches screens[0] outside retro_run sees a
       * stable buffer (the libretro contract is that the SW FB
       * pointer is invalid after retro_run returns). */
      screens[0].data        = (unsigned char *)screen_buf;
      drawvars.short_topleft = (unsigned short *)screen_buf;
      drawvars.int_topleft   = (unsigned int   *)screen_buf;
      return;
   }

   /* Fallback path: frontend doesn't support the SW FB API, or
    * returned a non-RGB565 buffer or a mismatched pitch this
    * frame.  Hand video_cb our heap buffer; the frontend will
    * copy/convert internally. */
   video_cb(screen_buf, SCREENWIDTH, SCREENHEIGHT, SCREENPITCH);
}

void I_SetPalette(int pal) { }

void I_InitGraphics(void)
{
   static int firsttime=1;

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

   /* The BG offscreen screen caches the status-bar background. The Doom bar
    * is ST_HEIGHT (32) rows; the Heretic bar is taller (42 rows, y 158..199),
    * so size BG for the larger of the two scaled heights. */
   screens[4].height = (ST_SCALED_HEIGHT+1);
   {
      int heretic_bar_scaled = (42 * SCREENHEIGHT) / 200 + 1;
      if (heretic_bar_scaled > screens[4].height)
         screens[4].height = heretic_bar_scaled;
   }
}

/* Aspect-ratio changes must NOT rebuild the framebuffer from inside
 * the menu callback: that path runs in the middle of D_DoomLoop, so
 * freeing/reallocating screens[] there leaves the renderer's cached
 * topleft pointers dangling and corrupts the very next frame (the
 * BSP traversal reads trampled seg data and segfaults).  Instead the
 * menu just flags the change; retro_run applies it at a safe point
 * before D_DoomLoop, when no render is in flight. */

void I_SetAspectRatio(void)
{
   aspect_change_pending = true;
}

/* Perform the deferred aspect-ratio change.  Called only from a safe
 * point in retro_run (no active render, no live cached pointers).
 * screen_buf is allocated once at MAX_SCREENWIDTH*MAX_SCREENHEIGHT so
 * widening never reallocates it.  We update SCREENWIDTH/SCREENPITCH,
 * rebuild the video mode, request a view-size recalc (the renderer
 * derives the widened FOV from the new dimensions) and push the new
 * geometry with SET_GEOMETRY -- the soft variant that is guaranteed
 * not to reinitialise the frontend's video driver (max_width/height
 * were already set to MAX_SCREENWIDTH/HEIGHT at load). */
static void I_ApplyAspectRatio(void)
{
   extern int screenblocks;
   int new_width;

   aspect_change_pending = false;

   new_width = I_AspectWidth();
   if (new_width == SCREENWIDTH)
      return;

   SCREENWIDTH = new_width;
   SCREENPITCH = (SCREENWIDTH * SURFACE_PIXEL_DEPTH);

   I_UpdateVideoMode();
   R_SetViewSize(screenblocks);

   /* I_UpdateVideoMode -> V_DestroyTrueColorPalette frees the 16-bit
    * palette and leaves V_Palette16 NULL.  At startup the subsequent
    * V_SetPalette rebuilds it, but on a runtime aspect change nothing
    * else does, so the next R_RenderPlayerView dereferences a NULL
    * V_Palette16 in the column/span drawers.  Rebuild it here from the
    * current palette index. */
   if (W_CheckNumForName("PLAYPAL") >= 0)
      V_UpdateTrueColorPalette();

   /* SET_GEOMETRY may only be called from within retro_run().  At
    * load (before the main loop) we skip it: retro_get_system_av_info
    * already reports the correct aspect for the initial geometry. */
   if (environ_cb && in_retro_run)
   {
      struct retro_game_geometry geom;
      geom.base_width   = SCREENWIDTH;
      geom.base_height  = SCREENHEIGHT;
      geom.max_width    = MAX_SCREENWIDTH;
      geom.max_height   = MAX_SCREENHEIGHT;
      switch (render_aspect)
      {
         case 1:  geom.aspect_ratio = 16.0f / 9.0f;  break;
         case 2:  geom.aspect_ratio = 16.0f / 10.0f; break;
         case 3:  geom.aspect_ratio = 32.0f / 9.0f;  break;
         case 4:  geom.aspect_ratio = 64.0f / 27.0f; break;
         default: geom.aspect_ratio = 4.0f / 3.0f;   break;
      }
      environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geom);
   }
}

/* i_system - i_main */

static dbool   InDisplay = false;

dbool   I_StartDisplay(void)
{
   struct retro_framebuffer fb;

   if (InDisplay)
      return false;

   InDisplay = true;

   /* Direct-render acquisition.  libretro.h: the buffer returned
    * from GET_CURRENT_SOFTWARE_FRAMEBUFFER is valid only until
    * retro_run returns, so do this once per frame and unbind in
    * I_FinishUpdate.  retro_load_game runs D_DoomLoop a few times
    * during init before the frontend's video pipeline is fully up
    * -- the in_retro_run gate skips acquisition during that
    * window. */
   direct_fb_data  = NULL;
   direct_fb_pitch = 0;

   if (in_retro_run && environ_cb)
   {
      fb.data         = NULL;
      fb.width        = SCREENWIDTH;
      fb.height       = SCREENHEIGHT;
      fb.pitch        = 0;
      fb.format       = RETRO_PIXEL_FORMAT_RGB565;
      fb.access_flags = RETRO_MEMORY_ACCESS_WRITE;
      fb.memory_flags = 0;

      if (environ_cb(RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER,
                     &fb)
            && fb.data
            && fb.format == RETRO_PIXEL_FORMAT_RGB565
            && fb.pitch  == (size_t)SCREENPITCH)
      {
         /* Repoint screens[0] and the renderer's cached top-left
          * pointers (latched at R_InitBuffer time) at the
          * frontend buffer.  Every column/span drawer reads
          * drawvars.short_topleft, so without re-latching here
          * the renderer would keep writing to screen_buf and we
          * would gain nothing.
          *
          * Pitch must match exactly -- the renderer treats
          * SURFACE_SHORT_PITCH (= SCREENWIDTH) as a global
          * constant, so a mismatched-pitch buffer cannot be
          * direct-rendered without a much larger refactor.  When
          * pitch differs we fall through to the heap path, and
          * I_FinishUpdate hands the frontend screen_buf for it
          * to copy into the differently-strided destination
          * itself.
          *
          * Format must be RGB565 -- frontends are allowed to
          * return a different format than SET_PIXEL_FORMAT
          * negotiated, e.g. when running with a HW backend that
          * needs an internal conversion stage.  We can only
          * direct-render when the formats agree. */
         direct_fb_data         = (unsigned char *)fb.data;
         direct_fb_pitch        = (unsigned int)fb.pitch;
         screens[0].data        = direct_fb_data;
         drawvars.short_topleft = (unsigned short *)direct_fb_data;
         drawvars.int_topleft   = (unsigned int   *)direct_fb_data;

         if (!sw_fb_checked && log_cb)
         {
            log_cb(RETRO_LOG_INFO,
                  "Software framebuffer acquired"
                  " (pitch: %u, direct render).\n",
                  (unsigned)fb.pitch);
            sw_fb_checked = true;
            have_sw_fb    = true;
         }
      }
   }

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
dbool HasTrailingSlash(const char* dn)
{
  size_t dn_len = strlen(dn);
  return ( dn && ((dn[dn_len - 1] == '/') || (dn[dn_len - 1] == '\\')));
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

   /* If the caller hands us an already-absolute path, don't prepend
    * a search dir to it.  FindFileInDir would otherwise stitch
    * `g_wad_dir + '/' + '/abs/path/to/file'` and the result wouldn't
    * exist.  This matters for m3u playlist support (issue #196):
    * the playlist parser resolves each entry to an absolute path
    * relative to the m3u file's directory and hands those paths to
    * the engine via -iwad / -file, and the engine's FindIWADFile
    * routes them through I_FindFile when it locates the IWAD.
    * Mirror what FindFileInDir does with the optional extension:
    * append `ext` if supplied. */
   if (wfname && (wfname[0] == '/' || wfname[0] == '\\' ||
                  (wfname[0] && wfname[1] == ':')))
   {
      size_t need = strlen(wfname) + (ext ? strlen(ext) : 0) + 1;
      char *abs = malloc(need);
      if (abs)
      {
         strcpy(abs, wfname);
         if (ext && ext[0] != '\0')
            strcat(abs, ext);
         if (path_is_valid(abs))
         {
            if (log_cb)
               log_cb(RETRO_LOG_DEBUG, "I_FindFile: using absolute path %s\n", abs);
            return abs;
         }
         free(abs);
      }
      /* Absolute path didn't resolve; fall through to the relative
       * search.  Almost certain to fail too, but symmetry with the
       * existing not-found behaviour keeps the diagnostics
       * unsurprising. */
   }

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
        prboom_system_dir = NULL;
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
  /* Remembers the rate last programmed into tic_vars.sample_step so a
   * sample-rate-only change (fps unchanged) still re-derives the step and
   * pushes SET_SYSTEM_AV_INFO.  Initialised to 0 so the first call always
   * takes the update path. */
  static double last_sample_rate = 0.0;
  struct retro_system_av_info info;
  retro_get_system_av_info(&info);
  if(tic_vars.fps != info.timing.fps || last_sample_rate != info.timing.sample_rate)
  {
     // Only update av_info if changed and it's not the first run
     if(tic_vars.fps)
        environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &info);

     tic_vars.fps = info.timing.fps;
     tic_vars.frac_step = FRACUNIT * TICRATE / tic_vars.fps;
     /* 16.16 fixed-point samples-per-frame.  Integer truncation here
      * (44100 / fps) drops the fractional remainder every frame, so for
      * any fps that does not divide the sample rate the core emits fewer
      * than sample_rate samples per second (e.g. 120fps -> 44040/s, a
      * 60-sample/s deficit) -- which silently forces the frontend's
      * dynamic-rate-control resampler to stretch the stream and slowly
      * drifts A/V sync.  Keep the fraction and let I_UpdateSound emit
      * floor or floor+1 frames via an accumulator so the long-run rate
      * is exact. */
     tic_vars.sample_step = (fixed_t)(((int64_t)info.timing.sample_rate
                                       << FRACBITS) / (int64_t)tic_vars.fps);
     last_sample_rate = info.timing.sample_rate;

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
