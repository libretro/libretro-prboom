/* Single translation unit that compiles stb_vorbis with only the in-memory
 * decode path enabled.  The sound loader hands it a whole Ogg lump already
 * resident in memory and wants interleaved 16-bit PCM back, so the file I/O
 * (stdio) and streaming (pushdata) halves of stb_vorbis are switched off to
 * keep the added object small.  stb_vorbis_decode_memory lives in the
 * pulldata/integer-conversion path, which stays enabled. */
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_NO_COMMENTS
#include "stb_vorbis.c"
