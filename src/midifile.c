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
//    Reading of MIDI files.
//
//-----------------------------------------------------------------------------

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "config.h"

#include "doomdef.h"
#include "doomtype.h"
#include "lprintf.h"
#include "midifile.h"

#define HEADER_CHUNK_ID "MThd"
#define TRACK_CHUNK_ID  "MTrk"
#define MAX_BUFFER_SIZE 0x10000

#ifndef ntohl
#ifdef MSB_FIRST
#define ntohl
#define ntohs
#else

#define ntohl(x) \
        (/*(long int)*/((((unsigned long int)(x) & 0x000000ffU) << 24) | \
                             (((unsigned long int)(x) & 0x0000ff00U) <<  8) | \
                             (((unsigned long int)(x) & 0x00ff0000U) >>  8) | \
                             (((unsigned long int)(x) & 0xff000000U) >> 24)))

#define ntohs(x) \
        (/*(short int)*/((((unsigned short int)(x) & 0x00ff) << 8) | \
                              (((unsigned short int)(x) & 0xff00) >> 8)))
#endif
#endif // ntohl


#ifdef _MSC_VER
#pragma pack(push, 1)
#endif

typedef struct
{
    unsigned char chunk_id[4];
    unsigned int chunk_size;
} PACKEDATTR chunk_header_t;

typedef struct
{
    chunk_header_t chunk_header;
    unsigned short format_type;
    unsigned short num_tracks;
    unsigned short time_division;
} PACKEDATTR midi_header_t;

#ifdef _MSC_VER
#pragma pack(pop)
#endif

typedef struct
{
    // Length in bytes:

    unsigned int data_len;

    // Events in this track:

    midi_event_t *events;
    unsigned int num_events;
    unsigned int num_event_mem; // NSM track size of structure
} midi_track_t;

struct midi_track_iter_s
{
    midi_track_t *track;
    unsigned int position;
};

struct midi_file_s
{
    midi_header_t header;

    // All tracks in this file:
    midi_track_t *tracks;
    unsigned int num_tracks;

    // Data buffer used to store data read for SysEx or meta events:
    unsigned char *buffer;
    unsigned int buffer_size;
};



// Check the header of a chunk:

static dbool   CheckChunkHeader(chunk_header_t *chunk,
                                const char *expected_id)
{
    return (memcmp((char *) chunk->chunk_id, expected_id, 4) == 0);
}

// Read a single byte.  Returns false on error.

static dbool   ReadByte(unsigned char *result, midimem_t *mf)
{
    if (mf->pos >= mf->len)
        return false;

    *result = mf->data[mf->pos++];
    return true;
}

static dbool   ReadMultipleBytes (void *dest, size_t len, midimem_t *mf)
{
  unsigned char *cdest = (unsigned char *) dest;
  unsigned i;
  for (i = 0; i < len; i++)
  {
    if (!ReadByte (cdest + i, mf))
      return false;
  }
  return true;
}

// Read a variable-length value.

static dbool   ReadVariableLength(unsigned int *result, midimem_t *mf)
{
    int i;
    unsigned char b = 0;

    *result = 0;

    for (i=0; i<4; ++i)
    {
        if (!ReadByte(&b, mf))
            return false;

        // Insert the bottom seven bits from this byte.

        *result <<= 7;
        *result |= b & 0x7f;

        // If the top bit is not set, this is the end.

        if ((b & 0x80) == 0)
            return true;
    }

    return false;
}

// Read a byte sequence into the data buffer.

static void *ReadByteSequence(unsigned int num_bytes, midimem_t *mf)
{
    unsigned int i;
    unsigned char *result;

    // events can be length 0.  malloc(0) is not portable (can return NULL)
    if (!num_bytes)
      return malloc (4);

    // Allocate a buffer:

    result = malloc(num_bytes);

    if (result == NULL)
        return NULL;

    // Read the data:

    for (i=0; i<num_bytes; ++i)
    {
        if (!ReadByte(&result[i], mf))
        {
            lprintf (LO_WARN, "ReadByteSequence: Error while reading byte %u\n", i);
            free(result);
            return NULL;
        }
    }

    return result;
}

// Read a MIDI channel event.
// two_param indicates that the event type takes two parameters
// (three byte) otherwise it is single parameter (two byte)

static dbool   ReadChannelEvent(midi_event_t *event,
                                unsigned char event_type, dbool   two_param,
                                midimem_t *mf)
{
    unsigned char b = 0;

    // Set basics:

    event->event_type = event_type & 0xf0;
    event->data.channel.channel = event_type & 0x0f;

    // Read parameters:

    if (!ReadByte(&b, mf))
    {
        lprintf (LO_WARN, "ReadChannelEvent: Error while reading channel "
                        "event parameters\n");
        return false;
    }

    event->data.channel.param1 = b;

    // Second parameter:

    if (two_param)
    {
        if (!ReadByte(&b, mf))
        {
            lprintf (LO_WARN, "ReadChannelEvent: Error while reading channel "
                            "event parameters\n");
            return false;
        }

        event->data.channel.param2 = b;
    }

    return true;
}

// Read sysex event:

static dbool   ReadSysExEvent(midi_event_t *event, int event_type,
                               midimem_t *mf)
{
    event->event_type = event_type;

    if (!ReadVariableLength(&event->data.sysex.length, mf))
    {
        lprintf (LO_WARN, "ReadSysExEvent: Failed to read length of "
                                        "SysEx block\n");
        return false;
    }

    // Read the byte sequence:

    event->data.sysex.data = ReadByteSequence(event->data.sysex.length, mf);

    if (event->data.sysex.data == NULL)
    {
        lprintf (LO_WARN, "ReadSysExEvent: Failed while reading SysEx event\n");
        return false;
    }

    return true;
}

// Read meta event:

static dbool   ReadMetaEvent(midi_event_t *event, midimem_t *mf)
{
    unsigned char b = 0;

    event->event_type = MIDI_EVENT_META;

    // Read meta event type:

    if (!ReadByte(&b, mf))
    {
        lprintf (LO_WARN, "ReadMetaEvent: Failed to read meta event type\n");
        return false;
    }

    event->data.meta.type = b;

    // Read length of meta event data:

    if (!ReadVariableLength(&event->data.meta.length, mf))
    {
        lprintf (LO_WARN, "ReadMetaEvent: Failed to read length of "
                                        "MetaEvent block\n");
        return false;
    }

    // Read the byte sequence:

    event->data.meta.data = ReadByteSequence(event->data.meta.length, mf);

    if (event->data.meta.data == NULL)
    {
        lprintf (LO_WARN, "ReadMetaEvent: Failed while reading MetaEvent\n");
        return false;
    }

    return true;
}

static dbool ReadEvent(midi_event_t *event, unsigned int *last_event_type,
                          midimem_t *mf)
{
    unsigned char event_type = 0;

    if (!ReadVariableLength(&event->delta_time, mf))
    {
        lprintf (LO_WARN, "ReadEvent: Failed to read event timestamp\n");
        return false;
    }

    if (!ReadByte(&event_type, mf))
    {
        lprintf (LO_WARN, "ReadEvent: Failed to read event type\n");
        return false;
    }

    // All event types have their top bit set.  Therefore, if
    // the top bit is not set, it is because we are using the "same
    // as previous event type" shortcut to save a byte.  Skip back
    // a byte so that we read this byte again.

    if ((event_type & 0x80) == 0)
    {
        event_type = *last_event_type;
        mf->pos--;
    }
    else
    {
        *last_event_type = event_type;
    }

    // Check event type:

    switch (event_type & 0xf0)
    {
        // Two parameter channel events:

        case MIDI_EVENT_NOTE_OFF:
        case MIDI_EVENT_NOTE_ON:
        case MIDI_EVENT_AFTERTOUCH:
        case MIDI_EVENT_CONTROLLER:
        case MIDI_EVENT_PITCH_BEND:
            return ReadChannelEvent(event, event_type, true, mf);

        // Single parameter channel events:

        case MIDI_EVENT_PROGRAM_CHANGE:
        case MIDI_EVENT_CHAN_AFTERTOUCH:
            return ReadChannelEvent(event, event_type, false, mf);

        default:
            break;
    }

    // Specific value?

    switch (event_type)
    {
        case MIDI_EVENT_SYSEX:
        case MIDI_EVENT_SYSEX_SPLIT:
            return ReadSysExEvent(event, event_type, mf);

        case MIDI_EVENT_META:
            return ReadMetaEvent(event, mf);

        default:
            break;
    }

    lprintf (LO_WARN, "ReadEvent: Unknown MIDI event type: 0x%x\n", event_type);
    return false;
}

// Free an event:

static void FreeEvent(midi_event_t *event)
{
    // Some event types have dynamically allocated buffers assigned
    // to them that must be freed.

    switch (event->event_type)
    {
        case MIDI_EVENT_SYSEX:
        case MIDI_EVENT_SYSEX_SPLIT:
            free(event->data.sysex.data);
            break;

        case MIDI_EVENT_META:
            free(event->data.meta.data);
            break;

        default:
            // Nothing to do.
            break;
    }
}

// Read and check the track chunk header

static dbool ReadTrackHeader(midi_track_t *track, midimem_t *mf)
{
    size_t records_read;
    chunk_header_t chunk_header;

    records_read = ReadMultipleBytes(&chunk_header, sizeof(chunk_header_t), mf);

    if (records_read < 1)
    {
        return false;
    }

    if (!CheckChunkHeader(&chunk_header, TRACK_CHUNK_ID))
    {
        return false;
    }

    track->data_len = ntohl(chunk_header.chunk_size);

    return true;
}

static dbool ReadTrack(midi_track_t *track, midimem_t *mf)
{
    midi_event_t *new_events = NULL;
    midi_event_t *event;
    unsigned int last_event_type;

    track->num_events = 0;
    track->events = NULL;
    track->num_event_mem = 0; // NSM

    // Read the header:

    if (!ReadTrackHeader(track, mf))
    {
        return false;
    }

    // Then the events:

    last_event_type = 0;

    for (;;)
    {
        // Resize the track slightly larger to hold another event:
        /*
        new_events = realloc(track->events,
                             sizeof(midi_event_t) * (track->num_events + 1));
        */
        if (track->num_events == track->num_event_mem)
        { // depending on the state of the heap and the malloc implementation, realloc()
          // one more event at a time can be VERY slow.  10sec+ in MSVC
          track->num_event_mem += 100; 
          new_events = realloc (track->events, sizeof (midi_event_t) * track->num_event_mem);
        }

        if (new_events == NULL)
        {
            return false;
        }

        track->events = new_events;

        // Read the next event:

        event = &track->events[track->num_events];
        if (!ReadEvent(event, &last_event_type, mf))
        {
            return false;
        }

        ++track->num_events;

        // End of track?

        if (event->event_type == MIDI_EVENT_META
         && event->data.meta.type == MIDI_META_END_OF_TRACK)
        {
            break;
        }
    }

    return true;
}

// Free a track:

static void FreeTrack(midi_track_t *track)
{
    unsigned i;

    for (i=0; i<track->num_events; ++i)
    {
        FreeEvent(&track->events[i]);
    }

    free(track->events);
}

static dbool ReadAllTracks(midi_file_t *file, midimem_t *mf)
{
    unsigned int i;

    // Allocate list of tracks and read each track:

    file->tracks = malloc(sizeof(midi_track_t) * file->num_tracks);

    if (file->tracks == NULL)
    {
        return false;
    }

    memset(file->tracks, 0, sizeof(midi_track_t) * file->num_tracks);

    // Read each track:

    for (i=0; i<file->num_tracks; ++i)
    {
        if (!ReadTrack(&file->tracks[i], mf))
        {
            return false;
        }
    }

    return true;
}

// Read and check the header chunk.

static dbool ReadFileHeader(midi_file_t *file, midimem_t *mf)
{
    size_t records_read;
    unsigned int format_type;

    records_read = ReadMultipleBytes (&file->header, sizeof(midi_header_t), mf);

    if (records_read < 1)
    {
        return false;
    }

    if (!CheckChunkHeader(&file->header.chunk_header, HEADER_CHUNK_ID)
     || ntohl(file->header.chunk_header.chunk_size) != 6)
    {
        lprintf (LO_WARN, "ReadFileHeader: Invalid MIDI chunk header! "
                        "chunk_size=%ld\n",
                        ntohl(file->header.chunk_header.chunk_size));
        return false;
    }

    format_type = ntohs(file->header.format_type);
    file->num_tracks = ntohs(file->header.num_tracks);

    if ((format_type != 0 && format_type != 1)
     || file->num_tracks < 1)
    {
        lprintf (LO_WARN, "ReadFileHeader: Only type 0/1 "
                                         "MIDI files supported!\n");
        return false;
    }
    // NSM
    file->header.time_division = ntohs (file->header.time_division);


    return true;
}

void MIDI_FreeFile(midi_file_t *file)
{
    unsigned i;

    if (file->tracks != NULL)
    {
        for (i=0; i<file->num_tracks; ++i)
        {
            FreeTrack(&file->tracks[i]);
        }

        free(file->tracks);
    }

    free(file);
}

midi_file_t *MIDI_LoadFile (midimem_t *mf)
{
    midi_file_t *file;

    file = malloc(sizeof(midi_file_t));

    if (file == NULL)
    {
        return NULL;
    }

    file->tracks = NULL;
    file->num_tracks = 0;
    file->buffer = NULL;
    file->buffer_size = 0;

    // Read MIDI file header

    if (!ReadFileHeader(file, mf))
    {
        MIDI_FreeFile(file);
        return NULL;
    }

    // Read all tracks:

    if (!ReadAllTracks(file, mf))
    {
        MIDI_FreeFile(file);
        return NULL;
    }

    return file;
}

// Get the number of tracks in a MIDI file.

unsigned int MIDI_NumTracks(const midi_file_t *file)
{
    return file->num_tracks;
}

// Start iterating over the events in a track.

midi_track_iter_t *MIDI_IterateTrack(const midi_file_t *file, unsigned int track)
{
    midi_track_iter_t *iter;

    assert(track < file->num_tracks);

    iter = malloc(sizeof(*iter));
    iter->track = &file->tracks[track];
    iter->position = 0;

    return iter;
}

void MIDI_FreeIterator(midi_track_iter_t *iter)
{
    free(iter);
}

// Get the time until the next MIDI event in a track.

unsigned int MIDI_GetDeltaTime(midi_track_iter_t *iter)
{
    if (iter->position < iter->track->num_events)
    {
        midi_event_t *next_event;

        next_event = &iter->track->events[iter->position];

        return next_event->delta_time;
    }
    else
    {
        return 0;
    }
}

// Get a pointer to the next MIDI event.

int MIDI_GetNextEvent(midi_track_iter_t *iter, midi_event_t **event)
{
    if (iter->position < iter->track->num_events)
    {
        *event = &iter->track->events[iter->position];
        ++iter->position;

        return 1;
    }
    else
    {
        return 0;
    }
}

unsigned int MIDI_GetFileTimeDivision(const midi_file_t *file)
{
    return file->header.time_division;
}

void MIDI_RestartIterator(midi_track_iter_t *iter)
{
    iter->position = 0;
}

// NSM: an alternate iterator tool.

midi_event_t **MIDI_GenerateFlatList (midi_file_t *file)
{
  int totalevents = 0;
  unsigned i, delta;
  int nextrk;

  int totaldelta = 0;

  int *trackpos = calloc (file->num_tracks, sizeof (int));
  int *tracktime = calloc (file->num_tracks, sizeof (int));
  int trackactive = file->num_tracks;

  midi_event_t **ret;
  midi_event_t **epos;

  for (i = 0; i < file->num_tracks; i++)
    totalevents += file->tracks[i].num_events;

  ret = malloc (totalevents * sizeof (midi_event_t **));

  epos = ret;

  while (trackactive)
  {
    delta = 0x10000000;
    nextrk = -1;
    for (i = 0; i < file->num_tracks; i++)
    {
      if (trackpos[i] != -1 && file->tracks[i].events[trackpos[i]].delta_time - tracktime[i] < delta)
      {
        delta = file->tracks[i].events[trackpos[i]].delta_time - tracktime[i];
        nextrk = i;
      }
    }
    if (nextrk == -1) // unexpected EOF (not every track got end track)
      break;

    *epos = file->tracks[nextrk].events + trackpos[nextrk];

    for (i = 0; i < file->num_tracks; i++)
    {
      if (i == (unsigned) nextrk)
      {
        tracktime[i] = 0;
        trackpos[i]++;
      }
      else
        tracktime[i] += delta;
    }
    // yes, this clobbers the original timecodes
    epos[0]->delta_time = delta;
    totaldelta += delta;

    if (epos[0]->event_type == MIDI_EVENT_META
      && epos[0]->data.meta.type == MIDI_META_END_OF_TRACK)
    { // change end of track into no op
      trackactive--;
      trackpos[nextrk] = -1;
      epos[0]->data.meta.type = MIDI_META_TEXT;
    }
    else if ((unsigned) trackpos[nextrk] == file->tracks[nextrk].num_events)
    {
      lprintf (LO_WARN, "MIDI_GenerateFlatList: Unexpected end of track\n");
      free (trackpos);
      free (tracktime);
      free (ret);
      return NULL;
    }
    epos++;
  }
  
  if (trackactive)
  { // unexpected EOF
    lprintf (LO_WARN, "MIDI_GenerateFlatList: Unexpected end of midi file\n");
    free (trackpos);
    free (tracktime);
    free (ret);
    return NULL;
  }
  
  // last end of track event is preserved though
  epos[-1]->data.meta.type = MIDI_META_END_OF_TRACK;

  free (trackpos);
  free (tracktime);
  
  if (totaldelta < 100)
  {
    lprintf (LO_WARN, "MIDI_GeneratFlatList: very short file %i\n", totaldelta);
    free (ret);
    return NULL;
  }


  //MIDI_PrintFlatListDBG (ret);

  return ret;
}



void MIDI_DestroyFlatList (midi_event_t **evs)
{
  free (evs);
}



// NSM: timing assist functions
// they compute samples per midi clock, where midi clock is the units
// that the time deltas are in, and samples is an arbitrary unit of which
// "sndrate" of them occur per second


static double compute_spmc_normal (unsigned mpq, unsigned tempo, unsigned sndrate)
{ // returns samples per midi clock

  // inputs: mpq (midi clocks per quarternote, from header)
  // tempo (from tempo event, in microseconds per quarternote)
  // sndrate (sound sample rate in hz)

  // samples   quarternote     microsec    samples    second
  // ------- = ----------- * ----------- * ------- * --------
  // midiclk     midiclk     quarternote   second    microsec

  // return  =  (1 / mpq)  *    tempo    * sndrate * (1 / 1000000)

  return (double) tempo / 1000000 * sndrate / mpq;

}

static double compute_spmc_smpte (unsigned smpte_fps, unsigned mpf, unsigned sndrate)
{ // returns samples per midi clock

  // inputs: smpte_fps (24, 25, 29, 30)
  // mpf (midi clocks per frame, 0-255)
  // sndrate (sound sample rate in hz)

  // tempo is ignored here

  // samples     frame      seconds    samples
  // ------- = --------- * --------- * -------
  // midiclk    midiclk      frame     second

  // return  = (1 / mpf) * (1 / fps) * sndrate


  double fps; // actual frames per second
  switch (smpte_fps)
  {
    case 24:
    case 25:
    case 30:
      fps = smpte_fps;
      break;
    case 29:
      // i hate NTSC, i really do
      fps = smpte_fps * 1000.0 / 1001.0;
      break;
    default:
      lprintf (LO_WARN, "MIDI_spmc: Unexpected SMPTE timestamp %i\n", smpte_fps);
      // assume
      fps = 30.0;
      break;
  }

  return (double) sndrate / fps / mpf;

}

// if event is NULL, compute with default starting tempo (120BPM)

double MIDI_spmc (const midi_file_t *file, const midi_event_t *ev, unsigned sndrate)
{
  int smpte_fps;
  unsigned tempo;
  unsigned headerval = MIDI_GetFileTimeDivision (file);

  if (headerval & 0x8000) // SMPTE
  { // i don't really have any files to test this on...
    smpte_fps = -(short) headerval >> 8;
    return compute_spmc_smpte (smpte_fps, headerval & 0xff, sndrate);
  }

  // normal timing
  tempo = 500000; // default 120BPM
  if (ev)
  {
    if (ev->event_type == MIDI_EVENT_META)
    {
      if (ev->data.meta.length == 3)
      {
        tempo = (unsigned) ev->data.meta.data[0] << 16 |
                (unsigned) ev->data.meta.data[1] << 8 |
                (unsigned) ev->data.meta.data[2];
      }
      else
        lprintf (LO_WARN, "MIDI_spmc: wrong length tempo meta message in midi file\n");
    }
    else
      lprintf (LO_WARN, "MIDI_spmc: passed non-meta event\n");
  }

  return compute_spmc_normal (headerval, tempo, sndrate);
}

/*
The timing system used by the OPL driver is very interesting. But there are too many edge cases
in multitrack (type 1) midi tempo changes that it simply can't handle without a major rework.
The alternative is that we recook the file into a single track file with no tempo changes at
load time.
*/

midi_file_t *MIDI_LoadFileSpecial (midimem_t *mf)
{
  midi_event_t **flatlist;
  midi_file_t *base = MIDI_LoadFile (mf);
  midi_file_t *ret;
  
  double opi;

  int epos = 0;

  if (!base)
    return NULL;

  flatlist = MIDI_GenerateFlatList (base);
  if (!flatlist)
  {
    MIDI_FreeFile (base);
    return NULL;
  }

  ret = malloc (sizeof (midi_file_t));

  ret->header.format_type = 0;
  ret->header.num_tracks = 1;
  ret->header.time_division = 10000;
  ret->num_tracks = 1;
  ret->buffer_size = 0;
  ret->buffer = NULL;
  ret->tracks = malloc (sizeof (midi_track_t));

  ret->tracks->num_events = 0;
  ret->tracks->num_event_mem = 0;
  ret->tracks->events = NULL;

  opi = MIDI_spmc (base, NULL, 20000);


  while (1)
  {
    midi_event_t *oldev;
    midi_event_t *nextev;

    if (ret->tracks->num_events == ret->tracks->num_event_mem)
    { 
      ret->tracks->num_event_mem += 100; 
      ret->tracks->events = realloc (ret->tracks->events, sizeof (midi_event_t) * ret->tracks->num_event_mem);
    }

    oldev = flatlist[epos];
    nextev = ret->tracks->events + ret->tracks->num_events;


    // figure delta time
    nextev->delta_time = (unsigned int)(opi * oldev->delta_time);

    if (oldev->event_type == MIDI_EVENT_SYSEX ||
        oldev->event_type == MIDI_EVENT_SYSEX_SPLIT)
      // opl player can't process any sysex...
    {
      epos++;
      continue;
    }

    if (oldev->event_type == MIDI_EVENT_META)
    {
      if (oldev->data.meta.type == MIDI_META_SET_TEMPO)
      { // adjust future tempo scaling
        opi = MIDI_spmc (base, oldev, 20000);
        // insert event as dummy
        nextev->event_type = MIDI_EVENT_META;
        nextev->data.meta.type = MIDI_META_TEXT;
        nextev->data.meta.length = 0;
        nextev->data.meta.data = malloc (4);
        epos++;
        ret->tracks->num_events++;
        continue;
      }
      if (oldev->data.meta.type == MIDI_META_END_OF_TRACK)
      { // reproduce event and break
        nextev->event_type = MIDI_EVENT_META;
        nextev->data.meta.type = MIDI_META_END_OF_TRACK;
        nextev->data.meta.length = 0;
        nextev->data.meta.data = malloc (4);
        epos++;
        ret->tracks->num_events++;
        break;
      }
      // other meta events not needed
      epos++;
      continue;
    }
    // non meta events can simply be copied (excluding delta time)
    memcpy (&nextev->event_type, &oldev->event_type, sizeof (midi_event_t) - sizeof (unsigned));
    epos++;
    ret->tracks->num_events++;
  }

  MIDI_DestroyFlatList (flatlist);
  MIDI_FreeFile (base);
  return ret;
}
