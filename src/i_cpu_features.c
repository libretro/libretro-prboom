/* i_cpu_features.c: minimal cpu_features_get() for the vendored
 * libretro-common decoders (rjpeg, encoding_crc32, encoding_deflate,
 * rvorbis/rmp3 SIMD lanes).
 *
 * The consumers only consult the returned SIMD mask to pick kernels; none
 * of the timing/topology surface of the full libretro-common features_cpu.c
 * is needed, so that file -- with its platform timing code and many
 * platform headers -- is still not pulled into this core.
 *
 * What changed against the earlier compile-time-only stub: encoding_crc32
 * dispatches its PCLMUL folding path on RETRO_SIMD_PCLMUL and (on ARM
 * builds without compile-time +crc) its CRC32-instruction path on
 * RETRO_SIMD_CRC32, and encoding_deflate keys its hash on RETRO_SIMD_SSE42.
 * All three are runtime properties of the machine, not of the compiler
 * flags, so a compile-time mask left those paths permanently dark on the
 * generic x86-64 builds this core ships as.  Probe once with CPUID (or the
 * ARM compile-time macros, which is what the distributed ARM builds key
 * on anyway) and cache the answer; the probe is branch-free after the
 * first call and safe from any thread (worst case both write the same
 * value). */

#include <stdint.h>

#include <libretro.h>                 /* RETRO_SIMD_* bit definitions */
#include <features/features_cpu.h>    /* cpu_features_get prototype   */

#if (defined(__i386__) || defined(__x86_64__) || defined(_M_IX86) || defined(_M_X64)) \
    && !defined(GEKKO) && !defined(__CELLOS_LV2__)
#define PRB_CPU_X86 1
#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif
#endif

static uint64_t prb_cpu_flags;     /* probed mask, always non-zero once set */
static int      prb_cpu_probed;

#ifdef PRB_CPU_X86
static void prb_cpuid(uint32_t leaf, uint32_t out[4])
{
#if defined(_MSC_VER) && !defined(__clang__)
   int regs[4];
   __cpuid(regs, (int)leaf);
   out[0] = (uint32_t)regs[0]; out[1] = (uint32_t)regs[1];
   out[2] = (uint32_t)regs[2]; out[3] = (uint32_t)regs[3];
#else
   uint32_t a, b, c, d;
   __asm__ __volatile__(
#if defined(__i386__) && defined(__PIC__)
      /* ebx is the PIC register on i386; preserve it around cpuid */
      "xchgl %%ebx, %1\n\tcpuid\n\txchgl %%ebx, %1"
      : "=a"(a), "=r"(b), "=c"(c), "=d"(d)
#else
      "cpuid"
      : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
#endif
      : "a"(leaf), "c"(0));
   out[0] = a; out[1] = b; out[2] = c; out[3] = d;
#endif
}
#endif

uint64_t cpu_features_get(void)
{
   uint64_t flags;

   if (prb_cpu_probed)
      return prb_cpu_flags;

   flags = 0;

#ifdef PRB_CPU_X86
   {
      uint32_t r[4];
      prb_cpuid(0, r);
      if (r[0] >= 1)
      {
         uint32_t ecx, edx;
         prb_cpuid(1, r);
         ecx = r[2];
         edx = r[3];
         if (edx & (1u << 26)) flags |= RETRO_SIMD_SSE2;
         if (ecx & (1u <<  0)) flags |= RETRO_SIMD_SSE3;
         if (ecx & (1u <<  9)) flags |= RETRO_SIMD_SSSE3;
         if (ecx & (1u << 19)) flags |= RETRO_SIMD_SSE4;
         if (ecx & (1u << 20)) flags |= RETRO_SIMD_SSE42;
         if (ecx & (1u <<  1)) flags |= RETRO_SIMD_PCLMUL;
      }
   }
#else
   /* Non-x86: the distributed builds select ARM kernels through
    * compile-time target macros, so mirror those here. */
#if defined(__ARM_NEON) || defined(__ARM_NEON__) || defined(_M_ARM64) || defined(__aarch64__)
   flags |= RETRO_SIMD_NEON;
#endif
#if defined(__ARM_FEATURE_CRC32)
   flags |= RETRO_SIMD_CRC32;
#endif
#if defined(__SSE2__)
   flags |= RETRO_SIMD_SSE2;   /* x86 targets that fell through the guard */
#endif
#endif

   /* Never store 0: a probe that found nothing still counts as done. */
   prb_cpu_flags  = flags ? flags : (uint64_t)1 << 63;
   prb_cpu_probed = 1;
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
