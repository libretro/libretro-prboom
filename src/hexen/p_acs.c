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
#include "z_zone.h"
#include "m_swap.h"
#include "w_wad.h"
#include "p_tick.h"
#include "p_mobj.h"
#include "lprintf.h"
#include "hexen/p_acs.h"

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
  if (map && map != gamemap)
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
  /* Replay any scripts deferred for this map.  Wired up with the cross-map
   * hub travel; for now the store simply drains. */
  acsstore_t *store;

  for (store = ACSStore; store->map != 0; store++)
  {
    if (store->map == gamemap)
    {
      P_StartACS(store->script, 0, store->args, NULL, NULL, 0);
      store->map = -1;
    }
  }
}

void P_TagFinished(int tag)
{
  /* Wakes scripts waiting on a sector tag; meaningful once the interpreter
   * lands.  Harmless no-op until then. */
  (void) tag;
}

/* Bytecode interpreter — stubbed until the opcode set lands in a following
 * commit.  A scheduled script simply unschedules itself so nothing runs and
 * no thinker leaks. */
void T_InterpretACS(acs_t *script)
{
  if (script->delayCount)
  {
    script->delayCount--;
    return;
  }
  if (script->infoIndex >= 0 && script->infoIndex < ACScriptCount)
    ACSInfo[script->infoIndex].state = ASTE_INACTIVE;
  P_RemoveThinker(&script->thinker);
}
