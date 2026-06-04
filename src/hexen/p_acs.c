/* Hexen ACS (Action Code Script) — loader and script bookkeeping.
 *
 * Scaffold commit: this loads a map's BEHAVIOR lump, registers its scripts,
 * auto-starts the "open" scripts, and provides P_StartACS / terminate /
 * suspend plus the cross-map script store.  The started scripts are real
 * thinkers, but the bytecode interpreter (T_InterpretACS) is a no-op here, so
 * scripts schedule and unschedule cleanly without yet executing.  The
 * interpreter and the ACS_Execute line specials land in following commits. */

#include <string.h>
#include <stdio.h>

#include "doomtype.h"
#include "doomstat.h"
#include "hexen/p_mapinfo.h"
#include "hexen/hexen.h"
#include "hexen/po_man.h"
#include "z_zone.h"
#include "m_swap.h"
#include "w_wad.h"
#include "p_tick.h"
#include "p_mobj.h"
#include "m_random.h"
#include "p_spec.h"
#include "r_state.h"
#include "r_data.h"
#include "sounds.h"
#include "s_sound.h"
#include "dsda_hacked.h"
#include "lprintf.h"
#include "hexen/p_acs.h"
#include "hexen/sn_sonix.h"
#include "hexen/p_spec_hexen.h"

#define OPEN_SCRIPTS_BASE 1000

typedef struct
{
  int marker;
  int infoOffset;
  int code;
} PACKEDATTR acsHeader_t;

int          ACScriptCount;
const byte  *ActionCodeBase;
static int   ActionCodeSize;
acsInfo_t   *ACSInfo;
int          MapVars[MAX_ACS_MAP_VARS];
int          WorldVars[MAX_ACS_WORLD_VARS];
acsstore_t   ACSStore[MAX_ACS_STORE + 1];

static int          ACStringCount;
static const char **ACStrings;
static int          PCodeOffset;

static acs_t       *ACScript;        /* currently-executing script */
static int          SpecArgs[8];     /* line-special argument scratch */
#define PRINT_BUFFER_SIZE 256
static char         PrintBuffer[PRINT_BUFFER_SIZE];

static acs_t *NewScript;

static void  StartOpenACS(int number, int infoIndex, int offset);
static int   GetACSIndex(int number);
static dbool AddToACSStore(int map, int number, byte *args);

static int ReadCodeInt(void)
{
  int          result;
  const int   *ptr;

  if (PCodeOffset + 3 >= ActionCodeSize)
    return 0;
  ptr = (const int *) (ActionCodeBase + PCodeOffset);
  result = LONG(*ptr);
  PCodeOffset += 4;
  return result;
}

static int ReadOffset(void)
{
  int offset = ReadCodeInt();
  if (offset < 0 || offset >= ActionCodeSize)
    offset = 0;
  return offset;
}

void P_LoadACScripts(int lump)
{
  int                i;
  int                offset;
  const acsHeader_t *header;
  acsInfo_t         *info;

  ACScriptCount = 0;
  ACSInfo = NULL;
  ACStrings = NULL;
  ACStringCount = 0;

  if (lump < 0)
  {
    memset(MapVars, 0, sizeof(MapVars));
    return;
  }

  ActionCodeBase = W_CacheLumpNum(lump);
  ActionCodeSize = W_LumpLength(lump);

  if (ActionCodeSize < (int) sizeof(acsHeader_t))
  {
    memset(MapVars, 0, sizeof(MapVars));
    return;
  }

  header = (const acsHeader_t *) ActionCodeBase;
  PCodeOffset = LONG(header->infoOffset);

  ACScriptCount = ReadCodeInt();
  if (ACScriptCount <= 0)
  {
    ACScriptCount = 0;
    memset(MapVars, 0, sizeof(MapVars));
    return;
  }

  ACSInfo = Z_Malloc(ACScriptCount * sizeof(acsInfo_t), PU_LEVEL, 0);
  memset(ACSInfo, 0, ACScriptCount * sizeof(acsInfo_t));
  for (i = 0, info = ACSInfo; i < ACScriptCount; i++, info++)
  {
    info->number   = ReadCodeInt();
    info->offset   = ReadOffset();
    info->argCount = ReadCodeInt();
    if (info->argCount > MAX_SCRIPT_ARGS)
      info->argCount = MAX_SCRIPT_ARGS;

    if (info->number >= OPEN_SCRIPTS_BASE)
    {                           /* auto-activate */
      info->number -= OPEN_SCRIPTS_BASE;
      StartOpenACS(info->number, i, info->offset);
      info->state = ASTE_RUNNING;
    }
    else
      info->state = ASTE_INACTIVE;
  }

  ACStringCount = ReadCodeInt();
  if (ACStringCount < 0)
    ACStringCount = 0;
  if (ACStringCount)
  {
    ACStrings = Z_Malloc(ACStringCount * sizeof(char *), PU_LEVEL, 0);
    for (i = 0; i < ACStringCount; i++)
    {
      offset = ReadOffset();
      ACStrings[i] = (const char *) ActionCodeBase + offset;
    }
  }

  memset(MapVars, 0, sizeof(MapVars));
}

static void StartOpenACS(int number, int infoIndex, int offset)
{
  acs_t *script;

  script = Z_Malloc(sizeof(acs_t), PU_LEVEL, 0);
  memset(script, 0, sizeof(acs_t));
  script->number = number;
  script->delayCount = 35;       /* world objects get 1s to initialise */
  script->infoIndex = infoIndex;
  script->ip = offset;
  script->thinker.function.arg1 = (void (*)(void *)) T_InterpretACS;
  P_AddThinker(&script->thinker);
}

static int GetACSIndex(int number)
{
  int i;

  for (i = 0; i < ACScriptCount; i++)
    if (ACSInfo[i].number == number)
      return i;
  return -1;
}

dbool P_StartACS(int number, int map, byte *args, mobj_t *activator,
                 line_t *line, int side)
{
  int     i;
  acs_t  *script;
  int     infoIndex;
  aste_t *statePtr;

  NewScript = NULL;
  /* ACS map numbers are MAPINFO warp numbers, so the current map must be
   * compared in warp space (they differ from lump numbers in PWADs like
   * Deathkings). */
  if (map && map != P_GetMapWarpTrans(gamemap))
    return AddToACSStore(map, number, args);

  infoIndex = GetACSIndex(number);
  if (infoIndex == -1)
    return false;               /* unknown script */

  statePtr = &ACSInfo[infoIndex].state;
  if (*statePtr == ASTE_SUSPENDED)
  {                             /* resume a suspended script */
    *statePtr = ASTE_RUNNING;
    return true;
  }
  if (*statePtr != ASTE_INACTIVE)
    return false;               /* already executing */

  script = Z_Malloc(sizeof(acs_t), PU_LEVEL, 0);
  memset(script, 0, sizeof(acs_t));
  script->number = number;
  script->infoIndex = infoIndex;
  P_SetTarget(&script->activator, activator);
  script->line = line;
  script->side = side;
  script->ip = ACSInfo[infoIndex].offset;
  script->thinker.function.arg1 = (void (*)(void *)) T_InterpretACS;
  for (i = 0; i < MAX_SCRIPT_ARGS && i < ACSInfo[infoIndex].argCount; i++)
    script->vars[i] = args[i];
  *statePtr = ASTE_RUNNING;
  P_AddThinker(&script->thinker);
  NewScript = script;
  return true;
}

dbool P_TerminateACS(int number, int map)
{
  int infoIndex;

  (void) map;
  infoIndex = GetACSIndex(number);
  if (infoIndex == -1)
    return false;
  if (ACSInfo[infoIndex].state == ASTE_INACTIVE ||
      ACSInfo[infoIndex].state == ASTE_TERMINATING)
    return false;
  ACSInfo[infoIndex].state = ASTE_TERMINATING;
  return true;
}

dbool P_SuspendACS(int number, int map)
{
  int infoIndex;

  (void) map;
  infoIndex = GetACSIndex(number);
  if (infoIndex == -1)
    return false;
  if (ACSInfo[infoIndex].state == ASTE_INACTIVE ||
      ACSInfo[infoIndex].state == ASTE_SUSPENDED ||
      ACSInfo[infoIndex].state == ASTE_TERMINATING)
    return false;
  ACSInfo[infoIndex].state = ASTE_SUSPENDED;
  return true;
}

static dbool AddToACSStore(int map, int number, byte *args)
{
  int i;
  int index = -1;

  for (i = 0; ACSStore[i].map != 0; i++)
  {
    if (ACSStore[i].script == number && ACSStore[i].map == map)
      return false;             /* don't allow duplicates */
    if (index == -1 && ACSStore[i].map == -1)
      index = i;                /* remember first empty slot */
  }
  if (index == -1)
  {
    index = i;
    if (index >= MAX_ACS_STORE)
      return false;
    ACSStore[index + 1].map = 0;
  }
  ACSStore[index].map = map;
  ACSStore[index].script = number;
  ACSStore[index].args[0] = args ? args[0] : 0;
  ACSStore[index].args[1] = args ? args[1] : 0;
  ACSStore[index].args[2] = args ? args[2] : 0;
  ACSStore[index].args[3] = args ? args[3] : 0;
  return true;
}

void P_ACSInitNewGame(void)
{
  memset(WorldVars, 0, sizeof(WorldVars));
  memset(ACSStore, 0, sizeof(ACSStore));
}

void P_CheckACSStore(void)
{
  /* Replay any scripts other maps deferred for this one (ACS_Execute with a
   * map argument): the cross-map half of Hexen's hub scripting. */
  acsstore_t *store;

  for (store = ACSStore; store->map != 0; store++)
  {
    if (store->map == P_GetMapWarpTrans(gamemap))
    {
      P_StartACS(store->script, 0, store->args, NULL, NULL, 0);
      store->map = -1;
    }
  }
}
/* ------------------------------------------------------------------------- */
/* Bytecode interpreter                                                      */
/* ------------------------------------------------------------------------- */

#define SCRIPT_CONTINUE 0
#define SCRIPT_STOP     1
#define SCRIPT_TERMINATE 2

#define GAME_SINGLE_PLAYER   0
#define GAME_NET_COOPERATIVE 1
#define GAME_NET_DEATHMATCH  2

#define TEXTURE_TOP    0
#define TEXTURE_MIDDLE 1
#define TEXTURE_BOTTOM 2

static void ScriptFinished(int number);

/* Raw-tag iterators (the exported P_FindSectorFromLineTag takes a line_t; ACS
 * works from bare tag numbers). */
static int ACS_FindSectorFromTag(int tag, int start)
{
  int i;
  for (i = start + 1; i < numsectors; i++)
    if (sectors[i].tag == tag)
      return i;
  return -1;
}

static int ACS_FindLineFromTag(int tag, int start)
{
  int i;
  for (i = start + 1; i < numlines; i++)
    if (lines[i].tag == tag)
      return i;
  return -1;
}

static void Push(int value)
{
  if (ACScript->stackPtr < ACS_STACK_DEPTH)
    ACScript->stack[ACScript->stackPtr++] = value;
}

static int Pop(void)
{
  if (ACScript->stackPtr <= 0)
    return 0;
  return ACScript->stack[--ACScript->stackPtr];
}

static int Top(void)
{
  if (ACScript->stackPtr <= 0)
    return 0;
  return ACScript->stack[ACScript->stackPtr - 1];
}

static void Drop(void)
{
  if (ACScript->stackPtr > 0)
    ACScript->stackPtr--;
}

static int ReadScriptVar(void)
{
  int var = ReadCodeInt();
  if (var < 0 || var >= MAX_ACS_SCRIPT_VARS)
    return 0;
  return var;
}

static int ReadMapVar(void)
{
  int var = ReadCodeInt();
  if (var < 0 || var >= MAX_ACS_MAP_VARS)
    return 0;
  return var;
}

static int ReadWorldVar(void)
{
  int var = ReadCodeInt();
  if (var < 0 || var >= MAX_ACS_WORLD_VARS)
    return 0;
  return var;
}

static const char *StringLookup(int index)
{
  if (index < 0 || index >= ACStringCount)
    return "";
  return ACStrings[index];
}

/* Resolve an ACS sound name to a sfx index.  ACS scripts name sounds by
 * their SNDINFO tag ("GlassShatter"), but the live S_sfx table no longer
 * carries the tags: S_HexenLoadSndInfo repoints each entry's name at the
 * actual lump SNDINFO assigns it.  The pristine seed table keeps the tag
 * names at the same indices, so resolve against that. */
static int ACS_GetSoundID(const char *name)
{
  int i;

  if (!name || !name[0])
    return 0;
  for (i = 1; i < HEXEN_NUMSFX; i++)
    if (hexen_S_sfx[i].name && !strcasecmp(name, hexen_S_sfx[i].name))
      return i;
  return 0;
}

/* Run a Hexen line special from ACS: the interpreter keeps args as ints in
 * SpecArgs, the dispatcher takes a byte[5]. */
static void ACS_ExecLineSpecial(int special)
{
  byte b[5];
  b[0] = (byte) SpecArgs[0];
  b[1] = (byte) SpecArgs[1];
  b[2] = (byte) SpecArgs[2];
  b[3] = (byte) SpecArgs[3];
  b[4] = (byte) SpecArgs[4];
  P_ExecuteHexenLineSpecial(special, b, ACScript->line, ACScript->side,
                            ACScript->activator);
}

static dbool TagBusy(int tag)
{
  int secnum = -1;

  while ((secnum = ACS_FindSectorFromTag(tag, secnum)) >= 0)
  {
    if (sectors[secnum].floordata || sectors[secnum].ceilingdata)
      return true;
  }
  return false;
}

static void ThingCount(int type, int tid)
{
  int          count;
  int          searcher;
  mobj_t      *mobj;
  mobjtype_t   moType;
  thinker_t   *think;

  if (!(type + tid))
    return;
  if (type < 0 || type >= TRANSLATE_THING_TYPE_COUNT)
  {
    Push(0);
    return;
  }
  moType = TranslateThingType[type];
  count = 0;
  searcher = -1;
  if (tid)
  {
    while ((mobj = P_FindMobjFromTID(tid, &searcher)) != NULL)
    {
      if (type == 0)
        count++;
      else if (moType == mobj->type)
      {
        if (mobj->flags & MF_COUNTKILL && mobj->health <= 0)
          continue;
        count++;
      }
    }
  }
  else
  {
    for (think = thinkercap.next; think != &thinkercap; think = think->next)
    {
      if (think->function.arg1 != (void (*)(void *)) P_MobjThinker)
        continue;
      mobj = (mobj_t *) think;
      if (mobj->type != moType)
        continue;
      if (mobj->flags & MF_COUNTKILL && mobj->health <= 0)
        continue;
      count++;
    }
  }
  Push(count);
}

static void ACS_PrintConcat(const char *s)
{
  size_t len = strlen(PrintBuffer);
  if (len < PRINT_BUFFER_SIZE - 1)
    snprintf(PrintBuffer + len, PRINT_BUFFER_SIZE - len, "%s", s);
}

static int CmdNOP(void)       { return SCRIPT_CONTINUE; }
static int CmdTerminate(void) { return SCRIPT_TERMINATE; }

static int CmdSuspend(void)
{
  ACSInfo[ACScript->infoIndex].state = ASTE_SUSPENDED;
  return SCRIPT_STOP;
}

static int CmdPushNumber(void) { Push(ReadCodeInt()); return SCRIPT_CONTINUE; }

static int CmdLSpec1(void)
{
  int special = ReadCodeInt();
  SpecArgs[0] = Pop();
  ACS_ExecLineSpecial(special);
  return SCRIPT_CONTINUE;
}
static int CmdLSpec2(void)
{
  int special = ReadCodeInt();
  SpecArgs[1] = Pop();
  SpecArgs[0] = Pop();
  ACS_ExecLineSpecial(special);
  return SCRIPT_CONTINUE;
}
static int CmdLSpec3(void)
{
  int special = ReadCodeInt();
  SpecArgs[2] = Pop();
  SpecArgs[1] = Pop();
  SpecArgs[0] = Pop();
  ACS_ExecLineSpecial(special);
  return SCRIPT_CONTINUE;
}
static int CmdLSpec4(void)
{
  int special = ReadCodeInt();
  SpecArgs[3] = Pop();
  SpecArgs[2] = Pop();
  SpecArgs[1] = Pop();
  SpecArgs[0] = Pop();
  ACS_ExecLineSpecial(special);
  return SCRIPT_CONTINUE;
}
static int CmdLSpec5(void)
{
  int special = ReadCodeInt();
  SpecArgs[4] = Pop();
  SpecArgs[3] = Pop();
  SpecArgs[2] = Pop();
  SpecArgs[1] = Pop();
  SpecArgs[0] = Pop();
  ACS_ExecLineSpecial(special);
  return SCRIPT_CONTINUE;
}
static int CmdLSpec1Direct(void)
{
  int special = ReadCodeInt();
  SpecArgs[0] = ReadCodeInt();
  ACS_ExecLineSpecial(special);
  return SCRIPT_CONTINUE;
}
static int CmdLSpec2Direct(void)
{
  int special = ReadCodeInt();
  SpecArgs[0] = ReadCodeInt();
  SpecArgs[1] = ReadCodeInt();
  ACS_ExecLineSpecial(special);
  return SCRIPT_CONTINUE;
}
static int CmdLSpec3Direct(void)
{
  int special = ReadCodeInt();
  SpecArgs[0] = ReadCodeInt();
  SpecArgs[1] = ReadCodeInt();
  SpecArgs[2] = ReadCodeInt();
  ACS_ExecLineSpecial(special);
  return SCRIPT_CONTINUE;
}
static int CmdLSpec4Direct(void)
{
  int special = ReadCodeInt();
  SpecArgs[0] = ReadCodeInt();
  SpecArgs[1] = ReadCodeInt();
  SpecArgs[2] = ReadCodeInt();
  SpecArgs[3] = ReadCodeInt();
  ACS_ExecLineSpecial(special);
  return SCRIPT_CONTINUE;
}
static int CmdLSpec5Direct(void)
{
  int special = ReadCodeInt();
  SpecArgs[0] = ReadCodeInt();
  SpecArgs[1] = ReadCodeInt();
  SpecArgs[2] = ReadCodeInt();
  SpecArgs[3] = ReadCodeInt();
  SpecArgs[4] = ReadCodeInt();
  ACS_ExecLineSpecial(special);
  return SCRIPT_CONTINUE;
}

static int CmdAdd(void)      { Push(Pop() + Pop()); return SCRIPT_CONTINUE; }
static int CmdSubtract(void) { int b = Pop(); Push(Pop() - b); return SCRIPT_CONTINUE; }
static int CmdMultiply(void) { Push(Pop() * Pop()); return SCRIPT_CONTINUE; }
static int CmdDivide(void)   { int b = Pop(); Push(b ? Pop() / b : (Drop(), 0)); return SCRIPT_CONTINUE; }
static int CmdModulus(void)  { int b = Pop(); Push(b ? Pop() % b : (Drop(), 0)); return SCRIPT_CONTINUE; }
static int CmdEQ(void)       { Push(Pop() == Pop()); return SCRIPT_CONTINUE; }
static int CmdNE(void)       { Push(Pop() != Pop()); return SCRIPT_CONTINUE; }
static int CmdLT(void)       { int b = Pop(); Push(Pop() <  b); return SCRIPT_CONTINUE; }
static int CmdGT(void)       { int b = Pop(); Push(Pop() >  b); return SCRIPT_CONTINUE; }
static int CmdLE(void)       { int b = Pop(); Push(Pop() <= b); return SCRIPT_CONTINUE; }
static int CmdGE(void)       { int b = Pop(); Push(Pop() >= b); return SCRIPT_CONTINUE; }

static int CmdAssignScriptVar(void) { ACScript->vars[ReadScriptVar()] = Pop(); return SCRIPT_CONTINUE; }
static int CmdAssignMapVar(void)    { MapVars[ReadMapVar()] = Pop(); return SCRIPT_CONTINUE; }
static int CmdAssignWorldVar(void)  { WorldVars[ReadWorldVar()] = Pop(); return SCRIPT_CONTINUE; }
static int CmdPushScriptVar(void)   { Push(ACScript->vars[ReadScriptVar()]); return SCRIPT_CONTINUE; }
static int CmdPushMapVar(void)      { Push(MapVars[ReadMapVar()]); return SCRIPT_CONTINUE; }
static int CmdPushWorldVar(void)    { Push(WorldVars[ReadWorldVar()]); return SCRIPT_CONTINUE; }
static int CmdAddScriptVar(void)    { ACScript->vars[ReadScriptVar()] += Pop(); return SCRIPT_CONTINUE; }
static int CmdAddMapVar(void)       { MapVars[ReadMapVar()] += Pop(); return SCRIPT_CONTINUE; }
static int CmdAddWorldVar(void)     { WorldVars[ReadWorldVar()] += Pop(); return SCRIPT_CONTINUE; }
static int CmdSubScriptVar(void)    { ACScript->vars[ReadScriptVar()] -= Pop(); return SCRIPT_CONTINUE; }
static int CmdSubMapVar(void)       { MapVars[ReadMapVar()] -= Pop(); return SCRIPT_CONTINUE; }
static int CmdSubWorldVar(void)     { WorldVars[ReadWorldVar()] -= Pop(); return SCRIPT_CONTINUE; }
static int CmdMulScriptVar(void)    { ACScript->vars[ReadScriptVar()] *= Pop(); return SCRIPT_CONTINUE; }
static int CmdMulMapVar(void)       { MapVars[ReadMapVar()] *= Pop(); return SCRIPT_CONTINUE; }
static int CmdMulWorldVar(void)     { WorldVars[ReadWorldVar()] *= Pop(); return SCRIPT_CONTINUE; }
static int CmdDivScriptVar(void)    { int v = ReadScriptVar(); int b = Pop(); if (b) ACScript->vars[v] /= b; return SCRIPT_CONTINUE; }
static int CmdDivMapVar(void)       { int v = ReadMapVar();    int b = Pop(); if (b) MapVars[v] /= b; return SCRIPT_CONTINUE; }
static int CmdDivWorldVar(void)     { int v = ReadWorldVar();  int b = Pop(); if (b) WorldVars[v] /= b; return SCRIPT_CONTINUE; }
static int CmdModScriptVar(void)    { int v = ReadScriptVar(); int b = Pop(); if (b) ACScript->vars[v] %= b; return SCRIPT_CONTINUE; }
static int CmdModMapVar(void)       { int v = ReadMapVar();    int b = Pop(); if (b) MapVars[v] %= b; return SCRIPT_CONTINUE; }
static int CmdModWorldVar(void)     { int v = ReadWorldVar();  int b = Pop(); if (b) WorldVars[v] %= b; return SCRIPT_CONTINUE; }
static int CmdIncScriptVar(void)    { ++ACScript->vars[ReadScriptVar()]; return SCRIPT_CONTINUE; }
static int CmdIncMapVar(void)       { ++MapVars[ReadMapVar()]; return SCRIPT_CONTINUE; }
static int CmdIncWorldVar(void)     { ++WorldVars[ReadWorldVar()]; return SCRIPT_CONTINUE; }
static int CmdDecScriptVar(void)    { --ACScript->vars[ReadScriptVar()]; return SCRIPT_CONTINUE; }
static int CmdDecMapVar(void)       { --MapVars[ReadMapVar()]; return SCRIPT_CONTINUE; }
static int CmdDecWorldVar(void)     { --WorldVars[ReadWorldVar()]; return SCRIPT_CONTINUE; }

static int CmdGoto(void)   { PCodeOffset = ReadOffset(); return SCRIPT_CONTINUE; }
static int CmdIfGoto(void) { int o = ReadOffset(); if (Pop()) PCodeOffset = o; return SCRIPT_CONTINUE; }
static int CmdDrop(void)   { Drop(); return SCRIPT_CONTINUE; }
static int CmdDelay(void)  { ACScript->delayCount = Pop(); return SCRIPT_STOP; }
static int CmdDelayDirect(void) { ACScript->delayCount = ReadCodeInt(); return SCRIPT_STOP; }

static int CmdRandom(void)
{
  int high = Pop();
  int low  = Pop();
  Push(low + (P_Random(pr_heretic) % (high - low + 1)));
  return SCRIPT_CONTINUE;
}
static int CmdRandomDirect(void)
{
  int low  = ReadCodeInt();
  int high = ReadCodeInt();
  Push(low + (P_Random(pr_heretic) % (high - low + 1)));
  return SCRIPT_CONTINUE;
}

static int CmdThingCount(void)
{
  int tid = Pop();
  ThingCount(Pop(), tid);
  return SCRIPT_CONTINUE;
}
static int CmdThingCountDirect(void)
{
  int type = ReadCodeInt();
  ThingCount(type, ReadCodeInt());
  return SCRIPT_CONTINUE;
}

static int CmdTagWait(void)
{
  ACSInfo[ACScript->infoIndex].waitValue = Pop();
  ACSInfo[ACScript->infoIndex].state = ASTE_WAITINGFORTAG;
  return SCRIPT_STOP;
}
static int CmdTagWaitDirect(void)
{
  ACSInfo[ACScript->infoIndex].waitValue = ReadCodeInt();
  ACSInfo[ACScript->infoIndex].state = ASTE_WAITINGFORTAG;
  return SCRIPT_STOP;
}
static int CmdPolyWait(void)
{
  ACSInfo[ACScript->infoIndex].waitValue = Pop();
  ACSInfo[ACScript->infoIndex].state = ASTE_WAITINGFORPOLY;
  return SCRIPT_STOP;
}
static int CmdPolyWaitDirect(void)
{
  ACSInfo[ACScript->infoIndex].waitValue = ReadCodeInt();
  ACSInfo[ACScript->infoIndex].state = ASTE_WAITINGFORPOLY;
  return SCRIPT_STOP;
}

static int CmdChangeFloor(void)
{
  int flat = R_FlatNumForName(StringLookup(Pop()));
  int tag  = Pop();
  int s = -1;
  while ((s = ACS_FindSectorFromTag(tag, s)) >= 0)
    sectors[s].floorpic = flat;
  return SCRIPT_CONTINUE;
}
static int CmdChangeFloorDirect(void)
{
  int tag  = ReadCodeInt();
  int flat = R_FlatNumForName(StringLookup(ReadCodeInt()));
  int s = -1;
  while ((s = ACS_FindSectorFromTag(tag, s)) >= 0)
    sectors[s].floorpic = flat;
  return SCRIPT_CONTINUE;
}
static int CmdChangeCeiling(void)
{
  int flat = R_FlatNumForName(StringLookup(Pop()));
  int tag  = Pop();
  int s = -1;
  while ((s = ACS_FindSectorFromTag(tag, s)) >= 0)
    sectors[s].ceilingpic = flat;
  return SCRIPT_CONTINUE;
}
static int CmdChangeCeilingDirect(void)
{
  int tag  = ReadCodeInt();
  int flat = R_FlatNumForName(StringLookup(ReadCodeInt()));
  int s = -1;
  while ((s = ACS_FindSectorFromTag(tag, s)) >= 0)
    sectors[s].ceilingpic = flat;
  return SCRIPT_CONTINUE;
}

static int CmdRestart(void) { PCodeOffset = ACSInfo[ACScript->infoIndex].offset; return SCRIPT_CONTINUE; }

static int CmdAndLogical(void)  { Push(Pop() && Pop()); return SCRIPT_CONTINUE; }
static int CmdOrLogical(void)   { Push(Pop() || Pop()); return SCRIPT_CONTINUE; }
static int CmdAndBitwise(void)  { Push(Pop() & Pop()); return SCRIPT_CONTINUE; }
static int CmdOrBitwise(void)   { Push(Pop() | Pop()); return SCRIPT_CONTINUE; }
static int CmdEorBitwise(void)  { Push(Pop() ^ Pop()); return SCRIPT_CONTINUE; }
static int CmdNegateLogical(void) { Push(!Pop()); return SCRIPT_CONTINUE; }
static int CmdLShift(void)      { int b = Pop(); Push(Pop() << b); return SCRIPT_CONTINUE; }
static int CmdRShift(void)      { int b = Pop(); Push(Pop() >> b); return SCRIPT_CONTINUE; }
static int CmdUnaryMinus(void)  { Push(-Pop()); return SCRIPT_CONTINUE; }
static int CmdIfNotGoto(void)   { int o = ReadOffset(); if (Pop() == 0) PCodeOffset = o; return SCRIPT_CONTINUE; }
static int CmdLineSide(void)    { Push(ACScript->side); return SCRIPT_CONTINUE; }

static int CmdScriptWait(void)
{
  ACSInfo[ACScript->infoIndex].waitValue = Pop();
  ACSInfo[ACScript->infoIndex].state = ASTE_WAITINGFORSCRIPT;
  return SCRIPT_STOP;
}
static int CmdScriptWaitDirect(void)
{
  ACSInfo[ACScript->infoIndex].waitValue = ReadCodeInt();
  ACSInfo[ACScript->infoIndex].state = ASTE_WAITINGFORSCRIPT;
  return SCRIPT_STOP;
}
static int CmdClearLineSpecial(void)
{
  if (ACScript->line)
    ACScript->line->special = 0;
  return SCRIPT_CONTINUE;
}
static int CmdCaseGoto(void)
{
  int value  = ReadCodeInt();
  int offset = ReadOffset();
  if (Top() == value)
  {
    PCodeOffset = offset;
    Drop();
  }
  return SCRIPT_CONTINUE;
}

static int CmdBeginPrint(void) { *PrintBuffer = 0; return SCRIPT_CONTINUE; }
static int CmdEndPrint(void)
{
  player_t *player;
  if (ACScript->activator && ACScript->activator->player)
    player = ACScript->activator->player;
  else
    player = &players[consoleplayer];
  player->message = PrintBuffer;
  return SCRIPT_CONTINUE;
}
static int CmdEndPrintBold(void)
{
  int i;
  for (i = 0; i < MAXPLAYERS; i++)
    if (playeringame[i])
      players[i].message = PrintBuffer;
  return SCRIPT_CONTINUE;
}
static int CmdPrintString(void) { ACS_PrintConcat(StringLookup(Pop())); return SCRIPT_CONTINUE; }
static int CmdPrintNumber(void)
{
  char tmp[16];
  snprintf(tmp, sizeof(tmp), "%d", Pop());
  ACS_PrintConcat(tmp);
  return SCRIPT_CONTINUE;
}
static int CmdPrintCharacter(void)
{
  char tmp[2];
  tmp[0] = (char) Pop();
  tmp[1] = '\0';
  ACS_PrintConcat(tmp);
  return SCRIPT_CONTINUE;
}

static int CmdPlayerCount(void)
{
  int i, count = 0;
  for (i = 0; i < MAXPLAYERS; i++)
    count += playeringame[i];
  Push(count);
  return SCRIPT_CONTINUE;
}
static int CmdGameType(void)
{
  int t;
  if (!netgame)        t = GAME_SINGLE_PLAYER;
  else if (deathmatch) t = GAME_NET_DEATHMATCH;
  else                 t = GAME_NET_COOPERATIVE;
  Push(t);
  return SCRIPT_CONTINUE;
}
static int CmdGameSkill(void) { Push(gameskill); return SCRIPT_CONTINUE; }
static int CmdTimer(void)     { Push(leveltime); return SCRIPT_CONTINUE; }

static int CmdSectorSound(void)
{
  int volume;
  mobj_t *mobj = NULL;
  if (ACScript->line)
    mobj = (mobj_t *) &ACScript->line->frontsector->soundorg;
  volume = Pop();
  (void) volume;
  S_StartSound(mobj, ACS_GetSoundID(StringLookup(Pop())));
  return SCRIPT_CONTINUE;
}
static int CmdThingSound(void)
{
  int tid, sound, searcher = -1;
  mobj_t *mobj;
  Pop();                        /* volume: sounds play at default volume */
  sound = ACS_GetSoundID(StringLookup(Pop()));
  tid = Pop();
  while ((mobj = P_FindMobjFromTID(tid, &searcher)) != NULL)
    S_StartSound(mobj, sound);
  return SCRIPT_CONTINUE;
}
static int CmdAmbientSound(void)
{
  int volume = Pop();
  (void) volume;
  S_StartSound(NULL, ACS_GetSoundID(StringLookup(Pop())));
  return SCRIPT_CONTINUE;
}
static int CmdSoundSequence(void)
{
  mobj_t *mobj = NULL;
  if (ACScript->line)
    mobj = (mobj_t *) &ACScript->line->frontsector->soundorg;
  SN_StartSequenceName(mobj, StringLookup(Pop()));
  return SCRIPT_CONTINUE;
}

static int CmdSetLineTexture(void)
{
  int texture = R_TextureNumForName(StringLookup(Pop()));
  int position = Pop();
  int side = Pop();
  int lineTag = Pop();
  int s = -1;
  while ((s = ACS_FindLineFromTag(lineTag, s)) >= 0)
  {
    line_t *line = &lines[s];
    if (line->sidenum[side] == NO_INDEX)
      continue;
    if (position == TEXTURE_MIDDLE)
      sides[line->sidenum[side]].midtexture = texture;
    else if (position == TEXTURE_BOTTOM)
      sides[line->sidenum[side]].bottomtexture = texture;
    else
      sides[line->sidenum[side]].toptexture = texture;
  }
  return SCRIPT_CONTINUE;
}
static int CmdSetLineBlocking(void)
{
  int blocking = Pop() ? ML_BLOCKING : 0;
  int lineTag = Pop();
  int s = -1;
  while ((s = ACS_FindLineFromTag(lineTag, s)) >= 0)
    lines[s].flags = (lines[s].flags & ~ML_BLOCKING) | blocking;
  return SCRIPT_CONTINUE;
}
static int CmdSetLineSpecial(void)
{
  int arg5 = Pop();
  int arg4 = Pop();
  int arg3 = Pop();
  int arg2 = Pop();
  int arg1 = Pop();
  int special = Pop();
  int lineTag = Pop();
  int s = -1;
  while ((s = ACS_FindLineFromTag(lineTag, s)) >= 0)
  {
    line_t *line = &lines[s];
    line->special = special;
    line->args[0] = (unsigned char) arg1;
    line->args[1] = (unsigned char) arg2;
    line->args[2] = (unsigned char) arg3;
    line->args[3] = (unsigned char) arg4;
    line->args[4] = (unsigned char) arg5;
  }
  return SCRIPT_CONTINUE;
}

static int (*PCodeCmds[]) (void) =
{
  CmdNOP, CmdTerminate, CmdSuspend, CmdPushNumber,
  CmdLSpec1, CmdLSpec2, CmdLSpec3, CmdLSpec4, CmdLSpec5,
  CmdLSpec1Direct, CmdLSpec2Direct, CmdLSpec3Direct, CmdLSpec4Direct, CmdLSpec5Direct,
  CmdAdd, CmdSubtract, CmdMultiply, CmdDivide, CmdModulus,
  CmdEQ, CmdNE, CmdLT, CmdGT, CmdLE, CmdGE,
  CmdAssignScriptVar, CmdAssignMapVar, CmdAssignWorldVar,
  CmdPushScriptVar, CmdPushMapVar, CmdPushWorldVar,
  CmdAddScriptVar, CmdAddMapVar, CmdAddWorldVar,
  CmdSubScriptVar, CmdSubMapVar, CmdSubWorldVar,
  CmdMulScriptVar, CmdMulMapVar, CmdMulWorldVar,
  CmdDivScriptVar, CmdDivMapVar, CmdDivWorldVar,
  CmdModScriptVar, CmdModMapVar, CmdModWorldVar,
  CmdIncScriptVar, CmdIncMapVar, CmdIncWorldVar,
  CmdDecScriptVar, CmdDecMapVar, CmdDecWorldVar,
  CmdGoto, CmdIfGoto, CmdDrop, CmdDelay, CmdDelayDirect,
  CmdRandom, CmdRandomDirect, CmdThingCount, CmdThingCountDirect,
  CmdTagWait, CmdTagWaitDirect, CmdPolyWait, CmdPolyWaitDirect,
  CmdChangeFloor, CmdChangeFloorDirect, CmdChangeCeiling, CmdChangeCeilingDirect,
  CmdRestart, CmdAndLogical, CmdOrLogical, CmdAndBitwise, CmdOrBitwise,
  CmdEorBitwise, CmdNegateLogical, CmdLShift, CmdRShift, CmdUnaryMinus,
  CmdIfNotGoto, CmdLineSide, CmdScriptWait, CmdScriptWaitDirect,
  CmdClearLineSpecial, CmdCaseGoto, CmdBeginPrint, CmdEndPrint,
  CmdPrintString, CmdPrintNumber, CmdPrintCharacter, CmdPlayerCount,
  CmdGameType, CmdGameSkill, CmdTimer, CmdSectorSound, CmdAmbientSound,
  CmdSoundSequence, CmdSetLineTexture, CmdSetLineBlocking, CmdSetLineSpecial,
  CmdThingSound, CmdEndPrintBold
};

#define ACS_NUM_PCODES ((int)(sizeof(PCodeCmds) / sizeof(PCodeCmds[0])))

static void ScriptFinished(int number)
{
  int i;

  /* Wake any scripts that were waiting for this one to end. */
  for (i = 0; i < ACScriptCount; i++)
    if (ACSInfo[i].state == ASTE_WAITINGFORSCRIPT &&
        ACSInfo[i].waitValue == number)
      ACSInfo[i].state = ASTE_RUNNING;
}

void P_TagFinished(int tag)
{
  int i;

  if (TagBusy(tag))
    return;
  for (i = 0; i < ACScriptCount; i++)
    if (ACSInfo[i].state == ASTE_WAITINGFORTAG &&
        ACSInfo[i].waitValue == tag)
      ACSInfo[i].state = ASTE_RUNNING;
}

/* Wake any scripts waiting on a polyobject once it has stopped moving. */
void P_PolyobjFinished(int po)
{
  int i;

  if (PO_Busy(po) == true)
  {
    return;
  }
  for (i = 0; i < ACScriptCount; i++)
  {
    if (ACSInfo[i].state == ASTE_WAITINGFORPOLY
        && ACSInfo[i].waitValue == po)
    {
      ACSInfo[i].state = ASTE_RUNNING;
    }
  }
}

void T_InterpretACS(acs_t *script)
{
  int cmd;
  int action;

  if (ACSInfo[script->infoIndex].state == ASTE_TERMINATING)
  {
    ACSInfo[script->infoIndex].state = ASTE_INACTIVE;
    ScriptFinished(script->number);
    P_RemoveThinker(&script->thinker);
    return;
  }
  if (ACSInfo[script->infoIndex].state != ASTE_RUNNING)
    return;
  if (script->delayCount)
  {
    script->delayCount--;
    return;
  }
  ACScript = script;
  PCodeOffset = ACScript->ip;

  do
  {
    cmd = ReadCodeInt();
    if (cmd < 0 || cmd >= ACS_NUM_PCODES)
    {
      action = SCRIPT_TERMINATE;
      break;
    }
    action = PCodeCmds[cmd]();
  } while (action == SCRIPT_CONTINUE);

  ACScript->ip = PCodeOffset;

  if (action == SCRIPT_TERMINATE)
  {
    ACSInfo[script->infoIndex].state = ASTE_INACTIVE;
    ScriptFinished(ACScript->number);
    P_RemoveThinker(&ACScript->thinker);
  }
}
