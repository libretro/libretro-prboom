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
 * $Id: bit.h,v 1.12 2004/01/23 09:41:32 rob Exp $
 */

# ifndef LIBMAD_BIT_H
# define LIBMAD_BIT_H

#include <stdint.h>

struct mad_bitptr {
  uint8_t const *byte;
  uint16_t cache;
  uint16_t left;
};

void mad_bit_init(struct mad_bitptr *, unsigned char const *);

unsigned int mad_bit_length(struct mad_bitptr const *,
			    struct mad_bitptr const *);

unsigned char const *mad_bit_nextbyte(struct mad_bitptr const *);

void mad_bit_skip(struct mad_bitptr *, unsigned int);
unsigned long mad_bit_read(struct mad_bitptr *, unsigned int);
void mad_bit_write(struct mad_bitptr *, unsigned int, unsigned long);

uint16_t mad_bit_crc(struct mad_bitptr a, unsigned int b, uint16_t c);

# endif
