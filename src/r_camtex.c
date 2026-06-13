/* r_camtex.c: ZDoom camera-to-texture render targets.  See r_camtex.h. */

#include <string.h>
#include <stdlib.h>

#include "doomstat.h"
#include "doomtype.h"
#include "m_fixed.h"
#include "tables.h"
#include "r_defs.h"
#include "r_state.h"
#include "r_main.h"
#include "r_data.h"
#include "r_patch.h"
#include "v_video.h"
#include "w_wad.h"
#include "p_mobj.h"
#include "lprintf.h"
#include "z_zone.h"
#include "r_camtex.h"

/* a declared camera texture and its current ACS binding */
typedef struct
{
  char    name[9];
  int     cam_w, cam_h;     /* camera render resolution */
  int     texnum;           /* resolved texture number, -1 until bound */
  int     tid;              /* bound camera actor tid, 0 = unbound */
  int     fov;              /* horizontal field of view in degrees */
} camtex_t;

#define MAX_CAMTEX 32
static camtex_t camtex[MAX_CAMTEX];
static int      num_camtex;
static int      num_bound;      /* how many have a live tid binding */

/* Inverse palette: maps a quantised RGB565 colour back to the nearest 8-bit
 * palette index, so a rendered (RGB565) camera frame can be written into the
 * 8-bit texture.  Built once from the current palette; rebuilt when the
 * palette pointer changes (gamma/video-mode swap). */
static const uint16_t *invpal_pal;          /* V_Palette16 it was built from */
static uint8_t         invpal[32768];       /* index by (r5<<10)|(g5<<5)|b5 */

extern uint16_t *V_Palette16;

static void camtex_build_invpal(void)
{
  int i, r, g, b;
  /* Gather each palette entry's full-bright RGB565 (weight 63 in V_Palette16).
   * Then for every 15-bit RGB555 cell, pick the nearest palette colour.  A
   * coarse 15-bit grid keeps the table at 32 KiB and the per-pixel lookup a
   * single indexed read. */
  uint8_t r5[256], g5[256], b5[256];
  for (i = 0; i < 256; i++)
  {
    uint16_t c = V_Palette16[i * VID_NUMCOLORWEIGHTS + (VID_NUMCOLORWEIGHTS - 1)];
    r5[i] = (uint8_t)((c >> 11) & 0x1f);
    g5[i] = (uint8_t)((c >> 6)  & 0x1f);   /* drop g's low bit to 5-bit */
    b5[i] = (uint8_t)( c        & 0x1f);
  }
  for (r = 0; r < 32; r++)
    for (g = 0; g < 32; g++)
      for (b = 0; b < 32; b++)
      {
        int best = 0, bestd = 0x7fffffff, k;
        for (k = 0; k < 256; k++)
        {
          int dr = r - r5[k], dg = g - g5[k], db = b - b5[k];
          int d  = dr * dr + dg * dg + db * db;
          if (d < bestd) { bestd = d; best = k; if (!d) break; }
        }
        invpal[(r << 10) | (g << 5) | b] = (uint8_t)best;
      }
  invpal_pal = V_Palette16;
}

void R_CamTexDeclare(const char *name, int cam_w, int cam_h)
{
  int i;
  if (!name || !name[0] || num_camtex >= MAX_CAMTEX)
    return;
  for (i = 0; i < num_camtex; i++)
    if (!strncasecmp(camtex[i].name, name, 8))
      return;                       /* already declared */
  memset(&camtex[num_camtex], 0, sizeof(camtex[num_camtex]));
  strncpy(camtex[num_camtex].name, name, 8);
  camtex[num_camtex].name[8] = 0;
  camtex[num_camtex].cam_w = (cam_w >= 1 && cam_w <= 1024) ? cam_w : 128;
  camtex[num_camtex].cam_h = (cam_h >= 1 && cam_h <= 1024) ? cam_h : 128;
  camtex[num_camtex].texnum = -1;
  num_camtex++;
}

void R_CamTexClearBindings(void)
{
  int i;
  for (i = 0; i < num_camtex; i++)
  {
    camtex[i].tid    = 0;
    camtex[i].texnum = -1;
  }
  num_bound = 0;
}

dbool R_CamTexActive(void)
{
  return num_bound > 0;
}

dbool R_CamTexBind(int tid, const char *texname, int fov)
{
  int i;
  if (!texname || !texname[0])
    return false;
  for (i = 0; i < num_camtex; i++)
    if (!strncasecmp(camtex[i].name, texname, 8))
    {
      int tn = R_CheckTextureNumForName(texname);
      if (tn < 0)
        return false;
      if (!camtex[i].tid)
        num_bound++;
      camtex[i].texnum = tn;
      camtex[i].tid    = tid;
      camtex[i].fov    = (fov >= 10 && fov <= 170) ? fov : 90;
      return true;
    }
  return false;
}

/* Defined in r_main.c: render the world from the current view globals into the
 * top-left w x h region of the given scratch screen surface, using a square
 * fov-degree projection.  Saves and restores all viewport state. */
void R_RenderViewToScratch(int scrn, int w, int h, int fovdeg);

/* SSE2/NEON when available, scalar otherwise.  Mirrors the guard r_draw.c
 * uses for its RGB565 folds. */
#if defined(__SSE2__) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2) || defined(_M_X64)
#include <emmintrin.h>
#define CAMTEX_SSE2 1
#elif defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(_M_ARM64)
#include <arm_neon.h>
#define CAMTEX_NEON 1
#endif

/* Pack a run of RGB565 pixels to 15-bit (RGB555) inverse-palette indices:
 *   idx = ((c & 0xF800)>>1) | ((c & 0x07C0)>>1) | (c & 0x001F)
 * i.e. keep red(5) and blue(5), drop green's low bit to 5.  The per-pixel
 * work is a handful of mask/shift/or ops with no data dependence between
 * pixels, so eight (SSE2) or eight (NEON) fold per step; the < 8 tail is
 * handled scalar.  The subsequent palette lookup is an inherently scalar
 * gather (no pre-AVX2 vector gather), kept in the caller. */
static void camtex_pack565_to_idx(const uint16_t *src, uint16_t *dst, int n)
{
  int i = 0;
#if defined(CAMTEX_SSE2)
  const __m128i mR = _mm_set1_epi16((short)0xF800);
  const __m128i mG = _mm_set1_epi16((short)0x07C0);
  const __m128i mB = _mm_set1_epi16((short)0x001F);
  for (; i + 8 <= n; i += 8)
  {
    __m128i c  = _mm_loadu_si128((const __m128i *)(src + i));
    __m128i r  = _mm_srli_epi16(_mm_and_si128(c, mR), 1);
    __m128i g  = _mm_srli_epi16(_mm_and_si128(c, mG), 1);
    __m128i b  = _mm_and_si128(c, mB);
    __m128i ix = _mm_or_si128(_mm_or_si128(r, g), b);
    _mm_storeu_si128((__m128i *)(dst + i), ix);
  }
#elif defined(CAMTEX_NEON)
  const uint16x8_t mR = vdupq_n_u16(0xF800);
  const uint16x8_t mG = vdupq_n_u16(0x07C0);
  const uint16x8_t mB = vdupq_n_u16(0x001F);
  for (; i + 8 <= n; i += 8)
  {
    uint16x8_t c  = vld1q_u16(src + i);
    uint16x8_t r  = vshrq_n_u16(vandq_u16(c, mR), 1);
    uint16x8_t g  = vshrq_n_u16(vandq_u16(c, mG), 1);
    uint16x8_t b  = vandq_u16(c, mB);
    uint16x8_t ix = vorrq_u16(vorrq_u16(r, g), b);
    vst1q_u16(dst + i, ix);
  }
#endif
  for (; i < n; i++)
  {
    uint16_t c = src[i];
    dst[i] = (uint16_t)((((c & 0xF800) >> 1) | ((c & 0x07C0) >> 1) | (c & 0x001F)));
  }
}

/* Copy/scale the rendered RGB565 region from a scratch surface into a camera
 * texture's 8-bit composite pixels, mapping each colour to the nearest palette
 * index.  The source image occupies the top-left rw x rh of the scratch
 * surface; the texture stores pixels column-major (columns[x]=pixels+x*ht).
 *
 * The strided source is first downsampled into a contiguous row-major RGB565
 * scratch, which is then transformed to 15-bit indices eight at a time (SSE2/
 * NEON) before the scalar palette gather + column-major store. */
static uint16_t *camtex_row565;     /* tw scratch, grows as needed */
static uint16_t *camtex_idx;        /* tw scratch of packed indices */
static int       camtex_rowcap;

static void camtex_blit_to_texture(int scrn, int rw, int rh, int texnum)
{
  const rpatch_t *tp = R_CacheTextureCompositePatchNum(texnum);
  const uint16_t *src = (const uint16_t *)screens[scrn].data;
  int tw = tp->width, th = tp->height;
  int x, y;
  uint8_t *pix = tp->pixels;     /* column-major: pix[x*th + y] */

  if (invpal_pal != V_Palette16)
    camtex_build_invpal();

  if (camtex_rowcap < tw)
  {
    free(camtex_row565);
    free(camtex_idx);
    camtex_row565 = (uint16_t *)malloc((size_t)tw * sizeof(uint16_t));
    camtex_idx    = (uint16_t *)malloc((size_t)tw * sizeof(uint16_t));
    camtex_rowcap = (camtex_row565 && camtex_idx) ? tw : 0;
  }
  if (!camtex_rowcap)
  {
    R_UnlockTextureCompositePatchNum(texnum);
    return;
  }

  /* Process one texture ROW (constant y, varying x) at a time: gather the
   * downsampled RGB565 into a contiguous row, vector-pack to indices, then
   * scalar-gather the palette index and scatter column-major. */
  for (y = 0; y < th; y++)
  {
    int sy = (th > 0) ? (y * rh) / th : 0;
    const uint16_t *srow = src + sy * SURFACE_SHORT_PITCH;
    for (x = 0; x < tw; x++)
    {
      int sx = (tw > 0) ? (x * rw) / tw : 0;
      camtex_row565[x] = srow[sx];
    }
    camtex_pack565_to_idx(camtex_row565, camtex_idx, tw);
    for (x = 0; x < tw; x++)
      pix[x * th + y] = invpal[camtex_idx[x] & 0x7fff];
  }
  R_UnlockTextureCompositePatchNum(texnum);
}

extern int viewwidth, viewheight;

void R_RenderCameraTextures(void)
{
  int i;
  if (!num_bound)
    return;
  if (invpal_pal != V_Palette16)
    camtex_build_invpal();

  for (i = 0; i < num_camtex; i++)
  {
    mobj_t *cam;
    int     searcher = -1;
    if (!camtex[i].tid || camtex[i].texnum < 0)
      continue;
    cam = P_FindMobjFromTID((short)camtex[i].tid, &searcher);
    if (!cam)
      continue;

    /* point the view at the camera actor and render into scratch surface 1.
     * The render uses the main viewport (full viewwidth x viewheight); the
     * blit downsamples that into the camera texture. */
    viewx = cam->x;
    viewy = cam->y;
    viewz = cam->z;
    viewangle = cam->angle;
    viewsin = finesine[viewangle >> ANGLETOFINESHIFT];
    viewcos = finecosine[viewangle >> ANGLETOFINESHIFT];

    R_RenderViewToScratch(1, viewwidth, viewheight, camtex[i].fov);
    camtex_blit_to_texture(1, viewwidth, viewheight, camtex[i].texnum);
  }
}
