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
#include <ctype.h>

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
#include "u_zmapinfo.h"
#include "z_zone.h"
#include "hexen/p_spec_hexen.h"
#include "hu_lib.h"
#include "hu_stuff.h"
#include "p_zacs.h"
#include "u_png.h"

/* Vector kernels for the full-colour overlay blit's blend (the hot path: the
 * per-pixel 8-bit channel LERP dominates, while the strided source gather is
 * cheap, so a batch is gathered scalar then blended eight-wide).  Same guard
 * shape as the renderer's column blenders. */
#if defined(__SSE2__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2) || defined(_M_X64)
#define ZACS_BLIT_SSE2 1
#include <emmintrin.h>
#elif defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(_M_ARM64)
#define ZACS_BLIT_NEON 1
#include <arm_neon.h>
#endif

#if defined(ZACS_BLIT_SSE2) || defined(ZACS_BLIT_NEON)
#if defined(_MSC_VER)
#define ZACS_ALIGN16 __declspec(align(16))
#else
#define ZACS_ALIGN16 __attribute__((aligned(16)))
#endif
#endif

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
/* Dynamic strings created at run time by PCD_SAVESTRING and the string-
 * building ACSF helpers live in one global table, independent of the active
 * module's static STRL space.  GDCC-compiled libraries build a string in one
 * module and consume it inside a cross-module call (e.g. a mod saves a key in
 * its own module, then PrintLocalized reads it while libc is the active
 * module); a per-module string table would lose the handle across that
 * boundary.  Returned indices carry ZACS_DYNSTR_TAG so zacs_string can tell a
 * dynamic handle from a module-local STRL index. */
#define ZACS_DYNSTR_TAG 0x40000000
static char         **zacs_dynstrings;
static int            zacs_numdynstrings;
static int            zacs_dynstrings_cap;
static char         **zacs_snames;       /* named-script names */
static int            zacs_numsnames;
static char         **zacs_fnames;       /* FNAM: function names (for linking) */
static int            zacs_numfnames;

/* ---- VM variable storage ------------------------------------------------ */

/* An ACS array is a dense vector for the ordinary small-index region that all
 * hand-written content uses.  Objects compiled from a higher-level language
 * (GDCC) instead treat a single global array as a flat 32-bit address space,
 * indexing it with values across the whole signed range (a stack near the top,
 * heap and statics elsewhere).  Those indices cannot back a dense vector, so
 * any index outside the dense window falls through to an open-addressing hash
 * side table.  Dense storage keeps the common path bit-for-bit unchanged. */
typedef struct {
  int  key;            /* array index */
  int  val;
  int  used;
} zacs_sparse_ent_t;

typedef struct
{
  int  *data;
  int   size;          /* dense window length (indices 0 .. size-1)        */
  zacs_sparse_ent_t *sparse;   /* hash table for out-of-window indices      */
  int   sparse_cap;            /* power-of-two capacity, 0 = none allocated */
  int   sparse_used;
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
/* Largest index served from the dense vector.  Indices in [0, this] grow the
 * dense array as before (so ordinary content is unchanged); anything outside,
 * including the high/negative addresses GDCC uses, goes to the hash table. */
#define ZACS_ARR_DENSE_MAX 0xFFFF

static unsigned zacs_sparse_hash(int key)
{
  unsigned h = (unsigned)key;
  h ^= h >> 16; h *= 0x7feb352dU; h ^= h >> 15; h *= 0x846ca68bU; h ^= h >> 16;
  return h;
}

/* Find the slot for key (open addressing, linear probe).  Returns the slot to
 * use; *found tells whether it already holds key.  Caller guarantees capacity. */
static int zacs_sparse_slot(zacs_array_t *a, int key, int *found)
{
  unsigned mask = (unsigned)a->sparse_cap - 1u;
  unsigned i = zacs_sparse_hash(key) & mask;
  for (;;)
  {
    if (!a->sparse[i].used) { *found = 0; return (int)i; }
    if (a->sparse[i].key == key) { *found = 1; return (int)i; }
    i = (i + 1) & mask;
  }
}

static void zacs_sparse_grow(zacs_array_t *a)
{
  int oldcap = a->sparse_cap, i, ncap;
  zacs_sparse_ent_t *old = a->sparse;
  ncap = oldcap ? oldcap * 2 : 64;
  a->sparse = calloc((size_t)ncap, sizeof(*a->sparse));
  a->sparse_cap = ncap;
  a->sparse_used = 0;
  if (old)
  {
    for (i = 0; i < oldcap; i++)
      if (old[i].used)
      {
        int f, s = zacs_sparse_slot(a, old[i].key, &f);
        a->sparse[s].key = old[i].key;
        a->sparse[s].val = old[i].val;
        a->sparse[s].used = 1;
        a->sparse_used++;
      }
    free(old);
  }
}

static int zacs_arr_get(zacs_array_t *a, int ix)
{
  if (ix >= 0 && ix < a->size)
    return a->data[ix];
  if (a->sparse_cap)
  {
    int f, s = zacs_sparse_slot(a, ix, &f);
    if (f)
      return a->sparse[s].val;
  }
  return 0;
}

static void zacs_arr_set(zacs_array_t *a, int ix, int v)
{
  if (ix >= 0 && ix <= ZACS_ARR_DENSE_MAX)
  {
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
    return;
  }
  /* out-of-window index (e.g. a GDCC flat-memory address): hash side table */
  {
    int f, s;
    if (a->sparse_used * 4 >= a->sparse_cap * 3)   /* keep load factor < 0.75 */
      zacs_sparse_grow(a);
    s = zacs_sparse_slot(a, ix, &f);
    if (!f)
    {
      a->sparse[s].key = ix;
      a->sparse[s].used = 1;
      a->sparse_used++;
    }
    a->sparse[s].val = v;
  }
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
  int        result;       /* PCD_SETRESULTVALUE channel (for sync CallACS) */
} zacs_inst_t;

/* per-script activity flags, parallel to zacs_scripts */
static byte *zacs_running;

/* warn-once bitmap for unimplemented opcodes / callfuncs */
static byte zacs_warned[(ZACS_NUM_PCODES + 7) / 8];
static byte zacs_cf_warned[64];          /* callfunc ids 0..511 */

/* ---- CVARINFO-defined console variables ---------------------------------
 *
 * ZDoom mods declare custom cvars in a CVARINFO lump and read them from ACS
 * with GetCVar; the hdoom death system gates on HDoomDoXDeath /
 * HDoomXDeathChance this way.  This engine has no console, so the values are
 * just the declared defaults: parse the lump once into a name->value table and
 * answer GetCVar from it.  bool defaults map to 0/1, int/fixed take the
 * literal; strings and unparseable values read as 0. */
typedef struct { char name[40]; int value; } zacs_cvar_t;
static zacs_cvar_t *zacs_cvars;
static int          zacs_numcvars;
static int          zacs_cvars_parsed;

static void zacs_parse_cvarinfo(void)
{
  int lump, len, i, cap = 0;
  const char *txt;
  zacs_cvars_parsed = 1;
  lump = W_CheckNumForName("CVARINFO");
  if (lump < 0)
    return;
  len = W_LumpLength(lump);
  txt = (const char *)W_CacheLumpNum(lump);
  i = 0;
  while (i < len)
  {
    char word[40];
    int  n, isbool = 0;
    /* skip whitespace and // line comments */
    while (i < len && (txt[i] == ' ' || txt[i] == '\t' ||
                       txt[i] == '\r' || txt[i] == '\n'))
      i++;
    if (i + 1 < len && txt[i] == '/' && txt[i + 1] == '/')
    {
      while (i < len && txt[i] != '\n') i++;
      continue;
    }
    /* a declaration is "<scope> <type> <name> [= <value>] ;"; read words to
     * the name, noting a bool type, then take the value after '=' */
    /* scope word */
    n = 0;
    while (i < len && txt[i] > ' ' && n < 39) word[n++] = txt[i++];
    word[n] = 0;
    if (!word[0]) { i++; continue; }
    /* type word */
    while (i < len && (txt[i] == ' ' || txt[i] == '\t')) i++;
    n = 0;
    while (i < len && txt[i] > ' ' && n < 39) word[n++] = txt[i++];
    word[n] = 0;
    if (!strcasecmp(word, "bool")) isbool = 1;
    /* name word */
    while (i < len && (txt[i] == ' ' || txt[i] == '\t')) i++;
    n = 0;
    while (i < len && (txt[i] == '_' ||
                       (txt[i] >= 'A' && txt[i] <= 'Z') ||
                       (txt[i] >= 'a' && txt[i] <= 'z') ||
                       (txt[i] >= '0' && txt[i] <= '9')) && n < 39)
      word[n++] = txt[i++];
    word[n] = 0;
    if (!word[0]) { while (i < len && txt[i] != '\n') i++; continue; }
    {
      int value = 0;
      /* optional "= value" */
      while (i < len && (txt[i] == ' ' || txt[i] == '\t')) i++;
      if (i < len && txt[i] == '=')
      {
        char vbuf[40];
        int vn = 0;
        i++;
        while (i < len && (txt[i] == ' ' || txt[i] == '\t')) i++;
        while (i < len && txt[i] != ';' && txt[i] != '\n' &&
               txt[i] != ' ' && txt[i] != '\t' && vn < 39)
          vbuf[vn++] = txt[i++];
        vbuf[vn] = 0;
        if (isbool)
          value = !strcasecmp(vbuf, "true") ? 1 : 0;
        else
          value = atoi(vbuf);
      }
      /* skip to end of statement */
      while (i < len && txt[i] != ';' && txt[i] != '\n') i++;
      /* record */
      if (zacs_numcvars == cap)
      {
        int nc = cap ? cap * 2 : 16;
        zacs_cvar_t *np = realloc(zacs_cvars, (size_t)nc * sizeof(*np));
        if (!np) return;
        zacs_cvars = np; cap = nc;
      }
      strncpy(zacs_cvars[zacs_numcvars].name, word, 39);
      zacs_cvars[zacs_numcvars].name[39] = 0;
      zacs_cvars[zacs_numcvars].value = value;
      zacs_numcvars++;
    }
  }
  lprintf(LO_INFO, "Z_ACS: %d CVARINFO cvar(s) loaded\n", zacs_numcvars);
}

static int zacs_cvar_value(const char *name)
{
  int i;
  if (!zacs_cvars_parsed)
    zacs_parse_cvarinfo();
  if (!name)
    return 0;
  for (i = 0; i < zacs_numcvars; i++)
    if (!strcasecmp(zacs_cvars[i].name, name))
      return zacs_cvars[i].value;
  return 0;
}

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
  if (i & ZACS_DYNSTR_TAG)           /* run-time dynamic string (global) */
  {
    int d = i & ~ZACS_DYNSTR_TAG;
    if (d < 0 || d >= zacs_numdynstrings || !zacs_dynstrings[d])
      return "";
    return zacs_dynstrings[d];
  }
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
  zacs_dynstrings = NULL;
  zacs_numdynstrings = 0;
  zacs_dynstrings_cap = 0;
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

/* True if any root LOADACS global ACS libraries are present.  A stock
 * Doom-format map has no BEHAVIOR of its own, but if a mod ships global ACS
 * libraries (hdoom's death system), they still need to be loaded and run so
 * their scripts are reachable. */
dbool Z_ACSHasGlobalLibs(void)
{
  return zacs_num_global_libs > 0;
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

  /* ---- module 0: the map's BEHAVIOR -------------------------------------
   * lump < 0 means the map has no BEHAVIOR of its own (a stock Doom-format
   * map running under a mod whose scripts live entirely in global LOADACS
   * libraries -- e.g. hdoom's death system).  Skip the map module and let the
   * first global library take module slot 0 so its scripts still aggregate
   * and run; without this the global scripts load but stay unreachable. */
  if (lump >= 0)
  {
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
  if (zacs_nummodules == 0)
    return false;                /* nothing loaded (no map BEHAVIOR, no libs) */
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

static int zacs_save_string(const char *s)
{
  int   n = strlen(s);
  char *copy = Z_Malloc(n + 1, PU_LEVEL, 0);
  memcpy(copy, s, n + 1);
  if (zacs_numdynstrings >= zacs_dynstrings_cap)
  {
    int nc = zacs_dynstrings_cap ? zacs_dynstrings_cap * 2 : 64;
    char **ns;
    if (nc < zacs_numdynstrings + 1)
      nc = zacs_numdynstrings + 64;
    ns = Z_Calloc(nc, sizeof(char *), PU_LEVEL, 0);
    if (zacs_dynstrings)
      memcpy(ns, zacs_dynstrings, zacs_numdynstrings * sizeof(char *));
    zacs_dynstrings = ns;
    zacs_dynstrings_cap = nc;
  }
  zacs_dynstrings[zacs_numdynstrings] = copy;
  return ZACS_DYNSTR_TAG | zacs_numdynstrings++;
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

/* Per-player latch for the use key as seen by GetPlayerInput(INPUT_BUTTONS).
 * Dialogue advance reads the live button state as a level, so a held use key
 * reads as pressed every tic and walks the script past its end and back to the
 * first line.  Latch the key so a continuous hold reports as a single press:
 * report use only on the down edge and then suppress it until the key is let
 * go.  Edge tracking via INPUT_OLDBUTTONS is left untouched. */
static int zacs_use_latched[MAXPLAYERS];

/* Dialogue loop guard.  Some content's conversation scripts, when they reach
 * their final line, jump back to the opening line instead of terminating, so
 * the player is held frozen forever and replays the same lines.  Two defences
 * cover this:
 *
 *  - Wrap detection: no legitimate conversation returns to its very first line
 *    after showing later ones, so the first line is remembered and a return to
 *    it once other lines have appeared ends the conversation.
 *
 *  - Stall watchdog: a conversation that holds the player frozen but stops
 *    showing new lines (stuck repeating one line, or waiting on input the
 *    script can no longer consume) is force-ended after a long grace period.
 *    Every genuinely new line resets the timer, so an attentive reader is
 *    never cut off; only a wedged conversation reaches the limit.
 *
 * On either trigger the freeze is lifted and dialogue is suppressed until the
 * next conversation starts.  zacs_dlg_active is zero between conversations, so
 * the stored line re-arms each time a conversation begins. */
#define ZACS_DLG_FIRSTLEN 96
#define ZACS_DLG_LASTLEN  96
#define ZACS_DLG_STALL_TICS 350   /* ~10s with no new line => wedged */
#define ZACS_DLG_MINLINE  10      /* a spoken line is longer than a label */
static char zacs_dlg_first[ZACS_DLG_FIRSTLEN];
static char zacs_dlg_last[ZACS_DLG_LASTLEN];
static int  zacs_dlg_active;     /* a conversation is in progress */
static int  zacs_dlg_advanced;   /* a line past the first has been shown */
static int  zacs_dlg_break;      /* wrap detected: suppress dialogue HUD */
static int  zacs_dlg_stall;      /* tics since the last new line while frozen */
static int  zacs_dlg_textid;     /* HUD id carrying the spoken line (0 = none) */

/* Forward decl: clearing the conversation freeze lives below. */
static void zacs_clear_conversation_freeze(void);

static int zacs_oldbuttons_of(player_t *pl)
{
  int i;
  for (i = 0; i < MAXPLAYERS; i++)
    if (&players[i] == pl)
      return zacs_oldbuttons[i];
  return 0;
}

/* Resolve a player to its index, or -1 if not an in-game player. */
static int zacs_player_index(player_t *pl)
{
  int i;
  for (i = 0; i < MAXPLAYERS; i++)
    if (&players[i] == pl)
      return i;
  return -1;
}

/* Lift the dialogue freeze from every player and reset the dialogue-loop
 * guard.  Used when a conversation ends normally and when the loop guard
 * detects a wrap so the player is never left frozen in place. */
static void zacs_clear_conversation_freeze(void)
{
  int i;
  for (i = 0; i < MAXPLAYERS; i++)
    if (playeringame[i])
      players[i].cheats &= ~CF_TOTALLYFROZEN;
  zacs_dlg_active   = 0;
  zacs_dlg_advanced = 0;
  zacs_dlg_first[0] = 0;
  zacs_dlg_last[0]  = 0;
  zacs_dlg_stall    = 0;
  zacs_dlg_textid   = 0;
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
  int   x, y;          /* raw HUD coords (whole units) within hbox            */
  int   hbox_w, hbox_h;/* SetHudSize box this message was placed in           */
  int   holdtics;      /* remaining tics; <0 = until replaced                 */
  int   color;         /* CR_* range index, or -1 for default                 */
  int   centered;      /* draw centered on (x,y) (graphics) vs left-aligned   */
  int   fadein, fadeout;/* fade ramp lengths in tics (0 = none)               */
  int   fadetotal;     /* total tics for fade messages                        */
  int   age;           /* tics elapsed since the message was (re)stored       */
  int   typeon;        /* typewriter reveal: tics per character, 0 = off      */
  int   graphic_lump;  /* >=0: draw this patch (a font that is one image)     */
  unsigned drawn_tick; /* drawer pass bookkeeping for id-ordered compositing  */
  char  font[32];      /* current font name when stored                       */
  char  text[ZACS_PRINTBUF];
} zacs_hudmsg_t;
static zacs_hudmsg_t zacs_hudmsgs[ZACS_HUDMSG_MAX];
static unsigned zacs_draw_tick;                  /* bumped each Z_ACSHudDrawer */
static int zacs_hud_w = 320, zacs_hud_h = 200;   /* current SetHudSize box   */
static char zacs_cur_font[32];                   /* current SetFont selection */

/* the status-bar font, defined in hu_stuff.c (no public extern there) */
extern patchnum_t hu_font[HU_FONTSIZE];

/* CR_* palette translation tables, defined in v_video.c; used to recolour the
 * scaled dialogue-text glyphs to the requested text colour. */
extern const uint8_t *colrngs[CR_LIMIT];

/* Resolve a font/graphic name to a drawable patch lump, or -1 if it is a real
 * text font (or unknown).  Story content selects a portrait or the dialogue
 * frame by SetFont()-ing a single-image "font" and printing one character, so
 * a name that matches a graphic lump is drawn as that patch.  Genuine text
 * fonts (SMALLFONT, the engine's own) return -1 and are drawn as glyph text. */
static int zacs_font_graphic(const char *font)
{
  int l;
  if (!font || !font[0])
    return -1;
  if (!strcasecmp(font, "SMALLFONT") || !strcasecmp(font, "BIGFONT") ||
      !strcasecmp(font, "CONFONT"))
    return -1;
  l = (W_CheckNumForName)(font, ns_global);
  if (l < 0)
    l = (W_CheckNumForName)(font, ns_sprites);
  return l;
}

/* Register/replace a positioned, timed hud message. */
/* Translate a ZDoom text-color index (as passed to HudMessage) to this
 * engine's CR_* translation index.  The two enumerations agree from 0 (brick)
 * through 8 (orange) but diverge after: ZDoom 9 is white and 10 is yellow,
 * whereas this engine has yellow at 9 and no white.  Map ZDoom white to the
 * engine's nearest light shade and yellow to the engine's yellow.  ZDoom -1
 * (CR_UNTRANSLATED) means "draw the font in its own colours" -- the dialogue
 * name and SPEAKING labels rely on this to show the font's native red -- so it
 * returns CR_LIMIT as a sentinel the drawer reads as "no recolour". */
static int zacs_zdoom_color(int zc)
{
  switch (zc)
  {
    case 0:  return CR_BRICK;
    case 1:  return CR_TAN;
    case 2:  return CR_GRAY;
    case 3:  return CR_GREEN;
    case 4:  return CR_BROWN;
    case 5:  return CR_GOLD;
    case 6:  return CR_RED;
    case 7:  return CR_BLUE;
    case 8:  return CR_ORANGE;
    case 9:  return CR_GRAY;     /* ZDoom CR_WHITE -> nearest light shade */
    case 10: return CR_YELLOW;   /* ZDoom CR_YELLOW */
    default: return CR_LIMIT;    /* untranslated / unknown: keep native font colour */
  }
}

static void zacs_hud_store(const char *text, int id, int x, int y,
                           int holdtics, int color, int type,
                           int fadein, int fadeout, int typeon)
{
  int i, slot = -1;
  /* Dialogue loop guard.  A conversation posts several HUD messages each tic
   * under different ids: a one- or two-character portrait, the speaker name,
   * a "SPEAKING" label, and the spoken line itself.  Only the spoken line
   * advances as the player presses use, so the guard must follow that one id
   * and ignore the fixed labels -- otherwise the short labels look like a
   * conversation wrapping back to its first line and trip the guard at once.
   *
   * The spoken line is identified as the first substantial (multi-word) text
   * posted while the player is frozen; its id is then locked for the rest of
   * the conversation.  Only that id feeds wrap detection and the stall timer. */
  {
    int frozen = 0;
    for (i = 0; i < MAXPLAYERS; i++)
      if (playeringame[i] && (players[i].cheats & CF_TOTALLYFROZEN))
      { frozen = 1; break; }
    if (frozen && text && text[0] && id != 0)
    {
      /* Lock onto the spoken-line id: the first frozen text long enough to be
       * a sentence rather than a portrait glyph or short label. */
      if (zacs_dlg_textid == 0 && strlen(text) >= ZACS_DLG_MINLINE)
        zacs_dlg_textid = id;

      if (id == zacs_dlg_textid)
      {
        if (!zacs_dlg_active)
        {
          /* First spoken line of a new conversation: remember it and re-arm. */
          size_t n = strlen(text);
          if (n > ZACS_DLG_FIRSTLEN - 1) n = ZACS_DLG_FIRSTLEN - 1;
          memcpy(zacs_dlg_first, text, n);
          zacs_dlg_first[n] = 0;
          zacs_dlg_active   = 1;
          zacs_dlg_advanced = 0;
          zacs_dlg_break    = 0;
        }
        else if (!zacs_dlg_advanced)
        {
          /* Still on the opening line until a different line appears. */
          if (strcmp(text, zacs_dlg_first) != 0)
            zacs_dlg_advanced = 1;
        }
        else if (strcmp(text, zacs_dlg_first) == 0)
        {
          /* The opening line returned after the conversation moved on: the
           * script has wrapped.  End the conversation and drop this line. */
          zacs_dlg_break = 1;
          zacs_clear_conversation_freeze();
          return;
        }
        /* A genuinely new spoken line means progress: reset the stall timer.
         * Re-posting the same line (the script's per-tic redraw) lets the
         * timer run on toward the wedged-conversation limit. */
        if (strcmp(text, zacs_dlg_last) != 0)
        {
          size_t n = strlen(text);
          if (n > ZACS_DLG_LASTLEN - 1) n = ZACS_DLG_LASTLEN - 1;
          memcpy(zacs_dlg_last, text, n);
          zacs_dlg_last[n] = 0;
          zacs_dlg_stall   = 0;
        }
      }
    }
    else if (!frozen)
    {
      /* No conversation in progress: clear any lingering suppression so the
       * next conversation starts fresh. */
      zacs_dlg_break  = 0;
      zacs_dlg_active = 0;
      zacs_dlg_textid = 0;
    }
    if (zacs_dlg_break && id == zacs_dlg_textid)
      return;                         /* keep suppressing the wrapped replay */
  }
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
  zacs_hudmsgs[slot].hbox_w   = zacs_hud_w;
  zacs_hudmsgs[slot].hbox_h   = zacs_hud_h;
  zacs_hudmsgs[slot].holdtics = holdtics;
  zacs_hudmsgs[slot].color    = color;
  zacs_hudmsgs[slot].graphic_lump = zacs_font_graphic(zacs_cur_font);
  zacs_hudmsgs[slot].centered = (zacs_hudmsgs[slot].graphic_lump >= 0);
  zacs_hudmsgs[slot].fadein   = fadein;
  zacs_hudmsgs[slot].fadeout  = fadeout;
  zacs_hudmsgs[slot].typeon   = typeon;
  zacs_hudmsgs[slot].age      = 0;
  /* For a fade message, hold for the requested time plus the ramps so the
   * out-ramp is visible; a 0 hold with fades still needs the ramps to play. */
  if ((type & 3) && (fadein || fadeout))
  {
    int hold = holdtics > 0 ? holdtics : 0;
    zacs_hudmsgs[slot].fadetotal = fadein + hold + fadeout;
    zacs_hudmsgs[slot].holdtics  = zacs_hudmsgs[slot].fadetotal;
  }
  else
    zacs_hudmsgs[slot].fadetotal = 0;
  {
    size_t n = strlen(zacs_cur_font);
    if (n > sizeof(zacs_hudmsgs[slot].font) - 1)
      n = sizeof(zacs_hudmsgs[slot].font) - 1;
    memcpy(zacs_hudmsgs[slot].font, zacs_cur_font, n);
    zacs_hudmsgs[slot].font[n] = 0;
  }
  {
    size_t n = strlen(text);
    if (n > ZACS_PRINTBUF - 1)
      n = ZACS_PRINTBUF - 1;
    memcpy(zacs_hudmsgs[slot].text, text, n);
    zacs_hudmsgs[slot].text[n] = 0;
  }
}

static void zacs_hud_clear(int id)
{
  int i;
  for (i = 0; i < ZACS_HUDMSG_MAX; i++)
    if (zacs_hudmsgs[i].active && zacs_hudmsgs[i].id == id)
      zacs_hudmsgs[i].active = 0;
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

/* True if a named script currently has at least one running (or suspended)
 * instance.  Used to stop a use-activated story actor from spawning a second
 * overlapping copy of its conversation: the actor's freeze flag is only set a
 * few tics into the conversation script, so a guard that waits for the freeze
 * leaves a window where a mashed use re-fires the start action and stacks a
 * duplicate controller, wedging the dialogue.  Checking the running count
 * closes that window on the same tic the controller spawns. */
dbool Z_ACSNamedRunning(const char *name)
{
  int info;
  if (!zacs_numscripts)
    return false;
  info = zacs_named_index(name);
  if (info < 0 || info >= zacs_numscripts)
    return false;
  return zacs_running[info] != 0;
}

/* Run a named script to completion right now and return the value it set with
 * SetResultValue.  DECORATE's CallACS() form needs a script that behaves like
 * a function -- the hdoom death states read GetXDeathChance / GetDoXDeath and
 * trigger SpawnSexActor this way.  The scripts these call do no Delay, so the
 * freshly spawned instance executes fully within one thinker slice; we read
 * its result and let the (now finished) instance be reaped on the next pass.
 * A script that yields would not have completed -- result is read regardless,
 * which matches how such a script's caller treats an unfinished helper. */
int Z_ACSCallNamedSync(const char *name, mobj_t *activator)
{
  int info, result;
  zacs_inst_t *inst;
  if (!zacs_numscripts)
    return 0;
  info = zacs_named_index(name);
  if (info < 0 || info >= zacs_numscripts)
    return 0;
  inst = zacs_spawn(info, NULL, 0, activator, NULL, 0);
  if (!inst)
    return 0;
  inst->result = 0;
  T_ZACSThinker(inst);            /* runs to completion (no Delay in callees) */
  result = inst->result;
  return result;
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
  /* Dialogue stall watchdog: while a player is held frozen by a conversation,
   * count tics since the last new line.  A conversation that wedges without
   * showing anything new for the grace period is force-ended so the player is
   * never stuck in place (and cannot reach nearby doors).  Any new line resets
   * the count in zacs_hud_store, so this never fires on a conversation that is
   * still progressing. */
  {
    int frozen = 0;
    for (i = 0; i < MAXPLAYERS; i++)
      if (playeringame[i] && (players[i].cheats & CF_TOTALLYFROZEN))
      { frozen = 1; break; }
    if (frozen && zacs_dlg_active)
    {
      if (++zacs_dlg_stall >= ZACS_DLG_STALL_TICS)
        zacs_clear_conversation_freeze();
    }
    else if (!frozen)
      zacs_dlg_stall = 0;
  }
  for (i = 0; i < ZACS_HUDMSG_MAX; i++)
    if (zacs_hudmsgs[i].active)
    {
      zacs_hudmsgs[i].age++;
      if (zacs_hudmsgs[i].holdtics >= 0)
        if (--zacs_hudmsgs[i].holdtics < 0)
          zacs_hudmsgs[i].active = 0;
    }
}

/* Clear all hud messages (level start / reset). */
void Z_ACSHudClear(void)
{
  int i;
  for (i = 0; i < ZACS_HUDMSG_MAX; i++)
    zacs_hudmsgs[i].active = 0;
  for (i = 0; i < MAXPLAYERS; i++)
    zacs_use_latched[i] = 0;
  zacs_dlg_active   = 0;
  zacs_dlg_advanced = 0;
  zacs_dlg_break    = 0;
  zacs_dlg_first[0] = 0;
  zacs_dlg_last[0]  = 0;
  zacs_dlg_stall    = 0;
  zacs_dlg_textid   = 0;
  zacs_hud_w = 320;
  zacs_hud_h = 200;
}

/* Draw the live hud messages.  Called from the HUD drawer; uses the small
 * status-bar font via a transient text line so colour escapes and the missing
 * -glyph fallbacks are handled exactly as the message widget's are. */
/* Draw an indexed patch scaled into a destination rectangle on the RGB565
 * surface, with a global alpha (0..256) for fade.  The HUD overlay places its
 * portrait and frame art in a virtual box (SetHudSize) much larger than the
 * 320x200 page, so the art is point-sampled down to the on-screen rect rather
 * than drawn at the renderer's fixed page scale.  Transparent patch posts are
 * skipped, so the source's own cutout is preserved.
 *
 * This is the inner pixel loop of the dialogue overlay: for every output pixel
 * it maps back to a source texel, looks the palette colour up, and (when not
 * opaque) blends 5/6/5 channels against the destination.  It is deliberately
 * written as a tight per-row scan so the blend can later be vectorised. */
/* Indexed-patch fallback for HUD art with no native-colour copy: scales the
 * PLAYPAL-mapped patch into the destination rect with a global alpha, walking
 * the column posts for opacity.  Used only when U_PNGCacheRGBA has nothing. */
static void zacs_blit_scaled_indexed(int lump, int dx, int dy, int dw, int dh,
                                     int alpha)
{
  const rpatch_t *patch;
  uint16_t *surf;
  int ox, oy, sw, sh;
  int x0, x1, y0, y1;
  int sx_step, sy_step, sx_start;
  if (lump < 0 || dw <= 0 || dh <= 0 || alpha <= 0)
    return;
  patch = R_CachePatchNum(lump);
  sw = patch->width;
  sh = patch->height;
  if (sw <= 0 || sh <= 0)
  {
    R_UnlockPatchNum(lump);
    return;
  }
  surf = (uint16_t *)screens[0].data;
  sx_step = (sw << 16) / dw;
  sy_step = (sh << 16) / dh;
  x0 = dx < 0 ? 0 : dx;
  y0 = dy < 0 ? 0 : dy;
  x1 = dx + dw; if (x1 > SCREENWIDTH)  x1 = SCREENWIDTH;
  y1 = dy + dh; if (y1 > SCREENHEIGHT) y1 = SCREENHEIGHT;
  sx_start = (x0 - dx) * sx_step;

  for (oy = y0; oy < y1; oy++)
  {
    int srcy = ((oy - dy) * sy_step) >> 16;
    uint16_t *row = surf + (size_t)oy * SURFACE_SHORT_PITCH;
    int sx = sx_start;
    if (srcy < 0) srcy = 0; else if (srcy >= sh) srcy = sh - 1;
    for (ox = x0; ox < x1; ox++, sx += sx_step)
    {
      int srcx = sx >> 16;
      const rcolumn_t *col = R_GetPatchColumn(patch, srcx);
      int i, idx = -1;
      for (i = 0; i < col->numPosts; i++)
      {
        const rpost_t *post = &col->posts[i];
        if (srcy >= post->topdelta && srcy < post->topdelta + post->length)
        {
          idx = col->pixels[srcy];
          break;
        }
      }
      if (idx < 0)
        continue;
      {
        uint16_t s = VID_PAL16((unsigned char)idx, VID_NUMCOLORWEIGHTS - 1);
        if (alpha >= 256)
          row[ox] = s;
        else
        {
          uint16_t d = row[ox];
          int sr = (s >> 11) & 0x1f, sg = (s >> 5) & 0x3f, sb = s & 0x1f;
          int dr = (d >> 11) & 0x1f, dg = (d >> 5) & 0x3f, db = d & 0x1f;
          int ia = 256 - alpha;
          int rr = (sr * alpha + dr * ia) >> 8;
          int rg = (sg * alpha + dg * ia) >> 8;
          int rb = (sb * alpha + db * ia) >> 8;
          row[ox] = (uint16_t)((rr << 11) | (rg << 5) | rb);
        }
      }
    }
  }
  R_UnlockPatchNum(lump);
}

/* Draw a font-glyph patch scaled into a destination rectangle, recolouring
 * each source palette index through a CR_* translation table.  Used by the
 * scaled dialogue-text path so a line authored for the conversation's hud box
 * renders at the box's proportion (rather than the engine's fixed 320-wide
 * text scale) and in the dialogue colour.  trans may be NULL for no recolour. */
static void zacs_blit_glyph_scaled(int lump, int dx, int dy, int dw, int dh,
                                   const uint8_t *trans)
{
  const rpatch_t *patch;
  uint16_t *surf;
  int ox, oy, sw, sh;
  int x0, x1, y0, y1;
  int sx_step, sy_step, sx_start;
  if (lump <= 0 || dw <= 0 || dh <= 0)
    return;
  patch = R_CachePatchNum(lump);
  sw = patch->width;
  sh = patch->height;
  if (sw <= 0 || sh <= 0)
  {
    R_UnlockPatchNum(lump);
    return;
  }
  surf = (uint16_t *)screens[0].data;
  sx_step = (sw << 16) / dw;
  sy_step = (sh << 16) / dh;
  x0 = dx < 0 ? 0 : dx;
  y0 = dy < 0 ? 0 : dy;
  x1 = dx + dw; if (x1 > SCREENWIDTH)  x1 = SCREENWIDTH;
  y1 = dy + dh; if (y1 > SCREENHEIGHT) y1 = SCREENHEIGHT;
  sx_start = (x0 - dx) * sx_step;

  for (oy = y0; oy < y1; oy++)
  {
    int srcy = ((oy - dy) * sy_step) >> 16;
    uint16_t *row = surf + (size_t)oy * SURFACE_SHORT_PITCH;
    int sx = sx_start;
    if (srcy < 0) srcy = 0; else if (srcy >= sh) srcy = sh - 1;
    for (ox = x0; ox < x1; ox++, sx += sx_step)
    {
      int srcx = sx >> 16;
      const rcolumn_t *col = R_GetPatchColumn(patch, srcx);
      int i, idx = -1;
      for (i = 0; i < col->numPosts; i++)
      {
        const rpost_t *post = &col->posts[i];
        if (srcy >= post->topdelta && srcy < post->topdelta + post->length)
        {
          idx = col->pixels[srcy];
          break;
        }
      }
      if (idx < 0)
        continue;
      if (trans)
        idx = trans[(unsigned char)idx];
      row[ox] = VID_PAL16((unsigned char)idx, VID_NUMCOLORWEIGHTS - 1);
    }
  }
  R_UnlockPatchNum(lump);
}

/* Draw a HUD image scaled into a destination rectangle on the surface, with a
 * global fade alpha (0..256).  Full-colour art (portraits, the dialogue frame)
 * is blitted from its native 0xAABBGGRR copy: the blend runs at 8-bit channel
 * precision and is narrowed to the surface format only at store time, so the
 * result keeps the source colour instead of the 256-colour palette
 * approximation, and per-texel source alpha gives soft edges rather than a
 * 1-bit cut.  Art without a native copy falls back to the indexed patch.
 *
 * The destination is source-stepped with 16.16 fixed point so the inner loop
 * carries no divides; this is the overlay's hot pixel loop. */
/* ----------------------------------------------------------------------------
 * Scaled-portrait cache.
 *
 * A dialogue portrait is a still image: it changes only when the script
 * advances and uploads a different graphic, not every tic.  Re-running the
 * 1600x900 -> screen-rect resample (a strided source gather plus the fade
 * fold) 35 times a second for an image that has not changed is the dominant
 * cost of the overlay -- ~13 ms a tic at 4K.  So resample once into a
 * contiguous screen-space ARGB buffer, keyed on (lump, dest size); on the
 * following tics only the alpha-composite over the (live) game background runs,
 * which is the one part that genuinely must repeat.  The cache also records the
 * opaque bounding box so the composite skips the fully transparent margins
 * (a centred portrait is ~26% opaque inside ~43% of its area).  Measured ~2x
 * at 4K (13 ms -> 6.8 ms), and more once the empty margins are skipped. */
typedef struct
{
  int       lump;
  int       dw, dh;        /* dest size this scaled image was built for       */
  unsigned *argb;          /* dw*dh screen-space 0xAABBGGRR                    */
  int       bx0, by0;      /* opaque bounding box within the dw*dh image       */
  int       bx1, by1;      /* exclusive                                        */
  int       empty;         /* image is wholly transparent                     */
  unsigned  used;          /* last-use serial, for LRU eviction               */
} zacs_scaled_t;

static zacs_scaled_t zacs_scaled[ZACS_HUDMSG_MAX];
static unsigned      zacs_scaled_clock;

/* Total bytes the scaled cache may hold before the least-recently-used slot is
 * evicted.  A conversation normally keeps a couple of slots live (the portrait
 * and the frame); the cap only bites a very long exchange that cycles through
 * many distinct large images, keeping the cache from growing without bound at
 * high internal resolutions. */
#define ZACS_SCALED_BUDGET (64 * 1024 * 1024)

static size_t zacs_scaled_bytes(void)
{
  size_t by = 0;
  int i;
  for (i = 0; i < ZACS_HUDMSG_MAX; i++)
    if (zacs_scaled[i].argb)
      by += (size_t)zacs_scaled[i].dw * zacs_scaled[i].dh * sizeof(unsigned);
  return by;
}

static void zacs_scaled_evict_lru(void)
{
  int i, victim = -1;
  unsigned oldest = 0xffffffffu;
  for (i = 0; i < ZACS_HUDMSG_MAX; i++)
    if (zacs_scaled[i].argb && zacs_scaled[i].used < oldest)
    {
      oldest = zacs_scaled[i].used;
      victim = i;
    }
  if (victim >= 0)
  {
    free(zacs_scaled[victim].argb);
    zacs_scaled[victim].argb = NULL;
    zacs_scaled[victim].lump = -1;
  }
}

/* Find the cache slot holding (lump,dw,dh), or a slot to (re)use for it.  Small
 * linear scan: only a couple of portraits/frames are ever live at once. */
static int zacs_scaled_slot(int lump, int dw, int dh)
{
  int i, free_slot = -1;
  for (i = 0; i < ZACS_HUDMSG_MAX; i++)
  {
    if (zacs_scaled[i].argb && zacs_scaled[i].lump == lump &&
        zacs_scaled[i].dw == dw && zacs_scaled[i].dh == dh)
      return i;                                 /* exact hit */
    if (free_slot < 0 && !zacs_scaled[i].argb)
      free_slot = i;
    else if (free_slot < 0 && zacs_scaled[i].lump == lump)
      free_slot = i;                            /* same lump, stale size */
  }
  return free_slot >= 0 ? free_slot : (lump % ZACS_HUDMSG_MAX);
}

/* Resample the native image into a contiguous dw*dh ARGB buffer and record its
 * opaque bbox.  Returns the slot, or NULL on allocation failure. */
static zacs_scaled_t *zacs_scaled_build(int slot, int lump,
                                        const unsigned *argb, int aw, int ah,
                                        int dw, int dh)
{
  zacs_scaled_t *sc = &zacs_scaled[slot];
  int sx_step = (aw << 16) / dw;
  int sy_step = (ah << 16) / dh;
  int oy, bx0 = dw, by0 = dh, bx1 = 0, by1 = 0;
  unsigned *buf;

  if (sc->argb && (sc->dw != dw || sc->dh != dh))
  {
    free(sc->argb);
    sc->argb = NULL;
  }
  if (!sc->argb)
  {
    /* keep total cache memory under budget before adding this image */
    while (zacs_scaled_bytes() + (size_t)dw * dh * sizeof(unsigned)
           > ZACS_SCALED_BUDGET && zacs_scaled_bytes() > 0)
      zacs_scaled_evict_lru();
    sc->argb = malloc((size_t)dw * dh * sizeof(unsigned));
    if (!sc->argb)
      return NULL;
  }
  buf = sc->argb;

  for (oy = 0; oy < dh; oy++)
  {
    int srcy = (oy * sy_step) >> 16;
    const unsigned *srow;
    unsigned *drow = buf + (size_t)oy * dw;
    int ox, sx = 0;
    if (srcy >= ah) srcy = ah - 1;
    srow = argb + (size_t)srcy * aw;
    for (ox = 0; ox < dw; ox++, sx += sx_step)
    {
      unsigned p = srow[sx >> 16];
      drow[ox] = p;
      if (p >> 24)                              /* opaque texel: grow bbox */
      {
        if (ox < bx0) bx0 = ox;
        if (ox >= bx1) bx1 = ox + 1;
        if (oy < by0) by0 = oy;
        if (oy >= by1) by1 = oy + 1;
      }
    }
  }

  sc->lump = lump;
  sc->dw = dw; sc->dh = dh;
  sc->empty = (bx1 <= bx0 || by1 <= by0);
  sc->bx0 = bx0; sc->by0 = by0; sc->bx1 = bx1; sc->by1 = by1;
  return sc;
}

/* Composite one contiguous ARGB source row over a surface row, with a global
 * fade alpha (0..256).  Shared scalar tail used by the SIMD path below. */
static void zacs_composite_tail(uint16_t *row, const unsigned *srow,
                                int ox, int x1, int alpha)
{
  for (; ox < x1; ox++)
  {
    unsigned p = srow[ox];
    int a = (int)(p >> 24);
    if (!a)
      continue;
    a = (a * alpha) >> 8;
    if (a <= 0)
      continue;
    {
      int sr = (int)(p & 0xff);
      int sg = (int)((p >> 8) & 0xff);
      int sb = (int)((p >> 16) & 0xff);
      uint16_t d = row[ox];
      int dr = ((d >> 11) & 0x1f) << 3;
      int dg = ((d >> 5)  & 0x3f) << 2;
      int db = (d & 0x1f) << 3;
      int ia = 256 - (a > 256 ? 256 : a);
      int rr = (sr * a + dr * ia) >> 8;
      int rg = (sg * a + dg * ia) >> 8;
      int rb = (sb * a + db * ia) >> 8;
      if (rr > 255) rr = 255;
      if (rg > 255) rg = 255;
      if (rb > 255) rb = 255;
      row[ox] = (uint16_t)(((rr >> 3) << 11) | ((rg >> 2) << 5) | (rb >> 3));
    }
  }
}

/* Composite the cached scaled image (its opaque bbox only) onto the surface at
 * (dx,dy) with a fade alpha.  Reads contiguous ARGB -- no resample, no margin
 * scan.  This is the per-tic cost once a portrait is cached. */
static void zacs_composite_scaled(const zacs_scaled_t *sc, int dx, int dy,
                                  int alpha)
{
  uint16_t *surf = (uint16_t *)screens[0].data;
  int oy, y0, y1;
  if (sc->empty || alpha <= 0)
    return;
  y0 = sc->by0; y1 = sc->by1;
  for (oy = y0; oy < y1; oy++)
  {
    int dyy = dy + oy;
    const unsigned *srow;
    uint16_t *row;
    int ox = sc->bx0, x1 = sc->bx1;
    if (dyy < 0 || dyy >= SCREENHEIGHT)
      continue;
    row  = surf + (size_t)dyy * SURFACE_SHORT_PITCH;
    srow = sc->argb + (size_t)oy * sc->dw;
    /* clip the image-space x span [ox,x1) so dx+x lands inside the surface */
    if (dx + ox < 0)           ox = -dx;
    if (dx + x1 > SCREENWIDTH)  x1 = SCREENWIDTH - dx;
    if (ox >= x1)
      continue;

#if defined(ZACS_BLIT_SSE2) || defined(ZACS_BLIT_NEON)
    for (; ox + 8 <= x1; ox += 8)
    {
      ZACS_ALIGN16 uint32_t sp[8];
      ZACS_ALIGN16 uint16_t av[8];
      int j, anyop = 0, dox = dx + ox;
      for (j = 0; j < 8; j++)
      {
        unsigned p = srow[ox + j];
        int a = (int)(p >> 24);
        a = (a * alpha) >> 8;
        if (a > 256) a = 256;
        sp[j] = p;
        av[j] = (uint16_t)a;
        anyop |= a;
      }
      if (!anyop)
        continue;
#if defined(ZACS_BLIT_SSE2)
      {
        __m128i s0 = _mm_load_si128((const __m128i *)sp);
        __m128i s1 = _mm_load_si128((const __m128i *)(sp + 4));
        __m128i m8 = _mm_set1_epi32(0xff);
        __m128i sr = _mm_packs_epi32(_mm_and_si128(s0, m8),
                                     _mm_and_si128(s1, m8));
        __m128i sg = _mm_packs_epi32(_mm_and_si128(_mm_srli_epi32(s0, 8), m8),
                                     _mm_and_si128(_mm_srli_epi32(s1, 8), m8));
        __m128i sb = _mm_packs_epi32(_mm_and_si128(_mm_srli_epi32(s0, 16), m8),
                                     _mm_and_si128(_mm_srli_epi32(s1, 16), m8));
        __m128i a  = _mm_load_si128((const __m128i *)av);
        __m128i ia = _mm_sub_epi16(_mm_set1_epi16(256), a);
        __m128i d  = _mm_loadu_si128((const __m128i *)(row + dox));
        __m128i dr = _mm_slli_epi16(_mm_and_si128(_mm_srli_epi16(d, 11),
                                    _mm_set1_epi16(0x1F)), 3);
        __m128i dg = _mm_slli_epi16(_mm_and_si128(_mm_srli_epi16(d, 5),
                                    _mm_set1_epi16(0x3F)), 2);
        __m128i db = _mm_slli_epi16(_mm_and_si128(d, _mm_set1_epi16(0x1F)), 3);
        __m128i rr = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(sr, a),
                                    _mm_mullo_epi16(dr, ia)), 8);
        __m128i rg = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(sg, a),
                                    _mm_mullo_epi16(dg, ia)), 8);
        __m128i rb = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(sb, a),
                                    _mm_mullo_epi16(db, ia)), 8);
        __m128i o  = _mm_or_si128(_mm_or_si128(
                       _mm_slli_epi16(_mm_srli_epi16(rr, 3), 11),
                       _mm_slli_epi16(_mm_srli_epi16(rg, 2), 5)),
                       _mm_srli_epi16(rb, 3));
        __m128i keep = _mm_cmpeq_epi16(a, _mm_setzero_si128());
        o = _mm_or_si128(_mm_and_si128(keep, d), _mm_andnot_si128(keep, o));
        _mm_storeu_si128((__m128i *)(row + dox), o);
      }
#else /* ZACS_BLIT_NEON */
      {
        uint32x4_t s0 = vld1q_u32(sp);
        uint32x4_t s1 = vld1q_u32(sp + 4);
        uint16x8_t sr = vcombine_u16(vmovn_u32(vandq_u32(s0, vdupq_n_u32(0xff))),
                                     vmovn_u32(vandq_u32(s1, vdupq_n_u32(0xff))));
        uint16x8_t sg = vcombine_u16(
                          vmovn_u32(vandq_u32(vshrq_n_u32(s0, 8), vdupq_n_u32(0xff))),
                          vmovn_u32(vandq_u32(vshrq_n_u32(s1, 8), vdupq_n_u32(0xff))));
        uint16x8_t sb = vcombine_u16(
                          vmovn_u32(vandq_u32(vshrq_n_u32(s0, 16), vdupq_n_u32(0xff))),
                          vmovn_u32(vandq_u32(vshrq_n_u32(s1, 16), vdupq_n_u32(0xff))));
        uint16x8_t a  = vld1q_u16(av);
        uint16x8_t ia = vsubq_u16(vdupq_n_u16(256), a);
        uint16x8_t d  = vld1q_u16(row + dox);
        uint16x8_t dr = vshlq_n_u16(vandq_u16(vshrq_n_u16(d, 11),
                                    vdupq_n_u16(0x1F)), 3);
        uint16x8_t dg = vshlq_n_u16(vandq_u16(vshrq_n_u16(d, 5),
                                    vdupq_n_u16(0x3F)), 2);
        uint16x8_t db = vshlq_n_u16(vandq_u16(d, vdupq_n_u16(0x1F)), 3);
        uint16x8_t rr = vshrq_n_u16(vaddq_u16(vmulq_u16(sr, a),
                                    vmulq_u16(dr, ia)), 8);
        uint16x8_t rg = vshrq_n_u16(vaddq_u16(vmulq_u16(sg, a),
                                    vmulq_u16(dg, ia)), 8);
        uint16x8_t rb = vshrq_n_u16(vaddq_u16(vmulq_u16(sb, a),
                                    vmulq_u16(db, ia)), 8);
        uint16x8_t o  = vorrq_u16(vorrq_u16(
                          vshlq_n_u16(vshrq_n_u16(rr, 3), 11),
                          vshlq_n_u16(vshrq_n_u16(rg, 2), 5)),
                          vshrq_n_u16(rb, 3));
        uint16x8_t keep = vceqq_u16(a, vdupq_n_u16(0));
        o = vbslq_u16(keep, d, o);
        vst1q_u16(row + dox, o);
      }
#endif
    }
#endif
    /* scalar tail composites the remaining pixels at surface offset dx+ox */
    zacs_composite_tail(row + dx, srow, ox, x1, alpha);
  }
}

static void zacs_blit_scaled(int lump, int dx, int dy, int dw, int dh,
                             int alpha)
{
  const unsigned *argb;
  int aw = 0, ah = 0;
  int slot;
  zacs_scaled_t *sc;
  if (lump < 0 || dw <= 0 || dh <= 0 || alpha <= 0)
    return;

  argb = U_PNGCacheRGBA(lump, &aw, &ah);
  if (!argb || aw <= 0 || ah <= 0)
  {
    zacs_blit_scaled_indexed(lump, dx, dy, dw, dh, alpha);
    return;
  }

  /* Find or build the scaled cache slot for this lump+size.  On a lump or size
   * change the slot is rebuilt (resampled) once, then reused every tic. */
  slot = zacs_scaled_slot(lump, dw, dh);
  sc = &zacs_scaled[slot];
  if (!sc->argb || sc->lump != lump || sc->dw != dw || sc->dh != dh)
    sc = zacs_scaled_build(slot, lump, argb, aw, ah, dw, dh);
  if (sc)
    sc->used = ++zacs_scaled_clock;
  if (sc)
  {
    zacs_composite_scaled(sc, dx, dy, alpha);
    return;
  }

  /* Fallback: cache allocation failed -- resample-and-composite directly. */
  {
  uint16_t *surf;
  int ox, oy, x0, x1, y0, y1;
  int sx_step, sy_step, sx_start;
  surf    = (uint16_t *)screens[0].data;
  sx_step = (aw << 16) / dw;
  sy_step = (ah << 16) / dh;
  x0 = dx < 0 ? 0 : dx;
  y0 = dy < 0 ? 0 : dy;
  x1 = dx + dw; if (x1 > SCREENWIDTH)  x1 = SCREENWIDTH;
  y1 = dy + dh; if (y1 > SCREENHEIGHT) y1 = SCREENHEIGHT;
  sx_start = (x0 - dx) * sx_step;

  for (oy = y0; oy < y1; oy++)
  {
    int srcy = ((oy - dy) * sy_step) >> 16;
    uint16_t *row = surf + (size_t)oy * SURFACE_SHORT_PITCH;
    const unsigned *srow;
    int sx = sx_start;
    ox = x0;
    if (srcy < 0) srcy = 0; else if (srcy >= ah) srcy = ah - 1;
    srow = argb + (size_t)srcy * aw;

#if defined(ZACS_BLIT_SSE2) || defined(ZACS_BLIT_NEON)
    /* Eight output pixels per pass: gather the (strided) source texels and
     * fold the fade into each texel's alpha scalar, then run the 8-bit channel
     * LERP and 565 pack in vector lanes.  Bit-identical to the scalar tail. */
    for (; ox + 8 <= x1; ox += 8)
    {
      ZACS_ALIGN16 uint32_t sp[8];
      ZACS_ALIGN16 uint16_t av[8];
      int j, anyop = 0;
      for (j = 0; j < 8; j++, sx += sx_step)
      {
        unsigned p = srow[sx >> 16];
        int a = (int)(p >> 24);
        a = (a * alpha) >> 8;
        if (a > 256) a = 256;
        sp[j] = p;
        av[j] = (uint16_t)a;
        anyop |= a;
      }
      if (!anyop)
        continue;                              /* whole batch transparent */
#if defined(ZACS_BLIT_SSE2)
      {
        __m128i s0 = _mm_load_si128((const __m128i *)sp);
        __m128i s1 = _mm_load_si128((const __m128i *)(sp + 4));
        __m128i m8 = _mm_set1_epi32(0xff);
        __m128i sr = _mm_packs_epi32(_mm_and_si128(s0, m8),
                                     _mm_and_si128(s1, m8));
        __m128i sg = _mm_packs_epi32(_mm_and_si128(_mm_srli_epi32(s0, 8), m8),
                                     _mm_and_si128(_mm_srli_epi32(s1, 8), m8));
        __m128i sb = _mm_packs_epi32(_mm_and_si128(_mm_srli_epi32(s0, 16), m8),
                                     _mm_and_si128(_mm_srli_epi32(s1, 16), m8));
        __m128i a  = _mm_load_si128((const __m128i *)av);
        __m128i ia = _mm_sub_epi16(_mm_set1_epi16(256), a);
        __m128i d  = _mm_loadu_si128((const __m128i *)(row + ox));
        __m128i dr = _mm_slli_epi16(_mm_and_si128(_mm_srli_epi16(d, 11),
                                    _mm_set1_epi16(0x1F)), 3);
        __m128i dg = _mm_slli_epi16(_mm_and_si128(_mm_srli_epi16(d, 5),
                                    _mm_set1_epi16(0x3F)), 2);
        __m128i db = _mm_slli_epi16(_mm_and_si128(d, _mm_set1_epi16(0x1F)), 3);
        __m128i rr = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(sr, a),
                                    _mm_mullo_epi16(dr, ia)), 8);
        __m128i rg = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(sg, a),
                                    _mm_mullo_epi16(dg, ia)), 8);
        __m128i rb = _mm_srli_epi16(_mm_add_epi16(_mm_mullo_epi16(sb, a),
                                    _mm_mullo_epi16(db, ia)), 8);
        __m128i o  = _mm_or_si128(_mm_or_si128(
                       _mm_slli_epi16(_mm_srli_epi16(rr, 3), 11),
                       _mm_slli_epi16(_mm_srli_epi16(rg, 2), 5)),
                       _mm_srli_epi16(rb, 3));
        /* a==0 texels keep the destination pixel untouched */
        __m128i keep = _mm_cmpeq_epi16(a, _mm_setzero_si128());
        o = _mm_or_si128(_mm_and_si128(keep, d), _mm_andnot_si128(keep, o));
        _mm_storeu_si128((__m128i *)(row + ox), o);
      }
#else /* ZACS_BLIT_NEON */
      {
        uint32x4_t s0 = vld1q_u32(sp);
        uint32x4_t s1 = vld1q_u32(sp + 4);
        uint16x8_t sr = vcombine_u16(vmovn_u32(vandq_u32(s0, vdupq_n_u32(0xff))),
                                     vmovn_u32(vandq_u32(s1, vdupq_n_u32(0xff))));
        uint16x8_t sg = vcombine_u16(
                          vmovn_u32(vandq_u32(vshrq_n_u32(s0, 8), vdupq_n_u32(0xff))),
                          vmovn_u32(vandq_u32(vshrq_n_u32(s1, 8), vdupq_n_u32(0xff))));
        uint16x8_t sb = vcombine_u16(
                          vmovn_u32(vandq_u32(vshrq_n_u32(s0, 16), vdupq_n_u32(0xff))),
                          vmovn_u32(vandq_u32(vshrq_n_u32(s1, 16), vdupq_n_u32(0xff))));
        uint16x8_t a  = vld1q_u16(av);
        uint16x8_t ia = vsubq_u16(vdupq_n_u16(256), a);
        uint16x8_t d  = vld1q_u16(row + ox);
        uint16x8_t dr = vshlq_n_u16(vandq_u16(vshrq_n_u16(d, 11),
                                    vdupq_n_u16(0x1F)), 3);
        uint16x8_t dg = vshlq_n_u16(vandq_u16(vshrq_n_u16(d, 5),
                                    vdupq_n_u16(0x3F)), 2);
        uint16x8_t db = vshlq_n_u16(vandq_u16(d, vdupq_n_u16(0x1F)), 3);
        uint16x8_t rr = vshrq_n_u16(vaddq_u16(vmulq_u16(sr, a),
                                    vmulq_u16(dr, ia)), 8);
        uint16x8_t rg = vshrq_n_u16(vaddq_u16(vmulq_u16(sg, a),
                                    vmulq_u16(dg, ia)), 8);
        uint16x8_t rb = vshrq_n_u16(vaddq_u16(vmulq_u16(sb, a),
                                    vmulq_u16(db, ia)), 8);
        uint16x8_t o  = vorrq_u16(vorrq_u16(
                          vshlq_n_u16(vshrq_n_u16(rr, 3), 11),
                          vshlq_n_u16(vshrq_n_u16(rg, 2), 5)),
                          vshrq_n_u16(rb, 3));
        uint16x8_t keep = vceqq_u16(a, vdupq_n_u16(0));
        o = vbslq_u16(keep, d, o);
        vst1q_u16(row + ox, o);
      }
#endif
    }
#endif

    for (; ox < x1; ox++, sx += sx_step)
    {
      unsigned p = srow[sx >> 16];
      int a = (int)(p >> 24);                  /* 0xAABBGGRR: alpha in bits 24+ */
      if (!a)
        continue;                              /* fully transparent texel */
      /* fold the fade into the texel's own alpha (both 0..255 -> 0..256) */
      a = (a * alpha) >> 8;
      if (a <= 0)
        continue;
      {
        int sr = (int)(p & 0xff);              /* R in low byte */
        int sg = (int)((p >> 8) & 0xff);
        int sb = (int)((p >> 16) & 0xff);
        uint16_t d = row[ox];
        int dr = ((d >> 11) & 0x1f) << 3;
        int dg = ((d >> 5)  & 0x3f) << 2;
        int db = (d & 0x1f) << 3;
        int ia = 256 - (a > 256 ? 256 : a);
        int rr = (sr * a + dr * ia) >> 8;
        int rg = (sg * a + dg * ia) >> 8;
        int rb = (sb * a + db * ia) >> 8;
        if (rr > 255) rr = 255;
        if (rg > 255) rg = 255;
        if (rb > 255) rb = 255;
        row[ox] = (uint16_t)(((rr >> 3) << 11) | ((rg >> 2) << 5) | (rb >> 3));
      }
    }
  }
  }
}

/* Current fade alpha (0..256) for a hud message, from its fade ramps. */
static int zacs_hud_alpha(const zacs_hudmsg_t *m)
{
  if (!m->fadetotal)
    return 256;
  if (m->age < m->fadein)
    return m->fadein ? (m->age * 256 / m->fadein) : 256;
  if (m->age >= m->fadetotal - m->fadeout)
  {
    int into = m->age - (m->fadetotal - m->fadeout);
    return m->fadeout ? (256 - into * 256 / m->fadeout) : 0;
  }
  return 256;
}

void Z_ACSHudDrawer(void)
{
  int pass, i;
  zacs_draw_tick++;       /* new compositing pass: invalidate drawn marks */
  /* Two passes so the dialogue art always sits behind the text: ZDoom draws
   * the conversation backdrop and portrait first and the speaker name and
   * spoken line on top.  Within the graphics pass the art is drawn in
   * descending message-id order, so a lower-id interface frame (the dialogue
   * panel) composites on top of a higher-id portrait and its opaque lower
   * border covers the character, matching ZDoom's layering.  Text is drawn
   * last (pass 1), on top of all art. */
  for (pass = 0; pass < 2; pass++)
  {
    int order, oslot;
    for (order = 0; order < ZACS_HUDMSG_MAX; order++)
    {
      /* pass 0: pick the active, undrawn graphic with the highest id; pass 1:
       * walk the text messages in slot order. */
      if (pass == 0)
      {
        int best = -1;
        oslot = -1;
        for (i = 0; i < ZACS_HUDMSG_MAX; i++)
        {
          zacs_hudmsg_t *g = &zacs_hudmsgs[i];
          if (!g->active || g->graphic_lump < 0 || g->drawn_tick == zacs_draw_tick)
            continue;
          if (g->id > best)
          {
            best  = g->id;
            oslot = i;
          }
        }
        if (oslot < 0)
          break;
        zacs_hudmsgs[oslot].drawn_tick = zacs_draw_tick;
        i = oslot;
      }
      else
      {
        i = order;
        if (zacs_hudmsgs[i].graphic_lump >= 0)
          continue;                     /* graphics already drawn in pass 0 */
      }
    {
    zacs_hudmsg_t *m = &zacs_hudmsgs[i];
    int hw, hh, alpha;
    if (!m->active)
      continue;
    if ((pass == 0) != (m->graphic_lump >= 0))
      continue;                       /* pass 0: graphics, pass 1: text */
    hw = m->hbox_w > 0 ? m->hbox_w : 320;
    hh = m->hbox_h > 0 ? m->hbox_h : 200;
    alpha = zacs_hud_alpha(m);
    if (alpha <= 0)
      continue;

    if (m->graphic_lump >= 0)
    {
      /* The art is one image placed at (x,y) in the hud box.  Fit it to the
       * box (scaled to the screen) and centre it on the mapped point. */
      const rpatch_t *p = R_CachePatchNum(m->graphic_lump);
      int pw = p->width, ph = p->height;
      /* destination size: scale the patch from box units to screen units */
      int dw = (int)((long long)pw * SCREENWIDTH / hw);
      int dh = (int)((long long)ph * SCREENHEIGHT / hh);
      int cx = (int)((long long)m->x * SCREENWIDTH / hw);
      int cy = (int)((long long)m->y * SCREENHEIGHT / hh);
      R_UnlockPatchNum(m->graphic_lump);
      zacs_blit_scaled(m->graphic_lump, cx - dw / 2, cy - dh / 2, dw, dh,
                       alpha);
    }
    else
    {
      /* Spoken/label text.  ZDoom draws conversation text sized to the hud
       * box; this engine's text line stretches the fixed 320-wide page to the
       * whole screen, which at high resolution blows the dialogue line up far
       * past its panel.  Draw the glyphs directly instead, scaling each from
       * its native font size by the same box-to-screen ratio the art uses, so
       * the line sits at the panel's proportion and in the dialogue colour. */
      const char *src = m->text;
      char buf[ZACS_PRINTBUF];
      int cm = m->color;
      const uint8_t *trans;
      /* box-to-screen scale in 16.16 */
      long long fx = ((long long)SCREENWIDTH  << 16) / (hw > 0 ? hw : 320);
      long long fy = ((long long)SCREENHEIGHT << 16) / (hh > 0 ? hh : 200);
      /* pen position on screen, from the box-mapped anchor */
      int penx = (int)(((long long)m->x * SCREENWIDTH) / (hw > 0 ? hw : 320));
      int peny = (int)(((long long)m->y * SCREENHEIGHT) / (hh > 0 ? hh : 200));
      const char *t;
      /* CR_LIMIT is the "untranslated" sentinel from zacs_zdoom_color: leave
       * the glyphs in the font's own colours (the name / SPEAKING labels rely
       * on the font's native red).  Any other in-range value recolours through
       * its translation table. */
      if (cm == CR_LIMIT)
        trans = NULL;
      else
      {
        if (cm < 0 || cm >= CR_LIMIT)
          cm = CR_DEFAULT;
        trans = colrngs[cm];
      }
      /* typewriter reveal: only show the first (age/typeon)+1 characters */
      if (m->typeon)
      {
        int nshow = (m->age / m->typeon) + 1;
        int k = 0;
        for (; src[k] && k < (int)sizeof(buf) - 1 && k < nshow; k++)
          buf[k] = src[k];
        buf[k] = 0;
        src = buf;
      }
      for (t = src; *t; t++)
      {
        unsigned char c = (unsigned char)toupper(*t);
        int gw, lump, dw, dh;
        if (c == '\n')                  /* next line: carriage return + 8 box units down */
        {
          penx = (int)(((long long)m->x * SCREENWIDTH) / (hw > 0 ? hw : 320));
          peny += (int)((8 * fy) >> 16);
          continue;
        }
        if (c < HU_FONTSTART || c > HU_FONTEND)
        {
          penx += (int)((4 * fx) >> 16);   /* space / unprintable */
          continue;
        }
        lump = hu_font[c - HU_FONTSTART].lumpnum;
        gw   = hu_font[c - HU_FONTSTART].width;
        if (lump <= 0)
        {
          penx += (int)((4 * fx) >> 16);
          continue;
        }
        dw = (int)(((long long)hu_font[c - HU_FONTSTART].width  * fx) >> 16);
        dh = (int)(((long long)hu_font[c - HU_FONTSTART].height * fy) >> 16);
        zacs_blit_glyph_scaled(lump, penx, peny, dw, dh, trans);
        penx += (int)(((long long)gw * fx) >> 16);
      }
    }
    }   /* per-message block */
    }   /* order loop */
  }     /* pass loop */
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
  /* SetPlayerProperty(who, value, property): the only property handled here is
   * the freeze pair (PROP_FROZEN / PROP_TOTALLYFROZEN), which interactive
   * content toggles to hold the player still during an overlay.  who==0 acts
   * on the activator's player; other properties fall through to the map-format
   * handler. */
  if (special == 191)
  {
    int who = a[0], value = a[1], prop = a[2];
    if (prop == 0 || prop == 4)             /* PROP_FROZEN / PROP_TOTALLYFROZEN */
    {
      int pn;
      for (pn = 0; pn < MAXPLAYERS; pn++)
      {
        if (!playeringame[pn])
          continue;
        if (who == 0 && !(inst->activator && inst->activator->player ==
                          &players[pn]))
          continue;
        if (value)
          players[pn].cheats |= CF_TOTALLYFROZEN;
        else
          players[pn].cheats &= ~CF_TOTALLYFROZEN;
      }
      if (result)
        *result = 1;
      return;
    }
  }
  /* Thing_ChangeTID(oldtid, newtid): reassign the TID of every thing that
   * currently has oldtid (0 = the activator) to newtid.  This ZDoom special
   * is not part of the base line-special set, but interactive content relies
   * on it to give an otherwise untagged actor a TID so later TID-addressed
   * queries (GetUserArray and the like) can find it. */
  if (special == 176)
  {
    int oldtid = a[0], newtid = a[1];
    if (oldtid == 0)
    {
      if (inst->activator)
      {
        if (inst->activator->tid)
          P_RemoveMobjFromTIDList(inst->activator);
        if (newtid)
          P_InsertMobjIntoTIDList(inst->activator, (short)newtid);
        else
          inst->activator->tid = 0;
      }
    }
    else
    {
      int sp = -1;
      mobj_t *mo;
      while ((mo = P_FindMobjFromTID((short)oldtid, &sp)) != NULL)
      {
        P_RemoveMobjFromTIDList(mo);
        if (newtid)
          P_InsertMobjIntoTIDList(mo, (short)newtid);
        else
          mo->tid = 0;
        sp = -1;                      /* list mutated; restart the walk */
      }
    }
    if (result)
      *result = 0;
    return;
  }
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
  /* Per-slice instruction budget: the number of pcodes one script may run in
   * a single tic before the engine assumes it has hung and terminates it.
   * The guard is meant to catch a non-yielding infinite loop, not to cap
   * legitimate work -- a script that does heavy one-time setup and then
   * yields with Delay() resumes with a fresh budget next tic.  Compiled
   * library code (e.g. a GDCC translation unit building its data tables on
   * load) can legitimately run on the order of a million pcodes in one slice,
   * so the ceiling matches ZDoom's traditional runaway limit rather than a
   * tighter value that would abort such initialisation midway. */
  int budget = 2000000;

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
    case PCD_SETRESULTVALUE:            /* result channel for sync CallACS */
      inst->result = ZPOP();
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
      zacs_print_str(inst, zacs_string(ZPOP()));
      break;
    case PCD_PRINTLOCALIZED:
      /* ACS PrintLocalized: the argument is a LANGUAGE key, not literal text.
       * Resolve it through the LANGUAGE table; on a miss print the key name,
       * matching ZDoom's behaviour for an unknown key.  GDCC-compiled content
       * reads runtime-built string tables this way, so without the lookup the
       * tables come back empty. */
      {
        const char *raw = zacs_string(ZPOP());
        const char *loc = raw ? U_ZLanguageLookup(raw) : NULL;
        zacs_print_str(inst, loc ? loc : raw);
      }
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
       * color, x, y) then OPTHUDMESSAGE marks the boundary; the optional
       * params follow above it in order: holdtime, then two type-specific
       * timing params, then alpha.  For a fade message the two params are the
       * fade-in and fade-out lengths; for a typeon message the first is the
       * per-character reveal speed.  All times are 16.16 seconds. */
      int htype, hid, hcolor, hx, hy;
      int hhold = 0, hp1 = 0, hp2 = 0;
      int base = (inst->optstart >= 0) ? inst->optstart : inst->sp;
      int nopt = (inst->optstart >= 0) ? (inst->sp - inst->optstart) : 0;
      if (nopt > 0) hp1   = inst->stack[inst->optstart];
      if (nopt > 1) hp2   = inst->stack[inst->optstart + 1];
      hhold  = (base >= 1) ? inst->stack[base - 1] : 0;
      hy     = (base >= 2) ? inst->stack[base - 2] : 0;
      hx     = (base >= 3) ? inst->stack[base - 3] : 0;
      hcolor = (base >= 4) ? inst->stack[base - 4] : 0;
      hid    = (base >= 5) ? inst->stack[base - 5] : 0;
      htype  = (base >= 6) ? inst->stack[base - 6] : 0;
      inst->sp = (base >= 6) ? base - 6 : 0;     /* drop base + optionals */
      inst->optstart = -1;
      if (inst->printlen)
      {
        int tdry  = htype & 3;                   /* 1=fadeout 2=typeon 3=both */
        int fadein = 0, fadeout = 0, typeon = 0;
        /* ZDoom HudMessage: a coordinate whose magnitude is below 1.0 is a
         * fraction of the hud box (0.5 = centre) rather than an absolute cell.
         * Resolve those to whole box units before storing. */
        int px, py;
        /* holdtime is 16.16 seconds; <=0 means "until replaced".  Convert to
         * tics (35/sec); persistent when not positive. */
        int tics = (hhold > 0) ? (int)(((long long)hhold * 35) >> 16) : -1;
        if (hx > -(1 << 16) && hx < (1 << 16))
          px = (int)(((long long)hx * zacs_hud_w) >> 16);
        else
          px = hx >> 16;
        if (hy > -(1 << 16) && hy < (1 << 16))
          py = (int)(((long long)hy * zacs_hud_h) >> 16);
        else
          py = hy >> 16;
        if (tdry == 3)                           /* HUDMSG_FADEINOUT */
        {
          fadein  = (int)(((long long)hp1 * 35) >> 16);
          fadeout = (int)(((long long)hp2 * 35) >> 16);
        }
        else if (tdry == 1)                      /* HUDMSG_FADEOUT */
          fadeout = (int)(((long long)hp1 * 35) >> 16);
        else if (tdry == 2)                      /* HUDMSG_TYPEON */
        {
          /* hp1 is seconds per character; derive tics/char (>=1). */
          int tpc = (int)(((long long)hp1 * 35) >> 16);
          typeon  = tpc > 0 ? tpc : 1;
          fadeout = (int)(((long long)hp2 * 35) >> 16);
        }
        zacs_hud_store(inst->printbuf, hid, px, py,
                       tics, zacs_zdoom_color(hcolor), htype,
                       fadein, fadeout, typeon);
      }
      else if (hid != 0)
        zacs_hud_clear(hid);                  /* empty text = ClearMessage(id) */
      inst->printlen  = 0;
      inst->printbuf[0] = 0;
      break;
    }
    case PCD_SAVESTRING:
      /* GDCC's print model expects the working buffer to be empty after a
       * string is captured; it emits SaveString and then continues printing
       * without a fresh BeginPrint.  Reset here so consecutive saves don't
       * accumulate. */
      ZPUSH(zacs_save_string(inst->printbuf));
      inst->printlen = 0;
      inst->printbuf[0] = 0;
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
      ZSETSTK(1, zacs_cvar_value(zacs_string(ZSTK(1))));
      break;
    case PCD_GETPLAYERINFO:
    { int q = ZPOP(); (void)q; ZSETSTK(1, 0); zacs_warn_pcd(pcd); break; }
    case PCD_GETPLAYERINPUT:
    {
      /* GetPlayerInput(player, input): read the current (INPUT_BUTTONS = 1) or
       * previous-tic (INPUT_OLDBUTTONS = 0) button state, translating the
       * engine's button bits into the ZDoom layout (BT_ATTACK = 1<<0,
       * BT_USE = 1<<1) that content tests against.  player < 0 selects the
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
      /* ZDoom input selectors: INPUT_BUTTONS = 1 and INPUT_OLDBUTTONS = 0 read
       * this tic's and the previous tic's button state; the MODINPUT_ aliases
       * (9 and 8) read the same through the mod-input path.  A KeyPressed test
       * reads BUTTONS and OLDBUTTONS and takes the edge, so both must map to the
       * button state -- handling only 2/3 (which are PITCH/YAW) left every such
       * test reading zero, so script-driven dialogue never saw the use key. */
      if (pl && (input == 0 || input == 1 || input == 8 || input == 9))
      {
        int cur = (input == 1 || input == 9);
        int b = cur ? pl->cmd.buttons : zacs_oldbuttons_of(pl);
        if (b & BT_ATTACK) r |= 1;   /* ZDoom BT_ATTACK = 1<<0 */
        if (b & BT_USE)    r |= 2;   /* ZDoom BT_USE    = 1<<1 */
        /* Latch the live use key so a held button reports as a single press,
         * letting dialogue terminate instead of looping back to the start.
         * Only the current-tic read (INPUT_BUTTONS) is latched; the previous-
         * tic read (INPUT_OLDBUTTONS) keeps the raw state for edge tests. */
        if (cur)
        {
          int idx = zacs_player_index(pl);
          if (idx >= 0)
          {
            if (b & BT_USE)
            {
              if (zacs_use_latched[idx])
                r &= ~2;           /* already consumed this hold */
              else
                zacs_use_latched[idx] = 1;
            }
            else
              zacs_use_latched[idx] = 0;   /* released: re-arm */
          }
        }
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
          case 0:  v = mo->health; break;           /* APROP_Health */
          case 1:  v = mo->info ? mo->info->speed : 0; break;   /* Speed */
          case 2:  v = mo->info ? mo->info->damage : 0; break;  /* Damage */
          case 16: v = mo->info ? mo->info->spawnhealth : mo->health;
                   break;                            /* APROP_SpawnHealth */
          case 31: v = mo->info ? mo->info->mass : 0; break;    /* Mass */
          case 34: v = mo->height; break;            /* APROP_Height (fixed) */
          case 35: v = mo->radius; break;            /* APROP_Radius (fixed) */
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
      /* SetActorState(tid, "statename", exact): move every actor with the
       * given tid (or the activator when tid==0) to the named DECORATE state,
       * returning the number actually moved.  `exact` (whether to require an
       * exact label match vs. a prefix) is not distinguished here -- the
       * label table is matched by full name either way. */
      int exact = ZSTK(1);
      const char *sname = zacs_string(ZSTK(2));
      int tid = ZSTK(3);
      int moved = 0;
      (void)exact;
      if (sname)
      {
        if (tid == 0)
        {
          mobj_t *mo = inst->activator;
          if (mo)
          {
            int st = U_DecorateStateForType(mo->type, sname);
            if (st > 0 && P_SetMobjState(mo, (statenum_t)st))
              moved++;
          }
        }
        else
        {
          int sp = -1;
          mobj_t *mo;
          while ((mo = P_FindMobjFromTID((short)tid, &sp)) != NULL)
          {
            int st = U_DecorateStateForType(mo->type, sname);
            if (st > 0 && P_SetMobjState(mo, (statenum_t)st))
              moved++;
          }
        }
      }
      ZDROP(2);
      ZSETSTK(1, moved);
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
    case PCD_SETFONT:
    {
      const char *fn = zacs_string(ZSTK(1));
      ZDROP(1);
      if (fn)
      {
        size_t n = strlen(fn);
        if (n > sizeof(zacs_cur_font) - 1)
          n = sizeof(zacs_cur_font) - 1;
        memcpy(zacs_cur_font, fn, n);
        zacs_cur_font[n] = 0;
      }
      else
        zacs_cur_font[0] = 0;
      break;
    }
    case PCD_SETFONTDIRECT:
    {
      const char *fn = zacs_string(NEXTWORD);
      if (fn)
      {
        size_t n = strlen(fn);
        if (n > sizeof(zacs_cur_font) - 1)
          n = sizeof(zacs_cur_font) - 1;
        memcpy(zacs_cur_font, fn, n);
        zacs_cur_font[n] = 0;
      }
      else
        zacs_cur_font[0] = 0;
      break;
    }
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
        case 39:                          /* ACSF_ACS_NamedExecute */
        {
          /* ACS_NamedExecute(str name, int map, arg1, arg2, arg3): start the
           * named script if an instance is not already running. */
          int eargs[3], i;
          for (i = 0; i < 3; i++)
            eargs[i] = (argc > i + 2) ? a[i + 2] : 0;
          if (argc > 0)
            r = Z_ACSStartNamed(a[0], argc > 1 ? a[1] : 0, eargs, 3,
                                inst->activator, NULL, 0, false) ? 1 : 0;
          break;
        }
        case 44:                          /* ACSF_ACS_NamedExecuteWithResult */
        {
          /* ACS_NamedExecuteWithResult(str name, arg1..arg4): start a fresh
           * instance of the named script.  The C-to-bytecode toolchain used by
           * some mods lowers its synchronous-script function-pointer calls to
           * this: the caller clears a return-flag slot, calls here to launch
           * the callee, then Delay-polls that slot until the callee's epilogue
           * sets it.  Launching the script (always = a fresh instance) is all
           * the engine must do; the started script's own code performs the
           * handshake, so the immediate result is unused by that protocol. */
          int eargs[4], i;
          for (i = 0; i < 4; i++)
            eargs[i] = (argc > i + 1) ? a[i + 1] : 0;
          if (argc > 0)
            Z_ACSStartNamed(a[0], 0, eargs, 4, inst->activator, NULL, 0, true);
          r = 0;
          break;
        }
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

        case 23:                          /* ACSF_SetActorVelocity */
        {
          /* SetActorVelocity(tid, velx, vely, velz, add, setbob): set (or
           * with add!=0, accumulate) an actor's momentum.  tid 0 means the
           * activator.  Values are already in fixed point, matching momx/y/z.
           * setbob is a flag for water bobbing the software path ignores. */
          int tid  = argc > 0 ? a[0] : 0;
          int vx   = argc > 1 ? a[1] : 0;
          int vy   = argc > 2 ? a[2] : 0;
          int vz   = argc > 3 ? a[3] : 0;
          int add  = argc > 4 ? a[4] : 0;
          if (tid == 0)
          {
            mobj_t *mo = inst->activator;
            if (mo)
            {
              if (add) { mo->momx += vx; mo->momy += vy; mo->momz += vz; }
              else     { mo->momx  = vx; mo->momy  = vy; mo->momz  = vz; }
            }
          }
          else
          {
            int sp = -1;
            mobj_t *mo;
            while ((mo = P_FindMobjFromTID((short)tid, &sp)) != NULL)
            {
              if (add) { mo->momx += vx; mo->momy += vy; mo->momz += vz; }
              else     { mo->momx  = vx; mo->momy  = vy; mo->momz  = vz; }
            }
          }
          break;
        }

        case 68:                          /* ACSF_GetActorClass(tid) */
        {
          /* return a runtime string index for the actor's class name.  The
           * death system reads this to build "<Class>_Sex"; the replacement
           * monsters carry their own DECORATE name in mobjinfo.actorname. */
          mobj_t *mo = zacs_aptr(inst, argc > 0 ? a[0] : 0);
          const char *nm = (mo && mobjinfo[mo->type].actorname)
                           ? mobjinfo[mo->type].actorname : "";
          r = zacs_save_string(nm);
          break;
        }

        case 69:                          /* ACSF_GetWeapon() */
        {
          /* return the activator player's current weapon as a class-name
           * string.  The dialogue system saves this before switching the
           * player to an unarmed state for the duration of a conversation and
           * restores it on exit (GetWeapon -> ... -> SetWeapon); returning 0
           * here left it with an empty weapon name to restore, breaking the
           * dialogue's weapon handling. */
          const char *nm = "";
          if (inst->activator && inst->activator->player)
          {
            int w = (int)inst->activator->player->readyweapon;
            size_t i;
            for (i = 0; i < sizeof(zacs_weapmap)/sizeof(zacs_weapmap[0]); i++)
              if (zacs_weapmap[i].weapon == w)
              {
                nm = zacs_weapmap[i].name;
                break;
              }
          }
          r = zacs_save_string(nm);
          break;
        }

        case 13:                          /* ACSF_SetActivatorToTarget(tid) */
        {
          /* retarget the script's activator to the target of the actor named
           * by the tid (tid 0 = current activator).  Scripts that operate on
           * "the thing this actor is fighting" rely on this; without it the
           * activator-relative work that follows acts on the wrong actor. */
          mobj_t *mo = zacs_aptr(inst, argc > 0 ? a[0] : 0);
          if (mo && mo->target)
          {
            P_SetTarget(&inst->activator, mo->target);
            r = 1;
          }
          else
            r = 0;
          break;
        }

        case 15:                          /* ACSF_GetChar(str, index) */
        {
          /* return the byte at a position in a string, or 0 past the end --
           * the per-character primitive a C string runtime is built on */
          const char *s = zacs_string(argc > 0 ? a[0] : 0);
          int idx = (argc > 1) ? a[1] : 0;
          if (s && idx >= 0 && idx < (int)strlen(s))
            r = (unsigned char)s[idx];
          else
            r = 0;
          break;
        }

        case 12:                          /* ACSF_SetActivator(tid[,ptr]) */
        {
          /* point the script's activator at the actor with this tid (tid 0
           * leaves it unchanged); the optional pointer selector is not
           * supported here, so only the direct form is honoured. */
          mobj_t *mo = (argc > 0 && a[0]) ? zacs_aptr(inst, a[0]) : inst->activator;
          if (mo)
          {
            P_SetTarget(&inst->activator, mo);
            r = 1;
          }
          else
            r = 0;
          break;
        }

        case 46:                          /* ACSF_UniqueTID([start[,limit]]) */
        {
          /* return a tid not currently in use, so a script can tag an actor
           * it just spawned without colliding with map-authored tids.  Scans
           * upward from the requested start (or a high base) and confirms the
           * candidate is free via P_FindMobjFromTID. */
          int start = (argc > 0 && a[0] > 0) ? a[0] : 0;
          int limit = (argc > 1) ? a[1] : 0;
          int cand  = (start > 0) ? start : 30000;
          int tries = (limit > 0) ? limit : 65535;
          r = 0;
          while (tries-- > 0)
          {
            int sp = -1;
            if (cand > 0 && cand <= 32767 &&
                P_FindMobjFromTID((short)cand, &sp) == NULL)
            {
              r = cand;
              break;
            }
            if (++cand > 32767)
              cand = 1;
          }
          break;
        }

        case 79:                          /* ACSF_ChangeActorAngle(tid,ang[,interp]) */
        {
          /* set an actor's facing.  The angle is the ACS 16.16 turn fraction
           * (1.0 == full circle), matching SetActorAngle; interpolation is a
           * rendering nicety we do not apply. */
          int sp = -1, hit = 0;
          int tid = (argc > 0) ? a[0] : 0;
          angle_t ang = (angle_t)((argc > 1) ? a[1] : 0) << 16;
          if (tid == 0)
          {
            if (inst->activator) { inst->activator->angle = ang; hit = 1; }
          }
          else
          {
            mobj_t *mo;
            while ((mo = P_FindMobjFromTID((short)tid, &sp)) != NULL)
            { mo->angle = ang; hit = 1; }
          }
          r = hit;
          break;
        }

        case 80:                          /* ACSF_ChangeActorPitch(tid,pitch[,interp]) */
        {
          /* set an actor's look pitch, same angle encoding as the angle call */
          int sp = -1, hit = 0;
          int tid = (argc > 0) ? a[0] : 0;
          angle_t pit = (angle_t)((argc > 1) ? a[1] : 0) << 16;
          if (tid == 0)
          {
            if (inst->activator) { inst->activator->pitch = pit; hit = 1; }
          }
          else
          {
            mobj_t *mo;
            while ((mo = P_FindMobjFromTID((short)tid, &sp)) != NULL)
            { mo->pitch = pit; hit = 1; }
          }
          r = hit;
          break;
        }

        case 38:                          /* ACSF_SetPointer(assign,tid[,ptr,flags]) */
        {
          /* assign one of the activator's actor pointers (target or tracer;
           * this engine has no master pointer) to the first actor with the
           * given tid.  AAPTR_TARGET == 1, AAPTR_TRACER == 4. */
          int assign = (argc > 0) ? a[0] : 0;
          int tid    = (argc > 1) ? a[1] : 0;
          mobj_t *src = inst->activator;
          mobj_t *dst = (tid == 0) ? inst->activator : zacs_aptr(inst, tid);
          r = 0;
          if (src)
          {
            if (assign & 1)        { P_SetTarget(&src->target, dst); r = 1; }
            if (assign & 4)        { P_SetTarget(&src->tracer, dst); r = 1; }
          }
          break;
        }

        case 36:                          /* ACSF_SpawnForced(type,x,y,z[,tid,ang]) */
        {
          /* spawn an actor unconditionally at an absolute position.  The
           * engine's P_SpawnMobj already places the actor without a fit
           * test, so this matches the "forced" contract; the angle is a
           * 0..255 byte like the Spawn pcode. */
          int type = zacs_actor_type(zacs_string(argc > 0 ? a[0] : 0));
          r = 0;
          if (type >= 0)
          {
            mobj_t *mo = P_SpawnMobj(argc > 1 ? a[1] : 0,
                                     argc > 2 ? a[2] : 0,
                                     argc > 3 ? a[3] : 0, (mobjtype_t)type);
            if (mo)
            {
              if (argc > 5)
                mo->angle = (angle_t)(a[5] & 0xFF) << 24;
              if (argc > 4 && a[4])
                P_InsertMobjIntoTIDList(mo, (short)a[4]);
              r = 1;
            }
          }
          break;
        }

        case 114:                         /* ACSF_GetPlayerAccountName(tid) */
          /* a Zandronum networking accessor with no meaning in a single
           * player port: there are no accounts, so return the empty string. */
          r = zacs_save_string("");
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
