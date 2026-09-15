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
static int  session_no     = 0;
static int  frame_in_sess  = 0;
static int  mismatches     = 0;
static size_t ref_ssize[2] = { 0, 0 };  /* first post-load size per content */
static unsigned long snd_hash;       /* running hash of this session's audio */
static unsigned long ref_snd[2]      = { 0, 0 };
static unsigned long long snd_frames;
static unsigned long long ref_snd_frames[2] = { 0, 0 };
static int  snd_bad        = 0;
static int  accepted_bad   = 0;
static unsigned long agg_hash;       /* whole-session frame hash */
static int  nodes_bad      = 0;
static int  ssize_bad      = 0;
static unsigned long *ref_hash[2];   /* first frame sequence per content */
static int  ref_seen[2]    = { 0, 0 };
static int  content_idx    = 0;
static int  ref_len;
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

      /* Every session has to draw the same thing as the first one:
       * state left behind by a teardown shows up here as a frame that
       * differs from its counterpart in session 1. */
      {
         unsigned long hsh = 2166136261UL;
         size_t k;
         for (k = 0; k < h * pitch; k++)
            hsh = (hsh ^ p[k]) * 16777619UL;
         hsh &= 0xffffffffUL;

         agg_hash = (agg_hash ^ hsh) * 16777619UL;
         agg_hash &= 0xffffffffUL;

         if (!ref_seen[content_idx])
         {
            if (frame_in_sess < ref_len)
               ref_hash[content_idx][frame_in_sess] = hsh;
         }
         else if (frame_in_sess < ref_len
               && ref_hash[content_idx][frame_in_sess] != hsh)
         {
            if (!mismatches)
               printf("FAIL: session %d frame %d differs from the first "
                      "session on this content (%08lx vs %08lx)\n",
                      session_no, frame_in_sess + 1,
                      ref_hash[content_idx][frame_in_sess], hsh);
            mismatches++;
         }
         frame_in_sess++;
      }
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

/* A session that leaves sound or music state behind renders the same
 * pixels but plays something else, so the audio stream gets the same
 * treatment as the frame sequence: hash it and hold every later session
 * on this content to what the first one produced. */
static void mix_sample(int16_t l, int16_t r)
{
   snd_hash = (snd_hash ^ (unsigned long)(unsigned short)l) * 16777619UL;
   snd_hash = (snd_hash ^ (unsigned long)(unsigned short)r) * 16777619UL;
   snd_hash &= 0xffffffffUL;
   snd_frames++;
}

/* Content the core has to reject, one file per failure branch in
 * retro_load_game, ordered by how far into init each one gets before it
 * gives up.  The last reaches D_DoomMainSetup, which means a session's
 * worth of subsystems are up when the teardown runs. */
static const char *make_bad(int which)
{
   static const char *names[3] =
      { "bad_short.wad", "bad_magic.wad", "bad_dir.wad" };
   unsigned char buf[16];
   FILE *o;

   if (which < 0 || which > 2)
      return NULL;
   o = fopen(names[which], "wb");
   if (!o)
      return NULL;

   switch (which)
   {
      case 0:   /* shorter than a header: "couldn't read WAD header" */
         fwrite("XX", 1, 2, o);
         break;
      case 1:   /* full header, magic neither IWAD nor PWAD */
         memset(buf, 0, sizeof(buf));
         memcpy(buf, "JUNK", 4);
         fwrite(buf, 1, 12, o);
         break;
      case 2:   /* PWAD magic, lump directory pointing past EOF: the core
                 * steers it alongside the real IWAD and fails inside
                 * D_DoomMainSetup, with most of init already done */
         memset(buf, 0, sizeof(buf));
         memcpy(buf, "PWAD", 4);
         buf[4] = 0x10;                       /* numlumps    = 16 */
         buf[8] = 0x00; buf[9] = 0x10;        /* infotable   = 4096 */
         fwrite(buf, 1, 12, o);
         break;
   }
   fclose(o);
   return names[which];
}

/* --- node-lump test content ------------------------------------------
 *
 * The extended-node parsers only run when a level is built, and nothing
 * the harness can do makes the core enter one: autostart needs -warp /
 * -skill / -episode and the core stages none of them.  What does enter a
 * level is the title screen's own demo sequence, so these PWADs replace
 * the map DEMO1 plays with a minimal square room whose NODES lump is a
 * ZDBSP XNOD image.  The map is read out of the demo header rather than
 * assumed: DEMO1 in a given IWAD need not be ExM1 (in Freedoom it is
 * E1M6), and a lane that replaced the wrong map would pass while
 * exercising nothing.
 */
static int demo1_episode = 0, demo1_map = 0;

static void put32(unsigned char *p, unsigned long v)
{
   p[0] = (unsigned char)(v);       p[1] = (unsigned char)(v >> 8);
   p[2] = (unsigned char)(v >> 16); p[3] = (unsigned char)(v >> 24);
}
static void put16(unsigned char *p, unsigned v)
{
   p[0] = (unsigned char)(v); p[1] = (unsigned char)(v >> 8);
}

/* Read DEMO1's episode/map so the PWAD replaces the level that actually
 * gets played.  Returns 0 if the iwad has no DEMO1. */
static int find_demo1_map(const char *iwad)
{
   FILE *f = fopen(iwad, "rb");
   unsigned char hdr[16], *dir;
   unsigned long numlumps, infotable, i;
   int found = 0;

   if (!f)
      return 0;
   if (fread(hdr, 1, 12, f) != 12) { fclose(f); return 0; }
   numlumps  = hdr[4] | (hdr[5]<<8) | ((unsigned long)hdr[6]<<16) | ((unsigned long)hdr[7]<<24);
   infotable = hdr[8] | (hdr[9]<<8) | ((unsigned long)hdr[10]<<16) | ((unsigned long)hdr[11]<<24);
   dir = (unsigned char*)malloc(numlumps * 16);
   if (!dir) { fclose(f); return 0; }
   fseek(f, (long)infotable, SEEK_SET);
   if (fread(dir, 16, numlumps, f) != numlumps) { free(dir); fclose(f); return 0; }

   for (i = 0; i < numlumps && !found; i++)
   {
      unsigned char *e = dir + i * 16;
      unsigned long pos;
      unsigned char db[8];

      if (memcmp(e + 8, "DEMO1", 5) != 0)
         continue;
      pos = e[0] | (e[1]<<8) | ((unsigned long)e[2]<<16) | ((unsigned long)e[3]<<24);
      fseek(f, (long)pos, SEEK_SET);
      if (fread(db, 1, 8, f) == 8)
      {
         demo1_episode = db[2];
         demo1_map     = db[3];
         found = 1;
      }
   }
   free(dir);
   fclose(f);
   return found;
}

/* One square room, four linedefs, one sector, plus a NODES lump built to
 * order.  which selects the defect: 0 none, 1 truncated mid-record,
 * 2 a subsector count the lump cannot hold, 3 a seg naming a linedef
 * that does not exist, 4 a seg naming a vertex that does not exist. */
static const char *make_node_wad(int which)
{
   static const char *names[9] =
      { "nodes_good.wad", "nodes_trunc.wad", "nodes_count.wad",
        "nodes_line.wad", "nodes_vert.wad", "nodes_child.wad",
        "nodes_blockmap.wad", "nodes_glunsup.wad", "nodes_classic.wad" };
   static const short vx[4][2] = { {0,0}, {256,0}, {256,256}, {0,256} };
   static const int   sg[4][4] = { {0,1,0,0}, {1,2,1,0}, {2,3,2,0}, {3,0,3,0} };
   unsigned char nodes[256], map_marker[9];
   unsigned char verts[16], lines[4*14], sides[4*30], sectors[26], things[10];
   unsigned char reject[1], block[24];
   unsigned char cseg[4*12], cssec[2*4], cnode[28];
   int blocklen = 0, cseglen = 0, csseclen = 0, cnodelen = 0;
   int nlen = 0, i;
   FILE *o;

   if (which < 0 || which > 8)
      return NULL;

   /* geometry */
   for (i = 0; i < 4; i++)
   {
      put16(verts + i*4,     (unsigned)vx[i][0]);
      put16(verts + i*4 + 2, (unsigned)vx[i][1]);
   }
   memset(sides, 0, sizeof(sides));
   for (i = 0; i < 4; i++)
   {
      memset(sides + i*30 + 4,  '-', 1);
      memset(sides + i*30 + 12, '-', 1);
      memcpy(sides + i*30 + 20, "STARTAN2", 8);
   }
   memset(lines, 0, sizeof(lines));
   for (i = 0; i < 4; i++)
   {
      put16(lines + i*14,      (unsigned)i);
      put16(lines + i*14 + 2,  (unsigned)((i+1) & 3));
      put16(lines + i*14 + 4,  1);            /* impassable */
      put16(lines + i*14 + 10, (unsigned)i);  /* front sidedef */
      put16(lines + i*14 + 12, 0xFFFF);       /* no back side */
   }
   memset(sectors, 0, sizeof(sectors));
   put16(sectors,     0);
   put16(sectors + 2, 128);
   memcpy(sectors + 4,  "FLOOR4_8", 8);
   memcpy(sectors + 12, "CEIL3_5 ", 8);
   put16(sectors + 20, 160);
   memset(things, 0, sizeof(things));
   put16(things,     128); put16(things + 2, 128);
   put16(things + 4, 90);  put16(things + 6, 1); put16(things + 8, 7);
   /* BLOCKMAP is left empty on purpose: P_LoadBlockMap rebuilds any lump
    * shorter than 8 bytes, which gives the map a correct one.  Writing a
    * minimal blockmap by hand instead gives P_BlockLinesIterator
    * something to walk off the end of, and the crash that produces has
    * nothing to do with what this lane is testing. */
   reject[0] = 0;

   /* Variant 6 supplies a blockmap instead: big enough that
    * P_LoadBlockMap believes it rather than rebuilding, with a cell
    * offset pointing past the end of the lump and no terminator
    * anywhere.  P_BlockLinesIterator walks from that offset. */
   if (which == 6)
   {
      put16(block + 0, 0);      put16(block + 2, 0);      /* origin */
      put16(block + 4, 2);      put16(block + 6, 2);      /* 2x2 cells */
      put16(block + 8,  8);     put16(block + 10, 9);
      put16(block + 12, 10);    put16(block + 14, 0x7FFF);/* past the end */
      put16(block + 16, 0);     put16(block + 18, 0);
      put16(block + 20, 0);     put16(block + 22, 0);     /* no -1 */
      blocklen = 24;
   }

   /* NODES: XNOD image */
   memcpy(nodes, "XNOD", 4);                         nlen = 4;
   put32(nodes + nlen, 4);  nlen += 4;               /* original vertices */
   put32(nodes + nlen, 0);  nlen += 4;               /* new vertices */
   put32(nodes + nlen, which == 2 ? 0x0FFFFFFFUL : 2); nlen += 4;  /* subsectors */
   put32(nodes + nlen, 2);  nlen += 4;
   put32(nodes + nlen, 2);  nlen += 4;
   put32(nodes + nlen, 4);  nlen += 4;               /* segs */
   for (i = 0; i < 4; i++)
   {
      unsigned long v1 = (unsigned long)sg[i][0];
      unsigned long ld = (unsigned long)sg[i][2];
      if (which == 3 && i == 0) ld = 9999;
      if (which == 4 && i == 0) v1 = 99999;
      put32(nodes + nlen, v1); nlen += 4;
      put32(nodes + nlen, (unsigned long)sg[i][1]); nlen += 4;
      put16(nodes + nlen, (unsigned)ld); nlen += 2;
      nodes[nlen++] = (unsigned char)sg[i][3];
   }
   put32(nodes + nlen, 1); nlen += 4;                /* one node */
   put16(nodes + nlen, 128);  nlen += 2;
   put16(nodes + nlen, 0);    nlen += 2;
   put16(nodes + nlen, 0);    nlen += 2;
   put16(nodes + nlen, 256);  nlen += 2;
   for (i = 0; i < 8; i++) { put16(nodes + nlen, 256); nlen += 2; }
   put32(nodes + nlen, 0x80000000UL); nlen += 4;
   /* Variant 5 names a subsector that does not exist.  R_PointInSubsector
    * walks these before anything else touches the level. */
   put32(nodes + nlen, which == 5 ? 0x8000BEEFUL : 0x80000001UL); nlen += 4;
   if (which == 1)
      nlen = 30;                                     /* cut mid-record */

   /* Variant 7 carries ZDBSP GL nodes, whose signature lives in SSECTORS
    * rather than NODES.  Binary maps with those are not supported, and
    * the point is that saying so has to decline the level: the classic
    * loaders would otherwise read this image as vanilla subsectors. */
   if (which == 7)
   {
      memcpy(nodes, "XGLN", 4);                      nlen = 4;
      put32(nodes + nlen, 4);  nlen += 4;
      put32(nodes + nlen, 0);  nlen += 4;
      put32(nodes + nlen, 1);  nlen += 4;
      put32(nodes + nlen, 4);  nlen += 4;
      put32(nodes + nlen, 4);  nlen += 4;
      for (i = 0; i < 4; i++)
      {
         put32(nodes + nlen, (unsigned long)sg[i][0]); nlen += 4;
         put32(nodes + nlen, 0xFFFFFFFFUL); nlen += 4;
         put16(nodes + nlen, (unsigned)sg[i][2]); nlen += 2;
         nodes[nlen++] = (unsigned char)sg[i][3];
      }
      put32(nodes + nlen, 1); nlen += 4;
      put16(nodes + nlen, 128); nlen += 2;
      put16(nodes + nlen, 0);   nlen += 2;
      put16(nodes + nlen, 0);   nlen += 2;
      put16(nodes + nlen, 256); nlen += 2;
      for (i = 0; i < 8; i++) { put16(nodes + nlen, 256); nlen += 2; }
      put32(nodes + nlen, 0x80000000UL); nlen += 4;
      put32(nodes + nlen, 0x80000000UL); nlen += 4;
   }

   /* Variant 8 uses the vanilla SEGS / SSECTORS / NODES lumps instead of
    * an extended-node image, so the classic loaders run.  Its first seg
    * names a linedef that is not in the map -- the same defect variant 3
    * puts in an XNOD seg, on the path every ordinary map takes. */
   if (which == 8)
   {
      nlen = 0;                         /* no extended-node lump */
      for (i = 0; i < 4; i++)
      {
         unsigned ld = (i == 0) ? 9999u : (unsigned)sg[i][2];
         put16(cseg + i*12,     (unsigned)sg[i][0]);   /* v1 */
         put16(cseg + i*12 + 2, (unsigned)sg[i][1]);   /* v2 */
         put16(cseg + i*12 + 4, 0);                    /* angle */
         put16(cseg + i*12 + 6, ld);                   /* linedef */
         put16(cseg + i*12 + 8, 0);                    /* side */
         put16(cseg + i*12 + 10, 0);                   /* offset */
      }
      cseglen = 4*12;
      put16(cssec + 0, 2); put16(cssec + 2, 0);        /* 2 segs from 0 */
      put16(cssec + 4, 2); put16(cssec + 6, 2);        /* 2 segs from 2 */
      csseclen = 2*4;
      put16(cnode + 0, 128); put16(cnode + 2, 0);
      put16(cnode + 4, 0);   put16(cnode + 6, 256);
      for (i = 0; i < 8; i++) put16(cnode + 8 + i*2, 256);
      put16(cnode + 24, 0x8000); put16(cnode + 26, 0x8001);
      cnodelen = 28;
   }

   sprintf((char*)map_marker, "E%dM%d", demo1_episode, demo1_map);

   o = fopen(names[which], "wb");
   if (!o)
      return NULL;
   {
      struct { const char *name; const unsigned char *d; int len; } L[11];
      unsigned char dirent[11*16], hdr[12];
      int n = 0, off = 12, k;

      L[n].name = (const char*)map_marker; L[n].d = NULL;     L[n].len = 0;               n++;
      L[n].name = "THINGS";   L[n].d = things;  L[n].len = (int)sizeof(things);  n++;
      L[n].name = "LINEDEFS"; L[n].d = lines;   L[n].len = (int)sizeof(lines);   n++;
      L[n].name = "SIDEDEFS"; L[n].d = sides;   L[n].len = (int)sizeof(sides);   n++;
      L[n].name = "VERTEXES"; L[n].d = verts;   L[n].len = (int)sizeof(verts);   n++;
      L[n].name = "SEGS";     L[n].d = cseg;    L[n].len = cseglen;              n++;
      L[n].name = "SSECTORS"; L[n].d = (which == 7) ? nodes : cssec;
                              L[n].len  = (which == 7) ? nlen : csseclen;        n++;
      L[n].name = "NODES";    L[n].d = (which == 8) ? cnode :
                                       (which == 7) ? NULL  : nodes;
                              L[n].len  = (which == 8) ? cnodelen :
                                          (which == 7) ? 0    : nlen;            n++;
      L[n].name = "SECTORS";  L[n].d = sectors; L[n].len = (int)sizeof(sectors); n++;
      L[n].name = "REJECT";   L[n].d = reject;  L[n].len = 1;                    n++;
      L[n].name = "BLOCKMAP"; L[n].d = block;   L[n].len = blocklen;             n++;

      memset(dirent, 0, sizeof(dirent));
      for (k = 0; k < n; k++)
      {
         put32(dirent + k*16, (unsigned long)off);
         put32(dirent + k*16 + 4, (unsigned long)L[k].len);
         strncpy((char*)dirent + k*16 + 8, L[k].name, 8);
         off += L[k].len;
      }
      memcpy(hdr, "PWAD", 4);
      put32(hdr + 4, (unsigned long)n);
      put32(hdr + 8, (unsigned long)off);
      fwrite(hdr, 1, 12, o);
      for (k = 0; k < n; k++)
         if (L[k].len)
            fwrite(L[k].d, 1, (size_t)L[k].len, o);
      fwrite(dirent, 1, (size_t)n * 16, o);
   }
   fclose(o);
   return names[which];
}

static void audio_sample(int16_t l, int16_t r) { mix_sample(l, r); }

static size_t audio_batch(const int16_t *d, size_t f)
{
   size_t i;
   if (d)
      for (i = 0; i < f; i++)
         mix_sample(d[i * 2], d[i * 2 + 1]);
   return f;
}
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
   int s, i, sessions = 3, runs = 12, demo = 0, alt = 0, failmode = 0, nodesmode = 0;
   const char *altpath = NULL;
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
      fprintf(stderr, "usage: %s core.so iwad.wad [sessions] [runs] "
                      "[demo|alt|fail|nodes]\n",
            argv[0]);
      return 2;
   }
   if (argc > 3) sessions = atoi(argv[3]);
   if (argc > 4) runs     = atoi(argv[4]);
   if (argc > 5 && !strcmp(argv[5], "demo")) demo = 1;
   /* fail mode drives a rejected load between good sessions.  RetroArch
    * does not call retro_unload_game after a load returns false, so the
    * only teardown a failed load ever gets is its own, and the session
    * that follows has to come up as if it had not happened. */
   if (argc > 5 && !strcmp(argv[5], "fail")) failmode = 1;
   if (argc > 5 && !strcmp(argv[5], "nodes")) nodesmode = 1;
   /* alt alternates the iwad with a second content file, so consecutive
    * sessions build different lump tables.  argv[6] names that file; with
    * no argv[6] it is the DEMO1 lump extracted from the iwad, which
    * exercises the -playdemo path but leaves the lump numbering alone. */
   if (argc > 5 && !strcmp(argv[5], "alt"))
   {
      alt = 1;
      if (argc > 6)
         altpath = argv[6];
      else
         demo = 1;
   }
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

   if (nodesmode)
   {
      /* Drive the extended-node parser: a baseline run on the iwad alone,
       * then one run per PWAD that replaces the demo's map with a square
       * room whose NODES lump is XNOD -- sound, then one defect each.
       *
       * Two things are required of the core.  It must survive every
       * malformed lump: before the parsers were bounded these crashed or
       * hung rather than declining the level.  And the sound PWAD must
       * render differently from the iwad alone, which is what proves the
       * replacement map is reached at all -- replace a map the demo does
       * not play and every other check here passes while testing
       * nothing. */
      static const char *label[9] =
         { "sound", "truncated", "bad subsector count",
           "seg names a missing linedef", "seg names a missing vertex",
           "node child names a missing subsector",
           "blockmap offset past the end, no terminator",
           "GL nodes on a binary map (unsupported)",
           "classic seg names a missing linedef" };
      unsigned long base_hash = 0;
      int w;

      if (!find_demo1_map(argv[2]))
      {
         fprintf(stderr, "%s has no DEMO1; nodes mode needs an iwad whose "
                         "title screen plays a demo\n", argv[2]);
         return 2;
      }
      printf("DEMO1 plays E%dM%d; PWADs replace that map\n",
            demo1_episode, demo1_map);

      retro_init();

      for (w = -1; w < 9; w++)
      {
         const char *path = (w < 0) ? argv[2] : make_node_wad(w);

         if (!path)
         {
            fprintf(stderr, "could not write node test content\n");
            return 2;
         }
         agg_hash      = 2166136261UL;
         frames_total  = 0;
         frame_in_sess = 0;

         memset(&info, 0, sizeof(info));
         info.path = path;
         in_load = 1;
         if (!retro_load_game(&info))
         {
            printf("FAIL: %s did not load\n", path);
            return 1;
         }
         in_load = 0;

         for (i = 0; i < runs; i++)
            retro_run();
         retro_unload_game();

         if (w < 0)
         {
            base_hash = agg_hash;
            printf("== iwad alone            : %08lx\n", base_hash);
         }
         else
         {
            printf("== %-24s: %08lx (%s)\n", path, agg_hash, label[w]);
            if (frames_total != runs)
            {
               printf("FAIL: %s stopped producing frames\n", path);
               nodes_bad++;
            }
            if (w == 0 && agg_hash == base_hash)
            {
               printf("FAIL: %s renders exactly as the iwad alone -- the "
                      "replacement map is never reached, so nothing here "
                      "tests the node parser\n", path);
               nodes_bad++;
            }
         }
      }

      retro_deinit();
      if (nodes_bad)
         return 1;
      printf("PASS\n");
      return 0;
   }

   ref_len     = runs;
   ref_hash[0] = (unsigned long*)calloc((size_t)runs, sizeof(**ref_hash));
   ref_hash[1] = (unsigned long*)calloc((size_t)runs, sizeof(**ref_hash));
   if (!ref_hash[0] || !ref_hash[1])
      return 2;

   retro_init();

   for (s = 1; s <= sessions; s++)
   {
      session_no    = s;
      frame_in_sess = 0;
      printf("== session %d: load\n", s);
      fflush(stdout);

      /* In alt mode consecutive sessions load different files, so the
       * next session's lump table is a different wad set with different
       * numbering.  Indices a teardown failed to drop then address the
       * wrong lump.  Each content keeps its own reference frames. */
      if (failmode && s > 1)
      {
         int w;
         for (w = 0; w < 3; w++)
         {
            const char *bad = make_bad(w);
            struct retro_game_info binfo;

            if (!bad)
               continue;
            printf("== before session %d: rejecting %s\n", s, bad);
            fflush(stdout);

            memset(&binfo, 0, sizeof(binfo));
            binfo.path = bad;

            in_load = 1;
            if (retro_load_game(&binfo))
            {
               printf("FAIL: core accepted malformed content %s\n", bad);
               accepted_bad++;
               retro_unload_game();
            }
            in_load = 0;
         }
      }

      content_idx = alt ? ((s - 1) & 1) : 0;
      snd_hash    = 2166136261UL;
      snd_frames  = 0;
      memset(&info, 0, sizeof(info));
      info.path = alt ? (content_idx ? (altpath ? altpath : content) : argv[2])
                      : content;

      in_load = 1;
      if (!retro_load_game(&info))
      {
         printf("session %d: retro_load_game FAILED\n", s);
         return 1;
      }
      in_load = 0;
      /* The size is derived from the live world, so a session that
       * starts with no level has to report the same figure as the
       * first one.  A larger number here means a teardown left a list
       * head pointing into freed level memory and the estimator is
       * walking it. */
      {
         size_t ssize = retro_serialize_size();
         printf("== session %d: loaded, serialize_size %u\n",
               s, (unsigned)ssize);
         if (!ref_seen[content_idx])
            ref_ssize[content_idx] = ssize;
         else if (ssize != ref_ssize[content_idx])
         {
            printf("FAIL: session %d serialize_size %u, the first session on "
                   "this content reported %u\n",
                  s, (unsigned)ssize, (unsigned)ref_ssize[content_idx]);
            ssize_bad++;
         }
      }
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

      if (!ref_seen[content_idx])
      {
         ref_snd[content_idx]        = snd_hash;
         ref_snd_frames[content_idx] = snd_frames;
      }
      else if (snd_frames != ref_snd_frames[content_idx]
            || snd_hash   != ref_snd[content_idx])
      {
         printf("FAIL: session %d audio differs from the first session on "
                "this content (%llu frames/%08lx vs %llu/%08lx)\n",
               s, snd_frames, snd_hash,
               ref_snd_frames[content_idx], ref_snd[content_idx]);
         snd_bad++;
      }
      ref_seen[content_idx] = 1;

      printf("== session %d: audio %llu frames, hash %08lx\n",
            s, snd_frames, snd_hash);
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
   printf("frames differing from session 1: %d\n", mismatches);

   if (mismatches)
      return 1;
   if (ssize_bad)
      return 1;
   if (snd_bad)
      return 1;
   if (accepted_bad)
      return 1;
   if (nonblank < sessions)
   {
      printf("FAIL: sessions rendered nothing but blank frames\n");
      return 1;
   }
   printf("PASS\n");
   return 0;
}
