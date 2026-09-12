/* Minimal libretro host: asserts the core pushes no video frame from
 * inside retro_load_game, and that it does push one per retro_run.
 *
 *   cc -o load_no_frame_test load_no_frame_test.c -ldl
 *   ./load_no_frame_test ./prboom_libretro.so /path/to/iwad.wad
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <dlfcn.h>

#include "libretro.h"

static int in_load          = 0;
static int frames_in_load   = 0;
static int frames_in_run    = 0;
static int frames_elsewhere = 0;
static int nonblank_frames  = 0;
static enum retro_pixel_format pix_fmt = RETRO_PIXEL_FORMAT_0RGB1555;
static int in_run           = 0;
static char sysdir[]        = ".";

static void log_cb(enum retro_log_level level, const char *fmt, ...)
{
   (void)level; (void)fmt;
}

static void video_refresh(const void *data, unsigned w, unsigned h, size_t pitch)
{
   if (in_load)
      frames_in_load++;
   else if (in_run)
      frames_in_run++;
   else
      frames_elsewhere++;

   if (data && w && h)
   {
      const unsigned char *p = (const unsigned char*)data;
      size_t y, x, nz = 0;
      for (y = 0; y < h; y += 8)
         for (x = 0; x < pitch; x += 16)
            if (p[y * pitch + x])
               nz++;
      if (nz > 16)
         nonblank_frames++;
   }
}

static void audio_sample(int16_t l, int16_t r) { (void)l; (void)r; }
static size_t audio_batch(const int16_t *d, size_t f) { (void)d; return f; }
static void input_poll(void) { }
static int16_t input_state(unsigned p, unsigned d, unsigned i, unsigned id)
{
   (void)p; (void)d; (void)i; (void)id; return 0;
}

static bool environ_cb(unsigned cmd, void *data)
{
   switch (cmd)
   {
      case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
         ((struct retro_log_callback*)data)->log = log_cb;
         return true;
      case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
         pix_fmt = *(const enum retro_pixel_format*)data;
         return true;
      case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
      case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
         *(const char**)data = sysdir;
         return true;
      case RETRO_ENVIRONMENT_GET_CAN_DUPE:
         *(bool*)data = true;
         return true;
      case RETRO_ENVIRONMENT_GET_VARIABLE:
         ((struct retro_variable*)data)->value = NULL;
         return false;
      case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
         *(bool*)data = false;
         return true;
      case RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER:
         /* No framebuffer service: the core must fall back to its
          * own buffer, which is the path the crash report shows. */
         return false;
      default:
         break;
   }
   return false;
}

#define SYM(h, t, n) do { \
   *(void**)(&n) = dlsym(h, #n); \
   if (!n) { fprintf(stderr, "missing %s\n", #n); return 2; } \
} while (0)

int main(int argc, char **argv)
{
   void *h;
   struct retro_game_info info;
   int i, rc = 0;
   void (*retro_init)(void);
   void (*retro_deinit)(void);
   void (*retro_run)(void);
   bool (*retro_load_game)(const struct retro_game_info*);
   void (*retro_unload_game)(void);
   void (*retro_set_environment)(retro_environment_t);
   void (*retro_set_video_refresh)(retro_video_refresh_t);
   void (*retro_set_audio_sample)(retro_audio_sample_t);
   void (*retro_set_audio_sample_batch)(retro_audio_sample_batch_t);
   void (*retro_set_input_poll)(retro_input_poll_t);
   void (*retro_set_input_state)(retro_input_state_t);

   if (argc < 3)
   {
      fprintf(stderr, "usage: %s core.so iwad.wad\n", argv[0]);
      return 2;
   }

   h = dlopen(argv[1], RTLD_NOW);
   if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }

   SYM(h, void, retro_init);
   SYM(h, void, retro_deinit);
   SYM(h, void, retro_run);
   SYM(h, void, retro_load_game);
   SYM(h, void, retro_unload_game);
   SYM(h, void, retro_set_environment);
   SYM(h, void, retro_set_video_refresh);
   SYM(h, void, retro_set_audio_sample);
   SYM(h, void, retro_set_audio_sample_batch);
   SYM(h, void, retro_set_input_poll);
   SYM(h, void, retro_set_input_state);

   retro_set_environment(environ_cb);
   retro_set_video_refresh(video_refresh);
   retro_set_audio_sample(audio_sample);
   retro_set_audio_sample_batch(audio_batch);
   retro_set_input_poll(input_poll);
   retro_set_input_state(input_state);

   retro_init();

   memset(&info, 0, sizeof(info));
   info.path = argv[2];

   in_load = 1;
   if (!retro_load_game(&info))
   {
      fprintf(stderr, "retro_load_game failed\n");
      return 2;
   }
   in_load = 0;

   in_run = 1;
   for (i = 0; i < 12; i++)
      retro_run();
   in_run = 0;

   retro_unload_game();
   retro_deinit();

   printf("frames during load: %d\n", frames_in_load);
   printf("frames during run : %d (12 runs)\n", frames_in_run);
   printf("frames elsewhere  : %d\n", frames_elsewhere);
   printf("non-blank frames  : %d\n", nonblank_frames);

   if (frames_in_load != 0)
   {
      printf("FAIL: video refresh called from retro_load_game\n");
      rc = 1;
   }
   if (frames_elsewhere != 0)
   {
      printf("FAIL: video refresh called outside retro_run\n");
      rc = 1;
   }
   if (frames_in_run < 12)
   {
      printf("FAIL: expected a frame per retro_run\n");
      rc = 1;
   }
   if (nonblank_frames == 0)
   {
      printf("FAIL: every frame was blank\n");
      rc = 1;
   }
   if (!rc)
      printf("PASS\n");
   return rc;
}
