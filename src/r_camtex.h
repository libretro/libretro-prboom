/* r_camtex.h: ZDoom camera-to-texture render targets.
 *
 * A "cameratexture" (declared in ANIMDEFS) is a wall texture whose pixels are
 * produced each frame by rendering the world from an in-world camera actor,
 * the way a security monitor shows a remote view.  ACS binds a camera to such
 * a texture with SetCameraToTexture(tid, "texname", fov).
 *
 * The software renderer composites to a 16-bit RGB565 surface, while wall
 * textures are 8-bit palette indices, so each frame the camera view is
 * rendered into a scratch surface and the result mapped back to palette
 * indices written into the texture's composite columns.  The render reuses
 * the ordinary BSP/wall/plane/sprite pipeline through a saved-and-restored
 * viewport, so the camera image is the real scene, not an approximation.
 */

#ifndef __R_CAMTEX__
#define __R_CAMTEX__

#include "doomtype.h"

/* Register a "cameratexture NAME w h fit cw ch" declaration parsed from
 * ANIMDEFS: the texture named `name` is a camera target rendered at cw x ch.
 * Safe to call before textures are registered; resolution to a texture number
 * is deferred to level setup. */
void R_CamTexDeclare(const char *name, int cam_w, int cam_h);

/* ACS SetCameraToTexture(tid, "texname", fov): bind the camera actor(s) with
 * the given tid to the named camera texture, with a horizontal field of view
 * in degrees.  Returns true if the texture is a known camera target. */
dbool R_CamTexBind(int tid, const char *texname, int fov);

/* Reset all runtime camera bindings (call at level teardown/setup). */
void R_CamTexClearBindings(void);

/* Render every bound camera texture for this frame.  Called once per displayed
 * frame after the main player view is drawn.  A no-op when nothing is bound. */
void R_RenderCameraTextures(void);

/* True when at least one camera texture has a live binding (cheap guard so the
 * caller can skip the per-frame setup entirely when none are in use). */
dbool R_CamTexActive(void);

#endif
