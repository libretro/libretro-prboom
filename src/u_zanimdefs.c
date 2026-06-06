/* u_zanimdefs.c: ZDoom ANIMDEFS texture and flat animations.
 *
 * ZDoom wads define animations in an ANIMDEFS lump as explicit frame
 * lists ("texture NAME" / "flat NAME" followed by "pic NAME tics N"
 * lines).  Unlike Boom's ANIMATED ranges, the frames need not be
 * consecutive in the texture directory -- half of chex3.wad's
 * animations (its 8-frame animated episode-3 sky among them) are
 * non-contiguous -- so they are kept as index lists and rotated through
 * texturetranslation/flattranslation each tic with the same phase
 * convention as the Doom animation loop: a wall using the Nth frame of
 * a sequence cycles through the sequence offset by N.
 *
 * Only the frame-list form is implemented; other ANIMDEFS blocks
 * (switches, warps, cameratextures, ranges) are skipped.  Frames with
 * differing tic counts use the first frame's count, which is uniform in
 * practice. */

#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "doomstat.h"
#include "w_wad.h"
#include "r_data.h"
#include "lprintf.h"
#include "u_scanner.h"
#include "u_zanimdefs.h"

#define MAX_ZANIM_FRAMES 32

typedef struct
{
  int   istexture;
  int   speed;                       /* tics per frame */
  int   numframes;
  int   frames[MAX_ZANIM_FRAMES];    /* texture numbers or flat numbers */
} zanim_t;

static zanim_t *zanims;
static int      num_zanims;

dbool U_ZAnimPresent;

/* Same-line argument fetch, as in u_zmapinfo.c. */
static int za_arg(u_scanner_t *s, unsigned int line)
{
  if (!U_GetNextToken(s, TRUE))
    return -1;
  return s->tokenLine == line ? 1 : 0;
}

static int Z_LookupPic(const char *name, int istexture)
{
  if (istexture)
    return R_CheckTextureNumForName(name);
  else
  {
    int lump = (W_CheckNumForName)(name, ns_flats);
    return lump < 0 ? -1 : lump - firstflat;
  }
}

void U_LoadAnimDefs(void)
{
  int          lump;
  u_scanner_t  s;
  zanim_t      cur;
  int          building = 0;     /* inside a texture/flat frame list */
  int          tok;

  U_ZAnimPresent = false;
  if (hexen || heretic)
    return;
  lump = (W_CheckNumForName)("ANIMDEFS", ns_global);
  if (lump < 0)
    return;

  memset(&cur, 0, sizeof(cur));
  s = U_ScanOpen(W_CacheLumpNum(lump), W_LumpLength(lump), "ANIMDEFS");
  tok = U_GetNextToken(&s, TRUE) ? 1 : -1;

  while (tok > 0)
  {
    unsigned int line = s.tokenLine;

    if (s.token == TK_Identifier)
    {
      int istex = !strcasecmp(s.string, "texture");
      if (istex || !strcasecmp(s.string, "flat"))
      {
        /* flush the previous list */
        if (building && cur.numframes > 1)
        {
          zanims = realloc(zanims, (num_zanims + 1) * sizeof(*zanims));
          zanims[num_zanims++] = cur;
        }
        building = 0;
        memset(&cur, 0, sizeof(cur));
        if ((tok = za_arg(&s, line)) > 0 &&
            Z_LookupPic(s.string, istex) >= 0)
        {
          cur.istexture = istex;
          building = 1;
        }
      }
      else if (building && !strcasecmp(s.string, "pic"))
      {
        if ((tok = za_arg(&s, line)) > 0)
        {
          int pic = Z_LookupPic(s.string, cur.istexture);
          if (pic >= 0 && cur.numframes < MAX_ZANIM_FRAMES)
            cur.frames[cur.numframes++] = pic;
          else
            building = 0;          /* missing frame: drop the sequence */
          /* "tics N" or "rand MIN MAX": first count is the speed */
          while ((tok = za_arg(&s, line)) > 0)
            if (s.token == TK_IntConst && !cur.speed)
              cur.speed = s.number;
        }
      }
      else
        building = 0;              /* switch/warp/range/...: not handled */
    }

    if (tok < 0)
      break;
    if (s.tokenLine != line)
    {
      tok = 1;                  /* token already current; reprocess it */
      continue;
    }
    while ((tok = U_GetNextToken(&s, TRUE) ? 1 : -1) > 0 &&
           s.tokenLine == line)
      ;
  }
  if (building && cur.numframes > 1)
  {
    zanims = realloc(zanims, (num_zanims + 1) * sizeof(*zanims));
    zanims[num_zanims++] = cur;
  }
  U_ScanClose(&s);
  W_UnlockLumpNum(lump);

  if (num_zanims)
  {
    U_ZAnimPresent = true;
    lprintf(LO_INFO, "U_LoadAnimDefs: %d animations\n", num_zanims);
  }
}

/* Called from P_UpdateSpecials after the Doom/Boom animation loop. */
void U_UpdateZAnims(void)
{
  int i, j;

  for (i = 0; i < num_zanims; i++)
  {
    const zanim_t *a = &zanims[i];
    int speed = a->speed > 0 ? a->speed : 8;
    int phase = (leveltime / speed) % a->numframes;

    for (j = 0; j < a->numframes; j++)
    {
      int pic = a->frames[(phase + j) % a->numframes];
      if (a->istexture)
        texturetranslation[a->frames[j]] = pic;
      else
        flattranslation[a->frames[j]] = pic;
    }
  }
}

void U_FreeZAnims(void)
{
  free(zanims);
  zanims = NULL;
  num_zanims = 0;
  U_ZAnimPresent = false;
}
