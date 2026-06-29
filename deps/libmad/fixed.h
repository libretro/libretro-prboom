/*
 * libmad - MPEG audio decoder library
 * Copyright (C) 2000-2004 Underbit Technologies, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * $Id: fixed.h,v 1.38 2004/02/17 02:02:03 rob Exp $
 */

# ifndef LIBMAD_FIXED_H
# define LIBMAD_FIXED_H

#include <stdint.h>

typedef signed int mad_fixed_t;

typedef signed int mad_fixed64hi_t;
#if defined(FPM_UNIFIED)
# if defined(_MSC_VER)
typedef signed __int64 mad_fixed64lo_t;
# else
typedef signed long long mad_fixed64lo_t;
# endif
#else
typedef unsigned int mad_fixed64lo_t;
#endif

#if defined(_MSC_VER)
#define mad_fixed64_t  signed __int64
#else
#define mad_fixed64_t  signed long long
#endif

typedef mad_fixed_t mad_sample_t;

/*
 * Fixed-point format: 0xABBBBBBB
 * A == whole part      (sign + 3 bits)
 * B == fractional part (28 bits)
 *
 * Values are signed two's complement, so the effective range is:
 * 0x80000000 to 0x7fffffff
 *       -8.0 to +7.9999999962747097015380859375
 *
 * The smallest representable value is:
 * 0x00000001 == 0.0000000037252902984619140625 (i.e. about 3.725e-9)
 *
 * 28 bits of fractional accuracy represent about
 * 8.6 digits of decimal accuracy.
 *
 * Fixed-point numbers can be added or subtracted as normal
 * integers, but multiplication requires shifting the 64-bit result
 * from 56 fractional bits back to 28 (and rounding.)
 *
 * Changing the definition of MAD_F_FRACBITS is only partially
 * supported, and must be done with care.
 */

# define MAD_F_FRACBITS		28

#  define MAD_F(x)		((mad_fixed_t) (x##L))

# define MAD_F_MIN		((mad_fixed_t) -0x80000000L)
# define MAD_F_MAX		((mad_fixed_t) +0x7fffffffL)

# define MAD_F_ONE		MAD_F(0x10000000)

#if defined(FPM_64BIT)

/*
 * This version should be the most accurate if 64-bit types are supported by
 * the compiler, although it may not be the most efficient.
 */
#define mad_f_mul(x, y) ((mad_fixed_t) (((mad_fixed64_t) (x) * (y)) >> MAD_F_FRACBITS))

/* --- Intel --------------------------------------------------------------- */

# elif defined(FPM_UNIFIED)

/*
 * Portable single-path fixed-point multiply / multiply-accumulate (opt-in).
 *
 * Modern GCC/Clang lower the 64-bit multiply-accumulate idiom below to the
 * architecture's native instruction (ARM smlal, AArch64 smaddl, MIPS madd,
 * x86 imul, PowerPC mullw/mulhw), so this one path replaces the former
 * per-CPU inline-asm variants (FPM_INTEL/FPM_ARM/FPM_MIPS/FPM_SPARC/FPM_PPC)
 * with identical-or-better codegen -- verified across gcc and clang at
 * -O2/-O3 and by end-to-end decode timing.
 *
 * The accumulator is a single signed 64-bit value carried in `lo`; `hi` is
 * unused but kept so the existing (hi, lo) call sites compile unchanged.
 * Accumulation is full 64-bit, scaled once, matching the accuracy of the old
 * asm paths -- more precise than FPM_DEFAULT, at a small CPU cost, which is
 * why FPM_DEFAULT remains the build default.
 */
#  define mad_f_mul(x, y)  \
    ((mad_fixed_t) (((mad_fixed64_t) (x) * (y)) >> MAD_F_FRACBITS))

#  define MAD_F_ML0(hi, lo, x, y)  ((hi) = 0, (lo)  = (mad_fixed64_t) (x) * (y))
#  define MAD_F_MLA(hi, lo, x, y)  ((lo) += (mad_fixed64_t) (x) * (y))
#  define MAD_F_MLN(hi, lo)        ((lo)  = -(lo))
#  if defined(OPT_ACCURACY)
#   define MAD_F_MLZ(hi, lo)  ((void) (hi),  \
      (mad_fixed_t) (((lo) + (((mad_fixed64_t) 1) << (MAD_F_FRACBITS - 1)))  \
                     >> MAD_F_FRACBITS))
#  else
#   define MAD_F_MLZ(hi, lo)  ((void) (hi), (mad_fixed_t) ((lo) >> MAD_F_FRACBITS))
#  endif
#  define mad_f_scale64(hi, lo)  ((void) (hi), (mad_fixed_t) ((lo) >> MAD_F_FRACBITS))


# elif defined(FPM_DEFAULT)

/*
 * This version is the most portable but it loses significant accuracy.
 * Furthermore, accuracy is biased against the second argument, so care
 * should be taken when ordering operands.
 *
 * The scale factors are constant as this is not used with SSO.
 *
 * Pre-rounding is required to stay within the limits of compliance.
 */
#   define mad_f_mul(x, y)	(((x) >> 12) * ((y) >> 16))

/* ------------------------------------------------------------------------- */

# else
#  error "no FPM selected"
# endif

/* default implementations */

# if !defined(mad_f_mul)
#  define mad_f_mul(x, y)  \
    ({ register mad_fixed64hi_t __hi;  \
       register mad_fixed64lo_t __lo;  \
       MAD_F_MLX(__hi, __lo, (x), (y));  \
       mad_f_scale64(__hi, __lo);  \
    })
# endif

# if !defined(MAD_F_MLA)
#  define MAD_F_ML0(hi, lo, x, y)	((lo)  = mad_f_mul((x), (y)))
#  define MAD_F_MLA(hi, lo, x, y)	((lo) += mad_f_mul((x), (y)))
#  define MAD_F_MLN(hi, lo)		((lo)  = -(lo))
#  define MAD_F_MLZ(hi, lo)		((void) (hi), (mad_fixed_t) (lo))
# endif

# if !defined(MAD_F_ML0)
#  define MAD_F_ML0(hi, lo, x, y)	MAD_F_MLX((hi), (lo), (x), (y))
# endif

# if !defined(MAD_F_MLN)
#  define MAD_F_MLN(hi, lo)		((hi) = ((lo) = -(lo)) ? ~(hi) : -(hi))
# endif

# if !defined(MAD_F_MLZ)
#  define MAD_F_MLZ(hi, lo)		mad_f_scale64((hi), (lo))
# endif

# if !defined(mad_f_scale64)
#   define mad_f_scale64(hi, lo)  \
    ((mad_fixed_t)  \
     (((hi) << (32 - MAD_F_FRACBITS)) |  \
      ((lo) >> MAD_F_FRACBITS)))
# endif

# endif
