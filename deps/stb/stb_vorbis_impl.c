/* Single translation unit that compiles stb_vorbis with only the in-memory
 * decode path enabled.  The sound loader hands it a whole Ogg lump already
 * resident in memory and wants interleaved 16-bit PCM back, so the file I/O
 * (stdio), streaming (pushdata) and comment-parsing halves of stb_vorbis are
 * switched off to keep the added object small.  stb_vorbis_decode_memory lives
 * in the pulldata/integer-conversion path, which stays enabled.
 *
 * Every public stb_vorbis symbol is renamed with a prb_ prefix below.  A
 * statically linked RetroArch already exports the stock stb_vorbis_* names
 * from its own audio mixer (libretro-common/audio/audio_mixer), so an
 * unprefixed copy here collides at link time (duplicate symbol).  The prefix
 * keeps this core's decoder private to the core.  The only entry point the
 * core actually calls is prb_stb_vorbis_decode_memory (see libretro_sound.c);
 * the rest are renamed too so none of them can clash either. */
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_COMMENTS

#define stb_vorbis_get_info                     prb_stb_vorbis_get_info
#define stb_vorbis_get_comment                  prb_stb_vorbis_get_comment
#define stb_vorbis_get_error                    prb_stb_vorbis_get_error
#define stb_vorbis_close                        prb_stb_vorbis_close
#define stb_vorbis_get_sample_offset            prb_stb_vorbis_get_sample_offset
#define stb_vorbis_get_file_offset              prb_stb_vorbis_get_file_offset
#define stb_vorbis_open_pushdata                prb_stb_vorbis_open_pushdata
#define stb_vorbis_decode_frame_pushdata        prb_stb_vorbis_decode_frame_pushdata
#define stb_vorbis_flush_pushdata               prb_stb_vorbis_flush_pushdata
#define stb_vorbis_decode_filename              prb_stb_vorbis_decode_filename
#define stb_vorbis_decode_memory                prb_stb_vorbis_decode_memory
#define stb_vorbis_open_memory                  prb_stb_vorbis_open_memory
#define stb_vorbis_open_filename                prb_stb_vorbis_open_filename
#define stb_vorbis_open_file                    prb_stb_vorbis_open_file
#define stb_vorbis_open_file_section            prb_stb_vorbis_open_file_section
#define stb_vorbis_seek_frame                   prb_stb_vorbis_seek_frame
#define stb_vorbis_seek                         prb_stb_vorbis_seek
#define stb_vorbis_seek_start                   prb_stb_vorbis_seek_start
#define stb_vorbis_stream_length_in_samples     prb_stb_vorbis_stream_length_in_samples
#define stb_vorbis_stream_length_in_seconds     prb_stb_vorbis_stream_length_in_seconds
#define stb_vorbis_get_frame_float              prb_stb_vorbis_get_frame_float
#define stb_vorbis_get_frame_short_interleaved  prb_stb_vorbis_get_frame_short_interleaved
#define stb_vorbis_get_frame_short              prb_stb_vorbis_get_frame_short
#define stb_vorbis_get_samples_float_interleaved prb_stb_vorbis_get_samples_float_interleaved
#define stb_vorbis_get_samples_float            prb_stb_vorbis_get_samples_float
#define stb_vorbis_get_samples_short_interleaved prb_stb_vorbis_get_samples_short_interleaved
#define stb_vorbis_get_samples_short            prb_stb_vorbis_get_samples_short

#include "stb_vorbis.c"
