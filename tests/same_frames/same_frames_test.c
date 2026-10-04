/* Minimal libretro host: content that restates what the game already
 * is must not change it.  Two loads, each in a process of its own so
 * both are first loads, run the title and demo sequence; they must draw
 * the same frames and end with a saved state of the same size.
 *
 *   pointer  the IWAD, against the IWAD behind a DeHackEd patch that
 *            points every frame at its own stock action.  A "Pointer"
 *            block copies the action a frame had before any patch ran;
 *            if the core has lost that record every frame gets a null
 *            action and nothing acts.
 *
 *   mapinfo  the IWAD, against the IWAD with a PWAD holding a ZDoom
 *            MAPINFO for a map the demo never visits.  The core
 *            translates that lump into UMAPINFO entries; the IWAD's
 *            demos were recorded without it and must still play.
 *
 *   bits     a no-monsters demo, against the same demo behind a patch
 *            restating the monsters' stock numeric Bits.  A numeric
 *            value names one half of the flags; if applying it loses
 *            the other half the things stop being monsters and the
 *            demo's map fills up.
 *
 *   cc -o same_frames_test same_frames_test.c -ldl
 *   ./same_frames_test ./prboom_libretro.so /path/to/doom1-iwad.wad [mode]
 *
 * Writes its content files in the working directory.
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
#define DEMO_TICS   2000  /* idle tics in the generated demo */

static enum retro_pixel_format pix_fmt = RETRO_PIXEL_FORMAT_0RGB1555;
static uint32_t hashes[RUNS + 1];  /* last slot: saved state size */
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

static void put32(unsigned char *p, uint32_t v)
{
   p[0] = (unsigned char)v;         p[1] = (unsigned char)(v >> 8);
   p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}

/* A PWAD of up to three lumps. */
static int write_pwad(const char *path, int n, const char **names,
      const unsigned char **data, const size_t *len)
{
   unsigned char hdr[12], ent[16];
   uint32_t pos = 12;
   FILE *f = fopen(path, "wb");
   int i;

   if (!f)
      return 0;
   for (i = 0; i < n; i++)
      pos += (uint32_t)len[i];
   memcpy(hdr, "PWAD", 4);
   put32(hdr + 4, (uint32_t)n);
   put32(hdr + 8, pos);
   fwrite(hdr, 1, 12, f);
   for (i = 0; i < n; i++)
      fwrite(data[i], 1, len[i], f);
   pos = 12;
   for (i = 0; i < n; i++)
   {
      memset(ent, 0, sizeof(ent));
      put32(ent, pos);
      put32(ent + 4, (uint32_t)len[i]);
      strncpy((char*)ent + 8, names[i], 8);
      fwrite(ent, 1, 16, f);
      pos += (uint32_t)len[i];
   }
   return fclose(f) == 0;
}

/* <name>.m3u in `cwd`: the IWAD, then `second` if there is one. */
static int write_m3u(char *out, size_t out_len, const char *cwd,
      const char *name, const char *iwad_abs, const char *second)
{
   char file[64];
   FILE *f;

   snprintf(file, sizeof(file), "%s.m3u", name);
   f = fopen(file, "w");
   if (!f)
      return 0;
   fprintf(f, "%s\n", iwad_abs);
   if (second)
      fprintf(f, "%s\n", second);
   fclose(f);
   snprintf(out, out_len, "%s/%s", cwd, file);
   return 1;
}

/* Stock numeric Bits of the Doom 1 monsters, by DeHackEd thing number. */
static const char bits_patch[] =
   "Patch File for DeHackEd v3.0\nDoom version = 19\nPatch format = 6\n\n"
   "Thing 2 (Zombieman)\nBits = 4194310\n\n"
   "Thing 3 (Shotgun guy)\nBits = 4194310\n\n"
   "Thing 12 (Imp)\nBits = 4194310\n\n"
   "Thing 13 (Demon)\nBits = 4194310\n\n"
   "Thing 14 (Spectre)\nBits = 4456454\n\n"
   "Thing 15 (Cacodemon)\nBits = 4211206\n\n"
   "Thing 16 (Baron of Hell)\nBits = 4194310\n\n"
   "Thing 19 (Lost soul)\nBits = 16902\n\n";

static const char zmapinfo[] =
   "map E3M9 \"Somewhere the demo does not go\"\n{\n\tnext = \"E3M1\"\n}\n";

/* Fills a[] and b[] with the two content paths for `mode`. */
static int write_content(const char *mode, const char *iwad,
      char *a, char *b, size_t len)
{
   char cwd[PATH_MAX];
   char iwad_abs[PATH_MAX];
   FILE *f;
   int i;

   if (!realpath(".", cwd) || !realpath(iwad, iwad_abs))
      return 0;

   if (!strcmp(mode, "pointer"))
   {
      f = fopen("identity.deh", "w");
      if (!f)
         return 0;
      fprintf(f, "Patch File for DeHackEd v3.0\n"
                 "Doom version = 19\nPatch format = 6\n\n");
      for (i = 0; i < NUM_FRAMES; i++)
         fprintf(f, "Pointer %d (Frame %d)\nCodep Frame = %d\n\n", i, i, i);
      fclose(f);
      snprintf(a, len, "%s", iwad_abs);
      return write_m3u(b, len, cwd, "identity", iwad_abs, "identity.deh");
   }

   if (!strcmp(mode, "mapinfo"))
   {
      const char *names[1];
      const unsigned char *data[1];
      size_t lens[1];
      names[0] = "MAPINFO";
      data[0]  = (const unsigned char*)zmapinfo;
      lens[0]  = sizeof(zmapinfo) - 1;
      if (!write_pwad("zmapinfo.wad", 1, names, data, lens))
         return 0;
      snprintf(a, len, "%s", iwad_abs);
      return write_m3u(b, len, cwd, "zmapinfo", iwad_abs, "zmapinfo.wad");
   }

   if (!strcmp(mode, "bits"))
   {
      /* A v1.9 demo: skill 3, E1M1, no monsters, one idle player. */
      static unsigned char demo[13 + 4 * DEMO_TICS + 1];
      static const unsigned char head[13] =
         { 109, 2, 1, 1, 0, 0, 0, 1, 0, 1, 0, 0, 0 };
      const char *names[4];
      const unsigned char *data[4];
      size_t lens[4];
      int n;

      memcpy(demo, head, sizeof(head));
      demo[sizeof(demo) - 1] = 0x80;
      /* Every demo slot, so the comparison does not depend on which
       * one the title sequence reaches first. */
      for (n = 0; n < 3; n++)
      {
         static const char *slot[3] = { "DEMO1", "DEMO2", "DEMO3" };
         names[n] = slot[n];
         data[n]  = demo;
         lens[n]  = sizeof(demo);
      }
      if (!write_pwad("nomonsters.wad", 3, names, data, lens))
         return 0;
      names[3] = "DEHACKED";
      data[3]  = (const unsigned char*)bits_patch;
      lens[3]  = sizeof(bits_patch) - 1;
      if (!write_pwad("nomonsters_bits.wad", 4, names, data, lens))
         return 0;
      return write_m3u(a, len, cwd, "nomonsters", iwad_abs, "nomonsters.wad")
          && write_m3u(b, len, cwd, "nomonsters_bits", iwad_abs,
                "nomonsters_bits.wad");
   }

   return 0;
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
   size_t (*retro_serialize_size)(void);
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
   SYM(h, retro_serialize_size);
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
   hashes[RUNS] = (uint32_t)retro_serialize_size();
   retro_unload_game();
   retro_deinit();

   if (write(fd, hashes, sizeof(hashes)) != (ssize_t)sizeof(hashes))
      return 2;
   return 0;
}

int main(int argc, char **argv)
{
   static uint32_t got[2][RUNS + 1];
   static char content[2][PATH_MAX + 64];
   const char *mode = (argc > 3) ? argv[3] : "pointer";
   int i, s, first = -1, differing = 0;

   if (argc < 3)
   {
      fprintf(stderr, "usage: %s core.so iwad.wad [pointer|mapinfo|bits]\n",
            argv[0]);
      return 2;
   }
   if (!write_content(mode, argv[2], content[0], content[1],
            sizeof(content[0])))
   {
      fprintf(stderr, "could not write the content for mode %s\n", mode);
      return 2;
   }

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

   printf("mode            : %s\n", mode);
   printf("frames compared : %d\n", RUNS);
   printf("frames differing: %d\n", differing);
   printf("state sizes     : %u, %u\n",
         (unsigned)got[0][RUNS], (unsigned)got[1][RUNS]);
   if (differing)
   {
      printf("FAIL: the two runs differ from frame %d on\n", first);
      return 1;
   }
   if (got[0][RUNS] != got[1][RUNS])
   {
      printf("FAIL: the two runs end with different saved state sizes\n");
      return 1;
   }
   printf("PASS\n");
   return 0;
}
