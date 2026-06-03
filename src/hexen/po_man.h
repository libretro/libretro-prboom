/* Emacs style mode select   -*- C -*-
 *-----------------------------------------------------------------------------
 *
 * Hexen polyobjects (po_man), after Raven Software's Hexen source and the
 * dsda-doom port of it.  Polyobjects are groups of segs spawned away from
 * their in-game position (at an anchor point) and translated to a start
 * spot at load time; at runtime the door / mover thinkers translate and
 * rotate the seg vertices, with a parallel blockmap for collision.
 *
 *-----------------------------------------------------------------------------*/

#ifndef __HEXEN_PO_MAN__
#define __HEXEN_PO_MAN__

#include "../r_defs.h"
#include "../p_mobj.h"

typedef enum
{
    PODOOR_NONE,
    PODOOR_SLIDE,
    PODOOR_SWING
} podoortype_t;

typedef struct
{
    thinker_t thinker;
    int polyobj;
    int speed;
    unsigned int dist;
    int angle;
    fixed_t xSpeed;             /* for sliding walls */
    fixed_t ySpeed;
} polyevent_t;

typedef struct
{
    thinker_t thinker;
    int polyobj;
    int speed;
    int dist;
    int totalDist;
    int direction;
    fixed_t xSpeed, ySpeed;
    int tics;
    int waitTics;
    podoortype_t type;
    dbool close;
} polydoor_t;

/* Hexen polyobject map things. */
#define PO_ANCHOR_TYPE      3000
#define PO_SPAWN_TYPE       3001
#define PO_SPAWNCRUSH_TYPE  3002

void T_PolyDoor(polydoor_t *pd);
void T_RotatePoly(polyevent_t *pe);
void T_MovePoly(polyevent_t *pe);
dbool EV_RotatePoly(line_t *line, byte *args, int direction, dbool overRide);
dbool EV_MovePoly(line_t *line, byte *args, dbool timesEight, dbool overRide);
dbool EV_OpenPolyDoor(line_t *line, byte *args, podoortype_t type);

dbool PO_MovePolyobj(int num, int x, int y);
dbool PO_RotatePolyobj(int num, angle_t angle);
dbool PO_Detect(int doomednum);
void PO_Init(int lump);
dbool PO_Busy(int polyobj);
void PO_ResetBlockMap(dbool allocate);

extern polyblock_t **PolyBlockMap;

#endif
