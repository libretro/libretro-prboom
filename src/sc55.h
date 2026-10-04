/*
 * Roland SC-55 emulation: a C89 single-file port of the Nuked-SC55
 * backend.  See sc55.c for the copyright notices.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */
#ifndef SC55_H
#define SC55_H

#include <stddef.h>
#include <stdint.h>

typedef struct sc55 sc55_t;

enum
{
    SC55_MODEL_MK2, /* SC-55mkII */
    SC55_MODEL_MK1  /* SC-55 */
};

enum
{
    SC55_ROM_ROM1,
    SC55_ROM_ROM2,
    SC55_ROM_SMROM,     /* sub-MCU, mkII only */
    SC55_ROM_WAVEROM1,
    SC55_ROM_WAVEROM2,
    SC55_ROM_WAVEROM3,  /* mk1 only */
    SC55_ROM_COUNT
};

/* NULL if the allocation fails. */
sc55_t *sc55_new(int model);
void    sc55_free(sc55_t *s);

/* `data` is the dump as it is on disk; wave ROMs are unscrambled here.
 * Returns 0 if the image does not fit the slot. */
int     sc55_load_rom(sc55_t *s, int slot, const unsigned char *data, size_t len);

/* Call once every ROM the model needs is loaded. */
void    sc55_reset(sc55_t *s);

/* Queue MIDI bytes for the unit's serial input.  The queue holds
 * sc55_midi_room() more bytes; anything past that overwrites what has
 * not been read yet. */
void    sc55_midi(sc55_t *s, const unsigned char *data, size_t len);
size_t  sc55_midi_room(const sc55_t *s);

/* Non-zero once the firmware has enabled its MIDI input, which is when
 * the unit has finished starting up. */
int     sc55_ready(const sc55_t *s);

/* Native output rate in Hz: 66207 for the mkII, 64000 for the mk1. */
unsigned sc55_rate(const sc55_t *s);

/* Run the unit until it has produced `count` stereo frames and store
 * them in `frames` as left/right pairs.  Full scale is +-2^29. */
size_t  sc55_run(sc55_t *s, int32_t *frames, size_t count);

#endif
