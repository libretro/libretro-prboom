/* u_zmapinfo.h: ZDoom old-syntax MAPINFO -> UMAPINFO translation
 * (see u_zmapinfo.c). */

#ifndef U_ZMAPINFO_H
#define U_ZMAPINFO_H

#include <stddef.h>
#include "doomtype.h"
#include "u_mapinfo.h"

int U_ParseZMapInfo(const char *buffer, size_t length);

/* LANGUAGE lump string lookup (NULL when absent). */
const char *U_ZLanguageLookup(const char *key);

/* apply LANGUAGE strings onto the BEX string table (before deh) */
void U_ZLanguageApplyStrings(void);

/* ZDoom MAPINFO 'noinfighting' flag for the given map entry */
dbool U_ZMapNoInfighting(const mapentry_t *e);

/* ZDoom MAPINFO 'sucktime' hours for the given map entry (0 = none) */
int U_ZMapSuckTime(const mapentry_t *e);

#endif
