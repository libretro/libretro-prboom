/* Regression lane: "auto" internal resolution from
 * RETRO_ENVIRONMENT_GET_DISPLAY_METADATA.
 *
 * The frontend reports a panel size; the core must send the query with
 * version 1 and size == sizeof the struct, render at the 32x20-step size
 * that fills the panel (a 320x200 frame is shown 4:3, so n steps cover
 * 32n x 24n panel pixels), clamp to the renderer's maximum, and use
 * 320x200 when the geometry is not reported.
 *
 *   ./auto_resolution_test <core> <iwad> <panel WxH | none> <expect WxH>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>

#ifdef _WIN32
#include <windows.h>
#define LIB_OPEN(p)    ((void*)LoadLibraryA(p))
#define LIB_SYM(h, n)  ((void*)GetProcAddress((HMODULE)(h), n))
#else
#include <dlfcn.h>
#define LIB_OPEN(p)    dlopen(p, RTLD_NOW)
#define LIB_SYM(h, n)  dlsym(h, n)
#endif

#include "libretro.h"

static unsigned panel_w = 0, panel_h = 0;
static unsigned frame_w = 0, frame_h = 0;
static int      bad_header = 0, queries = 0, frames = 0;
static char     sysdir[] = ".";

static void log_cb(enum retro_log_level level, const char *fmt, ...)
{
   (void)level; (void)fmt;
}

static void video_refresh(const void *data, unsigned w, unsigned h,
      size_t pitch)
{
   (void)pitch;
   if (data && w && h)
   {
      frame_w = w;
      frame_h = h;
      frames++;
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
         return true;
      case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
      case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
         *(const char**)data = sysdir;
         return true;
      case RETRO_ENVIRONMENT_GET_CAN_DUPE:
         *(bool*)data = true;
         return true;
      case RETRO_ENVIRONMENT_GET_VARIABLE:
         {
            struct retro_variable *v = (struct retro_variable*)data;
            if (v->key && !strcmp(v->key, "prboom-resolution"))
            {
               v->value = "auto";
               return true;
            }
            v->value = NULL;
         }
         return false;
      case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
         *(bool*)data = false;
         return true;
      case RETRO_ENVIRONMENT_GET_DISPLAY_METADATA:
         {
            struct retro_display_metadata *d =
               (struct retro_display_metadata*)data;
            queries++;
            if (!d || d->version != 1 || d->size != sizeof(*d))
            {
               bad_header++;
               return false;
            }
            d->geo.native_width  = panel_w;
            d->geo.native_height = panel_h;
         }
         return true;
      default:
         break;
   }
   return false;
}

#define SYM(h, n) do { \
   *(void**)(&n) = LIB_SYM(h, #n); \
   if (!n) { fprintf(stderr, "missing %s\n", #n); return 2; } \
} while (0)

int main(int argc, char **argv)
{
   void *h;
   struct retro_game_info info;
   unsigned want_w = 0, want_h = 0;
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

   if (argc < 5)
   {
      fprintf(stderr, "usage: %s core iwad WxH|none WxH\n", argv[0]);
      return 2;
   }
   if (strcmp(argv[3], "none")
         && sscanf(argv[3], "%ux%u", &panel_w, &panel_h) != 2)
      return 2;
   if (sscanf(argv[4], "%ux%u", &want_w, &want_h) != 2)
      return 2;

   h = LIB_OPEN(argv[1]);
   if (!h) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }

   SYM(h, retro_init);
   SYM(h, retro_deinit);
   SYM(h, retro_run);
   SYM(h, retro_load_game);
   SYM(h, retro_unload_game);
   SYM(h, retro_set_environment);
   SYM(h, retro_set_video_refresh);
   SYM(h, retro_set_audio_sample);
   SYM(h, retro_set_audio_sample_batch);
   SYM(h, retro_set_input_poll);
   SYM(h, retro_set_input_state);

   retro_set_environment(environ_cb);
   retro_set_video_refresh(video_refresh);
   retro_set_audio_sample(audio_sample);
   retro_set_audio_sample_batch(audio_batch);
   retro_set_input_poll(input_poll);
   retro_set_input_state(input_state);

   retro_init();

   memset(&info, 0, sizeof(info));
   info.path = argv[2];
   if (!retro_load_game(&info))
   {
      fprintf(stderr, "retro_load_game failed\n");
      return 2;
   }
   for (i = 0; i < 4; i++)
      retro_run();
   retro_unload_game();
   retro_deinit();

   printf("panel %s: frame %ux%u, expected %ux%u, %d queries\n",
          argv[3], frame_w, frame_h, want_w, want_h, queries);

   if (bad_header)
   {
      printf("FAIL: GET_DISPLAY_METADATA sent with a bad version/size\n");
      rc = 1;
   }
   if (!queries)
   {
      printf("FAIL: GET_DISPLAY_METADATA never queried\n");
      rc = 1;
   }
   if (frames < 4)
   {
      printf("FAIL: expected a frame per retro_run\n");
      rc = 1;
   }
   /* The aspect-ratio selector may widen from the 4:3 reference width,
    * never narrow it; the height is exact. */
   if (frame_h != want_h || frame_w < want_w)
   {
      printf("FAIL: wrong internal resolution\n");
      rc = 1;
   }
   if (!rc)
      printf("PASS\n");
   return rc;
}
