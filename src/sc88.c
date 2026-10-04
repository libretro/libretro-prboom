/* Roland SC-88 / SC-88VL / SC-88Pro emulation: see sc88.h.
 *
 * A C89 port of 88emu (gearmulator, The Usual Suspects, commit 0764877,
 * October 2026): the H8/500 CPU core and H8/510 peripherals, the XP
 * sound chip with its effects DSP, the LSP insertion-effect chip, the
 * SC-88 and SC-88Pro boards and their MIDI sub-MCU.  Checked sample for
 * sample and state for state against the C++ on random programs, wave
 * ROMs and MIDI; the C++'s JIT compilers are not carried over, only its
 * interpreters.
 *
 * Copyright (C) The Usual Suspects (C++ original)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "sc88.h"

#if defined(_MSC_VER)
#define SC88_INLINE static __inline
#elif defined(__GNUC__) || defined(__clang__)
#define SC88_INLINE static __inline__
#else
#define SC88_INLINE static
#endif

/* ---- XP sound chip: types -------------------------------------------- */

#define XP_MAX_VOICES         64
#define XP_WAVE_CHIP_SELECTS  8
#define XP_DSP_PROGRAM_SLOTS  288
#define XP_DSP_EXEC_SLOTS     256
#define XP_DSP_IRAM_SLOTS     64
#define XP_DSP_ERAM_WORDS     0x10000
#define XP_DSP_SERIAL_WORDS   32
#define XP_DSP_SERIAL_BUSES   4

/* wave control (0x0000) */
#define XP_WC_EXP0_OR_LIVE        0x00080u
#define XP_WC_LDPCM_FORMAT        0x00200u
#define XP_WC_ENGINE_HALT         0x00400u
#define XP_WC_REVERSE             0x00800u
#define XP_WC_ALTERNATE_LOOP      0x01000u
#define XP_WC_DIRECTION_STATE     0x02000u
#define XP_WC_LOOP_REASON6        0x04000u
#define XP_WC_IRQ_ENABLE          0x08000u
#define XP_WC_LOOP_EVENT_INHIBIT  0x10000u
#define XP_WC_LOOP_MARKER_FIRST   0x20000u
#define XP_WC_MUTE_STATUS         0x40000u
#define XP_WC_MUTE_REQUEST        0x80000u
#define XP_WC_PRODUCER_BLOCKED    0xc0000u

#define XP_IRQ_TVFQ_TERMINAL        0
#define XP_IRQ_TVFF_TERMINAL        1
#define XP_IRQ_PITCH_TERMINAL       2
#define XP_IRQ_AMPMOD_TERMINAL      3
#define XP_IRQ_AMP_TERMINAL         4
#define XP_IRQ_PLAYBACK_MARKER      5
#define XP_IRQ_ALT_PLAYBACK_MARKER  6
#define XP_IRQ_MUTE_TRANSITION      8

#define XP_FC_PAIR_ROLE                0x00010u
#define XP_FC_BOOSTER_MASK             0x000c0u
#define XP_FC_PAIRED_SECOND_MODE_MASK  0x00300u
#define XP_FC_MODE_MASK                0x00c00u
#define XP_FC_STRUCTURE_MASK           0x0f000u

enum { XP_PHASE_PARKED, XP_PHASE_PRELOAD, XP_PHASE_INITIALIZE, XP_PHASE_STARTING, XP_PHASE_RUNNING };
enum { XP_FMT_FCE_DPCM, XP_FMT_FCE_DPCM_EXP0, XP_FMT_LDPCM };
enum { XP_ROM_BITS8, XP_ROM_BITS16 };

typedef struct
{
    int ampCurve2EntryPending;
    int runtimePhase;
    int waveSampleFormat;
} xp_voice_cache_t;

typedef struct
{
    int released;
    int shadow;
} xp_voice_reset_t;

typedef struct
{
    uint32_t waveControl_0000;
    uint32_t sampleCurrent_0100;
    uint32_t sampleLoop_0200;
    uint32_t sampleEnd_0300;
    uint32_t waveFetchState_0400;
    uint16_t waveCircularBuffer_0800[8];
    uint32_t dpcmAccumulator_0c00;
    uint32_t pitchIncrement_0d00;
    uint32_t addressFraction_0e00;
    uint32_t playbackStateConfig_1000;
    uint32_t tvfQDestination_1100;
    uint32_t pitchDestination_1200;
    uint32_t tvfFDestination_1300;
    uint32_t ampModDestination_1400;
    uint32_t ampDestination_1500;
    uint32_t tvfQRamp_1600;
    uint32_t pitchRamp_1700;
    uint32_t tvfFRamp_1800;
    uint32_t ampModRamp_1900;
    uint32_t ampRamp_1a00;
    uint32_t pitchCurrent_1b00;
    uint32_t tvfFCurrent_1c00;
    uint32_t ampModCurrent_1d00;
    uint32_t ampCurrent_1e00;
    uint32_t filterConfig_2000;
    uint32_t tvfQCurrent_2100;
    uint32_t tvfFCoefficient_2200;
    uint32_t combinedAmp_2300;
    uint32_t pitchStep_2400;
    uint32_t tvfFStep_2500;
    uint32_t ampStep_2600;
    uint16_t tvaGain_2700;
    uint32_t filterBp_2800;
    uint32_t filterLp_2900;
    uint32_t filterOutput_2a00;
    uint16_t mixer_3a00[4];
    xp_voice_reset_t resetState_3900;
    xp_voice_cache_t runtimeCache;
} xp_voice_t;

/* DSP */

typedef struct
{
    uint32_t pram[XP_DSP_PROGRAM_SLOTS];
    uint16_t cram[XP_DSP_PROGRAM_SLOTS];
} xp_dsp_program_t;

typedef struct
{
    size_t  destination;
    int64_t contribution;
} xp_mixer_send_t;

typedef struct
{
    xp_mixer_send_t v[XP_MAX_VOICES][4];
} xp_mixer_frame_t;

typedef struct
{
    int64_t  sums[XP_DSP_IRAM_SLOTS];
    uint64_t seen;
    uint64_t clipped;
} xp_mixer_summary_t;

typedef struct
{
    int      executeProgram;
    int      serialInputEnabled;
    uint16_t serialAudio0Config;
    uint16_t serialAudio1Config;
    int      serialOutputEnabled;
    size_t   executionSlots;
    const xp_mixer_frame_t *mixerFrame;
} xp_dsp_request_t;

typedef struct
{
    xp_dsp_request_t request;
    size_t cycle;
    size_t pc;
} xp_dsp_context_t;

typedef struct
{
    int32_t countdown;
    int32_t value;
} xp_pending_eram_read_t;

typedef struct
{
    int64_t  accumulator;
    int64_t  iramReadLatch;
    int64_t  multiplyResultLatch;
    int64_t  multiplyFeedbackLatch;
    int64_t  eramReadLatch;
    int64_t  eramPendingWriteValue;
    uint16_t iram3ParameterLatch;
    uint16_t eramIndexedOffset;
    uint8_t  eramPrefixPending;
    uint8_t  eramPendingWrite;
    uint8_t  eramOffsetHigh;
    uint8_t  iramSelPhase;
    uint8_t  multiplyNegativeFraction;
    xp_pending_eram_read_t pendingEramReads[2];
    uint32_t eramPos;
    uint32_t outputWordPosition;
    uint8_t  dacPortPosition;
    uint8_t  outputPins;
    uint64_t mixerInitialized;
    int64_t  serialInputNode[2];
    uint32_t serialInputCount[2];
    uint32_t serialInputIndex[2];
    uint32_t serialOutputCount[XP_DSP_SERIAL_BUSES];
    int32_t  serialInput[2][XP_DSP_SERIAL_WORDS];
    int32_t  serialOutput[XP_DSP_SERIAL_BUSES][XP_DSP_SERIAL_WORDS];
    uint32_t iram1[XP_DSP_IRAM_SLOTS];
    uint32_t iram2[XP_DSP_IRAM_SLOTS];
    uint32_t iram3[XP_DSP_IRAM_SLOTS];
    uint32_t eram[XP_DSP_ERAM_WORDS];
} xp_dsp_state_t;

/* What of the frame's setup the program's lowering depends on. */
typedef struct
{
    uint8_t  serialInputEnabled;
    uint8_t  serialOutputEnabled;
    uint8_t  consumeStagedBusA;
    uint8_t  bcdEnabled;
    uint8_t  iram3ParameterReadBoundary;
    uint8_t  linked;
    uint16_t executionSlots;
} xp_dsp_config_t;

typedef struct
{
    xp_dsp_program_t program;
    xp_dsp_state_t   s;
    /* The C++ lowers the program to a flat form for its JIT.  Without
     * the JIT only one product of that is used: which mixer cells the
     * program touches, per IRAM phase.  It is cached under the same
     * rules: recomputed when the program is marked changed or the frame
     * setup differs. */
    xp_dsp_config_t  flatConfig;
    uint64_t         mixerCells[2];
    int              programTainted;
} xp_dsp_t;

typedef struct
{
    xp_voice_t voices[XP_MAX_VOICES];
    xp_dsp_t   dsp;
    uint32_t   readbackLatch;
    uint16_t   wideWriteLatch;
    uint16_t   highestVoice;
    uint16_t   irqStatus;
    uint16_t   irqConfigMask;
    uint16_t   irqAcknowledge;
    int        irqBlockedEvent;
    uint8_t    waveRomConfig[XP_WAVE_CHIP_SELECTS];
    uint16_t   waveRomPage;
    uint16_t   waveRomBank;
    uint16_t   serialAudioConfig[2];
    uint16_t   diagnosticSelect_3930;
    uint16_t   serialFormat_3932;
    uint8_t    voiceWindowSelect_3934;
    uint16_t   dspControl;
    uint16_t   iram3RampRates[4];
    uint64_t   sampleClock;
    int        interrupt;
} xp_state_t;

typedef struct
{
    const uint8_t *data;
    size_t   size;
    int      width;
    uint8_t  apertureBankShift;
    uint8_t  voiceBankShift;
} xp_wave_rom_t;

typedef void (*xp_irq_fn)(void *user, int level);

typedef struct xp
{
    xp_state_t          state;
    xp_mixer_frame_t    frame;
    size_t              frameVoices;
    xp_mixer_summary_t  mixerSummary;
    xp_wave_rom_t       waveRoms[XP_WAVE_CHIP_SELECTS];
    uint8_t             hostWriteBytes[0x4000];
    uint16_t            hostReadWord;
    uint16_t            hostReadAddress;
    int                 interruptLine;
    xp_irq_fn           interruptCallback;
    void               *interruptUser;
    int                 irqEventAcceptedThisStep;
} xp_t;

static void xp_raise_voice_loop_marker_event(xp_t *xp, size_t vi, xp_voice_t *v);
static void xp_raise_voice_mute_event(xp_t *xp, size_t vi, xp_voice_t *v);
static void xp_step_voice_ramps(xp_t *xp, size_t vi, xp_voice_t *v);

static const uint16_t xp_pitch_mantissa[256] = {
    16384, 16428, 16473, 16518, 16562, 16607, 16652, 16697,
    16743, 16788, 16834, 16879, 16925, 16971, 17017, 17063,
    17109, 17156, 17202, 17249, 17296, 17343, 17390, 17437,
    17484, 17531, 17579, 17627, 17674, 17722, 17770, 17819,
    17867, 17915, 17964, 18013, 18061, 18110, 18160, 18209,
    18258, 18308, 18357, 18407, 18457, 18507, 18557, 18607,
    18658, 18708, 18759, 18810, 18861, 18912, 18963, 19015,
    19066, 19118, 19170, 19222, 19274, 19326, 19379, 19431,
    19484, 19537, 19590, 19643, 19696, 19750, 19803, 19857,
    19911, 19965, 20019, 20073, 20127, 20182, 20237, 20292,
    20347, 20402, 20457, 20513, 20568, 20624, 20680, 20736,
    20792, 20849, 20905, 20962, 21019, 21076, 21133, 21190,
    21247, 21305, 21363, 21421, 21479, 21537, 21595, 21654,
    21713, 21772, 21831, 21890, 21949, 22009, 22068, 22128,
    22188, 22248, 22309, 22369, 22430, 22491, 22552, 22613,
    22674, 22735, 22797, 22859, 22921, 22983, 23045, 23108,
    23170, 23233, 23296, 23359, 23423, 23486, 23550, 23614,
    23678, 23742, 23806, 23871, 23936, 24001, 24066, 24131,
    24196, 24262, 24328, 24394, 24460, 24526, 24593, 24659,
    24726, 24793, 24860, 24928, 24995, 25063, 25131, 25199,
    25268, 25336, 25405, 25474, 25543, 25612, 25681, 25751,
    25821, 25891, 25961, 26031, 26102, 26173, 26244, 26315,
    26386, 26458, 26530, 26601, 26674, 26746, 26818, 26891,
    26964, 27037, 27110, 27184, 27258, 27332, 27406, 27480,
    27554, 27629, 27704, 27779, 27855, 27930, 28006, 28082,
    28158, 28234, 28311, 28388, 28464, 28542, 28619, 28697,
    28774, 28852, 28931, 29009, 29088, 29167, 29246, 29325,
    29405, 29484, 29564, 29644, 29725, 29805, 29886, 29967,
    30048, 30130, 30212, 30293, 30376, 30458, 30541, 30623,
    30706, 30790, 30873, 30957, 31041, 31125, 31209, 31294,
    31379, 31464, 31549, 31635, 31720, 31806, 31893, 31979,
    32066, 32153, 32240, 32327, 32415, 32503, 32591, 32679
};

static const uint16_t xp_interp[3][128] = {
    {
        3385, 3401, 3417, 3432, 3448, 3463, 3478, 3492, 3506, 3521, 3535, 3548, 3562, 3575, 3588, 3601,
        3614, 3626, 3638, 3650, 3662, 3673, 3685, 3696, 3707, 3718, 3728, 3739, 3749, 3759, 3768, 3778,
        3787, 3796, 3805, 3814, 3823, 3831, 3839, 3847, 3855, 3863, 3870, 3878, 3885, 3892, 3899, 3905,
        3912, 3918, 3924, 3930, 3936, 3942, 3948, 3953, 3958, 3963, 3968, 3973, 3978, 3983, 3987, 3991,
        3995, 4000, 4004, 4007, 4011, 4015, 4018, 4022, 4025, 4028, 4031, 4034, 4037, 4040, 4042, 4045,
        4047, 4050, 4052, 4054, 4057, 4059, 4061, 4063, 4064, 4066, 4068, 4070, 4071, 4073, 4074, 4076,
        4077, 4078, 4079, 4081, 4082, 4083, 4084, 4085, 4086, 4086, 4087, 4088, 4089, 4089, 4090, 4091,
        4091, 4092, 4092, 4093, 4093, 4094, 4094, 4094, 4094, 4095, 4095, 4095, 4095, 4095, 4095, 4095
    },
    {
         710,  726,  742,  758,  775,  792,  809,  826,  844,  861,  879,  897,  915,  933,  952,  971,
         990, 1009, 1028, 1047, 1067, 1087, 1106, 1126, 1147, 1167, 1188, 1208, 1229, 1250, 1271, 1292,
        1314, 1335, 1357, 1379, 1400, 1423, 1445, 1467, 1489, 1512, 1534, 1557, 1580, 1602, 1625, 1648,
        1671, 1695, 1718, 1741, 1764, 1788, 1811, 1835, 1858, 1882, 1906, 1929, 1953, 1977, 2000, 2024,
        2048, 2071, 2095, 2119, 2143, 2166, 2190, 2214, 2237, 2261, 2284, 2308, 2331, 2355, 2378, 2401,
        2425, 2448, 2471, 2494, 2517, 2539, 2562, 2585, 2607, 2630, 2652, 2674, 2696, 2718, 2740, 2762,
        2783, 2805, 2826, 2847, 2868, 2889, 2910, 2931, 2951, 2971, 2991, 3011, 3031, 3051, 3070, 3089,
        3108, 3127, 3146, 3164, 3182, 3200, 3218, 3236, 3253, 3271, 3288, 3304, 3321, 3338, 3354, 3370
    },
    {
           0,    0,    0,    1,    1,    1,    2,    2,    3,    3,    3,    4,    4,    5,    5,    6,
           6,    7,    8,    8,    9,   10,   10,   11,   12,   13,   14,   15,   16,   17,   18,   19,
          20,   22,   23,   24,   26,   27,   29,   30,   32,   34,   36,   38,   40,   42,   44,   46,
          49,   51,   53,   56,   59,   62,   65,   68,   71,   74,   77,   81,   84,   88,   92,   96,
         100,  104,  109,  113,  118,  122,  127,  132,  137,  143,  148,  154,  160,  165,  171,  178,
         184,  191,  197,  204,  211,  219,  226,  234,  241,  249,  257,  266,  274,  283,  292,  301,
         310,  319,  329,  339,  349,  359,  369,  380,  391,  402,  413,  424,  436,  448,  460,  472,
         484,  497,  510,  523,  536,  549,  563,  577,  591,  605,  619,  634,  648,  663,  679,  694
    }
};

/* ---- XP: voice helpers (xp_voice_common.h) ---------------------------- */

#define XP_VOICE_ADDRESS_MASK   0x0fffffu
#define XP_PLAYBACK_STARTING    0x10000u
#define XP_PLAYBACK_RUNNING     0x30000u
#define XP_RAMP_RATE_MASK       0x00fffu
#define XP_RAMP_INTERVAL_MASK   0x03000u
#define XP_RAMP_CURVE_MASK      0x0c000u
#define XP_RAMP_TRUNK_COMPLETE  0x04000u
#define XP_RAMP_EVENT_ARM       0x10000u
#define XP_RAMP_HOLD            0x20000u
#define XP_RAMP_ACK             1u
#define XP_RAMP_FIELD_MASK      0x3ffffu
#define XP_RAMP_SCRATCH_MASK    0xfffffu
#define XP_PHASE_MASK           0x3ffffu
#define XP_PHASE_FRACTION_MASK  0x03fffu
#define XP_DPCM_MASK            0x3ffffu
#define XP_FILTER_MASK          0xffffffu
#define XP_MIXER_DEST_MASK      0x003fu
#define XP_MIXER_LEVEL_SHIFT    6
#define XP_MIXER_PRODUCT_SHIFT  9

SC88_INLINE uint32_t xp_width_mask(unsigned width)
{
    return ((uint32_t)1 << width) - 1;
}

SC88_INLINE int32_t xp_sign_extend(uint32_t value, unsigned width)
{
    const uint32_t sign = (uint32_t)1 << (width - 1);
    return (int32_t)((value ^ sign) - sign);
}

SC88_INLINE int32_t xp_saturate24(int64_t value)
{
    if (value < -0x800000)
        return -0x800000;
    if (value > 0x7fffff)
        return 0x7fffff;
    return (int32_t)value;
}

SC88_INLINE int32_t xp_saturate16(int64_t value)
{
    if (value < -0x8000)
        return -0x8000;
    if (value > 0x7fff)
        return 0x7fff;
    return (int32_t)value;
}

SC88_INLINE uint32_t xp_decode_pitch(uint32_t pitch)
{
    uint32_t index, fraction, a, next, b, octave_shift;
    if (pitch == 0)
        return 0;
    index        = (pitch >> 6) & 0xff;
    fraction     = pitch & 0x3f;
    a            = ((64 - fraction) * xp_pitch_mantissa[index]) >> 2;
    next         = index + 1 == 256 ? 0x8000u : xp_pitch_mantissa[index + 1];
    b            = (fraction * next) >> 2;
    octave_shift = (~(pitch >> 14)) & 0x0f;
    return ((a + b) >> 1) >> octave_shift;
}

SC88_INLINE uint32_t xp_unpack_phase(uint32_t packed)
{
    return ((packed & 0x0f) << 14) | ((packed >> 4) & XP_PHASE_FRACTION_MASK);
}

SC88_INLINE uint32_t xp_pack_phase(uint32_t linear)
{
    return ((linear & XP_PHASE_FRACTION_MASK) << 4) | ((linear >> 14) & 0x0f);
}

SC88_INLINE uint8_t xp_reverse_two_bits(uint8_t value)
{
    return (uint8_t)(((value & 1) << 1) | ((value & 2) >> 1));
}

SC88_INLINE uint32_t xp_ramp_divider_mask(uint32_t control)
{
    switch ((control & XP_RAMP_INTERVAL_MASK) >> 12)
    {
        case 0:  return 0;
        case 1:  return 7;
        case 2:  return 31;
        default: return 127;
    }
}

SC88_INLINE uint32_t xp_linear_ramp_step(uint32_t current, uint32_t target, uint32_t control)
{
    const int32_t difference = (int32_t)target - (int32_t)current;
    const int64_t step = ((int64_t)(difference >> 3) * (int64_t)(control & XP_RAMP_RATE_MASK)) >> 10;
    return (uint32_t)(step * 2 + (difference > 0 ? 1 : 0)) & XP_RAMP_SCRATCH_MASK;
}

SC88_INLINE void xp_step_linear_ramp(uint32_t *current, uint32_t target, uint32_t scratch, uint8_t parity)
{
    int     ascending;
    int32_t step;
    int64_t delta, next;
    if (*current == target)
        return;
    ascending = *current < target;
    step      = xp_sign_extend(scratch, 20);
    delta     = (int64_t)(step + (ascending ? parity : 0)) >> 1;
    next      = (int64_t)*current + delta;
    if ((ascending && next > (int64_t)target) || (!ascending && next < (int64_t)target))
        next = (int64_t)target;
    *current = (uint32_t)next & XP_RAMP_FIELD_MASK;
}

SC88_INLINE void xp_step_log_ramp(uint32_t *current, uint32_t target, uint32_t control,
                                  uint32_t quantum, uint8_t parity)
{
    int32_t difference, scaled, correction;
    int64_t step, next, limit;
    if (*current == target)
        return;
    difference = (int32_t)target - (int32_t)*current;
    scaled     = difference / (int32_t)quantum;
    step       = (((int64_t)(scaled >> 3) * (int64_t)(control & XP_RAMP_RATE_MASK)) >> 10)
               * (int64_t)(int32_t)quantum;
    correction = quantum > 1 ? (int32_t)(quantum >> 1) : (int32_t)parity;
    next       = (int64_t)*current + step;
    if ((difference > 0 && next < (int64_t)target)
        || ((control & XP_RAMP_RATE_MASK) == 0 && *current != target))
        next += correction;
    limit = quantum > 1 ? (int64_t)XP_RAMP_SCRATCH_MASK : (int64_t)XP_RAMP_FIELD_MASK;
    if (next < 0)
        next = 0;
    else if (next > limit)
        next &= limit;
    if (quantum > 1)
        next &= ~(int64_t)1;
    *current = (uint32_t)next;
}

SC88_INLINE uint32_t xp_combine_amp(const xp_voice_t *v)
{
    const uint64_t cap = XP_RAMP_SCRATCH_MASK & ~(uint32_t)1;
    if ((v->ampModRamp_1900 & 0x04000) != 0)
    {
        const uint64_t sum = (uint64_t)(v->ampCurrent_1e00 + v->ampModCurrent_1d00) << 2;
        return (uint32_t)(sum < cap ? sum : cap);
    }
    {
        uint64_t product = ((uint64_t)(v->ampCurrent_1e00 >> 3) * (v->ampModCurrent_1d00 >> 4)) >> 8;
        product &= ~(uint64_t)1;
        return (uint32_t)(product < cap ? product : cap);
    }
}

/* ---- XP: wave playback (xp_voice.cpp) --------------------------------- */

static int xp_voice_reverse(const xp_voice_t *v)
{
    return ((v->waveControl_0000 & XP_WC_REVERSE) != 0)
        != ((v->waveControl_0000 & XP_WC_DIRECTION_STATE) != 0);
}

static int xp_voice_16bit_bus(const xp_t *xp, const xp_voice_t *v)
{
    const size_t chip_select = (v->waveControl_0000 >> 4) & 7;
    return (xp->state.waveRomConfig[chip_select] & 1) != 0;
}

static uint8_t xp_read_voice_wave_byte(const xp_t *xp, const xp_voice_t *v, uint32_t address)
{
    const size_t chip_select = (v->waveControl_0000 >> 4) & 7;
    const xp_wave_rom_t *rom = &xp->waveRoms[chip_select];
    const size_t bank = (size_t)(v->waveControl_0000 & 0x0f) >> rom->voiceBankShift;
    size_t voice_address = (size_t)address & XP_VOICE_ADDRESS_MASK;
    size_t full;
    if (!xp_voice_16bit_bus(xp, v) && rom->width == XP_ROM_BITS16)
        voice_address &= ~(size_t)1;
    full = (bank << 20) | voice_address;
    if (rom->data == NULL || full >= rom->size)
        return 0;
    return rom->data[full];
}

static uint16_t xp_encode_voice_sample(const xp_voice_t *v, uint8_t mantissa, uint8_t exponent)
{
    if (v->runtimeCache.waveSampleFormat != XP_FMT_FCE_DPCM)
        return mantissa;
    return (uint16_t)((exponent << 8) | mantissa);
}

static uint16_t xp_read_voice_sample(const xp_t *xp, const xp_voice_t *v, uint32_t address)
{
    const unsigned exponent_index = (unsigned)((address >> 4) & 3);
    const uint32_t exponent = (v->waveFetchState_0400 >> (exponent_index * 4)) & 0x0f;
    return xp_encode_voice_sample(v, xp_read_voice_wave_byte(xp, v, address), (uint8_t)exponent);
}

static int32_t xp_decode_voice_delta(const xp_voice_t *v, uint16_t encoded)
{
    int32_t  mantissa;
    unsigned exponent;
    if (v->runtimeCache.waveSampleFormat == XP_FMT_LDPCM)
    {
        const int32_t  code      = (int32_t)(int8_t)encoded;
        const uint32_t magnitude = (uint32_t)(code < 0 ? -code : code);
        const uint32_t shift     = magnitude >> 4;
        const uint32_t expanded  = shift == 0 ? (magnitude & 0x0f)
                                              : ((magnitude & 0x0f) + 0x10) << (shift - 1);
        return (int32_t)(expanded << 6) * (code < 0 ? -1 : 1);
    }
    mantissa = (int32_t)(int8_t)encoded;
    exponent = (unsigned)((encoded >> 8) & 0x0f);
    if (v->runtimeCache.waveSampleFormat == XP_FMT_FCE_DPCM_EXP0)
        exponent = 0;
    else if (exponent > 10)
        return 0;
    return mantissa * (int32_t)((uint32_t)1 << exponent);
}

static int32_t xp_interpolate_voice(const xp_voice_t *v, uint32_t phase)
{
    const size_t ratio    = (phase & XP_PHASE_FRACTION_MASK) >> 7;
    const size_t consumer = phase >> 14;
    int64_t  extended = (int64_t)xp_sign_extend(v->dpcmAccumulator_0c00, 18) * 4;
    size_t   tap;
    unsigned gain;
    int32_t  wrapped;

    switch (v->runtimeCache.waveSampleFormat)
    {
        case XP_FMT_LDPCM:
            for (tap = 0; tap < 3; ++tap)
            {
                const int32_t delta = xp_decode_voice_delta(v, v->waveCircularBuffer_0800[(consumer + tap) & 7]);
                const int64_t product = ((int64_t)xp_interp[tap][ratio] * delta) >> 8;
                extended += (product * 4) >> 4;
            }
            break;
        case XP_FMT_FCE_DPCM_EXP0:
            for (tap = 0; tap < 3; ++tap)
            {
                const int32_t mantissa = (int32_t)(int8_t)v->waveCircularBuffer_0800[(consumer + tap) & 7];
                const int64_t product = ((int64_t)xp_interp[tap][ratio] * mantissa * 64) >> 8;
                extended += (product * 4) >> 10;
            }
            break;
        default:
            for (tap = 0; tap < 3; ++tap)
            {
                const uint16_t encoded  = v->waveCircularBuffer_0800[(consumer + tap) & 7];
                const int32_t  mantissa = (int32_t)(int8_t)encoded;
                const unsigned exponent = (unsigned)((encoded >> 8) & 0x0f);
                if (exponent <= 10)
                {
                    const int64_t product = ((int64_t)xp_interp[tap][ratio] * mantissa * 64) >> 8;
                    extended += (product * 4) >> (10 - exponent);
                }
            }
            break;
    }
    gain    = (unsigned)((v->playbackStateConfig_1000 >> 3) & 3);
    wrapped = xp_sign_extend((uint32_t)extended & 0xfffff, 20);
    return wrapped >> (3 - gain);
}

static uint16_t xp_read_exponent_cache(const xp_t *xp, const xp_voice_t *v, uint32_t address)
{
    uint32_t a;
    uint8_t  low, high;
    if (v->runtimeCache.waveSampleFormat != XP_FMT_FCE_DPCM)
        return 0;
    a    = (address & ~(uint32_t)0x3f) >> 5;
    low  = xp_read_voice_wave_byte(xp, v, a);
    high = xp_read_voice_wave_byte(xp, v, a + 1);
    return (uint16_t)(low | ((uint16_t)high << 8));
}

static void xp_refresh_exponent_cache(const xp_t *xp, xp_voice_t *v, int looped)
{
    if (!looped)
    {
        const uint32_t boundary = xp_voice_reverse(v) ? 0x3f : 0;
        if ((v->sampleCurrent_0100 & 0x3f) != boundary)
            return;
    }
    v->waveFetchState_0400 = (v->waveFetchState_0400 & 0x0f0000)
                           | xp_read_exponent_cache(xp, v, v->sampleCurrent_0100);
}

static int xp_advance_voice_address(xp_t *xp, size_t vi, xp_voice_t *v)
{
    const int reverse = xp_voice_reverse(v);
    const int passed  = reverse ? v->sampleCurrent_0100 <= v->sampleLoop_0200
                                : v->sampleCurrent_0100 >= v->sampleLoop_0200;
    int      direction_state;
    uint32_t boundary;
    if (passed)
        xp_raise_voice_loop_marker_event(xp, vi, v);
    direction_state = (v->waveControl_0000 & XP_WC_DIRECTION_STATE) != 0;
    boundary        = direction_state ? v->sampleLoop_0200 : v->sampleEnd_0300;
    if ((v->sampleCurrent_0100 & XP_VOICE_ADDRESS_MASK) == (boundary & XP_VOICE_ADDRESS_MASK))
    {
        if ((v->waveControl_0000 & XP_WC_ALTERNATE_LOOP) != 0)
        {
            v->waveControl_0000 ^= XP_WC_DIRECTION_STATE;
            return 0;
        }
        if ((v->sampleLoop_0200 & XP_VOICE_ADDRESS_MASK) == (v->sampleEnd_0300 & XP_VOICE_ADDRESS_MASK))
            return 0;
        v->sampleCurrent_0100 = (direction_state ? v->sampleEnd_0300 : v->sampleLoop_0200)
                              & XP_VOICE_ADDRESS_MASK;
        return 1;
    }
    if (reverse)
        v->sampleCurrent_0100 = (v->sampleCurrent_0100 - 1) & XP_VOICE_ADDRESS_MASK;
    else
        v->sampleCurrent_0100 = (v->sampleCurrent_0100 + 1) & XP_VOICE_ADDRESS_MASK;
    return 0;
}

static void xp_refill_voice_cell(xp_t *xp, size_t vi, xp_voice_t *v)
{
    const size_t pointer = (size_t)(v->waveFetchState_0400 >> 16) & 0x0f;
    const size_t cell    = pointer & 7;
    int      looped;
    uint32_t next_pointer;
    v->waveCircularBuffer_0800[cell] = xp_read_voice_sample(xp, v, v->sampleCurrent_0100);
    looped = xp_advance_voice_address(xp, vi, v);
    xp_refresh_exponent_cache(xp, v, looped);
    next_pointer = (uint32_t)((pointer + 1) & 0x0f);
    v->waveFetchState_0400 = (next_pointer << 16) | (v->waveFetchState_0400 & 0xffff);
}

static void xp_refill_voice_pair(xp_t *xp, size_t vi, xp_voice_t *v)
{
    const size_t pointer = (size_t)(v->waveFetchState_0400 >> 16) & 0x0f;
    const size_t cell    = pointer & 7;
    int      looped;
    uint32_t next_pointer;
    v->waveCircularBuffer_0800[cell] = xp_read_voice_sample(xp, v, v->sampleCurrent_0100);
    looped = xp_advance_voice_address(xp, vi, v);
    xp_refresh_exponent_cache(xp, v, looped);
    v->waveCircularBuffer_0800[(cell + 1) & 7] = xp_read_voice_sample(xp, v, v->sampleCurrent_0100);
    looped = xp_advance_voice_address(xp, vi, v);
    xp_refresh_exponent_cache(xp, v, looped);
    next_pointer = (uint32_t)((pointer + 2) & 0x0f);
    v->waveFetchState_0400 = (next_pointer << 16) | (v->waveFetchState_0400 & 0xffff);
}

/* Returns non-zero when the filter is due; *sample is the voice's sample. */
static int xp_step_voice_source(xp_t *xp, size_t vi, xp_voice_t *v, int32_t *sample)
{
    uint32_t playback_counter, phase, phase_increment, next_phase, cell_count, cell;
    int32_t  interpolated;
    uint8_t  dither_phase;
    size_t   consumer;

    *sample = 0;
    xp_raise_voice_mute_event(xp, vi, v);

    if (v->runtimeCache.runtimePhase == XP_PHASE_PRELOAD)
    {
        if ((v->waveControl_0000 & XP_WC_LDPCM_FORMAT) != 0)
            v->runtimeCache.waveSampleFormat = XP_FMT_LDPCM;
        else if ((v->waveControl_0000 & XP_WC_EXP0_OR_LIVE) != 0)
            v->runtimeCache.waveSampleFormat = XP_FMT_FCE_DPCM_EXP0;
        else
            v->runtimeCache.waveSampleFormat = XP_FMT_FCE_DPCM;

        if ((v->waveControl_0000 & XP_WC_ENGINE_HALT) == 0)
        {
            const int reverse = xp_voice_reverse(v);
            size_t preload_count = 8 - 1;
            if (xp_voice_16bit_bus(xp, v))
            {
                const size_t cons = (size_t)(v->addressFraction_0e00 & 0x0f);
                preload_count = reverse ? 7 + (v->sampleCurrent_0100 & 1)
                                        : 8 - (v->sampleCurrent_0100 & 1);
                if (cons != 0)
                {
                    const size_t available = (cons & 8) == 0 ? 0
                        : reverse ? ((cons - 9) | 1) + (v->sampleCurrent_0100 & 1)
                                  : (cons & 6);
                    if (available < preload_count)
                        preload_count = available;
                }
            }
            if (preload_count == 0)
            {
                v->waveControl_0000 |= XP_WC_PRODUCER_BLOCKED;
            }
            else
            {
                size_t i;
                v->waveFetchState_0400 = xp_read_exponent_cache(xp, v, v->sampleCurrent_0100);
                for (i = 0; i < preload_count; ++i)
                {
                    int looped;
                    v->waveCircularBuffer_0800[i] = xp_read_voice_sample(xp, v, v->sampleCurrent_0100);
                    looped = xp_advance_voice_address(xp, vi, v);
                    xp_refresh_exponent_cache(xp, v, looped);
                }
                v->waveFetchState_0400 = ((uint32_t)preload_count << 16) | (v->waveFetchState_0400 & 0xffff);
                v->waveControl_0000 |= XP_WC_EXP0_OR_LIVE;
            }
        }
        v->runtimeCache.runtimePhase = XP_PHASE_INITIALIZE;
        return 0;
    }

    if (v->runtimeCache.runtimePhase == XP_PHASE_INITIALIZE)
    {
        const uint32_t pitch = v->pitchCurrent_1b00;
        const uint32_t tvf_f = v->tvfFCurrent_1c00;
        uint32_t pitch_target;
        if (v->pitchRamp_1700 == 0x04005 && v->pitchDestination_1200 == v->pitchCurrent_1b00)
            v->pitchCurrent_1b00 &= ~XP_RAMP_ACK;
        if (v->tvfFRamp_1800 == 0x04005 && v->tvfFDestination_1300 == v->tvfFCurrent_1c00)
            v->tvfFCurrent_1c00 &= ~XP_RAMP_ACK;
        if (v->ampRamp_1a00 == 0x04005 && v->ampDestination_1500 == v->ampCurrent_1e00)
            v->ampCurrent_1e00 &= ~XP_RAMP_ACK;
        v->pitchIncrement_0d00  = xp_decode_pitch(pitch);
        v->tvfFCoefficient_2200 = (xp_decode_pitch(tvf_f) << 2) & XP_RAMP_SCRATCH_MASK;
        pitch_target = v->pitchDestination_1200 & ~XP_RAMP_ACK;
        if ((v->pitchRamp_1700 & XP_RAMP_HOLD) == 0
            && ((xp->state.sampleClock >> 3) & xp_ramp_divider_mask(v->pitchRamp_1700)) == 0
            && v->pitchCurrent_1b00 != pitch_target)
        {
            const uint32_t curve  = v->pitchRamp_1700 & XP_RAMP_CURVE_MASK;
            const uint8_t  parity = (uint8_t)(((xp->state.sampleClock >> 3) ^ 1) & 1);
            if (curve == 0x04000)
            {
                const uint32_t step = xp_linear_ramp_step(v->pitchCurrent_1b00, pitch_target, v->pitchRamp_1700);
                xp_step_linear_ramp(&v->pitchCurrent_1b00, pitch_target, step, parity);
            }
            else
                xp_step_log_ramp(&v->pitchCurrent_1b00, pitch_target, v->pitchRamp_1700, 1, parity);
        }
        v->playbackStateConfig_1000 = XP_PLAYBACK_STARTING | (v->playbackStateConfig_1000 & 0x1f);
        v->runtimeCache.runtimePhase = XP_PHASE_STARTING;
        return 0;
    }

    if (v->runtimeCache.runtimePhase == XP_PHASE_STARTING)
    {
        const uint32_t target = v->tvfFDestination_1300 & ~XP_RAMP_ACK;
        if ((v->tvfFRamp_1800 & XP_RAMP_HOLD) == 0
            && ((xp->state.sampleClock >> 3) & xp_ramp_divider_mask(v->tvfFRamp_1800)) == 0
            && v->tvfFCurrent_1c00 != target)
        {
            const uint32_t curve  = v->tvfFRamp_1800 & XP_RAMP_CURVE_MASK;
            const uint8_t  parity = (uint8_t)(((xp->state.sampleClock >> 3) ^ 1) & 1);
            if (curve == 0x04000)
            {
                const uint32_t step = xp_linear_ramp_step(v->tvfFCurrent_1c00, target, v->tvfFRamp_1800);
                xp_step_linear_ramp(&v->tvfFCurrent_1c00, target, step, parity);
            }
            else
                xp_step_log_ramp(&v->tvfFCurrent_1c00, target, v->tvfFRamp_1800, 1, parity);
        }
        v->playbackStateConfig_1000 = XP_PLAYBACK_RUNNING | (v->playbackStateConfig_1000 & 0x1f);
        v->runtimeCache.runtimePhase = XP_PHASE_RUNNING;
        return 0;
    }

    if (v->runtimeCache.runtimePhase != XP_PHASE_RUNNING)
        return 0;

    playback_counter = (v->playbackStateConfig_1000 + 1) & 7;
    v->playbackStateConfig_1000 = XP_PLAYBACK_RUNNING | (v->playbackStateConfig_1000 & 0x18) | playback_counter;

    if ((v->waveControl_0000 & (XP_WC_ENGINE_HALT | XP_WC_PRODUCER_BLOCKED)) != 0)
    {
        if (playback_counter == 6)
        {
            v->pitchIncrement_0d00  = xp_decode_pitch(v->pitchCurrent_1b00);
            v->tvfFCoefficient_2200 = (xp_decode_pitch(v->tvfFCurrent_1c00) << 2) & XP_RAMP_SCRATCH_MASK;
        }
        xp_step_voice_ramps(xp, vi, v);
        return 0;
    }

    phase           = xp_unpack_phase(v->addressFraction_0e00);
    interpolated    = xp_interpolate_voice(v, phase);
    dither_phase    = (uint8_t)((xp->state.sampleClock + 3) & 3);
    phase_increment = (v->pitchIncrement_0d00 >> 2)
                    + ((v->pitchIncrement_0d00 & 3) > xp_reverse_two_bits(dither_phase));
    next_phase      = phase + phase_increment;
    v->addressFraction_0e00 = xp_pack_phase(next_phase & XP_PHASE_MASK);
    cell_count      = (next_phase >> 14) - (phase >> 14);
    consumer        = (size_t)(phase >> 14);
    for (cell = 0; cell < cell_count; ++cell)
    {
        const int32_t delta = xp_decode_voice_delta(v, v->waveCircularBuffer_0800[(consumer + cell) & 7]);
        v->dpcmAccumulator_0c00 = (v->dpcmAccumulator_0c00 + (uint32_t)delta) & XP_DPCM_MASK;
    }
    if (xp_voice_16bit_bus(xp, v))
    {
        const uint32_t pair_alignment = (v->waveFetchState_0400 >> 16) & 1;
        const uint32_t phase_offset   = pair_alignment << 14;
        const uint32_t pair_count     = ((next_phase + phase_offset) >> 15) - ((phase + phase_offset) >> 15);
        uint32_t pair;
        for (pair = 0; pair < pair_count; ++pair)
            xp_refill_voice_pair(xp, vi, v);
    }
    else
    {
        for (cell = 0; cell < cell_count; ++cell)
            xp_refill_voice_cell(xp, vi, v);
    }
    if (playback_counter == 6)
    {
        v->pitchIncrement_0d00  = xp_decode_pitch(v->pitchCurrent_1b00);
        v->tvfFCoefficient_2200 = (xp_decode_pitch(v->tvfFCurrent_1c00) << 2) & XP_RAMP_SCRATCH_MASK;
    }
    xp_step_voice_ramps(xp, vi, v);
    *sample = interpolated;
    return 1;
}

/* ---- XP: events (xp.cpp) ---------------------------------------------- */

static int xp_accept_voice_event(xp_t *xp, size_t vi, uint8_t reason)
{
    if (xp->state.interrupt || xp->irqEventAcceptedThisStep)
    {
        xp->state.irqBlockedEvent = 1;
        return 0;
    }
    xp->irqEventAcceptedThisStep = 1;
    xp->state.irqStatus = (uint16_t)((vi << 8) | reason);
    xp->state.interrupt = 1;
    return 1;
}

static void xp_raise_voice_loop_marker_event(xp_t *xp, size_t vi, xp_voice_t *v)
{
    uint8_t  reason;
    uint16_t event;
    if ((v->waveControl_0000 & XP_WC_LOOP_EVENT_INHIBIT) != 0)
        return;
    if ((v->waveControl_0000 & XP_WC_IRQ_ENABLE) == 0
        || (xp->state.irqConfigMask & ((uint16_t)1 << 5)) == 0)
        return;
    reason = (v->waveControl_0000 & XP_WC_LOOP_REASON6) != 0
           ? XP_IRQ_ALT_PLAYBACK_MARKER : XP_IRQ_PLAYBACK_MARKER;
    event  = (uint16_t)((vi << 8) | reason);
    if (!(xp->state.interrupt && xp->state.irqStatus == event)
        && !xp_accept_voice_event(xp, vi, reason))
        return;
    v->waveControl_0000 |= (v->waveControl_0000 & XP_WC_LOOP_MARKER_FIRST) != 0
                         ? XP_WC_LOOP_EVENT_INHIBIT : XP_WC_LOOP_MARKER_FIRST;
}

static void xp_raise_ramp_terminal_event(xp_t *xp, size_t vi, uint32_t *control, uint8_t reason)
{
    if ((*control & XP_RAMP_EVENT_ARM) == 0
        || (xp->state.irqConfigMask & ((uint16_t)1 << reason)) == 0)
        return;
    if (!xp_accept_voice_event(xp, vi, reason))
        return;
    *control |= XP_RAMP_HOLD;
}

static void xp_raise_voice_mute_event(xp_t *xp, size_t vi, xp_voice_t *v)
{
    if ((v->waveControl_0000 & (XP_WC_MUTE_REQUEST | XP_WC_MUTE_STATUS)) != XP_WC_MUTE_REQUEST
        || (v->waveControl_0000 & XP_WC_IRQ_ENABLE) == 0
        || (xp->state.irqConfigMask & ((uint16_t)1 << XP_IRQ_MUTE_TRANSITION)) == 0)
        return;
    if (!xp_accept_voice_event(xp, vi, XP_IRQ_MUTE_TRANSITION))
        return;
    v->waveControl_0000 |= XP_WC_MUTE_STATUS;
}

static void xp_write_release_mask(xp_t *xp, size_t word, uint16_t value)
{
    size_t bit;
    for (bit = 0; bit < 16; ++bit)
    {
        xp_voice_t *v = &xp->state.voices[word * 16 + bit];
        const int released = (value & ((uint16_t)1 << bit)) != 0;
        if (v->resetState_3900.shadow && !released)
        {
            v->resetState_3900.released = 0;
            v->runtimeCache.runtimePhase = XP_PHASE_PARKED;
        }
        v->resetState_3900.shadow = released;
    }
}

static void xp_commit_released_voices(xp_t *xp)
{
    size_t i;
    for (i = 0; i < XP_MAX_VOICES; ++i)
    {
        xp_voice_t *v = &xp->state.voices[i];
        if (v->resetState_3900.shadow && !v->resetState_3900.released)
        {
            v->resetState_3900.released = 1;
            v->runtimeCache.runtimePhase = XP_PHASE_PRELOAD;
        }
    }
}

/* ---- XP: ramps -------------------------------------------------------- */

#define XP_TICK_DUE(control, counter) (((counter) & xp_ramp_divider_mask(control)) == 0)
#define XP_DEST_PENDING(destination)  (((destination) & XP_RAMP_ACK) == 0)

static void xp_step_voice_ramps(xp_t *xp, size_t vi, xp_voice_t *v)
{
    const uint8_t  playback_counter = (uint8_t)(v->playbackStateConfig_1000 & 7);
    const uint64_t slow_counter = xp->state.sampleClock >> 3;
    const uint64_t amp_counter  = xp->state.sampleClock >> 1;
    uint8_t  parity, curve;
    int      pending, held;
    uint32_t target, previous, destination;
    int32_t  accumulator, delta;
    int64_t  next;

    if ((playback_counter & 1) == 0)
    {
        switch (playback_counter)
        {
            case 6:
            {
                const int      pend = XP_DEST_PENDING(v->pitchDestination_1200);
                const uint32_t tgt  = v->pitchDestination_1200 & ~XP_RAMP_ACK;
                int linear;
                if (pend || v->pitchCurrent_1b00 != tgt)
                    v->pitchIncrement_0d00 = xp_decode_pitch(v->pitchCurrent_1b00);
                linear = (v->pitchRamp_1700 & XP_RAMP_CURVE_MASK) == 0x04000;
                if (pend)
                {
                    v->pitchDestination_1200 |= XP_RAMP_ACK;
                    if (linear)
                    {
                        v->pitchStep_2400 = xp_linear_ramp_step(v->pitchCurrent_1b00, tgt, v->pitchRamp_1700);
                        break;
                    }
                }
                if ((v->pitchRamp_1700 & XP_RAMP_HOLD) == 0 && XP_TICK_DUE(v->pitchRamp_1700, slow_counter))
                {
                    const uint8_t par = (uint8_t)((slow_counter ^ 1) & 1);
                    if (linear)
                        xp_step_linear_ramp(&v->pitchCurrent_1b00, tgt, v->pitchStep_2400, par);
                    else
                        xp_step_log_ramp(&v->pitchCurrent_1b00, tgt, v->pitchRamp_1700, 1, par);
                    if (v->pitchCurrent_1b00 == tgt)
                        xp_raise_ramp_terminal_event(xp, vi, &v->pitchRamp_1700, XP_IRQ_PITCH_TERMINAL);
                }
                break;
            }
            case 0:
            {
                const uint32_t tgt = v->ampModDestination_1400 & ~XP_RAMP_ACK;
                if (XP_DEST_PENDING(v->ampModDestination_1400))
                    v->ampModDestination_1400 |= XP_RAMP_ACK;
                if ((v->ampModRamp_1900 & XP_RAMP_HOLD) == 0 && XP_TICK_DUE(v->ampModRamp_1900, slow_counter))
                {
                    xp_step_log_ramp(&v->ampModCurrent_1d00, tgt, v->ampModRamp_1900, 1,
                                     (uint8_t)(slow_counter & 1));
                    if (v->ampModCurrent_1d00 == tgt)
                        xp_raise_ramp_terminal_event(xp, vi, &v->ampModRamp_1900, XP_IRQ_AMPMOD_TERMINAL);
                }
                break;
            }
            case 2:
            {
                const uint32_t tgt = ((v->tvfQDestination_1100 & ~XP_RAMP_ACK) << 2) & XP_RAMP_SCRATCH_MASK;
                if (XP_DEST_PENDING(v->tvfQDestination_1100))
                    v->tvfQDestination_1100 |= XP_RAMP_ACK;
                if ((v->tvfQRamp_1600 & XP_RAMP_HOLD) == 0 && XP_TICK_DUE(v->tvfQRamp_1600, slow_counter))
                {
                    xp_step_log_ramp(&v->tvfQCurrent_2100, tgt, v->tvfQRamp_1600, 4, 0);
                    if (v->tvfQCurrent_2100 == tgt)
                        xp_raise_ramp_terminal_event(xp, vi, &v->tvfQRamp_1600, XP_IRQ_TVFQ_TERMINAL);
                }
                break;
            }
            case 4:
            {
                const int      pend = XP_DEST_PENDING(v->tvfFDestination_1300);
                const uint32_t tgt  = v->tvfFDestination_1300 & ~XP_RAMP_ACK;
                int linear;
                if (pend || v->tvfFCurrent_1c00 != tgt)
                    v->tvfFCoefficient_2200 = (xp_decode_pitch(v->tvfFCurrent_1c00) << 2) & XP_RAMP_SCRATCH_MASK;
                linear = (v->tvfFRamp_1800 & XP_RAMP_CURVE_MASK) == 0x04000;
                if (pend)
                {
                    v->tvfFDestination_1300 |= XP_RAMP_ACK;
                    if (linear)
                    {
                        v->tvfFStep_2500 = xp_linear_ramp_step(v->tvfFCurrent_1c00, tgt, v->tvfFRamp_1800);
                        break;
                    }
                }
                if ((v->tvfFRamp_1800 & XP_RAMP_HOLD) == 0 && XP_TICK_DUE(v->tvfFRamp_1800, slow_counter))
                {
                    const uint8_t par = (uint8_t)((slow_counter ^ 1) & 1);
                    if (linear)
                        xp_step_linear_ramp(&v->tvfFCurrent_1c00, tgt, v->tvfFStep_2500, par);
                    else
                        xp_step_log_ramp(&v->tvfFCurrent_1c00, tgt, v->tvfFRamp_1800, 1, par);
                    if (v->tvfFCurrent_1c00 == tgt)
                        xp_raise_ramp_terminal_event(xp, vi, &v->tvfFRamp_1800, XP_IRQ_TVFF_TERMINAL);
                }
                break;
            }
            default:
                break;
        }
        return;
    }

    v->combinedAmp_2300 = xp_combine_amp(v);
    parity  = (uint8_t)((playback_counter >> 1) & 1);
    curve   = (uint8_t)((v->ampRamp_1a00 & XP_RAMP_CURVE_MASK) >> 14);
    pending = XP_DEST_PENDING(v->ampDestination_1500);
    if (curve == 2 && (v->runtimeCache.ampCurve2EntryPending || pending))
    {
        v->ampDestination_1500 = (v->ampCurrent_1e00 >> 1) | XP_RAMP_ACK;
        v->ampStep_2600 = 0;
        v->runtimeCache.ampCurve2EntryPending = 0;
        return;
    }
    target = v->ampDestination_1500 & ~XP_RAMP_ACK;
    held   = (v->ampRamp_1a00 & XP_RAMP_HOLD) != 0;
    if (pending && curve != 3 && !held)
    {
        v->ampDestination_1500 |= XP_RAMP_ACK;
        if (curve == 1)
        {
            v->ampStep_2600 = xp_linear_ramp_step(v->ampCurrent_1e00, target, v->ampRamp_1a00);
            return;
        }
    }
    if (held || !XP_TICK_DUE(v->ampRamp_1a00, amp_counter))
        return;
    if (curve == 0)
    {
        xp_step_log_ramp(&v->ampCurrent_1e00, target, v->ampRamp_1a00, 1, parity);
        if (v->ampCurrent_1e00 == target)
            xp_raise_ramp_terminal_event(xp, vi, &v->ampRamp_1a00, XP_IRQ_AMP_TERMINAL);
        return;
    }
    if (curve == 1)
    {
        xp_step_linear_ramp(&v->ampCurrent_1e00, target, v->ampStep_2600, parity);
        if (v->ampCurrent_1e00 == target)
            xp_raise_ramp_terminal_event(xp, vi, &v->ampRamp_1a00, XP_IRQ_AMP_TERMINAL);
        return;
    }
    previous    = v->ampCurrent_1e00;
    accumulator = xp_sign_extend(v->ampStep_2600, 20);
    next        = (int64_t)v->ampCurrent_1e00 + (int64_t)((accumulator + parity) >> 1);
    if (next < 0 || next > (int64_t)XP_RAMP_FIELD_MASK)
        next = 0;
    v->ampCurrent_1e00 = (uint32_t)next;
    if (curve == 2)
    {
        const uint32_t threshold = v->ampDestination_1500 & ~XP_RAMP_ACK;
        delta = previous > threshold ? -(int32_t)(v->ampRamp_1a00 & XP_RAMP_RATE_MASK)
                                     :  (int32_t)(v->ampRamp_1a00 & XP_RAMP_RATE_MASK);
        v->ampStep_2600 = (uint32_t)(accumulator + delta) & XP_RAMP_SCRATCH_MASK;
        if (previous != 0 && v->ampCurrent_1e00 == 0)
        {
            v->ampRamp_1a00 |= XP_RAMP_TRUNK_COMPLETE;
            xp_raise_ramp_terminal_event(xp, vi, &v->ampRamp_1a00, XP_IRQ_AMP_TERMINAL);
        }
        return;
    }
    destination = v->ampDestination_1500 & ~XP_RAMP_ACK;
    if (previous == 0)
    {
        v->ampStep_2600 = accumulator < 0
            ? (uint32_t)(accumulator + (int32_t)(v->ampRamp_1a00 & XP_RAMP_RATE_MASK)) & XP_RAMP_SCRATCH_MASK
            : XP_RAMP_SCRATCH_MASK;
        xp_raise_ramp_terminal_event(xp, vi, &v->ampRamp_1a00, XP_IRQ_AMP_TERMINAL);
        return;
    }
    if (v->ampCurrent_1e00 == 0)
    {
        xp_raise_ramp_terminal_event(xp, vi, &v->ampRamp_1a00, XP_IRQ_AMP_TERMINAL);
        return;
    }
    delta = previous > destination ? -(int32_t)(v->ampRamp_1a00 & XP_RAMP_RATE_MASK)
                                   :  (int32_t)(v->ampRamp_1a00 & XP_RAMP_RATE_MASK);
    v->ampStep_2600 = (uint32_t)(accumulator + delta) & XP_RAMP_SCRATCH_MASK;
}

/* ---- XP: filter and amplifier ---------------------------------------- */

/* The original divides by a power of two, which rounds toward zero. */
#define XP_DIV_POW2(value, bits) ((int64_t)(value) / ((int64_t)1 << (bits)))

static int32_t xp_step_voice_filter(xp_voice_t *v, int32_t input, unsigned mode)
{
    const int32_t  old_bp    = xp_sign_extend(v->filterBp_2800, 24);
    const int32_t  old_lp    = xp_sign_extend(v->filterLp_2900, 24);
    const uint32_t frequency = v->tvfFCoefficient_2200 & XP_RAMP_SCRATCH_MASK;
    const uint32_t damping   = v->tvfQCurrent_2100 & XP_RAMP_SCRATCH_MASK;
    const int32_t next_lp   = xp_saturate24((int64_t)old_lp + XP_DIV_POW2((int64_t)old_bp * frequency, 19));
    const int32_t high_pass = xp_saturate24((int64_t)input - next_lp - XP_DIV_POW2((int64_t)old_bp * damping, 19));
    const int32_t next_bp   = xp_saturate24((int64_t)old_bp + XP_DIV_POW2((int64_t)high_pass * frequency, 19));
    v->filterLp_2900 = (uint32_t)next_lp & XP_FILTER_MASK;
    v->filterBp_2800 = (uint32_t)next_bp & XP_FILTER_MASK;
    switch (mode & 3)
    {
        case 0:  return next_lp;
        case 1:  return next_bp;
        case 2:  return high_pass;
        default: return xp_saturate24((int64_t)high_pass - next_lp);
    }
}

static int32_t xp_apply_tva(const xp_voice_t *v, int32_t input)
{
    const uint32_t gain = (uint32_t)v->tvaGain_2700 << 4;
    return xp_saturate24(XP_DIV_POW2((int64_t)input * gain, 19));
}

SC88_INLINE int32_t xp_add24(int32_t a, int32_t b)
{
    return xp_saturate24((int64_t)a + b);
}

SC88_INLINE int32_t xp_boost16(int32_t value, unsigned booster)
{
    /* a left shift of a possibly negative value, done as a multiply */
    return xp_saturate16((int64_t)value * ((int64_t)1 << booster));
}

SC88_INLINE int32_t xp_ring(int32_t modulator, int32_t carrier)
{
    return xp_saturate24(XP_DIV_POW2((int64_t)xp_saturate16(modulator) * carrier, 15));
}

static void xp_step_paired_voice_filter(xp_voice_t *owner, xp_voice_t *partner,
                                        int32_t owner_input, int32_t partner_input)
{
    const unsigned structure    = (unsigned)((owner->filterConfig_2000 & XP_FC_STRUCTURE_MASK) >> 12) + 1;
    const unsigned owner_mode   = (unsigned)((owner->filterConfig_2000 & XP_FC_MODE_MASK) >> 10);
    const unsigned partner_mode = (unsigned)((owner->filterConfig_2000 & XP_FC_PAIRED_SECOND_MODE_MASK) >> 8);
    const unsigned booster      = (unsigned)((owner->filterConfig_2000 & XP_FC_BOOSTER_MASK) >> 6);
    int32_t output = 0;
    int32_t t;

    /* The order of the calls matters: each filter call advances that
     * voice's filter state, so nested calls are written out innermost
     * first, as the C++ evaluates them. */
    switch (structure)
    {
        case 2:
            t = xp_add24(xp_apply_tva(owner, owner_input), partner_input);
            t = xp_step_voice_filter(owner, t, owner_mode);
            output = xp_step_voice_filter(partner, t, partner_mode);
            break;
        case 3:
            t = xp_add24(xp_apply_tva(owner, owner_input), partner_input);
            t = xp_step_voice_filter(owner, t, owner_mode);
            output = xp_step_voice_filter(partner, xp_boost16(t, booster), partner_mode);
            break;
        case 4:
            t = xp_boost16(xp_add24(xp_apply_tva(owner, owner_input), partner_input), booster);
            t = xp_step_voice_filter(owner, t, owner_mode);
            output = xp_step_voice_filter(partner, t, partner_mode);
            break;
        case 5:
            t = xp_ring(xp_apply_tva(owner, owner_input), partner_input);
            t = xp_step_voice_filter(owner, t, owner_mode);
            output = xp_step_voice_filter(partner, t, partner_mode);
            break;
        case 6:
            t = xp_add24(xp_ring(xp_apply_tva(owner, owner_input), partner_input), partner_input);
            t = xp_step_voice_filter(owner, t, owner_mode);
            output = xp_step_voice_filter(partner, t, partner_mode);
            break;
        case 7:
            t = xp_step_voice_filter(owner, owner_input, owner_mode);
            t = xp_ring(xp_apply_tva(owner, t), partner_input);
            output = xp_step_voice_filter(partner, t, partner_mode);
            break;
        case 8:
            t = xp_step_voice_filter(owner, owner_input, owner_mode);
            t = xp_add24(xp_ring(xp_apply_tva(owner, t), partner_input), partner_input);
            output = xp_step_voice_filter(partner, t, partner_mode);
            break;
        case 9:
        {
            /* the two filters are separate state, so their order is free */
            const int32_t a = xp_apply_tva(owner, xp_step_voice_filter(owner, owner_input, owner_mode));
            const int32_t b = xp_step_voice_filter(partner, partner_input, partner_mode);
            output = xp_ring(a, b);
            break;
        }
        case 10:
        {
            const int32_t filtered_partner = xp_step_voice_filter(partner, partner_input, partner_mode);
            t = xp_step_voice_filter(owner, owner_input, owner_mode);
            output = xp_add24(xp_ring(xp_apply_tva(owner, t), filtered_partner), filtered_partner);
            break;
        }
        default:
            break;
    }
    owner->filterOutput_2a00   = (uint32_t)xp_apply_tva(partner, output) & XP_FILTER_MASK;
    partner->filterOutput_2a00 = 0;
}

static void xp_step_tva_gain(xp_voice_t *v)
{
    const uint16_t gain   = v->tvaGain_2700;
    const uint32_t target = (v->combinedAmp_2300 & XP_RAMP_SCRATCH_MASK) >> 4;
    v->tvaGain_2700 = (uint16_t)(((uint64_t)7 * gain + target + 3) >> 3);
}

/* ---- XP: effects DSP, shared operations (xp_dsp_ops.h) ---------------- */

/* A left shift that may carry into the sign: done on the unsigned type. */
#define XP_SHL64(x, n) ((int64_t)((uint64_t)(x) << (n)))

SC88_INLINE int64_t xpd_sign_extend(uint64_t value, unsigned bits)
{
    const uint64_t mask = ((uint64_t)1 << bits) - 1;
    const uint64_t sign = (uint64_t)1 << (bits - 1);
    const uint64_t v    = value & mask;
    return (int64_t)((v ^ sign) - sign);
}

SC88_INLINE int64_t xpd_saturate(int64_t value, unsigned bits)
{
    const int64_t minimum = -((int64_t)1 << (bits - 1));
    const int64_t maximum = ((int64_t)1 << (bits - 1)) - 1;
    if (value < minimum)
        return minimum;
    if (value > maximum)
        return maximum;
    return value;
}

SC88_INLINE uint32_t xpd_encode24(int64_t value)
{
    return (uint32_t)xpd_saturate(value, 24) & 0x00ffffff;
}

SC88_INLINE uint8_t xp_dsp_iram3_partition_count(uint16_t serial_audio1)
{
    return (uint8_t)((serial_audio1 >> 8) & 0x1f);
}

SC88_INLINE uint8_t xpd_iram3_parameter_read_boundary(uint8_t partition_count)
{
    return (uint8_t)(XP_DSP_IRAM_SLOTS - partition_count);
}

SC88_INLINE int64_t xpd_decode_cram_immediate(uint16_t cram)
{
    int64_t result = xpd_sign_extend(cram & 0x7fff, 15);
    if ((cram & 0x8000) != 0)
        result = XP_SHL64(result, 13);
    return result;
}

static const uint8_t xpd_coef_shifts[4] = { 0, 1, 2, 4 };

SC88_INLINE int64_t xpd_cram_coefficient(uint16_t cram)
{
    return XP_SHL64(xpd_sign_extend(cram & 0x3fff, 14), xpd_coef_shifts[cram >> 14]);
}

SC88_INLINE int xpd_multiply_negative_fraction(int64_t numerator, unsigned shift)
{
    return numerator < 0 && (numerator & (((int64_t)1 << shift) - 1)) != 0;
}

SC88_INLINE uint32_t *xpd_mixer_iram(xp_dsp_state_t *s)
{
    return s->iramSelPhase ? s->iram2 : s->iram1;
}

SC88_INLINE uint32_t *xpd_processing_iram(xp_dsp_state_t *s)
{
    return s->iramSelPhase ? s->iram1 : s->iram2;
}

SC88_INLINE int64_t xpd_read_iram(const xp_dsp_state_t *s, uint8_t memaddr)
{
    if (memaddr < 0x80)
    {
        const int bank = ((memaddr >> 6) ^ s->iramSelPhase) != 0;
        return xpd_sign_extend((bank ? s->iram2 : s->iram1)[memaddr & 0x3f], 24);
    }
    if (memaddr < 0xc0)
    {
        const uint32_t *bank = (memaddr & 0x20) != 0 ? s->iram2 : s->iram1;
        return xpd_sign_extend(bank[0x20 + (memaddr & 0x1f)], 24);
    }
    return xpd_sign_extend(s->iram3[memaddr & 0x3f], 24);
}

SC88_INLINE void xpd_write_iram(xp_dsp_state_t *s, uint8_t memaddr, int64_t value)
{
    if (memaddr < 0x80)
    {
        const int bank = ((memaddr >> 6) ^ s->iramSelPhase) != 0;
        (bank ? s->iram2 : s->iram1)[memaddr & 0x3f] = xpd_encode24(value);
        return;
    }
    if (memaddr < 0xc0)
    {
        uint32_t *bank = (memaddr & 0x20) != 0 ? s->iram2 : s->iram1;
        bank[0x20 + (memaddr & 0x1f)] = xpd_encode24(value);
        return;
    }
    s->iram3[memaddr & 0x3f] = xpd_encode24(value);
}

SC88_INLINE void xpd_queue_eram_read(xp_dsp_state_t *s, int32_t value, int countdown)
{
    int i;
    for (i = 0; i < 2; ++i)
    {
        if (s->pendingEramReads[i].countdown >= 0)
            continue;
        s->pendingEramReads[i].value     = value;
        s->pendingEramReads[i].countdown = countdown;
        return;
    }
}

SC88_INLINE void xpd_advance_eram_reads(xp_dsp_state_t *s)
{
    int i;
    for (i = 0; i < 2; ++i)
    {
        xp_pending_eram_read_t *p = &s->pendingEramReads[i];
        if (p->countdown < 0 || --p->countdown != 0)
            continue;
        s->eramReadLatch = p->value;
        p->countdown     = -1;
    }
}

SC88_INLINE void xpd_consume_serial_input(xp_dsp_state_t *s, size_t port)
{
    if (s->serialInputIndex[port] < s->serialInputCount[port])
        s->serialInputNode[port] = s->serialInput[port][s->serialInputIndex[port]++];
}

static void xpd_execute_io(xp_dsp_state_t *s, uint8_t io_ctrl, int serial_state_enabled,
                           int serial_output_enabled, uint16_t serial_audio0, uint16_t serial_audio1)
{
    if (io_ctrl == 1)
    {
        if (serial_state_enabled)
        {
            if (serial_output_enabled && s->serialOutputCount[0] < XP_DSP_SERIAL_WORDS)
            {
                const uint32_t word = s->outputWordPosition & (XP_DSP_IRAM_SLOTS - 1);
                s->serialOutput[0][s->serialOutputCount[0]++] =
                    (int32_t)xpd_sign_extend(xpd_processing_iram(s)[word], 24);
            }
            ++s->outputWordPosition;
        }
    }
    else if (io_ctrl == 2)
    {
        if (serial_state_enabled)
        {
            const size_t   bus  = 1 + (size_t)s->dacPortPosition;
            const uint32_t word = s->outputWordPosition & (XP_DSP_IRAM_SLOTS - 1);
            const uint8_t  descriptor = s->dacPortPosition == 0 ? (uint8_t)(serial_audio0 >> 8)
                                                               : (uint8_t)serial_audio1;
            if (serial_output_enabled && (descriptor & 0xc0) != 0
                && s->serialOutputCount[bus] < XP_DSP_SERIAL_WORDS)
                s->serialOutput[bus][s->serialOutputCount[bus]++] =
                    (int32_t)xpd_sign_extend(xpd_processing_iram(s)[word], 24);
            ++s->dacPortPosition;
            ++s->outputWordPosition;
            if (s->dacPortPosition == 3)
                s->dacPortPosition = 0;
        }
    }
    else if (io_ctrl >= 4)
    {
        const uint8_t levels = (uint8_t)(io_ctrl & 3);
        if ((s->outputPins & 1) != 0 && (levels & 1) == 0)
            s->outputPins |= 4;
        s->outputPins = (uint8_t)((s->outputPins & 4) | levels);
    }
}

SC88_INLINE void xpd_replace_mixer(xp_dsp_state_t *s, size_t destination, int64_t value)
{
    if (destination >= XP_DSP_IRAM_SLOTS)
        return;
    xpd_mixer_iram(s)[destination] = xpd_encode24(value);
    s->mixerInitialized |= (uint64_t)1 << destination;
}

SC88_INLINE void xpd_accumulate_mixer(xp_dsp_state_t *s, size_t destination, int64_t value)
{
    int64_t current;
    if (destination >= XP_DSP_IRAM_SLOTS)
        return;
    if ((s->mixerInitialized & ((uint64_t)1 << destination)) == 0)
        xpd_replace_mixer(s, destination, 0);
    current = xpd_sign_extend(xpd_mixer_iram(s)[destination], 24);
    xpd_mixer_iram(s)[destination] = xpd_encode24(current + value);
}

SC88_INLINE void xpd_step_mixer_cycle(xp_dsp_state_t *s, const xp_mixer_frame_t *frame, size_t cycle)
{
    const size_t voice = cycle >> 2;
    const size_t phase = cycle & 3;
    const xp_mixer_send_t *e = frame->v[voice];
    if (phase == 0)
        xpd_accumulate_mixer(s, e[0].destination, e[0].contribution);
    else if (phase == 2)
    {
        xpd_accumulate_mixer(s, e[1].destination, e[1].contribution);
        xpd_accumulate_mixer(s, e[2].destination, e[2].contribution);
    }
    else if (phase == 3)
        xpd_accumulate_mixer(s, e[3].destination, e[3].contribution);
}

static void xpd_begin_frame(xp_dsp_state_t *s, xp_dsp_context_t *ctx, const xp_dsp_request_t *request)
{
    ctx->request = *request;
    ctx->cycle   = 0;
    ctx->pc      = 0;
    if (ctx->request.executionSlots > XP_DSP_EXEC_SLOTS)
        ctx->request.executionSlots = XP_DSP_EXEC_SLOTS;
    s->serialInputIndex[0] = s->serialInputIndex[1] = 0;
    s->serialOutputCount[0] = s->serialOutputCount[1] = 0;
    s->serialOutputCount[2] = s->serialOutputCount[3] = 0;
    s->outputWordPosition = 0;
    s->dacPortPosition    = 0;
    s->outputPins        &= 3;
}

static void xpd_end_frame(xp_dsp_state_t *s, const xp_dsp_context_t *ctx)
{
    if (ctx->request.executeProgram)
        s->eramPos = (s->eramPos - 1) & 0xffff;
    s->iramSelPhase = !s->iramSelPhase;
}

/* ---- XP: effects DSP, interpreter (xp_dsp_naive.cpp) ------------------ */

static int xpd_execute_eram(xp_dsp_state_t *s, uint16_t eram)
{
    if (s->eramPrefixPending)
    {
        const uint32_t offset  = ((uint32_t)s->eramOffsetHigh << 9) | (eram & 0x01ff);
        const uint32_t address = (s->eramPos + offset) & 0xffff;
        if (s->eramPendingWrite)
            s->eram[address] = xpd_encode24(s->eramPendingWriteValue);
        else
            xpd_queue_eram_read(s, (int32_t)xpd_sign_extend(s->eram[address], 24), 1);
        s->eramPrefixPending = 0;
        return 1;
    }
    if ((eram & 0x0180) != 0)
    {
        s->eramPrefixPending     = 1;
        s->eramPendingWrite      = (eram & 0x0100) != 0;
        s->eramOffsetHigh        = (uint8_t)(eram & 0x7f);
        s->eramPendingWriteValue = (eram & 0x0080) != 0 ? s->accumulator : s->iramReadLatch;
        return 0;
    }
    return 0;
}

#define XPD_MIN(a, b) ((a) < (b) ? (a) : (b))
#define XPD_MAX(a, b) ((a) > (b) ? (a) : (b))

static int64_t xpd_execute_primary(const xp_dsp_state_t *s, uint8_t function, uint8_t mode,
                                   uint16_t cram, int64_t acc)
{
    switch (function)
    {
        case 1:  return acc;
        case 2:  return acc + s->iramReadLatch;
        case 3:  return acc + s->multiplyResultLatch;
        case 4:  return s->iramReadLatch;
        case 5:  return s->multiplyResultLatch;
        case 6:  return -acc;
        case 7:  return s->iramReadLatch - acc;
        case 8:  return s->multiplyResultLatch - acc;
        case 9:  return s->iramReadLatch + s->multiplyResultLatch;
        case 10: return XPD_MIN(acc, s->iramReadLatch);
        case 11: return XPD_MAX(acc, s->iramReadLatch);
        case 12:
            if (mode == 2)
                return acc + (s->multiplyResultLatch >> 13);
            return mode == 3 ? s->multiplyResultLatch >> 13 : acc;
        case 13:
        {
            const int64_t accumulator_mask = ((int64_t)1 << 29) - 1;
            int64_t immediate, a;
            if (mode == 0)
                return acc;
            immediate = (cram & 0x8000) != 0 ? (int64_t)(cram & 0x7fff) << 13
                                             : xpd_sign_extend(cram & 0x7fff, 15) & 0x00ffffff;
            immediate &= accumulator_mask;
            a = acc & accumulator_mask;
            if (mode == 1)
                return a & immediate;
            return mode == 2 ? (a | immediate) : (a ^ immediate);
        }
        case 14:
        {
            const int64_t immediate = xpd_decode_cram_immediate(cram);
            int same_sign;
            if (mode == 0)
                return XPD_MIN(acc, immediate);
            same_sign = (acc < 0) == (immediate < 0);
            if (mode == 2)
                return same_sign ? XPD_MIN(acc, immediate) : XPD_MAX(acc, immediate);
            if (mode == 3)
                return same_sign ? XPD_MAX(acc, immediate) : XPD_MIN(acc, immediate);
            return XPD_MAX(acc, immediate);
        }
        case 15:
        {
            const int64_t immediate = xpd_decode_cram_immediate(cram);
            switch (mode)
            {
                case 0:  return acc + immediate;
                case 1:  return s->iramReadLatch + immediate;
                case 2:  return s->multiplyResultLatch + immediate;
                default: return -acc + immediate;
            }
        }
        default:
            return acc;
    }
}

static int64_t xpd_multiply_source(const xp_dsp_state_t *s, int64_t acc, unsigned select)
{
    switch (select & 3)
    {
        case 0:  return s->multiplyFeedbackLatch;
        case 1:  return xpd_saturate(acc, 24);
        case 2:  return s->iramReadLatch;
        default: return s->eramReadLatch;
    }
}

static int64_t xpd_execute_parallel(const xp_dsp_state_t *s, uint16_t cram, int64_t acc,
                                    int *multiply_issued, int64_t *next_multiply,
                                    int *next_negative_fraction, int64_t *multiply_input)
{
    int64_t result;

    if ((cram & 0x0200) != 0)
    {
        const int64_t saturated     = xpd_saturate(acc, 24);
        const uint8_t factor_select = (uint8_t)((cram >> 6) & 3);
        int64_t factor = 0;
        int64_t numerator;
        *multiply_input = xpd_multiply_source(s, acc, (unsigned)((cram >> 4) & 3));
        switch (factor_select)
        {
            case 0: factor = (saturated & 0x0fff) << 3; break;
            case 1: factor = (saturated & 0x7fffff) >> 8; break;
            case 2: factor = saturated >> 8; break;
            case 3: factor = xpd_sign_extend(s->iram3ParameterLatch, 16); break;
        }
        if ((cram & 0x0100) != 0)
        {
            factor = 0x7fff - factor;
            if (factor_select >= 2)
                factor -= 0x8000;
        }
        numerator = XP_SHL64(*multiply_input * factor, xpd_coef_shifts[cram >> 14]);
        *next_multiply          = xpd_saturate(numerator / 32768, 29);
        *next_negative_fraction = xpd_multiply_negative_fraction(numerator, 15);
        *multiply_issued        = 1;
    }

    result = acc;
    switch (cram & 0x0f)
    {
        case 0:  result = acc + s->multiplyResultLatch + s->iramReadLatch; break;
        case 2:  result = acc + s->iramReadLatch; break;
        case 3:  result = acc + s->multiplyResultLatch; break;
        case 4:  result = s->iramReadLatch; break;
        case 5:  result = s->multiplyResultLatch; break;
        case 6:  result = -acc; break;
        case 7:  result = s->iramReadLatch - acc; break;
        case 8:  result = s->multiplyResultLatch - acc; break;
        case 9:  result = s->iramReadLatch + s->multiplyResultLatch; break;
        case 10: result = XPD_MIN(acc, s->iramReadLatch); break;
        case 11: result = XPD_MAX(acc, s->iramReadLatch); break;
        case 12: result = s->iramReadLatch + s->multiplyResultLatch - acc; break;
        case 13: result = acc + s->multiplyResultLatch - s->iramReadLatch
                        - (s->multiplyNegativeFraction ? 1 : 0); break;
        case 14: result = s->multiplyResultLatch - s->iramReadLatch - acc
                        - (s->multiplyNegativeFraction ? 2 : 0); break;
        case 15: result = s->multiplyResultLatch - s->iramReadLatch
                        - (s->multiplyNegativeFraction ? 2 : 0); break;
        default: break;
    }
    switch ((cram >> 11) & 7)
    {
        case 1:
            if (result < 0)
                result = -result;
            break;
        case 2:
            result = xpd_sign_extend((uint64_t)result, 24);
            break;
        case 3:
            result = XP_SHL64(result, 1) | (((result >> 23) ^ (result >> 6) ^ (result >> 1)) & 1);
            break;
        case 4:
            result &= 0x00ffffff;
            if ((((result >> 23) ^ (result >> 22)) & 1) != 0)
                result ^= 0x007fffff;
            result = xpd_sign_extend((uint64_t)result, 24);
            break;
        case 5:
            result = xpd_sign_extend((uint64_t)result, 24);
            if (result < 0)
                result = ~result;
            break;
        default:
            break;
    }
    if ((cram & 0x0400) != 0)
        result = xpd_sign_extend((uint64_t)result, 24);
    return result;
}

static int xpd_branch_taken(const xp_dsp_state_t *s, uint8_t condition)
{
    switch (condition)
    {
        case 0:  return s->accumulator == 0;
        case 1:  return s->accumulator != 0;
        case 3: case 5: case 13: case 14:
            return 1;
        case 6: case 8:  return s->accumulator >= 0;
        case 7: case 9:  return s->accumulator < 0;
        case 10: return s->accumulator > 0;
        case 11: return s->accumulator <= 0;
        default: return 0;
    }
}

/* One slot of the frame.  Returns 0 when the frame's slots are used up. */
static int xpd_execute_slot(xp_dsp_state_t *s, const xp_dsp_program_t *program,
                            xp_dsp_context_t *ctx, int consume_staged_bus_a)
{
    size_t   pc, branch_target = 0;
    uint32_t word;
    uint8_t  io_ctrl, store, memaddr, op, mode, function;
    uint8_t  parameter_read_boundary;
    uint16_t eram, cram, next_parameter_latch;
    int      eram_port_busy, loads_iram_read_latch = 0, selects_iram3;
    int      multiply_issued = 0, next_negative_fraction = 0, branch = 0;
    int64_t  iram_read_value = 0, next_multiply = 0, multiply_input = 0, next_accumulator;

    if (ctx->cycle >= ctx->request.executionSlots)
        return 0;
    if (ctx->request.mixerFrame)
        xpd_step_mixer_cycle(s, ctx->request.mixerFrame, ctx->cycle);
    if (!ctx->request.executeProgram || ctx->pc >= XP_DSP_PROGRAM_SLOTS)
    {
        ++ctx->cycle;
        return 1;
    }

    pc      = ctx->pc;
    word    = program->pram[pc] & 0x0fffffff;
    io_ctrl = (uint8_t)((word >> 25) & 7);
    eram    = (uint16_t)((word >> 16) & 0x01ff);
    store   = (uint8_t)((word >> 14) & 3);
    memaddr = (uint8_t)((word >> 6) & 0xff);
    op      = (uint8_t)(word & 0x3f);
    cram    = program->cram[pc];
    parameter_read_boundary =
        xpd_iram3_parameter_read_boundary(xp_dsp_iram3_partition_count(ctx->request.serialAudio1Config));

    xpd_advance_eram_reads(s);
    eram_port_busy = xpd_execute_eram(s, eram);

    selects_iram3 = memaddr >= 0xc0;
    if (store == 1)
    {
        iram_read_value       = xpd_read_iram(s, memaddr);
        loads_iram_read_latch = !(selects_iram3 && (memaddr & 0x3f) >= parameter_read_boundary);
    }
    else if (store == 2)
        xpd_write_iram(s, memaddr, s->eramReadLatch);
    else if (store == 3)
        xpd_write_iram(s, memaddr, s->accumulator);

    xpd_execute_io(s, io_ctrl, ctx->request.serialInputEnabled,
                   ctx->request.serialInputEnabled && ctx->request.serialOutputEnabled,
                   ctx->request.serialAudio0Config, ctx->request.serialAudio1Config);

    mode     = (uint8_t)(op >> 4);
    function = (uint8_t)(op & 0x0f);
    if (function >= 1 && function <= 12)
    {
        int64_t coefficient_input, numerator;
        multiply_input    = xpd_multiply_source(s, s->accumulator, mode);
        coefficient_input = multiply_input;
        if (function == 12)
        {
            if (mode < 2)
                coefficient_input = s->serialInputNode[mode == 0 ? 0 : 1];
            else
                coefficient_input = s->multiplyFeedbackLatch;
        }
        numerator              = coefficient_input * xpd_cram_coefficient(cram);
        next_multiply          = xpd_saturate(numerator / 8192, 29);
        next_negative_fraction = xpd_multiply_negative_fraction(numerator, 13);
        multiply_issued        = 1;
    }

    next_parameter_latch = s->iram3ParameterLatch;
    if (store == 1 && selects_iram3 && (memaddr & 0x3f) >= parameter_read_boundary)
        next_parameter_latch = (uint16_t)((s->iram3[memaddr & 0x3f] >> 10) & 0xffff);

    next_accumulator = s->accumulator;
    if (function != 0)
        next_accumulator = xpd_execute_primary(s, function, mode, cram, s->accumulator);
    else if (op == 0x30)
        next_accumulator = xpd_execute_parallel(s, cram, s->accumulator, &multiply_issued,
                                                &next_multiply, &next_negative_fraction, &multiply_input);
    else if (op == 0x20 && !eram_port_busy)
    {
        uint32_t address;
        s->eramIndexedOffset = (uint16_t)((s->accumulator >> 12) & 0xffff);
        address = (s->eramPos + s->eramIndexedOffset) & 0xffff;
        xpd_queue_eram_read(s, (int32_t)xpd_sign_extend(s->eram[address], 24), 2);
    }
    else if (op == 0x10)
    {
        const uint8_t branch_control = (uint8_t)((cram >> 8) & 0x3f);
        branch        = xpd_branch_taken(s, (uint8_t)(branch_control >> 2));
        branch_target = (branch_control & 2) != 0 ? pc + 1 + (cram & 0xff) : (size_t)(cram & 0xff);
    }

    s->accumulator = xpd_sign_extend((uint64_t)next_accumulator, 29);
    if (store == 1)
        s->iram3ParameterLatch = next_parameter_latch;
    if (loads_iram_read_latch)
        s->iramReadLatch = iram_read_value;
    if (multiply_issued)
    {
        s->multiplyResultLatch      = next_multiply;
        s->multiplyNegativeFraction = (uint8_t)next_negative_fraction;
        s->multiplyFeedbackLatch    = multiply_input;
    }
    if (ctx->request.serialInputEnabled && consume_staged_bus_a && io_ctrl == 1)
        xpd_consume_serial_input(s, 0);
    else if (ctx->request.serialInputEnabled && io_ctrl == 2 && s->dacPortPosition == 1)
        xpd_consume_serial_input(s, 1);

    ctx->pc = branch ? branch_target : pc + 1;
    ++ctx->cycle;
    return 1;
}

/* ---- XP: effects DSP, program bookkeeping (xp_dsp.cpp, xp_dsp_program.cpp) */

static int xpd_cram_write_is_structural(uint32_t pram)
{
    const uint8_t op = (uint8_t)(pram & 0x3f);
    return (op & 0x0f) == 0 && (op == 0x10 || op == 0x30);
}

static int xpd_pram_write_is_structural(uint32_t old_pram, uint32_t new_pram)
{
    const uint32_t eram_mask         = 0x01ff0000;
    const uint32_t command_kind_mask = 0x01800000;
    if (((old_pram ^ new_pram) & ~eram_mask & 0x0fffffff) != 0)
        return 1;
    return ((old_pram ^ new_pram) & command_kind_mask) != 0;
}

static void xp_dsp_write_pram(xp_dsp_t *d, size_t slot, uint32_t value)
{
    uint32_t previous;
    if (slot >= XP_DSP_PROGRAM_SLOTS)
        return;
    previous = d->program.pram[slot];
    d->program.pram[slot] = value & 0x0fffffff;
    if (d->programTainted)
        return;
    if (xpd_pram_write_is_structural(previous, d->program.pram[slot]))
        d->programTainted = 1;
}

static void xp_dsp_write_cram(xp_dsp_t *d, size_t slot, uint16_t value)
{
    if (slot >= XP_DSP_PROGRAM_SLOTS)
        return;
    d->program.cram[slot] = value;
    if (d->programTainted)
        return;
    if (xpd_cram_write_is_structural(d->program.pram[slot]))
        d->programTainted = 1;
}

static void xpd_frame_config(xp_dsp_config_t *c, const xp_dsp_request_t *request,
                             int consume_staged_bus_a, int linked)
{
    c->linked              = (uint8_t)linked;
    c->serialInputEnabled  = (uint8_t)request->serialInputEnabled;
    c->serialOutputEnabled = (uint8_t)(request->serialInputEnabled && request->serialOutputEnabled);
    c->consumeStagedBusA   = (uint8_t)consume_staged_bus_a;
    c->bcdEnabled          = (uint8_t)((((request->serialAudio0Config >> 8) & 0xc0) != 0 ? 1 : 0)
                                     | ((request->serialAudio1Config & 0xc0) != 0 ? 6 : 0));
    c->iram3ParameterReadBoundary =
        xpd_iram3_parameter_read_boundary(xp_dsp_iram3_partition_count(request->serialAudio1Config));
    c->executionSlots = (uint16_t)(request->executionSlots < XP_DSP_EXEC_SLOTS
                                   ? request->executionSlots : XP_DSP_EXEC_SLOTS);
}

/* What the lowering needs to know of one slot to decide which mixer
 * cells the program touches and whether its branches can be flattened. */
enum { XPD_ARM_NONE, XPD_ARM_READ, XPD_ARM_WRITE_LATCH, XPD_ARM_WRITE_ACC };
enum { XPD_BANK_MIXER, XPD_BANK_PROCESSING, XPD_BANK_DIRECT1, XPD_BANK_DIRECT2, XPD_BANK_IRAM3 };
enum { XPD_OP_OTHER, XPD_OP_INDEXED_ERAM_READ, XPD_OP_BRANCH };

typedef struct
{
    uint8_t  ioCtrl;
    uint8_t  eramArm;
    uint8_t  transfer;       /* non-zero: the slot reads or writes IRAM */
    uint8_t  bank;
    uint8_t  index;
    uint8_t  op;
    uint8_t  branchLive;     /* a branch that can be taken */
    uint16_t branchTarget;
} xpd_slot_t;

static void xpd_decode_slot(uint32_t pram, uint16_t cram, size_t pc, xpd_slot_t *slot)
{
    /* condition codes 2, 4, 12 and 15 never branch */
    static const uint8_t live[16] = { 1, 1, 0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 0 };
    const uint32_t word    = pram & 0x0fffffff;
    const uint16_t eram    = (uint16_t)((word >> 16) & 0x01ff);
    const uint8_t  store   = (uint8_t)((word >> 14) & 3);
    const uint8_t  memaddr = (uint8_t)((word >> 6) & 0xff);
    const uint8_t  op      = (uint8_t)(word & 0x3f);

    slot->ioCtrl = (uint8_t)((word >> 25) & 7);
    if (memaddr < 0x80)
    {
        slot->bank  = (memaddr & 0x40) != 0 ? XPD_BANK_PROCESSING : XPD_BANK_MIXER;
        slot->index = (uint8_t)(memaddr & 0x3f);
    }
    else if (memaddr < 0xc0)
    {
        slot->bank  = (memaddr & 0x20) != 0 ? XPD_BANK_DIRECT2 : XPD_BANK_DIRECT1;
        slot->index = (uint8_t)(0x20 + (memaddr & 0x1f));
    }
    else
    {
        slot->bank  = XPD_BANK_IRAM3;
        slot->index = (uint8_t)(memaddr & 0x3f);
    }
    slot->eramArm = XPD_ARM_NONE;
    if ((eram & 0x0100) != 0)
        slot->eramArm = (eram & 0x0080) != 0 ? XPD_ARM_WRITE_ACC : XPD_ARM_WRITE_LATCH;
    else if ((eram & 0x0080) != 0)
        slot->eramArm = XPD_ARM_READ;
    slot->transfer     = store != 0;
    slot->op           = XPD_OP_OTHER;
    slot->branchLive   = 0;
    slot->branchTarget = 0;
    if ((op & 0x0f) != 0)
        return;
    if (op == 0x10)
    {
        const uint8_t  control = (uint8_t)((cram >> 8) & 0x3f);
        const uint16_t target  = (uint16_t)(cram & 0xff);
        slot->op           = XPD_OP_BRANCH;
        slot->branchLive   = live[control >> 2];
        slot->branchTarget = (control & 2) != 0 ? (uint16_t)(pc + 1 + target) : target;
    }
    else if (op == 0x20)
        slot->op = XPD_OP_INDEXED_ERAM_READ;
}

SC88_INLINE int xpd_slot_has_event(const xpd_slot_t *slot)
{
    return slot->ioCtrl == 1 || slot->ioCtrl == 2 || slot->eramArm != XPD_ARM_NONE
        || slot->op == XPD_OP_INDEXED_ERAM_READ;
}

SC88_INLINE void xpd_note_mixer_cell(uint64_t *cells, const xpd_slot_t *slot)
{
    uint64_t bit;
    if (!slot->transfer)
        return;
    bit = (uint64_t)1 << slot->index;
    switch (slot->bank)
    {
        case XPD_BANK_MIXER:   cells[0] |= bit; cells[1] |= bit; break;
        case XPD_BANK_DIRECT1: cells[0] |= bit; break;
        case XPD_BANK_DIRECT2: cells[1] |= bit; break;
        default: break;
    }
}

static int xpd_jumps_eligible(const xpd_slot_t *slots, const xp_dsp_config_t *config, uint16_t *last_slot)
{
    const size_t slot_count = config->executionSlots;
    size_t skip_max = 0, pc, limit;
    int    any = 0;
    for (pc = 0; pc < XP_DSP_PROGRAM_SLOTS; ++pc)
    {
        const xpd_slot_t *slot = &slots[pc];
        size_t target, skipped;
        if (slot->op != XPD_OP_BRANCH || !slot->branchLive)
            continue;
        any    = 1;
        target = slot->branchTarget;
        if (target <= pc || target >= XP_DSP_PROGRAM_SLOTS)
            return 0;
        for (skipped = pc + 1; skipped < target; ++skipped)
            if (xpd_slot_has_event(&slots[skipped]))
                return 0;
        skip_max += target - pc - 1;
    }
    if (!any || slot_count < 2)
        return 0;
    limit = slot_count - 1 + skip_max;
    if (limit > XP_DSP_PROGRAM_SLOTS - 1)
        limit = XP_DSP_PROGRAM_SLOTS - 1;
    *last_slot = (uint16_t)limit;
    for (pc = slot_count - 2; pc <= *last_slot; ++pc)
        if (xpd_slot_has_event(&slots[pc]))
            return 0;
    return 1;
}

/* The static lowering, followed only as far as it decides which slots
 * it covers and whether it gives up.  Returns 0 where the C++ does. */
static int xpd_lower_static(const xpd_slot_t *slots, const xp_dsp_config_t *config,
                            int jumps, uint16_t last_slot, uint64_t *cells)
{
    const uint16_t slot_count = config->executionSlots;
    const uint16_t emit_count = (uint16_t)(jumps ? last_slot + 1 : slot_count);
    uint16_t resync = 0, static_start, pc;
    int      pending_kind = XPD_ARM_NONE;
    int      queue[2];
    int      e;

    queue[0] = queue[1] = -1;
    while (resync < slot_count && slots[resync].eramArm != XPD_ARM_NONE)
        ++resync;
    static_start = (uint16_t)(resync + 1 < slot_count ? resync + 1 : slot_count);
    while (static_start < slot_count && slots[static_start - 1].op == XPD_OP_INDEXED_ERAM_READ)
        ++static_start;

    for (pc = 0; pc < emit_count; ++pc)
    {
        const xpd_slot_t *slot = &slots[pc];
        const int generic = pc < static_start;
        int port_busy = 0;
        xpd_note_mixer_cell(cells, slot);
        if (!generic)
        {
            for (e = 0; e < 2; ++e)
            {
                if (queue[e] < 0 || --queue[e] != 0)
                    continue;
                queue[e] = -1;
            }
        }
        if (jumps && slot->op == XPD_OP_BRANCH && slot->branchLive)
        {
            if (generic || pending_kind != XPD_ARM_NONE || slot->eramArm != XPD_ARM_NONE
                || queue[0] >= 0 || queue[1] >= 0)
                return 0;
        }
        if (pc > resync)
        {
            if (pending_kind != XPD_ARM_NONE)
            {
                if (!generic && pending_kind == XPD_ARM_READ)
                    queue[queue[0] < 0 ? 0 : 1] = 1;
                pending_kind = XPD_ARM_NONE;
                port_busy    = 1;
            }
            else if (slot->eramArm != XPD_ARM_NONE)
                pending_kind = slot->eramArm;
        }
        if (slot->op == XPD_OP_INDEXED_ERAM_READ && !generic && !port_busy)
            queue[queue[0] < 0 ? 0 : 1] = 2;
    }
    return 1;
}

static void xpd_lower(xp_dsp_t *d, const xp_dsp_config_t *config)
{
    xpd_slot_t slots[XP_DSP_PROGRAM_SLOTS];
    size_t   pc;
    int      branches = 0;
    uint16_t last_slot = 0;

    d->flatConfig    = *config;
    d->mixerCells[0] = d->mixerCells[1] = 0;
    for (pc = 0; pc < XP_DSP_PROGRAM_SLOTS; ++pc)
        xpd_decode_slot(d->program.pram[pc], d->program.cram[pc], pc, &slots[pc]);
    for (pc = 0; pc < config->executionSlots; ++pc)
        if (slots[pc].op == XPD_OP_BRANCH && slots[pc].branchLive)
            branches = 1;
    if (!branches)
    {
        xpd_lower_static(slots, config, 0, 0, d->mixerCells);
        return;
    }
    if (!config->linked && xpd_jumps_eligible(slots, config, &last_slot))
    {
        if (xpd_lower_static(slots, config, 1, last_slot, d->mixerCells))
            return;
        d->mixerCells[0] = d->mixerCells[1] = 0;
    }
    /* the dynamic lowering covers every slot */
    for (pc = 0; pc < XP_DSP_PROGRAM_SLOTS; ++pc)
        xpd_note_mixer_cell(d->mixerCells, &slots[pc]);
}

static void xp_dsp_sync_program(xp_dsp_t *d, const xp_dsp_request_t *request,
                                int consume_staged_bus_a, int linked)
{
    xp_dsp_config_t config;
    memset(&config, 0, sizeof(config));
    xpd_frame_config(&config, request, consume_staged_bus_a, linked);
    if (!d->programTainted
        && config.serialInputEnabled == d->flatConfig.serialInputEnabled
        && config.serialOutputEnabled == d->flatConfig.serialOutputEnabled
        && config.consumeStagedBusA == d->flatConfig.consumeStagedBusA
        && config.bcdEnabled == d->flatConfig.bcdEnabled
        && config.iram3ParameterReadBoundary == d->flatConfig.iram3ParameterReadBoundary
        && config.linked == d->flatConfig.linked
        && config.executionSlots == d->flatConfig.executionSlots)
        return;
    xpd_lower(d, &config);
    d->programTainted = 0;
}

/* Deposit the frame's voice sends into the mixer cells up front, when
 * the program does not look at those cells this frame.  Returns non-zero
 * and clears the request's mixer frame when it did. */
static int xp_dsp_hoist_deposits(xp_dsp_t *d, xp_dsp_request_t *request,
                                 const xp_mixer_summary_t *summary, int linked)
{
    static const uint8_t send_cycle[4] = { 0, 2, 2, 3 };
    uint32_t *bank;
    uint64_t  seen;
    size_t    cell;

    if (request->mixerFrame == NULL)
        return 0;
    xp_dsp_sync_program(d, request, !linked, linked);
    if (request->executeProgram && (summary->seen & d->mixerCells[d->s.iramSelPhase & 1]) != 0)
        return 0;
    bank = xpd_mixer_iram(&d->s);
    seen = summary->seen;
    for (cell = 0; seen != 0 && cell < XP_DSP_IRAM_SLOTS; ++cell)
    {
        const uint64_t bit = (uint64_t)1 << cell;
        size_t cycles, voice, send;
        if ((seen & bit) == 0)
            continue;
        seen &= ~bit;
        if ((summary->clipped & bit) == 0)
        {
            bank[cell] = (uint32_t)summary->sums[cell] & 0x00ffffff;
            continue;
        }
        cycles = request->executionSlots < XP_DSP_EXEC_SLOTS ? request->executionSlots : XP_DSP_EXEC_SLOTS;
        bank[cell] = 0;
        for (voice = 0; voice * 4 < cycles; ++voice)
            for (send = 0; send < 4; ++send)
            {
                const xp_mixer_send_t *event = &request->mixerFrame->v[voice][send];
                if (event->destination == cell && voice * 4 + send_cycle[send] < cycles)
                    bank[cell] = xpd_encode24(xpd_sign_extend(bank[cell], 24) + event->contribution);
            }
    }
    d->s.mixerInitialized |= summary->seen;
    request->mixerFrame = NULL;
    return 1;
}

static void xp_dsp_set_iram3_target(xp_dsp_t *d, size_t index, uint16_t target_in, uint8_t partition_count)
{
    size_t   parameter, slot;
    uint16_t target;
    if (index >= XP_DSP_IRAM_SLOTS)
        return;
    parameter = index & 0x1f;
    slot      = parameter | 0x20;
    target    = (uint16_t)(target_in & 0x03ff);
    if (parameter < partition_count)
    {
        d->s.iram3[slot] = target;
        return;
    }
    d->s.iram3[slot] = (d->s.iram3[slot] & ~(uint32_t)0x03ff) | target;
}

static void xp_dsp_step_iram3_ramps(xp_dsp_t *d, const uint16_t *rates, uint8_t partition_count,
                                    uint8_t rounding_phase)
{
    uint8_t rp = (uint8_t)(rounding_phase & 0xf);
    uint8_t threshold;
    size_t  parameter;

    rp        = (uint8_t)(((rp & 0x5) << 1) | ((rp & 0xa) >> 1));
    threshold = (uint8_t)(((rp << 2) | (rp >> 2)) & 0xf);

    for (parameter = partition_count; parameter < XP_DSP_IRAM_SLOTS / 2; ++parameter)
    {
        const size_t   slot    = parameter | 0x20;
        const uint32_t cell    = d->s.iram3[slot] & 0x03ffffff;
        const uint32_t low     = cell & 0x03ff;
        const int64_t  current = xpd_sign_extend(cell >> 10, 16);
        int64_t  exact_target, direction, rounded_target, difference, magnitude, next;
        uint32_t product;
        int32_t  delta;
        uint8_t  fraction;
        if (low == 0x0200)
            continue;
        exact_target = XP_SHL64(xpd_sign_extend(low, 10), 6);
        direction    = exact_target - current;
        if (direction == 0)
            continue;
        rounded_target = exact_target + (low > 0 && low < 0x01ff);
        difference     = rounded_target - current;
        if (direction < 0 && difference == 0)
            difference = -2;
        magnitude = difference < 0 ? -difference : difference;
        product   = (uint32_t)magnitude * rates[slot & 3];
        delta     = (int32_t)(product >> 17);
        fraction  = (uint8_t)(((product & 0x1ffff) + 0x1000) >> 13);
        if (fraction == 16)
        {
            ++delta;
            fraction = 0;
        }
        if ((difference > 0 && threshold < fraction)
            || (difference < 0 && fraction != 0 && threshold >= 16 - fraction))
            ++delta;
        next = current + (direction < 0 ? -delta : delta);
        if ((direction > 0 && next > exact_target) || (direction < 0 && next < exact_target))
            next = exact_target;
        d->s.iram3[slot] = (((uint32_t)next & 0xffff) << 10) | low;
    }
}

static void xp_dsp_set_serial_input(xp_dsp_t *d, size_t port, const int32_t *words, size_t count)
{
    size_t i;
    if (port >= 2)
        return;
    d->s.serialInputCount[port] = (uint32_t)(count < XP_DSP_SERIAL_WORDS ? count : XP_DSP_SERIAL_WORDS);
    for (i = 0; i < d->s.serialInputCount[port]; ++i)
        d->s.serialInput[port][i] = (int32_t)xpd_sign_extend((uint32_t)words[i], 24);
}

static void xp_dsp_step(xp_dsp_t *d, const xp_dsp_request_t *request)
{
    xp_dsp_context_t ctx;
    xp_dsp_sync_program(d, request, 1, 0);
    xpd_begin_frame(&d->s, &ctx, request);
    while (xpd_execute_slot(&d->s, &d->program, &ctx, 1))
    {
    }
    xpd_end_frame(&d->s, &ctx);
}

/* Two chips whose bus A outputs feed each other's bus A input, slot by
 * slot (the SC-8850's pair; kept because the SC-88Pro board may use it). */

/* The state a freshly constructed DSP has. */
static void xp_dsp_init(xp_dsp_t *d)
{
    memset(d, 0, sizeof(*d));
    d->s.pendingEramReads[0].countdown = -1;
    d->s.pendingEramReads[1].countdown = -1;
    d->flatConfig.iram3ParameterReadBoundary = XP_DSP_IRAM_SLOTS;
    d->flatConfig.executionSlots             = XP_DSP_EXEC_SLOTS;
    d->programTainted = 1;
}

/* ---- XP: host register interface (xp_host.cpp) ------------------------ */

#define XP_WIDE_STRIDE   4
#define XP_NARROW_STRIDE 2
#define XP_WAVE_BUFFER_END    0x0c00
#define XP_TVA_GAIN_END       0x2800
#define XP_CRAM_END           0x2e40
#define XP_IRAM_END           0x3300
#define XP_IRAM3_TARGETS_END  0x3380
#define XP_PRAM_END           0x3880
#define XP_MIXER_END          0x3c00
#define XP_APERTURE_END       0x4000

/* A voice register wider than 16 bits: where it is and how wide. */
static uint32_t *xp_find_voice_wide(xp_t *xp, uint16_t address, unsigned *width)
{
    const uint16_t bank  = (uint16_t)(address & 0xff00);
    const size_t   voice = (size_t)(address & 0x00ff) / XP_WIDE_STRIDE;
    xp_voice_t *v;
    if (voice >= XP_MAX_VOICES)
        return NULL;
    v = &xp->state.voices[voice];
    switch (bank)
    {
        case 0x0000: *width = 20; return &v->waveControl_0000;
        case 0x0100: *width = 20; return &v->sampleCurrent_0100;
        case 0x0200: *width = 20; return &v->sampleLoop_0200;
        case 0x0300: *width = 20; return &v->sampleEnd_0300;
        case 0x0400: *width = 20; return &v->waveFetchState_0400;
        case 0x0c00: *width = 18; return &v->dpcmAccumulator_0c00;
        case 0x0d00: *width = 18; return &v->pitchIncrement_0d00;
        case 0x0e00: *width = 18; return &v->addressFraction_0e00;
        case 0x1000: *width = 18; return &v->playbackStateConfig_1000;
        case 0x1100: *width = 18; return &v->tvfQDestination_1100;
        case 0x1200: *width = 18; return &v->pitchDestination_1200;
        case 0x1300: *width = 18; return &v->tvfFDestination_1300;
        case 0x1400: *width = 18; return &v->ampModDestination_1400;
        case 0x1500: *width = 18; return &v->ampDestination_1500;
        case 0x1600: *width = 18; return &v->tvfQRamp_1600;
        case 0x1700: *width = 18; return &v->pitchRamp_1700;
        case 0x1800: *width = 18; return &v->tvfFRamp_1800;
        case 0x1900: *width = 18; return &v->ampModRamp_1900;
        case 0x1a00: *width = 18; return &v->ampRamp_1a00;
        case 0x1b00: *width = 18; return &v->pitchCurrent_1b00;
        case 0x1c00: *width = 18; return &v->tvfFCurrent_1c00;
        case 0x1d00: *width = 18; return &v->ampModCurrent_1d00;
        case 0x1e00: *width = 18; return &v->ampCurrent_1e00;
        case 0x2000: *width = 20; return &v->filterConfig_2000;
        case 0x2100: *width = 20; return &v->tvfQCurrent_2100;
        case 0x2200: *width = 20; return &v->tvfFCoefficient_2200;
        case 0x2300: *width = 20; return &v->combinedAmp_2300;
        case 0x2400: *width = 20; return &v->pitchStep_2400;
        case 0x2500: *width = 20; return &v->tvfFStep_2500;
        case 0x2600: *width = 20; return &v->ampStep_2600;
        case 0x2800: *width = 24; return &v->filterBp_2800;
        case 0x2900: *width = 24; return &v->filterLp_2900;
        case 0x2a00: *width = 24; return &v->filterOutput_2a00;
        default:     return NULL;
    }
}

static uint32_t *xp_find_dsp_wide(xp_t *xp, uint16_t address, unsigned *width)
{
    if (address >= 0x3000 && address < XP_IRAM_END)
    {
        const size_t bank = (size_t)((address - 0x3000) >> 8);
        const size_t slot = (size_t)(address & 0x00ff) / XP_WIDE_STRIDE;
        if (slot >= XP_DSP_IRAM_SLOTS)
            return NULL;
        switch (bank)
        {
            case 0:  *width = 24; return &xp->state.dsp.s.iram1[slot];
            case 1:  *width = 24; return &xp->state.dsp.s.iram2[slot];
            case 2:  *width = 26; return &xp->state.dsp.s.iram3[slot];
            default: return NULL;
        }
    }
    if (address >= 0x3400 && address < XP_PRAM_END)
    {
        const size_t slot = (size_t)(address - 0x3400) / XP_WIDE_STRIDE;
        if (slot < XP_DSP_PROGRAM_SLOTS)
        {
            /* the C++ reaches the program through an accessor that marks
             * it changed, on a read as well */
            xp->state.dsp.programTainted = 1;
            *width = 28;
            return &xp->state.dsp.program.pram[slot];
        }
    }
    return NULL;
}

static uint16_t xp_translate_voice_window(const xp_t *xp, uint16_t address)
{
    const uint16_t offset = (uint16_t)(address - 0x3940);
    const uint16_t bank   = (uint16_t)((offset >> 2) << 8);
    const uint16_t half   = (uint16_t)(offset & 2);
    return (uint16_t)(bank | (xp->state.voiceWindowSelect_3934 * XP_WIDE_STRIDE) | half);
}

static uint16_t xp_translate_voice_mixer_window(const xp_t *xp, uint16_t address)
{
    const uint16_t send = (uint16_t)((address - 0x39f8) >> 1);
    return (uint16_t)(0x3a00 + send * 0x80 + xp->state.voiceWindowSelect_3934 * XP_NARROW_STRIDE);
}

static void xp_write_wide(xp_t *xp, uint32_t *reg, unsigned width, uint16_t address, uint16_t value)
{
    if ((address & 2) == 0)
    {
        xp->state.wideWriteLatch = value;
        return;
    }
    *reg = (((uint32_t)xp->state.wideWriteLatch << 16) | value) & xp_width_mask(width);
}

static uint16_t xp_read_wide(xp_t *xp, const uint32_t *reg, unsigned width, uint16_t address)
{
    if ((address & 2) == 0)
        return 0;
    xp->state.readbackLatch = *reg & xp_width_mask(width);
    return 0;
}

static void xp_update_interrupt_line(xp_t *xp)
{
    const int level = xp->state.interrupt;
    if (level == xp->interruptLine)
        return;
    xp->interruptLine = level;
    if (xp->interruptCallback)
        xp->interruptCallback(xp->interruptUser, level);
}

static uint16_t xp_read_wave_rom(xp_t *xp, uint16_t address)
{
    const size_t chip_select = (size_t)((xp->state.waveRomBank >> 4) & 7);
    const xp_wave_rom_t *rom = &xp->waveRoms[chip_select];
    const size_t high   = ((size_t)(xp->state.waveRomBank & 0x0f) >> rom->apertureBankShift) << 20;
    const size_t page   = (size_t)(xp->state.waveRomPage & 0x03ff) << 10;
    const size_t window = (size_t)(address - 0x3c00);
    const size_t rom_address = high | page | window;
    size_t read_size;
    if (rom->data == NULL)
    {
        xp->state.readbackLatch = 0;
        return 0;
    }
    read_size = rom->width == XP_ROM_BITS8 ? 1 : 2;
    if (rom_address + read_size > rom->size)
    {
        xp->state.readbackLatch = 0;
        return 0;
    }
    xp->state.readbackLatch = rom->data[rom_address];
    if (read_size == 2)
        xp->state.readbackLatch |= (uint32_t)((uint16_t)rom->data[rom_address + 1] << 8);
    return 0;
}

static uint16_t xp_host_read_internal(xp_t *xp, uint16_t address)
{
    uint32_t *reg;
    unsigned  width = 0;

    if ((address & 1) != 0)
        return 0;
    if (address >= 0x3940 && address < 0x39f0)
        return xp_host_read_internal(xp, xp_translate_voice_window(xp, address));
    if (address >= 0x39f8 && address < 0x3a00)
        return xp_host_read_internal(xp, xp_translate_voice_mixer_window(xp, address));
    reg = xp_find_voice_wide(xp, address, &width);
    if (reg != NULL)
        return xp_read_wide(xp, reg, width, address);
    if (address >= 0x0800 && address < XP_WAVE_BUFFER_END)
    {
        const size_t offset = (size_t)(address - 0x0800);
        const size_t cell   = offset >> 7;
        const size_t voice  = (offset & 0x7f) / XP_NARROW_STRIDE;
        xp->state.readbackLatch = xp->state.voices[voice].waveCircularBuffer_0800[cell] & 0x0fff;
        return 0;
    }
    if (address >= 0x2700 && address < XP_TVA_GAIN_END)
    {
        if ((address & 2) != 0)
        {
            const size_t voice = (size_t)(address - 0x2700) / XP_WIDE_STRIDE;
            xp->state.readbackLatch = xp->state.voices[voice].tvaGain_2700;
        }
        return 0;
    }
    reg = xp_find_dsp_wide(xp, address, &width);
    if (reg != NULL)
        return xp_read_wide(xp, reg, width, address);
    if (address >= 0x2c00 && address < XP_CRAM_END)
    {
        const size_t slot = (size_t)(address - 0x2c00) / XP_NARROW_STRIDE;
        xp->state.dsp.programTainted = 1;   /* as above */
        xp->state.readbackLatch = xp->state.dsp.program.cram[slot];
        return 0;
    }
    if (address >= 0x3300 && address < XP_IRAM3_TARGETS_END)
    {
        xp->state.readbackLatch = 0;
        return 0;
    }
    if (address >= 0x3a00 && address < XP_MIXER_END)
    {
        const size_t offset = (size_t)(address - 0x3a00);
        const size_t mixer  = offset >> 7;
        const size_t voice  = (offset & 0x7f) / XP_NARROW_STRIDE;
        xp->state.readbackLatch = xp->state.voices[voice].mixer_3a00[mixer];
        return 0;
    }
    if (address >= 0x3c00 && address < XP_APERTURE_END)
        return xp_read_wave_rom(xp, address);

    switch (address)
    {
        case 0x3900: case 0x3902: case 0x3904: case 0x3906:
            xp_commit_released_voices(xp);
            return 0;
        case 0x3908: case 0x390a: case 0x390c: case 0x390e:
        {
            const size_t cs = (size_t)(address - 0x3908);
            return (uint16_t)((uint16_t)xp->state.waveRomConfig[cs]
                            | ((uint16_t)xp->state.waveRomConfig[cs + 1] << 8));
        }
        case 0x3910: return (uint16_t)xp->state.readbackLatch;
        case 0x3912: return (uint16_t)(xp->state.readbackLatch >> 16);
        case 0x3914: return xp->state.highestVoice;
        case 0x3916: return 0;
        case 0x3918: return xp->state.irqStatus;
        case 0x391a:
            xp->state.interrupt = 0;
            return xp->state.irqAcknowledge;
        case 0x391c: return (xp->state.diagnosticSelect_3930 & 0x07ff) >= 0x05a0 ? 0x0040 : 0;
        case 0x3920: return xp->state.waveRomPage;
        case 0x3922: return xp->state.waveRomBank;
        case 0x3924: return xp->state.serialAudioConfig[0];
        case 0x3926: return xp->state.serialAudioConfig[1];
        case 0x3934: return 0;
        case 0x3928: case 0x392a: case 0x392c: case 0x392e:
            return xp->state.iram3RampRates[(size_t)(address - 0x3928) / XP_NARROW_STRIDE];
        default:
            return 0;
    }
}

static uint16_t xp_host_read(xp_t *xp, uint16_t address)
{
    const uint16_t result = xp_host_read_internal(xp, address);
    xp_update_interrupt_line(xp);
    return result;
}

static uint8_t xp_host_read8(xp_t *xp, uint16_t address8)
{
    const uint16_t address = (uint16_t)(address8 & 0x3fff);
    uint16_t word_address;
    if (address >= 0x3c00 && address < XP_APERTURE_END)
    {
        const size_t chip_select = (size_t)((xp->state.waveRomBank >> 4) & 7);
        if (xp->waveRoms[chip_select].width == XP_ROM_BITS8)
        {
            xp_read_wave_rom(xp, address);
            xp->hostReadAddress = 0xffff;
            return 0;
        }
    }
    word_address = (uint16_t)(address & ~(uint16_t)1);
    if ((address & 1) == 0 || xp->hostReadAddress != word_address)
    {
        xp->hostReadWord    = xp_host_read(xp, word_address);
        xp->hostReadAddress = word_address;
    }
    if ((address & 1) == 0)
        return (uint8_t)(xp->hostReadWord >> 8);
    xp->hostReadAddress = 0xffff;
    return (uint8_t)xp->hostReadWord;
}

static void xp_host_write_internal(xp_t *xp, uint16_t address, uint16_t value)
{
    uint32_t *reg;
    unsigned  width = 0;

    if ((address & 1) != 0)
        return;
    if (address >= 0x3940 && address < 0x39f0)
    {
        xp_host_write_internal(xp, xp_translate_voice_window(xp, address), value);
        return;
    }
    if (address >= 0x39f8 && address < 0x3a00)
    {
        xp_host_write_internal(xp, xp_translate_voice_mixer_window(xp, address), value);
        return;
    }
    reg = xp_find_voice_wide(xp, address, &width);
    if (reg != NULL)
    {
        xp_write_wide(xp, reg, width, address, value);
        if ((address & 0xff02) == (0x1a00 | 2))
        {
            const size_t   vi      = (size_t)(address & 0x00ff) / XP_WIDE_STRIDE;
            const uint32_t control = xp->state.voices[vi].ampRamp_1a00;
            xp->state.voices[vi].runtimeCache.ampCurve2EntryPending =
                (control & (XP_RAMP_CURVE_MASK | XP_RAMP_HOLD)) == 0x08000;
        }
        return;
    }
    if (address >= 0x0800 && address < XP_WAVE_BUFFER_END)
    {
        const size_t offset = (size_t)(address - 0x0800);
        const size_t cell   = offset >> 7;
        const size_t voice  = (offset & 0x7f) / XP_NARROW_STRIDE;
        xp->state.voices[voice].waveCircularBuffer_0800[cell] = value & 0x0fff;
        return;
    }
    if (address >= 0x2700 && address < XP_TVA_GAIN_END)
    {
        if ((address & 2) == 0)
            xp->state.wideWriteLatch = value;
        else
        {
            const size_t voice = (size_t)(address - 0x2700) / XP_WIDE_STRIDE;
            xp->state.voices[voice].tvaGain_2700 = value;
        }
        return;
    }
    if (address >= 0x3400 && address < XP_PRAM_END)
    {
        const size_t slot = (size_t)(address - 0x3400) / XP_WIDE_STRIDE;
        if (slot >= XP_DSP_PROGRAM_SLOTS)
            return;
        if ((address & 2) == 0)
            xp->state.wideWriteLatch = value;
        else
            xp_dsp_write_pram(&xp->state.dsp, slot, ((uint32_t)xp->state.wideWriteLatch << 16) | value);
        return;
    }
    reg = xp_find_dsp_wide(xp, address, &width);
    if (reg != NULL)
    {
        xp_write_wide(xp, reg, width, address, value);
        return;
    }
    if (address >= 0x2c00 && address < XP_CRAM_END)
    {
        const size_t slot = (size_t)(address - 0x2c00) / XP_NARROW_STRIDE;
        xp_dsp_write_cram(&xp->state.dsp, slot, value);
        return;
    }
    if (address >= 0x3300 && address < XP_IRAM3_TARGETS_END)
    {
        const size_t slot = (size_t)(address - 0x3300) / XP_NARROW_STRIDE;
        xp_dsp_set_iram3_target(&xp->state.dsp, slot, value,
                                xp_dsp_iram3_partition_count(xp->state.serialAudioConfig[1]));
        return;
    }
    if (address >= 0x3a00 && address < XP_MIXER_END)
    {
        const size_t offset = (size_t)(address - 0x3a00);
        const size_t mixer  = offset >> 7;
        const size_t voice  = (offset & 0x7f) / XP_NARROW_STRIDE;
        xp->state.voices[voice].mixer_3a00[mixer] = value;
        return;
    }
    switch (address)
    {
        case 0x3900: case 0x3902: case 0x3904: case 0x3906:
            xp_write_release_mask(xp, (size_t)(address - 0x3900) / XP_NARROW_STRIDE, value);
            return;
        case 0x3908: case 0x390a: case 0x390c: case 0x390e:
        {
            const size_t cs = (size_t)(address - 0x3908);
            xp->state.waveRomConfig[cs]     = (uint8_t)value;
            xp->state.waveRomConfig[cs + 1] = (uint8_t)(value >> 8);
            return;
        }
        case 0x3914: xp->state.highestVoice = value; return;
        case 0x3916: xp->state.dspControl = value; return;
        case 0x3918: xp->state.irqConfigMask = value; return;
        case 0x3920: xp->state.waveRomPage = value; return;
        case 0x3922: xp->state.waveRomBank = value; return;
        case 0x3924: xp->state.serialAudioConfig[0] = value; return;
        case 0x3926: xp->state.serialAudioConfig[1] = value; return;
        case 0x3928: case 0x392a: case 0x392c: case 0x392e:
            xp->state.iram3RampRates[(size_t)(address - 0x3928) / XP_NARROW_STRIDE] = value;
            return;
        case 0x3930: xp->state.diagnosticSelect_3930 = value; return;
        case 0x3932: xp->state.serialFormat_3932 = value; return;
        case 0x3934: xp->state.voiceWindowSelect_3934 = (uint8_t)(value & 0x3f); return;
        default:
            return;
    }
}

static void xp_host_write(xp_t *xp, uint16_t address, uint16_t value)
{
    xp_host_write_internal(xp, address, value);
    xp_update_interrupt_line(xp);
}

static void xp_host_write8(xp_t *xp, uint16_t address8, uint8_t value)
{
    const uint16_t address = (uint16_t)(address8 & 0x3fff);
    uint16_t word_address, word;
    xp->hostWriteBytes[address] = value;
    xp->hostReadAddress = 0xffff;
    if ((address & 1) == 0)
        return;
    word_address = (uint16_t)(address - 1);
    word = (uint16_t)(((uint16_t)xp->hostWriteBytes[word_address] << 8) | value);
    xp_host_write(xp, word_address, word);
}

static void xp_map_wave_rom(xp_t *xp, size_t chip_select, const uint8_t *data, size_t size,
                            int width, uint8_t aperture_bank_shift, uint8_t voice_bank_shift)
{
    if (chip_select >= XP_WAVE_CHIP_SELECTS)
        return;
    xp->waveRoms[chip_select].data  = data;
    xp->waveRoms[chip_select].size  = size;
    xp->waveRoms[chip_select].width = width;
    xp->waveRoms[chip_select].apertureBankShift = (uint8_t)(aperture_bank_shift < 3 ? aperture_bank_shift : 3);
    xp->waveRoms[chip_select].voiceBankShift    = (uint8_t)(voice_bank_shift < 3 ? voice_bank_shift : 3);
}

/* ---- XP: one sample (xp.cpp) ------------------------------------------ */

static void xp_prepare_mixer(xp_t *xp, size_t voice_count)
{
    size_t vi, si;
    xp->state.dsp.s.mixerInitialized = 0;
    memset(&xp->mixerSummary, 0, sizeof(xp->mixerSummary));
    for (vi = 0; vi < voice_count; ++vi)
    {
        const xp_voice_t *v = &xp->state.voices[vi];
        const int32_t output = xp_sign_extend(v->filterOutput_2a00, 24);
        xp_mixer_send_t *sends = xp->frame.v[vi];
        if (output == 0)
        {
            for (si = 0; si < 4; ++si)
            {
                const size_t destination = (size_t)(v->mixer_3a00[si] & XP_MIXER_DEST_MASK);
                sends[si].destination  = destination;
                sends[si].contribution = 0;
                if (destination < XP_DSP_IRAM_SLOTS)
                    xp->mixerSummary.seen |= (uint64_t)1 << destination;
            }
            continue;
        }
        for (si = 0; si < 4; ++si)
        {
            const uint16_t send        = v->mixer_3a00[si];
            const size_t   destination = (size_t)(send & XP_MIXER_DEST_MASK);
            const uint32_t level       = (uint32_t)(send >> XP_MIXER_LEVEL_SHIFT);
            const int64_t  contribution = XP_DIV_POW2((int64_t)output * level, XP_MIXER_PRODUCT_SHIFT);
            sends[si].destination  = destination;
            sends[si].contribution = contribution;
            /* DspMixerSummary::add */
            if (destination < XP_DSP_IRAM_SLOTS)
            {
                const uint64_t bit = (uint64_t)1 << destination;
                if (!(contribution == 0 && (xp->mixerSummary.seen & bit) != 0))
                {
                    int64_t *sum = &xp->mixerSummary.sums[destination];
                    xp->mixerSummary.seen |= bit;
                    *sum += contribution;
                    if (*sum < -0x800000 || *sum > 0x7fffff)
                        xp->mixerSummary.clipped |= bit;
                }
            }
        }
    }
    for (vi = voice_count; vi < XP_MAX_VOICES; ++vi)
        memset(xp->frame.v[vi], 0, sizeof(xp->frame.v[vi]));
}

SC88_INLINE void xp_update_tva_before_audio(xp_voice_t *v)
{
    const int phase = v->runtimeCache.runtimePhase;
    if (v->resetState_3900.released && (phase == XP_PHASE_STARTING || phase == XP_PHASE_RUNNING))
        xp_step_tva_gain(v);
}

static void xp_prepare_sample(xp_t *xp)
{
    size_t voice_count, vi;

    xp->irqEventAcceptedThisStep = 0;
    xp->state.irqBlockedEvent    = 0;
    voice_count = (size_t)(xp->state.highestVoice & 0x3f) + 1;
    if (voice_count > XP_MAX_VOICES)
        voice_count = XP_MAX_VOICES;
    xp->frameVoices = voice_count;
    xp_prepare_mixer(xp, voice_count);

    for (vi = 0; vi < voice_count; ++vi)
    {
        xp_voice_t *v = &xp->state.voices[vi];
        if (!v->resetState_3900.released && v->tvaGain_2700 != 0)
        {
            const uint32_t g = ((uint32_t)v->tvaGain_2700 * 7u) >> 3;
            v->tvaGain_2700 = (uint16_t)(g > 1 ? g : 1);
        }
    }

    for (vi = 0; vi < voice_count; ++vi)
    {
        xp_voice_t *v = &xp->state.voices[vi];
        const unsigned structure = (unsigned)((v->filterConfig_2000 & XP_FC_STRUCTURE_MASK) >> 12);
        const int paired = (vi & 1) == 0 && vi + 1 < voice_count
            && (v->filterConfig_2000 & XP_FC_PAIR_ROLE) != 0 && structure >= 1 && structure <= 9;
        if (paired)
        {
            xp_voice_t *partner = &xp->state.voices[vi + 1];
            int32_t owner_sample = 0, partner_sample = 0;
            int     owner_due = 0, partner_due = 0;
            if (v->resetState_3900.released)
            {
                xp_update_tva_before_audio(v);
                owner_due = xp_step_voice_source(xp, vi, v, &owner_sample);
            }
            else
                v->filterBp_2800 = v->filterLp_2900 = v->filterOutput_2a00 = 0;
            if (partner->resetState_3900.released)
            {
                xp_update_tva_before_audio(partner);
                partner_due = xp_step_voice_source(xp, vi + 1, partner, &partner_sample);
            }
            else
                partner->filterBp_2800 = partner->filterLp_2900 = partner->filterOutput_2a00 = 0;
            if (owner_due || partner_due)
                xp_step_paired_voice_filter(v, partner, owner_due ? owner_sample : 0,
                                            partner_due ? partner_sample : 0);
            ++vi;
        }
        else if (v->resetState_3900.released)
        {
            int32_t sample = 0;
            xp_update_tva_before_audio(v);
            if (xp_step_voice_source(xp, vi, v, &sample))
            {
                const int32_t filtered = xp_step_voice_filter(v, sample,
                    (unsigned)((v->filterConfig_2000 & XP_FC_MODE_MASK) >> 10));
                v->filterOutput_2a00 = (uint32_t)xp_apply_tva(v, filtered) & XP_FILTER_MASK;
            }
        }
        else
        {
            v->filterBp_2800     = 0;
            v->filterLp_2900     = 0;
            v->filterOutput_2a00 = 0;
        }
    }

    if ((xp->state.dspControl & 4) != 0 && (xp->state.sampleClock & 1) != 0)
        xp_dsp_step_iram3_ramps(&xp->state.dsp, xp->state.iram3RampRates,
                                xp_dsp_iram3_partition_count(xp->state.serialAudioConfig[1]),
                                (uint8_t)(xp->state.sampleClock >> 1));
}

static void xp_dsp_step_request(const xp_t *xp, xp_dsp_request_t *r)
{
    r->executeProgram      = (xp->state.dspControl & 4) != 0;
    r->serialInputEnabled  = (xp->state.dspControl & 2) != 0;
    r->serialAudio0Config  = xp->state.serialAudioConfig[0];
    r->serialAudio1Config  = xp->state.serialAudioConfig[1];
    r->serialOutputEnabled = (xp->state.dspControl & 3) == 3;
    r->executionSlots      = xp->frameVoices * 4;
    r->mixerFrame          = &xp->frame;
}

static void xp_finish_sample(xp_t *xp)
{
    ++xp->state.sampleClock;
    xp_update_interrupt_line(xp);
}

static void xp_step(xp_t *xp)
{
    xp_dsp_request_t request;
    xp_prepare_sample(xp);
    xp_dsp_step_request(xp, &request);
    xp_dsp_hoist_deposits(&xp->state.dsp, &request, &xp->mixerSummary, 0);
    xp_dsp_step(&xp->state.dsp, &request);
    xp_finish_sample(xp);
}

static void xp_state_init(xp_state_t *st)
{
    memset(st, 0, sizeof(*st));
    xp_dsp_init(&st->dsp);
    st->highestVoice = XP_MAX_VOICES - 1;
}

/* Bring the chip to its power-on state; the wave ROM mapping and the
 * interrupt callback are the board's and stay. */
static void xp_reset(xp_t *xp)
{
    const int was_asserted = xp->interruptLine;
    xp_state_init(&xp->state);
    memset(xp->hostWriteBytes, 0, sizeof(xp->hostWriteBytes));
    xp->hostReadWord    = 0;
    xp->hostReadAddress = 0xffff;
    xp->interruptLine   = 0;
    xp->irqEventAcceptedThisStep = 0;
    if (was_asserted && xp->interruptCallback)
        xp->interruptCallback(xp->interruptUser, 0);
}

static void xp_init(xp_t *xp)
{
    memset(xp, 0, sizeof(*xp));
    xp_state_init(&xp->state);
    xp->hostReadAddress = 0xffff;
}

/* ---- LSP effects DSP (SC-88Pro's insertion effects) -------------------
 * custom_chips/lsp: lsp_common.h, lsp_program.h, lsp_interpreter.h, lsp.h.
 * The C++ can compile the program to machine code; this is its
 * interpreter path, and what it prepares for the compiler alone is left
 * out. */

#define LSP_PROGRAM_WORDS    384
#define LSP_IRAM_PROGRAM_BASE 0x80
#define LSP_HOST_IRAM_SIZE   0x200
#define LSP_DATA_RING_SIZE   0x80
#define LSP_DATA_RING_MASK   (LSP_DATA_RING_SIZE - 1)
#define LSP_ERAM_SIZE        0x10000
#define LSP_ERAM_MASK        (LSP_ERAM_SIZE - 1)
#define LSP_CHANNEL_SPLIT    (LSP_PROGRAM_WORDS / 2)

enum { LSP_OP_SKIP, LSP_OP_MAC, LSP_OP_MUL, LSP_OP_SPECIAL };
enum { LSP_SRC_NONE, LSP_SRC_ASAT, LSP_SRC_BSAT, LSP_SRC_ARAW };

#define LSP_SLOT_JUMP_IF_NEGATIVE      0x0d
#define LSP_SLOT_JUMP_IF_NON_NEGATIVE  0x0e
#define LSP_SLOT_JUMP_ALWAYS           0x0f
#define LSP_SLOT_ERAM_WRITE_LATCH      0x10
#define LSP_SLOT_ERAM_TAP_AND_COEF1    0x13
#define LSP_SLOT_MUL_COEF1             0x14
#define LSP_SLOT_MUL_COEF2             0x15
#define LSP_SLOT_AUDIO_OUT             0x18
#define LSP_SLOT_ERAM_READ0            0x1a
#define LSP_SLOT_AUDIO_IN              0x1e

#define LSP_TAINT_CRAM        1
#define LSP_TAINT_ERAM        2
#define LSP_TAINT_STRUCTURAL  4

#define LSP_HOST_ADDR_LO   0x00
#define LSP_HOST_ADDR_HI   0x01
#define LSP_HOST_DATA_LO   0x02
#define LSP_HOST_DATA_MID  0x03
#define LSP_HOST_DATA_HI   0x04
#define LSP_HOST_CONFIG    0x06
#define LSP_HOST_READ_LO   0x08
#define LSP_HOST_READ_HI   0x09

typedef struct
{
    uint8_t  ii;
    uint8_t  rr;
    int8_t   cc;
    uint8_t  op;
    uint8_t  src;
    uint8_t  memOffs;
    uint8_t  scaler;
    uint8_t  immShift;
    uint8_t  writesAcc;
    uint8_t  accB;
    uint8_t  replace;
    uint8_t  abs;
    uint8_t  zeroCoef;
    uint8_t  mulLower;
    uint8_t  mulNegate;
    uint8_t  mulCoef2;
    uint8_t  mulZero;
    uint8_t  slot;
    uint8_t  imm50d0;
    uint8_t  prevMem;
    uint8_t  jump;        /* non-zero: the word before this one was a jump */
    uint16_t jumpDest;
    uint8_t  eramRead;
    uint8_t  eramWrite;
    uint8_t  eramSecondTap;
} lsp_instr_t;

typedef struct
{
    int32_t  audioInL;
    int32_t  audioInR;
    int32_t  audioOutL;
    int32_t  audioOutR;
    int32_t  accs[6];
    int32_t  eramReadValue;
    int32_t  eramWriteLatch;
    int32_t  eramSecondTapOffs;
    int32_t  multiplCoef1;
    int32_t  multiplCoef2;
    int32_t  audioOut;
    int32_t  audioIn;
    int32_t  jumpPending;
    uint8_t  bufferPos;
    uint16_t eramPos;
    int32_t  iram[LSP_DATA_RING_SIZE];
    int32_t  eram[LSP_ERAM_SIZE];
} lsp_runtime_t;

typedef struct
{
    int32_t     words[LSP_PROGRAM_WORDS];
    lsp_instr_t instr[LSP_PROGRAM_WORDS];
    int32_t     coefs[LSP_PROGRAM_WORDS];
    uint16_t    eramAddr[LSP_PROGRAM_WORDS];
    int         hasProgram;
    uint8_t     taintBits;
} lsp_program_t;

typedef struct
{
    uint16_t      config;
    int           running;
    uint32_t      hostLatch;
    uint16_t      hostReadAddr;
    lsp_runtime_t runtime;
    lsp_program_t program;
} lsp_t;

SC88_INLINE int32_t lsp_clamp24(int64_t v)
{
    if (v > 0x7fffff)
        return 0x7fffff;
    if (v < -0x800000)
        return -0x800000;
    return (int32_t)v;
}

SC88_INLINE int32_t lsp_sign_extend24(int32_t x)
{
    return (int32_t)((uint32_t)x << 8) >> 8;
}

static void lsp_instr_default(lsp_instr_t *o)
{
    memset(o, 0, sizeof(*o));
    o->scaler = 7;
}

static void lsp_decode(int32_t word, lsp_instr_t *o)
{
    uint8_t opcode;
    lsp_instr_default(o);
    if (word == 0)
        return;
    o->ii = (uint8_t)(word >> 16);
    o->rr = (uint8_t)(word >> 8);
    o->cc = (int8_t)word;
    opcode     = (uint8_t)(o->ii & 0xe0);
    o->memOffs = (uint8_t)(o->rr & 0x7f);
    o->scaler  = (o->rr & 0x80) ? 5 : 7;
    switch (o->ii & 0x18)
    {
        case 0x08: o->src = LSP_SRC_ASAT; break;
        case 0x10: o->src = LSP_SRC_BSAT; break;
        case 0x18: o->src = LSP_SRC_ARAW; break;
        default:   break;
    }
    if (o->memOffs >= 1 && o->memOffs <= 4)
        o->immShift = (uint8_t)(2 + o->memOffs * 5);
    if (opcode == 0x80)
    {
        o->op        = LSP_OP_MUL;
        o->mulLower  = (o->cc & 0x40) != 0;
        o->accB      = (o->cc & 0x10) != 0;
        o->mulNegate = (o->cc & 0x04) != 0;
        o->replace   = (o->cc & 0x08) != 0 && !o->mulLower;
        o->mulCoef2  = (o->cc & 0x02) != 0;
        o->mulZero   = o->cc == 0;
        o->writesAcc = 1;
    }
    else if (opcode == 0xc0 || opcode == 0xe0)
    {
        o->op      = LSP_OP_SPECIAL;
        o->accB    = (o->ii & 0x20) != 0;
        o->replace = (o->rr & 0x20) != 0;
        o->slot    = (uint8_t)(o->rr & 0x1f);
        o->imm50d0 = o->src == LSP_SRC_NONE && o->slot == LSP_SLOT_ERAM_WRITE_LATCH;
        switch (o->slot)
        {
            case LSP_SLOT_AUDIO_OUT:
            case LSP_SLOT_ERAM_READ0: case LSP_SLOT_ERAM_READ0 + 1:
            case LSP_SLOT_ERAM_READ0 + 2: case LSP_SLOT_ERAM_READ0 + 3:
            case LSP_SLOT_AUDIO_IN:
                o->writesAcc = 1;
                break;
            default:
                break;
        }
        if (o->imm50d0)
            o->writesAcc = 1;
    }
    else
    {
        o->op        = LSP_OP_MAC;
        o->abs       = opcode == 0xa0;
        o->replace   = (o->ii & 0x20) != 0 || o->abs;
        o->accB      = (o->ii & 0x40) != 0;
        o->writesAcc = 1;
    }
    if (o->writesAcc && o->op != LSP_OP_MUL && !o->replace && o->cc == 0)
    {
        o->zeroCoef  = 1;
        o->writesAcc = 0;
    }
}

typedef struct
{
    uint8_t  read;
    uint8_t  write;
    uint8_t  secondTap;
    uint16_t base;
} lsp_eram_access_t;

static void lsp_decode_eram(const lsp_program_t *p, uint32_t pc, lsp_eram_access_t *a)
{
    const lsp_instr_t *o = &p->instr[pc];
    int      start, i;
    uint32_t base = 0;

    a->read = a->write = a->secondTap = 0;
    a->base = 0;
    if (o->op != LSP_OP_SPECIAL || o->src == LSP_SRC_NONE)
        return;
    a->read  = o->slot >= LSP_SLOT_ERAM_READ0 && o->slot <= LSP_SLOT_ERAM_READ0 + 3;
    a->write = o->slot == LSP_SLOT_ERAM_WRITE_LATCH;
    if (!a->read && !a->write)
        return;
    start = (int)pc - (a->write ? 8 : 12);
    if (start < 0)
        return;
#define LSP_CTRL(n) ((uint8_t)((p->words[start + (n)] >> 16) & 0x07))
    a->secondTap = (LSP_CTRL(0) & 0x06) == 0x04;
    for (i = 1; i <= 6; ++i)
    {
        uint16_t incr = (uint16_t)(LSP_CTRL(i) << ((i - 1) * 3));
        if (a->secondTap)
            incr = (LSP_CTRL(i) == 0x02 && i == 1) ? 1 : 0;
        if (i < 6 || (LSP_CTRL(i) & 1))
            base += incr;
    }
#undef LSP_CTRL
    a->base = (uint16_t)base;
}

static int lsp_patch_eram_addresses(lsp_program_t *p, uint32_t first)
{
    lsp_eram_access_t fresh[13];
    const uint32_t end = first + 12 < LSP_PROGRAM_WORDS ? first + 12 : LSP_PROGRAM_WORDS - 1;
    uint32_t pc;
    for (pc = first; pc <= end; ++pc)
    {
        const lsp_instr_t *o = &p->instr[pc];
        lsp_eram_access_t a;
        lsp_decode_eram(p, pc, &a);
        if (a.read != o->eramRead || a.write != o->eramWrite || a.secondTap != o->eramSecondTap)
            return 0;
        fresh[pc - first] = a;
    }
    for (pc = first; pc <= end; ++pc)
        p->eramAddr[pc] = fresh[pc - first].base;
    return 1;
}

static void lsp_program_clear(lsp_program_t *p)
{
    uint32_t pc;
    memset(p->words, 0, sizeof(p->words));
    memset(p->coefs, 0, sizeof(p->coefs));
    memset(p->eramAddr, 0, sizeof(p->eramAddr));
    for (pc = 0; pc < LSP_PROGRAM_WORDS; ++pc)
        lsp_instr_default(&p->instr[pc]);
    p->hasProgram = 0;
    p->taintBits  = LSP_TAINT_STRUCTURAL;
}

static void lsp_program_write(lsp_program_t *p, uint32_t pc, int32_t word_in)
{
    const int32_t word = word_in & 0xffffff;
    const int32_t old  = p->words[pc];
    const int tainted  = (p->taintBits & LSP_TAINT_STRUCTURAL) != 0;
    int cc_only, eram_bits_only;

    if (old == word)
        return;
    p->words[pc] = word;

    cc_only = old != 0 && word != 0 && ((old ^ word) & 0xffff00) == 0;
    if (cc_only && !tainted)
    {
        const lsp_instr_t *i = &p->instr[pc];
        const int is_jump = i->op == LSP_OP_SPECIAL
            && (i->slot == LSP_SLOT_JUMP_IF_NEGATIVE || i->slot == LSP_SLOT_JUMP_IF_NON_NEGATIVE
                || i->slot == LSP_SLOT_JUMP_ALWAYS);
        const int crosses_zero = (i->writesAcc || i->zeroCoef) && !i->replace
            && ((old & 0xff) == 0) != ((word & 0xff) == 0);
        if (!crosses_zero && (i->op == LSP_OP_MAC || (i->op == LSP_OP_SPECIAL && !is_jump)))
        {
            p->instr[pc].cc = (int8_t)word;
            p->coefs[pc]    = (int8_t)word;
            p->taintBits   |= LSP_TAINT_CRAM;
            return;
        }
    }
    eram_bits_only = ((old ^ word) & ~0x070000) == 0;
    if (eram_bits_only && !tainted && lsp_patch_eram_addresses(p, pc))
    {
        p->taintBits |= LSP_TAINT_ERAM;
        return;
    }
    p->taintBits |= LSP_TAINT_STRUCTURAL;
    p->hasProgram = 1;
}

static void lsp_cache_program(lsp_program_t *p)
{
    uint32_t pc;
    for (pc = 0; pc < LSP_PROGRAM_WORDS; ++pc)
    {
        lsp_decode(p->words[pc], &p->instr[pc]);
        p->coefs[pc] = p->instr[pc].cc;
    }
    for (pc = 1; pc < LSP_PROGRAM_WORDS; ++pc)
    {
        lsp_instr_t       *o    = &p->instr[pc];
        const lsp_instr_t *prev = &p->instr[pc - 1];
        o->prevMem = prev->memOffs;
        if (prev->op != LSP_OP_SPECIAL)
            continue;
        if (prev->slot != LSP_SLOT_JUMP_IF_NEGATIVE && prev->slot != LSP_SLOT_JUMP_IF_NON_NEGATIVE
            && prev->slot != LSP_SLOT_JUMP_ALWAYS)
            continue;
        o->jump     = 1;
        o->jumpDest = (uint16_t)(((uint8_t)prev->cc << 1) - 0x80);
        if ((uint32_t)o->jumpDest == pc - 1)
        {
            o->jump     = 0;
            o->jumpDest = 0;
        }
    }
    for (pc = 0; pc < LSP_PROGRAM_WORDS; ++pc)
    {
        lsp_eram_access_t a;
        lsp_decode_eram(p, pc, &a);
        p->instr[pc].eramRead      = a.read;
        p->instr[pc].eramWrite     = a.write;
        p->instr[pc].eramSecondTap = a.secondTap;
        p->eramAddr[pc]            = a.base;
    }
    p->taintBits = 0;
}

SC88_INLINE int32_t lsp_read_ring(const lsp_runtime_t *rt, uint8_t mem_offs)
{
    return rt->iram[(mem_offs + rt->bufferPos) & LSP_DATA_RING_MASK];
}

SC88_INLINE void lsp_write_ring(lsp_runtime_t *rt, uint8_t mem_offs, int32_t value)
{
    rt->iram[(mem_offs + rt->bufferPos) & LSP_DATA_RING_MASK] = value;
}

/* One pass over the program: one output sample per channel. */
static void lsp_run_program(const lsp_program_t *p, lsp_runtime_t *rt)
{
    int      pred = rt->jumpPending != 0;
    uint32_t total = 0, pc;
    int32_t *a = rt->accs;

    rt->audioIn = rt->audioInR;
    for (pc = 0; pc < LSP_PROGRAM_WORDS && total < LSP_PROGRAM_WORDS; ++pc, ++total)
    {
        const lsp_instr_t *i = &p->instr[pc];
        int32_t new_a, new_b;

        if (i->eramRead)
        {
            const uint32_t addr = ((uint32_t)rt->eramPos + p->eramAddr[pc]
                + (i->eramSecondTap ? (uint32_t)rt->eramSecondTapOffs : 0u)) & LSP_ERAM_MASK;
            rt->eramReadValue = (int32_t)((uint32_t)rt->eram[addr] << 4);
        }

        new_a = a[2];
        new_b = a[5];
        if (i->op != LSP_OP_SKIP)
        {
            int32_t  src = 0;
            int32_t *dest = i->accB ? &new_b : &new_a;
            const int64_t live = *dest;
            const int32_t cc   = p->coefs[pc];

            switch (i->src)
            {
                case LSP_SRC_ASAT: src = lsp_clamp24(a[0]); break;
                case LSP_SRC_BSAT: src = lsp_clamp24(a[3]); break;
                case LSP_SRC_ARAW: src = lsp_sign_extend24(a[0]); break;
                default: break;
            }

            switch (i->op)
            {
                case LSP_OP_MAC:
                {
                    int64_t incr;
                    if (i->src != LSP_SRC_NONE)
                        lsp_write_ring(rt, i->memOffs, src);
                    /* cc shifted left may be negative: multiply instead */
                    incr = i->immShift
                        ? ((int64_t)cc * ((int64_t)1 << i->immShift)) >> i->scaler
                        : ((int64_t)lsp_read_ring(rt, i->memOffs) * cc) >> i->scaler;
                    *dest = (int32_t)(i->replace ? incr : live + incr);
                    if (i->abs && *dest < 0)
                        *dest = (int32_t)(0u - (uint32_t)*dest);
                    break;
                }
                case LSP_OP_MUL:
                {
                    int32_t op_b = i->mulCoef2 ? rt->multiplCoef2 : rt->multiplCoef1;
                    int64_t op_a, result;
                    op_b = i->mulLower ? (op_b & 0xffff) >> 9 : op_b >> 16;
                    if (i->src != LSP_SRC_NONE)
                        lsp_write_ring(rt, i->memOffs, src);
                    op_a   = i->immShift ? ((int64_t)1 << i->immShift)
                                         : (int64_t)lsp_read_ring(rt, i->memOffs);
                    result = i->mulZero ? 0 : op_a * op_b;
                    result >>= i->scaler + (i->mulLower ? 7 : 0);
                    if (i->mulNegate)
                        result = -result;
                    if (!i->replace)
                        result += lsp_clamp24(live);
                    *dest = (int32_t)result;
                    break;
                }
                case LSP_OP_SPECIAL:
                    if (i->imm50d0)
                    {
                        const uint8_t ucc = (uint8_t)cc;
                        int64_t incr = ((int64_t)lsp_read_ring(rt, i->prevMem) * ucc) >> 7;
                        if (i->prevMem >= 1 && i->prevMem <= 4)
                            incr = (int64_t)ucc << ((i->prevMem - 1) * 5);
                        incr >>= 1 + i->scaler;
                        *dest = (int32_t)(i->replace ? incr : live + incr);
                        break;
                    }
                    switch (i->slot)
                    {
                        case LSP_SLOT_JUMP_IF_NEGATIVE:     pred = src < 0; break;
                        case LSP_SLOT_JUMP_IF_NON_NEGATIVE: pred = src >= 0; break;
                        case LSP_SLOT_JUMP_ALWAYS:          pred = 1; break;
                        case LSP_SLOT_ERAM_WRITE_LATCH:     rt->eramWriteLatch = src; break;
                        case LSP_SLOT_ERAM_TAP_AND_COEF1:
                            if (i->src == LSP_SRC_ARAW)
                            {
                                rt->eramSecondTapOffs = a[0] >> 10;
                                rt->multiplCoef1      = (a[0] & 0x3ff) << 13;
                            }
                            break;
                        case LSP_SLOT_MUL_COEF1: rt->multiplCoef1 = src; break;
                        case LSP_SLOT_MUL_COEF2: rt->multiplCoef2 = src; break;
                        case LSP_SLOT_AUDIO_OUT:
                            rt->audioOut = src;
                            lsp_write_ring(rt, 0x78, src);
                            break;
                        case LSP_SLOT_ERAM_READ0: case LSP_SLOT_ERAM_READ0 + 1:
                        case LSP_SLOT_ERAM_READ0 + 2: case LSP_SLOT_ERAM_READ0 + 3:
                            src = rt->eramReadValue;
                            lsp_write_ring(rt, (uint8_t)(0x60 + i->slot), src);
                            break;
                        case LSP_SLOT_AUDIO_IN:
                            if (pc >= LSP_CHANNEL_SPLIT)
                                rt->audioIn = rt->audioInL;
                            src = rt->audioIn;
                            lsp_write_ring(rt, 0x7e, src);
                            break;
                        default:
                            break;
                    }
                    if (i->writesAcc)
                    {
                        const int64_t incr = ((int64_t)src * cc) >> i->scaler;
                        *dest = (int32_t)(i->replace ? incr : live + incr);
                    }
                    break;
                default:
                    break;
            }
        }

        a[0] = a[1]; a[1] = a[2]; a[2] = new_a;
        a[3] = a[4]; a[4] = a[5]; a[5] = new_b;

        if (i->eramWrite)
            rt->eram[((uint32_t)rt->eramPos + p->eramAddr[pc]) & LSP_ERAM_MASK] = rt->eramWriteLatch >> 4;

        if (i->jump && pred)
        {
            pc   = (uint32_t)i->jumpDest - 1;
            pred = 0;
        }
        if (i->op == LSP_OP_SPECIAL && i->slot == LSP_SLOT_AUDIO_OUT && pc < LSP_CHANNEL_SPLIT)
            rt->audioOutR = rt->audioOut;
    }
    rt->jumpPending = pred;
    /* end of pass */
    rt->audioOutL = rt->audioOut;
    rt->bufferPos = (uint8_t)((rt->bufferPos - 1) & LSP_DATA_RING_MASK);
    rt->eramPos   = (uint16_t)(rt->eramPos - 1);
}

static void lsp_init(lsp_t *l)
{
    memset(l, 0, sizeof(*l));
    lsp_program_clear(&l->program);
}

static void lsp_clear(lsp_t *l)
{
    lsp_program_clear(&l->program);
    memset(&l->runtime, 0, sizeof(l->runtime));
    l->config       = 0;
    l->running      = 0;
    l->hostLatch    = 0;
    l->hostReadAddr = 0;
}

static int32_t lsp_read_iram(const lsp_t *l, uint16_t addr)
{
    const uint16_t a = (uint16_t)(addr & (LSP_HOST_IRAM_SIZE - 1));
    return a < LSP_IRAM_PROGRAM_BASE ? l->runtime.iram[a] : l->program.words[a - LSP_IRAM_PROGRAM_BASE];
}

static void lsp_write_iram(lsp_t *l, uint16_t addr, int32_t word)
{
    const uint16_t a = (uint16_t)(addr & (LSP_HOST_IRAM_SIZE - 1));
    if (a < LSP_IRAM_PROGRAM_BASE)
        l->runtime.iram[a] = word & 0xffffff;
    else
        lsp_program_write(&l->program, (uint32_t)(a - LSP_IRAM_PROGRAM_BASE), word);
}

static uint8_t lsp_host_read(const lsp_t *l, uint16_t reg)
{
    const int32_t value = lsp_read_iram(l, l->hostReadAddr);
    switch (reg)
    {
        case LSP_HOST_DATA_MID: return 0x00;
        case LSP_HOST_ADDR_LO:  return (uint8_t)value;
        case LSP_HOST_ADDR_HI:  return (uint8_t)(value >> 8);
        case LSP_HOST_DATA_LO:  return (uint8_t)(value >> 16);
        default:                return 0x00;
    }
}

static void lsp_host_write(lsp_t *l, uint16_t reg, uint8_t value)
{
    switch (reg)
    {
        case LSP_HOST_DATA_LO:  l->hostLatch = (l->hostLatch & 0xffff00u) | value; return;
        case LSP_HOST_DATA_MID: l->hostLatch = (l->hostLatch & 0xff00ffu) | ((uint32_t)value << 8); return;
        case LSP_HOST_DATA_HI:  l->hostLatch = (l->hostLatch & 0x00ffffu) | ((uint32_t)value << 16); return;
        case LSP_HOST_ADDR_HI:
        case LSP_HOST_READ_HI:
            l->hostReadAddr = (uint16_t)((l->hostReadAddr & 0x00ff) | (value << 8));
            return;
        case LSP_HOST_READ_LO:
            l->hostReadAddr = (uint16_t)((l->hostReadAddr & 0xff00) | value);
            return;
        case LSP_HOST_CONFIG:
            l->config = (uint16_t)l->hostLatch;
            if (l->config == 0x0001)
            {
                if (l->program.taintBits & LSP_TAINT_STRUCTURAL)
                    lsp_cache_program(&l->program);
                l->running = 1;
            }
            else if (l->config == 0x1021)
                l->running = 0;
            return;
        case LSP_HOST_ADDR_LO:
            l->hostReadAddr = (uint16_t)((l->hostReadAddr & 0xff00) | value);
            lsp_write_iram(l, l->hostReadAddr, (int32_t)(l->hostLatch & 0xffffff));
            return;
        default:
            return;
    }
}

/* One stereo sample through the effect. */
static void lsp_process(lsp_t *l, int32_t in_l, int32_t in_r, int32_t *out_l, int32_t *out_r)
{
    if (!l->running || !l->program.hasProgram)
    {
        *out_l = 0;
        *out_r = 0;
        return;
    }
    l->runtime.audioInL = in_l;
    l->runtime.audioInR = in_r;
    if (l->program.taintBits & LSP_TAINT_STRUCTURAL)
        lsp_cache_program(&l->program);
    lsp_run_program(&l->program, &l->runtime);
    *out_l = l->runtime.audioOutL;
    *out_r = l->runtime.audioOutR;
}

/* ---- H8/500 CPU core ---------------------------------------------------
 * cpu/common (core.hpp, device.hpp) and cpu/h8500 (bus, chip, cpu, decode,
 * timing, exec).  The C++ binds each instruction to a template-built
 * handler and keeps one cached cell per code byte; this keeps the cache,
 * with its fill and invalidation rules, and runs a cell through one
 * interpreter instead of a handler per combination. */

typedef struct
{
    uint8_t  (*read8)(void *ctx, uint32_t a);
    void     (*write8)(void *ctx, uint32_t a, uint8_t v);
    uint16_t (*read16)(void *ctx, uint32_t a);              /* NULL: two byte reads, high first */
    void     (*write16)(void *ctx, uint32_t a, uint16_t v); /* NULL: two byte writes, high first */
    void      *ctx;
} h8_device_t;

SC88_INLINE uint16_t h8_dev_read16(const h8_device_t *d, uint32_t a)
{
    uint8_t hi;
    if (d->read16)
        return d->read16(d->ctx, a);
    hi = d->read8(d->ctx, a);
    return (uint16_t)(((uint16_t)hi << 8) | d->read8(d->ctx, a + 1));
}

SC88_INLINE void h8_dev_write16(const h8_device_t *d, uint32_t a, uint16_t v)
{
    if (d->write16)
    {
        d->write16(d->ctx, a, v);
        return;
    }
    d->write8(d->ctx, a, (uint8_t)(v >> 8));
    d->write8(d->ctx, a + 1, (uint8_t)v);
}

/* ---- bus ---- */

#define H8_LINE_SHIFT  7
#define H8_LINE_SIZE   (1u << H8_LINE_SHIFT)
#define H8_AT_CLASS    0x03
#define H8_AT_RDSLOW   0x04
#define H8_AT_WRSLOW   0x08
#define H8_AT_NOEXEC   0x10
#define H8_AT_CODE     0x20
#define H8_AT_WAIT_SHIFT 6

/* bus classes: width and states per access */
enum { H8_W16_S2, H8_W16_S3, H8_W8_S2, H8_W8_S3 };
enum { H8_LINE_UNMAPPED, H8_LINE_RAM, H8_LINE_ROM, H8_LINE_DEV };

struct h8_cpu;

typedef struct
{
    uint8_t  *mem;
    uint8_t  *line_kind;
    const h8_device_t **line_dev;
    uint8_t  *attr;
    size_t    lines;
    uint32_t  addr_mask;
    struct h8_cpu *sink;     /* told when a line holding cached code is written */
} h8_bus_t;

static void h8_cpu_code_line_written(struct h8_cpu *c, uint32_t addr);

static int h8_bus_init(h8_bus_t *b, unsigned addr_bits)
{
    size_t bytes;
    memset(b, 0, sizeof(*b));
    b->addr_mask = ((uint32_t)1 << addr_bits) - 1;
    bytes    = (size_t)b->addr_mask + 1;
    b->lines = bytes >> H8_LINE_SHIFT;
    b->mem       = (uint8_t*)malloc(bytes);
    b->line_kind = (uint8_t*)calloc(b->lines, 1);
    b->line_dev  = (const h8_device_t**)calloc(b->lines, sizeof(*b->line_dev));
    b->attr      = (uint8_t*)malloc(b->lines);
    if (!b->mem || !b->line_kind || !b->line_dev || !b->attr)
        return 0;
    memset(b->mem, 0xFF, bytes);
    memset(b->attr, H8_AT_WRSLOW | H8_W16_S3, b->lines);
    return 1;
}

static void h8_bus_free(h8_bus_t *b)
{
    free(b->mem);
    free(b->line_kind);
    free((void*)b->line_dev);
    free(b->attr);
    memset(b, 0, sizeof(*b));
}

static void h8_bus_map(h8_bus_t *b, uint32_t base, uint32_t size, int kind,
                       const h8_device_t *dev, int cls, uint8_t wait)
{
    uint8_t  at = (uint8_t)(cls | (wait << H8_AT_WAIT_SHIFT));
    uint32_t l;
    if (kind == H8_LINE_DEV)
        at |= H8_AT_RDSLOW | H8_AT_WRSLOW;
    if (kind == H8_LINE_ROM)
        at |= H8_AT_WRSLOW;
    for (l = base >> H8_LINE_SHIFT; l < (base + size) >> H8_LINE_SHIFT; ++l)
    {
        b->line_kind[l] = (uint8_t)kind;
        b->line_dev[l]  = dev;
        b->attr[l]      = (uint8_t)(at | (b->attr[l] & H8_AT_NOEXEC));
    }
    if (kind == H8_LINE_RAM)
        memset(&b->mem[base], 0, size);
}

static void h8_bus_set_unmapped(h8_bus_t *b, int cls, uint8_t wait)
{
    size_t i;
    for (i = 0; i < b->lines; ++i)
        if (b->line_kind[i] == H8_LINE_UNMAPPED)
            b->attr[i] = (uint8_t)(H8_AT_WRSLOW | cls | (wait << H8_AT_WAIT_SHIFT));
}

static void h8_bus_set_noexec(h8_bus_t *b, uint32_t base, uint32_t size, int noexec)
{
    uint32_t l;
    for (l = base >> H8_LINE_SHIFT; l < (base + size) >> H8_LINE_SHIFT; ++l)
        b->attr[l] = noexec ? (uint8_t)(b->attr[l] | H8_AT_NOEXEC) : (uint8_t)(b->attr[l] & ~H8_AT_NOEXEC);
}

static void h8_bus_load(h8_bus_t *b, uint32_t base, const uint8_t *data, size_t n)
{
    memcpy(&b->mem[base & b->addr_mask], data, n);
}

#define H8_ATTR(b, a)  ((b)->attr[(a) >> H8_LINE_SHIFT])
#define H8_BE16(p)     ((uint16_t)(((uint16_t)(p)[0] << 8) | (p)[1]))

SC88_INLINE uint8_t h8_bus_slow_read8(const h8_bus_t *b, uint32_t a)
{
    const h8_device_t *d = b->line_dev[a >> H8_LINE_SHIFT];
    return d ? d->read8(d->ctx, a) : b->mem[a];
}

SC88_INLINE uint16_t h8_bus_slow_read16(const h8_bus_t *b, uint32_t a)
{
    const h8_device_t *d = b->line_dev[a >> H8_LINE_SHIFT];
    return d ? h8_dev_read16(d, a) : H8_BE16(&b->mem[a]);
}

static void h8_bus_slow_write8(h8_bus_t *b, uint32_t a, uint8_t v)
{
    const size_t l = a >> H8_LINE_SHIFT;
    switch (b->line_kind[l])
    {
        case H8_LINE_DEV:
            b->line_dev[l]->write8(b->line_dev[l]->ctx, a, v);
            break;
        case H8_LINE_RAM:
            b->mem[a] = v;
            if ((b->attr[l] & H8_AT_CODE) && b->sink)
                h8_cpu_code_line_written(b->sink, a);
            break;
        default:
            break;
    }
}

static void h8_bus_slow_write16(h8_bus_t *b, uint32_t a, uint16_t v)
{
    const size_t l = a >> H8_LINE_SHIFT;
    switch (b->line_kind[l])
    {
        case H8_LINE_DEV:
            h8_dev_write16(b->line_dev[l], a, v);
            break;
        case H8_LINE_RAM:
            b->mem[a]     = (uint8_t)(v >> 8);
            b->mem[a + 1] = (uint8_t)v;
            if ((b->attr[l] & H8_AT_CODE) && b->sink)
                h8_cpu_code_line_written(b->sink, a);
            break;
        default:
            break;
    }
}

SC88_INLINE uint8_t h8_bus_read8(const h8_bus_t *b, uint32_t a)
{
    a &= b->addr_mask;
    return (H8_ATTR(b, a) & H8_AT_RDSLOW) ? h8_bus_slow_read8(b, a) : b->mem[a];
}

SC88_INLINE uint16_t h8_bus_read16(const h8_bus_t *b, uint32_t a)
{
    a &= b->addr_mask & ~1u;
    return (H8_ATTR(b, a) & H8_AT_RDSLOW) ? h8_bus_slow_read16(b, a) : H8_BE16(&b->mem[a]);
}

SC88_INLINE void h8_bus_write8(h8_bus_t *b, uint32_t a, uint8_t v)
{
    a &= b->addr_mask;
    if (H8_ATTR(b, a) & H8_AT_WRSLOW)
        h8_bus_slow_write8(b, a, v);
    else
        b->mem[a] = v;
}

SC88_INLINE void h8_bus_write16(h8_bus_t *b, uint32_t a, uint16_t v)
{
    a &= b->addr_mask & ~1u;
    if (H8_ATTR(b, a) & H8_AT_WRSLOW)
        h8_bus_slow_write16(b, a, v);
    else
    {
        b->mem[a]     = (uint8_t)(v >> 8);
        b->mem[a + 1] = (uint8_t)v;
    }
}

SC88_INLINE void h8_bus_mark_code(h8_bus_t *b, uint32_t a)
{
    a &= b->addr_mask;
    if (b->line_kind[a >> H8_LINE_SHIFT] == H8_LINE_RAM)
        b->attr[a >> H8_LINE_SHIFT] |= (uint8_t)(H8_AT_CODE | H8_AT_WRSLOW);
}

SC88_INLINE void h8_bus_unmark_code(h8_bus_t *b, uint32_t a)
{
    a &= b->addr_mask;
    if (b->line_kind[a >> H8_LINE_SHIFT] == H8_LINE_RAM)
        b->attr[a >> H8_LINE_SHIFT] &= (uint8_t)~(H8_AT_CODE | H8_AT_WRSLOW);
}

/* ---- chip configuration ---- */

enum { H8_MODEL_510, H8_MODEL_570, H8_MODEL_532 };

typedef struct
{
    int      model;
    uint8_t  mode;
    uint32_t rom_base, rom_size;
    uint32_t ram_base, ram_size;
    uint32_t regfield_base, regfield_size;
    uint32_t noexec_base, noexec_size;
    int      external_bus_16bit;
    uint8_t  max_mode_addr_bits;
} h8_chip_config_t;

static int h8_chip_max_mode(const h8_chip_config_t *c)
{
    switch (c->model)
    {
        case H8_MODEL_510: return c->mode == 3 || c->mode == 4;
        case H8_MODEL_570: return c->mode == 3 || c->mode == 5 || c->mode == 6;
        case H8_MODEL_532: return c->mode == 3 || c->mode == 4;
    }
    return 0;
}

static unsigned h8_chip_address_bits(const h8_chip_config_t *c)
{
    return h8_chip_max_mode(c) ? c->max_mode_addr_bits : 16;
}

static void h8_make_chip_config(h8_chip_config_t *c, int model, uint8_t mode)
{
    memset(c, 0, sizeof(*c));
    c->model = model;
    c->mode  = mode;
    c->regfield_base = 0xFE80; c->regfield_size = 0x180;
    c->noexec_base   = 0xFE80; c->noexec_size   = 0x180;
    c->external_bus_16bit = 1;
    c->max_mode_addr_bits = 24;
    switch (model)
    {
        case H8_MODEL_510:
            c->external_bus_16bit = (mode == 2 || mode == 4);
            break;
        case H8_MODEL_570:
            c->ram_base = 0xF680; c->ram_size = 0x0800;
            c->regfield_base = 0xFE80; c->regfield_size = 0x100;
            c->noexec_base   = 0xFE80; c->noexec_size   = 0x100;
            c->external_bus_16bit = (mode == 1 || mode == 3 || mode == 5);
            c->max_mode_addr_bits = 20;
            break;
        case H8_MODEL_532:
            c->rom_base = 0x0000; c->rom_size = 0x8000;
            c->ram_base = 0xFB80; c->ram_size = 0x0400;
            c->regfield_base = 0xFF80; c->regfield_size = 0x80;
            c->noexec_base   = 0xFF80; c->noexec_size   = 0x80;
            c->external_bus_16bit = 0;
            c->max_mode_addr_bits = 20;
            break;
    }
}

static void h8_configure_bus(h8_bus_t *b, const h8_chip_config_t *c)
{
    h8_bus_set_unmapped(b, c->external_bus_16bit ? H8_W16_S3 : H8_W8_S3, 0);
    if (c->rom_size)
        h8_bus_map(b, c->rom_base, c->rom_size, H8_LINE_ROM, NULL, H8_W16_S2, 0);
    if (c->ram_size)
        h8_bus_map(b, c->ram_base, c->ram_size, H8_LINE_RAM, NULL, H8_W16_S2, 0);
    h8_bus_set_noexec(b, c->noexec_base, c->noexec_size, 1);
}

/* ---- decode ---- */

enum
{
    H8_OP_INVALID = 0,
    H8_OP_MOV, H8_OP_MOVFPE, H8_OP_MOVTPE, H8_OP_LDM, H8_OP_STM, H8_OP_XCH, H8_OP_SWAP,
    H8_OP_ADD, H8_OP_ADDQ, H8_OP_ADDS, H8_OP_ADDX, H8_OP_DADD, H8_OP_SUB, H8_OP_SUBS, H8_OP_SUBX,
    H8_OP_DSUB, H8_OP_MULXU, H8_OP_DIVXU, H8_OP_CMP,
    H8_OP_EXTS, H8_OP_EXTU, H8_OP_TST, H8_OP_NEG, H8_OP_CLR, H8_OP_TAS,
    H8_OP_SHAL, H8_OP_SHAR, H8_OP_SHLL, H8_OP_SHLR, H8_OP_ROTL, H8_OP_ROTR, H8_OP_ROTXL, H8_OP_ROTXR,
    H8_OP_AND, H8_OP_OR, H8_OP_XOR, H8_OP_NOT,
    H8_OP_BSET, H8_OP_BCLR, H8_OP_BTST, H8_OP_BNOT,
    H8_OP_LDC, H8_OP_STC, H8_OP_ANDC, H8_OP_ORC, H8_OP_XORC, H8_OP_TRAPA, H8_OP_TRAPVS, H8_OP_RTE,
    H8_OP_LINK, H8_OP_UNLK, H8_OP_SLEEP, H8_OP_NOP,
    H8_OP_BCC, H8_OP_JMP, H8_OP_BSR, H8_OP_JSR, H8_OP_RTS, H8_OP_RTD, H8_OP_SCB,
    H8_OP_PJMP, H8_OP_PJSR, H8_OP_PRTS, H8_OP_PRTD
};

enum
{
    H8_EA_NONE = 0, H8_EA_REG, H8_EA_REGIND, H8_EA_DISP8, H8_EA_DISP16, H8_EA_PREDEC, H8_EA_POSTINC,
    H8_EA_ABS8, H8_EA_ABS16, H8_EA_IMM, H8_EA_ABS24, H8_EA_PCREL8, H8_EA_PCREL16
};

enum { H8_SZ_NONE = 0, H8_SZ_BYTE = 1, H8_SZ_WORD = 2 };

#define H8_F_STORE        0x01
#define H8_F_IMMSRC       0x02
#define H8_F_IMM8         0x04
#define H8_F_SHORT        0x08
#define H8_F_BITINREG     0x10
#define H8_F_MAXMODEONLY  0x20
#define H8_F_PREFIXED     0x40

#define H8_EA_IS_MEMORY(m) ((m) >= H8_EA_REGIND && (m) <= H8_EA_ABS16)

typedef struct
{
    uint8_t op, size, ea, ea_reg, reg, aux, length, flags;
    int32_t ea_ext, imm;
} h8_insn_t;

/* The decoder pulls code bytes through this, as the C++ does through a
 * callable: a read of device-backed memory happens once per byte use. */
typedef struct
{
    const h8_bus_t *bus;
    uint32_t page;
    uint16_t off;
    int      max_mode;
} h8_fetch_t;

SC88_INLINE uint8_t h8_fetch(const h8_fetch_t *f, unsigned i)
{
    const uint16_t o = (uint16_t)(f->off + i);
    return h8_bus_read8(f->bus, f->max_mode ? (f->page | o) : (uint32_t)o);
}

SC88_INLINE uint16_t h8_fetch16(const h8_fetch_t *f, unsigned pos)
{
    const unsigned hi = h8_fetch(f, pos);
    return (uint16_t)((hi << 8) | (unsigned)h8_fetch(f, pos + 1));
}

#define H8_FAIL() do { d->op = H8_OP_INVALID; d->length = (uint8_t)pos; return; } while (0)

static void h8_decode_general(const h8_fetch_t *f, h8_insn_t *d, uint8_t b0)
{
    unsigned pos = 1;
    int is_reg, is_imm, is_mem;
    uint8_t op;

    d->size   = (b0 & 0x08) ? H8_SZ_WORD : H8_SZ_BYTE;
    d->ea_reg = (uint8_t)(b0 & 7);
    switch (b0 >> 4)
    {
        case 0xA: d->ea = H8_EA_REG; break;
        case 0xB: d->ea = H8_EA_PREDEC; break;
        case 0xC: d->ea = H8_EA_POSTINC; break;
        case 0xD: d->ea = H8_EA_REGIND; break;
        case 0xE: d->ea = H8_EA_DISP8; d->ea_ext = (int8_t)h8_fetch(f, pos); pos += 1; break;
        case 0xF: d->ea = H8_EA_DISP16; d->ea_ext = (int16_t)h8_fetch16(f, pos); pos += 2; break;
        case 0x0:
            d->ea_reg = 0;
            if ((b0 & 0x07) == 0x04)
            {
                d->ea = H8_EA_IMM;
                if (d->size == H8_SZ_BYTE) { d->imm = (int8_t)h8_fetch(f, pos); pos += 1; }
                else { d->imm = (int16_t)h8_fetch16(f, pos); pos += 2; }
            }
            else
            {
                d->ea = H8_EA_ABS8;
                d->ea_ext = h8_fetch(f, pos); pos += 1;
            }
            break;
        default:
            d->ea_reg = 0;
            d->ea = H8_EA_ABS16;
            d->ea_ext = h8_fetch16(f, pos); pos += 2;
            break;
    }
    is_reg = d->ea == H8_EA_REG;
    is_imm = d->ea == H8_EA_IMM;
    is_mem = !is_reg && !is_imm;
    op = h8_fetch(f, pos++);
    d->reg = (uint8_t)(op & 7);

    if (op < 0x20)
    {
        switch (op)
        {
            case 0x00:
            {
                const uint8_t b = h8_fetch(f, pos++);
                d->reg = (uint8_t)(b & 7);
                d->flags |= H8_F_PREFIXED;
                switch (b & 0xF8)
                {
                    case 0x80: if (!is_mem) H8_FAIL(); d->op = H8_OP_MOVFPE; break;
                    case 0x90: if (!is_mem) H8_FAIL(); d->op = H8_OP_MOVTPE; d->flags |= H8_F_STORE; break;
                    case 0xA0: if (!is_reg) H8_FAIL(); d->op = H8_OP_DADD; break;
                    case 0xB0: if (!is_reg) H8_FAIL(); d->op = H8_OP_DSUB; break;
                    default: H8_FAIL();
                }
                break;
            }
            case 0x04: case 0x05: case 0x06: case 0x07:
                if (!is_mem) H8_FAIL();
                d->op = (op < 0x06) ? H8_OP_CMP : H8_OP_MOV;
                d->flags |= H8_F_IMMSRC;
                if ((op & 1) == 0)
                {
                    d->imm = (int8_t)h8_fetch(f, pos); pos += 1; d->flags |= H8_F_IMM8;
                }
                else
                {
                    d->imm = (int16_t)h8_fetch16(f, pos); pos += 2;
                }
                break;
            case 0x08: case 0x09: case 0x0C: case 0x0D:
                if (is_imm) H8_FAIL();
                d->op = H8_OP_ADDQ;
                d->flags |= H8_F_SHORT | H8_F_IMMSRC;
                d->imm = (op == 0x08) ? 1 : (op == 0x09) ? 2 : (op == 0x0C) ? -1 : -2;
                break;
            case 0x10: if (!is_reg) H8_FAIL(); d->op = H8_OP_SWAP; d->reg = d->ea_reg; break;
            case 0x11: if (!is_reg) H8_FAIL(); d->op = H8_OP_EXTS; d->reg = d->ea_reg; break;
            case 0x12: if (!is_reg) H8_FAIL(); d->op = H8_OP_EXTU; d->reg = d->ea_reg; break;
            case 0x13: if (is_imm) H8_FAIL(); d->op = H8_OP_CLR; break;
            case 0x14: if (is_imm) H8_FAIL(); d->op = H8_OP_NEG; break;
            case 0x15: if (is_imm) H8_FAIL(); d->op = H8_OP_NOT; break;
            case 0x16: if (is_imm) H8_FAIL(); d->op = H8_OP_TST; break;
            case 0x17: if (is_imm) H8_FAIL(); d->op = H8_OP_TAS; break;
            case 0x18: if (is_imm) H8_FAIL(); d->op = H8_OP_SHAL; break;
            case 0x19: if (is_imm) H8_FAIL(); d->op = H8_OP_SHAR; break;
            case 0x1A: if (is_imm) H8_FAIL(); d->op = H8_OP_SHLL; break;
            case 0x1B: if (is_imm) H8_FAIL(); d->op = H8_OP_SHLR; break;
            case 0x1C: if (is_imm) H8_FAIL(); d->op = H8_OP_ROTL; break;
            case 0x1D: if (is_imm) H8_FAIL(); d->op = H8_OP_ROTR; break;
            case 0x1E: if (is_imm) H8_FAIL(); d->op = H8_OP_ROTXL; break;
            case 0x1F: if (is_imm) H8_FAIL(); d->op = H8_OP_ROTXR; break;
            default: H8_FAIL();
        }
        d->length = (uint8_t)pos;
        return;
    }

    switch (op >> 3)
    {
        case 0x04: d->op = H8_OP_ADD; break;
        case 0x05: d->op = H8_OP_ADDS; break;
        case 0x06: d->op = H8_OP_SUB; break;
        case 0x07: d->op = H8_OP_SUBS; break;
        case 0x08: d->op = H8_OP_OR; break;
        case 0x09:
            if (is_imm) d->op = H8_OP_ORC; else { d->op = H8_OP_BSET; d->flags |= H8_F_BITINREG; }
            break;
        case 0x0A: d->op = H8_OP_AND; break;
        case 0x0B:
            if (is_imm) d->op = H8_OP_ANDC; else { d->op = H8_OP_BCLR; d->flags |= H8_F_BITINREG; }
            break;
        case 0x0C: d->op = H8_OP_XOR; break;
        case 0x0D:
            if (is_imm) d->op = H8_OP_XORC; else { d->op = H8_OP_BNOT; d->flags |= H8_F_BITINREG; }
            break;
        case 0x0E: d->op = H8_OP_CMP; break;
        case 0x0F:
            if (is_imm) H8_FAIL();
            d->op = H8_OP_BTST; d->flags |= H8_F_BITINREG;
            break;
        case 0x10: d->op = H8_OP_MOV; break;
        case 0x11: d->op = H8_OP_LDC; break;
        case 0x12:
            if (is_imm) H8_FAIL();
            if (is_reg) d->op = H8_OP_XCH; else { d->op = H8_OP_MOV; d->flags |= H8_F_STORE; }
            break;
        case 0x13:
            if (is_imm) H8_FAIL();
            d->op = H8_OP_STC; d->flags |= H8_F_STORE;
            break;
        case 0x14: d->op = H8_OP_ADDX; break;
        case 0x15: d->op = H8_OP_MULXU; break;
        case 0x16: d->op = H8_OP_SUBX; break;
        case 0x17: d->op = H8_OP_DIVXU; break;
        case 0x18: case 0x19: if (is_imm) H8_FAIL(); d->op = H8_OP_BSET; d->aux = (uint8_t)(op & 0x0F); break;
        case 0x1A: case 0x1B: if (is_imm) H8_FAIL(); d->op = H8_OP_BCLR; d->aux = (uint8_t)(op & 0x0F); break;
        case 0x1C: case 0x1D: if (is_imm) H8_FAIL(); d->op = H8_OP_BNOT; d->aux = (uint8_t)(op & 0x0F); break;
        default:              if (is_imm) H8_FAIL(); d->op = H8_OP_BTST; d->aux = (uint8_t)(op & 0x0F); break;
    }
    d->length = (uint8_t)pos;
}

static void h8_decode_special(const h8_fetch_t *f, h8_insn_t *d, uint8_t b0)
{
    unsigned pos = 1;

    if (b0 >= 0x20)
    {
        const uint8_t sz = (b0 & 0x08) ? H8_SZ_WORD : H8_SZ_BYTE;
        d->reg = (uint8_t)(b0 & 7);
        switch (b0 >> 4)
        {
            case 0x2:
                d->op = H8_OP_BCC; d->reg = (uint8_t)(b0 & 0x0F); d->ea = H8_EA_PCREL8;
                d->ea_ext = (int8_t)h8_fetch(f, pos); pos += 1;
                break;
            case 0x3:
                d->op = H8_OP_BCC; d->reg = (uint8_t)(b0 & 0x0F); d->ea = H8_EA_PCREL16;
                d->ea_ext = (int16_t)h8_fetch16(f, pos); pos += 2;
                break;
            case 0x4:
            case 0x5:
                d->op = (b0 >> 4) == 0x4 ? H8_OP_CMP : H8_OP_MOV;
                d->ea = H8_EA_IMM; d->flags |= H8_F_SHORT; d->size = sz;
                if (sz == H8_SZ_WORD) { d->imm = (int16_t)h8_fetch16(f, pos); pos += 2; }
                else { d->imm = (int8_t)h8_fetch(f, pos); pos += 1; }
                break;
            case 0x6:
            case 0x7:
                d->op = H8_OP_MOV; d->flags |= H8_F_SHORT; d->size = sz; d->ea = H8_EA_ABS8;
                d->ea_ext = h8_fetch(f, pos); pos += 1;
                if ((b0 >> 4) == 0x7) d->flags |= H8_F_STORE;
                break;
            case 0x8:
            case 0x9:
                d->op = H8_OP_MOV; d->flags |= H8_F_SHORT; d->size = sz; d->ea = H8_EA_DISP8; d->ea_reg = 6;
                d->ea_ext = (int8_t)h8_fetch(f, pos); pos += 1;
                if ((b0 >> 4) == 0x9) d->flags |= H8_F_STORE;
                break;
            default:
                H8_FAIL();
        }
        d->length = (uint8_t)pos;
        return;
    }

    switch (b0)
    {
        case 0x00: d->op = H8_OP_NOP; break;
        case 0x01: case 0x06: case 0x07:
        {
            const uint8_t b1 = h8_fetch(f, pos++);
            if ((b1 & 0xF8) != 0xB8) H8_FAIL();
            d->op = H8_OP_SCB; d->reg = (uint8_t)(b1 & 7);
            d->aux = (uint8_t)(b0 == 0x01 ? 0 : b0 == 0x06 ? 1 : 2);
            d->ea = H8_EA_PCREL8; d->ea_ext = (int8_t)h8_fetch(f, pos); pos += 1;
            break;
        }
        case 0x02: d->op = H8_OP_LDM; d->size = H8_SZ_WORD; d->reg = h8_fetch(f, pos++); break;
        case 0x12: d->op = H8_OP_STM; d->size = H8_SZ_WORD; d->reg = h8_fetch(f, pos++); break;
        case 0x03: case 0x13:
        {
            uint32_t a, b, c;
            d->op = (b0 == 0x03) ? H8_OP_PJSR : H8_OP_PJMP;
            d->ea = H8_EA_ABS24; d->flags |= H8_F_MAXMODEONLY;
            a = h8_fetch(f, pos); b = h8_fetch(f, pos + 1); c = h8_fetch(f, pos + 2);
            d->ea_ext = (int32_t)((a << 16) | (b << 8) | c);
            pos += 3;
            break;
        }
        case 0x08:
        {
            const uint8_t b1 = h8_fetch(f, pos++);
            d->op = H8_OP_TRAPA; d->reg = (uint8_t)(b1 & 0x0F);
            break;
        }
        case 0x09: d->op = H8_OP_TRAPVS; break;
        case 0x0A: d->op = H8_OP_RTE; break;
        case 0x0E: d->op = H8_OP_BSR; d->ea = H8_EA_PCREL8; d->ea_ext = (int8_t)h8_fetch(f, pos); pos += 1; break;
        case 0x1E: d->op = H8_OP_BSR; d->ea = H8_EA_PCREL16; d->ea_ext = (int16_t)h8_fetch16(f, pos); pos += 2; break;
        case 0x0F: d->op = H8_OP_UNLK; break;
        case 0x10: d->op = H8_OP_JMP; d->ea = H8_EA_ABS16; d->ea_ext = h8_fetch16(f, pos); pos += 2; break;
        case 0x18: d->op = H8_OP_JSR; d->ea = H8_EA_ABS16; d->ea_ext = h8_fetch16(f, pos); pos += 2; break;
        case 0x11:
        {
            const uint8_t b1 = h8_fetch(f, pos++);
            switch (b1)
            {
                case 0x14: d->op = H8_OP_PRTD; d->flags |= H8_F_MAXMODEONLY | H8_F_IMM8;
                           d->imm = (int8_t)h8_fetch(f, pos); pos += 1; break;
                case 0x19: d->op = H8_OP_PRTS; d->flags |= H8_F_MAXMODEONLY; break;
                case 0x1C: d->op = H8_OP_PRTD; d->flags |= H8_F_MAXMODEONLY;
                           d->imm = (int16_t)h8_fetch16(f, pos); pos += 2; break;
                default:
                {
                    int sub;
                    if (b1 < 0xC0) H8_FAIL();
                    d->ea_reg = (uint8_t)(b1 & 7);
                    sub = (b1 & 0x08) != 0;
                    switch (b1 >> 4)
                    {
                        case 0xC: d->op = sub ? H8_OP_PJSR : H8_OP_PJMP; d->ea = H8_EA_REGIND;
                                  d->flags |= H8_F_MAXMODEONLY; break;
                        case 0xD: d->op = sub ? H8_OP_JSR : H8_OP_JMP; d->ea = H8_EA_REGIND; break;
                        case 0xE: d->op = sub ? H8_OP_JSR : H8_OP_JMP; d->ea = H8_EA_DISP8;
                                  d->ea_ext = (int8_t)h8_fetch(f, pos); pos += 1; break;
                        default:  d->op = sub ? H8_OP_JSR : H8_OP_JMP; d->ea = H8_EA_DISP16;
                                  d->ea_ext = (int16_t)h8_fetch16(f, pos); pos += 2; break;
                    }
                    break;
                }
            }
            break;
        }
        case 0x14: d->op = H8_OP_RTD; d->flags |= H8_F_IMM8; d->imm = (int8_t)h8_fetch(f, pos); pos += 1; break;
        case 0x1C: d->op = H8_OP_RTD; d->imm = (int16_t)h8_fetch16(f, pos); pos += 2; break;
        case 0x17: d->op = H8_OP_LINK; d->flags |= H8_F_IMM8; d->imm = (int8_t)h8_fetch(f, pos); pos += 1; break;
        case 0x1F: d->op = H8_OP_LINK; d->imm = (int16_t)h8_fetch16(f, pos); pos += 2; break;
        case 0x19: d->op = H8_OP_RTS; break;
        case 0x1A: d->op = H8_OP_SLEEP; break;
        default: H8_FAIL();
    }
    d->length = (uint8_t)pos;
}

static void h8_decode(const h8_fetch_t *f, h8_insn_t *d)
{
    uint8_t b0;
    memset(d, 0, sizeof(*d));
    b0 = h8_fetch(f, 0);
    if ((b0 >= 0xA0) || (b0 & 0xE7) == 0x05 || (b0 & 0xF7) == 0x04)
        h8_decode_general(f, d, b0);
    else
        h8_decode_special(f, d, b0);
}

/* ---- timing (timing.cpp) ---- */

typedef struct { uint8_t I, JK; uint8_t c[10]; } h8_row_t;

static const h8_row_t h8_kAluB     = {1, 1, {2, 5, 5, 6, 5, 6, 5, 6, 3, 0}};
static const h8_row_t h8_kAluW     = {2, 1, {2, 5, 5, 6, 5, 6, 5, 6, 0, 4}};
static const h8_row_t h8_kAddsB    = {1, 1, {3, 5, 5, 6, 5, 6, 5, 6, 3, 0}};
static const h8_row_t h8_kAddsW    = {2, 1, {3, 5, 5, 6, 5, 6, 5, 6, 0, 4}};
static const h8_row_t h8_kRmwB     = {2, 1, {2, 7, 7, 8, 7, 8, 7, 8, 0, 0}};
static const h8_row_t h8_kRmwW     = {4, 1, {2, 7, 7, 8, 7, 8, 7, 8, 0, 0}};
static const h8_row_t h8_kBitModB  = {2, 1, {4, 7, 7, 8, 7, 8, 7, 8, 0, 0}};
static const h8_row_t h8_kBitModW  = {4, 1, {4, 7, 7, 8, 7, 8, 7, 8, 0, 0}};
static const h8_row_t h8_kBtstB    = {1, 1, {3, 5, 5, 6, 5, 6, 5, 6, 0, 0}};
static const h8_row_t h8_kBtstW    = {2, 1, {3, 5, 5, 6, 5, 6, 5, 6, 0, 0}};
static const h8_row_t h8_kClrB     = {1, 1, {2, 5, 5, 6, 5, 6, 5, 6, 0, 0}};
static const h8_row_t h8_kClrW     = {2, 1, {2, 5, 5, 6, 5, 6, 5, 6, 0, 0}};
static const h8_row_t h8_kCmpImm8  = {1, 2, {0, 6, 6, 7, 6, 7, 6, 7, 0, 0}};
static const h8_row_t h8_kCmpImm16 = {2, 3, {0, 7, 7, 8, 7, 8, 7, 8, 0, 0}};
static const h8_row_t h8_kMovImm8  = {1, 2, {0, 7, 7, 8, 7, 8, 7, 8, 0, 0}};
static const h8_row_t h8_kMovImm16 = {2, 3, {0, 8, 8, 9, 8, 9, 8, 9, 0, 0}};
static const h8_row_t h8_kDivB     = {1, 1, {20, 23, 23, 24, 23, 24, 23, 24, 21, 0}};
static const h8_row_t h8_kDivW     = {2, 1, {26, 29, 29, 30, 29, 30, 29, 30, 0, 28}};
static const h8_row_t h8_kDivZeroBMin = {6, 1, {20, 23, 23, 24, 23, 24, 23, 24, 21, 0}};
static const h8_row_t h8_kDivZeroBMax = {10, 1, {25, 28, 28, 29, 28, 29, 28, 29, 26, 0}};
static const h8_row_t h8_kDivZeroWMin = {6, 1, {20, 23, 23, 24, 23, 24, 23, 24, 0, 27}};
static const h8_row_t h8_kDivZeroWMax = {10, 1, {25, 28, 28, 29, 28, 29, 28, 29, 0, 32}};
static const h8_row_t h8_kDivOvfB  = {1, 1, {8, 11, 11, 12, 11, 12, 11, 12, 9, 0}};
static const h8_row_t h8_kDivOvfW  = {2, 1, {8, 11, 11, 12, 11, 12, 11, 12, 0, 10}};
static const h8_row_t h8_kLdcB     = {1, 1, {3, 6, 6, 7, 6, 7, 6, 7, 4, 0}};
static const h8_row_t h8_kLdcW     = {2, 1, {4, 7, 7, 8, 7, 8, 7, 8, 0, 6}};
static const h8_row_t h8_kMovPe    = {0, 2, {0, 13, 13, 14, 13, 13, 13, 14, 0, 0}};
static const h8_row_t h8_kMulB     = {1, 1, {16, 19, 19, 20, 19, 20, 19, 20, 18, 0}};
static const h8_row_t h8_kMulW     = {2, 1, {23, 25, 25, 26, 25, 26, 25, 26, 0, 25}};
static const h8_row_t h8_kStcB     = {1, 1, {4, 7, 7, 8, 7, 8, 7, 8, 0, 0}};
static const h8_row_t h8_kStcW     = {2, 1, {4, 7, 7, 8, 7, 8, 7, 8, 0, 0}};
static const h8_row_t h8_kTas      = {2, 1, {4, 7, 7, 8, 7, 8, 7, 8, 0, 0}};
static const h8_row_t h8_kCtlImm   = {0, 1, {0, 0, 0, 0, 0, 0, 0, 0, 5, 9}};
static const h8_row_t h8_kCmpE     = {0, 0, {0, 0, 0, 0, 0, 0, 0, 0, 2, 0}};
static const h8_row_t h8_kCmpI     = {0, 0, {0, 0, 0, 0, 0, 0, 0, 0, 0, 3}};
static const h8_row_t h8_kMovE     = {0, 0, {0, 0, 0, 0, 0, 0, 0, 0, 2, 0}};
static const h8_row_t h8_kMovI     = {0, 0, {0, 0, 0, 0, 0, 0, 0, 0, 0, 3}};
static const h8_row_t h8_kMovLSB   = {1, 0, {0, 0, 0, 0, 0, 0, 5, 0, 0, 0}};
static const h8_row_t h8_kMovLSW   = {2, 0, {0, 0, 0, 0, 0, 0, 5, 0, 0, 0}};
static const h8_row_t h8_kMovFB    = {1, 0, {0, 0, 5, 0, 0, 0, 0, 0, 0, 0}};
static const h8_row_t h8_kMovFW    = {2, 0, {0, 0, 5, 0, 0, 0, 0, 0, 0, 0}};
static const h8_row_t h8_kDaddDsub = {0, 2, {4, 0, 0, 0, 0, 0, 0, 0, 0, 0}};
static const h8_row_t h8_kExt      = {0, 1, {3, 0, 0, 0, 0, 0, 0, 0, 0, 0}};
static const h8_row_t h8_kXch      = {0, 1, {4, 0, 0, 0, 0, 0, 0, 0, 0, 0}};

enum { H8_COND_NORMAL, H8_COND_TAKEN, H8_COND_NOT_TAKEN, H8_COND_SCB_MINUS1, H8_COND_DIV_ZERO, H8_COND_DIV_OVERFLOW };

typedef struct { uint8_t states, I, JK; } h8_base_timing_t;

static int h8_ea_column(const h8_insn_t *d)
{
    switch (d->ea)
    {
        case H8_EA_REG:     return 0;
        case H8_EA_REGIND:  return 1;
        case H8_EA_DISP8:   return 2;
        case H8_EA_DISP16:  return 3;
        case H8_EA_PREDEC:  return 4;
        case H8_EA_POSTINC: return 5;
        case H8_EA_ABS8:    return 6;
        case H8_EA_ABS16:   return 7;
        case H8_EA_IMM:     return d->size == H8_SZ_WORD ? 9 : 8;
        default:            return -1;
    }
}

static h8_base_timing_t h8_from_row(const h8_row_t *r, const h8_insn_t *d)
{
    static const uint8_t kJ[10] = {1, 1, 2, 3, 1, 1, 2, 3, 2, 3};
    const int col = h8_ea_column(d);
    h8_base_timing_t t;
    t.I      = r->I;
    t.JK     = (uint8_t)(r->JK + (col >= 0 ? kJ[col] : 0));
    t.states = col >= 0 ? r->c[col] : 0;
    return t;
}

static h8_base_timing_t h8_bt(unsigned states, unsigned I, unsigned JK)
{
    h8_base_timing_t t;
    t.states = (uint8_t)states;
    t.I      = (uint8_t)I;
    t.JK     = (uint8_t)JK;
    return t;
}

#define H8_HAS(d, f) (((d)->flags & (f)) != 0)

static h8_base_timing_t h8_base_timing(const h8_insn_t *d, int max_mode, int cond, unsigned n_regs)
{
    const int W = d->size == H8_SZ_WORD;
    switch (d->op)
    {
        case H8_OP_MOV:
            if (H8_HAS(d, H8_F_SHORT))
            {
                if (d->ea == H8_EA_IMM)  return h8_from_row(W ? &h8_kMovI : &h8_kMovE, d);
                if (d->ea == H8_EA_ABS8) return h8_from_row(W ? &h8_kMovLSW : &h8_kMovLSB, d);
                return h8_from_row(W ? &h8_kMovFW : &h8_kMovFB, d);
            }
            if (H8_HAS(d, H8_F_IMMSRC))
                return h8_from_row(H8_HAS(d, H8_F_IMM8) ? &h8_kMovImm8 : &h8_kMovImm16, d);
            return h8_from_row(W ? &h8_kAluW : &h8_kAluB, d);
        case H8_OP_CMP:
            if (H8_HAS(d, H8_F_SHORT))
                return h8_from_row(W ? &h8_kCmpI : &h8_kCmpE, d);
            if (H8_HAS(d, H8_F_IMMSRC))
                return h8_from_row(H8_HAS(d, H8_F_IMM8) ? &h8_kCmpImm8 : &h8_kCmpImm16, d);
            return h8_from_row(W ? &h8_kAluW : &h8_kAluB, d);
        case H8_OP_ADD: case H8_OP_SUB: case H8_OP_AND: case H8_OP_OR: case H8_OP_XOR:
        case H8_OP_ADDX: case H8_OP_SUBX:
            return h8_from_row(W ? &h8_kAluW : &h8_kAluB, d);
        case H8_OP_ADDS: case H8_OP_SUBS:
            return h8_from_row(W ? &h8_kAddsW : &h8_kAddsB, d);
        case H8_OP_ADDQ: case H8_OP_NEG: case H8_OP_NOT:
        case H8_OP_SHAL: case H8_OP_SHAR: case H8_OP_SHLL: case H8_OP_SHLR:
        case H8_OP_ROTL: case H8_OP_ROTR: case H8_OP_ROTXL: case H8_OP_ROTXR:
            return h8_from_row(W ? &h8_kRmwW : &h8_kRmwB, d);
        case H8_OP_BSET: case H8_OP_BCLR: case H8_OP_BNOT:
            return h8_from_row(W ? &h8_kBitModW : &h8_kBitModB, d);
        case H8_OP_BTST:
            return h8_from_row(W ? &h8_kBtstW : &h8_kBtstB, d);
        case H8_OP_CLR: case H8_OP_TST:
            return h8_from_row(W ? &h8_kClrW : &h8_kClrB, d);
        case H8_OP_TAS:
            return h8_from_row(&h8_kTas, d);
        case H8_OP_MULXU:
            return h8_from_row(W ? &h8_kMulW : &h8_kMulB, d);
        case H8_OP_DIVXU:
            if (cond == H8_COND_DIV_OVERFLOW)
                return h8_from_row(W ? &h8_kDivOvfW : &h8_kDivOvfB, d);
            if (cond == H8_COND_DIV_ZERO)
            {
                const h8_row_t *r = W ? (max_mode ? &h8_kDivZeroWMax : &h8_kDivZeroWMin)
                                      : (max_mode ? &h8_kDivZeroBMax : &h8_kDivZeroBMin);
                h8_base_timing_t t = h8_from_row(r, d);
                if (H8_EA_IS_MEMORY(d->ea))
                    t.I = (uint8_t)(t.I + (W ? 2 : 1));
                return t;
            }
            return h8_from_row(W ? &h8_kDivW : &h8_kDivB, d);
        case H8_OP_LDC: return h8_from_row(W ? &h8_kLdcW : &h8_kLdcB, d);
        case H8_OP_STC: return h8_from_row(W ? &h8_kStcW : &h8_kStcB, d);
        case H8_OP_ANDC: case H8_OP_ORC: case H8_OP_XORC: return h8_from_row(&h8_kCtlImm, d);
        case H8_OP_MOVFPE: case H8_OP_MOVTPE: return h8_from_row(&h8_kMovPe, d);
        case H8_OP_DADD: case H8_OP_DSUB: return h8_from_row(&h8_kDaddDsub, d);
        case H8_OP_EXTS: case H8_OP_EXTU: case H8_OP_SWAP: return h8_from_row(&h8_kExt, d);
        case H8_OP_XCH: return h8_from_row(&h8_kXch, d);
        case H8_OP_BCC:
            if (d->ea == H8_EA_PCREL8)
                return cond == H8_COND_TAKEN ? h8_bt(7, 0, 5) : h8_bt(3, 0, 2);
            return cond == H8_COND_TAKEN ? h8_bt(7, 0, 6) : h8_bt(3, 0, 3);
        case H8_OP_BSR:
            return d->ea == H8_EA_PCREL8 ? h8_bt(9, 2, 4) : h8_bt(9, 2, 5);
        case H8_OP_JMP:
            switch (d->ea)
            {
                case H8_EA_ABS16:  return h8_bt(7, 0, 5);
                case H8_EA_REGIND: return h8_bt(6, 0, 5);
                case H8_EA_DISP8:  return h8_bt(7, 0, 5);
                default:           return h8_bt(8, 0, 6);
            }
        case H8_OP_JSR:
            switch (d->ea)
            {
                case H8_EA_ABS16:  return h8_bt(9, 2, 5);
                case H8_EA_REGIND: return h8_bt(9, 2, 5);
                case H8_EA_DISP8:  return h8_bt(9, 2, 5);
                default:           return h8_bt(10, 2, 6);
            }
        case H8_OP_LDM:  return h8_bt(6 + 4 * n_regs, 2 * n_regs, 2);
        case H8_OP_STM:  return h8_bt(6 + 3 * n_regs, 2 * n_regs, 2);
        case H8_OP_LINK: return H8_HAS(d, H8_F_IMM8) ? h8_bt(6, 2, 2) : h8_bt(7, 2, 3);
        case H8_OP_NOP:  return h8_bt(2, 0, 1);
        case H8_OP_RTD:  return H8_HAS(d, H8_F_IMM8) ? h8_bt(9, 2, 4) : h8_bt(9, 2, 5);
        case H8_OP_RTE:  return max_mode ? h8_bt(15, 6, 4) : h8_bt(13, 4, 4);
        case H8_OP_RTS:  return h8_bt(8, 2, 4);
        case H8_OP_SCB:
            switch (cond)
            {
                case H8_COND_TAKEN:      return h8_bt(8, 0, 6);
                case H8_COND_SCB_MINUS1: return h8_bt(4, 0, 3);
                default:                 return h8_bt(3, 0, 3);
            }
        case H8_OP_SLEEP: return h8_bt(2, 0, 0);
        case H8_OP_TRAPA: return max_mode ? h8_bt(22, 10, 4) : h8_bt(17, 6, 4);
        case H8_OP_TRAPVS:
            if (cond != H8_COND_TAKEN)
                return h8_bt(3, 0, 1);
            return max_mode ? h8_bt(23, 10, 4) : h8_bt(18, 6, 4);
        case H8_OP_UNLK: return h8_bt(5, 2, 1);
        case H8_OP_PJMP: return d->ea == H8_EA_ABS24 ? h8_bt(9, 0, 6) : h8_bt(8, 0, 5);
        case H8_OP_PJSR: return d->ea == H8_EA_ABS24 ? h8_bt(15, 4, 6) : h8_bt(13, 4, 5);
        case H8_OP_PRTS: return h8_bt(12, 4, 5);
        case H8_OP_PRTD: return H8_HAS(d, H8_F_IMM8) ? h8_bt(13, 4, 5) : h8_bt(13, 4, 6);
        default:
            break;
    }
    return h8_bt(0, 0, 0);
}

static unsigned h8_parity_adjustment(const h8_insn_t *d, int odd_start, int cond)
{
    static const uint8_t kEven[10] = {0, 1, 0, 1, 1, 1, 0, 1, 0, 0};
    static const uint8_t kOdd[10]  = {0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    int col;
    switch (d->op)
    {
        case H8_OP_BSR: case H8_OP_JMP: case H8_OP_JSR: case H8_OP_RTS: case H8_OP_RTD: case H8_OP_RTE:
        case H8_OP_TRAPA: case H8_OP_PJMP: case H8_OP_PJSR: case H8_OP_PRTS: case H8_OP_PRTD:
            return odd_start ? 1 : 0;
        case H8_OP_BCC: case H8_OP_SCB: case H8_OP_TRAPVS:
            return (cond == H8_COND_TAKEN && odd_start) ? 1 : 0;
        case H8_OP_MOVFPE: case H8_OP_MOVTPE:
            return 1;
        case H8_OP_MOV:
            if (H8_HAS(d, H8_F_IMMSRC) && !H8_HAS(d, H8_F_SHORT))
            {
                static const uint8_t kEven8[8] = {0, 2, 0, 2, 2, 2, 0, 2};
                static const uint8_t kOdd8[8]  = {0, 0, 2, 0, 0, 0, 2, 0};
                if (H8_HAS(d, H8_F_IMM8))
                    return 1;
                col = h8_ea_column(d);
                if (col < 0 || col > 7)
                    return 0;
                return odd_start ? kOdd8[col] : kEven8[col];
            }
            break;
        default:
            break;
    }
    col = h8_ea_column(d);
    if (col < 0)
        return 0;
    return odd_start ? kOdd[col] : kEven[col];
}

SC88_INLINE int h8_bus_is_16bit(int cls) { return cls == H8_W16_S2 || cls == H8_W16_S3; }

/* penalty of `icnt` operand accesses on a bus of the attribute's class */
SC88_INLINE unsigned h8_opnd_pen(int word, uint8_t attr, unsigned icnt)
{
    static const uint8_t kByte[4] = {0, 2, 0, 2};
    static const uint8_t kWord[4] = {0, 1, 2, 4};
    const unsigned cls  = attr & H8_AT_CLASS;
    const unsigned wait = attr >> H8_AT_WAIT_SHIFT;
    unsigned pen = ((word ? kWord[cls] : kByte[cls]) * icnt) >> 1;
    if (wait)
        pen += wait * ((word && h8_bus_is_16bit((int)cls)) ? (icnt + 1) / 2 : icnt);
    return pen;
}

/* ---- the CPU ---- */

#define H8_SR_C  0x0001
#define H8_SR_V  0x0002
#define H8_SR_Z  0x0004
#define H8_SR_N  0x0008
#define H8_CCR_MASK   0x000F
#define H8_MASK_SHIFT 8
#define H8_MASK_BITS  0x0700
#define H8_SR_T       0x8000
#define H8_SR_MASK    0x870F

#define H8_VEC_INVALID_INSN   2
#define H8_VEC_ZERO_DIVIDE    3
#define H8_VEC_TRAPVS         4
#define H8_VEC_ADDRESS_ERROR  8
#define H8_VEC_TRACE          9
#define H8_VEC_NMI            11
#define H8_VEC_TRAPA_BASE     16
#define H8_VEC_IRQ_BASE       32

#define H8_PEND_ADDRERR 0x01u
#define H8_PEND_DEFER   0x02u
#define H8_PEND_TRACE   0x04u
#define H8_PEND_NMI     0x08u
#define H8_PEND_IRQ     0x10u
#define H8_PEND_SLEEP   0x20u
#define H8_PEND_BREAK   0x40u

#define H8_FORCE      (1 << 28)
#define H8_MAX_SLICE  (1 << 24)
#define H8_SLEEP_IDLE_STATES 2
#define H8_CELLS_PER_PAGE 0x10000u

/* what a cell does */
enum
{
    H8_FN_FILL = 0, H8_FN_NOEXEC, H8_FN_INVALID, H8_FN_ALU, H8_FN_RMW, H8_FN_XCH, H8_FN_SWAP,
    H8_FN_EXTS, H8_FN_EXTU, H8_FN_DADD, H8_FN_DSUB, H8_FN_LOGIC_CR, H8_FN_TRAPA, H8_FN_TRAPVS,
    H8_FN_RTE, H8_FN_LINK, H8_FN_UNLK, H8_FN_SLEEP, H8_FN_NOP, H8_FN_LDM, H8_FN_STM, H8_FN_BCC,
    H8_FN_JMP, H8_FN_BSR, H8_FN_RTS, H8_FN_SCB, H8_FN_PJMP, H8_FN_PRTS
};

enum
{
    H8_ALU_ADD, H8_ALU_SUB, H8_ALU_AND, H8_ALU_OR, H8_ALU_XOR, H8_ALU_ADDX, H8_ALU_SUBX, H8_ALU_CMP,
    H8_ALU_MOV, H8_ALU_MOVFPE, H8_ALU_ADDS, H8_ALU_SUBS, H8_ALU_LDC, H8_ALU_MULXU, H8_ALU_DIVXU
};

enum
{
    H8_RMW_ADDQ, H8_RMW_NEG, H8_RMW_NOT, H8_RMW_CLR, H8_RMW_TST, H8_RMW_TAS,
    H8_RMW_SHAL, H8_RMW_SHAR, H8_RMW_SHLL, H8_RMW_SHLR, H8_RMW_ROTL, H8_RMW_ROTR, H8_RMW_ROTXL, H8_RMW_ROTXR,
    H8_RMW_BSETI, H8_RMW_BCLRI, H8_RMW_BNOTI, H8_RMW_BTSTI, H8_RMW_BSETR, H8_RMW_BCLRR, H8_RMW_BNOTR, H8_RMW_BTSTR,
    H8_RMW_CMPIMM, H8_RMW_MOVIMM, H8_RMW_MOVSTORE, H8_RMW_MOVTPE, H8_RMW_STC
};

typedef struct
{
    uint8_t  fn;       /* H8_FN_* */
    uint8_t  k;        /* kind within fn: ALU/RMW kind, condition code, logic op, flag */
    uint8_t  sz;       /* H8_SZ_BYTE / H8_SZ_WORD, as the handler was chosen */
    uint8_t  ea;       /* H8_EA_*, as the handler was chosen */
    uint16_t imm;
    uint8_t  cyc, cyc2, r, x, icnt, icnt2;
} h8_cell_t;

typedef struct
{
    uint16_t r[8];
    uint16_t pc;
    uint16_t sr;
    uint8_t  cp, dp, ep, tp, br;
} h8_regs_t;

typedef struct h8_cpu
{
    /* slice core */
    int32_t  budget;
    uint32_t pending;
    int32_t  slice_len;
    int      in_slice;
    int      cut;
    uint64_t total_states;

    h8_bus_t *bus;
    h8_chip_config_t cfg;
    void   (*irq_ack)(void *ctx, uint8_t vector);
    void    *irq_ack_ctx;
    int    (*trapa_hook)(void *ctx, uint8_t vector);   /* returns 0 to swallow the trap */
    void    *trapa_ctx;

    h8_regs_t regs;
    int      max_mode;
    int      sleeping;
    int      irq_taken;
    uint8_t  irq_level;
    uint8_t  irq_vector;
    uint8_t  last_attr;
    int      mask_changed;
    uint64_t mask_effective_at;
    uint64_t deferred_boundary;

    unsigned   page;                 /* the code page the cells in use belong to */
    h8_cell_t *pages[256];
    uint64_t insn_count, exc_count, fault_count;
} h8_cpu_t;

SC88_INLINE int32_t h8_true_budget(const h8_cpu_t *c)
{
    return c->budget + (c->pending ? H8_FORCE : 0);
}

static uint64_t h8_total_states(const h8_cpu_t *c)
{
    return c->in_slice ? c->total_states + (uint64_t)((int64_t)c->slice_len - (int64_t)h8_true_budget(c))
                       : c->total_states;
}

SC88_INLINE void h8_raise(h8_cpu_t *c, uint32_t bit)
{
    if (!c->pending)
        c->budget -= H8_FORCE;
    c->pending |= bit;
}

SC88_INLINE void h8_clear_pending(h8_cpu_t *c, uint32_t bit)
{
    if (c->pending & bit)
    {
        c->pending &= ~bit;
        if (!c->pending)
            c->budget += H8_FORCE;
    }
}

static void h8_cut_slice(h8_cpu_t *c, uint64_t at)
{
    int64_t left, now, remaining;
    int32_t delta;
    if (c->sleeping || !c->in_slice)
        return;
    left      = h8_true_budget(c);
    now       = (int64_t)c->total_states + ((int64_t)c->slice_len - left);
    remaining = (int64_t)at - now;
    if (remaining < 0)
        remaining = 0;
    if (remaining >= left)
        return;
    delta = (int32_t)(left - remaining);
    c->slice_len -= delta;
    c->budget    -= delta;
    c->cut = 1;
}

SC88_INLINE uint8_t h8_interrupt_mask(const h8_cpu_t *c)
{
    return (uint8_t)((c->regs.sr & H8_MASK_BITS) >> H8_MASK_SHIFT);
}

SC88_INLINE void h8_reeval_irq(h8_cpu_t *c)
{
    if (c->irq_level && c->irq_level > h8_interrupt_mask(c))
        h8_raise(c, H8_PEND_IRQ);
    else
        h8_clear_pending(c, H8_PEND_IRQ);
}

SC88_INLINE void h8_reeval_trace(h8_cpu_t *c)
{
    if (c->regs.sr & H8_SR_T)
        h8_raise(c, H8_PEND_TRACE);
    else
        h8_clear_pending(c, H8_PEND_TRACE);
}

static void h8_set_irq(h8_cpu_t *c, uint8_t level, uint8_t vector)
{
    c->irq_level  = level;
    c->irq_vector = vector;
    h8_reeval_irq(c);
}

SC88_INLINE uint32_t h8_code_addr(const h8_cpu_t *c, uint16_t a)
{
    return c->max_mode ? (((uint32_t)c->regs.cp << 16) | a) : a;
}

SC88_INLINE uint32_t h8_stack_addr(const h8_cpu_t *c, uint16_t a)
{
    return c->max_mode ? (((uint32_t)c->regs.tp << 16) | a) : a;
}

SC88_INLINE uint32_t h8_data_addr(const h8_cpu_t *c, unsigned reg, uint16_t a)
{
    uint8_t page;
    if (!c->max_mode)
        return a;
    page = reg < 4 ? c->regs.dp : reg < 6 ? c->regs.ep : c->regs.tp;
    return ((uint32_t)page << 16) | a;
}

SC88_INLINE uint32_t h8_mem_read8(h8_cpu_t *c, uint32_t a)
{
    uint8_t at;
    a &= c->bus->addr_mask;
    at = H8_ATTR(c->bus, a);
    c->last_attr = at;
    return (at & H8_AT_RDSLOW) ? h8_bus_slow_read8(c->bus, a) : c->bus->mem[a];
}

SC88_INLINE uint32_t h8_mem_read16(h8_cpu_t *c, uint32_t a)
{
    uint8_t at;
    a &= c->bus->addr_mask;
    if (a & 1)
    {
        h8_raise(c, H8_PEND_ADDRERR);
        a &= ~1u;
    }
    at = H8_ATTR(c->bus, a);
    c->last_attr = at;
    return (at & H8_AT_RDSLOW) ? h8_bus_slow_read16(c->bus, a) : H8_BE16(&c->bus->mem[a]);
}

SC88_INLINE void h8_mem_write8(h8_cpu_t *c, uint32_t a, uint32_t v)
{
    uint8_t at;
    a &= c->bus->addr_mask;
    at = H8_ATTR(c->bus, a);
    c->last_attr = at;
    if (at & H8_AT_WRSLOW)
        h8_bus_slow_write8(c->bus, a, (uint8_t)v);
    else
        c->bus->mem[a] = (uint8_t)v;
}

SC88_INLINE void h8_mem_write16(h8_cpu_t *c, uint32_t a, uint32_t v)
{
    uint8_t at;
    a &= c->bus->addr_mask;
    if (a & 1)
    {
        h8_raise(c, H8_PEND_ADDRERR);
        a &= ~1u;
    }
    at = H8_ATTR(c->bus, a);
    c->last_attr = at;
    if (at & H8_AT_WRSLOW)
        h8_bus_slow_write16(c->bus, a, (uint16_t)v);
    else
    {
        c->bus->mem[a]     = (uint8_t)(v >> 8);
        c->bus->mem[a + 1] = (uint8_t)v;
    }
}

SC88_INLINE void h8_push16(h8_cpu_t *c, uint16_t v)
{
    c->regs.r[7] = (uint16_t)(c->regs.r[7] - 2);
    h8_mem_write16(c, h8_stack_addr(c, c->regs.r[7]), v);
}

SC88_INLINE uint16_t h8_pop16(h8_cpu_t *c)
{
    const uint16_t v = (uint16_t)h8_mem_read16(c, h8_stack_addr(c, c->regs.r[7]));
    c->regs.r[7] = (uint16_t)(c->regs.r[7] + 2);
    return v;
}

/* ---- the cell cache ---- */

static void h8_reset_cells(h8_cpu_t *c, unsigned page, uint32_t first, uint32_t count)
{
    h8_cell_t *cells = c->pages[page];
    uint32_t end, i;
    if (!cells)
        return;
    end = first + count < H8_CELLS_PER_PAGE ? first + count : H8_CELLS_PER_PAGE;
    for (i = first; i < end; ++i)
    {
        memset(&cells[i], 0, sizeof(cells[i]));
        cells[i].x = 1;
    }
}

/* Make `page` the code page in use; its cells are allocated on first use. */
static void h8_cells_for(h8_cpu_t *c, uint8_t cp)
{
    const unsigned page = c->max_mode ? cp : 0;
    if (!c->pages[page])
    {
        c->pages[page] = (h8_cell_t*)malloc(H8_CELLS_PER_PAGE * sizeof(h8_cell_t));
        if (c->pages[page])
            h8_reset_cells(c, page, 0, H8_CELLS_PER_PAGE);
    }
    c->page = page;
}

static void h8_invalidate_all(h8_cpu_t *c)
{
    unsigned p;
    for (p = 0; p < 256; ++p)
        h8_reset_cells(c, p, 0, H8_CELLS_PER_PAGE);
}

static void h8_invalidate_range(h8_cpu_t *c, uint32_t addr, uint32_t len)
{
    const unsigned page  = c->max_mode ? ((addr >> 16) & 0xFF) : 0;
    const uint32_t first = addr & 0xFFFF;
    const uint32_t back  = first < 5 ? first : 5;
    h8_reset_cells(c, page, first - back, len + back);
    if (back < 5)
        h8_reset_cells(c, page, H8_CELLS_PER_PAGE - (5 - back), 5 - back);
}

static void h8_cpu_code_line_written(h8_cpu_t *c, uint32_t addr)
{
    h8_invalidate_range(c, addr & ~(H8_LINE_SIZE - 1), H8_LINE_SIZE);
    h8_bus_unmark_code(c->bus, addr);
}

/* ---- control registers and exceptions ---- */

static uint32_t h8_read_cr(const h8_cpu_t *c, uint8_t cr, int word)
{
    if (word)
    {
        switch (cr)
        {
            case 0: case 1: return c->regs.sr;
            case 3: return (uint32_t)c->regs.br * 0x0101u;
            case 4: return ((uint32_t)c->regs.ep << 8) | c->regs.dp;
            case 5: return (uint32_t)c->regs.dp * 0x0101u;
            case 7: return (uint32_t)c->regs.tp * 0x0101u;
            default: return 0;
        }
    }
    switch (cr)
    {
        case 0: case 1: return c->regs.sr & 0xFF;
        case 3: return c->regs.br;
        case 4: return c->regs.ep;
        case 5: return c->regs.dp;
        case 7: return c->regs.tp;
        default: return 0;
    }
}

static void h8_set_sr(h8_cpu_t *c, uint16_t v)
{
    const uint16_t old = c->regs.sr;
    c->regs.sr = (uint16_t)(v & H8_SR_MASK);
    if ((old ^ c->regs.sr) & H8_MASK_BITS)
        c->mask_changed = 1;
    h8_reeval_irq(c);
    h8_reeval_trace(c);
}

static void h8_write_cr(h8_cpu_t *c, uint8_t cr, uint32_t v, int word)
{
    if (word)
    {
        switch (cr)
        {
            case 0: case 1: h8_set_sr(c, (uint16_t)v); break;
            case 3: c->regs.br = (uint8_t)v; break;
            case 4: c->regs.ep = (uint8_t)(v >> 8); c->regs.dp = (uint8_t)v; break;
            case 5: c->regs.dp = (uint8_t)v; break;
            case 7: c->regs.tp = (uint8_t)v; break;
            default: break;
        }
        return;
    }
    switch (cr)
    {
        case 0: case 1: c->regs.sr = (uint16_t)((c->regs.sr & 0xFF00) | (v & H8_CCR_MASK)); break;
        case 3: c->regs.br = (uint8_t)v; break;
        case 4: c->regs.ep = (uint8_t)v; break;
        case 5: c->regs.dp = (uint8_t)v; break;
        case 7: c->regs.tp = (uint8_t)v; break;
        default: break;
    }
}

static void h8_enter_exception(h8_cpu_t *c, uint8_t vector, uint16_t push_pc, int new_mask)
{
    ++c->exc_count;
    if (new_mask < 0)
        ++c->fault_count;
    h8_push16(c, push_pc);
    if (c->max_mode)
        h8_push16(c, c->regs.cp);
    h8_push16(c, c->regs.sr);
    c->regs.sr = (uint16_t)(c->regs.sr & ~H8_SR_T);
    if (new_mask >= 0)
        c->regs.sr = (uint16_t)((c->regs.sr & ~H8_MASK_BITS) | ((uint16_t)new_mask << H8_MASK_SHIFT));
    if (c->max_mode)
    {
        const uint32_t va = (uint32_t)vector * 4;
        c->regs.cp = h8_bus_read8(c->bus, va + 1);
        c->regs.pc = h8_bus_read16(c->bus, va + 2);
    }
    else
        c->regs.pc = h8_bus_read16(c->bus, (uint32_t)vector * 2);
    c->sleeping = 0;
    h8_reeval_irq(c);
    h8_reeval_trace(c);
}

SC88_INLINE unsigned h8_exception_states(const h8_cpu_t *c) { return c->max_mode ? 21 : 16; }
SC88_INLINE unsigned h8_irq_states(const h8_cpu_t *c)       { return c->max_mode ? 23 : 18; }

static void h8_service_pending(h8_cpu_t *c)
{
    h8_clear_pending(c, H8_PEND_SLEEP);
    h8_clear_pending(c, H8_PEND_BREAK);
    if (c->pending & H8_PEND_ADDRERR)
    {
        h8_clear_pending(c, H8_PEND_ADDRERR);
        h8_enter_exception(c, H8_VEC_ADDRESS_ERROR, c->regs.pc, -1);
        c->budget -= (int32_t)h8_exception_states(c);
        return;
    }
    if (c->pending & H8_PEND_DEFER)
    {
        h8_clear_pending(c, H8_PEND_DEFER);
        if (c->mask_changed)
            c->mask_effective_at = h8_total_states(c) + 3;
        c->mask_changed      = 0;
        c->deferred_boundary = h8_total_states(c);
        return;
    }
    if (h8_total_states(c) == c->deferred_boundary)
        return;
    if (h8_total_states(c) < c->mask_effective_at)
        return;
    if (c->pending & H8_PEND_TRACE)
    {
        h8_enter_exception(c, H8_VEC_TRACE, c->regs.pc, -1);
        c->budget -= (int32_t)h8_exception_states(c);
        return;
    }
    if (c->pending & H8_PEND_NMI)
    {
        h8_clear_pending(c, H8_PEND_NMI);
        c->irq_taken = 1;
        h8_enter_exception(c, H8_VEC_NMI, c->regs.pc, 7);
        c->budget -= (int32_t)h8_irq_states(c);
        return;
    }
    if (c->pending & H8_PEND_IRQ)
    {
        const uint8_t vector = c->irq_vector;
        c->irq_taken = 1;
        h8_enter_exception(c, vector, c->regs.pc, c->irq_level);
        c->budget -= (int32_t)h8_irq_states(c);
        if (c->irq_ack)
            c->irq_ack(c->irq_ack_ctx, vector);
        return;
    }
}

/* ---- filling a cell (exec.cpp: fill, build, select) ---- */

static unsigned h8_popcount8(unsigned v)
{
    unsigned n = 0;
    for (v &= 0xFF; v; v &= v - 1)
        ++n;
    return n;
}

static void h8_static_states(const h8_cpu_t *c, const h8_insn_t *d, uint16_t pc, uint8_t fetch_attr,
                             int cond, unsigned n_regs, uint8_t *states, uint8_t *I)
{
    const h8_base_timing_t b = h8_base_timing(d, c->max_mode, cond, n_regs);
    const int      fc   = fetch_attr & H8_AT_CLASS;
    const unsigned wait = fetch_attr >> H8_AT_WAIT_SHIFT;
    unsigned st = b.states;
    if (h8_bus_is_16bit(fc))
        st += h8_parity_adjustment(d, (pc & 1) != 0, cond);
    switch (fc)   /* fetch penalty */
    {
        case H8_W16_S2: break;
        case H8_W16_S3: st += b.JK / 2; break;
        case H8_W8_S2:  st += b.JK; break;
        default:        st += 2u * b.JK; break;
    }
    if (wait)
        st += wait * (h8_bus_is_16bit(fc) ? (b.JK + 1u) / 2 : b.JK);
    *states = (uint8_t)st;
    *I      = b.I;
}

#define H8_PICK_ALU(kind, size, mode) \
    do { if ((mode) >= H8_EA_REG && (mode) <= H8_EA_IMM) { cell->fn = H8_FN_ALU; cell->k = (kind); \
         cell->sz = (size); cell->ea = (mode); } else cell->fn = H8_FN_INVALID; } while (0)
#define H8_PICK_RMW(kind, size, mode) \
    do { if ((mode) >= H8_EA_REG && (mode) <= H8_EA_ABS16) { cell->fn = H8_FN_RMW; cell->k = (kind); \
         cell->sz = (size); cell->ea = (mode); } else cell->fn = H8_FN_INVALID; } while (0)

static void h8_select(const h8_insn_t *d, h8_cell_t *cell)
{
    const uint8_t sz = d->size == H8_SZ_WORD ? H8_SZ_WORD : H8_SZ_BYTE;
    const int bit_in_reg = H8_HAS(d, H8_F_BITINREG);
    switch (d->op)
    {
        case H8_OP_MOV:
            if (H8_HAS(d, H8_F_IMMSRC))     H8_PICK_RMW(H8_RMW_MOVIMM, sz, d->ea);
            else if (H8_HAS(d, H8_F_STORE)) H8_PICK_RMW(H8_RMW_MOVSTORE, sz, d->ea);
            else                            H8_PICK_ALU(H8_ALU_MOV, sz, d->ea);
            break;
        case H8_OP_MOVFPE: H8_PICK_ALU(H8_ALU_MOVFPE, H8_SZ_BYTE, d->ea); break;
        case H8_OP_MOVTPE: H8_PICK_RMW(H8_RMW_MOVTPE, H8_SZ_BYTE, d->ea); break;
        case H8_OP_LDM:  cell->fn = H8_FN_LDM; break;
        case H8_OP_STM:  cell->fn = H8_FN_STM; break;
        case H8_OP_XCH:  cell->fn = H8_FN_XCH; break;
        case H8_OP_SWAP: cell->fn = H8_FN_SWAP; break;
        case H8_OP_ADD:  H8_PICK_ALU(H8_ALU_ADD, sz, d->ea); break;
        case H8_OP_ADDQ: H8_PICK_RMW(H8_RMW_ADDQ, sz, d->ea); break;
        case H8_OP_ADDS: H8_PICK_ALU(H8_ALU_ADDS, sz, d->ea); break;
        case H8_OP_ADDX: H8_PICK_ALU(H8_ALU_ADDX, sz, d->ea); break;
        case H8_OP_DADD: cell->fn = H8_FN_DADD; break;
        case H8_OP_SUB:  H8_PICK_ALU(H8_ALU_SUB, sz, d->ea); break;
        case H8_OP_SUBS: H8_PICK_ALU(H8_ALU_SUBS, sz, d->ea); break;
        case H8_OP_SUBX: H8_PICK_ALU(H8_ALU_SUBX, sz, d->ea); break;
        case H8_OP_DSUB: cell->fn = H8_FN_DSUB; break;
        case H8_OP_MULXU: H8_PICK_ALU(H8_ALU_MULXU, sz, d->ea); break;
        case H8_OP_DIVXU: H8_PICK_ALU(H8_ALU_DIVXU, sz, d->ea); break;
        case H8_OP_CMP:
            if (H8_HAS(d, H8_F_IMMSRC)) H8_PICK_RMW(H8_RMW_CMPIMM, sz, d->ea);
            else                        H8_PICK_ALU(H8_ALU_CMP, sz, d->ea);
            break;
        case H8_OP_EXTS: cell->fn = H8_FN_EXTS; break;
        case H8_OP_EXTU: cell->fn = H8_FN_EXTU; break;
        case H8_OP_TST:  H8_PICK_RMW(H8_RMW_TST, sz, d->ea); break;
        case H8_OP_NEG:  H8_PICK_RMW(H8_RMW_NEG, sz, d->ea); break;
        case H8_OP_CLR:  H8_PICK_RMW(H8_RMW_CLR, sz, d->ea); break;
        case H8_OP_TAS:  H8_PICK_RMW(H8_RMW_TAS, H8_SZ_BYTE, d->ea); break;
        case H8_OP_SHAL: H8_PICK_RMW(H8_RMW_SHAL, sz, d->ea); break;
        case H8_OP_SHAR: H8_PICK_RMW(H8_RMW_SHAR, sz, d->ea); break;
        case H8_OP_SHLL: H8_PICK_RMW(H8_RMW_SHLL, sz, d->ea); break;
        case H8_OP_SHLR: H8_PICK_RMW(H8_RMW_SHLR, sz, d->ea); break;
        case H8_OP_ROTL: H8_PICK_RMW(H8_RMW_ROTL, sz, d->ea); break;
        case H8_OP_ROTR: H8_PICK_RMW(H8_RMW_ROTR, sz, d->ea); break;
        case H8_OP_ROTXL: H8_PICK_RMW(H8_RMW_ROTXL, sz, d->ea); break;
        case H8_OP_ROTXR: H8_PICK_RMW(H8_RMW_ROTXR, sz, d->ea); break;
        case H8_OP_AND:  H8_PICK_ALU(H8_ALU_AND, sz, d->ea); break;
        case H8_OP_OR:   H8_PICK_ALU(H8_ALU_OR, sz, d->ea); break;
        case H8_OP_XOR:  H8_PICK_ALU(H8_ALU_XOR, sz, d->ea); break;
        case H8_OP_NOT:  H8_PICK_RMW(H8_RMW_NOT, sz, d->ea); break;
        case H8_OP_BSET: H8_PICK_RMW(bit_in_reg ? H8_RMW_BSETR : H8_RMW_BSETI, sz, d->ea); break;
        case H8_OP_BCLR: H8_PICK_RMW(bit_in_reg ? H8_RMW_BCLRR : H8_RMW_BCLRI, sz, d->ea); break;
        case H8_OP_BNOT: H8_PICK_RMW(bit_in_reg ? H8_RMW_BNOTR : H8_RMW_BNOTI, sz, d->ea); break;
        case H8_OP_BTST: H8_PICK_RMW(bit_in_reg ? H8_RMW_BTSTR : H8_RMW_BTSTI, sz, d->ea); break;
        case H8_OP_LDC:  H8_PICK_ALU(H8_ALU_LDC, sz, d->ea); break;
        case H8_OP_STC:  H8_PICK_RMW(H8_RMW_STC, sz, d->ea); break;
        case H8_OP_ANDC: cell->fn = H8_FN_LOGIC_CR; cell->sz = sz; cell->k = 0; break;
        case H8_OP_ORC:  cell->fn = H8_FN_LOGIC_CR; cell->sz = sz; cell->k = 1; break;
        case H8_OP_XORC: cell->fn = H8_FN_LOGIC_CR; cell->sz = sz; cell->k = 2; break;
        case H8_OP_TRAPA:  cell->fn = H8_FN_TRAPA; break;
        case H8_OP_TRAPVS: cell->fn = H8_FN_TRAPVS; break;
        case H8_OP_RTE:   cell->fn = H8_FN_RTE; break;
        case H8_OP_LINK:  cell->fn = H8_FN_LINK; break;
        case H8_OP_UNLK:  cell->fn = H8_FN_UNLK; break;
        case H8_OP_SLEEP: cell->fn = H8_FN_SLEEP; break;
        case H8_OP_NOP:   cell->fn = H8_FN_NOP; break;
        case H8_OP_BCC:   cell->fn = H8_FN_BCC; cell->k = (uint8_t)(d->reg & 0x0F); break;
        case H8_OP_JMP:
        case H8_OP_JSR:
            cell->fn = H8_FN_JMP;
            cell->k  = d->op == H8_OP_JSR;
            cell->ea = (d->ea == H8_EA_ABS16 || d->ea == H8_EA_REGIND || d->ea == H8_EA_DISP8)
                     ? d->ea : (uint8_t)H8_EA_DISP16;
            break;
        case H8_OP_BSR: cell->fn = H8_FN_BSR; break;
        case H8_OP_RTS: cell->fn = H8_FN_RTS; cell->k = 0; break;
        case H8_OP_RTD: cell->fn = H8_FN_RTS; cell->k = 1; break;
        case H8_OP_SCB: cell->fn = H8_FN_SCB; break;
        case H8_OP_PJMP:
        case H8_OP_PJSR:
            cell->fn = H8_FN_PJMP;
            cell->k  = d->op == H8_OP_PJSR;
            cell->ea = d->ea == H8_EA_ABS24 ? (uint8_t)H8_EA_ABS24 : (uint8_t)H8_EA_REGIND;
            break;
        case H8_OP_PRTS: cell->fn = H8_FN_PRTS; cell->k = 0; break;
        case H8_OP_PRTD: cell->fn = H8_FN_PRTS; cell->k = 1; break;
        default:
            cell->fn = H8_FN_INVALID;
            break;
    }
}

static void h8_build(const h8_cpu_t *c, const h8_insn_t *d, uint16_t pc, uint8_t fetch_attr, h8_cell_t *cell)
{
    unsigned n_regs;
    int primary = H8_COND_NORMAL, alternate = H8_COND_NORMAL, two = 0;

    h8_select(d, cell);
    cell->r = (uint8_t)((d->ea_reg << 4) | (d->reg & 0x0F));
    cell->x = (uint8_t)((d->length & 7) | ((d->aux & 0x0F) << 4));
    switch (d->op)
    {
        case H8_OP_LDM: case H8_OP_STM:
            cell->imm = d->reg;
            break;
        case H8_OP_PJMP: case H8_OP_PJSR:
            if (d->ea == H8_EA_ABS24)
            {
                cell->r   = (uint8_t)((uint32_t)d->ea_ext >> 16);
                cell->imm = (uint16_t)d->ea_ext;
            }
            break;
        default:
            if (d->ea == H8_EA_IMM || d->ea == H8_EA_NONE)
                cell->imm = (uint16_t)d->imm;
            else
                cell->imm = (uint16_t)d->ea_ext;
            break;
    }
    n_regs = (d->op == H8_OP_LDM || d->op == H8_OP_STM) ? h8_popcount8(d->reg) : 0;
    switch (d->op)
    {
        case H8_OP_BCC: case H8_OP_SCB: case H8_OP_TRAPVS:
            primary = H8_COND_NOT_TAKEN; alternate = H8_COND_TAKEN; two = 1;
            break;
        case H8_OP_DIVXU:
            alternate = H8_COND_DIV_ZERO; two = 1;
            break;
        default:
            break;
    }
    h8_static_states(c, d, pc, fetch_attr, primary, n_regs, &cell->cyc, &cell->icnt);
    if (two)
        h8_static_states(c, d, pc, fetch_attr, alternate, n_regs, &cell->cyc2, &cell->icnt2);
    if (H8_HAS(d, H8_F_IMMSRC))
    {
        const uint16_t v = (uint16_t)d->imm;
        cell->cyc2  = (uint8_t)(v >> 8);
        cell->icnt2 = (uint8_t)v;
    }
}

static void h8_fill(h8_cpu_t *c, h8_cell_t *ip, uint16_t pc)
{
    const uint32_t fa = h8_code_addr(c, pc) & c->bus->addr_mask;
    const uint8_t  at = H8_ATTR(c->bus, fa);
    h8_cell_t cell;

    memset(&cell, 0, sizeof(cell));
    cell.x    = 1;
    cell.icnt = (uint8_t)(c->max_mode ? 10 : 6);
    if (at & H8_AT_NOEXEC)
        cell.fn = H8_FN_NOEXEC;
    else
    {
        h8_fetch_t f;
        h8_insn_t  d;
        f.bus      = c->bus;
        f.page     = fa & 0xFF0000u;
        f.off      = (uint16_t)fa;
        f.max_mode = c->max_mode;
        h8_decode(&f, &d);
        if (d.op == H8_OP_INVALID || (H8_HAS(&d, H8_F_MAXMODEONLY) && !c->max_mode))
            cell.fn = H8_FN_INVALID;
        else
        {
            h8_build(c, &d, pc, at, &cell);
            h8_bus_mark_code(c->bus, fa);
            h8_bus_mark_code(c->bus, h8_code_addr(c, (uint16_t)(pc + d.length - 1)));
        }
    }
    *ip = cell;
}

/* ---- running a cell (exec.cpp: the handlers) ----
 * A write can land on the instruction being executed, which blanks its
 * cell; the C++ handlers go on reading the cell afterwards.  So the cell
 * is read through `ip` at the same points here, never from a copy. */

#define H8_W(sz)      ((sz) == H8_SZ_WORD)
#define H8_MASK(w)    ((w) ? 0xFFFFu : 0xFFu)
#define H8_MSB(w)     ((w) ? 0x8000u : 0x80u)
#define H8_GET_REG(w, n)     ((w) ? (uint32_t)R->r[n] : (uint32_t)(R->r[n] & 0xFF))
#define H8_SET_REG(w, n, v)  do { if (w) R->r[n] = (uint16_t)(v); \
                                  else R->r[n] = (uint16_t)((R->r[n] & 0xFF00) | ((v) & 0xFF)); } while (0)
#define H8_MRD(w, a)         ((w) ? h8_mem_read16(c, (a)) : h8_mem_read8(c, (a)))
#define H8_MWR(w, a, v)      do { if (w) h8_mem_write16(c, (a), (v)); else h8_mem_write8(c, (a), (v)); } while (0)
#define H8_LEN(ip)           ((ip)->x & 7)
#define H8_PC_NEXT(ip)       ((uint16_t)(pc + H8_LEN(ip)))
#define H8_STACK_PEN(n)      h8_opnd_pen(1, c->last_attr, (n))
#define H8_EA_PEN(m, w, ip)  (H8_EA_IS_MEMORY(m) ? h8_opnd_pen((w), c->last_attr, (ip)->icnt) : 0u)

static void h8_set_nz(h8_cpu_t *c, int w, uint32_t v)
{
    uint16_t f = (uint16_t)(c->regs.sr & ~(H8_SR_N | H8_SR_Z));
    v &= H8_MASK(w);
    if (v & H8_MSB(w))
        f |= H8_SR_N;
    if (v == 0)
        f |= H8_SR_Z;
    c->regs.sr = f;
}

static void h8_flags_add(h8_cpu_t *c, int w, uint32_t a, uint32_t b, uint32_t r, int extend)
{
    const uint32_t m = H8_MASK(w), msb = H8_MSB(w);
    uint16_t f = (uint16_t)(c->regs.sr & ~(H8_SR_N | H8_SR_Z | H8_SR_V | H8_SR_C));
    if (r & msb)
        f |= H8_SR_N;
    if ((r & m) == 0 && (!extend || (c->regs.sr & H8_SR_Z)))
        f |= H8_SR_Z;
    if (~(a ^ b) & (a ^ r) & msb)
        f |= H8_SR_V;
    if (r & (msb << 1))
        f |= H8_SR_C;
    c->regs.sr = f;
}

static void h8_flags_sub(h8_cpu_t *c, int w, uint32_t a, uint32_t b, uint32_t r, int extend)
{
    const uint32_t m = H8_MASK(w), msb = H8_MSB(w);
    uint16_t f = (uint16_t)(c->regs.sr & ~(H8_SR_N | H8_SR_Z | H8_SR_V | H8_SR_C));
    if (r & msb)
        f |= H8_SR_N;
    if ((r & m) == 0 && (!extend || (c->regs.sr & H8_SR_Z)))
        f |= H8_SR_Z;
    if ((a ^ b) & (a ^ r) & msb)
        f |= H8_SR_V;
    if (r & (msb << 1))
        f |= H8_SR_C;
    c->regs.sr = f;
}

static int h8_cond_true(unsigned cc, uint16_t sr)
{
    const int cf = (sr & H8_SR_C) != 0, v = (sr & H8_SR_V) != 0;
    const int z = (sr & H8_SR_Z) != 0, n = (sr & H8_SR_N) != 0;
    switch (cc)
    {
        case 0:  return 1;
        case 1:  return 0;
        case 2:  return !(cf || z);
        case 3:  return cf || z;
        case 4:  return !cf;
        case 5:  return cf;
        case 6:  return !z;
        case 7:  return z;
        case 8:  return !v;
        case 9:  return v;
        case 10: return !n;
        case 11: return n;
        case 12: return n == v;
        case 13: return n != v;
        case 14: return !z && (n == v);
        default: return z || (n != v);
    }
}

static uint32_t h8_ea(h8_cpu_t *c, const h8_cell_t *ip, int m, int w)
{
    h8_regs_t *R = &c->regs;
    const unsigned n = ip->r >> 4;
    switch (m)
    {
        case H8_EA_REGIND:
            return h8_data_addr(c, n, R->r[n]);
        case H8_EA_DISP8:
        case H8_EA_DISP16:
            return h8_data_addr(c, n, (uint16_t)(R->r[n] + (int16_t)ip->imm));
        case H8_EA_PREDEC:
            if (!w)
            {
                if (n == 7)
                {
                    R->r[7] = (uint16_t)(R->r[7] - 2);
                    return h8_data_addr(c, 7, (uint16_t)(R->r[7] + 1));
                }
                R->r[n] = (uint16_t)(R->r[n] - 1);
                return h8_data_addr(c, n, R->r[n]);
            }
            R->r[n] = (uint16_t)(R->r[n] - 2);
            return h8_data_addr(c, n, R->r[n]);
        case H8_EA_POSTINC:
        {
            uint32_t a;
            if (!w)
            {
                if (n == 7)
                {
                    a = h8_data_addr(c, 7, (uint16_t)(R->r[7] + 1));
                    R->r[7] = (uint16_t)(R->r[7] + 2);
                    return a;
                }
                a = h8_data_addr(c, n, R->r[n]);
                R->r[n] = (uint16_t)(R->r[n] + 1);
                return a;
            }
            a = h8_data_addr(c, n, R->r[n]);
            R->r[n] = (uint16_t)(R->r[n] + 2);
            return a;
        }
        case H8_EA_ABS8:
            return ((uint32_t)R->br << 8) | (uint8_t)ip->imm;
        default:   /* Abs16 */
            return c->max_mode ? (((uint32_t)R->dp << 16) | ip->imm) : ip->imm;
    }
}

static uint8_t h8_bcd_add(uint8_t a, uint8_t b, int cin, int *carry_out)
{
    unsigned lo = (a & 0x0Fu) + (b & 0x0Fu) + (cin ? 1u : 0u);
    unsigned hi = (unsigned)(a >> 4) + (unsigned)(b >> 4);
    if (lo > 9) { lo -= 10; hi += 1; }
    *carry_out = 0;
    if (hi > 9) { hi -= 10; *carry_out = 1; }
    return (uint8_t)((hi << 4) | lo);
}

static uint8_t h8_bcd_sub(uint8_t a, uint8_t b, int cin, int *borrow_out)
{
    int lo = (int)(a & 0x0F) - (int)(b & 0x0F) - (cin ? 1 : 0);
    int hi = (int)(a >> 4) - (int)(b >> 4);
    if (lo < 0) { lo += 10; hi -= 1; }
    *borrow_out = 0;
    if (hi < 0) { hi += 10; *borrow_out = 1; }
    return (uint8_t)((hi << 4) | lo);
}

/* End of an instruction: charge its states and set where the next one is. */
#define H8_END_AT(target, states)  do { c->regs.pc = (uint16_t)(target); c->budget -= (int32_t)(states); return; } while (0)
#define H8_END_SEQ(states)         do { c->regs.pc = (uint16_t)(pc + H8_LEN(ip)); c->budget -= (int32_t)(states); return; } while (0)
/* ...after the handler set cp:pc itself (exception, far jump, return) */
#define H8_END_FAR(states)         do { h8_cells_for(c, c->regs.cp); c->budget -= (int32_t)(states); return; } while (0)

static void h8_exec_alu(h8_cpu_t *c, const h8_cell_t *ip, uint16_t pc)
{
    h8_regs_t *R = &c->regs;
    const int      K = ip->k, M = ip->ea;
    const int      w = H8_W(ip->sz);
    const uint32_t m = H8_MASK(w);
    const unsigned rd = ip->r & 7;
    uint32_t s;
    unsigned states;
    uint16_t next;
    int      far = 0;

    if (M == H8_EA_REG)
        s = H8_GET_REG(w, ip->r >> 4);
    else if (M == H8_EA_IMM)
        s = (uint32_t)ip->imm & m;
    else
    {
        const uint32_t a = h8_ea(c, ip, M, w);
        s = H8_MRD(w, a);
    }
    ++c->insn_count;
    next   = H8_PC_NEXT(ip);
    states = ip->cyc + H8_EA_PEN(M, w, ip);

    switch (K)
    {
        case H8_ALU_ADD:
        {
            const uint32_t a = H8_GET_REG(w, rd), r = a + s;
            h8_flags_add(c, w, a, s, r, 0);
            H8_SET_REG(w, rd, r);
            break;
        }
        case H8_ALU_SUB:
        {
            const uint32_t a = H8_GET_REG(w, rd), r = a - s;
            h8_flags_sub(c, w, a, s, r, 0);
            H8_SET_REG(w, rd, r);
            break;
        }
        case H8_ALU_ADDX:
        {
            const uint32_t a = H8_GET_REG(w, rd), r = a + s + ((R->sr & H8_SR_C) ? 1u : 0u);
            h8_flags_add(c, w, a, s, r, 1);
            H8_SET_REG(w, rd, r);
            break;
        }
        case H8_ALU_SUBX:
        {
            const uint32_t a = H8_GET_REG(w, rd), r = a - s - ((R->sr & H8_SR_C) ? 1u : 0u);
            h8_flags_sub(c, w, a, s, r, 0);
            H8_SET_REG(w, rd, r);
            break;
        }
        case H8_ALU_CMP:
        {
            const uint32_t a = H8_GET_REG(w, rd);
            h8_flags_sub(c, w, a, s, a - s, 0);
            break;
        }
        case H8_ALU_AND: case H8_ALU_OR: case H8_ALU_XOR:
        {
            const uint32_t a = H8_GET_REG(w, rd);
            const uint32_t r = (K == H8_ALU_AND) ? (a & s) : (K == H8_ALU_OR) ? (a | s) : (a ^ s);
            H8_SET_REG(w, rd, r);
            h8_set_nz(c, w, r);
            R->sr &= (uint16_t)~H8_SR_V;
            break;
        }
        case H8_ALU_MOV:
            H8_SET_REG(w, rd, s);
            h8_set_nz(c, w, s);
            R->sr &= (uint16_t)~H8_SR_V;
            break;
        case H8_ALU_MOVFPE:
            H8_SET_REG(0, rd, s);
            break;
        case H8_ALU_ADDS: case H8_ALU_SUBS:
        {
            uint32_t v = s;
            if (!w)
                v = (uint32_t)(int16_t)(int8_t)s & 0xFFFF;
            R->r[rd] = (uint16_t)(K == H8_ALU_ADDS ? R->r[rd] + v : R->r[rd] - v);
            break;
        }
        case H8_ALU_LDC:
        {
            int done = 0;
            if (!w && (M == H8_EA_POSTINC || M == H8_EA_PREDEC) && (ip->r >> 4) == 7 && rd == 4)
            {
                const uint16_t sp_word = (M == H8_EA_POSTINC) ? (uint16_t)(R->r[7] - 2) : R->r[7];
                const uint32_t wd = h8_mem_read16(c, h8_stack_addr(c, sp_word));
                R->ep = (uint8_t)(wd >> 8);
                R->dp = (uint8_t)wd;
                done = 1;
            }
            if (!done)
                h8_write_cr(c, (uint8_t)rd, s, w);
            h8_raise(c, H8_PEND_DEFER);
            break;
        }
        case H8_ALU_MULXU:
            if (!w)
            {
                const uint32_t p = (uint32_t)(R->r[rd] & 0xFF) * s;
                R->r[rd] = (uint16_t)p;
                h8_set_nz(c, 1, p);
            }
            else
            {
                const uint32_t p = (uint32_t)R->r[rd] * s;
                uint16_t f;
                R->r[rd] = (uint16_t)(p >> 16);
                R->r[(rd + 1) & 7] = (uint16_t)p;
                f = (uint16_t)(R->sr & ~(H8_SR_N | H8_SR_Z));
                if (p & 0x80000000u)
                    f |= H8_SR_N;
                if (p == 0)
                    f |= H8_SR_Z;
                R->sr = f;
            }
            R->sr &= (uint16_t)~(H8_SR_V | H8_SR_C);
            break;
        case H8_ALU_DIVXU:
            if (s == 0)
            {
                R->sr = (uint16_t)((R->sr & ~(H8_SR_N | H8_SR_V | H8_SR_C)) | H8_SR_Z);
                h8_enter_exception(c, H8_VEC_ZERO_DIVIDE, H8_PC_NEXT(ip), -1);
                states = ip->cyc2 + H8_STACK_PEN(ip->icnt2);
                far = 1;
            }
            else if (!w)
            {
                const uint32_t dividend = R->r[rd];
                const uint32_t q = dividend / s, rem = dividend % s;
                if (q > 0xFF)
                {
                    R->sr = (uint16_t)((R->sr & ~(H8_SR_N | H8_SR_Z | H8_SR_C)) | H8_SR_V);
                    states -= 12;
                }
                else
                {
                    R->r[rd] = (uint16_t)((rem << 8) | q);
                    h8_set_nz(c, 0, q);
                    R->sr &= (uint16_t)~(H8_SR_V | H8_SR_C);
                }
            }
            else
            {
                const uint32_t dividend = ((uint32_t)R->r[rd] << 16) | R->r[(rd + 1) & 7];
                const uint32_t q = dividend / s, rem = dividend % s;
                if (q > 0xFFFF)
                {
                    R->sr = (uint16_t)((R->sr & ~(H8_SR_N | H8_SR_Z | H8_SR_C)) | H8_SR_V);
                    states -= 18;
                }
                else
                {
                    R->r[rd] = (uint16_t)rem;
                    R->r[(rd + 1) & 7] = (uint16_t)q;
                    h8_set_nz(c, 1, q);
                    R->sr &= (uint16_t)~(H8_SR_V | H8_SR_C);
                }
            }
            break;
        default:
            break;
    }
    if (far)
        H8_END_FAR(states);
    H8_END_AT(next, states);
}

static void h8_exec_rmw(h8_cpu_t *c, const h8_cell_t *ip, uint16_t pc)
{
    h8_regs_t *R = &c->regs;
    const int      K = ip->k, M = ip->ea;
    const int      w = H8_W(ip->sz);
    const uint32_t m = H8_MASK(w), msb = H8_MSB(w);
    const int      is_reg = M == H8_EA_REG;
    /* the operand: a register number or a memory address */
    const uint32_t oa = is_reg ? (uint32_t)(ip->r >> 4) : h8_ea(c, ip, M, w);
    unsigned states;

#define O_READ()    (is_reg ? H8_GET_REG(w, oa) : H8_MRD(w, oa))
#define O_WRITE(v)  do { if (is_reg) H8_SET_REG(w, oa, (v)); else H8_MWR(w, oa, (v)); } while (0)

    ++c->insn_count;
    switch (K)
    {
        case H8_RMW_ADDQ:
        {
            const uint32_t v = O_READ();
            const uint32_t s = (uint32_t)(int16_t)(uint16_t)(((uint16_t)ip->cyc2 << 8) | ip->icnt2) & m;
            const uint32_t r = v + s;
            h8_flags_add(c, w, v, s, r, 0);
            O_WRITE(r);
            break;
        }
        case H8_RMW_NEG:
        {
            const uint32_t v = O_READ(), r = 0u - v;
            h8_flags_sub(c, w, 0, v, r, 0);
            O_WRITE(r);
            break;
        }
        case H8_RMW_NOT:
        {
            const uint32_t r = ~O_READ() & m;
            O_WRITE(r);
            h8_set_nz(c, w, r);
            R->sr &= (uint16_t)~H8_SR_V;
            break;
        }
        case H8_RMW_CLR:
            O_WRITE(0);
            R->sr = (uint16_t)((R->sr & ~(H8_SR_N | H8_SR_V | H8_SR_C)) | H8_SR_Z);
            break;
        case H8_RMW_TST:
        {
            const uint32_t v = O_READ();
            h8_set_nz(c, w, v);
            R->sr &= (uint16_t)~(H8_SR_V | H8_SR_C);
            break;
        }
        case H8_RMW_TAS:
        {
            const uint32_t v = O_READ();
            h8_set_nz(c, w, v);
            R->sr &= (uint16_t)~(H8_SR_V | H8_SR_C);
            O_WRITE(v | 0x80);
            break;
        }
        case H8_RMW_SHAL: case H8_RMW_SHAR: case H8_RMW_SHLL: case H8_RMW_SHLR:
        case H8_RMW_ROTL: case H8_RMW_ROTR: case H8_RMW_ROTXL: case H8_RMW_ROTXR:
        {
            const uint32_t v = O_READ();
            const int oldc = (R->sr & H8_SR_C) != 0;
            uint32_t r = 0;
            int cf = 0, vf = 0;
            uint16_t f;
            switch (K)
            {
                case H8_RMW_SHAL:  cf = (v & msb) != 0; r = (v << 1) & m; vf = ((v ^ r) & msb) != 0; break;
                case H8_RMW_SHAR:  cf = (v & 1) != 0; r = (v >> 1) | (v & msb); break;
                case H8_RMW_SHLL:  cf = (v & msb) != 0; r = (v << 1) & m; break;
                case H8_RMW_SHLR:  cf = (v & 1) != 0; r = v >> 1; break;
                case H8_RMW_ROTL:  cf = (v & msb) != 0; r = ((v << 1) | (cf ? 1u : 0u)) & m; break;
                case H8_RMW_ROTR:  cf = (v & 1) != 0; r = (v >> 1) | (cf ? msb : 0u); break;
                case H8_RMW_ROTXL: cf = (v & msb) != 0; r = ((v << 1) | (oldc ? 1u : 0u)) & m; break;
                default:           cf = (v & 1) != 0; r = (v >> 1) | (oldc ? msb : 0u); break;
            }
            f = (uint16_t)(R->sr & ~(H8_SR_N | H8_SR_Z | H8_SR_V | H8_SR_C));
            if (r & msb) f |= H8_SR_N;
            if (r == 0)  f |= H8_SR_Z;
            if (vf)      f |= H8_SR_V;
            if (cf)      f |= H8_SR_C;
            R->sr = f;
            O_WRITE(r);
            break;
        }
        case H8_RMW_BSETI: case H8_RMW_BCLRI: case H8_RMW_BNOTI: case H8_RMW_BTSTI:
        case H8_RMW_BSETR: case H8_RMW_BCLRR: case H8_RMW_BNOTR: case H8_RMW_BTSTR:
        {
            const int      from_reg = K >= H8_RMW_BSETR;
            const unsigned bit  = from_reg ? (unsigned)(R->r[ip->r & 7] & 0x0F) : (unsigned)(ip->x >> 4);
            const uint32_t mask = (uint32_t)1 << bit;
            const uint32_t v    = O_READ();
            if (v & mask)
                R->sr &= (uint16_t)~H8_SR_Z;
            else
                R->sr |= H8_SR_Z;
            if (K == H8_RMW_BSETI || K == H8_RMW_BSETR)
                O_WRITE(v | mask);
            else if (K == H8_RMW_BCLRI || K == H8_RMW_BCLRR)
                O_WRITE(v & ~mask);
            else if (K == H8_RMW_BNOTI || K == H8_RMW_BNOTR)
                O_WRITE(v ^ mask);
            break;
        }
        case H8_RMW_CMPIMM:
        {
            const uint32_t v = O_READ();
            const uint32_t s = (uint32_t)(uint16_t)(((uint16_t)ip->cyc2 << 8) | ip->icnt2) & m;
            h8_flags_sub(c, w, v, s, v - s, 0);
            break;
        }
        case H8_RMW_MOVIMM:
        {
            const uint32_t v = (uint32_t)(uint16_t)(((uint16_t)ip->cyc2 << 8) | ip->icnt2) & m;
            O_WRITE(v);
            h8_set_nz(c, w, v);
            R->sr &= (uint16_t)~H8_SR_V;
            break;
        }
        case H8_RMW_MOVSTORE:
        {
            const uint32_t v = H8_GET_REG(w, ip->r & 7);
            O_WRITE(v);
            h8_set_nz(c, w, v);
            R->sr &= (uint16_t)~H8_SR_V;
            break;
        }
        case H8_RMW_MOVTPE:
        {
            const uint32_t v = R->r[ip->r & 7] & 0xFF;
            O_WRITE(v);
            break;
        }
        case H8_RMW_STC:
        {
            const uint8_t cr = (uint8_t)(ip->r & 7);
            int done = 0;
            if (!w && (M == H8_EA_PREDEC || M == H8_EA_POSTINC) && (ip->r >> 4) == 7)
            {
                const uint16_t sp_word = (M == H8_EA_POSTINC) ? (uint16_t)(R->r[7] - 2) : R->r[7];
                const uint32_t b  = h8_read_cr(c, cr, 0) & 0xFF;
                const uint32_t wd = (cr == 4) ? (((uint32_t)R->ep << 8) | R->dp) : ((b << 8) | b);
                h8_mem_write16(c, h8_stack_addr(c, sp_word), wd);
                done = 1;
            }
            if (!done)
            {
                const uint32_t v = h8_read_cr(c, cr, w) & m;
                O_WRITE(v);
            }
            break;
        }
        default:
            break;
    }
#undef O_READ
#undef O_WRITE
    states = ip->cyc + H8_EA_PEN(M, w, ip);
    H8_END_SEQ(states);
}

/* Run the cell at the current pc. */
static void h8_exec(h8_cpu_t *c)
{
    h8_regs_t *R = &c->regs;
    const uint16_t pc = R->pc;
    h8_cell_t *ip = &c->pages[c->page][pc];

    if (ip->fn == H8_FN_FILL)
        h8_fill(c, ip, pc);

    switch (ip->fn)
    {
        case H8_FN_ALU:
            h8_exec_alu(c, ip, pc);
            return;
        case H8_FN_RMW:
            h8_exec_rmw(c, ip, pc);
            return;
        case H8_FN_XCH:
        {
            const unsigned rs = ip->r >> 4, rd = ip->r & 7;
            const uint16_t t = R->r[rs];
            R->r[rs] = R->r[rd];
            R->r[rd] = t;
            ++c->insn_count;
            H8_END_SEQ(ip->cyc);
        }
        case H8_FN_SWAP:
        {
            const unsigned rd = ip->r & 7;
            const uint16_t v = R->r[rd];
            R->r[rd] = (uint16_t)((v << 8) | (v >> 8));
            h8_set_nz(c, 1, R->r[rd]);
            R->sr &= (uint16_t)~H8_SR_V;
            ++c->insn_count;
            H8_END_SEQ(ip->cyc);
        }
        case H8_FN_EXTS:
        case H8_FN_EXTU:
        {
            const unsigned rd = ip->r & 7;
            R->r[rd] = ip->fn == H8_FN_EXTS ? (uint16_t)(int16_t)(int8_t)R->r[rd] : (uint16_t)(R->r[rd] & 0x00FF);
            h8_set_nz(c, 1, R->r[rd]);
            R->sr &= (uint16_t)~(H8_SR_V | H8_SR_C);
            ++c->insn_count;
            H8_END_SEQ(ip->cyc);
        }
        case H8_FN_DADD:
        case H8_FN_DSUB:
        {
            const unsigned rs = ip->r >> 4, rd = ip->r & 7;
            int out;
            const uint8_t r = ip->fn == H8_FN_DSUB
                ? h8_bcd_sub((uint8_t)R->r[rd], (uint8_t)R->r[rs], (R->sr & H8_SR_C) != 0, &out)
                : h8_bcd_add((uint8_t)R->r[rd], (uint8_t)R->r[rs], (R->sr & H8_SR_C) != 0, &out);
            uint16_t f;
            H8_SET_REG(0, rd, r);
            f = (uint16_t)(R->sr & ~(H8_SR_Z | H8_SR_C));
            if (r == 0 && (R->sr & H8_SR_Z))
                f |= H8_SR_Z;
            if (out)
                f |= H8_SR_C;
            R->sr = f;
            ++c->insn_count;
            H8_END_SEQ(ip->cyc);
        }
        case H8_FN_LOGIC_CR:
        {
            const int      w   = H8_W(ip->sz);
            const uint8_t  cr  = (uint8_t)(ip->r & 7);
            const uint32_t imm = (uint32_t)ip->imm & H8_MASK(w);
            const uint32_t cur = h8_read_cr(c, cr, w);
            const uint32_t r   = ip->k == 0 ? (cur & imm) : ip->k == 1 ? (cur | imm) : (cur ^ imm);
            h8_write_cr(c, cr, r, w);
            if (cr != 0 && cr != 1)
            {
                h8_set_nz(c, w, r);
                R->sr &= (uint16_t)~H8_SR_V;
            }
            h8_raise(c, H8_PEND_DEFER);
            ++c->insn_count;
            H8_END_SEQ(ip->cyc);
        }
        case H8_FN_TRAPA:
        {
            const uint8_t vector = (uint8_t)(H8_VEC_TRAPA_BASE + (ip->r & 0x0F));
            ++c->insn_count;
            if (c->trapa_hook && !c->trapa_hook(c->trapa_ctx, vector))
                H8_END_SEQ(ip->cyc);
            h8_enter_exception(c, vector, H8_PC_NEXT(ip), -1);
            H8_END_FAR(ip->cyc + H8_STACK_PEN(ip->icnt));
        }
        case H8_FN_TRAPVS:
            ++c->insn_count;
            if (R->sr & H8_SR_V)
            {
                h8_enter_exception(c, H8_VEC_TRAPVS, H8_PC_NEXT(ip), -1);
                H8_END_FAR(ip->cyc2 + H8_STACK_PEN(ip->icnt2));
            }
            H8_END_SEQ(ip->cyc);
        case H8_FN_RTE:
            h8_write_cr(c, 0, h8_pop16(c), 1);
            if (c->max_mode)
                R->cp = (uint8_t)h8_pop16(c);
            R->pc = h8_pop16(c);
            h8_raise(c, H8_PEND_DEFER);
            ++c->insn_count;
            H8_END_FAR(ip->cyc + H8_STACK_PEN(ip->icnt));
        case H8_FN_LINK:
            h8_push16(c, R->r[6]);
            R->r[6] = R->r[7];
            R->r[7] = (uint16_t)(R->r[7] + (int16_t)ip->imm);
            ++c->insn_count;
            H8_END_SEQ(ip->cyc + H8_STACK_PEN(ip->icnt));
        case H8_FN_UNLK:
            R->r[7] = R->r[6];
            R->r[6] = h8_pop16(c);
            ++c->insn_count;
            H8_END_SEQ(ip->cyc + H8_STACK_PEN(ip->icnt));
        case H8_FN_SLEEP:
            c->sleeping = 1;
            h8_raise(c, H8_PEND_SLEEP);
            ++c->insn_count;
            H8_END_SEQ(ip->cyc);
        case H8_FN_NOP:
            ++c->insn_count;
            H8_END_SEQ(ip->cyc);
        case H8_FN_LDM:
        {
            const unsigned list = ip->imm & 0xFF;
            unsigned i;
            for (i = 0; i < 8; ++i)
            {
                uint16_t v;
                if (!(list & (1u << i)))
                    continue;
                v = h8_pop16(c);
                if (i != 7)
                    R->r[i] = v;
            }
            ++c->insn_count;
            H8_END_SEQ(ip->cyc + H8_STACK_PEN(ip->icnt));
        }
        case H8_FN_STM:
        {
            const unsigned list = ip->imm & 0xFF;
            int i;
            for (i = 7; i >= 0; --i)
            {
                if (!(list & (1u << i)))
                    continue;
                if (i == 7)
                {
                    R->r[7] = (uint16_t)(R->r[7] - 2);
                    h8_mem_write16(c, h8_stack_addr(c, R->r[7]), R->r[7]);
                }
                else
                    h8_push16(c, R->r[i]);
            }
            ++c->insn_count;
            H8_END_SEQ(ip->cyc + H8_STACK_PEN(ip->icnt));
        }
        case H8_FN_BCC:
            ++c->insn_count;
            if (h8_cond_true(ip->k, R->sr))
                H8_END_AT((uint16_t)(H8_PC_NEXT(ip) + (int16_t)ip->imm), ip->cyc2);
            H8_END_SEQ(ip->cyc);
        case H8_FN_JMP:
        {
            const unsigned n = ip->r >> 4;
            uint16_t target;
            unsigned states;
            if (ip->ea == H8_EA_ABS16)
                target = ip->imm;
            else if (ip->ea == H8_EA_REGIND)
                target = R->r[n];
            else
                target = (uint16_t)(R->r[n] + (int16_t)ip->imm);
            states = ip->cyc;
            if (ip->k)
            {
                h8_push16(c, H8_PC_NEXT(ip));
                states += H8_STACK_PEN(ip->icnt);
            }
            ++c->insn_count;
            H8_END_AT(target, states);
        }
        case H8_FN_BSR:
        {
            const uint16_t ret = H8_PC_NEXT(ip);
            h8_push16(c, ret);
            ++c->insn_count;
            H8_END_AT((uint16_t)(ret + (int16_t)ip->imm), ip->cyc + H8_STACK_PEN(ip->icnt));
        }
        case H8_FN_RTS:
        {
            const uint16_t target = h8_pop16(c);
            if (ip->k)
                R->r[7] = (uint16_t)(R->r[7] + (int16_t)ip->imm);
            ++c->insn_count;
            H8_END_AT(target, ip->cyc + H8_STACK_PEN(ip->icnt));
        }
        case H8_FN_SCB:
        {
            const unsigned rn = ip->r & 7;
            int exit_loop;
            switch (ip->x >> 4)
            {
                case 1:  exit_loop = !(R->sr & H8_SR_Z); break;
                case 2:  exit_loop = (R->sr & H8_SR_Z) != 0; break;
                default: exit_loop = 0; break;
            }
            ++c->insn_count;
            if (exit_loop)
                H8_END_SEQ(ip->cyc);
            R->r[rn] = (uint16_t)(R->r[rn] - 1);
            if (R->r[rn] == 0xFFFF)
                H8_END_SEQ(ip->cyc + 1u);
            H8_END_AT((uint16_t)(H8_PC_NEXT(ip) + (int16_t)ip->imm), ip->cyc2);
        }
        case H8_FN_PJMP:
        {
            uint8_t  page;
            uint16_t target;
            unsigned states;
            if (ip->ea == H8_EA_ABS24)
            {
                page   = ip->r;
                target = ip->imm;
            }
            else
            {
                const unsigned n = ip->r >> 4;
                page   = (uint8_t)R->r[n];
                target = R->r[(n + 1) & 7];
            }
            states = ip->cyc;
            if (ip->k)
            {
                h8_push16(c, H8_PC_NEXT(ip));
                h8_push16(c, R->cp);
                states += H8_STACK_PEN(ip->icnt);
            }
            R->cp = page;
            R->pc = target;
            ++c->insn_count;
            H8_END_FAR(states);
        }
        case H8_FN_PRTS:
            R->cp = (uint8_t)h8_pop16(c);
            R->pc = h8_pop16(c);
            if (ip->k)
                R->r[7] = (uint16_t)(R->r[7] + (int16_t)ip->imm);
            ++c->insn_count;
            H8_END_FAR(ip->cyc + H8_STACK_PEN(ip->icnt));
        case H8_FN_NOEXEC:
            h8_enter_exception(c, H8_VEC_ADDRESS_ERROR, pc, -1);
            H8_END_FAR(h8_exception_states(c) + H8_STACK_PEN(ip->icnt));
        default:   /* H8_FN_INVALID */
            h8_enter_exception(c, H8_VEC_INVALID_INSN, pc, -1);
            H8_END_FAR(h8_exception_states(c) + H8_STACK_PEN(ip->icnt));
    }
}

/* ---- running (cpu.cpp) ---- */

static uint64_t h8_run_slice(h8_cpu_t *c, int32_t slice)
{
    int64_t used;
    h8_reeval_trace(c);
    h8_reeval_irq(c);
    /* begin slice */
    c->budget    = slice - (c->pending ? H8_FORCE : 0);
    c->slice_len = slice;
    c->in_slice  = 1;

    if (c->sleeping)
    {
        const uint64_t now = h8_total_states(c);
        if (!(c->pending & (H8_PEND_NMI | H8_PEND_IRQ)))
            c->budget -= slice;
        else if (now < c->mask_effective_at)
        {
            uint64_t s = slice > 1 ? (uint64_t)slice : 1;
            if (c->mask_effective_at - now < s)
                s = c->mask_effective_at - now;
            if (s < 1)
                s = 1;
            c->budget -= (int32_t)s;
        }
        else
            h8_service_pending(c);
    }
    else
    {
        h8_cells_for(c, c->regs.cp);
        do
        {
            h8_exec(c);
        } while (c->budget > 0);
        if (c->pending)
            h8_service_pending(c);
    }
    /* end slice */
    used = (int64_t)c->slice_len - (int64_t)h8_true_budget(c);
    c->in_slice = 0;
    c->total_states += (uint64_t)used;
    return (uint64_t)used;
}

static uint64_t h8_poll(h8_cpu_t *c)
{
    uint64_t used;
    h8_reeval_trace(c);
    h8_reeval_irq(c);
    if (!(c->pending & (H8_PEND_ADDRERR | H8_PEND_DEFER | H8_PEND_TRACE | H8_PEND_NMI | H8_PEND_IRQ)))
        return 0;
    c->budget = 0 - (c->pending ? H8_FORCE : 0);
    h8_service_pending(c);
    used = (uint64_t)(-(int64_t)h8_true_budget(c));
    c->total_states += used;
    return used;
}

static uint64_t h8_run(h8_cpu_t *c, uint64_t states)
{
    uint64_t used = 0;
    c->irq_taken = 0;
    while (used < states)
    {
        const int32_t slice = (int32_t)(states - used < (uint64_t)H8_MAX_SLICE ? states - used : (uint64_t)H8_MAX_SLICE);
        int cut;
        if (c->sleeping && !(c->pending & (H8_PEND_NMI | H8_PEND_IRQ)))
        {
            c->total_states += (uint64_t)slice;
            used += (uint64_t)slice;
            continue;
        }
        used += h8_run_slice(c, slice);
        cut = c->cut;
        c->cut = 0;
        if (cut)
            break;
    }
    return used;
}

static void h8_cpu_init(h8_cpu_t *c, h8_bus_t *bus, const h8_chip_config_t *cfg)
{
    memset(c, 0, sizeof(*c));
    c->bus = bus;
    c->cfg = *cfg;
    c->regs.sr = H8_MASK_BITS;
    c->deferred_boundary = ~(uint64_t)0;
    c->max_mode = h8_chip_max_mode(cfg);
    bus->sink = c;
}

static void h8_cpu_free(h8_cpu_t *c)
{
    unsigned p;
    for (p = 0; p < 256; ++p)
        free(c->pages[p]);
    if (c->bus)
        c->bus->sink = NULL;
    memset(c, 0, sizeof(*c));
}

static void h8_cpu_reset(h8_cpu_t *c)
{
    c->max_mode = h8_chip_max_mode(&c->cfg);
    c->regs.sr  = (uint16_t)((c->regs.sr & ~H8_SR_T & H8_SR_MASK) | H8_MASK_BITS);
    c->sleeping  = 0;
    c->irq_taken = 0;
    c->pending   = 0;
    c->budget    = 0;
    if (c->max_mode)
    {
        c->regs.cp = h8_bus_read8(c->bus, 1);
        c->regs.pc = h8_bus_read16(c->bus, 2);
    }
    else
        c->regs.pc = h8_bus_read16(c->bus, 0);
    h8_reeval_irq(c);
}

/* ---- H8/510 on-chip peripherals and the machine around the CPU ---------
 * cpu/common (sched.hpp, iomux.hpp) and cpu/h8500 (intc, dtc, frt, tmr,
 * wdt, sci, adc, ports, machine.hpp), for the H8/510 only: the SC-88 and
 * SC-88Pro use no other chip of the family. */

/* ---- scheduler ---- */

#define H8_NEVER       (~(uint64_t)0)
#define H8_SCHED_MAX   64

typedef void (*h8_event_fn)(void *ctx, uint64_t when, uint64_t now);

typedef struct
{
    uint64_t    when;
    uint32_t    id;
    h8_event_fn fn;
    void       *ctx;
} h8_event_t;

typedef struct
{
    h8_event_t ev[H8_SCHED_MAX];
    unsigned   count;
    uint32_t   next_id;
    h8_cpu_t  *cpu;          /* told when an event lands before the next one due */
} h8_sched_t;

static void h8_sched_init(h8_sched_t *s, h8_cpu_t *cpu)
{
    memset(s, 0, sizeof(*s));
    s->next_id = 1;
    s->cpu     = cpu;
}

/* the earliest event; ties go to the one scheduled first */
static int h8_sched_first(const h8_sched_t *s)
{
    int best = -1;
    unsigned i;
    for (i = 0; i < s->count; ++i)
        if (best < 0 || s->ev[i].when < s->ev[best].when
            || (s->ev[i].when == s->ev[best].when && s->ev[i].id < s->ev[best].id))
            best = (int)i;
    return best;
}

static uint64_t h8_sched_next_time(const h8_sched_t *s)
{
    const int i = h8_sched_first(s);
    return i < 0 ? H8_NEVER : s->ev[i].when;
}

static uint32_t h8_sched_schedule(h8_sched_t *s, uint64_t when, h8_event_fn fn, void *ctx)
{
    const uint64_t before = h8_sched_next_time(s);
    const uint32_t id = s->next_id++;
    if (s->count < H8_SCHED_MAX)
    {
        h8_event_t *e = &s->ev[s->count++];
        e->when = when;
        e->id   = id;
        e->fn   = fn;
        e->ctx  = ctx;
    }
    if (s->cpu && when < before)
        h8_cut_slice(s->cpu, when);
    return id;
}

static void h8_sched_cancel(h8_sched_t *s, uint32_t id)
{
    unsigned i;
    for (i = 0; i < s->count; ++i)
        if (s->ev[i].id == id)
        {
            s->ev[i] = s->ev[--s->count];
            return;
        }
}

static void h8_sched_run_due(h8_sched_t *s, uint64_t now)
{
    for (;;)
    {
        const int i = h8_sched_first(s);
        h8_event_t e;
        if (i < 0 || s->ev[i].when > now)
            return;
        e = s->ev[i];
        s->ev[i] = s->ev[--s->count];
        e.fn(e.ctx, e.when, now);
    }
}

/* ---- register field: which device answers at which address ---- */

#define H8_IO_MAX 0x180

typedef struct
{
    uint32_t base, size;
    const h8_device_t *owners[H8_IO_MAX];
    const h8_device_t *fallback;
    h8_device_t dev;          /* the field itself, as the bus sees it */
} h8_iomux_t;

SC88_INLINE const h8_device_t *h8_io_owner(const h8_iomux_t *m, uint32_t addr)
{
    const uint32_t off = addr - m->base;
    return off < m->size ? m->owners[off] : NULL;
}

static void h8_io_assign(h8_iomux_t *m, uint32_t addr, uint32_t len, const h8_device_t *dev)
{
    uint32_t a;
    for (a = addr; a < addr + len; ++a)
    {
        const uint32_t off = a - m->base;
        if (off < m->size)
            m->owners[off] = dev;
    }
}

static uint8_t h8_io_read8(void *ctx, uint32_t a)
{
    const h8_iomux_t *m = (const h8_iomux_t*)ctx;
    const h8_device_t *d = h8_io_owner(m, a);
    return d ? d->read8(d->ctx, a) : (m->fallback ? m->fallback->read8(m->fallback->ctx, a) : (uint8_t)0xFF);
}

static void h8_io_write8(void *ctx, uint32_t a, uint8_t v)
{
    const h8_iomux_t *m = (const h8_iomux_t*)ctx;
    const h8_device_t *d = h8_io_owner(m, a);
    if (d)
        d->write8(d->ctx, a, v);
    else if (m->fallback)
        m->fallback->write8(m->fallback->ctx, a, v);
}

static uint16_t h8_io_read16(void *ctx, uint32_t a)
{
    const h8_iomux_t *m = (const h8_iomux_t*)ctx;
    const h8_device_t *d = h8_io_owner(m, a);
    uint8_t hi;
    if (d && d == h8_io_owner(m, a + 1))
        return h8_dev_read16(d, a);
    if (!d && !h8_io_owner(m, a + 1) && m->fallback)
        return h8_dev_read16(m->fallback, a);
    hi = h8_io_read8(ctx, a);
    return (uint16_t)(((uint16_t)hi << 8) | h8_io_read8(ctx, a + 1));
}

static void h8_io_write16(void *ctx, uint32_t a, uint16_t v)
{
    const h8_iomux_t *m = (const h8_iomux_t*)ctx;
    const h8_device_t *d = h8_io_owner(m, a);
    if (d && d == h8_io_owner(m, a + 1))
        h8_dev_write16(d, a, v);
    else if (!d && !h8_io_owner(m, a + 1) && m->fallback)
        h8_dev_write16(m->fallback, a, v);
    else
    {
        h8_io_write8(ctx, a, (uint8_t)(v >> 8));
        h8_io_write8(ctx, a + 1, (uint8_t)v);
    }
}

static void h8_io_init(h8_iomux_t *m, uint32_t base, uint32_t size)
{
    memset(m, 0, sizeof(*m));
    m->base = base;
    m->size = size < H8_IO_MAX ? size : H8_IO_MAX;
    m->dev.read8   = h8_io_read8;
    m->dev.write8  = h8_io_write8;
    m->dev.read16  = h8_io_read16;
    m->dev.write16 = h8_io_write16;
    m->dev.ctx     = m;
}

/* ---- interrupt controller ---- */

enum
{
    H8_IRQ_IRQ0, H8_IRQ_WDT, H8_IRQ_IRQ1, H8_IRQ_IRQ2, H8_IRQ_IRQ3,
    H8_IRQ_FRT1_ICI, H8_IRQ_FRT1_OCIA, H8_IRQ_FRT1_OCIB, H8_IRQ_FRT1_FOVI,
    H8_IRQ_FRT2_ICI, H8_IRQ_FRT2_OCIA, H8_IRQ_FRT2_OCIB, H8_IRQ_FRT2_FOVI,
    H8_IRQ_FRT3_ICI, H8_IRQ_FRT3_OCIA, H8_IRQ_FRT3_OCIB, H8_IRQ_FRT3_FOVI,
    H8_IRQ_TMR_CMIA, H8_IRQ_TMR_CMIB, H8_IRQ_TMR_OVI,
    H8_IRQ_SCI1_ERI, H8_IRQ_SCI1_RXI, H8_IRQ_SCI1_TXI,
    H8_IRQ_SCI2_ERI, H8_IRQ_SCI2_RXI, H8_IRQ_SCI2_TXI,
    H8_IRQ_ADI,
    H8_IRQ_COUNT
};

#define H8_ABSENT 0xFF

typedef struct { uint8_t vector, ipr, ipr_high, rank, dte, dte_bit; } h8_irq_info_t;

#define H8_S(vec, ipr, high, rank, dte, bit) { (vec) / 2, (ipr), (high), (rank), (dte), (bit) }
#define H8_N()                               { 0, 0, 0, H8_ABSENT, 0xFF, 0 }

static const h8_irq_info_t h8_510_irq[H8_IRQ_COUNT] =
{
    H8_S(0x40, 0, 1, 0, 0, 4),
    H8_S(0x42, 0, 1, 1, 0xFF, 0),
    H8_S(0x48, 0, 0, 2, 0, 0),
    H8_S(0x4A, 0, 0, 3, 0, 1),
    H8_S(0x4C, 0, 0, 4, 0, 2),
    H8_S(0x50, 1, 1, 5, 1, 4),
    H8_S(0x52, 1, 1, 6, 1, 5),
    H8_S(0x54, 1, 1, 7, 1, 6),
    H8_S(0x56, 1, 1, 8, 0xFF, 0),
    H8_S(0x58, 1, 0, 9, 1, 0),
    H8_S(0x5A, 1, 0, 10, 1, 1),
    H8_S(0x5C, 1, 0, 11, 1, 2),
    H8_S(0x5E, 1, 0, 12, 0xFF, 0),
    H8_N(), H8_N(), H8_N(), H8_N(),
    H8_S(0x60, 2, 1, 13, 2, 4),
    H8_S(0x62, 2, 1, 14, 2, 5),
    H8_S(0x64, 2, 1, 15, 0xFF, 0),
    H8_S(0x68, 2, 0, 16, 0xFF, 0),
    H8_S(0x6A, 2, 0, 17, 2, 1),
    H8_S(0x6C, 2, 0, 18, 2, 2),
    H8_S(0x70, 3, 1, 19, 0xFF, 0),
    H8_S(0x72, 3, 1, 20, 3, 5),
    H8_S(0x74, 3, 1, 21, 3, 6),
    H8_S(0x78, 3, 0, 22, 3, 0)
};

#define H8_INTC_IPR    0xFF00u
#define H8_INTC_DTE    0xFF08u
#define H8_INTC_NMICR  0xFF1Cu
#define H8_INTC_IRQCR  0xFF1Du

struct h8_dtc;

typedef struct
{
    h8_cpu_t *cpu;
    struct h8_dtc *dtc;
    uint8_t req[H8_IRQ_COUNT];
    uint8_t latched[H8_IRQ_COUNT];
    uint8_t irq_pin_low[4];
    uint8_t nmi_pin_high;
    uint8_t ipr[4];
    uint8_t dte[4];
    uint8_t nmicr;
    uint8_t irqcr;
    h8_device_t dev;
} h8_intc_t;

static int h8_dtc_request(struct h8_dtc *d, int src, uint8_t vector);

SC88_INLINE uint8_t h8_intc_level_of(const h8_intc_t *ic, int src)
{
    const h8_irq_info_t *i = &h8_510_irq[src];
    return (uint8_t)((ic->ipr[i->ipr] >> (i->ipr_high ? 4 : 0)) & 7);
}

SC88_INLINE int h8_intc_dtc_enabled(const h8_intc_t *ic, int src)
{
    const h8_irq_info_t *i = &h8_510_irq[src];
    return i->dte != 0xFF && ((ic->dte[i->dte] >> i->dte_bit) & 1);
}

static void h8_intc_update(h8_intc_t *ic)
{
    unsigned best_level = 0;
    uint8_t  best_rank = 0xFF;
    int      best = -1, s;
    for (s = 0; s < H8_IRQ_COUNT; ++s)
    {
        const h8_irq_info_t *info = &h8_510_irq[s];
        unsigned lvl;
        if (!ic->req[s] && !ic->latched[s])
            continue;
        if (info->rank == H8_ABSENT)
            continue;
        if (ic->req[s] && !ic->latched[s] && h8_intc_dtc_enabled(ic, s) && ic->dtc)
        {
            if (h8_dtc_request(ic->dtc, s, info->vector))
                continue;
        }
        lvl = h8_intc_level_of(ic, s);
        if (lvl > best_level || (lvl == best_level && best >= 0 && info->rank < best_rank))
        {
            best_level = lvl;
            best_rank  = info->rank;
            best       = s;
        }
    }
    if (best < 0 || best_level == 0)
    {
        h8_set_irq(ic->cpu, 0, 0);
        return;
    }
    h8_set_irq(ic->cpu, (uint8_t)best_level, h8_510_irq[best].vector);
}

static void h8_intc_set_request(h8_intc_t *ic, int src, int active)
{
    active = active != 0;
    if (ic->req[src] == active)
        return;
    ic->req[src] = (uint8_t)active;
    h8_intc_update(ic);
}

static void h8_intc_raise_cpu_interrupt(h8_intc_t *ic, int src)
{
    ic->latched[src] = 1;
    h8_intc_update(ic);
}

#define H8_INTC_IRQ_ENABLED(ic, n) (((ic)->irqcr >> (n)) & 1)

static void h8_intc_set_irq_pin(h8_intc_t *ic, unsigned n, int low)
{
    int was_low;
    if (n > 3)
        return;
    was_low = ic->irq_pin_low[n];
    ic->irq_pin_low[n] = low != 0;
    if (!H8_INTC_IRQ_ENABLED(ic, n))
        return;
    if (n == 0)
        h8_intc_set_request(ic, H8_IRQ_IRQ0, low);
    else if (low && !was_low)
        h8_intc_set_request(ic, H8_IRQ_IRQ1 + (int)n - 1, 1);
}

static void h8_intc_irq_acknowledged(void *ctx, uint8_t vector)
{
    h8_intc_t *ic = (h8_intc_t*)ctx;
    int s;
    for (s = 0; s < H8_IRQ_COUNT; ++s)
    {
        if (h8_510_irq[s].rank == H8_ABSENT || h8_510_irq[s].vector != vector)
            continue;
        if (!ic->req[s] && !ic->latched[s])
            continue;
        if (s == H8_IRQ_IRQ1 || s == H8_IRQ_IRQ2 || s == H8_IRQ_IRQ3)
            ic->req[s] = 0;
        ic->latched[s] = 0;
        break;
    }
    h8_intc_update(ic);
}

static uint8_t h8_intc_read8(void *ctx, uint32_t addr)
{
    const h8_intc_t *ic = (const h8_intc_t*)ctx;
    if (addr >= H8_INTC_IPR && addr < H8_INTC_IPR + 4) return ic->ipr[addr - H8_INTC_IPR];
    if (addr >= H8_INTC_DTE && addr < H8_INTC_DTE + 4) return ic->dte[addr - H8_INTC_DTE];
    if (addr == H8_INTC_NMICR) return ic->nmicr;
    if (addr == H8_INTC_IRQCR) return ic->irqcr;
    return 0xFF;
}

static void h8_intc_write8(void *ctx, uint32_t addr, uint8_t value)
{
    h8_intc_t *ic = (h8_intc_t*)ctx;
    if (addr >= H8_INTC_IPR && addr < H8_INTC_IPR + 4)
    {
        ic->ipr[addr - H8_INTC_IPR] = value & 0x77;
        h8_intc_update(ic);
    }
    else if (addr >= H8_INTC_DTE && addr < H8_INTC_DTE + 4)
    {
        ic->dte[addr - H8_INTC_DTE] = value;
        h8_intc_update(ic);
    }
    else if (addr == H8_INTC_NMICR)
        ic->nmicr = (uint8_t)(0xFE | (value & 1));
    else if (addr == H8_INTC_IRQCR)
    {
        unsigned n;
        ic->irqcr = (uint8_t)(0xF0 | (value & 0x0F));
        h8_intc_set_request(ic, H8_IRQ_IRQ0, H8_INTC_IRQ_ENABLED(ic, 0) && ic->irq_pin_low[0]);
        for (n = 1; n <= 3; ++n)
            if (!H8_INTC_IRQ_ENABLED(ic, n))
                h8_intc_set_request(ic, H8_IRQ_IRQ1 + (int)n - 1, 0);
    }
}

static void h8_intc_reset(h8_intc_t *ic)
{
    memset(ic->req, 0, sizeof(ic->req));
    memset(ic->latched, 0, sizeof(ic->latched));
    memset(ic->ipr, 0, sizeof(ic->ipr));
    memset(ic->dte, 0, sizeof(ic->dte));
    ic->nmicr = 0xFE;
    ic->irqcr = 0xF0;
    h8_intc_update(ic);
}

static void h8_intc_init(h8_intc_t *ic, h8_cpu_t *cpu)
{
    memset(ic, 0, sizeof(*ic));
    ic->cpu = cpu;
    ic->nmi_pin_high = 1;
    ic->nmicr = 0xFE;
    ic->irqcr = 0xF0;
    ic->dev.read8  = h8_intc_read8;
    ic->dev.write8 = h8_intc_write8;
    ic->dev.ctx    = ic;
    cpu->irq_ack     = h8_intc_irq_acknowledged;
    cpu->irq_ack_ctx = ic;
}

/* ---- free-running timer ---- */

#define H8_FRT_ICIE  0x80
#define H8_FRT_OCIEB 0x40
#define H8_FRT_OCIEA 0x20
#define H8_FRT_OVIE  0x10
#define H8_FRT_OEB   0x08
#define H8_FRT_OEA   0x04
#define H8_FRT_ICF   0x80
#define H8_FRT_OCFB  0x40
#define H8_FRT_OCFA  0x20
#define H8_FRT_OVF   0x10
#define H8_FRT_OLVLB 0x08
#define H8_FRT_OLVLA 0x04
#define H8_FRT_IEDG  0x02
#define H8_FRT_CCLRA 0x01

typedef struct
{
    h8_sched_t *sched;
    h8_cpu_t   *clock;
    h8_intc_t  *intc;
    uint32_t base;
    int      src_ici, src_ocia, src_ocib, src_fovi;
    uint32_t event;
    uint8_t  tcr, tcsr, flags_read;
    uint16_t frc, ocra, ocrb, icr;
    uint8_t  temp;
    uint64_t tick, now;
    int      out_a, out_b;
    h8_device_t dev;
} h8_frt_t;

SC88_INLINE unsigned h8_frt_shift(const h8_frt_t *f)
{
    switch (f->tcr & 3)
    {
        case 0:  return 2;
        case 1:  return 3;
        case 2:  return 5;
        default: return 0;
    }
}

#define H8_FRT_INTERNAL(f) (((f)->tcr & 3) != 3)

static void h8_frt_update_requests(h8_frt_t *f)
{
    h8_intc_set_request(f->intc, f->src_ici,  (f->tcsr & H8_FRT_ICF)  && (f->tcr & H8_FRT_ICIE));
    h8_intc_set_request(f->intc, f->src_ocia, (f->tcsr & H8_FRT_OCFA) && (f->tcr & H8_FRT_OCIEA));
    h8_intc_set_request(f->intc, f->src_ocib, (f->tcsr & H8_FRT_OCFB) && (f->tcr & H8_FRT_OCIEB));
    h8_intc_set_request(f->intc, f->src_fovi, (f->tcsr & H8_FRT_OVF)  && (f->tcr & H8_FRT_OVIE));
}

static void h8_frt_transition(h8_frt_t *f)
{
    const int match_a  = f->frc == f->ocra;
    const int match_b  = f->frc == f->ocrb;
    const int clear    = match_a && (f->tcsr & H8_FRT_CCLRA);
    const int overflow = f->frc == 0xFFFF && !clear;
    if (match_a)
    {
        f->tcsr |= H8_FRT_OCFA;
        if (f->tcr & H8_FRT_OEA)
            f->out_a = (f->tcsr & H8_FRT_OLVLA) != 0;
    }
    if (match_b)
    {
        f->tcsr |= H8_FRT_OCFB;
        if (f->tcr & H8_FRT_OEB)
            f->out_b = (f->tcsr & H8_FRT_OLVLB) != 0;
    }
    if (overflow)
        f->tcsr |= H8_FRT_OVF;
    f->frc = clear ? 0 : (uint16_t)(f->frc + 1);
}

static void h8_frt_sync(h8_frt_t *f, uint64_t now)
{
    uint64_t cur;
    if (now <= f->now)
        return;
    f->now = now;
    if (!H8_FRT_INTERNAL(f))
        return;
    cur = now >> h8_frt_shift(f);
    while (f->tick < cur)
    {
        const uint64_t a = (uint64_t)(uint16_t)(f->ocra - f->frc) + 1;
        const uint64_t b = (uint64_t)(uint16_t)(f->ocrb - f->frc) + 1;
        const uint64_t o = (uint64_t)(uint16_t)(0xFFFF - f->frc) + 1;
        uint64_t d = a < b ? a : b;
        if (o < d)
            d = o;
        if (f->tick + d > cur)
            break;
        f->frc   = (uint16_t)(f->frc + (d - 1));
        f->tick += d;
        h8_frt_transition(f);
    }
    f->frc  = (uint16_t)(f->frc + (cur - f->tick));
    f->tick = cur;
    h8_frt_update_requests(f);
}

static void h8_frt_on_event(void *self, uint64_t when, uint64_t now);

static void h8_frt_reschedule(h8_frt_t *f)
{
    int      want = 0;
    uint64_t best = H8_NEVER, t;
    if (f->event)
    {
        h8_sched_cancel(f->sched, f->event);
        f->event = 0;
    }
    if (!H8_FRT_INTERNAL(f))
        return;
    if ((f->tcr & H8_FRT_OCIEA) && !(f->tcsr & H8_FRT_OCFA))
    {
        want = 1;
        t = (uint64_t)(uint16_t)(f->ocra - f->frc) + 1;
        if (t < best) best = t;
    }
    if ((f->tcr & H8_FRT_OCIEB) && !(f->tcsr & H8_FRT_OCFB))
    {
        want = 1;
        t = (uint64_t)(uint16_t)(f->ocrb - f->frc) + 1;
        if (t < best) best = t;
    }
    if ((f->tcr & H8_FRT_OVIE) && !(f->tcsr & H8_FRT_OVF))
    {
        want = 1;
        t = (uint64_t)(uint16_t)(0xFFFF - f->frc) + 1;
        if (t < best) best = t;
    }
    if (!want)
        return;
    f->event = h8_sched_schedule(f->sched, (f->tick + best) << h8_frt_shift(f), h8_frt_on_event, f);
}

static void h8_frt_on_event(void *self, uint64_t when, uint64_t now)
{
    h8_frt_t *f = (h8_frt_t*)self;
    (void)when;
    f->event = 0;
    h8_frt_sync(f, now);
    h8_frt_reschedule(f);
}

static void h8_frt_dtc_clear(h8_frt_t *f, int src)
{
    if (src == f->src_ici)       f->tcsr &= (uint8_t)~H8_FRT_ICF;
    else if (src == f->src_ocia) f->tcsr &= (uint8_t)~H8_FRT_OCFA;
    else if (src == f->src_ocib) f->tcsr &= (uint8_t)~H8_FRT_OCFB;
    h8_frt_update_requests(f);
    h8_frt_reschedule(f);
}

static uint8_t h8_frt_read8(void *ctx, uint32_t addr)
{
    h8_frt_t *f = (h8_frt_t*)ctx;
    h8_frt_sync(f, h8_total_states(f->clock));
    switch (addr - f->base)
    {
        case 0: return f->tcr;
        case 1:
            f->flags_read |= (uint8_t)(f->tcsr & 0xF0);
            return f->tcsr;
        case 2: f->temp = (uint8_t)f->frc; return (uint8_t)(f->frc >> 8);
        case 3: return f->temp;
        case 4: return (uint8_t)(f->ocra >> 8);
        case 5: return (uint8_t)f->ocra;
        case 6: return (uint8_t)(f->ocrb >> 8);
        case 7: return (uint8_t)f->ocrb;
        case 8: f->temp = (uint8_t)f->icr; return (uint8_t)(f->icr >> 8);
        case 9: return f->temp;
        default: return 0xFF;
    }
}

static void h8_frt_write8(void *ctx, uint32_t addr, uint8_t v)
{
    h8_frt_t *f = (h8_frt_t*)ctx;
    h8_frt_sync(f, h8_total_states(f->clock));
    switch (addr - f->base)
    {
        case 0:
        {
            const unsigned old_shift = h8_frt_shift(f);
            f->tcr = v;
            if (h8_frt_shift(f) != old_shift && H8_FRT_INTERNAL(f))
                f->tick = f->now >> h8_frt_shift(f);
            h8_frt_update_requests(f);
            h8_frt_reschedule(f);
            break;
        }
        case 1:
        {
            uint8_t cleared = 0;
            unsigned bit;
            for (bit = 0x10; bit < 0x100; bit <<= 1)
                if (!(v & bit) && (f->flags_read & bit))
                    cleared |= (uint8_t)bit;
            f->tcsr = (uint8_t)((f->tcsr & 0xF0 & ~cleared) | (v & 0x0F));
            f->flags_read &= (uint8_t)~cleared;
            h8_frt_update_requests(f);
            h8_frt_reschedule(f);
            break;
        }
        case 2: case 4: case 6:
            f->temp = v;
            break;
        case 3:
            f->frc = (uint16_t)(((uint16_t)f->temp << 8) | v);
            h8_frt_reschedule(f);
            break;
        case 5:
            f->ocra = (uint16_t)(((uint16_t)f->temp << 8) | v);
            h8_frt_reschedule(f);
            break;
        case 7:
            f->ocrb = (uint16_t)(((uint16_t)f->temp << 8) | v);
            h8_frt_reschedule(f);
            break;
        default:
            break;
    }
}

static void h8_frt_reset(h8_frt_t *f)
{
    if (f->event)
        h8_sched_cancel(f->sched, f->event);
    f->event = 0;
    f->tcr = 0;
    f->tcsr = 0;
    f->flags_read = 0;
    f->frc = 0;
    f->ocra = f->ocrb = 0xFFFF;
    f->icr = 0;
    f->temp = 0;
    f->tick = 0;
    f->now = 0;
    f->out_a = f->out_b = 0;
    h8_frt_update_requests(f);
}

static void h8_frt_init(h8_frt_t *f, h8_sched_t *sched, h8_cpu_t *clock, h8_intc_t *intc,
                        uint32_t base, int first_src)
{
    memset(f, 0, sizeof(*f));
    f->sched = sched;
    f->clock = clock;
    f->intc  = intc;
    f->base  = base;
    f->src_ici  = first_src;
    f->src_ocia = first_src + 1;
    f->src_ocib = first_src + 2;
    f->src_fovi = first_src + 3;
    f->ocra = f->ocrb = 0xFFFF;
    f->dev.read8  = h8_frt_read8;
    f->dev.write8 = h8_frt_write8;
    f->dev.ctx    = f;
}

/* ---- 8-bit timer ---- */

#define H8_TMR_CMIEB 0x80
#define H8_TMR_CMIEA 0x40
#define H8_TMR_OVIE  0x20
#define H8_TMR_CMFB  0x80
#define H8_TMR_CMFA  0x40
#define H8_TMR_OVF   0x20

typedef struct
{
    h8_sched_t *sched;
    h8_cpu_t   *clock;
    h8_intc_t  *intc;
    uint32_t base;
    uint32_t event;
    uint8_t  tcr, tcsr, flags_read, tcora, tcorb, tcnt;
    uint64_t tick, now;
    int      out;
    h8_device_t dev;
} h8_tmr_t;

SC88_INLINE unsigned h8_tmr_shift(const h8_tmr_t *t)
{
    switch (t->tcr & 7)
    {
        case 1:  return 3;
        case 2:  return 6;
        case 3:  return 10;
        default: return 0;
    }
}

static void h8_tmr_update_requests(h8_tmr_t *t)
{
    h8_intc_set_request(t->intc, H8_IRQ_TMR_CMIA, (t->tcsr & H8_TMR_CMFA) && (t->tcr & H8_TMR_CMIEA));
    h8_intc_set_request(t->intc, H8_IRQ_TMR_CMIB, (t->tcsr & H8_TMR_CMFB) && (t->tcr & H8_TMR_CMIEB));
    h8_intc_set_request(t->intc, H8_IRQ_TMR_OVI,  (t->tcsr & H8_TMR_OVF)  && (t->tcr & H8_TMR_OVIE));
}

static void h8_tmr_transition(h8_tmr_t *t)
{
    const int match_a = t->tcnt == t->tcora;
    const int match_b = t->tcnt == t->tcorb;
    const unsigned cclr = (t->tcr >> 3) & 3;
    const int clear    = (cclr == 1 && match_a) || (cclr == 2 && match_b);
    const int overflow = t->tcnt == 0xFF && !clear;
    unsigned action = 0;
    if (match_a) t->tcsr |= H8_TMR_CMFA;
    if (match_b) t->tcsr |= H8_TMR_CMFB;
    if (overflow) t->tcsr |= H8_TMR_OVF;
    if (match_a && (unsigned)(t->tcsr & 3) > action)
        action = t->tcsr & 3;
    if (match_b && (unsigned)((t->tcsr >> 2) & 3) > action)
        action = (t->tcsr >> 2) & 3;
    switch (action)
    {
        case 1: t->out = 0; break;
        case 2: t->out = 1; break;
        case 3: t->out = !t->out; break;
        default: break;
    }
    t->tcnt = clear ? 0 : (uint8_t)(t->tcnt + 1);
}

static void h8_tmr_sync(h8_tmr_t *t, uint64_t now)
{
    uint64_t cur;
    if (now <= t->now)
        return;
    t->now = now;
    if (h8_tmr_shift(t) == 0)
        return;
    cur = now >> h8_tmr_shift(t);
    while (t->tick < cur)
    {
        const uint64_t a = (uint64_t)(uint8_t)(t->tcora - t->tcnt) + 1;
        const uint64_t b = (uint64_t)(uint8_t)(t->tcorb - t->tcnt) + 1;
        const uint64_t o = (uint64_t)(uint8_t)(0xFF - t->tcnt) + 1;
        uint64_t d = a < b ? a : b;
        if (o < d)
            d = o;
        if (t->tick + d > cur)
            break;
        t->tcnt  = (uint8_t)(t->tcnt + (d - 1));
        t->tick += d;
        h8_tmr_transition(t);
    }
    t->tcnt = (uint8_t)(t->tcnt + (cur - t->tick));
    t->tick = cur;
    h8_tmr_update_requests(t);
}

static void h8_tmr_on_event(void *self, uint64_t when, uint64_t now);

static void h8_tmr_reschedule(h8_tmr_t *t)
{
    uint64_t best = H8_NEVER, v;
    if (t->event)
    {
        h8_sched_cancel(t->sched, t->event);
        t->event = 0;
    }
    if (h8_tmr_shift(t) == 0)
        return;
    if ((t->tcr & H8_TMR_CMIEA) && !(t->tcsr & H8_TMR_CMFA))
    {
        v = (uint64_t)(uint8_t)(t->tcora - t->tcnt) + 1;
        if (v < best) best = v;
    }
    if ((t->tcr & H8_TMR_CMIEB) && !(t->tcsr & H8_TMR_CMFB))
    {
        v = (uint64_t)(uint8_t)(t->tcorb - t->tcnt) + 1;
        if (v < best) best = v;
    }
    if ((t->tcr & H8_TMR_OVIE) && !(t->tcsr & H8_TMR_OVF))
    {
        v = (uint64_t)(uint8_t)(0xFF - t->tcnt) + 1;
        if (v < best) best = v;
    }
    if (best == H8_NEVER)
        return;
    t->event = h8_sched_schedule(t->sched, (t->tick + best) << h8_tmr_shift(t), h8_tmr_on_event, t);
}

static void h8_tmr_on_event(void *self, uint64_t when, uint64_t now)
{
    h8_tmr_t *t = (h8_tmr_t*)self;
    (void)when;
    t->event = 0;
    h8_tmr_sync(t, now);
    h8_tmr_reschedule(t);
}

static void h8_tmr_dtc_clear(h8_tmr_t *t, int src)
{
    if (src == H8_IRQ_TMR_CMIA)      t->tcsr &= (uint8_t)~H8_TMR_CMFA;
    else if (src == H8_IRQ_TMR_CMIB) t->tcsr &= (uint8_t)~H8_TMR_CMFB;
    h8_tmr_update_requests(t);
    h8_tmr_reschedule(t);
}

static uint8_t h8_tmr_read8(void *ctx, uint32_t addr)
{
    h8_tmr_t *t = (h8_tmr_t*)ctx;
    h8_tmr_sync(t, h8_total_states(t->clock));
    switch (addr - t->base)
    {
        case 0: return t->tcr;
        case 1:
            t->flags_read |= (uint8_t)(t->tcsr & 0xE0);
            return (uint8_t)(t->tcsr | 0x10);
        case 2: return t->tcora;
        case 3: return t->tcorb;
        case 4: return t->tcnt;
        default: return 0xFF;
    }
}

static void h8_tmr_write8(void *ctx, uint32_t addr, uint8_t v)
{
    h8_tmr_t *t = (h8_tmr_t*)ctx;
    h8_tmr_sync(t, h8_total_states(t->clock));
    switch (addr - t->base)
    {
        case 0:
        {
            const unsigned old_shift = h8_tmr_shift(t);
            t->tcr = v;
            if (h8_tmr_shift(t) != old_shift && h8_tmr_shift(t) != 0)
                t->tick = t->now >> h8_tmr_shift(t);
            h8_tmr_update_requests(t);
            h8_tmr_reschedule(t);
            break;
        }
        case 1:
        {
            uint8_t cleared = 0;
            unsigned bit;
            for (bit = 0x20; bit < 0x100; bit <<= 1)
                if (!(v & bit) && (t->flags_read & bit))
                    cleared |= (uint8_t)bit;
            t->tcsr = (uint8_t)((t->tcsr & 0xE0 & ~cleared) | (v & 0x0F));
            t->flags_read &= (uint8_t)~cleared;
            h8_tmr_update_requests(t);
            h8_tmr_reschedule(t);
            break;
        }
        case 2: t->tcora = v; h8_tmr_reschedule(t); break;
        case 3: t->tcorb = v; h8_tmr_reschedule(t); break;
        case 4: t->tcnt = v;  h8_tmr_reschedule(t); break;
        default: break;
    }
}

static void h8_tmr_reset(h8_tmr_t *t)
{
    if (t->event)
        h8_sched_cancel(t->sched, t->event);
    t->event = 0;
    t->tcr = 0;
    t->tcsr = 0;
    t->flags_read = 0;
    t->tcora = t->tcorb = 0xFF;
    t->tcnt = 0;
    t->tick = 0;
    t->now = 0;
    t->out = 0;
    h8_tmr_update_requests(t);
}

static void h8_tmr_init(h8_tmr_t *t, h8_sched_t *sched, h8_cpu_t *clock, h8_intc_t *intc, uint32_t base)
{
    memset(t, 0, sizeof(*t));
    t->sched = sched;
    t->clock = clock;
    t->intc  = intc;
    t->base  = base;
    t->tcora = t->tcorb = 0xFF;
    t->dev.read8  = h8_tmr_read8;
    t->dev.write8 = h8_tmr_write8;
    t->dev.ctx    = t;
}

/* ---- watchdog ---- */

#define H8_WDT_OVF   0x80
#define H8_WDT_WTIT  0x40
#define H8_WDT_TME   0x20
#define H8_WDT_WRST  0x80
#define H8_WDT_RSTOE 0x40
#define H8_WDT_TCSR   0xFF10u
#define H8_WDT_RSTCSR 0xFF1Eu

struct h8_machine;
static void h8m_watchdog_reset(struct h8_machine *m);

typedef struct
{
    h8_sched_t *sched;
    h8_cpu_t   *clock;
    h8_intc_t  *intc;
    struct h8_machine *reset_sink;
    uint32_t event;
    uint8_t  tcsr, tcnt, rstcsr;
    int      ovf_read;
    uint64_t tick, now;
    h8_device_t dev;
} h8_wdt_t;

SC88_INLINE unsigned h8_wdt_shift(const h8_wdt_t *w)
{
    static const uint8_t shift[8] = {1, 5, 6, 7, 8, 9, 11, 12};
    return shift[w->tcsr & 7];
}

#define H8_WDT_RUNNING(w) (((w)->tcsr & H8_WDT_TME) != 0)

static void h8_wdt_update_request(h8_wdt_t *w)
{
    h8_intc_set_request(w->intc, H8_IRQ_WDT, (w->tcsr & H8_WDT_OVF) && !(w->tcsr & H8_WDT_WTIT));
}

static void h8_wdt_reset(h8_wdt_t *w, int by_watchdog)
{
    if (w->event)
        h8_sched_cancel(w->sched, w->event);
    w->event = 0;
    w->tcsr = 0;
    w->tcnt = 0;
    if (!by_watchdog)
        w->rstcsr = 0;
    w->ovf_read = 0;
    w->tick = 0;
    w->now = 0;
    h8_wdt_update_request(w);
}

static void h8_wdt_overflow(h8_wdt_t *w)
{
    if (w->tcsr & H8_WDT_WTIT)
    {
        w->rstcsr |= H8_WDT_WRST;
        if (w->reset_sink)
            h8m_watchdog_reset(w->reset_sink);
        else
            h8_wdt_reset(w, 1);
    }
    else
    {
        w->tcsr |= H8_WDT_OVF;
        h8_wdt_update_request(w);
    }
}

static void h8_wdt_sync(h8_wdt_t *w, uint64_t now)
{
    uint64_t cur;
    if (now <= w->now)
        return;
    w->now = now;
    if (!H8_WDT_RUNNING(w))
        return;
    cur = now >> h8_wdt_shift(w);
    while (w->tick < cur)
    {
        const uint64_t d = (uint64_t)(uint8_t)(0xFF - w->tcnt) + 1;
        if (w->tick + d > cur)
            break;
        w->tick += d;
        w->tcnt = 0;
        h8_wdt_overflow(w);
        if (!H8_WDT_RUNNING(w))
        {
            w->tick = cur;
            return;
        }
    }
    w->tcnt = (uint8_t)(w->tcnt + (cur - w->tick));
    w->tick = cur;
}

static void h8_wdt_on_event(void *self, uint64_t when, uint64_t now);

static void h8_wdt_reschedule(h8_wdt_t *w)
{
    uint64_t d;
    if (w->event)
    {
        h8_sched_cancel(w->sched, w->event);
        w->event = 0;
    }
    if (!H8_WDT_RUNNING(w))
        return;
    d = (uint64_t)(uint8_t)(0xFF - w->tcnt) + 1;
    w->event = h8_sched_schedule(w->sched, (w->tick + d) << h8_wdt_shift(w), h8_wdt_on_event, w);
}

static void h8_wdt_on_event(void *self, uint64_t when, uint64_t now)
{
    h8_wdt_t *w = (h8_wdt_t*)self;
    (void)when;
    w->event = 0;
    h8_wdt_sync(w, now);
    h8_wdt_reschedule(w);
}

static uint8_t h8_wdt_read8(void *ctx, uint32_t addr)
{
    h8_wdt_t *w = (h8_wdt_t*)ctx;
    h8_wdt_sync(w, h8_total_states(w->clock));
    if (addr == H8_WDT_TCSR)
    {
        if (w->tcsr & H8_WDT_OVF)
            w->ovf_read = 1;
        return (uint8_t)(w->tcsr | 0x18);
    }
    if (addr == H8_WDT_TCSR + 1)
        return w->tcnt;
    if (addr == H8_WDT_RSTCSR + 1)
        return (uint8_t)(w->rstcsr | 0x3F);
    return 0xFF;
}

/* The watchdog takes word writes only, keyed by the high byte. */
static void h8_wdt_write8(void *ctx, uint32_t addr, uint8_t value)
{
    (void)ctx; (void)addr; (void)value;
}

static void h8_wdt_write16(void *ctx, uint32_t addr, uint16_t value)
{
    h8_wdt_t *w = (h8_wdt_t*)ctx;
    const uint8_t key = (uint8_t)(value >> 8), data = (uint8_t)value;
    h8_wdt_sync(w, h8_total_states(w->clock));
    if (addr == H8_WDT_TCSR)
    {
        if (key == 0xA5)
        {
            uint8_t next = (uint8_t)((w->tcsr & H8_WDT_OVF) | (data & 0x67));
            int was_running;
            unsigned old_shift;
            if (!(data & H8_WDT_OVF) && w->ovf_read)
                next &= (uint8_t)~H8_WDT_OVF;
            w->ovf_read = 0;
            was_running = H8_WDT_RUNNING(w);
            old_shift   = h8_wdt_shift(w);
            w->tcsr = next;
            if (!H8_WDT_RUNNING(w))
                w->tcnt = 0;
            else if (!was_running || h8_wdt_shift(w) != old_shift)
                w->tick = w->now >> h8_wdt_shift(w);
            h8_wdt_update_request(w);
            h8_wdt_reschedule(w);
        }
        else if (key == 0x5A)
        {
            w->tcnt = data;
            h8_wdt_reschedule(w);
        }
    }
    else if (addr == H8_WDT_RSTCSR)
    {
        if (key == 0xA5 && data == 0x00)
            w->rstcsr &= (uint8_t)~H8_WDT_WRST;
        else if (key == 0x5A)
            w->rstcsr = (uint8_t)((w->rstcsr & ~H8_WDT_RSTOE) | (data & H8_WDT_RSTOE));
    }
}

static void h8_wdt_init(h8_wdt_t *w, h8_sched_t *sched, h8_cpu_t *clock, h8_intc_t *intc)
{
    memset(w, 0, sizeof(*w));
    w->sched = sched;
    w->clock = clock;
    w->intc  = intc;
    w->dev.read8   = h8_wdt_read8;
    w->dev.write8  = h8_wdt_write8;
    w->dev.write16 = h8_wdt_write16;
    w->dev.ctx     = w;
}

/* ---- serial port ---- */

#define H8_SCI_CA   0x80
#define H8_SCI_CHR  0x40
#define H8_SCI_PE   0x20
#define H8_SCI_STOP 0x08
#define H8_SCI_TIE  0x80
#define H8_SCI_RIE  0x40
#define H8_SCI_TE   0x20
#define H8_SCI_RE   0x10
#define H8_SCI_CKE1 0x02
#define H8_SCI_TDRE 0x80
#define H8_SCI_RDRF 0x40
#define H8_SCI_ORER 0x20
#define H8_SCI_FER  0x10
#define H8_SCI_PER  0x08

typedef struct { uint8_t byte, fer, per; } h8_rx_frame_t;

typedef struct
{
    h8_sched_t *sched;
    h8_cpu_t   *clock;
    h8_intc_t  *intc;
    uint32_t base;
    int      src_eri, src_rxi, src_txi;
    void   (*tx_sink)(void *ctx, uint8_t byte, uint64_t at);
    void    *tx_ctx;
    void   (*rx_hook)(void *ctx);
    void    *rx_ctx;
    uint32_t tx_event, rx_event;
    uint8_t  smr, brr, scr, tdr, ssr, rdr;
    uint8_t  flags_read;
    uint64_t ext_bit_states;
    uint8_t  tsr;
    int      tsr_valid;
    uint64_t tx_end;
    h8_rx_frame_t *rxq;       /* bytes on their way in, oldest first */
    size_t   rxq_head, rxq_count, rxq_cap;
    uint64_t rx_end;
    uint64_t now;
    h8_device_t dev;
} h8_sci_t;

static uint64_t h8_sci_bit_states(const h8_sci_t *s)
{
    uint64_t n, base;
    if ((s->scr & H8_SCI_CKE1) && s->ext_bit_states)
        return s->ext_bit_states;
    n    = (uint64_t)1 << (2 * (s->smr & 3));
    base = (s->smr & H8_SCI_CA) ? 4 : 32;
    return base * n * ((uint64_t)s->brr + 1);
}

static uint64_t h8_sci_frame_states(const h8_sci_t *s)
{
    unsigned data, parity, stop;
    if (s->smr & H8_SCI_CA)
        return 8 * h8_sci_bit_states(s);
    data   = (s->smr & H8_SCI_CHR) ? 7 : 8;
    parity = (s->smr & H8_SCI_PE) ? 1 : 0;
    stop   = (s->smr & H8_SCI_STOP) ? 2 : 1;
    return (uint64_t)(1 + data + parity + stop) * h8_sci_bit_states(s);
}

static void h8_sci_update_requests(h8_sci_t *s)
{
    h8_intc_set_request(s->intc, s->src_txi, (s->scr & H8_SCI_TIE) && (s->ssr & H8_SCI_TDRE));
    h8_intc_set_request(s->intc, s->src_rxi, (s->scr & H8_SCI_RIE) && (s->ssr & H8_SCI_RDRF));
    h8_intc_set_request(s->intc, s->src_eri, (s->scr & H8_SCI_RIE)
                        && (s->ssr & (H8_SCI_ORER | H8_SCI_FER | H8_SCI_PER)));
}

static void h8_sci_sync(h8_sci_t *s, uint64_t now);

static void h8_sci_on_tx_event(void *self, uint64_t when, uint64_t now)
{
    h8_sci_t *s = (h8_sci_t*)self;
    (void)when;
    s->tx_event = 0;
    h8_sci_sync(s, now);
}

static void h8_sci_on_rx_event(void *self, uint64_t when, uint64_t now)
{
    h8_sci_t *s = (h8_sci_t*)self;
    (void)when;
    s->rx_event = 0;
    h8_sci_sync(s, now);
}

static void h8_sci_schedule_tx(h8_sci_t *s, uint64_t when)
{
    if (s->tx_event)
        h8_sched_cancel(s->sched, s->tx_event);
    s->tx_event = h8_sched_schedule(s->sched, when, h8_sci_on_tx_event, s);
}

static void h8_sci_schedule_rx(h8_sci_t *s, uint64_t when)
{
    if (s->rx_event)
        h8_sched_cancel(s->sched, s->rx_event);
    s->rx_event = h8_sched_schedule(s->sched, when, h8_sci_on_rx_event, s);
}

static void h8_sci_start_tx_frame(h8_sci_t *s, uint64_t at)
{
    s->tsr = ((s->smr & H8_SCI_CHR) && !(s->smr & H8_SCI_CA)) ? (uint8_t)(s->tdr & 0x7F) : s->tdr;
    s->tsr_valid = 1;
    s->tx_end = at + h8_sci_frame_states(s);
    s->ssr |= H8_SCI_TDRE;
    h8_sci_schedule_tx(s, s->tx_end);
    h8_sci_update_requests(s);
}

static void h8_sci_complete_rx_frame(h8_sci_t *s, uint64_t at)
{
    h8_rx_frame_t f = s->rxq[s->rxq_head];
    s->rxq_head = (s->rxq_head + 1) % s->rxq_cap;
    s->rxq_count--;
    if (s->scr & H8_SCI_RE)
    {
        if (s->smr & H8_SCI_CA)
        {
            f.fer = 0;
            f.per = 0;
        }
        else if (!(s->smr & H8_SCI_PE))
            f.per = 0;
        if (s->ssr & H8_SCI_RDRF)
        {
            s->ssr |= H8_SCI_ORER;
            if (f.fer) s->ssr |= H8_SCI_FER;
            if (f.per) s->ssr |= H8_SCI_PER;
        }
        else
        {
            s->rdr = ((s->smr & H8_SCI_CHR) && !(s->smr & H8_SCI_CA)) ? (uint8_t)(f.byte & 0x7F) : f.byte;
            if (f.fer || f.per)
            {
                if (f.fer) s->ssr |= H8_SCI_FER;
                if (f.per) s->ssr |= H8_SCI_PER;
            }
            else
                s->ssr |= H8_SCI_RDRF;
        }
        if (s->rx_hook)
            s->rx_hook(s->rx_ctx);
    }
    s->rx_end = s->rxq_count == 0 ? 0 : at + h8_sci_frame_states(s);
    if (s->rx_end)
        h8_sci_schedule_rx(s, s->rx_end);
    h8_sci_update_requests(s);
}

static void h8_sci_sync(h8_sci_t *s, uint64_t now)
{
    if (now < s->now)
        return;
    s->now = now;
    if (s->tx_end && now >= s->tx_end)
    {
        const uint64_t ended = s->tx_end;
        s->tx_end = 0;
        if (s->tsr_valid && s->tx_sink)
            s->tx_sink(s->tx_ctx, s->tsr, ended);
        s->tsr_valid = 0;
        if ((s->scr & H8_SCI_TE) && !(s->ssr & H8_SCI_TDRE))
            h8_sci_start_tx_frame(s, ended);
    }
    while (s->rx_end && now >= s->rx_end)
        h8_sci_complete_rx_frame(s, s->rx_end);
}

#define H8_SCI_TSR_BUSY(s, now) ((s)->tx_end != 0 && (now) < (s)->tx_end)

static void h8_sci_receive_byte(h8_sci_t *s, uint8_t byte, int framing_error, int parity_error)
{
    const uint64_t now = h8_total_states(s->clock);
    h8_rx_frame_t *slot;
    h8_sci_sync(s, now);
    if (s->rxq_count == s->rxq_cap)
    {
        const size_t cap = s->rxq_cap ? s->rxq_cap * 2 : 64;
        h8_rx_frame_t *q = (h8_rx_frame_t*)malloc(cap * sizeof(*q));
        size_t i;
        if (!q)
            return;
        for (i = 0; i < s->rxq_count; ++i)
            q[i] = s->rxq[(s->rxq_head + i) % s->rxq_cap];
        free(s->rxq);
        s->rxq      = q;
        s->rxq_cap  = cap;
        s->rxq_head = 0;
    }
    slot = &s->rxq[(s->rxq_head + s->rxq_count) % s->rxq_cap];
    slot->byte = byte;
    slot->fer  = framing_error != 0;
    slot->per  = parity_error != 0;
    s->rxq_count++;
    if (!s->rx_end)
    {
        s->rx_end = now + h8_sci_frame_states(s);
        h8_sci_schedule_rx(s, s->rx_end);
    }
}

static void h8_sci_dtc_wrote_tdr(h8_sci_t *s, uint8_t value)
{
    const uint64_t now = h8_total_states(s->clock);
    h8_sci_sync(s, now);
    s->tdr = value;
    s->ssr &= (uint8_t)~H8_SCI_TDRE;
    if ((s->scr & H8_SCI_TE) && !H8_SCI_TSR_BUSY(s, now))
        h8_sci_start_tx_frame(s, now);
    h8_sci_update_requests(s);
}

static uint8_t h8_sci_dtc_read_rdr(h8_sci_t *s)
{
    s->ssr &= (uint8_t)~H8_SCI_RDRF;
    h8_sci_update_requests(s);
    return s->rdr;
}

static uint8_t h8_sci_read8(void *ctx, uint32_t addr)
{
    h8_sci_t *s = (h8_sci_t*)ctx;
    h8_sci_sync(s, h8_total_states(s->clock));
    switch (addr - s->base)
    {
        case 0: return (uint8_t)(s->smr | 0x04);
        case 1: return s->brr;
        case 2: return (uint8_t)(s->scr | 0x0C);
        case 3: return s->tdr;
        case 4:
            s->flags_read |= (uint8_t)(s->ssr & 0xF8);
            return (uint8_t)(s->ssr | 0x07);
        case 5: return s->rdr;
        default: return 0xFF;
    }
}

static void h8_sci_write8(void *ctx, uint32_t addr, uint8_t v)
{
    h8_sci_t *s = (h8_sci_t*)ctx;
    const uint64_t now = h8_total_states(s->clock);
    h8_sci_sync(s, now);
    switch (addr - s->base)
    {
        case 0: s->smr = (uint8_t)(v | 0x04); break;
        case 1: s->brr = v; break;
        case 2:
        {
            const uint8_t old = s->scr;
            s->scr = (uint8_t)(v | 0x0C);
            if ((s->scr & H8_SCI_TE) && !(old & H8_SCI_TE))
            {
                s->tsr_valid = 0;
                s->tx_end = now + h8_sci_frame_states(s);
                h8_sci_schedule_tx(s, s->tx_end);
            }
            else if (!(s->scr & H8_SCI_TE) && (old & H8_SCI_TE))
                s->ssr |= H8_SCI_TDRE;
            h8_sci_update_requests(s);
            break;
        }
        case 3:
            s->tdr = v;
            break;
        case 4:
        {
            uint8_t cleared = 0;
            unsigned bit;
            for (bit = 0x08; bit < 0x100; bit <<= 1)
                if (!(v & bit) && (s->flags_read & bit))
                    cleared |= (uint8_t)bit;
            s->ssr &= (uint8_t)~cleared;
            s->flags_read &= (uint8_t)~cleared;
            if ((cleared & H8_SCI_TDRE) && (s->scr & H8_SCI_TE) && !H8_SCI_TSR_BUSY(s, now))
                h8_sci_start_tx_frame(s, now);
            h8_sci_update_requests(s);
            break;
        }
        default:
            break;
    }
}

static void h8_sci_reset(h8_sci_t *s)
{
    if (s->tx_event) h8_sched_cancel(s->sched, s->tx_event);
    if (s->rx_event) h8_sched_cancel(s->sched, s->rx_event);
    s->tx_event = s->rx_event = 0;
    s->smr = 0x04;
    s->brr = 0xFF;
    s->scr = 0x0C;
    s->tdr = 0xFF;
    s->ssr = 0x87;
    s->rdr = 0;
    s->flags_read = 0;
    s->tsr = 0xFF;
    s->tsr_valid = 0;
    s->tx_end = 0;
    s->rxq_head = s->rxq_count = 0;
    s->rx_end = 0;
    s->now = 0;
    h8_sci_update_requests(s);
}

static void h8_sci_init(h8_sci_t *s, h8_sched_t *sched, h8_cpu_t *clock, h8_intc_t *intc,
                        uint32_t base, int first_src)
{
    memset(s, 0, sizeof(*s));
    s->sched = sched;
    s->clock = clock;
    s->intc  = intc;
    s->base  = base;
    s->src_eri = first_src;
    s->src_rxi = first_src + 1;
    s->src_txi = first_src + 2;
    s->smr = 0x04; s->brr = 0xFF; s->scr = 0x0C; s->tdr = 0xFF; s->ssr = 0x87;
    s->tsr = 0xFF;
    s->dev.read8  = h8_sci_read8;
    s->dev.write8 = h8_sci_write8;
    s->dev.ctx    = s;
}

/* ---- A/D converter ---- */

#define H8_ADC_ADF  0x80
#define H8_ADC_ADIE 0x40
#define H8_ADC_ADST 0x20
#define H8_ADC_SCAN 0x10
#define H8_ADC_CKS  0x08
#define H8_ADC_TRGE 0x80

typedef struct
{
    h8_sched_t *sched;
    h8_cpu_t   *clock;
    h8_intc_t  *intc;
    uint32_t base;
    uint32_t event;
    uint16_t (*sampler)(void *ctx, unsigned channel);
    void    *sampler_ctx;
    uint8_t  ch_mask;
    uint16_t addr[4];
    uint16_t inputs[4];
    uint8_t  adcsr, adcr, temp;
    unsigned channel;
    uint64_t end, now;
    h8_device_t dev;
} h8_adc_t;

#define H8_ADC_CONV_STATES(a, first) (((a)->adcsr & H8_ADC_CKS) ? ((first) ? 134u : 128u) : ((first) ? 266u : 256u))
#define H8_ADC_LAST_CH(a)   ((unsigned)((a)->adcsr & (a)->ch_mask))
#define H8_ADC_FIRST_CH(a)  (((a)->adcsr & H8_ADC_SCAN) ? (unsigned)(uint8_t)((a)->adcsr & (a)->ch_mask & ~3u) \
                                                        : (unsigned)(uint8_t)((a)->adcsr & (a)->ch_mask))

static uint16_t h8_adc_sample(const h8_adc_t *a, unsigned ch)
{
    return a->sampler ? (uint16_t)(a->sampler(a->sampler_ctx, ch) & 0x3FF) : a->inputs[ch & 3];
}

static void h8_adc_update_request(h8_adc_t *a)
{
    h8_intc_set_request(a->intc, H8_IRQ_ADI, (a->adcsr & H8_ADC_ADF) && (a->adcsr & H8_ADC_ADIE));
}

static void h8_adc_sync(h8_adc_t *a, uint64_t now)
{
    uint64_t t, n, wraps, done, i;
    unsigned first, last, g, pos;
    if (now < a->now)
        return;
    a->now = now;
    if (!a->end || now < a->end)
        return;
    if (!(a->adcsr & H8_ADC_SCAN))
    {
        a->addr[a->channel & 3] = (uint16_t)(h8_adc_sample(a, a->channel) << 6);
        a->adcsr |= H8_ADC_ADF;
        a->adcsr &= (uint8_t)~H8_ADC_ADST;
        a->end = 0;
        h8_adc_update_request(a);
        return;
    }
    t     = H8_ADC_CONV_STATES(a, 0);
    n     = (now - a->end) / t + 1;
    first = H8_ADC_FIRST_CH(a);
    last  = H8_ADC_LAST_CH(a);
    g     = last - first + 1;
    pos   = a->channel - first;
    wraps = (pos + n) / g;
    done  = n < g ? n : g;
    for (i = 0; i < done; ++i)
    {
        const unsigned ch = first + (unsigned)((pos + n - done + i) % g);
        a->addr[ch & 3] = (uint16_t)(h8_adc_sample(a, ch) << 6);
    }
    if (wraps)
        a->adcsr |= H8_ADC_ADF;
    a->channel = first + (unsigned)((pos + n) % g);
    a->end += n * t;
    h8_adc_update_request(a);
}

static void h8_adc_on_event(void *self, uint64_t when, uint64_t now);

static void h8_adc_reschedule(h8_adc_t *a)
{
    uint64_t at;
    if (a->event)
    {
        h8_sched_cancel(a->sched, a->event);
        a->event = 0;
    }
    if (!a->end)
        return;
    at = a->end;
    if (a->adcsr & H8_ADC_SCAN)
    {
        if (a->adcsr & H8_ADC_ADF)
            return;
        at += (uint64_t)(H8_ADC_LAST_CH(a) - a->channel) * H8_ADC_CONV_STATES(a, 0);
    }
    a->event = h8_sched_schedule(a->sched, at, h8_adc_on_event, a);
}

static void h8_adc_on_event(void *self, uint64_t when, uint64_t now)
{
    h8_adc_t *a = (h8_adc_t*)self;
    (void)when;
    a->event = 0;
    h8_adc_sync(a, now);
    h8_adc_reschedule(a);
}

static void h8_adc_dtc_clear(h8_adc_t *a)
{
    h8_adc_sync(a, h8_total_states(a->clock));
    a->adcsr &= (uint8_t)~H8_ADC_ADF;
    h8_adc_update_request(a);
    h8_adc_reschedule(a);
}

static uint8_t h8_adc_read8(void *ctx, uint32_t addr)
{
    h8_adc_t *a = (h8_adc_t*)ctx;
    uint32_t off;
    h8_adc_sync(a, h8_total_states(a->clock));
    off = addr - a->base;
    if (off < 8)
    {
        const uint16_t r = a->addr[off >> 1];
        if ((off & 1) == 0)
        {
            a->temp = (uint8_t)r;
            return (uint8_t)(r >> 8);
        }
        return a->temp;
    }
    if (off == 8)
        return a->adcsr;
    if (off == 9)
        return (uint8_t)(a->adcr | 0x7F);
    return 0xFF;
}

static void h8_adc_write8(void *ctx, uint32_t addr, uint8_t v)
{
    h8_adc_t *a = (h8_adc_t*)ctx;
    const uint64_t now = h8_total_states(a->clock);
    uint32_t off;
    h8_adc_sync(a, now);
    off = addr - a->base;
    if (off == 8)
    {
        const int was_running = (a->adcsr & H8_ADC_ADST) != 0;
        int running;
        a->adcsr = (uint8_t)((v & 0x7F) | (a->adcsr & v & H8_ADC_ADF));
        running = (a->adcsr & H8_ADC_ADST) != 0;
        if (running && !was_running)
        {
            a->channel = (uint8_t)H8_ADC_FIRST_CH(a);
            a->end = now + H8_ADC_CONV_STATES(a, 1);
            h8_adc_reschedule(a);
        }
        else if (!running && was_running)
        {
            a->end = 0;
            h8_adc_reschedule(a);
        }
        else
            h8_adc_reschedule(a);
        h8_adc_update_request(a);
    }
    else if (off == 9)
        a->adcr = (uint8_t)(v & H8_ADC_TRGE);
}

static void h8_adc_reset(h8_adc_t *a)
{
    if (a->event)
        h8_sched_cancel(a->sched, a->event);
    a->event = 0;
    memset(a->addr, 0, sizeof(a->addr));
    a->adcsr = 0;
    a->adcr = 0;
    a->temp = 0;
    a->channel = 0;
    a->end = 0;
    a->now = 0;
    h8_adc_update_request(a);
}

static void h8_adc_init(h8_adc_t *a, h8_sched_t *sched, h8_cpu_t *clock, h8_intc_t *intc, uint32_t base)
{
    memset(a, 0, sizeof(*a));
    a->sched = sched;
    a->clock = clock;
    a->intc  = intc;
    a->base  = base;
    a->ch_mask = 3;
    a->dev.read8  = h8_adc_read8;
    a->dev.write8 = h8_adc_write8;
    a->dev.ctx    = a;
}

/* ---- I/O ports and system registers ---- */

typedef struct { uint8_t ddr, dr, pins; } h8_port_t;

typedef struct
{
    const h8_chip_config_t *cfg;
    uint32_t  base;
    h8_port_t p[9];
    void    (*write_hook)(void *ctx, unsigned port, uint8_t dr, uint8_t ddr);
    uint8_t (*read_hook)(void *ctx, unsigned port, uint8_t value);
    void     *hook_ctx;
    h8_device_t dev;
} h8_ports_t;

static const uint8_t h8_dr_port[16]      = {0, 0, 1, 2, 0, 0, 3, 4, 0, 0, 5, 6, 0, 0, 7, 8};
static const uint8_t h8_ddr_port_510[16] = {1, 2, 0, 0, 3, 4, 0, 0, 5, 6, 0, 0, 0, 8, 0, 0};

#define H8_PORT_IDX(port) (((port) - 1) & 15)

static uint8_t h8_ports_read_dr(const h8_ports_t *ps, unsigned port)
{
    const h8_port_t *p = &ps->p[H8_PORT_IDX(port)];
    const int bus16 = ps->cfg->mode == 2 || ps->cfg->mode == 4;
    const int max   = h8_chip_max_mode(ps->cfg);
    switch (port)
    {
        case 1: if (bus16) return 0xFF; break;
        case 2: if (max) return p->dr; break;
        case 7: return (uint8_t)(p->pins & 0x0F);
        default: break;
    }
    return (uint8_t)((p->dr & p->ddr) | (p->pins & ~p->ddr));
}

static uint8_t h8_ports_read8(void *ctx, uint32_t addr)
{
    const h8_ports_t *ps = (const h8_ports_t*)ctx;
    const uint32_t off = addr - ps->base;
    unsigned port;
    if (off >= 16)
        return 0xFF;
    if (h8_ddr_port_510[off])
        return 0xFF;
    port = h8_dr_port[off];
    if (port)
    {
        const uint8_t v = h8_ports_read_dr(ps, port);
        return ps->read_hook ? ps->read_hook(ps->hook_ctx, port, v) : v;
    }
    return 0xFF;
}

static void h8_ports_write8(void *ctx, uint32_t addr, uint8_t v)
{
    h8_ports_t *ps = (h8_ports_t*)ctx;
    const uint32_t off = addr - ps->base;
    unsigned port;
    if (off >= 16)
        return;
    port = h8_ddr_port_510[off];
    if (port)
    {
        ps->p[H8_PORT_IDX(port)].ddr = v;
        if (ps->write_hook)
            ps->write_hook(ps->hook_ctx, port, ps->p[H8_PORT_IDX(port)].dr, v);
        return;
    }
    port = h8_dr_port[off];
    if (port)
    {
        if (port == 7)   /* input only */
            return;
        ps->p[H8_PORT_IDX(port)].dr = v;
        if (ps->write_hook)
            ps->write_hook(ps->hook_ctx, port, v, ps->p[H8_PORT_IDX(port)].ddr);
    }
}

static void h8_ports_reset(h8_ports_t *ps)
{
    unsigned i;
    for (i = 0; i < 9; ++i)
    {
        ps->p[i].ddr = 0;
        ps->p[i].dr  = 0;
    }
}

static void h8_ports_init(h8_ports_t *ps, const h8_chip_config_t *cfg, uint32_t base)
{
    unsigned i;
    memset(ps, 0, sizeof(*ps));
    ps->cfg  = cfg;
    ps->base = base;
    for (i = 0; i < 9; ++i)
        ps->p[i].pins = 0xFF;
    ps->dev.read8  = h8_ports_read8;
    ps->dev.write8 = h8_ports_write8;
    ps->dev.ctx    = ps;
}

typedef struct
{
    const h8_chip_config_t *cfg;
    uint8_t rfshcr, wcr, arbt, ar3t, sbycr, brcr;
    h8_device_t dev;
} h8_sysregs_t;

static void h8_sysregs_reset(h8_sysregs_t *r)
{
    r->rfshcr = 0xD8;
    r->wcr    = 0xF3;
    r->arbt   = 0xFF;
    r->ar3t   = 0x00;
    r->sbycr  = 0x7F;
    r->brcr   = 0xFE;
}

static uint8_t h8_sysregs_read8(void *ctx, uint32_t addr)
{
    const h8_sysregs_t *r = (const h8_sysregs_t*)ctx;
    switch (addr)
    {
        case 0xFED8: return r->rfshcr;
        case 0xFF14: return (uint8_t)(r->wcr | 0xF0);
        case 0xFF16: return r->arbt;
        case 0xFF17: return r->ar3t;
        case 0xFF19: return (uint8_t)(0xC0 | (r->cfg->mode & 7));
        case 0xFF1A: return (uint8_t)(r->sbycr | 0x7F);
        case 0xFF1B: return (uint8_t)(r->brcr | 0xFE);
        default: return 0xFF;
    }
}

static void h8_sysregs_write8(void *ctx, uint32_t addr, uint8_t v)
{
    h8_sysregs_t *r = (h8_sysregs_t*)ctx;
    switch (addr)
    {
        case 0xFED8: r->rfshcr = v; break;
        case 0xFF14: r->wcr = (uint8_t)(v & 0x0F); break;
        case 0xFF16: r->arbt = v; break;
        case 0xFF17: r->ar3t = v; break;
        case 0xFF1A: r->sbycr = (uint8_t)(v & 0x80); break;
        case 0xFF1B: r->brcr = (uint8_t)(v & 0x01); break;
        default: break;
    }
}

/* ---- the machine ---- */

typedef struct h8_dtc
{
    struct h8_machine *m;
    uint32_t pending;
    uint8_t  vector[H8_IRQ_COUNT];
    uint64_t transfers;
} h8_dtc_t;

typedef struct h8_machine
{
    h8_chip_config_t cfg;
    h8_bus_t     bus;
    h8_cpu_t     cpu;
    h8_iomux_t   io;
    h8_sched_t   sched;
    h8_intc_t    intc;
    h8_frt_t     frt[2];
    h8_tmr_t     tmr;
    h8_wdt_t     wdt;
    h8_sci_t     sci[2];
    h8_adc_t     adc;
    h8_ports_t   ports;
    h8_sysregs_t sysregs;
    h8_dtc_t     dtc;
    uint64_t     resets;
} h8_machine_t;

/* ---- data transfer controller ---- */

static uint32_t h8_dtc_vector_addr(int src)
{
    switch (src)
    {
        case H8_IRQ_IRQ0:      return 0xC0;
        case H8_IRQ_IRQ1:      return 0xC8;
        case H8_IRQ_IRQ2:      return 0xCA;
        case H8_IRQ_IRQ3:      return 0xCC;
        case H8_IRQ_FRT1_ICI:  return 0xD0;
        case H8_IRQ_FRT1_OCIA: return 0xD2;
        case H8_IRQ_FRT1_OCIB: return 0xD4;
        case H8_IRQ_FRT2_ICI:  return 0xD8;
        case H8_IRQ_FRT2_OCIA: return 0xDA;
        case H8_IRQ_FRT2_OCIB: return 0xDC;
        case H8_IRQ_TMR_CMIA:  return 0xE0;
        case H8_IRQ_TMR_CMIB:  return 0xE2;
        case H8_IRQ_SCI1_RXI:  return 0xEA;
        case H8_IRQ_SCI1_TXI:  return 0xEC;
        case H8_IRQ_SCI2_RXI:  return 0xF2;
        case H8_IRQ_SCI2_TXI:  return 0xF4;
        case H8_IRQ_ADI:       return 0xF8;
        default:               return 0;
    }
}

static int h8_dtc_request(h8_dtc_t *d, int src, uint8_t vector)
{
    if (!h8_dtc_vector_addr(src))
        return 0;
    d->pending |= (uint32_t)1 << src;
    h8_raise(&d->m->cpu, H8_PEND_BREAK);
    d->vector[src] = vector;
    return 1;
}

static unsigned h8_dtc_access_states(const h8_bus_t *bus, uint32_t addr, int word)
{
    switch (H8_ATTR(bus, addr & bus->addr_mask) & H8_AT_CLASS)
    {
        case H8_W16_S2: return 2;
        case H8_W16_S3: return 3;
        case H8_W8_S2:  return word ? 4 : 2;
        default:        return word ? 6 : 3;
    }
}

/* what the peripheral does once its data has been moved */
static void h8_dtc_flag_clear(h8_machine_t *m, int src, uint16_t data)
{
    switch (src)
    {
        case H8_IRQ_FRT1_ICI: case H8_IRQ_FRT1_OCIA: case H8_IRQ_FRT1_OCIB:
            h8_frt_dtc_clear(&m->frt[0], src);
            break;
        case H8_IRQ_FRT2_ICI: case H8_IRQ_FRT2_OCIA: case H8_IRQ_FRT2_OCIB:
            h8_frt_dtc_clear(&m->frt[1], src);
            break;
        case H8_IRQ_SCI1_RXI: h8_sci_dtc_read_rdr(&m->sci[0]); break;
        case H8_IRQ_SCI2_RXI: h8_sci_dtc_read_rdr(&m->sci[1]); break;
        case H8_IRQ_SCI1_TXI: h8_sci_dtc_wrote_tdr(&m->sci[0], (uint8_t)data); break;
        case H8_IRQ_SCI2_TXI: h8_sci_dtc_wrote_tdr(&m->sci[1], (uint8_t)data); break;
        case H8_IRQ_TMR_CMIA: case H8_IRQ_TMR_CMIB:
            h8_tmr_dtc_clear(&m->tmr, src);
            break;
        case H8_IRQ_ADI:
            h8_adc_dtc_clear(&m->adc);
            break;
        default:
            break;
    }
}

static void h8_dtc_transfer(h8_dtc_t *d, int src, uint8_t vector)
{
    h8_machine_t *m = d->m;
    h8_bus_t *bus = &m->bus;
    const int      max = m->cpu.max_mode;
    const uint32_t va  = h8_dtc_vector_addr(src);
    const uint16_t ta   = h8_bus_read16(bus, max ? (va * 2 + 2) : va);
    const uint16_t dtmr = h8_bus_read16(bus, ta);
    uint16_t dtsr = h8_bus_read16(bus, (uint16_t)(ta + 2));
    uint16_t dtdr = h8_bus_read16(bus, (uint16_t)(ta + 4));
    uint16_t dtcr = h8_bus_read16(bus, (uint16_t)(ta + 6));
    const int word = (dtmr & 0x8000) != 0;
    const int si   = (dtmr & 0x4000) != 0;
    const int di   = (dtmr & 0x2000) != 0;
    uint16_t data;
    unsigned states;

    if (word)
    {
        data = h8_bus_read16(bus, dtsr);
        h8_bus_write16(bus, dtdr, data);
    }
    else
    {
        data = h8_bus_read8(bus, dtsr);
        h8_bus_write8(bus, dtdr, (uint8_t)data);
    }
    states = 26 + h8_dtc_access_states(bus, dtsr, word) + h8_dtc_access_states(bus, dtdr, word);
    if (si)
    {
        dtsr = (uint16_t)(dtsr + (word ? 2 : 1));
        h8_bus_write16(bus, (uint16_t)(ta + 2), dtsr);
        states += 2;
    }
    if (di)
    {
        dtdr = (uint16_t)(dtdr + (word ? 2 : 1));
        h8_bus_write16(bus, (uint16_t)(ta + 4), dtdr);
        states += 2;
    }
    dtcr = (uint16_t)(dtcr - 1);
    h8_bus_write16(bus, (uint16_t)(ta + 6), dtcr);
    m->cpu.total_states += states;
    ++d->transfers;
    h8_dtc_flag_clear(m, src, data);
    if (src == H8_IRQ_IRQ1 || src == H8_IRQ_IRQ2 || src == H8_IRQ_IRQ3)
        h8_intc_irq_acknowledged(&m->intc, vector);
    if (dtcr == 0)
        h8_intc_raise_cpu_interrupt(&m->intc, src);
}

static void h8_dtc_service(h8_dtc_t *d)
{
    while (d->pending)
    {
        unsigned s = 0;
        while (!(d->pending & ((uint32_t)1 << s)))
            ++s;
        d->pending &= ~((uint32_t)1 << s);
        h8_dtc_transfer(d, (int)s, d->vector[s]);
    }
}

static void h8m_reset_internal(h8_machine_t *m, int by_watchdog)
{
    ++m->resets;
    h8_intc_reset(&m->intc);
    h8_frt_reset(&m->frt[0]);
    h8_frt_reset(&m->frt[1]);
    h8_tmr_reset(&m->tmr);
    h8_wdt_reset(&m->wdt, by_watchdog);
    h8_sci_reset(&m->sci[0]);
    h8_sci_reset(&m->sci[1]);
    h8_adc_reset(&m->adc);
    h8_ports_reset(&m->ports);
    h8_sysregs_reset(&m->sysregs);
    m->dtc.pending = 0;
    h8_cpu_reset(&m->cpu);
}

static void h8m_watchdog_reset(h8_machine_t *m) { h8m_reset_internal(m, 1); }
static void h8m_reset(h8_machine_t *m)          { h8m_reset_internal(m, 0); }

SC88_INLINE uint64_t h8m_now(const h8_machine_t *m) { return h8_total_states(&m->cpu); }

static uint64_t h8m_run(h8_machine_t *m, uint64_t states)
{
    const uint64_t start = h8m_now(m);
    const uint64_t end   = start + states;
    while (h8m_now(m) < end)
    {
        uint64_t next, want;
        h8_sched_run_due(&m->sched, h8m_now(m));
        h8_dtc_service(&m->dtc);
        h8_poll(&m->cpu);
        next = h8_sched_next_time(&m->sched);
        if (end < next)
            next = end;
        want = next > h8m_now(m) ? next - h8m_now(m) : 1;
        h8_run(&m->cpu, want);
    }
    h8_sched_run_due(&m->sched, h8m_now(m));
    h8_dtc_service(&m->dtc);
    h8_poll(&m->cpu);
    return h8m_now(m) - start;
}

/* An H8/510 in the given mode.  The machine must not move in memory
 * afterwards: its parts point at each other. */
static int h8m_init(h8_machine_t *m, uint8_t mode)
{
    memset(m, 0, sizeof(*m));
    h8_make_chip_config(&m->cfg, H8_MODEL_510, mode);
    if (!h8_bus_init(&m->bus, h8_chip_address_bits(&m->cfg)))
        return 0;
    h8_cpu_init(&m->cpu, &m->bus, &m->cfg);
    h8_io_init(&m->io, m->cfg.regfield_base, m->cfg.regfield_size);
    h8_sched_init(&m->sched, &m->cpu);
    h8_intc_init(&m->intc, &m->cpu);
    h8_frt_init(&m->frt[0], &m->sched, &m->cpu, &m->intc, 0xFEA0, H8_IRQ_FRT1_ICI);
    h8_frt_init(&m->frt[1], &m->sched, &m->cpu, &m->intc, 0xFEB0, H8_IRQ_FRT2_ICI);
    h8_tmr_init(&m->tmr, &m->sched, &m->cpu, &m->intc, 0xFEC0);
    h8_wdt_init(&m->wdt, &m->sched, &m->cpu, &m->intc);
    h8_sci_init(&m->sci[0], &m->sched, &m->cpu, &m->intc, 0xFEC8, H8_IRQ_SCI1_ERI);
    h8_sci_init(&m->sci[1], &m->sched, &m->cpu, &m->intc, 0xFED0, H8_IRQ_SCI2_ERI);
    h8_adc_init(&m->adc, &m->sched, &m->cpu, &m->intc, 0xFE90);
    h8_ports_init(&m->ports, &m->cfg, 0xFE80);
    m->sysregs.cfg = &m->cfg;
    m->sysregs.dev.read8  = h8_sysregs_read8;
    m->sysregs.dev.write8 = h8_sysregs_write8;
    m->sysregs.dev.ctx    = &m->sysregs;
    h8_sysregs_reset(&m->sysregs);
    m->dtc.m = m;

    h8_configure_bus(&m->bus, &m->cfg);
    h8_bus_map(&m->bus, m->cfg.regfield_base, m->cfg.regfield_size, H8_LINE_DEV, &m->io.dev, H8_W8_S3, 0);
    h8_bus_set_noexec(&m->bus, m->cfg.noexec_base, m->cfg.noexec_size, 1);

    h8_io_assign(&m->io, H8_INTC_IPR, 4, &m->intc.dev);
    h8_io_assign(&m->io, H8_INTC_DTE, 4, &m->intc.dev);
    h8_io_assign(&m->io, H8_INTC_NMICR, 1, &m->intc.dev);
    h8_io_assign(&m->io, H8_INTC_IRQCR, 1, &m->intc.dev);
    m->wdt.reset_sink = m;
    h8_io_assign(&m->io, 0xFEA0, 10, &m->frt[0].dev);
    h8_io_assign(&m->io, 0xFEB0, 10, &m->frt[1].dev);
    h8_io_assign(&m->io, 0xFEC0, 5, &m->tmr.dev);
    h8_io_assign(&m->io, H8_WDT_TCSR, 2, &m->wdt.dev);
    h8_io_assign(&m->io, H8_WDT_RSTCSR, 2, &m->wdt.dev);
    h8_io_assign(&m->io, 0xFEC8, 6, &m->sci[0].dev);
    h8_io_assign(&m->io, 0xFED0, 6, &m->sci[1].dev);
    h8_io_assign(&m->io, 0xFE90, 10, &m->adc.dev);
    h8_io_assign(&m->io, 0xFE80, 16, &m->ports.dev);
    h8_io_assign(&m->io, 0xFED8, 1, &m->sysregs.dev);
    h8_io_assign(&m->io, 0xFF14, 1, &m->sysregs.dev);
    h8_io_assign(&m->io, 0xFF16, 2, &m->sysregs.dev);
    h8_io_assign(&m->io, 0xFF19, 3, &m->sysregs.dev);
    m->intc.dtc = &m->dtc;
    return 1;
}

static void h8m_free(h8_machine_t *m)
{
    free(m->sci[0].rxq);
    free(m->sci[1].rxq);
    h8_cpu_free(&m->cpu);
    h8_bus_free(&m->bus);
}

/* ---- SC-88 / SC-88VL / SC-88Pro boards ---------------------------------
 * 88lib/boards/sc88.{h,cpp}, sc88pro.{h,cpp}, mcu/sc88_submcu.cpp and the
 * wave-ROM descrambler from common/romDescramble.h.
 *
 * Left out: the XP-GS model that shares the SC-88 board class, and the
 * front-panel LCD, which the firmware only writes to (the gate array's
 * LCD-ready interrupt, which it does depend on, is kept). */

#define SC88_SAMPLE_RATE  32000u
#define SC88_CPU_CLOCK_HZ 10000000u
#define SC88_ROM_SIZE     0x80000u
#define SC88PRO_ROM_SIZE  0x100000u
#define SC88_SRAM_SIZE    0x10000u
#define SC88PRO_WAVE_SIZE (20u * 1024u * 1024u)

#ifndef SC88_H
enum { SC88_MODEL_SC88, SC88_MODEL_SC88VL, SC88_MODEL_SC88PRO };
#endif

/* sub-MCU shared RAM */
#define SM_WINDOW_SIZE  0x0100
#define SM_SYSEX_STAGE  0x0014
#define SM_VERSION_HI   0x00C0
#define SM_VERSION_LO   0x00C1
#define SM_START        0x00C2
#define SM_COMMAND      0x00DC
#define SM_CHANNEL      0x00DD
#define SM_PARAM1       0x00DE
#define SM_PARAM2       0x00DF
#define SM_SEMAPHORE    0x00FD
#define SM_PANEL_DATA   0x00FE
#define SM_PANEL_CTRL   0x00FF
#define SM_TX_READ      0xd4
#define SM_TX_WRITE     0xd5

/* gate array */
#define GA_LEDS          0xC100
#define GA_LED_CONTROL   0xC101
#define GA_IRQ_STATUS    0xC104
#define GA_IRQ_MASK      0xC105
#define GA_LCD_START     0xC11E
#define GA_LCD_INSTR     0xC11F
#define GA_LCD_DATA      0xC120
#define GA_LCD_DATA_END  0xC12C
#define GA_LCD_BURST     (GA_LCD_DATA_END - GA_LCD_DATA + 1)

#define SC88_IRQ_GATE_ARRAY 0
#define SC88_IRQ_XP         1
#define SC88_IRQ_SUB_MCU    2

#define SC88_POWER_ON_MIDI_DELAY SC88_SAMPLE_RATE
#define SC88_ANALOG_BATTERY      0x2a0
#define SC88_P5DR_STRAPS         0x2d
#define SC88_LCD_READY_DELAY     12
#define SC88_MIDI_BITS_PER_BYTE  10u
#define SC88_MIDI_BAUD           31250u

static const uint8_t sc88_port_dr_offset[9] = { 0, 0x02, 0x03, 0x06, 0x07, 0x0a, 0x0b, 0x0e, 0x0f };

/* ---- wave ROM descrambler (Pcm16) ---- */

static const uint8_t sc88_pcm16_address_bits[20] =
    { 0, 4, 2, 3, 1, 13, 7, 12, 5, 10, 16, 9, 6, 8, 14, 17, 11, 15, 18, 19 };
static const uint8_t sc88_pcm16_data_bits[8] = { 2, 0, 4, 5, 7, 6, 3, 1 };

/* logical byte i is physical byte p(i), with its data bits moved */
static void sc88_pcm16_descramble(const uint8_t *raw, size_t len, uint8_t *dst)
{
    uint8_t data_map[256];
    uint32_t addr_lo[1024], addr_hi[1024];
    unsigned v, b;
    size_t i;
    for (v = 0; v < 256; ++v)
    {
        unsigned out = 0;
        for (b = 0; b < 8; ++b)
            if (v & (1u << sc88_pcm16_data_bits[b]))
                out |= 1u << b;
        data_map[v] = (uint8_t)out;
    }
    for (v = 0; v < 1024; ++v)
    {
        uint32_t lo = 0, hi = 0;
        for (b = 0; b < 10; ++b)
        {
            if (v & (1u << b))
                lo |= (uint32_t)1 << sc88_pcm16_address_bits[b];
            if (v & (1u << b))
                hi |= (uint32_t)1 << sc88_pcm16_address_bits[b + 10];
        }
        addr_lo[v] = lo;
        addr_hi[v] = hi;
    }
    for (i = 0; i < len; ++i)
    {
        const size_t phys = (i & ~(size_t)0xFFFFF) | addr_lo[i & 1023] | addr_hi[(i >> 10) & 1023];
        dst[i] = data_map[phys < len ? raw[phys] : 0];
    }
}

/* ---- MIDI sub-MCU (sc88_submcu.cpp) ---- */

#define SM_SOURCES      3
#define SM_MAX_SYSEX    0x100
#define SM_PAYLOAD_MAX  128

typedef struct
{
    uint8_t wireBytes, command, channel, param1, param2;
    uint8_t payload_len;
    uint8_t payload[SM_PAYLOAD_MAX];
} sm_record_t;

typedef struct
{
    uint8_t  runningStatus;
    uint8_t  data[2];
    uint8_t  have;
    uint8_t  bytesThisMessage;
    uint8_t  inSysEx;
    uint8_t  sysExOverrun;
    uint16_t sysExLen;
    uint8_t  sysEx[SM_MAX_SYSEX];
} sm_source_t;

typedef struct
{
    uint8_t bytes[0x9c];
    uint8_t len;
    uint8_t end;
    uint8_t offset;
} sm_packet_t;

struct sc88_board;
static void sc88b_queue_push(struct sc88_board *b, const sm_record_t *r);

typedef struct
{
    struct sc88_board *board;   /* where its records go */
    uint16_t     stageOffset;
    uint16_t     stageCapacity;
    sm_source_t  sources[SM_SOURCES];
    sm_packet_t *out;           /* packets the firmware queued for MIDI out */
    size_t       out_head, out_count, out_cap;
    uint32_t     outputPhase;
    uint32_t     outputPause;
    void       (*midi_out)(void *ctx, uint8_t byte);
    void        *midi_out_ctx;
} sm_t;

static void sm_reset(sm_t *s)
{
    memset(s->sources, 0, sizeof(s->sources));
    s->out_head = s->out_count = 0;
    s->outputPhase = s->outputPause = 0;
}

static void sm_start_output(sm_t *s, uint8_t *ram)
{
    ram[SM_TX_READ] = ram[SM_TX_WRITE] = 0x24;
    s->out_head = s->out_count = 0;
    s->outputPhase = s->outputPause = 0;
}

static void sm_commit_output(sm_t *s, const uint8_t *ram, uint8_t write)
{
    unsigned pos = ram[SM_TX_WRITE], available, length, i;
    sm_packet_t *p;
    if (pos < 0x24 || pos >= 0xc0 || write < 0x24 || write >= 0xc0 || pos == write)
        return;
    available = (write + 0x9c - pos) % 0x9c;
    length    = ram[pos];
    if (!length || ((length + 4) & ~3u) != available)
        return;
    if (s->out_count == s->out_cap)
    {
        const size_t cap = s->out_cap ? s->out_cap * 2 : 16;
        sm_packet_t *q = (sm_packet_t*)malloc(cap * sizeof(*q));
        size_t k;
        if (!q)
            return;
        for (k = 0; k < s->out_count; ++k)
            q[k] = s->out[(s->out_head + k) % s->out_cap];
        free(s->out);
        s->out      = q;
        s->out_cap  = cap;
        s->out_head = 0;
    }
    p = &s->out[(s->out_head + s->out_count) % s->out_cap];
    p->end    = write;
    p->len    = (uint8_t)length;
    p->offset = 0;
    for (i = 0; i < length; ++i)
    {
        if (++pos == 0xc0)
            pos = 0x24;
        p->bytes[i] = ram[pos];
    }
    s->out_count++;
}

static void sm_clock_output(sm_t *s, uint8_t *ram, uint32_t sample_rate)
{
    sm_packet_t *p;
    if (s->outputPause)
    {
        --s->outputPause;
        return;
    }
    if (s->out_count == 0)
    {
        s->outputPhase = 0;
        return;
    }
    p = &s->out[s->out_head];
    if (p->offset == 0 && p->len == 3 && p->bytes[0] == 0xff && p->bytes[1] == 0)
    {
        s->outputPause = (uint32_t)p->bytes[2] * sample_rate / 1000;
        p->offset = p->len;
    }
    else
    {
        uint8_t byte;
        s->outputPhase += 3125;
        if (s->outputPhase < sample_rate)
            return;
        s->outputPhase -= sample_rate;
        byte = p->bytes[p->offset++];
        if (s->midi_out)
            s->midi_out(s->midi_out_ctx, byte);
    }
    if (p->offset == p->len)
    {
        sm_record_t ready;
        ram[SM_TX_READ] = p->end;
        s->out_head = (s->out_head + 1) % s->out_cap;
        s->out_count--;
        memset(&ready, 0, sizeof(ready));
        ready.command   = 0xe1;
        ready.wireBytes = 0;
        sc88b_queue_push(s->board, &ready);
    }
}

static void sm_voice_message(sm_t *s, uint8_t source, sm_source_t *src)
{
    const uint8_t status = (uint8_t)(src->runningStatus & 0xf0);
    sm_record_t r;
    memset(&r, 0, sizeof(r));
    r.command   = (uint8_t)((status >> 4) - 7);
    r.channel   = (uint8_t)((src->runningStatus & 0x0f) | (source << 4));
    r.param1    = src->data[0];
    r.param2    = src->have > 1 ? src->data[1] : 0;
    r.wireBytes = src->bytesThisMessage > 1 ? src->bytesThisMessage : 1;
    src->have = 0;
    src->bytesThisMessage = 0;
    sc88b_queue_push(s->board, &r);
}

static void sm_raw_transfer(sm_t *s, uint8_t source, uint8_t command, const sm_source_t *src)
{
    const size_t length = (size_t)src->sysExLen - 1;
    const size_t limit  = s->stageCapacity < 0x7f ? s->stageCapacity : 0x7f;
    sm_record_t r;
    if (length > limit)
        return;
    memset(&r, 0, sizeof(r));
    r.command   = command;
    r.channel   = (uint8_t)(0x80 | (source << 4));
    r.param1    = (uint8_t)length;
    r.param2    = (uint8_t)s->stageOffset;
    r.payload_len = (uint8_t)length;
    memcpy(r.payload, src->sysEx + 1, length);
    r.wireBytes = (uint8_t)(src->sysExLen + 1);
    sc88b_queue_push(s->board, &r);
}

static void sm_decoded_transfer(sm_t *s, uint8_t source_in, const sm_source_t *src)
{
    const uint8_t  srcbits = (uint8_t)(source_in << 4);
    const size_t   dt1 = 5, header = 4;
    const size_t   size = src->sysExLen;
    const size_t   chunk_max = s->stageCapacity < 0x7f ? s->stageCapacity : 0x7f;
    uint8_t  staged[160];
    uint8_t  sum = 0, command, source = srcbits, address_delta = 0;
    size_t   i, body, len, sent = 0;
    int      first = 1;

    if (size < dt1 + 4)
        return;
    for (i = dt1; i < size; ++i)
        sum = (uint8_t)(sum + src->sysEx[i]);
    if ((sum & 0x7f) != 0)
    {
        sm_record_t e;
        memset(&e, 0, sizeof(e));
        e.command   = 0xe0;
        e.channel   = srcbits;
        e.param1    = 3;
        e.wireBytes = (uint8_t)(size + 1 < 255 ? size + 1 : 255);
        sc88b_queue_push(s->board, &e);
        return;
    }
    body    = size - dt1;
    command = src->sysEx[dt1];
    if (command == 0)
        command = 0x20;
    if (command >= 0x50 && command <= 0x5f)
    {
        command = (uint8_t)(command - 0x10);
        source  = 1 << 4;
        address_delta = 0x10;
    }
    len = header + body;
    if (len > 0x8a + 7 - 1)
        return;
    memset(staged, 0, header);
    memcpy(staged + header, src->sysEx + dt1, body);
    if (address_delta)
    {
        staged[header]  = command;
        staged[len - 1] = (uint8_t)((staged[len - 1] + address_delta) & 0x7f);
    }
    while (sent < len)
    {
        const size_t take = len - sent < chunk_max ? len - sent : chunk_max;
        size_t wire = take;
        sm_record_t r;
        memset(&r, 0, sizeof(r));
        r.command = first ? command : 0xe7;
        r.channel = (uint8_t)(0x80 | source | (sent + take < len ? 0x40 : 0x00));
        r.param1  = (uint8_t)take;
        r.param2  = (uint8_t)s->stageOffset;
        r.payload_len = (uint8_t)take;
        memcpy(r.payload, staged + sent, take);
        if (first)
            wire += dt1 - header;
        if (sent + take >= len)
            ++wire;
        r.wireBytes = (uint8_t)(wire < 255 ? wire : 255);
        sc88b_queue_push(s->board, &r);
        sent += take;
        first = 0;
    }
}

static void sm_end_of_sysex(sm_t *s, uint8_t source, const sm_source_t *src)
{
    const uint8_t *sx = src->sysEx;
    const size_t   n  = src->sysExLen;
    int display_model;
    if (n < 3)
        return;
    if (sx[1] == 0x7e || sx[1] == 0x7f)
    {
        if (sx[1] == 0x7e && n >= 5 && sx[3] == 0x09)
            sm_raw_transfer(s, source, 0xee, src);
        else if (sx[1] == 0x7f && n >= 7 && sx[3] == 0x04 && sx[4] == 0x01)
            sm_raw_transfer(s, source, 0xef, src);
        return;
    }
    if (sx[1] == 0x43)
    {
        if (n >= 8 && sx[2] == 0x10 && sx[3] == 0x4c)
            sm_raw_transfer(s, source, 0xec, src);
        return;
    }
    if (sx[1] != 0x41)
        return;
    if (n < 6)
        return;
    display_model = sx[3] == 0x45 && n >= 7 && (sx[5] & 0xf0) == 0x10;
    if (sx[3] != 0x42 && !display_model)
        return;
    if (sx[4] != 0x12)
        return;
    sm_decoded_transfer(s, source, src);
}

static void sm_midi_in(sm_t *s, uint8_t source, uint8_t byte)
{
    sm_source_t *src;
    uint8_t status, need;
    if (source >= SM_SOURCES)
        return;
    src = &s->sources[source];
    if (byte >= 0xf8)
        return;
    ++src->bytesThisMessage;
    if (byte >= 0x80)
    {
        if (src->inSysEx)
        {
            src->inSysEx = 0;
            if (byte == 0xf7 && !src->sysExOverrun)
            {
                sm_end_of_sysex(s, source, src);
                src->sysExLen = 0;
                return;
            }
            src->sysExLen = 0;
        }
        if (byte == 0xf0)
        {
            src->inSysEx = 1;
            src->sysExOverrun = 0;
            src->sysEx[0] = byte;
            src->sysExLen = 1;
            src->runningStatus = 0;
            src->bytesThisMessage = 1;
            return;
        }
        if (byte >= 0xf1)
        {
            src->runningStatus = 0;
            src->have = 0;
            src->bytesThisMessage = 0;
            return;
        }
        src->runningStatus = byte;
        src->have = 0;
        src->bytesThisMessage = 1;
        return;
    }
    if (src->inSysEx)
    {
        if (src->sysExLen >= SM_MAX_SYSEX)
            src->sysExOverrun = 1;
        else
            src->sysEx[src->sysExLen++] = byte;
        return;
    }
    if (!src->runningStatus)
        return;
    src->data[src->have++] = byte;
    status = (uint8_t)(src->runningStatus & 0xf0);
    need   = (status == 0xc0 || status == 0xd0) ? 1 : 2;
    if (src->have >= need)
        sm_voice_message(s, source, src);
}

/* ---- the board ---- */

typedef struct
{
    struct sc88_board *board;
    uint32_t page;
    h8_device_t dev;
} sc88_page_dev_t;

typedef struct sc88_board
{
    int          model;
    int          valid;
    h8_machine_t m;
    xp_t         xp;
    lsp_t       *lsp;            /* SC-88Pro only */
    uint8_t     *rom;
    size_t       rom_size;
    uint8_t     *sram;
    uint8_t     *wave;           /* the board's own copy (descrambled for the Pro) */
    size_t       wave_size;
    int          xp_enabled;
    int          lsp_enabled;
    int          lcd_enabled;
    uint64_t     samples_rendered;
    uint64_t     cycle_target;
    uint32_t     cycle_frac;
    uint32_t     buttons;
    uint8_t      scan_column;
    uint8_t      panel_ctrl;
    uint8_t      leds;
    uint8_t      led_control;
    uint8_t      p5dr;           /* SC-88 straps */
    uint16_t     analog[4];
    uint8_t      ga_int[4];
    uint8_t      ga_irq_mask;
    uint8_t      ga_int_trigger;
    uint8_t      lcd_instr;
    uint8_t      lcd_staged;
    uint8_t      lcd_buffer[GA_LCD_BURST];
    uint32_t     ga_lcd_event;
    uint8_t      sm_ram[SM_WINDOW_SIZE];
    int          sm_started;
    sm_t         sm;
    sm_record_t *q;              /* MIDI messages waiting for the firmware */
    size_t       q_head, q_count, q_cap;
    sm_record_t  mailbox;
    int          mailbox_full;
    uint32_t     midi_wire_delay;
    uint32_t     midi_wire_frac;
    /* SC-88Pro */
    uint8_t      p3dr;
    int          lsp_return_enabled;
    int          serial_midi;
    /* bus devices */
    sc88_page_dev_t pages[4];    /* SC-88: pages 0, 8, E, F */
    h8_device_t  regfield_tail;  /* SC-88 */
    h8_device_t  board_bus;      /* SC-88Pro */
} sc88_board_t;

static void sc88b_queue_push(sc88_board_t *b, const sm_record_t *r)
{
    if (b->q_count == b->q_cap)
    {
        const size_t cap = b->q_cap ? b->q_cap * 2 : 64;
        sm_record_t *q = (sm_record_t*)malloc(cap * sizeof(*q));
        size_t k;
        if (!q)
            return;
        for (k = 0; k < b->q_count; ++k)
            q[k] = b->q[(b->q_head + k) % b->q_cap];
        free(b->q);
        b->q      = q;
        b->q_cap  = cap;
        b->q_head = 0;
    }
    b->q[(b->q_head + b->q_count) % b->q_cap] = *r;
    b->q_count++;
}

SC88_INLINE void sc88b_request_irq(sc88_board_t *b, int line, int level)
{
    if (line >= 0)
        h8_intc_set_irq_pin(&b->m.intc, (unsigned)line, level);
}

static void sc88b_set_ga_int(sc88_board_t *b, uint8_t line, int level)
{
    if (line >= 4)
        return;
    if (level && !b->ga_int[line] && !(b->ga_irq_mask & (1u << line)))
        b->ga_int_trigger = (uint8_t)(line + 1);
    b->ga_int[line] = (uint8_t)(level != 0);
    sc88b_request_irq(b, SC88_IRQ_GATE_ARRAY, b->ga_int_trigger != 0);
}

static void sc88b_lcd_ready(void *self, uint64_t when, uint64_t now)
{
    sc88_board_t *b = (sc88_board_t*)self;
    (void)when; (void)now;
    b->ga_lcd_event = 0;
    sc88b_set_ga_int(b, 0, 0);
    sc88b_set_ga_int(b, 0, 1);
}

static void sc88b_lcd_send_burst(sc88_board_t *b)
{
    b->lcd_staged = 0;
    if (b->ga_lcd_event)
        h8_sched_cancel(&b->m.sched, b->ga_lcd_event);
    b->ga_lcd_event = h8_sched_schedule(&b->m.sched, h8m_now(&b->m) + SC88_LCD_READY_DELAY, sc88b_lcd_ready, b);
}

static uint8_t sc88b_sm_read(sc88_board_t *b, uint16_t addr)
{
    switch (addr)
    {
        case SM_VERSION_HI: return 0x01;
        case SM_VERSION_LO: return 0x23;
        case SM_COMMAND:
            sc88b_request_irq(b, SC88_IRQ_SUB_MCU, 0);
            b->mailbox_full = 0;
            return b->mailbox.command;
        case SM_CHANNEL:   return b->mailbox.channel;
        case SM_PARAM1:    return b->mailbox.param1;
        case SM_PARAM2:    return b->mailbox.param2;
        case SM_SEMAPHORE: return 0x80;
        case SM_PANEL_DATA:
        {
            uint8_t rows = 0xff, col;
            for (col = 0; col < 4; ++col)
                if (b->scan_column & (1u << col))
                    rows &= (uint8_t)~((b->buttons >> (col * 8)) & 0xff);
            return rows;
        }
        default:
            return addr < SM_WINDOW_SIZE ? b->sm_ram[addr] : (uint8_t)0xff;
    }
}

static void sc88b_sm_write(sc88_board_t *b, uint16_t addr, uint8_t val)
{
    if (addr >= SM_WINDOW_SIZE)
        return;
    if (addr == SM_TX_WRITE)
        sm_commit_output(&b->sm, b->sm_ram, val);
    b->sm_ram[addr] = val;
    switch (addr)
    {
        case SM_START:
            sm_start_output(&b->sm, b->sm_ram);
            b->sm_started = 1;
            break;
        case SM_PANEL_DATA:
            b->scan_column = val & 0x0f;
            break;
        case SM_PANEL_CTRL:
            if (b->model != SC88_MODEL_SC88PRO)
                b->panel_ctrl = val;
            break;
        default:
            break;
    }
}

static uint8_t sc88b_ga_read(sc88_board_t *b, uint16_t addr)
{
    if (b->model != SC88_MODEL_SC88PRO && addr < SM_WINDOW_SIZE)
        return sc88b_sm_read(b, addr);
    switch (addr)
    {
        case GA_LEDS:      return b->leds;
        case GA_IRQ_MASK:  return b->ga_irq_mask;
        case GA_IRQ_STATUS:
        {
            const uint8_t status = b->ga_int_trigger;
            b->ga_int_trigger = 0;
            sc88b_request_irq(b, SC88_IRQ_GATE_ARRAY, 0);
            return status;
        }
        case GA_LED_CONTROL:
            if (b->model == SC88_MODEL_SC88PRO)
                return b->led_control;
            return 0xff;
        default:
            return 0xff;
    }
}

static void sc88b_ga_write(sc88_board_t *b, uint16_t addr, uint8_t val)
{
    if (b->model != SC88_MODEL_SC88PRO && addr < SM_WINDOW_SIZE)
    {
        sc88b_sm_write(b, addr, val);
        return;
    }
    switch (addr)
    {
        case GA_LEDS:     b->leds = val; return;
        case GA_IRQ_MASK: b->ga_irq_mask = val; return;
        case GA_LCD_INSTR: b->lcd_instr = val; return;
        case GA_LCD_START: sc88b_lcd_send_burst(b); return;
        case GA_LED_CONTROL:
            if (b->model == SC88_MODEL_SC88PRO)
            {
                b->led_control = val;
                return;
            }
            break;
        default:
            break;
    }
    if (addr >= GA_LCD_DATA && addr <= GA_LCD_DATA_END)
    {
        const uint32_t idx = (uint32_t)(addr - GA_LCD_DATA);
        b->lcd_buffer[idx] = val;
        if (idx >= b->lcd_staged)
            b->lcd_staged = (uint8_t)(idx + 1);
    }
}

/* SC-88: the four external pages */
static uint8_t sc88b_ext_read8(sc88_board_t *b, uint32_t addr)
{
    const uint32_t page = addr >> 16;
    const uint16_t off  = (uint16_t)addr;
    switch (page)
    {
        case 0x0: return off < 0x8000 ? b->rom[off] : b->sram[off];
        case 0x1: case 0x2: case 0x3: case 0x4: case 0x5: case 0x6: case 0x7:
            return b->rom[addr & (SC88_ROM_SIZE - 1)];
        case 0x8: return b->sram[off];
        case 0xe:
            if (off >= 0x4000)
                return b->sram[off];
            return xp_host_read8(&b->xp, off);
        case 0xf: return sc88b_ga_read(b, off);
        default:  return 0xff;
    }
}

static void sc88b_ext_write8(sc88_board_t *b, uint32_t addr, uint8_t val)
{
    const uint32_t page = addr >> 16;
    const uint16_t off  = (uint16_t)addr;
    switch (page)
    {
        case 0x0:
            if (off >= 0x8000)
                b->sram[off] = val;
            return;
        case 0x8:
            b->sram[off] = val;
            return;
        case 0xe:
            if (off >= 0x4000)
            {
                b->sram[off] = val;
                return;
            }
            xp_host_write8(&b->xp, off, val);
            return;
        case 0xf:
            sc88b_ga_write(b, off, val);
            return;
        default:
            return;
    }
}

static uint8_t sc88b_page_read8(void *ctx, uint32_t a)
{
    sc88_page_dev_t *p = (sc88_page_dev_t*)ctx;
    return sc88b_ext_read8(p->board, (p->page << 16) | (a & 0xffff));
}

static void sc88b_page_write8(void *ctx, uint32_t a, uint8_t v)
{
    sc88_page_dev_t *p = (sc88_page_dev_t*)ctx;
    sc88b_ext_write8(p->board, (p->page << 16) | (a & 0xffff), v);
}

static uint8_t sc88b_tail_read8(void *ctx, uint32_t a)
{
    sc88_board_t *b = (sc88_board_t*)ctx;
    return (a & 0xffff) >= 0xff20 ? sc88b_ext_read8(b, a & 0xffff) : (uint8_t)0xff;
}

static void sc88b_tail_write8(void *ctx, uint32_t a, uint8_t v)
{
    sc88_board_t *b = (sc88_board_t*)ctx;
    if ((a & 0xffff) >= 0xff20)
        sc88b_ext_write8(b, a & 0xffff, v);
}

/* SC-88Pro: one device over the board's address map */
static uint8_t sc88pro_ext_read8(void *ctx, uint32_t addr)
{
    sc88_board_t *b = (sc88_board_t*)ctx;
    const uint8_t  page = (uint8_t)(addr >> 16);
    const uint16_t off  = (uint16_t)addr;
    if (page <= 0x7F)
        return b->rom[addr & (SC88PRO_ROM_SIZE - 1)];
    if (page >= 0xC0 && page <= 0xC7)
        return b->sram[off];
    if (page == 0xC8)
        return xp_host_read8(&b->xp, off);
    if (page >= 0xE0 && page <= 0xE7)
        return sc88b_sm_read(b, off);
    if (page == 0xEF)
        return sc88b_ga_read(b, off);
    if (page == 0xF0)
        return lsp_host_read(b->lsp, off);
    return 0xff;
}

static void sc88pro_ext_write8(void *ctx, uint32_t addr, uint8_t val)
{
    sc88_board_t *b = (sc88_board_t*)ctx;
    const uint8_t  page = (uint8_t)(addr >> 16);
    const uint16_t off  = (uint16_t)addr;
    if (page >= 0xC0 && page <= 0xC7)
        b->sram[off] = val;
    else if (page == 0xC8)
        xp_host_write8(&b->xp, off, val);
    else if (page >= 0xE0 && page <= 0xE7)
        sc88b_sm_write(b, off, val);
    else if (page == 0xEF)
        sc88b_ga_write(b, off, val);
    else if (page == 0xF0)
        lsp_host_write(b->lsp, off, val);
}

static uint8_t sc88b_port_read(sc88_board_t *b, uint32_t addr, uint8_t value)
{
    if (b->model == SC88_MODEL_SC88PRO)
    {
        switch (addr)
        {
            case 0xfe86: return b->p3dr;
            case 0xfe87: return 0x00;
            case 0xfe8a: return SC88_P5DR_STRAPS;
            case 0xfe8e: return 0xf3;
            case 0xfe8f: return b->xp.state.interrupt ? (uint8_t)0xfd : (uint8_t)0xff;
            default:     return 0xff;
        }
    }
    switch (addr)
    {
        case 0xfe86: return 0x00;
        case 0xfe87: return 0x00;
        case 0xfe8a: return b->p5dr;
        case 0xfe8f:
            return b->xp.state.interrupt ? (uint8_t)(value & ~0x02) : (uint8_t)(value | 0x02);
        default:
            return value;
    }
}

static void sc88b_port_write(sc88_board_t *b, uint32_t addr, uint8_t val)
{
    if (b->model == SC88_MODEL_SC88PRO)
    {
        if (addr == 0xfe86)
        {
            b->p3dr = val;
            b->lsp_return_enabled = (val & 0x80) != 0;
        }
        else if (addr == 0xfe8b)
            b->lcd_enabled = (val & 1) != 0;
        return;
    }
    if (addr == 0xfe8b && b->model == SC88_MODEL_SC88VL)
        b->lcd_enabled = (val & 1) != 0;
}

static uint8_t sc88b_ports_read_hook(void *ctx, unsigned port, uint8_t value)
{
    sc88_board_t *b = (sc88_board_t*)ctx;
    return sc88b_port_read(b, 0xfe80u + sc88_port_dr_offset[port], value);
}

static void sc88b_ports_write_hook(void *ctx, unsigned port, uint8_t dr, uint8_t ddr)
{
    sc88_board_t *b = (sc88_board_t*)ctx;
    (void)ddr;
    b->m.ports.p[H8_PORT_IDX(port)].pins = dr;
    sc88b_port_write(b, 0xfe80u + sc88_port_dr_offset[port], dr);
}

static uint16_t sc88b_sampler(void *ctx, unsigned channel)
{
    sc88_board_t *b = (sc88_board_t*)ctx;
    if (b->model == SC88_MODEL_SC88PRO)
        return channel == 0 ? (uint16_t)SC88_ANALOG_BATTERY : (uint16_t)0;
    return channel < 4 ? b->analog[channel] : (uint16_t)0;
}

static void sc88b_xp_irq(void *ctx, int level)
{
    sc88b_request_irq((sc88_board_t*)ctx, SC88_IRQ_XP, level);
}

static void sc88b_wire_chip(sc88_board_t *b)
{
    h8_bus_t *bus = &b->m.bus;
    if (b->model == SC88_MODEL_SC88PRO)
    {
        h8_bus_map(bus, 0x00000, 0xfe80, H8_LINE_ROM, NULL, H8_W16_S2, 0);
        h8_bus_load(bus, 0x00000, b->rom, 0xfe80);
        h8_bus_map(bus, 0x10000, SC88PRO_ROM_SIZE - 0x10000, H8_LINE_ROM, NULL, H8_W16_S2, 0);
        h8_bus_load(bus, 0x10000, b->rom + 0x10000, SC88PRO_ROM_SIZE - 0x10000);
        b->board_bus.read8  = sc88pro_ext_read8;
        b->board_bus.write8 = sc88pro_ext_write8;
        b->board_bus.ctx    = b;
        h8_bus_map(bus, 0x100000, 0x800000 - 0x100000, H8_LINE_DEV, &b->board_bus, H8_W16_S2, 0);
        h8_bus_map(bus, 0xc00000, 0x090000, H8_LINE_DEV, &b->board_bus, H8_W16_S2, 0);
        h8_bus_map(bus, 0xe00000, 0x080000, H8_LINE_DEV, &b->board_bus, H8_W16_S3, 0);
        h8_bus_map(bus, 0xef0000, 0x020000, H8_LINE_DEV, &b->board_bus, H8_W16_S3, 0);
    }
    else
    {
        static const uint32_t page_no[4] = { 0x0, 0x8, 0xe, 0xf };
        unsigned i;
        for (i = 0; i < 4; ++i)
        {
            b->pages[i].board = b;
            b->pages[i].page  = page_no[i];
            b->pages[i].dev.read8  = sc88b_page_read8;
            b->pages[i].dev.write8 = sc88b_page_write8;
            b->pages[i].dev.ctx    = &b->pages[i];
        }
        h8_bus_map(bus, 0x00000, 0x8000, H8_LINE_ROM, NULL, H8_W16_S2, 0);
        h8_bus_load(bus, 0x00000, b->rom, 0x8000);
        h8_bus_map(bus, 0x10000, SC88_ROM_SIZE - 0x10000, H8_LINE_ROM, NULL, H8_W16_S2, 0);
        h8_bus_load(bus, 0x10000, b->rom + 0x10000, SC88_ROM_SIZE - 0x10000);
        h8_bus_map(bus, 0x08000, 0xfe80 - 0x8000, H8_LINE_DEV, &b->pages[0].dev, H8_W16_S2, 0);
        h8_bus_map(bus, 0x80000, 0x10000, H8_LINE_DEV, &b->pages[1].dev, H8_W16_S2, 0);
        h8_bus_map(bus, 0xe0000, 0x10000, H8_LINE_DEV, &b->pages[2].dev, H8_W16_S3, 0);
        h8_bus_map(bus, 0xf0000, 0x10000, H8_LINE_DEV, &b->pages[3].dev, H8_W16_S3, 0);
        b->regfield_tail.read8  = sc88b_tail_read8;
        b->regfield_tail.write8 = sc88b_tail_write8;
        b->regfield_tail.ctx    = b;
        b->m.io.fallback = &b->regfield_tail;
    }
    h8_invalidate_all(&b->m.cpu);
    b->m.ports.read_hook  = sc88b_ports_read_hook;
    b->m.ports.write_hook = sc88b_ports_write_hook;
    b->m.ports.hook_ctx   = b;
    b->m.adc.sampler      = sc88b_sampler;
    b->m.adc.sampler_ctx  = b;
    /* serial MIDI out is not needed: nothing listens */
}

static void sc88b_power_cycle(sc88_board_t *b)
{
    xp_reset(&b->xp);
    h8m_reset(&b->m);
    b->lcd_enabled = 1;
    if (b->lsp)
        lsp_clear(b->lsp);
    b->samples_rendered = 0;
    b->cycle_target = h8m_now(&b->m);
    b->cycle_frac   = 0;
    b->buttons      = 0;
    b->scan_column  = 0;
    b->panel_ctrl   = 0;
    b->leds         = 0;
    b->led_control  = 0;
    memset(b->ga_int, 0, sizeof(b->ga_int));
    b->ga_irq_mask    = 0x0f;
    b->ga_int_trigger = 0;
    b->lcd_instr  = 0;
    b->lcd_staged = 0;
    memset(b->lcd_buffer, 0, sizeof(b->lcd_buffer));
    if (b->ga_lcd_event)
    {
        h8_sched_cancel(&b->m.sched, b->ga_lcd_event);
        b->ga_lcd_event = 0;
    }
    memset(b->sm_ram, 0, sizeof(b->sm_ram));
    b->sm_started = 0;
    sm_reset(&b->sm);
    b->q_head = b->q_count = 0;
    memset(&b->mailbox, 0, sizeof(b->mailbox));
    b->mailbox.wireBytes = 3;
    b->mailbox_full    = 0;
    b->midi_wire_delay = 0;
    b->midi_wire_frac  = 0;
    b->p3dr = 0;
    b->lsp_return_enabled = 0;
}

static void sc88b_pump_midi_in(sc88_board_t *b)
{
    const sm_record_t *r;
    size_t i;
    if (!b->sm_started || b->samples_rendered < SC88_POWER_ON_MIDI_DELAY)
        return;
    if (b->midi_wire_delay)
    {
        --b->midi_wire_delay;
        return;
    }
    if (b->mailbox_full || b->q_count == 0)
        return;
    r = &b->q[b->q_head];
    if (b->model == SC88_MODEL_SC88PRO)
    {
        uint32_t samples;
        for (i = 0; i < r->payload_len && SM_SYSEX_STAGE + i < SM_WINDOW_SIZE; ++i)
            b->sm_ram[SM_SYSEX_STAGE + i] = r->payload[i];
        b->mailbox = *r;
        b->mailbox_full = 1;
        samples = (uint32_t)r->wireBytes * SC88_MIDI_BITS_PER_BYTE * SC88_SAMPLE_RATE;
        b->midi_wire_delay = samples / SC88_MIDI_BAUD;
        b->midi_wire_frac += samples % SC88_MIDI_BAUD;
        if (b->midi_wire_frac >= SC88_MIDI_BAUD)
        {
            b->midi_wire_frac -= SC88_MIDI_BAUD;
            ++b->midi_wire_delay;
        }
        b->q_head = (b->q_head + 1) % b->q_cap;
        b->q_count--;
        sc88b_request_irq(b, SC88_IRQ_SUB_MCU, 1);
    }
    else
    {
        uint32_t units;
        b->mailbox = *r;
        b->q_head = (b->q_head + 1) % b->q_cap;
        b->q_count--;
        for (i = 0; i < b->mailbox.payload_len; ++i)
        {
            const size_t at = (size_t)b->mailbox.param2 + i;
            if (at < SM_WINDOW_SIZE)
                b->sm_ram[at] = b->mailbox.payload[i];
        }
        b->mailbox_full = 1;
        sc88b_request_irq(b, SC88_IRQ_SUB_MCU, 1);
        units = (uint32_t)b->mailbox.wireBytes * SC88_SAMPLE_RATE * SC88_MIDI_BITS_PER_BYTE + b->midi_wire_frac;
        b->midi_wire_delay = units / SC88_MIDI_BAUD;
        b->midi_wire_frac  = units % SC88_MIDI_BAUD;
    }
}

/* One sample: run the CPU up to it, then the sound chip and effects. */
static void sc88b_render_sample(sc88_board_t *b, int32_t *out_l, int32_t *out_r)
{
    xp_dsp_state_t *d;
    *out_l = *out_r = 0;
    if (!b->valid)
        return;
    ++b->samples_rendered;
    sm_clock_output(&b->sm, b->sm_ram, SC88_SAMPLE_RATE);
    sc88b_pump_midi_in(b);
    b->cycle_target += SC88_CPU_CLOCK_HZ / SC88_SAMPLE_RATE;
    b->cycle_frac   += SC88_CPU_CLOCK_HZ % SC88_SAMPLE_RATE;
    if (b->cycle_frac >= SC88_SAMPLE_RATE)
    {
        b->cycle_frac -= SC88_SAMPLE_RATE;
        ++b->cycle_target;
    }
    if (b->cycle_target > h8m_now(&b->m))
        h8m_run(&b->m, b->cycle_target - h8m_now(&b->m));
    if (!b->xp_enabled)
        return;
    xp_step(&b->xp);
    d = &b->xp.state.dsp.s;
    if (d->serialOutputCount[2] >= 2)
    {
        *out_l = d->serialOutput[2][0];
        *out_r = d->serialOutput[2][1];
    }
    if (b->model == SC88_MODEL_SC88PRO)
    {
        int32_t returns[2];
        returns[0] = returns[1] = 0;
        if (b->lsp_enabled)
        {
            const uint32_t sends = d->serialOutputCount[1];
            int32_t lo, ro;
            lsp_process(b->lsp, sends > 0 ? d->serialOutput[1][0] : 0, sends > 1 ? d->serialOutput[1][1] : 0, &lo, &ro);
            if (b->lsp_return_enabled)
            {
                returns[0] = lo >> 8;
                returns[1] = ro >> 8;
            }
        }
        xp_dsp_set_serial_input(&b->xp.state.dsp, 0, returns, 2);
    }
}

/* MIDI in: one short message (status + data) or one SysEx. */

static void sc88b_run_samples(sc88_board_t *b, uint32_t n)
{
    int32_t l, r;
    while (n-- > 0)
        sc88b_render_sample(b, &l, &r);
}

/* What the firmware does when SELECT + INST L/R then ALL is pressed: put
 * its settings to factory defaults.  Run once at power-on, as the C++
 * does by default, so that settings left in SRAM do not matter. */
static void sc88b_factory_reset(sc88_board_t *b)
{
    const int pro = b->model == SC88_MODEL_SC88PRO;
    const uint32_t boot_wait      = 8 * SC88_SAMPLE_RATE;
    const uint32_t modifier_lead  = pro ? SC88_SAMPLE_RATE / 20 : SC88_SAMPLE_RATE / 5;
    const uint32_t chord_hold     = pro ? SC88_SAMPLE_RATE / 10 : SC88_SAMPLE_RATE / 2;
    const uint32_t key_gap        = SC88_SAMPLE_RATE / 4;
    const uint32_t execute_hold   = SC88_SAMPLE_RATE / 10;
    const uint32_t execute_settle = 2 * SC88_SAMPLE_RATE;
    const uint32_t select = 1u << 25, inst_l = 1u << 3, inst_r = 1u << 4, inst_all = 1u << 6;
    const int xp_was = b->xp_enabled, lsp_was = b->lsp_enabled;
    int attempt;
    if (!b->valid || (pro && b->serial_midi))
        return;
    b->xp_enabled  = 0;
    b->lsp_enabled = 0;
    sc88b_run_samples(b, boot_wait);
    b->buttons = select;
    sc88b_run_samples(b, modifier_lead);
    b->buttons = select | inst_l | inst_r;
    sc88b_run_samples(b, chord_hold);
    b->buttons = select;
    sc88b_run_samples(b, modifier_lead);
    b->buttons = 0;
    sc88b_run_samples(b, key_gap);
    b->buttons = inst_all;
    sc88b_run_samples(b, execute_hold);
    b->buttons = 0;
    sc88b_run_samples(b, execute_settle);
    if (pro)
        for (attempt = 0; attempt < 3 && (b->leds & ((1u << 2) | (1u << 3))); ++attempt)
        {
            b->buttons = 1u << 2;   /* SC-55 MAP */
            sc88b_run_samples(b, chord_hold);
            b->buttons = 0;
            sc88b_run_samples(b, key_gap);
        }
    b->xp_enabled  = xp_was;
    b->lsp_enabled = lsp_was;
    sc88b_power_cycle(b);
}

static void sc88b_free(sc88_board_t *b)
{
    h8m_free(&b->m);
    free(b->lsp);
    free(b->rom);
    free(b->sram);
    free(b->wave);
    free(b->q);
    free(b->sm.out);
    memset(b, 0, sizeof(*b));
}

/* Build a board from its firmware and wave ROM, which are copied.  The
 * board must not move in memory afterwards: its parts point at each
 * other.  Returns 0 if it could not be built at all; a board whose
 * firmware is the wrong size is built but not valid, and stays silent. */
static int sc88b_init(sc88_board_t *b, int model, const uint8_t *firmware, size_t firmware_size,
                      const uint8_t *wave, size_t wave_size, int factory_reset)
{
    const int pro = model == SC88_MODEL_SC88PRO;
    const size_t rom_size = pro ? SC88PRO_ROM_SIZE : SC88_ROM_SIZE;
    size_t cs;

    memset(b, 0, sizeof(*b));
    b->model       = model;
    b->xp_enabled  = 1;
    b->lsp_enabled = 1;
    b->lcd_enabled = 1;
    b->p5dr        = SC88_P5DR_STRAPS;
    b->ga_irq_mask = 0x0f;
    b->mailbox.wireBytes = 3;
    if (!h8m_init(&b->m, 4))
        return 0;
    xp_init(&b->xp);
    b->sm.board = b;
    if (pro)
    {
        b->sm.stageOffset   = SM_SYSEX_STAGE;
        b->sm.stageCapacity = SM_COMMAND - SM_SYSEX_STAGE;
        b->lsp = (lsp_t*)malloc(sizeof(lsp_t));
        if (!b->lsp)
            return 0;
        lsp_init(b->lsp);
    }
    else
    {
        b->sm.stageOffset   = 0x10;
        b->sm.stageCapacity = 0x80 - 0x10;
    }
    b->sram = (uint8_t*)calloc(SC88_SRAM_SIZE, 1);
    if (!b->sram)
        return 0;
    if (firmware_size != rom_size)
        return 1;
    b->rom = (uint8_t*)malloc(rom_size);
    if (!b->rom)
        return 0;
    memcpy(b->rom, firmware, rom_size);
    b->rom_size = rom_size;

    if (wave_size)
    {
        b->wave = (uint8_t*)malloc(wave_size);
        if (!b->wave)
            return 0;
        if (pro && wave_size == SC88PRO_WAVE_SIZE)
            sc88_pcm16_descramble(wave, wave_size, b->wave);
        else
            memcpy(b->wave, wave, wave_size);
        b->wave_size = wave_size;
    }
    if (pro)
    {
        const size_t window = 0x400000;
        for (cs = 0; cs < 5; ++cs)
        {
            const size_t offset = cs * window;
            if (offset >= b->wave_size)
                break;
            xp_map_wave_rom(&b->xp, cs, b->wave + offset,
                            b->wave_size - offset < window ? b->wave_size - offset : window, XP_ROM_BITS16, 0, 0);
        }
    }
    else
    {
        const size_t chip = 0x200000;
        size_t chips = b->wave_size / chip;
        if (chips > XP_WAVE_CHIP_SELECTS)
            chips = XP_WAVE_CHIP_SELECTS;
        for (cs = 0; cs < chips; ++cs)
            xp_map_wave_rom(&b->xp, cs, b->wave + cs * chip, chip, XP_ROM_BITS16, 0, 0);
    }

    sc88b_wire_chip(b);
    b->xp.interruptCallback = sc88b_xp_irq;
    b->xp.interruptUser     = b;
    if (pro)
    {
        /* firmware built for serial MIDI points its SCI vector at the
         * invalid-instruction handler's */
        const uint8_t *v37 = b->rom + 37 * 4, *v2 = b->rom + 2 * 4;
        b->serial_midi = v37[1] == v2[1] && v37[2] == v2[2] && v37[3] == v2[3];
    }
    else
        b->analog[0] = SC88_ANALOG_BATTERY;
    b->valid = 1;
    sc88b_power_cycle(b);
    if (factory_reset)
        sc88b_factory_reset(b);
    return 1;
}

/* ---- public API (sc88.h) ---------------------------------------------- */

struct sc88
{
    int           model;
    int           built;
    unsigned char *rom[SC88_ROM_COUNT];
    size_t        rom_len[SC88_ROM_COUNT];
    sc88_board_t  board;
    /* power-on factory reset, run as part of the first frames */
    int           fr_stage;          /* -1: done */
    uint32_t      fr_left;
    int           fr_attempts;
    int           fr_xp, fr_lsp;
    unsigned char *pending;          /* MIDI sent during the factory reset */
    size_t        pending_len, pending_cap;
};

/* A dump of the control ROM may have its 16-bit words byte-swapped; the
 * vector table at the start tells which way round it is.  This is what
 * the ROM table's digests are taken of. */
void sc88_normalize_firmware(unsigned char *data, size_t len)
{
    const int vectors = 60;
    int score_as_is = 0, score_swapped = 0, v;
    size_t i;
    if (len < (size_t)vectors * 4)
        return;
    for (v = 0; v < vectors; ++v)
    {
        const unsigned char *p = data + v * 4;
        unsigned char q[4];
        q[0] = p[1]; q[1] = p[0]; q[2] = p[3]; q[3] = p[2];
        if (!(p[0] == 0xff && p[1] == 0xff && p[2] == 0xff && p[3] == 0xff)
            && p[0] == 0x00 && p[1] <= 0x0f && p[2] <= 0x0f)
            ++score_as_is;
        if (!(q[0] == 0xff && q[1] == 0xff && q[2] == 0xff && q[3] == 0xff)
            && q[0] == 0x00 && q[1] <= 0x0f && q[2] <= 0x0f)
            ++score_swapped;
    }
    if (score_swapped <= score_as_is)
        return;
    for (i = 0; i + 1 < len; i += 2)
    {
        const unsigned char t = data[i];
        data[i]     = data[i + 1];
        data[i + 1] = t;
    }
}

static size_t sc88_rom_size(int model, int slot)
{
    if (model == SC88_MODEL_SC88PRO)
    {
        switch (slot)
        {
            case SC88_ROM_FIRMWARE: return 0x100000;
            case SC88_ROM_WAVE0:    return 0x800000;
            case SC88_ROM_WAVE1:    return 0x800000;
            case SC88_ROM_WAVE2:    return 0x400000;
            default:                return 0;
        }
    }
    return slot == SC88_ROM_FIRMWARE ? 0x80000 : 0x200000;
}

sc88_t *sc88_new(int model)
{
    sc88_t *s;
    if (model < 0 || model >= SC88_MODEL_COUNT)
        return NULL;
    s = (sc88_t*)calloc(1, sizeof(sc88_t));
    if (s)
    {
        s->model    = model;
        s->fr_stage = -1;
    }
    return s;
}

void sc88_free(sc88_t *s)
{
    int i;
    if (!s)
        return;
    if (s->built)
        sc88b_free(&s->board);
    for (i = 0; i < SC88_ROM_COUNT; ++i)
        free(s->rom[i]);
    free(s->pending);
    free(s);
}

int sc88_load_rom(sc88_t *s, int slot, const unsigned char *data, size_t len)
{
    unsigned char *copy;
    if (!s || s->built || slot < 0 || slot >= SC88_ROM_COUNT || len != sc88_rom_size(s->model, slot) || !len)
        return 0;
    copy = (unsigned char*)malloc(len);
    if (!copy)
        return 0;
    memcpy(copy, data, len);
    if (slot == SC88_ROM_FIRMWARE)
        sc88_normalize_firmware(copy, len);
    free(s->rom[slot]);
    s->rom[slot]     = copy;
    s->rom_len[slot] = len;
    return 1;
}

/* The factory reset: SELECT, then SELECT + INST L + INST R, then ALL,
 * with the timing the C++ device uses; the SC-88Pro may also need its
 * map switched back to SC-88 afterwards. */
typedef struct { uint32_t buttons; uint32_t samples; } sc88_fr_step_t;

static void sc88_fr_steps(int pro, sc88_fr_step_t *st)
{
    const uint32_t rate = SC88_SAMPLE_RATE;
    const uint32_t select = 1u << 25, inst_l = 1u << 3, inst_r = 1u << 4, inst_all = 1u << 6;
    st[0].buttons = 0;                         st[0].samples = 8 * rate;
    st[1].buttons = select;                    st[1].samples = pro ? rate / 20 : rate / 5;
    st[2].buttons = select | inst_l | inst_r;  st[2].samples = pro ? rate / 10 : rate / 2;
    st[3].buttons = select;                    st[3].samples = pro ? rate / 20 : rate / 5;
    st[4].buttons = 0;                         st[4].samples = rate / 4;
    st[5].buttons = inst_all;                  st[5].samples = rate / 10;
    st[6].buttons = 0;                         st[6].samples = 2 * rate;
    /* SC-88Pro, while its map LEDs say SC-55: press SC-55 MAP, wait */
    st[7].buttons = 1u << 2;                   st[7].samples = rate / 10;
    st[8].buttons = 0;                         st[8].samples = rate / 4;
}

#define SC88_FR_STEPS 9

static void sc88_fr_enter(sc88_t *s, int stage)
{
    sc88_fr_step_t st[SC88_FR_STEPS];
    sc88_fr_steps(s->model == SC88_MODEL_SC88PRO, st);
    s->fr_stage = stage;
    s->fr_left  = st[stage].samples;
    s->board.buttons = st[stage].buttons;
}

static void sc88_fr_finish(sc88_t *s)
{
    sc88_board_t *b = &s->board;
    size_t i;
    b->xp_enabled  = s->fr_xp;
    b->lsp_enabled = s->fr_lsp;
    sc88b_power_cycle(b);
    s->fr_stage = -1;
    for (i = 0; i < s->pending_len; ++i)
        sm_midi_in(&b->sm, 0, s->pending[i]);
    s->pending_len = 0;
}

/* After the last step of a stage: where to go next. */
static void sc88_fr_next(sc88_t *s)
{
    const int pro = s->model == SC88_MODEL_SC88PRO;
    const unsigned map_leds = (1u << 2) | (1u << 3);
    int next = s->fr_stage + 1;
    if (next == 7 || next == 9)
    {
        /* after the reset itself, and after each SC-55 MAP press */
        if (next == 9)
            ++s->fr_attempts;
        if (!pro || s->fr_attempts >= 3 || !(s->board.leds & map_leds))
        {
            sc88_fr_finish(s);
            return;
        }
        next = 7;
    }
    sc88_fr_enter(s, next);
}

void sc88_reset(sc88_t *s)
{
    sc88_board_t *b;
    unsigned char *fw, *wave = NULL;
    size_t wave_len = 0, k;
    int ok;
    if (!s || s->built)
        return;
    fw = s->rom[SC88_ROM_FIRMWARE];
    if (!fw)
        return;
    if (s->model == SC88_MODEL_SC88PRO)
    {
        /* the board takes the three parts as one image and descrambles it */
        if (s->rom[SC88_ROM_WAVE0] && s->rom[SC88_ROM_WAVE1] && s->rom[SC88_ROM_WAVE2])
        {
            wave_len = s->rom_len[SC88_ROM_WAVE0] + s->rom_len[SC88_ROM_WAVE1] + s->rom_len[SC88_ROM_WAVE2];
            wave = (unsigned char*)malloc(wave_len);
            if (wave)
            {
                memcpy(wave, s->rom[SC88_ROM_WAVE0], s->rom_len[SC88_ROM_WAVE0]);
                memcpy(wave + s->rom_len[SC88_ROM_WAVE0], s->rom[SC88_ROM_WAVE1], s->rom_len[SC88_ROM_WAVE1]);
                memcpy(wave + s->rom_len[SC88_ROM_WAVE0] + s->rom_len[SC88_ROM_WAVE1],
                       s->rom[SC88_ROM_WAVE2], s->rom_len[SC88_ROM_WAVE2]);
            }
            else
                wave_len = 0;
        }
    }
    else
    {
        /* four 2 MB chips, each descrambled, in bank order */
        wave_len = 4 * (size_t)0x200000;
        wave = (unsigned char*)malloc(wave_len);
        if (wave)
            for (k = 0; k < 4; ++k)
            {
                if (s->rom[SC88_ROM_WAVE0 + k])
                    sc88_pcm16_descramble(s->rom[SC88_ROM_WAVE0 + k], 0x200000, wave + k * 0x200000);
                else
                    memset(wave + k * 0x200000, 0, 0x200000);
            }
        else
            wave_len = 0;
    }
    ok = sc88b_init(&s->board, s->model, fw, s->rom_len[SC88_ROM_FIRMWARE], wave, wave_len, 0);
    free(wave);
    for (k = 0; k < SC88_ROM_COUNT; ++k)
    {
        free(s->rom[k]);
        s->rom[k] = NULL;
    }
    if (!ok)
    {
        sc88b_free(&s->board);
        return;
    }
    s->built = 1;
    b = &s->board;
    /* the VE-GS Pro firmware takes MIDI on the serial port and has no panel */
    if (b->valid && !(s->model == SC88_MODEL_SC88PRO && b->serial_midi))
    {
        s->fr_xp  = b->xp_enabled;
        s->fr_lsp = b->lsp_enabled;
        b->xp_enabled  = 0;
        b->lsp_enabled = 0;
        s->fr_attempts = 0;
        sc88_fr_enter(s, 0);
    }
}

void sc88_midi(sc88_t *s, const unsigned char *data, size_t len)
{
    sc88_board_t *b;
    size_t i;
    if (!s || !s->built)
        return;
    b = &s->board;
    if (s->fr_stage >= 0)
    {
        if (s->pending_len + len > s->pending_cap)
        {
            size_t cap = s->pending_cap ? s->pending_cap : 256;
            unsigned char *p;
            while (cap < s->pending_len + len)
                cap *= 2;
            p = (unsigned char*)realloc(s->pending, cap);
            if (!p)
                return;
            s->pending     = p;
            s->pending_cap = cap;
        }
        memcpy(s->pending + s->pending_len, data, len);
        s->pending_len += len;
        return;
    }
    for (i = 0; i < len; ++i)
    {
        if (b->model == SC88_MODEL_SC88PRO && b->serial_midi)
            h8_sci_receive_byte(&b->m.sci[0], data[i], 0, 0);
        else
            sm_midi_in(&b->sm, 0, data[i]);
    }
}

uint32_t sc88_voices(const sc88_t *s)
{
    uint32_t mask = 0;
    unsigned i;
    if (!s || !s->built || s->fr_stage >= 0)
        return 0;
    for (i = 0; i < XP_MAX_VOICES; ++i)
    {
        const xp_voice_t *v = &s->board.xp.state.voices[i];
        if (v->resetState_3900.released && v->runtimeCache.runtimePhase == XP_PHASE_RUNNING)
            mask |= (uint32_t)1 << (i < 31 ? i : 31);
    }
    return mask;
}

unsigned sc88_rate(const sc88_t *s)
{
    (void)s;
    return SC88_SAMPLE_RATE;
}

/* Frames are stereo pairs of 24-bit words: full scale is 2^23. */
size_t sc88_run(sc88_t *s, int32_t *frames, size_t count)
{
    size_t n;
    if (!s || !s->built)
    {
        memset(frames, 0, count * 2 * sizeof(int32_t));
        return count;
    }
    for (n = 0; n < count; ++n)
    {
        int32_t l, r;
        sc88b_render_sample(&s->board, &l, &r);
        frames[n * 2]     = l;
        frames[n * 2 + 1] = r;
        if (s->fr_stage >= 0 && --s->fr_left == 0)
            sc88_fr_next(s);
    }
    return count;
}

#ifdef SC88_TEST
/* For the tests: non-zero while the power-on factory reset runs. */
int sc88_test_booting(const sc88_t *s)
{
    return s && s->built && s->fr_stage >= 0;
}
#endif
