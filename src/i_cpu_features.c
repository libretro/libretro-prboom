/* i_cpu_features.c: minimal cpu_features_get() for the vendored rjpeg
 * decoder (libretro/libretro-common/formats/jpeg/rjpeg.c).
 *
 * rjpeg only consults the returned SIMD mask to pick its IDCT and colour
 * kernels.  A compile-time mask matches exactly the instruction set the
 * decoder was actually built for: the SSE2 kernels are compiled (and the
 * mask set) only when __SSE2__ is defined, otherwise the scalar path runs.
 * That is all rjpeg needs here, so the full libretro-common features_cpu.c
 * -- with its platform timing code and many platform headers -- is not
 * pulled into this core just to decode a handful of JPEG textures at level
 * load.  (ARM builds select the NEON kernels through rjpeg's own RJPEG_NEON
 * compile guards, independent of this mask.)
 */

#include <stdint.h>

#include <libretro.h>                 /* RETRO_SIMD_* bit definitions */
#include <features/features_cpu.h>    /* cpu_features_get prototype   */

uint64_t cpu_features_get(void)
{
   uint64_t flags = 0;
#if defined(__SSE2__)
   flags |= RETRO_SIMD_SSE2;
#endif
   return flags;
}
