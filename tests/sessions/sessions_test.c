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
static int  nonblank       = 0;
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
   if (in_load)
      frames_in_load++;
   frames_total++;

   if (data && w && h)
   {
      const unsigned char *p = (const unsigned char*)data;
      size_t y, x, nz = 0;
      for (y = 0; y < h; y += 8)
         for (x = 0; x < pitch; x += 16)
            if (p[y * pitch + x])
               nz++;
      if (nz > 16)
         nonblank++;
   }
}

/* Write the first DEMO lump of an iwad out as a standalone .lmp, so the
 * -playdemo content path gets driven as well as the title screen.  The
 * file lands beside the iwad, which is where the core looks for it. */
static const char *make_demo(const char *iwad, char *out, size_t outlen)
{
   FILE *f = fopen(iwad, "rb");
   unsigned char hdr[16], *dir, *lump;
   unsigned long numlumps, infotable, i;
   const char *slash;
   size_t dlen;

   if (!f)
      return NULL;
   if (fread(hdr, 1, 12, f) != 12)
   { fclose(f); return NULL; }
   numlumps  = hdr[4] | (hdr[5]<<8) | ((unsigned long)hdr[6]<<16) | ((unsigned long)hdr[7]<<24);
   infotable = hdr[8] | (hdr[9]<<8) | ((unsigned long)hdr[10]<<16) | ((unsigned long)hdr[11]<<24);

   dir = (unsigned char*)malloc(numlumps * 16);
   if (!dir) { fclose(f); return NULL; }
   fseek(f, (long)infotable, SEEK_SET);
   if (fread(dir, 16, numlumps, f) != numlumps)
   { free(dir); fclose(f); return NULL; }

   for (i = 0; i < numlumps; i++)
   {
      unsigned char *e = dir + i * 16;
      unsigned long pos, len;
      FILE *o;

      if (memcmp(e + 8, "DEMO1", 5) != 0)
         continue;
      pos = e[0] | (e[1]<<8) | ((unsigned long)e[2]<<16) | ((unsigned long)e[3]<<24);
      len = e[4] | (e[5]<<8) | ((unsigned long)e[6]<<16) | ((unsigned long)e[7]<<24);
      lump = (unsigned char*)malloc(len);
      if (!lump)
         break;
      fseek(f, (long)pos, SEEK_SET);
      if (fread(lump, 1, len, f) != len)
      { free(lump); break; }

      slash = strrchr(iwad, '/');
      dlen  = slash ? (size_t)(slash - iwad) + 1 : 0;
      if (dlen + 10 >= outlen)
      { free(lump); break; }
      memcpy(out, iwad, dlen);
      strcpy(out + dlen, "demo1.lmp");

      o = fopen(out, "wb");
      if (o)
      {
         fwrite(lump, 1, len, o);
         fclose(o);
         free(lump); free(dir); fclose(f);
         return out;
      }
      free(lump);
      break;
   }
   free(dir);
   fclose(f);
   return NULL;
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
   int s, i, sessions = 3, runs = 12, demo = 0;
   char demopath[1024];
   const char *content;
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
      fprintf(stderr, "usage: %s core.so iwad.wad [sessions] [runs] [demo]\n", argv[0]);
      return 2;
   }
   if (argc > 3) sessions = atoi(argv[3]);
   if (argc > 4) runs     = atoi(argv[4]);
   if (argc > 5 && !strcmp(argv[5], "demo")) demo = 1;
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

   content = argv[2];
   if (demo)
   {
      content = make_demo(argv[2], demopath, sizeof(demopath));
      if (!content)
      {
         fprintf(stderr, "could not extract a DEMO1 lump from %s\n", argv[2]);
         return 2;
      }
      printf("content: %s (-playdemo path)\n", content);
   }

   retro_init();

   for (s = 1; s <= sessions; s++)
   {
      printf("== session %d: load\n", s);
      fflush(stdout);

      memset(&info, 0, sizeof(info));
      info.path = content;

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
   printf("non-blank frames  : %d\n", nonblank);

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
   if (nonblank < sessions)
   {
      printf("FAIL: sessions rendered nothing but blank frames\n");
      return 1;
   }
   printf("PASS\n");
   return 0;
}
