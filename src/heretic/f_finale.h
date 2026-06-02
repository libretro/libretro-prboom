/* Heretic finale entry points (see heretic/f_finale.c). */
#ifndef HERETIC_F_FINALE_H
#define HERETIC_F_FINALE_H

#include "d_event.h"

void  Heretic_F_StartFinale(void);
dbool Heretic_F_Responder(event_t *event);
void  Heretic_F_Ticker(void);
void  Heretic_F_Drawer(void);

#endif
