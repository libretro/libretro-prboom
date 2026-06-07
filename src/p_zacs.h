/* p_zacs.h: ZDoom ACS virtual machine.
 *
 * Runs the enhanced ACS object formats ZDoom-era maps carry in their
 * BEHAVIOR lumps: raw ACS0, big-enhanced ACSE, and acc's default
 * little-enhanced ACSe (packed variable-width pcodes) including the
 * ACS0-wrapped compatibility layout.  Covers the full ZDoom pcode set:
 * every opcode decodes its operands correctly (the interpreter can never
 * desync on unknown bytecode); opcodes whose semantics this engine
 * cannot honour yet execute as stack-disciplined no-ops and report
 * themselves once.
 *
 * This VM is separate from the Hexen game's p_acs interpreter: the
 * Hexen game keeps its vanilla VM and savegame format untouched; the
 * zdoom map format routes its ACS specials here. */

#ifndef __P_ZACS__
#define __P_ZACS__

#include "doomtype.h"
#include "r_defs.h"
#include "p_mobj.h"

/* Parse the BEHAVIOR lump for the current level.  Returns true when at
 * least one script was registered.  Frees the previous level's tables. */
dbool Z_ACSLoadBehavior(int lump);

/* True when the current level has ZACS scripts loaded. */
dbool Z_ACSActive(void);

/* Run all OPEN scripts (called once from P_SetupLevel after spawn). */
void Z_ACSRunOpenScripts(void);

/* ACS_Execute / ACS_ExecuteAlways.  args may be NULL. */
dbool Z_ACSStart(int number, int map, const int *args, int argc,
                 mobj_t *activator, line_t *line, int side, dbool always);

dbool Z_ACSSuspend(int number);
dbool Z_ACSTerminate(int number);

/* Per-player ENTER scripts (hook point; currently invoked with the
 * console player from Z_ACSRunOpenScripts). */
void Z_ACSRunEnterScripts(mobj_t *playermo);

#endif
