/* Emacs style mode select   -*- C -*-
 *-----------------------------------------------------------------------------
 *
 *  Hexen hub world-state persistence (see sv_save.h).
 *
 *  Derived from Raven's sv_save.c by way of dsda-doom, reshaped for this
 *  core: the archives are in-memory per-map buffers that live only for the
 *  current session, so structures are snapshotted raw and only the pointers
 *  that cannot survive a level reload are translated -- mobj cross-links
 *  become archive indices, sector/line pointers become array indices, while
 *  pointers into static tables (states[], mobjinfo[]) are kept as-is.
 *
 *  The specialval_t unions are handled more carefully than Raven did: a
 *  union slot is only treated as a mobj pointer when its value matches a
 *  live archived mobj, otherwise its raw (integer) content is preserved.
 *  Raven dereferenced the union blindly, which only worked on DOS flat
 *  memory.
 *
 *  Not yet archived: active sound sequences (a restored map starts silent
 *  until the next sequence trigger), and the hub archives are not written
 *  into savegames (savegame integration is a separate step).
 *
 *-----------------------------------------------------------------------------
 */

#include <stddef.h>
#include <stdlib.h>

#include "doomstat.h"
#include "p_tick.h"
#include "p_spec.h"
#include "p_mobj.h"
#include "p_map.h"
#include "p_maputl.h"
#include "p_setup.h"
#include "d_player.h"
#include "g_game.h"
#include "r_state.h"
#include "lprintf.h"
#include "p_enemy.h"
#include "z_zone.h"

#include "hexen/p_acs.h"
#include "hexen/po_man.h"
#include "hexen/p_mapinfo.h"
#include "hexen/p_spec_hexen.h"
#include "hexen/sn_sonix.h"
#include "hexen/sv_save.h"
#include "p_saveg.h"
#include "r_main.h"
#include "g_game.h"

#define SV_MAX_MAPS         99
#define MAX_TARGET_PLAYERS 512

#define MOBJ_NULL      (-1)
#define MOBJ_XX_PLAYER (-2)
#define MOBJ_RAW       (-3)   /* specialval_t slot holds a plain integer */

/* Thinker classes archived per map.  Mobjs are handled separately. */
enum
{
  TC_END,
  TC_MOVE_CEILING,
  TC_VERTICAL_DOOR,
  TC_MOVE_FLOOR,
  TC_PLAT_RAISE,
  TC_INTERPRET_ACS,
  TC_FLOOR_WAGGLE,
  TC_LIGHT,
  TC_BUILD_PILLAR,
  TC_ROTATE_POLY,
  TC_MOVE_POLY,
  TC_POLY_DOOR
};

typedef struct
{
  size_t size;
  byte  *buffer;
} map_archive_t;

static map_archive_t map_archive[SV_MAX_MAPS];
static map_archive_t *ma_p;
static byte *buffer_p;

static int      MobjCount;
static mobj_t **MobjList;
static mobj_t ***TargetPlayerAddrs;
static int      TargetPlayerCount;

extern polyobj_t *polyobjs;
extern int        po_NumPolyobjs;
extern int        inv_ptr;
extern int        curpos;

/* ------------------------------------------------------------------------ */
/* Archive buffers                                                           */
/* ------------------------------------------------------------------------ */

static dbool MapArchiveExists(int map)
{
  return (map >= 0 && map < SV_MAX_MAPS && map_archive[map].buffer != NULL);
}

void SV_HubInit(void)
{
  int map;

  for (map = 0; map < SV_MAX_MAPS; map++)
    if (map_archive[map].buffer)
    {
      free(map_archive[map].buffer);
      map_archive[map].buffer = NULL;
      map_archive[map].size = 0;
    }
}

static void SV_OpenWrite(int map)
{
  ma_p = &map_archive[map];
  if (ma_p->buffer)
    free(ma_p->buffer);
  ma_p->size = 1024;
  ma_p->buffer = malloc(ma_p->size);
  buffer_p = ma_p->buffer;
}

static void SV_OpenRead(int map)
{
  ma_p = &map_archive[map];
  buffer_p = ma_p->buffer;
}

static void SV_Write(const void *buffer, size_t size)
{
  size_t delta = buffer_p - ma_p->buffer;

  while (delta + size > ma_p->size)
  {
    ma_p->size += 4096;
    ma_p->buffer = realloc(ma_p->buffer, ma_p->size);
    buffer_p = ma_p->buffer + delta;
  }
  memcpy(buffer_p, buffer, size);
  buffer_p += size;
}

static void SV_Read(void *buffer, size_t size)
{
  if ((size_t)(buffer_p - ma_p->buffer) + size > ma_p->size)
    I_Error("SV_Read: invalid map archive");
  memcpy(buffer, buffer_p, size);
  buffer_p += size;
}

static void SV_WriteLong(int val)  { SV_Write(&val, sizeof(int)); }

static int SV_ReadLong(void)
{
  int result;
  SV_Read(&result, sizeof(int));
  return result;
}

static void AssertSegment(int segType)
{
  if (SV_ReadLong() != segType)
    I_Error("SV: corrupt map archive (segment %d)", segType);
}

#define ASEG_MAP_HEADER 102
#define ASEG_WORLD      103
#define ASEG_POLYOBJS   104
#define ASEG_MOBJS      105
#define ASEG_THINKERS   106
#define ASEG_SCRIPTS    107
#define ASEG_MISC       108
#define ASEG_SOUNDS     110   /* appended; 109 is ASEG_END below */
#define ASEG_END        109

/* ------------------------------------------------------------------------ */
/* Mobj numbering and pointer translation                                    */
/* ------------------------------------------------------------------------ */

static dbool SV_IsMobjThinker(thinker_t *th)
{
  return th->function.arg1 == (void (*)(void *)) P_MobjThinker ||
         (th->function.arg1 == (void (*)(void *)) P_RemoveThinkerDelayed &&
          th->references);
}

static void SetMobjArchiveNums(void)
{
  thinker_t *th;
  mobj_t    *mobj;

  MobjCount = 0;
  for (th = thinkercap.next; th != &thinkercap; th = th->next)
  {
    if (!SV_IsMobjThinker(th))
      continue;
    mobj = (mobj_t *) th;
    if (mobj->player)
      continue;                 /* players are recreated at the destination */
    mobj->archiveNum = MobjCount++;
  }
}

static int GetMobjNum(mobj_t *mobj)
{
  if (mobj == NULL)
    return MOBJ_NULL;
  if (mobj->player)
    return MOBJ_XX_PLAYER;
  return mobj->archiveNum;
}

/* A specialval_t slot is only a mobj reference if it matches a live archived
 * mobj; anything else (counters, indexes, stale values) stays raw. */
static int GetMobjNumSafe(mobj_t *candidate)
{
  thinker_t *th;

  if (candidate == NULL)
    return MOBJ_RAW;            /* raw zero round-trips through the union */
  for (th = thinkercap.next; th != &thinkercap; th = th->next)
  {
    if (!SV_IsMobjThinker(th))
      continue;
    if ((mobj_t *) th == candidate)
      return GetMobjNum(candidate);
  }
  return MOBJ_RAW;
}

static void SetMobjPtr(mobj_t **ptr, int archiveNum)
{
  if (archiveNum == MOBJ_NULL)
  {
    *ptr = NULL;
    return;
  }
  if (archiveNum == MOBJ_XX_PLAYER)
  {
    if (TargetPlayerCount == MAX_TARGET_PLAYERS)
      I_Error("SV: exceeded MAX_TARGET_PLAYERS");
    TargetPlayerAddrs[TargetPlayerCount++] = ptr;
    *ptr = NULL;
    return;
  }
  if (archiveNum < 0 || archiveNum >= MobjCount)
    I_Error("SV: bad mobj archive number %d", archiveNum);
  P_SetTarget(ptr, MobjList[archiveNum]);
}

/* ------------------------------------------------------------------------ */
/* World (sectors / lines / sides)                                           */
/* ------------------------------------------------------------------------ */

static void ArchiveWorld(void)
{
  int       i, j;
  sector_t *sec;
  line_t   *li;
  side_t   *si;

  SV_WriteLong(ASEG_WORLD);
  for (i = 0, sec = sectors; i < numsectors; i++, sec++)
  {
    SV_WriteLong(sec->floorheight);
    SV_WriteLong(sec->ceilingheight);
    SV_WriteLong(sec->floorpic);
    SV_WriteLong(sec->ceilingpic);
    SV_WriteLong(sec->lightlevel);
    SV_WriteLong(sec->special);
    SV_WriteLong(sec->tag);
    SV_WriteLong(sec->seqType);
  }
  for (i = 0, li = lines; i < numlines; i++, li++)
  {
    SV_WriteLong(li->flags);
    SV_WriteLong(li->special);
    for (j = 0; j < 5; j++)
      SV_WriteLong(li->args[j]);
    for (j = 0; j < 2; j++)
    {
      if (li->sidenum[j] == NO_INDEX)
        continue;
      si = &sides[li->sidenum[j]];
      SV_WriteLong(si->textureoffset);
      SV_WriteLong(si->rowoffset);
      SV_WriteLong(si->toptexture);
      SV_WriteLong(si->bottomtexture);
      SV_WriteLong(si->midtexture);
    }
  }
}

static void UnarchiveWorld(void)
{
  int       i, j;
  sector_t *sec;
  line_t   *li;
  side_t   *si;

  AssertSegment(ASEG_WORLD);
  for (i = 0, sec = sectors; i < numsectors; i++, sec++)
  {
    sec->floorheight   = SV_ReadLong();
    sec->ceilingheight = SV_ReadLong();
    sec->floorpic      = SV_ReadLong();
    sec->ceilingpic    = SV_ReadLong();
    sec->lightlevel    = SV_ReadLong();
    sec->special       = SV_ReadLong();
    sec->tag           = SV_ReadLong();
    sec->seqType       = SV_ReadLong();
    sec->floordata     = NULL;
    sec->ceilingdata   = NULL;
    sec->soundtarget   = NULL;
  }
  for (i = 0, li = lines; i < numlines; i++, li++)
  {
    li->flags   = SV_ReadLong();
    li->special = SV_ReadLong();
    for (j = 0; j < 5; j++)
      li->args[j] = (byte) SV_ReadLong();
    for (j = 0; j < 2; j++)
    {
      if (li->sidenum[j] == NO_INDEX)
        continue;
      si = &sides[li->sidenum[j]];
      si->textureoffset = SV_ReadLong();
      si->rowoffset     = SV_ReadLong();
      si->toptexture    = SV_ReadLong();
      si->bottomtexture = SV_ReadLong();
      si->midtexture    = SV_ReadLong();
    }
  }
}

/* ------------------------------------------------------------------------ */
/* Polyobjects                                                               */
/* ------------------------------------------------------------------------ */

static void ArchivePolyobjs(void)
{
  int i;

  SV_WriteLong(ASEG_POLYOBJS);
  SV_WriteLong(po_NumPolyobjs);
  for (i = 0; i < po_NumPolyobjs; i++)
  {
    SV_WriteLong(polyobjs[i].tag);
    SV_WriteLong((int) polyobjs[i].angle);
    SV_WriteLong(polyobjs[i].startSpot.x);
    SV_WriteLong(polyobjs[i].startSpot.y);
  }
}

static void UnarchivePolyobjs(void)
{
  int     i;
  fixed_t deltaX, deltaY;
  angle_t ang;

  AssertSegment(ASEG_POLYOBJS);
  if (SV_ReadLong() != po_NumPolyobjs)
    I_Error("SV: bad polyobj count");
  for (i = 0; i < po_NumPolyobjs; i++)
  {
    if (SV_ReadLong() != polyobjs[i].tag)
      I_Error("SV: invalid polyobj tag");
    ang    = (angle_t) SV_ReadLong();
    PO_RotatePolyobj(polyobjs[i].tag, ang - polyobjs[i].angle);
    deltaX = SV_ReadLong() - polyobjs[i].startSpot.x;
    deltaY = SV_ReadLong() - polyobjs[i].startSpot.y;
    PO_MovePolyobj(polyobjs[i].tag, deltaX, deltaY);
  }
}

/* ------------------------------------------------------------------------ */
/* Mobjs                                                                     */
/* ------------------------------------------------------------------------ */

static void ArchiveMobjs(void)
{
  int        count;
  thinker_t *th;
  mobj_t    *mobj;

  SV_WriteLong(ASEG_MOBJS);
  SV_WriteLong(MobjCount);
  count = 0;
  for (th = thinkercap.next; th != &thinkercap; th = th->next)
  {
    if (!SV_IsMobjThinker(th))
      continue;
    mobj = (mobj_t *) th;
    if (mobj->player)
      continue;
    count++;
    /* delayed-removal corpses are written so references resolve, but they
     * are not resurrected on the other side */
    SV_WriteLong(th->function.arg1 ==
                 (void (*)(void *)) P_RemoveThinkerDelayed);
    {
      /* the record must be position-independent so user savegames can carry
       * it across runs of the program: state is the one pointer the reader
       * uses that would otherwise stay raw */
      mobj_t copy = *mobj;
      copy.state = (state_t *) (uintptr_t) (mobj->state - states);
      SV_Write(&copy, sizeof(mobj_t));
    }
    SV_WriteLong(GetMobjNum(mobj->target));
    SV_WriteLong(GetMobjNum(mobj->tracer));
    SV_WriteLong(GetMobjNum(mobj->lastenemy));
    SV_WriteLong(GetMobjNumSafe(mobj->special1.m));
    SV_WriteLong(GetMobjNumSafe(mobj->special2.m));
  }
  if (count != MobjCount)
    I_Error("SV: bad mobj count");
}

static void UnarchiveMobjs(void)
{
  int     i;
  int     num;
  mobj_t *mobj;
  dbool  *delayed;

  AssertSegment(ASEG_MOBJS);
  TargetPlayerAddrs = malloc(MAX_TARGET_PLAYERS * sizeof(mobj_t **));
  TargetPlayerCount = 0;
  MobjCount = SV_ReadLong();
  MobjList  = malloc(MobjCount * sizeof(mobj_t *));
  delayed   = malloc(MobjCount * sizeof(dbool));

  /* allocate every shell first so cross-references resolve in one pass */
  for (i = 0; i < MobjCount; i++)
    MobjList[i] = Z_Malloc(sizeof(mobj_t), PU_LEVEL, NULL);

  for (i = 0; i < MobjCount; i++)
  {
    mobj = MobjList[i];
    delayed[i] = SV_ReadLong();
    SV_Read(mobj, sizeof(mobj_t));
    mobj->state = states + (uintptr_t) mobj->state;

    /* invalidate everything that cannot survive the reload */
    memset(&mobj->thinker, 0, sizeof(thinker_t));
    mobj->snext = NULL;
    mobj->sprev = NULL;   /* ptr-to-ptr back-links in this fork */
    mobj->bnext = NULL;
    mobj->bprev = NULL;
    mobj->subsector = NULL;
    mobj->touching_sectorlist = NULL;
    mobj->player = NULL;
    mobj->target = NULL;
    mobj->tracer = NULL;
    mobj->lastenemy = NULL;

    num = SV_ReadLong();
    SetMobjPtr(&mobj->target, num);
    num = SV_ReadLong();
    SetMobjPtr(&mobj->tracer, num);
    num = SV_ReadLong();
    SetMobjPtr(&mobj->lastenemy, num);
    num = SV_ReadLong();
    if (num != MOBJ_RAW)
    {
      mobj->special1.m = NULL;
      SetMobjPtr(&mobj->special1.m, num);
    }
    num = SV_ReadLong();
    if (num != MOBJ_RAW)
    {
      mobj->special2.m = NULL;
      SetMobjPtr(&mobj->special2.m, num);
    }

    mobj->thinker.function.arg1 = (void (*)(void *)) P_MobjThinker;
    P_AddThinker(&mobj->thinker);
    P_SetThingPosition(mobj);
    mobj->info = &mobjinfo[mobj->type];
    mobj->floorz = mobj->subsector->sector->floorheight;
    mobj->ceilingz = mobj->subsector->sector->ceilingheight;
  }

  /* now that all references are counted, dispose of the dying */
  for (i = 0; i < MobjCount; i++)
    if (delayed[i])
      P_RemoveMobj(MobjList[i]);

  free(delayed);
}

/* ------------------------------------------------------------------------ */
/* Sector / poly / ACS thinkers                                              */
/* ------------------------------------------------------------------------ */

static polyobj_t *SV_GetPolyobj(int tag)
{
  int i;

  for (i = 0; i < po_NumPolyobjs; i++)
    if (polyobjs[i].tag == tag)
      return &polyobjs[i];
  return NULL;
}

typedef struct
{
  int    tclass;
  void (*fn)(void *);
  size_t size;
  int    sectorOfs;   /* offsetof a sector_t* to translate, or -1 */
} thinkInfo_t;

#define TINFO(tc, fn, type, sofs) \
  { tc, (void (*)(void *)) fn, sizeof(type), sofs }

static const thinkInfo_t ThinkerInfo[] =
{
  TINFO(TC_MOVE_CEILING,  T_HexenMoveCeiling, ceiling_t,     offsetof(ceiling_t, sector)),
  TINFO(TC_VERTICAL_DOOR, T_HexenVerticalDoor, vldoor_t,     offsetof(vldoor_t, sector)),
  TINFO(TC_MOVE_FLOOR,    T_MoveFloor,        floormove_t,   offsetof(floormove_t, sector)),
  TINFO(TC_PLAT_RAISE,    T_PlatRaise,        plat_t,        offsetof(plat_t, sector)),
  TINFO(TC_INTERPRET_ACS, T_InterpretACS,     acs_t,         -1),
  TINFO(TC_FLOOR_WAGGLE,  T_FloorWaggle,      planeWaggle_t, offsetof(planeWaggle_t, sector)),
  TINFO(TC_LIGHT,         T_HexenLight,       light_t,       offsetof(light_t, sector)),
  TINFO(TC_BUILD_PILLAR,  T_HexenBuildPillar, pillar_t,      offsetof(pillar_t, sector)),
  TINFO(TC_ROTATE_POLY,   T_RotatePoly,       polyevent_t,   -1),
  TINFO(TC_MOVE_POLY,     T_MovePoly,         polyevent_t,   -1),
  TINFO(TC_POLY_DOOR,     T_PolyDoor,         polydoor_t,    -1),
};

#define NUM_THINKER_INFO ((int)(sizeof(ThinkerInfo) / sizeof(ThinkerInfo[0])))

static void ArchiveThinkers(void)
{
  thinker_t *th;
  int        i;

  SV_WriteLong(ASEG_THINKERS);
  for (th = thinkercap.next; th != &thinkercap; th = th->next)
  {
    for (i = 0; i < NUM_THINKER_INFO; i++)
    {
      if (th->function.arg1 != (void (*)(void *)) ThinkerInfo[i].fn)
        continue;
      SV_WriteLong(ThinkerInfo[i].tclass);
      SV_Write(th, ThinkerInfo[i].size);
      if (ThinkerInfo[i].sectorOfs >= 0)
      {
        sector_t *sec =
          *(sector_t **) ((byte *) th + ThinkerInfo[i].sectorOfs);
        SV_WriteLong((int)(sec - sectors));
      }
      if (ThinkerInfo[i].tclass == TC_INTERPRET_ACS)
      {
        acs_t *script = (acs_t *) th;
        SV_WriteLong(GetMobjNum(script->activator));
        SV_WriteLong(script->line ? (int)(script->line - lines) : -1);
      }
      if (ThinkerInfo[i].tclass == TC_VERTICAL_DOOR)
      {
        /* keep the record position-independent (see ArchiveMobjs) */
        vldoor_t *door = (vldoor_t *) th;
        SV_WriteLong(door->line ? (int)(door->line - lines) : -1);
      }
      break;
    }
    if (i == NUM_THINKER_INFO &&
        !SV_IsMobjThinker(th) &&
        th->function.arg1 != (void (*)(void *)) P_RemoveThinkerDelayed)
      lprintf(LO_WARN,
              "SV: unarchived thinker class %p on map %d\n",
              (void *) th->function.arg1, gamemap);
  }
  SV_WriteLong(TC_END);
}

static void UnarchiveThinkers(void)
{
  int        tclass;
  int        i;
  thinker_t *th;

  AssertSegment(ASEG_THINKERS);
  for (;;)
  {
    tclass = SV_ReadLong();
    if (tclass == TC_END)
      break;
    for (i = 0; i < NUM_THINKER_INFO; i++)
      if (ThinkerInfo[i].tclass == tclass)
        break;
    if (i == NUM_THINKER_INFO)
      I_Error("SV: unknown thinker class %d", tclass);

    th = Z_Malloc(ThinkerInfo[i].size, PU_LEVEL, NULL);
    SV_Read(th, ThinkerInfo[i].size);
    memset(th, 0, sizeof(thinker_t));   /* links/refcounts are stale */
    th->function.arg1 = (void (*)(void *)) ThinkerInfo[i].fn;

    if (ThinkerInfo[i].sectorOfs >= 0)
    {
      sector_t **secp =
        (sector_t **) ((byte *) th + ThinkerInfo[i].sectorOfs);
      *secp = &sectors[SV_ReadLong()];
    }

    switch (tclass)
    {
      case TC_MOVE_CEILING:
        ((ceiling_t *) th)->sector->ceilingdata = th;
        /* T_HexenMoveCeiling finishes via P_RemoveActiveCeiling, which walks
         * ceiling->list; without re-adding, the restored thinker holds a
         * stale list node and removal is a use-after-free */
        P_AddActiveCeiling((ceiling_t *) th);
        break;
      case TC_VERTICAL_DOOR:
      {
        vldoor_t *door = (vldoor_t *) th;
        int       lnum = SV_ReadLong();

        door->line = lnum >= 0 ? &lines[lnum] : NULL;
        door->sector->ceilingdata = th;
        break;
      }
      case TC_MOVE_FLOOR:
        ((floormove_t *) th)->sector->floordata = th;
        break;
      case TC_BUILD_PILLAR:
        ((pillar_t *) th)->sector->floordata = th;
        break;
      case TC_FLOOR_WAGGLE:
        ((planeWaggle_t *) th)->sector->floordata = th;
        break;
      case TC_PLAT_RAISE:
        ((plat_t *) th)->sector->floordata = th;
        P_AddActivePlat((plat_t *) th);
        break;
      case TC_INTERPRET_ACS:
      {
        acs_t *script = (acs_t *) th;
        int    num    = SV_ReadLong();
        int    lnum;

        script->activator = NULL;
        SetMobjPtr(&script->activator, num);
        lnum = SV_ReadLong();
        script->line = lnum >= 0 ? &lines[lnum] : NULL;
        break;
      }
      case TC_ROTATE_POLY:
      case TC_MOVE_POLY:
      case TC_POLY_DOOR:
      {
        /* both event structs lead with the polyobj number */
        polyobj_t *po = SV_GetPolyobj(((polyevent_t *) th)->polyobj);
        if (po)
          po->specialdata = th;
        break;
      }
      default:
        break;
    }
    P_AddThinker(th);
  }
}

/* ------------------------------------------------------------------------ */
/* ACS state and misc                                                        */
/* ------------------------------------------------------------------------ */

static void ArchiveScripts(void)
{
  SV_WriteLong(ASEG_SCRIPTS);
  SV_WriteLong(ACScriptCount);
  SV_Write(ACSInfo, ACScriptCount * sizeof(acsInfo_t));
  SV_Write(MapVars, sizeof(MapVars[0]) * MAX_ACS_MAP_VARS);
}

static void UnarchiveScripts(void)
{
  AssertSegment(ASEG_SCRIPTS);
  if (SV_ReadLong() != ACScriptCount)
    I_Error("SV: ACS script count changed");
  SV_Read(ACSInfo, ACScriptCount * sizeof(acsInfo_t));
  SV_Read(MapVars, sizeof(MapVars[0]) * MAX_ACS_MAP_VARS);
}

/* Active sound sequences, so a looping mover keeps its place in its script
 * when the player comes back to the map (vanilla ASEG_SOUNDS). */
static void ArchiveSounds(void)
{
  seqnode_t *node;
  sector_t  *sec;
  int        i;

  SV_WriteLong(ASEG_SOUNDS);
  SV_WriteLong(ActiveSequences);

  for (node = SequenceListHead; node; node = node->next)
  {
    SV_WriteLong(node->sequence);
    SV_WriteLong(node->delayTics);
    SV_WriteLong(node->volume);
    SV_WriteLong(SN_GetSequenceOffset(node->sequence, node->sequencePtr));
    SV_WriteLong(node->currentSoundID);

    for (i = 0; i < po_NumPolyobjs; i++)
      if (node->mobj == (mobj_t *) &polyobjs[i].startSpot)
        break;

    if (i == po_NumPolyobjs)
    {                       /* attached to a sector, not a polyobj */
      sec = R_PointInSubsector(node->mobj->x, node->mobj->y)->sector;
      SV_WriteLong(0);
      SV_WriteLong((int) (sec - sectors));
    }
    else
    {
      SV_WriteLong(1);
      SV_WriteLong(i);
    }
  }
}

static void UnarchiveSounds(void)
{
  int     i;
  int     numSequences;
  int     sequence;
  int     delayTics;
  int     volume;
  int     seqOffset;
  int     soundID;
  int     polySnd;
  int     secNum;
  mobj_t *sndMobj;

  AssertSegment(ASEG_SOUNDS);
  numSequences = SV_ReadLong();

  for (i = 0; i < numSequences; i++)
  {
    sequence  = SV_ReadLong();
    delayTics = SV_ReadLong();
    volume    = SV_ReadLong();
    seqOffset = SV_ReadLong();
    soundID   = SV_ReadLong();
    polySnd   = SV_ReadLong();
    secNum    = SV_ReadLong();

    if (!polySnd)
      sndMobj = (mobj_t *) &sectors[secNum].soundorg;
    else
      sndMobj = (mobj_t *) &polyobjs[secNum].startSpot;

    SN_StartSequence(sndMobj, sequence);
    SN_ChangeNodeData(i, seqOffset, delayTics, volume, soundID);
  }
}

static void ArchiveMisc(void)
{
  int i;

  SV_WriteLong(ASEG_MISC);
  for (i = 0; i < CORPSEQUEUESIZE; i++)
    SV_WriteLong(GetMobjNum(corpseQueue[i]));
  SV_WriteLong(corpseQueueSlot);
}

static void UnarchiveMisc(void)
{
  int i;
  int num;

  AssertSegment(ASEG_MISC);
  for (i = 0; i < CORPSEQUEUESIZE; i++)
  {
    corpseQueue[i] = NULL;
    num = SV_ReadLong();
    if (num != MOBJ_XX_PLAYER)
      SetMobjPtr(&corpseQueue[i], num);
  }
  corpseQueueSlot = SV_ReadLong();
}

/* ------------------------------------------------------------------------ */
/* Save / load / travel                                                      */
/* ------------------------------------------------------------------------ */

static void SV_SaveMap(void)
{
  SV_OpenWrite(gamemap);
  SV_WriteLong(ASEG_MAP_HEADER);
  SV_WriteLong(leveltime);

  SetMobjArchiveNums();

  ArchiveWorld();
  ArchivePolyobjs();
  ArchiveMobjs();
  ArchiveThinkers();
  ArchiveScripts();
  ArchiveSounds();
  ArchiveMisc();

  SV_WriteLong(ASEG_END);
}

static void RemoveAllThinkers(void)
{
  thinker_t *th;
  thinker_t *next;

  th = thinkercap.next;
  while (th != &thinkercap)
  {
    next = th->next;
    if (SV_IsMobjThinker(th))
    {
      P_RemoveMobj((mobj_t *) th);
      P_RemoveThinkerDelayed(th);
    }
    else
      Z_Free(th);
    th = next;
  }
  P_InitThinkers();
}

static void SV_LoadMap(void)
{
  /* load a base level, then replace its state with the archive */
  G_InitNew(gameskill, gameepisode, gamemap);

  RemoveAllThinkers();

  SV_OpenRead(gamemap);
  AssertSegment(ASEG_MAP_HEADER);
  leveltime = SV_ReadLong();

  UnarchiveWorld();
  UnarchivePolyobjs();
  UnarchiveMobjs();
  UnarchiveThinkers();
  UnarchiveScripts();
  UnarchiveSounds();
  UnarchiveMisc();

  AssertSegment(ASEG_END);

  /* the TID cache built during P_SetupLevel points at the fresh mobjs that
   * RemoveAllThinkers just destroyed; rebuild it from the restored set */
  P_CreateTIDList();

  free(MobjList);
  MobjList = NULL;
}

/* ------------------------------------------------------------------------ */
/* User savegames                                                            */
/* ------------------------------------------------------------------------ */

/* The per-map archives above only live in memory, so a user save made
 * mid-hub used to forget every other map in the cluster: loading it and
 * walking back through a portal re-entered the old map fresh.  Embed the
 * archive buffers (now position-independent) in the savegame stream, plus
 * RebornPosition so respawns after the load use the right player start. */

void SV_ArchiveMaps(void)
{
  int map;
  int present = 0;

  if (!hexen)
    return;

  for (map = 0; map < SV_MAX_MAPS; map++)
    if (map_archive[map].buffer)
      present++;

  CheckSaveGame(2 * sizeof(int));
  memcpy(save_p, &RebornPosition, sizeof(int));
  save_p += sizeof(int);
  memcpy(save_p, &present, sizeof(int));
  save_p += sizeof(int);

  for (map = 0; map < SV_MAX_MAPS; map++)
  {
    int size;

    if (!map_archive[map].buffer)
      continue;

    size = (int) map_archive[map].size;
    CheckSaveGame(2 * sizeof(int) + size);
    memcpy(save_p, &map, sizeof(int));
    save_p += sizeof(int);
    memcpy(save_p, &size, sizeof(int));
    save_p += sizeof(int);
    memcpy(save_p, map_archive[map].buffer, size);
    save_p += size;
  }
}

void SV_UnArchiveMaps(void)
{
  int i;
  int present;

  if (!hexen)
    return;

  SV_HubInit();                 /* forget whatever hub we were in */

  memcpy(&RebornPosition, save_p, sizeof(int));
  save_p += sizeof(int);
  memcpy(&present, save_p, sizeof(int));
  save_p += sizeof(int);

  for (i = 0; i < present; i++)
  {
    int map, size;

    memcpy(&map, save_p, sizeof(int));
    save_p += sizeof(int);
    memcpy(&size, save_p, sizeof(int));
    save_p += sizeof(int);

    if (map < 0 || map >= SV_MAX_MAPS || size <= 0)
      I_Error("SV_UnArchiveMaps: corrupt hub archive");

    map_archive[map].size = size;
    map_archive[map].buffer = malloc(size);
    memcpy(map_archive[map].buffer, save_p, size);
    save_p += size;
  }
}

static dbool hub_travel;

dbool SV_IsHubTravel(void)
{
  return hub_travel;
}

void SV_MapTeleport(int map, int position)
{
  int      i;
  player_t playerBackup[MAXPLAYERS];
  mobj_t  *targetPlayerMobj;
  int      inventoryPtr;
  int      currentInvPos;
  dbool    playerWasReborn;

  hub_travel = TRUE;

  if (!deathmatch)
  {
    if (P_GetMapCluster(gamemap) == P_GetMapCluster(map))
      SV_SaveMap();             /* same cluster: archive the old map */
    else
      SV_HubInit();             /* new cluster: forget the old hub */
  }

  for (i = 0; i < MAXPLAYERS; i++)
    playerBackup[i] = players[i];

  /* G_InitNew tramples the inventory cursor */
  inventoryPtr  = inv_ptr;
  currentInvPos = curpos;

  TargetPlayerAddrs = NULL;
  TargetPlayerCount = 0;

  gamemap = map;   /* G_InitNew refreshes gamemapinfo for both paths */

  if (!deathmatch && MapArchiveExists(gamemap))
  {
    SV_LoadMap();
    P_MapStart();
  }
  else
  {
    G_InitNew(gameskill, gameepisode, gamemap);
    P_MapStart();

    /* destroy the freshly spawned player mobjs; the real ones follow */
    for (i = 0; i < MAXPLAYERS; i++)
      if (playeringame[i])
        P_RemoveMobj(players[i].mo);
  }

  /* restore the travellers */
  targetPlayerMobj = NULL;
  for (i = 0; i < MAXPLAYERS; i++)
  {
    if (!playeringame[i])
      continue;
    players[i] = playerBackup[i];
    players[i].attacker = NULL;
    players[i].poisoner = NULL;

    if (netgame && players[i].playerstate == PST_DEAD)
      players[i].playerstate = PST_REBORN;

    playerWasReborn = (players[i].playerstate == PST_REBORN);
    (void) playerWasReborn;

    if (deathmatch)
    {
      memset(players[i].frags, 0, sizeof(players[i].frags));
      G_DeathMatchSpawnPlayer(i);
    }
    else
    {
      const mapthing_t *start;

      if (position >= 0 && position < MAX_PLAYER_STARTS &&
          playerstarts[position][i].options)
        start = &playerstarts[position][i];
      else
        start = &playerstarts[0][i];
      P_SpawnPlayer(i, start);
    }

    if (targetPlayerMobj == NULL)
      targetPlayerMobj = players[i].mo;
  }

  /* anything that targeted a player mobj on the restored map follows the
   * first arrival */
  if (TargetPlayerAddrs)
  {
    for (i = 0; i < TargetPlayerCount; i++)
      *TargetPlayerAddrs[i] = targetPlayerMobj;
    free(TargetPlayerAddrs);
    TargetPlayerAddrs = NULL;
  }

  /* telefrag anything overlapping an arrival */
  for (i = 0; i < MAXPLAYERS; i++)
    if (playeringame[i])
      P_TeleportMove(players[i].mo, players[i].mo->x, players[i].mo->y,
                     FALSE);

  inv_ptr = inventoryPtr;
  curpos  = currentInvPos;

  /* run any scripts other maps deferred for this one */
  if (!deathmatch)
    P_CheckACSStore();

  P_MapEnd();

  hub_travel = FALSE;
}
