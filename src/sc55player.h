/* Roland SC-55 music player (sc55player.c). */
#ifndef SC55PLAYER_H
#define SC55PLAYER_H

#include "musicplayer.h"

extern const music_player_t sc55_player;

/* Non-zero if a complete ROM set was found and the unit is up.  The
 * first call of a session looks for the ROMs. */
int I_SC55Available(void);

#endif
