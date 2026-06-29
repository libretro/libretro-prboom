#ifndef PRBOOM_AUDIO_FLOAT_COMPAT_H
#define PRBOOM_AUDIO_FLOAT_COMPAT_H

/* Forward-compat shim for the float audio-output callback. These are defined
 * by libretro.h once libretro-common is synced; the #ifndef guards make this
 * a no-op against a header that already has them, so it can be removed on the
 * next libretro-common bump. See RETRO_ENVIRONMENT_GET_AUDIO_SAMPLE_BATCH_FLOAT. */
#include <libretro.h>
#include <stddef.h>

#ifndef RETRO_ENVIRONMENT_GET_AUDIO_SAMPLE_BATCH_FLOAT
#define RETRO_ENVIRONMENT_GET_AUDIO_SAMPLE_BATCH_FLOAT (85 | RETRO_ENVIRONMENT_EXPERIMENTAL)
typedef size_t (RETRO_CALLCONV *retro_audio_sample_batch_float_t)(
      const float *data, size_t frames);
struct retro_audio_sample_float_callback
{
   retro_audio_sample_batch_float_t batch;
};
#endif

#endif /* PRBOOM_AUDIO_FLOAT_COMPAT_H */
