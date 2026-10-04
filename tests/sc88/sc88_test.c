/* Golden lane for src/sc88.c.
 *
 * Builds an SC-88 and an SC-88Pro from made-up ROMs: firmware that sets
 * up the interrupts, a timer and the sound chip's output, then loops
 * through accesses to every device on the board, and wave ROMs of noise.
 * Each unit runs its power-on factory reset, then two seconds with a
 * fixed run of MIDI (GS reset, programs, notes, controllers, a DT1).
 * The audio and the boot length must be what was recorded from the
 * build that matched 88emu's C++ sample for sample on these ROMs.
 *
 * The Makefile compiles sc88.c as strict C89 with warnings as errors,
 * so this lane is also the compile gate.  No ROMs are needed.
 *
 *   sc88_test           run the check
 *   sc88_test DIR       also write the ROMs and the MIDI run to DIR
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sc88.h"

int sc88_test_booting(const sc88_t *s);

#define FRAMES 64000

static unsigned long rng = 1;

static unsigned rnd(void)
{
    rng = (rng * 1103515245ul + 12345ul) & 0xfffffffful;
    return (unsigned)(rng >> 8) & 0xffffff;
}

static unsigned char *fw;
static size_t at;

static void put(int n, const unsigned char *b)
{
    memcpy(fw + at, b, (size_t)n);
    at += (size_t)n;
}

static const unsigned char setup_code[] =
{
    0x0C, 0xF0, 0x00, 0x87,
    0x04, 0x00, 0x8D, 0x15, 0xFF, 0x00, 0x06, 0x77, 0x15, 0xFF, 0x01, 0x06, 0x77, 0x15, 0xFF, 0x1D, 0x06, 0x0F,
    0x15, 0xFE, 0xA4, 0x06, 0x04, 0x15, 0xFE, 0xA5, 0x06, 0x00, 0x15, 0xFE, 0xA1, 0x06, 0x01, 0x15, 0xFE, 0xA0, 0x06, 0x20,
    0x0C, 0xF8, 0xFF, 0x58
};

static void put1(unsigned v) { fw[at++] = (unsigned char)v; }

/* MOV.W #v, @a on page `pg` (DP is set first) */
static void word_write(unsigned pg, unsigned a, unsigned v)
{
    put1(0x04); put1(pg); put1(0x8D);
    put1(0x1D); put1(a >> 8); put1(a & 0xff); put1(0x07); put1(v >> 8); put1(v & 0xff);
}

static void byte_write(unsigned pg, unsigned a, unsigned v)
{
    put1(0x04); put1(pg); put1(0x8D);
    put1(0x15); put1(a >> 8); put1(a & 0xff); put1(0x06); put1(v & 0xff);
}

static void byte_read(unsigned pg, unsigned a, unsigned reg)
{
    put1(0x04); put1(pg); put1(0x8D);
    put1(0x15); put1(a >> 8); put1(a & 0xff); put1(0x80 | reg);
}

/* register-only instructions, all valid */
static void reg_op(void)
{
    static const unsigned char ops[] = { 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80 };
    const unsigned rs = rnd() % 6, rd = rnd() % 6;
    switch (rnd() % 4)
    {
        case 0:  put1(0xA8 | rs); put1(ops[rnd() % 7] | rd); break;          /* word ALU */
        case 1:  put1(0xA0 | rs); put1(ops[rnd() % 7] | rd); break;          /* byte ALU */
        case 2:  put1(0xA8 | rs); put1(0x16); break;                          /* TST.W */
        default: put1(0xA8 | rs); put1(0x18 + rnd() % 8); break;             /* shift / rotate */
    }
}

static void device_access(int pro, int handler)
{
    const unsigned xp = pro ? 0xC8 : 0x0E, ram = pro ? 0xC0 : 0x08;
    unsigned t = rnd() % 8;
    if (handler || t == 0)
    {
        /* acknowledge: timer flags, gate array, sub-MCU command, sound chip */
        byte_read(0x00, 0xFEA1, 3);
        byte_write(0x00, 0xFEA1, 0x01);
        if (pro)
        {
            byte_read(0xEF, 0xC104, 0);
            byte_read(0xE0, 0x00DC, 1);
        }
        else
        {
            byte_read(0x0F, 0xC104, 0);
            byte_read(0x0F, 0x00DC, 1);
        }
        put1(0x04); put1(xp); put1(0x8D); put1(0x1D); put1(0x39); put1(0x1A); put1(0x82);
        if (handler)
            return;
    }
    if (t <= 3)
    {
        unsigned a, v = rnd() & 0xffff;
        switch (rnd() % 8)
        {
            case 0:  a = 0x3916; v = 0x0007; break;
            case 1:  a = 0x3440 + (rnd() % 0x220) * 2; if (!(a & 2) && (rnd() & 1)) v = (v & ~0x0E00u) | 0x0400; break;
            case 2:  a = 0x3900 + (rnd() & 6); v = (rnd() & 1) ? 0xFFFF : v; break;
            case 3:  a = 0x3a00 + (rnd() & 0x180) + (rnd() & 0x7e); v = (v & 0xFFC0) | (rnd() & 15); break;
            case 4:  a = ((rnd() & 1) ? 0x3000 : 0x3100) + (rnd() % 0x10) * 2; break;
            case 5:  a = 0x2c00 + (rnd() % 0x120) * 2; break;
            default: a = (rnd() % 0x2c00) & ~1u; break;
        }
        word_write(xp, a, v);
        if (a >= 0x3900 && a < 0x3908)
        {
            put1(0x1D); put1(a >> 8); put1(a & 0xff); put1(0x83);   /* a read commits releases */
        }
        put1(0x04); put1(ram); put1(0x8D);
        return;
    }
    if (t == 4)
    {
        if (pro && (rnd() & 1))
        {
            const unsigned addr = (rnd() & 1) ? 0x80 + rnd() % 384 : rnd() & 0x7f;
            byte_write(0xF0, 2, rnd()); byte_write(0xF0, 3, rnd()); byte_write(0xF0, 4, rnd());
            byte_write(0xF0, 1, addr >> 8); byte_write(0xF0, 0, addr);
            if ((rnd() & 15) == 0)
            {
                byte_write(0xF0, 2, 1); byte_write(0xF0, 3, 0); byte_write(0xF0, 6, 0);
            }
        }
        else if (pro)
            byte_write(0xEF, 0xC100 + rnd() % 0x2d, rnd());
        else
            byte_write(0x0F, (rnd() & 1) ? 0xC100 + rnd() % 0x2d : rnd() & 0xff, rnd());
        return;
    }
    if (t == 5)
    {
        put1(0x04); put1(ram); put1(0x8D);
        put1(0x1D); put1(0x80 | (rnd() & 0x7f)); put1(rnd() & 0xfe); put1(((rnd() & 1) ? 0x80 : 0x90) | (rnd() % 6));
        return;
    }
    /* on-chip I/O through BR=FE: reads, and writes that leave FRT1 alone */
    {
        const int store = rnd() & 1;
        unsigned lo = 0x80 + (rnd() & 0x7f);
        if (store && lo >= 0xA0 && lo < 0xB0)
            lo += 0x10;
        put1(0x04); put1(0xFE); put1(0x8B); put1((store ? 0x70 : 0x60) | (rnd() % 6)); put1(lo);
    }
}

static void make_firmware(int pro, size_t size)
{
    const unsigned ram = pro ? 0xC0 : 0x00, xp = pro ? 0xC8 : 0x0E;
    size_t loop, h, v;
    unsigned k;
    memset(fw, 0, size);
    /* setup at 0x200: stack in RAM, interrupt priorities and enables,
     * FRT1 compare-match A as a wake-up timer, unmask */
    at = 0x200;
    put1(0x04); put1(ram); put1(0x8F);
    put((int)sizeof(setup_code), setup_code);
    /* the sound chip: DSP on, serial out on, the first 16 program slots
     * send processing RAM words 0..15 out */
    word_write(xp, 0x3916, 0x0007);
    word_write(xp, 0x3926, 0xC0C0);
    word_write(xp, 0x3924, 0xC0C0);
    for (k = 0; k < 16; k++)
    {
        word_write(xp, 0x3400 + k * 4, 0x0400);
        word_write(xp, 0x3402 + k * 4, 0x0000);
    }
    put1(0x10); put1(0x10); put1(0x00);     /* JMP 1000 */
    at = 0x1000;
    loop = at;
    while (at < 0x7000)
    {
        const unsigned t = rnd() % 10;
        if (t < 4)
            device_access(pro, 0);
        else if (t == 4 && rnd() % 40 == 0)
            put1(0x1A);                     /* SLEEP */
        else
            reg_op();
    }
    put1(0x10); put1(loop >> 8); put1(loop & 0xff);
    /* one handler per vector, below 0x1000 as a real ROM's are */
    h = 0x400;
    fw[2] = 0x02; fw[3] = 0x00;
    for (v = 4; v < 0x100; v += 4)
    {
        const size_t entry = h;
        at = h;
        device_access(pro, 1);
        put1(0x0A);                         /* RTE */
        h = at;
        fw[v + 2] = (unsigned char)(entry >> 8);
        fw[v + 3] = (unsigned char)entry;
    }
}

/* The MIDI run: frame, then the bytes. */
static const unsigned char midi_run[] =
{
    0, 0, 11, 0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7,
    0, 200, 2, 0xC0, 19,
    0, 210, 3, 0x90, 60, 100,
    0, 220, 3, 0x91, 64, 90,
    3, 0, 3, 0xB0, 7, 110,
    8, 0, 3, 0xE0, 0, 80,
    16, 0, 11, 0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x01, 0x30, 0x04, 0x0B, 0xF7,
    48, 0, 3, 0x80, 60, 0,
    96, 0, 3, 0x81, 64, 0,
    120, 0, 3, 0x99, 36, 127
};

static unsigned long long run_unit(int model, const char *dir)
{
    const int pro = model == SC88_MODEL_SC88PRO;
    const size_t fw_size = pro ? 0x100000 : 0x80000;
    const size_t wave_sizes[2][4] = { { 0x200000, 0x200000, 0x200000, 0x200000 }, { 0x800000, 0x800000, 0x400000, 0 } };
    unsigned long long h = 0xcbf29ce484222325ull;
    unsigned char *wave[4];
    int32_t frame[2];
    sc88_t *s;
    long boot = 0, f;
    size_t p = 0, i;
    int k;

    fw = (unsigned char*)malloc(fw_size);
    make_firmware(pro, fw_size);
    for (k = 0; k < 4; k++)
    {
        const size_t n = wave_sizes[pro][k];
        wave[k] = n ? (unsigned char*)malloc(n) : NULL;
        for (i = 0; i < n; i++)
            wave[k][i] = (unsigned char)(rnd() >> 4);
    }
    if (dir)
    {
        char path[1024];
        FILE *o;
        sprintf(path, "%s/%s_control.bin", dir, pro ? "pro" : "sc88");
        o = fopen(path, "wb"); fwrite(fw, 1, fw_size, o); fclose(o);
        for (k = 0; k < 4; k++)
            if (wave[k])
            {
                sprintf(path, "%s/%s_wave%d.bin", dir, pro ? "pro" : "sc88", k);
                o = fopen(path, "wb"); fwrite(wave[k], 1, wave_sizes[pro][k], o); fclose(o);
            }
    }

    s = sc88_new(model);
    sc88_load_rom(s, SC88_ROM_FIRMWARE, fw, fw_size);
    for (k = 0; k < 4; k++)
        if (wave[k])
            sc88_load_rom(s, SC88_ROM_WAVE0 + k, wave[k], wave_sizes[pro][k]);
    sc88_reset(s);
    while (sc88_test_booting(s))
    {
        sc88_run(s, frame, 1);
        boot++;
        if (frame[0] || frame[1])
            break;
    }
    for (f = 0; f < FRAMES; f++)
    {
        while (p < sizeof(midi_run) && ((long)midi_run[p] << 8 | midi_run[p + 1]) * 16 == f)
        {
            sc88_midi(s, midi_run + p + 3, midi_run[p + 2]);
            p += 3 + midi_run[p + 2];
        }
        sc88_run(s, frame, 1);
        for (k = 0; k < 2; k++)
        {
            h ^= (unsigned long long)(uint32_t)frame[k];
            h *= 0x100000001b3ull;
        }
    }
    printf("%s: factory reset %ld frames, audio %016llx\n", pro ? "SC-88Pro" : "SC-88", boot, h);
    sc88_free(s);
    free(fw);
    for (k = 0; k < 4; k++)
        free(wave[k]);
    return h ^ (unsigned long long)boot;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : NULL;
    const unsigned long long a = run_unit(SC88_MODEL_SC88, dir);
    const unsigned long long b = run_unit(SC88_MODEL_SC88PRO, dir);
    const unsigned long long want_a = SC88_GOLD_A, want_b = SC88_GOLD_B;
    if (a != want_a || b != want_b)
    {
        printf("FAIL: expected %016llx %016llx, got %016llx %016llx\n", want_a, want_b, a, b);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
