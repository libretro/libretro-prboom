// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// Copyright(C) 2009 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
// 02111-1307, USA.
//
// DESCRIPTION:
//     OPL interface.
//
//-----------------------------------------------------------------------------


#include "config.h"

#include <stdlib.h>


#include "opl.h"
#include "opl_queue.h"
#include "dbopl.h"

#include "i_sound.h" // mus_opl_gain

#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int init_stage_reg_writes = 1;

unsigned int opl_sample_rate = 22050;

/* The OPL chip is emulated at its true hardware rate (OPLRATE = 14.318 MHz
 * / 288 ~= 49716 Hz) so that envelopes, the LFO, vibrato/tremolo and phase
 * accumulators advance exactly per-sample as they do on real silicon, then
 * the mono chip output is resampled once to the frontend rate.  The old
 * behaviour rendered the chip directly at the output rate, which scaled the
 * chip's increments by 49716/rate (only approximately correct) and, at
 * 48 kHz, folded the chip's top octave -- it produces content up to ~24.86
 * kHz -- back down into the audible band.  opl_sample_rate now always holds
 * the native rate (it drives Chip__Setup and the ms/tics->sample timer math,
 * both of which want the hardware rate); opl_output_rate holds the frontend
 * rate used only by the output resampler below. */
#define OPL_NATIVE_RATE 49716u
static unsigned int opl_output_rate = 22050;

/* 16.16 polyphase resampler: native-rate mono chip output -> output stereo.
 * State persists across OPL_Render_Samples calls so the stream is seamless.
 *
 * The native buffer holds the raw 32-bit chip sum straight from dbopl -- no
 * gain, no clamp.  The previous int16 buffer applied mus_opl_gain with a
 * truncating integer divide and hard-clipped hot passages per-sample at the
 * native rate, i.e. it quantized and clipped *before* the FIR, whose
 * accumulator is float anyway.  Gain is now applied in float to the filtered
 * sample and the result is quantized exactly once, at emission (int16 or
 * float per the output lane). */
#define OPL_NSRC_FRAMES 4096
static int32_t      opl_nsrc[OPL_NSRC_FRAMES];  /* native-rate mono source     */
static int          opl_nsrc_have = 0;          /* native frames buffered      */
static int          opl_nsrc_pos  = 0;          /* next unconsumed native frame */
static unsigned     opl_rs_frac   = 0;          /* 16.16 fractional read cursor */
static unsigned     opl_rs_step   = 1u << 16;   /* 16.16 native frames/out frame */

/* Windowed-sinc polyphase reconstruction filter for the resampler.  A plain
 * linear interpolator only gently lowpasses, so when downsampling (output <
 * native, e.g. 49716->44100 or ->32000) the chip's content above the output
 * Nyquist folds back in.  This is a Kaiser-windowed sinc with the cutoff
 * tracked to the output Nyquist on downsample (fc = output/native) and at the
 * native Nyquist otherwise, evaluated at OPL_RS_NP fractional sub-phases.  At
 * unity (output == native) the phase-0 row is a unit impulse, so the path
 * stays a bit-exact passthrough. */
#define OPL_RS_NZ     16                      /* sinc zero crossings per side  */
#define OPL_RS_NTAPS  (2 * OPL_RS_NZ)         /* taps per output sample (32)   */
#define OPL_RS_NP     512                     /* polyphase sub-phases          */
#define OPL_RS_PSHIFT 7                        /* 16-bit frac -> 9-bit phase idx */
#define OPL_RS_BETA   7.0                      /* Kaiser window beta            */
static float        opl_rs_tab[OPL_RS_NP][OPL_RS_NTAPS];


#define MAX_SOUND_SLICE_TIME 100 /* ms */

typedef struct
{
    unsigned int rate;        // Number of times the timer is advanced per sec.
    unsigned int enabled;     // Non-zero if timer is enabled.
    unsigned int value;       // Last value that was set.
    unsigned int expire_time; // Calculated time that timer will expire.
} opl_timer_t;


// Queue of callbacks waiting to be invoked.

static opl_callback_queue_t *callback_queue;


// Current time, in number of samples since startup:

static unsigned int current_time;

// If non-zero, playback is currently paused.

static int opl_paused;

// Time offset (in samples) due to the fact that callbacks
// were previously paused.

static unsigned int pause_offset;

// OPL software emulator structure.

static Chip opl_chip;

// Register number that was written.

static int register_num = 0;

// Timers; DBOPL does not do timer stuff itself.

static opl_timer_t timer1 = { 12500, 0, 0, 0 };
static opl_timer_t timer2 = { 3125, 0, 0, 0 };


//
// Init/shutdown code.
//

/* Modified Bessel function of the first kind, order 0 (for the Kaiser
 * window).  Series converges quickly for the small arguments used here. */
static double opl_i0(double z)
{
    double sum = 1.0, term = 1.0, k = 1.0;
    double zz = z * z / 4.0;
    do
    {
        term *= zz / (k * k);
        sum  += term;
        k    += 1.0;
    } while (term > 1e-12 * sum && k < 200.0);
    return sum;
}

/* Build the polyphase Kaiser-windowed-sinc table for the current resample
 * ratio.  fc is the cutoff as a fraction of the native Nyquist: 1.0 for
 * unity/upsample, output/native (< 1) for downsample so images above the
 * output Nyquist are rejected.  A plain linear interpolator only gently
 * lowpasses, so an aggressive downsample (e.g. 49716->32000) folds the chip's
 * upper content audibly; this filter pushes that alias well below -50 dB
 * while keeping a flat passband.  Each phase row is normalized to unity DC
 * gain, and the phase-0 row is a unit impulse at unity ratio, so the path
 * stays a bit-exact passthrough when output == native. */
static void opl_build_resampler_table(double fc)
{
    double i0b = opl_i0(OPL_RS_BETA);
    int p, t;

    for (p = 0; p < OPL_RS_NP; p++)
    {
        double f   = (double)p / (double)OPL_RS_NP;  /* fractional delay [0,1) */
        double sum = 0.0;

        for (t = 0; t < OPL_RS_NTAPS; t++)
        {
            double n  = (double)(t - (OPL_RS_NZ - 1)); /* tap offset [-(NZ-1),NZ] */
            double x  = f - n;                         /* sinc argument           */
            double wx = x / (double)OPL_RS_NZ;
            double w, h;

            if (wx <= -1.0 || wx >= 1.0)
                w = 0.0;
            else
                w = opl_i0(OPL_RS_BETA * sqrt(1.0 - wx * wx)) / i0b; /* Kaiser */

            if (x == 0.0)
                h = fc;
            else
            {
                double a = M_PI * fc * x;
                h = fc * sin(a) / a;
            }

            opl_rs_tab[p][t] = (float)(w * h);
            sum += w * h;
        }

        if (sum != 0.0)
        {
            double inv = 1.0 / sum;
            for (t = 0; t < OPL_RS_NTAPS; t++)
                opl_rs_tab[p][t] = (float)((double)opl_rs_tab[p][t] * inv);
        }
    }
}

// Initialize the OPL library.  Returns true if initialized
// successfully.

int OPL_Init (unsigned int rate)
{
    opl_output_rate = rate ? rate : OPL_NATIVE_RATE;
    opl_sample_rate = OPL_NATIVE_RATE;   /* emulate the chip at hardware rate */
    opl_paused = 0;
    pause_offset = 0;

    /* native frames per output frame, 16.16.  When the output rate equals
     * the native rate this is exactly 1.0 and the resampler is a passthrough
     * (mono copy, no interpolation error). */
    opl_rs_step = (unsigned)(((uint64_t)OPL_NATIVE_RATE << 16) / opl_output_rate);
    opl_rs_frac = 0;
    opl_nsrc_have = 0;
    opl_nsrc_pos  = 0;

    /* cutoff = output Nyquist when downsampling, native Nyquist otherwise */
    opl_build_resampler_table(opl_output_rate < OPL_NATIVE_RATE
                              ? (double)opl_output_rate / (double)OPL_NATIVE_RATE
                              : 1.0);

    // Queue structure of callbacks to invoke.

    callback_queue = OPL_Queue_Create();
    current_time = 0;

    // Create the emulator structure:

    DBOPL_InitTables();
    Chip__Chip(&opl_chip);
    Chip__Setup(&opl_chip, opl_sample_rate);


    OPL_InitRegisters();

    init_stage_reg_writes = 0;

    return 1;
}

// Shut down the OPL library.

void OPL_Shutdown(void)
{
    if (callback_queue)
    {
      OPL_Queue_Destroy(callback_queue);
      callback_queue = NULL;
    }
}

void OPL_SetCallback(unsigned int ms,
                                opl_callback_t callback,
                                void *data)
{
    OPL_Queue_Push(callback_queue, callback, data,
                   current_time - pause_offset + (ms * opl_sample_rate) / 1000);
}

void OPL_ClearCallbacks(void)
{
    OPL_Queue_Clear(callback_queue);
}

static void OPLTimer_CalculateEndTime(opl_timer_t *timer)
{
    int tics;

    // If the timer is enabled, calculate the time when the timer
    // will expire.

    if (timer->enabled)
    {
        tics = 0x100 - timer->value;
        timer->expire_time = current_time
                           + (tics * opl_sample_rate) / timer->rate;
    }
}


static void WriteRegister(unsigned int reg_num, unsigned int value)
{
    switch (reg_num)
    {
        case OPL_REG_TIMER1:
            timer1.value = value;
            OPLTimer_CalculateEndTime(&timer1);
            break;

        case OPL_REG_TIMER2:
            timer2.value = value;
            OPLTimer_CalculateEndTime(&timer2);
            break;

        case OPL_REG_TIMER_CTRL:
            if (value & 0x80)
            {
                timer1.enabled = 0;
                timer2.enabled = 0;
            }
            else
            {
                if ((value & 0x40) == 0)
                {
                    timer1.enabled = (value & 0x01) != 0;
                    OPLTimer_CalculateEndTime(&timer1);
                }

                if ((value & 0x20) == 0)
                {
                    timer1.enabled = (value & 0x02) != 0;
                    OPLTimer_CalculateEndTime(&timer2);
                }
            }

            break;

        default:
            Chip__WriteReg(&opl_chip, reg_num, (unsigned char) value);
            break;
    }
}

static void OPL_AdvanceTime(unsigned int nsamples) 
{
    opl_callback_t callback;
    void *callback_data;


    // Advance time.

    current_time += nsamples;

    if (opl_paused)
    {
        pause_offset += nsamples;
    }

    // Are there callbacks to invoke now?  Keep invoking them
    // until there are none more left.

    while (!OPL_Queue_IsEmpty(callback_queue)
        && current_time >= OPL_Queue_Peek(callback_queue) + pause_offset)
    {
        // Pop the callback from the queue to invoke it.

        if (!OPL_Queue_Pop(callback_queue, &callback, &callback_data))
        {
            break;
        }


        callback(callback_data);

    }

}

/* Render `native_len` mono samples at the native chip rate, advancing the
 * MIDI/MUS event queue (also clocked at the native rate) between callbacks.
 * dbopl zero-fills and accumulates into the destination itself, so the chip
 * renders straight into the resample buffer -- no intermediate copy, and the
 * raw 32-bit sum is preserved for the FIR (gain/quantization happen once,
 * at emission). */
static void OPL_Render_Native (int32_t *buffer, unsigned native_len)
{
    unsigned int filled = 0;

    // Repeatedly call the OPL emulator update function until the buffer is
    // full.

    while (filled < native_len)
    {
        unsigned int next_callback_time;
        unsigned int nsamples;


        // Work out the time until the next callback waiting in
        // the callback queue must be invoked.  We can then fill the
        // buffer with this many samples.

        if (opl_paused || OPL_Queue_IsEmpty(callback_queue))
        {
            nsamples = native_len - filled;
        }
        else
        {
            next_callback_time = OPL_Queue_Peek(callback_queue) + pause_offset;

            nsamples = next_callback_time - current_time;

            if (nsamples > native_len - filled)
            {
                nsamples = native_len - filled;
            }
        }


        // Add emulator output to buffer (mono, native rate, raw chip sum).

        Chip__GenerateBlock2(&opl_chip, nsamples, buffer + filled);
        filled += nsamples;

        // Invoke callbacks for this point in time.

        OPL_AdvanceTime(nsamples);
    }
}

/* Advance the resampler one output frame: ensure the tap window is buffered
 * (generating native-rate chip samples on demand), apply the polyphase
 * windowed-sinc filter, step the read cursor, and return the filtered sample
 * in raw chip units -- mus_opl_gain is NOT applied here.  Shared by the s16
 * and float emitters below so the cursor arithmetic exists exactly once. */
static float OPL_Resample_Frame (void)
{
    int ridx = opl_nsrc_pos + (int)(opl_rs_frac >> 16);

    // Ensure the filter's history (ridx-(NZ-1)) and look-ahead (ridx+NZ)
    // are both inside the source buffer.
    while (ridx + OPL_RS_NZ >= opl_nsrc_have)
    {
        int keep_from = ridx - (OPL_RS_NZ - 1);

        if (keep_from > 0)
        {
            int rem = opl_nsrc_have - keep_from;
            int i;
            if (rem < 0)
                rem = 0;
            // Retain NZ-1 samples of history plus the unconsumed tail so
            // the filter taps and interpolation stay seamless.
            for (i = 0; i < rem; i++)
                opl_nsrc[i] = opl_nsrc[keep_from + i];
            opl_nsrc_pos   = ridx - keep_from;   // fold integer part, keep frac
            opl_rs_frac   &= 0xFFFF;
            opl_nsrc_have  = rem;
        }

        // Append-generate native samples; the chip is continuous, so the
        // new block abuts the retained tail with no discontinuity.
        OPL_Render_Native(opl_nsrc + opl_nsrc_have,
                          (unsigned)(OPL_NSRC_FRAMES - opl_nsrc_have));
        opl_nsrc_have = OPL_NSRC_FRAMES;
        ridx = opl_nsrc_pos + (int)(opl_rs_frac >> 16);
    }

    {
        const float *k = opl_rs_tab[(opl_rs_frac >> OPL_RS_PSHIFT) & (OPL_RS_NP - 1)];
        int   base = ridx - (OPL_RS_NZ - 1);
        float acc  = 0.0f;
        int   t;

        for (t = 0; t < OPL_RS_NTAPS; t++)
        {
            int j = base + t;
            // zero-pad history before the stream start (startup transient)
            if (j >= 0)
                acc += (float)opl_nsrc[j] * k[t];
        }

        opl_rs_frac += opl_rs_step;
        return acc;
    }
}

void OPL_Render_Samples (void *dest, unsigned buffer_len)
{
    short *out  = (short *) dest;
    /* mus_opl_gain semantics unchanged: 50 = unity, applied here in float
     * (was a truncating integer *gain/50 per native sample pre-FIR). */
    float  gain = (float) mus_opl_gain * (1.0f / 50.0f);

    // Pull `buffer_len` output frames through the resampler and quantize
    // once, writing stereo (the OPL is mono; both channels carry the same
    // sample).

    while (buffer_len > 0)
    {
        float acc = OPL_Resample_Frame () * gain;
        int   s   = (int)(acc < 0.0f ? acc - 0.5f : acc + 0.5f);

        if (s > 32767)        s = 32767;
        else if (s < -32768)  s = -32768;
        out[0] = (short) s;
        out[1] = (short) s;
        out += 2;
        buffer_len--;
    }
}

/* Float twin of OPL_Render_Samples: same resampler core and gain staging,
 * but the filtered sample goes out as normalized [-1,1] float stereo with
 * no int16 round-trip.  Used only when the frontend negotiated float audio
 * output (see render_float in musicplayer.h). */
void OPL_Render_Samples_Float (void *dest, unsigned buffer_len)
{
    float *out  = (float *) dest;
    float  gain = (float) mus_opl_gain * (1.0f / (50.0f * 32768.0f));

    while (buffer_len > 0)
    {
        float s = OPL_Resample_Frame () * gain;

        if (s >  1.0f)        s =  1.0f;
        else if (s < -1.0f)   s = -1.0f;
        out[0] = s;
        out[1] = s;
        out += 2;
        buffer_len--;
    }
}

/* Discard any native samples buffered ahead of the output cursor.  Called
 * when playback (re)starts so a song change cannot leak the tail of the
 * previous song's audio (up to one native block) into the new one. */
void OPL_FlushResampler (void)
{
    opl_nsrc_have = 0;
    opl_nsrc_pos  = 0;
    opl_rs_frac   = 0;
}

void OPL_WritePort(opl_port_t port, unsigned int value)
{
    if (port == OPL_REGISTER_PORT)
    {
        register_num = value;
    }
    else if (port == OPL_DATA_PORT)
    {
        WriteRegister(register_num, value);
    }
}

unsigned int OPL_ReadPort(opl_port_t port)
{
    unsigned int result = 0;

    if (timer1.enabled && current_time > timer1.expire_time)
    {
        result |= 0x80;   // Either have expired
        result |= 0x40;   // Timer 1 has expired
    }

    if (timer2.enabled && current_time > timer2.expire_time)
    {
        result |= 0x80;   // Either have expired
        result |= 0x20;   // Timer 2 has expired
    }

    return result;
}

//
// Higher-level functions, based on the lower-level functions above
// (register write, etc).
//

unsigned int OPL_ReadStatus(void)
{
    return OPL_ReadPort(OPL_REGISTER_PORT);
}

// Write an OPL register value

void OPL_WriteRegister(int reg, int value)
{
    int i;

    OPL_WritePort(OPL_REGISTER_PORT, reg);

    // For timing, read the register port six times after writing the
    // register number to cause the appropriate delay

    for (i=0; i<6; ++i)
    {
        // An oddity of the Doom OPL code: at startup initialization,
        // the spacing here is performed by reading from the register
        // port; after initialization, the data port is read, instead.

        if (init_stage_reg_writes)
        {
            OPL_ReadPort(OPL_REGISTER_PORT);
        }
        else
        {
            OPL_ReadPort(OPL_DATA_PORT);
        }
    }

    OPL_WritePort(OPL_DATA_PORT, value);

    // Read the register port 24 times after writing the value to
    // cause the appropriate delay

    for (i=0; i<24; ++i)
    {
        OPL_ReadStatus();
    }
}


// Initialize registers on startup

void OPL_InitRegisters(void)
{
    int r;

    // Initialize level registers

    for (r=OPL_REGS_LEVEL; r <= OPL_REGS_LEVEL + OPL_NUM_OPERATORS; ++r)
    {
        OPL_WriteRegister(r, 0x3f);
    }

    // Initialize other registers
    // These two loops write to registers that actually don't exist,
    // but this is what Doom does ...
    // Similarly, the <= is also intenational.

    for (r=OPL_REGS_ATTACK; r <= OPL_REGS_WAVEFORM + OPL_NUM_OPERATORS; ++r)
    {
        OPL_WriteRegister(r, 0x00);
    }

    // More registers ...

    for (r=1; r < OPL_REGS_LEVEL; ++r)
    {
        OPL_WriteRegister(r, 0x00);
    }

    // Re-initialize the low registers:

    // Reset both timers and enable interrupts:
    OPL_WriteRegister(OPL_REG_TIMER_CTRL,      0x60);
    OPL_WriteRegister(OPL_REG_TIMER_CTRL,      0x80);

    // "Allow FM chips to control the waveform of each operator":
    OPL_WriteRegister(OPL_REG_WAVEFORM_ENABLE, 0x20);

    // Keyboard split point on (?)
    OPL_WriteRegister(OPL_REG_FM_MODE,         0x40);
}



void OPL_SetPaused(int paused)
{
    opl_paused = paused;
}
