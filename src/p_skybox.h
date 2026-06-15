/* p_skybox.h: per-sector 3D skyboxes (ZDoom SkyViewpoint 9080 / SkyPicker
 * 9081).
 *
 * A tagged SkyViewpoint (tid != 0) is a named skybox camera.  A SkyPicker
 * placed in a sector names one of those tids in arg0; that sector then
 * shows the chosen skybox instead of the level's default (the untagged
 * SkyViewpoint, handled separately by skyview).
 *
 * Both things are recorded at thing-load time and resolved once after all
 * things load, since a picker may be processed before the viewpoint it
 * names.  The result is sector->skybox: an index into the skybox camera
 * table, or -1 for "use the default sky/skyview".
 */

#ifndef P_SKYBOX_H
#define P_SKYBOX_H

#include "m_fixed.h"

/* one resolved skybox camera */
typedef struct {
  int     tid;
  fixed_t x, y, z;
  angle_t angle;
} skybox_t;

extern skybox_t *skyboxes;
extern int       numskyboxes;

/* record a tagged SkyViewpoint (tid != 0) at thing-load time */
void P_AddSkyboxViewpoint(int tid, fixed_t x, fixed_t y, fixed_t z,
                          angle_t angle);

/* record a SkyPicker request: sector at (x,y) wants the skybox named by
 * targettid */
void P_AddSkyboxPicker(fixed_t x, fixed_t y, int targettid);

/* drop the previous level's skybox tables */
void P_ClearSkyboxes(void);

/* resolve pending pickers against the recorded viewpoints, filling
 * sector->skybox */
void P_SpawnSkyboxes(void);

#endif
