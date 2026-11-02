/* ppmd7v.h -- PPMd var.H decoder with software-paged model memory (DSiDL) */
#ifndef PPMD7V_H
#define PPMD7V_H

#include <stdio.h>
#include "archive_ppmd7_private.h"

typedef struct
{
  UInt32 MinContext, MaxContext, FoundState;
  unsigned OrderFall, InitEsc, PrevSuccess, MaxOrder, HiBitsFlag;
  Int32 RunLength, InitRL;

  UInt32 Size;
  UInt32 GlueCount;
  UInt32 LoUnit, HiUnit, Text, UnitsStart;
  UInt32 AlignOffset;

  Byte Indx2Units[PPMD_NUM_INDEXES];
  Byte Units2Indx[128];
  CPpmd_Void_Ref FreeList[PPMD_NUM_INDEXES];
  Byte NS2Indx[256], NS2BSIndx[256], HB2Flag[256];
  CPpmd_See DummySee, See[25][16];
  UInt16 BinSumm[128][64];

  /* memory */
  Byte *flat;          /* whole model in RAM, or NULL */
  Byte *frames;        /* paged mode: nslots frames of VPAGE bytes */
  Int32 *ptab;         /* page -> slot or -1 */
  Int32 *spage;        /* slot -> page or -1 */
  Byte *sflags;
  Byte *ondisk;        /* bitmap: page has been written to the swap file */
  FILE *swap;
  UInt32 nslots, npages, clock, shift;
  UInt32 lpage, lslot;
  Byte *lframe;
  int io_error;
} CPpmd7v;

extern char ppmd7v_swap_path[300];
extern size_t ppmd7v_ram_limit;
extern size_t ppmd7v_reserve;
extern unsigned long ppmd7v_faults;
extern void (*ppmd7v_yield)(void);

void Ppmd7v_Construct(CPpmd7v *p);
Bool Ppmd7v_Alloc(CPpmd7v *p, UInt32 size);
void Ppmd7v_Free(CPpmd7v *p);
void Ppmd7v_Init(CPpmd7v *p, unsigned maxOrder);
int Ppmd7v_DecodeSymbol(CPpmd7v *p, IPpmd7_RangeDec *rc);
int Ppmd7v_IoError(const CPpmd7v *p);

#endif
