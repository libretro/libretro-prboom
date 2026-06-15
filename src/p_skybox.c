/* p_skybox.c: per-sector 3D skyboxes (SkyViewpoint 9080 / SkyPicker 9081).
 * See p_skybox.h. */

#include "doomstat.h"
#include "r_state.h"
#include "r_defs.h"
#include "r_main.h"
#include "lprintf.h"
#include "p_skybox.h"

skybox_t *skyboxes    = NULL;
int       numskyboxes = 0;
static int skyboxalloc = 0;

/* pending SkyPicker requests, resolved in P_SpawnSkyboxes */
typedef struct { fixed_t x, y; int tid; } pickerreq_t;
static pickerreq_t *pickers    = NULL;
static int          numpickers = 0;
static int          pickeralloc = 0;

void P_ClearSkyboxes(void)
{
  free(skyboxes);  skyboxes = NULL;  numskyboxes = 0; skyboxalloc = 0;
  free(pickers);   pickers  = NULL;  numpickers  = 0; pickeralloc = 0;
}

void P_AddSkyboxViewpoint(int tid, fixed_t x, fixed_t y, fixed_t z,
                          angle_t angle)
{
  if (numskyboxes == skyboxalloc)
  {
    int na = skyboxalloc ? skyboxalloc * 2 : 8;
    skybox_t *g = (skybox_t *)realloc(skyboxes, (size_t)na * sizeof(*g));
    if (!g) return;
    skyboxes = g; skyboxalloc = na;
  }
  skyboxes[numskyboxes].tid   = tid;
  skyboxes[numskyboxes].x     = x;
  skyboxes[numskyboxes].y     = y;
  skyboxes[numskyboxes].z     = z;
  skyboxes[numskyboxes].angle = angle;
  numskyboxes++;
}

void P_AddSkyboxPicker(fixed_t x, fixed_t y, int targettid)
{
  if (numpickers == pickeralloc)
  {
    int na = pickeralloc ? pickeralloc * 2 : 32;
    pickerreq_t *g = (pickerreq_t *)realloc(pickers, (size_t)na * sizeof(*g));
    if (!g) return;
    pickers = g; pickeralloc = na;
  }
  pickers[numpickers].x   = x;
  pickers[numpickers].y   = y;
  pickers[numpickers].tid = targettid;
  numpickers++;
}

static int skybox_index_for_tid(int tid)
{
  int i;
  for (i = 0; i < numskyboxes; i++)
    if (skyboxes[i].tid == tid)
      return i;
  return -1;
}

void P_SpawnSkyboxes(void)
{
  int i, assigned = 0;

  /* every sector starts on the default sky */
  for (i = 0; i < numsectors; i++)
    sectors[i].skybox = -1;

  if (!numpickers || !numskyboxes)
    return;

  for (i = 0; i < numpickers; i++)
  {
    subsector_t *ss = R_PointInSubsector(pickers[i].x, pickers[i].y);
    int idx;
    if (!ss || !ss->sector)
      continue;
    idx = skybox_index_for_tid(pickers[i].tid);
    if (idx < 0)
      continue;                 /* picker names a tid with no viewpoint */
    ss->sector->skybox = idx;
    assigned++;
  }

  if (assigned)
    lprintf(LO_INFO, "P_SpawnSkyboxes: %d skybox camera(s), %d sector(s) "
            "assigned via SkyPicker\n", numskyboxes, assigned);

}
