/* Drive a core through several load / run / unload sessions in one
 * process, the way a frontend does on a content switch.  Every session
 * has to behave like the first: a frame per retro_run, no frame from
 * inside retro_load_game, and no shutdown request.
 *
 *   ./sessions_test core.so iwad.wad [sessions] [runs] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <dlfcn.h>

#include "libretro.h"

static int  in_load        = 0;
static int  frames_in_load = 0;
static int  frames_total   = 0;
static int  shutdowns      = 0;
static char sysdir[]       = ".";
static int  verbose        = 0;

static void log_cb(enum retro_log_level level, const char *fmt, ...)
{
   va_list ap;
   if (!verbose)
      return;
   (void)level;
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
}

static void video_refresh(const void *data, unsigned w, unsigned h, size_t pitch)
{
   (void)data; (void)w; (void)h; (void)pitch;
   if (in_load)
      frames_in_load++;
   frames_total++;
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
         ((struct retro_variable*)data)->value = NULL;
         return false;
      case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
         *(bool*)data = false;
         return true;
      case RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER:
         return false;
      case RETRO_ENVIRONMENT_SHUTDOWN:
         shutdowns++;
         return true;
      default:
         break;
   }
   return false;
}

#define SYM(h, n) do { \
   *(void**)(&n) = dlsym(h, #n); \
   if (!n) { fprintf(stderr, "missing %s\n", #n); return 2; } \
} while (0)

int main(int argc, char **argv)
{
   void *h;
   struct retro_game_info info;
   int s, i, sessions = 3, runs = 12;
   void (*retro_init)(void);
   void (*retro_deinit)(void);
   void (*retro_run)(void);
   size_t (*retro_serialize_size)(void);
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
      fprintf(stderr, "usage: %s core.so iwad.wad [sessions] [runs]\n", argv[0]);
      return 2;
   }
   if (argc > 3) sessions = atoi(argv[3]);
   if (argc > 4) runs     = atoi(argv[4]);
   if (getenv("PRB_VERBOSE")) verbose = 1;

   h = dlopen(argv[1], RTLD_NOW);
   if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }

   SYM(h, retro_init);
   SYM(h, retro_deinit);
   SYM(h, retro_run);
   SYM(h, retro_serialize_size);
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

   for (s = 1; s <= sessions; s++)
   {
      printf("== session %d: load\n", s);
      fflush(stdout);

      memset(&info, 0, sizeof(info));
      info.path = argv[2];

      in_load = 1;
      if (!retro_load_game(&info))
      {
         printf("session %d: retro_load_game FAILED\n", s);
         return 1;
      }
      in_load = 0;
      printf("== session %d: loaded, serialize_size %u\n",
            s, (unsigned)retro_serialize_size());
      fflush(stdout);

      for (i = 0; i < runs; i++)
      {
         retro_run();
         if (((i + 1) % 4) == 0)
         {
            printf("== session %d: ran %d\n", s, i + 1);
            fflush(stdout);
         }
      }

      printf("== session %d: unload\n", s);
      fflush(stdout);
      retro_unload_game();
   }

   retro_deinit();

   printf("frames during load: %d, total %d (expected %d)\n",
         frames_in_load, frames_total, sessions * runs);
   printf("shutdown requests : %d\n", shutdowns);

   if (frames_in_load)
   {
      printf("FAIL: video refresh called from retro_load_game\n");
      return 1;
   }
   if (shutdowns)
   {
      printf("FAIL: core asked the frontend to shut down\n");
      return 1;
   }
   if (frames_total != sessions * runs)
   {
      printf("FAIL: a session stopped producing frames\n");
      return 1;
   }
   printf("PASS\n");
   return 0;
}
