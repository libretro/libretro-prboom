/* udmf.h -- MSVC C89 port of DSDA-Doom's UDMF parser.
 *
 * Original (C) 2022 Ryan Krafnick, GPLv2.  Ported from C++ to C89 for the
 * prboom/libretro core: no std::vector (manual grow-arrays), no //, all
 * declarations at block tops, dbool/TRUE/FALSE.  Struct layout is kept
 * identical to DSDA's so the P_LoadUDMF* consumers can be ported verbatim.
 */

#ifndef __DSDA_UDMF__
#define __DSDA_UDMF__

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#ifdef PRBOOM_UDMF_STANDALONE
/* standalone parser test: skip the build-generated config.h */
#else
#include "config.h" /* int types via the core's portable headers */
#endif

/* If the core's int types aren't available standalone, fall back. */
#ifndef PRBOOM_UDMF_HAVE_INTTYPES
#include <limits.h>
typedef unsigned short udmf_u16_t;
typedef unsigned int   udmf_u32_t;
typedef unsigned long  udmf_u64a_t; /* >= 32 bits; flags use up to 45 bits */
#endif

/* Line flags need >32 bits (up to bit 44), so use a 64-bit-capable type.
 * MSVC C89 lacks <stdint.h>; use the widest portable unsigned available.
 * On LP64/Win64 'unsigned long' may be 32-bit (Windows), so prefer a
 * compiler 64-bit type when known. */
#if defined(_MSC_VER)
typedef unsigned __int64 udmf_line_flags_t;
#elif defined(__GNUC__)
typedef unsigned long long udmf_line_flags_t;
#else
typedef unsigned long udmf_line_flags_t; /* must be >= 64-bit on this target */
#endif

typedef unsigned short udmf_side_flags_t;
typedef unsigned short udmf_sector_flags_t;
typedef unsigned int   udmf_thing_flags_t;

typedef enum {
  UDMF_NONE,
  UDMF_DOOM,
  UDMF_HERETIC,
  UDMF_HEXEN,
  UDMF_DSDA
} udmf_namespace_t;

extern udmf_namespace_t udmf_namespace;

/* ---- line flags ---- */
#define UDMF_ML_BLOCKING           0x0000000000000001UL
#define UDMF_ML_BLOCKMONSTERS      0x0000000000000002UL
#define UDMF_ML_TWOSIDED           0x0000000000000004UL
#define UDMF_ML_DONTPEGTOP         0x0000000000000008UL
#define UDMF_ML_DONTPEGBOTTOM      0x0000000000000010UL
#define UDMF_ML_SECRET             0x0000000000000020UL
#define UDMF_ML_SOUNDBLOCK         0x0000000000000040UL
#define UDMF_ML_DONTDRAW           0x0000000000000080UL
#define UDMF_ML_MAPPED             0x0000000000000100UL
#define UDMF_ML_PASSUSE            0x0000000000000200UL
#define UDMF_ML_TRANSLUCENT        0x0000000000000400UL
#define UDMF_ML_JUMPOVER           0x0000000000000800UL
#define UDMF_ML_BLOCKFLOATERS      0x0000000000001000UL
#define UDMF_ML_PLAYERCROSS        0x0000000000002000UL
#define UDMF_ML_PLAYERUSE          0x0000000000004000UL
#define UDMF_ML_MONSTERCROSS       0x0000000000008000UL
#define UDMF_ML_MONSTERUSE         0x0000000000010000UL
#define UDMF_ML_IMPACT             0x0000000000020000UL
#define UDMF_ML_PLAYERPUSH         0x0000000000040000UL
#define UDMF_ML_MONSTERPUSH        0x0000000000080000UL
#define UDMF_ML_MISSILECROSS       0x0000000000100000UL
#define UDMF_ML_REPEATSPECIAL      0x0000000000200000UL
#define UDMF_ML_PLAYERUSEBACK      0x0000000000400000UL
#define UDMF_ML_ANYCROSS           0x0000000000800000UL
#define UDMF_ML_MONSTERACTIVATE    0x0000000001000000UL
#define UDMF_ML_BLOCKPLAYERS       0x0000000002000000UL
#define UDMF_ML_BLOCKEVERYTHING    0x0000000004000000UL
#define UDMF_ML_FIRSTSIDEONLY      0x0000000008000000UL
#define UDMF_ML_ZONEBOUNDARY       0x0000000010000000UL
#define UDMF_ML_CLIPMIDTEX         0x0000000020000000UL
#define UDMF_ML_WRAPMIDTEX         0x0000000040000000UL
#define UDMF_ML_MIDTEX3D           0x0000000080000000UL
#define UDMF_ML_MIDTEX3DIMPASSIBLE 0x0000000100000000UL
#define UDMF_ML_CHECKSWITCHRANGE   0x0000000200000000UL
#define UDMF_ML_BLOCKPROJECTILES   0x0000000400000000UL
#define UDMF_ML_BLOCKUSE           0x0000000800000000UL
#define UDMF_ML_BLOCKSIGHT         0x0000001000000000UL
#define UDMF_ML_BLOCKHITSCAN       0x0000002000000000UL
#define UDMF_ML_TRANSPARENT        0x0000004000000000UL
#define UDMF_ML_REVEALED           0x0000008000000000UL
#define UDMF_ML_NOSKYWALLS         0x0000010000000000UL
#define UDMF_ML_DRAWFULLHEIGHT     0x0000020000000000UL
#define UDMF_ML_DAMAGESPECIAL      0x0000040000000000UL
#define UDMF_ML_DEATHSPECIAL       0x0000080000000000UL
#define UDMF_ML_BLOCKLANDMONSTERS  0x0000100000000000UL

typedef struct {
  int id;
  char* moreids;
  int v1;
  int v2;
  int special;
  int arg0;
  int arg1;
  int arg2;
  int arg3;
  int arg4;
  char* arg0str;
  int sidefront;
  int sideback;
  float alpha;
  int locknumber;
  int automapstyle;
  int health;
  int healthgroup;
  udmf_line_flags_t flags;
} udmf_line_t;

/* ---- side flags ---- */
#define UDMF_SF_LIGHTABSOLUTE       0x0001
#define UDMF_SF_LIGHTFOG            0x0002
#define UDMF_SF_NOFAKECONTRAST      0x0004
#define UDMF_SF_SMOOTHLIGHTING      0x0008
#define UDMF_SF_CLIPMIDTEX          0x0010
#define UDMF_SF_WRAPMIDTEX          0x0020
#define UDMF_SF_NODECALS            0x0040
#define UDMF_SF_LIGHTABSOLUTETOP    0x0080
#define UDMF_SF_LIGHTABSOLUTEMID    0x0100
#define UDMF_SF_LIGHTABSOLUTEBOTTOM 0x0200

typedef struct {
  int offsetx;
  int offsety;
  char texturetop[9];
  char texturebottom[9];
  char texturemiddle[9];
  int sector;
  float scalex_top;
  float scaley_top;
  float scalex_mid;
  float scaley_mid;
  float scalex_bottom;
  float scaley_bottom;
  float offsetx_top;
  float offsety_top;
  float offsetx_mid;
  float offsety_mid;
  float offsetx_bottom;
  float offsety_bottom;
  int light;
  int light_top;
  int light_mid;
  int light_bottom;
  float xscroll;
  float yscroll;
  float xscrolltop;
  float yscrolltop;
  float xscrollmid;
  float yscrollmid;
  float xscrollbottom;
  float yscrollbottom;
  udmf_side_flags_t flags;
} udmf_side_t;

typedef struct {
  const char* x;
  const char* y;
} udmf_vertex_t;

/* ---- sector flags ---- */
#define UDMF_SECF_LIGHTFLOORABSOLUTE   0x0001
#define UDMF_SECF_LIGHTCEILINGABSOLUTE 0x0002
#define UDMF_SECF_SILENT               0x0004
#define UDMF_SECF_NOFALLINGDAMAGE      0x0008
#define UDMF_SECF_DROPACTORS           0x0010
#define UDMF_SECF_NORESPAWN            0x0020
#define UDMF_SECF_HIDDEN               0x0040
#define UDMF_SECF_WATERZONE            0x0080
#define UDMF_SECF_DAMAGETERRAINEFFECT  0x0100
#define UDMF_SECF_DAMAGEHAZARD         0x0200
#define UDMF_SECF_NOATTACK             0x0400

#define UDMF_SCROLL_TEXTURE 0x01
#define UDMF_SCROLL_STATIC  0x02
#define UDMF_SCROLL_PLAYER  0x04
#define UDMF_SCROLL_MONSTER 0x08

#define UDMF_THRUST_STATIC     0x01
#define UDMF_THRUST_PLAYER     0x02
#define UDMF_THRUST_MONSTER    0x04
#define UDMF_THRUST_PROJECTILE 0x08
#define UDMF_THRUST_GROUNDED   0x10
#define UDMF_THRUST_AIRBORNE   0x20
#define UDMF_THRUST_CEILING    0x40
#define UDMF_THRUST_WINDTHRUST 0x80

typedef struct {
  int heightfloor;
  int heightceiling;
  char texturefloor[9];
  char textureceiling[9];
  int lightlevel;
  int special;
  int id;
  char* skyfloor;
  char* skyceiling;
  char* colormap;
  char* moreids;
  float xpanningfloor;
  float ypanningfloor;
  float xpanningceiling;
  float ypanningceiling;
  float xscalefloor;
  float yscalefloor;
  float xscaleceiling;
  float yscaleceiling;
  float rotationfloor;
  float rotationceiling;
  int lightfloor;
  int lightceiling;
  const char* gravity;
  int damageamount;
  int damageinterval;
  int leakiness;
  float xscrollfloor;
  float yscrollfloor;
  int scrollfloormode;
  float xscrollceiling;
  float yscrollceiling;
  int scrollceilingmode;
  char* xthrust;
  char* ythrust;
  int thrustgroup;
  int thrustlocation;
  char* frictionfactor;
  char* movefactor;
  udmf_sector_flags_t flags;
} udmf_sector_t;

/* ---- thing flags ---- */
#define UDMF_TF_SKILL1      0x00000001
#define UDMF_TF_SKILL2      0x00000002
#define UDMF_TF_SKILL3      0x00000004
#define UDMF_TF_SKILL4      0x00000008
#define UDMF_TF_SKILL5      0x00000010
#define UDMF_TF_AMBUSH      0x00000020
#define UDMF_TF_SINGLE      0x00000040
#define UDMF_TF_DM          0x00000080
#define UDMF_TF_COOP        0x00000100
#define UDMF_TF_FRIEND      0x00000200
#define UDMF_TF_DORMANT     0x00000400
#define UDMF_TF_CLASS1      0x00000800
#define UDMF_TF_CLASS2      0x00001000
#define UDMF_TF_CLASS3      0x00002000
#define UDMF_TF_STANDING    0x00004000
#define UDMF_TF_STRIFEALLY  0x00008000
#define UDMF_TF_TRANSLUCENT 0x00010000
#define UDMF_TF_INVISIBLE   0x00020000
#define UDMF_TF_COUNTSECRET 0x00040000

typedef struct {
  int id;
  const char* x;
  const char* y;
  const char* height;
  int angle;
  int type;
  int special;
  int arg0;
  int arg1;
  int arg2;
  int arg3;
  int arg4;
  char* arg0str;
  const char* gravity;
  const char* health;
  float scalex;
  float scaley;
  float scale;
  float alpha;
  int floatbobphase;
  udmf_thing_flags_t flags;
} udmf_thing_t;

typedef struct {
  size_t num_lines;
  udmf_line_t* lines;

  size_t num_sides;
  udmf_side_t* sides;

  size_t num_vertices;
  udmf_vertex_t* vertices;

  size_t num_sectors;
  udmf_sector_t* sectors;

  size_t num_things;
  udmf_thing_t* things;
} udmf_t;

extern udmf_t udmf;

/* Error callback: aborts (does not return).  void-returning to match the
 * scanner's error-callback type it is forwarded to. */
typedef void (*udmf_errorfunc)(const char *fmt, ...);

void dsda_ParseUDMF(const unsigned char* buffer, size_t length, udmf_errorfunc err);

#ifdef __cplusplus
}
#endif

#endif
