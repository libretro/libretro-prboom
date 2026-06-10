/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 * DESCRIPTION:
 *      Raw-MIDI-out music player.
 *
 *      Unlike the OPL and FluidSynth players, this one does not
 *      synthesise audio.  It walks the song's flat event list on the
 *      same sample clock the other players use (MIDI_spmc gives the
 *      samples-per-MIDI-clock at the output rate) and, as each event
 *      comes due, re-encodes it as raw MIDI wire bytes and writes them
 *      to the frontend's MIDI output interface.  The frontend forwards
 *      those bytes to a real or virtual MIDI device, which produces the
 *      sound.  render() therefore emits only silence into the audio
 *      buffer -- the music is heard out of band, through the device.
 *
 *      Because the audio is produced externally and on the host's own
 *      clock, this player cannot be captured by video recording and is
 *      not sample-accurate against the emulated frame; it is offered as
 *      a "MIDI Hardware: libretro" option for users who specifically
 *      want host-side MIDI (e.g. a hardware module or an OS synth).
 *
 *---------------------------------------------------------------------
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <libretro.h>

#include "libretro_midiout.h"
#include "midifile.h"
#include "lprintf.h"

/* Bridge functions implemented in libretro/libretro.c.  They wrap the
 * frontend retro_midi_interface so this TU stays free of libretro.c
 * globals. */
int  I_LibretroMidiAvailable(void);
int  I_LibretroMidiWrite(unsigned char byte, unsigned delta_us);
void I_LibretroMidiFlush(void);

static int                 lm_soundrate = 0;
static int                 lm_volume    = 15;   /* 0..15 */
static int                 lm_playing   = 0;
static int                 lm_paused    = 0;
static int                 lm_looping   = 0;

static midi_file_t        *lm_midifile  = NULL;
static midi_event_t      **lm_events    = NULL;
static unsigned            lm_eventpos  = 0;
static double              lm_spmc      = 0.0;   /* samples per MIDI clock */
static double              lm_delta     = 0.0;   /* fractional sample carry */

/* Last channel-volume controller value the song set, per channel, so
 * the master-volume scaling in lm_setvolume can be re-applied without
 * losing the song's own dynamics.  -1 = song has not set it yet. */
static int                 lm_chan_vol[16];

/* ------------------------------------------------------------------ */

static const char *lm_name(void)
{
   return "libretro MIDI output";
}

static int lm_init(int samplerate)
{
   lm_soundrate = samplerate;
   return 1; /* always "succeeds"; availability is checked at registersong */
}

static void lm_all_notes_off(void)
{
   int ch;
   for (ch = 0; ch < 16; ch++)
   {
      I_LibretroMidiWrite((unsigned char)(0xb0 | ch), 0); /* control change */
      I_LibretroMidiWrite(123, 0);                        /* all notes off  */
      I_LibretroMidiWrite(0, 0);
   }
   I_LibretroMidiFlush();
}

static void lm_reset_controllers(void)
{
   int ch;
   for (ch = 0; ch < 16; ch++)
   {
      I_LibretroMidiWrite((unsigned char)(0xb0 | ch), 0);
      I_LibretroMidiWrite(121, 0); /* reset all controllers */
      I_LibretroMidiWrite(0, 0);
   }
   I_LibretroMidiFlush();
}

static void lm_shutdown(void)
{
   if (lm_playing)
      lm_all_notes_off();
   lm_playing = 0;
}

/* Send the channel-volume controller (CC7) for a channel, scaled by
 * the Doom master volume (0..15).  songvol is the value the song last
 * asked for (0..127). */
static void lm_send_scaled_volume(int ch, int songvol)
{
   int v;
   if (songvol < 0)
      songvol = 100; /* GM default channel volume */
   v = (songvol * lm_volume) / 15;
   if (v > 127) v = 127;
   if (v < 0)   v = 0;
   I_LibretroMidiWrite((unsigned char)(0xb0 | (ch & 15)), 0);
   I_LibretroMidiWrite(7, 0);
   I_LibretroMidiWrite((unsigned char)v, 0);
}

static void lm_setvolume(int v)
{
   int ch;
   lm_volume = v;
   if (!lm_playing)
      return;
   /* Re-apply master scaling to every channel's last known volume so a
    * volume change takes effect immediately, like the other players. */
   for (ch = 0; ch < 16; ch++)
      lm_send_scaled_volume(ch, lm_chan_vol[ch]);
   I_LibretroMidiFlush();
}

static void lm_pause(void)
{
   lm_paused = 1;
   /* Silence the device while paused; notes resume on the next events. */
   lm_all_notes_off();
}

static void lm_resume(void)
{
   lm_paused = 0;
}

static const void *lm_registersong(const void *data, unsigned len)
{
   midimem_t mf;

   /* Decline unless the frontend actually offers MIDI output right now.
    * Returning NULL makes I_RegisterSong fall through / fail cleanly,
    * and the dispatch in libretro_sound.c only selects this player when
    * the user picked it, so a no-MIDI frontend just yields silence for
    * that choice rather than a hard failure. */
   if (!I_LibretroMidiAvailable())
   {
      lprintf(LO_INFO,
              "libretro MIDI: frontend MIDI output unavailable; "
              "song not registered.\n");
      return NULL;
   }

   mf.len  = len;
   mf.pos  = 0;
   mf.data = data;

   lm_midifile = MIDI_LoadFile(&mf);
   if (!lm_midifile)
   {
      lprintf(LO_WARN, "lm_registersong: Failed to load MIDI.\n");
      return NULL;
   }

   lm_events = MIDI_GenerateFlatList(lm_midifile);
   if (!lm_events)
   {
      MIDI_FreeFile(lm_midifile);
      lm_midifile = NULL;
      return NULL;
   }

   lm_eventpos = 0;
   lm_spmc     = MIDI_spmc(lm_midifile, NULL, lm_soundrate);

   /* handle not used by caller */
   return data;
}

static void lm_unregistersong(const void *handle)
{
   (void)handle;
   if (lm_events)
   {
      MIDI_DestroyFlatList(lm_events);
      lm_events = NULL;
   }
   if (lm_midifile)
   {
      MIDI_FreeFile(lm_midifile);
      lm_midifile = NULL;
   }
}

static void lm_play(const void *handle, int looping)
{
   int ch;
   (void)handle;

   lm_eventpos = 0;
   lm_looping  = looping;
   lm_playing  = 1;
   lm_paused   = 0;
   lm_delta    = 0.0;

   for (ch = 0; ch < 16; ch++)
      lm_chan_vol[ch] = -1;

   /* Start from a clean device state. */
   lm_reset_controllers();
   lm_all_notes_off();
}

static void lm_stop(void)
{
   if (!lm_playing)
      return;
   lm_playing = 0;
   lm_all_notes_off();
   lm_reset_controllers();
}

/* Re-encode one MIDI event to wire bytes and write it to the frontend.
 * Channel-volume controllers (CC7) are intercepted so the song's value
 * is recorded and the emitted value is scaled by the master volume.
 * END_OF_TRACK is handled by the caller (iteration control). */
static void lm_apply_event(midi_event_t *ev)
{
   unsigned ch;

   switch (ev->event_type)
   {
      case MIDI_EVENT_NOTE_OFF:
         ch = ev->data.channel.channel & 15;
         I_LibretroMidiWrite((unsigned char)(0x80 | ch), 0);
         I_LibretroMidiWrite((unsigned char)ev->data.channel.param1, 0);
         I_LibretroMidiWrite((unsigned char)ev->data.channel.param2, 0);
         break;

      case MIDI_EVENT_NOTE_ON:
         ch = ev->data.channel.channel & 15;
         I_LibretroMidiWrite((unsigned char)(0x90 | ch), 0);
         I_LibretroMidiWrite((unsigned char)ev->data.channel.param1, 0);
         I_LibretroMidiWrite((unsigned char)ev->data.channel.param2, 0);
         break;

      case MIDI_EVENT_AFTERTOUCH:
         ch = ev->data.channel.channel & 15;
         I_LibretroMidiWrite((unsigned char)(0xa0 | ch), 0);
         I_LibretroMidiWrite((unsigned char)ev->data.channel.param1, 0);
         I_LibretroMidiWrite((unsigned char)ev->data.channel.param2, 0);
         break;

      case MIDI_EVENT_CONTROLLER:
         ch = ev->data.channel.channel & 15;
         if (ev->data.channel.param1 == 7) /* channel volume */
         {
            lm_chan_vol[ch] = (int)ev->data.channel.param2;
            lm_send_scaled_volume((int)ch, lm_chan_vol[ch]);
         }
         else
         {
            I_LibretroMidiWrite((unsigned char)(0xb0 | ch), 0);
            I_LibretroMidiWrite((unsigned char)ev->data.channel.param1, 0);
            I_LibretroMidiWrite((unsigned char)ev->data.channel.param2, 0);
         }
         break;

      case MIDI_EVENT_PROGRAM_CHANGE:
         ch = ev->data.channel.channel & 15;
         I_LibretroMidiWrite((unsigned char)(0xc0 | ch), 0);
         I_LibretroMidiWrite((unsigned char)ev->data.channel.param1, 0);
         break;

      case MIDI_EVENT_CHAN_AFTERTOUCH:
         ch = ev->data.channel.channel & 15;
         I_LibretroMidiWrite((unsigned char)(0xd0 | ch), 0);
         I_LibretroMidiWrite((unsigned char)ev->data.channel.param1, 0);
         break;

      case MIDI_EVENT_PITCH_BEND:
         ch = ev->data.channel.channel & 15;
         I_LibretroMidiWrite((unsigned char)(0xe0 | ch), 0);
         I_LibretroMidiWrite((unsigned char)ev->data.channel.param1, 0);
         I_LibretroMidiWrite((unsigned char)ev->data.channel.param2, 0);
         break;

      case MIDI_EVENT_SYSEX:
      case MIDI_EVENT_SYSEX_SPLIT:
      {
         /* Forward the SYSEX payload verbatim.  MIDI_GenerateFlatList
          * stores the bytes following the 0xF0/0xF7 status; emit the
          * status, the body, and a terminating 0xF7 if the body did
          * not already carry one. */
         unsigned i;
         unsigned l = ev->data.sysex.length;
         I_LibretroMidiWrite((unsigned char)ev->event_type, 0);
         for (i = 0; i < l; i++)
            I_LibretroMidiWrite(ev->data.sysex.data[i], 0);
         if (l == 0 || ev->data.sysex.data[l - 1] != 0xf7)
            I_LibretroMidiWrite(0xf7, 0);
         break;
      }

      case MIDI_EVENT_META:
         /* Tempo changes retune the sample clock; nothing goes to the
          * device.  Other meta events (track name, markers, etc.) are
          * not MIDI wire data and are skipped. */
         if (ev->data.meta.type == MIDI_META_SET_TEMPO)
            lm_spmc = MIDI_spmc(lm_midifile, ev, lm_soundrate);
         break;

      default:
         break;
   }
}

static void lm_render(void *vdest, unsigned length)
{
   /* This player produces no audio of its own: the host device does.
    * Always clear the buffer so the mixer sees silence from music. */
   memset(vdest, 0, (size_t)length * 2 * sizeof(int16_t));

   if (!lm_playing || lm_paused || !lm_events)
      return;

   {
      unsigned samplesdone = 0;
      int      emitted     = 0;

      for (;;)
      {
         midi_event_t *ev = lm_events[lm_eventpos];
         double        eventdelta = ev->delta_time * lm_spmc;
         unsigned      samples    = (unsigned)(eventdelta + lm_delta);

         if (samples + samplesdone > length)
            break;  /* next event lands beyond this chunk */

         samplesdone += samples;
         lm_delta    -= samples;

         lm_apply_event(ev);
         emitted = 1;

         if (ev->event_type == MIDI_EVENT_META &&
             ev->data.meta.type == MIDI_META_END_OF_TRACK)
         {
            if (lm_looping)
            {
               lm_eventpos = 0;
               lm_delta   += eventdelta;
               lm_all_notes_off(); /* clear notes held across the loop */
               continue;
            }
            /* Non-looping: stop and silence the device. */
            lm_stop();
            I_LibretroMidiFlush();
            return;
         }

         lm_delta += eventdelta;
         lm_eventpos++;
      }

      /* Carry the unconsumed portion of this chunk into the next call,
       * matching the FluidSynth/OPL fractional-sample accounting. */
      if (samplesdone < length)
         lm_delta -= (double)(length - samplesdone);

      if (emitted)
         I_LibretroMidiFlush();
   }
}

/* State save/restore -----------------------------------------------------
 *
 * Mirrors the FluidSynth player: persist the flat-list index, the
 * sub-event sample carry, the current tempo and the loop/pause flags,
 * then on restore replay every event up to the saved index so the
 * device's program/controller/pitch state matches.  Notes that were
 * sounding at the save point are not re-keyed (we cannot read the
 * device back); they simply resume from the next NOTE_ON in the stream.
 */

#define LM_STATE_MAGIC        0x4C4D4F31u  /* 'LMO1' */
#define LM_STATE_VERSION      1u
#define LM_STATE_FLAG_LOOPING 0x01u
#define LM_STATE_FLAG_PAUSED  0x02u

typedef struct {
   uint32_t magic;
   uint16_t version;
   uint16_t flags;
   uint32_t eventpos;
   double   delta;
   double   spmc;
} lm_state_t;

static size_t lm_serialize(void *dest, size_t cap)
{
   lm_state_t s;

   if (!lm_events || !lm_midifile) return 0;
   if (!lm_playing)                return 0;

   if (!dest)
      return sizeof(lm_state_t);
   if (cap < sizeof(lm_state_t))
      return 0;

   s.magic    = LM_STATE_MAGIC;
   s.version  = LM_STATE_VERSION;
   s.flags    = (uint16_t)((lm_looping ? LM_STATE_FLAG_LOOPING : 0u)
                         | (lm_paused  ? LM_STATE_FLAG_PAUSED  : 0u));
   s.eventpos = (uint32_t)lm_eventpos;
   s.delta    = lm_delta;
   s.spmc     = lm_spmc;
   memcpy(dest, &s, sizeof s);
   return sizeof s;
}

static int lm_unserialize(const void *src, size_t size)
{
   lm_state_t s;
   uint32_t   i;
   int        ch;

   if (!lm_events || !lm_midifile)      return 0;
   if (size < sizeof(lm_state_t))       return 0;

   memcpy(&s, src, sizeof s);
   if (s.magic   != LM_STATE_MAGIC)     return 0;
   if (s.version != LM_STATE_VERSION)   return 0;

   /* Reset the device and replay forward so program/controller/bend
    * state matches the save point. */
   lm_all_notes_off();
   lm_reset_controllers();
   for (ch = 0; ch < 16; ch++)
      lm_chan_vol[ch] = -1;

   lm_eventpos = 0;
   lm_delta    = 0.0;
   lm_spmc     = MIDI_spmc(lm_midifile, NULL, lm_soundrate);

   for (i = 0; i < s.eventpos; i++)
   {
      midi_event_t *ev = lm_events[i];
      if (!ev)
         break;
      if (ev->event_type == MIDI_EVENT_META &&
          ev->data.meta.type == MIDI_META_END_OF_TRACK)
         break;
      /* Replay program/controller/tempo/bend state but suppress the
       * note keystrokes -- re-keying every note from song start would
       * spray the device with NOTE_ON bursts. */
      if (ev->event_type != MIDI_EVENT_NOTE_ON &&
          ev->event_type != MIDI_EVENT_NOTE_OFF)
         lm_apply_event(ev);
   }

   lm_eventpos = s.eventpos;
   lm_delta    = s.delta;
   lm_spmc     = s.spmc;
   lm_looping  = (s.flags & LM_STATE_FLAG_LOOPING) ? 1 : 0;
   lm_paused   = (s.flags & LM_STATE_FLAG_PAUSED)  ? 1 : 0;
   lm_playing  = 1;
   I_LibretroMidiFlush();
   return 1;
}

const music_player_t libretro_midi_player =
{
   lm_name,
   lm_init,
   lm_shutdown,
   lm_setvolume,
   lm_pause,
   lm_resume,
   lm_registersong,
   lm_unregistersong,
   lm_play,
   lm_stop,
   lm_render,
   lm_serialize,
   lm_unserialize
};
