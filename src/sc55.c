/*
 * Roland SC-55 emulation in one C89 file.
 *
 * A port of the Nuked-SC55 backend (https://github.com/jcmoyer/Nuked-SC55,
 * itself a fork of https://github.com/nukeykt/Nuked-SC55):
 *
 * Copyright (C) 2021, 2024 nukeykt
 * Copyright (C) 2024-2026 J.C. Moyer
 *
 * The H8/532 core, its timers and interrupt controller, the PCM chip and
 * the mkII's sub-MCU are carried over function for function.  The LCD,
 * the front panel and the JV-880 are not.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * MCU emulator for Roland Sound Canvas (SC-55mkII, SC-55), PCM chip
 * emulation; with thanks to John McMaster (https://siliconprawn.org)
 * for the PCM chip decap.
 */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "sc55.h"

#if defined(_MSC_VER)
#define SC55_INLINE static __inline
#elif defined(__GNUC__)
#define SC55_INLINE static __inline__
#else
#define SC55_INLINE static
#endif

/* A left shift that may push bits into or past the sign, done on the
 * unsigned type so it is defined; the result is the two's complement
 * value the hardware arithmetic expects. */
#define SC55_SHL(x, n) ((int32_t)((uint32_t)(x) << (n)))

#define ROM1_SIZE      0x8000
#define ROM2_SIZE      0x80000
#define SMROM_SIZE     0x1000
#define WAVEROM1_SIZE  0x200000
#define WAVEROM2_SIZE  0x200000
#define WAVEROM3_SIZE  0x100000

#define RAM_SIZE   0x400
#define SRAM_SIZE  0x8000

#define uart_buffer_size 8192

/* Frames a single MCU step can add beyond what the caller asked for. */
#define SC55_SPILL 8

typedef struct sc55_mcu    mcu_t;
typedef struct sc55_pcm    pcm_t;
typedef struct sc55_submcu submcu_t;
typedef struct sc55_timer  mcu_timer_t;

typedef int         Romset;
typedef int         MCU_Interrupt_Source;
typedef signed char MCU_Exception_Source;
typedef uint8_t     MCU_Operand_Size;

enum { OPERAND_BYTE, OPERAND_WORD };

enum {
    ROMSET_MK2, ROMSET_ST, ROMSET_MK1, ROMSET_CM300, ROMSET_JV880,
    ROMSET_SCB55, ROMSET_RLP3237, ROMSET_SC155, ROMSET_SC155MK2
};

enum {
    INTERRUPT_SOURCE_NMI = 0,
    INTERRUPT_SOURCE_IRQ0, /* GPINT */
    INTERRUPT_SOURCE_IRQ1,
    INTERRUPT_SOURCE_FRT0_ICI,
    INTERRUPT_SOURCE_FRT0_OCIA,
    INTERRUPT_SOURCE_FRT0_OCIB,
    INTERRUPT_SOURCE_FRT0_FOVI,
    INTERRUPT_SOURCE_FRT1_ICI,
    INTERRUPT_SOURCE_FRT1_OCIA,
    INTERRUPT_SOURCE_FRT1_OCIB,
    INTERRUPT_SOURCE_FRT1_FOVI,
    INTERRUPT_SOURCE_FRT2_ICI,
    INTERRUPT_SOURCE_FRT2_OCIA,
    INTERRUPT_SOURCE_FRT2_OCIB,
    INTERRUPT_SOURCE_FRT2_FOVI,
    INTERRUPT_SOURCE_TIMER_CMIA,
    INTERRUPT_SOURCE_TIMER_CMIB,
    INTERRUPT_SOURCE_TIMER_OVI,
    INTERRUPT_SOURCE_ANALOG,
    INTERRUPT_SOURCE_UART_RX,
    INTERRUPT_SOURCE_UART_TX,
    INTERRUPT_SOURCE_MAX
};

enum {
    EXCEPTION_SOURCE_ADDRESS_ERROR = 0,
    EXCEPTION_SOURCE_INVALID_INSTRUCTION,
    EXCEPTION_SOURCE_TRACE
};

enum {
    SM_STATUS_C = 1,
    SM_STATUS_Z = 2,
    SM_STATUS_I = 4,
    SM_STATUS_D = 8,
    SM_STATUS_B = 16,
    SM_STATUS_T = 32,
    SM_STATUS_V = 64,
    SM_STATUS_N = 128
};

/* 16-bit free running timers */
typedef struct
{
    uint64_t deadline;
    uint8_t  tcr;
    uint8_t  tcsr;
    uint16_t frc;
    uint16_t ocra;
    uint16_t ocrb;
    uint16_t icr;
    uint8_t  status_rd; /* not an actual FRT register */
    uint8_t  stride;
} frt_t;

/* 8-bit timer */
typedef struct
{
    uint64_t deadline;
    uint16_t stride;
    uint8_t  tcr;
    uint8_t  tcsr;
    uint8_t  tcora;
    uint8_t  tcorb;
    uint8_t  tcnt;
    uint8_t  status_rd;
} tmr_t;

struct sc55_timer
{
    uint64_t cycles;
    mcu_t   *mcu;
    frt_t    frt[3];
    tmr_t    tmr;
    uint8_t  tempreg;
    uint8_t  frt_step_table[4];
    uint16_t tmr_step_table[8];
};

typedef struct
{
    /* config_reg_3c */
    uint32_t orval;
    int      dac_mask; /* unused */
    uint8_t  noise_mask;
    uint8_t  write_mask;
    int      oversampling;
    /* config_reg_3d */
    /* important that this starts at 1, see derivation in PCM_Write */
    uint8_t  reg_slots;
} PCM_Config;

struct sc55_pcm
{
    uint32_t ram1[32][8];
    uint16_t ram2[32][16];
    mcu_t   *mcu;
    uint64_t cycles;
    uint32_t voice_mask;          /* same size as voice_mask_pending */
    uint32_t voice_mask_pending;  /* 28 bits wide? */
    uint32_t write_latch;         /* 20 bits wide? */
    uint32_t read_latch;          /* 20 bits wide? */
    uint32_t wave_read_address;
    uint16_t tv_counter;          /* 14 bits wide? */
    uint8_t  wave_byte_latch;
    uint8_t  select_channel;      /* 5 bits wide? */
    uint8_t  config_reg_3c;       /* SC55:c3 JV880:c0 */
    uint8_t  config_reg_3d;
    uint8_t  irq_channel;         /* range 1..32 */
    int      irq_assert;
    int      voice_mask_updating;
    int      nfs;
    int32_t  accum_l;
    int32_t  accum_r;
    int32_t  rcsum[2];
    PCM_Config config;
    uint16_t eram[0x4000];
    uint8_t  waverom1[WAVEROM1_SIZE];
    uint8_t  waverom2[WAVEROM2_SIZE];
    uint8_t  waverom3[WAVEROM3_SIZE];
    int      enable_oversampling;
};

struct sc55_submcu
{
    uint16_t pc;
    uint8_t  a;
    uint8_t  x;
    uint8_t  y;
    uint8_t  s;
    uint8_t  sr;
    uint64_t cycles;
    uint8_t  sleep;
    mcu_t   *mcu;
    uint8_t  rom[SMROM_SIZE];
    uint8_t  ram[128];
    uint8_t  shared_ram[192];
    uint8_t  access[0x18];
    uint8_t  p0_dir;
    uint8_t  p1_dir;
    uint8_t  device_mode[32];
    uint8_t  cts;
    uint64_t timer_cycles;
    uint8_t  timer_prescaler;
    uint8_t  timer_counter;
    uint8_t  uart_rx_gotbyte;
};

struct sc55_mcu
{
    uint16_t r[8];
    uint16_t pc;
    uint16_t sr;
    uint8_t  cp, dp, ep, tp, br;
    uint8_t  sleep;
    uint8_t  ex_ignore;
    MCU_Exception_Source exception_pending;
    uint32_t interrupt_pending;   /* bit per MCU_Interrupt_Source */
    uint16_t trapa_pending;       /* bit per TRAPA vector */
    uint64_t cycles;

    uint8_t  rom1[ROM1_SIZE];
    uint8_t  rom2[ROM2_SIZE];
    uint8_t  ram[RAM_SIZE];
    uint8_t  sram[SRAM_SIZE];

    uint8_t  dev_register[0x80];

    uint16_t ad_val[4];
    uint8_t  ad_nibble;
    uint8_t  sw_pos;
    uint8_t  io_sd;

    submcu_t    *sm;
    pcm_t       *pcm;
    mcu_timer_t *timer;

    uint32_t uart_write_ptr;
    uint32_t uart_read_ptr;
    uint8_t  uart_buffer[uart_buffer_size];

    uint8_t  uart_rx_byte;
    uint64_t uart_rx_delay;
    uint64_t uart_tx_delay;

    Romset   romset;

    int is_mk1;   /* 0 - SC-55mkII, SC-55ST. 1 - SC-55, CM-300/SCC-1 */
    int is_cm300; /* 0 - SC-55, 1 - CM-300/SCC-1 */
    int is_st;    /* 0 - SC-55mk2, 1 - SC-55ST */
    int is_jv880; /* always 0: the JV-880 is not built */
    int is_scb55; /* 0 - sub mcu (e.g SC-55mk2), 1 - no sub mcu (e.g SCB-55) */
    int is_sc155; /* 0 - SC-55(MK2), 1 - SC-155(MK2) */

    uint32_t rom2_mask;

    int      ga_int[8];
    uint8_t  ga_int_enable;  /* mask of ga_int indices */
    uint8_t  ga_int_trigger; /* index into ga_int */
    int      ga_lcd_counter; /* timer range 0..500, decrements to 0 */

    uint32_t button_pressed;

    uint8_t  p0_data;
    uint8_t  p1_data;

    int      adf_rd;

    uint64_t analog_end_time;

    int      ssr_rd;

    uint32_t operand_type;
    uint16_t operand_ea;
    uint8_t  operand_ep;
    MCU_Operand_Size operand_size;
    uint8_t  operand_reg;
    uint8_t  operand_status;
    uint16_t operand_data;
    uint8_t  opcode_extended;

    /* Where the PCM chip's frames go: out_cap frames into `out`, then up
     * to SC55_SPILL more into `spill`. */
    int32_t *out;
    int32_t *spill;
    size_t   out_count;
    size_t   out_cap;
};

/* The LCD is not emulated. */
#define LCD_Enable(lcd, enable)     ((void)0)
#define LCD_Write(lcd, address, v)  ((void)0)

/* Values are byte offsets from the start of register fields in memory */
/* (ff80..ffff) to the field named by the enumeration item. This enumeration */
/* also acts as an index type for mcu->dev_register. Not all of these indices */
/* are used. Notably, timers are handled by the mcu_timer module instead and */
/* their data in dev_register will hold an unspecified value. */
enum MCU_Register_Field
{
    DEV_P1DDR      = 0x00,
    DEV_P2DDR      = 0x01,
    DEV_P1DR       = 0x02,
    DEV_P2DR       = 0x03,
    DEV_P3DDR      = 0x04,
    DEV_P4DDR      = 0x05,
    DEV_P3DR       = 0x06,
    DEV_P4DR       = 0x07,
    DEV_P5DDR      = 0x08,
    DEV_P6DDR      = 0x09,
    DEV_P5DR       = 0x0a,
    DEV_P6DR       = 0x0b,
    DEV_P7DDR      = 0x0c,
    DEV_P7DR       = 0x0e,
    DEV_P8DR       = 0x0f,
    DEV_FRT1_TCR   = 0x10,
    DEV_FRT1_TCSR  = 0x11,
    DEV_FRT1_FRCH  = 0x12,
    DEV_FRT1_FRCL  = 0x13,
    DEV_FRT1_OCRAH = 0x14,
    DEV_FRT1_OCRAL = 0x15,
    DEV_FRT1_OCRBH = 0x16,
    DEV_FRT1_OCRBL = 0x17,
    DEV_FRT1_ICRH  = 0x18,
    DEV_FRT1_ICRL  = 0x19,
    DEV_FRT2_TCR   = 0x20,
    DEV_FRT2_TCSR  = 0x21,
    DEV_FRT2_FRCH  = 0x22,
    DEV_FRT2_FRCL  = 0x23,
    DEV_FRT2_OCRAH = 0x24,
    DEV_FRT2_OCRAL = 0x25,
    DEV_FRT2_OCRBH = 0x26,
    DEV_FRT2_OCRBL = 0x27,
    DEV_FRT2_ICRH  = 0x28,
    DEV_FRT2_ICRL  = 0x29,
    DEV_FRT3_TCR   = 0x30,
    DEV_FRT3_TCSR  = 0x31,
    DEV_FRT3_FRCH  = 0x32,
    DEV_FRT3_FRCL  = 0x33,
    DEV_FRT3_OCRAH = 0x34,
    DEV_FRT3_OCRAL = 0x35,
    DEV_FRT3_OCRBH = 0x36,
    DEV_FRT3_OCRBL = 0x37,
    DEV_FRT3_ICRH  = 0x38,
    DEV_FRT3_ICRL  = 0x39,
    DEV_PWM1_TCR   = 0x40,
    DEV_PWM1_DTR   = 0x41,
    DEV_PWM1_TCNT  = 0x42,
    DEV_PWM2_TCR   = 0x44,
    DEV_PWM2_DTR   = 0x45,
    DEV_PWM2_TCNT  = 0x46,
    DEV_PWM3_TCR   = 0x48,
    DEV_PWM3_DTR   = 0x49,
    DEV_PWM3_TCNT  = 0x4a,
    DEV_TMR_TCR    = 0x50,
    DEV_TMR_TCSR   = 0x51,
    DEV_TMR_TCORA  = 0x52,
    DEV_TMR_TCORB  = 0x53,
    DEV_TMR_TCNT   = 0x54,
    DEV_SMR        = 0x58,
    DEV_BRR        = 0x59,
    DEV_SCR        = 0x5a,
    DEV_TDR        = 0x5b,
    DEV_SSR        = 0x5c,
    DEV_RDR        = 0x5d,
    DEV_ADDRAH     = 0x60,
    DEV_ADDRAL     = 0x61,
    DEV_ADDRBH     = 0x62,
    DEV_ADDRBL     = 0x63,
    DEV_ADDRCH     = 0x64,
    DEV_ADDRCL     = 0x65,
    DEV_ADDRDH     = 0x66,
    DEV_ADDRDL     = 0x67,
    DEV_ADCSR      = 0x68,
    DEV_IPRA       = 0x70,
    DEV_IPRB       = 0x71,
    DEV_IPRC       = 0x72,
    DEV_IPRD       = 0x73,
    DEV_DTEA       = 0x74,
    DEV_DTEB       = 0x75,
    DEV_DTEC       = 0x76,
    DEV_DTED       = 0x77,
    DEV_WCR        = 0x78,
    DEV_RAMCR      = 0x79,
    DEV_P1CR       = 0x7c,
    DEV_P9DDR      = 0x7e,
    DEV_P9DR       = 0x7f
};

#define sr_mask 0x870f
enum {
    STATUS_T = 0x8000,
    STATUS_N = 0x08,
    STATUS_Z = 0x04,
    STATUS_V = 0x02,
    STATUS_C = 0x01,
    STATUS_INT_MASK = 0x700
};

enum {
    VECTOR_RESET = 0,
    VECTOR_RESERVED1, /* UNUSED */
    VECTOR_INVALID_INSTRUCTION,
    VECTOR_DIVZERO,
    VECTOR_TRAP,
    VECTOR_RESERVED2, /* UNUSED */
    VECTOR_RESERVED3, /* UNUSED */
    VECTOR_RESERVED4, /* UNUSED */
    VECTOR_ADDRESS_ERROR,
    VECTOR_TRACE,
    VECTOR_RESERVED5, /* UNUSED */
    VECTOR_NMI,
    VECTOR_RESERVED6, /* UNUSED */
    VECTOR_RESERVED7, /* UNUSED */
    VECTOR_RESERVED8, /* UNUSED */
    VECTOR_RESERVED9, /* UNUSED */
    VECTOR_TRAPA_0,
    VECTOR_TRAPA_1,
    VECTOR_TRAPA_2,
    VECTOR_TRAPA_3,
    VECTOR_TRAPA_4,
    VECTOR_TRAPA_5,
    VECTOR_TRAPA_6,
    VECTOR_TRAPA_7,
    VECTOR_TRAPA_8,
    VECTOR_TRAPA_9,
    VECTOR_TRAPA_A,
    VECTOR_TRAPA_B,
    VECTOR_TRAPA_C,
    VECTOR_TRAPA_D,
    VECTOR_TRAPA_E,
    VECTOR_TRAPA_F,
    VECTOR_IRQ0,
    VECTOR_IRQ1,
    VECTOR_INTERNAL_INTERRUPT_88, /* UNUSED */
    VECTOR_INTERNAL_INTERRUPT_8C, /* UNUSED */
    VECTOR_INTERNAL_INTERRUPT_90, /* FRT1 ICI */
    VECTOR_INTERNAL_INTERRUPT_94, /* FRT1 OCIA */
    VECTOR_INTERNAL_INTERRUPT_98, /* FRT1 OCIB */
    VECTOR_INTERNAL_INTERRUPT_9C, /* FRT1 FOVI */
    VECTOR_INTERNAL_INTERRUPT_A0, /* FRT2 ICI */
    VECTOR_INTERNAL_INTERRUPT_A4, /* FRT2 OCIA */
    VECTOR_INTERNAL_INTERRUPT_A8, /* FRT2 OCIB */
    VECTOR_INTERNAL_INTERRUPT_AC, /* FRT2 FOVI */
    VECTOR_INTERNAL_INTERRUPT_B0, /* FRT3 ICI */
    VECTOR_INTERNAL_INTERRUPT_B4, /* FRT3 OCIA */
    VECTOR_INTERNAL_INTERRUPT_B8, /* FRT3 OCIB */
    VECTOR_INTERNAL_INTERRUPT_BC, /* FRT3 FOVI */
    VECTOR_INTERNAL_INTERRUPT_C0, /* CMIA */
    VECTOR_INTERNAL_INTERRUPT_C4, /* CMIB */
    VECTOR_INTERNAL_INTERRUPT_C8, /* OVI */
    VECTOR_INTERNAL_INTERRUPT_CC, /* UNUSED */
    VECTOR_INTERNAL_INTERRUPT_D0, /* ERI */
    VECTOR_INTERNAL_INTERRUPT_D4, /* RXI */
    VECTOR_INTERNAL_INTERRUPT_D8, /* TXI */
    VECTOR_INTERNAL_INTERRUPT_DC, /* UNUSED */
    VECTOR_INTERNAL_INTERRUPT_E0 /* ADI */
};

/* Index of the lowest set bit of a non-zero word. */
SC55_INLINE unsigned sc55_ctz(uint32_t v)
{
    static const uint8_t debruijn[32] = {
        0, 1, 28, 2, 29, 14, 24, 3, 30, 22, 20, 15, 25, 17, 4, 8,
        31, 27, 13, 23, 21, 19, 16, 7, 26, 12, 18, 6, 11, 5, 10, 9
    };
    return debruijn[(uint32_t)((v & (0u - v)) * 0x077CB531u) >> 27];
}

SC55_INLINE void MCU_SetRegisterByte(mcu_t *mcu, uint8_t reg, uint8_t val);
SC55_INLINE uint32_t MCU_GetVectorAddress(mcu_t *mcu, uint32_t vector);
SC55_INLINE uint8_t MCU_GetPageForRegister(mcu_t *mcu, uint8_t reg);
SC55_INLINE void MCU_ControlRegisterWrite(mcu_t *mcu, uint32_t reg, MCU_Operand_Size siz, uint32_t data);
SC55_INLINE uint32_t MCU_ControlRegisterRead(mcu_t *mcu, uint32_t reg, MCU_Operand_Size siz);
SC55_INLINE void MCU_SetStatus(mcu_t *mcu, int condition, uint16_t mask);
SC55_INLINE void MCU_PushStack(mcu_t *mcu, uint16_t data);
SC55_INLINE uint16_t MCU_PopStack(mcu_t *mcu);
static void MCU_Interrupt_Start(mcu_t *mcu, int32_t mask);
static void MCU_Interrupt_SetRequest(mcu_t *mcu, MCU_Interrupt_Source interrupt, int value);
static void MCU_Interrupt_Exception(mcu_t *mcu, MCU_Exception_Source exception);
static void MCU_Interrupt_TRAPA(mcu_t *mcu, uint8_t vector);
static void MCU_Interrupt_StartVector(mcu_t *mcu, uint32_t vector, int32_t mask);
static void MCU_Interrupt_GetVL(const mcu_t *mcu, uint32_t source, int32_t *vector, int32_t *level);
static void MCU_Interrupt_Handle(mcu_t *mcu);
SC55_INLINE uint64_t AlignForward(uint64_t value, uint64_t interval);
static void TIMER_Init(mcu_timer_t *timer, mcu_t *mcu);
static void TIMER_Reset(mcu_timer_t *timer);
static void TIMER_WriteFRT(mcu_timer_t *timer, uint32_t address, uint8_t data);
static uint8_t TIMER_ReadFRT(mcu_timer_t *timer, uint32_t address);
static void TIMER_WriteTMR(mcu_timer_t *timer, uint32_t address, uint8_t data);
static uint8_t TIMER_ReadTMR(mcu_timer_t *timer, uint32_t address);
SC55_INLINE void TIMER_ClockFrt(mcu_timer_t *timer, int frt_id);
SC55_INLINE void TIMER_ClockTmr(mcu_timer_t *timer);
static void TIMER_Clock(mcu_timer_t *timer, uint64_t cycles);
static void TIMER_NotifyRomsetChange(mcu_timer_t *timer);
SC55_INLINE uint8_t PCM_ReadROM(pcm_t *pcm, uint32_t address);
static void PCM_Write(pcm_t *pcm, uint32_t address, uint8_t data);
static uint8_t PCM_Read(pcm_t *pcm, uint32_t address);
static void PCM_Init(pcm_t *pcm, mcu_t *mcu);
SC55_INLINE int32_t sx20(int32_t in);
SC55_INLINE int32_t addclip20(int32_t add1, int32_t add2, int32_t cin);
SC55_INLINE int32_t multi(int32_t val1, int8_t val2);
SC55_INLINE void calc_tv(pcm_t *pcm, int e, int adjust, uint16_t *levelcur, int active, int *volmul);
SC55_INLINE int eram_unpack(pcm_t *pcm, uint32_t addr, int type);
SC55_INLINE void eram_pack(pcm_t *pcm, uint32_t addr, uint32_t val);
static void PCM_GetConfig(PCM_Config *config, uint8_t config_byte);
static void PCM_Update(pcm_t *pcm, uint64_t cycles);
static uint32_t PCM_GetOutputFrequency(const pcm_t *pcm);
static void SM_ErrorTrap(submcu_t *sm);
static uint8_t SM_Read(submcu_t *sm, uint16_t address);
static void SM_Write(submcu_t *sm, uint16_t address, uint8_t data);
static void SM_SysWrite(submcu_t *sm, uint32_t address, uint8_t data);
static uint8_t SM_SysRead(submcu_t *sm, uint32_t address);
static uint16_t SM_GetVectorAddress(submcu_t *sm, uint32_t vector);
static void SM_SetStatus(submcu_t *sm, uint32_t condition, uint32_t mask);
static void SM_Init(submcu_t *sm, mcu_t *mcu);
static void SM_Reset(submcu_t *sm);
static uint8_t SM_ReadAdvance(submcu_t *sm);
static uint16_t SM_ReadAdvance16(submcu_t *sm);
static uint16_t SM_Read16(submcu_t *sm, uint16_t address);
static void SM_Update_NZ(submcu_t *sm, uint8_t val);
static void SM_PushStack(submcu_t *sm, uint8_t data);
static uint8_t SM_PopStack(submcu_t *sm);
static void SM_Opcode_NotImplemented(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_SEI(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_CLD(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_CLT(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_LDX(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_LDY(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_TXS(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_TXA(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_STA(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_INX(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_INY(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_BBC_BBS(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_CPX(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_CPY(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_BEQ(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_BCC(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_BCS(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_LDM(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_LDA(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_CLI(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_STP(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_PHA(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_SEB_CLB(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_RTI(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_PLA(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_BRA(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_JSR(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_CMP(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_BNE(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_RTS(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_JMP(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_ORA(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_DEC(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_TAX(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_STX(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_STY(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_SEC(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_NOP(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_BPL(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_CLC(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_AND(submcu_t *sm, uint8_t opcode);
static void SM_Opcode_INC(submcu_t *sm, uint8_t opcode);
static void SM_StartVector(submcu_t *sm, uint32_t vector);
static void SM_HandleInterrupt(submcu_t *sm);
static void SM_UpdateTimer(submcu_t *sm);
static void SM_UpdateUART(submcu_t *sm);
static void SM_Update(submcu_t *sm, uint64_t cycles);
static int32_t MCU_SUB_Common(mcu_t *mcu, int32_t t1, int32_t t2, int32_t c_bit, MCU_Operand_Size siz);
static int32_t MCU_ADD_Common(mcu_t *mcu, int32_t t1, int32_t t2, int32_t c_bit, MCU_Operand_Size siz);
static void MCU_Operand_Nop(mcu_t *mcu, uint8_t operand);
static void MCU_Operand_Sleep(mcu_t *mcu, uint8_t operand);
static void MCU_Operand_NotImplemented(mcu_t *mcu, uint8_t operand);
static void MCU_LDM(mcu_t *mcu, uint8_t operand);
static void MCU_STM(mcu_t *mcu, uint8_t operand);
static void MCU_TRAPA(mcu_t *mcu, uint8_t operand);
static void MCU_Jump_PJSR(mcu_t *mcu, uint8_t operand);
static void MCU_Jump_JSR(mcu_t *mcu, uint8_t operand);
static void MCU_Jump_RTE(mcu_t *mcu, uint8_t operand);
static void MCU_Jump_Bcc(mcu_t *mcu, uint8_t operand);
static void MCU_Jump_RTS(mcu_t *mcu, uint8_t operand);
static void MCU_Jump_RTD(mcu_t *mcu, uint8_t operand);
static void MCU_Jump_JMP(mcu_t *mcu, uint8_t operand);
static void MCU_Jump_BSR(mcu_t *mcu, uint8_t operand);
static void MCU_Jump_PJMP(mcu_t *mcu, uint8_t operand);
static uint32_t MCU_Operand_Read(mcu_t *mcu);
static void MCU_Operand_Write(mcu_t *mcu, uint32_t data);
static void MCU_Operand_General(mcu_t *mcu, uint8_t operand);
static void MCU_SetStatusCommon(mcu_t *mcu, uint32_t val, MCU_Operand_Size siz);
static void MCU_Opcode_Short_MOVE(mcu_t *mcu, uint8_t opcode);
static void MCU_Opcode_Short_MOVI(mcu_t *mcu, uint8_t opcode);
static void MCU_Opcode_Short_MOVF(mcu_t *mcu, uint8_t opcode);
static void MCU_Opcode_Short_MOVL(mcu_t *mcu, uint8_t opcode);
static void MCU_Opcode_Short_MOVS(mcu_t *mcu, uint8_t opcode);
static void MCU_Opcode_Short_CMP(mcu_t *mcu, uint8_t opcode);
static void MCU_Opcode_NotImplemented(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg);
static void MCU_Opcode_MOVG_Immediate(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg);
static void MCU_Opcode_BSET_ORC(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg);
static void MCU_Opcode_BCLR_ANDC(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg);
static void MCU_Opcode_BTST(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg);
static void MCU_Opcode_CLR(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg);
static void MCU_Opcode_LDC(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg);
static void MCU_Opcode_STC(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg);
static void MCU_Opcode_BSET(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg);
static void MCU_Opcode_BCLR(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg);
static void MCU_Opcode_MOVG(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg);
static void MCU_Opcode_BTSTI(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg);
static void MCU_Opcode_BNOTI(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg);
static void MCU_Opcode_OR(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg);
static void MCU_Opcode_CMP(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg);
static void MCU_Opcode_ADDQ(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg);
static void MCU_Opcode_ADD(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg);
static void MCU_Opcode_SUB(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg);
static void MCU_Opcode_SUBS(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg);
static void MCU_Opcode_AND(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg);
static void MCU_Opcode_SHLR(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg);
static void MCU_Opcode_MULXU(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg);
static void MCU_Opcode_DIVXU(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg);
static void MCU_Opcode_ADDS(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg);
static void MCU_Opcode_XOR(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg);
static void MCU_Opcode_ADDX(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg);
static void MCU_Opcode_SUBX(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg);
static void MCU_ErrorTrap(mcu_t *mcu);
static uint8_t RCU_Read(void);
static uint16_t MCU_SC155Sliders(mcu_t *mcu, uint32_t index);
static uint16_t MCU_AnalogReadPin(mcu_t *mcu, uint32_t pin);
static void MCU_AnalogSample(mcu_t *mcu, uint8_t channel);
static void MCU_DeviceWrite(mcu_t *mcu, uint32_t address, uint8_t data);
static uint8_t MCU_DeviceRead(mcu_t *mcu, uint32_t address);
static void MCU_DeviceReset(mcu_t *mcu);
static void MCU_UpdateAnalog(mcu_t *mcu, uint64_t cycles);
static uint8_t MCU_Read(mcu_t *mcu, uint32_t address);
static uint16_t MCU_Read16(mcu_t *mcu, uint32_t address);
static uint32_t MCU_Read32(mcu_t *mcu, uint32_t address);
static void MCU_Write(mcu_t *mcu, uint32_t address, uint8_t value);
static void MCU_Write16(mcu_t *mcu, uint32_t address, uint16_t value);
static void MCU_ReadInstruction(mcu_t *mcu);
static void MCU_Init(mcu_t *mcu, submcu_t *sm, pcm_t *pcm, mcu_timer_t *timer);
static void MCU_Reset(mcu_t *mcu);
static void MCU_PostUART(mcu_t *mcu, uint8_t data);
static void MCU_UpdateUART_RX(mcu_t *mcu);
static void MCU_UpdateUART_TX(mcu_t *mcu);
static void MCU_Step(mcu_t *mcu);
static uint8_t MCU_ReadP0(mcu_t *mcu);
static uint8_t MCU_ReadP1(mcu_t *mcu);
static void MCU_WriteP0(mcu_t *mcu, uint8_t data);
static void MCU_WriteP1(mcu_t *mcu, uint8_t data);
static void MCU_PostSample(mcu_t *mcu, int32_t left, int32_t right);
static void MCU_GA_SetGAInt(mcu_t *mcu, uint8_t line, int value);
static void MCU_SetRomset(mcu_t *mcu, Romset romset);
static void (*MCU_Opcode_Table[32])(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg);
static void (*MCU_Operand_Table[256])(mcu_t *mcu, uint8_t operand);

SC55_INLINE uint32_t MCU_GetAddress(uint8_t page, uint16_t address) {
    return ((uint32_t)page << 16) + address;
}

SC55_INLINE uint8_t MCU_ReadCode(mcu_t *mcu) {
    /* Most fetches come from the low ROM; answer those without the
     * address decode. */
    if (mcu->cp == 0 && !(mcu->pc & 0x8000))
        return mcu->rom1[mcu->pc];
    return MCU_Read(mcu, MCU_GetAddress(mcu->cp, mcu->pc));
}

SC55_INLINE uint8_t MCU_ReadCodeAdvance(mcu_t *mcu) {
    uint8_t ret = MCU_ReadCode(mcu);
    mcu->pc++;
    return ret;
}

SC55_INLINE void MCU_SetRegisterByte(mcu_t *mcu, uint8_t reg, uint8_t val)
{
    mcu->r[reg] = val;
}

SC55_INLINE uint32_t MCU_GetVectorAddress(mcu_t *mcu, uint32_t vector)
{
    return MCU_Read32(mcu, vector * 4);
}

SC55_INLINE uint8_t MCU_GetPageForRegister(mcu_t *mcu, uint8_t reg)
{
    if (reg >= 6)
        return mcu->tp;
    else if (reg >= 4)
        return mcu->ep;
    return mcu->dp;
}

SC55_INLINE void MCU_ControlRegisterWrite(mcu_t *mcu, uint32_t reg, MCU_Operand_Size siz, uint32_t data)
{
    switch (siz)
    {
    case OPERAND_WORD:
        if (reg == 0)
        {
            mcu->sr = (uint16_t)data;
            mcu->sr &= sr_mask;
        }
        else if (reg == 5) /* FIXME: undocumented */
        {
            mcu->dp = (uint8_t)(data & 0xff);
        }
        else if (reg == 4) /* FIXME: undocumented */
        {
            mcu->ep = (uint8_t)(data & 0xff);
        }
        else if (reg == 3) /* FIXME: undocumented */
        {
            mcu->br = (uint8_t)(data & 0xff);
        }
        else
        {
            MCU_ErrorTrap(mcu);
        }
        break;
    case OPERAND_BYTE:
        if (reg == 1)
        {
            mcu->sr &= ~0xff;
            mcu->sr |= data & 0xff;
            mcu->sr &= sr_mask;
        }
        else if (reg == 3)
        {
            mcu->br = (uint8_t)data;
        }
        else if (reg == 4)
        {
            mcu->ep = (uint8_t)data;
        }
        else if (reg == 5)
        {
            mcu->dp = (uint8_t)data;
        }
        else if (reg == 7)
        {
            mcu->tp = (uint8_t)data;
        }
        else
        {
            MCU_ErrorTrap(mcu);
        }
        break;
    }
}

SC55_INLINE uint32_t MCU_ControlRegisterRead(mcu_t *mcu, uint32_t reg, MCU_Operand_Size siz)
{
    uint32_t ret = 0;
    switch (siz)
    {
    case OPERAND_WORD:
        if (reg == 0)
        {
            ret = mcu->sr & sr_mask;
        }
        else if (reg == 5) /* FIXME: undocumented */
        {
            ret = (uint32_t)mcu->dp | ((uint32_t)mcu->dp << 8);
        }
        else if (reg == 4) /* FIXME: undocumented */
        {
            ret = (uint32_t)mcu->ep | ((uint32_t)mcu->ep << 8);
        }
        else if (reg == 3) /* FIXME: undocumented */
        {
            ret = (uint32_t)mcu->br | ((uint32_t)mcu->br << 8);
        }
        else
        {
            MCU_ErrorTrap(mcu);
        }
        ret &= 0xffff;
        break;
    case OPERAND_BYTE:
        if (reg == 1)
        {
            ret = mcu->sr & sr_mask;
        }
        else if (reg == 3)
        {
            ret = mcu->br;
        }
        else if (reg == 4)
        {
            ret = mcu->ep;
        }
        else if (reg == 5)
        {
            ret = mcu->dp;
        }
        else if (reg == 7)
        {
            ret = mcu->tp;
        }
        else
        {
            MCU_ErrorTrap(mcu);
        }
        ret &= 0xff;
        break;
    }
    return ret;
}

SC55_INLINE void MCU_SetStatus(mcu_t *mcu, int condition, uint16_t mask)
{
    if (condition)
        mcu->sr |= mask;
    else
        mcu->sr &= ~mask;
}

SC55_INLINE void MCU_PushStack(mcu_t *mcu, uint16_t data)
{
    if (mcu->r[7] & 1)
        MCU_Interrupt_Exception(mcu, EXCEPTION_SOURCE_ADDRESS_ERROR);
    mcu->r[7] -= 2;
    MCU_Write16(mcu, mcu->r[7], data);
}

SC55_INLINE uint16_t MCU_PopStack(mcu_t *mcu)
{
    uint16_t ret;
    if (mcu->r[7] & 1)
        MCU_Interrupt_Exception(mcu, EXCEPTION_SOURCE_ADDRESS_ERROR);
    ret = MCU_Read16(mcu, mcu->r[7]);
    mcu->r[7] += 2;
    return ret;
}

static void MCU_Interrupt_Start(mcu_t *mcu, int32_t mask)
{
    MCU_PushStack(mcu, mcu->pc);
    MCU_PushStack(mcu, mcu->cp);
    MCU_PushStack(mcu, mcu->sr);
    mcu->sr &= ~STATUS_T;
    if (mask >= 0)
    {
        mcu->sr &= ~STATUS_INT_MASK;
        mcu->sr |= (uint16_t)(mask << 8);
    }
    mcu->sleep = 0;
}

static void MCU_Interrupt_SetRequest(mcu_t *mcu, MCU_Interrupt_Source interrupt, int value)
{
    if (value)
        mcu->interrupt_pending |= (uint32_t)1 << interrupt;
    else
        mcu->interrupt_pending &= ~((uint32_t)1 << interrupt);
}

static void MCU_Interrupt_Exception(mcu_t *mcu, MCU_Exception_Source exception)
{
#if 0
    if (interrupt == INTERRUPT_SOURCE_IRQ0 && (mcu->dev_register[DEV_P1CR] & 0x20) == 0)
        return;
    if (interrupt == INTERRUPT_SOURCE_IRQ1 && (mcu->dev_register[DEV_P1CR] & 0x40) == 0)
        return;
#endif
    mcu->exception_pending = exception;
}

static void MCU_Interrupt_TRAPA(mcu_t *mcu, uint8_t vector)
{
    mcu->trapa_pending |= (uint16_t)(1 << vector);
}

static void MCU_Interrupt_StartVector(mcu_t *mcu, uint32_t vector, int32_t mask)
{
    uint32_t address = MCU_GetVectorAddress(mcu, vector);
    MCU_Interrupt_Start(mcu, mask);
    mcu->cp = (uint8_t)(address >> 16);
    mcu->pc = (uint16_t)address;
}

static void MCU_Interrupt_GetVL(const mcu_t *mcu, uint32_t source, int32_t *vector, int32_t *level)
{
    switch (source)
    {
    case INTERRUPT_SOURCE_IRQ0:
        if ((mcu->dev_register[DEV_P1CR] & 0x20) == 0)
            break;
        *vector = VECTOR_IRQ0;
        *level  = (mcu->dev_register[DEV_IPRA] >> 4) & 7;
        break;
    case INTERRUPT_SOURCE_IRQ1:
        if ((mcu->dev_register[DEV_P1CR] & 0x40) == 0)
            break;
        *vector = VECTOR_IRQ1;
        *level  = (mcu->dev_register[DEV_IPRA] >> 0) & 7;
        break;
    case INTERRUPT_SOURCE_FRT0_OCIA:
        *vector = VECTOR_INTERNAL_INTERRUPT_94;
        *level  = (mcu->dev_register[DEV_IPRB] >> 4) & 7;
        break;
    case INTERRUPT_SOURCE_FRT0_OCIB:
        *vector = VECTOR_INTERNAL_INTERRUPT_98;
        *level  = (mcu->dev_register[DEV_IPRB] >> 4) & 7;
        break;
    case INTERRUPT_SOURCE_FRT0_FOVI:
        *vector = VECTOR_INTERNAL_INTERRUPT_9C;
        *level  = (mcu->dev_register[DEV_IPRB] >> 4) & 7;
        break;
    case INTERRUPT_SOURCE_FRT1_OCIA:
        *vector = VECTOR_INTERNAL_INTERRUPT_A4;
        *level  = (mcu->dev_register[DEV_IPRB] >> 0) & 7;
        break;
    case INTERRUPT_SOURCE_FRT1_OCIB:
        *vector = VECTOR_INTERNAL_INTERRUPT_A8;
        *level  = (mcu->dev_register[DEV_IPRB] >> 0) & 7;
        break;
    case INTERRUPT_SOURCE_FRT1_FOVI:
        *vector = VECTOR_INTERNAL_INTERRUPT_AC;
        *level  = (mcu->dev_register[DEV_IPRB] >> 0) & 7;
        break;
    case INTERRUPT_SOURCE_FRT2_OCIA:
        *vector = VECTOR_INTERNAL_INTERRUPT_B4;
        *level  = (mcu->dev_register[DEV_IPRC] >> 4) & 7;
        break;
    case INTERRUPT_SOURCE_FRT2_OCIB:
        *vector = VECTOR_INTERNAL_INTERRUPT_B8;
        *level  = (mcu->dev_register[DEV_IPRC] >> 4) & 7;
        break;
    case INTERRUPT_SOURCE_FRT2_FOVI:
        *vector = VECTOR_INTERNAL_INTERRUPT_BC;
        *level  = (mcu->dev_register[DEV_IPRC] >> 4) & 7;
        break;
    case INTERRUPT_SOURCE_TIMER_CMIA:
        *vector = VECTOR_INTERNAL_INTERRUPT_C0;
        *level  = (mcu->dev_register[DEV_IPRC] >> 0) & 7;
        break;
    case INTERRUPT_SOURCE_TIMER_CMIB:
        *vector = VECTOR_INTERNAL_INTERRUPT_C4;
        *level  = (mcu->dev_register[DEV_IPRC] >> 0) & 7;
        break;
    case INTERRUPT_SOURCE_TIMER_OVI:
        *vector = VECTOR_INTERNAL_INTERRUPT_C8;
        *level  = (mcu->dev_register[DEV_IPRC] >> 0) & 7;
        break;
    case INTERRUPT_SOURCE_ANALOG:
        *vector = VECTOR_INTERNAL_INTERRUPT_E0;
        *level  = (mcu->dev_register[DEV_IPRD] >> 0) & 7;
        break;
    case INTERRUPT_SOURCE_UART_RX:
        *vector = VECTOR_INTERNAL_INTERRUPT_D4;
        *level  = (mcu->dev_register[DEV_IPRD] >> 4) & 7;
        break;
    case INTERRUPT_SOURCE_UART_TX:
        *vector = VECTOR_INTERNAL_INTERRUPT_D8;
        *level  = (mcu->dev_register[DEV_IPRD] >> 4) & 7;
        break;
    default:
        break;
    }
}

static void MCU_Interrupt_Handle(mcu_t *mcu)
{
    uint32_t mask;
    uint32_t pending;

    if (mcu->trapa_pending)
    {
        unsigned vector = sc55_ctz(mcu->trapa_pending);
        mcu->trapa_pending &= (uint16_t)~(1 << vector);
        MCU_Interrupt_StartVector(mcu, VECTOR_TRAPA_0 + vector, -1);
        return;
    }
    if (mcu->exception_pending >= 0)
    {
        switch (mcu->exception_pending)
        {
            case EXCEPTION_SOURCE_ADDRESS_ERROR:
                MCU_Interrupt_StartVector(mcu, VECTOR_ADDRESS_ERROR, -1);
                break;
            case EXCEPTION_SOURCE_INVALID_INSTRUCTION:
                MCU_Interrupt_StartVector(mcu, VECTOR_INVALID_INSTRUCTION, -1);
                break;
            case EXCEPTION_SOURCE_TRACE:
                MCU_Interrupt_StartVector(mcu, VECTOR_TRACE, -1);
                break;
        }
        mcu->exception_pending = (MCU_Exception_Source)-1;
        return;
    }
    pending = mcu->interrupt_pending;
    if (!pending)
        return;
    if (pending & ((uint32_t)1 << INTERRUPT_SOURCE_NMI))
    {
        MCU_Interrupt_StartVector(mcu, VECTOR_NMI, 7);
        return;
    }
    /* Sources are served lowest number first. */
    mask = (mcu->sr >> 8) & 7;
    for (; pending; pending &= pending - 1)
    {
        int32_t vector = -1;
        int32_t level = 0;
        MCU_Interrupt_GetVL(mcu, sc55_ctz(pending), &vector, &level);
        if ((int32_t)mask < level)
        {
            MCU_Interrupt_StartVector(mcu, (uint32_t)vector, level);
            return;
        }
    }
}

enum TMR_TCR_Bits
{
    TMR_TCR_CKS0  = 1 << 0, /* Clock Select 0 */
    TMR_TCR_CKS1  = 1 << 1, /* Clock Select 1 */
    TMR_TCR_CKS2  = 1 << 2, /* Clock Select 2 */
    TMR_TCR_CCLR0 = 1 << 3, /* Counter Clear 0 */
    TMR_TCR_CCLR1 = 1 << 4, /* Counter Clear 1 */
    TMR_TCR_OVIE  = 1 << 5, /* Timer Overflow Interrupt Enable */
    TMR_TCR_CMIEA = 1 << 6, /* Compare-match Interrupt Enable A */
    TMR_TCR_CMIEB = 1 << 7 /* Compare-match Interrupt Enable B */
};

enum TMR_TCSR_Bits
{
    TMR_TCSR_OS0  = 1 << 0, /* Output Select 0 */
    TMR_TCSR_OS1  = 1 << 1, /* Output Select 1 */
    TMR_TCSR_OS2  = 1 << 2, /* Output Select 2 */
    TMR_TCSR_OS3  = 1 << 3, /* Output Select 3 */
    TMR_TCSR_BIT4 = 1 << 4, /* Reserved */
    TMR_TCSR_OVF  = 1 << 5, /* Timer Overflow Flag */
    TMR_TCSR_CMFA = 1 << 6, /* Compare-Match Flag A */
    TMR_TCSR_CMFB = 1 << 7 /* Compare-Match Flag B */
};

enum FRT_TCR_Bits
{
    FRT_TCR_CKS0  = 1 << 0, /* Clock Select 0 */
    FRT_TCR_CKS1  = 1 << 1, /* Clock Select 1 */
    FRT_TCR_OEA   = 1 << 2, /* Output Enable A */
    FRT_TCR_OEB   = 1 << 3, /* Output Enable B */
    FRT_TCR_OVIE  = 1 << 4, /* Timer overflow Interrupt Enable */
    FRT_TCR_OCIEA = 1 << 5, /* Output Compare Interrupt Enable A */
    FRT_TCR_OCIEB = 1 << 6, /* Output Compare Interrupt Enable B */
    FRT_TCR_ICIE  = 1 << 7 /* Input Capture Interrupt Enable */
};

enum FRT_TCSR_Bits
{
    FRT_TCSR_CCLRA = 1 << 0, /* Counter Clear A */
    FRT_TCSR_IEDG  = 1 << 1, /* Input Edge Select */
    FRT_TCSR_OLVLA = 1 << 2, /* Output Level A */
    FRT_TCSR_OLVLB = 1 << 3, /* Output Level B */
    FRT_TCSR_OVF   = 1 << 4, /* Timer Overflow Flag */
    FRT_TCSR_OCFA  = 1 << 5, /* Output Compare Flag A */
    FRT_TCSR_OCFB  = 1 << 6, /* Output Compare Flag B */
    FRT_TCSR_ICF   = 1 << 7 /* Input Capture Flag */
};

/* Values are byte offsets from start of FRTs in memory (ffa0, ffb0, ffc0) */
enum FRT_Field_Offset
{
    REG_TCR   = 0x00,
    REG_TCSR  = 0x01,
    REG_FRCH  = 0x02,
    REG_FRCL  = 0x03,
    REG_OCRAH = 0x04,
    REG_OCRAL = 0x05,
    REG_OCRBH = 0x06,
    REG_OCRBL = 0x07,
    REG_ICRH  = 0x08,
    REG_ICRL  = 0x09
};

/* Calculates the next deadline for a timer. This is actually just a */
/* pointer alignment algorithm. `interval` must be power-of-two. */
SC55_INLINE uint64_t AlignForward(uint64_t value, uint64_t interval)
{
    return (value + (interval - 1)) & (~(interval - 1));
}

static void TIMER_Init(mcu_timer_t *timer, mcu_t *mcu)
{
    timer->mcu = mcu;
}

static void TIMER_Reset(mcu_timer_t *timer)
{
    int i;
    for (i = 0; i < 3; ++i)
    {
        memset(&timer->frt[i], 0, sizeof(timer->frt[i]));
        timer->frt[i].ocra   = 0xffff;
        timer->frt[i].ocrb   = 0xffff;
        timer->frt[i].stride = 4;
    }
    memset(&timer->tmr, 0, sizeof(timer->tmr));
    timer->tmr.deadline = ~(uint64_t)0;
    timer->tmr.tcsr     = TMR_TCSR_BIT4;
    timer->tmr.tcora    = 0xff;
    timer->tmr.tcorb    = 0xff;
}

static void TIMER_WriteFRT(mcu_timer_t *timer, uint32_t address, uint8_t data)
{
    frt_t *frt;
    uint32_t t = (address >> 4) - 1;
    if (t > 2)
        return;
    frt = &timer->frt[t];

    address &= 0x0f;
    switch (address)
    {
    case REG_TCR: {
        uint8_t stride;
        frt->tcr = data;

        stride = timer->frt_step_table[frt->tcr & (FRT_TCR_CKS0 | FRT_TCR_CKS1)];

        frt->deadline = AlignForward(timer->cycles, stride);
        frt->stride   = stride;

        break;
    }
    case REG_TCSR:
        frt->tcsr &= ~0xf;
        frt->tcsr |= data & 0xf;
        if ((data & FRT_TCSR_OVF) == 0 && (frt->status_rd & FRT_TCSR_OVF) != 0)
        {
            frt->tcsr      &= ~FRT_TCSR_OVF;
            frt->status_rd &= ~FRT_TCSR_OVF;
            MCU_Interrupt_SetRequest(timer->mcu, (MCU_Interrupt_Source)(INTERRUPT_SOURCE_FRT0_FOVI + t * 4), 0);
        }
        if ((data & FRT_TCSR_OCFA) == 0 && (frt->status_rd & FRT_TCSR_OCFA) != 0)
        {
            frt->tcsr      &= ~FRT_TCSR_OCFA;
            frt->status_rd &= ~FRT_TCSR_OCFA;
            MCU_Interrupt_SetRequest(timer->mcu, (MCU_Interrupt_Source)(INTERRUPT_SOURCE_FRT0_OCIA + t * 4), 0);
        }
        if ((data & FRT_TCSR_OCFB) == 0 && (frt->status_rd & FRT_TCSR_OCFB) != 0)
        {
            frt->tcsr      &= ~FRT_TCSR_OCFB;
            frt->status_rd &= ~FRT_TCSR_OCFB;
            MCU_Interrupt_SetRequest(timer->mcu, (MCU_Interrupt_Source)(INTERRUPT_SOURCE_FRT0_OCIB + t * 4), 0);
        }
        break;
    case REG_FRCH:
    case REG_OCRAH:
    case REG_OCRBH:
    case REG_ICRH:
        timer->tempreg = data;
        break;
    case REG_FRCL:
        frt->frc = (uint16_t)((timer->tempreg << 8) | data);
        break;
    case REG_OCRAL:
        frt->ocra = (uint16_t)((timer->tempreg << 8) | data);
        break;
    case REG_OCRBL:
        frt->ocrb = (uint16_t)((timer->tempreg << 8) | data);
        break;
    case REG_ICRL:
        frt->icr = (uint16_t)((timer->tempreg << 8) | data);
        break;
    }
}

static uint8_t TIMER_ReadFRT(mcu_timer_t *timer, uint32_t address)
{
    frt_t *frt;
    uint32_t t = (address >> 4) - 1;
    if (t > 2)
        return 0xff;
    frt = &timer->frt[t];

    address &= 0x0f;
    switch (address)
    {
    case REG_TCR:
        return frt->tcr;
    case REG_TCSR: {
        uint8_t ret    = frt->tcsr;
        frt->status_rd |= frt->tcsr & 0xf0;
        /* frt->status_rd |= 0xf0; */
        return ret;
    }
    case REG_FRCH:
        timer->tempreg = (uint8_t)frt->frc;
        return (uint8_t)(frt->frc >> 8);
    case REG_OCRAH:
        timer->tempreg = (uint8_t)frt->ocra;
        return (uint8_t)(frt->ocra >> 8);
    case REG_OCRBH:
        timer->tempreg = (uint8_t)frt->ocrb;
        return (uint8_t)(frt->ocrb >> 8);
    case REG_ICRH:
        timer->tempreg = (uint8_t)frt->icr;
        return (uint8_t)(frt->icr >> 8);
    case REG_FRCL:
    case REG_OCRAL:
    case REG_OCRBL:
    case REG_ICRL:
        return timer->tempreg;
    }
    return 0xff;
}

static void TIMER_WriteTMR(mcu_timer_t *timer, uint32_t address, uint8_t data)
{
    tmr_t *tmr = &timer->tmr;

    switch (address)
    {
    case DEV_TMR_TCR: {
        uint16_t stride;
        tmr->tcr = data;

        stride = timer->tmr_step_table[tmr->tcr & (TMR_TCR_CKS0 | TMR_TCR_CKS1 | TMR_TCR_CKS2)];

        if (stride == 0)
        {
            tmr->deadline = (uint64_t)(-1);
        }
        else
        {
            tmr->deadline = AlignForward(timer->cycles, stride);
        }

        tmr->stride = stride;

        break;
    }
    case DEV_TMR_TCSR:
        tmr->tcsr &= ~0xf;
        tmr->tcsr |= data & 0xf;
        if ((data & TMR_TCSR_OVF) == 0 && (tmr->status_rd & TMR_TCSR_OVF) != 0)
        {
            tmr->tcsr      &= ~TMR_TCSR_OVF;
            tmr->status_rd &= ~TMR_TCSR_OVF;
            MCU_Interrupt_SetRequest(timer->mcu, INTERRUPT_SOURCE_TIMER_OVI, 0);
        }
        if ((data & TMR_TCSR_CMFA) == 0 && (tmr->status_rd & TMR_TCSR_CMFA) != 0)
        {
            tmr->tcsr      &= ~TMR_TCSR_CMFA;
            tmr->status_rd &= ~TMR_TCSR_CMFA;
            MCU_Interrupt_SetRequest(timer->mcu, INTERRUPT_SOURCE_TIMER_CMIA, 0);
        }
        if ((data & TMR_TCSR_CMFB) == 0 && (tmr->status_rd & TMR_TCSR_CMFB) != 0)
        {
            tmr->tcsr      &= ~TMR_TCSR_CMFB;
            tmr->status_rd &= ~TMR_TCSR_CMFB;
            MCU_Interrupt_SetRequest(timer->mcu, INTERRUPT_SOURCE_TIMER_CMIB, 0);
        }
        break;
    case DEV_TMR_TCORA:
        tmr->tcora = data;
        break;
    case DEV_TMR_TCORB:
        tmr->tcorb = data;
        break;
    case DEV_TMR_TCNT:
        tmr->tcnt = data;
        break;
    }
}

static uint8_t TIMER_ReadTMR(mcu_timer_t *timer, uint32_t address)
{
    tmr_t *tmr = &timer->tmr;

    switch (address)
    {
    case DEV_TMR_TCR:
        return tmr->tcr;
    case DEV_TMR_TCSR: {
        uint8_t ret    = tmr->tcsr;
        tmr->status_rd |= tmr->tcsr & (TMR_TCSR_OVF | TMR_TCSR_CMFA | TMR_TCSR_CMFB);
        return ret;
    }
    case DEV_TMR_TCORA:
        return tmr->tcora;
    case DEV_TMR_TCORB:
        return tmr->tcorb;
    case DEV_TMR_TCNT:
        return tmr->tcnt;
    }
    return 0xff;
}

SC55_INLINE void TIMER_ClockFrt(mcu_timer_t *timer, int frt_id)
{
    frt_t *frt = &timer->frt[frt_id];

    const int matcha = frt->frc == frt->ocra;
    const int matchb = frt->frc == frt->ocrb;
    if ((frt->tcsr & FRT_TCSR_CCLRA) && matcha) /* CCLRA */
    {
        frt->frc = 0;
    }
    else
    {
        ++frt->frc;
        if (frt->frc == 0)
        {
            frt->tcsr |= FRT_TCSR_OVF;
        }
    }

    /* flags */
    if (matcha)
        frt->tcsr |= FRT_TCSR_OCFA;
    if (matchb)
        frt->tcsr |= FRT_TCSR_OCFB;

    if ((frt->tcr & FRT_TCR_OVIE) != 0 && (frt->tcsr & FRT_TCSR_OVF) != 0)
        MCU_Interrupt_SetRequest(timer->mcu, (MCU_Interrupt_Source)(INTERRUPT_SOURCE_FRT0_FOVI + frt_id * 4), 1);
    if ((frt->tcr & FRT_TCR_OCIEA) != 0 && (frt->tcsr & FRT_TCSR_OCFA) != 0)
        MCU_Interrupt_SetRequest(timer->mcu, (MCU_Interrupt_Source)(INTERRUPT_SOURCE_FRT0_OCIA + frt_id * 4), 1);
    if ((frt->tcr & FRT_TCR_OCIEB) != 0 && (frt->tcsr & FRT_TCSR_OCFB) != 0)
        MCU_Interrupt_SetRequest(timer->mcu, (MCU_Interrupt_Source)(INTERRUPT_SOURCE_FRT0_OCIB + frt_id * 4), 1);
}

SC55_INLINE void TIMER_ClockTmr(mcu_timer_t *timer)
{
    tmr_t *tmr = &timer->tmr;

    const int matcha = tmr->tcnt == tmr->tcora;
    const int matchb = tmr->tcnt == tmr->tcorb;
    if ((tmr->tcr & (TMR_TCR_CCLR0 | TMR_TCR_CCLR1)) == TMR_TCR_CCLR0 && matcha)
    {
        tmr->tcnt = 0;
    }
    else if ((tmr->tcr & (TMR_TCR_CCLR0 | TMR_TCR_CCLR1)) == TMR_TCR_CCLR1 && matchb)
    {
        tmr->tcnt = 0;
    }
    else
    {
        ++tmr->tcnt;
        if (tmr->tcnt == 0)
        {
            tmr->tcsr |= TMR_TCSR_OVF;
        }
    }

    /* flags */
    if (matcha)
        tmr->tcsr |= TMR_TCSR_CMFA;
    if (matchb)
        tmr->tcsr |= TMR_TCSR_CMFB;

    if ((tmr->tcr & TMR_TCR_OVIE) != 0 && (tmr->tcsr & TMR_TCSR_OVF) != 0)
        MCU_Interrupt_SetRequest(timer->mcu, INTERRUPT_SOURCE_TIMER_OVI, 1);
    if ((tmr->tcr & TMR_TCR_CMIEA) != 0 && (tmr->tcsr & TMR_TCSR_CMFA) != 0)
        MCU_Interrupt_SetRequest(timer->mcu, INTERRUPT_SOURCE_TIMER_CMIA, 1);
    if ((tmr->tcr & TMR_TCR_CMIEB) != 0 && (tmr->tcsr & TMR_TCSR_CMFB) != 0)
        MCU_Interrupt_SetRequest(timer->mcu, INTERRUPT_SOURCE_TIMER_CMIB, 1);
}

static void TIMER_Clock(mcu_timer_t *timer, uint64_t cycles)
{
    int i;
    const uint64_t target_cycles = cycles / 2;

    timer->cycles = target_cycles;

    for (i = 0; i < 3; i++)
    {
        while (timer->frt[i].deadline < target_cycles)
        {
            TIMER_ClockFrt(timer, i);
            timer->frt[i].deadline += timer->frt[i].stride;
        }
    }

    while (timer->tmr.deadline < target_cycles)
    {
        TIMER_ClockTmr(timer);
        timer->tmr.deadline += timer->tmr.stride;
    }
}

static void TIMER_NotifyRomsetChange(mcu_timer_t *timer)
{
    /* Indexed by the low CKSn bits of the TCR.  A step of 0 means do
     * not step. */
    static const uint8_t  frt_generic[4] = {4, 8, 32, 2};
    static const uint8_t  frt_mk1[4]     = {4, 8, 32, 4};
    static const uint16_t tmr_generic[8] = {0, 8, 64, 1024, 0, 2, 2, 2};
    static const uint16_t tmr_mk1[8]     = {0, 8, 64, 1024, 0, 4, 4, 4};
    const int is_mk1 = timer->mcu->is_mk1;
    memcpy(timer->frt_step_table, is_mk1 ? frt_mk1 : frt_generic, sizeof(timer->frt_step_table));
    memcpy(timer->tmr_step_table, is_mk1 ? tmr_mk1 : tmr_generic, sizeof(timer->tmr_step_table));
}

SC55_INLINE uint8_t PCM_ReadROM(pcm_t *pcm, uint32_t address)
{
    int bank;
    if (pcm->config_reg_3d & 0x20)
        bank = (address >> 21) & 7;
    else
        bank = (address >> 19) & 7;
    switch (bank)
    {
        case 0:
            if (pcm->mcu->is_mk1)
                return pcm->waverom1[address & 0xfffff];
            else
                return pcm->waverom1[address & 0x1fffff];
        case 1:
            return pcm->waverom2[address & 0xfffff];
        case 2:
            return pcm->waverom3[address & 0xfffff];
        default:
            break;
    }
    return 0;
}

static void PCM_Write(pcm_t *pcm, uint32_t address, uint8_t data)
{
    address &= 0x3f;
    if (address < 0x4) /* voice enable */
    {
        switch (address & 3)
        {
            case 0:
                pcm->voice_mask_pending &= ~0xf000000u;
                pcm->voice_mask_pending |= (uint32_t)(data & 0xf) << 24;
                break;
            case 1:
                pcm->voice_mask_pending &= ~0xff0000u;
                pcm->voice_mask_pending |= (uint32_t)(data & 0xff) << 16;
                break;
            case 2:
                pcm->voice_mask_pending &= ~0xff00u;
                pcm->voice_mask_pending |= (uint32_t)(data & 0xff) << 8;
                break;
            case 3:
                pcm->voice_mask_pending &= ~0xffu;
                pcm->voice_mask_pending |= (uint32_t)(data & 0xff) << 0;
                break;
        }
        pcm->voice_mask_updating = 1;
    }
    else if (address >= 0x20 && address < 0x24) /* wave rom */
    {
        switch (address & 3)
        {
            case 1:
                pcm->wave_read_address &= ~0xff0000u;
                pcm->wave_read_address |= (uint32_t)(data & 0xff) << 16;
                break;
            case 2:
                pcm->wave_read_address &= ~0xff00u;
                pcm->wave_read_address |= (uint32_t)(data & 0xff) << 8;
                break;
            case 3:
                pcm->wave_read_address &= ~0xffu;
                pcm->wave_read_address |= (uint32_t)(data & 0xff) << 0;
                pcm->wave_byte_latch = PCM_ReadROM(pcm, pcm->wave_read_address);
                break;
        }
    }
    else if (address == 0x3c)
    {
        pcm->config_reg_3c = data;
        PCM_GetConfig(&pcm->config, data);
    }
    else if (address == 0x3d)
    {
        pcm->config_reg_3d = data;
        pcm->config.reg_slots = (data & 31) + 1;
    }
    else if (address == 0x3e)
    {
        pcm->select_channel = data & 0x1f;
    }
    else if ((address >= 0x4 && address < 0x10) || (address >= 0x24 && address < 0x30))
    {
        switch (address & 3)
        {
            case 1:
                pcm->write_latch &= ~0xf0000u;
                pcm->write_latch |= (uint32_t)(data & 0xf) << 16;
                break;
            case 2:
                pcm->write_latch &= ~0xff00u;
                pcm->write_latch |= (uint32_t)(data & 0xff) << 8;
                break;
            case 3:
                pcm->write_latch &= ~0xffu;
                pcm->write_latch |= (uint32_t)(data & 0xff) << 0;
                break;
        }
        if ((address & 3) == 3)
        {
            int ix = 0;
            if (address & 32)
                ix |= 1;
            if ((address & 8) == 0)
                ix |= 4;
            if ((address & 4) == 0)
                ix |= 2;

            pcm->ram1[pcm->select_channel][ix] = pcm->write_latch;
        }
    }
    else if ((address >= 0x10 && address < 0x20) || (address >= 0x30 && address < 0x38))
    {
        switch (address & 1)
        {
        case 0:
            pcm->write_latch &= ~0xff00u;
            pcm->write_latch |= (uint32_t)(data & 0xff) << 8;
            break;
        case 1:
            pcm->write_latch &= ~0xffu;
            pcm->write_latch |= (uint32_t)(data & 0xff) << 0;
            break;
        }
        if ((address & 1) == 1)
        {
            int ix = (address >> 1) & 7;
            if (address & 32)
                ix |= 8;

            pcm->ram2[pcm->select_channel][ix] = (uint16_t)(pcm->write_latch);
        }
    }
}

/* rv: [30][2], [30][3] */
/* ch: [31][2], [31][5] */

static uint8_t PCM_Read(pcm_t *pcm, uint32_t address)
{
    address &= 0x3f;
    /* fprintf(stderr, "PCM Read: %.2x\n", address); */

    if (address < 0x4)
    {
        if (pcm->voice_mask_updating)
            pcm->voice_mask = pcm->voice_mask_pending;
        pcm->voice_mask_updating = 0;
    }
    else if (address == 0x3c || address == 0x3e) /* status */
    {
        uint8_t status = 0;
        if (address == 0x3e && pcm->irq_assert)
        {
            pcm->irq_assert = 0;
            if (pcm->mcu->is_jv880)
                MCU_GA_SetGAInt(pcm->mcu, 5, 0);
            else
                MCU_Interrupt_SetRequest(pcm->mcu, INTERRUPT_SOURCE_IRQ0, 0);
        }

        status |= pcm->irq_channel;
        if (pcm->voice_mask_updating)
            status |= 32;

        return status;
    }
    else if (address == 0x3f)
    {
        return pcm->wave_byte_latch;
    }
    else if ((address >= 0x4 && address < 0x10) || (address >= 0x24 && address < 0x30))
    {
        if ((address & 3) == 1)
        {
            int ix = 0;
            if (address & 32)
                ix |= 1;
            if ((address & 8) == 0)
                ix |= 4;
            if ((address & 4) == 0)
                ix |= 2;

            pcm->read_latch = pcm->ram1[pcm->select_channel][ix];
        }
    }
    else if ((address >= 0x10 && address < 0x20) || (address >= 0x30 && address < 0x38))
    {
        if ((address & 1) == 0)
        {
            int ix = (address >> 1) & 7;
            if (address & 32)
                ix |= 8;

            pcm->read_latch = pcm->ram2[pcm->select_channel][ix];
        }
    }
    else if (address >= 0x39 && address <= 0x3b)
    {
        switch (address & 3)
        {
            case 1:
                return (pcm->read_latch >> 16) & 0xf;
            case 2:
                return (pcm->read_latch >> 8) & 0xff;
            case 3:
                return (pcm->read_latch >> 0) & 0xff;
        }
    }

    return 0;
}

static void PCM_Init(pcm_t *pcm, mcu_t *mcu)
{
    pcm->mcu = mcu;
}

/* Sign-extends a 20-bit signed integer to a 32-bit signed integer. */
SC55_INLINE int32_t sx20(int32_t in)
{
    return SC55_SHL(in, 12) >> 12;
}

SC55_INLINE int32_t addclip20(int32_t add1, int32_t add2, int32_t cin)
{
    return sx20(add1) + sx20(add2) + cin;
}

SC55_INLINE int32_t multi(int32_t val1, int8_t val2)
{
    return sx20(val1) * val2;
}

static const int interp_lut[3][128] = {
    {
        3385, 3401, 3417, 3432, 3448, 3463, 3478, 3492, 3506, 3521, 3535, 3548, 3562, 3575, 3588, 3601,
        3614, 3626, 3638, 3650, 3662, 3673, 3685, 3696, 3707, 3718, 3728, 3739, 3749, 3759, 3768, 3778,
        3787, 3796, 3805, 3814, 3823, 3831, 3839, 3847, 3855, 3863, 3870, 3878, 3885, 3892, 3899, 3905,
        3912, 3918, 3924, 3930, 3936, 3942, 3948, 3953, 3958, 3963, 3968, 3973, 3978, 3983, 3987, 3991,
        3995, 4000, 4004, 4007, 4011, 4015, 4018, 4022, 4025, 4028, 4031, 4034, 4037, 4040, 4042, 4045,
        4047, 4050, 4052, 4054, 4057, 4059, 4061, 4063, 4064, 4066, 4068, 4070, 4071, 4073, 4074, 4076,
        4077, 4078, 4079, 4081, 4082, 4083, 4084, 4085, 4086, 4086, 4087, 4088, 4089, 4089, 4090, 4091,
        4091, 4092, 4092, 4093, 4093, 4094, 4094, 4094, 4094, 4095, 4095, 4095, 4095, 4095, 4095, 4095,
    },

    {
        710, 726, 742, 758, 775, 792, 809, 826, 844, 861, 879, 897, 915, 933, 952, 971,
        990, 1009, 1028, 1047, 1067, 1087, 1106, 1126, 1147, 1167, 1188, 1208, 1229, 1250, 1271, 1292,
        1314, 1335, 1357, 1379, 1400, 1423, 1445, 1467, 1489, 1512, 1534, 1557, 1580, 1602, 1625, 1648,
        1671, 1695, 1718, 1741, 1764, 1788, 1811, 1835, 1858, 1882, 1906, 1929, 1953, 1977, 2000, 2024,
        2048, 2071, 2095, 2119, 2143, 2166, 2190, 2214, 2237, 2261, 2284, 2308, 2331, 2355, 2378, 2401,
        2425, 2448, 2471, 2494, 2517, 2539, 2562, 2585, 2607, 2630, 2652, 2674, 2696, 2718, 2740, 2762,
        2783, 2805, 2826, 2847, 2868, 2889, 2910, 2931, 2951, 2971, 2991, 3011, 3031, 3051, 3070, 3089,
        3108, 3127, 3146, 3164, 3182, 3200, 3218, 3236, 3253, 3271, 3288, 3304, 3321, 3338, 3354, 3370,
    },

    {
        0, 0, 0, 1, 1, 1, 2, 2, 3, 3, 3, 4, 4, 5, 5, 6,
        6, 7, 8, 8, 9, 10, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
        20, 22, 23, 24, 26, 27, 29, 30, 32, 34, 36, 38, 40, 42, 44, 46,
        49, 51, 53, 56, 59, 62, 65, 68, 71, 74, 77, 81, 84, 88, 92, 96,
        100, 104, 109, 113, 118, 122, 127, 132, 137, 143, 148, 154, 160, 165, 171, 178,
        184, 191, 197, 204, 211, 219, 226, 234, 241, 249, 257, 266, 274, 283, 292, 301,
        310, 319, 329, 339, 349, 359, 369, 380, 391, 402, 413, 424, 436, 448, 460, 472,
        484, 497, 510, 523, 536, 549, 563, 577, 591, 605, 619, 634, 648, 663, 679, 694,
    },
};

SC55_INLINE void calc_tv(pcm_t *pcm, int e, int adjust, uint16_t *levelcur, int active, int *volmul)
{
    int speed;
    int target;
    int w1;
    int w2;
    int w3;
    int type;
    int write;
    int addlow;
    /* int adjust = ram2[3+e]; */
    /* int levelcur = ram2[9+e] & 0x7fff; */
    *levelcur &= 0x7fff;
    speed = adjust & 0xff;
    target = (adjust >> 8) & 0xff;

    w1 = (speed & 0xf0) == 0;
    w2 = w1 || (speed & 0x10) != 0;
    w3 = pcm->nfs &&
        ((speed & 0x80) == 0 || ((speed & 0x40) == 0 && (!w2 || (speed & 0x20) == 0)));

    type = (int)w2 | ((int)w3 << 3);
    if (speed & 0x20)
        type |= 2;
    if ((speed & 0x80) == 0 || (speed & 0x40) == 0)
        type |= 4;

    write = !active;
    addlow = 0;
    if (type & 4)
    {
        if (pcm->tv_counter & 8)
            addlow |= 1;
        if (pcm->tv_counter & 4)
            addlow |= 2;
        if (pcm->tv_counter & 2)
            addlow |= 4;
        if (pcm->tv_counter & 1)
            addlow |= 8;
        write |= 1;
    }
    else
    {
        switch (type & 3)
        {
        case 0:
            if (pcm->tv_counter & 0x20)
                addlow |= 1;
            if (pcm->tv_counter & 0x10)
                addlow |= 2;
            if (pcm->tv_counter & 8)
                addlow |= 4;
            if (pcm->tv_counter & 4)
                addlow |= 8;
            write |= (pcm->tv_counter & 3) == 0;
            break;
        case 1:
            if (pcm->tv_counter & 0x80)
                addlow |= 1;
            if (pcm->tv_counter & 0x40)
                addlow |= 2;
            if (pcm->tv_counter & 0x20)
                addlow |= 4;
            if (pcm->tv_counter & 0x10)
                addlow |= 8;
            write |= (pcm->tv_counter & 15) == 0;
            break;
        case 2:
            if (pcm->tv_counter & 0x200)
                addlow |= 1;
            if (pcm->tv_counter & 0x100)
                addlow |= 2;
            if (pcm->tv_counter & 0x80)
                addlow |= 4;
            if (pcm->tv_counter & 0x40)
                addlow |= 8;
            write |= (pcm->tv_counter & 63) == 0;
            break;
        case 3:
            if (pcm->tv_counter & 0x800)
                addlow |= 1;
            if (pcm->tv_counter & 0x400)
                addlow |= 2;
            if (pcm->tv_counter & 0x200)
                addlow |= 4;
            if (pcm->tv_counter & 0x100)
                addlow |= 8;
            write |= (pcm->tv_counter & 127) == 0;
            break;
        }
    }

    if ((type & 8) == 0)
    {
        int sum1;
        int neg;
        int preshift;
        int shifted;
        int sum2;
        int shift = speed & 15;
        shift = (10 - shift) & 15;

        sum1 = (target << 11); /* 5 */
        if (e != 2 || active)
            sum1 -= (*levelcur << 4); /* 6 */
        neg = (sum1 & 0x80000) != 0;
        (void)neg; /* unused */

        preshift = sum1;

        shifted = preshift >> shift;
        shifted -= sum1;

        sum2 = (target << 11) + addlow + shifted;
        if (write && pcm->nfs)
            *levelcur = (sum2 >> 4) & 0x7fff;

        if (e == 0)
        {
            *volmul = (sum2 >> 4) & 0x7ffe;
        }
        else if (e == 1)
        {
            *volmul = (sum2 >> 4) & 0x7ffe;
        }
    }
    else
    {
        int sum1;
        int neg;
        int preshift;
        int shifted;
        int sum2;
        int sum2_l;
        int sum3;
        int neg2;
        int xnor;
        int shift = (speed >> 4) & 14;
        shift |= (int)w2;
        shift = (10 - shift) & 15;

        sum1 = target << 11; /* 5 */
        if (e != 2 || active)
            sum1 -= (*levelcur << 4); /* 6 */
        neg = (sum1 & 0x80000) != 0;
        preshift = (speed & 15) << 9;
        if (!w1)
            preshift |= 0x2000;
        if (neg)
            preshift ^= ~0x3f;

        shifted = preshift >> shift;
        sum2 = shifted;
        if (e != 2 || active)
            sum2 += (*levelcur << 4) | addlow;

        sum2_l = (sum2 >> 4);

        sum3 = (target << 11) - SC55_SHL(sum2_l, 4);

        neg2 = (sum3 & 0x80000) != 0;
        xnor = !(neg2 ^ neg);

        if (write && pcm->nfs)
        {
            if (xnor)
                *levelcur = sum2_l & 0x7fff;
            else
                *levelcur = (uint16_t)(target << 7);
        }

        if (e == 0)
        {
            *volmul = sum2_l & 0x7ffe;
        }
        else if (e == 1)
        {
            if (xnor)
                *volmul = sum2_l & 0x7ffe;
            else
                *volmul = target << 7;
        }
    }
}

SC55_INLINE int eram_unpack(pcm_t *pcm, uint32_t addr, int type)
{
    int data;
    int val;
    int sh;
    addr &= 0x3fff;
    data = pcm->eram[addr];
    val = data & 0x3fff;
    sh = (data >> 14) & 3;

    val = SC55_SHL(val, 18);
    return val >> (18 - sh * 2 + type);
}

SC55_INLINE void eram_pack(pcm_t *pcm, uint32_t addr, uint32_t val)
{
    int sh;
    int top;
    int data;
    addr &= 0x3fff;
    sh = 0;
    top = (val >> 13) & 0x7f;
    if (top & 0x40)
        top ^= 0x7f;
    if (top >= 16)
        sh = 3;
    else if (top >= 4)
        sh = 2;
    else if (top >= 1)
        sh = 1;
    else
        sh = 0;

    data = (val >> (sh * 2)) & 0x3fff;
    data |= sh << 14;
    pcm->eram[addr] = (uint16_t)data;
}

static void PCM_GetConfig(PCM_Config *config, uint8_t config_byte)
{
    if ((config_byte & 0x30) != 0)
    {
        switch ((config_byte >> 2) & 3)
        {
        case 1:
            config->noise_mask = 3;
            break;
        case 2:
            config->noise_mask = 7;
            break;
        case 3:
            config->noise_mask = 15;
            break;
        }
        switch (config_byte & 3)
        {
        case 1:
            config->orval |= 1 << 8;
            break;
        case 2:
            config->orval |= 1 << 10;
            break;
        }
        config->write_mask = 15;
        config->dac_mask   = ~15;
    }
    else
    {
        switch ((config_byte >> 2) & 3)
        {
        case 2:
            config->noise_mask = 1;
            break;
        case 3:
            config->noise_mask = 3;
            break;
        }
        switch (config_byte & 3)
        {
        case 1:
            config->orval |= 1 << 6;
            break;
        case 2:
            config->orval |= 1 << 8;
            break;
        }
        config->write_mask = 3;
        config->dac_mask   = ~3;
    }
    if ((config_byte & 0x80) == 0)
    {
        config->write_mask = 0;
    }
    if ((config_byte & 0x30) == 0x30)
    {
        config->orval |= 1 << 12;
    }
    if (config_byte & 0x40)
    {
        config->oversampling = 1;
    }
}

/* A slot whose voice is not keyed contributes nothing to the mix; what
 * the full path leaves behind for it is done here directly.  Not for
 * the very first sample (nfs clear), nor for slots 28 to 31, whose rows
 * double as the effect registers; those take the full path. */
static void PCM_IdleSlot(pcm_t *pcm, int slot, const int *rcadd, const int *rcadd2)
{
    uint32_t *ram1 = pcm->ram1[slot];
    uint16_t *ram2 = pcm->ram2[slot];
    const int last = slot == pcm->config.reg_slots - 1;
    const int slot2 = last ? 31 : slot + 1;
    int32_t suml;
    int32_t sumr;

    calc_tv(pcm, 2, ram2[5], &ram2[11], 0, NULL);

    switch (slot2)
    {
        case 17:
            pcm->ram1[31][1] = (uint32_t)addclip20((int32_t)pcm->ram1[31][1], rcadd[0] >> 1, rcadd[0] & 1);
            break;
        case 18:
            pcm->ram1[31][3] = (uint32_t)addclip20((int32_t)pcm->ram1[31][3], rcadd[1] >> 1, rcadd[1] & 1);
            break;
        case 21:
            pcm->ram1[31][1] = (uint32_t)addclip20((int32_t)pcm->ram1[31][1], rcadd[2] >> 1, rcadd[2] & 1);
            break;
        case 22:
            pcm->ram1[31][3] = (uint32_t)addclip20((int32_t)pcm->ram1[31][3], rcadd[3] >> 1, rcadd[3] & 1);
            break;
        case 23:
            pcm->ram1[31][1] = (uint32_t)addclip20((int32_t)pcm->ram1[31][1], rcadd[4] >> 1, rcadd[4] & 1);
            break;
        case 31:
            pcm->ram1[31][3] = (uint32_t)addclip20((int32_t)pcm->ram1[31][3], rcadd[5] >> 1, rcadd[5] & 1);
            break;
    }

    suml = addclip20((int32_t)pcm->ram1[31][1], 0, 0);
    sumr = addclip20((int32_t)pcm->ram1[31][3], 0, 0);

    switch (slot2)
    {
        case 17:
            pcm->rcsum[1] = addclip20(pcm->rcsum[1], rcadd2[0] >> 1, rcadd2[0] & 1);
            break;
        case 18:
            pcm->rcsum[1] = addclip20(pcm->rcsum[1], rcadd2[1] >> 1, rcadd2[1] & 1);
            break;
        case 21:
            pcm->rcsum[0] = addclip20(pcm->rcsum[0], rcadd2[2] >> 1, rcadd2[2] & 1);
            break;
        case 22:
            pcm->rcsum[1] = addclip20(pcm->rcsum[1], rcadd2[3] >> 1, rcadd2[3] & 1);
            break;
        case 23:
            pcm->rcsum[0] = addclip20(pcm->rcsum[0], rcadd2[4] >> 1, rcadd2[4] & 1);
            break;
        case 31:
            pcm->rcsum[1] = addclip20(pcm->rcsum[1], rcadd2[5] >> 1, rcadd2[5] & 1);
            break;
    }

    pcm->rcsum[0] = addclip20(pcm->rcsum[0], 0, 0);
    pcm->rcsum[1] = addclip20(pcm->rcsum[1], 0, 0);

    if (!last)
    {
        pcm->ram1[31][1] = (uint32_t)suml;
        pcm->ram1[31][3] = (uint32_t)sumr;
    }
    else
    {
        pcm->accum_l = suml;
        pcm->accum_r = sumr;
    }

    ram1[1] = 0;
    ram1[3] = 0;
    ram1[5] = 0;
    ram2[8] = 0;
    ram2[9] = 0;
    ram2[10] = 0;
}

static void PCM_Update(pcm_t *pcm, uint64_t cycles)
{
    while (pcm->cycles < cycles)
    {
        int rcadd[6];
        int rcadd2[6];
        int slot;
        uint64_t new_cycles;
        const uint32_t voice_active = pcm->voice_mask & pcm->voice_mask_pending;
        { /* final mixing */
            int32_t samp_l;
            int32_t samp_r;
            int shifter = pcm->ram2[30][10];
            int xr = ((shifter >> 0) ^ (shifter >> 1) ^ (shifter >> 7) ^ (shifter >> 12)) & 1;
            shifter = (shifter >> 1) | (xr << 15);
            pcm->ram2[30][10] = (uint16_t)shifter;

            pcm->accum_l = addclip20(pcm->accum_l, (int32_t)pcm->ram1[30][0], 0);
            pcm->accum_r = addclip20(pcm->accum_r, (int32_t)pcm->ram1[30][1], 0);

            pcm->ram1[30][2] = (uint32_t)addclip20(pcm->accum_l,
                (int32_t)(pcm->config.orval | (shifter & pcm->config.noise_mask)), 0);

            pcm->ram1[30][4] = (uint32_t)addclip20(pcm->accum_r,
                (int32_t)(pcm->config.orval | (shifter & pcm->config.noise_mask)), 0);

            pcm->ram1[30][0] = (uint32_t)(pcm->accum_l & pcm->config.write_mask);
            pcm->ram1[30][1] = (uint32_t)(pcm->accum_r & pcm->config.write_mask);

            samp_l = (int32_t)((pcm->ram1[30][2] & (uint32_t)(~pcm->config.write_mask)) << 12);
            samp_r = (int32_t)((pcm->ram1[30][4] & (uint32_t)(~pcm->config.write_mask)) << 12);

            MCU_PostSample(pcm->mcu, samp_l, samp_r);

            xr = ((shifter >> 0) ^ (shifter >> 1) ^ (shifter >> 7) ^ (shifter >> 12)) & 1;
            shifter = (shifter >> 1) | (xr << 15);

            pcm->accum_l = addclip20(pcm->accum_l, (int32_t)pcm->ram1[30][0], 0);
            pcm->accum_r = addclip20(pcm->accum_r, (int32_t)pcm->ram1[30][1], 0);

            pcm->ram1[30][3] = (uint32_t)addclip20(pcm->accum_l,
                (int32_t)(pcm->config.orval | (shifter & pcm->config.noise_mask)), 0);

            pcm->ram1[30][5] = (uint32_t)addclip20(pcm->accum_r,
                (int32_t)(pcm->config.orval | (shifter & pcm->config.noise_mask)), 0);

            if (pcm->enable_oversampling && pcm->config.oversampling) /* oversampling */
            {
                pcm->ram2[30][10] = (uint16_t)shifter;

                pcm->ram1[30][0] = (uint32_t)(pcm->accum_l & pcm->config.write_mask);
                pcm->ram1[30][1] = (uint32_t)(pcm->accum_r & pcm->config.write_mask);

                samp_l = (int32_t)((pcm->ram1[30][3] & (uint32_t)(~pcm->config.write_mask)) << 12);
                samp_r = (int32_t)((pcm->ram1[30][5] & (uint32_t)(~pcm->config.write_mask)) << 12);

                MCU_PostSample(pcm->mcu, samp_l, samp_r);
            }
        }

        { /* global counter for envelopes */
            if (!pcm->nfs)
                pcm->tv_counter = pcm->ram2[31][8]; /* fixme */

            pcm->tv_counter -= 1;

            pcm->tv_counter &= 0x3fff;
        }

        /* chorus/reverb */

        { /* fixme */
            if (pcm->ram2[31][8] & 0x8000)
                pcm->ram2[31][9] = pcm->ram2[31][8] & 0x7fff;
            else
                pcm->ram2[31][10] = pcm->ram2[31][8] & 0x7fff;

            if ((0x4000 - pcm->ram2[31][8]) & 0x8000)
                pcm->ram2[31][10] = (0x4000 - pcm->ram2[31][8]) & 0x7fff;
            else
                pcm->ram2[31][9] = (0x4000 - pcm->ram2[31][8]) & 0x7fff;
        }

        {
            int v1 = pcm->ram2[31][1];

            int m1 = multi((int32_t)pcm->ram1[29][1], (int8_t)(v1 >> 8)) >> 5; /* 14 */
            int m2 = multi(pcm->rcsum[1], (int8_t)(v1 & 255)) >> 5; /* 15 */

            pcm->ram1[29][1] = (uint32_t)addclip20(m1 >> 1, m2 >> 1, (m1 | m2) & 1); /* 16 */
        }

        {
            const int okey = (pcm->ram2[31][7] & 0x20) != 0;
            const int key = 1;
            const int active = okey && key;
            int u = 0;
            calc_tv(pcm, 1, pcm->ram2[30][0], &pcm->ram2[30][9], active, &u);
        }

        {
            int v1 = pcm->ram2[30][1];
            int m1 = multi((int32_t)pcm->ram1[29][0], (int8_t)(v1 >> 8)) >> 5; /* 17 */
            int m2 = multi(pcm->rcsum[0], (int8_t)(v1 & 255)) >> 5; /* 18 */

            pcm->ram1[29][0] = (uint32_t)addclip20(m1 >> 1, m2 >> 1, (m1 | m2) & 1); /* 19 */
        }

        memset(rcadd, 0, sizeof(rcadd));
        memset(rcadd2, 0, sizeof(rcadd2));

        {
            {
                int v3;
                int m2;
                /* 1 */
                int v1 = pcm->ram2[30][4];
                int m1 = multi((int32_t)pcm->ram1[29][0], (int8_t)(v1 >> 8)) >> 6;
                int v2 = 0;
                int s1 = eram_unpack(pcm, pcm->ram2[28][1] + pcm->tv_counter, 1);
                int s2 = eram_unpack(pcm, pcm->ram2[28][1] + pcm->tv_counter, 0);
                if ((v1 & 0x30) != 0)
                {
                    v2 = s1;
                }
                v3 = addclip20(m1, v2 ^ 0xfffff, 1);
                pcm->ram1[29][4] = (uint32_t)v3;
                m2 = multi(v3, (int8_t)(v1 & 255)) >> 5;
                pcm->ram1[29][5] = (uint32_t)addclip20(m2 >> 1, s2, m2 & 1);
            }
            {
                int v3;
                int m2;
                /* 2 */
                int v1 = pcm->ram2[30][4];
                int v2 = 0;
                int s1 = eram_unpack(pcm, pcm->ram2[28][2] + pcm->tv_counter, 1);
                int s2 = eram_unpack(pcm, pcm->ram2[28][2] + pcm->tv_counter, 0);
                if ((v1 & 0x30) != 0)
                {
                    v2 = s1;
                }
                v3 = addclip20((int32_t)pcm->ram1[29][5], v2 ^ 0xfffff, 1);
                pcm->ram1[29][5] = (uint32_t)v3;
                m2 = multi(v3, (int8_t)(v1 & 255)) >> 5;
                pcm->ram1[28][0] = (uint32_t)addclip20(m2 >> 1, s2, m2 & 1);
            }
            {
                int v3;
                int m2;
                /* 3 */
                int v1 = pcm->ram2[30][4];
                int v2 = 0;
                int s1 = eram_unpack(pcm, pcm->ram2[28][3] + pcm->tv_counter, 1);
                int s2 = eram_unpack(pcm, pcm->ram2[28][3] + pcm->tv_counter, 0);
                if ((v1 & 0x30) != 0)
                {
                    v2 = s1;
                }
                v3 = addclip20((int32_t)pcm->ram1[28][0], v2 ^ 0xfffff, 1);
                pcm->ram1[28][0] = (uint32_t)v3;
                m2 = multi(v3, (int8_t)(v1 & 255)) >> 5;
                pcm->ram1[28][1] = (uint32_t)addclip20(m2 >> 1, s2, m2 & 1);

                pcm->ram1[28][2] = (uint32_t)eram_unpack(pcm, pcm->ram2[28][5] + pcm->tv_counter, 0);
            }
            {
                int v3;
                int m2;
                /* 4 */
                int v1 = pcm->ram2[30][5];
                int v2 = 0;
                int s1 = eram_unpack(pcm, pcm->ram2[28][4] + pcm->tv_counter, 1);
                int s2 = eram_unpack(pcm, pcm->ram2[28][4] + pcm->tv_counter, 0);
                if ((v1 & 0x30) != 0)
                {
                    v2 = s1;
                }
                v3 = addclip20((int32_t)pcm->ram1[28][1], v2 ^ 0xfffff, 1);
                pcm->ram1[28][1] = (uint32_t)v3;
                m2 = multi(v3, (int8_t)(v1 & 255)) >> 5;
                pcm->ram1[28][3] = (uint32_t)addclip20(m2 >> 1, s2, m2 & 1);

                pcm->ram1[28][4] = (uint32_t)eram_unpack(pcm, pcm->ram2[29][1] + pcm->tv_counter, 0);
            }
            {
                /* 5 */

                int v1 = pcm->ram2[30][7];
                int m1 = multi((int32_t)pcm->ram1[29][2], (int8_t)(v1 >> 8)) >> 5;
                int s1 = eram_unpack(pcm, pcm->ram2[29][0] + pcm->tv_counter, 0);
                int m2 = multi(s1, (int8_t)(v1 & 255)) >> 5;
                pcm->ram1[29][2] = (uint32_t)addclip20(m1 >> 1, m2 >> 1, (m1 | m2) & 1);

                eram_pack(pcm, pcm->ram2[28][0] + pcm->tv_counter, pcm->ram1[29][4]);
            }
            {
                /* 6 */

                int v1 = pcm->ram2[30][8];
                int m1 = multi((int32_t)pcm->ram1[29][3], (int8_t)(v1 >> 8)) >> 5;
                int s1 = eram_unpack(pcm, pcm->ram2[29][8] + pcm->tv_counter, 0);
                int m2 = multi(s1, (int8_t)(v1 & 255)) >> 5;
                pcm->ram1[29][3] = (uint32_t)addclip20(m1 >> 1, m2 >> 1, (m1 | m2) & 1);

                eram_pack(pcm, pcm->ram2[28][1] + pcm->tv_counter, pcm->ram1[29][5]);

                eram_pack(pcm, pcm->ram2[28][2] + pcm->tv_counter, pcm->ram1[28][0]);
            }
            {
                /* 7 */

                int v1 = pcm->ram2[30][9];
                int v2 = (int)pcm->ram1[28][3];
                int m1 = multi((int32_t)pcm->ram1[29][2], (int8_t)(v1 >> 8)) >> 5;
                int m2 = multi((int32_t)pcm->ram1[29][3], (int8_t)(v1 >> 8)) >> 5;
                pcm->ram1[28][3] = (uint32_t)addclip20(v2, m1 >> 1, m1 & 1);
                pcm->ram1[28][5] = (uint32_t)addclip20(v2, m2 >> 1, m2 & 1);

                eram_pack(pcm, pcm->ram2[28][3] + pcm->tv_counter, pcm->ram1[28][1]);
            }
            {
                int m2;
                /* 8 */

                int v1 = pcm->ram2[30][6];
                int m1 = multi((int32_t)pcm->ram1[28][2], (int8_t)(v1 >> 8)) >> 5;

                int v2 = addclip20((int32_t)pcm->ram1[28][3], m1 >> 1, m1 & 1);
                pcm->ram1[28][3] = (uint32_t)v2;
                m2 = multi(v2, (int8_t)(v1 & 255)) >> 5;
                pcm->ram1[28][2] = (uint32_t)addclip20((int32_t)pcm->ram1[28][2], m2 >> 1, m2 & 1);

                pcm->ram1[28][1] = (uint32_t)eram_unpack(pcm, pcm->ram2[28][9] + pcm->tv_counter, 0);
            }
            {
                int m2;
                /* 9 */

                int v1 = pcm->ram2[30][6];
                int m1 = multi((int32_t)pcm->ram1[28][4], (int8_t)(v1 >> 8)) >> 5;

                int v2 = addclip20((int32_t)pcm->ram1[28][5], m1 >> 1, m1 & 1);
                pcm->ram1[28][5] = (uint32_t)v2;
                m2 = multi(v2, (int8_t)(v1 & 255)) >> 5;
                pcm->ram1[28][4] = (uint32_t)addclip20((int32_t)pcm->ram1[28][4], m2 >> 1, m2 & 1);

                pcm->ram1[29][4] = (uint32_t)eram_unpack(pcm, pcm->ram2[29][5] + pcm->tv_counter, 0);
            }
            {
                int m2;
                /* 10 */

                int v1 = pcm->ram2[30][6];
                int v2 = (int)pcm->ram1[28][1];
                int m1 = multi(v2, (int8_t)(v1 >> 8)) >> 5;
                int s1 = eram_unpack(pcm, pcm->ram2[28][8] + pcm->tv_counter, 0);
                int v3 = addclip20(m1 >> 1, s1, m1 & 1);
                pcm->ram1[28][1] = (uint32_t)v3;
                m2 = multi(v3, (int8_t)(v1 & 255)) >> 5;
                pcm->ram1[29][5] = (uint32_t)addclip20(m2 >> 1, v2, m2 & 1);

                eram_pack(pcm, pcm->ram2[28][4] + pcm->tv_counter, pcm->ram1[28][3]);
            }
            {
                int m2;
                /* 11 */

                int v1 = pcm->ram2[30][6];
                int v2 = (int)pcm->ram1[29][4];
                int m1 = multi(v2, (int8_t)(v1 >> 8)) >> 5;
                int s1 = eram_unpack(pcm, pcm->ram2[29][4] + pcm->tv_counter, 0);
                int v3 = addclip20(m1 >> 1, s1, m1 & 1);
                pcm->ram1[29][4] = (uint32_t)v3;
                m2 = multi(v3, (int8_t)(v1 & 255)) >> 5;
                pcm->ram1[28][0] = (uint32_t)addclip20(m2 >> 1, v2, m2 & 1);

                eram_pack(pcm, pcm->ram2[28][5] + pcm->tv_counter, pcm->ram1[28][2]);

                eram_pack(pcm, pcm->ram2[29][0] + pcm->tv_counter, pcm->ram1[28][5]);
            }
            {
                /* 12 */

                pcm->ram1[28][5] = (uint32_t)eram_unpack(pcm, pcm->ram2[28][6] + pcm->tv_counter, 0);
            }

            {
                /* 13 */

                int s1 = eram_unpack(pcm, pcm->ram2[28][10] + pcm->tv_counter, 0);
                pcm->ram1[28][5] = (uint32_t)addclip20((int32_t)pcm->ram1[28][5], s1, 0);

                pcm->ram1[28][2] = (uint32_t)eram_unpack(pcm, pcm->ram2[29][2] + pcm->tv_counter, 0);
            }

            {
                /* 14 */

                int s1 = eram_unpack(pcm, pcm->ram2[29][6] + pcm->tv_counter, 0);
                int t1 = addclip20(s1, (int32_t)pcm->ram1[28][2], 0); /* 6 */

                pcm->ram1[28][5] = (uint32_t)addclip20(t1, (int32_t)pcm->ram1[28][5], 0);

                pcm->ram1[28][2] = (uint32_t)eram_unpack(pcm, pcm->ram2[28][7] + pcm->tv_counter, 0);
            }

            {
                /* 15 */

                int s1 = eram_unpack(pcm, pcm->ram2[28][11] + pcm->tv_counter, 0);
                pcm->ram1[28][2] = (uint32_t)addclip20((int32_t)pcm->ram1[28][2], s1, 0);

                pcm->ram1[28][3] = (uint32_t)eram_unpack(pcm, pcm->ram2[29][3] + pcm->tv_counter, 0);
            }

            {
                /* 16 */

                int s1 = eram_unpack(pcm, pcm->ram2[29][7] + pcm->tv_counter, 0);
                int t1 = addclip20(s1, (int32_t)pcm->ram1[28][2], 0);
                pcm->ram1[28][2] = (uint32_t)addclip20(t1, (int32_t)pcm->ram1[28][3], 0);

                eram_pack(pcm, pcm->ram2[29][1] + pcm->tv_counter, pcm->ram1[28][4]);

                eram_pack(pcm, pcm->ram2[28][8] + pcm->tv_counter, pcm->ram1[28][1]);
            }

            {
                int t1;
                /* 17 */
                int v1 = pcm->ram2[30][2];
                int v2 = (int)pcm->ram1[28][5];

                int m1 = multi(v2, (int8_t)(v1 >> 8)) >> 5;

                rcadd[0] = m1;

                rcadd2[0] = multi(v2, (int8_t)(v1 & 255)) >> 5;

                t1 = eram_unpack(pcm, pcm->ram2[29][10] + pcm->tv_counter + 1, 0); /* ? 3a6e */
                eram_pack(pcm, pcm->ram2[28][9] + pcm->tv_counter, pcm->ram1[29][5]);
                pcm->ram1[29][5] = (uint32_t)t1;
            }

            {
                /* 18 */
                int v1 = pcm->ram2[30][3];
                int v2 = (int)pcm->ram1[28][2];

                int m1 = multi(v2, (int8_t)(v1 >> 8)) >> 5;

                rcadd[1] = m1;

                rcadd2[1] = multi(v2, (int8_t)(v1 & 255)) >> 5;

                pcm->ram1[28][1] = (uint32_t)eram_unpack(pcm, pcm->ram2[29][11] + pcm->tv_counter + 1, 0); /* ? 3a1e */
            }
            {
                int m1;
                int m2;
                int t2;
                /* 19 */

                int v1 = pcm->ram2[31][9];

                int s1 = eram_unpack(pcm, pcm->ram2[29][10] + pcm->tv_counter, 0); /* ? 3a6d */

                eram_pack(pcm, pcm->ram2[29][4] + pcm->tv_counter, pcm->ram1[29][4]);

                m1 = multi(s1, (int8_t)(v1 >> 8)) >> 5;
                m2 = multi((int32_t)pcm->ram1[29][5], (int8_t)(v1 >> 8)) >> 5;

                t2 = addclip20(s1, (m1 >> 1) ^ 0xfffff, 1);

                pcm->ram1[29][5] = (uint32_t)addclip20(t2, m2 >> 1, m2 & 1);
            }
            {
                int m1;
                int m2;
                int t2;
                /* 20 */

                int v1 = pcm->ram2[31][10];

                int s1 = eram_unpack(pcm, pcm->ram2[29][11] + pcm->tv_counter, 0); /* ? 3a1d */

                eram_pack(pcm, pcm->ram2[29][5] + pcm->tv_counter, pcm->ram1[28][0]);

                m1 = multi(s1, (int8_t)(v1 >> 8)) >> 5;
                m2 = multi((int32_t)pcm->ram1[28][1], (int8_t)(v1 >> 8)) >> 5;

                t2 = addclip20(s1, (m1 >> 1) ^ 0xfffff, 1);

                pcm->ram1[28][1] = (uint32_t)addclip20(t2, m2 >> 1, m2 & 1);

                eram_pack(pcm, pcm->ram2[29][9] + pcm->tv_counter, pcm->ram1[29][1]);
            }
            {
                /* 21 */

                int v1 = pcm->ram2[31][2];
                int v2 = (int)pcm->ram1[29][5];

                int m1 = multi(v2, (int8_t)(v1 >> 8)) >> 5;
                int m2 = multi(v2, (int8_t)(v1 & 255)) >> 5;

                rcadd[2] = m1;
                rcadd2[2] = m2;
            }
            {
                /* 22 */

                int v1 = pcm->ram2[31][3];
                int v2 = (int)pcm->ram1[29][5];

                int m1 = multi(v2, (int8_t)(v1 >> 8)) >> 5;
                int m2 = multi(v2, (int8_t)(v1 & 255)) >> 5;

                rcadd[3] = m1;
                rcadd2[3] = m2;
            }
            {
                /* 23 */

                int v1 = pcm->ram2[31][4];
                int v2 = (int)pcm->ram1[28][1];

                int m1 = multi(v2, (int8_t)(v1 >> 8)) >> 5;
                int m2 = multi(v2, (int8_t)(v1 & 255)) >> 5;

                rcadd[4] = m1;
                rcadd2[4] = m2;
            }
            {
                /* 31 */

                int v1 = pcm->ram2[31][5];
                int v2 = (int)pcm->ram1[28][1];

                int m1 = multi(v2, (int8_t)(v1 >> 8)) >> 5;
                int m2 = multi(v2, (int8_t)(v1 & 255)) >> 5;

                rcadd[5] = m1;
                rcadd2[5] = m2;

                {
                    int address;
                    int address_end;
                    int address_loop;
                    int sub_phase;
                    int interp_ratio;
                    int sub_phase_of;
                    int address_cnt;
                    int cmp1;
                    int cmp2;
                    int address_cmp;
                    int next_b15;
                    int next_address;
                    int address_cnt2;
                    int address_add;
                    int address_sub;
                    int t1;
                    int t2;
                    int t3;
                    int t4;
                    /* address generator */

                    const int key = 1;
                    const int okey = (pcm->ram2[31][7] & 0x20) != 0;
                    const int active = key && okey;
                    const int kon = key && !okey;

                    int b15 = (pcm->ram2[31][8] & 0x8000) != 0; /* 0 */
                    const int b6 = (pcm->ram2[31][7] & 0x40) != 0; /* 1 */
                    const int b7 = (pcm->ram2[31][7] & 0x80) != 0; /* 1 */
                    int old_nibble = (pcm->ram2[31][7] >> 12) & 15; /* 1 */
                    (void)old_nibble; /* unused */

                    address = (int)pcm->ram1[31][4]; /* 0 */
                    address_end = (int)pcm->ram1[31][0]; /* 1 or 2 */
                    address_loop = (int)pcm->ram1[31][2]; /* 2 or 1 */

                    sub_phase = (pcm->ram2[31][8] & 0x3fff); /* 1 */
                    interp_ratio = (sub_phase >> 7) & 127;
                    (void)interp_ratio; /* unused */
                    sub_phase += pcm->ram2[pcm->ram2[31][7] & 31][0]; /* 5 */
                    sub_phase_of = (sub_phase >> 14) & 7;
                    if (pcm->nfs)
                    {
                        pcm->ram2[31][8] &= ~0x3fff;
                        pcm->ram2[31][8] |= sub_phase & 0x3fff;
                    }

                    /* address 0 */
                    address_cnt = address;

                    cmp1 = b15 ? address_loop : address_end;
                    cmp2 = address_cnt;
                    address_cmp = (cmp1 & 0xfffff) == (cmp2 & 0xfffff); /* 9 */
                    next_b15 = b15;

                    next_address = address_cnt; /* 11 */

                    cmp1 = (!b6 && address_cmp) ? address_loop : address_cnt;
                    cmp2 = address_cnt;
                    address_cnt2 = (kon || (!b6 && address_cmp)) ? cmp1 : cmp2;

                    address_add = (!address_cmp && b6 && !b15) || (!address_cmp && !b6);
                    address_sub = !address_cmp && b6 && b15;
                    if (b7)
                        address_cnt2 -= address_add - address_sub;
                    else
                        address_cnt2 += address_add - address_sub;
                    address_cnt = address_cnt2 & 0xfffff; /* 11 */
                    b15 = b6 && (b15 ^ address_cmp); /* 11 */

                    cmp1 = b15 ? address_loop : address_end;
                    cmp2 = address_cnt;
                    address_cmp = (cmp1 & 0xfffff) == (cmp2 & 0xfffff); /* 13 */

                    if (sub_phase_of >= 1)
                    {
                        next_address = address_cnt; /* 13 */
                        next_b15 = b15;
                    }

                    if (active && pcm->nfs)
                        pcm->ram1[31][4] = (uint32_t)next_address;

                    if (pcm->nfs)
                    {
                        pcm->ram2[31][8] &= ~0x8000;
                        pcm->ram2[31][8] |= (uint16_t)(next_b15 << 15);
                    }

                    t1 = address_loop; /* 18 */
                    t2 = (int)pcm->ram1[31][4] - t1; /* 19 */
                    t3 = address_end - t2; /* 20 */
                    t4 = (int)pcm->ram1[31][4]; /* 23 */

                    pcm->ram2[29][10] = (uint16_t)t3;
                    pcm->ram2[29][11] = (uint16_t)t4;
                }
            }
        }

        pcm->ram1[31][1] = 0;
        pcm->ram1[31][3] = 0;
        pcm->rcsum[0] = 0;
        pcm->rcsum[1] = 0;

        for (slot = 0; slot < pcm->config.reg_slots; slot++)
        {
            if (slot < 28 && pcm->nfs && !((voice_active >> slot) & 1))
            {
                PCM_IdleSlot(pcm, slot, rcadd, rcadd2);
                continue;
            }
            {
            int nibble_address;
            int address_b4;
            int wave_address;
            int xor2;
            int check1;
            int xor1;
            int nibble_add;
            int nibble_subtract;
            int newnibble;
            int newnibble_sel;
            int sub_phase;
            int interp_ratio;
            int sub_phase_of;
            int address_cnt;
            int samp0;
            int nibble_cmp2;
            int address_cmp;
            int next_address;
            int usenew;
            int next_b15;
            int address_cnt2;
            int address_add;
            int address_sub;
            int samp1;
            int nibble_cmp3;
            int samp2;
            int nibble_cmp4;
            int samp3;
            int nibble_cmp5;
            int nibble_cmp6;
            int reference;
            int preshift;
            int select_nibble;
            int shift;
            int shifted;
            int test;
            int step0;
            int step1;
            int step2;
            int reg1;
            int reg3;
            int reg2_6;
            int filter;
            int v3;
            int volmul1;
            int volmul2;
            int sample;
            int multiv1;
            int multiv2;
            int sample2;
            int multiv3;
            int multiv4;
            int sample3;
            int pan;
            int rc;
            int sampl;
            int sampr;
            int rc0;
            int rc1;
            int slot2;
            int32_t suml;
            int32_t sumr;
            uint32_t *ram1 = pcm->ram1[slot];
            uint16_t *ram2 = pcm->ram2[slot];
            const int okey = (ram2[7] & 0x20) != 0;
            const int key = (voice_active >> slot) & 1;

            const int active = okey && key;
            const int kon = key && !okey;

            /* address generator */

            int b15 = (ram2[8] & 0x8000) != 0; /* 0 */
            const int b6 = (ram2[7] & 0x40) != 0; /* 1 */
            const int b7 = (ram2[7] & 0x80) != 0; /* 1 */
            int hiaddr = (ram2[7] >> 8) & 15; /* 1 */
            int old_nibble = (ram2[7] >> 12) & 15; /* 1 */

            int address = (int)ram1[4]; /* 0 */
            int address_end = (int)ram1[0]; /* 1 or 2 */
            int address_loop = (int)ram1[2]; /* 2 or 1 */

            int cmp1 = b15 ? address_loop : address_end;
            int cmp2 = address;
            const int nibble_cmp1 = (cmp1 & 0xffff0) == (cmp2 & 0xffff0); /* 2 */
            int irq_flag = 0;

            /* fixme: */
            if (kon)
                irq_flag = ((cmp1 + address_loop) & 0x100000) != 0;
            else
                irq_flag = ((address + ((-address_loop) & 0xfffff)) & 0x100000) != 0;
            irq_flag ^= b7;

            nibble_address = (!b6 && nibble_cmp1) ? address_loop : address; /* 3 */
            address_b4 = (nibble_address & 0x10) != 0;
            wave_address = nibble_address >> 5;
            xor2 = (address_b4 ^ b7);
            check1 = xor2 && active;
            xor1 = (b15 ^ !nibble_cmp1);
            nibble_add = b6 ? check1 && xor1 : (!nibble_cmp1 && check1);
            nibble_subtract = b6 && !xor1 && active && !xor2;
            if (b7)
                wave_address -= nibble_add - nibble_subtract;
            else
                wave_address += nibble_add - nibble_subtract;
            wave_address &= 0xfffff;

            newnibble = PCM_ReadROM(pcm, (uint32_t)((hiaddr << 20) | wave_address));
            newnibble_sel = address_b4 ^ ((b6 || !nibble_cmp1) && okey);
            if (newnibble_sel)
                newnibble = (newnibble >> 4) & 15;
            else
                newnibble &= 15;

            sub_phase = (ram2[8] & 0x3fff); /* 1 */
            interp_ratio = (sub_phase >> 7) & 127;
            sub_phase += pcm->ram2[ram2[7] & 31][0]; /* 5 */
            sub_phase_of = (sub_phase >> 14) & 7;
            if (pcm->nfs)
            {
                ram2[8] &= ~0x3fff;
                ram2[8] |= sub_phase & 0x3fff;
            }

            /* address 0 */
            address_cnt = address;
            samp0 = (int8_t)PCM_ReadROM(pcm, (uint32_t)((hiaddr << 20) | address_cnt)); /* 18 */

            cmp1 = address;
            cmp2 = address_cnt;
            nibble_cmp2 = (cmp1 & 0xffff0) == (cmp2 & 0xffff0); /* 8 */
            cmp1 = b15 ? address_loop : address_end;
            cmp2 = address_cnt;
            address_cmp = (cmp1 & 0xfffff) == (cmp2 & 0xfffff); /* 9 */

            next_address = address_cnt; /* 11 */
            usenew = !nibble_cmp2;
            next_b15 = b15;

            cmp1 = (!b6 && address_cmp) ? address_loop : address_cnt;
            cmp2 = address_cnt;
            address_cnt2 = (kon || (!b6 && address_cmp)) ? cmp1 : cmp2;

            address_add = (!address_cmp && b6 && !b15) || (!address_cmp && !b6);
            address_sub = !address_cmp && b6 && b15;
            if (b7)
                address_cnt2 -= address_add - address_sub;
            else
                address_cnt2 += address_add - address_sub;
            address_cnt = address_cnt2 & 0xfffff; /* 11 */
            b15 = b6 && (b15 ^ address_cmp); /* 11 */

            samp1 = (int8_t)PCM_ReadROM(pcm, (uint32_t)((hiaddr << 20) | address_cnt)); /* 20 */

            cmp1 = address;
            cmp2 = address_cnt;
            nibble_cmp3 = (cmp1 & 0xffff0) == (cmp2 & 0xffff0); /* 12 */
            cmp1 = b15 ? address_loop : address_end;
            cmp2 = address_cnt;
            address_cmp = (cmp1 & 0xfffff) == (cmp2 & 0xfffff); /* 13 */

            if (sub_phase_of >= 1)
            {
                next_address = address_cnt; /* 13 */
                usenew = !nibble_cmp3;
                next_b15 = b15;
            }

            cmp1 = (!b6 && address_cmp) ? address_loop : address_cnt;
            cmp2 = address_cnt;
            address_cnt2 = (kon || (!b6 && address_cmp)) ? cmp1 : cmp2;

            address_add = (!address_cmp && b6 && !b15) || (!address_cmp && !b6);
            address_sub = !address_cmp && b6 && b15;
            if (b7)
                address_cnt2 -= address_add - address_sub;
            else
                address_cnt2 += address_add - address_sub;
            address_cnt = address_cnt2 & 0xfffff; /* 15 */
            b15 = b6 && (b15 ^ address_cmp); /* 15 */

            samp2 = (int8_t)PCM_ReadROM(pcm, (uint32_t)((hiaddr << 20) | address_cnt)); /* 1 */

            cmp1 = address;
            cmp2 = address_cnt;
            nibble_cmp4 = (cmp1 & 0xffff0) == (cmp2 & 0xffff0); /* 16 */
            cmp1 = b15 ? address_loop : address_end;
            cmp2 = address_cnt;
            address_cmp = (cmp1 & 0xfffff) == (cmp2 & 0xfffff); /* 17 */

            if (sub_phase_of >= 2)
            {
                next_address = address_cnt; /* 17 */
                usenew = !nibble_cmp4;
                next_b15 = b15;
            }

            cmp1 = (!b6 && address_cmp) ? address_loop : address_cnt;
            cmp2 = address_cnt;
            address_cnt2 = (kon || (!b6 && address_cmp)) ? cmp1 : cmp2;

            address_add = (!address_cmp && b6 && !b15) || (!address_cmp && !b6);
            address_sub = !address_cmp && b6 && b15;
            if (b7)
                address_cnt2 -= address_add - address_sub;
            else
                address_cnt2 += address_add - address_sub;
            address_cnt = address_cnt2 & 0xfffff; /* 19 */
            b15 = b6 && (b15 ^ address_cmp); /* 19 */

            samp3 = (int8_t)PCM_ReadROM(pcm, (uint32_t)((hiaddr << 20) | address_cnt)); /* 5 */

            cmp1 = address;
            cmp2 = address_cnt;
            nibble_cmp5 = (cmp1 & 0xffff0) == (cmp2 & 0xffff0); /* 20 */
            cmp1 = b15 ? address_loop : address_end;
            cmp2 = address_cnt;
            address_cmp = (cmp1 & 0xfffff) == (cmp2 & 0xfffff); /* 21 */

            if (sub_phase_of >= 3)
            {
                next_address = address_cnt; /* 21 */
                usenew = !nibble_cmp5;
                next_b15 = b15;
            }

            cmp1 = (!b6 && address_cmp) ? address_loop : address_cnt;
            cmp2 = address_cnt;
            address_cnt2 = (kon || (!b6 && address_cmp)) ? cmp1 : cmp2;

            address_add = (!address_cmp && b6 && !b15) || (!address_cmp && !b6);
            address_sub = !address_cmp && b6 && b15;
            if (b7)
                address_cnt2 -= address_add - address_sub;
            else
                address_cnt2 += address_add - address_sub;
            address_cnt = address_cnt2 & 0xfffff; /* 23 */
            /* b15 = b6 && (b15 ^ address_cmp); // 23 */

            cmp1 = address;
            cmp2 = address_cnt;
            nibble_cmp6 = (cmp1 & 0xffff0) == (cmp2 & 0xffff0); /* 24 */

            if (sub_phase_of >= 4)
            {
                next_address = address_cnt; /* 1 */
                usenew = !nibble_cmp6;
                /* b15 is not updated? */
            }

            if (active && pcm->nfs)
                ram1[4] = (uint32_t)next_address;

            if (pcm->nfs)
            {
                ram2[8] &= ~0x8000;
                ram2[8] |= (uint16_t)(next_b15 << 15);
            }

            /* dpcm */

            /* 18 */
            reference = (int)ram1[5];

            /* 19 */
            preshift = SC55_SHL(samp0, 10);
            select_nibble = nibble_cmp2 ? old_nibble : newnibble;
            shift = (10 - select_nibble) & 15;

            shifted = SC55_SHL(preshift, 1) >> shift;

            if (sub_phase_of >= 1)
                reference = addclip20(reference, shifted >> 1, shifted & 1);

            preshift = SC55_SHL(samp1, 10);
            select_nibble = nibble_cmp3 ? old_nibble : newnibble;
            shift = (10 - select_nibble) & 15;

            shifted = SC55_SHL(preshift, 1) >> shift;

            if (sub_phase_of >= 2)
                reference = addclip20(reference, shifted >> 1, shifted & 1);

            preshift = SC55_SHL(samp2, 10);
            select_nibble = nibble_cmp4 ? old_nibble : newnibble;
            shift = (10 - select_nibble) & 15;

            shifted = SC55_SHL(preshift, 1) >> shift;

            if (sub_phase_of >= 3)
                reference = addclip20(reference, shifted >> 1, shifted & 1);

            preshift = SC55_SHL(samp3, 10);
            select_nibble = nibble_cmp5 ? old_nibble : newnibble;
            shift = (10 - select_nibble) & 15;

            shifted = SC55_SHL(preshift, 1) >> shift;

            if (sub_phase_of >= 4)
                reference = addclip20(reference, shifted >> 1, shifted & 1);

            /* interpolation */

            test = (int)ram1[5];

            step0 = multi(interp_lut[0][interp_ratio] << 6, (int8_t)samp0) >> 8;
            select_nibble = nibble_cmp2 ? old_nibble : newnibble;
            shift = (10 - select_nibble) & 15;
            step0 = SC55_SHL(step0, 1) >> shift;

            test = addclip20(test, step0 >> 1, step0 & 1);

            step1 = multi(interp_lut[1][interp_ratio] << 6, (int8_t)samp1) >> 8;
            select_nibble = nibble_cmp3 ? old_nibble : newnibble;
            shift = (10 - select_nibble) & 15;
            step1 = SC55_SHL(step1, 1) >> shift;

            test = addclip20(test, step1 >> 1, step1 & 1);

            step2 = multi(interp_lut[2][interp_ratio] << 6, (int8_t)samp2) >> 8;
            select_nibble = nibble_cmp4 ? old_nibble : newnibble;
            shift = (10 - select_nibble) & 15;
            step2 = SC55_SHL(step2, 1) >> shift;

            reg1 = (int)ram1[1];
            reg3 = (int)ram1[3];
            reg2_6 = (ram2[6] >> 8) & 127;

            test = addclip20(test, step2 >> 1, step2 & 1);

            filter = ram2[11];

            if (pcm->mcu->is_mk1)
            {
                int mult4;
                int mult5;
                int v4;
                int v5;
                int mult1 = multi(reg1, (int8_t)(filter >> 8)); /* 8 */
                int mult2 = multi(reg1, (int8_t)((filter >> 1) & 127)); /* 9 */
                int mult3 = multi(reg1, (int8_t)reg2_6); /* 10 */

                int v2 = addclip20(reg3, mult1 >> 6, (mult1 >> 5) & 1); /* 9 */
                int v1 = addclip20(v2, mult2 >> 13, (mult2 >> 12) & 1); /* 10 */
                int subvar = addclip20(v1, (mult3 >> 6), (mult3 >> 5) & 1); /* 11 */

                ram1[3] = (uint32_t)v1;

                v3 = addclip20(test, subvar ^ 0xfffff, 1); /* 12 */

                mult4 = multi(v3, (int8_t)(filter >> 8));
                mult5 = multi(v3, (int8_t)((filter >> 1) & 127));
                v4 = addclip20(reg1, mult4 >> 6, (mult4 >> 5) & 1); /* 14 */
                v5 = addclip20(v4, mult5 >> 13, (mult5 >> 12) & 1); /* 15 */

                ram1[1] = (uint32_t)v5;
            }
            else
            {
                int tests;
                int mult4;
                int mult5;
                int v4;
                int v5;
                /* hack: use 32-bit math to avoid overflow */
                int mult1 = reg1 * (int8_t)(filter >> 8); /* 8 */
                int mult2 = reg1 * (int8_t)((filter >> 1) & 127); /* 9 */
                int mult3 = reg1 * (int8_t)reg2_6; /* 10 */

                int v2 = reg3 + (mult1 >> 6) + ((mult1 >> 5) & 1); /* 9 */
                int v1 = v2 + (mult2 >> 13) + ((mult2 >> 12) & 1); /* 10 */
                int subvar = v1 + (mult3 >> 6) + ((mult3 >> 5) & 1); /* 11 */

                ram1[3] = (uint32_t)v1;

                tests = test;
                tests = SC55_SHL(tests, 12);
                tests >>= 12;

                v3 = tests - subvar; /* 12 */

                mult4 = v3 * (int8_t)(filter >> 8);
                mult5 = v3 * (int8_t)((filter >> 1) & 127);
                v4 = reg1 + (mult4 >> 6) + ((mult4 >> 5) & 1); /* 14 */
                v5 = v4 + (mult5 >> 13) + ((mult5 >> 12) & 1); /* 15 */

                ram1[1] = (uint32_t)v5;
            }

            ram1[5] = (uint32_t)reference;

            if (active && (ram2[6] & 1) != 0 && (ram2[8] & 0x4000) == 0 && !pcm->irq_assert && irq_flag)
            {
                /* fprintf(stderr, "irq voice %i\n", slot); */
                if (pcm->nfs)
                    ram2[8] |= 0x4000;
                pcm->irq_assert = 1;
                pcm->irq_channel = (uint8_t)slot;
                if (pcm->mcu->is_jv880)
                    MCU_GA_SetGAInt(pcm->mcu, 5, 1);
                else
                    MCU_Interrupt_SetRequest(pcm->mcu, INTERRUPT_SOURCE_IRQ0, 1);
            }

            volmul1 = 0;
            volmul2 = 0;

            calc_tv(pcm, 0, ram2[3], &ram2[9], active, &volmul1);
            calc_tv(pcm, 1, ram2[4], &ram2[10], active, &volmul2);
            calc_tv(pcm, 2, ram2[5], &ram2[11], active, NULL);

            /* if (volmul1 && volmul2) */
            /* volmul1 += 0; */

            sample = (ram2[6] & 2) == 0 ? (int)ram1[3] : v3;
            /* sample = test; */

            multiv1 = multi(sample, (int8_t)(volmul1 >> 8));
            multiv2 = multi(sample, (int8_t)((volmul1 >> 1) & 127));

            sample2 = addclip20(multiv1 >> 6, multiv2 >> 13, ((multiv2 >> 12) | (multiv1 >> 5)) & 1);

            multiv3 = multi(sample2, (int8_t)(volmul2 >> 8));
            multiv4 = multi(sample2, (int8_t)((volmul2 >> 1) & 127));

            sample3 = addclip20(multiv3 >> 6, multiv4 >> 13, ((multiv4 >> 12) | (multiv3 >> 5)) & 1);

            pan = active ? ram2[1] : 0;
            rc = active ? ram2[2] : 0;

            sampl = multi(sample3, (int8_t)((pan >> 8) & 255));
            sampr = multi(sample3, (int8_t)((pan >> 0) & 255));

            rc0 = multi(sample3, (int8_t)((rc >> 8) & 255)) >> 5; /* reverb */
            rc1 = multi(sample3, (int8_t)((rc >> 0) & 255)) >> 5; /* chorus */

            /* mix reverb/chorus? */
            slot2 = (slot == pcm->config.reg_slots - 1) ? 31 : slot + 1;
            switch (slot2)
            {
                /* 17, 18 - reverb */

                case 17:
                    pcm->ram1[31][1] = (uint32_t)addclip20((int32_t)pcm->ram1[31][1], rcadd[0] >> 1, rcadd[0] & 1);
                    break;
                case 18:
                    pcm->ram1[31][3] = (uint32_t)addclip20((int32_t)pcm->ram1[31][3], rcadd[1] >> 1, rcadd[1] & 1);
                    break;
                case 21:
                    pcm->ram1[31][1] = (uint32_t)addclip20((int32_t)pcm->ram1[31][1], rcadd[2] >> 1, rcadd[2] & 1);
                    break;
                case 22:
                    pcm->ram1[31][3] = (uint32_t)addclip20((int32_t)pcm->ram1[31][3], rcadd[3] >> 1, rcadd[3] & 1);
                    break;
                case 23:
                    pcm->ram1[31][1] = (uint32_t)addclip20((int32_t)pcm->ram1[31][1], rcadd[4] >> 1, rcadd[4] & 1);
                    break;
                case 31:
                    pcm->ram1[31][3] = (uint32_t)addclip20((int32_t)pcm->ram1[31][3], rcadd[5] >> 1, rcadd[5] & 1);
                    break;
            }

            suml = addclip20((int32_t)pcm->ram1[31][1], sampl >> 6, (sampl >> 5) & 1);
            sumr = addclip20((int32_t)pcm->ram1[31][3], sampr >> 6, (sampr >> 5) & 1);

            switch (slot2)
            {
                case 17:
                    pcm->rcsum[1] = addclip20(pcm->rcsum[1], rcadd2[0] >> 1, rcadd2[0] & 1);
                    break;
                case 18:
                    pcm->rcsum[1] = addclip20(pcm->rcsum[1], rcadd2[1] >> 1, rcadd2[1] & 1);
                    break;
                case 21:
                    pcm->rcsum[0] = addclip20(pcm->rcsum[0], rcadd2[2] >> 1, rcadd2[2] & 1);
                    break;
                case 22:
                    pcm->rcsum[1] = addclip20(pcm->rcsum[1], rcadd2[3] >> 1, rcadd2[3] & 1);
                    break;
                case 23:
                    pcm->rcsum[0] = addclip20(pcm->rcsum[0], rcadd2[4] >> 1, rcadd2[4] & 1);
                    break;
                case 31:
                    pcm->rcsum[1] = addclip20(pcm->rcsum[1], rcadd2[5] >> 1, rcadd2[5] & 1);
                    break;
            }

            pcm->rcsum[0] = addclip20(pcm->rcsum[0], rc0 >> 1, rc0 & 1);
            pcm->rcsum[1] = addclip20(pcm->rcsum[1], rc1 >> 1, rc1 & 1);

            if (slot != pcm->config.reg_slots - 1)
            {
                pcm->ram1[31][1] = (uint32_t)suml;
                pcm->ram1[31][3] = (uint32_t)sumr;
            }
            else
            {
                pcm->accum_l = suml;
                pcm->accum_r = sumr;
            }

            if (key && pcm->nfs)
            {
                ram2[7] &= ~0xf020;
                ram2[7] |= (uint16_t)(((usenew || kon) ? newnibble : old_nibble) << 12);

                /* update key */
                ram2[7] |= (uint16_t)(key << 5);
            }

            if (!active)
            {
                if (pcm->nfs)
                {
                    ram1[1] = 0;
                    ram1[3] = 0;
                    ram1[5] = 0;
                }

                ram2[8] = 0;
                ram2[9] = 0;
                ram2[10] = 0;
            }
            }
        }

        if (pcm->nfs)
        {
            pcm->ram2[31][7] |= 0x20;
        }

        pcm->nfs = 1;

        new_cycles = (uint64_t)(pcm->config.reg_slots + 1) * 25;

        pcm->cycles += pcm->mcu->is_jv880 ? (new_cycles * 25) / 29 : new_cycles;
    }
}

static uint32_t PCM_GetOutputFrequency(const pcm_t *pcm)
{
    uint32_t freq = (pcm->mcu->is_mk1 || pcm->mcu->is_jv880) ? 64000 : 66207;
    if (pcm->enable_oversampling)
    {
        return freq;
    }
    else
    {
        return freq / 2;
    }
}

enum {
    SM_VECTOR_UART3_TX = 0,
    SM_VECTOR_UART2_TX,
    SM_VECTOR_UART1_TX,
    SM_VECTOR_COLLISION,
    SM_VECTOR_TIMER_X,
    SM_VECTOR_IPCM0,
    SM_VECTOR_UART3_RX,
    SM_VECTOR_UART2_RX,
    SM_VECTOR_UART1_RX,
    SM_VECTOR_RESET
};

enum {
    SM_DEV_P1_DATA = 0x00,
    SM_DEV_P1_DIR = 0x01,
    SM_DEV_RAM_DIR = 0x02,
    SM_DEV_UART1_MODE_STATUS = 0x05,
    SM_DEV_UART1_CTRL = 0x06,
    SM_DEV_UART2_DATA = 0x08,
    SM_DEV_UART2_MODE_STATUS = 0x09,
    SM_DEV_UART2_CTRL = 0x0a,
    SM_DEV_UART3_MODE_STATUS = 0x0d,
    SM_DEV_UART3_CTRL = 0x0e,
    SM_DEV_IPCM0 = 0x10,
    SM_DEV_IPCM1 = 0x11,
    SM_DEV_IPCM2 = 0x12,
    SM_DEV_IPCM3 = 0x13,
    SM_DEV_IPCE0 = 0x14,
    SM_DEV_IPCE1 = 0x15,
    SM_DEV_IPCE2 = 0x16,
    SM_DEV_IPCE3 = 0x17,
    SM_DEV_SEMAPHORE = 0x19,
    SM_DEV_COLLISION = 0x1a,
    SM_DEV_INT_ENABLE = 0x1b,
    SM_DEV_INT_REQUEST = 0x1c,
    SM_DEV_PRESCALER = 0x1d,
    SM_DEV_TIMER = 0x1e,
    SM_DEV_TIMER_CTRL = 0x1f
};

static void SM_ErrorTrap(submcu_t *sm)
{
    (void)sm;
}

static uint8_t SM_Read(submcu_t *sm, uint16_t address)
{
    address &= 0x1fff;
    if (address & 0x1000)
    {
        return sm->rom[address & 0xfff];
    }
    else if (address < 0x80)
    {
        return sm->ram[address];
    }
    else if (address >= 0xc0 && address < 0xd8)
    {
        return sm->access[address & 0x1f];
    }
    else if (address >= 0xe0 && address < 0x100)
    {
        address &= 0x1f;
        switch (address)
        {
            case SM_DEV_UART2_DATA:
            {
                sm->uart_rx_gotbyte = 0;
                return sm->mcu->uart_rx_byte;
            }
            case SM_DEV_UART1_MODE_STATUS:
            {
                uint8_t ret = 0;
                ret |= 5;
                return ret;
            }
            case SM_DEV_UART2_MODE_STATUS:
            {
                uint8_t ret = (uint8_t)(sm->uart_rx_gotbyte << 1);
                ret |= 5;
                return ret;
            }
            case SM_DEV_UART3_MODE_STATUS:
            {
                uint8_t ret = 0;
                ret |= 5;
                return ret;
            }
            case SM_DEV_P1_DATA:
                return MCU_ReadP1(sm->mcu);
            case SM_DEV_P1_DIR:
                return sm->p1_dir;
            case SM_DEV_PRESCALER:
                return sm->timer_prescaler;
            case SM_DEV_TIMER:
                return sm->timer_counter;
        }
        return sm->device_mode[address];
    }
    else if (address >= 0x200 && address < 0x2c0)
    {
        address &= 0xff;
        if (sm->device_mode[SM_DEV_RAM_DIR] & (1<<(address>>5)))
            sm->access[address>>3] &= (uint8_t)(~(1<<(address&7)));
        return sm->shared_ram[address];
    }
    else
    {
        return 0;
    }
}

static void SM_Write(submcu_t *sm, uint16_t address, uint8_t data)
{
    address &= 0x1fff;
    if (address < 0x80)
    {
        sm->ram[address] = data;
    }
    else if (address >= 0xe0 && address < 0x100)
    {
        address &= 0x1f;
        switch (address)
        {
            case SM_DEV_P1_DATA:
                MCU_WriteP1(sm->mcu, data);
                break;
            case SM_DEV_P1_DIR:
                sm->p1_dir = data;
                break;
            case SM_DEV_IPCM0:
            case SM_DEV_IPCM1:
            case SM_DEV_IPCM2:
            case SM_DEV_IPCM3:
                sm->device_mode[address] = data;
                break;
            case SM_DEV_IPCE0:
            case SM_DEV_IPCE1:
            case SM_DEV_IPCE2:
            case SM_DEV_IPCE3:
                sm->device_mode[address] = data;
                break;
            case SM_DEV_INT_REQUEST:
                sm->device_mode[SM_DEV_INT_REQUEST] &= data;
                break;
            case SM_DEV_COLLISION:
                sm->device_mode[SM_DEV_COLLISION] &= ~0x7f;
                sm->device_mode[SM_DEV_COLLISION] |= data & 0x7f;
                if ((data & 0x80) == 0)
                    sm->device_mode[SM_DEV_COLLISION] &= ~0x80;
                break;
            default:
                sm->device_mode[address] = data;
                break;
        }
        if (address == SM_DEV_UART3_MODE_STATUS || address == SM_DEV_UART3_CTRL)
            MCU_GA_SetGAInt(sm->mcu, 5, (sm->device_mode[SM_DEV_UART3_MODE_STATUS] & 0x80) != 0
                && (sm->device_mode[SM_DEV_UART3_CTRL] & 0x20) == 0);
    }
    else if (address >= 0x200 && address < 0x2c0)
    {
        address &= 0xff;
        sm->access[address>>3] |= 1<<(address&7);
        sm->shared_ram[address] = data;
    }
    else
    {
    }
}

static void SM_SysWrite(submcu_t *sm, uint32_t address, uint8_t data)
{
    address &= 0xff;
    if (address < 0xc0)
    {
        address &= 0xff;
        sm->access[address>>3] |= 1<<(address&7);
        sm->shared_ram[address] = data;
    }
    else if (address >= 0xf8 && address < 0xfc)
    {
        sm->device_mode[SM_DEV_IPCM0 + (address & 3)] = data;
        if ((address & 3) == 0)
        {
            sm->device_mode[SM_DEV_INT_REQUEST] |= 0x10;
            sm->device_mode[SM_DEV_SEMAPHORE] &= ~0x80;
        }
    }
    else if (address == 0xff)
    {
        sm->device_mode[SM_DEV_SEMAPHORE] &= ~0x1f;
        sm->device_mode[SM_DEV_SEMAPHORE] |= data & 0x1f;
    }
    else if (address == 0xf5)
    {
        MCU_WriteP1(sm->mcu, data);
    }
    else if (address == 0xf6)
    {
        MCU_WriteP0(sm->mcu, data);
    }
    else if (address == 0xf7)
    {
        sm->p0_dir = data;
    }
    else
    {
    }
}

static uint8_t SM_SysRead(submcu_t *sm, uint32_t address)
{
    address &= 0xff;
    if (address < 0xc0)
    {
        if ((sm->device_mode[SM_DEV_RAM_DIR] & (1<<(address>>5))) == 0)
            sm->access[address>>3] &= (uint8_t)(~(1<<(address&7)));
        return sm->shared_ram[address];
    }
    else if (address >= 0xf8 && address < 0xfc)
    {
        uint8_t val;
        if ((address & 3) == 0)
        {
            sm->device_mode[SM_DEV_INT_REQUEST] |= 0x10;
        }
        val = sm->device_mode[SM_DEV_IPCE0 + (address & 3)];
        sm->device_mode[SM_DEV_IPCE0 + (address & 3)] = 0; /* FIXME */
        return val;
    }
    else if (address == 0xff)
    {
        return sm->device_mode[SM_DEV_SEMAPHORE];
    }
    else if (address == 0xf5)
    {
        return MCU_ReadP1(sm->mcu);
    }
    else if (address == 0xf6)
    {
        return MCU_ReadP0(sm->mcu);
    }
    else if (address == 0xf7)
    {
        return sm->p0_dir;
    }
    else
    {
        return 0;
    }
}

static uint16_t SM_GetVectorAddress(submcu_t *sm, uint32_t vector)
{
    uint16_t pc = SM_Read(sm, (uint16_t)(0x1fec + vector * 2));
    pc |= (uint16_t)(SM_Read(sm, (uint16_t)(0x1fec + vector * 2 + 1)) << 8);
    return pc;
}

static void SM_SetStatus(submcu_t *sm, uint32_t condition, uint32_t mask)
{
    if (condition)
        sm->sr |= (uint8_t)mask;
    else
        sm->sr &= (uint8_t)(~mask);
}

static void SM_Init(submcu_t *sm, mcu_t *mcu)
{
    sm->mcu = mcu;
}

static void SM_Reset(submcu_t *sm)
{
    sm->pc = SM_GetVectorAddress(sm, SM_VECTOR_RESET);
    sm->a = 0;
    sm->x = 0;
    sm->y = 0;
    sm->s = 0;
    sm->sr = 0;
    sm->cycles = 0;
    sm->sleep = 0;
}

static uint8_t SM_ReadAdvance(submcu_t *sm)
{
    uint8_t byte = SM_Read(sm, sm->pc);
    sm->pc++;
    return byte;
}

static uint16_t SM_ReadAdvance16(submcu_t *sm)
{
    uint16_t word = SM_ReadAdvance(sm);
    word |= (uint16_t)(SM_ReadAdvance(sm) << 8);
    return word;
}

static uint16_t SM_Read16(submcu_t *sm, uint16_t address)
{
    uint16_t word = SM_Read(sm, address);
    word |= (uint16_t)(SM_Read(sm, address) << 8);
    return word;
}

static void SM_Update_NZ(submcu_t *sm, uint8_t val)
{
    SM_SetStatus(sm, val == 0, SM_STATUS_Z);
    SM_SetStatus(sm, val & 0x80, SM_STATUS_N);
}

static void SM_PushStack(submcu_t *sm, uint8_t data)
{
    SM_Write(sm, sm->s, data);
    sm->s--;
}

static uint8_t SM_PopStack(submcu_t *sm)
{
    sm->s++;
    return SM_Read(sm, sm->s);
}

static void SM_Opcode_NotImplemented(submcu_t *sm, uint8_t opcode)
{
    (void)opcode;
    SM_ErrorTrap(sm);
}

static void SM_Opcode_SEI(submcu_t *sm, uint8_t opcode) /* 78 */
{
    (void)opcode;
    SM_SetStatus(sm, 1, SM_STATUS_I);
}

static void SM_Opcode_CLD(submcu_t *sm, uint8_t opcode) /* d8 */
{
    (void)opcode;
    SM_SetStatus(sm, 0, SM_STATUS_D);
}

static void SM_Opcode_CLT(submcu_t *sm, uint8_t opcode) /* 12 */
{
    (void)opcode;
    SM_SetStatus(sm, 0, SM_STATUS_T);
}

static void SM_Opcode_LDX(submcu_t *sm, uint8_t opcode) /* a2, a6, ae, b6, be */
{
    uint8_t val = 0;
    switch (opcode)
    {
        case 0xa2:
            val = SM_ReadAdvance(sm);
            break;
        case 0xa6:
            val = SM_Read(sm, SM_ReadAdvance(sm));
            break;
        case 0xb6:
            val = SM_Read(sm, (SM_ReadAdvance(sm) + sm->y) & 0xff);
            break;
        case 0xae:
            val = SM_Read(sm, SM_ReadAdvance16(sm));
            break;
        case 0xbe:
            val = SM_Read(sm, SM_ReadAdvance16(sm) + sm->y);
            break;
    }
    sm->x = val;
    SM_Update_NZ(sm, sm->x);
}

static void SM_Opcode_LDY(submcu_t *sm, uint8_t opcode) /* a0, a4, ac, b4, bc */
{
    uint8_t val = 0;
    switch (opcode)
    {
        case 0xa0:
            val = SM_ReadAdvance(sm);
            break;
        case 0xa4:
            val = SM_Read(sm, SM_ReadAdvance(sm));
            break;
        case 0xac:
            val = SM_Read(sm, SM_ReadAdvance16(sm));
            break;
        case 0xb4:
            val = SM_Read(sm, (SM_ReadAdvance(sm) + sm->x) & 0xff);
            break;
        case 0xbc:
            val = SM_Read(sm, SM_ReadAdvance16(sm) + sm->x);
            break;
    }
    sm->y = val;
    SM_Update_NZ(sm, sm->y);
}

static void SM_Opcode_TXS(submcu_t *sm, uint8_t opcode) /* 9a */
{
    (void)opcode;
    sm->s = sm->x;
}

static void SM_Opcode_TXA(submcu_t *sm, uint8_t opcode) /* 8a */
{
    (void)opcode;
    sm->a = sm->x;
    SM_Update_NZ(sm, sm->a);
}

static void SM_Opcode_STA(submcu_t *sm, uint8_t opcode) /* 85, 95, 8d, 9d, 99, 81, 91 */
{
    uint16_t dest = 0;
    switch (opcode)
    {
        case 0x85:
            dest = SM_ReadAdvance(sm);
            break;
        case 0x95:
            dest = SM_ReadAdvance(sm) + sm->x;
            break;
        case 0x8d:
            dest = SM_ReadAdvance16(sm);
            break;
        case 0x9d:
            dest = SM_ReadAdvance16(sm) + sm->x;
            break;
        case 0x99:
            dest = SM_ReadAdvance16(sm) + sm->y;
            break;
        case 0x81:
            dest = SM_Read16(sm, (SM_ReadAdvance(sm) + sm->x) & 0xff);
            break;
        case 0x91:
            dest = SM_Read16(sm, SM_ReadAdvance(sm)) + sm->y;
            break;
    }

    SM_Write(sm, dest, sm->a);
}

static void SM_Opcode_INX(submcu_t *sm, uint8_t opcode) /* e8 */
{
    (void)opcode;
    sm->x++;
    SM_Update_NZ(sm, sm->x);
}

static void SM_Opcode_INY(submcu_t *sm, uint8_t opcode) /* c8 */
{
    (void)opcode;
    sm->y++;
    SM_Update_NZ(sm, sm->y);
}

static void SM_Opcode_BBC_BBS(submcu_t *sm, uint8_t opcode)
{
    int8_t diff;
    int32_t set;
    int32_t zp = (opcode & 4) != 0;
    int32_t bit = (opcode >> 5) & 7;
    int32_t type = (opcode >> 4) & 1;
    uint8_t val = 0;

    if (!zp)
    {
        val = sm->a;
    }
    else
    {
        val = SM_Read(sm, SM_ReadAdvance(sm));
    }

    diff = (int8_t)SM_ReadAdvance(sm);

    set = (val >> bit) & 1;

    if (set != type)
        sm->pc += (uint16_t)diff;
}

static void SM_Opcode_CPX(submcu_t *sm, uint8_t opcode) /* e0, e4, ec */
{
    int diff;
    uint8_t operand = 0;
    switch (opcode)
    {
        case 0xe0:
            operand = SM_ReadAdvance(sm);
            break;
        case 0xe4:
            operand = SM_Read(sm, SM_ReadAdvance(sm));
            break;
        case 0xec:
            operand = SM_Read(sm, SM_ReadAdvance16(sm));
            break;
    }
    diff = sm->x - operand;
    SM_SetStatus(sm, (diff & 0x100) == 0, SM_STATUS_C);
    SM_Update_NZ(sm, (uint8_t)diff);
}

static void SM_Opcode_CPY(submcu_t *sm, uint8_t opcode) /* c0, c4, cc */
{
    int diff;
    uint8_t operand = 0;
    switch (opcode)
    {
        case 0xc0:
            operand = SM_ReadAdvance(sm);
            break;
        case 0xc4:
            operand = SM_Read(sm, SM_ReadAdvance(sm));
            break;
        case 0xcc:
            operand = SM_Read(sm, SM_ReadAdvance16(sm));
            break;
    }
    diff = sm->y - operand;
    SM_SetStatus(sm, (diff & 0x100) == 0, SM_STATUS_C);
    SM_Update_NZ(sm, (uint8_t)diff);
}

static void SM_Opcode_BEQ(submcu_t *sm, uint8_t opcode) /* f0 */
{
    int8_t diff;
    (void)opcode;
    diff = (int8_t)SM_ReadAdvance(sm);
    if ((sm->sr & SM_STATUS_Z) != 0)
        sm->pc += (uint16_t)diff;
}

static void SM_Opcode_BCC(submcu_t *sm, uint8_t opcode) /* 90 */
{
    int8_t diff;
    (void)opcode;
    diff = (int8_t)SM_ReadAdvance(sm);
    if ((sm->sr & SM_STATUS_C) == 0)
        sm->pc += (uint16_t)diff;
}

static void SM_Opcode_BCS(submcu_t *sm, uint8_t opcode) /* b0 */
{
    int8_t diff;
    (void)opcode;
    diff = (int8_t)SM_ReadAdvance(sm);
    if ((sm->sr & SM_STATUS_C) != 0)
        sm->pc += (uint16_t)diff;
}

static void SM_Opcode_LDM(submcu_t *sm, uint8_t opcode) /* 3c */
{
    uint8_t val;
    (void)opcode;
    val = SM_ReadAdvance(sm);
    SM_Write(sm, SM_ReadAdvance(sm), val);
}

static void SM_Opcode_LDA(submcu_t *sm, uint8_t opcode) /* a9, a5, b5, ad, bd, b9, a1, b1 */
{
    uint8_t val = 0;
    switch (opcode)
    {
        case 0xa9:
            val = SM_ReadAdvance(sm);
            break;
        case 0xa5:
            val = SM_Read(sm, SM_ReadAdvance(sm));
            break;
        case 0xb5:
            val = SM_Read(sm, (SM_ReadAdvance(sm) + sm->x) & 0xff);
            break;
        case 0xad:
            val = SM_Read(sm, SM_ReadAdvance16(sm));
            break;
        case 0xbd:
            val = SM_Read(sm, SM_ReadAdvance16(sm) + sm->x);
            break;
        case 0xb9:
            val = SM_Read(sm, SM_ReadAdvance16(sm) + sm->y);
            break;
        case 0xa1:
            val = SM_Read(sm, SM_Read16(sm, (SM_ReadAdvance(sm) + sm->x) & 0xff));
            break;
        case 0xb1:
            val = SM_Read(sm, SM_Read16(sm, SM_ReadAdvance(sm)) + sm->y);
            break;
    }

    if ((sm->sr & SM_STATUS_T) == 0)
    {
        sm->a = val;
        SM_Update_NZ(sm, val);
    }
    else
    {
        /* FIXME */
        SM_Write(sm, sm->x, val);
    }
}

static void SM_Opcode_CLI(submcu_t *sm, uint8_t opcode) /* 58 */
{
    (void)opcode;
    SM_SetStatus(sm, 0, SM_STATUS_I);
}

static void SM_Opcode_STP(submcu_t *sm, uint8_t opcode) /* 42 */
{
    (void)opcode;
    sm->sleep = 1;
}

static void SM_Opcode_PHA(submcu_t *sm, uint8_t opcode) /* 48 */
{
    (void)opcode;
    SM_PushStack(sm, sm->a);
}

static void SM_Opcode_SEB_CLB(submcu_t *sm, uint8_t opcode)
{
    int32_t zp = (opcode & 4) != 0;
    int32_t bit = (opcode >> 5) & 7;
    int32_t type = (opcode >> 4) & 1;
    uint8_t val = 0;
    uint8_t dest = 0;

    if (!zp)
    {
        val = sm->a;
    }
    else
    {
        dest = SM_ReadAdvance(sm);
        val = SM_Read(sm, dest);
    }

    if (type)
        val &= (uint8_t)(~(1 << bit));
    else
        val |= 1 << bit;

    if (!zp)
    {
        sm->a = val;
    }
    else
    {
        SM_Write(sm, dest, val);
    }
}

static void SM_Opcode_RTI(submcu_t *sm, uint8_t opcode) /* 40 */
{
    (void)opcode;
    sm->sr = SM_PopStack(sm);
    sm->pc = SM_PopStack(sm);
    sm->pc |= (uint16_t)(SM_PopStack(sm) << 8);
}

static void SM_Opcode_PLA(submcu_t *sm, uint8_t opcode) /* 68 */
{
    (void)opcode;
    sm->a = SM_PopStack(sm);
    SM_Update_NZ(sm, sm->a);
}

static void SM_Opcode_BRA(submcu_t *sm, uint8_t opcode) /* 80 */
{
    int8_t disp;
    (void)opcode;
    disp = (int8_t)SM_ReadAdvance(sm);
    sm->pc += (uint16_t)disp;
}

static void SM_Opcode_JSR(submcu_t *sm, uint8_t opcode) /* 20, 02, 22 */
{
    uint16_t newpc = 0;
    switch (opcode)
    {
        case 0x20:
            newpc = SM_ReadAdvance16(sm);
            break;
        case 0x02:
            newpc = SM_Read16(sm, SM_ReadAdvance(sm));
            break;
        case 0x22:
            newpc = 0xff00 | SM_ReadAdvance(sm);
            break;
    }

    SM_PushStack(sm, (uint8_t)(sm->pc >> 8));
    SM_PushStack(sm, (uint8_t)sm->pc);
    sm->pc = newpc;
}

static void SM_Opcode_CMP(submcu_t *sm, uint8_t opcode) /* c9, c5, d5, cd, dd, d9, c1, d1 */
{
    int diff;
    uint8_t operand = 0;
    switch (opcode)
    {
        case 0xc9:
            operand = SM_ReadAdvance(sm);
            break;
        case 0xc5:
            operand = SM_Read(sm, SM_ReadAdvance(sm));
            break;
        case 0xd5:
            operand = SM_Read(sm, (SM_ReadAdvance(sm)+sm->x)&0xff);
            break;
        case 0xcd:
            operand = SM_Read(sm, SM_ReadAdvance16(sm));
            break;
        case 0xdd:
            operand = SM_Read(sm, SM_ReadAdvance16(sm) + sm->x);
            break;
        case 0xd9:
            operand = SM_Read(sm, SM_ReadAdvance16(sm) + sm->y);
            break;
        case 0xc1:
            operand = SM_Read(sm, SM_Read16(sm, (SM_ReadAdvance(sm) + sm->x) & 0xff));
            break;
        case 0xd1:
            operand = SM_Read(sm, SM_Read16(sm, SM_ReadAdvance(sm)) + sm->y);
            break;
    }
    diff = sm->a - operand;
    SM_SetStatus(sm, (diff & 0x100) == 0, SM_STATUS_C);
    SM_Update_NZ(sm, (uint8_t)diff);
}

static void SM_Opcode_BNE(submcu_t *sm, uint8_t opcode) /* d0 */
{
    int8_t diff;
    (void)opcode;
    diff = (int8_t)SM_ReadAdvance(sm);
    if ((sm->sr & SM_STATUS_Z) == 0)
        sm->pc += (uint16_t)diff;
}

static void SM_Opcode_RTS(submcu_t *sm, uint8_t opcode) /* 60 */
{
    (void)opcode;
    sm->pc = SM_PopStack(sm);
    sm->pc |= (uint16_t)(SM_PopStack(sm) << 8);
}

static void SM_Opcode_JMP(submcu_t *sm, uint8_t opcode) /* 4c, 6c, b2 */
{
    switch (opcode)
    {
        case 0x4c:
            sm->pc = SM_ReadAdvance16(sm);
            break;
        case 0x6c:
            sm->pc = SM_Read16(sm, SM_ReadAdvance16(sm));
            break;
        case 0xb2:
            sm->pc = SM_Read16(sm, SM_ReadAdvance(sm));
            break;
    }
}

static void SM_Opcode_ORA(submcu_t *sm, uint8_t opcode) /* 09, 05, 15, 0d, 1d, 01, 11 */
{
    uint8_t val = 0;
    uint8_t val2 = 0;

    if ((sm->sr & SM_STATUS_T) == 0)
    {
        val = sm->a;
    }
    else
    {
        /* FIXME */
        val = SM_Read(sm, sm->x);
    }

    switch (opcode)
    {
        case 0x09:
            val2 = SM_ReadAdvance(sm);
            break;
        case 0x05:
            val2 = SM_Read(sm, SM_ReadAdvance(sm));
            break;
        case 0x15:
            val2 = SM_Read(sm, (SM_ReadAdvance(sm) + sm->x) & 0xff);
            break;
        case 0x0d:
            val2 = SM_Read(sm, SM_ReadAdvance16(sm));
            break;
        case 0x1d:
            val2 = SM_Read(sm, SM_ReadAdvance16(sm) + sm->x);
            break;
        case 0x19:
            val2 = SM_Read(sm, SM_ReadAdvance16(sm) + sm->y);
            break;
        case 0x01:
            val2 = SM_Read(sm, SM_Read16(sm, (SM_ReadAdvance(sm) + sm->x) & 0xff));
            break;
        case 0x11:
            val2 = SM_Read(sm, SM_Read16(sm, SM_ReadAdvance(sm)) + sm->y);
            break;
    }

    val |= val2;

    if ((sm->sr & SM_STATUS_T) == 0)
    {
        sm->a = val;

        SM_Update_NZ(sm, val);
    }
    else
    {
        /* FIXME */
        SM_Write(sm, sm->x, val);
    }
}

static void SM_Opcode_DEC(submcu_t *sm, uint8_t opcode) /* 1a, c6, d6, ce, de */
{
    uint8_t val = 0;
    uint16_t dest = 0;
    switch (opcode)
    {
        case 0x1a:
            sm->a--;
            SM_Update_NZ(sm, sm->a);
            return;
        case 0xc6:
            dest = SM_ReadAdvance(sm);
            break;
        case 0xd6:
            dest = (SM_ReadAdvance(sm) + sm->x) & 0xff;
            break;
        case 0xce:
            dest = SM_ReadAdvance16(sm);
            break;
        case 0xde:
            dest = SM_ReadAdvance16(sm) + sm->x;
            break;
    }
    val = SM_Read(sm, dest);
    val--;
    SM_Write(sm, dest, val);
    SM_Update_NZ(sm, val);
}

static void SM_Opcode_TAX(submcu_t *sm, uint8_t opcode) /* aa */
{
    (void)opcode;
    sm->x = sm->a;
    SM_Update_NZ(sm, sm->x);
}

static void SM_Opcode_STX(submcu_t *sm, uint8_t opcode) /* 86 96 8e */
{
    uint16_t dest = 0;
    switch (opcode)
    {
        case 0x86:
            dest = SM_ReadAdvance(sm);
            break;
        case 0x96:
            dest = SM_ReadAdvance(sm) + sm->x;
            break;
        case 0x8e:
            dest = SM_ReadAdvance16(sm);
            break;
    }

    SM_Write(sm, dest, sm->x);
}

static void SM_Opcode_STY(submcu_t *sm, uint8_t opcode) /* 84 8c 94 */
{
    uint16_t dest = 0;
    switch (opcode)
    {
        case 0x84:
            dest = SM_ReadAdvance(sm);
            break;
        case 0x94:
            dest = (SM_ReadAdvance(sm) + sm->x) & 0xff;
            break;
        case 0x8c:
            dest = SM_ReadAdvance16(sm);
            break;
    }

    SM_Write(sm, dest, sm->y);
}

static void SM_Opcode_SEC(submcu_t *sm, uint8_t opcode) /* 38 */
{
    (void)opcode;
    SM_SetStatus(sm, 1, SM_STATUS_C);
}

static void SM_Opcode_NOP(submcu_t *sm, uint8_t opcode) /* EA */
{
    (void)sm;
    (void)opcode;
}

static void SM_Opcode_BPL(submcu_t *sm, uint8_t opcode) /* 10 */
{
    int8_t diff;
    (void)opcode;
    diff = (int8_t)SM_ReadAdvance(sm);
    if ((sm->sr & SM_STATUS_N) == 0)
        sm->pc += (uint16_t)diff;
}

static void SM_Opcode_CLC(submcu_t *sm, uint8_t opcode) /* 18 */
{
    (void)opcode;
    SM_SetStatus(sm, 0, SM_STATUS_C);
}

static void SM_Opcode_AND(submcu_t *sm, uint8_t opcode) /* 29, 25, 35, 2d, 3d, 21, 31 */
{
    uint8_t val = 0;
    uint8_t val2 = 0;

    if ((sm->sr & SM_STATUS_T) == 0)
    {
        val = sm->a;
    }
    else
    {
        /* FIXME */
        val = SM_Read(sm, sm->x);
    }

    switch (opcode)
    {
        case 0x29:
            val2 = SM_ReadAdvance(sm);
            break;
        case 0x25:
            val2 = SM_Read(sm, SM_ReadAdvance(sm));
            break;
        case 0x35:
            val2 = SM_Read(sm, (SM_ReadAdvance(sm) + sm->x) & 0xff);
            break;
        case 0x2d:
            val2 = SM_Read(sm, SM_ReadAdvance16(sm));
            break;
        case 0x3d:
            val2 = SM_Read(sm, SM_ReadAdvance16(sm) + sm->x);
            break;
        case 0x39:
            val2 = SM_Read(sm, SM_ReadAdvance16(sm) + sm->y);
            break;
        case 0x21:
            val2 = SM_Read(sm, SM_Read16(sm, (SM_ReadAdvance(sm) + sm->x) & 0xff));
            break;
        case 0x31:
            val2 = SM_Read(sm, SM_Read16(sm, SM_ReadAdvance(sm)) + sm->y);
            break;
    }

    val &= val2;

    if ((sm->sr & SM_STATUS_T) == 0)
    {
        sm->a = val;

        SM_Update_NZ(sm, val);
    }
    else
    {
        /* FIXME */
        SM_Write(sm, sm->x, val);
    }
}

static void SM_Opcode_INC(submcu_t *sm, uint8_t opcode) /* 3a, e6, f6, ee, fe */
{
    uint8_t val = 0;
    uint16_t dest = 0;
    switch (opcode)
    {
        case 0x3a:
            sm->a++;
            SM_Update_NZ(sm, sm->a);
            return;
        case 0xe6:
            dest = SM_ReadAdvance(sm);
            break;
        case 0xf6:
            dest = (SM_ReadAdvance(sm) + sm->x) & 0xff;
            break;
        case 0xee:
            dest = SM_ReadAdvance16(sm);
            break;
        case 0xfe:
            dest = SM_ReadAdvance16(sm) + sm->x;
            break;
    }
    val = SM_Read(sm, dest);
    val++;
    SM_Write(sm, dest, val);
    SM_Update_NZ(sm, val);
}

static void (*SM_Opcode_Table[256])(submcu_t *sm, uint8_t opcode) =
{
    SM_Opcode_NotImplemented, /* 00 */
    SM_Opcode_ORA, /* 01 */
    SM_Opcode_JSR, /* 02 */
    SM_Opcode_BBC_BBS, /* 03 */
    SM_Opcode_NotImplemented, /* 04 */
    SM_Opcode_ORA, /* 05 */
    SM_Opcode_NotImplemented, /* 06 */
    SM_Opcode_BBC_BBS, /* 07 */
    SM_Opcode_NotImplemented, /* 08 */
    SM_Opcode_ORA, /* 09 */
    SM_Opcode_NotImplemented, /* 0a */
    SM_Opcode_SEB_CLB, /* 0b */
    SM_Opcode_NotImplemented, /* 0c */
    SM_Opcode_ORA, /* 0d */
    SM_Opcode_NotImplemented, /* 0e */
    SM_Opcode_SEB_CLB, /* 0f */
    SM_Opcode_BPL, /* 10 */
    SM_Opcode_ORA, /* 11 */
    SM_Opcode_CLT, /* 12 */
    SM_Opcode_BBC_BBS, /* 13 */
    SM_Opcode_NotImplemented, /* 14 */
    SM_Opcode_ORA, /* 15 */
    SM_Opcode_NotImplemented, /* 16 */
    SM_Opcode_BBC_BBS, /* 17 */
    SM_Opcode_CLC, /* 18 */
    SM_Opcode_ORA, /* 19 */
    SM_Opcode_DEC, /* 1a */
    SM_Opcode_SEB_CLB, /* 1b */
    SM_Opcode_NotImplemented, /* 1c */
    SM_Opcode_ORA, /* 1d */
    SM_Opcode_NotImplemented, /* 1e */
    SM_Opcode_SEB_CLB, /* 1f */
    SM_Opcode_JSR, /* 20 */
    SM_Opcode_AND, /* 21 */
    SM_Opcode_JSR, /* 22 */
    SM_Opcode_BBC_BBS, /* 23 */
    SM_Opcode_NotImplemented, /* 24 */
    SM_Opcode_AND, /* 25 */
    SM_Opcode_NotImplemented, /* 26 */
    SM_Opcode_BBC_BBS, /* 27 */
    SM_Opcode_NotImplemented, /* 28 */
    SM_Opcode_AND, /* 29 */
    SM_Opcode_NotImplemented, /* 2a */
    SM_Opcode_SEB_CLB, /* 2b */
    SM_Opcode_NotImplemented, /* 2c */
    SM_Opcode_AND, /* 2d */
    SM_Opcode_NotImplemented, /* 2e */
    SM_Opcode_SEB_CLB, /* 2f */
    SM_Opcode_NotImplemented, /* 30 */
    SM_Opcode_AND, /* 31 */
    SM_Opcode_NotImplemented, /* 32 */
    SM_Opcode_BBC_BBS, /* 33 */
    SM_Opcode_NotImplemented, /* 34 */
    SM_Opcode_AND, /* 35 */
    SM_Opcode_NotImplemented, /* 36 */
    SM_Opcode_BBC_BBS, /* 37 */
    SM_Opcode_SEC, /* 38 */
    SM_Opcode_AND, /* 39 */
    SM_Opcode_INC, /* 3a */
    SM_Opcode_SEB_CLB, /* 3b */
    SM_Opcode_LDM, /* 3c */
    SM_Opcode_AND, /* 3d */
    SM_Opcode_NotImplemented, /* 3e */
    SM_Opcode_SEB_CLB, /* 3f */
    SM_Opcode_RTI, /* 40 */
    SM_Opcode_NotImplemented, /* 41 */
    SM_Opcode_STP, /* 42 */
    SM_Opcode_BBC_BBS, /* 43 */
    SM_Opcode_NotImplemented, /* 44 */
    SM_Opcode_NotImplemented, /* 45 */
    SM_Opcode_NotImplemented, /* 46 */
    SM_Opcode_BBC_BBS, /* 47 */
    SM_Opcode_PHA, /* 48 */
    SM_Opcode_NotImplemented, /* 49 */
    SM_Opcode_NotImplemented, /* 4a */
    SM_Opcode_SEB_CLB, /* 4b */
    SM_Opcode_JMP, /* 4c */
    SM_Opcode_NotImplemented, /* 4d */
    SM_Opcode_NotImplemented, /* 4e */
    SM_Opcode_SEB_CLB, /* 4f */
    SM_Opcode_NotImplemented, /* 50 */
    SM_Opcode_NotImplemented, /* 51 */
    SM_Opcode_NotImplemented, /* 52 */
    SM_Opcode_BBC_BBS, /* 53 */
    SM_Opcode_NotImplemented, /* 54 */
    SM_Opcode_NotImplemented, /* 55 */
    SM_Opcode_NotImplemented, /* 56 */
    SM_Opcode_BBC_BBS, /* 57 */
    SM_Opcode_CLI, /* 58 */
    SM_Opcode_NotImplemented, /* 59 */
    SM_Opcode_NotImplemented, /* 5a */
    SM_Opcode_SEB_CLB, /* 5b */
    SM_Opcode_NotImplemented, /* 5c */
    SM_Opcode_NotImplemented, /* 5d */
    SM_Opcode_NotImplemented, /* 5e */
    SM_Opcode_SEB_CLB, /* 5f */
    SM_Opcode_RTS, /* 60 */
    SM_Opcode_NotImplemented, /* 61 */
    SM_Opcode_NotImplemented, /* 62 */
    SM_Opcode_BBC_BBS, /* 63 */
    SM_Opcode_NotImplemented, /* 64 */
    SM_Opcode_NotImplemented, /* 65 */
    SM_Opcode_NotImplemented, /* 66 */
    SM_Opcode_BBC_BBS, /* 67 */
    SM_Opcode_PLA, /* 68 */
    SM_Opcode_NotImplemented, /* 69 */
    SM_Opcode_NotImplemented, /* 6a */
    SM_Opcode_SEB_CLB, /* 6b */
    SM_Opcode_JMP, /* 6c */
    SM_Opcode_NotImplemented, /* 6d */
    SM_Opcode_NotImplemented, /* 6e */
    SM_Opcode_SEB_CLB, /* 6f */
    SM_Opcode_NotImplemented, /* 70 */
    SM_Opcode_NotImplemented, /* 71 */
    SM_Opcode_NotImplemented, /* 72 */
    SM_Opcode_BBC_BBS, /* 73 */
    SM_Opcode_NotImplemented, /* 74 */
    SM_Opcode_NotImplemented, /* 75 */
    SM_Opcode_NotImplemented, /* 76 */
    SM_Opcode_BBC_BBS, /* 77 */
    SM_Opcode_SEI, /* 78 */
    SM_Opcode_NotImplemented, /* 79 */
    SM_Opcode_NotImplemented, /* 7a */
    SM_Opcode_SEB_CLB, /* 7b */
    SM_Opcode_NotImplemented, /* 7c */
    SM_Opcode_NotImplemented, /* 7d */
    SM_Opcode_NotImplemented, /* 7e */
    SM_Opcode_SEB_CLB, /* 7f */
    SM_Opcode_BRA, /* 80 */
    SM_Opcode_STA, /* 81 */
    SM_Opcode_NotImplemented, /* 82 */
    SM_Opcode_BBC_BBS, /* 83 */
    SM_Opcode_STY, /* 84 */
    SM_Opcode_STA, /* 85 */
    SM_Opcode_STX, /* 86 */
    SM_Opcode_BBC_BBS, /* 87 */
    SM_Opcode_NotImplemented, /* 88 */
    SM_Opcode_NotImplemented, /* 89 */
    SM_Opcode_TXA, /* 8a */
    SM_Opcode_SEB_CLB, /* 8b */
    SM_Opcode_STY, /* 8c */
    SM_Opcode_STA, /* 8d */
    SM_Opcode_STX, /* 8e */
    SM_Opcode_SEB_CLB, /* 8f */
    SM_Opcode_BCC, /* 90 */
    SM_Opcode_STA, /* 91 */
    SM_Opcode_NotImplemented, /* 92 */
    SM_Opcode_BBC_BBS, /* 93 */
    SM_Opcode_STY, /* 94 */
    SM_Opcode_STA, /* 95 */
    SM_Opcode_STX, /* 96 */
    SM_Opcode_BBC_BBS, /* 97 */
    SM_Opcode_NotImplemented, /* 98 */
    SM_Opcode_STA, /* 99 */
    SM_Opcode_TXS, /* 9a */
    SM_Opcode_SEB_CLB, /* 9b */
    SM_Opcode_NotImplemented, /* 9c */
    SM_Opcode_STA, /* 9d */
    SM_Opcode_NotImplemented, /* 9e */
    SM_Opcode_SEB_CLB, /* 9f */
    SM_Opcode_LDY, /* a0 */
    SM_Opcode_LDA, /* a1 */
    SM_Opcode_LDX, /* a2 */
    SM_Opcode_BBC_BBS, /* a3 */
    SM_Opcode_LDY, /* a4 */
    SM_Opcode_LDA, /* a5 */
    SM_Opcode_LDX, /* a6 */
    SM_Opcode_BBC_BBS, /* a7 */
    SM_Opcode_NotImplemented, /* a8 */
    SM_Opcode_LDA, /* a9 */
    SM_Opcode_TAX, /* aa */
    SM_Opcode_SEB_CLB, /* ab */
    SM_Opcode_LDY, /* ac */
    SM_Opcode_LDA, /* ad */
    SM_Opcode_LDX, /* ae */
    SM_Opcode_SEB_CLB, /* af */
    SM_Opcode_BCS, /* b0 */
    SM_Opcode_LDA, /* b1 */
    SM_Opcode_JMP, /* b2 */
    SM_Opcode_BBC_BBS, /* b3 */
    SM_Opcode_LDY, /* b4 */
    SM_Opcode_LDA, /* b5 */
    SM_Opcode_LDX, /* b6 */
    SM_Opcode_BBC_BBS, /* b7 */
    SM_Opcode_NotImplemented, /* b8 */
    SM_Opcode_LDA, /* b9 */
    SM_Opcode_NotImplemented, /* ba */
    SM_Opcode_SEB_CLB, /* bb */
    SM_Opcode_LDY, /* bc */
    SM_Opcode_LDA, /* bd */
    SM_Opcode_LDX, /* be */
    SM_Opcode_SEB_CLB, /* bf */
    SM_Opcode_CPY, /* c0 */
    SM_Opcode_CMP, /* c1 */
    SM_Opcode_NotImplemented, /* c2 */
    SM_Opcode_BBC_BBS, /* c3 */
    SM_Opcode_CPY, /* c4 */
    SM_Opcode_CMP, /* c5 */
    SM_Opcode_DEC, /* c6 */
    SM_Opcode_BBC_BBS, /* c7 */
    SM_Opcode_INY, /* c8 */
    SM_Opcode_CMP, /* c9 */
    SM_Opcode_NotImplemented, /* ca */
    SM_Opcode_SEB_CLB, /* cb */
    SM_Opcode_CPY, /* cc */
    SM_Opcode_CMP, /* cd */
    SM_Opcode_DEC, /* ce */
    SM_Opcode_SEB_CLB, /* cf */
    SM_Opcode_BNE, /* d0 */
    SM_Opcode_CMP, /* d1 */
    SM_Opcode_NotImplemented, /* d2 */
    SM_Opcode_BBC_BBS, /* d3 */
    SM_Opcode_NotImplemented, /* d4 */
    SM_Opcode_CMP, /* d5 */
    SM_Opcode_DEC, /* d6 */
    SM_Opcode_BBC_BBS, /* d7 */
    SM_Opcode_CLD, /* d8 */
    SM_Opcode_CMP, /* d9 */
    SM_Opcode_NotImplemented, /* da */
    SM_Opcode_SEB_CLB, /* db */
    SM_Opcode_NotImplemented, /* dc */
    SM_Opcode_CMP, /* dd */
    SM_Opcode_DEC, /* de */
    SM_Opcode_SEB_CLB, /* df */
    SM_Opcode_CPX, /* e0 */
    SM_Opcode_NotImplemented, /* e1 */
    SM_Opcode_NotImplemented, /* e2 */
    SM_Opcode_BBC_BBS, /* e3 */
    SM_Opcode_CPX, /* e4 */
    SM_Opcode_NotImplemented, /* e5 */
    SM_Opcode_INC, /* e6 */
    SM_Opcode_BBC_BBS, /* e7 */
    SM_Opcode_INX, /* e8 */
    SM_Opcode_NotImplemented, /* e9 */
    SM_Opcode_NOP, /* ea */
    SM_Opcode_SEB_CLB, /* eb */
    SM_Opcode_CPX, /* ec */
    SM_Opcode_NotImplemented, /* ed */
    SM_Opcode_INC, /* ee */
    SM_Opcode_SEB_CLB, /* ef */
    SM_Opcode_BEQ, /* f0 */
    SM_Opcode_NotImplemented, /* f1 */
    SM_Opcode_NotImplemented, /* f2 */
    SM_Opcode_BBC_BBS, /* f3 */
    SM_Opcode_NotImplemented, /* f4 */
    SM_Opcode_NotImplemented, /* f5 */
    SM_Opcode_INC, /* f6 */
    SM_Opcode_BBC_BBS, /* f7 */
    SM_Opcode_NotImplemented, /* f8 */
    SM_Opcode_NotImplemented, /* f9 */
    SM_Opcode_NotImplemented, /* fa */
    SM_Opcode_SEB_CLB, /* fb */
    SM_Opcode_NotImplemented, /* fc */
    SM_Opcode_NotImplemented, /* fd */
    SM_Opcode_INC, /* fe */
    SM_Opcode_SEB_CLB, /* ff */
};

static void SM_StartVector(submcu_t *sm, uint32_t vector)
{
    SM_PushStack(sm, (uint8_t)(sm->pc >> 8));
    SM_PushStack(sm, (uint8_t)sm->pc);
    SM_PushStack(sm, sm->sr);

    sm->sr |= SM_STATUS_I;
    sm->sleep = 0;

    sm->pc = SM_GetVectorAddress(sm, vector);
}

static void SM_HandleInterrupt(submcu_t *sm)
{
    if (sm->sr & SM_STATUS_I)
        return;

    if ((sm->device_mode[SM_DEV_UART1_CTRL] & 0x8) != 0
        && (sm->device_mode[SM_DEV_INT_ENABLE] & 0x80) != 0
        && (sm->device_mode[SM_DEV_INT_REQUEST] & 0x80) != 0)
    {
        sm->device_mode[SM_DEV_INT_REQUEST] &= ~0x80;
        SM_StartVector(sm, SM_VECTOR_UART1_RX);
        return;
    }
    if ((sm->device_mode[SM_DEV_UART2_CTRL] & 0x8) != 0
        && (sm->device_mode[SM_DEV_INT_ENABLE] & 0x40) != 0
        && (sm->device_mode[SM_DEV_INT_REQUEST] & 0x40) != 0)
    {
        sm->device_mode[SM_DEV_INT_REQUEST] &= ~0x40;
        SM_StartVector(sm, SM_VECTOR_UART2_RX);
        return;
    }
    if ((sm->device_mode[SM_DEV_UART3_CTRL] & 0x8) != 0
        && (sm->device_mode[SM_DEV_INT_ENABLE] & 0x20) != 0
        && (sm->device_mode[SM_DEV_INT_REQUEST] & 0x20) != 0)
    {
        sm->device_mode[SM_DEV_INT_REQUEST] &= ~0x20;
        SM_StartVector(sm, SM_VECTOR_UART3_RX);
        return;
    }
    if ((sm->device_mode[SM_DEV_TIMER_CTRL] & 0x80) != 0
        && (sm->device_mode[SM_DEV_INT_ENABLE] & 0x10) != 0
        && (sm->device_mode[SM_DEV_INT_REQUEST] & 0x10) != 0)
    {
        sm->device_mode[SM_DEV_INT_REQUEST] &= ~0x10;
        SM_StartVector(sm, SM_VECTOR_IPCM0);
        return;
    }
    if ((sm->device_mode[SM_DEV_TIMER_CTRL] & 0x40) != 0
        && (sm->device_mode[SM_DEV_INT_ENABLE] & 0x8) != 0
        && (sm->device_mode[SM_DEV_INT_REQUEST] & 0x8) != 0)
    {
        sm->device_mode[SM_DEV_INT_REQUEST] &= ~0x8;
        SM_StartVector(sm, SM_VECTOR_TIMER_X);
        return;
    }
    if ((sm->device_mode[SM_DEV_COLLISION] & 0xc0) == 0xc0)
    {
        sm->device_mode[SM_DEV_COLLISION] &= ~0x80;
        SM_StartVector(sm, SM_VECTOR_COLLISION);
        return;
    }
    if (((sm->device_mode[SM_DEV_UART1_CTRL] & 0x10) == 0
        || (sm->cts & 1) != 0)
        && (sm->device_mode[SM_DEV_INT_ENABLE] & 0x4) != 0
        && (sm->device_mode[SM_DEV_INT_REQUEST] & 0x4) != 0)
    {
        sm->device_mode[SM_DEV_INT_REQUEST] &= ~0x4;
        SM_StartVector(sm, SM_VECTOR_UART1_TX);
        return;
    }
    if (((sm->device_mode[SM_DEV_UART2_CTRL] & 0x10) == 0
        || (sm->cts & 2) != 0)
        && (sm->device_mode[SM_DEV_INT_ENABLE] & 0x2) != 0
        && (sm->device_mode[SM_DEV_INT_REQUEST] & 0x2) != 0)
    {
        sm->device_mode[SM_DEV_INT_REQUEST] &= ~0x2;
        SM_StartVector(sm, SM_VECTOR_UART2_TX);
        return;
    }
    if (((sm->device_mode[SM_DEV_UART3_CTRL] & 0x10) == 0
        || (sm->cts & 4) != 0)
        && (sm->device_mode[SM_DEV_INT_ENABLE] & 0x1) != 0
        && (sm->device_mode[SM_DEV_INT_REQUEST] & 0x1) != 0)
    {
        sm->device_mode[SM_DEV_INT_REQUEST] &= ~0x1;
        SM_StartVector(sm, SM_VECTOR_UART3_TX);
        return;
    }
}

static void SM_UpdateTimer(submcu_t *sm)
{
    while (sm->timer_cycles < sm->cycles)
    {
        if ((sm->device_mode[SM_DEV_TIMER_CTRL] & 0x20) == 0 && !sm->sleep)
        {
            if (sm->timer_prescaler == 0)
            {
                sm->timer_prescaler = sm->device_mode[SM_DEV_PRESCALER];

                if (sm->timer_counter == 0)
                {
                    sm->timer_counter = sm->device_mode[SM_DEV_TIMER];
                    sm->device_mode[SM_DEV_INT_REQUEST] |= 0x8;
                }
                else
                    sm->timer_counter--;
            }
            else
                sm->timer_prescaler--;
        }
        sm->timer_cycles += 16;
    }
}

static void SM_UpdateUART(submcu_t *sm)
{
    mcu_t *mcu = sm->mcu;

    if ((sm->device_mode[SM_DEV_UART1_CTRL] & 4) == 0) /* RX disabled */
        return;
    if (mcu->uart_write_ptr == mcu->uart_read_ptr) /* no byte */
        return;

    if (sm->uart_rx_gotbyte)
        return;

    if (sm->cycles < mcu->uart_rx_delay)
        return;

    mcu->uart_rx_byte = mcu->uart_buffer[mcu->uart_read_ptr];
    mcu->uart_read_ptr = (mcu->uart_read_ptr + 1) % uart_buffer_size;
    sm->uart_rx_gotbyte = 1;
    sm->device_mode[SM_DEV_INT_REQUEST] |= 0x40;

    mcu->uart_rx_delay = sm->cycles + 3000 * 4;
}

static void SM_Update(submcu_t *sm, uint64_t cycles)
{
    while (sm->cycles < cycles * 5)
    {
        SM_HandleInterrupt(sm);

        if (!sm->sleep)
        {
            uint8_t opcode = SM_ReadAdvance(sm);

            SM_Opcode_Table[opcode](sm, opcode);
        }

        sm->cycles += 12 * 4; /* FIXME */

        SM_UpdateTimer(sm);
        SM_UpdateUART(sm);
    }
}

static int32_t MCU_SUB_Common(mcu_t *mcu, int32_t t1, int32_t t2, int32_t c_bit, MCU_Operand_Size siz)
{
    int32_t st1, st2;
    int N = 0, Z = 0, C = 0, V = 0;
    switch (siz)
    {
    case OPERAND_WORD:
        st1 = (int16_t)t1;
        st2 = (int16_t)t2;
        t1 = (uint16_t)t1;
        t2 = (uint16_t)t2;
        t1 -= t2;
        t1 -= c_bit;
        C = (t1 >> 16) & 1;

        t1 &= 0xffff;
        N = (t1 & 0x8000) != 0;
        Z = t1 == 0;

        st1 -= st2;
        st1 -= c_bit;
        if (st1 < INT16_MIN || st1 > INT16_MAX)
            V = 1;
        break;
    case OPERAND_BYTE:
        st1 = (int8_t)t1;
        st2 = (int8_t)t2;
        t1 = (uint8_t)t1;
        t2 = (uint8_t)t2;
        t1 -= t2;
        t1 -= c_bit;
        C = (t1 >> 8) & 1;

        t1 &= 0xff;
        N = (t1 & 0x80) != 0;
        Z = t1 == 0;

        st1 -= st2;
        st1 -= c_bit;
        if (st1 < INT8_MIN || st1 > INT8_MAX)
            V = 1;
        break;
    default:
        /* reason: siz provided always valid */
        break;
    }
    MCU_SetStatus(mcu, N, STATUS_N);
    MCU_SetStatus(mcu, Z, STATUS_Z);
    MCU_SetStatus(mcu, C, STATUS_C);
    MCU_SetStatus(mcu, V, STATUS_V);

    return t1;
}

static int32_t MCU_ADD_Common(mcu_t *mcu, int32_t t1, int32_t t2, int32_t c_bit, MCU_Operand_Size siz)
{
    int32_t st1, st2;
    int N = 0, Z = 0, C = 0, V = 0;
    switch (siz)
    {
    case OPERAND_WORD:
        st1 = (int16_t)t1;
        st2 = (int16_t)t2;
        t1 = (uint16_t)t1;
        t2 = (uint16_t)t2;
        t1 += t2;
        t1 += c_bit;
        C = (t1 >> 16) & 1;

        t1 &= 0xffff;
        N = (t1 & 0x8000) != 0;
        Z = t1 == 0;

        st1 += st2;
        st1 += c_bit;
        if (st1 < INT16_MIN || st1 > INT16_MAX)
            V = 1;
        break;
    case OPERAND_BYTE:
        st1 = (int8_t)t1;
        st2 = (int8_t)t2;
        t1 = (uint8_t)t1;
        t2 = (uint8_t)t2;
        t1 += t2;
        t1 += c_bit;
        C = (t1 >> 8) & 1;

        t1 &= 0xff;
        N = (t1 & 0x80) != 0;
        Z = t1 == 0;

        st1 += st2;
        st1 += c_bit;
        if (st1 < INT8_MIN || st1 > INT8_MAX)
            V = 1;
        break;
    default:
        /* reason: siz provided always valid */
        break;
    }
    MCU_SetStatus(mcu, N, STATUS_N);
    MCU_SetStatus(mcu, Z, STATUS_Z);
    MCU_SetStatus(mcu, C, STATUS_C);
    MCU_SetStatus(mcu, V, STATUS_V);

    return t1;
}

static void MCU_Operand_Nop(mcu_t *mcu, uint8_t operand)
{
    (void)mcu;
    (void)operand;
}

static void MCU_Operand_Sleep(mcu_t *mcu, uint8_t operand)
{
    (void)operand;
    mcu->sleep = 1;
}

static void MCU_Operand_NotImplemented(mcu_t *mcu, uint8_t operand)
{
    (void)operand;
    MCU_ErrorTrap(mcu);
}

enum {
    GENERAL_DIRECT = 0,
    GENERAL_INDIRECT,
    GENERAL_ABSOLUTE,
    GENERAL_IMMEDIATE
};

enum {
    INCREASE_NONE = 0,
    INCREASE_DECREASE,
    INCREASE_INCREASE
};

static void MCU_LDM(mcu_t *mcu, uint8_t operand)
{
    uint8_t rlist;
    int32_t i;
    (void)operand;
    rlist = MCU_ReadCodeAdvance(mcu);
    for (i = 0; i < 8; i++)
    {
        if (rlist & (1 << i))
        {
            uint16_t data = MCU_PopStack(mcu);
            if (i != 7)
                mcu->r[i] = data;
        }
    }
}

static void MCU_STM(mcu_t *mcu, uint8_t operand)
{
    uint8_t rlist;
    int32_t i;
    (void)operand;
    rlist = MCU_ReadCodeAdvance(mcu);
    for (i = 7; i >= 0; i--)
    {
        if (rlist & (1 << i))
        {
            uint16_t data = mcu->r[i];
            if (i == 7)
                data -= 2;
            MCU_PushStack(mcu, data);
        }
    }
}

static void MCU_TRAPA(mcu_t *mcu, uint8_t operand)
{
    uint32_t opcode;
    (void)operand;
    opcode = MCU_ReadCodeAdvance(mcu);
    if ((opcode & 0xf0) == 0x10)
    {
        MCU_Interrupt_TRAPA(mcu, opcode & 0x0f);
    }
    else
    {
        MCU_ErrorTrap(mcu);
    }
}

static void MCU_Jump_PJSR(mcu_t *mcu, uint8_t operand)
{
    uint32_t ocp;
    uint32_t opc;
    uint8_t page;
    uint16_t address;
    (void)operand;
    ocp = mcu->cp;
    (void)ocp; /* unused */
    opc = mcu->pc;
    (void)opc; /* unused */
    page = MCU_ReadCodeAdvance(mcu);
    address = (uint16_t)(MCU_ReadCodeAdvance(mcu) << 8);
    address |= MCU_ReadCodeAdvance(mcu);
    MCU_PushStack(mcu, mcu->pc);
    MCU_PushStack(mcu, mcu->cp);
    mcu->cp = page;
    if (mcu->cp == 0x27)
        mcu->cp += 0;
    mcu->pc = address;
}

static void MCU_Jump_JSR(mcu_t *mcu, uint8_t operand)
{
    uint16_t address;
    (void)operand;
    address = (uint16_t)(MCU_ReadCodeAdvance(mcu) << 8);
    address |= MCU_ReadCodeAdvance(mcu);
    MCU_PushStack(mcu, mcu->pc);
    mcu->pc = address;
}

static void MCU_Jump_RTE(mcu_t *mcu, uint8_t operand)
{
    (void)operand;
    mcu->sr = MCU_PopStack(mcu);
    mcu->cp = (uint8_t)MCU_PopStack(mcu);
    mcu->pc = MCU_PopStack(mcu);
    mcu->ex_ignore = 1;
}

static void MCU_Jump_Bcc(mcu_t *mcu, uint8_t operand)
{
    uint16_t disp;
    uint32_t cond;
    int branch = 0;
    int N, C, Z, V;
    if (operand & 0x10)
    {
        disp = (uint16_t)(MCU_ReadCodeAdvance(mcu) << 8);
        disp |= MCU_ReadCodeAdvance(mcu);
    }
    else
    {
        disp = (uint16_t)(int8_t)MCU_ReadCodeAdvance(mcu);
    }
    cond = operand & 0x0f;

    N = (mcu->sr & STATUS_N) != 0;
    C = (mcu->sr & STATUS_C) != 0;
    Z = (mcu->sr & STATUS_Z) != 0;
    V = (mcu->sr & STATUS_V) != 0;

    switch (cond)
    {
    case 0x0: /* BRA/BT */
        branch = 1;
        break;
    case 0x1: /* BRN/BF */
        branch = 0;
        break;
    case 0x2: /* BHI */
        branch = (C | Z) == 0;
        break;
    case 0x3: /* BLS */
        branch = (C | Z) == 1;
        break;
    case 0x4: /* BCC/BHS */
        branch = C == 0;
        break;
    case 0x5: /* BCS/BLO */
        branch = C == 1;
        break;
    case 0x6: /* BNE */
        branch = Z == 0;
        break;
    case 0x7: /* BEQ */
        branch = Z == 1;
        break;
    case 0x8: /* BVC */
        branch = V == 0;
        break;
    case 0x9: /* BVS */
        branch = V == 1;
        break;
    case 0xa: /* BPL */
        branch = N == 0;
        break;
    case 0xb: /* BMI */
        branch = N == 1;
        break;
    case 0xc: /* BGE */
        branch = (N ^ V) == 0;
        break;
    case 0xd: /* BLT */
        branch = (N ^ V) == 1;
        break;
    case 0xe: /* BGT */
        branch = (Z | (N ^ V)) == 0;
        break;
    case 0xf: /* BLE */
        branch = (Z | (N ^ V)) == 1;
        break;
    }

    if (branch)
    {
        mcu->pc += disp;
    }
}

static void MCU_Jump_RTS(mcu_t *mcu, uint8_t operand)
{
    (void)operand;
    mcu->pc = MCU_PopStack(mcu);
}

static void MCU_Jump_RTD(mcu_t *mcu, uint8_t operand)
{
    int16_t imm = (int8_t)MCU_ReadCodeAdvance(mcu);
    mcu->pc = MCU_PopStack(mcu);

    if (operand == 0x14)
    {
        mcu->r[7] = (uint16_t)(mcu->r[7] + imm);
        if (mcu->r[7] & 1)
            MCU_ErrorTrap(mcu);
    }
    else if (operand == 0x1c)
    {
        /* TODO */
        MCU_ErrorTrap(mcu);
    }
    else
    {
        MCU_ErrorTrap(mcu);
    }
}

static void MCU_Jump_JMP(mcu_t *mcu, uint8_t operand)
{
    if (operand == 0x11)
    {
        uint8_t opcode = MCU_ReadCodeAdvance(mcu);
        uint8_t opcode_h = opcode >> 3;
        uint8_t opcode_l = opcode & 0x07;
        if (opcode == 0x19)
        {
            mcu->cp = (uint8_t)MCU_PopStack(mcu);
            mcu->pc = MCU_PopStack(mcu);
        }
        else if (opcode_h == 0x19)
        {
            MCU_PushStack(mcu, mcu->pc);
            MCU_PushStack(mcu, mcu->cp);
            opcode_l &= ~1;
            mcu->cp = (uint8_t)(mcu->r[opcode_l] & 0xff);
            mcu->pc = (uint16_t)mcu->r[opcode_l + 1];
        }
        else if (opcode_h == 0x1a)
        {
            mcu->pc = mcu->r[opcode_l];
        }
        else if (opcode_h == 0x1b)
        {
            MCU_PushStack(mcu, mcu->pc);
            mcu->pc = mcu->r[opcode_l];
        }
        else
        {
            MCU_ErrorTrap(mcu);
        }
    }
    else if (operand == 0x01)
    {
        uint8_t opcode = MCU_ReadCodeAdvance(mcu);
        uint8_t reg = opcode & 0x07;
        opcode >>= 3;
        if (opcode == 0x17)
        {
            uint16_t disp = (uint16_t)(int8_t)MCU_ReadCodeAdvance(mcu);
            mcu->r[reg]--;
            if (mcu->r[reg] != 0xffff)
            {
                mcu->pc += disp;
            }
        }
        else
        {
            MCU_ErrorTrap(mcu);
        }
    }
    else if (operand == 0x10)
    {
        uint16_t addr;
        addr = (uint16_t)(MCU_ReadCodeAdvance(mcu) << 8);
        addr |= MCU_ReadCodeAdvance(mcu);
        mcu->pc = addr;
    }
    else if (operand == 0x06)
    {
        uint8_t opcode = MCU_ReadCodeAdvance(mcu);
        uint8_t reg = opcode & 0x07;
        opcode >>= 3;
        if (opcode == 0x17)
        {
            uint16_t disp = (uint16_t)(int8_t)MCU_ReadCodeAdvance(mcu);
            const int Z = (mcu->sr & STATUS_Z) != 0;
            if (Z)
            {
                mcu->r[reg]--;
                if (mcu->r[reg] != 0xffff)
                {
                    mcu->pc += disp;
                }
            }
        }
        else
        {
            MCU_ErrorTrap(mcu);
        }
    }
    else if (operand == 0x07)
    {
        uint8_t opcode = MCU_ReadCodeAdvance(mcu);
        uint8_t reg = opcode & 0x07;
        opcode >>= 3;
        if (opcode == 0x17)
        {
            uint16_t disp = (uint16_t)(int8_t)MCU_ReadCodeAdvance(mcu);
            const int Z = (mcu->sr & STATUS_Z) != 0;
            if (!Z)
            {
                mcu->r[reg]--;
                if (mcu->r[reg] != 0xffff)
                {
                    mcu->pc += disp;
                }
            }
        }
        else
        {
            MCU_ErrorTrap(mcu);
        }
    }
    else
    {
        MCU_ErrorTrap(mcu);
    }
}

static void MCU_Jump_BSR(mcu_t *mcu, uint8_t operand)
{
    uint16_t disp;
    if (operand == 0x0e)
    {
        disp = (uint16_t)(int8_t)MCU_ReadCodeAdvance(mcu);
    }
    else
    {
        disp = (uint16_t)(MCU_ReadCodeAdvance(mcu) << 8);
        disp |= MCU_ReadCodeAdvance(mcu);
    }
    MCU_PushStack(mcu, mcu->pc);
    mcu->pc += disp;
}

static void MCU_Jump_PJMP(mcu_t *mcu, uint8_t operand)
{
    uint8_t page;
    uint16_t address;
    (void)operand;
    page = MCU_ReadCodeAdvance(mcu);
    address = (uint16_t)(MCU_ReadCodeAdvance(mcu) << 8);
    address |= MCU_ReadCodeAdvance(mcu);
    mcu->cp = page;
    mcu->pc = address;
}

static uint32_t MCU_Operand_Read(mcu_t *mcu)
{
    switch (mcu->operand_type)
    {
    case GENERAL_DIRECT:
        switch (mcu->operand_size)
        {
        case OPERAND_WORD:
            return mcu->r[mcu->operand_reg];
        case OPERAND_BYTE:
            return mcu->r[mcu->operand_reg] & 0xff;
        }
        break;
    case GENERAL_INDIRECT:
    case GENERAL_ABSOLUTE:
        switch (mcu->operand_size)
        {
        case OPERAND_WORD:
            if (mcu->operand_ea & 1)
            {
                MCU_Interrupt_Exception(mcu, EXCEPTION_SOURCE_ADDRESS_ERROR);
            }
            return MCU_Read16(mcu, MCU_GetAddress(mcu->operand_ep, mcu->operand_ea));
        case OPERAND_BYTE:
            return MCU_Read(mcu, MCU_GetAddress(mcu->operand_ep, mcu->operand_ea));
        }
        break;
    case GENERAL_IMMEDIATE:
        return mcu->operand_data;
    }
    return 0;
}

static void MCU_Operand_Write(mcu_t *mcu, uint32_t data)
{
    switch (mcu->operand_type)
    {
    case GENERAL_DIRECT:
        switch (mcu->operand_size)
        {
        case OPERAND_WORD:
            mcu->r[mcu->operand_reg] = (uint16_t)data;
            break;
        case OPERAND_BYTE:
            mcu->r[mcu->operand_reg] &= ~0xff;
            mcu->r[mcu->operand_reg] |= data & 0xff;
            break;
        }
        break;
    case GENERAL_INDIRECT:
    case GENERAL_ABSOLUTE:
        switch (mcu->operand_size)
        {
        case OPERAND_WORD:
            if (mcu->operand_ea & 1)
            {
                MCU_Interrupt_Exception(mcu, EXCEPTION_SOURCE_ADDRESS_ERROR);
            }
            MCU_Write16(mcu, MCU_GetAddress(mcu->operand_ep, mcu->operand_ea), (uint16_t)data);
            break;
        case OPERAND_BYTE:
            MCU_Write(mcu, MCU_GetAddress(mcu->operand_ep, mcu->operand_ea), (uint8_t)data);
            break;
        }
        break;
    case GENERAL_IMMEDIATE:
        MCU_Interrupt_Exception(mcu, EXCEPTION_SOURCE_INVALID_INSTRUCTION);
        break;
    }
}

static void MCU_Operand_General(mcu_t *mcu, uint8_t operand)
{
    uint8_t reg;
    MCU_Operand_Size siz;
    uint16_t data;
    uint32_t addr;
    uint8_t addrpage;
    uint16_t ea;
    uint8_t ep;
    uint8_t opcode;
    uint8_t opcode_reg;
    uint32_t type = GENERAL_DIRECT;
    uint32_t disp = 0;
    uint32_t increase = INCREASE_NONE;
    uint32_t absolute = 0;
    (void)absolute; /* unused */
    reg = 0;
    siz = OPERAND_BYTE;
    data = 0;
    addr = 0;
    addrpage = 0;
    ea = 0;
    ep = 0;
    if (operand & 0x08)
        siz = OPERAND_WORD;
    else
        siz = OPERAND_BYTE;
    reg = operand & 0x07;
    switch (operand & 0xf0)
    {
    case 0xa0:
        type = GENERAL_DIRECT;
        break;
    case 0xd0:
        type = GENERAL_INDIRECT;
        break;
    case 0xe0:
        type = GENERAL_INDIRECT;
        disp = (uint32_t)(int8_t)MCU_ReadCodeAdvance(mcu);
        break;
    case 0xf0:
        type = GENERAL_INDIRECT;
        disp = MCU_ReadCodeAdvance(mcu);
        disp <<= 8;
        disp |= MCU_ReadCodeAdvance(mcu);
        break;
    case 0xb0:
        type = GENERAL_INDIRECT;
        increase = INCREASE_DECREASE;
        break;
    case 0xc0:
        type = GENERAL_INDIRECT;
        increase = INCREASE_INCREASE;
        break;
    case 0x00:
        if (reg == 5)
        {
            type = GENERAL_ABSOLUTE;
            addr = (uint32_t)mcu->br << 8;
            addr |= MCU_ReadCodeAdvance(mcu);
            addrpage = 0;
        }
        else if (reg == 4)
        {
            type = GENERAL_IMMEDIATE;
            data = MCU_ReadCodeAdvance(mcu);
            if (siz == OPERAND_WORD)
            {
                data <<= 8;
                data |= MCU_ReadCodeAdvance(mcu);
            }
        }
        break;
    case 0x10:
        if (reg == 5)
        {
            type = GENERAL_ABSOLUTE;
            addr = (uint32_t)MCU_ReadCodeAdvance(mcu) << 8;
            addr |= MCU_ReadCodeAdvance(mcu);
            addrpage = mcu->dp;
        }
        break;
    }
    if (type == GENERAL_INDIRECT)
    {
        if (increase == INCREASE_DECREASE)
        {
            if (siz == OPERAND_WORD || reg == 7)
            {
                mcu->r[reg] -= 2;
            }
            else
            {
                mcu->r[reg] -= 1;
            }
        }
        ea = (uint16_t)(mcu->r[reg] + disp);
        if (increase == INCREASE_INCREASE)
        {
            if (siz == OPERAND_WORD || reg == 7)
            {
                mcu->r[reg] += 2;
            }
            else
            {
                mcu->r[reg] += 1;
            }
        }

        ep = MCU_GetPageForRegister(mcu, reg);
    }
    else if (type == GENERAL_ABSOLUTE)
    {
        ea = (uint16_t)addr;

        ep = addrpage;
    }

    opcode = MCU_ReadCodeAdvance(mcu);
    mcu->opcode_extended = opcode == 0x00;
    if (mcu->opcode_extended)
    {
        opcode = MCU_ReadCodeAdvance(mcu);
    }
    opcode_reg = opcode & 0x07;
    opcode >>= 3;

    mcu->operand_type = type;
    mcu->operand_ea = ea;
    mcu->operand_ep = ep;
    mcu->operand_size = siz;
    mcu->operand_reg = reg;
    mcu->operand_data = data;
    mcu->operand_status = 0;

    MCU_Opcode_Table[opcode](mcu, opcode, opcode_reg);
}

static void MCU_SetStatusCommon(mcu_t *mcu, uint32_t val, MCU_Operand_Size siz)
{
    switch (siz)
    {
    case OPERAND_WORD:
        val &= 0xffff;
        MCU_SetStatus(mcu, val & 0x8000, STATUS_N);
        break;
    case OPERAND_BYTE:
        val &= 0xff;
        MCU_SetStatus(mcu, val & 0x80, STATUS_N);
        break;
    }
    MCU_SetStatus(mcu, val == 0, STATUS_Z);
    MCU_SetStatus(mcu, 0, STATUS_V);
}

static void MCU_Opcode_Short_MOVE(mcu_t *mcu, uint8_t opcode)
{
    uint32_t reg = opcode & 0x07;
    uint8_t data = MCU_ReadCodeAdvance(mcu);
    mcu->r[reg] &= ~0xff;
    mcu->r[reg] |= data;
    MCU_SetStatusCommon(mcu, data, OPERAND_BYTE);
}

static void MCU_Opcode_Short_MOVI(mcu_t *mcu, uint8_t opcode)
{
    uint32_t reg = opcode & 0x07;
    uint16_t data;
    data = (uint16_t)(MCU_ReadCodeAdvance(mcu) << 8);
    data |= MCU_ReadCodeAdvance(mcu);
    mcu->r[reg] = data;
    MCU_SetStatusCommon(mcu, data, OPERAND_WORD);
}

static void MCU_Opcode_Short_MOVF(mcu_t *mcu, uint8_t opcode)
{
    uint32_t reg = opcode & 0x07;
    uint32_t siz = (opcode & 0x08) != 0;
    int8_t disp = (int8_t)MCU_ReadCodeAdvance(mcu);
    uint32_t addr = (mcu->r[6] + disp) & 0xffff;
    addr |= (uint32_t)(mcu->tp << 16);
    if ((opcode & 0x10) == 0)
    {
        uint16_t data;
        if (siz)
        {
            data = MCU_Read16(mcu, addr);
            mcu->r[reg] &= ~0xff;
            mcu->r[reg] |= data;
            MCU_SetStatusCommon(mcu, data, OPERAND_BYTE);
        }
        else
        {
            data = MCU_Read(mcu, addr);
            mcu->r[reg] = data;
            MCU_SetStatusCommon(mcu, data, OPERAND_WORD);
        }
    }
    else
    {
        uint16_t data;
        if (siz)
        {
            data = mcu->r[reg] & 0xff;
            MCU_Write(mcu, addr, (uint8_t)data);
            MCU_SetStatusCommon(mcu, data, OPERAND_BYTE);
        }
        else
        {
            data = mcu->r[reg];
            MCU_Write16(mcu, addr, data);
            MCU_SetStatusCommon(mcu, data, OPERAND_WORD);
        }
    }
}

static void MCU_Opcode_Short_MOVL(mcu_t *mcu, uint8_t opcode)
{
    uint32_t reg = opcode & 0x07;
    uint32_t siz = (opcode & 0x08) != 0;
    uint16_t addr = (uint16_t)(mcu->br << 8);
    uint32_t data;
    addr |= MCU_ReadCodeAdvance(mcu);
    if (siz)
    {
        if (addr & 1)
            MCU_Interrupt_Exception(mcu, EXCEPTION_SOURCE_ADDRESS_ERROR);
        data = MCU_Read16(mcu, addr);
        mcu->r[reg] = (uint16_t)data;
        MCU_SetStatusCommon(mcu, data, OPERAND_WORD);
    }
    else
    {
        data = MCU_Read(mcu, addr);
        mcu->r[reg] &= ~0xff;
        mcu->r[reg] |= (uint16_t)data;
        MCU_SetStatusCommon(mcu, data, OPERAND_BYTE);
    }
}

static void MCU_Opcode_Short_MOVS(mcu_t *mcu, uint8_t opcode)
{
    uint32_t reg = opcode & 0x07;
    uint32_t siz = (opcode & 0x08) != 0;
    uint16_t addr = (uint16_t)(mcu->br << 8);
    uint16_t data;
    addr |= MCU_ReadCodeAdvance(mcu);
    if (siz)
    {
        if (addr & 1)
            MCU_Interrupt_Exception(mcu, EXCEPTION_SOURCE_ADDRESS_ERROR);
        data = mcu->r[reg];
        MCU_Write16(mcu, addr, data);
        MCU_SetStatusCommon(mcu, data, OPERAND_WORD);
    }
    else
    {
        data = mcu->r[reg] & 0xff;
        MCU_Write(mcu, addr, (uint8_t)data);
        MCU_SetStatusCommon(mcu, data, OPERAND_BYTE);
    }
}

static void MCU_Opcode_Short_CMP(mcu_t *mcu, uint8_t opcode)
{
    uint32_t reg = opcode & 0x07;
    const MCU_Operand_Size siz = (opcode & 0x08) ? OPERAND_WORD : OPERAND_BYTE;
    int32_t t1, t2;
    switch (siz)
    {
    case OPERAND_WORD:
        t2 = MCU_ReadCodeAdvance(mcu) << 8;
        t2 |= MCU_ReadCodeAdvance(mcu);
        break;
    case OPERAND_BYTE:
        t2 = MCU_ReadCodeAdvance(mcu);
        break;
    default:
        /* reason: initialized to one of the two values above */
        break;
    }
    t1 = mcu->r[reg];
    MCU_SUB_Common(mcu, t1, t2, 0, siz);
}

static void MCU_Opcode_NotImplemented(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg)
{
    (void)opcode;
    (void)opcode_reg;
    MCU_ErrorTrap(mcu);
}

static void MCU_Opcode_MOVG_Immediate(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg)
{
    uint32_t data;
    (void)opcode;
    if (opcode_reg == 6 && (mcu->operand_type == GENERAL_INDIRECT || mcu->operand_type == GENERAL_ABSOLUTE))
    {
        data = (uint32_t)(int8_t)MCU_ReadCodeAdvance(mcu);
        MCU_Operand_Write(mcu, data);
        MCU_SetStatusCommon(mcu, data, mcu->operand_size);
    }
    else if (opcode_reg == 7 && (mcu->operand_type == GENERAL_INDIRECT || mcu->operand_type == GENERAL_ABSOLUTE))
    {
        data = (uint32_t)MCU_ReadCodeAdvance(mcu) << 8;
        data |= MCU_ReadCodeAdvance(mcu);
        MCU_Operand_Write(mcu, data);
        MCU_SetStatusCommon(mcu, data, mcu->operand_size);
    }
    else if (opcode_reg == 4 && (mcu->operand_type == GENERAL_INDIRECT || mcu->operand_type == GENERAL_ABSOLUTE) &&
             mcu->operand_size == OPERAND_BYTE)
    {
        uint32_t t1 = MCU_Operand_Read(mcu);
        uint32_t t2 = MCU_ReadCodeAdvance(mcu);
        MCU_SUB_Common(mcu, (int32_t)t1, (int32_t)t2, 0, OPERAND_BYTE);
    }
    else if (opcode_reg == 4 && (mcu->operand_type == GENERAL_INDIRECT || mcu->operand_type == GENERAL_ABSOLUTE) &&
             mcu->operand_size == OPERAND_WORD) /* FIXME */
    {
        uint32_t t1 = MCU_Operand_Read(mcu);
        uint32_t t2 = (uint16_t)((int8_t)MCU_ReadCodeAdvance(mcu));
        MCU_SUB_Common(mcu, (int32_t)t1, (int32_t)t2, 0, OPERAND_WORD);
    }
    else if (opcode_reg == 5 && (mcu->operand_type == GENERAL_INDIRECT || mcu->operand_type == GENERAL_ABSOLUTE) &&
             mcu->operand_size == OPERAND_WORD)
    {
        uint32_t t1, t2;
        t1 = MCU_Operand_Read(mcu);
        t2 = (uint32_t)MCU_ReadCodeAdvance(mcu) << 8;
        t2 |= MCU_ReadCodeAdvance(mcu);
        MCU_SUB_Common(mcu, (int32_t)t1, (int32_t)t2, 0, OPERAND_WORD);
    }
    else if (opcode_reg == 5 && (mcu->operand_type == GENERAL_INDIRECT || mcu->operand_type == GENERAL_ABSOLUTE) &&
             mcu->operand_size == OPERAND_BYTE) /* FIXME */
    {
        uint32_t t1, t2;
        t1 = MCU_Operand_Read(mcu);
        t2 = (uint32_t)MCU_ReadCodeAdvance(mcu) << 8;
        t2 |= MCU_ReadCodeAdvance(mcu);
        MCU_SUB_Common(mcu, (int32_t)t1, (int32_t)t2, 0, OPERAND_BYTE);
    }
    else
    {
        MCU_ErrorTrap(mcu);
    }
}

static void MCU_Opcode_BSET_ORC(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg)
{
    (void)opcode;
    if (mcu->operand_type == GENERAL_IMMEDIATE) /* ORC */
    {
        uint32_t data = MCU_Operand_Read(mcu);
        uint32_t val = MCU_ControlRegisterRead(mcu, opcode_reg, mcu->operand_size);
        val |= data;
        MCU_ControlRegisterWrite(mcu, opcode_reg, mcu->operand_size, val);
        if (opcode_reg >= 2)
        {
            MCU_SetStatusCommon(mcu, val, mcu->operand_size);
        }
        mcu->ex_ignore = 1;
    }
    else /* BSET */
    {
        uint32_t data = MCU_Operand_Read(mcu);
        uint32_t bit = mcu->r[opcode_reg] & 0x0f;
        MCU_SetStatus(mcu, (data & (1 << bit)) == 0, STATUS_Z);
        data |= 1 << bit;
        MCU_Operand_Write(mcu, data);
    }
}

static void MCU_Opcode_BCLR_ANDC(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg)
{
    (void)opcode;
    if (mcu->operand_type == GENERAL_IMMEDIATE) /* ANDC */
    {
        uint32_t data = MCU_Operand_Read(mcu);
        uint32_t val = MCU_ControlRegisterRead(mcu, opcode_reg, mcu->operand_size);
        val &= data;
        MCU_ControlRegisterWrite(mcu, opcode_reg, mcu->operand_size, val);
        if (opcode_reg >= 2)
        {
            MCU_SetStatusCommon(mcu, val, mcu->operand_size);
        }
        mcu->ex_ignore = 1;
    }
    else /* BCLR */
    {
        uint32_t data = MCU_Operand_Read(mcu);
        uint32_t bit = mcu->r[opcode_reg] & 0x0f;
        MCU_SetStatus(mcu, (data & (1 << bit)) == 0, STATUS_Z);
        data &= (uint32_t)(~(1 << bit));
        MCU_Operand_Write(mcu, data);
    }
}

static void MCU_Opcode_BTST(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg)
{
    (void)opcode;
    if (mcu->operand_type != GENERAL_IMMEDIATE)
    {
        uint32_t data = MCU_Operand_Read(mcu);
        uint32_t bit = mcu->r[opcode_reg] & 0x0f;
        MCU_SetStatus(mcu, (data & (1 << bit)) == 0, STATUS_Z);
    }
    else
    {
        MCU_ErrorTrap(mcu);
    }
}

static void MCU_Opcode_CLR(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg)
{
    (void)opcode;
    if (opcode_reg == 3 && mcu->operand_type != GENERAL_IMMEDIATE) /* CLR */
    {
        MCU_Operand_Write(mcu, 0);
        MCU_SetStatus(mcu, 0, STATUS_N);
        MCU_SetStatus(mcu, 1, STATUS_Z);
        MCU_SetStatus(mcu, 0, STATUS_V);
        MCU_SetStatus(mcu, 0, STATUS_C);
    }
    else if (opcode_reg == 6 && mcu->operand_type != GENERAL_IMMEDIATE) /* TST */
    {
        uint32_t data = MCU_Operand_Read(mcu);
        MCU_SetStatusCommon(mcu, data, mcu->operand_size);
        MCU_SetStatus(mcu, 0, STATUS_C);
    }
    else if (opcode_reg == 2 && mcu->operand_type == GENERAL_DIRECT && mcu->operand_size == OPERAND_BYTE) /* EXTU */
    {
        uint16_t data = mcu->r[mcu->operand_reg] & 0xff;
        mcu->r[mcu->operand_reg] = data;
        MCU_SetStatus(mcu, 0, STATUS_N);
        MCU_SetStatus(mcu, data == 0, STATUS_Z);
        MCU_SetStatus(mcu, 0, STATUS_V);
        MCU_SetStatus(mcu, 0, STATUS_C);
    }
    else if (opcode_reg == 0 && mcu->operand_type == GENERAL_DIRECT && mcu->operand_size == OPERAND_BYTE) /* SWAP */
    {
        uint16_t data = mcu->r[mcu->operand_reg];
        uint8_t data_h = (uint8_t)(data >> 8);
        uint8_t data_l = (uint8_t)(data & 0xff);
        data = (uint16_t)((data_l << 8) | data_h);
        mcu->r[mcu->operand_reg] = data;
        MCU_SetStatusCommon(mcu, data, OPERAND_WORD);
    }
    else if (opcode_reg == 5 && mcu->operand_type != GENERAL_IMMEDIATE) /* NOT */
    {
        uint32_t data = MCU_Operand_Read(mcu);
        data = ~data;
        MCU_Operand_Write(mcu, data);
        MCU_SetStatusCommon(mcu, data, mcu->operand_size);
    }
    else if (opcode_reg == 4 && mcu->operand_type != GENERAL_IMMEDIATE) /* NEG */
    {
        uint32_t data = MCU_Operand_Read(mcu);
        data = (uint32_t)MCU_SUB_Common(mcu, 0, (int32_t)data, 0, mcu->operand_size);
        MCU_Operand_Write(mcu, data);
    }
    else if (opcode_reg == 1 && mcu->operand_type == GENERAL_DIRECT && mcu->operand_size == OPERAND_BYTE) /* EXTS */
    {
        uint32_t data = mcu->r[mcu->operand_reg];
        mcu->r[mcu->operand_reg] = (uint16_t)(int8_t)data;
        MCU_SetStatusCommon(mcu, data, OPERAND_WORD);
    }
    else
    {
        MCU_ErrorTrap(mcu);
    }
}

static void MCU_Opcode_LDC(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg)
{
    uint32_t data;
    (void)opcode;
    data = MCU_Operand_Read(mcu);
    MCU_ControlRegisterWrite(mcu, opcode_reg, mcu->operand_size, data);
    mcu->ex_ignore = 1;
}

static void MCU_Opcode_STC(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg)
{
    uint32_t data;
    (void)opcode;
    data = MCU_ControlRegisterRead(mcu, opcode_reg, mcu->operand_size);
    MCU_Operand_Write(mcu, data);
}

static void MCU_Opcode_BSET(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg)
{
    if (mcu->operand_type != GENERAL_IMMEDIATE)
    {
        uint32_t data = MCU_Operand_Read(mcu);
        uint32_t bit = (uint32_t)opcode_reg | (uint32_t)((opcode & 1) << 3);
        MCU_SetStatus(mcu, (data & (1 << bit)) == 0, STATUS_Z);
        data |= 1 << bit;
        MCU_Operand_Write(mcu, data);
    }
    else
    {
        MCU_ErrorTrap(mcu);
    }
}

static void MCU_Opcode_BCLR(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg)
{
    if (mcu->operand_type != GENERAL_IMMEDIATE)
    {
        uint32_t data = MCU_Operand_Read(mcu);
        uint32_t bit = (uint32_t)opcode_reg | (uint32_t)((opcode & 1) << 3);
        MCU_SetStatus(mcu, (data & (1 << bit)) == 0, STATUS_Z);
        data &= (uint32_t)(~(1 << bit));
        MCU_Operand_Write(mcu, data);
    }
    else
    {
        MCU_ErrorTrap(mcu);
    }
}

static void MCU_Opcode_MOVG(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg)
{
    if (mcu->opcode_extended)
    {
        if (opcode == 0x12)
        {
            /* FIXME */
            MCU_ErrorTrap(mcu);
        }
        else
        {
            MCU_ErrorTrap(mcu);
        }
    }
    else
    {
        uint8_t d = (opcode & 2) != 0;
        uint32_t data;
        if (d)
        {
            if (mcu->operand_type == GENERAL_DIRECT) /* XCH */
            {
                switch (mcu->operand_size)
                {
                case OPERAND_WORD: {
                    const uint16_t r1      = mcu->r[opcode_reg];
                    const uint16_t r2      = mcu->r[mcu->operand_reg];
                    mcu->r[opcode_reg]      = r2;
                    mcu->r[mcu->operand_reg] = r1;
                    break;
                }
                case OPERAND_BYTE:
                    MCU_ErrorTrap(mcu);
                    break;
                }
            }
            else
            {
                data = mcu->r[opcode_reg];
                MCU_Operand_Write(mcu, data);
                MCU_SetStatusCommon(mcu, data, mcu->operand_size);
            }
        }
        else
        {
            data = MCU_Operand_Read(mcu);
            switch (mcu->operand_size)
            {
            case OPERAND_WORD:
                mcu->r[opcode_reg] = (uint16_t)data;
                break;
            case OPERAND_BYTE:
                mcu->r[opcode_reg] &= ~0xff;
                mcu->r[opcode_reg] |= data & 0xff;
                break;
            }
            MCU_SetStatusCommon(mcu, data, mcu->operand_size);
        }
    }
}

static void MCU_Opcode_BTSTI(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg)
{
    if (mcu->operand_type != GENERAL_IMMEDIATE)
    {
        uint32_t data = MCU_Operand_Read(mcu);
        uint32_t bit = (uint32_t)opcode_reg | (uint32_t)((opcode & 1) << 3);
        MCU_SetStatus(mcu, (data & (1 << bit)) == 0, STATUS_Z);
    }
    else
    {
        MCU_ErrorTrap(mcu);
    }
}

static void MCU_Opcode_BNOTI(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg)
{
    if (mcu->operand_type != GENERAL_IMMEDIATE)
    {
        uint32_t data = MCU_Operand_Read(mcu);
        uint32_t bit = (uint32_t)opcode_reg | (uint32_t)((opcode & 1) << 3);
        MCU_SetStatus(mcu, (data & (1 << bit)) == 0, STATUS_Z);
        data ^= (1 << bit);
        MCU_Operand_Write(mcu, data);
    }
    else
    {
        MCU_ErrorTrap(mcu);
    }
}

static void MCU_Opcode_OR(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg)
{
    uint32_t data;
    (void)opcode;
    data = MCU_Operand_Read(mcu);
    mcu->r[opcode_reg] |= (uint16_t)data;
    MCU_SetStatusCommon(mcu, mcu->r[opcode_reg], mcu->operand_size);
}

static void MCU_Opcode_CMP(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg)
{
    int32_t t1;
    int32_t t2;
    (void)opcode;
    t1 = mcu->r[opcode_reg];
    t2 = (int32_t)MCU_Operand_Read(mcu);
    MCU_SUB_Common(mcu, t1, t2, 0, mcu->operand_size);
}

static void MCU_Opcode_ADDQ(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg)
{
    int32_t t1;
    int32_t t2;
    (void)opcode;
    t1 = (int32_t)MCU_Operand_Read(mcu);
    t2 = 0;
    switch (opcode_reg)
    {
    case 0:
        t2 = 1;
        break;
    case 1:
        t2 = 2;
        break;
    case 4:
        t2 = -1;
        break;
    case 5:
        t2 = -2;
        break;
    default:
        MCU_ErrorTrap(mcu);
        break;
    }
    t1 = MCU_ADD_Common(mcu, t1, t2, 0, mcu->operand_size);
    MCU_Operand_Write(mcu, (uint32_t)t1);
}

static void MCU_Opcode_ADD(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg)
{
    int32_t t1;
    int32_t t2;
    (void)opcode;
    t1 = mcu->r[opcode_reg];
    t2 = (int32_t)MCU_Operand_Read(mcu);
    t1 = MCU_ADD_Common(mcu, t1, t2, 0, mcu->operand_size);
    switch (mcu->operand_size)
    {
    case OPERAND_WORD:
        mcu->r[opcode_reg] = (uint16_t)t1;
        break;
    case OPERAND_BYTE:
        mcu->r[opcode_reg] &= ~0xff;
        mcu->r[opcode_reg] |= t1 & 0xff;
        break;
    }
}

static void MCU_Opcode_SUB(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg)
{
    int32_t t1;
    int32_t t2;
    (void)opcode;
    t1 = mcu->r[opcode_reg];
    t2 = (int32_t)MCU_Operand_Read(mcu);
    t1 = MCU_SUB_Common(mcu, t1, t2, 0, mcu->operand_size);
    switch (mcu->operand_size)
    {
    case OPERAND_WORD:
        mcu->r[opcode_reg] = (uint16_t)t1;
        break;
    case OPERAND_BYTE:
        mcu->r[opcode_reg] &= ~0xff;
        mcu->r[opcode_reg] |= t1 & 0xff;
        break;
    }
}

static void MCU_Opcode_SUBS(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg)
{
    int32_t t1;
    int32_t t2;
    (void)opcode;
    t1 = mcu->r[opcode_reg];
    t2 = (int32_t)MCU_Operand_Read(mcu);
    switch (mcu->operand_size)
    {
    case OPERAND_WORD:
        mcu->r[opcode_reg] = (uint16_t)(t1 - t2);
        break;
    case OPERAND_BYTE:
        mcu->r[opcode_reg] = (uint16_t)(t1 - (int8_t)t2);
        break;
    }
}

static void MCU_Opcode_AND(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg)
{
    uint32_t data;
    (void)opcode;
    data = mcu->r[opcode_reg];
    data &= MCU_Operand_Read(mcu);
    switch (mcu->operand_size)
    {
    case OPERAND_WORD:
        mcu->r[opcode_reg] = (uint16_t)data;
        break;
    case OPERAND_BYTE:
        mcu->r[opcode_reg] &= ~0xff;
        mcu->r[opcode_reg] |= data & 0xff;
        break;
    }
    MCU_SetStatusCommon(mcu, mcu->r[opcode_reg], mcu->operand_size);
}

static void MCU_Opcode_SHLR(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg)
{
    (void)opcode;
    if (opcode_reg == 0x03 && mcu->operand_type != GENERAL_IMMEDIATE) /* SHLR */
    {
        uint32_t data = MCU_Operand_Read(mcu);
        const int C = data & 1;
        data >>= 1;
        MCU_Operand_Write(mcu, data);
        MCU_SetStatus(mcu, C, STATUS_C);
        MCU_SetStatusCommon(mcu, data, mcu->operand_size);
    }
    else if (opcode_reg == 0x02 && mcu->operand_type != GENERAL_IMMEDIATE) /* SHLL */
    {
        uint32_t data = MCU_Operand_Read(mcu);
        int C = 0;
        switch (mcu->operand_size)
        {
        case OPERAND_WORD:
            C = (data & 0x8000) != 0;
            break;
        case OPERAND_BYTE:
            C = (data & 0x80) != 0;
            break;
        default:
            /* reason: operand_size set to one of these values in decoder */
            break;
        }
        data <<= 1;
        MCU_Operand_Write(mcu, data);
        MCU_SetStatus(mcu, C, STATUS_C);
        MCU_SetStatusCommon(mcu, data, mcu->operand_size);
    }
    else if (opcode_reg == 0x06 && mcu->operand_type != GENERAL_IMMEDIATE) /* ROTXL */
    {
        uint32_t data = MCU_Operand_Read(mcu);
        uint32_t bit = (mcu->sr & STATUS_C) != 0;
        int C = 0;
        switch (mcu->operand_size)
        {
        case OPERAND_WORD:
            C = (data & 0x8000) != 0;
            break;
        case OPERAND_BYTE:
            C = (data & 0x80) != 0;
            break;
        default:
            /* reason: operand_size set to valid value in decoder */
            break;
        }
        data <<= 1;
        data |= bit;
        MCU_Operand_Write(mcu, data);
        MCU_SetStatus(mcu, C, STATUS_C);
        MCU_SetStatusCommon(mcu, data, mcu->operand_size);
    }
    else if (opcode_reg == 0x04 && mcu->operand_type != GENERAL_IMMEDIATE) /* ROTL */
    {
        uint32_t data = MCU_Operand_Read(mcu);
        int C = 0;
        switch (mcu->operand_size)
        {
        case OPERAND_WORD:
            C = (data & 0x8000) != 0;
            break;
        case OPERAND_BYTE:
            C = (data & 0x80) != 0;
            break;
        default:
            /* reason: operand_size set to valid value in decoder */
            break;
        }
        data <<= 1;
        data |= (uint32_t)C;
        MCU_Operand_Write(mcu, data);
        MCU_SetStatus(mcu, C, STATUS_C);
        MCU_SetStatusCommon(mcu, data, mcu->operand_size);
    }
    else if (opcode_reg == 0x00 && mcu->operand_type != GENERAL_IMMEDIATE) /* SHAL */
    {
        uint32_t data = MCU_Operand_Read(mcu);
        int C = 0;
        switch (mcu->operand_size)
        {
        case OPERAND_WORD:
            C = (data & 0x8000) != 0;
            break;
        case OPERAND_BYTE:
            C = (data & 0x80) != 0;
            break;
        default:
            /* reason: operand_size set to valid value in decoder */
            break;
        }
        data <<= 1;
        MCU_Operand_Write(mcu, data);
        MCU_SetStatus(mcu, C, STATUS_C);
        MCU_SetStatusCommon(mcu, data, mcu->operand_size);
    }
    else if (opcode_reg == 0x01 && mcu->operand_type != GENERAL_IMMEDIATE) /* SHAR */
    {
        uint32_t data = MCU_Operand_Read(mcu);
        const int C = data & 0x1;
        uint32_t msb = 0;
        switch (mcu->operand_size)
        {
        case OPERAND_WORD:
            msb = data & 0x8000;
            data &= 0xffff;
            break;
        case OPERAND_BYTE:
            msb = data & 0x80;
            data &= 0xff;
            break;
        default:
            /* reason: operand_size always set to valid value in decoder */
            break;
        }
        data >>= 1;
        data |= msb;
        MCU_Operand_Write(mcu, data);
        MCU_SetStatus(mcu, C, STATUS_C);
        MCU_SetStatusCommon(mcu, data, mcu->operand_size);
    }
    else if (opcode_reg == 0x05 && mcu->operand_type != GENERAL_IMMEDIATE) /* ROTR */
    {
        uint32_t data = MCU_Operand_Read(mcu);
        const int C = (data & 0x1) != 0;
        data >>= 1;
        switch (mcu->operand_size)
        {
        case OPERAND_WORD:
            data |= (uint32_t)C << 15;
            break;
        case OPERAND_BYTE:
            data |= (uint32_t)C << 7;
            break;
        }
        MCU_Operand_Write(mcu, data);
        MCU_SetStatus(mcu, C, STATUS_C);
        MCU_SetStatusCommon(mcu, data, mcu->operand_size);
    }
    else
    {
        MCU_ErrorTrap(mcu);
    }
}

static void MCU_Opcode_MULXU(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg)
{
    uint32_t t1;
    uint32_t t2;
    int N = 0;
    int Z;
    (void)opcode;
    t1 = MCU_Operand_Read(mcu);
    t2 = mcu->r[opcode_reg];
    switch (mcu->operand_size)
    {
    case OPERAND_BYTE:
        t2 &= 0xff;
        break;
    case OPERAND_WORD:
        /* explicitly do nothing */
        break;
    }
    t1 *= t2;

    switch (mcu->operand_size)
    {
    case OPERAND_WORD:
        opcode_reg &= ~1;
        mcu->r[opcode_reg | 0] = (uint16_t)(t1 >> 16);
        mcu->r[opcode_reg | 1] = (uint16_t)t1;
        N = (t1 & 0x80000000UL) != 0; /* FIXME */
        break;
    case OPERAND_BYTE:
        t1 &= 0xffff;
        mcu->r[opcode_reg] = (uint16_t)t1;
        N = (t1 & 0x8000UL) != 0; /* FIXME */
        break;
    default:
        /* reason: operand_size always set to a valid value in decoder */
        break;
    }
    Z = t1 == 0;
    MCU_SetStatus(mcu, N, STATUS_N);
    MCU_SetStatus(mcu, Z, STATUS_Z);
    MCU_SetStatus(mcu, 0, STATUS_V);
    MCU_SetStatus(mcu, 0, STATUS_C);
}

static void MCU_Opcode_DIVXU(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg)
{
    uint32_t t1;
    uint32_t t2;
    uint32_t R;
    uint32_t Q;
    (void)opcode;
    t1 = MCU_Operand_Read(mcu);

    if (!t1)
    {
        MCU_ErrorTrap(mcu); /* FIXME: implement proper exception */
        MCU_SetStatus(mcu, 0, STATUS_N);
        MCU_SetStatus(mcu, 1, STATUS_Z);
        MCU_SetStatus(mcu, 0, STATUS_V);
        MCU_SetStatus(mcu, 0, STATUS_C);
        return;
    }

    switch (mcu->operand_size)
    {
    case OPERAND_WORD:
        opcode_reg &= ~1;
        t2 = (uint32_t)mcu->r[opcode_reg | 0] << 16;
        t2 |= mcu->r[opcode_reg | 1];

        R = t2 % t1;
        Q = t2 / t1;

        if (Q > UINT16_MAX)
        {
            MCU_SetStatus(mcu, 0, STATUS_N);
            MCU_SetStatus(mcu, 0, STATUS_Z);
            MCU_SetStatus(mcu, 1, STATUS_V);
            MCU_SetStatus(mcu, 0, STATUS_C);
        }
        else
        {
            mcu->r[opcode_reg | 0] = (uint16_t)R;
            mcu->r[opcode_reg | 1] = (uint16_t)Q;
            MCU_SetStatusCommon(mcu, Q, OPERAND_WORD);
            MCU_SetStatus(mcu, 0, STATUS_C);
        }
        break;
    case OPERAND_BYTE:
        t2 = mcu->r[opcode_reg];

        R = t2 % t1;
        Q = t2 / t1;

        if (Q > UINT8_MAX)
        {
            MCU_SetStatus(mcu, 0, STATUS_N);
            MCU_SetStatus(mcu, 0, STATUS_Z);
            MCU_SetStatus(mcu, 1, STATUS_V);
            MCU_SetStatus(mcu, 0, STATUS_C);
        }
        else
        {
            R &= 0xff;
            Q &= 0xff;
            mcu->r[opcode_reg] = (uint16_t)((R << 8) | Q);
            MCU_SetStatusCommon(mcu, Q, OPERAND_BYTE);
            MCU_SetStatus(mcu, 0, STATUS_C);
        }
        break;
    }
}

static void MCU_Opcode_ADDS(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg)
{
    uint32_t data;
    (void)opcode;
    data = MCU_Operand_Read(mcu);
    switch (mcu->operand_size)
    {
    case OPERAND_BYTE:
        data = (uint32_t)(int8_t)data;
        break;
    case OPERAND_WORD:
        /* explicitly do nothing */
        break;
    }
    mcu->r[opcode_reg] = (uint16_t)(mcu->r[opcode_reg] + data);
}

static void MCU_Opcode_XOR(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg)
{
    uint32_t data;
    (void)opcode;
    data = MCU_Operand_Read(mcu);
    mcu->r[opcode_reg] ^= (uint16_t)data;
    MCU_SetStatusCommon(mcu, mcu->r[opcode_reg], mcu->operand_size);
}

static void MCU_Opcode_ADDX(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg)
{
    int32_t t1;
    int32_t t2;
    int C;
    int Z;
    (void)opcode;
    t1 = mcu->r[opcode_reg];
    t2 = (int32_t)MCU_Operand_Read(mcu);
    C = (mcu->sr & STATUS_C) != 0;
    Z = (mcu->sr & STATUS_Z) != 0;
    t1 = MCU_ADD_Common(mcu, t1, t2, C, mcu->operand_size);
    if (!Z)
        MCU_SetStatus(mcu, 0, STATUS_Z);
    switch (mcu->operand_size)
    {
    case OPERAND_WORD:
        mcu->r[opcode_reg] = (uint16_t)t1;
        break;
    case OPERAND_BYTE:
        mcu->r[opcode_reg] &= ~0xff;
        mcu->r[opcode_reg] |= t1 & 0xff;
        break;
    }
}

static void MCU_Opcode_SUBX(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg)
{
    int32_t t1;
    int32_t t2;
    int C;
    (void)opcode;
    t1 = mcu->r[opcode_reg];
    t2 = (int32_t)MCU_Operand_Read(mcu);
    C = (mcu->sr & STATUS_C) != 0;
    t1 = MCU_SUB_Common(mcu, t1, t2, C, mcu->operand_size);
    switch (mcu->operand_size)
    {
    case OPERAND_WORD:
        mcu->r[opcode_reg] = (uint16_t)t1;
        break;
    case OPERAND_BYTE:
        mcu->r[opcode_reg] &= ~0xff;
        mcu->r[opcode_reg] |= t1 & 0xff;
        break;
    }
}

static void (*MCU_Operand_Table[256])(mcu_t *mcu, uint8_t operand) = {
    MCU_Operand_Nop, /* 00 */
    MCU_Jump_JMP, /* 01 */
    MCU_LDM, /* 02 */
    MCU_Jump_PJSR, /* 03 */
    MCU_Operand_General, /* 04 */
    MCU_Operand_General, /* 05 */
    MCU_Jump_JMP, /* 06 */
    MCU_Jump_JMP, /* 07 */
    MCU_TRAPA, /* 08 */
    MCU_Operand_NotImplemented, /* 09 */
    MCU_Jump_RTE, /* 0A */
    MCU_Operand_NotImplemented, /* 0B */
    MCU_Operand_General, /* 0C */
    MCU_Operand_General, /* 0D */
    MCU_Jump_BSR, /* 0E */
    MCU_Operand_NotImplemented, /* 0F */
    MCU_Jump_JMP, /* 10 */
    MCU_Jump_JMP, /* 11 */
    MCU_STM, /* 12 */
    MCU_Jump_PJMP, /* 13 */
    MCU_Jump_RTD, /* 14 */
    MCU_Operand_General, /* 15 */
    MCU_Operand_NotImplemented, /* 16 */
    MCU_Operand_NotImplemented, /* 17 */
    MCU_Jump_JSR, /* 18 */
    MCU_Jump_RTS, /* 19 */
    MCU_Operand_Sleep, /* 1A */
    MCU_Operand_NotImplemented, /* 1B */
    MCU_Jump_RTD, /* 1C */
    MCU_Operand_General, /* 1D */
    MCU_Jump_BSR, /* 1E */
    MCU_Operand_NotImplemented, /* 1F */
    MCU_Jump_Bcc, /* 20 */
    MCU_Jump_Bcc, /* 21 */
    MCU_Jump_Bcc, /* 22 */
    MCU_Jump_Bcc, /* 23 */
    MCU_Jump_Bcc, /* 24 */
    MCU_Jump_Bcc, /* 25 */
    MCU_Jump_Bcc, /* 26 */
    MCU_Jump_Bcc, /* 27 */
    MCU_Jump_Bcc, /* 28 */
    MCU_Jump_Bcc, /* 29 */
    MCU_Jump_Bcc, /* 2A */
    MCU_Jump_Bcc, /* 2B */
    MCU_Jump_Bcc, /* 2C */
    MCU_Jump_Bcc, /* 2D */
    MCU_Jump_Bcc, /* 2E */
    MCU_Jump_Bcc, /* 2F */
    MCU_Jump_Bcc, /* 30 */
    MCU_Jump_Bcc, /* 31 */
    MCU_Jump_Bcc, /* 32 */
    MCU_Jump_Bcc, /* 33 */
    MCU_Jump_Bcc, /* 34 */
    MCU_Jump_Bcc, /* 35 */
    MCU_Jump_Bcc, /* 36 */
    MCU_Jump_Bcc, /* 37 */
    MCU_Jump_Bcc, /* 38 */
    MCU_Jump_Bcc, /* 39 */
    MCU_Jump_Bcc, /* 3A */
    MCU_Jump_Bcc, /* 3B */
    MCU_Jump_Bcc, /* 3C */
    MCU_Jump_Bcc, /* 3D */
    MCU_Jump_Bcc, /* 3E */
    MCU_Jump_Bcc, /* 3F */
    MCU_Opcode_Short_CMP, /* 40 */
    MCU_Opcode_Short_CMP, /* 41 */
    MCU_Opcode_Short_CMP, /* 42 */
    MCU_Opcode_Short_CMP, /* 43 */
    MCU_Opcode_Short_CMP, /* 44 */
    MCU_Opcode_Short_CMP, /* 45 */
    MCU_Opcode_Short_CMP, /* 46 */
    MCU_Opcode_Short_CMP, /* 47 */
    MCU_Opcode_Short_CMP, /* 48 */
    MCU_Opcode_Short_CMP, /* 49 */
    MCU_Opcode_Short_CMP, /* 4A */
    MCU_Opcode_Short_CMP, /* 4B */
    MCU_Opcode_Short_CMP, /* 4C */
    MCU_Opcode_Short_CMP, /* 4D */
    MCU_Opcode_Short_CMP, /* 4E */
    MCU_Opcode_Short_CMP, /* 4F */
    MCU_Opcode_Short_MOVE, /* 50 */
    MCU_Opcode_Short_MOVE, /* 51 */
    MCU_Opcode_Short_MOVE, /* 52 */
    MCU_Opcode_Short_MOVE, /* 53 */
    MCU_Opcode_Short_MOVE, /* 54 */
    MCU_Opcode_Short_MOVE, /* 55 */
    MCU_Opcode_Short_MOVE, /* 56 */
    MCU_Opcode_Short_MOVE, /* 57 */
    MCU_Opcode_Short_MOVI, /* 58 */
    MCU_Opcode_Short_MOVI, /* 59 */
    MCU_Opcode_Short_MOVI, /* 5A */
    MCU_Opcode_Short_MOVI, /* 5B */
    MCU_Opcode_Short_MOVI, /* 5C */
    MCU_Opcode_Short_MOVI, /* 5D */
    MCU_Opcode_Short_MOVI, /* 5E */
    MCU_Opcode_Short_MOVI, /* 5F */
    MCU_Opcode_Short_MOVL, /* 60 */
    MCU_Opcode_Short_MOVL, /* 61 */
    MCU_Opcode_Short_MOVL, /* 62 */
    MCU_Opcode_Short_MOVL, /* 63 */
    MCU_Opcode_Short_MOVL, /* 64 */
    MCU_Opcode_Short_MOVL, /* 65 */
    MCU_Opcode_Short_MOVL, /* 66 */
    MCU_Opcode_Short_MOVL, /* 67 */
    MCU_Opcode_Short_MOVL, /* 68 */
    MCU_Opcode_Short_MOVL, /* 69 */
    MCU_Opcode_Short_MOVL, /* 6A */
    MCU_Opcode_Short_MOVL, /* 6B */
    MCU_Opcode_Short_MOVL, /* 6C */
    MCU_Opcode_Short_MOVL, /* 6D */
    MCU_Opcode_Short_MOVL, /* 6E */
    MCU_Opcode_Short_MOVL, /* 6F */
    MCU_Opcode_Short_MOVS, /* 70 */
    MCU_Opcode_Short_MOVS, /* 71 */
    MCU_Opcode_Short_MOVS, /* 72 */
    MCU_Opcode_Short_MOVS, /* 73 */
    MCU_Opcode_Short_MOVS, /* 74 */
    MCU_Opcode_Short_MOVS, /* 75 */
    MCU_Opcode_Short_MOVS, /* 76 */
    MCU_Opcode_Short_MOVS, /* 77 */
    MCU_Opcode_Short_MOVS, /* 78 */
    MCU_Opcode_Short_MOVS, /* 79 */
    MCU_Opcode_Short_MOVS, /* 7A */
    MCU_Opcode_Short_MOVS, /* 7B */
    MCU_Opcode_Short_MOVS, /* 7C */
    MCU_Opcode_Short_MOVS, /* 7D */
    MCU_Opcode_Short_MOVS, /* 7E */
    MCU_Opcode_Short_MOVS, /* 7F */
    MCU_Opcode_Short_MOVF, /* 80 */
    MCU_Opcode_Short_MOVF, /* 81 */
    MCU_Opcode_Short_MOVF, /* 82 */
    MCU_Opcode_Short_MOVF, /* 83 */
    MCU_Opcode_Short_MOVF, /* 84 */
    MCU_Opcode_Short_MOVF, /* 85 */
    MCU_Opcode_Short_MOVF, /* 86 */
    MCU_Opcode_Short_MOVF, /* 87 */
    MCU_Opcode_Short_MOVF, /* 88 */
    MCU_Opcode_Short_MOVF, /* 89 */
    MCU_Opcode_Short_MOVF, /* 8A */
    MCU_Opcode_Short_MOVF, /* 8B */
    MCU_Opcode_Short_MOVF, /* 8C */
    MCU_Opcode_Short_MOVF, /* 8D */
    MCU_Opcode_Short_MOVF, /* 8E */
    MCU_Opcode_Short_MOVF, /* 8F */
    MCU_Opcode_Short_MOVF, /* 90 */
    MCU_Opcode_Short_MOVF, /* 91 */
    MCU_Opcode_Short_MOVF, /* 92 */
    MCU_Opcode_Short_MOVF, /* 93 */
    MCU_Opcode_Short_MOVF, /* 94 */
    MCU_Opcode_Short_MOVF, /* 95 */
    MCU_Opcode_Short_MOVF, /* 96 */
    MCU_Opcode_Short_MOVF, /* 97 */
    MCU_Opcode_Short_MOVF, /* 98 */
    MCU_Opcode_Short_MOVF, /* 99 */
    MCU_Opcode_Short_MOVF, /* 9A */
    MCU_Opcode_Short_MOVF, /* 9B */
    MCU_Opcode_Short_MOVF, /* 9C */
    MCU_Opcode_Short_MOVF, /* 9D */
    MCU_Opcode_Short_MOVF, /* 9E */
    MCU_Opcode_Short_MOVF, /* 9F */
    MCU_Operand_General, /* A0 */
    MCU_Operand_General, /* A1 */
    MCU_Operand_General, /* A2 */
    MCU_Operand_General, /* A3 */
    MCU_Operand_General, /* A4 */
    MCU_Operand_General, /* A5 */
    MCU_Operand_General, /* A6 */
    MCU_Operand_General, /* A7 */
    MCU_Operand_General, /* A8 */
    MCU_Operand_General, /* A9 */
    MCU_Operand_General, /* AA */
    MCU_Operand_General, /* AB */
    MCU_Operand_General, /* AC */
    MCU_Operand_General, /* AD */
    MCU_Operand_General, /* AE */
    MCU_Operand_General, /* AF */
    MCU_Operand_General, /* B0 */
    MCU_Operand_General, /* B1 */
    MCU_Operand_General, /* B2 */
    MCU_Operand_General, /* B3 */
    MCU_Operand_General, /* B4 */
    MCU_Operand_General, /* B5 */
    MCU_Operand_General, /* B6 */
    MCU_Operand_General, /* B7 */
    MCU_Operand_General, /* B8 */
    MCU_Operand_General, /* B9 */
    MCU_Operand_General, /* BA */
    MCU_Operand_General, /* BB */
    MCU_Operand_General, /* BC */
    MCU_Operand_General, /* BD */
    MCU_Operand_General, /* BE */
    MCU_Operand_General, /* BF */
    MCU_Operand_General, /* C0 */
    MCU_Operand_General, /* C1 */
    MCU_Operand_General, /* C2 */
    MCU_Operand_General, /* C3 */
    MCU_Operand_General, /* C4 */
    MCU_Operand_General, /* C5 */
    MCU_Operand_General, /* C6 */
    MCU_Operand_General, /* C7 */
    MCU_Operand_General, /* C8 */
    MCU_Operand_General, /* C9 */
    MCU_Operand_General, /* CA */
    MCU_Operand_General, /* CB */
    MCU_Operand_General, /* CC */
    MCU_Operand_General, /* CD */
    MCU_Operand_General, /* CE */
    MCU_Operand_General, /* CF */
    MCU_Operand_General, /* D0 */
    MCU_Operand_General, /* D1 */
    MCU_Operand_General, /* D2 */
    MCU_Operand_General, /* D3 */
    MCU_Operand_General, /* D4 */
    MCU_Operand_General, /* D5 */
    MCU_Operand_General, /* D6 */
    MCU_Operand_General, /* D7 */
    MCU_Operand_General, /* D8 */
    MCU_Operand_General, /* D9 */
    MCU_Operand_General, /* DA */
    MCU_Operand_General, /* DB */
    MCU_Operand_General, /* DC */
    MCU_Operand_General, /* DD */
    MCU_Operand_General, /* DE */
    MCU_Operand_General, /* DF */
    MCU_Operand_General, /* E0 */
    MCU_Operand_General, /* E1 */
    MCU_Operand_General, /* E2 */
    MCU_Operand_General, /* E3 */
    MCU_Operand_General, /* E4 */
    MCU_Operand_General, /* E5 */
    MCU_Operand_General, /* E6 */
    MCU_Operand_General, /* E7 */
    MCU_Operand_General, /* E8 */
    MCU_Operand_General, /* E9 */
    MCU_Operand_General, /* EA */
    MCU_Operand_General, /* EB */
    MCU_Operand_General, /* EC */
    MCU_Operand_General, /* ED */
    MCU_Operand_General, /* EE */
    MCU_Operand_General, /* EF */
    MCU_Operand_General, /* F0 */
    MCU_Operand_General, /* F1 */
    MCU_Operand_General, /* F2 */
    MCU_Operand_General, /* F3 */
    MCU_Operand_General, /* F4 */
    MCU_Operand_General, /* F5 */
    MCU_Operand_General, /* F6 */
    MCU_Operand_General, /* F7 */
    MCU_Operand_General, /* F8 */
    MCU_Operand_General, /* F9 */
    MCU_Operand_General, /* FA */
    MCU_Operand_General, /* FB */
    MCU_Operand_General, /* FC */
    MCU_Operand_General, /* FD */
    MCU_Operand_General, /* FE */
    MCU_Operand_General, /* FF */
};

static void (*MCU_Opcode_Table[32])(mcu_t *mcu, uint8_t opcode, uint8_t opcode_reg) = {
    MCU_Opcode_MOVG_Immediate, /* 00 */
    MCU_Opcode_ADDQ, /* 01 */
    MCU_Opcode_CLR, /* 02 */
    MCU_Opcode_SHLR, /* 03 */
    MCU_Opcode_ADD, /* 04 */
    MCU_Opcode_ADDS, /* 05 */
    MCU_Opcode_SUB, /* 06 */
    MCU_Opcode_SUBS, /* 07 */
    MCU_Opcode_OR, /* 08 */
    MCU_Opcode_BSET_ORC, /* 09 */
    MCU_Opcode_AND, /* 0A */
    MCU_Opcode_BCLR_ANDC, /* 0B */
    MCU_Opcode_XOR, /* 0C */
    MCU_Opcode_NotImplemented, /* 0D */
    MCU_Opcode_CMP, /* 0E */
    MCU_Opcode_BTST, /* 0F */
    MCU_Opcode_MOVG, /* 10 */
    MCU_Opcode_LDC, /* 11 */
    MCU_Opcode_MOVG, /* 12 */
    MCU_Opcode_STC, /* 13 */
    MCU_Opcode_ADDX, /* 14 */
    MCU_Opcode_MULXU, /* 15 */
    MCU_Opcode_SUBX, /* 16 */
    MCU_Opcode_DIVXU, /* 17 */
    MCU_Opcode_BSET, /* 18 */
    MCU_Opcode_BSET, /* 19 */
    MCU_Opcode_BCLR, /* 1A */
    MCU_Opcode_BCLR, /* 1B */
    MCU_Opcode_BNOTI, /* 1C */
    MCU_Opcode_BNOTI, /* 1D */
    MCU_Opcode_BTSTI, /* 1E */
    MCU_Opcode_BTSTI, /* 1F */
};

static void MCU_ErrorTrap(mcu_t *mcu)
{
    (void)mcu;
}

static uint8_t RCU_Read(void)
{
    return 0;
}

enum {
    ANALOG_LEVEL_RCU_LOW = 0,
    ANALOG_LEVEL_RCU_HIGH = 0,
    ANALOG_LEVEL_SW_0 = 0,
    ANALOG_LEVEL_SW_1 = 0x155,
    ANALOG_LEVEL_SW_2 = 0x2aa,
    ANALOG_LEVEL_SW_3 = 0x3ff,
    ANALOG_LEVEL_BATTERY = 0x2a0
};

static uint16_t MCU_SC155Sliders(mcu_t *mcu, uint32_t index)
{
    (void)mcu;
    (void)index;
    /* 0 - 1/9 */
    /* 1 - 2/10 */
    /* 2 - 3/11 */
    /* 3 - 4/12 */
    /* 4 - 5/13 */
    /* 5 - 6/14 */
    /* 6 - 7/15 */
    /* 7 - 8/16 */
    /* 8 - ALL */
    return 0x0;
}

static uint16_t MCU_AnalogReadPin(mcu_t *mcu, uint32_t pin)
{
    if (mcu->is_cm300)
        return 0;
    if (mcu->is_jv880)
    {
        if (pin == 1)
            return ANALOG_LEVEL_BATTERY;
        return 0x3ff;
    }
    if (0)
    {
        uint8_t rcu;
READ_RCU:
        rcu = RCU_Read();
        if (rcu & (1 << pin))
            return ANALOG_LEVEL_RCU_HIGH;
        else
            return ANALOG_LEVEL_RCU_LOW;
    }
    if (mcu->is_mk1)
    {
        if (mcu->is_sc155 && (mcu->dev_register[DEV_P9DR] & 1) != 0)
        {
            return MCU_SC155Sliders(mcu, pin);
        }
        if (pin == 7)
        {
            if (mcu->is_sc155 && (mcu->dev_register[DEV_P9DR] & 2) != 0)
                return MCU_SC155Sliders(mcu, 8);
            else
                return ANALOG_LEVEL_BATTERY;
        }
        else
            goto READ_RCU;
    }
    else
    {
        if (mcu->is_sc155 && (mcu->io_sd & 16) != 0)
        {
            return MCU_SC155Sliders(mcu, pin);
        }
        if (pin == 7)
        {
            if (mcu->is_mk1)
                return ANALOG_LEVEL_BATTERY;
            switch ((mcu->io_sd >> 2) & 3)
            {
            case 0: /* Battery voltage */
                return ANALOG_LEVEL_BATTERY;
            case 1: /* NC */
                if (mcu->is_sc155)
                    return MCU_SC155Sliders(mcu, 8);
                return 0;
            case 2: /* SW */
                switch (mcu->sw_pos)
                {
                case 0:
                default:
                    return ANALOG_LEVEL_SW_0;
                case 1:
                    return ANALOG_LEVEL_SW_1;
                case 2:
                    return ANALOG_LEVEL_SW_2;
                case 3:
                    return ANALOG_LEVEL_SW_3;
                }
            case 3: /* RCU */
                goto READ_RCU;
            }
        }
        else
            goto READ_RCU;
    }
    /* TODO: really unreachable? maybe addressed upstream later? */
    return 0;
}

static void MCU_AnalogSample(mcu_t *mcu, uint8_t channel)
{
    uint16_t value = MCU_AnalogReadPin(mcu, channel);
    uint16_t dest = (channel << 1) & 6;
    mcu->dev_register[DEV_ADDRAH + dest] = (uint8_t)(value >> 2);
    mcu->dev_register[DEV_ADDRAL + dest] = (uint8_t)((value << 6) & 0xc0);
}

static void MCU_DeviceWrite(mcu_t *mcu, uint32_t address, uint8_t data)
{
    address &= 0x7f;
    if (address >= 0x10 && address < 0x40)
    {
        TIMER_WriteFRT(mcu->timer, address, data);
        return;
    }
    if (address >= 0x50 && address < 0x55)
    {
        TIMER_WriteTMR(mcu->timer, address, data);
        return;
    }
    switch (address)
    {
    case DEV_P1DDR: /* P1DDR */
        break;
    case DEV_P5DDR:
        break;
    case DEV_P6DDR:
        break;
    case DEV_P7DDR:
        break;
    case DEV_SCR:
        break;
    case DEV_WCR:
        break;
    case DEV_P9DDR:
        break;
    case DEV_RAMCR:
        break;
    case DEV_P1CR: /* P1CR */
        break;
    case DEV_DTEA:
        break;
    case DEV_DTEB:
        break;
    case DEV_DTEC:
        break;
    case DEV_DTED:
        break;
    case DEV_SMR:
        break;
    case DEV_BRR:
        break;
    case DEV_IPRA:
        break;
    case DEV_IPRB:
        break;
    case DEV_IPRC:
        break;
    case DEV_IPRD:
        break;
    case DEV_PWM1_DTR:
        break;
    case DEV_PWM1_TCR:
        break;
    case DEV_PWM2_DTR:
        break;
    case DEV_PWM2_TCR:
        break;
    case DEV_PWM3_DTR:
        break;
    case DEV_PWM3_TCR:
        break;
    case DEV_P7DR:
        break;
    case DEV_TMR_TCNT:
        break;
    case DEV_TMR_TCR:
        break;
    case DEV_TMR_TCSR:
        break;
    case DEV_TMR_TCORA:
        break;
    case DEV_TDR:
        break;
    case DEV_ADCSR:
    {
        mcu->dev_register[address] &= ~0x7f;
        mcu->dev_register[address] |= data & 0x7f;
        if ((data & 0x80) == 0 && mcu->adf_rd)
        {
            mcu->dev_register[address] &= ~0x80;
            MCU_Interrupt_SetRequest(mcu, INTERRUPT_SOURCE_ANALOG, 0);
        }
        if ((data & 0x40) == 0)
            MCU_Interrupt_SetRequest(mcu, INTERRUPT_SOURCE_ANALOG, 0);
        return;
    }
    case DEV_SSR:
    {
        if ((data & 0x80) == 0 && (mcu->ssr_rd & 0x80) != 0)
        {
            mcu->dev_register[address] &= ~0x80;
            mcu->uart_tx_delay = mcu->cycles + 3000;
            MCU_Interrupt_SetRequest(mcu, INTERRUPT_SOURCE_UART_TX, 0);
        }
        if ((data & 0x40) == 0 && (mcu->ssr_rd & 0x40) != 0)
        {
            mcu->uart_rx_delay = mcu->cycles + 3000;
            mcu->dev_register[address] &= ~0x40;
            MCU_Interrupt_SetRequest(mcu, INTERRUPT_SOURCE_UART_RX, 0);
        }
        if ((data & 0x20) == 0 && (mcu->ssr_rd & 0x20) != 0)
        {
            mcu->dev_register[address] &= ~0x20;
        }
        if ((data & 0x10) == 0 && (mcu->ssr_rd & 0x10) != 0)
        {
            mcu->dev_register[address] &= ~0x10;
        }
        break;
    }
    default:
        address += 0;
        break;
    }
    mcu->dev_register[address] = data;
}

static uint8_t MCU_DeviceRead(mcu_t *mcu, uint32_t address)
{
    address &= 0x7f;
    if (address >= 0x10 && address < 0x40)
    {
        return TIMER_ReadFRT(mcu->timer, address);
    }
    if (address >= 0x50 && address < 0x55)
    {
        return TIMER_ReadTMR(mcu->timer, address);
    }
    switch (address)
    {
    case DEV_ADDRAH:
    case DEV_ADDRAL:
    case DEV_ADDRBH:
    case DEV_ADDRBL:
    case DEV_ADDRCH:
    case DEV_ADDRCL:
    case DEV_ADDRDH:
    case DEV_ADDRDL:
        return mcu->dev_register[address];
    case DEV_ADCSR:
        mcu->adf_rd = (mcu->dev_register[address] & 0x80) != 0;
        return mcu->dev_register[address];
    case DEV_SSR:
        mcu->ssr_rd = mcu->dev_register[address];
        return mcu->dev_register[address];
    case DEV_RDR:
        return mcu->uart_rx_byte;
    case 0x00:
        return 0xff;
    case DEV_P7DR:
    {
        uint8_t data;
        uint32_t button_pressed;
        if (!mcu->is_jv880) return 0xff;

        data = 0xff;
        button_pressed = mcu->button_pressed;

        if (mcu->io_sd == 0xfb)
            data &= ((button_pressed >> 0) & 0x1f) ^ 0xFF;
        if (mcu->io_sd == 0xf7)
            data &= ((button_pressed >> 5) & 0x1f) ^ 0xFF;
        if (mcu->io_sd == 0xef)
            data &= ((button_pressed >> 10) & 0xf) ^ 0xFF;

        data |= 0x80;
        return data;
    }
    case DEV_P9DR:
    {
        uint8_t dir;
        uint8_t val;
        uint8_t cfg = 0;
        if (!mcu->is_mk1)
            cfg = mcu->is_sc155 ? 0 : 2; /* bit 1: 0 - SC-155mk2 (???), 1 - SC-55mk2 */

        dir = mcu->dev_register[DEV_P9DDR];

        val = cfg & (dir ^ 0xff);
        val |= mcu->dev_register[DEV_P9DR] & dir;
        return val;
    }
    case DEV_SCR:
    case DEV_TDR:
    case DEV_SMR:
        return mcu->dev_register[address];
    case DEV_IPRC:
    case DEV_IPRD:
    case DEV_DTEC:
    case DEV_DTED:
        return mcu->dev_register[address];
    }
    return mcu->dev_register[address];
}

static void MCU_DeviceReset(mcu_t *mcu)
{
    mcu->dev_register[DEV_P1DDR] = 0x03;
    mcu->dev_register[DEV_P1DR]  = 0;
    mcu->dev_register[DEV_P1CR]  = 0x87;

    mcu->dev_register[DEV_P2DDR] = 0xE0;
    mcu->dev_register[DEV_P2DR]  = 0xE0;

    mcu->dev_register[DEV_P3DDR] = 0;
    mcu->dev_register[DEV_P3DR]  = 0;

    mcu->dev_register[DEV_P4DDR] = 0;
    mcu->dev_register[DEV_P4DR]  = 0;

    mcu->dev_register[DEV_P5DDR] = 0;
    mcu->dev_register[DEV_P5DR]  = 0;

    mcu->dev_register[DEV_P6DDR] = 0xF0;
    mcu->dev_register[DEV_P6DR]  = 0xF0;

    mcu->dev_register[DEV_P7DDR] = 0;
    mcu->dev_register[DEV_P7DR]  = 0;

    mcu->dev_register[DEV_P8DR] = 0;

    mcu->dev_register[DEV_P9DDR] = 0;
    mcu->dev_register[DEV_P9DR]  = 0;

    /* dev_register bypassed for timers */
    TIMER_Reset(mcu->timer);

    mcu->dev_register[DEV_RDR] = 0;
    mcu->dev_register[DEV_TDR] = 0xFF;
    mcu->dev_register[DEV_SMR] = 0x04;
    mcu->dev_register[DEV_SCR] = 0x0C;
    mcu->dev_register[DEV_SSR] = 0x87;
    mcu->dev_register[DEV_BRR] = 0xFF;

    mcu->dev_register[DEV_ADDRAH] = 0;
    mcu->dev_register[DEV_ADDRAL] = 0;
    mcu->dev_register[DEV_ADDRBH] = 0;
    mcu->dev_register[DEV_ADDRBL] = 0;
    mcu->dev_register[DEV_ADDRCH] = 0;
    mcu->dev_register[DEV_ADDRCL] = 0;
    mcu->dev_register[DEV_ADDRDH] = 0;
    mcu->dev_register[DEV_ADDRDL] = 0;
    mcu->dev_register[DEV_ADCSR]  = 0;

    mcu->dev_register[DEV_IPRA] = 0;
    mcu->dev_register[DEV_IPRB] = 0;
    mcu->dev_register[DEV_IPRC] = 0;
    mcu->dev_register[DEV_IPRD] = 0;
    mcu->dev_register[DEV_DTEA] = 0;
    mcu->dev_register[DEV_DTEB] = 0;
    mcu->dev_register[DEV_DTEC] = 0;
    mcu->dev_register[DEV_DTED] = 0;

    mcu->dev_register[DEV_WCR] = 0xF3;

    mcu->dev_register[DEV_RAMCR] = 0x80;
}

static void MCU_UpdateAnalog(mcu_t *mcu, uint64_t cycles)
{
    const uint8_t ctrl = mcu->dev_register[DEV_ADCSR];
    const int isscan = (ctrl & 16) != 0;

    if (ctrl & 0x20)
    {
        if (mcu->analog_end_time == 0)
            mcu->analog_end_time = cycles + 200;
        else if (mcu->analog_end_time < cycles)
        {
            if (isscan)
            {
                uint8_t i;
                uint8_t base = ctrl & 4;
                for (i = 0; i <= (ctrl & 3); i++)
                    MCU_AnalogSample(mcu, base + i);
                mcu->analog_end_time = cycles + 200;
            }
            else
            {
                MCU_AnalogSample(mcu, ctrl & 7);
                mcu->dev_register[DEV_ADCSR] &= ~0x20;
                mcu->analog_end_time = 0;
            }
            mcu->dev_register[DEV_ADCSR] |= 0x80;
            if (ctrl & 0x40)
                MCU_Interrupt_SetRequest(mcu, INTERRUPT_SOURCE_ANALOG, 1);
        }
    }
    else
        mcu->analog_end_time = 0;
}

static uint8_t MCU_Read(mcu_t *mcu, uint32_t address)
{
    uint8_t page;
    uint8_t ret;
    uint32_t address_rom = address & 0x3ffff;
    if (address & 0x80000 && !mcu->is_jv880)
        address_rom |= 0x40000;
    page = (address >> 16) & 0xf;
    address &= 0xffff;
    ret = 0xff;
    switch (page)
    {
    case 0:
        if (!(address & 0x8000))
            ret = mcu->rom1[address & 0x7fff];
        else
        {
            if (!mcu->is_mk1)
            {
                uint16_t base = mcu->is_jv880 ? 0xf000 : 0xe000;
                if (address >= base && address < (base | 0x400u))
                {
                    ret = PCM_Read(mcu->pcm, address & 0x3f);
                }
                else if (!mcu->is_scb55 && address >= 0xec00u && address < 0xf000u)
                {
                    ret = SM_SysRead(mcu->sm, address & 0xff);
                }
                else if (address >= 0xff80)
                {
                    ret = MCU_DeviceRead(mcu, address & 0x7f);
                }
                else if (address >= 0xfb80u && address < 0xff80u
                    && (mcu->dev_register[DEV_RAMCR] & 0x80) != 0)
                    ret = mcu->ram[(address - 0xfb80) & 0x3ff];
                else if (address >= 0x8000u && address < 0xe000u)
                {
                    ret = mcu->sram[address & 0x7fff];
                }
                else if (address == (base | 0x402u))
                {
                    ret = (uint8_t)mcu->ga_int_trigger;
                    mcu->ga_int_trigger = 0;
                    MCU_Interrupt_SetRequest(mcu, mcu->is_jv880 ? INTERRUPT_SOURCE_IRQ0 : INTERRUPT_SOURCE_IRQ1, 0);
                }
                else
                {
                    ret = 0xff;
                }

                /* e402:2-0 irq source */

            }
            else
            {
                if (address >= 0xe000 && address < 0xe040)
                {
                    ret = PCM_Read(mcu->pcm, address & 0x3f);
                }
                else if (address >= 0xff80)
                {
                    ret = MCU_DeviceRead(mcu, address & 0x7f);
                }
                else if (address >= 0xfb80 && address < 0xff80
                    && (mcu->dev_register[DEV_RAMCR] & 0x80) != 0)
                {
                    ret = mcu->ram[(address - 0xfb80) & 0x3ff];
                }
                else if (address >= 0x8000 && address < 0xe000)
                {
                    ret = mcu->sram[address & 0x7fff];
                }
                else if (address >= 0xf000 && address < 0xf100)
                {
                    uint8_t data;
                    uint32_t button_pressed;
                    mcu->io_sd = address & 0xff;

                    if (mcu->is_cm300)
                        return 0xff;

                    LCD_Enable(mcu->lcd, (mcu->io_sd & 8) != 0);

                    data = 0xff;
                    button_pressed = mcu->button_pressed;

                    if ((mcu->io_sd & 1) == 0)
                        data &= ((button_pressed >> 0) & 255) ^ 255;
                    if ((mcu->io_sd & 2) == 0)
                        data &= ((button_pressed >> 8) & 255) ^ 255;
                    if ((mcu->io_sd & 4) == 0)
                        data &= ((button_pressed >> 16) & 255) ^ 255;
                    if ((mcu->io_sd & 8) == 0)
                        data &= (uint8_t)(((button_pressed >> 24) & 255) ^ 255);
                    return data;
                }
                else if (address == 0xf106)
                {
                    ret = (uint8_t)mcu->ga_int_trigger;
                    mcu->ga_int_trigger = 0;
                    MCU_Interrupt_SetRequest(mcu, INTERRUPT_SOURCE_IRQ1, 0);
                }
                else
                {
                    ret = 0xff;
                }

                /* f106:2-0 irq source */

            }
        }
        break;
#if 0
    case 3:
        ret = rom2[address | 0x30000];
        break;
    case 4:
        ret = rom2[address];
        break;
    case 10:
        ret = rom2[address | 0x60000]; /* FIXME */
        break;
    case 1:
        ret = rom2[address | 0x10000];
        break;
#endif
    case 1:
        ret = mcu->rom2[address_rom & mcu->rom2_mask];
        break;
    case 2:
        ret = mcu->rom2[address_rom & mcu->rom2_mask];
        break;
    case 3:
        ret = mcu->rom2[address_rom & mcu->rom2_mask];
        break;
    case 4:
        ret = mcu->rom2[address_rom & mcu->rom2_mask];
        break;
    case 8:
        if (!mcu->is_jv880)
            ret = mcu->rom2[address_rom & mcu->rom2_mask];
        else
            ret = 0xff;
        break;
    case 9:
        if (!mcu->is_jv880)
            ret = mcu->rom2[address_rom & mcu->rom2_mask];
        else
            ret = 0xff;
        break;
    case 14:
    case 15:
        if (!mcu->is_jv880)
            ret = mcu->rom2[address_rom & mcu->rom2_mask];
        else
            ret = 0xff;
        break;
    case 10:
    case 11:
        if (!mcu->is_mk1)
            ret = mcu->sram[address & 0x7fff]; /* FIXME */
        else
            ret = 0xff;
        break;
    case 12:
    case 13:
        if (mcu->is_jv880)
            ret = 0xff;
        else
            ret = 0xff;
        break;
    case 5:
        if (mcu->is_mk1)
            ret = mcu->sram[address & 0x7fff]; /* FIXME */
        else
            ret = 0xff;
        break;
    default:
        ret = 0x00;
        break;
    }
    return ret;
}

static uint16_t MCU_Read16(mcu_t *mcu, uint32_t address)
{
    uint8_t b0;
    uint8_t b1;
    address &= ~1u;
    b0 = MCU_Read(mcu, address);
    b1 = MCU_Read(mcu, address+1);
    return (uint16_t)((b0 << 8) + b1);
}

static uint32_t MCU_Read32(mcu_t *mcu, uint32_t address)
{
    uint8_t b0;
    uint8_t b1;
    uint8_t b2;
    uint8_t b3;
    address &= ~3u;
    b0 = MCU_Read(mcu, address);
    b1 = MCU_Read(mcu, address+1);
    b2 = MCU_Read(mcu, address+2);
    b3 = MCU_Read(mcu, address+3);
    return ((uint32_t)b0 << 24) + ((uint32_t)b1 << 16) + ((uint32_t)b2 << 8) + b3;
}

static void MCU_Write(mcu_t *mcu, uint32_t address, uint8_t value)
{
    uint8_t page = (address >> 16) & 0xf;
    address &= 0xffff;
    if (page == 0)
    {
        if (address & 0x8000)
        {
            if (!mcu->is_mk1)
            {
                uint16_t base = mcu->is_jv880 ? 0xf000u : 0xe000u;
                if (address >= (base | 0x400u) && address < (base | 0x800u))
                {
                    if (address == (base | 0x404u) || address == (base | 0x405u))
                        LCD_Write(mcu->lcd, address & 1, value);
                    else if (address == (base | 0x401u))
                    {
                        mcu->io_sd = value;
                        LCD_Enable(mcu->lcd, (value & 1) == 0);
                    }
                    else if (address == (base | 0x402u))
                        mcu->ga_int_enable = (uint8_t)(value << 1);

                    /* e400: always 4? */
                    /* e401: SC0-6? */
                    /* e402: enable/disable IRQ? */
                    /* e403: always 1? */
                    /* e404: LCD */
                    /* e405: LCD */
                    /* e406: 0 or 40 */
                    /* e407: 0, e406 continuation? */

                }
                else if (address >= (base | 0x000u) && address < (base | 0x400u))
                {
                    PCM_Write(mcu->pcm, address & 0x3f, value);
                }
                else if (!mcu->is_scb55 && address >= 0xec00 && address < 0xf000)
                {
                    SM_SysWrite(mcu->sm, address & 0xff, value);
                }
                else if (address >= 0xff80)
                {
                    MCU_DeviceWrite(mcu, address & 0x7f, value);
                }
                else if (address >= 0xfb80 && address < 0xff80
                    && (mcu->dev_register[DEV_RAMCR] & 0x80) != 0)
                {
                    mcu->ram[(address - 0xfb80) & 0x3ff] = value;
                }
                else if (address >= 0x8000 && address < 0xe000)
                {
                    mcu->sram[address & 0x7fff] = value;
                }
                else
                {
                }
            }
            else
            {
                if (address >= 0xe000 && address < 0xe040)
                {
                    PCM_Write(mcu->pcm, address & 0x3f, value);
                }
                else if (address >= 0xff80)
                {
                    MCU_DeviceWrite(mcu, address & 0x7f, value);
                }
                else if (address >= 0xfb80 && address < 0xff80
                    && (mcu->dev_register[DEV_RAMCR] & 0x80) != 0)
                {
                    mcu->ram[(address - 0xfb80) & 0x3ff] = value;
                }
                else if (address >= 0x8000 && address < 0xe000)
                {
                    mcu->sram[address & 0x7fff] = value;
                }
                else if (address >= 0xf000 && address < 0xf100)
                {
                    mcu->io_sd = address & 0xff;
                    LCD_Enable(mcu->lcd, (mcu->io_sd & 8) != 0);
                }
                else if (address == 0xf105)
                {
                    LCD_Write(mcu->lcd, 0, value);
                    mcu->ga_lcd_counter = 500;
                }
                else if (address == 0xf104)
                {
                    LCD_Write(mcu->lcd, 1, value);
                    mcu->ga_lcd_counter = 500;
                }
                else if (address == 0xf107)
                {
                    mcu->io_sd = value;
                }
                else
                {
                }
            }
        }
        else if (mcu->is_jv880 && address >= 0x6196 && address <= 0x6199)
        {
            /* nop: the jv880 rom writes into the rom at 002E77-002E7D */
        }
        else
        {
        }
    }
    else if (page == 5 && mcu->is_mk1)
    {
        mcu->sram[address & 0x7fff] = value; /* FIXME */
    }
    else if (page == 10 && !mcu->is_mk1)
    {
        mcu->sram[address & 0x7fff] = value; /* FIXME */
    }
    else
    {
    }
}

static void MCU_Write16(mcu_t *mcu, uint32_t address, uint16_t value)
{
    address &= ~1u;
    MCU_Write(mcu, address, (uint8_t)(value >> 8));
    MCU_Write(mcu, address + 1, (uint8_t)(value & 0xff));
}

static void MCU_ReadInstruction(mcu_t *mcu)
{
    uint8_t operand;
#if NUKED_ENABLE_DECODER2
    decoder2::FetchDecodeExecuteNext(mcu);
#else
    operand = MCU_ReadCodeAdvance(mcu);
    MCU_Operand_Table[operand](mcu, operand);
#endif

    if (mcu->sr & STATUS_T)
    {
        MCU_Interrupt_Exception(mcu, EXCEPTION_SOURCE_TRACE);
    }
}

static void MCU_Init(mcu_t *mcu, submcu_t *sm, pcm_t *pcm, mcu_timer_t *timer)
{
    mcu->sm = sm;
    mcu->pcm = pcm;
    mcu->timer = timer;
}

static void MCU_Reset(mcu_t *mcu)
{
    uint32_t reset_address;
    mcu->r[0] = 0;
    mcu->r[1] = 0;
    mcu->r[2] = 0;
    mcu->r[3] = 0;
    mcu->r[4] = 0;
    mcu->r[5] = 0;
    mcu->r[6] = 0;
    mcu->r[7] = 0;

    mcu->pc = 0;

    mcu->sr = 0x700;

    mcu->cp = 0;
    mcu->dp = 0;
    mcu->ep = 0;
    mcu->tp = 0;
    mcu->br = 0;

    reset_address = MCU_GetVectorAddress(mcu, VECTOR_RESET);
    mcu->cp = (reset_address >> 16) & 0xff;
    mcu->pc = reset_address & 0xffff;

    mcu->exception_pending = (MCU_Exception_Source)-1;

    MCU_DeviceReset(mcu);

    if (mcu->is_mk1)
    {
        mcu->ga_int_enable = 255;
    }
}

static void MCU_PostUART(mcu_t *mcu, uint8_t data)
{
    mcu->uart_buffer[mcu->uart_write_ptr] = data;
    mcu->uart_write_ptr = (mcu->uart_write_ptr + 1) % uart_buffer_size;
}

static void MCU_UpdateUART_RX(mcu_t *mcu)
{
    if ((mcu->dev_register[DEV_SCR] & 16) == 0) /* RX disabled */
        return;
    if (mcu->uart_write_ptr == mcu->uart_read_ptr) /* no byte */
        return;

    if (mcu->dev_register[DEV_SSR] & 0x40)
        return;

    if (mcu->cycles < mcu->uart_rx_delay)
        return;

    mcu->uart_rx_byte = mcu->uart_buffer[mcu->uart_read_ptr];
    mcu->uart_read_ptr = (mcu->uart_read_ptr + 1) % uart_buffer_size;
    mcu->dev_register[DEV_SSR] |= 0x40;
    MCU_Interrupt_SetRequest(mcu, INTERRUPT_SOURCE_UART_RX, (mcu->dev_register[DEV_SCR] & 0x40) != 0);
}

/* dummy TX */
static void MCU_UpdateUART_TX(mcu_t *mcu)
{
    if ((mcu->dev_register[DEV_SCR] & 32) == 0) /* TX disabled */
        return;

    if (mcu->dev_register[DEV_SSR] & 0x80)
        return;

    if (mcu->cycles < mcu->uart_tx_delay)
        return;

    mcu->dev_register[DEV_SSR] |= 0x80;
    MCU_Interrupt_SetRequest(mcu, INTERRUPT_SOURCE_UART_TX, (mcu->dev_register[DEV_SCR] & 0x80) != 0);

    /* fprintf(stderr, "tx:%x\n", mcu->dev_register[DEV_TDR]); */
}

static void MCU_Step(mcu_t *mcu)
{
    if (!mcu->ex_ignore)
        MCU_Interrupt_Handle(mcu);
    else
        mcu->ex_ignore = 0;

    if (!mcu->sleep)
        MCU_ReadInstruction(mcu);

    mcu->cycles += 12; /* FIXME: assume 12 cycles per instruction */

    /* if (mcu->cycles % 24000000 == 0) */
    /* fprintf(stderr, "seconds: %i\n", (int)(mcu->cycles / 24000000)); */

    PCM_Update(mcu->pcm, mcu->cycles);

    TIMER_Clock(mcu->timer, mcu->cycles);

    if (!mcu->is_mk1 && !mcu->is_jv880 && !mcu->is_scb55)
        SM_Update(mcu->sm, mcu->cycles);
    else
    {
        MCU_UpdateUART_RX(mcu);
        MCU_UpdateUART_TX(mcu);
    }

    MCU_UpdateAnalog(mcu, mcu->cycles);

    if (mcu->is_mk1)
    {
        if (mcu->ga_lcd_counter)
        {
            mcu->ga_lcd_counter--;
            if (mcu->ga_lcd_counter == 0)
            {
                MCU_GA_SetGAInt(mcu, 1, 0);
                MCU_GA_SetGAInt(mcu, 1, 1);
            }
        }
    }
}

static uint8_t MCU_ReadP0(mcu_t *mcu)
{
    (void)mcu;
    return 0xff;
}

static uint8_t MCU_ReadP1(mcu_t *mcu)
{
    uint8_t data = 0xff;
    uint32_t button_pressed = mcu->button_pressed;

    if ((mcu->p0_data & 1) == 0)
        data &= ((button_pressed >> 0) & 255) ^ 255;
    if ((mcu->p0_data & 2) == 0)
        data &= ((button_pressed >> 8) & 255) ^ 255;
    if ((mcu->p0_data & 4) == 0)
        data &= ((button_pressed >> 16) & 255) ^ 255;
    if ((mcu->p0_data & 8) == 0)
        data &= (uint8_t)(((button_pressed >> 24) & 255) ^ 255);

    return data;
}

static void MCU_WriteP0(mcu_t *mcu, uint8_t data)
{
    mcu->p0_data = data;
}

static void MCU_WriteP1(mcu_t *mcu, uint8_t data)
{
    mcu->p1_data = data;
}

static void MCU_PostSample(mcu_t *mcu, int32_t left, int32_t right)
{
    int32_t *dst;
    if (mcu->out_count < mcu->out_cap)
        dst = mcu->out + mcu->out_count * 2;
    else if (mcu->out_count - mcu->out_cap < SC55_SPILL)
        dst = mcu->spill + (mcu->out_count - mcu->out_cap) * 2;
    else
        return;
    dst[0] = left;
    dst[1] = right;
    mcu->out_count++;
}

static void MCU_GA_SetGAInt(mcu_t *mcu, uint8_t line, int value)
{
    /* guesswork */
    if (value && !mcu->ga_int[line] && (mcu->ga_int_enable & (1 << line)) != 0)
        mcu->ga_int_trigger = line;
    mcu->ga_int[line] = value;

    if (mcu->is_jv880)
        MCU_Interrupt_SetRequest(mcu, INTERRUPT_SOURCE_IRQ0, mcu->ga_int_trigger != 0);
    else
        MCU_Interrupt_SetRequest(mcu, INTERRUPT_SOURCE_IRQ1, mcu->ga_int_trigger != 0);
}

static void MCU_SetRomset(mcu_t *mcu, Romset romset)
{
    mcu->romset   = romset;
    mcu->is_mk1   = 0;
    mcu->is_cm300 = 0;
    mcu->is_st    = 0;
    mcu->is_jv880 = 0;
    mcu->is_scb55 = 0;
    mcu->is_sc155 = 0;

    switch (romset)
    {
    case ROMSET_MK2:
        break;
    case ROMSET_SC155MK2:
        mcu->is_sc155 = 1;
        break;
    case ROMSET_ST:
        mcu->is_st = 1;
        break;
    case ROMSET_MK1:
        mcu->is_mk1 = 1;
        break;
    case ROMSET_SC155:
        mcu->is_mk1   = 1;
        mcu->is_sc155 = 1;
        break;
    case ROMSET_CM300:
        mcu->is_mk1   = 1;
        mcu->is_cm300 = 1;
        break;
    case ROMSET_JV880:
        mcu->is_jv880 = 1;
        break;
    case ROMSET_SCB55:
    case ROMSET_RLP3237:
        mcu->is_scb55 = 1;
        break;
    }

    TIMER_NotifyRomsetChange(mcu->timer);
}

struct sc55
{
    mcu_t       mcu;
    submcu_t    sm;
    mcu_timer_t timer;
    pcm_t       pcm;
    int32_t     spill[SC55_SPILL * 2];
    size_t      spill_count;
};

/* Wave ROM dumps have their address and data lines shuffled.  The
 * address shuffle works within each megabyte, so it is split into two
 * 10-bit lookups. */
static void sc55_unscramble(const uint8_t *src, uint8_t *dst, size_t len)
{
    static const uint8_t aa[20] = {
        2, 0, 3, 4, 1, 9, 13, 10, 18, 17, 6, 15, 11, 16, 8, 5, 12, 7, 14, 19
    };
    static const uint8_t dd[8] = {
        2, 0, 4, 5, 7, 6, 3, 1
    };
    uint8_t   dmap[256];
    uint32_t *amap;
    size_t    i;
    int       j;

    amap = (uint32_t*)malloc(2 * 1024 * sizeof(*amap));
    if (!amap)
    {
        memset(dst, 0, len);
        return;
    }
    for (i = 0; i < 256; i++)
    {
        uint8_t data = 0;
        for (j = 0; j < 8; j++)
            if (i & ((size_t)1 << dd[j]))
                data |= (uint8_t)(1 << j);
        dmap[i] = data;
    }
    for (i = 0; i < 1024; i++)
    {
        uint32_t lo = 0, hi = 0;
        for (j = 0; j < 10; j++)
            if (i & ((size_t)1 << j))
            {
                lo |= (uint32_t)1 << aa[j];
                hi |= (uint32_t)1 << aa[j + 10];
            }
        amap[i]        = lo;
        amap[1024 + i] = hi;
    }
    for (i = 0; i < len; i++)
    {
        size_t address = (i & ~(size_t)0xfffff)
                       | amap[i & 1023] | amap[1024 + ((i >> 10) & 1023)];
        dst[i] = (address < len) ? dmap[src[address]] : 0;
    }
    free(amap);
}

sc55_t *sc55_new(int model)
{
    Romset  romset;
    sc55_t *s = (sc55_t*)calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    s->mcu.sw_pos    = 3;
    s->mcu.rom2_mask = ROM2_SIZE - 1;
    s->pcm.config.reg_slots    = 1;
    s->pcm.enable_oversampling = 1;

    MCU_Init(&s->mcu, &s->sm, &s->pcm, &s->timer);
    SM_Init(&s->sm, &s->mcu);
    PCM_Init(&s->pcm, &s->mcu);
    TIMER_Init(&s->timer, &s->mcu);
    TIMER_Reset(&s->timer);
    switch (model)
    {
        case SC55_MODEL_MK1:     romset = ROMSET_MK1;     break;
        case SC55_MODEL_ST:      romset = ROMSET_ST;      break;
        case SC55_MODEL_SC155:   romset = ROMSET_SC155;   break;
        case SC55_MODEL_CM300:   romset = ROMSET_CM300;   break;
        case SC55_MODEL_SCB55:   romset = ROMSET_SCB55;   break;
        case SC55_MODEL_RLP3237: romset = ROMSET_RLP3237; break;
        default:                 romset = ROMSET_MK2;     break;
    }
    MCU_SetRomset(&s->mcu, romset);

    s->mcu.spill = s->spill;
    return s;
}

void sc55_free(sc55_t *s)
{
    free(s);
}

int sc55_load_rom(sc55_t *s, int slot, const unsigned char *data, size_t len)
{
    uint8_t *dst;
    size_t   cap;
    int      wave = 0;

    switch (slot)
    {
        case SC55_ROM_ROM1:     dst = s->mcu.rom1;     cap = ROM1_SIZE;     break;
        case SC55_ROM_ROM2:     dst = s->mcu.rom2;     cap = ROM2_SIZE;     break;
        case SC55_ROM_SMROM:    dst = s->sm.rom;       cap = SMROM_SIZE;    break;
        case SC55_ROM_WAVEROM1: dst = s->pcm.waverom1; cap = WAVEROM1_SIZE; wave = 1; break;
        case SC55_ROM_WAVEROM2: dst = s->pcm.waverom2; cap = WAVEROM2_SIZE; wave = 1; break;
        case SC55_ROM_WAVEROM3: dst = s->pcm.waverom3; cap = WAVEROM3_SIZE; wave = 1; break;
        default:
            return 0;
    }
    if (!data || !len || len > cap)
        return 0;
    if (slot == SC55_ROM_ROM2)
    {
        if (len & (len - 1)) /* the mask below needs a power of two */
            return 0;
        s->mcu.rom2_mask = (uint32_t)len - 1;
    }
    if (wave)
        sc55_unscramble(data, dst, len);
    else
        memcpy(dst, data, len);
    return 1;
}

void sc55_reset(sc55_t *s)
{
    MCU_Reset(&s->mcu);
    SM_Reset(&s->sm);
}

void sc55_midi(sc55_t *s, const unsigned char *data, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++)
        MCU_PostUART(&s->mcu, data[i]);
}

size_t sc55_midi_room(const sc55_t *s)
{
    uint32_t used = (s->mcu.uart_write_ptr + uart_buffer_size - s->mcu.uart_read_ptr) % uart_buffer_size;
    return (size_t)(uart_buffer_size - 1 - used);
}

uint32_t sc55_voices(const sc55_t *s)
{
    return s->pcm.voice_mask & s->pcm.voice_mask_pending;
}

unsigned sc55_rate(const sc55_t *s)
{
    return PCM_GetOutputFrequency(&s->pcm);
}

size_t sc55_run(sc55_t *s, int32_t *frames, size_t count)
{
    mcu_t *mcu = &s->mcu;
    size_t n   = s->spill_count;

    /* Frames the last call produced beyond what it was asked for. */
    if (n > count)
        n = count;
    memcpy(frames, s->spill, n * 2 * sizeof(int32_t));
    if (n < s->spill_count)
        memmove(s->spill, s->spill + n * 2, (s->spill_count - n) * 2 * sizeof(int32_t));
    s->spill_count -= n;
    if (n == count)
        return n;

    /* The caller's buffer is filled directly; only the last step's
     * overshoot lands in the spill. */
    mcu->out       = frames + n * 2;
    mcu->out_cap   = count - n;
    mcu->out_count = 0;
    while (mcu->out_count < mcu->out_cap)
        MCU_Step(mcu);
    s->spill_count = mcu->out_count - mcu->out_cap;
    mcu->out       = NULL;
    mcu->out_cap   = 0;
    mcu->out_count = 0;
    return count;
}

#ifdef SC55_TEST
/* Hooks for the differential harness, which drives this file and the
 * C++ original side by side. */
static uint32_t sc55_test_fnv(uint32_t h, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t*)data;
    while (len--)
        h = (h ^ *p++) * 16777619u;
    return h;
}

#define SC55_HF(field) h = sc55_test_fnv(h, &(field), sizeof(field))
uint32_t sc55_test_hash(const sc55_t *s)
{
    uint32_t h = 2166136261u;
    int i;
    SC55_HF(s->mcu.r); SC55_HF(s->mcu.pc); SC55_HF(s->mcu.sr);
    SC55_HF(s->mcu.cp); SC55_HF(s->mcu.dp); SC55_HF(s->mcu.ep); SC55_HF(s->mcu.tp); SC55_HF(s->mcu.br);
    SC55_HF(s->mcu.sleep); SC55_HF(s->mcu.ex_ignore); SC55_HF(s->mcu.exception_pending);
    SC55_HF(s->mcu.interrupt_pending); SC55_HF(s->mcu.trapa_pending); SC55_HF(s->mcu.cycles);
    SC55_HF(s->mcu.ram); SC55_HF(s->mcu.sram); SC55_HF(s->mcu.dev_register);
    SC55_HF(s->mcu.io_sd); SC55_HF(s->mcu.uart_read_ptr); SC55_HF(s->mcu.uart_rx_byte);
    SC55_HF(s->mcu.ga_int_enable); SC55_HF(s->mcu.ga_int_trigger);
    for (i = 0; i < 3; i++)
    {
        SC55_HF(s->timer.frt[i].deadline); SC55_HF(s->timer.frt[i].tcr); SC55_HF(s->timer.frt[i].tcsr);
        SC55_HF(s->timer.frt[i].frc); SC55_HF(s->timer.frt[i].ocra); SC55_HF(s->timer.frt[i].ocrb);
        SC55_HF(s->timer.frt[i].icr); SC55_HF(s->timer.frt[i].status_rd); SC55_HF(s->timer.frt[i].stride);
    }
    SC55_HF(s->timer.tmr.deadline); SC55_HF(s->timer.tmr.stride); SC55_HF(s->timer.tmr.tcr);
    SC55_HF(s->timer.tmr.tcsr); SC55_HF(s->timer.tmr.tcora); SC55_HF(s->timer.tmr.tcorb);
    SC55_HF(s->timer.tmr.tcnt); SC55_HF(s->timer.tmr.status_rd);
    SC55_HF(s->pcm.ram1); SC55_HF(s->pcm.ram2); SC55_HF(s->pcm.cycles);
    SC55_HF(s->pcm.voice_mask); SC55_HF(s->pcm.voice_mask_pending); SC55_HF(s->pcm.write_latch);
    SC55_HF(s->pcm.read_latch); SC55_HF(s->pcm.wave_read_address); SC55_HF(s->pcm.tv_counter);
    SC55_HF(s->pcm.wave_byte_latch); SC55_HF(s->pcm.select_channel); SC55_HF(s->pcm.config_reg_3c);
    SC55_HF(s->pcm.config_reg_3d); SC55_HF(s->pcm.irq_channel); SC55_HF(s->pcm.accum_l); SC55_HF(s->pcm.accum_r);
    SC55_HF(s->pcm.rcsum); SC55_HF(s->pcm.eram);
    SC55_HF(s->sm.pc); SC55_HF(s->sm.a); SC55_HF(s->sm.x); SC55_HF(s->sm.y); SC55_HF(s->sm.s); SC55_HF(s->sm.sr);
    SC55_HF(s->sm.cycles); SC55_HF(s->sm.sleep); SC55_HF(s->sm.ram); SC55_HF(s->sm.shared_ram);
    SC55_HF(s->sm.access); SC55_HF(s->sm.device_mode); SC55_HF(s->sm.timer_cycles);
    SC55_HF(s->sm.timer_prescaler); SC55_HF(s->sm.timer_counter);
    return h;
}
#undef SC55_HF

/* One MCU step; frames it produces go to `frames` (room for
 * SC55_SPILL), and the count is returned. */
size_t sc55_test_step(sc55_t *s, int32_t *frames)
{
    size_t n;
    s->mcu.out       = frames;
    s->mcu.out_cap   = SC55_SPILL;
    s->mcu.out_count = 0;
    MCU_Step(&s->mcu);
    n = s->mcu.out_count;
    s->mcu.out     = NULL;
    s->mcu.out_cap = 0;
    s->mcu.out_count = 0;
    return n;
}

void sc55_test_pcm_write(sc55_t *s, uint32_t address, uint8_t data)
{
    PCM_Write(&s->pcm, address, data);
}
#endif
