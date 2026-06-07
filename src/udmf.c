/* udmf.c -- MSVC C89 port of DSDA-Doom's UDMF parser.
 *
 * Original (C) 2022 Ryan Krafnick, GPLv2.  Translated from C++ to C89:
 *   - std::vector<T>  -> manual realloc-doubling grow-arrays (udmf_arr_t)
 *   - Scanner class   -> scanner_t + scanner_* free functions (see scanner.c)
 *   - new/delete      -> the level zone allocator (Z_MallocLevel/Z_StrdupLevel)
 *   - //, bool, etc.  -> C comments, dbool/TRUE/FALSE, declarations at block tops
 *
 * Behavior is intended to be identical to the original.
 */

#include <stdlib.h>
#include <string.h>

#include "scanner.h"
#include "udmf.h"
#include "z_zone.h"

/* The parser allocates level-lifetime storage for the strings it hands back
 * (vertex/thing coordinate text, texture-overflow names, etc.).  Map these
 * onto the engine zone allocator with the PU_LEVEL tag so they are released
 * automatically when the level is unloaded. */
void *Z_MallocLevel(size_t size)      { return Z_Malloc(size, PU_LEVEL, 0); }
char *Z_StrdupLevel(const char *s)    { return Z_Strdup(s, PU_LEVEL, 0); }

/* These are provided by the engine at integration time (level zone alloc,
 * auto-freed on level unload) and by the game-mode globals.  For the
 * standalone parser test, local definitions are supplied in udmf_test.c. */
extern int raven;    /* nonzero for Heretic/Hexen builds */
extern int heretic;  /* nonzero when running Heretic     */
extern int hexen;    /* nonzero when running Hexen        */

udmf_namespace_t udmf_namespace = UDMF_NONE;

/* ------------------------------------------------------------------ */
/* dsda_StringToFixed: parse a UDMF coordinate/value string ("128",    */
/* "-64.5", "0.375") to fixed_t.  C89, no sscanf -- a hand cursor      */
/* parser matching the semantics of DSDA's helper (integer part shifted */
/* by FRACBITS, up to 8 fractional digits scaled by their power of 10).*/
/* ------------------------------------------------------------------ */
fixed_t udmf_to_fixed(const char *x)
{
  dbool negative;
  fixed_t ipart;
  const char *p;
  long frac_num;
  long frac_div;
  int  frac_digits;

  if (!x || !*x)
    return 0;

  p = x;
  negative = FALSE;
  if (*p == '-') { negative = TRUE; p++; }
  else if (*p == '+') { p++; }

  /* integer part */
  ipart = 0;
  while (*p >= '0' && *p <= '9')
  {
    ipart = ipart * 10 + (*p - '0');
    p++;
  }
  ipart <<= FRACBITS;

  /* fractional part: accumulate up to 8 digits, tracking the divisor */
  frac_num = 0;
  frac_div = 1;
  frac_digits = 0;
  if (*p == '.')
  {
    p++;
    while (*p >= '0' && *p <= '9' && frac_digits < 8)
    {
      frac_num = frac_num * 10 + (*p - '0');
      frac_div *= 10;
      frac_digits++;
      p++;
    }
  }

  if (frac_num)
    ipart += (fixed_t)(((long long)frac_num * FRACUNIT) / frac_div);

  return negative ? -ipart : ipart;
}

udmf_t udmf;

/* ------------------------------------------------------------------ */
/* Generic grow-array (replaces std::vector).  Stores elements of a    */
/* fixed size; doubles capacity on demand.  Never shrinks.             */
/* ------------------------------------------------------------------ */
typedef struct {
  void  *data;
  size_t count;
  size_t capacity;
  size_t elem;
} udmf_arr_t;

static void udmf_arr_init(udmf_arr_t *a, size_t elem)
{
  a->data     = NULL;
  a->count    = 0;
  a->capacity = 0;
  a->elem     = elem;
}

static void udmf_arr_clear(udmf_arr_t *a)
{
  if (a->data)
    free(a->data);
  a->data     = NULL;
  a->count    = 0;
  a->capacity = 0;
}

/* Append one element (copied from src); returns pointer to the stored copy. */
static void *udmf_arr_push(udmf_arr_t *a, const void *src)
{
  void *slot;
  if (a->count == a->capacity)
  {
    size_t newcap = a->capacity ? a->capacity * 2 : 64;
    void  *p = realloc(a->data, newcap * a->elem);
    if (!p)
      return NULL; /* out of memory; caller's ErrorF path handles emptiness */
    a->data     = p;
    a->capacity = newcap;
  }
  slot = (char *)a->data + a->count * a->elem;
  memcpy(slot, src, a->elem);
  a->count++;
  return slot;
}

static udmf_arr_t udmf_lines;
static udmf_arr_t udmf_sides;
static udmf_arr_t udmf_vertices;
static udmf_arr_t udmf_sectors;
static udmf_arr_t udmf_things;

static udmf_errorfunc udmf_err = NULL;

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static void dsda_SkipValue(scanner_t *s)
{
  if (scanner_check_token(s, '='))
  {
    while (scanner_tokens_left(s))
    {
      if (scanner_check_token(s, ';'))
        break;
      scanner_get_next_token(s, TRUE);
    }
    return;
  }

  scanner_must_get_token(s, '{');
  {
    int brace_count = 1;
    while (scanner_tokens_left(s))
    {
      if (scanner_check_token(s, '}'))
        --brace_count;
      else if (scanner_check_token(s, '{'))
        ++brace_count;

      if (!brace_count)
        break;

      scanner_get_next_token(s, TRUE);
    }
    return;
  }
}

/* The scanner drops the sign when scanning; re-prepend it for the string
 * forms (vertex/thing coords keep their textual representation). */
static char *dsda_FloatString(scanner_t *s)
{
  char *buffer;
  if (s->decimal >= 0)
    return Z_StrdupLevel(s->string);

  buffer = (char *)Z_MallocLevel(strlen(s->string) + 2);
  buffer[0] = '-';
  buffer[1] = '\0';
  strcat(buffer, s->string);
  return buffer;
}

/* SCAN_* macros: faithful translations of the originals.  Each consumes
 * '=' <value> ';' and stores into the target field. */
#define SCAN_INT(x)          do { scanner_must_get_token(s, '=');                 \
                                  scanner_must_get_integer(s);                    \
                                  (x) = s->number;                                \
                                  scanner_must_get_token(s, ';'); } while (0)

#define SCAN_FLOAT(x)        do { scanner_must_get_token(s, '=');                 \
                                  scanner_must_get_float(s);                      \
                                  (x) = (float)s->decimal;                        \
                                  scanner_must_get_token(s, ';'); } while (0)

#define SCAN_FLAG(x, f)      do { scanner_must_get_token(s, '=');                 \
                                  scanner_must_get_token(s, TK_BoolConst);        \
                                  if (s->boolean) (x) |= (f);                     \
                                  scanner_must_get_token(s, ';'); } while (0)

#define SCAN_STRING_N(x, n)  do { scanner_must_get_token(s, '=');                 \
                                  scanner_must_get_token(s, TK_StringConst);      \
                                  strncpy((x), s->string, (n));                   \
                                  scanner_must_get_token(s, ';'); } while (0)

#define SCAN_STRING(x)       do { scanner_must_get_token(s, '=');                 \
                                  scanner_must_get_token(s, TK_StringConst);      \
                                  (x) = Z_StrdupLevel(s->string);                 \
                                  scanner_must_get_token(s, ';'); } while (0)

#define SCAN_FLOAT_STRING(x) do { scanner_must_get_token(s, '=');                 \
                                  scanner_must_get_float(s);                      \
                                  (x) = dsda_FloatString(s);                      \
                                  scanner_must_get_token(s, ';'); } while (0)

/* ------------------------------------------------------------------ */
/* Block parsers                                                       */
/* ------------------------------------------------------------------ */

static void dsda_ParseUDMFLineDef(scanner_t *s)
{
  udmf_line_t line;
  memset(&line, 0, sizeof(line));
  line.id = (udmf_namespace == UDMF_DSDA || udmf_namespace == UDMF_HEXEN ||
             udmf_namespace == UDMF_ZDOOM) ? -1 : 0;
  line.sideback = -1;
  line.alpha = 1.0f;

  scanner_must_get_token(s, '{');
  while (!scanner_check_token(s, '}'))
  {
    scanner_must_get_token(s, TK_Identifier);

    if      (scanner_string_match(s, "id"))           SCAN_INT(line.id);
    else if (scanner_string_match(s, "v1"))           SCAN_INT(line.v1);
    else if (scanner_string_match(s, "v2"))           SCAN_INT(line.v2);
    else if (scanner_string_match(s, "special"))      SCAN_INT(line.special);
    else if (scanner_string_match(s, "arg0"))         SCAN_INT(line.arg0);
    else if (scanner_string_match(s, "arg1"))         SCAN_INT(line.arg1);
    else if (scanner_string_match(s, "arg2"))         SCAN_INT(line.arg2);
    else if (scanner_string_match(s, "arg3"))         SCAN_INT(line.arg3);
    else if (scanner_string_match(s, "arg4"))         SCAN_INT(line.arg4);
    else if (scanner_string_match(s, "sidefront"))    SCAN_INT(line.sidefront);
    else if (scanner_string_match(s, "sideback"))     SCAN_INT(line.sideback);
    else if (scanner_string_match(s, "locknumber"))   SCAN_INT(line.locknumber);
    else if (scanner_string_match(s, "automapstyle")) SCAN_INT(line.automapstyle);
    else if (scanner_string_match(s, "health"))       SCAN_INT(line.health);
    else if (scanner_string_match(s, "healthgroup"))  SCAN_INT(line.healthgroup);
    else if (scanner_string_match(s, "alpha"))        SCAN_FLOAT(line.alpha);
    else if (scanner_string_match(s, "blocking"))           SCAN_FLAG(line.flags, UDMF_ML_BLOCKING);
    else if (scanner_string_match(s, "blockmonsters"))      SCAN_FLAG(line.flags, UDMF_ML_BLOCKMONSTERS);
    else if (scanner_string_match(s, "twosided"))           SCAN_FLAG(line.flags, UDMF_ML_TWOSIDED);
    else if (scanner_string_match(s, "dontpegtop"))         SCAN_FLAG(line.flags, UDMF_ML_DONTPEGTOP);
    else if (scanner_string_match(s, "dontpegbottom"))      SCAN_FLAG(line.flags, UDMF_ML_DONTPEGBOTTOM);
    else if (scanner_string_match(s, "secret"))             SCAN_FLAG(line.flags, UDMF_ML_SECRET);
    else if (scanner_string_match(s, "blocksound"))         SCAN_FLAG(line.flags, UDMF_ML_SOUNDBLOCK);
    else if (scanner_string_match(s, "dontdraw"))           SCAN_FLAG(line.flags, UDMF_ML_DONTDRAW);
    else if (scanner_string_match(s, "mapped"))             SCAN_FLAG(line.flags, UDMF_ML_MAPPED);
    else if (scanner_string_match(s, "passuse"))            SCAN_FLAG(line.flags, UDMF_ML_PASSUSE);
    else if (scanner_string_match(s, "translucent"))        SCAN_FLAG(line.flags, UDMF_ML_TRANSLUCENT);
    else if (scanner_string_match(s, "jumpover"))           SCAN_FLAG(line.flags, UDMF_ML_JUMPOVER);
    else if (scanner_string_match(s, "blockfloaters"))      SCAN_FLAG(line.flags, UDMF_ML_BLOCKFLOATERS);
    else if (scanner_string_match(s, "playercross"))        SCAN_FLAG(line.flags, UDMF_ML_PLAYERCROSS);
    else if (scanner_string_match(s, "playeruse"))          SCAN_FLAG(line.flags, UDMF_ML_PLAYERUSE);
    else if (scanner_string_match(s, "monstercross"))       SCAN_FLAG(line.flags, UDMF_ML_MONSTERCROSS);
    else if (scanner_string_match(s, "monsteruse"))         SCAN_FLAG(line.flags, UDMF_ML_MONSTERUSE);
    else if (scanner_string_match(s, "impact"))             SCAN_FLAG(line.flags, UDMF_ML_IMPACT);
    else if (scanner_string_match(s, "playerpush"))         SCAN_FLAG(line.flags, UDMF_ML_PLAYERPUSH);
    else if (scanner_string_match(s, "monsterpush"))        SCAN_FLAG(line.flags, UDMF_ML_MONSTERPUSH);
    else if (scanner_string_match(s, "missilecross"))       SCAN_FLAG(line.flags, UDMF_ML_MISSILECROSS);
    else if (scanner_string_match(s, "repeatspecial"))      SCAN_FLAG(line.flags, UDMF_ML_REPEATSPECIAL);
    else if (scanner_string_match(s, "playeruseback"))      SCAN_FLAG(line.flags, UDMF_ML_PLAYERUSEBACK);
    else if (scanner_string_match(s, "anycross"))           SCAN_FLAG(line.flags, UDMF_ML_ANYCROSS);
    else if (scanner_string_match(s, "monsteractivate"))    SCAN_FLAG(line.flags, UDMF_ML_MONSTERACTIVATE);
    else if (scanner_string_match(s, "blockplayers"))       SCAN_FLAG(line.flags, UDMF_ML_BLOCKPLAYERS);
    else if (scanner_string_match(s, "blockeverything"))    SCAN_FLAG(line.flags, UDMF_ML_BLOCKEVERYTHING);
    else if (scanner_string_match(s, "firstsideonly"))      SCAN_FLAG(line.flags, UDMF_ML_FIRSTSIDEONLY);
    else if (scanner_string_match(s, "zoneboundary"))       SCAN_FLAG(line.flags, UDMF_ML_ZONEBOUNDARY);
    else if (scanner_string_match(s, "clipmidtex"))         SCAN_FLAG(line.flags, UDMF_ML_CLIPMIDTEX);
    else if (scanner_string_match(s, "wrapmidtex"))         SCAN_FLAG(line.flags, UDMF_ML_WRAPMIDTEX);
    else if (scanner_string_match(s, "midtex3d"))           SCAN_FLAG(line.flags, UDMF_ML_MIDTEX3D);
    else if (scanner_string_match(s, "midtex3dimpassible")) SCAN_FLAG(line.flags, UDMF_ML_MIDTEX3DIMPASSIBLE);
    else if (scanner_string_match(s, "checkswitchrange"))   SCAN_FLAG(line.flags, UDMF_ML_CHECKSWITCHRANGE);
    else if (scanner_string_match(s, "blockprojectiles"))   SCAN_FLAG(line.flags, UDMF_ML_BLOCKPROJECTILES);
    else if (scanner_string_match(s, "blockuse"))           SCAN_FLAG(line.flags, UDMF_ML_BLOCKUSE);
    else if (scanner_string_match(s, "blocksight"))         SCAN_FLAG(line.flags, UDMF_ML_BLOCKSIGHT);
    else if (scanner_string_match(s, "blockhitscan"))       SCAN_FLAG(line.flags, UDMF_ML_BLOCKHITSCAN);
    else if (scanner_string_match(s, "transparent"))        SCAN_FLAG(line.flags, UDMF_ML_TRANSPARENT);
    else if (scanner_string_match(s, "revealed"))           SCAN_FLAG(line.flags, UDMF_ML_REVEALED);
    else if (scanner_string_match(s, "noskywalls"))         SCAN_FLAG(line.flags, UDMF_ML_NOSKYWALLS);
    else if (scanner_string_match(s, "drawfullheight"))     SCAN_FLAG(line.flags, UDMF_ML_DRAWFULLHEIGHT);
    else if (scanner_string_match(s, "damagespecial"))      SCAN_FLAG(line.flags, UDMF_ML_DAMAGESPECIAL);
    else if (scanner_string_match(s, "deathspecial"))       SCAN_FLAG(line.flags, UDMF_ML_DEATHSPECIAL);
    else if (scanner_string_match(s, "blocklandmonsters"))  SCAN_FLAG(line.flags, UDMF_ML_BLOCKLANDMONSTERS);
    else if (scanner_string_match(s, "moreids"))            SCAN_STRING(line.moreids);
    else if (scanner_string_match(s, "arg0str"))            SCAN_STRING(line.arg0str);
    else dsda_SkipValue(s);
  }

  udmf_arr_push(&udmf_lines, &line);
}

static void dsda_ParseUDMFSideDef(scanner_t *s)
{
  udmf_side_t side;
  memset(&side, 0, sizeof(side));

  scanner_must_get_token(s, '{');
  while (!scanner_check_token(s, '}'))
  {
    scanner_must_get_token(s, TK_Identifier);

    if      (scanner_string_match(s, "offsetx"))        SCAN_INT(side.offsetx);
    else if (scanner_string_match(s, "offsety"))        SCAN_INT(side.offsety);
    else if (scanner_string_match(s, "sector"))         SCAN_INT(side.sector);
    else if (scanner_string_match(s, "light"))          SCAN_INT(side.light);
    else if (scanner_string_match(s, "light_top"))      SCAN_INT(side.light_top);
    else if (scanner_string_match(s, "light_mid"))      SCAN_INT(side.light_mid);
    else if (scanner_string_match(s, "light_bottom"))   SCAN_INT(side.light_bottom);
    else if (scanner_string_match(s, "scalex_top"))     SCAN_FLOAT(side.scalex_top);
    else if (scanner_string_match(s, "scaley_top"))     SCAN_FLOAT(side.scaley_top);
    else if (scanner_string_match(s, "scalex_mid"))     SCAN_FLOAT(side.scalex_mid);
    else if (scanner_string_match(s, "scaley_mid"))     SCAN_FLOAT(side.scaley_mid);
    else if (scanner_string_match(s, "scalex_bottom"))  SCAN_FLOAT(side.scalex_bottom);
    else if (scanner_string_match(s, "scaley_bottom"))  SCAN_FLOAT(side.scaley_bottom);
    else if (scanner_string_match(s, "offsetx_top"))    SCAN_FLOAT(side.offsetx_top);
    else if (scanner_string_match(s, "offsety_top"))    SCAN_FLOAT(side.offsety_top);
    else if (scanner_string_match(s, "offsetx_mid"))    SCAN_FLOAT(side.offsetx_mid);
    else if (scanner_string_match(s, "offsety_mid"))    SCAN_FLOAT(side.offsety_mid);
    else if (scanner_string_match(s, "offsetx_bottom")) SCAN_FLOAT(side.offsetx_bottom);
    else if (scanner_string_match(s, "offsety_bottom")) SCAN_FLOAT(side.offsety_bottom);
    else if (scanner_string_match(s, "xscroll"))        SCAN_FLOAT(side.xscroll);
    else if (scanner_string_match(s, "yscroll"))        SCAN_FLOAT(side.yscroll);
    else if (scanner_string_match(s, "xscrolltop"))     SCAN_FLOAT(side.xscrolltop);
    else if (scanner_string_match(s, "yscrolltop"))     SCAN_FLOAT(side.yscrolltop);
    else if (scanner_string_match(s, "xscrollmid"))     SCAN_FLOAT(side.xscrollmid);
    else if (scanner_string_match(s, "yscrollmid"))     SCAN_FLOAT(side.yscrollmid);
    else if (scanner_string_match(s, "xscrollbottom"))  SCAN_FLOAT(side.xscrollbottom);
    else if (scanner_string_match(s, "yscrollbottom"))  SCAN_FLOAT(side.yscrollbottom);
    else if (scanner_string_match(s, "lightabsolute"))        SCAN_FLAG(side.flags, UDMF_SF_LIGHTABSOLUTE);
    else if (scanner_string_match(s, "lightfog"))             SCAN_FLAG(side.flags, UDMF_SF_LIGHTFOG);
    else if (scanner_string_match(s, "nofakecontrast"))       SCAN_FLAG(side.flags, UDMF_SF_NOFAKECONTRAST);
    else if (scanner_string_match(s, "smoothlighting"))       SCAN_FLAG(side.flags, UDMF_SF_SMOOTHLIGHTING);
    else if (scanner_string_match(s, "clipmidtex"))           SCAN_FLAG(side.flags, UDMF_SF_CLIPMIDTEX);
    else if (scanner_string_match(s, "wrapmidtex"))           SCAN_FLAG(side.flags, UDMF_SF_WRAPMIDTEX);
    else if (scanner_string_match(s, "nodecals"))             SCAN_FLAG(side.flags, UDMF_SF_NODECALS);
    else if (scanner_string_match(s, "lightabsolute_top"))    SCAN_FLAG(side.flags, UDMF_SF_LIGHTABSOLUTETOP);
    else if (scanner_string_match(s, "lightabsolute_mid"))    SCAN_FLAG(side.flags, UDMF_SF_LIGHTABSOLUTEMID);
    else if (scanner_string_match(s, "lightabsolute_bottom")) SCAN_FLAG(side.flags, UDMF_SF_LIGHTABSOLUTEBOTTOM);
    else if (scanner_string_match(s, "texturetop"))     SCAN_STRING_N(side.texturetop, 8);
    else if (scanner_string_match(s, "texturebottom"))  SCAN_STRING_N(side.texturebottom, 8);
    else if (scanner_string_match(s, "texturemiddle"))  SCAN_STRING_N(side.texturemiddle, 8);
    else dsda_SkipValue(s);
  }

  udmf_arr_push(&udmf_sides, &side);
}

static void dsda_ParseUDMFVertex(scanner_t *s)
{
  udmf_vertex_t vertex;
  memset(&vertex, 0, sizeof(vertex));

  scanner_must_get_token(s, '{');
  while (!scanner_check_token(s, '}'))
  {
    scanner_must_get_token(s, TK_Identifier);

    if      (scanner_string_match(s, "x")) SCAN_FLOAT_STRING(vertex.x);
    else if (scanner_string_match(s, "y")) SCAN_FLOAT_STRING(vertex.y);
    else dsda_SkipValue(s);
  }

  udmf_arr_push(&udmf_vertices, &vertex);
}

static void dsda_ParseUDMFSector(scanner_t *s)
{
  udmf_sector_t sector;
  memset(&sector, 0, sizeof(sector));

  scanner_must_get_token(s, '{');
  while (!scanner_check_token(s, '}'))
  {
    scanner_must_get_token(s, TK_Identifier);

    if      (scanner_string_match(s, "heightfloor"))      SCAN_INT(sector.heightfloor);
    else if (scanner_string_match(s, "heightceiling"))    SCAN_INT(sector.heightceiling);
    else if (scanner_string_match(s, "lightlevel"))       SCAN_INT(sector.lightlevel);
    else if (scanner_string_match(s, "special"))          SCAN_INT(sector.special);
    else if (scanner_string_match(s, "id"))               SCAN_INT(sector.id);
    else if (scanner_string_match(s, "lightfloor"))       SCAN_INT(sector.lightfloor);
    else if (scanner_string_match(s, "lightceiling"))     SCAN_INT(sector.lightceiling);
    else if (scanner_string_match(s, "damageamount"))     SCAN_INT(sector.damageamount);
    else if (scanner_string_match(s, "damageinterval"))   SCAN_INT(sector.damageinterval);
    else if (scanner_string_match(s, "leakiness"))        SCAN_INT(sector.leakiness);
    else if (scanner_string_match(s, "scrollfloormode"))  SCAN_INT(sector.scrollfloormode);
    else if (scanner_string_match(s, "scrollceilingmode"))SCAN_INT(sector.scrollceilingmode);
    else if (scanner_string_match(s, "thrustgroup"))      SCAN_INT(sector.thrustgroup);
    else if (scanner_string_match(s, "thrustlocation"))   SCAN_INT(sector.thrustlocation);
    else if (scanner_string_match(s, "xpanningfloor"))    SCAN_FLOAT(sector.xpanningfloor);
    else if (scanner_string_match(s, "ypanningfloor"))    SCAN_FLOAT(sector.ypanningfloor);
    else if (scanner_string_match(s, "xpanningceiling"))  SCAN_FLOAT(sector.xpanningceiling);
    else if (scanner_string_match(s, "ypanningceiling"))  SCAN_FLOAT(sector.ypanningceiling);
    else if (scanner_string_match(s, "xscalefloor"))      SCAN_FLOAT(sector.xscalefloor);
    else if (scanner_string_match(s, "yscalefloor"))      SCAN_FLOAT(sector.yscalefloor);
    else if (scanner_string_match(s, "xscaleceiling"))    SCAN_FLOAT(sector.xscaleceiling);
    else if (scanner_string_match(s, "yscaleceiling"))    SCAN_FLOAT(sector.yscaleceiling);
    else if (scanner_string_match(s, "rotationfloor"))    SCAN_FLOAT(sector.rotationfloor);
    else if (scanner_string_match(s, "rotationceiling"))  SCAN_FLOAT(sector.rotationceiling);
    else if (scanner_string_match(s, "xscrollfloor"))     SCAN_FLOAT(sector.xscrollfloor);
    else if (scanner_string_match(s, "yscrollfloor"))     SCAN_FLOAT(sector.yscrollfloor);
    else if (scanner_string_match(s, "xscrollceiling"))   SCAN_FLOAT(sector.xscrollceiling);
    else if (scanner_string_match(s, "yscrollceiling"))   SCAN_FLOAT(sector.yscrollceiling);
    else if (scanner_string_match(s, "xthrust"))          SCAN_FLOAT_STRING(sector.xthrust);
    else if (scanner_string_match(s, "ythrust"))          SCAN_FLOAT_STRING(sector.ythrust);
    else if (scanner_string_match(s, "gravity"))          SCAN_FLOAT_STRING(sector.gravity);
    else if (scanner_string_match(s, "frictionfactor"))   SCAN_FLOAT_STRING(sector.frictionfactor);
    else if (scanner_string_match(s, "movefactor"))       SCAN_FLOAT_STRING(sector.movefactor);
    else if (scanner_string_match(s, "lightfloorabsolute"))   SCAN_FLAG(sector.flags, UDMF_SECF_LIGHTFLOORABSOLUTE);
    else if (scanner_string_match(s, "lightceilingabsolute")) SCAN_FLAG(sector.flags, UDMF_SECF_LIGHTCEILINGABSOLUTE);
    else if (scanner_string_match(s, "silent"))               SCAN_FLAG(sector.flags, UDMF_SECF_SILENT);
    else if (scanner_string_match(s, "nofallingdamage"))      SCAN_FLAG(sector.flags, UDMF_SECF_NOFALLINGDAMAGE);
    else if (scanner_string_match(s, "dropactors"))           SCAN_FLAG(sector.flags, UDMF_SECF_DROPACTORS);
    else if (scanner_string_match(s, "norespawn"))            SCAN_FLAG(sector.flags, UDMF_SECF_NORESPAWN);
    else if (scanner_string_match(s, "hidden"))               SCAN_FLAG(sector.flags, UDMF_SECF_HIDDEN);
    else if (scanner_string_match(s, "waterzone"))            SCAN_FLAG(sector.flags, UDMF_SECF_WATERZONE);
    else if (scanner_string_match(s, "damageterraineffect"))  SCAN_FLAG(sector.flags, UDMF_SECF_DAMAGETERRAINEFFECT);
    else if (scanner_string_match(s, "damagehazard"))         SCAN_FLAG(sector.flags, UDMF_SECF_DAMAGEHAZARD);
    else if (scanner_string_match(s, "noattack"))             SCAN_FLAG(sector.flags, UDMF_SECF_NOATTACK);
    else if (scanner_string_match(s, "texturefloor"))     SCAN_STRING_N(sector.texturefloor, 8);
    else if (scanner_string_match(s, "textureceiling"))   SCAN_STRING_N(sector.textureceiling, 8);
    else if (scanner_string_match(s, "colormap"))         SCAN_STRING(sector.colormap);
    else if (scanner_string_match(s, "skyfloor"))         SCAN_STRING(sector.skyfloor);
    else if (scanner_string_match(s, "skyceiling"))       SCAN_STRING(sector.skyceiling);
    else if (scanner_string_match(s, "moreids"))          SCAN_STRING(sector.moreids);
    else dsda_SkipValue(s);
  }

  udmf_arr_push(&udmf_sectors, &sector);
}

static void dsda_ParseUDMFThing(scanner_t *s)
{
  udmf_thing_t thing;
  memset(&thing, 0, sizeof(thing));
  thing.gravity = "1.0";
  thing.health = "1.0";
  thing.floatbobphase = -1;
  thing.alpha = 1.0f;

  scanner_must_get_token(s, '{');
  while (!scanner_check_token(s, '}'))
  {
    scanner_must_get_token(s, TK_Identifier);

    if      (scanner_string_match(s, "id"))            SCAN_INT(thing.id);
    else if (scanner_string_match(s, "angle"))         SCAN_INT(thing.angle);
    else if (scanner_string_match(s, "type"))          SCAN_INT(thing.type);
    else if (scanner_string_match(s, "special"))       SCAN_INT(thing.special);
    else if (scanner_string_match(s, "arg0"))          SCAN_INT(thing.arg0);
    else if (scanner_string_match(s, "arg1"))          SCAN_INT(thing.arg1);
    else if (scanner_string_match(s, "arg2"))          SCAN_INT(thing.arg2);
    else if (scanner_string_match(s, "arg3"))          SCAN_INT(thing.arg3);
    else if (scanner_string_match(s, "arg4"))          SCAN_INT(thing.arg4);
    else if (scanner_string_match(s, "floatbobphase")) SCAN_INT(thing.floatbobphase);
    else if (scanner_string_match(s, "x"))             SCAN_FLOAT_STRING(thing.x);
    else if (scanner_string_match(s, "y"))             SCAN_FLOAT_STRING(thing.y);
    else if (scanner_string_match(s, "height"))        SCAN_FLOAT_STRING(thing.height);
    else if (scanner_string_match(s, "gravity"))       SCAN_FLOAT_STRING(thing.gravity);
    else if (scanner_string_match(s, "health"))        SCAN_FLOAT_STRING(thing.health);
    else if (scanner_string_match(s, "scalex"))        SCAN_FLOAT(thing.scalex);
    else if (scanner_string_match(s, "scaley"))        SCAN_FLOAT(thing.scaley);
    else if (scanner_string_match(s, "scale"))         SCAN_FLOAT(thing.scale);
    else if (scanner_string_match(s, "alpha"))         SCAN_FLOAT(thing.alpha);
    else if (scanner_string_match(s, "skill1"))        SCAN_FLAG(thing.flags, UDMF_TF_SKILL1);
    else if (scanner_string_match(s, "skill2"))        SCAN_FLAG(thing.flags, UDMF_TF_SKILL2);
    else if (scanner_string_match(s, "skill3"))        SCAN_FLAG(thing.flags, UDMF_TF_SKILL3);
    else if (scanner_string_match(s, "skill4"))        SCAN_FLAG(thing.flags, UDMF_TF_SKILL4);
    else if (scanner_string_match(s, "skill5"))        SCAN_FLAG(thing.flags, UDMF_TF_SKILL5);
    else if (scanner_string_match(s, "ambush"))        SCAN_FLAG(thing.flags, UDMF_TF_AMBUSH);
    else if (scanner_string_match(s, "single"))        SCAN_FLAG(thing.flags, UDMF_TF_SINGLE);
    else if (scanner_string_match(s, "dm"))            SCAN_FLAG(thing.flags, UDMF_TF_DM);
    else if (scanner_string_match(s, "coop"))          SCAN_FLAG(thing.flags, UDMF_TF_COOP);
    else if (scanner_string_match(s, "friend"))        SCAN_FLAG(thing.flags, UDMF_TF_FRIEND);
    else if (scanner_string_match(s, "dormant"))       SCAN_FLAG(thing.flags, UDMF_TF_DORMANT);
    else if (scanner_string_match(s, "class1"))        SCAN_FLAG(thing.flags, UDMF_TF_CLASS1);
    else if (scanner_string_match(s, "class2"))        SCAN_FLAG(thing.flags, UDMF_TF_CLASS2);
    else if (scanner_string_match(s, "class3"))        SCAN_FLAG(thing.flags, UDMF_TF_CLASS3);
    else if (scanner_string_match(s, "standing"))      SCAN_FLAG(thing.flags, UDMF_TF_STANDING);
    else if (scanner_string_match(s, "strifeally"))    SCAN_FLAG(thing.flags, UDMF_TF_STRIFEALLY);
    else if (scanner_string_match(s, "translucent"))   SCAN_FLAG(thing.flags, UDMF_TF_TRANSLUCENT);
    else if (scanner_string_match(s, "invisible"))     SCAN_FLAG(thing.flags, UDMF_TF_INVISIBLE);
    else if (scanner_string_match(s, "countsecret"))   SCAN_FLAG(thing.flags, UDMF_TF_COUNTSECRET);
    else if (scanner_string_match(s, "arg0str"))       SCAN_STRING(thing.arg0str);
    else dsda_SkipValue(s);
  }

  udmf_arr_push(&udmf_things, &thing);
}

/* ------------------------------------------------------------------ */
/* Top-level dispatch                                                  */
/* ------------------------------------------------------------------ */

static void dsda_ParseUDMFIdentifier(scanner_t *s)
{
  scanner_must_get_token(s, TK_Identifier);

  if (scanner_string_match(s, "namespace"))
  {
    scanner_must_get_token(s, '=');
    scanner_must_get_token(s, TK_StringConst);

    if (scanner_string_match(s, "doom") && !raven)
      udmf_namespace = UDMF_DOOM;
    else if (scanner_string_match(s, "heretic") && heretic)
      udmf_namespace = UDMF_HERETIC;
    else if (scanner_string_match(s, "hexen") && hexen)
      udmf_namespace = UDMF_HEXEN;
    else if (scanner_string_match(s, "dsda") && !raven)
      udmf_namespace = UDMF_DSDA;
    else if (scanner_string_match(s, "zdoom") && !raven)
      udmf_namespace = UDMF_ZDOOM;
    else
      udmf_err("Unsupported UDMF namespace \"%s\"", s->string);

    scanner_must_get_token(s, ';');
  }
  else if (scanner_string_match(s, "linedef")) dsda_ParseUDMFLineDef(s);
  else if (scanner_string_match(s, "sidedef")) dsda_ParseUDMFSideDef(s);
  else if (scanner_string_match(s, "vertex"))  dsda_ParseUDMFVertex(s);
  else if (scanner_string_match(s, "sector"))  dsda_ParseUDMFSector(s);
  else if (scanner_string_match(s, "thing"))   dsda_ParseUDMFThing(s);
  else dsda_SkipValue(s);
}

void dsda_ParseUDMF(const unsigned char* buffer, size_t length, udmf_errorfunc err)
{
  scanner_t s;

  udmf_err = err;
  scanner_set_error_callback(err);

  /* Release any backing arrays from a previous parse.  The element payloads
   * (strings) live in the level zone and are freed by the engine on level
   * unload, so we only free the array storage itself here. */
  udmf_arr_clear(&udmf_lines);
  udmf_arr_clear(&udmf_sides);
  udmf_arr_clear(&udmf_vertices);
  udmf_arr_clear(&udmf_sectors);
  udmf_arr_clear(&udmf_things);

  udmf_arr_init(&udmf_lines,    sizeof(udmf_line_t));
  udmf_arr_init(&udmf_sides,    sizeof(udmf_side_t));
  udmf_arr_init(&udmf_vertices, sizeof(udmf_vertex_t));
  udmf_arr_init(&udmf_sectors,  sizeof(udmf_sector_t));
  udmf_arr_init(&udmf_things,   sizeof(udmf_thing_t));

  scanner_init(&s, (const char *)buffer, (int)length);

  while (scanner_tokens_left(&s))
    dsda_ParseUDMFIdentifier(&s);

  if (!udmf_lines.count || !udmf_sides.count || !udmf_vertices.count ||
      !udmf_sectors.count || !udmf_things.count)
    udmf_err("Insufficient UDMF data");

  udmf.lines        = (udmf_line_t *)udmf_lines.data;
  udmf.num_lines    = udmf_lines.count;
  udmf.sides        = (udmf_side_t *)udmf_sides.data;
  udmf.num_sides    = udmf_sides.count;
  udmf.vertices     = (udmf_vertex_t *)udmf_vertices.data;
  udmf.num_vertices = udmf_vertices.count;
  udmf.sectors      = (udmf_sector_t *)udmf_sectors.data;
  udmf.num_sectors  = udmf_sectors.count;
  udmf.things       = (udmf_thing_t *)udmf_things.data;
  udmf.num_things   = udmf_things.count;

  scanner_free(&s);
}
