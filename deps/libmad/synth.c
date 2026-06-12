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
 * $Id: synth.c,v 1.25 2004/01/23 09:41:33 rob Exp $
 */

# include "fixed.h"
# include "frame.h"
# include "synth.h"

/*
 * NAME:	synth->init()
 * DESCRIPTION:	initialize synth struct
 */
void mad_synth_init(struct mad_synth *synth)
{
  mad_synth_mute(synth);

  synth->phase = 0;

  synth->pcm.samplerate = 0;
  synth->pcm.channels   = 0;
  synth->pcm.length     = 0;
}

/*
 * NAME:	synth->mute()
 * DESCRIPTION:	zero all polyphase filterbank values, resetting synthesis
 */
void mad_synth_mute(struct mad_synth *synth)
{
  unsigned int ch, s, v;

  for (ch = 0; ch < 2; ++ch) {
    for (s = 0; s < 16; ++s) {
      for (v = 0; v < 8; ++v) {
	synth->filter[ch][0][0][s][v] = synth->filter[ch][0][1][s][v] =
	synth->filter[ch][1][0][s][v] = synth->filter[ch][1][1][s][v] = 0;
      }
    }
  }
}

/*
 * An optional optimization called here the Subband Synthesis Optimization
 * (SSO) improves the performance of subband synthesis at the expense of
 * accuracy.
 *
 * The idea is to simplify 32x32->64-bit multiplication to 32x32->32 such
 * that extra scaling and rounding are not necessary. This often allows the
 * compiler to use faster 32-bit multiply-accumulate instructions instead of
 * explicit 64-bit multiply, shift, and add instructions.
 *
 * SSO works like this: a full 32x32->64-bit multiply of two mad_fixed_t
 * values requires the result to be right-shifted 28 bits to be properly
 * scaled to the same fixed-point format. Right shifts can be applied at any
 * time to either operand or to the result, so the optimization involves
 * careful placement of these shifts to minimize the loss of accuracy.
 *
 * First, a 14-bit shift is applied with rounding at compile-time to the D[]
 * table of coefficients for the subband synthesis window. This only loses 2
 * bits of accuracy because the lower 12 bits are always zero. A second
 * 12-bit shift occurs after the DCT calculation. This loses 12 bits of
 * accuracy. Finally, a third 2-bit shift occurs just before the sample is
 * saved in the PCM buffer. 14 + 12 + 2 == 28 bits.
 */

/* FPM_DEFAULT without OPT_SSO will actually lose accuracy and performance */

/* second SSO shift, with rounding */

#  define SHIFT(x)  (((x) + (1L << 11)) >> 12)

/* possible DCT speed optimization */

# if defined(MAD_F_MLX)
#  define OPT_DCTO
#  define MUL(x, y)  \
    ({ mad_fixed64hi_t hi;  \
       mad_fixed64lo_t lo;  \
       MAD_F_MLX(hi, lo, (x), (y));  \
       hi << (32 - MAD_F_FRACBITS - 3);  \
    })
# else
#  undef OPT_DCTO
#  define MUL(x, y)  mad_f_mul((x), (y))
# endif

/*
 * NAME:	dct32()
 * DESCRIPTION:	perform fast in[32]->out[32] DCT
 */
static
void dct32(mad_fixed_t const in[32], unsigned int slot,
	   mad_fixed_t lo[16][8], mad_fixed_t hi[16][8])
{
  mad_fixed_t t0,   t1,   t2,   t3,   t4,   t5,   t6,   t7;
  mad_fixed_t t8,   t9,   t10,  t11,  t12,  t13,  t14,  t15;
  mad_fixed_t t16,  t17,  t18,  t19,  t20,  t21,  t22,  t23;
  mad_fixed_t t24,  t25,  t26,  t27,  t28,  t29,  t30,  t31;
  mad_fixed_t t32,  t33,  t34,  t35,  t36,  t37,  t38,  t39;
  mad_fixed_t t40,  t41,  t42,  t43,  t44,  t45,  t46,  t47;
  mad_fixed_t t48,  t49,  t50,  t51,  t52,  t53,  t54,  t55;
  mad_fixed_t t56,  t57,  t58,  t59,  t60,  t61,  t62,  t63;
  mad_fixed_t t64,  t65,  t66,  t67,  t68,  t69,  t70,  t71;
  mad_fixed_t t72,  t73,  t74,  t75,  t76,  t77,  t78,  t79;
  mad_fixed_t t80,  t81,  t82,  t83,  t84,  t85,  t86,  t87;
  mad_fixed_t t88,  t89,  t90,  t91,  t92,  t93,  t94,  t95;
  mad_fixed_t t96,  t97,  t98,  t99,  t100, t101, t102, t103;
  mad_fixed_t t104, t105, t106, t107, t108, t109, t110, t111;
  mad_fixed_t t112, t113, t114, t115, t116, t117, t118, t119;
  mad_fixed_t t120, t121, t122, t123, t124, t125, t126, t127;
  mad_fixed_t t128, t129, t130, t131, t132, t133, t134, t135;
  mad_fixed_t t136, t137, t138, t139, t140, t141, t142, t143;
  mad_fixed_t t144, t145, t146, t147, t148, t149, t150, t151;
  mad_fixed_t t152, t153, t154, t155, t156, t157, t158, t159;
  mad_fixed_t t160, t161, t162, t163, t164, t165, t166, t167;
  mad_fixed_t t168, t169, t170, t171, t172, t173, t174, t175;
  mad_fixed_t t176;

  /* costab[i] = cos(PI / (2 * 32) * i) */

# if defined(OPT_DCTO)
#  define costab1	MAD_F(0x7fd8878e)
#  define costab2	MAD_F(0x7f62368f)
#  define costab3	MAD_F(0x7e9d55fc)
#  define costab4	MAD_F(0x7d8a5f40)
#  define costab5	MAD_F(0x7c29fbee)
#  define costab6	MAD_F(0x7a7d055b)
#  define costab7	MAD_F(0x78848414)
#  define costab8	MAD_F(0x7641af3d)
#  define costab9	MAD_F(0x73b5ebd1)
#  define costab10	MAD_F(0x70e2cbc6)
#  define costab11	MAD_F(0x6dca0d14)
#  define costab12	MAD_F(0x6a6d98a4)
#  define costab13	MAD_F(0x66cf8120)
#  define costab14	MAD_F(0x62f201ac)
#  define costab15	MAD_F(0x5ed77c8a)
#  define costab16	MAD_F(0x5a82799a)
#  define costab17	MAD_F(0x55f5a4d2)
#  define costab18	MAD_F(0x5133cc94)
#  define costab19	MAD_F(0x4c3fdff4)
#  define costab20	MAD_F(0x471cece7)
#  define costab21	MAD_F(0x41ce1e65)
#  define costab22	MAD_F(0x3c56ba70)
#  define costab23	MAD_F(0x36ba2014)
#  define costab24	MAD_F(0x30fbc54d)
#  define costab25	MAD_F(0x2b1f34eb)
#  define costab26	MAD_F(0x25280c5e)
#  define costab27	MAD_F(0x1f19f97b)
#  define costab28	MAD_F(0x18f8b83c)
#  define costab29	MAD_F(0x12c8106f)
#  define costab30	MAD_F(0x0c8bd35e)
#  define costab31	MAD_F(0x0647d97c)
# else
#  define costab1	MAD_F(0x0ffb10f2)  /* 0.998795456 */
#  define costab2	MAD_F(0x0fec46d2)  /* 0.995184727 */
#  define costab3	MAD_F(0x0fd3aac0)  /* 0.989176510 */
#  define costab4	MAD_F(0x0fb14be8)  /* 0.980785280 */
#  define costab5	MAD_F(0x0f853f7e)  /* 0.970031253 */
#  define costab6	MAD_F(0x0f4fa0ab)  /* 0.956940336 */
#  define costab7	MAD_F(0x0f109082)  /* 0.941544065 */
#  define costab8	MAD_F(0x0ec835e8)  /* 0.923879533 */
#  define costab9	MAD_F(0x0e76bd7a)  /* 0.903989293 */
#  define costab10	MAD_F(0x0e1c5979)  /* 0.881921264 */
#  define costab11	MAD_F(0x0db941a3)  /* 0.857728610 */
#  define costab12	MAD_F(0x0d4db315)  /* 0.831469612 */
#  define costab13	MAD_F(0x0cd9f024)  /* 0.803207531 */
#  define costab14	MAD_F(0x0c5e4036)  /* 0.773010453 */
#  define costab15	MAD_F(0x0bdaef91)  /* 0.740951125 */
#  define costab16	MAD_F(0x0b504f33)  /* 0.707106781 */
#  define costab17	MAD_F(0x0abeb49a)  /* 0.671558955 */
#  define costab18	MAD_F(0x0a267993)  /* 0.634393284 */
#  define costab19	MAD_F(0x0987fbfe)  /* 0.595699304 */
#  define costab20	MAD_F(0x08e39d9d)  /* 0.555570233 */
#  define costab21	MAD_F(0x0839c3cd)  /* 0.514102744 */
#  define costab22	MAD_F(0x078ad74e)  /* 0.471396737 */
#  define costab23	MAD_F(0x06d74402)  /* 0.427555093 */
#  define costab24	MAD_F(0x061f78aa)  /* 0.382683432 */
#  define costab25	MAD_F(0x0563e69d)  /* 0.336889853 */
#  define costab26	MAD_F(0x04a5018c)  /* 0.290284677 */
#  define costab27	MAD_F(0x03e33f2f)  /* 0.242980180 */
#  define costab28	MAD_F(0x031f1708)  /* 0.195090322 */
#  define costab29	MAD_F(0x0259020e)  /* 0.146730474 */
#  define costab30	MAD_F(0x01917a6c)  /* 0.098017140 */
#  define costab31	MAD_F(0x00c8fb30)  /* 0.049067674 */
# endif

  t0   = in[0]  + in[31];  t16  = MUL(in[0]  - in[31], costab1);
  t1   = in[15] + in[16];  t17  = MUL(in[15] - in[16], costab31);

  t41  = t16 + t17;
  t59  = MUL(t16 - t17, costab2);
  t33  = t0  + t1;
  t50  = MUL(t0  - t1,  costab2);

  t2   = in[7]  + in[24];  t18  = MUL(in[7]  - in[24], costab15);
  t3   = in[8]  + in[23];  t19  = MUL(in[8]  - in[23], costab17);

  t42  = t18 + t19;
  t60  = MUL(t18 - t19, costab30);
  t34  = t2  + t3;
  t51  = MUL(t2  - t3,  costab30);

  t4   = in[3]  + in[28];  t20  = MUL(in[3]  - in[28], costab7);
  t5   = in[12] + in[19];  t21  = MUL(in[12] - in[19], costab25);

  t43  = t20 + t21;
  t61  = MUL(t20 - t21, costab14);
  t35  = t4  + t5;
  t52  = MUL(t4  - t5,  costab14);

  t6   = in[4]  + in[27];  t22  = MUL(in[4]  - in[27], costab9);
  t7   = in[11] + in[20];  t23  = MUL(in[11] - in[20], costab23);

  t44  = t22 + t23;
  t62  = MUL(t22 - t23, costab18);
  t36  = t6  + t7;
  t53  = MUL(t6  - t7,  costab18);

  t8   = in[1]  + in[30];  t24  = MUL(in[1]  - in[30], costab3);
  t9   = in[14] + in[17];  t25  = MUL(in[14] - in[17], costab29);

  t45  = t24 + t25;
  t63  = MUL(t24 - t25, costab6);
  t37  = t8  + t9;
  t54  = MUL(t8  - t9,  costab6);

  t10  = in[6]  + in[25];  t26  = MUL(in[6]  - in[25], costab13);
  t11  = in[9]  + in[22];  t27  = MUL(in[9]  - in[22], costab19);

  t46  = t26 + t27;
  t64  = MUL(t26 - t27, costab26);
  t38  = t10 + t11;
  t55  = MUL(t10 - t11, costab26);

  t12  = in[2]  + in[29];  t28  = MUL(in[2]  - in[29], costab5);
  t13  = in[13] + in[18];  t29  = MUL(in[13] - in[18], costab27);

  t47  = t28 + t29;
  t65  = MUL(t28 - t29, costab10);
  t39  = t12 + t13;
  t56  = MUL(t12 - t13, costab10);

  t14  = in[5]  + in[26];  t30  = MUL(in[5]  - in[26], costab11);
  t15  = in[10] + in[21];  t31  = MUL(in[10] - in[21], costab21);

  t48  = t30 + t31;
  t66  = MUL(t30 - t31, costab22);
  t40  = t14 + t15;
  t57  = MUL(t14 - t15, costab22);

  t69  = t33 + t34;  t89  = MUL(t33 - t34, costab4);
  t70  = t35 + t36;  t90  = MUL(t35 - t36, costab28);
  t71  = t37 + t38;  t91  = MUL(t37 - t38, costab12);
  t72  = t39 + t40;  t92  = MUL(t39 - t40, costab20);
  t73  = t41 + t42;  t94  = MUL(t41 - t42, costab4);
  t74  = t43 + t44;  t95  = MUL(t43 - t44, costab28);
  t75  = t45 + t46;  t96  = MUL(t45 - t46, costab12);
  t76  = t47 + t48;  t97  = MUL(t47 - t48, costab20);

  t78  = t50 + t51;  t100 = MUL(t50 - t51, costab4);
  t79  = t52 + t53;  t101 = MUL(t52 - t53, costab28);
  t80  = t54 + t55;  t102 = MUL(t54 - t55, costab12);
  t81  = t56 + t57;  t103 = MUL(t56 - t57, costab20);

  t83  = t59 + t60;  t106 = MUL(t59 - t60, costab4);
  t84  = t61 + t62;  t107 = MUL(t61 - t62, costab28);
  t85  = t63 + t64;  t108 = MUL(t63 - t64, costab12);
  t86  = t65 + t66;  t109 = MUL(t65 - t66, costab20);

  t113 = t69  + t70;
  t114 = t71  + t72;

  /*  0 */ hi[15][slot] = SHIFT(t113 + t114);
  /* 16 */ lo[ 0][slot] = SHIFT(MUL(t113 - t114, costab16));

  t115 = t73  + t74;
  t116 = t75  + t76;

  t32  = t115 + t116;

  /*  1 */ hi[14][slot] = SHIFT(t32);

  t118 = t78  + t79;
  t119 = t80  + t81;

  t58  = t118 + t119;

  /*  2 */ hi[13][slot] = SHIFT(t58);

  t121 = t83  + t84;
  t122 = t85  + t86;

  t67  = t121 + t122;

  t49  = (t67 * 2) - t32;

  /*  3 */ hi[12][slot] = SHIFT(t49);

  t125 = t89  + t90;
  t126 = t91  + t92;

  t93  = t125 + t126;

  /*  4 */ hi[11][slot] = SHIFT(t93);

  t128 = t94  + t95;
  t129 = t96  + t97;

  t98  = t128 + t129;

  t68  = (t98 * 2) - t49;

  /*  5 */ hi[10][slot] = SHIFT(t68);

  t132 = t100 + t101;
  t133 = t102 + t103;

  t104 = t132 + t133;

  t82  = (t104 * 2) - t58;

  /*  6 */ hi[ 9][slot] = SHIFT(t82);

  t136 = t106 + t107;
  t137 = t108 + t109;

  t110 = t136 + t137;

  t87  = (t110 * 2) - t67;

  t77  = (t87 * 2) - t68;

  /*  7 */ hi[ 8][slot] = SHIFT(t77);

  t141 = MUL(t69 - t70, costab8);
  t142 = MUL(t71 - t72, costab24);
  t143 = t141 + t142;

  /*  8 */ hi[ 7][slot] = SHIFT(t143);
  /* 24 */ lo[ 8][slot] =
	     SHIFT((MUL(t141 - t142, costab16) * 2) - t143);

  t144 = MUL(t73 - t74, costab8);
  t145 = MUL(t75 - t76, costab24);
  t146 = t144 + t145;

  t88  = (t146 * 2) - t77;

  /*  9 */ hi[ 6][slot] = SHIFT(t88);

  t148 = MUL(t78 - t79, costab8);
  t149 = MUL(t80 - t81, costab24);
  t150 = t148 + t149;

  t105 = (t150 * 2) - t82;

  /* 10 */ hi[ 5][slot] = SHIFT(t105);

  t152 = MUL(t83 - t84, costab8);
  t153 = MUL(t85 - t86, costab24);
  t154 = t152 + t153;

  t111 = (t154 * 2) - t87;

  t99  = (t111 * 2) - t88;

  /* 11 */ hi[ 4][slot] = SHIFT(t99);

  t157 = MUL(t89 - t90, costab8);
  t158 = MUL(t91 - t92, costab24);
  t159 = t157 + t158;

  t127 = (t159 * 2) - t93;

  /* 12 */ hi[ 3][slot] = SHIFT(t127);

  t160 = (MUL(t125 - t126, costab16) * 2) - t127;

  /* 20 */ lo[ 4][slot] = SHIFT(t160);
  /* 28 */ lo[12][slot] =
	     SHIFT((((MUL(t157 - t158, costab16) * 2) - t159) * 2) - t160);

  t161 = MUL(t94 - t95, costab8);
  t162 = MUL(t96 - t97, costab24);
  t163 = t161 + t162;

  t130 = (t163 * 2) - t98;

  t112 = (t130 * 2) - t99;

  /* 13 */ hi[ 2][slot] = SHIFT(t112);

  t164 = (MUL(t128 - t129, costab16) * 2) - t130;

  t166 = MUL(t100 - t101, costab8);
  t167 = MUL(t102 - t103, costab24);
  t168 = t166 + t167;

  t134 = (t168 * 2) - t104;

  t120 = (t134 * 2) - t105;

  /* 14 */ hi[ 1][slot] = SHIFT(t120);

  t135 = (MUL(t118 - t119, costab16) * 2) - t120;

  /* 18 */ lo[ 2][slot] = SHIFT(t135);

  t169 = (MUL(t132 - t133, costab16) * 2) - t134;

  t151 = (t169 * 2) - t135;

  /* 22 */ lo[ 6][slot] = SHIFT(t151);

  t170 = (((MUL(t148 - t149, costab16) * 2) - t150) * 2) - t151;

  /* 26 */ lo[10][slot] = SHIFT(t170);
  /* 30 */ lo[14][slot] =
	     SHIFT((((((MUL(t166 - t167, costab16) * 2) -
		       t168) * 2) - t169) * 2) - t170);

  t171 = MUL(t106 - t107, costab8);
  t172 = MUL(t108 - t109, costab24);
  t173 = t171 + t172;

  t138 = (t173 * 2) - t110;

  t123 = (t138 * 2) - t111;

  t139 = (MUL(t121 - t122, costab16) * 2) - t123;

  t117 = (t123 * 2) - t112;

  /* 15 */ hi[ 0][slot] = SHIFT(t117);

  t124 = (MUL(t115 - t116, costab16) * 2) - t117;

  /* 17 */ lo[ 1][slot] = SHIFT(t124);

  t131 = (t139 * 2) - t124;

  /* 19 */ lo[ 3][slot] = SHIFT(t131);

  t140 = (t164 * 2) - t131;

  /* 21 */ lo[ 5][slot] = SHIFT(t140);

  t174 = (MUL(t136 - t137, costab16) * 2) - t138;

  t155 = (t174 * 2) - t139;

  t147 = (t155 * 2) - t140;

  /* 23 */ lo[ 7][slot] = SHIFT(t147);

  t156 = (((MUL(t144 - t145, costab16) * 2) - t146) * 2) - t147;

  /* 25 */ lo[ 9][slot] = SHIFT(t156);

  t175 = (((MUL(t152 - t153, costab16) * 2) - t154) * 2) - t155;

  t165 = (t175 * 2) - t156;

  /* 27 */ lo[11][slot] = SHIFT(t165);

  t176 = (((((MUL(t161 - t162, costab16) * 2) -
	     t163) * 2) - t164) * 2) - t165;

  /* 29 */ lo[13][slot] = SHIFT(t176);
  /* 31 */ lo[15][slot] =
	     SHIFT((((((((MUL(t171 - t172, costab16) * 2) -
			 t173) * 2) - t174) * 2) - t175) * 2) - t176);

  /*
   * Totals:
   *  80 multiplies
   *  80 additions
   * 119 subtractions
   *  49 shifts (not counting SSO)
   */
}

# undef MUL
# undef SHIFT

/* third SSO shift and/or D[] optimization preshift */

#  define ML0(hi, lo, x, y)	((lo)  = (x) * (y))
#  define MLA(hi, lo, x, y)	((lo) += (x) * (y))
#  define MLN(hi, lo)		((lo)  = -(lo))
#  define MLZ(hi, lo)		((void) (hi), (mad_fixed_t) (lo))
#  define SHIFT(x)		((x) >> 2)
#  define PRESHIFT(x)		((MAD_F(x) + (1L << 13)) >> 14)

static
mad_fixed_t const D[17][32] = {
/*
 * These are the coefficients for the subband synthesis window. This is a
 * reordered version of Table B.3 from ISO/IEC 11172-3.
 *
 * Every value is parameterized so that shift optimizations can be made at
 * compile-time. For example, every value can be right-shifted 12 bits to
 * minimize multiply instruction times without any loss of accuracy.
 */

  {  PRESHIFT(0x00000000) /*  0.000000000 */,	/*  0 */
    -PRESHIFT(0x0001d000) /* -0.000442505 */,
     PRESHIFT(0x000d5000) /*  0.003250122 */,
    -PRESHIFT(0x001cb000) /* -0.007003784 */,
     PRESHIFT(0x007f5000) /*  0.031082153 */,
    -PRESHIFT(0x01421000) /* -0.078628540 */,
     PRESHIFT(0x019ae000) /*  0.100311279 */,
    -PRESHIFT(0x09271000) /* -0.572036743 */,
     PRESHIFT(0x1251e000) /*  1.144989014 */,
     PRESHIFT(0x09271000) /*  0.572036743 */,
     PRESHIFT(0x019ae000) /*  0.100311279 */,
     PRESHIFT(0x01421000) /*  0.078628540 */,
     PRESHIFT(0x007f5000) /*  0.031082153 */,
     PRESHIFT(0x001cb000) /*  0.007003784 */,
     PRESHIFT(0x000d5000) /*  0.003250122 */,
     PRESHIFT(0x0001d000) /*  0.000442505 */,

     PRESHIFT(0x00000000) /*  0.000000000 */,
    -PRESHIFT(0x0001d000) /* -0.000442505 */,
     PRESHIFT(0x000d5000) /*  0.003250122 */,
    -PRESHIFT(0x001cb000) /* -0.007003784 */,
     PRESHIFT(0x007f5000) /*  0.031082153 */,
    -PRESHIFT(0x01421000) /* -0.078628540 */,
     PRESHIFT(0x019ae000) /*  0.100311279 */,
    -PRESHIFT(0x09271000) /* -0.572036743 */,
     PRESHIFT(0x1251e000) /*  1.144989014 */,
     PRESHIFT(0x09271000) /*  0.572036743 */,
     PRESHIFT(0x019ae000) /*  0.100311279 */,
     PRESHIFT(0x01421000) /*  0.078628540 */,
     PRESHIFT(0x007f5000) /*  0.031082153 */,
     PRESHIFT(0x001cb000) /*  0.007003784 */,
     PRESHIFT(0x000d5000) /*  0.003250122 */,
     PRESHIFT(0x0001d000) /*  0.000442505 */ },

  { -PRESHIFT(0x00001000) /* -0.000015259 */,	/*  1 */
    -PRESHIFT(0x0001f000) /* -0.000473022 */,
     PRESHIFT(0x000da000) /*  0.003326416 */,
    -PRESHIFT(0x00207000) /* -0.007919312 */,
     PRESHIFT(0x007d0000) /*  0.030517578 */,
    -PRESHIFT(0x0158d000) /* -0.084182739 */,
     PRESHIFT(0x01747000) /*  0.090927124 */,
    -PRESHIFT(0x099a8000) /* -0.600219727 */,
     PRESHIFT(0x124f0000) /*  1.144287109 */,
     PRESHIFT(0x08b38000) /*  0.543823242 */,
     PRESHIFT(0x01bde000) /*  0.108856201 */,
     PRESHIFT(0x012b4000) /*  0.073059082 */,
     PRESHIFT(0x0080f000) /*  0.031478882 */,
     PRESHIFT(0x00191000) /*  0.006118774 */,
     PRESHIFT(0x000d0000) /*  0.003173828 */,
     PRESHIFT(0x0001a000) /*  0.000396729 */,

    -PRESHIFT(0x00001000) /* -0.000015259 */,
    -PRESHIFT(0x0001f000) /* -0.000473022 */,
     PRESHIFT(0x000da000) /*  0.003326416 */,
    -PRESHIFT(0x00207000) /* -0.007919312 */,
     PRESHIFT(0x007d0000) /*  0.030517578 */,
    -PRESHIFT(0x0158d000) /* -0.084182739 */,
     PRESHIFT(0x01747000) /*  0.090927124 */,
    -PRESHIFT(0x099a8000) /* -0.600219727 */,
     PRESHIFT(0x124f0000) /*  1.144287109 */,
     PRESHIFT(0x08b38000) /*  0.543823242 */,
     PRESHIFT(0x01bde000) /*  0.108856201 */,
     PRESHIFT(0x012b4000) /*  0.073059082 */,
     PRESHIFT(0x0080f000) /*  0.031478882 */,
     PRESHIFT(0x00191000) /*  0.006118774 */,
     PRESHIFT(0x000d0000) /*  0.003173828 */,
     PRESHIFT(0x0001a000) /*  0.000396729 */ },

  { -PRESHIFT(0x00001000) /* -0.000015259 */,	/*  2 */
    -PRESHIFT(0x00023000) /* -0.000534058 */,
     PRESHIFT(0x000de000) /*  0.003387451 */,
    -PRESHIFT(0x00245000) /* -0.008865356 */,
     PRESHIFT(0x007a0000) /*  0.029785156 */,
    -PRESHIFT(0x016f7000) /* -0.089706421 */,
     PRESHIFT(0x014a8000) /*  0.080688477 */,
    -PRESHIFT(0x0a0d8000) /* -0.628295898 */,
     PRESHIFT(0x12468000) /*  1.142211914 */,
     PRESHIFT(0x083ff000) /*  0.515609741 */,
     PRESHIFT(0x01dd8000) /*  0.116577148 */,
     PRESHIFT(0x01149000) /*  0.067520142 */,
     PRESHIFT(0x00820000) /*  0.031738281 */,
     PRESHIFT(0x0015b000) /*  0.005294800 */,
     PRESHIFT(0x000ca000) /*  0.003082275 */,
     PRESHIFT(0x00018000) /*  0.000366211 */,

    -PRESHIFT(0x00001000) /* -0.000015259 */,
    -PRESHIFT(0x00023000) /* -0.000534058 */,
     PRESHIFT(0x000de000) /*  0.003387451 */,
    -PRESHIFT(0x00245000) /* -0.008865356 */,
     PRESHIFT(0x007a0000) /*  0.029785156 */,
    -PRESHIFT(0x016f7000) /* -0.089706421 */,
     PRESHIFT(0x014a8000) /*  0.080688477 */,
    -PRESHIFT(0x0a0d8000) /* -0.628295898 */,
     PRESHIFT(0x12468000) /*  1.142211914 */,
     PRESHIFT(0x083ff000) /*  0.515609741 */,
     PRESHIFT(0x01dd8000) /*  0.116577148 */,
     PRESHIFT(0x01149000) /*  0.067520142 */,
     PRESHIFT(0x00820000) /*  0.031738281 */,
     PRESHIFT(0x0015b000) /*  0.005294800 */,
     PRESHIFT(0x000ca000) /*  0.003082275 */,
     PRESHIFT(0x00018000) /*  0.000366211 */ },

  { -PRESHIFT(0x00001000) /* -0.000015259 */,	/*  3 */
    -PRESHIFT(0x00026000) /* -0.000579834 */,
     PRESHIFT(0x000e1000) /*  0.003433228 */,
    -PRESHIFT(0x00285000) /* -0.009841919 */,
     PRESHIFT(0x00765000) /*  0.028884888 */,
    -PRESHIFT(0x0185d000) /* -0.095169067 */,
     PRESHIFT(0x011d1000) /*  0.069595337 */,
    -PRESHIFT(0x0a7fe000) /* -0.656219482 */,
     PRESHIFT(0x12386000) /*  1.138763428 */,
     PRESHIFT(0x07ccb000) /*  0.487472534 */,
     PRESHIFT(0x01f9c000) /*  0.123474121 */,
     PRESHIFT(0x00fdf000) /*  0.061996460 */,
     PRESHIFT(0x00827000) /*  0.031845093 */,
     PRESHIFT(0x00126000) /*  0.004486084 */,
     PRESHIFT(0x000c4000) /*  0.002990723 */,
     PRESHIFT(0x00015000) /*  0.000320435 */,

    -PRESHIFT(0x00001000) /* -0.000015259 */,
    -PRESHIFT(0x00026000) /* -0.000579834 */,
     PRESHIFT(0x000e1000) /*  0.003433228 */,
    -PRESHIFT(0x00285000) /* -0.009841919 */,
     PRESHIFT(0x00765000) /*  0.028884888 */,
    -PRESHIFT(0x0185d000) /* -0.095169067 */,
     PRESHIFT(0x011d1000) /*  0.069595337 */,
    -PRESHIFT(0x0a7fe000) /* -0.656219482 */,
     PRESHIFT(0x12386000) /*  1.138763428 */,
     PRESHIFT(0x07ccb000) /*  0.487472534 */,
     PRESHIFT(0x01f9c000) /*  0.123474121 */,
     PRESHIFT(0x00fdf000) /*  0.061996460 */,
     PRESHIFT(0x00827000) /*  0.031845093 */,
     PRESHIFT(0x00126000) /*  0.004486084 */,
     PRESHIFT(0x000c4000) /*  0.002990723 */,
     PRESHIFT(0x00015000) /*  0.000320435 */ },

  { -PRESHIFT(0x00001000) /* -0.000015259 */,	/*  4 */
    -PRESHIFT(0x00029000) /* -0.000625610 */,
     PRESHIFT(0x000e3000) /*  0.003463745 */,
    -PRESHIFT(0x002c7000) /* -0.010848999 */,
     PRESHIFT(0x0071e000) /*  0.027801514 */,
    -PRESHIFT(0x019bd000) /* -0.100540161 */,
     PRESHIFT(0x00ec0000) /*  0.057617187 */,
    -PRESHIFT(0x0af15000) /* -0.683914185 */,
     PRESHIFT(0x12249000) /*  1.133926392 */,
     PRESHIFT(0x075a0000) /*  0.459472656 */,
     PRESHIFT(0x0212c000) /*  0.129577637 */,
     PRESHIFT(0x00e79000) /*  0.056533813 */,
     PRESHIFT(0x00825000) /*  0.031814575 */,
     PRESHIFT(0x000f4000) /*  0.003723145 */,
     PRESHIFT(0x000be000) /*  0.002899170 */,
     PRESHIFT(0x00013000) /*  0.000289917 */,

    -PRESHIFT(0x00001000) /* -0.000015259 */,
    -PRESHIFT(0x00029000) /* -0.000625610 */,
     PRESHIFT(0x000e3000) /*  0.003463745 */,
    -PRESHIFT(0x002c7000) /* -0.010848999 */,
     PRESHIFT(0x0071e000) /*  0.027801514 */,
    -PRESHIFT(0x019bd000) /* -0.100540161 */,
     PRESHIFT(0x00ec0000) /*  0.057617187 */,
    -PRESHIFT(0x0af15000) /* -0.683914185 */,
     PRESHIFT(0x12249000) /*  1.133926392 */,
     PRESHIFT(0x075a0000) /*  0.459472656 */,
     PRESHIFT(0x0212c000) /*  0.129577637 */,
     PRESHIFT(0x00e79000) /*  0.056533813 */,
     PRESHIFT(0x00825000) /*  0.031814575 */,
     PRESHIFT(0x000f4000) /*  0.003723145 */,
     PRESHIFT(0x000be000) /*  0.002899170 */,
     PRESHIFT(0x00013000) /*  0.000289917 */ },

  { -PRESHIFT(0x00001000) /* -0.000015259 */,	/*  5 */
    -PRESHIFT(0x0002d000) /* -0.000686646 */,
     PRESHIFT(0x000e4000) /*  0.003479004 */,
    -PRESHIFT(0x0030b000) /* -0.011886597 */,
     PRESHIFT(0x006cb000) /*  0.026535034 */,
    -PRESHIFT(0x01b17000) /* -0.105819702 */,
     PRESHIFT(0x00b77000) /*  0.044784546 */,
    -PRESHIFT(0x0b619000) /* -0.711318970 */,
     PRESHIFT(0x120b4000) /*  1.127746582 */,
     PRESHIFT(0x06e81000) /*  0.431655884 */,
     PRESHIFT(0x02288000) /*  0.134887695 */,
     PRESHIFT(0x00d17000) /*  0.051132202 */,
     PRESHIFT(0x0081b000) /*  0.031661987 */,
     PRESHIFT(0x000c5000) /*  0.003005981 */,
     PRESHIFT(0x000b7000) /*  0.002792358 */,
     PRESHIFT(0x00011000) /*  0.000259399 */,

    -PRESHIFT(0x00001000) /* -0.000015259 */,
    -PRESHIFT(0x0002d000) /* -0.000686646 */,
     PRESHIFT(0x000e4000) /*  0.003479004 */,
    -PRESHIFT(0x0030b000) /* -0.011886597 */,
     PRESHIFT(0x006cb000) /*  0.026535034 */,
    -PRESHIFT(0x01b17000) /* -0.105819702 */,
     PRESHIFT(0x00b77000) /*  0.044784546 */,
    -PRESHIFT(0x0b619000) /* -0.711318970 */,
     PRESHIFT(0x120b4000) /*  1.127746582 */,
     PRESHIFT(0x06e81000) /*  0.431655884 */,
     PRESHIFT(0x02288000) /*  0.134887695 */,
     PRESHIFT(0x00d17000) /*  0.051132202 */,
     PRESHIFT(0x0081b000) /*  0.031661987 */,
     PRESHIFT(0x000c5000) /*  0.003005981 */,
     PRESHIFT(0x000b7000) /*  0.002792358 */,
     PRESHIFT(0x00011000) /*  0.000259399 */ },

  { -PRESHIFT(0x00001000) /* -0.000015259 */,	/*  6 */
    -PRESHIFT(0x00031000) /* -0.000747681 */,
     PRESHIFT(0x000e4000) /*  0.003479004 */,
    -PRESHIFT(0x00350000) /* -0.012939453 */,
     PRESHIFT(0x0066c000) /*  0.025085449 */,
    -PRESHIFT(0x01c67000) /* -0.110946655 */,
     PRESHIFT(0x007f5000) /*  0.031082153 */,
    -PRESHIFT(0x0bd06000) /* -0.738372803 */,
     PRESHIFT(0x11ec7000) /*  1.120223999 */,
     PRESHIFT(0x06772000) /*  0.404083252 */,
     PRESHIFT(0x023b3000) /*  0.139450073 */,
     PRESHIFT(0x00bbc000) /*  0.045837402 */,
     PRESHIFT(0x00809000) /*  0.031387329 */,
     PRESHIFT(0x00099000) /*  0.002334595 */,
     PRESHIFT(0x000b0000) /*  0.002685547 */,
     PRESHIFT(0x00010000) /*  0.000244141 */,

    -PRESHIFT(0x00001000) /* -0.000015259 */,
    -PRESHIFT(0x00031000) /* -0.000747681 */,
     PRESHIFT(0x000e4000) /*  0.003479004 */,
    -PRESHIFT(0x00350000) /* -0.012939453 */,
     PRESHIFT(0x0066c000) /*  0.025085449 */,
    -PRESHIFT(0x01c67000) /* -0.110946655 */,
     PRESHIFT(0x007f5000) /*  0.031082153 */,
    -PRESHIFT(0x0bd06000) /* -0.738372803 */,
     PRESHIFT(0x11ec7000) /*  1.120223999 */,
     PRESHIFT(0x06772000) /*  0.404083252 */,
     PRESHIFT(0x023b3000) /*  0.139450073 */,
     PRESHIFT(0x00bbc000) /*  0.045837402 */,
     PRESHIFT(0x00809000) /*  0.031387329 */,
     PRESHIFT(0x00099000) /*  0.002334595 */,
     PRESHIFT(0x000b0000) /*  0.002685547 */,
     PRESHIFT(0x00010000) /*  0.000244141 */ },

  { -PRESHIFT(0x00002000) /* -0.000030518 */,	/*  7 */
    -PRESHIFT(0x00035000) /* -0.000808716 */,
     PRESHIFT(0x000e3000) /*  0.003463745 */,
    -PRESHIFT(0x00397000) /* -0.014022827 */,
     PRESHIFT(0x005ff000) /*  0.023422241 */,
    -PRESHIFT(0x01dad000) /* -0.115921021 */,
     PRESHIFT(0x0043a000) /*  0.016510010 */,
    -PRESHIFT(0x0c3d9000) /* -0.765029907 */,
     PRESHIFT(0x11c83000) /*  1.111373901 */,
     PRESHIFT(0x06076000) /*  0.376800537 */,
     PRESHIFT(0x024ad000) /*  0.143264771 */,
     PRESHIFT(0x00a67000) /*  0.040634155 */,
     PRESHIFT(0x007f0000) /*  0.031005859 */,
     PRESHIFT(0x0006f000) /*  0.001693726 */,
     PRESHIFT(0x000a9000) /*  0.002578735 */,
     PRESHIFT(0x0000e000) /*  0.000213623 */,

    -PRESHIFT(0x00002000) /* -0.000030518 */,
    -PRESHIFT(0x00035000) /* -0.000808716 */,
     PRESHIFT(0x000e3000) /*  0.003463745 */,
    -PRESHIFT(0x00397000) /* -0.014022827 */,
     PRESHIFT(0x005ff000) /*  0.023422241 */,
    -PRESHIFT(0x01dad000) /* -0.115921021 */,
     PRESHIFT(0x0043a000) /*  0.016510010 */,
    -PRESHIFT(0x0c3d9000) /* -0.765029907 */,
     PRESHIFT(0x11c83000) /*  1.111373901 */,
     PRESHIFT(0x06076000) /*  0.376800537 */,
     PRESHIFT(0x024ad000) /*  0.143264771 */,
     PRESHIFT(0x00a67000) /*  0.040634155 */,
     PRESHIFT(0x007f0000) /*  0.031005859 */,
     PRESHIFT(0x0006f000) /*  0.001693726 */,
     PRESHIFT(0x000a9000) /*  0.002578735 */,
     PRESHIFT(0x0000e000) /*  0.000213623 */ },

  { -PRESHIFT(0x00002000) /* -0.000030518 */,	/*  8 */
    -PRESHIFT(0x0003a000) /* -0.000885010 */,
     PRESHIFT(0x000e0000) /*  0.003417969 */,
    -PRESHIFT(0x003df000) /* -0.015121460 */,
     PRESHIFT(0x00586000) /*  0.021575928 */,
    -PRESHIFT(0x01ee6000) /* -0.120697021 */,
     PRESHIFT(0x00046000) /*  0.001068115 */,
    -PRESHIFT(0x0ca8d000) /* -0.791213989 */,
     PRESHIFT(0x119e9000) /*  1.101211548 */,
     PRESHIFT(0x05991000) /*  0.349868774 */,
     PRESHIFT(0x02578000) /*  0.146362305 */,
     PRESHIFT(0x0091a000) /*  0.035552979 */,
     PRESHIFT(0x007d1000) /*  0.030532837 */,
     PRESHIFT(0x00048000) /*  0.001098633 */,
     PRESHIFT(0x000a1000) /*  0.002456665 */,
     PRESHIFT(0x0000d000) /*  0.000198364 */,

    -PRESHIFT(0x00002000) /* -0.000030518 */,
    -PRESHIFT(0x0003a000) /* -0.000885010 */,
     PRESHIFT(0x000e0000) /*  0.003417969 */,
    -PRESHIFT(0x003df000) /* -0.015121460 */,
     PRESHIFT(0x00586000) /*  0.021575928 */,
    -PRESHIFT(0x01ee6000) /* -0.120697021 */,
     PRESHIFT(0x00046000) /*  0.001068115 */,
    -PRESHIFT(0x0ca8d000) /* -0.791213989 */,
     PRESHIFT(0x119e9000) /*  1.101211548 */,
     PRESHIFT(0x05991000) /*  0.349868774 */,
     PRESHIFT(0x02578000) /*  0.146362305 */,
     PRESHIFT(0x0091a000) /*  0.035552979 */,
     PRESHIFT(0x007d1000) /*  0.030532837 */,
     PRESHIFT(0x00048000) /*  0.001098633 */,
     PRESHIFT(0x000a1000) /*  0.002456665 */,
     PRESHIFT(0x0000d000) /*  0.000198364 */ },

  { -PRESHIFT(0x00002000) /* -0.000030518 */,	/*  9 */
    -PRESHIFT(0x0003f000) /* -0.000961304 */,
     PRESHIFT(0x000dd000) /*  0.003372192 */,
    -PRESHIFT(0x00428000) /* -0.016235352 */,
     PRESHIFT(0x00500000) /*  0.019531250 */,
    -PRESHIFT(0x02011000) /* -0.125259399 */,
    -PRESHIFT(0x003e6000) /* -0.015228271 */,
    -PRESHIFT(0x0d11e000) /* -0.816864014 */,
     PRESHIFT(0x116fc000) /*  1.089782715 */,
     PRESHIFT(0x052c5000) /*  0.323318481 */,
     PRESHIFT(0x02616000) /*  0.148773193 */,
     PRESHIFT(0x007d6000) /*  0.030609131 */,
     PRESHIFT(0x007aa000) /*  0.029937744 */,
     PRESHIFT(0x00024000) /*  0.000549316 */,
     PRESHIFT(0x0009a000) /*  0.002349854 */,
     PRESHIFT(0x0000b000) /*  0.000167847 */,

    -PRESHIFT(0x00002000) /* -0.000030518 */,
    -PRESHIFT(0x0003f000) /* -0.000961304 */,
     PRESHIFT(0x000dd000) /*  0.003372192 */,
    -PRESHIFT(0x00428000) /* -0.016235352 */,
     PRESHIFT(0x00500000) /*  0.019531250 */,
    -PRESHIFT(0x02011000) /* -0.125259399 */,
    -PRESHIFT(0x003e6000) /* -0.015228271 */,
    -PRESHIFT(0x0d11e000) /* -0.816864014 */,
     PRESHIFT(0x116fc000) /*  1.089782715 */,
     PRESHIFT(0x052c5000) /*  0.323318481 */,
     PRESHIFT(0x02616000) /*  0.148773193 */,
     PRESHIFT(0x007d6000) /*  0.030609131 */,
     PRESHIFT(0x007aa000) /*  0.029937744 */,
     PRESHIFT(0x00024000) /*  0.000549316 */,
     PRESHIFT(0x0009a000) /*  0.002349854 */,
     PRESHIFT(0x0000b000) /*  0.000167847 */ },

  { -PRESHIFT(0x00002000) /* -0.000030518 */,	/* 10 */
    -PRESHIFT(0x00044000) /* -0.001037598 */,
     PRESHIFT(0x000d7000) /*  0.003280640 */,
    -PRESHIFT(0x00471000) /* -0.017349243 */,
     PRESHIFT(0x0046b000) /*  0.017257690 */,
    -PRESHIFT(0x0212b000) /* -0.129562378 */,
    -PRESHIFT(0x0084a000) /* -0.032379150 */,
    -PRESHIFT(0x0d78a000) /* -0.841949463 */,
     PRESHIFT(0x113be000) /*  1.077117920 */,
     PRESHIFT(0x04c16000) /*  0.297210693 */,
     PRESHIFT(0x02687000) /*  0.150497437 */,
     PRESHIFT(0x0069c000) /*  0.025817871 */,
     PRESHIFT(0x0077f000) /*  0.029281616 */,
     PRESHIFT(0x00002000) /*  0.000030518 */,
     PRESHIFT(0x00093000) /*  0.002243042 */,
     PRESHIFT(0x0000a000) /*  0.000152588 */,

    -PRESHIFT(0x00002000) /* -0.000030518 */,
    -PRESHIFT(0x00044000) /* -0.001037598 */,
     PRESHIFT(0x000d7000) /*  0.003280640 */,
    -PRESHIFT(0x00471000) /* -0.017349243 */,
     PRESHIFT(0x0046b000) /*  0.017257690 */,
    -PRESHIFT(0x0212b000) /* -0.129562378 */,
    -PRESHIFT(0x0084a000) /* -0.032379150 */,
    -PRESHIFT(0x0d78a000) /* -0.841949463 */,
     PRESHIFT(0x113be000) /*  1.077117920 */,
     PRESHIFT(0x04c16000) /*  0.297210693 */,
     PRESHIFT(0x02687000) /*  0.150497437 */,
     PRESHIFT(0x0069c000) /*  0.025817871 */,
     PRESHIFT(0x0077f000) /*  0.029281616 */,
     PRESHIFT(0x00002000) /*  0.000030518 */,
     PRESHIFT(0x00093000) /*  0.002243042 */,
     PRESHIFT(0x0000a000) /*  0.000152588 */ },

  { -PRESHIFT(0x00003000) /* -0.000045776 */,	/* 11 */
    -PRESHIFT(0x00049000) /* -0.001113892 */,
     PRESHIFT(0x000d0000) /*  0.003173828 */,
    -PRESHIFT(0x004ba000) /* -0.018463135 */,
     PRESHIFT(0x003ca000) /*  0.014801025 */,
    -PRESHIFT(0x02233000) /* -0.133590698 */,
    -PRESHIFT(0x00ce4000) /* -0.050354004 */,
    -PRESHIFT(0x0ddca000) /* -0.866363525 */,
     PRESHIFT(0x1102f000) /*  1.063217163 */,
     PRESHIFT(0x04587000) /*  0.271591187 */,
     PRESHIFT(0x026cf000) /*  0.151596069 */,
     PRESHIFT(0x0056c000) /*  0.021179199 */,
     PRESHIFT(0x0074e000) /*  0.028533936 */,
    -PRESHIFT(0x0001d000) /* -0.000442505 */,
     PRESHIFT(0x0008b000) /*  0.002120972 */,
     PRESHIFT(0x00009000) /*  0.000137329 */,

    -PRESHIFT(0x00003000) /* -0.000045776 */,
    -PRESHIFT(0x00049000) /* -0.001113892 */,
     PRESHIFT(0x000d0000) /*  0.003173828 */,
    -PRESHIFT(0x004ba000) /* -0.018463135 */,
     PRESHIFT(0x003ca000) /*  0.014801025 */,
    -PRESHIFT(0x02233000) /* -0.133590698 */,
    -PRESHIFT(0x00ce4000) /* -0.050354004 */,
    -PRESHIFT(0x0ddca000) /* -0.866363525 */,
     PRESHIFT(0x1102f000) /*  1.063217163 */,
     PRESHIFT(0x04587000) /*  0.271591187 */,
     PRESHIFT(0x026cf000) /*  0.151596069 */,
     PRESHIFT(0x0056c000) /*  0.021179199 */,
     PRESHIFT(0x0074e000) /*  0.028533936 */,
    -PRESHIFT(0x0001d000) /* -0.000442505 */,
     PRESHIFT(0x0008b000) /*  0.002120972 */,
     PRESHIFT(0x00009000) /*  0.000137329 */ },

  { -PRESHIFT(0x00003000) /* -0.000045776 */,	/* 12 */
    -PRESHIFT(0x0004f000) /* -0.001205444 */,
     PRESHIFT(0x000c8000) /*  0.003051758 */,
    -PRESHIFT(0x00503000) /* -0.019577026 */,
     PRESHIFT(0x0031a000) /*  0.012115479 */,
    -PRESHIFT(0x02326000) /* -0.137298584 */,
    -PRESHIFT(0x011b5000) /* -0.069168091 */,
    -PRESHIFT(0x0e3dd000) /* -0.890090942 */,
     PRESHIFT(0x10c54000) /*  1.048156738 */,
     PRESHIFT(0x03f1b000) /*  0.246505737 */,
     PRESHIFT(0x026ee000) /*  0.152069092 */,
     PRESHIFT(0x00447000) /*  0.016708374 */,
     PRESHIFT(0x00719000) /*  0.027725220 */,
    -PRESHIFT(0x00039000) /* -0.000869751 */,
     PRESHIFT(0x00084000) /*  0.002014160 */,
     PRESHIFT(0x00008000) /*  0.000122070 */,

    -PRESHIFT(0x00003000) /* -0.000045776 */,
    -PRESHIFT(0x0004f000) /* -0.001205444 */,
     PRESHIFT(0x000c8000) /*  0.003051758 */,
    -PRESHIFT(0x00503000) /* -0.019577026 */,
     PRESHIFT(0x0031a000) /*  0.012115479 */,
    -PRESHIFT(0x02326000) /* -0.137298584 */,
    -PRESHIFT(0x011b5000) /* -0.069168091 */,
    -PRESHIFT(0x0e3dd000) /* -0.890090942 */,
     PRESHIFT(0x10c54000) /*  1.048156738 */,
     PRESHIFT(0x03f1b000) /*  0.246505737 */,
     PRESHIFT(0x026ee000) /*  0.152069092 */,
     PRESHIFT(0x00447000) /*  0.016708374 */,
     PRESHIFT(0x00719000) /*  0.027725220 */,
    -PRESHIFT(0x00039000) /* -0.000869751 */,
     PRESHIFT(0x00084000) /*  0.002014160 */,
     PRESHIFT(0x00008000) /*  0.000122070 */ },

  { -PRESHIFT(0x00004000) /* -0.000061035 */,	/* 13 */
    -PRESHIFT(0x00055000) /* -0.001296997 */,
     PRESHIFT(0x000bd000) /*  0.002883911 */,
    -PRESHIFT(0x0054c000) /* -0.020690918 */,
     PRESHIFT(0x0025d000) /*  0.009231567 */,
    -PRESHIFT(0x02403000) /* -0.140670776 */,
    -PRESHIFT(0x016ba000) /* -0.088775635 */,
    -PRESHIFT(0x0e9be000) /* -0.913055420 */,
     PRESHIFT(0x1082d000) /*  1.031936646 */,
     PRESHIFT(0x038d4000) /*  0.221984863 */,
     PRESHIFT(0x026e7000) /*  0.151962280 */,
     PRESHIFT(0x0032e000) /*  0.012420654 */,
     PRESHIFT(0x006df000) /*  0.026840210 */,
    -PRESHIFT(0x00053000) /* -0.001266479 */,
     PRESHIFT(0x0007d000) /*  0.001907349 */,
     PRESHIFT(0x00007000) /*  0.000106812 */,

    -PRESHIFT(0x00004000) /* -0.000061035 */,
    -PRESHIFT(0x00055000) /* -0.001296997 */,
     PRESHIFT(0x000bd000) /*  0.002883911 */,
    -PRESHIFT(0x0054c000) /* -0.020690918 */,
     PRESHIFT(0x0025d000) /*  0.009231567 */,
    -PRESHIFT(0x02403000) /* -0.140670776 */,
    -PRESHIFT(0x016ba000) /* -0.088775635 */,
    -PRESHIFT(0x0e9be000) /* -0.913055420 */,
     PRESHIFT(0x1082d000) /*  1.031936646 */,
     PRESHIFT(0x038d4000) /*  0.221984863 */,
     PRESHIFT(0x026e7000) /*  0.151962280 */,
     PRESHIFT(0x0032e000) /*  0.012420654 */,
     PRESHIFT(0x006df000) /*  0.026840210 */,
    -PRESHIFT(0x00053000) /* -0.001266479 */,
     PRESHIFT(0x0007d000) /*  0.001907349 */,
     PRESHIFT(0x00007000) /*  0.000106812 */ },

  { -PRESHIFT(0x00004000) /* -0.000061035 */,	/* 14 */
    -PRESHIFT(0x0005b000) /* -0.001388550 */,
     PRESHIFT(0x000b1000) /*  0.002700806 */,
    -PRESHIFT(0x00594000) /* -0.021789551 */,
     PRESHIFT(0x00192000) /*  0.006134033 */,
    -PRESHIFT(0x024c8000) /* -0.143676758 */,
    -PRESHIFT(0x01bf2000) /* -0.109161377 */,
    -PRESHIFT(0x0ef69000) /* -0.935195923 */,
     PRESHIFT(0x103be000) /*  1.014617920 */,
     PRESHIFT(0x032b4000) /*  0.198059082 */,
     PRESHIFT(0x026bc000) /*  0.151306152 */,
     PRESHIFT(0x00221000) /*  0.008316040 */,
     PRESHIFT(0x006a2000) /*  0.025909424 */,
    -PRESHIFT(0x0006a000) /* -0.001617432 */,
     PRESHIFT(0x00075000) /*  0.001785278 */,
     PRESHIFT(0x00007000) /*  0.000106812 */,

    -PRESHIFT(0x00004000) /* -0.000061035 */,
    -PRESHIFT(0x0005b000) /* -0.001388550 */,
     PRESHIFT(0x000b1000) /*  0.002700806 */,
    -PRESHIFT(0x00594000) /* -0.021789551 */,
     PRESHIFT(0x00192000) /*  0.006134033 */,
    -PRESHIFT(0x024c8000) /* -0.143676758 */,
    -PRESHIFT(0x01bf2000) /* -0.109161377 */,
    -PRESHIFT(0x0ef69000) /* -0.935195923 */,
     PRESHIFT(0x103be000) /*  1.014617920 */,
     PRESHIFT(0x032b4000) /*  0.198059082 */,
     PRESHIFT(0x026bc000) /*  0.151306152 */,
     PRESHIFT(0x00221000) /*  0.008316040 */,
     PRESHIFT(0x006a2000) /*  0.025909424 */,
    -PRESHIFT(0x0006a000) /* -0.001617432 */,
     PRESHIFT(0x00075000) /*  0.001785278 */,
     PRESHIFT(0x00007000) /*  0.000106812 */ },

  { -PRESHIFT(0x00005000) /* -0.000076294 */,	/* 15 */
    -PRESHIFT(0x00061000) /* -0.001480103 */,
     PRESHIFT(0x000a3000) /*  0.002487183 */,
    -PRESHIFT(0x005da000) /* -0.022857666 */,
     PRESHIFT(0x000b9000) /*  0.002822876 */,
    -PRESHIFT(0x02571000) /* -0.146255493 */,
    -PRESHIFT(0x0215c000) /* -0.130310059 */,
    -PRESHIFT(0x0f4dc000) /* -0.956481934 */,
     PRESHIFT(0x0ff0a000) /*  0.996246338 */,
     PRESHIFT(0x02cbf000) /*  0.174789429 */,
     PRESHIFT(0x0266e000) /*  0.150115967 */,
     PRESHIFT(0x00120000) /*  0.004394531 */,
     PRESHIFT(0x00662000) /*  0.024932861 */,
    -PRESHIFT(0x0007f000) /* -0.001937866 */,
     PRESHIFT(0x0006f000) /*  0.001693726 */,
     PRESHIFT(0x00006000) /*  0.000091553 */,

    -PRESHIFT(0x00005000) /* -0.000076294 */,
    -PRESHIFT(0x00061000) /* -0.001480103 */,
     PRESHIFT(0x000a3000) /*  0.002487183 */,
    -PRESHIFT(0x005da000) /* -0.022857666 */,
     PRESHIFT(0x000b9000) /*  0.002822876 */,
    -PRESHIFT(0x02571000) /* -0.146255493 */,
    -PRESHIFT(0x0215c000) /* -0.130310059 */,
    -PRESHIFT(0x0f4dc000) /* -0.956481934 */,
     PRESHIFT(0x0ff0a000) /*  0.996246338 */,
     PRESHIFT(0x02cbf000) /*  0.174789429 */,
     PRESHIFT(0x0266e000) /*  0.150115967 */,
     PRESHIFT(0x00120000) /*  0.004394531 */,
     PRESHIFT(0x00662000) /*  0.024932861 */,
    -PRESHIFT(0x0007f000) /* -0.001937866 */,
     PRESHIFT(0x0006f000) /*  0.001693726 */,
     PRESHIFT(0x00006000) /*  0.000091553 */ },

  { -PRESHIFT(0x00005000) /* -0.000076294 */,	/* 16 */
    -PRESHIFT(0x00068000) /* -0.001586914 */,
     PRESHIFT(0x00092000) /*  0.002227783 */,
    -PRESHIFT(0x0061f000) /* -0.023910522 */,
    -PRESHIFT(0x0002d000) /* -0.000686646 */,
    -PRESHIFT(0x025ff000) /* -0.148422241 */,
    -PRESHIFT(0x026f7000) /* -0.152206421 */,
    -PRESHIFT(0x0fa13000) /* -0.976852417 */,
     PRESHIFT(0x0fa13000) /*  0.976852417 */,
     PRESHIFT(0x026f7000) /*  0.152206421 */,
     PRESHIFT(0x025ff000) /*  0.148422241 */,
     PRESHIFT(0x0002d000) /*  0.000686646 */,
     PRESHIFT(0x0061f000) /*  0.023910522 */,
    -PRESHIFT(0x00092000) /* -0.002227783 */,
     PRESHIFT(0x00068000) /*  0.001586914 */,
     PRESHIFT(0x00005000) /*  0.000076294 */,

    -PRESHIFT(0x00005000) /* -0.000076294 */,
    -PRESHIFT(0x00068000) /* -0.001586914 */,
     PRESHIFT(0x00092000) /*  0.002227783 */,
    -PRESHIFT(0x0061f000) /* -0.023910522 */,
    -PRESHIFT(0x0002d000) /* -0.000686646 */,
    -PRESHIFT(0x025ff000) /* -0.148422241 */,
    -PRESHIFT(0x026f7000) /* -0.152206421 */,
    -PRESHIFT(0x0fa13000) /* -0.976852417 */,
     PRESHIFT(0x0fa13000) /*  0.976852417 */,
     PRESHIFT(0x026f7000) /*  0.152206421 */,
     PRESHIFT(0x025ff000) /*  0.148422241 */,
     PRESHIFT(0x0002d000) /*  0.000686646 */,
     PRESHIFT(0x0061f000) /*  0.023910522 */,
    -PRESHIFT(0x00092000) /* -0.002227783 */,
     PRESHIFT(0x00068000) /*  0.001586914 */,
     PRESHIFT(0x00005000) /*  0.000076294 */ }
};

# if defined(ASO_SYNTH)
void synth_full(struct mad_synth *, struct mad_frame const *,
		unsigned int, unsigned int);
# else
/*
 * NAME:	synth->full()
 * DESCRIPTION:	perform full frequency PCM synthesis
 */
static
void synth_full(struct mad_synth *synth, struct mad_frame const *frame,
		unsigned int nch, unsigned int ns)
{
  unsigned int phase, ch, s, sb, pe, po;
  mad_fixed_t *pcm1, *pcm2, (*filter)[2][2][16][8];
  mad_fixed_t const (*sbsample)[36][32];
  register mad_fixed_t (*fe)[8], (*fx)[8], (*fo)[8];
  register mad_fixed_t const (*Dptr)[32], *ptr;
  register mad_fixed64hi_t hi;
  register mad_fixed64lo_t lo;

  for (ch = 0; ch < nch; ++ch) {
    sbsample = &frame->sbsample[ch];
    filter   = &synth->filter[ch];
    phase    = synth->phase;
    pcm1     = synth->pcm.samples[ch];

    for (s = 0; s < ns; ++s) {
      dct32((*sbsample)[s], phase >> 1,
	    (*filter)[0][phase & 1], (*filter)[1][phase & 1]);

      pe = phase & ~1;
      po = ((phase - 1) & 0xf) | 1;

      /* calculate 32 samples */

      fe = &(*filter)[0][ phase & 1][0];
      fx = &(*filter)[0][~phase & 1][0];
      fo = &(*filter)[1][~phase & 1][0];

      Dptr = &D[0];

      ptr = *Dptr + po;
      ML0(hi, lo, (*fx)[0], ptr[ 0]);
      MLA(hi, lo, (*fx)[1], ptr[14]);
      MLA(hi, lo, (*fx)[2], ptr[12]);
      MLA(hi, lo, (*fx)[3], ptr[10]);
      MLA(hi, lo, (*fx)[4], ptr[ 8]);
      MLA(hi, lo, (*fx)[5], ptr[ 6]);
      MLA(hi, lo, (*fx)[6], ptr[ 4]);
      MLA(hi, lo, (*fx)[7], ptr[ 2]);
      MLN(hi, lo);

      ptr = *Dptr + pe;
      MLA(hi, lo, (*fe)[0], ptr[ 0]);
      MLA(hi, lo, (*fe)[1], ptr[14]);
      MLA(hi, lo, (*fe)[2], ptr[12]);
      MLA(hi, lo, (*fe)[3], ptr[10]);
      MLA(hi, lo, (*fe)[4], ptr[ 8]);
      MLA(hi, lo, (*fe)[5], ptr[ 6]);
      MLA(hi, lo, (*fe)[6], ptr[ 4]);
      MLA(hi, lo, (*fe)[7], ptr[ 2]);

      *pcm1++ = SHIFT(MLZ(hi, lo));

      pcm2 = pcm1 + 30;

      for (sb = 1; sb < 16; ++sb) {
	++fe;
	++Dptr;

	/* D[32 - sb][i] == -D[sb][31 - i] */

	ptr = *Dptr + po;
	ML0(hi, lo, (*fo)[0], ptr[ 0]);
	MLA(hi, lo, (*fo)[1], ptr[14]);
	MLA(hi, lo, (*fo)[2], ptr[12]);
	MLA(hi, lo, (*fo)[3], ptr[10]);
	MLA(hi, lo, (*fo)[4], ptr[ 8]);
	MLA(hi, lo, (*fo)[5], ptr[ 6]);
	MLA(hi, lo, (*fo)[6], ptr[ 4]);
	MLA(hi, lo, (*fo)[7], ptr[ 2]);
	MLN(hi, lo);

	ptr = *Dptr + pe;
	MLA(hi, lo, (*fe)[7], ptr[ 2]);
	MLA(hi, lo, (*fe)[6], ptr[ 4]);
	MLA(hi, lo, (*fe)[5], ptr[ 6]);
	MLA(hi, lo, (*fe)[4], ptr[ 8]);
	MLA(hi, lo, (*fe)[3], ptr[10]);
	MLA(hi, lo, (*fe)[2], ptr[12]);
	MLA(hi, lo, (*fe)[1], ptr[14]);
	MLA(hi, lo, (*fe)[0], ptr[ 0]);

	*pcm1++ = SHIFT(MLZ(hi, lo));

	ptr = *Dptr - pe;
	ML0(hi, lo, (*fe)[0], ptr[31 - 16]);
	MLA(hi, lo, (*fe)[1], ptr[31 - 14]);
	MLA(hi, lo, (*fe)[2], ptr[31 - 12]);
	MLA(hi, lo, (*fe)[3], ptr[31 - 10]);
	MLA(hi, lo, (*fe)[4], ptr[31 -  8]);
	MLA(hi, lo, (*fe)[5], ptr[31 -  6]);
	MLA(hi, lo, (*fe)[6], ptr[31 -  4]);
	MLA(hi, lo, (*fe)[7], ptr[31 -  2]);

	ptr = *Dptr - po;
	MLA(hi, lo, (*fo)[7], ptr[31 -  2]);
	MLA(hi, lo, (*fo)[6], ptr[31 -  4]);
	MLA(hi, lo, (*fo)[5], ptr[31 -  6]);
	MLA(hi, lo, (*fo)[4], ptr[31 -  8]);
	MLA(hi, lo, (*fo)[3], ptr[31 - 10]);
	MLA(hi, lo, (*fo)[2], ptr[31 - 12]);
	MLA(hi, lo, (*fo)[1], ptr[31 - 14]);
	MLA(hi, lo, (*fo)[0], ptr[31 - 16]);

	*pcm2-- = SHIFT(MLZ(hi, lo));

	++fo;
      }

      ++Dptr;

      ptr = *Dptr + po;
      ML0(hi, lo, (*fo)[0], ptr[ 0]);
      MLA(hi, lo, (*fo)[1], ptr[14]);
      MLA(hi, lo, (*fo)[2], ptr[12]);
      MLA(hi, lo, (*fo)[3], ptr[10]);
      MLA(hi, lo, (*fo)[4], ptr[ 8]);
      MLA(hi, lo, (*fo)[5], ptr[ 6]);
      MLA(hi, lo, (*fo)[6], ptr[ 4]);
      MLA(hi, lo, (*fo)[7], ptr[ 2]);

      *pcm1 = SHIFT(-MLZ(hi, lo));
      pcm1 += 16;

      phase = (phase + 1) % 16;
    }
  }
}
# endif

/*
 * NAME:	synth->half()
 * DESCRIPTION:	perform half frequency PCM synthesis
 */
static
void synth_half(struct mad_synth *synth, struct mad_frame const *frame,
		unsigned int nch, unsigned int ns)
{
  unsigned int phase, ch, s, sb, pe, po;
  mad_fixed_t *pcm1, *pcm2, (*filter)[2][2][16][8];
  mad_fixed_t const (*sbsample)[36][32];
  register mad_fixed_t (*fe)[8], (*fx)[8], (*fo)[8];
  register mad_fixed_t const (*Dptr)[32], *ptr;
  register mad_fixed64hi_t hi;
  register mad_fixed64lo_t lo;

  for (ch = 0; ch < nch; ++ch) {
    sbsample = &frame->sbsample[ch];
    filter   = &synth->filter[ch];
    phase    = synth->phase;
    pcm1     = synth->pcm.samples[ch];

    for (s = 0; s < ns; ++s) {
      dct32((*sbsample)[s], phase >> 1,
	    (*filter)[0][phase & 1], (*filter)[1][phase & 1]);

      pe = phase & ~1;
      po = ((phase - 1) & 0xf) | 1;

      /* calculate 16 samples */

      fe = &(*filter)[0][ phase & 1][0];
      fx = &(*filter)[0][~phase & 1][0];
      fo = &(*filter)[1][~phase & 1][0];

      Dptr = &D[0];

      ptr = *Dptr + po;
      ML0(hi, lo, (*fx)[0], ptr[ 0]);
      MLA(hi, lo, (*fx)[1], ptr[14]);
      MLA(hi, lo, (*fx)[2], ptr[12]);
      MLA(hi, lo, (*fx)[3], ptr[10]);
      MLA(hi, lo, (*fx)[4], ptr[ 8]);
      MLA(hi, lo, (*fx)[5], ptr[ 6]);
      MLA(hi, lo, (*fx)[6], ptr[ 4]);
      MLA(hi, lo, (*fx)[7], ptr[ 2]);
      MLN(hi, lo);

      ptr = *Dptr + pe;
      MLA(hi, lo, (*fe)[0], ptr[ 0]);
      MLA(hi, lo, (*fe)[1], ptr[14]);
      MLA(hi, lo, (*fe)[2], ptr[12]);
      MLA(hi, lo, (*fe)[3], ptr[10]);
      MLA(hi, lo, (*fe)[4], ptr[ 8]);
      MLA(hi, lo, (*fe)[5], ptr[ 6]);
      MLA(hi, lo, (*fe)[6], ptr[ 4]);
      MLA(hi, lo, (*fe)[7], ptr[ 2]);

      *pcm1++ = SHIFT(MLZ(hi, lo));

      pcm2 = pcm1 + 14;

      for (sb = 1; sb < 16; ++sb) {
	++fe;
	++Dptr;

	/* D[32 - sb][i] == -D[sb][31 - i] */

	if (!(sb & 1)) {
	  ptr = *Dptr + po;
	  ML0(hi, lo, (*fo)[0], ptr[ 0]);
	  MLA(hi, lo, (*fo)[1], ptr[14]);
	  MLA(hi, lo, (*fo)[2], ptr[12]);
	  MLA(hi, lo, (*fo)[3], ptr[10]);
	  MLA(hi, lo, (*fo)[4], ptr[ 8]);
	  MLA(hi, lo, (*fo)[5], ptr[ 6]);
	  MLA(hi, lo, (*fo)[6], ptr[ 4]);
	  MLA(hi, lo, (*fo)[7], ptr[ 2]);
	  MLN(hi, lo);

	  ptr = *Dptr + pe;
	  MLA(hi, lo, (*fe)[7], ptr[ 2]);
	  MLA(hi, lo, (*fe)[6], ptr[ 4]);
	  MLA(hi, lo, (*fe)[5], ptr[ 6]);
	  MLA(hi, lo, (*fe)[4], ptr[ 8]);
	  MLA(hi, lo, (*fe)[3], ptr[10]);
	  MLA(hi, lo, (*fe)[2], ptr[12]);
	  MLA(hi, lo, (*fe)[1], ptr[14]);
	  MLA(hi, lo, (*fe)[0], ptr[ 0]);

	  *pcm1++ = SHIFT(MLZ(hi, lo));

	  ptr = *Dptr - po;
	  ML0(hi, lo, (*fo)[7], ptr[31 -  2]);
	  MLA(hi, lo, (*fo)[6], ptr[31 -  4]);
	  MLA(hi, lo, (*fo)[5], ptr[31 -  6]);
	  MLA(hi, lo, (*fo)[4], ptr[31 -  8]);
	  MLA(hi, lo, (*fo)[3], ptr[31 - 10]);
	  MLA(hi, lo, (*fo)[2], ptr[31 - 12]);
	  MLA(hi, lo, (*fo)[1], ptr[31 - 14]);
	  MLA(hi, lo, (*fo)[0], ptr[31 - 16]);

	  ptr = *Dptr - pe;
	  MLA(hi, lo, (*fe)[0], ptr[31 - 16]);
	  MLA(hi, lo, (*fe)[1], ptr[31 - 14]);
	  MLA(hi, lo, (*fe)[2], ptr[31 - 12]);
	  MLA(hi, lo, (*fe)[3], ptr[31 - 10]);
	  MLA(hi, lo, (*fe)[4], ptr[31 -  8]);
	  MLA(hi, lo, (*fe)[5], ptr[31 -  6]);
	  MLA(hi, lo, (*fe)[6], ptr[31 -  4]);
	  MLA(hi, lo, (*fe)[7], ptr[31 -  2]);

	  *pcm2-- = SHIFT(MLZ(hi, lo));
	}

	++fo;
      }

      ++Dptr;

      ptr = *Dptr + po;
      ML0(hi, lo, (*fo)[0], ptr[ 0]);
      MLA(hi, lo, (*fo)[1], ptr[14]);
      MLA(hi, lo, (*fo)[2], ptr[12]);
      MLA(hi, lo, (*fo)[3], ptr[10]);
      MLA(hi, lo, (*fo)[4], ptr[ 8]);
      MLA(hi, lo, (*fo)[5], ptr[ 6]);
      MLA(hi, lo, (*fo)[6], ptr[ 4]);
      MLA(hi, lo, (*fo)[7], ptr[ 2]);

      *pcm1 = SHIFT(-MLZ(hi, lo));
      pcm1 += 8;

      phase = (phase + 1) % 16;
    }
  }
}

/*
 * NAME:	synth->frame()
 * DESCRIPTION:	perform PCM synthesis of frame subband samples
 */
void mad_synth_frame(struct mad_synth *synth, struct mad_frame const *frame)
{
  unsigned int nch, ns;
  void (*synth_frame)(struct mad_synth *, struct mad_frame const *,
		      unsigned int, unsigned int);

  nch = MAD_NCHANNELS(&frame->header);
  ns  = MAD_NSBSAMPLES(&frame->header);

  synth->pcm.samplerate = frame->header.samplerate;
  synth->pcm.channels   = nch;
  synth->pcm.length     = 32 * ns;

  synth_frame = synth_full;

  if (frame->options & MAD_OPTION_HALFSAMPLERATE) {
    synth->pcm.samplerate /= 2;
    synth->pcm.length     /= 2;

    synth_frame = synth_half;
  }

  synth_frame(synth, frame, nch, ns);

  synth->phase = (synth->phase + ns) % 16;
}
