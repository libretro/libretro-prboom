/* Emacs style mode select   -*- C -*-
 *-----------------------------------------------------------------------------
 *
 *  Hexen sprite-name table.
 *
 *  Indexes the HEXEN_SPR_* block of spritenum_t (a separate 0-based index
 *  space, like the Heretic table).  Each entry is the 4-character sprite
 *  lump prefix.  Ported from dsda-doom's hexen sprite table; the explicit
 *  [HEXEN_NUMSPRITES + 1] size makes the compiler count-check the table
 *  against the enum.
 *
 *-----------------------------------------------------------------------------
 */

#include "doomtype.h"
#include "info.h"
#include "hexen/hexen.h"

const char *hexen_sprnames[HEXEN_NUMSPRITES + 1] = {
    "MAN1","ACLO","TLGL","FBL1","XPL1","ARRW","DART","RIPP","CFCF","BLAD",
    "SHRD","FFSM","FFLG","PTN1","PTN2","SOAR","INVU","SUMN","TSPK","TELO",
    "TRNG","ROCK","FOGS","FOGM","FOGL","SGSA","SGSB","PORK","EGGM","FHFX",
    "SPHL","STWN","GMPD","ASKU","ABGM","AGMR","AGMG","AGG2","AGMB","AGB2",
    "ABK1","ABK2","ASK2","AFWP","ACWP","AMWP","AGER","AGR2","AGR3","AGR4",
    "TRCH","PSBG","ATLP","THRW","SPED","BMAN","BRAC","BLST","HRAD","SPSH",
    "LVAS","SLDG","STTW","RCK1","RCK2","RCK3","RCK4","CDLR","TRE1","TRDT",
    "TRE2","TRE3","STM1","STM2","STM3","STM4","MSH1","MSH2","MSH3","MSH4",
    "MSH5","MSH6","MSH7","MSH8","SGMP","SGM1","SGM2","SGM3","SLC1","SLC2",
    "SLC3","MSS1","MSS2","SWMV","CPS1","CPS2","TMS1","TMS2","TMS3","TMS4",
    "TMS5","TMS6","TMS7","CPS3","STT2","STT3","STT4","STT5","GAR1","GAR2",
    "GAR3","GAR4","GAR5","GAR6","GAR7","GAR8","GAR9","BNR1","TRE4","TRE5",
    "TRE6","TRE7","LOGG","ICT1","ICT2","ICT3","ICT4","ICM1","ICM2","ICM3",
    "ICM4","RKBL","RKBS","RKBK","RBL1","RBL2","RBL3","VASE","POT1","POT2",
    "POT3","PBIT","CPS4","CPS5","CPS6","CPB1","CPB2","CPB3","CPB4","BDRP",
    "BDSH","BDPL","CNDL","LEF1","LEF3","LEF2","TWTR","WLTR","BARL","SHB1",
    "SHB2","BCKT","SHRM","FBUL","FSKL","BRTR","SUIT","BBLL","CAND","IRON",
    "XMAS","CDRN","CHNS","TST1","TST2","TST3","TST4","TST5","TST6","TST7",
    "TST8","TST9","TST0","TELE","TSMK","FPCH","WFAX","FAXE","WFHM","FHMR",
    "FSRD","FSFX","CMCE","WCSS","CSSF","WCFM","CFLM","CFFX","CHLY","SPIR",
    "MWND","WMLG","MLNG","MLFX","MLF2","MSTF","MSP1","MSP2","WFR1","WFR2",
    "WFR3","WCH1","WCH2","WCH3","WMS1","WMS2","WMS3","WPIG","WMCS","CONE",
    "SHEX","BLOD","GIBS","PLAY","FDTH","BSKL","ICEC","CLER","MAGE","PIGY",
    "CENT","CTXD","CTFX","CTDP","DEMN","DEMA","DEMB","DEMC","DEMD","DEME",
    "DMFX","DEM2","DMBA","DMBB","DMBC","DMBD","DMBE","D2FX","WRTH","WRT2",
    "WRBL","MNTR","FX12","FX13","MNSM","SSPT","SSDV","SSXD","SSFX","BISH",
    "BPFX","DRAG","DRFX","ARM1","ARM2","ARM3","ARM4","MAN2","MAN3","KEY1",
    "KEY2","KEY3","KEY4","KEY5","KEY6","KEY7","KEY8","KEY9","KEYA","KEYB",
    "ETTN","ETTB","FDMN","FDMB","ICEY","ICPR","ICWS","SORC","SBMP","SBS4",
    "SBMB","SBS3","SBMG","SBS1","SBS2","SBFX","RADE","WATR","KORX","ABAT",
  NULL
};
