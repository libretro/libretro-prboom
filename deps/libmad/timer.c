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
 * $Id: timer.c,v 1.18 2004/01/23 09:41:33 rob Exp $
 */

# include <stdio.h>

# include "timer.h"

mad_timer_t const mad_timer_zero = { 0, 0 };

/*
 * NAME:	gcd()
 * DESCRIPTION:	compute greatest common denominator
 */
static
unsigned long gcd(unsigned long num1, unsigned long num2)
{
  unsigned long tmp;

  while (num2) {
    tmp  = num2;
    num2 = num1 % num2;
    num1 = tmp;
  }

  return num1;
}

/*
 * NAME:	reduce_rational()
 * DESCRIPTION:	convert rational expression to lowest terms
 */
static
void reduce_rational(unsigned long *numer, unsigned long *denom)
{
  unsigned long factor = gcd(*numer, *denom);

  *numer /= factor;
  *denom /= factor;
}

/*
 * NAME:	scale_rational()
 * DESCRIPTION:	solve numer/denom == ?/scale avoiding overflowing
 */
static
unsigned long scale_rational(unsigned long numer, unsigned long denom,
			     unsigned long scale)
{
  reduce_rational(&numer, &denom);
  reduce_rational(&scale, &denom);

  if (denom < scale)
    return numer * (scale / denom) + numer * (scale % denom) / denom;
  if (denom < numer)
    return scale * (numer / denom) + scale * (numer % denom) / denom;

  return numer * scale / denom;
}

/*
 * NAME:	timer->set()
 * DESCRIPTION:	set timer to specific (positive) value
 */
void mad_timer_set(mad_timer_t *timer, unsigned long seconds,
		   unsigned long numer, unsigned long denom)
{
  timer->seconds = seconds;
  if (numer >= denom && denom > 0) {
    timer->seconds += numer / denom;
    numer %= denom;
  }

  switch (denom) {
  case 0:
  case 1:
    timer->fraction = 0;
    break;

  case MAD_TIMER_RESOLUTION:
    timer->fraction = numer;
    break;

  case 1000:
    timer->fraction = numer * (MAD_TIMER_RESOLUTION /  1000);
    break;

  case 8000:
    timer->fraction = numer * (MAD_TIMER_RESOLUTION /  8000);
    break;

  case 11025:
    timer->fraction = numer * (MAD_TIMER_RESOLUTION / 11025);
    break;

  case 12000:
    timer->fraction = numer * (MAD_TIMER_RESOLUTION / 12000);
    break;

  case 16000:
    timer->fraction = numer * (MAD_TIMER_RESOLUTION / 16000);
    break;

  case 22050:
    timer->fraction = numer * (MAD_TIMER_RESOLUTION / 22050);
    break;

  case 24000:
    timer->fraction = numer * (MAD_TIMER_RESOLUTION / 24000);
    break;

  case 32000:
    timer->fraction = numer * (MAD_TIMER_RESOLUTION / 32000);
    break;

  case 44100:
    timer->fraction = numer * (MAD_TIMER_RESOLUTION / 44100);
    break;

  case 48000:
    timer->fraction = numer * (MAD_TIMER_RESOLUTION / 48000);
    break;

  default:
    timer->fraction = scale_rational(numer, denom, MAD_TIMER_RESOLUTION);
    break;
  }

  if (timer->fraction >= MAD_TIMER_RESOLUTION)
  {
	  timer->seconds  += timer->fraction / MAD_TIMER_RESOLUTION;
	  timer->fraction %= MAD_TIMER_RESOLUTION;
  }
}
