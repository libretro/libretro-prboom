/* u_zmapinfo.h: ZDoom old-syntax MAPINFO -> UMAPINFO translation
 * (see u_zmapinfo.c). */

#ifndef U_ZMAPINFO_H
#define U_ZMAPINFO_H

#include <stddef.h>

int U_ParseZMapInfo(const char *buffer, size_t length);

/* LANGUAGE lump string lookup (NULL when absent). */
const char *U_ZLanguageLookup(const char *key);

/* apply LANGUAGE strings onto the BEX string table (before deh) */
void U_ZLanguageApplyStrings(void);

#endif
