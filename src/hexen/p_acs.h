/* Hexen ACS (Action Code Script) — data model and public interface.
 *
 * This is the scaffold: the BEHAVIOR-lump loader and script bookkeeping.  The
 * bytecode interpreter (T_InterpretACS) and the ACS_Execute line specials are
 * added in following commits; until then T_InterpretACS is a safe no-op so a
 * map's scripts load and register without running. */

#ifndef __HEXEN_P_ACS__
#define __HEXEN_P_ACS__

#include "r_defs.h"
#include "p_mobj.h"

#define MAX_ACS_SCRIPT_VARS 10
#define MAX_ACS_MAP_VARS    32
#define MAX_ACS_WORLD_VARS  64
#define ACS_STACK_DEPTH     32
#define MAX_ACS_STORE       20
#define MAX_SCRIPT_ARGS     4

typedef enum
{
  ASTE_INACTIVE,
  ASTE_RUNNING,
  ASTE_SUSPENDED,
  ASTE_WAITINGFORTAG,
  ASTE_WAITINGFORPOLY,
  ASTE_WAITINGFORSCRIPT,
  ASTE_TERMINATING
} aste_t;

typedef struct acs_s     acs_t;
typedef struct acsInfo_s acsInfo_t;

struct acsInfo_s
{
  int    number;
  int    offset;
  int    argCount;
  aste_t state;
  int    waitValue;
};

struct acs_s
{
  thinker_t thinker;
  mobj_t   *activator;
  line_t   *line;
  int       side;
  int       number;
  int       infoIndex;
  int       delayCount;
  int       stack[ACS_STACK_DEPTH];
  int       stackPtr;
  int       vars[MAX_ACS_SCRIPT_VARS];
  int       ip;
};

typedef struct
{
  int  map;                     /* target map */
  int  script;                  /* script number on target map */
  byte args[4];
} acsstore_t;

void  P_LoadACScripts(int lump);
dbool P_StartACS(int number, int map, byte *args, mobj_t *activator,
                 line_t *line, int side);
void CheckACSPresent(int number);
dbool P_TerminateACS(int number, int map);
dbool P_SuspendACS(int number, int map);
void  T_InterpretACS(acs_t *script);
void  P_TagFinished(int tag);
void  P_PolyobjFinished(int po);
void  P_ACSInitNewGame(void);
void  P_CheckACSStore(void);

extern int          ACScriptCount;
extern const byte  *ActionCodeBase;
extern acsInfo_t   *ACSInfo;
extern int          MapVars[MAX_ACS_MAP_VARS];
extern int          WorldVars[MAX_ACS_WORLD_VARS];
extern acsstore_t   ACSStore[MAX_ACS_STORE + 1];

#endif
