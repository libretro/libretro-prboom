/* Emacs style mode select   -*- C89 -*-
 *-----------------------------------------------------------------------------
 *
 * ZDoom SNDINFO monster-sound remaps for Doom-engine games.
 *
 * ZDoom-targeted wads such as chex3.wad carry a SNDINFO lump that binds
 * the engine's logical sound names ("demon/sight", "skull/melee") to
 * replacement lumps.  This engine plays sounds through mobjinfo's five
 * sound fields and a handful of hardcoded attack enums, so without the
 * lump's bindings the custom audio is unreachable: chex3's flemoids
 * grunt with Doom's zombie voices, and its quadrumpus class is silent
 * where the wad points a logical name at a lump the IWAD never shipped
 * (knight/sight -> dsbrssit exists, but our knight also needs it
 * because chex3 carries no DSKNTSIT).
 *
 * Parse the lump's monster lines - a tag containing '/' distinguishes
 * the ZDoom form from Hexen's $-keyword SNDINFO, which the Hexen layer
 * owns - and route each binding onto the owning mobj's sound field.
 * Bindings are per-mobj, not global sfx renames: chex3 redirects
 * sfx-sharing monsters differently (six monsters share dsdmact as
 * their active sound and the wad gives three of them three different
 * replacements), so the slot, not the sound, must change.
 *
 * A binding's target is either a lump the engine already has an sfx
 * for (demon/sight -> dscybsit reuses the cyberdemon's slot) or a
 * custom lump (grunt/active -> dscommon), which gets a fresh slot
 * grown through dsda_GetSfx and named so the lazy "ds%s" lump lookup
 * finds the wad's data.  Missing lumps skip the binding.
 *
 *-----------------------------------------------------------------------------
 */

#include "doomstat.h"
#include "info.h"
#include "sounds.h"
#include "w_wad.h"
#include "lprintf.h"
#include "dsda_hacked.h"
#include "u_zsndinfo.h"

#include <stdlib.h>
#include <string.h>

/* logical-name prefix -> the mobj whose field the binding lands on */
static const struct {
  const char *prefix;
  int mt;
} prefix_map[] = {
  { "grunt",    MT_POSSESSED },
  { "shotguy",  MT_SHOTGUY   },
  { "imp",      MT_TROOP     },
  { "demon",    MT_SERGEANT  },
  { "spectre",  MT_SHADOWS   },
  { "caco",     MT_HEAD      },
  { "baron",    MT_BRUISER   },
  { "knight",   MT_KNIGHT    },
  { "skull",    MT_SKULL     },
  { "spider",   MT_SPIDER    },
  { "cyber",    MT_CYBORG    },
  { "chainguy", MT_CHAINGUY  },
};

/* "<prefix>/shotx" is the prefix's projectile impact, not the monster */
static const struct {
  const char *prefix;
  int mt;
} shotx_map[] = {
  { "imp",   MT_TROOPSHOT   },
  { "caco",  MT_HEADSHOT    },
  { "baron", MT_BRUISERSHOT },
};

enum { ZF_SEE, ZF_ATTACK, ZF_PAIN, ZF_DEATH, ZF_ACTIVE };

static const struct {
  const char *suffix;
  int field;
} suffix_map[] = {
  { "sight",  ZF_SEE    },
  { "attack", ZF_ATTACK },
  { "melee",  ZF_ATTACK },
  { "pain",   ZF_PAIN   },
  { "death",  ZF_DEATH  },
  { "active", ZF_ACTIVE },
};

static int *field_of(mobjinfo_t *mi, int field)
{
  switch (field)
  {
    case ZF_SEE:    return &mi->seesound;
    case ZF_ATTACK: return &mi->attacksound;
    case ZF_PAIN:   return &mi->painsound;
    case ZF_DEATH:  return &mi->deathsound;
    default:        return &mi->activesound;
  }
}

/* slots this parse created, for dedupe (dssplat2 serves two bindings) */
#define MAX_ZSND_NEW 32
static int new_slots[MAX_ZSND_NEW];
static int num_new_slots;

/* find the sfx whose lump is `lump`, creating a slot for a lump no existing
 * sfx covers; -1 if the lump is absent from the wads.  `lump` may be a
 * Doom-style dsxxxx name or a ZDoom bare lump name (e.g. SOULSWA1); the sfx
 * slot is named with the DS prefix stripped when present, since the sfx
 * loader prepends DS first and falls back to the bare name. */
static int resolve_sfx(const char *lump, int orig_num_sfx, int *cursor,
                       int template_idx)
{
  int i;
  const char *stem = (lump[0] == 'd' || lump[0] == 'D') &&
                     (lump[1] == 's' || lump[1] == 'S')
                       ? lump + 2          /* past "ds" */
                       : lump;             /* bare ZDoom lump name */

  if (W_CheckNumForName(lump) < 0)
    return -1;

  for (i = 1; i < orig_num_sfx; i++)
    if (S_sfx[i].name && !strcasecmp(S_sfx[i].name, stem))
      return i;

  for (i = 0; i < num_new_slots; i++)
    if (!strcasecmp(S_sfx[new_slots[i]].name, stem))
      return new_slots[i];

  if (num_new_slots >= MAX_ZSND_NEW)
    return -1;

  {
    int idx = (*cursor)++;
    /* dsda_GetSfx can move S_sfx; read the template through the new
     * base after the growth, never through a pre-growth pointer */
    sfxinfo_t *sfx = dsda_GetSfx(idx);
    char *name = malloc(strlen(stem) + 1);

    strcpy(name, stem);
    sfx->name = name;
    if (template_idx > 0)
    {
      sfx->singularity = S_sfx[template_idx].singularity;
      sfx->priority    = S_sfx[template_idx].priority;
    }
    else
    {
      sfx->singularity = false;
      sfx->priority    = 98;
    }
    /* pitch/volume/lumpnum/usefulness already seeded by reset_sfx */

    new_slots[num_new_slots++] = idx;
    return idx;
  }
}

void U_ZDoomLoadSndInfo(void)
{
  int lumpnum, orig_num_sfx, cursor, applied = 0;
  const char *data, *p, *end;

  if (raven)
    return;                              /* Hexen's layer owns SNDINFO */

  lumpnum = W_CheckNumForName("SNDINFO");
  if (lumpnum < 0)
    return;

  data = W_CacheLumpNum(lumpnum);
  end  = data + W_LumpLength(lumpnum);
  orig_num_sfx = num_sfx;
  cursor       = num_sfx;
  num_new_slots = 0;

  for (p = data; p < end; )
  {
    char tag[40], lump[16];
    unsigned n;
    const char *slash;

    /* skip whitespace and blank lines */
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
      p++;
    if (p >= end)
      break;
    if (*p == ';' || (*p == '/' && p + 1 < end && p[1] == '/'))
    {
      while (p < end && *p != '\n')
        p++;
      continue;
    }

    for (n = 0; p < end && *p > ' ' && n < sizeof(tag) - 1; p++)
      tag[n++] = *p;
    tag[n] = 0;
    while (p < end && (*p == ' ' || *p == '\t'))
      p++;
    for (n = 0; p < end && *p > ' ' && n < sizeof(lump) - 1; p++)
      lump[n++] = *p;
    lump[n] = 0;
    while (p < end && *p != '\n')
      p++;

    /* the ZDoom form is "logical/name lumpname"; Hexen tags and
     * $-commands carry no slash.  The target lump may be a Doom dsxxxx
     * name or a bare ZDoom lump name -- accept both. */
    slash = strchr(tag, '/');
    if (!slash || !lump[0])
      continue;

    /* weapons/rocklx: the rocket's impact */
    if (!strcasecmp(tag, "weapons/rocklx"))
    {
      int idx = resolve_sfx(lump, orig_num_sfx, &cursor,
                            mobjinfo[MT_ROCKET].deathsound);
      if (idx > 0)
      {
        mobjinfo[MT_ROCKET].deathsound = idx;
        applied++;
      }
      continue;
    }

    {
      size_t plen = (size_t)(slash - tag);
      const char *suffix = slash + 1;
      unsigned k;
      int mt = -1, field = -1;

      if (!strcasecmp(suffix, "shotx"))
      {
        for (k = 0; k < sizeof(shotx_map) / sizeof(shotx_map[0]); k++)
          if (strlen(shotx_map[k].prefix) == plen &&
              !strncasecmp(tag, shotx_map[k].prefix, plen))
          {
            mt = shotx_map[k].mt;
            field = ZF_DEATH;
            break;
          }
      }
      else
      {
        for (k = 0; k < sizeof(prefix_map) / sizeof(prefix_map[0]); k++)
          if (strlen(prefix_map[k].prefix) == plen &&
              !strncasecmp(tag, prefix_map[k].prefix, plen))
          {
            mt = prefix_map[k].mt;
            break;
          }
        for (k = 0; k < sizeof(suffix_map) / sizeof(suffix_map[0]); k++)
          if (!strcasecmp(suffix, suffix_map[k].suffix))
          {
            field = suffix_map[k].field;
            break;
          }
      }

      if (mt >= 0 && field >= 0)
      {
        int *slot = field_of(&mobjinfo[mt], field);
        int idx = resolve_sfx(lump, orig_num_sfx, &cursor, *slot);
        if (idx > 0)
        {
          *slot = idx;
          applied++;
        }
      }
    }
  }

  W_UnlockLumpNum(lumpnum);

  /* the doubling growth can leave capacity-tail slots past the cursor;
   * their NULL names would reach the precache's "ds%s" format.  Give
   * every uncreated slot an empty name. */
  {
    int i;
    for (i = 0; i < num_sfx; i++)
      if (!S_sfx[i].name)
        S_sfx[i].name = "";
  }

  if (applied)
    lprintf(LO_INFO, "U_ZDoomLoadSndInfo: %d monster sound bindings\n",
            applied);
}
