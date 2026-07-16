/* u_dynlight.h: ZDoom GLDEFS dynamic (point) light definitions for the
 * software renderer.
 *
 * ZDoom wads declare light objects in a *DEFS lump:
 *
 *     pointlight NAME   { color R G B; size RADIUS; }
 *     pulselight NAME   { ... }        // parsed as a static light for now
 *     flickerlight NAME { ... }        // ditto
 *
 * and bind them to an actor's sprite frames:
 *
 *     object SomeActor { frame SPR { light NAME } ... }
 *
 * ZDoom's dynamic lights are a hardware-renderer feature; this module gives
 * the software renderer a monochrome approximation.  It parses the light
 * definitions and the frame bindings, keyed by the 4-char sprite name (the
 * actor class scoping is ignored -- any thing displaying a bound sprite
 * emits the light), which the renderer resolves through mo->sprite.  Colour
 * is parsed but the current pass only uses the light's luminous strength to
 * brighten nearby surfaces; the palette renderer cannot tint. */

#ifndef U_DYNLIGHT_H
#define U_DYNLIGHT_H

#include "doomtype.h"

typedef struct
{
  char  name[64];
  float r, g, b;      /* 0..1 colour                                    */
  int   size;         /* radius in map units                            */
  float strength;     /* 0..1 luminous strength (max colour channel)    */
} dynlight_def_t;

/* Parse every *DEFS lump's pointlight/pulselight/flickerlight and object
 * blocks.  Safe to call once after W_Init; a second call is a no-op. */
void U_LoadDynLights(void);

/* The light bound to a 4-char sprite name, or NULL. */
const dynlight_def_t *U_DynLightForSprite(const char *sprname);

/* True if any light binding was parsed (renderer fast-out). */
dbool U_DynLightsPresent(void);

int  U_DynLightCount(void);
void U_FreeDynLights(void);

#endif
