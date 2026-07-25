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

/* cpu_features_get_core_amount: libretro-common declares it in
 * features/features_cpu.h but the full features_cpu.c is not vendored (see
 * above).  The threaded wall replay only needs it to size "Auto", so a small
 * portable query is enough; anything unknown answers 1, which selects the
 * single-threaded path. */
#if defined(_WIN32)
#include <windows.h>
#elif defined(HAVE_UNISTD_H) || defined(__unix__) || defined(__APPLE__) || defined(__linux__)
#include <unistd.h>
#endif

unsigned cpu_features_get_core_amount(void)
{
#if defined(_WIN32)
   SYSTEM_INFO si;
   GetSystemInfo(&si);
   return (unsigned)si.dwNumberOfProcessors;
#elif defined(_SC_NPROCESSORS_ONLN)
   long n = sysconf(_SC_NPROCESSORS_ONLN);
   return n > 0 ? (unsigned)n : 1;
#else
   return 1;
#endif
}
