/* Emacs style mode select   -*- C -*-
 *-----------------------------------------------------------------------------
 *
 *  Hexen flat/texture animation and scrolling line specials.
 *  See p_anim.h.
 *
 *-----------------------------------------------------------------------------
 */

#include <stdlib.h>
#include <string.h>

#include "doomstat.h"
#include "m_random.h"
#include "r_state.h"
#include "r_main.h"
#include "w_wad.h"
#include "u_scanner.h"
#include "lprintf.h"

#include "hexen/p_anim.h"

#define ANIM_SCRIPT_NAME "ANIMDEFS"
#define MAX_ANIM_DEFS    20
#define MAX_FRAME_DEFS   96
#define MAX_LINE_ANIMS   64

#define ANIM_FLAT    0
#define ANIM_TEXTURE 1

typedef struct
{
  int index;
  int tics;
} frameDef_t;

typedef struct
{
  int type;
  int index;
  int tics;
  int currentFrameDef;
  int startFrameDef;
  int endFrameDef;
} animDef_t;

static animDef_t  AnimDefs[MAX_ANIM_DEFS];
static frameDef_t FrameDefs[MAX_FRAME_DEFS];
static int        AnimDefCount;

static short   numlinespecials;
static line_t *linespeciallist[MAX_LINE_ANIMS];

void P_InitFTAnims(void)
{
  u_scanner_t s;
  animDef_t  *ad;
  int         lump;
  int         length;
  const char *data;
  int         fd;
  dbool       more;

  if (!hexen)
    return;

  lump = W_CheckNumForName(ANIM_SCRIPT_NAME);
  if (lump < 0)
    return;

  length = W_LumpLength(lump);
  data   = (const char *) W_CacheLumpNum(lump);

  /* The u_scanner understands C-style comments but not the Hexen scripts'
   * ';' line comments, so strip those into a scratch copy first (the same
   * dance p_mapinfo.c does; ANIMDEFS has no quoted strings to protect). */
  {
    char *scratch = (char *) malloc(length + 1);
    int   in, out = 0;

    for (in = 0; in < length; in++)
    {
      char c = data[in];
      if (c == ';')
      {                         /* drop to end of line */
        while (in < length && data[in] != '\n' && data[in] != '\r')
          in++;
        if (in < length)
          scratch[out++] = data[in];   /* keep the newline */
        continue;
      }
      scratch[out++] = c;
    }
    scratch[out] = '\0';

    s = U_ScanOpen(scratch, out, ANIM_SCRIPT_NAME);
    free(scratch);
  }

  fd = 0;
  ad = AnimDefs;
  AnimDefCount = 0;

  more = U_GetNextToken(&s, TRUE);
  while (more)
  {
    dbool ignore;

    if (AnimDefCount == MAX_ANIM_DEFS)
      I_Error("P_InitFTAnims: too many AnimDefs.");

    if (!strcasecmp(s.string, "flat"))
      ad->type = ANIM_FLAT;
    else if (!strcasecmp(s.string, "texture"))
      ad->type = ANIM_TEXTURE;
    else
      U_Error(&s, "P_InitFTAnims: expected flat or texture, got '%s'",
              s.string);

    U_GetNextToken(&s, TRUE);   /* name */
    ignore = FALSE;
    if (ad->type == ANIM_FLAT)
    {
      if ((W_CheckNumForName)(s.string, ns_flats) == -1)
        ignore = TRUE;
      else
        ad->index = R_FlatNumForName(s.string);
    }
    else
    {
      if (R_CheckTextureNumForName(s.string) == -1)
        ignore = TRUE;
      else
        ad->index = R_TextureNumForName(s.string);
    }
    ad->startFrameDef = fd;

    for (;;)
    {
      more = U_GetNextToken(&s, TRUE);
      if (!more || strcasecmp(s.string, "pic"))
        break;                  /* the token (if any) starts the next def */

      if (fd == MAX_FRAME_DEFS)
        I_Error("P_InitFTAnims: too many FrameDefs.");

      U_MustGetInteger(&s);
      if (!ignore)
        FrameDefs[fd].index = ad->index + s.number - 1;

      U_GetNextToken(&s, TRUE);
      if (!strcasecmp(s.string, "tics"))
      {
        U_MustGetInteger(&s);
        if (!ignore)
        {
          FrameDefs[fd].tics = s.number;
          fd++;
        }
      }
      else if (!strcasecmp(s.string, "rand"))
      {
        int base, mod;

        U_MustGetInteger(&s);
        base = s.number;
        U_MustGetInteger(&s);
        if (!ignore)
        {
          mod = s.number - base + 1;
          FrameDefs[fd].tics = (base << 16) + (mod << 8);
          fd++;
        }
      }
      else
        U_Error(&s, "P_InitFTAnims: expected tics or rand, got '%s'",
                s.string);
    }

    if (!ignore && fd - ad->startFrameDef < 2)
      I_Error("P_InitFTAnims: AnimDef has framecount < 2.");

    if (!ignore)
    {
      ad->endFrameDef = fd - 1;
      ad->currentFrameDef = ad->endFrameDef;
      ad->tics = 1;             /* force the first game tic to animate */
      AnimDefCount++;
      ad++;
    }
  }
  U_ScanClose(&s);
}

void P_SpawnLineSpecials(void)
{
  int i;

  if (!hexen)
    return;

  numlinespecials = 0;
  for (i = 0; i < numlines; i++)
  {
    switch (lines[i].special)
    {
      case 100:                 /* Scroll_Texture_Left  */
      case 101:                 /* Scroll_Texture_Right */
      case 102:                 /* Scroll_Texture_Up    */
      case 103:                 /* Scroll_Texture_Down  */
        if (numlinespecials == MAX_LINE_ANIMS)
        {
          lprintf(LO_WARN,
                  "P_SpawnLineSpecials: too many scrolling lines\n");
          return;
        }
        linespeciallist[numlinespecials++] = &lines[i];
        break;
      default:
        break;
    }
  }
}

void P_AnimateHexenSurfaces(void)
{
  int        i;
  animDef_t *ad;
  line_t    *line;

  if (!hexen)
    return;

  /* animate flats and textures */
  for (i = 0; i < AnimDefCount; i++)
  {
    ad = &AnimDefs[i];
    ad->tics--;
    if (ad->tics == 0)
    {
      if (ad->currentFrameDef == ad->endFrameDef)
        ad->currentFrameDef = ad->startFrameDef;
      else
        ad->currentFrameDef++;
      ad->tics = FrameDefs[ad->currentFrameDef].tics;
      if (ad->tics > 255)
      {                         /* random tics: base<<16 | range<<8 */
        ad->tics = (ad->tics >> 16)
                 + P_Random(pr_heretic) % ((ad->tics & 0xff00) >> 8);
      }
      if (ad->type == ANIM_FLAT)
        flattranslation[ad->index] = FrameDefs[ad->currentFrameDef].index;
      else
        texturetranslation[ad->index] = FrameDefs[ad->currentFrameDef].index;
    }
  }

  /* scroll the walls */
  for (i = 0; i < numlinespecials; i++)
  {
    line = linespeciallist[i];
    switch (line->special)
    {
      case 100:                 /* Scroll_Texture_Left  */
        sides[line->sidenum[0]].textureoffset +=
          line->args[0] << 10;
        break;
      case 101:                 /* Scroll_Texture_Right */
        sides[line->sidenum[0]].textureoffset -=
          line->args[0] << 10;
        break;
      case 102:                 /* Scroll_Texture_Up    */
        sides[line->sidenum[0]].rowoffset +=
          line->args[0] << 10;
        break;
      case 103:                 /* Scroll_Texture_Down  */
        sides[line->sidenum[0]].rowoffset -=
          line->args[0] << 10;
        break;
      default:
        break;
    }
  }
}
