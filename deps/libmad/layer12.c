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
 * $Id: layer12.c,v 1.17 2004/02/05 09:02:39 rob Exp $
 */

# include "fixed.h"
# include "bit.h"
# include "stream.h"
# include "frame.h"
# include "layer12.h"

/*
 * scalefactor table
 * used in both Layer I and Layer II decoding
 */
static
mad_fixed_t const sf_table[64] = {
/*
 * These are the scalefactor values for Layer I and Layer II.
 * The values are from Table B.1 of ISO/IEC 11172-3.
 *
 * There is some error introduced by the 32-bit fixed-point representation;
 * the amount of error is shown. For 16-bit PCM output, this shouldn't be
 * too much of a problem.
 *
 * Strictly speaking, Table B.1 has only 63 entries (0-62), thus a strict
 * interpretation of ISO/IEC 11172-3 would suggest that a scalefactor index of
 * 63 is invalid. However, for better compatibility with current practices, we
 * add a 64th entry.
 */

  MAD_F(0x20000000),  /* 2.000000000000 => 2.000000000000, e  0.000000000000 */
  MAD_F(0x1965fea5),  /* 1.587401051968 => 1.587401051074, e  0.000000000894 */
  MAD_F(0x1428a2fa),  /* 1.259921049895 => 1.259921051562, e -0.000000001667 */
  MAD_F(0x10000000),  /* 1.000000000000 => 1.000000000000, e  0.000000000000 */
  MAD_F(0x0cb2ff53),  /* 0.793700525984 => 0.793700527400, e -0.000000001416 */
  MAD_F(0x0a14517d),  /* 0.629960524947 => 0.629960525781, e -0.000000000833 */
  MAD_F(0x08000000),  /* 0.500000000000 => 0.500000000000, e  0.000000000000 */
  MAD_F(0x06597fa9),  /* 0.396850262992 => 0.396850261837, e  0.000000001155 */

  MAD_F(0x050a28be),  /* 0.314980262474 => 0.314980261028, e  0.000000001446 */
  MAD_F(0x04000000),  /* 0.250000000000 => 0.250000000000, e  0.000000000000 */
  MAD_F(0x032cbfd5),  /* 0.198425131496 => 0.198425132781, e -0.000000001285 */
  MAD_F(0x0285145f),  /* 0.157490131237 => 0.157490130514, e  0.000000000723 */
  MAD_F(0x02000000),  /* 0.125000000000 => 0.125000000000, e  0.000000000000 */
  MAD_F(0x01965fea),  /* 0.099212565748 => 0.099212564528, e  0.000000001220 */
  MAD_F(0x01428a30),  /* 0.078745065618 => 0.078745067120, e -0.000000001501 */
  MAD_F(0x01000000),  /* 0.062500000000 => 0.062500000000, e  0.000000000000 */

  MAD_F(0x00cb2ff5),  /* 0.049606282874 => 0.049606282264, e  0.000000000610 */
  MAD_F(0x00a14518),  /* 0.039372532809 => 0.039372533560, e -0.000000000751 */
  MAD_F(0x00800000),  /* 0.031250000000 => 0.031250000000, e  0.000000000000 */
  MAD_F(0x006597fb),  /* 0.024803141437 => 0.024803142995, e -0.000000001558 */
  MAD_F(0x0050a28c),  /* 0.019686266405 => 0.019686266780, e -0.000000000375 */
  MAD_F(0x00400000),  /* 0.015625000000 => 0.015625000000, e  0.000000000000 */
  MAD_F(0x0032cbfd),  /* 0.012401570719 => 0.012401569635, e  0.000000001084 */
  MAD_F(0x00285146),  /* 0.009843133202 => 0.009843133390, e -0.000000000188 */

  MAD_F(0x00200000),  /* 0.007812500000 => 0.007812500000, e  0.000000000000 */
  MAD_F(0x001965ff),  /* 0.006200785359 => 0.006200786680, e -0.000000001321 */
  MAD_F(0x001428a3),  /* 0.004921566601 => 0.004921566695, e -0.000000000094 */
  MAD_F(0x00100000),  /* 0.003906250000 => 0.003906250000, e  0.000000000000 */
  MAD_F(0x000cb2ff),  /* 0.003100392680 => 0.003100391477, e  0.000000001202 */
  MAD_F(0x000a1451),  /* 0.002460783301 => 0.002460781485, e  0.000000001816 */
  MAD_F(0x00080000),  /* 0.001953125000 => 0.001953125000, e  0.000000000000 */
  MAD_F(0x00065980),  /* 0.001550196340 => 0.001550197601, e -0.000000001262 */

  MAD_F(0x00050a29),  /* 0.001230391650 => 0.001230392605, e -0.000000000955 */
  MAD_F(0x00040000),  /* 0.000976562500 => 0.000976562500, e  0.000000000000 */
  MAD_F(0x00032cc0),  /* 0.000775098170 => 0.000775098801, e -0.000000000631 */
  MAD_F(0x00028514),  /* 0.000615195825 => 0.000615194440, e  0.000000001385 */
  MAD_F(0x00020000),  /* 0.000488281250 => 0.000488281250, e  0.000000000000 */
  MAD_F(0x00019660),  /* 0.000387549085 => 0.000387549400, e -0.000000000315 */
  MAD_F(0x0001428a),  /* 0.000307597913 => 0.000307597220, e  0.000000000693 */
  MAD_F(0x00010000),  /* 0.000244140625 => 0.000244140625, e  0.000000000000 */

  MAD_F(0x0000cb30),  /* 0.000193774542 => 0.000193774700, e -0.000000000158 */
  MAD_F(0x0000a145),  /* 0.000153798956 => 0.000153798610, e  0.000000000346 */
  MAD_F(0x00008000),  /* 0.000122070313 => 0.000122070313, e  0.000000000000 */
  MAD_F(0x00006598),  /* 0.000096887271 => 0.000096887350, e -0.000000000079 */
  MAD_F(0x000050a3),  /* 0.000076899478 => 0.000076901168, e -0.000000001689 */
  MAD_F(0x00004000),  /* 0.000061035156 => 0.000061035156, e  0.000000000000 */
  MAD_F(0x000032cc),  /* 0.000048443636 => 0.000048443675, e -0.000000000039 */
  MAD_F(0x00002851),  /* 0.000038449739 => 0.000038448721, e  0.000000001018 */

  MAD_F(0x00002000),  /* 0.000030517578 => 0.000030517578, e  0.000000000000 */
  MAD_F(0x00001966),  /* 0.000024221818 => 0.000024221838, e -0.000000000020 */
  MAD_F(0x00001429),  /* 0.000019224870 => 0.000019226223, e -0.000000001354 */
  MAD_F(0x00001000),  /* 0.000015258789 => 0.000015258789, e -0.000000000000 */
  MAD_F(0x00000cb3),  /* 0.000012110909 => 0.000012110919, e -0.000000000010 */
  MAD_F(0x00000a14),  /* 0.000009612435 => 0.000009611249, e  0.000000001186 */
  MAD_F(0x00000800),  /* 0.000007629395 => 0.000007629395, e -0.000000000000 */
  MAD_F(0x00000659),  /* 0.000006055454 => 0.000006053597, e  0.000000001858 */

  MAD_F(0x0000050a),  /* 0.000004806217 => 0.000004805624, e  0.000000000593 */
  MAD_F(0x00000400),  /* 0.000003814697 => 0.000003814697, e  0.000000000000 */
  MAD_F(0x0000032d),  /* 0.000003027727 => 0.000003028661, e -0.000000000934 */
  MAD_F(0x00000285),  /* 0.000002403109 => 0.000002402812, e  0.000000000296 */
  MAD_F(0x00000200),  /* 0.000001907349 => 0.000001907349, e -0.000000000000 */
  MAD_F(0x00000196),  /* 0.000001513864 => 0.000001512468, e  0.000000001396 */
  MAD_F(0x00000143),  /* 0.000001201554 => 0.000001203269, e -0.000000001714 */
  MAD_F(0x00000000)   /* this compatibility entry is not part of Table B.1 */
};

/* --- Layer I ------------------------------------------------------------- */

/* linear scaling table */
static
mad_fixed_t const linear_table[14] = {
  MAD_F(0x15555555),  /* 2^2  / (2^2  - 1) == 1.33333333333333 */
  MAD_F(0x12492492),  /* 2^3  / (2^3  - 1) == 1.14285714285714 */
  MAD_F(0x11111111),  /* 2^4  / (2^4  - 1) == 1.06666666666667 */
  MAD_F(0x10842108),  /* 2^5  / (2^5  - 1) == 1.03225806451613 */
  MAD_F(0x10410410),  /* 2^6  / (2^6  - 1) == 1.01587301587302 */
  MAD_F(0x10204081),  /* 2^7  / (2^7  - 1) == 1.00787401574803 */
  MAD_F(0x10101010),  /* 2^8  / (2^8  - 1) == 1.00392156862745 */
  MAD_F(0x10080402),  /* 2^9  / (2^9  - 1) == 1.00195694716243 */
  MAD_F(0x10040100),  /* 2^10 / (2^10 - 1) == 1.00097751710655 */
  MAD_F(0x10020040),  /* 2^11 / (2^11 - 1) == 1.00048851978505 */
  MAD_F(0x10010010),  /* 2^12 / (2^12 - 1) == 1.00024420024420 */
  MAD_F(0x10008004),  /* 2^13 / (2^13 - 1) == 1.00012208521548 */
  MAD_F(0x10004001),  /* 2^14 / (2^14 - 1) == 1.00006103888177 */
  MAD_F(0x10002000)   /* 2^15 / (2^15 - 1) == 1.00003051850948 */
};

/*
 * NAME:	I_sample()
 * DESCRIPTION:	decode one requantized Layer I sample from a bitstream
 */
static
mad_fixed_t I_sample(struct mad_bitptr *ptr, unsigned int nb)
{
  mad_fixed_t sample;

  sample = mad_bit_read(ptr, nb);

  /* invert most significant bit, extend sign, then scale to fixed format */

  sample ^= 1 << (nb - 1);
  sample |= -(sample & (1 << (nb - 1)));

  sample <<= MAD_F_FRACBITS - (nb - 1);

  /* requantize the sample */

  /* s'' = (2^nb / (2^nb - 1)) * (s''' + 2^(-nb + 1)) */

  sample += MAD_F_ONE >> (nb - 1);

  return mad_f_mul(sample, linear_table[nb - 2]);

  /* s' = factor * s'' */
  /* (to be performed by caller) */
}

/*
 * NAME:	layer->I()
 * DESCRIPTION:	decode a single Layer I frame
 */
int mad_layer_I(struct mad_stream *stream, struct mad_frame *frame)
{
  struct mad_header *header = &frame->header;
  unsigned int nch, bound, ch, s, sb, nb;
  unsigned char allocation[2][32], scalefactor[2][32];

  nch = MAD_NCHANNELS(header);

  bound = 32;
  if (header->mode == MAD_MODE_JOINT_STEREO) {
    header->flags |= MAD_FLAG_I_STEREO;
    bound = 4 + header->mode_extension * 4;
  }

  /* check CRC word */

  if (header->flags & MAD_FLAG_PROTECTION) {
    header->crc_check =
      mad_bit_crc(stream->ptr, 4 * (bound * nch + (32 - bound)),
		  header->crc_check);

    if (header->crc_check != header->crc_target &&
	!(frame->options & MAD_OPTION_IGNORECRC)) {
      stream->error = MAD_ERROR_BADCRC;
      return -1;
    }
  }

  /* decode bit allocations */

  for (sb = 0; sb < bound; ++sb) {
    for (ch = 0; ch < nch; ++ch) {
      nb = mad_bit_read(&stream->ptr, 4);

      if (nb == 15) {
	stream->error = MAD_ERROR_BADBITALLOC;
	return -1;
      }

      allocation[ch][sb] = nb ? nb + 1 : 0;
    }
  }

  for (sb = bound; sb < 32; ++sb) {
    nb = mad_bit_read(&stream->ptr, 4);

    if (nb == 15) {
      stream->error = MAD_ERROR_BADBITALLOC;
      return -1;
    }

    allocation[0][sb] =
    allocation[1][sb] = nb ? nb + 1 : 0;
  }

  /* decode scalefactors */

  for (sb = 0; sb < 32; ++sb) {
    for (ch = 0; ch < nch; ++ch) {
      if (allocation[ch][sb]) {
	scalefactor[ch][sb] = mad_bit_read(&stream->ptr, 6);
      }
    }
  }

  /* decode samples */

  for (s = 0; s < 12; ++s) {
    for (sb = 0; sb < bound; ++sb) {
      for (ch = 0; ch < nch; ++ch) {
	nb = allocation[ch][sb];
	frame->sbsample[ch][s][sb] = nb ?
	  mad_f_mul(I_sample(&stream->ptr, nb),
		    sf_table[scalefactor[ch][sb]]) : 0;
      }
    }

    for (sb = bound; sb < 32; ++sb) {
      if ((nb = allocation[0][sb])) {
	mad_fixed_t sample;

	sample = I_sample(&stream->ptr, nb);

	for (ch = 0; ch < nch; ++ch) {
	  frame->sbsample[ch][s][sb] =
	    mad_f_mul(sample, sf_table[scalefactor[ch][sb]]);
	}
      }
      else {
	for (ch = 0; ch < nch; ++ch)
	  frame->sbsample[ch][s][sb] = 0;
      }
    }
  }

  return 0;
}

/* --- Layer II ------------------------------------------------------------ */

/* possible quantization per subband table */
static
struct {
  unsigned int sblimit;
  unsigned char const offsets[30];
} const sbquant_table[5] = {
  /* ISO/IEC 11172-3 Table B.2a */
  { 27, { 7, 7, 7, 6, 6, 6, 6, 6, 6, 6, 6, 3, 3, 3, 3, 3,	/* 0 */
	  3, 3, 3, 3, 3, 3, 3, 0, 0, 0, 0 } },
  /* ISO/IEC 11172-3 Table B.2b */
  { 30, { 7, 7, 7, 6, 6, 6, 6, 6, 6, 6, 6, 3, 3, 3, 3, 3,	/* 1 */
	  3, 3, 3, 3, 3, 3, 3, 0, 0, 0, 0, 0, 0, 0 } },
  /* ISO/IEC 11172-3 Table B.2c */
  {  8, { 5, 5, 2, 2, 2, 2, 2, 2 } },				/* 2 */
  /* ISO/IEC 11172-3 Table B.2d */
  { 12, { 5, 5, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2 } },		/* 3 */
  /* ISO/IEC 13818-3 Table B.1 */
  { 30, { 4, 4, 4, 4, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1,	/* 4 */
	  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 } }
};

/* bit allocation table */
static
struct {
  unsigned short nbal;
  unsigned short offset;
} const bitalloc_table[8] = {
  { 2, 0 },  /* 0 */
  { 2, 3 },  /* 1 */
  { 3, 3 },  /* 2 */
  { 3, 1 },  /* 3 */
  { 4, 2 },  /* 4 */
  { 4, 3 },  /* 5 */
  { 4, 4 },  /* 6 */
  { 4, 5 }   /* 7 */
};

/* offsets into quantization class table */
static
unsigned char const offset_table[6][15] = {
  { 0, 1, 16                                             },  /* 0 */
  { 0, 1,  2, 3, 4, 5, 16                                },  /* 1 */
  { 0, 1,  2, 3, 4, 5,  6, 7,  8,  9, 10, 11, 12, 13, 14 },  /* 2 */
  { 0, 1,  3, 4, 5, 6,  7, 8,  9, 10, 11, 12, 13, 14, 15 },  /* 3 */
  { 0, 1,  2, 3, 4, 5,  6, 7,  8,  9, 10, 11, 12, 13, 16 },  /* 4 */
  { 0, 2,  4, 5, 6, 7,  8, 9, 10, 11, 12, 13, 14, 15, 16 }   /* 5 */
};

/* quantization class table */
static
struct quantclass {
  unsigned short nlevels;
  unsigned char group;
  unsigned char bits;
  mad_fixed_t C;
  mad_fixed_t D;
} const qc_table[17] = {
/*
 * These are the Layer II classes of quantization.
 * The table is derived from Table B.4 of ISO/IEC 11172-3.
 */
  {     3, 2,  5,
    MAD_F(0x15555555) /* 1.33333333333 => 1.33333333209, e  0.00000000124 */,
    MAD_F(0x08000000) /* 0.50000000000 => 0.50000000000, e  0.00000000000 */ },
  {     5, 3,  7,
    MAD_F(0x1999999a) /* 1.60000000000 => 1.60000000149, e -0.00000000149 */,
    MAD_F(0x08000000) /* 0.50000000000 => 0.50000000000, e  0.00000000000 */ },
  {     7, 0,  3,
    MAD_F(0x12492492) /* 1.14285714286 => 1.14285714179, e  0.00000000107 */,
    MAD_F(0x04000000) /* 0.25000000000 => 0.25000000000, e  0.00000000000 */ },
  {     9, 4, 10,
    MAD_F(0x1c71c71c) /* 1.77777777777 => 1.77777777612, e  0.00000000165 */,
    MAD_F(0x08000000) /* 0.50000000000 => 0.50000000000, e  0.00000000000 */ },
  {    15, 0,  4,
    MAD_F(0x11111111) /* 1.06666666666 => 1.06666666642, e  0.00000000024 */,
    MAD_F(0x02000000) /* 0.12500000000 => 0.12500000000, e  0.00000000000 */ },
  {    31, 0,  5,
    MAD_F(0x10842108) /* 1.03225806452 => 1.03225806355, e  0.00000000097 */,
    MAD_F(0x01000000) /* 0.06250000000 => 0.06250000000, e  0.00000000000 */ },
  {    63, 0,  6,
    MAD_F(0x10410410) /* 1.01587301587 => 1.01587301493, e  0.00000000094 */,
    MAD_F(0x00800000) /* 0.03125000000 => 0.03125000000, e  0.00000000000 */ },
  {   127, 0,  7,
    MAD_F(0x10204081) /* 1.00787401575 => 1.00787401572, e  0.00000000003 */,
    MAD_F(0x00400000) /* 0.01562500000 => 0.01562500000, e  0.00000000000 */ },
  {   255, 0,  8,
    MAD_F(0x10101010) /* 1.00392156863 => 1.00392156839, e  0.00000000024 */,
    MAD_F(0x00200000) /* 0.00781250000 => 0.00781250000, e  0.00000000000 */ },
  {   511, 0,  9,
    MAD_F(0x10080402) /* 1.00195694716 => 1.00195694715, e  0.00000000001 */,
    MAD_F(0x00100000) /* 0.00390625000 => 0.00390625000, e  0.00000000000 */ },
  {  1023, 0, 10,
    MAD_F(0x10040100) /* 1.00097751711 => 1.00097751617, e  0.00000000094 */,
    MAD_F(0x00080000) /* 0.00195312500 => 0.00195312500, e  0.00000000000 */ },
  {  2047, 0, 11,
    MAD_F(0x10020040) /* 1.00048851979 => 1.00048851967, e  0.00000000012 */,
    MAD_F(0x00040000) /* 0.00097656250 => 0.00097656250, e  0.00000000000 */ },
  {  4095, 0, 12,
    MAD_F(0x10010010) /* 1.00024420024 => 1.00024420023, e  0.00000000001 */,
    MAD_F(0x00020000) /* 0.00048828125 => 0.00048828125, e  0.00000000000 */ },
  {  8191, 0, 13,
    MAD_F(0x10008004) /* 1.00012208522 => 1.00012208521, e  0.00000000001 */,
    MAD_F(0x00010000) /* 0.00024414063 => 0.00024414062, e  0.00000000000 */ },
  { 16383, 0, 14,
    MAD_F(0x10004001) /* 1.00006103888 => 1.00006103888, e -0.00000000000 */,
    MAD_F(0x00008000) /* 0.00012207031 => 0.00012207031, e -0.00000000000 */ },
  { 32767, 0, 15,
    MAD_F(0x10002000) /* 1.00003051851 => 1.00003051758, e  0.00000000093 */,
    MAD_F(0x00004000) /* 0.00006103516 => 0.00006103516, e  0.00000000000 */ },
  { 65535, 0, 16,
    MAD_F(0x10001000) /* 1.00001525902 => 1.00001525879, e  0.00000000023 */,
    MAD_F(0x00002000) /* 0.00003051758 => 0.00003051758, e  0.00000000000 */ }
};

/*
 * NAME:	II_samples()
 * DESCRIPTION:	decode three requantized Layer II samples from a bitstream
 */
static
void II_samples(struct mad_bitptr *ptr,
		struct quantclass const *quantclass,
		mad_fixed_t output[3])
{
  unsigned int nb, s, sample[3];

  if ((nb = quantclass->group)) {
    unsigned int c, nlevels;

    /* degrouping */
    c = mad_bit_read(ptr, quantclass->bits);
    nlevels = quantclass->nlevels;

    for (s = 0; s < 3; ++s) {
      sample[s] = c % nlevels;
      c /= nlevels;
    }
  }
  else {
    nb = quantclass->bits;

    for (s = 0; s < 3; ++s)
      sample[s] = mad_bit_read(ptr, nb);
  }

  for (s = 0; s < 3; ++s) {
    mad_fixed_t requantized;

    /* invert most significant bit, extend sign, then scale to fixed format */

    requantized  = sample[s] ^ (1 << (nb - 1));
    requantized |= -(requantized & (1 << (nb - 1)));

    requantized <<= MAD_F_FRACBITS - (nb - 1);

    /* requantize the sample */

    /* s'' = C * (s''' + D) */

    output[s] = mad_f_mul(requantized + quantclass->D, quantclass->C);

    /* s' = factor * s'' */
    /* (to be performed by caller) */
  }
}

/*
 * NAME:	layer->II()
 * DESCRIPTION:	decode a single Layer II frame
 */
int mad_layer_II(struct mad_stream *stream, struct mad_frame *frame)
{
  struct mad_header *header = &frame->header;
  struct mad_bitptr start;
  unsigned int index, sblimit, nbal, nch, bound, gr, ch, s, sb;
  unsigned char const *offsets;
  unsigned char allocation[2][32], scfsi[2][32], scalefactor[2][32][3];
  mad_fixed_t samples[3];

  nch = MAD_NCHANNELS(header);

  if (header->flags & MAD_FLAG_LSF_EXT)
    index = 4;
  else if (header->flags & MAD_FLAG_FREEFORMAT)
    goto freeformat;
  else {
    unsigned long bitrate_per_channel;

    bitrate_per_channel = header->bitrate;
    if (nch == 2) {
      bitrate_per_channel /= 2;
    }
    else {  /* nch == 1 */
      if (bitrate_per_channel > 192000) {
	/*
	 * ISO/IEC 11172-3 does not allow single channel mode for 224, 256,
	 * 320, or 384 kbps bitrates in Layer II.
	 */
	stream->error = MAD_ERROR_BADMODE;
	return -1;
      }
    }

    if (bitrate_per_channel <= 48000)
      index = (header->samplerate == 32000) ? 3 : 2;
    else if (bitrate_per_channel <= 80000)
      index = 0;
    else {
    freeformat:
      index = (header->samplerate == 48000) ? 0 : 1;
    }
  }

  sblimit = sbquant_table[index].sblimit;
  offsets = sbquant_table[index].offsets;

  bound = 32;
  if (header->mode == MAD_MODE_JOINT_STEREO) {
    header->flags |= MAD_FLAG_I_STEREO;
    bound = 4 + header->mode_extension * 4;
  }

  if (bound > sblimit)
    bound = sblimit;

  start = stream->ptr;

  /* decode bit allocations */

  for (sb = 0; sb < bound; ++sb) {
    nbal = bitalloc_table[offsets[sb]].nbal;

    for (ch = 0; ch < nch; ++ch)
      allocation[ch][sb] = mad_bit_read(&stream->ptr, nbal);
  }

  for (sb = bound; sb < sblimit; ++sb) {
    nbal = bitalloc_table[offsets[sb]].nbal;

    allocation[0][sb] =
    allocation[1][sb] = mad_bit_read(&stream->ptr, nbal);
  }

  /* decode scalefactor selection info */

  for (sb = 0; sb < sblimit; ++sb) {
    for (ch = 0; ch < nch; ++ch) {
      if (allocation[ch][sb])
	scfsi[ch][sb] = mad_bit_read(&stream->ptr, 2);
    }
  }

  /* check CRC word */

  if (header->flags & MAD_FLAG_PROTECTION) {
    header->crc_check =
      mad_bit_crc(start, mad_bit_length(&start, &stream->ptr),
		  header->crc_check);

    if (header->crc_check != header->crc_target &&
	!(frame->options & MAD_OPTION_IGNORECRC)) {
      stream->error = MAD_ERROR_BADCRC;
      return -1;
    }
  }

  /* decode scalefactors */

  for (sb = 0; sb < sblimit; ++sb) {
    for (ch = 0; ch < nch; ++ch) {
      if (allocation[ch][sb]) {
	scalefactor[ch][sb][0] = mad_bit_read(&stream->ptr, 6);

	switch (scfsi[ch][sb]) {
	case 2:
	  scalefactor[ch][sb][2] =
	  scalefactor[ch][sb][1] =
	  scalefactor[ch][sb][0];
	  break;

	case 0:
	  scalefactor[ch][sb][1] = mad_bit_read(&stream->ptr, 6);
	  /* fall through */

	case 1:
	case 3:
	  scalefactor[ch][sb][2] = mad_bit_read(&stream->ptr, 6);
	}

	if (scfsi[ch][sb] & 1)
	  scalefactor[ch][sb][1] = scalefactor[ch][sb][scfsi[ch][sb] - 1];
      }
    }
  }

  /* decode samples */

  for (gr = 0; gr < 12; ++gr) {
    for (sb = 0; sb < bound; ++sb) {
      for (ch = 0; ch < nch; ++ch) {
	if ((index = allocation[ch][sb])) {
	  index = offset_table[bitalloc_table[offsets[sb]].offset][index - 1];

	  II_samples(&stream->ptr, &qc_table[index], samples);

	  for (s = 0; s < 3; ++s) {
	    frame->sbsample[ch][3 * gr + s][sb] =
	      mad_f_mul(samples[s], sf_table[scalefactor[ch][sb][gr / 4]]);
	  }
	}
	else {
	  for (s = 0; s < 3; ++s)
	    frame->sbsample[ch][3 * gr + s][sb] = 0;
	}
      }
    }

    for (sb = bound; sb < sblimit; ++sb) {
      if ((index = allocation[0][sb])) {
	index = offset_table[bitalloc_table[offsets[sb]].offset][index - 1];

	II_samples(&stream->ptr, &qc_table[index], samples);

	for (ch = 0; ch < nch; ++ch) {
	  for (s = 0; s < 3; ++s) {
	    frame->sbsample[ch][3 * gr + s][sb] =
	      mad_f_mul(samples[s], sf_table[scalefactor[ch][sb][gr / 4]]);
	  }
	}
      }
      else {
	for (ch = 0; ch < nch; ++ch) {
	  for (s = 0; s < 3; ++s)
	    frame->sbsample[ch][3 * gr + s][sb] = 0;
	}
      }
    }

    for (ch = 0; ch < nch; ++ch) {
      for (s = 0; s < 3; ++s) {
	for (sb = sblimit; sb < 32; ++sb)
	  frame->sbsample[ch][3 * gr + s][sb] = 0;
      }
    }
  }

  return 0;
}
