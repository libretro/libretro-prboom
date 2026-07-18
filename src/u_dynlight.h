/* u_dynlight.h: ZDoom GLDEFS dynamic (point) light definitions for the
 * software renderer.
 *
 * ZDoom wads declare light objects in a *DEFS lump:
 *
 *     pointlight NAME   { color R G B; size RADIUS; }
 *     pulselight NAME   { ... size R; secondarysize R2; interval SECS; }
 *     flickerlight NAME { ... size R; secondarysize R2; chance C; }
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

typedef enum
{
  DL_POINT = 0,       /* static                                         */
  DL_PULSE,           /* radius oscillates size<->size2 over interval   */
  DL_FLICKER          /* radius randomly picks size or size2 per tic    */
} dynlight_kind_t;

typedef struct
{
  char  name[64];
  float r, g, b;      /* 0..1 colour                                    */
  int   size;         /* radius in map units (primary)                  */
  float strength;     /* 0..1 luminous strength (max colour channel)    */
  int   kind;         /* dynlight_kind_t                                */
  int   size2;        /* secondary radius (pulse/flicker)               */
  int   interval;     /* pulse period in tics                           */
  int   chance;       /* flicker: 0..360 threshold (prob of `size`)     */
} dynlight_def_t;

/* Current radius of a (possibly animated) light at game tic `tics`.  `seed`
 * makes flicker independent per light instance; it is ignored for static and
 * pulse lights.  Deterministic and integer-only: pulse follows finesine over
 * leveltime, flicker hashes (tics, seed) -- neither touches the game RNG, so
 * demos and netplay stay in sync. */
int U_DynLightRadius(const dynlight_def_t *d, int tics, unsigned seed);

/* Parse every *DEFS lump's pointlight/pulselight/flickerlight and object
 * blocks.  Safe to call once after W_Init; a second call is a no-op. */
void U_LoadDynLights(void);

/* The light bound to a 4-char sprite name, or NULL. */
const dynlight_def_t *U_DynLightForSprite(const char *sprname);
const dynlight_def_t *U_DynLightForSpriteNum(int spritenum);

/* True if any light binding was parsed (renderer fast-out). */
dbool U_DynLightsPresent(void);

int  U_DynLightCount(void);
void U_FreeDynLights(void);

/* GLDEFS glow: any glow definitions loaded (renderer zero-cost gate). */
extern int u_glow_present;
/* Glow definition for a flat picnum (sector floorpic/ceilingpic), or NULL.
 * Opaque handle; colour as 0xRRGGBB and fade height via the accessors. */
const void *U_GlowForFlat(int flatpic);
int U_GlowColor(const void *gd);
int U_GlowHeight(const void *gd);

#endif
