#ifndef __PCHECKSUM_H__
#define __PCHECKSUM_H__

extern void (*P_Checksum)(int);
extern void P_ChecksumFinal(void);
void P_RecordChecksum(const char *file);
//void P_VerifyChecksum(const char *file);

#endif
