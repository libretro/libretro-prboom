/* See end of file for license */

#ifndef POCKETMOD_H_INCLUDED
#define POCKETMOD_H_INCLUDED

#ifdef __cplusplus
extern "C" {
#endif

/* Portable 64-bit integer types for the fixed-point mixer.  This decoder is
 * integer-only end-to-end: sample positions, pitch increments, tick timing and
 * the mix accumulator are all fixed-point, and pocketmod_render() emits signed
 * 16-bit interleaved stereo directly.  No float is used anywhere, so the output
 * is bit-identical across compilers and architectures (no x87/SSE rounding
 * divergence, no -ffast-math sensitivity).  MSVC C89 has no <stdint.h>, so use
 * __int64 there and the long-long extension elsewhere. */
#if defined(_MSC_VER)
typedef signed   __int64 pm_s64;
typedef unsigned __int64 pm_u64;
#else
typedef signed   long long pm_s64;
typedef unsigned long long pm_u64;
#endif

typedef struct pocketmod_context pocketmod_context;
int pocketmod_init(pocketmod_context *c, const void *data, int size, int rate);
int pocketmod_render(pocketmod_context *c, void *buffer, int size);
int pocketmod_loop_count(pocketmod_context *c);

#ifndef POCKETMOD_MAX_CHANNELS
#define POCKETMOD_MAX_CHANNELS 32
#endif

#ifndef POCKETMOD_MAX_SAMPLES
#define POCKETMOD_MAX_SAMPLES 31
#endif

typedef struct {
    signed char *data;          /* Sample data buffer                      */
    unsigned int length;        /* Data length (in bytes)                  */
} _pocketmod_sample;

typedef struct {
    unsigned char dirty;        /* Pitch/volume dirty flags                */
    unsigned char sample;       /* Sample number (0..31)                   */
    unsigned char volume;       /* Base volume without tremolo (0..64)     */
    unsigned char balance;      /* Stereo balance (0..255)                 */
    unsigned short period;      /* Note period (113..856)                  */
    unsigned short delayed;     /* Delayed note period (113..856)          */
    unsigned short target;      /* Target period (for tone portamento)     */
    unsigned char finetune;     /* Note finetune (0..15)                   */
    unsigned char loop_count;   /* E6x loop counter                        */
    unsigned char loop_line;    /* E6x target line                         */
    unsigned char lfo_step;     /* Vibrato/tremolo LFO step counter        */
    unsigned char lfo_type[2];  /* LFO type for vibrato/tremolo            */
    unsigned char effect;       /* Current effect (0x0..0xf or 0xe0..0xef) */
    unsigned char param;        /* Raw effect parameter value              */
    unsigned char param3;       /* Parameter memory for 3xx                */
    unsigned char param4;       /* Parameter memory for 4xy                */
    unsigned char param7;       /* Parameter memory for 7xy                */
    unsigned char param9;       /* Parameter memory for 9xx                */
    unsigned char paramE1;      /* Parameter memory for E1x                */
    unsigned char paramE2;      /* Parameter memory for E2x                */
    unsigned char paramEA;      /* Parameter memory for EAx                */
    unsigned char paramEB;      /* Parameter memory for EBx                */
    unsigned char real_volume;  /* Volume (with tremolo adjustment)        */
    pm_s64 position;            /* Position in sample data (Q32.32, <0=off) */
    pm_s64 increment;          /* Position increment per output (Q32.32)   */
} _pocketmod_chan;

struct pocketmod_context
{
    /* Read-only song data */
    _pocketmod_sample samples[POCKETMOD_MAX_SAMPLES];
    unsigned char *source;      /* Pointer to source MOD data              */
    unsigned char *order;       /* Pattern order table                     */
    unsigned char *patterns;    /* Start of pattern data                   */
    unsigned char length;       /* Patterns in the order (1..128)          */
    unsigned char reset;        /* Pattern to loop back to (0..127)        */
    unsigned char num_patterns; /* Patterns in the file (1..128)           */
    unsigned char num_samples;  /* Sample count (15 or 31)                 */
    unsigned char num_channels; /* Channel count (1..32)                   */

    /* Timing variables */
    int samples_per_second;     /* Sample rate (set by user)               */
    int ticks_per_line;         /* A.K.A. song speed (initially 6)         */
    pm_s64 samples_per_tick;    /* Depends on sample rate and BPM (Q32.32) */
    pm_u64 clock;               /* 3546894.6 * 2^32 / rate (per-song const) */
    int out_gain;               /* Music volume 0..15 (folded pre-clamp)   */

    /* Loop detection state */
    unsigned char visited[16];  /* Bit mask of previously visited patterns */
    int loop_count;             /* How many times the song has looped      */

    /* Render state */
    _pocketmod_chan channels[POCKETMOD_MAX_CHANNELS];
    unsigned char pattern_delay;/* EEx pattern delay counter               */
    unsigned int lfo_rng;       /* RNG used for the random LFO waveform    */

    /* Position in song (from least to most granular) */
    signed char pattern;        /* Current pattern in order                */
    signed char line;           /* Current line in pattern                 */
    short tick;                 /* Current tick in line                    */
    pm_s64 sample;              /* Current sample in tick (Q32.32)         */
};

#ifdef POCKETMOD_IMPLEMENTATION

/* Memorize a parameter unless the new value is zero */
#define POCKETMOD_MEM(dst, src) do { \
        (dst) = (src) ? (src) : (dst); \
    } while (0)

/* Same thing, but memorize each nibble separately */
#define POCKETMOD_MEM2(dst, src) do { \
        (dst) = (((src) & 0x0f) ? ((src) & 0x0f) : ((dst) & 0x0f)) \
              | (((src) & 0xf0) ? ((src) & 0xf0) : ((dst) & 0xf0)); \
    } while (0)

/* Shortcut to sample metadata (sample must be nonzero) */
#define POCKETMOD_SAMPLE(c, sample) ((c)->source + 12 + 30 * (sample))

/* Channel dirty flags */
#define POCKETMOD_PITCH  0x01
#define POCKETMOD_VOLUME 0x02

/* The size of one output sample in bytes (signed 16-bit interleaved stereo) */
#define POCKETMOD_SAMPLE_SIZE sizeof(short[2])

/* Fixed-point configuration.
 * - Sample position / pitch increment live in Q32.32 (PM_POS_FRAC = 32).
 * - Note periods are computed in Q16.16 (PM_PER_FRAC = 16) so vibrato and
 *   arpeggio retain sub-period precision before deriving the increment.
 * - The interpolation weight uses the top PM_INTERP_BITS of the position
 *   fraction; the per-channel mix accumulates in 64-bit and is scaled to s16
 *   once per frame, so there is ample headroom for up to 32 channels. */
#define PM_POS_FRAC    32
#define PM_PER_FRAC    16
#define PM_INTERP_BITS 16
/* acc / PM_OUT_DIV maps the full-scale mix back to the s16 range.  Derivation:
 *   out = volume/(128*64*4) * level * sample * out_gain/15 * 32768
 * with sample carried as (sample<<PM_INTERP_BITS) and gain folding
 * real_volume*(255-balance)*out_gain (left) the 32768 cancels 128*64*4=32768,
 * leaving 255 * 2^PM_INTERP_BITS * 15 = 250675200. */
#define PM_OUT_DIV ((pm_s64)255 * (1 << PM_INTERP_BITS) * 15)

/* Frames mixed per chunk before scaling to s16.  pocketmod splits a tick at
 * chunk boundaries, so for the output to match the original decoder the chunk
 * must be >= the caller's render burst (modplayer uses 1024-frame bursts);
 * then this cap never binds below samples_remaining and the tick-split points
 * are identical to the pre-rewrite float path.  (The split itself is inherent
 * to pocketmod and was buffer-size dependent before this change too.) */
#define PM_CHUNK 1024

/* round(2^(x/12) * 2^PM_PER_FRAC) for x in 0..15 -- the arpeggio ratios as
 * Q16.16 fixed-point divisors (entry 0 == 1.0 exactly). */
static const int _pocketmod_arp[16] = {
    65536, 69433, 73562, 77936,
    82570, 87480, 92682, 98193,
    104032, 110218, 116772, 123715,
    131072, 138866, 147123, 155872
};

/* Finetune adjustment table. Three octaves for each finetune setting. */
static const signed char _pocketmod_finetune[16][36] = {
    {  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0},
    { -6, -6, -5, -5, -4, -3, -3, -3, -3, -3, -3, -3, -3, -3, -2, -3, -2, -2, -2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,  0},
    {-12,-12,-10,-11, -8, -8, -7, -7, -6, -6, -6, -6, -6, -6, -5, -5, -4, -4, -4, -3, -3, -3, -3, -2, -3, -3, -2, -3, -3, -2, -2, -2, -2, -2, -2, -1},
    {-18,-17,-16,-16,-13,-12,-12,-11,-10,-10,-10, -9, -9, -9, -8, -8, -7, -6, -6, -5, -5, -5, -5, -4, -5, -4, -3, -4, -4, -3, -3, -3, -3, -2, -2, -2},
    {-24,-23,-21,-21,-18,-17,-16,-15,-14,-13,-13,-12,-12,-12,-11,-10, -9, -8, -8, -7, -7, -7, -7, -6, -6, -6, -5, -5, -5, -4, -4, -4, -4, -3, -3, -3},
    {-30,-29,-26,-26,-23,-21,-20,-19,-18,-17,-17,-16,-15,-14,-13,-13,-11,-11,-10, -9, -9, -9, -8, -7, -8, -7, -6, -6, -6, -5, -5, -5, -5, -4, -4, -4},
    {-36,-34,-32,-31,-27,-26,-24,-23,-22,-21,-20,-19,-18,-17,-16,-15,-14,-13,-12,-11,-11,-10,-10, -9, -9, -9, -7, -8, -7, -6, -6, -6, -6, -5, -5, -4},
    {-42,-40,-37,-36,-32,-30,-29,-27,-25,-24,-23,-22,-21,-20,-18,-18,-16,-15,-14,-13,-13,-12,-12,-10,-10,-10, -9, -9, -9, -8, -7, -7, -7, -6, -6, -5},
    { 51, 48, 46, 42, 42, 38, 36, 34, 32, 30, 24, 27, 25, 24, 23, 21, 21, 19, 18, 17, 16, 15, 14, 14, 12, 12, 12, 10, 10, 10,  9,  8,  8,  8,  7,  7},
    { 44, 42, 40, 37, 37, 35, 32, 31, 29, 27, 25, 24, 22, 21, 20, 19, 18, 17, 16, 15, 15, 14, 13, 12, 11, 10, 10,  9,  9,  9,  8,  7,  7,  7,  6,  6},
    { 38, 36, 34, 32, 31, 30, 28, 27, 25, 24, 22, 21, 19, 18, 17, 16, 16, 15, 14, 13, 13, 12, 11, 11,  9,  9,  9,  8,  7,  7,  7,  6,  6,  6,  5,  5},
    { 31, 30, 29, 26, 26, 25, 24, 22, 21, 20, 18, 17, 16, 15, 14, 13, 13, 12, 12, 11, 11, 10,  9,  9,  8,  7,  8,  7,  6,  6,  6,  5,  5,  5,  5,  5},
    { 25, 24, 23, 21, 21, 20, 19, 18, 17, 16, 14, 14, 13, 12, 11, 10, 11, 10, 10,  9,  9,  8,  7,  7,  6,  6,  6,  5,  5,  5,  5,  4,  4,  4,  3,  4},
    { 19, 18, 17, 16, 16, 15, 15, 14, 13, 12, 11, 10,  9,  9,  9,  8,  8, 18,  7,  7,  7,  6,  5,  6,  5,  4,  5,  4,  4,  4,  4,  3,  3,  3,  3,  3},
    { 12, 12, 12, 10, 11, 11, 10, 10,  9,  8,  7,  7,  6,  6,  6,  5,  6,  5,  5,  5,  5,  4,  4,  4,  3,  3,  3,  3,  2,  3,  3,  2,  2,  2,  2,  2},
    {  6,  6,  6,  5,  6,  6,  6,  5,  5,  5,  4,  4,  3,  3,  3,  3,  3,  3,  3,  3,  3,  2,  2,  2,  2,  1,  2,  1,  1,  1,  1,  1,  1,  1,  1,  1}
};

/* Min/max helper functions */
static int _pocketmod_min(int x, int y) { return x < y ? x : y; }
static int _pocketmod_max(int x, int y) { return x > y ? x : y; }

/* Clamp a volume value to the 0..64 range */
static int _pocketmod_clamp_volume(int x)
{
    x = _pocketmod_max(x, 0x00);
    x = _pocketmod_min(x, 0x40);
    return x;
}

/* Zero out a block of memory */
static void _pocketmod_zero(void *data, int size)
{
    char *byte = data, *end = byte + size;
    while (byte != end) { *byte++ = 0; }
}

/* Convert a period (at finetune = 0) to a note index in 0..35 */
static int _pocketmod_period_to_note(int period)
{
    switch (period) {
        case 856: return  0; case 808: return  1; case 762: return  2;
        case 720: return  3; case 678: return  4; case 640: return  5;
        case 604: return  6; case 570: return  7; case 538: return  8;
        case 508: return  9; case 480: return 10; case 453: return 11;
        case 428: return 12; case 404: return 13; case 381: return 14;
        case 360: return 15; case 339: return 16; case 320: return 17;
        case 302: return 18; case 285: return 19; case 269: return 20;
        case 254: return 21; case 240: return 22; case 226: return 23;
        case 214: return 24; case 202: return 25; case 190: return 26;
        case 180: return 27; case 170: return 28; case 160: return 29;
        case 151: return 30; case 143: return 31; case 135: return 32;
        case 127: return 33; case 120: return 34; case 113: return 35;
        default: return 0;
    }
}

/* Table-based sine wave oscillator */
static int _pocketmod_sin(int step)
{
    /* round(sin(x * pi / 32) * 255) for x in 0..15 */
    static const unsigned char sin[16] = {
        0x00, 0x19, 0x32, 0x4a, 0x62, 0x78, 0x8e, 0xa2,
        0xb4, 0xc5, 0xd4, 0xe0, 0xec, 0xf4, 0xfa, 0xfe
    };
    int x = sin[step & 0x0f];
    x = (step & 0x1f) < 0x10 ? x : 0xff - x;
    return step < 0x20 ? x : -x;
}

/* Oscillators for vibrato/tremolo effects */
static int _pocketmod_lfo(pocketmod_context *c, _pocketmod_chan *ch, int step)
{
    switch (ch->lfo_type[ch->effect == 7] & 3) {
        case 0: return _pocketmod_sin(step & 0x3f);         /* Sine   */
        case 1: return 0xff - ((step & 0x3f) << 3);         /* Saw    */
        case 2: return (step & 0x3f) < 0x20 ? 0xff : -0xff; /* Square */
        case 3: return (c->lfo_rng & 0x1ff) - 0xff;         /* Random */
        default: return 0; /* Hush little compiler */
    }
}

static void _pocketmod_update_pitch(pocketmod_context *c, _pocketmod_chan *ch)
{
    /* Don't do anything if the period is zero */
    ch->increment = 0;
    if (ch->period) {
        pm_s64 period = (pm_s64) ch->period << PM_PER_FRAC; /* Q16.16 */

        /* Apply vibrato (if active) */
        if (ch->effect == 0x4 || ch->effect == 0x6) {
            int step = (ch->param4 >> 4) * ch->lfo_step;
            int rate = ch->param4 & 0x0f;
            /* period += lfo*rate/128 (in Q16.16) */
            period += (((pm_s64)_pocketmod_lfo(c, ch, step) * rate) << PM_PER_FRAC) / 128;

        /* Apply arpeggio (if active) */
        } else if (ch->effect == 0x0 && ch->param) {
            int step = (ch->param >> ((2 - c->tick % 3) << 2)) & 0x0f;
            /* period /= 2^(step/12), divisor in Q16.16 */
            period = (period << PM_PER_FRAC) / _pocketmod_arp[step];
        }

        /* Calculate sample buffer position increment (Q32.32):
         *   increment = 3546894.6 / (period_real * rate)
         * c->clock = 3546894.6 * 2^32 / rate (precomputed at init), and
         * period = period_real * 2^PM_PER_FRAC, so
         *   increment = (c->clock << PM_PER_FRAC) / period. */
        if (period > 0) {
            ch->increment = (pm_s64)((c->clock << PM_PER_FRAC) / (pm_u64)period);
        }
    }

    /* Clear the pitch dirty flag */
    ch->dirty &= ~POCKETMOD_PITCH;
}

static void _pocketmod_update_volume(pocketmod_context *c, _pocketmod_chan *ch)
{
    int volume = ch->volume;
    if (ch->effect == 0x7) {
        int step = ch->lfo_step * (ch->param7 >> 4);
        volume += _pocketmod_lfo(c, ch, step) * (ch->param7 & 0x0f) >> 6;
    }
    ch->real_volume = _pocketmod_clamp_volume(volume);
    ch->dirty &= ~POCKETMOD_VOLUME;
}

static void _pocketmod_pitch_slide(_pocketmod_chan *ch, int amount)
{
    int max = 856 + _pocketmod_finetune[ch->finetune][ 0];
    int min = 113 + _pocketmod_finetune[ch->finetune][35];
    ch->period += amount;
    ch->period = _pocketmod_max(ch->period, min);
    ch->period = _pocketmod_min(ch->period, max);
    ch->dirty |= POCKETMOD_PITCH;
}

static void _pocketmod_volume_slide(_pocketmod_chan *ch, int param)
{
    /* Undocumented quirk: If both x and y are nonzero, then the value of x */
    /* takes precedence. (Yes, there are songs that rely on this behavior.) */
    int change = (param & 0xf0) ? (param >> 4) : -(param & 0x0f);
    ch->volume = _pocketmod_clamp_volume(ch->volume + change);
    ch->dirty |= POCKETMOD_VOLUME;
}

static void _pocketmod_next_line(pocketmod_context *c)
{
    unsigned char (*data)[4];
    int i, pos, pattern_break = -1;

    /* When entering a new pattern order index, mark it as "visited" */
    if (c->line == 0) {
        c->visited[c->pattern >> 3] |= 1 << (c->pattern & 7);
    }

    /* Move to the next pattern if this was the last line */
    if (++c->line == 64) {
        if (++c->pattern == c->length) {
            c->pattern = c->reset;
        }
        c->line = 0;
    }

    /* Find the pattern data for the current line */
    pos = (c->order[c->pattern] * 64 + c->line) * c->num_channels * 4;
    data = (unsigned char(*)[4]) (c->patterns + pos);
    for (i = 0; i < c->num_channels; i++) {

        /* Decode columns */
        int sample = (data[i][0] & 0xf0) | (data[i][2] >> 4);
        int period = ((data[i][0] & 0x0f) << 8) | data[i][1];
        int effect = ((data[i][2] & 0x0f) << 8) | data[i][3];

        /* Memorize effect parameter values */
        _pocketmod_chan *ch = &c->channels[i];
        ch->effect = (effect >> 8) != 0xe ? (effect >> 8) : (effect >> 4);
        ch->param = (effect >> 8) != 0xe ? (effect & 0xff) : (effect & 0x0f);

        /* Set sample */
        if (sample) {
            if (sample <= POCKETMOD_MAX_SAMPLES) {
                unsigned char *sample_data = POCKETMOD_SAMPLE(c, sample);
                ch->sample = sample;
                ch->finetune = sample_data[2] & 0x0f;
                ch->volume = _pocketmod_min(sample_data[3], 0x40);
                if (ch->effect != 0xED) {
                    ch->dirty |= POCKETMOD_VOLUME;
                }
            } else {
                ch->sample = 0;
            }
        }

        /* Set note */
        if (period) {
            int note = _pocketmod_period_to_note(period);
            period += _pocketmod_finetune[ch->finetune][note];
            if (ch->effect != 0x3) {
                if (ch->effect != 0xED) {
                    ch->period = period;
                    ch->dirty |= POCKETMOD_PITCH;
                    ch->position = 0;
                    ch->lfo_step = 0;
                } else {
                    ch->delayed = period;
                }
            }
        }

        /* Handle pattern effects */
        switch (ch->effect) {

            /* Memorize parameters */
            case 0x3: POCKETMOD_MEM(ch->param3, ch->param); /* Fall through */
            case 0x5: POCKETMOD_MEM(ch->target, period); break;
            case 0x4: POCKETMOD_MEM2(ch->param4, ch->param); break;
            case 0x7: POCKETMOD_MEM2(ch->param7, ch->param); break;
            case 0xE1: POCKETMOD_MEM(ch->paramE1, ch->param); break;
            case 0xE2: POCKETMOD_MEM(ch->paramE2, ch->param); break;
            case 0xEA: POCKETMOD_MEM(ch->paramEA, ch->param); break;
            case 0xEB: POCKETMOD_MEM(ch->paramEB, ch->param); break;

            /* 8xx: Set stereo balance (nonstandard) */
            case 0x8: {
                ch->balance = ch->param;
            } break;

            /* 9xx: Set sample offset */
            case 0x9: {
                if (period != 0 || sample != 0) {
                    ch->param9 = ch->param ? ch->param : ch->param9;
                    ch->position = (pm_s64)(ch->param9 << 8) << PM_POS_FRAC;
                }
            } break;

            /* Bxx: Jump to pattern */
            case 0xB: {
                c->pattern = ch->param < c->length ? ch->param : 0;
                c->line = -1;
            } break;

            /* Cxx: Set volume */
            case 0xC: {
                ch->volume = _pocketmod_clamp_volume(ch->param);
                ch->dirty |= POCKETMOD_VOLUME;
            } break;

            /* Dxy: Pattern break */
            case 0xD: {
                pattern_break = (ch->param >> 4) * 10 + (ch->param & 15);
            } break;

            /* E4x: Set vibrato waveform */
            case 0xE4: {
                ch->lfo_type[0] = ch->param;
            } break;

            /* E5x: Set sample finetune */
            case 0xE5: {
                ch->finetune = ch->param;
                ch->dirty |= POCKETMOD_PITCH;
            } break;

            /* E6x: Pattern loop */
            case 0xE6: {
                if (ch->param) {
                    if (!ch->loop_count) {
                        ch->loop_count = ch->param;
                        c->line = ch->loop_line;
                    } else if (--ch->loop_count) {
                        c->line = ch->loop_line;
                    }
                } else {
                    ch->loop_line = c->line - 1;
                }
            } break;

            /* E7x: Set tremolo waveform */
            case 0xE7: {
                ch->lfo_type[1] = ch->param;
            } break;

            /* E8x: Set stereo balance (nonstandard) */
            case 0xE8: {
                ch->balance = ch->param << 4;
            } break;

            /* EEx: Pattern delay */
            case 0xEE: {
                c->pattern_delay = ch->param;
            } break;

            /* Fxx: Set speed */
            case 0xF: {
                if (ch->param != 0) {
                    if (ch->param < 0x20) {
                        c->ticks_per_line = ch->param;
                    } else {
                        /* samples_per_tick = rate / (0.4 * bpm)
                         *                  = rate * 5 / (2 * bpm)   (Q32.32) */
                        c->samples_per_tick =
                            (((pm_s64)c->samples_per_second * 5) << PM_POS_FRAC)
                            / ((pm_s64)2 * ch->param);
                    }
                }
            } break;

            default: break;
        }
    }

    /* Pattern breaks are handled here, so that only one jump happens even  */
    /* when multiple Dxy commands appear on the same line. (You guessed it: */
    /* There are songs that rely on this behavior!)                         */
    if (pattern_break != -1) {
        c->line = (pattern_break < 64 ? pattern_break : 0) - 1;
        if (++c->pattern == c->length) {
            c->pattern = c->reset;
        }
    }
}

static void _pocketmod_next_tick(pocketmod_context *c)
{
    int i;

    /* Move to the next line if this was the last tick */
    if (++c->tick == c->ticks_per_line) {
        if (c->pattern_delay > 0) {
            c->pattern_delay--;
        } else {
            _pocketmod_next_line(c);
        }
        c->tick = 0;
    }

    /* Make per-tick adjustments for all channels */
    for (i = 0; i < c->num_channels; i++) {
        _pocketmod_chan *ch = &c->channels[i];
        int param = ch->param;

        /* Advance the LFO random number generator */
        c->lfo_rng = 0x0019660d * c->lfo_rng + 0x3c6ef35f;

        /* Handle effects that may happen on any tick of a line */
        switch (ch->effect) {

            /* 0xy: Arpeggio */
            case 0x0: {
                ch->dirty |= POCKETMOD_PITCH;
            } break;

            /* E9x: Retrigger note every x ticks */
            case 0xE9: {
                if (!(param && c->tick % param)) {
                    ch->position = 0;
                    ch->lfo_step = 0;
                }
            } break;

            /* ECx: Cut note after x ticks */
            case 0xEC: {
                if (c->tick == param) {
                    ch->volume = 0;
                    ch->dirty |= POCKETMOD_VOLUME;
                }
            } break;

            /* EDx: Delay note for x ticks */
            case 0xED: {
                if (c->tick == param && ch->sample) {
                    ch->dirty |= POCKETMOD_VOLUME | POCKETMOD_PITCH;
                    ch->period = ch->delayed;
                    ch->position = 0;
                    ch->lfo_step = 0;
                }
            } break;

            default: break;
        }

        /* Handle effects that only happen on the first tick of a line */
        if (c->tick == 0) {
            switch (ch->effect) {
                case 0xE1: _pocketmod_pitch_slide(ch, -ch->paramE1); break;
                case 0xE2: _pocketmod_pitch_slide(ch, +ch->paramE2); break;
                case 0xEA: _pocketmod_volume_slide(ch, ch->paramEA << 4); break;
                case 0xEB: _pocketmod_volume_slide(ch, ch->paramEB & 15); break;
                default: break;
            }

        /* Handle effects that are not applied on the first tick of a line */
        } else {
            switch (ch->effect) {

                /* 1xx: Portamento up */
                case 0x1: {
                    _pocketmod_pitch_slide(ch, -param);
                } break;

                /* 2xx: Portamento down */
                case 0x2: {
                    _pocketmod_pitch_slide(ch, +param);
                } break;

                /* 5xy: Volume slide + tone portamento */
                case 0x5: {
                    _pocketmod_volume_slide(ch, param);
                } /* Fall through */

                /* 3xx: Tone portamento */
                case 0x3: {
                    int rate = ch->param3;
                    int order = ch->period < ch->target;
                    int closer = ch->period + (order ? rate : -rate);
                    int new_order = closer < ch->target;
                    ch->period = new_order == order ? closer : ch->target;
                    ch->dirty |= POCKETMOD_PITCH;
                } break;

                /* 6xy: Volume slide + vibrato */
                case 0x6: {
                    _pocketmod_volume_slide(ch, param);
                } /* Fall through */

                /* 4xy: Vibrato */
                case 0x4: {
                    ch->lfo_step++;
                    ch->dirty |= POCKETMOD_PITCH;
                } break;

                /* 7xy: Tremolo */
                case 0x7: {
                    ch->lfo_step++;
                    ch->dirty |= POCKETMOD_VOLUME;
                } break;

                /* Axy: Volume slide */
                case 0xA: {
                    _pocketmod_volume_slide(ch, param);
                } break;

                default: break;
            }
        }

        /* Update channel volume/pitch if either is out of date */
        if (ch->dirty & POCKETMOD_VOLUME) { _pocketmod_update_volume(c, ch); }
        if (ch->dirty & POCKETMOD_PITCH) { _pocketmod_update_pitch(c, ch); }
    }
}

static void _pocketmod_render_channel(pocketmod_context *c,
                                      _pocketmod_chan *chan,
                                      pm_s64 *output,
                                      int samples_to_write)
{
    /* Gather some loop data */
    _pocketmod_sample *sample = &c->samples[chan->sample - 1];
    unsigned char *data = POCKETMOD_SAMPLE(c, chan->sample);
    const int loop_start = ((data[4] << 8) | data[5]) << 1;
    const int loop_length = ((data[6] << 8) | data[7]) << 1;
    const int loop_end = loop_length > 2 ? loop_start + loop_length : 0xffffff;
    const int sample_end = 1 + _pocketmod_min(loop_end, (int)sample->length);
    const pm_s64 end_pos = (pm_s64) sample_end << PM_POS_FRAC;

    /* Per-channel integer gains.  Folds the music volume (out_gain, 0..15) in
     * before the final clamp, exactly as the float path did (volume then
     * scale-to-s16 then clamp), but with no float and no rounding divergence.
     *   gain_l = real_volume * (255 - balance) * out_gain   (<= 64*255*15)
     *   gain_r = real_volume *  balance        * out_gain */
    const int gain_l = chan->real_volume * (255 - chan->balance) * c->out_gain;
    const int gain_r = chan->real_volume * (0   + chan->balance) * c->out_gain;

    int i, num;

    /* A zero increment would divide by zero below; the float path's only guard
     * was position/sample, so guard increment here.  Do NOT skip on zero gain:
     * a muted-but-playing channel must keep advancing its position (tracker
     * music mutes a running sample and later ramps it back, expecting the
     * sample to have advanced in the meantime). */
    if (chan->increment <= 0) {
        return;
    }

    /* Write samples */
    do {

        /* Calculate how many samples we can write in one go.  Both operands
         * are Q32.32, so the 2^32 scale cancels and the quotient is a plain
         * sample count (floor, matching the float truncation). */
        pm_s64 remain = end_pos - chan->position;
        num = (remain > 0) ? (int)(remain / chan->increment) : 0;
        num = _pocketmod_min(num, samples_to_write);

        /* Resample and mix 'num' samples */
        for (i = 0; i < num; i++) {
            int x0 = (int)(chan->position >> PM_POS_FRAC);
            int s; /* interpolated value * 2^PM_INTERP_BITS */
#ifdef POCKETMOD_NO_INTERPOLATION
            s = (int)sample->data[x0] << PM_INTERP_BITS;
#else
            {
                int x1 = x0 + 1 - loop_length * (x0 + 1 >= loop_end);
                /* top PM_INTERP_BITS of the Q32.32 fraction == interp weight */
                int t  = (int)((chan->position >> (PM_POS_FRAC - PM_INTERP_BITS))
                               & ((1 << PM_INTERP_BITS) - 1));
                int s0 = sample->data[x0];
                int s1 = sample->data[x1];
                s = (s0 << PM_INTERP_BITS) + (s1 - s0) * t;
            }
#endif
            chan->position += chan->increment;
            *output++ += (pm_s64) gain_l * s;
            *output++ += (pm_s64) gain_r * s;
        }

        /* Rewind the sample when reaching the loop point */
        if (chan->position >= ((pm_s64) loop_end << PM_POS_FRAC)) {
            chan->position -= (pm_s64) loop_length << PM_POS_FRAC;

        /* Cut the sample if the end is reached */
        } else if (chan->position >= ((pm_s64) sample->length << PM_POS_FRAC)) {
            chan->position = -1;
            break;
        }

        samples_to_write -= num;
    } while (num > 0);
}

static int _pocketmod_ident(pocketmod_context *c, unsigned char *data, int size)
{
    int i, j;

    /* 31-instrument files are at least 1084 bytes long */
    if (size >= 1084) {

        /* The format tag is located at offset 1080 */
        unsigned char *tag = data + 1080;

        /* List of recognized format tags (possibly incomplete) */
        static const struct {
            char name[5];
            char channels;
        } tags[] = {
            /* TODO: FLT8 intentionally omitted because I haven't been able */
            /* to find a specimen to test its funky pattern pairing format  */
            {"M.K.",  4}, {"M!K!",  4}, {"FLT4",  4}, {"4CHN",  4},
            {"OKTA",  8}, {"OCTA",  8}, {"CD81",  8}, {"FA08",  8},
            {"1CHN",  1}, {"2CHN",  2}, {"3CHN",  3}, {"4CHN",  4},
            {"5CHN",  5}, {"6CHN",  6}, {"7CHN",  7}, {"8CHN",  8},
            {"9CHN",  9}, {"10CH", 10}, {"11CH", 11}, {"12CH", 12},
            {"13CH", 13}, {"14CH", 14}, {"15CH", 15}, {"16CH", 16},
            {"17CH", 17}, {"18CH", 18}, {"19CH", 19}, {"20CH", 20},
            {"21CH", 21}, {"22CH", 22}, {"23CH", 23}, {"24CH", 24},
            {"25CH", 25}, {"26CH", 26}, {"27CH", 27}, {"28CH", 28},
            {"29CH", 29}, {"30CH", 30}, {"31CH", 31}, {"32CH", 32}
        };

        /* Check the format tag to determine if this is a 31-sample MOD */
        for (i = 0; i < (int) (sizeof(tags) / sizeof(*tags)); i++) {
            if (tags[i].name[0] == tag[0] && tags[i].name[1] == tag[1]
             && tags[i].name[2] == tag[2] && tags[i].name[3] == tag[3]) {
                c->num_channels = tags[i].channels;
                c->length = data[950];
                c->reset = data[951];
                c->order = &data[952];
                c->patterns = &data[1084];
                c->num_samples = 31;
                return 1;
            }
        }
    }

    /* A 15-instrument MOD has to be at least 600 bytes long */
    if (size < 600) {
        return 0;
    }

    /* Check that the song title only contains ASCII bytes (or null) */
    for (i = 0; i < 20; i++) {
        if (data[i] != '\0' && (data[i] < ' ' || data[i] > '~')) {
            return 0;
        }
    }

    /* Check that sample names only contain ASCII bytes (or null) */
    for (i = 0; i < 15; i++) {
        for (j = 0; j < 22; j++) {
            char chr = data[20 + i * 30 + j];
            if (chr != '\0' && (chr < ' ' || chr > '~')) {
                return 0;
            }
        }
    }

    /* It looks like we have an older 15-instrument MOD */
    c->length = data[470];
    c->reset = data[471];
    c->order = &data[472];
    c->patterns = &data[600];
    c->num_samples = 15;
    c->num_channels = 4;
    return 1;
}

int pocketmod_init(pocketmod_context *c, const void *data, int size, int rate)
{
    int i, remaining, header_bytes, pattern_bytes;
    unsigned char *byte = (unsigned char*) c;
    signed char *sample_data;

    /* Check that arguments look more or less sane */
    if (!c || !data || rate <= 0 || size <= 0) {
        return 0;
    }

    /* Zero out the whole context and identify the MOD type */
    _pocketmod_zero(c, sizeof(pocketmod_context));
    c->source = (unsigned char*) data;
    if (!_pocketmod_ident(c, c->source, size)) {
        return 0;
    }

    /* Check that we are compiled with support for enough channels */
    if (c->num_channels > POCKETMOD_MAX_CHANNELS) {
        return 0;
    }

    /* Check that we have enough sample slots for this file */
    if (POCKETMOD_MAX_SAMPLES < 31) {
        byte = (unsigned char*) data + 20;
        for (i = 0; i < c->num_samples; i++) {
            unsigned int length = 2 * ((byte[22] << 8) | byte[23]);
            if (i >= POCKETMOD_MAX_SAMPLES && length > 2) {
                return 0; /* Can't fit this sample */
            }
            byte += 30;
        }
    }

    /* Check that the song length is in valid range (1..128) */
    if (c->length == 0 || c->length > 128) {
        return 0;
    }

    /* Make sure that the reset pattern doesn't take us out of bounds */
    if (c->reset >= c->length) {
        c->reset = 0;
    }

    /* Count how many patterns there are in the file */
    c->num_patterns = 0;
    for (i = 0; i < 128 && c->order[i] < 128; i++) {
        c->num_patterns = _pocketmod_max(c->num_patterns, c->order[i]);
    }
    pattern_bytes = 256 * c->num_channels * ++c->num_patterns;
    header_bytes = (int) ((char*) c->patterns - (char*) data);

    /* Check that each pattern in the order is within file bounds */
    for (i = 0; i < c->length; i++) {
        if (header_bytes + 256 * c->num_channels * c->order[i] > size) {
            return 0; /* Reading this pattern would be a buffer over-read! */
        }
    }

    /* Check that the pattern data doesn't extend past the end of the file */
    if (header_bytes + pattern_bytes > size) {
        return 0;
    }

    /* Load sample payload data, truncating ones that extend outside the file */
    remaining = size - header_bytes - pattern_bytes;
    sample_data = (signed char*) data + header_bytes + pattern_bytes;
    for (i = 0; i < c->num_samples; i++) {
        unsigned char *data = POCKETMOD_SAMPLE(c, i + 1);
        unsigned int length = ((data[0] << 8) | data[1]) << 1;
        _pocketmod_sample *sample = &c->samples[i];
        sample->data = sample_data;
        sample->length = _pocketmod_min(length > 2 ? length : 0, remaining);
        sample_data += sample->length;
        remaining -= sample->length;
    }

    /* Set up ProTracker default panning for all channels */
    for (i = 0; i < c->num_channels; i++) {
        c->channels[i].balance = 0x80 + ((((i + 1) >> 1) & 1) ? 0x20 : -0x20);
    }

    /* Prepare to render from the start */
    c->ticks_per_line = 6;
    c->samples_per_second = rate;
    /* clock = 3546894.6 * 2^32 / rate, computed with integers (numerator is
     * 35468946 << 32 <= ~1.5e17, well inside 64 bits for any sane rate). */
    c->clock = (pm_u64)(((pm_u64)35468946 << 32) / ((pm_u64)10 * (unsigned)rate));
    /* default samples_per_tick = rate / 50 (125 BPM), Q32.32 */
    c->samples_per_tick = ((pm_s64)rate << PM_POS_FRAC) / 50;
    c->out_gain = 15;           /* full music volume until set otherwise */
    c->lfo_rng = 0xbadc0de;
    c->line = -1;
    c->tick = c->ticks_per_line - 1;
    _pocketmod_next_tick(c);
    return 1;
}

int pocketmod_render(pocketmod_context *c, void *buffer, int buffer_size)
{
    int i, samples_rendered = 0;
    int samples_remaining = buffer_size / (int)POCKETMOD_SAMPLE_SIZE;
    if (c && buffer) {
        short (*output)[2] = (short(*)[2]) buffer;
        /* 64-bit stereo mix accumulator for one chunk.  static (like the
         * float scratch this replaces) to keep it off the audio-thread stack
         * on memory-constrained targets; the decoder runs single-instance. */
        static pm_s64 mix[PM_CHUNK * 2];
        while (samples_remaining > 0) {

            /* Calculate the number of samples left in this tick (Q32.32 ->
             * integer floor), guaranteeing forward progress with !num. */
            int num = (int) ((c->samples_per_tick - c->sample) >> PM_POS_FRAC);
            num = _pocketmod_min(num + !num, samples_remaining);
            if (num > PM_CHUNK) {
                num = PM_CHUNK;
            }

            /* Render and mix 'num' frames from each channel */
            _pocketmod_zero(mix, num * (int)sizeof(pm_s64) * 2);
            for (i = 0; i < c->num_channels; i++) {
                _pocketmod_chan *chan = &c->channels[i];
                if (chan->sample != 0 && chan->position >= 0) {
                    _pocketmod_render_channel(c, chan, mix, num);
                }
            }

            /* Scale the accumulator to s16 and clamp.  PM_OUT_DIV brings the
             * folded (volume * level * out_gain * 2^PM_INTERP_BITS) mix back to
             * full-scale s16; clamp to the signed-16 range. */
            for (i = 0; i < num; i++) {
                pm_s64 l = mix[i * 2 + 0] / PM_OUT_DIV;
                pm_s64 r = mix[i * 2 + 1] / PM_OUT_DIV;
                if (l >  32767) l =  32767;
                if (l < -32768) l = -32768;
                if (r >  32767) r =  32767;
                if (r < -32768) r = -32768;
                output[i][0] = (short) l;
                output[i][1] = (short) r;
            }

            samples_remaining -= num;
            samples_rendered += num;
            output += num;

            /* Advance song position by 'num' samples (Q32.32) */
            if ((c->sample += (pm_s64) num << PM_POS_FRAC) >= c->samples_per_tick) {
                c->sample -= c->samples_per_tick;
                _pocketmod_next_tick(c);

                /* Stop if a new pattern was reached */
                if (c->line == 0 && c->tick == 0) {

                    /* Increment loop counter as needed */
                    if (c->visited[c->pattern >> 3] & (1 << (c->pattern & 7))) {
                        _pocketmod_zero(c->visited, sizeof(c->visited));
                        c->loop_count++;
                    }
                    break;
                }
            }
        }
    }
    return samples_rendered * (int)POCKETMOD_SAMPLE_SIZE;
}

int pocketmod_loop_count(pocketmod_context *c)
{
    return c->loop_count;
}

#endif /* #ifdef POCKETMOD_IMPLEMENTATION */

#ifdef __cplusplus
}
#endif

#endif /* #ifndef POCKETMOD_H_INCLUDED */

/*******************************************************************************

MIT License

Copyright (c) 2018 rombankzero

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*******************************************************************************/
