/* Emacs style mode select   -*- C -*-
 *-----------------------------------------------------------------------------
 *
 * Hexen polyobjects, after Raven Software's Hexen source and the dsda-doom
 * port of it.  This file carries the load-time spawning (anchor / start
 * spot translation), the parallel collision blockmap, and the movement
 * primitives (PO_MovePolyobj / PO_RotatePolyobj) that rewrite the seg
 * vertices.  The mover thinkers and the Polyobj_* line specials sit on top
 * of these primitives.
 *
 *-----------------------------------------------------------------------------*/

#include <string.h>

#include "doomdef.h"
#include "doomstat.h"
#include "p_mobj.h"
#include "r_defs.h"
#include "r_main.h"
#include "lprintf.h"
#include "w_wad.h"
#include "p_setup.h"
#include "m_bbox.h"
#include "m_swap.h"
#include "p_tick.h"
#include "p_inter.h"
#include "p_map.h"
#include "p_maputl.h"
#include "z_zone.h"
#include "i_system.h"
#include "doomdata.h"
#include "po_man.h"

#define PO_MAXPOLYSEGS 64

polyobj_t *polyobjs;     /* list of all poly-objects on the level */
int po_NumPolyobjs;
polyblock_t **PolyBlockMap;

static int PolySegCount;
static fixed_t PolyStartX;
static fixed_t PolyStartY;

static polyobj_t *GetPolyobj(int polyNum)
{
  int i;

  for (i = 0; i < po_NumPolyobjs; i++)
  {
    if (polyobjs[i].tag == polyNum)
    {
      return &polyobjs[i];
    }
  }
  return NULL;
}

/* Re-attach a polyobject to the subsector containing its centre after a
 * move or rotation, so the renderer draws it in the right place. */
void ResetPolySubSector(polyobj_t *po)
{
  int i;
  vertex_t avg;
  seg_t **polySeg;
  subsector_t *new_sub;

  avg.x = 0;
  avg.y = 0;
  polySeg = po->segs;

  for (i = 0; i < po->numsegs; i++, polySeg++)
  {
    avg.x += (*polySeg)->v1->x >> FRACBITS;
    avg.y += (*polySeg)->v1->y >> FRACBITS;
  }

  avg.x /= po->numsegs;
  avg.y /= po->numsegs;

  new_sub = R_PointInSubsector(avg.x << FRACBITS, avg.y << FRACBITS);

  if (new_sub->poly)
  {
    return; /* colliding poly objects? */
  }

  po->subsector->poly = NULL;
  po->subsector = new_sub;
  po->subsector->poly = po;
}

/* Push a thing standing against a moving polyobject seg.  The force scales
 * with the mover's speed once the mover thinkers exist; for now a static
 * unit push, matching the no-mover case of the original. */
static void ThrustMobj(mobj_t *mobj, seg_t *seg, polyobj_t *po)
{
  int thrustAngle;
  int thrustX;
  int thrustY;

  if (!(mobj->flags & MF_SHOOTABLE) && !mobj->player)
  {
    return;
  }
  thrustAngle = (seg->angle - ANG90) >> ANGLETOFINESHIFT;

  thrustX = FixedMul(FRACUNIT, finecosine[thrustAngle]);
  thrustY = FixedMul(FRACUNIT, finesine[thrustAngle]);
  mobj->momx += thrustX;
  mobj->momy += thrustY;
  if (po->crush)
  {
    if (po->hurt || !P_CheckPosition(mobj, mobj->x + thrustX, mobj->y + thrustY))
    {
      P_DamageMobj(mobj, NULL, NULL, 3);
    }
  }
}

static void UpdateSegBBox(seg_t *seg, polyobj_t *po)
{
  line_t *line;

  (void)po;
  line = seg->linedef;

  if (seg->v1->x < seg->v2->x)
  {
    line->bbox[BOXLEFT] = seg->v1->x;
    line->bbox[BOXRIGHT] = seg->v2->x;
  }
  else
  {
    line->bbox[BOXLEFT] = seg->v2->x;
    line->bbox[BOXRIGHT] = seg->v1->x;
  }
  if (seg->v1->y < seg->v2->y)
  {
    line->bbox[BOXBOTTOM] = seg->v1->y;
    line->bbox[BOXTOP] = seg->v2->y;
  }
  else
  {
    line->bbox[BOXBOTTOM] = seg->v2->y;
    line->bbox[BOXTOP] = seg->v1->y;
  }

  /* Update the line's slopetype. */
  line->dx = line->v2->x - line->v1->x;
  line->dy = line->v2->y - line->v1->y;
  if (!line->dx)
  {
    line->slopetype = ST_VERTICAL;
  }
  else if (!line->dy)
  {
    line->slopetype = ST_HORIZONTAL;
  }
  else
  {
    if (FixedDiv(line->dy, line->dx) > 0)
    {
      line->slopetype = ST_POSITIVE;
    }
    else
    {
      line->slopetype = ST_NEGATIVE;
    }
  }
}

void UnLinkPolyobj(polyobj_t *po)
{
  polyblock_t *link;
  int i, j;
  int index;

  /* remove the polyobj from each blockmap section */
  for (j = po->bbox[BOXBOTTOM]; j <= po->bbox[BOXTOP]; j++)
  {
    index = j * bmapwidth;
    for (i = po->bbox[BOXLEFT]; i <= po->bbox[BOXRIGHT]; i++)
    {
      if (i >= 0 && i < bmapwidth && j >= 0 && j < bmapheight)
      {
        link = PolyBlockMap[index + i];
        while (link != NULL && link->polyobj != po)
        {
          link = link->next;
        }
        if (link == NULL)
        { /* polyobj not located in the link cell */
          continue;
        }
        link->polyobj = NULL;
      }
    }
  }
}

void LinkPolyobj(polyobj_t *po)
{
  int leftX, rightX;
  int topY, bottomY;
  seg_t **tempSeg;
  polyblock_t **link;
  polyblock_t *tempLink;
  int i, j;

  /* calculate the polyobj bbox */
  tempSeg = po->segs;
  rightX = leftX = (*tempSeg)->v1->x;
  topY = bottomY = (*tempSeg)->v1->y;

  for (i = 0; i < po->numsegs; i++, tempSeg++)
  {
    if ((*tempSeg)->v1->x > rightX)
    {
      rightX = (*tempSeg)->v1->x;
    }
    if ((*tempSeg)->v1->x < leftX)
    {
      leftX = (*tempSeg)->v1->x;
    }
    if ((*tempSeg)->v1->y > topY)
    {
      topY = (*tempSeg)->v1->y;
    }
    if ((*tempSeg)->v1->y < bottomY)
    {
      bottomY = (*tempSeg)->v1->y;
    }
  }
  po->bbox[BOXRIGHT] = (rightX - bmaporgx) >> MAPBLOCKSHIFT;
  po->bbox[BOXLEFT] = (leftX - bmaporgx) >> MAPBLOCKSHIFT;
  po->bbox[BOXTOP] = (topY - bmaporgy) >> MAPBLOCKSHIFT;
  po->bbox[BOXBOTTOM] = (bottomY - bmaporgy) >> MAPBLOCKSHIFT;
  /* add the polyobj to each blockmap section */
  for (j = po->bbox[BOXBOTTOM] * bmapwidth;
       j <= po->bbox[BOXTOP] * bmapwidth; j += bmapwidth)
  {
    for (i = po->bbox[BOXLEFT]; i <= po->bbox[BOXRIGHT]; i++)
    {
      if (i >= 0 && i < bmapwidth && j >= 0
          && j < bmapheight * bmapwidth)
      {
        link = &PolyBlockMap[j + i];
        if (!(*link))
        { /* create a new link at the current block cell */
          *link = Z_Malloc(sizeof(polyblock_t), PU_LEVEL, 0);
          (*link)->next = NULL;
          (*link)->prev = NULL;
          (*link)->polyobj = po;
          continue;
        }
        else
        {
          tempLink = *link;
          while (tempLink->next != NULL && tempLink->polyobj != NULL)
          {
            tempLink = tempLink->next;
          }
        }
        if (tempLink->polyobj == NULL)
        {
          tempLink->polyobj = po;
          continue;
        }
        else
        {
          tempLink->next = Z_Malloc(sizeof(polyblock_t), PU_LEVEL, 0);
          tempLink->next->next = NULL;
          tempLink->next->prev = tempLink;
          tempLink->next->polyobj = po;
        }
      }
      /* else, don't link the polyobj, since it's off the map */
    }
  }
}

static dbool CheckMobjBlocking(seg_t *seg, polyobj_t *po)
{
  mobj_t *mobj;
  int i, j;
  int left, right, top, bottom;
  fixed_t tmbbox[4];
  line_t *ld;
  dbool blocked;

  ld = seg->linedef;

  top = (ld->bbox[BOXTOP] - bmaporgy + MAXRADIUS) >> MAPBLOCKSHIFT;
  bottom = (ld->bbox[BOXBOTTOM] - bmaporgy - MAXRADIUS) >> MAPBLOCKSHIFT;
  left = (ld->bbox[BOXLEFT] - bmaporgx - MAXRADIUS) >> MAPBLOCKSHIFT;
  right = (ld->bbox[BOXRIGHT] - bmaporgx + MAXRADIUS) >> MAPBLOCKSHIFT;

  blocked = false;

  bottom = bottom < 0 ? 0 : bottom;
  bottom = bottom >= bmapheight ? bmapheight - 1 : bottom;
  top = top < 0 ? 0 : top;
  top = top >= bmapheight ? bmapheight - 1 : top;
  left = left < 0 ? 0 : left;
  left = left >= bmapwidth ? bmapwidth - 1 : left;
  right = right < 0 ? 0 : right;
  right = right >= bmapwidth ? bmapwidth - 1 : right;

  for (j = bottom * bmapwidth; j <= top * bmapwidth; j += bmapwidth)
  {
    for (i = left; i <= right; i++)
    {
      for (mobj = blocklinks[j + i]; mobj; mobj = mobj->bnext)
      {
        if (mobj->flags & MF_SOLID || mobj->player)
        {
          tmbbox[BOXTOP] = mobj->y + mobj->radius;
          tmbbox[BOXBOTTOM] = mobj->y - mobj->radius;
          tmbbox[BOXLEFT] = mobj->x - mobj->radius;
          tmbbox[BOXRIGHT] = mobj->x + mobj->radius;

          if (tmbbox[BOXRIGHT] <= ld->bbox[BOXLEFT]
              || tmbbox[BOXLEFT] >= ld->bbox[BOXRIGHT]
              || tmbbox[BOXTOP] <= ld->bbox[BOXBOTTOM]
              || tmbbox[BOXBOTTOM] >= ld->bbox[BOXTOP])
          {
            continue;
          }
          if (P_BoxOnLineSide(tmbbox, ld) != -1)
          {
            continue;
          }
          ThrustMobj(mobj, seg, po);
          blocked = true;
        }
      }
    }
  }
  return blocked;
}

dbool PO_MovePolyobj(int num, int x, int y)
{
  int count;
  seg_t **segList;
  seg_t **veryTempSeg;
  polyobj_t *po;
  vertex_t *prevPts;
  dbool blocked;

  if (!(po = GetPolyobj(num)))
  {
    I_Error("PO_MovePolyobj:  Invalid polyobj number: %d\n", num);
  }

  UnLinkPolyobj(po);

  segList = po->segs;
  prevPts = po->prevPts;
  blocked = false;

  validcount++;
  for (count = po->numsegs; count; count--, segList++, prevPts++)
  {
    if ((*segList)->linedef->validcount != validcount)
    {
      (*segList)->linedef->bbox[BOXTOP] += y;
      (*segList)->linedef->bbox[BOXBOTTOM] += y;
      (*segList)->linedef->bbox[BOXLEFT] += x;
      (*segList)->linedef->bbox[BOXRIGHT] += x;
      (*segList)->linedef->validcount = validcount;
    }
    for (veryTempSeg = po->segs; veryTempSeg != segList; veryTempSeg++)
    {
      if ((*veryTempSeg)->v1 == (*segList)->v1)
      {
        break;
      }
    }
    if (veryTempSeg == segList)
    {
      (*segList)->v1->x += x;
      (*segList)->v1->y += y;
    }
    (*prevPts).x += x; /* previous points are unique for each seg */
    (*prevPts).y += y;
  }
  segList = po->segs;
  for (count = po->numsegs; count; count--, segList++)
  {
    if (CheckMobjBlocking(*segList, po))
    {
      blocked = true;
    }
  }
  if (blocked)
  {
    count = po->numsegs;
    segList = po->segs;
    prevPts = po->prevPts;
    validcount++;
    while (count--)
    {
      if ((*segList)->linedef->validcount != validcount)
      {
        (*segList)->linedef->bbox[BOXTOP] -= y;
        (*segList)->linedef->bbox[BOXBOTTOM] -= y;
        (*segList)->linedef->bbox[BOXLEFT] -= x;
        (*segList)->linedef->bbox[BOXRIGHT] -= x;
        (*segList)->linedef->validcount = validcount;
      }
      for (veryTempSeg = po->segs; veryTempSeg != segList; veryTempSeg++)
      {
        if ((*veryTempSeg)->v1 == (*segList)->v1)
        {
          break;
        }
      }
      if (veryTempSeg == segList)
      {
        (*segList)->v1->x -= x;
        (*segList)->v1->y -= y;
      }
      (*prevPts).x -= x;
      (*prevPts).y -= y;
      segList++;
      prevPts++;
    }
    LinkPolyobj(po);
    return false;
  }
  po->startSpot.x += x;
  po->startSpot.y += y;
  LinkPolyobj(po);
  ResetPolySubSector(po);
  return true;
}

static void RotatePt(int an, fixed_t *x, fixed_t *y, fixed_t startSpotX,
                     fixed_t startSpotY)
{
  fixed_t trx, try_;
  fixed_t gxt, gyt;

  trx = *x;
  try_ = *y;

  gxt = FixedMul(trx, finecosine[an]);
  gyt = FixedMul(try_, finesine[an]);
  *x = (gxt - gyt) + startSpotX;

  gxt = FixedMul(trx, finesine[an]);
  gyt = FixedMul(try_, finecosine[an]);
  *y = (gyt + gxt) + startSpotY;
}

dbool PO_RotatePolyobj(int num, angle_t angle)
{
  int count;
  seg_t **segList;
  vertex_t *originalPts;
  vertex_t *prevPts;
  int an;
  polyobj_t *po;
  dbool blocked;

  if (!(po = GetPolyobj(num)))
  {
    I_Error("PO_RotatePolyobj:  Invalid polyobj number: %d\n", num);
  }
  an = (po->angle + angle) >> ANGLETOFINESHIFT;

  UnLinkPolyobj(po);

  segList = po->segs;
  originalPts = po->originalPts;
  prevPts = po->prevPts;

  for (count = po->numsegs; count; count--, segList++, originalPts++,
       prevPts++)
  {
    prevPts->x = (*segList)->v1->x;
    prevPts->y = (*segList)->v1->y;
    (*segList)->v1->x = originalPts->x;
    (*segList)->v1->y = originalPts->y;
    RotatePt(an, &(*segList)->v1->x, &(*segList)->v1->y, po->startSpot.x,
             po->startSpot.y);
  }
  segList = po->segs;
  blocked = false;
  validcount++;
  for (count = po->numsegs; count; count--, segList++)
  {
    if (CheckMobjBlocking(*segList, po))
    {
      blocked = true;
    }
    if ((*segList)->linedef->validcount != validcount)
    {
      UpdateSegBBox(*segList, po);
      (*segList)->linedef->validcount = validcount;
    }
    (*segList)->angle += angle;
  }
  if (blocked)
  {
    segList = po->segs;
    prevPts = po->prevPts;
    for (count = po->numsegs; count; count--, segList++, prevPts++)
    {
      (*segList)->v1->x = prevPts->x;
      (*segList)->v1->y = prevPts->y;
    }
    segList = po->segs;
    validcount++;
    for (count = po->numsegs; count; count--, segList++, prevPts++)
    {
      if ((*segList)->linedef->validcount != validcount)
      {
        UpdateSegBBox(*segList, po);
        (*segList)->linedef->validcount = validcount;
      }
      (*segList)->angle -= angle;
    }
    LinkPolyobj(po);
    return false;
  }
  po->angle += angle;
  LinkPolyobj(po);
  ResetPolySubSector(po);
  return true;
}

void PO_ResetBlockMap(dbool allocate)
{
  if (allocate)
    PolyBlockMap = Z_Malloc(bmapwidth * bmapheight * sizeof(polyblock_t *),
                            PU_LEVEL, 0);
  memset(PolyBlockMap, 0, bmapwidth * bmapheight * sizeof(polyblock_t *));
}

static void InitBlockMap(void)
{
  int i;

  PO_ResetBlockMap(false);

  for (i = 0; i < po_NumPolyobjs; i++)
  {
    LinkPolyobj(&polyobjs[i]);
  }
}

static void IterFindPolySegs(int x, int y, seg_t **segList)
{
  int i;

  if (x == PolyStartX && y == PolyStartY)
  {
    return;
  }
  for (i = 0; i < numsegs; i++)
  {
    if (segs[i].linedef &&
        segs[i].v1->x == x &&
        segs[i].v1->y == y)
    {
      if (!segList)
      {
        PolySegCount++;
      }
      else
      {
        *segList++ = &segs[i];
      }
      IterFindPolySegs(segs[i].v2->x, segs[i].v2->y, segList);
      return;
    }
  }
  I_Error("IterFindPolySegs:  Non-closed Polyobj located.\n");
}

static void SpawnPolyobj(int index, int tag, dbool crush, dbool hurt)
{
  int i;
  int j;
  int psIndex;
  int psIndexOld;
  seg_t *polySegList[PO_MAXPOLYSEGS];

  for (i = 0; i < numsegs; i++)
  {
    if (segs[i].linedef &&
        segs[i].linedef->special == PO_LINE_START &&
        segs[i].linedef->args[0] == tag)
    {
      if (polyobjs[index].segs)
      {
        I_Error("SpawnPolyobj:  Polyobj %d already spawned.\n", tag);
      }
      segs[i].linedef->special = 0;
      segs[i].linedef->args[0] = 0;
      PolySegCount = 1;
      PolyStartX = segs[i].v1->x;
      PolyStartY = segs[i].v1->y;
      IterFindPolySegs(segs[i].v2->x, segs[i].v2->y, NULL);

      polyobjs[index].numsegs = PolySegCount;
      polyobjs[index].segs = Z_Malloc(PolySegCount * sizeof(seg_t *),
                                      PU_LEVEL, 0);
      *(polyobjs[index].segs) = &segs[i]; /* insert the first seg */
      IterFindPolySegs(segs[i].v2->x, segs[i].v2->y,
                       polyobjs[index].segs + 1);
      polyobjs[index].crush = crush;
      polyobjs[index].hurt = hurt;
      polyobjs[index].tag = tag;
      polyobjs[index].seqType = segs[i].linedef->args[2];
      if (polyobjs[index].seqType < 0
          || polyobjs[index].seqType >= SEQTYPE_NUMSEQ)
      {
        polyobjs[index].seqType = 0;
      }
      break;
    }
  }
  if (!polyobjs[index].segs)
  { /* didn't find a polyobj through PO_LINE_START */
    psIndex = 0;
    polyobjs[index].numsegs = 0;
    for (j = 1; j < PO_MAXPOLYSEGS; j++)
    {
      psIndexOld = psIndex;
      for (i = 0; i < numsegs; i++)
      {
        if (segs[i].linedef &&
            segs[i].linedef->special == PO_LINE_EXPLICIT &&
            segs[i].linedef->args[0] == tag)
        {
          if (!segs[i].linedef->args[1])
          {
            I_Error("SpawnPolyobj:  Explicit line missing order number (probably %d) in poly %d.\n",
                    j + 1, tag);
          }
          if (segs[i].linedef->args[1] == j)
          {
            polySegList[psIndex] = &segs[i];
            polyobjs[index].numsegs++;
            psIndex++;
            if (psIndex > PO_MAXPOLYSEGS)
            {
              I_Error("SpawnPolyobj:  psIndex > PO_MAXPOLYSEGS\n");
            }
          }
        }
      }
      /* Clear out any specials for these segs...we cannot clear them out
       * in the above loop, since we aren't guaranteed one seg per
       * linedef. */
      for (i = 0; i < numsegs; i++)
      {
        if (segs[i].linedef &&
            segs[i].linedef->special == PO_LINE_EXPLICIT &&
            segs[i].linedef->args[0] == tag
            && segs[i].linedef->args[1] == j)
        {
          segs[i].linedef->special = 0;
          segs[i].linedef->args[0] = 0;
        }
      }
      if (psIndex == psIndexOld)
      { /* Check if an explicit line order has been skipped.
         * A line has been skipped if there are any more explicit
         * lines with the current tag value. */
        for (i = 0; i < numsegs; i++)
        {
          if (segs[i].linedef &&
              segs[i].linedef->special == PO_LINE_EXPLICIT &&
              segs[i].linedef->args[0] == tag)
          {
            I_Error("SpawnPolyobj:  Missing explicit line %d for poly %d\n",
                    j, tag);
          }
        }
      }
    }
    if (polyobjs[index].numsegs)
    {
      PolySegCount = polyobjs[index].numsegs; /* PolySegCount used globally */
      polyobjs[index].crush = crush;
      polyobjs[index].hurt = hurt;
      polyobjs[index].tag = tag;
      polyobjs[index].segs = Z_Malloc(polyobjs[index].numsegs * sizeof(seg_t *),
                                      PU_LEVEL, 0);
      for (i = 0; i < polyobjs[index].numsegs; i++)
      {
        polyobjs[index].segs[i] = polySegList[i];
      }
      polyobjs[index].seqType = (*polyobjs[index].segs)->linedef->args[3];
    }

    if (!polyobjs[index].segs)
    {
      I_Error("SpawnPolyobj: Missing start / explicit line for poly %d\n", tag);
    }

    /* Next, change the polyobjs first line to point to a mirror
     * if it exists. */
    (*polyobjs[index].segs)->linedef->args[1] =
        (*polyobjs[index].segs)->linedef->args[2];
  }
}

static void TranslateToStartSpot(int tag, int originX, int originY)
{
  seg_t **tempSeg;
  seg_t **veryTempSeg;
  vertex_t *tempPt;
  subsector_t *sub;
  polyobj_t *po;
  int deltaX;
  int deltaY;
  vertex_t avg; /* used to find a polyobj's center, and hence subsector */
  int i;

  po = NULL;
  for (i = 0; i < po_NumPolyobjs; i++)
  {
    if (polyobjs[i].tag == tag)
    {
      po = &polyobjs[i];
      break;
    }
  }
  if (!po)
  { /* didn't match the tag with a polyobj tag */
    I_Error("TranslateToStartSpot:  Unable to match polyobj tag: %d\n", tag);
  }
  if (po->segs == NULL)
  {
    I_Error("TranslateToStartSpot:  Anchor point located without a StartSpot point: %d\n",
            tag);
  }
  po->originalPts = Z_Malloc(po->numsegs * sizeof(vertex_t), PU_LEVEL, 0);
  po->prevPts = Z_Malloc(po->numsegs * sizeof(vertex_t), PU_LEVEL, 0);
  deltaX = originX - po->startSpot.x;
  deltaY = originY - po->startSpot.y;

  tempSeg = po->segs;
  tempPt = po->originalPts;
  avg.x = 0;
  avg.y = 0;

  validcount++;
  for (i = 0; i < po->numsegs; i++, tempSeg++, tempPt++)
  {
    if ((*tempSeg)->linedef->validcount != validcount)
    {
      (*tempSeg)->linedef->bbox[BOXTOP] -= deltaY;
      (*tempSeg)->linedef->bbox[BOXBOTTOM] -= deltaY;
      (*tempSeg)->linedef->bbox[BOXLEFT] -= deltaX;
      (*tempSeg)->linedef->bbox[BOXRIGHT] -= deltaX;
      (*tempSeg)->linedef->validcount = validcount;
    }
    for (veryTempSeg = po->segs; veryTempSeg != tempSeg; veryTempSeg++)
    {
      if ((*veryTempSeg)->v1 == (*tempSeg)->v1)
      {
        break;
      }
    }
    if (veryTempSeg == tempSeg)
    { /* the point hasn't been translated, yet */
      (*tempSeg)->v1->x -= deltaX;
      (*tempSeg)->v1->y -= deltaY;
    }

    avg.x += (*tempSeg)->v1->x >> FRACBITS;
    avg.y += (*tempSeg)->v1->y >> FRACBITS;
    /* the original Pts are based off the startSpot Pt, and are
     * unique to each seg, not each linedef */
    tempPt->x = (*tempSeg)->v1->x - po->startSpot.x;
    tempPt->y = (*tempSeg)->v1->y - po->startSpot.y;
  }
  avg.x /= po->numsegs;
  avg.y /= po->numsegs;
  sub = R_PointInSubsector(avg.x << FRACBITS, avg.y << FRACBITS);
  if (sub->poly != NULL)
  {
    I_Error("PO_TranslateToStartSpot:  Multiple polyobjs in a single subsector.\n");
  }
  po->subsector = sub;
  sub->poly = po;
}

/* Called from P_SpawnMapThing: polyobject anchor and start-spot things are
 * bookkeeping, not world mobjs.  Counts the polyobjects while at it. */
dbool PO_Detect(int doomednum)
{
  if (!hexen)
    return false;

  if (doomednum == PO_ANCHOR_TYPE)
  {
    return true;
  }

  if (doomednum >= PO_SPAWN_TYPE && doomednum <= PO_SPAWNCRUSH_TYPE)
  {
    po_NumPolyobjs++;
    return true;
  }

  return false;
}

/* Re-reads the raw THINGS lump for the polyobject anchor / start spots
 * (the narrow mapthing_t the regular loader uses cannot carry them all,
 * and the spawn pass needs two ordered sweeps anyway). */
static void PO_LoadThings(int lump)
{
  const byte *data;
  int i;
  int numthings;
  int polyIndex;
  const hexen_mapthing_t *mt;
  short x, y, angle, type;

  data = W_CacheLumpNum(lump);
  numthings = W_LumpLength(lump) / sizeof(hexen_mapthing_t);
  mt = (const hexen_mapthing_t *) data;
  polyIndex = 0; /* index polyobj number */
  /* Find the startSpot points, and spawn each polyobj */
  for (i = 0; i < numthings; i++, mt++)
  {
    x = SHORT(mt->x);
    y = SHORT(mt->y);
    angle = SHORT(mt->angle);
    type = SHORT(mt->type);

    /* 3001 = no crush, 3002 = crushing */
    if (type >= PO_SPAWN_TYPE && type <= PO_SPAWNCRUSH_TYPE)
    { /* Polyobj StartSpot Pt. */
      polyobjs[polyIndex].startSpot.x = x << FRACBITS;
      polyobjs[polyIndex].startSpot.y = y << FRACBITS;
      SpawnPolyobj(polyIndex, angle, (type == PO_SPAWNCRUSH_TYPE), false);
      polyIndex++;
    }
  }
  mt = (const hexen_mapthing_t *) data;
  for (i = 0; i < numthings; i++, mt++)
  {
    x = SHORT(mt->x);
    y = SHORT(mt->y);
    angle = SHORT(mt->angle);
    type = SHORT(mt->type);
    if (type == PO_ANCHOR_TYPE)
    { /* Polyobj Anchor Pt. */
      TranslateToStartSpot(angle, x << FRACBITS, y << FRACBITS);
    }
  }
  W_UnlockLumpNum(lump);
}

void PO_Init(int lump)
{
  int i;

  if (!po_NumPolyobjs)
    return;

  polyobjs = Z_Malloc(po_NumPolyobjs * sizeof(polyobj_t), PU_LEVEL, 0);
  memset(polyobjs, 0, po_NumPolyobjs * sizeof(polyobj_t));

  PO_LoadThings(lump);

  /* check for a startspot without an anchor point */
  for (i = 0; i < po_NumPolyobjs; i++)
  {
    if (!polyobjs[i].originalPts)
    {
      I_Error("PO_Init:  StartSpot located without an Anchor point: %d\n",
              polyobjs[i].tag);
    }
  }
  InitBlockMap();
}

dbool PO_Busy(int polyobj)
{
  polyobj_t *poly;

  poly = GetPolyobj(polyobj);
  if (!poly || !poly->specialdata)
  {
    return false;
  }
  return true;
}
