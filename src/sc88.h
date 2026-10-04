/* Roland SC-88 / SC-88VL / SC-88Pro: the units' own firmware on an
 * emulated H8/510, with the XP sound chip and (SC-88Pro) the LSP
 * insertion-effect chip.  A C89 port of 88emu, part of gearmulator by
 * The Usual Suspects (https://github.com/dsp56300/gearmulator).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#ifndef SC88_H
#define SC88_H

#include <stddef.h>
#include <stdint.h>

typedef struct sc88 sc88_t;

enum
{
    SC88_MODEL_SC88,
    SC88_MODEL_SC88VL,
    SC88_MODEL_SC88PRO,     /* also the VE-GS Pro board, whose firmware takes serial MIDI */
    SC88_MODEL_COUNT
};

/* SC-88/VL: the control ROM (512 KB) and the four 2 MB wave ROMs
 * IC325..IC328.  SC-88Pro: the control ROM (1 MB) and the wave ROMs
 * R01567167 (8 MB), R01567178 (8 MB), R01233667 (4 MB). */
enum
{
    SC88_ROM_FIRMWARE,
    SC88_ROM_WAVE0,
    SC88_ROM_WAVE1,
    SC88_ROM_WAVE2,
    SC88_ROM_WAVE3,         /* SC-88/VL only */
    SC88_ROM_COUNT
};

sc88_t  *sc88_new(int model);
void     sc88_free(sc88_t *s);

/* Copies the data.  The control ROM may be in either byte order. */
int      sc88_load_rom(sc88_t *s, int slot, const unsigned char *data, size_t len);

/* In place: puts a control ROM dump in the CPU's byte order. */
void     sc88_normalize_firmware(unsigned char *data, size_t len);

/* Builds the unit from the loaded ROMs, which are released.  The first
 * ~11 s of frames run the panel's factory reset, silently; MIDI sent in
 * that time is held and given to the unit after it. */
void     sc88_reset(sc88_t *s);

/* Raw MIDI bytes, running status and SysEx included. */
void     sc88_midi(sc88_t *s, const unsigned char *data, size_t len);

/* Bit per sounding voice (voices from 31 up share bit 31). */
uint32_t sc88_voices(const sc88_t *s);

unsigned sc88_rate(const sc88_t *s);

/* Stereo frames of 24-bit words, full scale 2^23.  Returns count. */
size_t   sc88_run(sc88_t *s, int32_t *frames, size_t count);

#endif
