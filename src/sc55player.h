/* Roland Sound Canvas music players (sc55player.c): the SC-55 and the
 * SC-88 family share one player, so asking for one unit puts the other
 * away. */
#ifndef SC55PLAYER_H
#define SC55PLAYER_H

#include "musicplayer.h"

extern const music_player_t sc55_player;
extern const music_player_t sc88_player;

/* Non-zero if a complete ROM set was found and the unit is up.  The
 * first call of a session looks for the ROMs. */
int I_SC55Available(void);
int I_SC88Available(void);

#endif
