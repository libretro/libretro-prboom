/* Minimal libretro host: a DeHackEd patch that points every frame at
 * its own stock action must change nothing.
 *
 * A "Pointer" block copies the action a frame had before any patch
 * ran.  If the core has lost that record, the copy hands every frame
 * a null action and the game stops acting: weapons do not raise,
 * monsters do not move.  So the IWAD is run twice over the title and
 * demo sequence, once bare and once behind an identity patch, and
 * the two runs must draw the same frames.
 *
 * Each run gets a process of its own, so the comparison is between
 * two first loads and nothing but the patch differs.
 *
 *   cc -o deh_pointer_test deh_pointer_test.c -ldl
 *   ./deh_pointer_test ./prboom_libretro.so /path/to/iwad.wad
 *
 * Writes identity.deh and identity.m3u in the working directory.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <limits.h>
#include <unistd.h>
#include <sys/wait.h>
#include <dlfcn.h>

#include "libretro.h"

#define RUNS        900   /* title, then well into the first demo */
#define NUM_FRAMES  967   /* frames in the stock Doom table */

static enum retro_pixel_format pix_fmt = RETRO_PIXEL_FORMAT_0RGB1555;
static uint32_t hashes[RUNS];
static int run_no = -1;
static char sysdir[] = ".";

static void log_cb(enum retro_log_level level, const char *fmt, ...)
{
   (void)level; (void)fmt;
}

static void video_refresh(const void *data, unsigned w, unsigned h, size_t pitch)
{
   const unsigned char *p = (const unsigned char*)data;
   size_t bpp = (pix_fmt == RETRO_PIXEL_FORMAT_XRGB8888) ? 4 : 2;
   uint32_t hash = 2166136261u;
   size_t x, y;

   if (run_no < 0 || run_no >= RUNS)
      return;
   if (p)
      for (y = 0; y < h; y++)
         for (x = 0; x < w * bpp; x++)
            hash = (hash ^ p[y * pitch + x]) * 16777619u;
   hashes[run_no] = hash;
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
      default:
         break;
   }
   return false;
}

/* identity.deh: every frame takes the action of the frame with its
 * own number.  identity.m3u: the IWAD, then that patch. */
static int write_content(const char *iwad, char *m3u, size_t m3u_len)
{
   char cwd[PATH_MAX];
   char iwad_abs[PATH_MAX];
   FILE *f;
   int i;

   if (!realpath(".", cwd) || !realpath(iwad, iwad_abs))
      return 0;

   f = fopen("identity.deh", "w");
   if (!f)
      return 0;
   fprintf(f, "Patch File for DeHackEd v3.0\n"
              "Doom version = 19\nPatch format = 6\n\n");
   for (i = 0; i < NUM_FRAMES; i++)
      fprintf(f, "Pointer %d (Frame %d)\nCodep Frame = %d\n\n", i, i, i);
   fclose(f);

   f = fopen("identity.m3u", "w");
   if (!f)
      return 0;
   fprintf(f, "%s\nidentity.deh\n", iwad_abs);
   fclose(f);

   snprintf(m3u, m3u_len, "%s/identity.m3u", cwd);
   return 1;
}

#define SYM(h, n) do { \
   *(void**)(&n) = dlsym(h, #n); \
   if (!n) { fprintf(stderr, "missing %s\n", #n); return 2; } \
} while (0)

/* Load `path`, run it, and write the frame hashes to `fd`. */
static int run_session(const char *core, const char *path, int fd)
{
   void *h;
   struct retro_game_info info;
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

   h = dlopen(core, RTLD_NOW);
   if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }

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
   info.path = path;
   if (!retro_load_game(&info))
   {
      fprintf(stderr, "retro_load_game failed for %s\n", path);
      return 2;
   }
   for (run_no = 0; run_no < RUNS; run_no++)
      retro_run();
   run_no = -1;
   retro_unload_game();
   retro_deinit();

   if (write(fd, hashes, sizeof(hashes)) != (ssize_t)sizeof(hashes))
      return 2;
   return 0;
}

int main(int argc, char **argv)
{
   static uint32_t got[2][RUNS];
   char m3u[PATH_MAX + 32];
   const char *content[2];
   int i, s, first = -1, differing = 0;

   if (argc < 3)
   {
      fprintf(stderr, "usage: %s core.so iwad.wad\n", argv[0]);
      return 2;
   }
   if (!write_content(argv[2], m3u, sizeof(m3u)))
   {
      fprintf(stderr, "could not write identity.deh / identity.m3u\n");
      return 2;
   }
   content[0] = argv[2];
   content[1] = m3u;

   for (s = 0; s < 2; s++)
   {
      int fds[2], status = 0;
      size_t have = 0;
      pid_t pid;

      if (pipe(fds) != 0)
         return 2;
      fflush(NULL);
      pid = fork();
      if (pid < 0)
         return 2;
      if (pid == 0)
      {
         close(fds[0]);
         _exit(run_session(argv[1], content[s], fds[1]));
      }
      close(fds[1]);
      while (have < sizeof(got[s]))
      {
         ssize_t n = read(fds[0], (char*)got[s] + have, sizeof(got[s]) - have);
         if (n <= 0)
            break;
         have += (size_t)n;
      }
      close(fds[0]);
      if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status)
            || WEXITSTATUS(status) != 0 || have != sizeof(got[s]))
      {
         fprintf(stderr, "run %d (%s) did not complete\n", s, content[s]);
         return 2;
      }
   }

   for (i = 0; i < RUNS; i++)
      if (got[0][i] != got[1][i])
      {
         if (first < 0)
            first = i;
         differing++;
      }

   printf("frames compared : %d\n", RUNS);
   printf("frames differing: %d\n", differing);
   if (differing)
   {
      printf("FAIL: identity patch changed the game from frame %d on\n", first);
      return 1;
   }
   printf("PASS\n");
   return 0;
}
