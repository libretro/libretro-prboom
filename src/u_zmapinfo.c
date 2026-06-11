/* u_zmapinfo.c: translate ZDoom old-syntax MAPINFO into UMAPINFO entries.
 *
 * ZDoom-targeted wads carry episode structure in a MAPINFO lump: per-map
 * next/secretnext chains (including the EndGame / endbunny episode-end
 * sentinels), sky and music names, par times, and boss special actions,
 * plus clusterdef blocks holding the episode-end text screens.  chex3.wad
 * ends every episode at M5 ("next EndGame1"), so without this the engine
 * would march on to a nonexistent E1M6.
 *
 * Rather than adding another mapinfo consumer to the game code, the lump
 * is translated into the engine's existing UMAPINFO tables (U_mapinfo /
 * mapentry_t), which g_game, the intermission and the finale already
 * consult: "next EndGameN" becomes the endpic "!" sentinel, "next
 * endbunny" becomes "$BUNNY", clusterdef exittext becomes the intertext
 * of the maps that leave the cluster, and specialaction_* lines become
 * UMAPINFO bossactions for the classic A_BossDeath callers.
 *
 * "lookup" indirections resolve through the wad's LANGUAGE lump (ZDoom
 * string table); keys missing there are left unset so the engine
 * defaults apply.  Only the old (brace-less) MAPINFO syntax is handled;
 * unknown keys are skipped to the end of their line. */

#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "doomstat.h"
#include "info.h"
#include "dsda_hacked.h"
#include "w_wad.h"
#include "lprintf.h"
#include "u_scanner.h"
#include "u_mapinfo.h"
#include "d_deh.h"
#include "u_zmapinfo.h"

/* ---- LANGUAGE lump string table ---------------------------------------- */

typedef struct
{
  char *key;
  char *value;
} zlang_entry_t;

static int *map_noinfight;   /* ZDoom 'noinfighting' per map */
static int *map_sucktime;    /* ZDoom 'sucktime' hours per map (0 = none) */

static zlang_entry_t *zlang;
static int            zlang_count;
static int            zlang_parsed;

/* Parse one LANGUAGE lump's "KEY = "value" ... ;" entries into the table.
 * A key is either a bareword identifier (VNS_NUM_SCRIPTS, GOTCHAINSAW) or a
 * quoted string ("knight/default" -> "st_hkn01" portrait aliases that the
 * VN runtime resolves the same way through PrintLocalized).  Adjacent string
 * constants are concatenated, matching ZDoom. */
static void Z_ParseLanguageLump(int lump)
{
  u_scanner_t s;

  s = U_ScanOpen(W_CacheLumpNum(lump), W_LumpLength(lump), "LANGUAGE");
  while (U_HasTokensLeft(&s))
  {
    char *key = NULL;

    /* "[enu default]" section headers: skip the bracketed tokens */
    if (U_CheckToken(&s, '['))
    {
      while (U_HasTokensLeft(&s) && !U_CheckToken(&s, ']'))
        U_GetNextToken(&s, TRUE);
      continue;
    }

    if (s.token == TK_Identifier || s.token == TK_StringConst)
      key = strdup(s.string);

    if (key && U_CheckToken(&s, '='))
    {
      /* concatenate adjacent string constants up to ';' */
      char  *val = NULL;
      size_t len = 0;
      while (U_GetNextToken(&s, TRUE) && s.token == TK_StringConst)
      {
        size_t add = strlen(s.string);
        val = realloc(val, len + add + 1);
        memcpy(val + len, s.string, add + 1);
        len += add;
      }
      if (val)
      {
        zlang = realloc(zlang, (zlang_count + 1) * sizeof(*zlang));
        zlang[zlang_count].key = key;
        zlang[zlang_count].value = val;
        zlang_count++;
      }
      else
        free(key);
      continue;        /* current token is the non-string (';' etc.) */
    }
    free(key);
    U_GetNextToken(&s, TRUE);
  }
  U_ScanClose(&s);
  W_UnlockLumpNum(lump);
}

/* A pk3 names every root-level "language.*" member LANGUAGE (basename up to
 * the first '.'), so a mod's per-topic tables (language.vns_aosoth, ...)
 * arrive as several lumps that all share that name.  Walk every one; later
 * lumps win on duplicate keys, which is the load order ZDoom uses too. */
static void Z_ParseLanguage(void)
{
  int lump;

  zlang_parsed = 1;
  for (lump = -1; (lump = W_ListNumFromName("LANGUAGE", lump)) >= 0; )
    Z_ParseLanguageLump(lump);
}

/* Route the LANGUAGE table onto the engine's BEX string slots: pickup
 * and lock messages, menu prompts, cheat feedback, automap level names,
 * episode end texts.  Keys ZDoom renamed map onto their BEX mnemonics;
 * keys with no engine counterpart (OB_* obituaries, LOCKDEFS-only
 * messages) simply find no slot and are skipped.  Runs before DEHACKED
 * processing, so a deh patch still overrides, matching ZDoom. */
void U_ZLanguageApplyStrings(void)
{
  static const struct { const char *zname; const char *bex; } alias[] = {
    { "GOTGOGGLES",    "GOTVISOR"      },
    { "GOTBLUEFLEM",   "GOTBLUESKUL"   },
    { "GOTYELLOWFLEM", "GOTYELWSKUL"   },
    { "GOTREDFLEM",    "GOTREDSKULL"   },
  };
  int i, applied = 0;

  if (!zlang_parsed)
    Z_ParseLanguage();

  for (i = 0; i < zlang_count; i++)
  {
    const char *key = zlang[i].key;
    unsigned k;
    for (k = 0; k < sizeof(alias) / sizeof(alias[0]); k++)
      if (!strcasecmp(key, alias[k].zname))
      {
        key = alias[k].bex;
        break;
      }
    if (deh_SetStringByMnemonic(key, zlang[i].value))
      applied++;
  }

  if (applied)
    lprintf(LO_INFO, "U_ZLanguageApplyStrings: %d strings from LANGUAGE\n",
            applied);
}

/* ZDoom MAPINFO 'noinfighting': monsters on this map never acquire
 * other monsters as targets from damage (retaliation against players
 * is untouched).  Queried with the live gamemapinfo pointer. */
/* ZDoom MAPINFO 'sucktime': hours of level time after which the
 * intermission shows SUCKS instead of the clock (0 = never). */
int U_ZMapSuckTime(const mapentry_t *e)
{
  int idx;
  if (!e || !U_mapinfo.maps || !map_sucktime)
    return 0;
  idx = (int)(e - U_mapinfo.maps);
  if (idx < 0 || idx >= (int)U_mapinfo.mapcount)
    return 0;
  return map_sucktime[idx];
}

dbool U_ZMapNoInfighting(const mapentry_t *e)
{
  int idx;
  if (!e || !U_mapinfo.maps || !map_noinfight)
    return false;
  idx = (int)(e - U_mapinfo.maps);
  if (idx < 0 || idx >= (int)U_mapinfo.mapcount)
    return false;
  return map_noinfight[idx] != 0;
}

const char *U_ZLanguageLookup(const char *key)
{
  int i;
  if (!zlang_parsed)
    Z_ParseLanguage();
  /* scan newest-first so a later lump's redefinition wins, as in ZDoom */
  for (i = zlang_count - 1; i >= 0; i--)
    if (!strcasecmp(zlang[i].key, key))
      return zlang[i].value;
  return NULL;
}

/* ---- MAPINFO translation ------------------------------------------------ */

#define MAX_ZCLUSTERS 16

typedef struct
{
  int   id;
  char  flat[9];
  char  music[9];
  char *exittext;            /* resolved through LANGUAGE when "lookup" */
} zcluster_t;

static zcluster_t zclusters[MAX_ZCLUSTERS];
static int        num_zclusters;

/* per-map cluster ids, parallel to U_mapinfo.maps */
static int *map_cluster;

static void zlump_name(char out[9], const char *in)
{
  /* clamped memcpy rather than strncpy: the source can legitimately be
   * longer than 8 chars (truncation is the point), which is exactly
   * what -Wstringop-truncation flags strncpy for */
  size_t n = strlen(in);
  if (n > 8)
    n = 8;
  memcpy(out, in, n);
  out[n] = 0;
}

static mapentry_t *Z_NewMapEntry(const char *mapname)
{
  mapentry_t *e;
  U_mapinfo.mapcount++;
  U_mapinfo.maps = realloc(U_mapinfo.maps,
                           U_mapinfo.mapcount * sizeof(*U_mapinfo.maps));
  map_cluster = realloc(map_cluster, U_mapinfo.mapcount * sizeof(*map_cluster));
  map_cluster[U_mapinfo.mapcount - 1] = -1;
  map_noinfight = realloc(map_noinfight,
                          U_mapinfo.mapcount * sizeof(*map_noinfight));
  map_noinfight[U_mapinfo.mapcount - 1] = 0;
  map_sucktime = realloc(map_sucktime,
                         U_mapinfo.mapcount * sizeof(*map_sucktime));
  map_sucktime[U_mapinfo.mapcount - 1] = 0;
  e = &U_mapinfo.maps[U_mapinfo.mapcount - 1];
  memset(e, 0, sizeof(*e));
  e->mapname = strdup(mapname);
  return e;
}

/* Same-line argument fetch: advance the scanner; returns 1 when the new
 * token is still on `line` (an argument), 0 when it begins the next line
 * (current token must be reprocessed by the caller), -1 at end of input.
 * New-syntax (ZMAPINFO-style) lumps write `key = value`; the '=' is
 * skipped so both syntaxes feed the same key handlers. */
static int z_arg(u_scanner_t *s, unsigned int line)
{
  if (!U_GetNextToken(s, TRUE))
    return -1;
  if (s->token == '=' && s->tokenLine == line)
  {
    if (!U_GetNextToken(s, TRUE))
      return -1;
  }
  return s->tokenLine == line ? 1 : 0;
}

/* ---- episode menu translation ------------------------------------------ */

/* m_menu.c's UMAPINFO hook; same private declaration u_mapinfo.c uses. */
void M_AddEpisode(const char *map, char *def);

/* Episode block being collected.  ZDoom's old-syntax episode directive is
 *
 *     clearepisodes
 *     episode E1M1
 *     picname M_EPI1
 *
 * (chex3.wad defines exactly three of these).  Without translating them
 * the menu falls back to M_Init's E?M1 lump probe, and when chex3.wad is
 * routed as a PWAD on top of an Ultimate Doom IWAD that probe finds
 * doom.wad's E4M1, so the episode menu grows a fourth entry drawn with
 * doom.wad's M_EPI4 patch ("Thy Flesh Consumed") that starts Doom maps. */
static char  zepi_map[9];
static char  zepi_pic[9];
static char *zepi_name;      /* heap; may be NULL */
static char  zepi_key;
static int   zepi_open;      /* currently inside an episode block */
static int   zepi_optional;  /* ZDoom 'optional': skip if map lump absent */
static int   zepi_remove;    /* ZDoom 'remove': do not add this episode */
static int   zepi_added;     /* episodes fed to the menu (for the summary) */

/* Close the pending episode block and feed it to the menu.  M_AddEpisode
 * keeps the title pointer (second line of def) alive as the menu item's
 * alttext, so the composed buffer is deliberately never freed. */
static void Z_FlushEpisode(void)
{
  char  *def;
  size_t len;

  if (!zepi_open)
    return;
  zepi_open = 0;

  /* 'remove' asks for deletion of an existing entry; deleting from the
   * already-built menu is not supported, so at least do not add it.
   * 'optional' episodes only appear when their start map exists. */
  if (zepi_remove ||
      (zepi_optional && W_CheckNumForName(zepi_map) < 0))
  {
    free(zepi_name);
    zepi_name = NULL;
    return;
  }

  len = strlen(zepi_pic) + 1;
  if (zepi_name)
    len += strlen(zepi_name) + 1;
  if (zepi_key)
    len += 2;
  def = malloc(len);
  strcpy(def, zepi_pic);
  if (zepi_name)
  {
    strcat(def, "\n");
    strcat(def, zepi_name);
    if (zepi_key)
    {
      size_t at = strlen(def);
      def[at]     = '\n';
      def[at + 1] = zepi_key;
      def[at + 2] = '\0';
    }
  }
  free(zepi_name);
  zepi_name = NULL;
  M_AddEpisode(zepi_map, def);
  zepi_added++;
}

/* the classic A_BossDeath callers a ZDoom special action may come from */
static void Z_AddBossActions(mapentry_t *e, int special, int tag)
{
  static const char *callers[] =
    { "BaronOfHell", "Cyberdemon", "SpiderMastermind", NULL };
  int c, i;

  for (c = 0; callers[c]; c++)
    for (i = 0; i < num_mobj_types; i++)
      if (mobjinfo[i].actorname &&
          !strcasecmp(mobjinfo[i].actorname, callers[c]))
      {
        e->numbossactions = (e->numbossactions <= 0)
                              ? 1 : e->numbossactions + 1;
        e->bossactions = realloc(e->bossactions,
                                 e->numbossactions * sizeof(bossaction_t));
        e->bossactions[e->numbossactions - 1].type    = i;
        e->bossactions[e->numbossactions - 1].special = special;
        e->bossactions[e->numbossactions - 1].tag     = tag;
        break;
      }
}

int U_ParseZMapInfo(const char *buffer, size_t length)
{
  u_scanner_t s = U_ScanOpen(buffer, length, "MAPINFO");
  mapentry_t *map = NULL;        /* current map block */
  zcluster_t *clus = NULL;       /* current clusterdef block */
  unsigned int i;
  int tok = U_GetNextToken(&s, TRUE) ? 1 : -1;

  while (tok > 0)
  {
    unsigned int line = s.tokenLine;

    if (s.token == TK_Identifier)
    {
      /* ---- block openers ---- */
      if (!strcasecmp(s.string, "map"))
      {
        clus = NULL;
        map = NULL;
        Z_FlushEpisode();
        if ((tok = z_arg(&s, line)) > 0 && s.token == TK_Identifier)
        {
          map = Z_NewMapEntry(s.string);
          if ((tok = z_arg(&s, line)) > 0)
          {
            if (!strcasecmp(s.string, "lookup"))
            {
              if ((tok = z_arg(&s, line)) > 0)
              {
                const char *v = U_ZLanguageLookup(s.string);
                if (v)
                  map->levelname = strdup(v);
              }
            }
            else if (s.token == TK_StringConst)
            {
              /* new syntax: map MAPxx "display name" */
              map->levelname = strdup(s.string);
            }
          }
        }
      }
      else if (!strcasecmp(s.string, "clusterdef") ||
               (!map && !strcasecmp(s.string, "cluster")))
      {
        map = NULL;
        clus = NULL;
        Z_FlushEpisode();
        if ((tok = z_arg(&s, line)) > 0 && num_zclusters < MAX_ZCLUSTERS)
        {
          clus = &zclusters[num_zclusters++];
          memset(clus, 0, sizeof(*clus));
          clus->id = s.number;
        }
      }
      else if (!strcasecmp(s.string, "episode"))
      {
        map = NULL;
        clus = NULL;
        Z_FlushEpisode();
        if ((tok = z_arg(&s, line)) > 0)
        {
          zlump_name(zepi_map, s.string);
          zepi_pic[0]   = 0;
          zepi_name     = NULL;
          zepi_key      = 0;
          zepi_optional = 0;
          zepi_remove   = 0;
          zepi_open     = 1;
        }
      }
      else if (!strcasecmp(s.string, "clearepisodes"))
      {
        static char zepi_clear[] = "-";
        map = NULL;
        clus = NULL;
        Z_FlushEpisode();
        M_AddEpisode("", zepi_clear);
      }
      else if (!strcasecmp(s.string, "gameinfo")        ||
               !strcasecmp(s.string, "skill")           ||
               !strcasecmp(s.string, "adddefaultmap")   ||
               !strcasecmp(s.string, "defaultmap")      ||
               !strcasecmp(s.string, "doomednums")      ||
               !strcasecmp(s.string, "spawnnums")       ||
               !strcasecmp(s.string, "conversationids") ||
               !strcasecmp(s.string, "intermission")    ||
               !strcasecmp(s.string, "automap")         ||
               !strcasecmp(s.string, "gamedefaults")    ||
               !strcasecmp(s.string, "damagetype"))
      {
        /* top-level blocks (both syntaxes) whose keys must not leak
         * into a stale map / cluster / episode context */
        map = NULL;
        clus = NULL;
        Z_FlushEpisode();
      }
      /* ---- episode keys ---- */
      else if (zepi_open)
      {
        if (!strcasecmp(s.string, "picname"))
        {
          if ((tok = z_arg(&s, line)) > 0)
            zlump_name(zepi_pic, s.string);
        }
        else if (!strcasecmp(s.string, "name"))
        {
          if ((tok = z_arg(&s, line)) > 0)
          {
            if (!strcasecmp(s.string, "lookup"))
            {
              if ((tok = z_arg(&s, line)) > 0)
              {
                const char *v = U_ZLanguageLookup(s.string);
                if (v)
                {
                  free(zepi_name);
                  zepi_name = strdup(v);
                }
              }
            }
            else
            {
              free(zepi_name);
              zepi_name = strdup(s.string);
            }
          }
        }
        else if (!strcasecmp(s.string, "key"))
        {
          if ((tok = z_arg(&s, line)) > 0)
            zepi_key = s.string[0];
        }
        else if (!strcasecmp(s.string, "optional"))
          zepi_optional = 1;
        else if (!strcasecmp(s.string, "remove"))
          zepi_remove = 1;
      }
      /* ---- cluster keys ---- */
      else if (clus)
      {
        if (!strcasecmp(s.string, "flat"))
        {
          if ((tok = z_arg(&s, line)) > 0)
            zlump_name(clus->flat, s.string);
        }
        else if (!strcasecmp(s.string, "music"))
        {
          if ((tok = z_arg(&s, line)) > 0)
            zlump_name(clus->music, s.string);
        }
        else if (!strcasecmp(s.string, "exittext"))
        {
          if ((tok = z_arg(&s, line)) > 0)
          {
            if (!strcasecmp(s.string, "lookup"))
            {
              if ((tok = z_arg(&s, line)) > 0)
              {
                const char *v = U_ZLanguageLookup(s.string);
                if (v && !clus->exittext)
                  clus->exittext = strdup(v);
              }
            }
            else if (!clus->exittext)
              clus->exittext = strdup(s.string);
          }
        }
      }
      /* ---- map keys ---- */
      else if (map)
      {
        if (!strcasecmp(s.string, "next"))
        {
          if ((tok = z_arg(&s, line)) > 0)
          {
            if (!strncasecmp(s.string, "EndGame", 7))
              strcpy(map->endpic, "!");
            else if (!strcasecmp(s.string, "endbunny"))
              strcpy(map->endpic, "$BUNNY");
            else
              zlump_name(map->nextmap, s.string);
          }
        }
        else if (!strcasecmp(s.string, "secretnext"))
        {
          if ((tok = z_arg(&s, line)) > 0)
            zlump_name(map->nextsecret, s.string);
        }
        else if (!strcasecmp(s.string, "sky1"))
        {
          if ((tok = z_arg(&s, line)) > 0)
            zlump_name(map->skytexture, s.string);
        }
        else if (!strcasecmp(s.string, "music"))
        {
          if ((tok = z_arg(&s, line)) > 0)
          {
            if (!strncasecmp(s.string, "$MUSIC_", 7))
            {
              /* gzdoom language indirection for the stock tracks:
               * $MUSIC_RUNNIN is D_RUNNIN */
              char dname[10];
              size_t n = strlen(s.string + 7);
              if (n > 6)
                n = 6;
              dname[0] = 'D';
              dname[1] = '_';
              memcpy(dname + 2, s.string + 7, n);
              dname[2 + n] = 0;
              zlump_name(map->music, dname);
            }
            else
              zlump_name(map->music, s.string);
          }
        }
        else if (!strcasecmp(s.string, "titlepatch"))
        {
          if ((tok = z_arg(&s, line)) > 0)
            zlump_name(map->levelpic, s.string);
        }
        else if (!strcasecmp(s.string, "par"))
        {
          if ((tok = z_arg(&s, line)) > 0)
            map->partime = s.number;
        }
        else if (!strcasecmp(s.string, "noinfighting"))
        {
          /* flag key, no argument */
          map_noinfight[U_mapinfo.mapcount - 1] = 1;
        }
        else if (!strcasecmp(s.string, "sucktime"))
        {
          if ((tok = z_arg(&s, line)) > 0)
            map_sucktime[U_mapinfo.mapcount - 1] = s.number;
        }
        else if (!strcasecmp(s.string, "cluster"))
        {
          if ((tok = z_arg(&s, line)) > 0)
            map_cluster[U_mapinfo.mapcount - 1] = s.number;
        }
        else if (!strcasecmp(s.string, "nointermission"))
          map->nointermission = 1;
        else if (!strcasecmp(s.string, "specialaction_lowerfloor"))
          Z_AddBossActions(map, 23, 666);
        else if (!strcasecmp(s.string, "specialaction_opendoor"))
          Z_AddBossActions(map, 29, 666);
        else if (!strcasecmp(s.string, "specialaction_exitlevel"))
          Z_AddBossActions(map, 11, 0);
      }
    }

    if (tok < 0)
      break;
    if (s.tokenLine != line)
    {
      tok = 1;                  /* token already current; reprocess it */
      continue;
    }                 /* already at the next line's first token */
    /* skip the rest of this line */
    while ((tok = U_GetNextToken(&s, TRUE) ? 1 : -1) > 0 &&
           s.tokenLine == line)
      ;
  }
  U_ScanClose(&s);

  /* an episode block ending at EOF still needs flushing */
  Z_FlushEpisode();

  /* episode-end maps inherit their cluster's text screen */
  for (i = 0; i < U_mapinfo.mapcount; i++)
  {
    mapentry_t *e = &U_mapinfo.maps[i];
    int c;
    if (!e->endpic[0] || map_cluster[i] < 0)
      continue;
    for (c = 0; c < num_zclusters; c++)
      if (zclusters[c].id == map_cluster[i])
      {
        if (zclusters[c].exittext && !e->intertext)
          e->intertext = strdup(zclusters[c].exittext);
        if (zclusters[c].flat[0])
          strcpy(e->interbackdrop, zclusters[c].flat);
        if (zclusters[c].music[0])
          strcpy(e->intermusic, zclusters[c].music);
        break;
      }
  }

  {
    int nboss = 0, ntext = 0;
    for (i = 0; i < U_mapinfo.mapcount; i++)
    {
      if (U_mapinfo.maps[i].numbossactions > 0)
        nboss += U_mapinfo.maps[i].numbossactions;
      if (U_mapinfo.maps[i].intertext)
        ntext++;
    }
    lprintf(LO_INFO,
            "U_ParseZMapInfo: %u maps, %d clusters, %d episodes, "
            "%d boss actions, %d text screens\n",
            U_mapinfo.mapcount, num_zclusters, zepi_added, nboss, ntext);
  }
  return 1;
}
