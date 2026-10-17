/* Regression lane: HDR10 negotiation goes through the versioned
 * RETRO_ENVIRONMENT_GET_HDR_METADATA query.
 *
 * With the Color Format option at "30bits (HDR)" the core must
 *  - send GET_HDR_METADATA with version 1 and size == sizeof the struct,
 *  - pick HDR10_2101010 only when source_formats has the HDR10 bit
 *    (ignoring bits it does not know), XRGB8888 when it
 *    is clear or the query is refused,
 *  - re-query during retro_run so paper-white changes are picked up,
 *  - never send the retired experimental commands 88-92,
 *  - never ask for SCRGB_RGBA16F, even when it is advertised,
 *  - push one non-blank frame per retro_run.
 *
 *   ./hdr_negotiate_test <core> <iwad> <yes|no|refuse>
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

#define MODE_YES    0
#define MODE_NO     1
#define MODE_REFUSE 2

static int  mode                = MODE_YES;
static int  bad_header          = 0;
static int  retired_cmds        = 0;
static int  queries_in_load     = 0;
static int  queries_in_run      = 0;
static int  in_load             = 0;
static int  in_run              = 0;
static int  frames_in_run       = 0;
static int  nonblank_frames     = 0;
static int  fmt_set             = 0;
static int  fp16_asks           = 0;
static enum retro_pixel_format pix_fmt = RETRO_PIXEL_FORMAT_0RGB1555;
static float paper_white        = 300.0f;
static char sysdir[]            = ".";

static void log_cb(enum retro_log_level level, const char *fmt, ...)
{
   (void)level; (void)fmt;
}

static void video_refresh(const void *data, unsigned w, unsigned h,
      size_t pitch)
{
   if (in_run)
      frames_in_run++;
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

static bool answer_hdr(struct retro_hdr_metadata *m)
{
   if (in_load)
      queries_in_load++;
   else if (in_run)
      queries_in_run++;
   if (!m || m->version != 1 || m->size != sizeof(*m))
   {
      bad_header++;
      return false;
   }
   if (mode == MODE_REFUSE)
      return false;
   m->output_mode      = 1;
   /* yes: HDR10 with scRGB and a bit the core must ignore; no: scRGB
    * alone, which is not HDR10 */
   m->source_formats   = (mode == MODE_YES)
      ? (RETRO_HDR_SOURCE_FORMAT_HDR10_2101010
            | RETRO_HDR_SOURCE_FORMAT_SCRGB_RGBA16F | 0x80000000u)
      : RETRO_HDR_SOURCE_FORMAT_SCRGB_RGBA16F;
   m->paper_white_nits = paper_white;
   m->max_nits         = 1000.0f;
   m->expand_gamut     = 0;
   return true;
}

static bool environ_cb(unsigned cmd, void *data)
{
   unsigned base = cmd & ~RETRO_ENVIRONMENT_EXPERIMENTAL;
   if ((cmd & RETRO_ENVIRONMENT_EXPERIMENTAL) && base >= 88 && base <= 92)
   {
      retired_cmds++;
      return false;
   }

   switch (cmd)
   {
      case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
         ((struct retro_log_callback*)data)->log = log_cb;
         return true;
      case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
         if (*(const enum retro_pixel_format*)data
               == RETRO_PIXEL_FORMAT_SCRGB_RGBA16F)
         {
            fp16_asks++;
            return false;
         }
         pix_fmt = *(const enum retro_pixel_format*)data;
         fmt_set = 1;
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
            if (v->key && !strcmp(v->key, "prboom-color_format"))
            {
               v->value = "30bits (HDR)";
               return true;
            }
            v->value = NULL;
         }
         return false;
      case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
         *(bool*)data = false;
         return true;
      case RETRO_ENVIRONMENT_GET_HDR_METADATA:
         return answer_hdr((struct retro_hdr_metadata*)data);
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
   int i, rc = 0;
   enum retro_pixel_format want;
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

   if (argc < 4)
   {
      fprintf(stderr, "usage: %s core iwad yes|no|refuse\n", argv[0]);
      return 2;
   }
   if      (!strcmp(argv[3], "yes"))    mode = MODE_YES;
   else if (!strcmp(argv[3], "no"))     mode = MODE_NO;
   else if (!strcmp(argv[3], "refuse")) mode = MODE_REFUSE;
   else
   {
      fprintf(stderr, "unknown mode %s\n", argv[3]);
      return 2;
   }

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

   in_load = 1;
   if (!retro_load_game(&info))
   {
      fprintf(stderr, "retro_load_game failed\n");
      return 2;
   }
   in_load = 0;

   /* Move paper white mid-session: the core polls about once a second
    * of game time, so 80 runs cover at least two polls. */
   in_run = 1;
   for (i = 0; i < 80; i++)
   {
      if (i == 20)
         paper_white = 250.0f;
      retro_run();
   }
   in_run = 0;

   retro_unload_game();
   retro_deinit();

   want = (mode == MODE_YES) ? RETRO_PIXEL_FORMAT_HDR10_2101010
                             : RETRO_PIXEL_FORMAT_XRGB8888;

   printf("mode %s: format %d, queries load %d run %d, frames %d (%d "
          "non-blank)\n", argv[3], (int)pix_fmt, queries_in_load,
          queries_in_run, frames_in_run, nonblank_frames);

   if (!fmt_set || pix_fmt != want)
   {
      printf("FAIL: expected pixel format %d\n", (int)want);
      rc = 1;
   }
   if (fp16_asks)
   {
      printf("FAIL: %d requests for SCRGB_RGBA16F\n", fp16_asks);
      rc = 1;
   }
   if (retired_cmds)
   {
      printf("FAIL: %d retired HDR commands (88-92) sent\n", retired_cmds);
      rc = 1;
   }
   if (bad_header)
   {
      printf("FAIL: %d GET_HDR_METADATA calls with a bad version/size\n",
             bad_header);
      rc = 1;
   }
   if (queries_in_load == 0)
   {
      printf("FAIL: GET_HDR_METADATA not queried during load\n");
      rc = 1;
   }
   if (mode == MODE_YES && queries_in_run == 0)
   {
      printf("FAIL: HDR session never re-queried GET_HDR_METADATA\n");
      rc = 1;
   }
   if (frames_in_run < 80)
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
