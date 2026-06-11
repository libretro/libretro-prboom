/* p_zacs.c: ZDoom ACS virtual machine.  See p_zacs.h.
 *
 * Object formats (acc / ZDoom):
 *   "ACS\0" + plain script directory                       -> ACS0
 *   "ACSE" / "ACSe" + chunk directory                       -> enhanced
 *   "ACS\0" whose dirofs-4 spells ACSE/ACSe                 -> wrapped
 *     enhanced (the dword before the marker locates the chunk
 *     directory; the ACS0 directory is a decoy of TERMINATEs)
 *
 * ACSe ("little enhanced", acc's default) packs pcodes: one byte, or
 * two when the first byte is >= 240; many operands shrink to bytes.
 * ACS0/ACSE read everything as little-endian dwords.  Word reads are
 * byte-wise: enhanced code is unaligned by construction.
 *
 * The interpreter covers the complete 386-entry ZDoom pcode set.
 * Opcodes this engine cannot honour execute as stack-disciplined
 * no-ops and report themselves once by name. */

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "doomstat.h"
#include "d_items.h"
#include "dsda_hacked.h"
#include "doomdef.h"
#include "d_player.h"
#include "info.h"
#include "lprintf.h"
#include "m_random.h"
#include "p_inter.h"
#include "p_map.h"
#include "p_mobj.h"
#include "u_decorate.h"
#include "p_setup.h"
#include "p_spec.h"
#include "p_tick.h"
#include "map_format.h"
#include "r_state.h"
#include "r_sky.h"
#include "r_main.h"
#include "s_sound.h"
#include "sounds.h"
#include "w_wad.h"
#include "z_zone.h"
#include "hexen/p_spec_hexen.h"
#include "hu_lib.h"
#include "hu_stuff.h"
#include "p_zacs.h"

/* ---- limits ------------------------------------------------------------ */

#define ZACS_STACK     200
#define ZACS_LOCALS    64        /* args + locals per frame */
#define ZACS_CALLDEPTH 16
#define ZACS_MAP_VARS  128
#define ZACS_MAX_MODULES 8       /* module 0 = map BEHAVIOR, 1.. = libraries */
#define ZACS_WORLD_VARS  256
#define ZACS_GLOBAL_VARS 64
#define ZACS_WORLD_ARRAYS  256
#define ZACS_GLOBAL_ARRAYS 64
#define ZACS_PRINTBUF  512

/* ---- pcode enum + names (generated from gzdoom's table) ---------------- */

#include "p_zacs_tab.h"

/* ---- object file state -------------------------------------------------- */

enum { ZFMT_ACS0, ZFMT_ACSE, ZFMT_ACSe };

typedef struct
{
  int   number;          /* negative for named scripts: -(SNAM index + 1) */
  int   type;            /* 0 closed, 1 OPEN, 4 ENTER, ... */
  int   argc;
  int   address;         /* byte offset into behavior data */
  int   flags;           /* SFLG */
  int   varcount;        /* SVCT (locals incl. args); default ZACS_LOCALS */
  int   module;          /* which module's data the address indexes (0 = map) */
} zacs_script_t;

typedef struct
{
  int argc;
  int locals;
  int hasreturn;
  int address;
  int module;            /* which module owns the body (0 = map; see below) */
} zacs_func_t;

static const byte   *zacs_data;      /* behavior lump bytes (PU_LEVEL copy) */
static int           zacs_len;
static int           zacs_fmt;

static zacs_script_t *zacs_scripts;
static int            zacs_numscripts;
static zacs_func_t   *zacs_funcs;
static int            zacs_numfuncs;
static char         **zacs_strings;
static int            zacs_numstrings;
static char         **zacs_snames;       /* named-script names */
static int            zacs_numsnames;
static char         **zacs_fnames;       /* FNAM: function names (for linking) */
static int            zacs_numfnames;

/* ---- VM variable storage ------------------------------------------------ */

typedef struct
{
  int *data;
  int  size;
} zacs_array_t;

static int          *zacs_mapvars;       /* -> active module's map-var store */
static zacs_array_t *zacs_maparrays;     /* per ARAY chunk entry */
static int           zacs_nummaparrays;

/* ---- ACS module table --------------------------------------------------- */
/* A ZDoom map BEHAVIOR may #import library objects named by its LOAD chunk
 * (e.g. ZDCMP2's BEHAVIOR imports the acs/zdcmp2.o library).  Each object is
 * loaded as a separate module; module 0 is always the map.  At a cross-module
 * function call the interpreter's "active view" globals above are repointed at
 * the callee's module for the duration of the (synchronous) call, then
 * restored.  Maps with no LOAD chunk produce exactly one module and the active
 * view never changes -- behaviour identical to the single-module path. */
typedef struct
{
  const byte   *data;
  int           len;
  int           fmt;
  zacs_func_t  *funcs;       int numfuncs;
  char        **strings;     int numstrings;
  char        **fnames;      int numfnames;
  zacs_array_t *maparrays;   int nummaparrays;
  zacs_script_t *scripts;    int numscripts;   /* this module's SPTR table */
  char        **snames;      int numsnames;    /* this module's SNAM names */
  int           mapvars[ZACS_MAP_VARS];
} zacs_module_t;

static zacs_module_t zacs_modules[ZACS_MAX_MODULES];
static int           zacs_nummodules;
static int           zacs_active;        /* index of the active module */
static char         *zacs_load_list[ZACS_MAX_MODULES]; /* lib names from LOAD */
static int           zacs_load_count;

/* Global ACS libraries named by a root LOADACS lump (ZDoom).  Unlike a
 * map BEHAVIOR's LOAD chunk -- which lists imports for that one map --
 * LOADACS registers libraries that every map's scripts can call.  Parsed
 * once at startup; merged into each map's module set in Z_ACSLoadBehavior
 * so their exported functions are linkable and their OPEN scripts run. */
#define ZACS_MAX_GLOBAL_LIBS 16
static char  zacs_global_libs[ZACS_MAX_GLOBAL_LIBS][9];
static int   zacs_num_global_libs;

/* world / global state persists across maps within a session */
static int          zacs_worldvars[ZACS_WORLD_VARS];
static int          zacs_globalvars[ZACS_GLOBAL_VARS];
static zacs_array_t zacs_worldarrays[ZACS_WORLD_ARRAYS];
static zacs_array_t zacs_globalarrays[ZACS_GLOBAL_ARRAYS];

/* growable sparse-ish array access: zero default, grow on write */
static int zacs_arr_get(zacs_array_t *a, int ix)
{
  if (ix < 0 || ix >= a->size)
    return 0;
  return a->data[ix];
}

static void zacs_arr_set(zacs_array_t *a, int ix, int v)
{
  if (ix < 0 || ix > 0xFFFF)            /* sanity cap: 64K elements */
    return;
  if (ix >= a->size)
  {
    int ns = a->size ? a->size : 64;
    while (ix >= ns)
      ns *= 2;
    a->data = realloc(a->data, ns * sizeof(int));
    memset(a->data + a->size, 0, (ns - a->size) * sizeof(int));
    a->size = ns;
  }
  a->data[ix] = v;
}

/* ---- script instances (thinkers) ---------------------------------------- */

enum
{
  ZSTATE_RUNNING,
  ZSTATE_DELAYED,        /* statedata = tics remaining */
  ZSTATE_TAGWAIT,        /* statedata = sector tag */
  ZSTATE_POLYWAIT,       /* statedata = polyobj number */
  ZSTATE_SCRIPTWAIT,     /* statedata = script number */
  ZSTATE_SUSPENDED,
  ZSTATE_DONE
};

typedef struct zacs_frame_s
{
  int   return_ip;
  int   locals[ZACS_LOCALS];
  dbool discard;          /* PCD_CALLDISCARD: drop the return value */
  int   caller_module;    /* module active before this call (restore on RET) */
} zacs_frame_t;

typedef struct zacs_inst_s
{
  thinker_t  thinker;
  int        script;       /* script number */
  int        info;         /* index into zacs_scripts */
  int        module;       /* module whose data ip indexes (0 = map) */
  int        ip;           /* byte offset */
  int        state;
  int        statedata;
  mobj_t    *activator;
  line_t    *line;
  int        side;
  int        stack[ZACS_STACK];
  int        sp;
  int        locals[ZACS_LOCALS];
  zacs_frame_t frames[ZACS_CALLDEPTH];
  int        fp;
  int        optstart;     /* hudmessage parameter marker, -1 none */
  char       printbuf[ZACS_PRINTBUF];
  int        printlen;
} zacs_inst_t;

/* per-script activity flags, parallel to zacs_scripts */
static byte *zacs_running;

/* warn-once bitmap for unimplemented opcodes / callfuncs */
static byte zacs_warned[(ZACS_NUM_PCODES + 7) / 8];
static byte zacs_cf_warned[64];          /* callfunc ids 0..511 */

static void zacs_warn_pcd(int pcd)
{
  if (zacs_warned[pcd >> 3] & (1 << (pcd & 7)))
    return;
  zacs_warned[pcd >> 3] |= (byte)(1 << (pcd & 7));
  lprintf(LO_WARN, "ZACS: pcode %s not supported by this engine (no-op)\n",
          zacs_pcode_names[pcd]);
}

/* ---- little-endian unaligned reads -------------------------------------- */

static int zacs_rd32(int ofs)
{
  if (ofs < 0 || ofs + 4 > zacs_len)
    return 0;
  return (int)((unsigned)zacs_data[ofs] |
               ((unsigned)zacs_data[ofs + 1] << 8) |
               ((unsigned)zacs_data[ofs + 2] << 16) |
               ((unsigned)zacs_data[ofs + 3] << 24));
}

static int zacs_rd16(int ofs)
{
  if (ofs < 0 || ofs + 2 > zacs_len)
    return 0;
  return (short)((unsigned)zacs_data[ofs] |
                 ((unsigned)zacs_data[ofs + 1] << 8));
}

static int zacs_rd8(int ofs)
{
  if (ofs < 0 || ofs >= zacs_len)
    return 0;
  return zacs_data[ofs];
}

/* ---- chunk walking ------------------------------------------------------ */

static int zacs_find_chunk(int start, int end, const char *tag,
                           int *out_ofs, int *out_len)
{
  int p = start;
  while (p + 8 <= end)
  {
    int size = zacs_rd32(p + 4);
    if (size < 0 || p + 8 + size > end)
      return 0;
    if (!memcmp(zacs_data + p, tag, 4))
    {
      *out_ofs = p + 8;
      *out_len = size;
      return 1;
    }
    p += 8 + size;
  }
  return 0;
}

static void zacs_load_strings(int co, int cl, dbool encrypted)
{
  /* STRL/STRE layout: pad dword, count, pad dword, count offsets
   * (relative to the chunk data start), then the string bytes. */
  int count = zacs_rd32(co + 4);
  int i;

  if (count <= 0 || count > 65536)
    return;
  zacs_numstrings = count;
  zacs_strings = Z_Calloc(count, sizeof(char *), PU_LEVEL, 0);
  for (i = 0; i < count; i++)
  {
    int sofs = co + zacs_rd32(co + 12 + 4 * i);
    int maxn = zacs_len - sofs;
    int n = 0;
    char *s;
    if (sofs < co || maxn <= 0)
    {
      zacs_strings[i] = (char *)"";
      continue;
    }
    if (encrypted)
    {
      /* STRE: bytes XORed with (position + relative-offset/2 + key)
       * pattern; key = offset * 157135 -- mirror gzdoom */
      int key = (zacs_rd32(co + 12 + 4 * i)) * 157135;
      while (n < maxn)
      {
        int c = zacs_data[sofs + n] ^ (key + n / 2);
        if (!(c & 0xFF))
          break;
        n++;
      }
      s = Z_Malloc(n + 1, PU_LEVEL, 0);
      {
        int k;
        for (k = 0; k < n; k++)
          s[k] = (char)(zacs_data[sofs + k] ^ (key + k / 2));
      }
      s[n] = 0;
    }
    else
    {
      while (n < maxn && zacs_data[sofs + n])
        n++;
      s = Z_Malloc(n + 1, PU_LEVEL, 0);
      memcpy(s, zacs_data + sofs, n);
      s[n] = 0;
    }
    zacs_strings[i] = s;
  }
}

static const char *zacs_string(int i)
{
  i &= 0xFFFF;                      /* strip library tag bits */
  if (i < 0 || i >= zacs_numstrings || !zacs_strings[i])
    return "";
  return zacs_strings[i];
}

static dbool zacs_load_enhanced(int chunk_start, int chunk_end)
{
  int co, cl, i;

  /* SPTR: scripts.  ACSe packs entries in 8 bytes, ACSE in 12. */
  if (!zacs_find_chunk(chunk_start, chunk_end, "SPTR", &co, &cl))
  {
    lprintf(LO_WARN, "ZACS: enhanced object with no SPTR chunk\n");
    return false;
  }
  if (zacs_fmt == ZFMT_ACSe)
  {
    zacs_numscripts = cl / 8;
    zacs_scripts = Z_Calloc(zacs_numscripts, sizeof(*zacs_scripts),
                            PU_LEVEL, 0);
    for (i = 0; i < zacs_numscripts; i++)
    {
      zacs_scripts[i].number  = zacs_rd16(co + 8 * i);
      zacs_scripts[i].type    = zacs_rd8(co + 8 * i + 2);
      zacs_scripts[i].argc    = zacs_rd8(co + 8 * i + 3);
      zacs_scripts[i].address = zacs_rd32(co + 8 * i + 4);
      zacs_scripts[i].varcount = ZACS_LOCALS;
    }
  }
  else
  {
    zacs_numscripts = cl / 12;
    zacs_scripts = Z_Calloc(zacs_numscripts, sizeof(*zacs_scripts),
                            PU_LEVEL, 0);
    for (i = 0; i < zacs_numscripts; i++)
    {
      zacs_scripts[i].number  = zacs_rd16(co + 12 * i);
      zacs_scripts[i].type    = zacs_rd16(co + 12 * i + 2);
      zacs_scripts[i].address = zacs_rd32(co + 12 * i + 4);
      zacs_scripts[i].argc    = zacs_rd32(co + 12 * i + 8);
      zacs_scripts[i].varcount = ZACS_LOCALS;
    }
  }

  /* SFLG: per-script flags */
  if (zacs_find_chunk(chunk_start, chunk_end, "SFLG", &co, &cl))
    for (i = 0; i + 4 <= cl; i += 4)
    {
      int num = zacs_rd16(co + i), j;
      int flg = zacs_rd16(co + i + 2);
      for (j = 0; j < zacs_numscripts; j++)
        if (zacs_scripts[j].number == num)
          zacs_scripts[j].flags = flg;
    }

  /* SVCT: per-script local-variable counts */
  if (zacs_find_chunk(chunk_start, chunk_end, "SVCT", &co, &cl))
    for (i = 0; i + 4 <= cl; i += 4)
    {
      int num = zacs_rd16(co + i), j;
      int cnt = zacs_rd16(co + i + 2);
      if (cnt > ZACS_LOCALS)
      {
        lprintf(LO_WARN, "ZACS: script %d wants %d locals (cap %d)\n",
                num, cnt, ZACS_LOCALS);
        cnt = ZACS_LOCALS;
      }
      for (j = 0; j < zacs_numscripts; j++)
        if (zacs_scripts[j].number == num)
          zacs_scripts[j].varcount = cnt;
    }

  /* strings */
  if (zacs_find_chunk(chunk_start, chunk_end, "STRL", &co, &cl))
    zacs_load_strings(co, cl, false);
  else if (zacs_find_chunk(chunk_start, chunk_end, "STRE", &co, &cl))
    zacs_load_strings(co, cl, true);

  /* MINI: map-variable initializers */
  if (zacs_find_chunk(chunk_start, chunk_end, "MINI", &co, &cl))
  {
    int first = zacs_rd32(co);
    int count = (cl - 4) / 4;
    for (i = 0; i < count; i++)
      if (first + i >= 0 && first + i < ZACS_MAP_VARS)
        zacs_mapvars[first + i] = zacs_rd32(co + 4 + 4 * i);
  }

  /* ARAY: map arrays (number, size pairs); AINI initializers */
  if (zacs_find_chunk(chunk_start, chunk_end, "ARAY", &co, &cl))
  {
    zacs_nummaparrays = 0;
    for (i = 0; i + 8 <= cl; i += 8)
    {
      int num = zacs_rd32(co + i);
      if (num + 1 > zacs_nummaparrays)
        zacs_nummaparrays = num + 1;
    }
    if (zacs_nummaparrays > 0 && zacs_nummaparrays <= 4096)
    {
      zacs_maparrays = Z_Calloc(zacs_nummaparrays, sizeof(zacs_array_t),
                                PU_LEVEL, 0);
      for (i = 0; i + 8 <= cl; i += 8)
      {
        int num  = zacs_rd32(co + i);
        int size = zacs_rd32(co + i + 4);
        if (num >= 0 && num < zacs_nummaparrays && size > 0 && size <= 0x10000)
          zacs_arr_set(&zacs_maparrays[num], size - 1, 0);
      }
    }
    else
      zacs_nummaparrays = 0;
  }
  {
    int p = chunk_start;
    while (zacs_maparrays && p + 8 <= chunk_end)
    {
      int size = zacs_rd32(p + 4);
      if (size < 0 || p + 8 + size > chunk_end)
        break;
      if (!memcmp(zacs_data + p, "AINI", 4) && size >= 4)
      {
        int num = zacs_rd32(p + 8);
        int count = (size - 4) / 4;
        if (num >= 0 && num < zacs_nummaparrays)
          for (i = 0; i < count; i++)
            zacs_arr_set(&zacs_maparrays[num], i, zacs_rd32(p + 12 + 4 * i));
      }
      p += 8 + size;
    }
  }

  /* FUNC: callable functions */
  if (zacs_find_chunk(chunk_start, chunk_end, "FUNC", &co, &cl))
  {
    zacs_numfuncs = cl / 8;
    zacs_funcs = Z_Calloc(zacs_numfuncs, sizeof(*zacs_funcs), PU_LEVEL, 0);
    for (i = 0; i < zacs_numfuncs; i++)
    {
      zacs_funcs[i].argc      = zacs_rd8(co + 8 * i);
      zacs_funcs[i].locals    = zacs_rd8(co + 8 * i + 1);
      zacs_funcs[i].hasreturn = zacs_rd8(co + 8 * i + 2);
      zacs_funcs[i].address   = zacs_rd32(co + 8 * i + 4);
    }
  }

  /* SNAM: named scripts (referenced by ACS_NamedExecute etc.) */
  if (zacs_find_chunk(chunk_start, chunk_end, "SNAM", &co, &cl))
  {
    int count = zacs_rd32(co);
    if (count > 0 && count <= 4096)
    {
      zacs_numsnames = count;
      zacs_snames = Z_Calloc(count, sizeof(char *), PU_LEVEL, 0);
      for (i = 0; i < count; i++)
      {
        int sofs = co + zacs_rd32(co + 4 + 4 * i);
        int maxn = zacs_len - sofs, n = 0;
        char *s;
        if (sofs < co || maxn <= 0)
        {
          zacs_snames[i] = (char *)"";
          continue;
        }
        while (n < maxn && zacs_data[sofs + n])
          n++;
        s = Z_Malloc(n + 1, PU_LEVEL, 0);
        memcpy(s, zacs_data + sofs, n);
        s[n] = 0;
        zacs_snames[i] = s;
        /* named scripts carry number -(i+1) in SPTR */
      }
    }
  }

  /* FNAM: function names, parallel to FUNC (needed to link library imports) */
  if (zacs_find_chunk(chunk_start, chunk_end, "FNAM", &co, &cl))
  {
    int count = zacs_rd32(co);
    if (count > 0 && count <= 4096)
    {
      zacs_numfnames = count;
      zacs_fnames = Z_Calloc(count, sizeof(char *), PU_LEVEL, 0);
      for (i = 0; i < count; i++)
      {
        int sofs = co + zacs_rd32(co + 4 + 4 * i);
        int maxn = zacs_len - sofs, n = 0;
        char *s;
        if (sofs < co || maxn <= 0)
        {
          zacs_fnames[i] = (char *)"";
          continue;
        }
        while (n < maxn && zacs_data[sofs + n])
          n++;
        s = Z_Malloc(n + 1, PU_LEVEL, 0);
        memcpy(s, zacs_data + sofs, n);
        s[n] = 0;
        zacs_fnames[i] = s;
      }
    }
  }

  /* LOAD: names of library objects to import (resolved by the caller).  The
   * chunk is a sequence of NUL-terminated names. */
  if (zacs_find_chunk(chunk_start, chunk_end, "LOAD", &co, &cl) && cl > 0)
  {
    int p = 0;
    while (p < cl && zacs_load_count < ZACS_MAX_MODULES)
    {
      const char *nm = (const char *)zacs_data + co + p;
      int nl = 0;
      while (p + nl < cl && nm[nl])
        nl++;
      if (nl > 0)
      {
        char *s = Z_Malloc(nl + 1, PU_LEVEL, 0);
        memcpy(s, nm, nl);
        s[nl] = 0;
        zacs_load_list[zacs_load_count++] = s;
      }
      p += nl + 1;
    }
  }

  return zacs_numscripts > 0;
}

/* Load a single ACS object (map BEHAVIOR or a library) into the active-view
 * globals.  The caller is responsible for pointing zacs_mapvars at the target
 * module's store (and zeroing it) beforehand. */
static dbool zacs_load_one(int lump)
{
  int len, dirofs;
  const byte *raw;

  zacs_data = NULL;
  zacs_len = 0;
  zacs_fmt = ZFMT_ACS0;
  zacs_scripts = NULL;
  zacs_numscripts = 0;
  zacs_funcs = NULL;
  zacs_numfuncs = 0;
  zacs_strings = NULL;
  zacs_numstrings = 0;
  zacs_snames = NULL;
  zacs_numsnames = 0;
  zacs_fnames = NULL;
  zacs_numfnames = 0;
  zacs_maparrays = NULL;
  zacs_nummaparrays = 0;

  if (lump < 0)
    return false;
  len = W_LumpLength(lump);
  if (len < 12)
    return false;

  raw = W_CacheLumpNum(lump);
  {
    byte *copy = Z_Malloc(len, PU_LEVEL, 0);
    memcpy(copy, raw, len);
    zacs_data = copy;
  }
  W_UnlockLumpNum(lump);
  zacs_len = len;

  if (!memcmp(zacs_data, "ACSE", 4) || !memcmp(zacs_data, "ACSe", 4))
  {
    zacs_fmt = (zacs_data[3] == 'e') ? ZFMT_ACSe : ZFMT_ACSE;
    dirofs = zacs_rd32(4);
    if (!zacs_load_enhanced(dirofs, zacs_len))
      return false;
  }
  else if (!memcmp(zacs_data, "ACS", 3) && zacs_data[3] == 0)
  {
    dirofs = zacs_rd32(4);
    if (dirofs >= 8 + 4 && dirofs <= zacs_len &&
        (!memcmp(zacs_data + dirofs - 4, "ACSE", 4) ||
         !memcmp(zacs_data + dirofs - 4, "ACSe", 4)))
    {
      /* ACS0-wrapped enhanced object: chunk directory location is the
       * dword before the marker; the ACS0 directory is a decoy. */
      int chunk_start = zacs_rd32(dirofs - 8);
      zacs_fmt = (zacs_data[dirofs - 1] == 'e') ? ZFMT_ACSe : ZFMT_ACSE;
      if (!zacs_load_enhanced(chunk_start, dirofs - 8))
        return false;
    }
    else
    {
      /* plain ACS0 */
      int n, i;
      zacs_fmt = ZFMT_ACS0;
      n = zacs_rd32(dirofs);
      if (n <= 0 || n > 4096)
        return false;
      zacs_numscripts = n;
      zacs_scripts = Z_Calloc(n, sizeof(*zacs_scripts), PU_LEVEL, 0);
      for (i = 0; i < n; i++)
      {
        int num = zacs_rd32(dirofs + 4 + 12 * i);
        zacs_scripts[i].number  = num % 1000;
        zacs_scripts[i].type    = num / 1000;
        zacs_scripts[i].address = zacs_rd32(dirofs + 8 + 12 * i);
        zacs_scripts[i].argc    = zacs_rd32(dirofs + 12 + 12 * i);
        zacs_scripts[i].varcount = ZACS_LOCALS;
      }
      /* ACS0 string table follows the script directory */
      {
        int sofs = dirofs + 4 + 12 * n;
        int count = zacs_rd32(sofs);
        if (count > 0 && count <= 65536)
        {
          zacs_numstrings = count;
          zacs_strings = Z_Calloc(count, sizeof(char *), PU_LEVEL, 0);
          for (i = 0; i < count; i++)
          {
            int so = zacs_rd32(sofs + 4 + 4 * i);
            int maxn = zacs_len - so, k = 0;
            char *s;
            if (so <= 0 || maxn <= 0)
            {
              zacs_strings[i] = (char *)"";
              continue;
            }
            while (k < maxn && zacs_data[so + k])
              k++;
            s = Z_Malloc(k + 1, PU_LEVEL, 0);
            memcpy(s, zacs_data + so, k);
            s[k] = 0;
            zacs_strings[i] = s;
          }
        }
      }
    }
  }
  else
    return false;

  return true;
}

/* Snapshot the just-loaded active view into module slot idx, tagging every
 * function with its owning module so library-internal calls stay local. */
static void zacs_snapshot_module(int idx)
{
  zacs_module_t *m = &zacs_modules[idx];
  int i;
  for (i = 0; i < zacs_numfuncs; i++)
    zacs_funcs[i].module = idx;
  for (i = 0; i < zacs_numscripts; i++)
    zacs_scripts[i].module = idx;
  m->data          = zacs_data;
  m->len           = zacs_len;
  m->fmt           = zacs_fmt;
  m->funcs         = zacs_funcs;
  m->numfuncs      = zacs_numfuncs;
  m->strings       = zacs_strings;
  m->numstrings    = zacs_numstrings;
  m->fnames        = zacs_fnames;
  m->numfnames     = zacs_numfnames;
  m->maparrays     = zacs_maparrays;
  m->nummaparrays  = zacs_nummaparrays;
  m->scripts       = zacs_scripts;
  m->numscripts    = zacs_numscripts;
  m->snames        = zacs_snames;
  m->numsnames     = zacs_numsnames;
  /* m->mapvars was written in place via the zacs_mapvars pointer */
}

/* Repoint the interpreter's active view at module idx. */
static void zacs_activate(int idx)
{
  zacs_module_t *m = &zacs_modules[idx];
  zacs_data         = m->data;
  zacs_len          = m->len;
  zacs_fmt          = m->fmt;
  zacs_funcs        = m->funcs;
  zacs_numfuncs     = m->numfuncs;
  zacs_strings      = m->strings;
  zacs_numstrings   = m->numstrings;
  zacs_maparrays    = m->maparrays;
  zacs_nummaparrays = m->nummaparrays;
  zacs_mapvars      = m->mapvars;
  zacs_active       = idx;
}

static dbool zacs_name_eq(const char *a, const char *b)
{
  while (*a && *b)
  {
    int ca = *a++, cb = *b++;
    if (ca >= 'a' && ca <= 'z') ca -= 32;
    if (cb >= 'a' && cb <= 'z') cb -= 32;
    if (ca != cb)
      return false;
  }
  return *a == *b;
}

/* Find the lump that is the compiled ACS object for a LOAD name.  Several
 * lumps can share the 8-char name (map marker, .acs source, .o object); pick
 * the one whose bytes begin with the "ACS" object signature. */
static int zacs_find_acs_object_lump(const char *name)
{
  char up[9];
  int i, k, nl = (int)strlen(name);
  if (nl > 8) nl = 8;
  for (k = 0; k < nl; k++)
  {
    char c = name[k];
    up[k] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
  }
  for (; k < 9; k++)
    up[k] = 0;
  for (i = numlumps - 1; i >= 0; i--)
  {
    if (strncmp(lumpinfo[i].name, up, 8) != 0)
      continue;
    if (W_LumpLength(i) >= 4)
    {
      const byte *d = W_CacheLumpNum(i);
      dbool isacs = (d[0] == 'A' && d[1] == 'C' && d[2] == 'S');
      W_UnlockLumpNum(i);
      if (isacs)
        return i;
    }
  }
  return -1;
}

/* Resolve module-0 function imports (address 0) against library exports. */
static void zacs_link_imports(void)
{
  int src, i, mi, j, linked = 0, total_imports = 0;

  /* Resolve unresolved function imports (address 0) in every module against
   * the exports of every other module.  Module 0 is the map; modules 1.. are
   * libraries (map LOAD imports and global LOADACS libs).  Linking every
   * module -- not just the map -- lets libraries call into one another, which
   * global libraries commonly do (e.g. a script pack calling a libc helper). */
  for (src = 0; src < zacs_nummodules; src++)
  {
    zacs_module_t *ms = &zacs_modules[src];
    for (i = 0; i < ms->numfuncs; i++)
    {
      if (ms->funcs[i].address != 0)
        continue;
      if (i >= ms->numfnames || !ms->fnames[i] || !ms->fnames[i][0])
        continue;
      total_imports++;
      for (mi = 0; mi < zacs_nummodules; mi++)
      {
        zacs_module_t *ml;
        if (mi == src)
          continue;
        ml = &zacs_modules[mi];
        for (j = 0; j < ml->numfuncs; j++)
        {
          if (ml->funcs[j].address == 0)
            continue;
          if (j >= ml->numfnames || !ml->fnames[j])
            continue;
          if (zacs_name_eq(ms->fnames[i], ml->fnames[j]))
          {
            ms->funcs[i].address   = ml->funcs[j].address;
            ms->funcs[i].argc      = ml->funcs[j].argc;
            ms->funcs[i].locals    = ml->funcs[j].locals;
            ms->funcs[i].hasreturn = ml->funcs[j].hasreturn;
            ms->funcs[i].module    = mi;
            linked++;
            mi = zacs_nummodules;      /* break outer */
            break;
          }
        }
      }
    }
  }
  lprintf(LO_INFO, "ZACS: linked %d/%d library function import(s) across "
          "%d module(s)\n", linked, total_imports, zacs_nummodules - 1);
}

/* Parse a root LOADACS lump into the global-library name list.  Lines are
 * one library name each; '//' and ';' begin comments and blank lines are
 * skipped.  Called once at startup (after the wads are open).  Idempotent:
 * re-parsing replaces the list. */
void Z_ACSLoadGlobalLibraries(void)
{
  int lump, len, i;
  const char *buf;

  zacs_num_global_libs = 0;

  lump = (W_CheckNumForName)("LOADACS", ns_global);
  if (lump < 0)
    return;
  len = W_LumpLength(lump);
  buf = (const char *)W_CacheLumpNum(lump);
  if (!buf || len <= 0)
    return;

  i = 0;
  while (i < len && zacs_num_global_libs < ZACS_MAX_GLOBAL_LIBS)
  {
    int n = 0;
    char name[9];

    /* skip whitespace and line breaks */
    while (i < len && (buf[i] == ' ' || buf[i] == '\t' ||
                       buf[i] == '\r' || buf[i] == '\n'))
      i++;
    if (i >= len)
      break;

    /* comment line: ';' or '//' to end of line */
    if (buf[i] == ';' ||
        (buf[i] == '/' && i + 1 < len && buf[i + 1] == '/'))
    {
      while (i < len && buf[i] != '\n')
        i++;
      continue;
    }

    /* a bare library name token */
    while (i < len && (unsigned char)buf[i] > ' ' && n < 8)
    {
      char c = buf[i++];
      name[n++] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    }
    name[n] = 0;
    /* drop any trailing token characters past 8 */
    while (i < len && (unsigned char)buf[i] > ' ')
      i++;

    if (n > 0)
    {
      memcpy(zacs_global_libs[zacs_num_global_libs], name, 9);
      zacs_num_global_libs++;
    }
  }

  W_UnlockLumpNum(lump);

  if (zacs_num_global_libs)
    lprintf(LO_INFO, "Z_ACSLoadGlobalLibraries: %d global ACS librar%s\n",
            zacs_num_global_libs,
            zacs_num_global_libs == 1 ? "y" : "ies");
}

dbool Z_ACSLoadBehavior(int lump)
{
  int liblist[ZACS_MAX_MODULES];
  int nlibs = 0, i;

  memset(zacs_warned, 0, sizeof(zacs_warned));
  memset(zacs_cf_warned, 0, sizeof(zacs_cf_warned));
  zacs_running = NULL;
  zacs_nummodules = 0;
  zacs_active = 0;
  zacs_load_count = 0;

  /* ---- module 0: the map's BEHAVIOR ------------------------------------- */
  zacs_mapvars = zacs_modules[0].mapvars;
  memset(zacs_mapvars, 0, ZACS_MAP_VARS * sizeof(int));
  if (!zacs_load_one(lump))
    return false;
  zacs_snapshot_module(0);
  zacs_nummodules = 1;

  /* capture the LOAD names before loading libraries clobbers the list */
  for (i = 0; i < zacs_load_count && nlibs < ZACS_MAX_MODULES; i++)
  {
    int libl = zacs_find_acs_object_lump(zacs_load_list[i]);
    if (libl >= 0)
      liblist[nlibs++] = libl;
    else
      lprintf(LO_WARN, "ZACS: LOAD library '%s' not found\n",
              zacs_load_list[i]);
  }

  /* ---- global LOADACS libraries ----------------------------------------
   * Libraries named by a root LOADACS lump are loaded for every map, on top
   * of whatever the map's own LOAD chunk imported.  Append any not already
   * present (a map may also LOAD a global lib explicitly). */
  for (i = 0; i < zacs_num_global_libs && nlibs < ZACS_MAX_MODULES; i++)
  {
    int libl = zacs_find_acs_object_lump(zacs_global_libs[i]);
    int j, dup = 0;
    if (libl < 0)
    {
      lprintf(LO_WARN, "ZACS: LOADACS library '%s' not found\n",
              zacs_global_libs[i]);
      continue;
    }
    for (j = 0; j < nlibs; j++)
      if (liblist[j] == libl)
      {
        dup = 1;
        break;
      }
    if (!dup)
      liblist[nlibs++] = libl;
  }

  /* ---- libraries -------------------------------------------------------- */
  for (i = 0; i < nlibs && zacs_nummodules < ZACS_MAX_MODULES; i++)
  {
    int k = zacs_nummodules;
    zacs_mapvars = zacs_modules[k].mapvars;
    memset(zacs_mapvars, 0, ZACS_MAP_VARS * sizeof(int));
    if (zacs_load_one(liblist[i]))
    {
      zacs_snapshot_module(k);
      zacs_nummodules++;
    }
  }

  /* ---- activate the map and link imports -------------------------------- */
  zacs_activate(0);
  if (zacs_nummodules > 1)
    zacs_link_imports();

  /* ---- aggregate every module's scripts into one registry --------------
   * The per-module SPTR tables were snapshot into zacs_modules[].scripts
   * (each entry tagged with its module).  Map script bodies still live in
   * module 0, but a library can carry OPEN/ENTER scripts (a script
   * library's initialiser) that must run at level start; with a single
   * map-only script table they never did.  Build a combined index space
   * over all modules and point zacs_scripts/zacs_numscripts at it so the
   * open/enter pass and zacs_running[] cover library scripts too. */
  if (zacs_nummodules > 1)
  {
    int total = 0, mi, j, w = 0;
    zacs_script_t *agg;
    for (mi = 0; mi < zacs_nummodules; mi++)
      total += zacs_modules[mi].numscripts;
    agg = Z_Calloc(total > 0 ? total : 1, sizeof(*agg), PU_LEVEL, 0);
    for (mi = 0; mi < zacs_nummodules; mi++)
      for (j = 0; j < zacs_modules[mi].numscripts; j++)
        agg[w++] = zacs_modules[mi].scripts[j];
    zacs_scripts    = agg;
    zacs_numscripts = total;
  }

  zacs_running = Z_Calloc(zacs_numscripts, 1, PU_LEVEL, 0);
  lprintf(LO_INFO, "Z_ACSLoadBehavior: %d scripts, %d strings, "
          "%d functions, %d map arrays (%s)\n",
          zacs_numscripts, zacs_numstrings, zacs_numfuncs,
          zacs_nummaparrays,
          zacs_fmt == ZFMT_ACSe ? "ACSe" :
          zacs_fmt == ZFMT_ACSE ? "ACSE" : "ACS0");
  return true;
}

dbool Z_ACSActive(void)
{
  return zacs_numscripts > 0;
}

/* ======================================================================== */
/* runtime helpers                                                          */
/* ======================================================================== */

static int zacs_script_index(int number)
{
  int i;
  for (i = 0; i < zacs_numscripts; i++)
    if (zacs_scripts[i].number == number)
      return i;
  return -1;
}

/* Resolve a named script ("VNS_Execute") to an aggregate script index.
 * Named scripts carry number -(local SNAM index + 1) within their own
 * module, so the name is matched against each module's SNAM table and the
 * aggregate entry tagged with that module and number is returned.  Modules
 * are searched map-first, so a map's named script shadows a like-named
 * library one, matching ZDoom. */
static int zacs_named_index(const char *name)
{
  int m;
  if (!name || !name[0])
    return -1;
  for (m = 0; m < zacs_nummodules; m++)
  {
    int i;
    for (i = 0; i < zacs_modules[m].numsnames; i++)
      if (zacs_modules[m].snames[i] &&
          !strcasecmp(zacs_modules[m].snames[i], name))
      {
        int want = -(i + 1), j;
        for (j = 0; j < zacs_numscripts; j++)
          if (zacs_scripts[j].module == m &&
              zacs_scripts[j].number == want)
            return j;
      }
  }
  return -1;
}

/* actor class name -> mobj type via the decorate-compatible name table */
static int zacs_actor_type(const char *name)
{
  int i;
  if (!name || !*name)
    return -1;
  for (i = 0; i < num_mobj_types; i++)
    if (mobjinfo[i].actorname && !strcasecmp(mobjinfo[i].actorname, name))
      return i;
  return -1;
}

/* sound name -> sfx id; accepts both bare lump-style names and
 * "category/name" logical names by also trying the basename */
static int zacs_sfx_for_name(const char *name)
{
  int i;
  const char *base;
  if (!name || !*name)
    return -1;
  base = strrchr(name, '/');
  base = base ? base + 1 : name;
  for (i = 1; i < num_sfx; i++)
  {
    if (!S_sfx[i].name)
      continue;
    if (!strcasecmp(S_sfx[i].name, name) || !strcasecmp(S_sfx[i].name, base))
      return i;
  }
  return -1;
}

static mobj_t *zacs_aptr(zacs_inst_t *inst, int tid)
{
  if (tid == 0)
    return inst->activator;
  {
    int sp = -1;
    return P_FindMobjFromTID((short)tid, &sp);
  }
}

/* Resolve a DECORATE user-variable name on a mobj to a pointer into its
 * lazily-allocated storage, allocating on first write.  scalar names index
 * slot 0 of their span; arrays add a clamped element index.  Returns NULL if
 * the name is not declared by the actor's type or (read path) unallocated. */
static int *zacs_uservar_slot(mobj_t *mo, const char *name, int index,
                              dbool create)
{
  int base = 0, len = 0, total;
  if (!mo || !name)
    return NULL;
  if (!U_DecorateUserVarSlot(mo->type, name, &base, &len))
    return NULL;
  if (index < 0) index = 0;
  if (index >= len) index = len - 1;        /* clamp into the declared span */
  total = U_DecorateUserVarCount(mo->type);
  if (total <= 0)
    return NULL;
  if (!mo->user_vars)
  {
    if (!create)
      return NULL;
    mo->user_vars = Z_Malloc(total * sizeof(int), PU_LEVEL, NULL);
    memset(mo->user_vars, 0, total * sizeof(int));
  }
  return &mo->user_vars[base + index];
}


static dbool zacs_tag_busy(int tag)
{
  int secnum = -1;
  while ((secnum = P_FindSectorFromTag(tag, secnum)) >= 0)
    if (sectors[secnum].floordata || sectors[secnum].ceilingdata)
      return true;
  return false;
}

/* ---- doom-mapped ZDoom inventory ---------------------------------------- */

typedef struct zacs_token_s
{
  char name[32];
  int count;
  struct zacs_token_s *next;
} zacs_token_t;

static zacs_token_t *zacs_tokens[MAXPLAYERS];

static zacs_token_t *zacs_token_find(int pnum, const char *name)
{
  zacs_token_t *t;
  for (t = zacs_tokens[pnum]; t; t = t->next)
    if (!strcasecmp(t->name, name))
      return t;
  return NULL;
}

static const struct { const char *name; int key; } zacs_keymap[] =
{
  { "BlueCard", it_bluecard },   { "YellowCard", it_yellowcard },
  { "RedCard", it_redcard },     { "BlueSkull", it_blueskull },
  { "YellowSkull", it_yellowskull }, { "RedSkull", it_redskull },
};

static const struct { const char *name; int ammo; } zacs_ammomap[] =
{
  { "Clip", AM_CLIP }, { "Shell", AM_SHELL },
  { "RocketAmmo", AM_MISL }, { "Cell", AM_CELL },
};

static const struct { const char *name; int weapon; } zacs_weapmap[] =
{
  { "Fist", WP_FIST }, { "Pistol", WP_PISTOL }, { "Shotgun", WP_SHOTGUN },
  { "Chaingun", WP_CHAINGUN }, { "RocketLauncher", WP_MISSILE },
  { "PlasmaRifle", WP_PLASMA }, { "BFG9000", WP_BFG },
  { "Chainsaw", WP_CHAINSAW }, { "SuperShotgun", WP_SUPERSHOTGUN },
};

static int zacs_check_inventory(mobj_t *mo, const char *name)
{
  player_t *p;
  size_t i;
  if (!mo || !name || !*name)
    return 0;
  if (!mo->player)
    return 0;
  p = mo->player;
  for (i = 0; i < sizeof(zacs_keymap)/sizeof(zacs_keymap[0]); i++)
    if (!strcasecmp(name, zacs_keymap[i].name))
      return p->cards[zacs_keymap[i].key] ? 1 : 0;
  for (i = 0; i < sizeof(zacs_ammomap)/sizeof(zacs_ammomap[0]); i++)
    if (!strcasecmp(name, zacs_ammomap[i].name))
      return p->ammo[zacs_ammomap[i].ammo];
  for (i = 0; i < sizeof(zacs_weapmap)/sizeof(zacs_weapmap[0]); i++)
    if (!strcasecmp(name, zacs_weapmap[i].name))
      return p->weaponowned[zacs_weapmap[i].weapon] ? 1 : 0;
  if (!strcasecmp(name, "Health"))
    return p->health;
  if (!strcasecmp(name, "BasicArmor") || !strcasecmp(name, "Armor"))
    return p->armorpoints;
  if (!strcasecmp(name, "Backpack"))
    return p->backpack ? 1 : 0;
  {
    zacs_token_t *t = zacs_token_find(p - players, name);
    return t ? t->count : 0;
  }
}

static void zacs_give_inventory(mobj_t *mo, const char *name, int count)
{
  player_t *p;
  size_t i;
  if (!mo || !mo->player || !name || !*name || count <= 0)
    return;
  p = mo->player;
  for (i = 0; i < sizeof(zacs_keymap)/sizeof(zacs_keymap[0]); i++)
    if (!strcasecmp(name, zacs_keymap[i].name))
    {
      p->cards[zacs_keymap[i].key] = true;
      return;
    }
  for (i = 0; i < sizeof(zacs_ammomap)/sizeof(zacs_ammomap[0]); i++)
    if (!strcasecmp(name, zacs_ammomap[i].name))
    {
      int a = zacs_ammomap[i].ammo;
      p->ammo[a] += count;
      if (p->ammo[a] > p->maxammo[a])
        p->ammo[a] = p->maxammo[a];
      return;
    }
  for (i = 0; i < sizeof(zacs_weapmap)/sizeof(zacs_weapmap[0]); i++)
    if (!strcasecmp(name, zacs_weapmap[i].name))
    {
      p->weaponowned[zacs_weapmap[i].weapon] = true;
      return;
    }
  if (!strcasecmp(name, "Health"))
  {
    p->health += count;
    if (p->health > 100)
      p->health = 100;
    p->mo->health = p->health;
    return;
  }
  if (!strcasecmp(name, "BasicArmor") || !strcasecmp(name, "Armor"))
  {
    p->armorpoints += count;
    if (!p->armortype)
      p->armortype = 1;
    return;
  }
  if (!strcasecmp(name, "Backpack"))
  {
    if (!p->backpack)
    {
      int a;
      for (a = 0; a < NUMAMMO; a++)
        p->maxammo[a] *= 2;
      p->backpack = true;
    }
    return;
  }
  {
    int pnum = p - players;
    zacs_token_t *t = zacs_token_find(pnum, name);
    if (!t)
    {
      t = Z_Calloc(1, sizeof(*t), PU_LEVEL, 0);
      strncpy(t->name, name, sizeof(t->name) - 1);
      t->next = zacs_tokens[pnum];
      zacs_tokens[pnum] = t;
    }
    t->count += count;
  }
}

static void zacs_take_inventory(mobj_t *mo, const char *name, int count)
{
  player_t *p;
  size_t i;
  if (!mo || !mo->player || !name || !*name || count <= 0)
    return;
  p = mo->player;
  for (i = 0; i < sizeof(zacs_keymap)/sizeof(zacs_keymap[0]); i++)
    if (!strcasecmp(name, zacs_keymap[i].name))
    {
      p->cards[zacs_keymap[i].key] = false;
      return;
    }
  for (i = 0; i < sizeof(zacs_ammomap)/sizeof(zacs_ammomap[0]); i++)
    if (!strcasecmp(name, zacs_ammomap[i].name))
    {
      int a = zacs_ammomap[i].ammo;
      p->ammo[a] -= count;
      if (p->ammo[a] < 0)
        p->ammo[a] = 0;
      return;
    }
  for (i = 0; i < sizeof(zacs_weapmap)/sizeof(zacs_weapmap[0]); i++)
    if (!strcasecmp(name, zacs_weapmap[i].name))
    {
      p->weaponowned[zacs_weapmap[i].weapon] = false;
      return;
    }
  if (!strcasecmp(name, "Health"))
  {
    /* ZDoom TakeInventory("Health") reduces but never kills */
    p->health -= count;
    if (p->health < 1)
      p->health = 1;
    p->mo->health = p->health;
    return;
  }
  if (!strcasecmp(name, "BasicArmor") || !strcasecmp(name, "Armor"))
  {
    p->armorpoints -= count;
    if (p->armorpoints < 0)
      p->armorpoints = 0;
    return;
  }
  {
    zacs_token_t *t = zacs_token_find(p - players, name);
    if (t)
    {
      t->count -= count;
      if (t->count < 0)
        t->count = 0;
    }
  }
}

static void zacs_clear_inventory(mobj_t *mo)
{
  player_t *p;
  int i;
  if (!mo || !mo->player)
    return;
  p = mo->player;
  for (i = 0; i < NUMCARDS; i++)
    p->cards[i] = false;
  for (i = 0; i < NUMWEAPONS; i++)
    p->weaponowned[i] = false;
  p->weaponowned[WP_FIST] = true;
  p->weaponowned[WP_PISTOL] = true;
  for (i = 0; i < NUMAMMO; i++)
  {
    p->ammo[i] = 0;
    if (p->backpack)
      p->maxammo[i] /= 2;
  }
  p->backpack = false;
  p->ammo[AM_CLIP] = 50;
  p->armorpoints = 0;
  p->armortype = 0;
  if (p->readyweapon > WP_PISTOL)
    p->pendingweapon = WP_PISTOL;
  zacs_tokens[p - players] = NULL;     /* PU_LEVEL memory, just drop */
}

/* ---- dynamic string append (PCD_SAVESTRING) ----------------------------- */

static int zacs_strings_cap;

static int zacs_save_string(const char *s)
{
  int n = strlen(s);
  char *copy = Z_Malloc(n + 1, PU_LEVEL, 0);
  memcpy(copy, s, n + 1);
  if (zacs_numstrings >= zacs_strings_cap)
  {
    int nc = zacs_strings_cap ? zacs_strings_cap * 2 : 64;
    char **ns;
    if (nc < zacs_numstrings + 1)
      nc = zacs_numstrings + 64;
    ns = Z_Calloc(nc, sizeof(char *), PU_LEVEL, 0);
    if (zacs_strings)
      memcpy(ns, zacs_strings, zacs_numstrings * sizeof(char *));
    zacs_strings = ns;
    zacs_strings_cap = nc;
  }
  zacs_strings[zacs_numstrings] = copy;
  return zacs_numstrings++;
}

/* ---- print buffer -------------------------------------------------------- */

static void zacs_print_str(zacs_inst_t *inst, const char *s)
{
  while (*s && inst->printlen < ZACS_PRINTBUF - 1)
    inst->printbuf[inst->printlen++] = *s++;
  inst->printbuf[inst->printlen] = 0;
}

static void zacs_print_num(zacs_inst_t *inst, int v)
{
  char b[16];
  int n = 0, neg = 0, k;
  unsigned u;
  if (v < 0)
  {
    neg = 1;
    u = (unsigned)(-(v + 1)) + 1u;
  }
  else
    u = (unsigned)v;
  do { b[n++] = (char)('0' + u % 10); u /= 10; } while (u);
  if (neg && inst->printlen < ZACS_PRINTBUF - 1)
    inst->printbuf[inst->printlen++] = '-';
  for (k = n - 1; k >= 0; k--)
    if (inst->printlen < ZACS_PRINTBUF - 1)
      inst->printbuf[inst->printlen++] = b[k];
  inst->printbuf[inst->printlen] = 0;
}


/* ======================================================================== */
/* script instances                                                          */
/* ======================================================================== */

#include "hexen/po_man.h"

static void T_ZACSThinker(zacs_inst_t *inst);

static char zacs_msgpool[8][ZACS_PRINTBUF];
static int  zacs_msgrot;

/* Previous-tic button snapshot per player, for GetPlayerInput INPUT_OLDBUTTONS.
 * Rolled once per game tic by Z_ACSHudTicker. */
static int zacs_oldbuttons[MAXPLAYERS];
static int zacs_oldbuttons_snap[MAXPLAYERS];

static int zacs_oldbuttons_of(player_t *pl)
{
  int i;
  for (i = 0; i < MAXPLAYERS; i++)
    if (&players[i] == pl)
      return zacs_oldbuttons[i];
  return 0;
}

/* On-screen HudMessage store.  ZDoom's verbose HudMessage places text at a
 * point in a virtual HUD space (320x200 by default, or the SetHudSize box)
 * and holds it for a time.  We keep a small set of live messages keyed by the
 * caller's id (a fresh id 0 always adds; a non-zero id replaces the message
 * with that id, as ZDoom does) and render them from the HUD drawer. */
#define ZACS_HUDMSG_MAX 16
typedef struct {
  int   active;
  int   id;
  int   x, y;          /* virtual coords, already mapped to 320x200          */
  int   holdtics;      /* remaining tics; <0 = until replaced                 */
  int   color;         /* CR_* range index, or -1 for default                 */
  char  text[ZACS_PRINTBUF];
} zacs_hudmsg_t;
static zacs_hudmsg_t zacs_hudmsgs[ZACS_HUDMSG_MAX];
static int zacs_hud_w = 320, zacs_hud_h = 200;   /* current SetHudSize box   */

/* the status-bar font, defined in hu_stuff.c (no public extern there) */
extern patchnum_t hu_font[HU_FONTSIZE];

/* Map a 16.16 HUD coordinate in the current hud box to 320x200 virtual space. */
static int zacs_hud_map_x(int fx)
{
  int px = fx >> 16;
  if (zacs_hud_w <= 0) return px;
  return (int)((long long)px * 320 / zacs_hud_w);
}
static int zacs_hud_map_y(int fy)
{
  int py = fy >> 16;
  if (zacs_hud_h <= 0) return py;
  return (int)((long long)py * 200 / zacs_hud_h);
}

/* Register/replace a positioned, timed hud message. */
static void zacs_hud_store(const char *text, int id, int x, int y,
                           int holdtics, int color)
{
  int i, slot = -1;
  if (id != 0)
    for (i = 0; i < ZACS_HUDMSG_MAX; i++)
      if (zacs_hudmsgs[i].active && zacs_hudmsgs[i].id == id)
      { slot = i; break; }
  if (slot < 0)
    for (i = 0; i < ZACS_HUDMSG_MAX; i++)
      if (!zacs_hudmsgs[i].active) { slot = i; break; }
  if (slot < 0)
    slot = 0;                       /* all full: reuse the first */
  zacs_hudmsgs[slot].active   = 1;
  zacs_hudmsgs[slot].id       = id;
  zacs_hudmsgs[slot].x        = x;
  zacs_hudmsgs[slot].y        = y;
  zacs_hudmsgs[slot].holdtics = holdtics;
  zacs_hudmsgs[slot].color    = color;
  strncpy(zacs_hudmsgs[slot].text, text, ZACS_PRINTBUF - 1);
  zacs_hudmsgs[slot].text[ZACS_PRINTBUF - 1] = 0;
}

static void zacs_deliver(zacs_inst_t *inst, dbool bold)
{
  char *slot;
  if (!inst->printlen)
    return;
  slot = zacs_msgpool[zacs_msgrot++ & 7];
  memcpy(slot, inst->printbuf, inst->printlen + 1);
  if (bold)
  {
    int i;
    for (i = 0; i < MAXPLAYERS; i++)
      if (playeringame[i])
        players[i].message = slot;
  }
  else if (inst->activator && inst->activator->player)
    inst->activator->player->message = slot;
  else
    players[consoleplayer].message = slot;
}

static zacs_inst_t *zacs_spawn(int info, const int *args, int argc,
                               mobj_t *activator, line_t *line, int side)
{
  zacs_inst_t *inst = Z_Calloc(1, sizeof(*inst), PU_LEVEL, 0);
  int i;

  inst->script = zacs_scripts[info].number;
  inst->info = info;
  inst->module = zacs_scripts[info].module;
  inst->ip = zacs_scripts[info].address;
  inst->state = ZSTATE_RUNNING;
  inst->side = side;
  inst->line = line;
  inst->optstart = -1;
  P_SetTarget(&inst->activator, activator);
  for (i = 0; i < argc && i < zacs_scripts[info].argc && i < ZACS_LOCALS; i++)
    inst->locals[i] = args ? args[i] : 0;
  inst->thinker.function.arg1 = (void (*)(void *)) T_ZACSThinker;
  P_AddThinker(&inst->thinker);
  if (zacs_running[info] < 255)
    zacs_running[info]++;
  return inst;
}

static void zacs_finish(zacs_inst_t *inst)
{
  inst->state = ZSTATE_DONE;
  if (zacs_running[inst->info])
    zacs_running[inst->info]--;
  P_SetTarget(&inst->activator, NULL);
  P_RemoveThinker(&inst->thinker);
}

/* walk live instances of a script number */
static zacs_inst_t *zacs_find_inst(int number, zacs_inst_t *after)
{
  thinker_t *th = after ? after->thinker.next : thinkercap.next;
  for (; th != &thinkercap; th = th->next)
    if (th->function.arg1 == (void (*)(void *)) T_ZACSThinker &&
        ((zacs_inst_t *)th)->script == number &&
        ((zacs_inst_t *)th)->state != ZSTATE_DONE)
      return (zacs_inst_t *)th;
  return NULL;
}

dbool Z_ACSStart(int number, int map, const int *args, int argc,
                 mobj_t *activator, line_t *line, int side, dbool always)
{
  int info;

  if (!zacs_numscripts)
    return false;
  if (map && map != gamemap)
  {
    static dbool warned;
    if (!warned)
    {
      warned = true;
      lprintf(LO_WARN, "ZACS: cross-map ACS store not supported "
              "(script %d for map %d)\n", number, map);
    }
    return false;
  }
  info = zacs_script_index(number);
  if (info < 0)
    return false;
  if (!always && zacs_running[info])
  {
    /* resume a suspended instance */
    zacs_inst_t *inst = zacs_find_inst(number, NULL);
    if (inst && inst->state == ZSTATE_SUSPENDED)
    {
      inst->state = ZSTATE_RUNNING;
      return true;
    }
    return false;
  }
  zacs_spawn(info, args, argc, activator, line, side);
  return true;
}

/* Start (or, with `always`, force a fresh instance of) the script at a known
 * aggregate index.  Shared by the numbered and named entry points. */
static dbool zacs_start_info(int info, const int *args, int argc,
                             mobj_t *activator, line_t *line, int side,
                             dbool always)
{
  if (info < 0)
    return false;
  if (!always && zacs_running[info])
  {
    zacs_inst_t *inst = zacs_find_inst(zacs_scripts[info].number, NULL);
    if (inst && inst->state == ZSTATE_SUSPENDED)
    {
      inst->state = ZSTATE_RUNNING;
      return true;
    }
    return false;
  }
  zacs_spawn(info, args, argc, activator, line, side);
  return true;
}

/* ACS_NamedExecute / ACS_NamedExecuteAlways and friends: like Z_ACSStart but
 * the script is identified by name rather than number.  `name_index` is an
 * index into the active behavior's string table (how the line specials and
 * ACSF builtins pass the script name). */
dbool Z_ACSStartNamed(int name_index, int map, const int *args, int argc,
                      mobj_t *activator, line_t *line, int side, dbool always)
{
  int info;
  if (!zacs_numscripts || (map && map != gamemap))
    return false;
  info = zacs_named_index(zacs_string(name_index));
  return zacs_start_info(info, args, argc, activator, line, side, always);
}

/* Start a named script given the name string directly (rather than as a
 * behavior string-table index).  Used by callers outside the VM -- a
 * DECORATE actor running ACS_NamedExecuteAlways from a state action. */
dbool Z_ACSStartNamedStr(const char *name, const int *args, int argc,
                         mobj_t *activator, dbool always)
{
  int info;
  if (!zacs_numscripts)
    return false;
  info = zacs_named_index(name);
  return zacs_start_info(info, args, argc, activator, NULL, 0, always);
}

dbool Z_ACSSuspend(int number)
{
  zacs_inst_t *inst = zacs_find_inst(number, NULL);
  if (!inst || inst->state == ZSTATE_SUSPENDED)
    return false;
  inst->state = ZSTATE_SUSPENDED;
  return true;
}

dbool Z_ACSTerminate(int number)
{
  dbool any = false;
  zacs_inst_t *inst = NULL;
  while ((inst = zacs_find_inst(number, inst)) != NULL)
  {
    zacs_finish(inst);
    any = true;
  }
  return any;
}

/* Name of an aggregated script entry, or NULL for a numbered (non-named)
 * script.  Named scripts carry number -(SNAM index + 1) in their owning
 * module's name table. */
static const char *zacs_script_name(int i)
{
  int num, sidx, mod;
  if (i < 0 || i >= zacs_numscripts)
    return NULL;
  num = zacs_scripts[i].number;
  if (num >= 32768)
    num -= 65536;                 /* stored as unsigned 16-bit */
  if (num >= 0)
    return NULL;                  /* numbered script, no name */
  sidx = -num - 1;
  mod  = zacs_scripts[i].module;
  if (mod < 0 || mod >= zacs_nummodules)
    return NULL;
  if (sidx < 0 || sidx >= zacs_modules[mod].numsnames)
    return NULL;
  return zacs_modules[mod].snames[sidx];
}

/* A GDCC-compiled object carries a global-constructor script whose name ends
 * in "$init"; it lays out that object's static data and C runtime in the
 * global memory segment and must run before any other script in the object,
 * or the dependent scripts read an unset runtime and misbehave. */
static dbool zacs_is_init_script(int i)
{
  const char *nm = zacs_script_name(i);
  size_t n;
  if (!nm)
    return false;
  n = strlen(nm);
  return (n >= 5 && !strcmp(nm + n - 5, "$init"));
}

void Z_ACSRunOpenScripts(void)
{
  int i;
  /* First pass: GDCC global-constructor ("$init") OPEN scripts, which set up
   * their object's static data and C runtime before anything else runs.
   * Running a dependent OPEN script ahead of its object's constructor leaves
   * that script reading an unset runtime. */
  for (i = 0; i < zacs_numscripts; i++)
    if (zacs_scripts[i].type == 1 && zacs_is_init_script(i))
      zacs_spawn(i, NULL, 0, NULL, NULL, 0);
  /* Second pass: the remaining OPEN scripts, now that any constructors ran. */
  for (i = 0; i < zacs_numscripts; i++)
    if (zacs_scripts[i].type == 1 && !zacs_is_init_script(i))
      zacs_spawn(i, NULL, 0, NULL, NULL, 0);
}

void Z_ACSRunEnterScripts(mobj_t *playermo)
{
  int i;
  for (i = 0; i < zacs_numscripts; i++)
    if (zacs_scripts[i].type == 4)              /* ENTER */
      zacs_spawn(i, NULL, 0, playermo, NULL, 0);
}

/* Tic the live hud messages: a finite hold time counts down and clears. */
void Z_ACSHudTicker(void)
{
  int i;
  for (i = 0; i < MAXPLAYERS; i++)
  {
    zacs_oldbuttons[i] = zacs_oldbuttons_snap[i];
    zacs_oldbuttons_snap[i] = playeringame[i] ? players[i].cmd.buttons : 0;
  }
  for (i = 0; i < ZACS_HUDMSG_MAX; i++)
    if (zacs_hudmsgs[i].active && zacs_hudmsgs[i].holdtics >= 0)
      if (--zacs_hudmsgs[i].holdtics < 0)
        zacs_hudmsgs[i].active = 0;
}

/* Clear all hud messages (level start / reset). */
void Z_ACSHudClear(void)
{
  int i;
  for (i = 0; i < ZACS_HUDMSG_MAX; i++)
    zacs_hudmsgs[i].active = 0;
  zacs_hud_w = 320;
  zacs_hud_h = 200;
}

/* Draw the live hud messages.  Called from the HUD drawer; uses the small
 * status-bar font via a transient text line so colour escapes and the missing
 * -glyph fallbacks are handled exactly as the message widget's are. */
void Z_ACSHudDrawer(void)
{
  int i;
  for (i = 0; i < ZACS_HUDMSG_MAX; i++)
  {
    hu_textline_t l;
    const char *t;
    if (!zacs_hudmsgs[i].active)
      continue;
    HUlib_initTextLine(&l, zacs_hudmsgs[i].x, zacs_hudmsgs[i].y,
                       hu_font, HU_FONTSTART,
                       zacs_hudmsgs[i].color >= 0 ? zacs_hudmsgs[i].color
                                                  : CR_RED);
    for (t = zacs_hudmsgs[i].text; *t; t++)
      HUlib_addCharToTextLine(&l, *t);
    HUlib_drawTextLine(&l, false);
  }
}

/* ======================================================================== */
/* the interpreter                                                           */
/* ======================================================================== */

#define ZPUSH(v) do { if (inst->sp < ZACS_STACK) inst->stack[inst->sp++] = (v); } while (0)
#define ZPOP()   (inst->sp > 0 ? inst->stack[--inst->sp] : 0)
#define ZSTK(n)  (inst->sp >= (n) ? inst->stack[inst->sp - (n)] : 0)
#define ZSETSTK(n, v) do { if (inst->sp >= (n)) inst->stack[inst->sp - (n)] = (v); } while (0)
#define ZDROP(n) do { inst->sp = (inst->sp >= (n)) ? inst->sp - (n) : 0; } while (0)

#define NEXTWORD  (tmp = zacs_rd32(ip), ip += 4, tmp)
#define NEXTBYTE  (zacs_fmt == ZFMT_ACSe ? (tmp = zacs_rd8(ip), ip += 1, tmp) : (tmp = zacs_rd32(ip), ip += 4, tmp))
#define NEXTSHORT (zacs_fmt == ZFMT_ACSe ? (tmp = zacs_rd16(ip), ip += 2, tmp) : (tmp = zacs_rd32(ip), ip += 4, tmp))
#define RAWBYTE   (tmp = zacs_rd8(ip), ip += 1, tmp)

/* variable access (locals points at the active frame's variables) */
static int  zvar_get_script(int *locals, int i)
{ return (i >= 0 && i < ZACS_LOCALS) ? locals[i] : 0; }
static void zvar_set_script(int *locals, int i, int v)
{ if (i >= 0 && i < ZACS_LOCALS) locals[i] = v; }
static int  zvar_get_map(int *locals, int i)
{ (void)locals; return (i >= 0 && i < ZACS_MAP_VARS) ? zacs_mapvars[i] : 0; }
static void zvar_set_map(int *locals, int i, int v)
{ (void)locals; if (i >= 0 && i < ZACS_MAP_VARS) zacs_mapvars[i] = v; }
static int  zvar_get_world(int *locals, int i)
{ (void)locals; return (i >= 0 && i < ZACS_WORLD_VARS) ? zacs_worldvars[i] : 0; }
static void zvar_set_world(int *locals, int i, int v)
{ (void)locals; if (i >= 0 && i < ZACS_WORLD_VARS) zacs_worldvars[i] = v; }
static int  zvar_get_global(int *locals, int i)
{ (void)locals; return (i >= 0 && i < ZACS_GLOBAL_VARS) ? zacs_globalvars[i] : 0; }
static void zvar_set_global(int *locals, int i, int v)
{ (void)locals; if (i >= 0 && i < ZACS_GLOBAL_VARS) zacs_globalvars[i] = v; }

static int zarr_get_map(zacs_inst_t *inst, int a, int ix)
{ (void)inst; return (a >= 0 && a < zacs_nummaparrays) ? zacs_arr_get(&zacs_maparrays[a], ix) : 0; }
static void zarr_set_map(zacs_inst_t *inst, int a, int ix, int v)
{ (void)inst; if (a >= 0 && a < zacs_nummaparrays) zacs_arr_set(&zacs_maparrays[a], ix, v); }
static int zarr_get_world(zacs_inst_t *inst, int a, int ix)
{ (void)inst; return zacs_arr_get(&zacs_worldarrays[a & (ZACS_WORLD_ARRAYS-1)], ix); }
static void zarr_set_world(zacs_inst_t *inst, int a, int ix, int v)
{ (void)inst; zacs_arr_set(&zacs_worldarrays[a & (ZACS_WORLD_ARRAYS-1)], ix, v); }
static int zarr_get_global(zacs_inst_t *inst, int a, int ix)
{ (void)inst; return zacs_arr_get(&zacs_globalarrays[a & (ZACS_GLOBAL_ARRAYS-1)], ix); }
static void zarr_set_global(zacs_inst_t *inst, int a, int ix, int v)
{ (void)inst; zacs_arr_set(&zacs_globalarrays[a & (ZACS_GLOBAL_ARRAYS-1)], ix, v); }
/* script-local arrays need acc's SARY metadata; not supported yet */
static int zarr_get_script(zacs_inst_t *inst, int a, int ix)
{ (void)inst; (void)a; (void)ix; return 0; }
static void zarr_set_script(zacs_inst_t *inst, int a, int ix, int v)
{ (void)inst; (void)a; (void)ix; (void)v; }

#define ZVAR_GET(cls, i)       zvar_get_##cls(locals, i)
#define ZVAR_SET(cls, i, v)    zvar_set_##cls(locals, i, v)
#define ZARR_GET(cls, a, i)    zarr_get_##cls(inst, a, i)
#define ZARR_SET(cls, a, i, v) zarr_set_##cls(inst, a, i, v)

/* 32-bit random in [min,max] from the non-demo random source */
static int zacs_random(int min, int max)
{
  unsigned span;
  unsigned r;
  if (max < min)
  {
    int t = min; min = max; max = t;
  }
  span = (unsigned)(max - min) + 1u;
  r = ((unsigned)M_Random() << 24) ^ ((unsigned)M_Random() << 16) ^
      ((unsigned)M_Random() << 8) ^ (unsigned)M_Random();
  if (!span)
    return min + (int)r;
  return min + (int)(r % span);
}

static void zacs_dispatch_lspec(zacs_inst_t *inst, int special,
                                int *a, int *result)
{
  int r = 0;
  if (map_format.execute_line_special)
    r = map_format.execute_line_special(special, a, inst->line, inst->side,
                                        inst->activator);
  if (result)
    *result = r;
}

static void T_ZACSThinker(zacs_inst_t *inst)
{
  int ip;
  int *locals;
  int tmp;
  int budget = 500000;

  switch (inst->state)
  {
    case ZSTATE_DONE:
    case ZSTATE_SUSPENDED:
      return;
    case ZSTATE_DELAYED:
      if (--inst->statedata > 0)
        return;
      inst->state = ZSTATE_RUNNING;
      break;
    case ZSTATE_TAGWAIT:
      if (zacs_tag_busy(inst->statedata))
        return;
      inst->state = ZSTATE_RUNNING;
      break;
    case ZSTATE_POLYWAIT:
      if (PO_Busy(inst->statedata))
        return;
      inst->state = ZSTATE_RUNNING;
      break;
    case ZSTATE_SCRIPTWAIT:
    {
      int wi = zacs_script_index(inst->statedata);
      if (wi >= 0 && zacs_running[wi])
        return;
      inst->state = ZSTATE_RUNNING;
      break;
    }
    default:
      break;
  }

  /* A script body executes against its own module's data.  Map scripts are
   * module 0; a library OPEN/ENTER script runs against the module that
   * defined it.  Cross-module calls restore the caller's module before any
   * yield, so the active module equals inst->module at every instance
   * boundary -- activate it here. */
  if (zacs_nummodules > 1 && zacs_active != inst->module)
    zacs_activate(inst->module);

  ip = inst->ip;
  locals = inst->fp ? inst->frames[inst->fp - 1].locals : inst->locals;

  while (inst->state == ZSTATE_RUNNING)
  {
    int pcd;

    if (--budget < 0)
    {
      lprintf(LO_WARN, "ZACS: runaway script %d terminated\n", inst->script);
      zacs_finish(inst);
      return;
    }
    if (ip < 0 || ip >= zacs_len)
    {
      lprintf(LO_WARN, "ZACS: script %d ran off the object (ip=%d)\n",
              inst->script, ip);
      zacs_finish(inst);
      return;
    }

    if (zacs_fmt == ZFMT_ACSe)
    {
      pcd = zacs_rd8(ip++);
      if (pcd >= 240)
        pcd = 240 + ((pcd - 240) << 8) + zacs_rd8(ip++);
    }
    else
      pcd = NEXTWORD;

    if (pcd < 0 || pcd >= ZACS_NUM_PCODES)
    {
      lprintf(LO_WARN, "ZACS: script %d hit invalid pcode %d\n",
              inst->script, pcd);
      zacs_finish(inst);
      return;
    }

    switch (pcd)
    {
    case PCD_NOP:
      break;
    case PCD_TERMINATE:
      zacs_finish(inst);
      return;
    case PCD_SUSPEND:
      inst->state = ZSTATE_SUSPENDED;
      break;
    case PCD_PUSHNUMBER:
      ZPUSH(NEXTWORD);
      break;
    case PCD_PUSHBYTE:
      ZPUSH(RAWBYTE);
      break;
    case PCD_PUSH2BYTES:
      ZPUSH(RAWBYTE); ZPUSH(RAWBYTE);
      break;
    case PCD_PUSH3BYTES:
      ZPUSH(RAWBYTE); ZPUSH(RAWBYTE); ZPUSH(RAWBYTE);
      break;
    case PCD_PUSH4BYTES:
      ZPUSH(RAWBYTE); ZPUSH(RAWBYTE); ZPUSH(RAWBYTE); ZPUSH(RAWBYTE);
      break;
    case PCD_PUSH5BYTES:
      ZPUSH(RAWBYTE); ZPUSH(RAWBYTE); ZPUSH(RAWBYTE); ZPUSH(RAWBYTE);
      ZPUSH(RAWBYTE);
      break;
    case PCD_PUSHBYTES:
    {
      int n = RAWBYTE, k;
      for (k = 0; k < n; k++)
        ZPUSH(RAWBYTE);
      break;
    }

    /* ---- line specials -------------------------------------------------- */
    case PCD_LSPEC1: case PCD_LSPEC2: case PCD_LSPEC3:
    case PCD_LSPEC4: case PCD_LSPEC5:
    {
      int n = pcd - PCD_LSPEC1 + 1, k;
      int special = NEXTBYTE;
      int a[5] = {0,0,0,0,0};
      for (k = n - 1; k >= 0; k--)
        a[k] = ZPOP();
      zacs_dispatch_lspec(inst, special, a, NULL);
      break;
    }
    case PCD_LSPEC5RESULT:
    {
      int special = NEXTBYTE, k, r;
      int a[5];
      for (k = 4; k >= 0; k--)
        a[k] = ZPOP();
      zacs_dispatch_lspec(inst, special, a, &r);
      ZPUSH(r);
      break;
    }
    case PCD_LSPEC5EX:
    case PCD_LSPEC5EXRESULT:
    {
      int special = NEXTWORD, k, r;
      int a[5];
      for (k = 4; k >= 0; k--)
        a[k] = ZPOP();
      zacs_dispatch_lspec(inst, special, a, &r);
      if (pcd == PCD_LSPEC5EXRESULT)
        ZPUSH(r);
      break;
    }
    case PCD_LSPEC1DIRECT: case PCD_LSPEC2DIRECT: case PCD_LSPEC3DIRECT:
    case PCD_LSPEC4DIRECT: case PCD_LSPEC5DIRECT:
    {
      int n = pcd - PCD_LSPEC1DIRECT + 1, k;
      int special = NEXTBYTE;
      int a[5] = {0,0,0,0,0};
      for (k = 0; k < n; k++)
        a[k] = NEXTWORD;
      zacs_dispatch_lspec(inst, special, a, NULL);
      break;
    }
    case PCD_LSPEC1DIRECTB: case PCD_LSPEC2DIRECTB: case PCD_LSPEC3DIRECTB:
    case PCD_LSPEC4DIRECTB: case PCD_LSPEC5DIRECTB:
    {
      int n = pcd - PCD_LSPEC1DIRECTB + 1, k;
      int special = RAWBYTE;
      int a[5] = {0,0,0,0,0};
      for (k = 0; k < n; k++)
        a[k] = RAWBYTE;
      zacs_dispatch_lspec(inst, special, a, NULL);
      break;
    }
    case PCD_LSPEC6:
    {
      int k;
      int a[5];
      int special = NEXTBYTE;
      ZDROP(1);                       /* sixth arg unsupported */
      for (k = 4; k >= 0; k--)
        a[k] = ZPOP();
      zacs_dispatch_lspec(inst, special, a, NULL);
      break;
    }
    case PCD_LSPEC6DIRECT:
    {
      int k;
      int a[5];
      int special = NEXTBYTE;
      for (k = 0; k < 5; k++)
        a[k] = NEXTWORD;
      (void)NEXTWORD;
      zacs_dispatch_lspec(inst, special, a, NULL);
      break;
    }
    case PCD_CLEARLINESPECIAL:
      if (inst->line)
        inst->line->special = 0;
      break;
    case PCD_SETLINESPECIAL:
    {
      int a5 = ZPOP(), a4 = ZPOP(), a3 = ZPOP(), a2 = ZPOP(), a1 = ZPOP();
      int special = ZPOP();
      int id = ZPOP();
      int i;
      for (i = 0; i < numlines; i++)
        if (lines[i].tag == id)
        {
          lines[i].special = (short)special;
          lines[i].args[0] = a1; lines[i].args[1] = a2;
          lines[i].args[2] = a3; lines[i].args[3] = a4;
          lines[i].args[4] = a5;
        }
      break;
    }
    case PCD_SETTHINGSPECIAL:
    {
      int a5 = ZPOP(), a4 = ZPOP(), a3 = ZPOP(), a2 = ZPOP(), a1 = ZPOP();
      int special = ZPOP();
      int tid = ZPOP();
      int sp = -1;
      mobj_t *mo;
      while ((mo = P_FindMobjFromTID((short)tid, &sp)) != NULL)
      {
        mo->special = special;
        mo->special_args[0] = a1; mo->special_args[1] = a2;
        mo->special_args[2] = a3; mo->special_args[3] = a4;
        mo->special_args[4] = a5;
      }
      break;
    }

    /* ---- stack / arithmetic --------------------------------------------- */
    case PCD_ADD: { int b = ZPOP(); ZSETSTK(1, ZSTK(1) + b); break; }
    case PCD_SUBTRACT: { int b = ZPOP(); ZSETSTK(1, ZSTK(1) - b); break; }
    case PCD_MULTIPLY: { int b = ZPOP(); ZSETSTK(1, ZSTK(1) * b); break; }
    case PCD_DIVIDE:
    { int b = ZPOP(); ZSETSTK(1, b ? ZSTK(1) / b : 0); break; }
    case PCD_MODULUS:
    { int b = ZPOP(); ZSETSTK(1, b ? ZSTK(1) % b : 0); break; }
    case PCD_EQ: { int b = ZPOP(); ZSETSTK(1, ZSTK(1) == b); break; }
    case PCD_NE: { int b = ZPOP(); ZSETSTK(1, ZSTK(1) != b); break; }
    case PCD_LT: { int b = ZPOP(); ZSETSTK(1, ZSTK(1) < b); break; }
    case PCD_GT: { int b = ZPOP(); ZSETSTK(1, ZSTK(1) > b); break; }
    case PCD_LE: { int b = ZPOP(); ZSETSTK(1, ZSTK(1) <= b); break; }
    case PCD_GE: { int b = ZPOP(); ZSETSTK(1, ZSTK(1) >= b); break; }
    case PCD_ANDLOGICAL: { int b = ZPOP(); ZSETSTK(1, ZSTK(1) && b); break; }
    case PCD_ORLOGICAL:  { int b = ZPOP(); ZSETSTK(1, ZSTK(1) || b); break; }
    case PCD_ANDBITWISE: { int b = ZPOP(); ZSETSTK(1, ZSTK(1) & b); break; }
    case PCD_ORBITWISE:  { int b = ZPOP(); ZSETSTK(1, ZSTK(1) | b); break; }
    case PCD_EORBITWISE: { int b = ZPOP(); ZSETSTK(1, ZSTK(1) ^ b); break; }
    case PCD_NEGATELOGICAL: ZSETSTK(1, !ZSTK(1)); break;
    case PCD_NEGATEBINARY:  ZSETSTK(1, ~ZSTK(1)); break;
    case PCD_UNARYMINUS:    ZSETSTK(1, -ZSTK(1)); break;
    case PCD_LSHIFT: { int b = ZPOP(); ZSETSTK(1, ZSTK(1) << (b & 31)); break; }
    case PCD_RSHIFT: { int b = ZPOP(); ZSETSTK(1, ZSTK(1) >> (b & 31)); break; }
    case PCD_DUP:  { int v = ZSTK(1); ZPUSH(v); break; }
    case PCD_SWAP:
    { int a = ZSTK(2), b = ZSTK(1); ZSETSTK(2, b); ZSETSTK(1, a); break; }
    case PCD_DROP:
    case PCD_SETRESULTVALUE:            /* result channel unsupported */
      ZDROP(1);
      break;
    case PCD_FIXEDMUL:
    { int b = ZPOP(); ZSETSTK(1, FixedMul(ZSTK(1), b)); break; }
    case PCD_FIXEDDIV:
    { int b = ZPOP(); ZSETSTK(1, b ? FixedDiv(ZSTK(1), b) : 0); break; }
    case PCD_SIN:
      ZSETSTK(1, finesine[(((angle_t)ZSTK(1) << 16) >> ANGLETOFINESHIFT) & FINEMASK]);
      break;
    case PCD_COS:
      ZSETSTK(1, finecosine[(((angle_t)ZSTK(1) << 16) >> ANGLETOFINESHIFT) & FINEMASK]);
      break;
    case PCD_VECTORANGLE:
    {
      int y = ZPOP();
      ZSETSTK(1, (int)(R_PointToAngle2(0, 0, ZSTK(1), y) >> 16));
      break;
    }

    /* ---- control flow ----------------------------------------------------*/
    case PCD_GOTO:
      ip = zacs_rd32(ip);
      break;
    case PCD_GOTOSTACK:
      ip = ZPOP();
      break;
    case PCD_IFGOTO:
    { int dest = NEXTWORD; if (ZPOP()) ip = dest; break; }
    case PCD_IFNOTGOTO:
    { int dest = NEXTWORD; if (!ZPOP()) ip = dest; break; }
    case PCD_CASEGOTO:
    {
      int val = NEXTWORD, dest = NEXTWORD;
      if (ZSTK(1) == val)
      {
        ZDROP(1);
        ip = dest;
      }
      break;
    }
    case PCD_CASEGOTOSORTED:
    {
      int ncases, lo, hi, v;
      ip = (ip + 3) & ~3;
      ncases = zacs_rd32(ip);
      ip += 4;
      v = ZSTK(1);
      lo = 0;
      hi = ncases - 1;
      while (lo <= hi)
      {
        int mid = (lo + hi) / 2;
        int cv = zacs_rd32(ip + mid * 8);
        if (cv == v)
        {
          ZDROP(1);
          ip = zacs_rd32(ip + mid * 8 + 4);
          break;
        }
        if (cv < v)
          lo = mid + 1;
        else
          hi = mid - 1;
      }
      if (lo > hi)
        ip += ncases * 8;
      break;
    }
    case PCD_RESTART:
      ip = zacs_scripts[inst->info].address;
      break;
    case PCD_CALL:
    case PCD_CALLDISCARD:
    case PCD_CALLSTACK:
    {
      int fi = (pcd == PCD_CALLSTACK) ? ZPOP() : NEXTBYTE;
      zacs_func_t *f;
      zacs_frame_t *fr;
      int k;
      fi &= 0xFFFF;
      if (fi < 0 || fi >= zacs_numfuncs || inst->fp >= ZACS_CALLDEPTH)
      {
        lprintf(LO_WARN, "ZACS: bad function call %d in script %d\n",
                fi, inst->script);
        zacs_finish(inst);
        return;
      }
      f = &zacs_funcs[fi];
      /* An imported function (ACS library import) has address 0 because its
       * body lives in another module.  If library linking did not resolve it,
       * do not jump to offset 0 -- that runs the object header as pcode and
       * sprays garbage (bad calls, bogus R_FlatNumForName lookups).  Treat
       * it as a clean no-op: consume the arguments and yield 0. */
      if (f->address == 0)
      {
        for (k = f->argc - 1; k >= 0; k--)
          (void)ZPOP();
        if (pcd != PCD_CALLDISCARD)
          ZPUSH(0);
        break;
      }
      {
        /* Capture target before activating: zacs_activate() repoints
         * zacs_funcs, which would invalidate f. */
        int tgt_module = f->module;
        int tgt_addr   = f->address;
        int tgt_argc   = f->argc;
        fr = &inst->frames[inst->fp++];
        memset(fr->locals, 0, sizeof(fr->locals));
        fr->return_ip = ip;
        fr->discard = (pcd == PCD_CALLDISCARD);
        fr->caller_module = zacs_active;
        for (k = tgt_argc - 1; k >= 0; k--)
          fr->locals[k] = ZPOP();
        locals = fr->locals;
        if (tgt_module != zacs_active)
          zacs_activate(tgt_module);
        ip = tgt_addr;
      }
      break;
    }
    case PCD_RETURNVOID:
    case PCD_RETURNVAL:
    {
      int v = (pcd == PCD_RETURNVAL) ? ZPOP() : 0;
      zacs_frame_t *fr;
      if (!inst->fp)
      {
        zacs_finish(inst);
        return;
      }
      fr = &inst->frames[--inst->fp];
      if (fr->caller_module != zacs_active)
        zacs_activate(fr->caller_module);
      ip = fr->return_ip;
      locals = inst->fp ? inst->frames[inst->fp - 1].locals : inst->locals;
      if (!fr->discard)
        ZPUSH(v);
      break;
    }
    case PCD_PUSHFUNCTION:
      ZPUSH(NEXTBYTE);
      break;
    case PCD_TAGSTRING:                 /* single behavior: index unchanged */
      break;

    /* ---- waits ------------------------------------------------------------*/
    case PCD_DELAY:
      inst->statedata = ZPOP();
      goto delay_common;
    case PCD_DELAYDIRECT:
      inst->statedata = NEXTWORD;
      goto delay_common;
    case PCD_DELAYDIRECTB:
      inst->statedata = RAWBYTE;
    delay_common:
      if (inst->statedata > 0)
        inst->state = ZSTATE_DELAYED;
      break;
    case PCD_TAGWAIT:
      inst->statedata = ZPOP();
      inst->state = ZSTATE_TAGWAIT;
      break;
    case PCD_TAGWAITDIRECT:
      inst->statedata = NEXTWORD;
      inst->state = ZSTATE_TAGWAIT;
      break;
    case PCD_POLYWAIT:
      inst->statedata = ZPOP();
      inst->state = ZSTATE_POLYWAIT;
      break;
    case PCD_POLYWAITDIRECT:
      inst->statedata = NEXTWORD;
      inst->state = ZSTATE_POLYWAIT;
      break;
    case PCD_SCRIPTWAIT:
      inst->statedata = ZPOP();
      inst->state = ZSTATE_SCRIPTWAIT;
      break;
    case PCD_SCRIPTWAITDIRECT:
      inst->statedata = NEXTWORD;
      inst->state = ZSTATE_SCRIPTWAIT;
      break;
    case PCD_SCRIPTWAITNAMED:
    {
      const char *nm = zacs_string(ZPOP());
      int i;
      inst->statedata = 0;
      for (i = 0; i < zacs_numsnames; i++)
        if (zacs_snames[i] && !strcasecmp(zacs_snames[i], nm))
        {
          inst->statedata = -(i + 1);
          break;
        }
      inst->state = ZSTATE_SCRIPTWAIT;
      break;
    }
    case PCD_RANDOM:
    {
      int max = ZPOP();
      ZSETSTK(1, zacs_random(ZSTK(1), max));
      break;
    }
    case PCD_RANDOMDIRECT:
    {
      int min = NEXTWORD, max = NEXTWORD;
      ZPUSH(zacs_random(min, max));
      break;
    }
    case PCD_RANDOMDIRECTB:
    {
      int min = RAWBYTE, max = RAWBYTE;
      ZPUSH(zacs_random(min, max));
      break;
    }

    /* ---- generated variable / array operations --------------------------- */
#include "p_zacs_ops.h"

    /* ---- print family ------------------------------------------------------*/
    case PCD_BEGINPRINT:
      inst->printlen = 0;
      inst->printbuf[0] = 0;
      inst->optstart = -1;
      break;
    case PCD_PRINTSTRING:
    case PCD_PRINTLOCALIZED:
      zacs_print_str(inst, zacs_string(ZPOP()));
      break;
    case PCD_PRINTBIND:
      zacs_print_str(inst, zacs_string(ZPOP()));
      break;
    case PCD_PRINTNUMBER:
      zacs_print_num(inst, ZPOP());
      break;
    case PCD_PRINTCHARACTER:
    {
      int c = ZPOP();
      if (inst->printlen < ZACS_PRINTBUF - 1)
      {
        inst->printbuf[inst->printlen++] = (char)c;
        inst->printbuf[inst->printlen] = 0;
      }
      break;
    }
    case PCD_PRINTFIXED:
    {
      int v = ZPOP();
      zacs_print_num(inst, v >> FRACBITS);
      zacs_print_str(inst, ".");
      zacs_print_num(inst, (int)(((int64_t)(v & 0xFFFF) * 1000) >> FRACBITS));
      break;
    }
    case PCD_PRINTNAME:
    {
      int v = ZPOP();
      if (v == 0)
        zacs_print_str(inst, inst->activator && inst->activator->player ?
                       "Player" : "Unknown");
      else
        zacs_print_num(inst, v);
      break;
    }
    case PCD_PRINTBINARY:
    {
      unsigned v = (unsigned)ZPOP();
      char b[33];
      int k = 32;
      b[32] = 0;
      do { b[--k] = (char)('0' + (v & 1)); v >>= 1; } while (v && k);
      zacs_print_str(inst, b + k);
      break;
    }
    case PCD_PRINTHEX:
    {
      unsigned v = (unsigned)ZPOP();
      char b[9];
      int k = 8;
      b[8] = 0;
      do { b[--k] = "0123456789ABCDEF"[v & 15]; v >>= 4; } while (v && k);
      zacs_print_str(inst, b + k);
      break;
    }
    case PCD_PRINTMAPCHARARRAY:
    case PCD_PRINTWORLDCHARARRAY:
    case PCD_PRINTGLOBALCHARARRAY:
    case PCD_PRINTSCRIPTCHARARRAY:
    case PCD_PRINTMAPCHRANGE:
    case PCD_PRINTWORLDCHRANGE:
    case PCD_PRINTGLOBALCHRANGE:
    case PCD_PRINTSCRIPTCHRANGE:
    {
      int capacity = 0x7FFFFFFF, offset = 0, a, c;
      dbool ranged = (pcd >= PCD_PRINTMAPCHRANGE && pcd <= PCD_PRINTGLOBALCHRANGE)
                  || pcd == PCD_PRINTSCRIPTCHRANGE;
      if (ranged)
      {
        capacity = ZPOP();
        offset = ZPOP();
        if (capacity < 1 || offset < 0)
        {
          ZDROP(2);
          break;
        }
      }
      a = ZPOP();
      offset += ZPOP();
      while (capacity-- > 0)
      {
        if (pcd == PCD_PRINTMAPCHARARRAY || pcd == PCD_PRINTMAPCHRANGE)
          c = zarr_get_map(inst, a, offset);
        else if (pcd == PCD_PRINTWORLDCHARARRAY || pcd == PCD_PRINTWORLDCHRANGE)
          c = zarr_get_world(inst, a, offset);
        else if (pcd == PCD_PRINTGLOBALCHARARRAY || pcd == PCD_PRINTGLOBALCHRANGE)
          c = zarr_get_global(inst, a, offset);
        else
          c = zarr_get_script(inst, a, offset);
        if (!c)
          break;
        if (inst->printlen < ZACS_PRINTBUF - 1)
        {
          inst->printbuf[inst->printlen++] = (char)c;
          inst->printbuf[inst->printlen] = 0;
        }
        offset++;
      }
      break;
    }
    case PCD_ENDPRINT:
      zacs_deliver(inst, false);
      break;
    case PCD_ENDPRINTBOLD:
      zacs_deliver(inst, true);
      break;
    case PCD_ENDLOG:
      lprintf(LO_INFO, "ZACS log: %s\n", inst->printbuf);
      break;
    case PCD_MOREHUDMESSAGE:
      inst->optstart = -1;
      break;
    case PCD_OPTHUDMESSAGE:
      inst->optstart = inst->sp;
      break;
    case PCD_ENDHUDMESSAGE:
    case PCD_ENDHUDMESSAGEBOLD:
    {
      /* Verbose HudMessage lowering pushes the five base params (type, id,
       * color, x, y) and then OPTHUDMESSAGE marks the boundary; the optional
       * params (holdtime first) are pushed above it.  So holdtime is at the
       * top and the base block sits just below optstart.  Read both, then
       * collapse the stack. */
      int htype, hid, hcolor, hx, hy, hhold = 0;
      int base = (inst->optstart >= 0) ? inst->optstart : inst->sp;
      if (inst->optstart >= 0 && inst->sp > inst->optstart)
        hhold = inst->stack[inst->optstart];     /* first optional param */
      hy     = (base >= 1) ? inst->stack[base - 1] : 0;
      hx     = (base >= 2) ? inst->stack[base - 2] : 0;
      hcolor = (base >= 3) ? inst->stack[base - 3] : 0;
      hid    = (base >= 4) ? inst->stack[base - 4] : 0;
      htype  = (base >= 5) ? inst->stack[base - 5] : 0;
      (void)htype;
      inst->sp = (base >= 5) ? base - 5 : 0;     /* drop base + optionals */
      inst->optstart = -1;
      if (inst->printlen)
      {
        /* holdtime is 16.16 seconds; <=0 means "until replaced".  Convert to
         * tics (35/sec); persistent when not positive. */
        int tics = (hhold > 0) ? (int)(((long long)hhold * 35) >> 16) : -1;
        zacs_hud_store(inst->printbuf, hid,
                       zacs_hud_map_x(hx), zacs_hud_map_y(hy),
                       tics, hcolor ? hcolor : -1);
      }
      inst->printlen  = 0;
      inst->printbuf[0] = 0;
      break;
    }
    case PCD_SAVESTRING:
      ZPUSH(zacs_save_string(inst->printbuf));
      break;
    case PCD_STRLEN:
      ZSETSTK(1, (int)strlen(zacs_string(ZSTK(1))));
      break;
    case PCD_STRCPYTOMAPCHRANGE:
    case PCD_STRCPYTOWORLDCHRANGE:
    case PCD_STRCPYTOGLOBALCHRANGE:
    case PCD_STRCPYTOSCRIPTCHRANGE:
    {
      /* stack: arrayid(5) offset(6=index base) capacity(3) stroffset(1)
       * stringid(2); net effect: pop 6 push success */
      int stroffset = ZPOP();
      int stringid  = ZPOP();
      int capacity  = ZPOP();
      int index     = ZPOP();
      int a         = ZPOP();
      int base      = ZPOP();
      const char *s = zacs_string(stringid);
      int ok = 1;
      index += base;
      if (index < 0 || stroffset < 0 || capacity < 0)
        ok = 0;
      else
      {
        int k;
        for (k = 0; k < stroffset && s[k]; k++)
          ;
        s += k;
        while (capacity-- > 0)
        {
          int c = *s ? *s : 0;
          if (pcd == PCD_STRCPYTOMAPCHRANGE)
            zarr_set_map(inst, a, index, c);
          else if (pcd == PCD_STRCPYTOWORLDCHRANGE)
            zarr_set_world(inst, a, index, c);
          else if (pcd == PCD_STRCPYTOGLOBALCHRANGE)
            zarr_set_global(inst, a, index, c);
          index++;
          if (!*s)
            break;
          s++;
        }
        ok = !*s;
      }
      ZPUSH(ok);
      break;
    }

    /* ---- world interaction --------------------------------------------------*/
    case PCD_CHANGEFLOOR:
    case PCD_CHANGECEILING:
    {
      int flat = R_FlatNumForName(zacs_string(ZPOP()));
      int tag = ZPOP();
      int sn = -1;
      while ((sn = P_FindSectorFromTag(tag, sn)) >= 0)
      {
        if (pcd == PCD_CHANGEFLOOR)
          sectors[sn].floorpic = (short)flat;
        else
          sectors[sn].ceilingpic = (short)flat;
      }
      break;
    }
    case PCD_CHANGEFLOORDIRECT:
    case PCD_CHANGECEILINGDIRECT:
    {
      int tag = NEXTWORD;
      int flat = R_FlatNumForName(zacs_string(NEXTWORD));
      int sn = -1;
      while ((sn = P_FindSectorFromTag(tag, sn)) >= 0)
      {
        if (pcd == PCD_CHANGEFLOORDIRECT)
          sectors[sn].floorpic = (short)flat;
        else
          sectors[sn].ceilingpic = (short)flat;
      }
      break;
    }
    case PCD_CHANGESKY:
    {
      int s2 = ZPOP(), s1 = ZPOP();
      int t = R_CheckTextureNumForName(zacs_string(s1));
      (void)s2;
      if (t >= 0)
        skytexture = t;
      break;
    }
    case PCD_SETLINETEXTURE:
    {
      int tex, position, sidesel, id, i;
      tex = R_SafeTextureNumForName(zacs_string(ZPOP()), 0);
      position = ZPOP();
      sidesel = ZPOP();
      id = ZPOP();
      for (i = 0; i < numlines; i++)
      {
        if (lines[i].tag != id)
          continue;
        if (lines[i].sidenum[sidesel & 1] != NO_INDEX)
        {
          side_t *sd = &sides[lines[i].sidenum[sidesel & 1]];
          if (position == 0)
            sd->toptexture = (short)tex;
          else if (position == 1)
            sd->midtexture = (short)tex;
          else
            sd->bottomtexture = (short)tex;
        }
      }
      break;
    }
    case PCD_SETLINEBLOCKING:
    {
      int block = ZPOP(), id = ZPOP(), i;
      for (i = 0; i < numlines; i++)
        if (lines[i].tag == id)
        {
          if (block)
            lines[i].flags |= ML_BLOCKING;
          else
            lines[i].flags &= ~ML_BLOCKING;
        }
      break;
    }
    case PCD_SETLINEMONSTERBLOCKING:
    {
      int block = ZPOP(), id = ZPOP(), i;
      for (i = 0; i < numlines; i++)
        if (lines[i].tag == id)
        {
          if (block)
            lines[i].flags |= ML_BLOCKMONSTERS;
          else
            lines[i].flags &= ~ML_BLOCKMONSTERS;
        }
      break;
    }
    case PCD_GETLINEROWOFFSET:
      ZPUSH(inst->line && inst->line->sidenum[0] != NO_INDEX ?
            sides[inst->line->sidenum[0]].rowoffset >> FRACBITS : 0);
      break;
    case PCD_LINESIDE:
      ZPUSH(inst->side);
      break;
    case PCD_GETSECTORFLOORZ:
    case PCD_GETSECTORCEILINGZ:
    {
      int y = ZPOP(), x = ZPOP(), tag = ZSTK(1);
      int sn = P_FindSectorFromTag(tag, -1);
      int z = 0;
      (void)x; (void)y;
      if (sn >= 0)
        z = (pcd == PCD_GETSECTORFLOORZ) ?
            sectors[sn].floorheight : sectors[sn].ceilingheight;
      ZSETSTK(1, z);
      break;
    }
    case PCD_GETSECTORLIGHTLEVEL:
    {
      int sn = P_FindSectorFromTag(ZSTK(1), -1);
      ZSETSTK(1, sn >= 0 ? sectors[sn].lightlevel : 0);
      break;
    }
    case PCD_SECTORDAMAGE:
    {
      int flags = ZPOP();
      int protection = ZPOP();
      int type = ZPOP();
      int amount = ZPOP();
      int tag = ZPOP();
      int sn = -1;
      (void)flags; (void)protection; (void)type;
      while ((sn = P_FindSectorFromTag(tag, sn)) >= 0)
      {
        mobj_t *mo;
        for (mo = sectors[sn].thinglist; mo; mo = mo->snext)
          if (mo->player || (mo->flags & MF_SHOOTABLE))
            P_DamageMobj(mo, NULL, NULL, amount);
      }
      break;
    }
    case PCD_CHECKACTORFLOORTEXTURE:
    case PCD_CHECKACTORCEILINGTEXTURE:
    {
      const char *nm = zacs_string(ZPOP());
      mobj_t *mo = zacs_aptr(inst, ZSTK(1));
      int match = 0;
      if (mo && mo->subsector)
      {
        int flat = (pcd == PCD_CHECKACTORFLOORTEXTURE) ?
                   mo->subsector->sector->floorpic :
                   mo->subsector->sector->ceilingpic;
        int want = R_FlatNumForName(nm);
        match = (flat == want);
      }
      ZSETSTK(1, match);
      break;
    }
    case PCD_REPLACETEXTURES:
      ZDROP(3);
      zacs_warn_pcd(pcd);
      break;

    /* ---- sound and music ---------------------------------------------------*/
    case PCD_SECTORSOUND:
    {
      int vol = ZPOP();
      int sfx = zacs_sfx_for_name(zacs_string(ZPOP()));
      (void)vol;
      if (sfx > 0 && inst->line)
        S_StartSound((mobj_t *)&inst->line->frontsector->soundorg, sfx);
      break;
    }
    case PCD_AMBIENTSOUND:
    case PCD_LOCALAMBIENTSOUND:
    {
      int vol = ZPOP();
      int sfx = zacs_sfx_for_name(zacs_string(ZPOP()));
      (void)vol;
      if (sfx > 0)
        S_StartSound(NULL, sfx);
      break;
    }
    case PCD_ACTIVATORSOUND:
    {
      int vol = ZPOP();
      int sfx = zacs_sfx_for_name(zacs_string(ZPOP()));
      (void)vol;
      if (sfx > 0 && inst->activator)
        S_StartSound(inst->activator, sfx);
      break;
    }
    case PCD_THINGSOUND:
    {
      int vol = ZPOP();
      int sfx = zacs_sfx_for_name(zacs_string(ZPOP()));
      int tid = ZPOP();
      int sp = -1;
      mobj_t *mo;
      (void)vol;
      if (sfx > 0)
        while ((mo = P_FindMobjFromTID((short)tid, &sp)) != NULL)
          S_StartSound(mo, sfx);
      break;
    }
    case PCD_SOUNDSEQUENCE:
      ZDROP(1);                       /* no SNDSEQ on Doom-format games */
      break;
    case PCD_SETMUSIC:
    case PCD_LOCALSETMUSIC:
    {
      const char *nm;
      ZDROP(2);                       /* order, position */
      nm = zacs_string(ZPOP());
      S_ChangeMusicByName((char *)nm, true);
      break;
    }
    case PCD_SETMUSICDIRECT:
    case PCD_LOCALSETMUSICDIRECT:
    {
      const char *nm = zacs_string(NEXTWORD);
      (void)NEXTWORD;
      (void)NEXTWORD;
      S_ChangeMusicByName((char *)nm, true);
      break;
    }
    case PCD_MUSICCHANGE:
      ZDROP(2);
      zacs_warn_pcd(pcd);
      break;

    /* ---- game state queries -------------------------------------------------*/
    case PCD_GAMETYPE:
      ZPUSH(deathmatch ? 2 : netgame ? 1 : 0);
      break;
    case PCD_GAMESKILL:
      ZPUSH(gameskill);
      break;
    case PCD_TIMER:
      ZPUSH(leveltime);
      break;
    case PCD_PLAYERCOUNT:
    {
      int i, n = 0;
      for (i = 0; i < MAXPLAYERS; i++)
        if (playeringame[i])
          n++;
      ZPUSH(n);
      break;
    }
    case PCD_SINGLEPLAYER:
      ZPUSH(!netgame);
      break;
    case PCD_ISNETWORKGAME:
      ZPUSH(netgame);
      break;
    case PCD_PLAYERHEALTH:
      ZPUSH(inst->activator ? (inst->activator->player ?
            inst->activator->player->health : inst->activator->health) : 0);
      break;
    case PCD_PLAYERARMORPOINTS:
      ZPUSH(inst->activator && inst->activator->player ?
            inst->activator->player->armorpoints : 0);
      break;
    case PCD_PLAYERFRAGS:
      ZPUSH(0);
      break;
    case PCD_PLAYERNUMBER:
      ZPUSH(inst->activator && inst->activator->player ?
            (int)(inst->activator->player - players) : -1);
      break;
    case PCD_PLAYERINGAME:
      ZSETSTK(1, ZSTK(1) >= 0 && ZSTK(1) < MAXPLAYERS &&
              playeringame[ZSTK(1)]);
      break;
    case PCD_PLAYERISBOT:
      ZSETSTK(1, 0);
      break;
    case PCD_PLAYERCLASS:
      ZSETSTK(1, 0);
      break;
    case PCD_ACTIVATORTID:
      ZPUSH(inst->activator ? inst->activator->tid : 0);
      break;
    case PCD_GETSCREENWIDTH:
      ZPUSH(320);
      break;
    case PCD_GETSCREENHEIGHT:
      ZPUSH(200);
      break;
    case PCD_GETLEVELINFO:
    {
      int what = ZSTK(1), v = 0;
      switch (what)
      {
        case 0: v = 0; break;                          /* par time */
        case 1: v = 0; break;                          /* cluster */
        case 2: v = gamemap; break;
        case 3: v = totalsecret; break;
        case 4: v = players[consoleplayer].secretcount; break;
        case 5: v = totalitems; break;
        case 6: v = players[consoleplayer].itemcount; break;
        case 7: v = totalkills; break;
        case 8: v = players[consoleplayer].killcount; break;
        default: v = 0; break;
      }
      ZSETSTK(1, v);
      break;
    }
    case PCD_GETCVAR:
      ZSETSTK(1, 0);
      zacs_warn_pcd(pcd);
      break;
    case PCD_GETPLAYERINFO:
    { int q = ZPOP(); (void)q; ZSETSTK(1, 0); zacs_warn_pcd(pcd); break; }
    case PCD_GETPLAYERINPUT:
    {
      /* GetPlayerInput(player, input): read the current (INPUT_BUTTONS = 3) or
       * previous-tic (INPUT_OLDBUTTONS = 2) button state, translating the
       * engine's button bits into the ZDoom layout (BT_ATTACK = 1<<0,
       * BT_USE = 1<<2) that content tests against.  player < 0 selects the
       * activator's player; other selectors yield 0. */
      int input = ZPOP();
      int pnum  = ZSTK(1);
      int r = 0;
      player_t *pl = NULL;
      if (pnum < 0)
        pl = (inst->activator && inst->activator->player)
               ? inst->activator->player : &players[consoleplayer];
      else if (pnum >= 0 && pnum < MAXPLAYERS && playeringame[pnum])
        pl = &players[pnum];
      if (pl && (input == 2 || input == 3))
      {
        int b = (input == 3) ? pl->cmd.buttons : zacs_oldbuttons_of(pl);
        if (b & BT_ATTACK) r |= 1;
        if (b & BT_USE)    r |= 4;
      }
      ZSETSTK(1, r);
      break;
    }
    case PCD_GETSIGILPIECES:
      ZPUSH(0);
      break;
    case PCD_GETAMMOCAPACITY:
    {
      const char *nm = zacs_string(ZSTK(1));
      int v = 0;
      size_t i;
      if (inst->activator && inst->activator->player)
        for (i = 0; i < sizeof(zacs_ammomap)/sizeof(zacs_ammomap[0]); i++)
          if (!strcasecmp(nm, zacs_ammomap[i].name))
            v = inst->activator->player->maxammo[zacs_ammomap[i].ammo];
      ZSETSTK(1, v);
      break;
    }
    case PCD_SETAMMOCAPACITY:
    {
      int cap = ZPOP();
      const char *nm = zacs_string(ZPOP());
      size_t i;
      if (inst->activator && inst->activator->player && cap >= 0)
        for (i = 0; i < sizeof(zacs_ammomap)/sizeof(zacs_ammomap[0]); i++)
          if (!strcasecmp(nm, zacs_ammomap[i].name))
            inst->activator->player->maxammo[zacs_ammomap[i].ammo] = cap;
      break;
    }

    /* ---- doom key checks (Skulltag pcodes) -----------------------------------*/
    case PCD_PLAYERBLUESKULL: case PCD_PLAYERREDSKULL:
    case PCD_PLAYERYELLOWSKULL: case PCD_PLAYERBLUECARD:
    case PCD_PLAYERREDCARD: case PCD_PLAYERYELLOWCARD:
    {
      player_t *p = inst->activator ? inst->activator->player : NULL;
      int card =
        pcd == PCD_PLAYERBLUESKULL ? it_blueskull :
        pcd == PCD_PLAYERREDSKULL ? it_redskull :
        pcd == PCD_PLAYERYELLOWSKULL ? it_yellowskull :
        pcd == PCD_PLAYERBLUECARD ? it_bluecard :
        pcd == PCD_PLAYERREDCARD ? it_redcard : it_yellowcard;
      ZPUSH(p ? (p->cards[card] ? 1 : 0) : 0);
      break;
    }
    case PCD_PLAYERMASTERSKULL: case PCD_PLAYERMASTERCARD:
    case PCD_PLAYERBLACKSKULL: case PCD_PLAYERSILVERSKULL:
    case PCD_PLAYERGOLDSKULL: case PCD_PLAYERBLACKCARD:
    case PCD_PLAYERSILVERCARD: case PCD_PLAYERTEAM:
    case PCD_PLAYEREXPERT: case PCD_BLUETEAMCOUNT:
    case PCD_REDTEAMCOUNT: case PCD_BLUETEAMSCORE:
    case PCD_REDTEAMSCORE: case PCD_ISONEFLAGCTF:
      ZPUSH(0);
      break;

    /* ---- actors ----------------------------------------------------------*/
    case PCD_THINGCOUNT:
    case PCD_THINGCOUNTDIRECT:
    {
      int tid, type;
      if (pcd == PCD_THINGCOUNTDIRECT)
      {
        type = NEXTWORD;
        tid = NEXTWORD;
      }
      else
      {
        tid = ZPOP();
        type = ZPOP();
      }
      {
        int n = 0;
        if (type != 0)
        {
          static dbool warned;
          if (!warned)
          {
            warned = true;
            lprintf(LO_WARN, "ZACS: ThingCount by spawn id unsupported; "
                    "counting by tid only\n");
          }
        }
        if (tid)
        {
          int sp = -1;
          while (P_FindMobjFromTID((short)tid, &sp))
            n++;
        }
        else
        {
          thinker_t *th;
          for (th = thinkercap.next; th != &thinkercap; th = th->next)
            if (th->function.arg1 == (void (*)(void *)) P_MobjThinker &&
                (((mobj_t *)th)->flags & MF_COUNTKILL) &&
                ((mobj_t *)th)->health > 0)
              n++;
        }
        ZPUSH(n);
      }
      break;
    }
    case PCD_THINGCOUNTNAME:
    {
      int tid = ZPOP();
      int type = zacs_actor_type(zacs_string(ZSTK(1)));
      int n = 0;
      if (tid)
      {
        int sp = -1;
        mobj_t *mo;
        while ((mo = P_FindMobjFromTID((short)tid, &sp)) != NULL)
          if (type < 0 || mo->type == type)
            n++;
      }
      else if (type >= 0)
      {
        thinker_t *th;
        for (th = thinkercap.next; th != &thinkercap; th = th->next)
          if (th->function.arg1 == (void (*)(void *)) P_MobjThinker &&
              ((mobj_t *)th)->type == type && ((mobj_t *)th)->health > 0)
            n++;
      }
      ZSETSTK(1, n);
      break;
    }
    case PCD_THINGCOUNTSECTOR:
    case PCD_THINGCOUNTNAMESECTOR:
      ZDROP(2);
      ZSETSTK(1, 0);
      zacs_warn_pcd(pcd);
      break;
    case PCD_GETACTORX:
    case PCD_GETACTORY:
    case PCD_GETACTORZ:
    case PCD_GETACTORFLOORZ:
    case PCD_GETACTORCEILINGZ:
    case PCD_GETACTORANGLE:
    case PCD_GETACTORPITCH:
    case PCD_GETACTORLIGHTLEVEL:
    {
      mobj_t *mo = zacs_aptr(inst, ZSTK(1));
      int v = 0;
      if (mo)
        switch (pcd)
        {
          case PCD_GETACTORX: v = mo->x; break;
          case PCD_GETACTORY: v = mo->y; break;
          case PCD_GETACTORZ: v = mo->z; break;
          case PCD_GETACTORFLOORZ: v = mo->floorz; break;
          case PCD_GETACTORCEILINGZ: v = mo->ceilingz; break;
          case PCD_GETACTORANGLE: v = (int)(mo->angle >> 16); break;
          case PCD_GETACTORLIGHTLEVEL:
            v = mo->subsector ? mo->subsector->sector->lightlevel : 0;
            break;
          default: v = 0; break;
        }
      ZSETSTK(1, v);
      break;
    }
    case PCD_SETACTORANGLE:
    {
      int ang = ZPOP();
      int tid = ZPOP();
      int sp = -1;
      mobj_t *mo;
      if (tid == 0)
      {
        if (inst->activator)
          inst->activator->angle = (angle_t)ang << 16;
      }
      else
        while ((mo = P_FindMobjFromTID((short)tid, &sp)) != NULL)
          mo->angle = (angle_t)ang << 16;
      break;
    }
    case PCD_SETACTORPITCH:
      ZDROP(2);
      break;
    case PCD_SETACTORPOSITION:
    {
      int fog = ZPOP();
      fixed_t z = ZPOP(), y = ZPOP(), x = ZPOP();
      mobj_t *mo = zacs_aptr(inst, ZSTK(1));
      int ok = 0;
      (void)fog;
      if (mo && P_TeleportMove(mo, x, y, false))
      {
        mo->z = z;
        ok = 1;
      }
      ZSETSTK(1, ok);
      break;
    }
    case PCD_SPAWN:
    case PCD_SPAWNDIRECT:
    {
      int type, x, y, z, tid, angle;
      if (pcd == PCD_SPAWNDIRECT)
      {
        type = zacs_actor_type(zacs_string(NEXTWORD));
        x = NEXTWORD; y = NEXTWORD; z = NEXTWORD;
        tid = NEXTWORD; angle = NEXTWORD;
      }
      else
      {
        angle = ZPOP(); tid = ZPOP(); z = ZPOP(); y = ZPOP(); x = ZPOP();
        type = zacs_actor_type(zacs_string(ZSTK(1)));
      }
      {
        int ok = 0;
        if (type >= 0)
        {
          mobj_t *mo = P_SpawnMobj(x, y, z, type);
          if (mo)
          {
            mo->angle = (angle_t)(angle & 0xFF) << 24;
            if (tid)
              P_InsertMobjIntoTIDList(mo, (short)tid);
            ok = 1;
          }
        }
        if (pcd == PCD_SPAWNDIRECT)
          ZPUSH(ok);
        else
          ZSETSTK(1, ok);
      }
      break;
    }
    case PCD_SPAWNSPOT:
    case PCD_SPAWNSPOTDIRECT:
    case PCD_SPAWNSPOTFACING:
    {
      int type, spottid, tid, angle = 0;
      dbool facing = (pcd == PCD_SPAWNSPOTFACING);
      if (pcd == PCD_SPAWNSPOTDIRECT)
      {
        type = zacs_actor_type(zacs_string(NEXTWORD));
        spottid = NEXTWORD; tid = NEXTWORD; angle = NEXTWORD;
      }
      else if (facing)
      {
        tid = ZPOP(); spottid = ZPOP();
        type = zacs_actor_type(zacs_string(ZSTK(1)));
      }
      else
      {
        angle = ZPOP(); tid = ZPOP(); spottid = ZPOP();
        type = zacs_actor_type(zacs_string(ZSTK(1)));
      }
      {
        int n = 0;
        if (type >= 0)
        {
          int sp = -1;
          mobj_t *spot;
          while ((spot = P_FindMobjFromTID((short)spottid, &sp)) != NULL)
          {
            mobj_t *mo = P_SpawnMobj(spot->x, spot->y, spot->z, type);
            if (!mo)
              continue;
            mo->angle = facing ? spot->angle : (angle_t)(angle & 0xFF) << 24;
            if (tid)
              P_InsertMobjIntoTIDList(mo, (short)tid);
            n++;
          }
        }
        if (pcd == PCD_SPAWNSPOTDIRECT)
          ZPUSH(n);
        else
          ZSETSTK(1, n);
      }
      break;
    }
    case PCD_THINGDAMAGE2:
    {
      const char *dt = zacs_string(ZPOP());
      int amount = ZPOP();
      int tid = ZSTK(1);
      int sp = -1, n = 0;
      mobj_t *mo;
      (void)dt;
      while ((mo = P_FindMobjFromTID((short)tid, &sp)) != NULL)
      {
        P_DamageMobj(mo, NULL, inst->activator, amount);
        n++;
      }
      ZSETSTK(1, n);
      break;
    }
    case PCD_CLASSIFYACTOR:
    {
      mobj_t *mo = zacs_aptr(inst, ZSTK(1));
      int v = 0;
      if (mo)
      {
        if (mo->player)
          v |= 2 | 1;                       /* PLAYER | (world actor) */
        else if (mo->flags & MF_MISSILE)
          v |= 128;
        else if (mo->flags & MF_COUNTKILL)
          v |= 16;
        else
          v |= 256;                         /* GENERIC */
        v |= (mo->health > 0) ? 32 : 64;    /* ALIVE : DEAD */
      }
      ZSETSTK(1, v);
      break;
    }
    case PCD_GETACTORPROPERTY:
    {
      int prop = ZPOP();
      mobj_t *mo = zacs_aptr(inst, ZSTK(1));
      int v = 0;
      if (mo)
        switch (prop)
        {
          case 0: v = mo->health; break;            /* APROP_Health */
          default:
            zacs_warn_pcd(pcd);
            break;
        }
      ZSETSTK(1, v);
      break;
    }
    case PCD_SETACTORPROPERTY:
    {
      int val = ZPOP();
      int prop = ZPOP();
      mobj_t *mo = zacs_aptr(inst, ZPOP());
      if (mo)
        switch (prop)
        {
          case 0:
            mo->health = val;
            if (mo->player)
              mo->player->health = val;
            break;
          default:
            zacs_warn_pcd(pcd);
            break;
        }
      break;
    }
    case PCD_SETACTORSTATE:
    {
      ZDROP(2);
      ZSETSTK(1, 0);
      zacs_warn_pcd(pcd);
      break;
    }
    case PCD_MORPHACTOR:
      ZDROP(6);
      ZSETSTK(1, 0);
      zacs_warn_pcd(pcd);
      break;
    case PCD_UNMORPHACTOR:
      ZDROP(1);
      ZSETSTK(1, 0);
      zacs_warn_pcd(pcd);
      break;
    case PCD_THING_PROJECTILE2:
      ZDROP(7);
      zacs_warn_pcd(pcd);
      break;
    case PCD_SPAWNPROJECTILE:
      ZDROP(7);
      zacs_warn_pcd(pcd);
      break;
    case PCD_CHECKPLAYERCAMERA:
      ZSETSTK(1, -1);
      break;

    /* ---- inventory ---------------------------------------------------------*/
    case PCD_CHECKINVENTORY:
      ZSETSTK(1, zacs_check_inventory(inst->activator, zacs_string(ZSTK(1))));
      break;
    case PCD_CHECKINVENTORYDIRECT:
      ZPUSH(zacs_check_inventory(inst->activator, zacs_string(NEXTWORD)));
      break;
    case PCD_CHECKACTORINVENTORY:
    {
      const char *nm = zacs_string(ZPOP());
      ZSETSTK(1, zacs_check_inventory(zacs_aptr(inst, ZSTK(1)), nm));
      break;
    }
    case PCD_GIVEINVENTORY:
    {
      int n = ZPOP();
      zacs_give_inventory(inst->activator, zacs_string(ZPOP()), n);
      break;
    }
    case PCD_GIVEINVENTORYDIRECT:
    {
      const char *nm = zacs_string(NEXTWORD);
      zacs_give_inventory(inst->activator, nm, NEXTWORD);
      break;
    }
    case PCD_GIVEACTORINVENTORY:
    {
      int n = ZPOP();
      const char *nm = zacs_string(ZPOP());
      int tid = ZPOP();
      if (tid == 0)
        zacs_give_inventory(inst->activator, nm, n);
      else
      {
        int sp = -1;
        mobj_t *mo;
        while ((mo = P_FindMobjFromTID((short)tid, &sp)) != NULL)
          zacs_give_inventory(mo, nm, n);
      }
      break;
    }
    case PCD_TAKEINVENTORY:
    {
      int n = ZPOP();
      zacs_take_inventory(inst->activator, zacs_string(ZPOP()), n);
      break;
    }
    case PCD_TAKEINVENTORYDIRECT:
    {
      const char *nm = zacs_string(NEXTWORD);
      zacs_take_inventory(inst->activator, nm, NEXTWORD);
      break;
    }
    case PCD_TAKEACTORINVENTORY:
    {
      int n = ZPOP();
      const char *nm = zacs_string(ZPOP());
      int tid = ZPOP();
      if (tid == 0)
        zacs_take_inventory(inst->activator, nm, n);
      else
      {
        int sp = -1;
        mobj_t *mo;
        while ((mo = P_FindMobjFromTID((short)tid, &sp)) != NULL)
          zacs_take_inventory(mo, nm, n);
      }
      break;
    }
    case PCD_CLEARINVENTORY:
      zacs_clear_inventory(inst->activator);
      break;
    case PCD_CLEARACTORINVENTORY:
    {
      int tid = ZPOP();
      int sp = -1;
      mobj_t *mo;
      if (tid == 0)
        zacs_clear_inventory(inst->activator);
      else
        while ((mo = P_FindMobjFromTID((short)tid, &sp)) != NULL)
          zacs_clear_inventory(mo);
      break;
    }
    case PCD_USEINVENTORY:
      ZSETSTK(1, 0);
      zacs_warn_pcd(pcd);
      break;
    case PCD_USEACTORINVENTORY:
      ZDROP(1);
      ZSETSTK(1, 0);
      zacs_warn_pcd(pcd);
      break;
    case PCD_CHECKWEAPON:
    {
      const char *nm = zacs_string(ZSTK(1));
      int match = 0;
      size_t i;
      if (inst->activator && inst->activator->player)
        for (i = 0; i < sizeof(zacs_weapmap)/sizeof(zacs_weapmap[0]); i++)
          if (!strcasecmp(nm, zacs_weapmap[i].name))
          {
            match = ((int)inst->activator->player->readyweapon ==
                     zacs_weapmap[i].weapon);
            break;
          }
      ZSETSTK(1, match);
      break;
    }
    case PCD_SETWEAPON:
    {
      const char *nm = zacs_string(ZSTK(1));
      int ok = 0;
      size_t i;
      if (inst->activator && inst->activator->player)
        for (i = 0; i < sizeof(zacs_weapmap)/sizeof(zacs_weapmap[0]); i++)
          if (!strcasecmp(nm, zacs_weapmap[i].name) &&
              inst->activator->player->weaponowned[zacs_weapmap[i].weapon])
          {
            inst->activator->player->pendingweapon = zacs_weapmap[i].weapon;
            ok = 1;
            break;
          }
      ZSETSTK(1, ok);
      break;
    }

    /* ---- decode-correct stubs: features without an engine counterpart ----- */
    case PCD_SETGRAVITY:        ZDROP(1); zacs_warn_pcd(pcd); break;
    case PCD_SETGRAVITYDIRECT:  (void)NEXTWORD; zacs_warn_pcd(pcd); break;
    case PCD_SETAIRCONTROL:     ZDROP(1); zacs_warn_pcd(pcd); break;
    case PCD_SETAIRCONTROLDIRECT: (void)NEXTWORD; zacs_warn_pcd(pcd); break;
    case PCD_SETFLOORTRIGGER:
    case PCD_SETCEILINGTRIGGER: ZDROP(8); zacs_warn_pcd(pcd); break;
    case PCD_FADETO:            ZDROP(5); zacs_warn_pcd(pcd); break;
    case PCD_FADERANGE:         ZDROP(9); zacs_warn_pcd(pcd); break;
    case PCD_CANCELFADE:        break;
    case PCD_PLAYMOVIE:         ZSETSTK(1, 0); zacs_warn_pcd(pcd); break;
    case PCD_SETFONT:           ZDROP(1); break;
    case PCD_SETFONTDIRECT:     (void)NEXTWORD; break;
    case PCD_SETSTYLE:          ZDROP(1); break;
    case PCD_SETSTYLEDIRECT:    (void)NEXTWORD; break;
    case PCD_SETHUDSIZE:
    {
      /* args pushed width, height, statusbar -> top-down: statusbar(1),
       * height(2), width(3).  A zero box falls back to the 320x200 default. */
      int sw = ZSTK(3), sh = ZSTK(2);
      zacs_hud_w = sw > 0 ? sw : 320;
      zacs_hud_h = sh > 0 ? sh : 200;
      ZDROP(3);
      break;
    }
    case PCD_SETMUGSHOTSTATE:   ZDROP(1); break;
    case PCD_STARTTRANSLATION:  ZDROP(1); zacs_warn_pcd(pcd); break;
    case PCD_TRANSLATIONRANGE1: ZDROP(4); break;
    case PCD_TRANSLATIONRANGE2: ZDROP(8); break;
    case PCD_TRANSLATIONRANGE3: ZDROP(8); break;
    case PCD_TRANSLATIONRANGE4: ZDROP(5); break;
    case PCD_TRANSLATIONRANGE5: ZDROP(6); break;
    case PCD_ENDTRANSLATION:    break;
    case PCD_WRITETOINI:        ZDROP(3); zacs_warn_pcd(pcd); break;
    case PCD_GETFROMINI:        ZDROP(2); ZSETSTK(1, 0); zacs_warn_pcd(pcd); break;
    case PCD_SETMARINEWEAPON:   ZDROP(2); zacs_warn_pcd(pcd); break;
    case PCD_SETMARINESPRITE:   ZDROP(2); zacs_warn_pcd(pcd); break;
    case PCD_SETCAMERATOTEXTURE: ZDROP(3); zacs_warn_pcd(pcd); break;
    case PCD_GRABINPUT:         ZDROP(2); zacs_warn_pcd(pcd); break;
    case PCD_SETMOUSEPOINTER:   ZDROP(3); zacs_warn_pcd(pcd); break;
    case PCD_MOVEMOUSEPOINTER:  ZDROP(2); zacs_warn_pcd(pcd); break;
    case PCD_CHANGELEVEL:       ZDROP(4); zacs_warn_pcd(pcd); break;
    case PCD_CONSOLECOMMAND:    ZDROP(3); zacs_warn_pcd(pcd); break;
    case PCD_CONSOLECOMMANDDIRECT:
      (void)NEXTWORD; (void)NEXTWORD; (void)NEXTWORD;
      zacs_warn_pcd(pcd);
      break;

    /* ---- builtin function dispatch (PCD_CALLFUNC) -------------------------- */
    case PCD_CALLFUNC:
    {
      int argc = NEXTBYTE;
      int func = NEXTSHORT;
      int a[8], k, r = 0;
      for (k = argc - 1; k >= 0; k--)
      {
        if (k < 8)
          a[k] = ZPOP();
        else
          ZDROP(1);
      }
      switch (func)
      {
        case 9:                           /* ACSF_GetActorVelX */
        case 10:                          /* ACSF_GetActorVelY */
        case 11:                          /* ACSF_GetActorVelZ */
        {
          mobj_t *mo = zacs_aptr(inst, argc > 0 ? a[0] : 0);
          if (mo)
            r = (func == 9) ? mo->momx : (func == 10) ? mo->momy : mo->momz;
          break;
        }
        case 78:                          /* ACSF_GetActorPowerupTics */
          r = 0;
          break;

        /* ---- DECORATE user variables ---------------------------------
         * SetUserVariable/GetUserVariable address a scalar "var int" by name;
         * SetUserArray/GetUserArray index into a "var int name[N]".  A set
         * applies to every actor matching the tid (tid 0 = activator); a get
         * reads the first match.  Names resolve through the actor type's
         * DECORATE declaration map. */
        case 24:                          /* SetUserVariable(tid,name,val) */
        case 28:                          /* SetUserArray(tid,name,idx,val) */
        {
          int tid  = (argc > 0) ? a[0] : 0;
          const char *nm = zacs_string(argc > 1 ? a[1] : 0);
          int idx  = (func == 28) ? ((argc > 2) ? a[2] : 0) : 0;
          int val  = (func == 28) ? ((argc > 3) ? a[3] : 0)
                                  : ((argc > 2) ? a[2] : 0);
          if (nm)
          {
            if (tid == 0)
            {
              int *slot = zacs_uservar_slot(inst->activator, nm, idx, true);
              if (slot) *slot = val;
            }
            else
            {
              int sp = -1; mobj_t *mo;
              while ((mo = P_FindMobjFromTID((short)tid, &sp)) != NULL)
              {
                int *slot = zacs_uservar_slot(mo, nm, idx, true);
                if (slot) *slot = val;
              }
            }
          }
          break;
        }
        case 25:                          /* GetUserVariable(tid,name) */
        case 29:                          /* GetUserArray(tid,name,idx) */
        {
          int tid  = (argc > 0) ? a[0] : 0;
          const char *nm = zacs_string(argc > 1 ? a[1] : 0);
          int idx  = (func == 29) ? ((argc > 2) ? a[2] : 0) : 0;
          mobj_t *mo = zacs_aptr(inst, tid);
          int *slot = zacs_uservar_slot(mo, nm, idx, false);
          r = slot ? *slot : 0;
          break;
        }

        /* ---- pure math ACSF ------------------------------------------
         * Self-contained numeric helpers (no map/actor state).  Sqrt is an
         * integer square root; FixedSqrt operates on 16.16 fixed-point;
         * VectorLength returns the 16.16 magnitude of a 2D fixed vector. */
        case 48:                          /* ACSF_Sqrt(n) */
        {
          unsigned int v = (argc > 0 && a[0] > 0) ? (unsigned int)a[0] : 0;
          unsigned int x = 0, b2 = 1u << 30;
          while (b2 > v) b2 >>= 2;
          while (b2)
          {
            if (v >= x + b2) { v -= x + b2; x = (x >> 1) + b2; }
            else x >>= 1;
            b2 >>= 2;
          }
          r = (int)x;
          break;
        }
        case 49:                          /* ACSF_FixedSqrt(n) */
        {
          double v = (argc > 0) ? a[0] / 65536.0 : 0.0;
          if (v < 0.0) v = 0.0;
          r = (int)(sqrt(v) * 65536.0);
          break;
        }
        case 50:                          /* ACSF_VectorLength(x, y) */
        {
          double x = (argc > 0) ? a[0] / 65536.0 : 0.0;
          double y = (argc > 1) ? a[1] / 65536.0 : 0.0;
          r = (int)(sqrt(x * x + y * y) * 65536.0);
          break;
        }

        /* ---- string ACSF family --------------------------------------
         * Self-contained string helpers the VN/script libraries rely on.
         * strcmp/stricmp compare; StrLeft/StrRight/StrMid build a new
         * string through zacs_save_string and return its index. */
        case 63:                          /* ACSF_strcmp(a, b [, n]) */
        case 64:                          /* ACSF_stricmp(a, b [, n]) */
        {
          const char *sa = zacs_string(argc > 0 ? a[0] : 0);
          const char *sb = zacs_string(argc > 1 ? a[1] : 0);
          int n = (argc > 2) ? a[2] : -1;
          if (!sa) sa = "";
          if (!sb) sb = "";
          if (func == 63)
            r = (n >= 0) ? strncmp(sa, sb, (size_t)n) : strcmp(sa, sb);
          else
            r = (n >= 0) ? strncasecmp(sa, sb, (size_t)n)
                         : strcasecmp(sa, sb);
          break;
        }
        case 65:                          /* ACSF_StrLeft(str, len) */
        case 66:                          /* ACSF_StrRight(str, len) */
        {
          const char *s = zacs_string(argc > 0 ? a[0] : 0);
          int len = (argc > 1) ? a[1] : 0;
          int slen, take;
          char buf[256];
          if (!s) s = "";
          slen = (int)strlen(s);
          if (len < 0) len = 0;
          take = (len < slen) ? len : slen;
          if (take > (int)sizeof(buf) - 1) take = (int)sizeof(buf) - 1;
          if (func == 65)                  /* leftmost `take` chars */
            memcpy(buf, s, (size_t)take);
          else                             /* rightmost `take` chars */
            memcpy(buf, s + (slen - take), (size_t)take);
          buf[take] = 0;
          r = zacs_save_string(buf);
          break;
        }
        case 67:                          /* ACSF_StrMid(str, pos, len) */
        {
          const char *s = zacs_string(argc > 0 ? a[0] : 0);
          int pos = (argc > 1) ? a[1] : 0;
          int len = (argc > 2) ? a[2] : 0;
          int slen, take;
          char buf[256];
          if (!s) s = "";
          slen = (int)strlen(s);
          if (pos < 0) pos = 0;
          if (len < 0) len = 0;
          if (pos > slen) pos = slen;
          take = (pos + len > slen) ? (slen - pos) : len;
          if (take < 0) take = 0;
          if (take > (int)sizeof(buf) - 1) take = (int)sizeof(buf) - 1;
          memcpy(buf, s + pos, (size_t)take);
          buf[take] = 0;
          r = zacs_save_string(buf);
          break;
        }
        default:
          if (func >= 0 && func < 512 &&
              !(zacs_cf_warned[func >> 3] & (1 << (func & 7))))
          {
            zacs_cf_warned[func >> 3] |= (byte)(1 << (func & 7));
            lprintf(LO_WARN, "ZACS: builtin function %d not supported "
                    "(returns 0)\n", func);
          }
          break;
      }
      ZPUSH(r);
      break;
    }

    default:
      /* every pcode above 386 is invalid; everything inside the table is
       * handled.  Reaching here means a case is genuinely missing. */
      zacs_warn_pcd(pcd);
      zacs_finish(inst);
      return;
    }
  }

  inst->ip = ip;
  if (inst->state == ZSTATE_DONE)
    return;
}
