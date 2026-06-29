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

/* 16.16 linear resampler: native-rate mono chip output -> output stereo.
 * State persists across OPL_Render_Samples calls so the stream is seamless. */
#define OPL_NSRC_FRAMES 4096
static int16_t      opl_nsrc[OPL_NSRC_FRAMES];  /* native-rate mono source     */
static int          opl_nsrc_have = 0;          /* native frames buffered      */
static int          opl_nsrc_pos  = 0;          /* next unconsumed native frame */
static unsigned     opl_rs_frac   = 0;          /* 16.16 fractional read cursor */
static unsigned     opl_rs_step   = 1u << 16;   /* 16.16 native frames/out frame */


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

// Temporary mixing buffer used by the mixing callback.

static int *mix_buffer = NULL;

// Register number that was written.

static int register_num = 0;

// Timers; DBOPL does not do timer stuff itself.

static opl_timer_t timer1 = { 12500, 0, 0, 0 };
static opl_timer_t timer2 = { 3125, 0, 0, 0 };


//
// Init/shutdown code.
//

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

    // Queue structure of callbacks to invoke.

    callback_queue = OPL_Queue_Create();
    current_time = 0;


    mix_buffer = malloc(opl_sample_rate * sizeof(uint32_t));

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
      free(mix_buffer);

      callback_queue = NULL;
      mix_buffer = NULL;
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

static void FillBuffer(int16_t *buffer, unsigned int nsamples)
{
    unsigned int i;
    int sampval;
    
    Chip__GenerateBlock2(&opl_chip, nsamples, (int32_t *)mix_buffer);

    // OPL output is mono; write one sample per native frame.  The resampler
    // in OPL_Render_Samples expands to stereo at the output rate.

    for (i=0; i<nsamples; ++i)
    {
        sampval = mix_buffer[i] * mus_opl_gain / 50;
        // clip
        if (sampval > 32767)
            sampval = 32767;
        else if (sampval < -32768)
            sampval = -32768;
        buffer[i] = (int16_t) sampval;
    }
}


/* Render `native_len` mono samples at the native chip rate, advancing the
 * MIDI/MUS event queue (also clocked at the native rate) between callbacks.
 * This is the old OPL_Render_Samples body, minus the stereo doubling, which
 * the output resampler now handles. */
static void OPL_Render_Native (int16_t *buffer, unsigned native_len)
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


        // Add emulator output to buffer (mono, native rate).

        FillBuffer(buffer + filled, nsamples);
        filled += nsamples;

        // Invoke callbacks for this point in time.

        OPL_AdvanceTime(nsamples);
    }
}

void OPL_Render_Samples (void *dest, unsigned buffer_len)
{
    short *out = (short *) dest;

    // Pull `buffer_len` output frames through the 16.16 linear resampler,
    // generating native-rate mono chip samples on demand and writing stereo
    // (the OPL is mono; both channels carry the same sample).

    while (buffer_len > 0)
    {
        int base;

        // Ensure the current and next native source frame are available.
        while (opl_nsrc_pos + (int)(opl_rs_frac >> 16) + 1 >= opl_nsrc_have)
        {
            int consumed = opl_nsrc_pos + (int)(opl_rs_frac >> 16);
            int rem;

            if (consumed > 0)
            {
                int i;
                rem = opl_nsrc_have - consumed;
                if (rem < 0)
                    rem = 0;
                // retain the unconsumed tail so interpolation stays seamless
                for (i = 0; i < rem; i++)
                    opl_nsrc[i] = opl_nsrc[consumed + i];
                opl_nsrc_pos = 0;
                opl_rs_frac &= 0xFFFF;
            }
            else
                rem = opl_nsrc_have;   // nothing consumed yet; keep everything

            // Append-generate native samples; the chip is continuous, so the
            // new block abuts the retained tail with no discontinuity.
            OPL_Render_Native(opl_nsrc + rem, (unsigned)(OPL_NSRC_FRAMES - rem));
            opl_nsrc_have = OPL_NSRC_FRAMES;
        }

        base = opl_nsrc_pos + (int)(opl_rs_frac >> 16);
        {
            unsigned f = opl_rs_frac & 0xFFFF;
            int a = opl_nsrc[base];
            int b = (base + 1 < opl_nsrc_have) ? opl_nsrc[base + 1] : a;
            int s = a + (((b - a) * (int)f) >> 16);
            out[0] = (short) s;
            out[1] = (short) s;
            out += 2;
        }
        opl_rs_frac += opl_rs_step;
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
