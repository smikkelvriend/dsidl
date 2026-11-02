/* ppmd7v.c -- PPMd var.H decoder with a software-paged model memory.
 *
 * DSiDL: RAR 2.9/3.x/4.x archives made with "text compression" (PPMd) need
 * the exact model memory size the encoder used (often 25 MB or more), but a
 * DSi only has 16 MB of RAM. This is a translation of libarchive's Ppmd7.c
 * (Igor Pavlov, public domain; based on PPMd var.H by Dmitry Shkarin, public
 * domain) in which every access to model memory goes through a small page
 * table. Pages that do not fit in RAM are swapped to a file on the SD card.
 * When the whole model fits in RAM a flat buffer is used instead.
 *
 * All model pointers are 32-bit offsets ("refs") with the same layout as the
 * original, so the decoder behaves bit-for-bit identically.
 */

#include "archive_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ppmd7v.h"

#ifdef ARM9
#include <fat.h>
#endif

#define MAX_FREQ 124
#define UNIT_SIZE 12
#define U2B(nu) ((UInt32)(nu) * UNIT_SIZE)
#define U2I(nu) (p->Units2Indx[(nu) - 1])
#define I2U(indx) (p->Indx2Units[indx])

#define VPAGE 1536u /* multiple of 12 (units) and of 512 (SD sectors) */
#define F_REF 1
#define F_DIRTY 2

char ppmd7v_swap_path[300] = "ppmd_swap.tmp";
size_t ppmd7v_ram_limit = 0; /* 0 = no limit (for tests) */
size_t ppmd7v_reserve = 1u << 20; /* RAM to leave free in paged mode */
unsigned long ppmd7v_faults;
void (*ppmd7v_yield)(void) = NULL;
static unsigned yield_count;

/* Creates the swap file at full size so seeks never have to extend it, and
 * (on the DSi) builds a cluster map so every seek is O(1). */
static FILE *open_swap(CPpmd7v *p)
{
  FILE *f = fopen(ppmd7v_swap_path, "w+b");
  if (!f)
    return NULL;
  if (fseek(f, (long)p->npages * VPAGE - 1, SEEK_SET) != 0 || fputc(0, f) == EOF)
  {
    fclose(f);
    remove(ppmd7v_swap_path);
    return NULL;
  }
  fclose(f);
  f = fopen(ppmd7v_swap_path, "r+b");
  if (!f)
    return NULL;
  setvbuf(f, NULL, _IONBF, 0);
#ifdef ARM9
  fatInitLookupCacheFile(f, 32 * 1024);
#endif
  return f;
}

static const UInt16 kInitBinEsc[] = { 0x3CDD, 0x1F3F, 0x59BF, 0x48F3, 0x64A1, 0x5ABC, 0x6632, 0x6051};
static const Byte PPMD7_kExpEscape[16] = { 25, 14, 9, 7, 5, 5, 4, 4, 4, 3, 3, 3, 2, 2, 2, 2 };

/* ---------------- virtual memory ---------------- */

static Byte *vm_fault(CPpmd7v *p, UInt32 pg)
{
  UInt32 slot;
  Int32 old;
  ppmd7v_faults++;
  /* CLOCK victim selection */
  for (;;)
  {
    slot = p->clock;
    if (++p->clock == p->nslots)
      p->clock = 0;
    if (!(p->sflags[slot] & F_REF))
      break;
    p->sflags[slot] &= ~F_REF;
  }
  old = p->spage[slot];
  if (old >= 0)
  {
    if (p->sflags[slot] & F_DIRTY)
    {
      if (!p->swap)
      {
        p->swap = open_swap(p);
        if (!p->swap)
          p->io_error = 1;
      }
      if (p->swap)
      {
        if (fseek(p->swap, (long)old * VPAGE, SEEK_SET) != 0 ||
            fwrite(p->frames + (size_t)slot * VPAGE, 1, VPAGE, p->swap) != VPAGE)
          p->io_error = 1;
        else
          p->ondisk[old >> 3] |= (Byte)(1 << (old & 7));
      }
    }
    p->ptab[old] = -1;
  }
  {
    Byte *f = p->frames + (size_t)slot * VPAGE;
    if (p->ondisk[pg >> 3] & (1 << (pg & 7)))
    {
      if (fseek(p->swap, (long)pg * VPAGE, SEEK_SET) != 0 ||
          fread(f, 1, VPAGE, p->swap) != VPAGE)
        p->io_error = 1;
    }
    else
      memset(f, 0, VPAGE);
  }
  p->ptab[pg] = (Int32)slot;
  p->spage[slot] = (Int32)pg;
  p->sflags[slot] = F_REF;
  return p->frames + (size_t)slot * VPAGE;
}

static Byte *vm_slow(CPpmd7v *p, UInt32 v, int w)
{
  UInt32 pg = v / VPAGE;
  Int32 slot;
  Byte *f;
  if (pg >= p->npages)
  {
    /* Should never happen for valid streams; keep it memory-safe */
    p->io_error = 1;
    pg = 0;
    v = 0;
  }
  slot = p->ptab[pg];
  if (slot < 0)
  {
    f = vm_fault(p, pg);
    slot = p->ptab[pg];
  }
  else
  {
    f = p->frames + (size_t)slot * VPAGE;
    p->sflags[slot] |= F_REF;
  }
  p->lpage = pg;
  p->lslot = (UInt32)slot;
  p->lframe = f;
  if (w)
    p->sflags[slot] |= F_DIRTY;
  return f + (v - pg * VPAGE);
}

static inline Byte *VR(CPpmd7v *p, UInt32 r)
{
  UInt32 v;
  if (p->flat)
    return p->flat + r;
  v = r + p->shift;
  if (v / VPAGE == p->lpage)
    return p->lframe + (v - p->lpage * VPAGE);
  return vm_slow(p, v, 0);
}

static inline Byte *VW(CPpmd7v *p, UInt32 r)
{
  UInt32 v;
  if (p->flat)
    return p->flat + r;
  v = r + p->shift;
  if (v / VPAGE == p->lpage)
  {
    p->sflags[p->lslot] |= F_DIRTY;
    return p->lframe + (v - p->lpage * VPAGE);
  }
  return vm_slow(p, v, 1);
}

static inline unsigned R8(CPpmd7v *p, UInt32 r) { return *VR(p, r); }
static inline unsigned R16(CPpmd7v *p, UInt32 r) { return *(UInt16 *)VR(p, r); }
static inline UInt32 R32(CPpmd7v *p, UInt32 r) { return *(UInt32 *)VR(p, r); }
static inline void W8(CPpmd7v *p, UInt32 r, unsigned v) { *VW(p, r) = (Byte)v; }
static inline void W16(CPpmd7v *p, UInt32 r, unsigned v) { *(UInt16 *)VW(p, r) = (UInt16)v; }
static inline void W32(CPpmd7v *p, UInt32 r, UInt32 v) { *(UInt32 *)VW(p, r) = v; }

/* context fields */
#define NS(c) R16(p, (c))
#define SF(c) R16(p, (c) + 2)
#define STATS(c) R32(p, (c) + 4)
#define SUFFIX(c) R32(p, (c) + 8)
#define SET_NS(c, v) W16(p, (c), (v))
#define SET_SF(c, v) W16(p, (c) + 2, (v))
#define SET_STATS(c, v) W32(p, (c) + 4, (v))
#define SET_SUFFIX(c, v) W32(p, (c) + 8, (v))
#define ONE_STATE(c) ((c) + 2)
/* state fields (6 bytes) */
#define SSIZE 6
#define SYM(s) R8(p, (s))
#define FREQ(s) R8(p, (s) + 1)
#define SET_FREQ(s, v) W8(p, (s) + 1, (v))
/* node fields */
#define N_STAMP(n) R16(p, (n))
#define N_NU(n) R16(p, (n) + 2)
#define N_NEXT(n) R32(p, (n) + 4)
#define N_PREV(n) R32(p, (n) + 8)

static UInt32 SUCC(CPpmd7v *p, UInt32 s)
{
  const Byte *b = VR(p, s);
  return (UInt32)((const UInt16 *)b)[1] | ((UInt32)((const UInt16 *)b)[2] << 16);
}

static void SET_SUCC(CPpmd7v *p, UInt32 s, UInt32 v)
{
  Byte *b = VW(p, s);
  ((UInt16 *)b)[1] = (UInt16)(v & 0xFFFF);
  ((UInt16 *)b)[2] = (UInt16)((v >> 16) & 0xFFFF);
}

static void getS(CPpmd7v *p, UInt32 s, CPpmd_State *st) { memcpy(st, VR(p, s), SSIZE); }
static void putS(CPpmd7v *p, UInt32 s, const CPpmd_State *st) { memcpy(VW(p, s), st, SSIZE); }
static void copyS(CPpmd7v *p, UInt32 d, UInt32 s) { CPpmd_State t; getS(p, s, &t); putS(p, d, &t); }
static void swapS(CPpmd7v *p, UInt32 a, UInt32 b)
{
  CPpmd_State ta, tb;
  getS(p, a, &ta);
  getS(p, b, &tb);
  putS(p, a, &tb);
  putS(p, b, &ta);
}

static void setSuccLocal(CPpmd_State *s, UInt32 v)
{
  s->SuccessorLow = (UInt16)(v & 0xFFFF);
  s->SuccessorHigh = (UInt16)((v >> 16) & 0xFFFF);
}

static void copyUnits(CPpmd7v *p, UInt32 dest, UInt32 src, UInt32 num)
{
  Byte tmp[UNIT_SIZE];
  do
  {
    memcpy(tmp, VR(p, src), UNIT_SIZE);
    memcpy(VW(p, dest), tmp, UNIT_SIZE);
    src += UNIT_SIZE;
    dest += UNIT_SIZE;
  } while (--num);
}

/* ---------------- Base ---------------- */

void Ppmd7v_Construct(CPpmd7v *p)
{
  unsigned i, k, m;
  unsigned initEsc = p->InitEsc; /* set by the caller before Construct */

  memset(p, 0, sizeof(*p));
  p->InitEsc = initEsc;
  p->lpage = 0xFFFFFFFF;

  for (i = 0, k = 0; i < PPMD_NUM_INDEXES; i++)
  {
    unsigned step = (i >= 12 ? 4 : (i >> 2) + 1);
    do { p->Units2Indx[k++] = (Byte)i; } while(--step);
    p->Indx2Units[i] = (Byte)k;
  }

  p->NS2BSIndx[0] = (0 << 1);
  p->NS2BSIndx[1] = (1 << 1);
  memset(p->NS2BSIndx + 2, (2 << 1), 9);
  memset(p->NS2BSIndx + 11, (3 << 1), 256 - 11);

  for (i = 0; i < 3; i++)
    p->NS2Indx[i] = (Byte)i;
  for (m = i, k = 1; i < 256; i++)
  {
    p->NS2Indx[i] = (Byte)m;
    if (--k == 0)
      k = (++m) - 2;
  }

  memset(p->HB2Flag, 0, 0x40);
  memset(p->HB2Flag + 0x40, 8, 0x100 - 0x40);
}

void Ppmd7v_Free(CPpmd7v *p)
{
  free(p->flat);
  free(p->frames);
  free(p->ptab);
  free(p->spage);
  free(p->sflags);
  free(p->ondisk);
  if (p->swap)
  {
    fclose(p->swap);
    remove(ppmd7v_swap_path);
  }
  p->flat = p->frames = p->sflags = p->ondisk = NULL;
  p->ptab = p->spage = NULL;
  p->swap = NULL;
  p->Size = 0;
  p->nslots = p->npages = 0;
  p->lpage = 0xFFFFFFFF;
  p->io_error = 0;
}

static int alloc_fits(CPpmd7v *p, size_t total)
{
  if (ppmd7v_ram_limit && total > ppmd7v_ram_limit)
    return 0;
  p->flat = malloc(total);
  return p->flat != NULL;
}

Bool Ppmd7v_Alloc(CPpmd7v *p, UInt32 size)
{
  size_t total, slots;
  if (p->Size == size && (p->flat || p->frames))
  {
    /* Same size again: start with a clean swap state */
    if (p->frames)
    {
      UInt32 i;
      for (i = 0; i < p->npages; i++)
        p->ptab[i] = -1;
      for (i = 0; i < p->nslots; i++)
      {
        p->spage[i] = -1;
        p->sflags[i] = 0;
      }
      memset(p->ondisk, 0, (p->npages + 7) / 8);
      p->lpage = 0xFFFFFFFF;
      p->io_error = 0;
    }
    return True;
  }
  if (size < UNIT_SIZE)
    return False;
  Ppmd7v_Free(p);
  p->AlignOffset = 4 - (size & 3);
  total = (size_t)p->AlignOffset + size + UNIT_SIZE;
  p->Size = size;
  if (alloc_fits(p, total))
    return True;

  /* Paged mode: unit boundaries must line up with page boundaries */
  {
    UInt32 hi = p->AlignOffset + size;
    p->shift = (VPAGE - hi % VPAGE) % VPAGE;
  }
  p->npages = (UInt32)((total + p->shift + VPAGE - 1) / VPAGE);
  p->ptab = malloc(sizeof(Int32) * p->npages);
  p->ondisk = calloc(1, (p->npages + 7) / 8);
  if (!p->ptab || !p->ondisk)
  {
    Ppmd7v_Free(p);
    return False;
  }
  {
    UInt32 i;
    for (i = 0; i < p->npages; i++)
      p->ptab[i] = -1;
  }
  /* As many frames as RAM allows (keep some room for everything else) */
  slots = p->npages;
  if (ppmd7v_ram_limit && slots > ppmd7v_ram_limit / VPAGE)
    slots = ppmd7v_ram_limit / VPAGE;
  for (;;)
  {
    if (slots < 64)
    {
      Ppmd7v_Free(p);
      return False;
    }
    p->frames = malloc(slots * VPAGE);
    if (p->frames)
    {
      /* leave room for the rest of the program */
      void *spare = malloc(ppmd7v_reserve);
      if (spare)
      {
        free(spare);
        break;
      }
      free(p->frames);
      p->frames = NULL;
    }
    slots -= slots / 16 + 1;
  }
  p->nslots = (UInt32)slots;
  p->spage = malloc(sizeof(Int32) * slots);
  p->sflags = calloc(1, slots);
  if (!p->spage || !p->sflags)
  {
    Ppmd7v_Free(p);
    return False;
  }
  {
    UInt32 i;
    for (i = 0; i < slots; i++)
      p->spage[i] = -1;
  }
  p->clock = 0;
  p->lpage = 0xFFFFFFFF;
  return True;
}

int Ppmd7v_IoError(const CPpmd7v *p) { return p->io_error; }

static void InsertNode(CPpmd7v *p, UInt32 node, unsigned indx)
{
  W32(p, node, p->FreeList[indx]);
  p->FreeList[indx] = node;
}

static UInt32 RemoveNode(CPpmd7v *p, unsigned indx)
{
  UInt32 node = p->FreeList[indx];
  p->FreeList[indx] = R32(p, node);
  return node;
}

static void SplitBlock(CPpmd7v *p, UInt32 ptr, unsigned oldIndx, unsigned newIndx)
{
  unsigned i, nu = I2U(oldIndx) - I2U(newIndx);
  ptr = ptr + U2B(I2U(newIndx));
  if (I2U(i = U2I(nu)) != nu)
  {
    unsigned k = I2U(--i);
    InsertNode(p, ptr + U2B(k), nu - k - 1);
  }
  InsertNode(p, ptr, i);
}

static void GlueFreeBlocks(CPpmd7v *p)
{
  UInt32 head = p->AlignOffset + p->Size;
  UInt32 n = head;
  unsigned i;

  p->GlueCount = 255;

  for (i = 0; i < PPMD_NUM_INDEXES; i++)
  {
    UInt16 nu = I2U(i);
    UInt32 next = p->FreeList[i];
    p->FreeList[i] = 0;
    while (next != 0)
    {
      UInt32 node = next;
      W32(p, node + 4, n);          /* node->Next = n */
      W32(p, n + 8, next);          /* NODE(n)->Prev = next */
      n = next;
      next = R32(p, node);
      W16(p, node, 0);              /* Stamp */
      W16(p, node + 2, nu);         /* NU */
    }
  }
  W16(p, head, 1);
  W32(p, head + 4, n);
  W32(p, n + 8, head);
  if (p->LoUnit != p->HiUnit)
    W16(p, p->LoUnit, 1);

  while (n != head)
  {
    UInt32 node = n;
    UInt32 nu = N_NU(node);
    for (;;)
    {
      UInt32 node2 = n + nu * UNIT_SIZE;
      nu += N_NU(node2);
      if (N_STAMP(node2) != 0 || nu >= 0x10000)
        break;
      {
        UInt32 prev2 = N_PREV(node2), next2 = N_NEXT(node2);
        W32(p, prev2 + 4, next2);
        W32(p, next2 + 8, prev2);
      }
      W16(p, node + 2, nu);
    }
    n = N_NEXT(node);
  }

  for (n = N_NEXT(head); n != head;)
  {
    UInt32 node = n;
    unsigned nu;
    UInt32 next = N_NEXT(node);
    for (nu = N_NU(node); nu > 128; nu -= 128, node += 128 * UNIT_SIZE)
      InsertNode(p, node, PPMD_NUM_INDEXES - 1);
    if (I2U(i = U2I(nu)) != nu)
    {
      unsigned k = I2U(--i);
      InsertNode(p, node + k * UNIT_SIZE, nu - k - 1);
    }
    InsertNode(p, node, i);
    n = next;
  }
}

static UInt32 AllocUnitsRare(CPpmd7v *p, unsigned indx)
{
  unsigned i;
  UInt32 retVal;
  if (p->GlueCount == 0)
  {
    GlueFreeBlocks(p);
    if (p->FreeList[indx] != 0)
      return RemoveNode(p, indx);
  }
  i = indx;
  do
  {
    if (++i == PPMD_NUM_INDEXES)
    {
      UInt32 numBytes = U2B(I2U(indx));
      p->GlueCount--;
      return ((UInt32)(p->UnitsStart - p->Text) > numBytes) ? (p->UnitsStart -= numBytes) : 0;
    }
  }
  while (p->FreeList[i] == 0);
  retVal = RemoveNode(p, i);
  SplitBlock(p, retVal, i, indx);
  return retVal;
}

static UInt32 AllocUnits(CPpmd7v *p, unsigned indx)
{
  UInt32 numBytes;
  if (p->FreeList[indx] != 0)
    return RemoveNode(p, indx);
  numBytes = U2B(I2U(indx));
  if (numBytes <= (UInt32)(p->HiUnit - p->LoUnit))
  {
    UInt32 retVal = p->LoUnit;
    p->LoUnit += numBytes;
    return retVal;
  }
  return AllocUnitsRare(p, indx);
}

static UInt32 ShrinkUnits(CPpmd7v *p, UInt32 oldPtr, unsigned oldNU, unsigned newNU)
{
  unsigned i0 = U2I(oldNU);
  unsigned i1 = U2I(newNU);
  if (i0 == i1)
    return oldPtr;
  if (p->FreeList[i1] != 0)
  {
    UInt32 ptr = RemoveNode(p, i1);
    copyUnits(p, ptr, oldPtr, newNU);
    InsertNode(p, oldPtr, i0);
    return ptr;
  }
  SplitBlock(p, oldPtr, i0, i1);
  return oldPtr;
}

static void RestartModel(CPpmd7v *p)
{
  unsigned i, k, m;

  memset(p->FreeList, 0, sizeof(p->FreeList));
  p->Text = p->AlignOffset;
  p->HiUnit = p->Text + p->Size;
  p->LoUnit = p->UnitsStart = p->HiUnit - p->Size / 8 / UNIT_SIZE * 7 * UNIT_SIZE;
  p->GlueCount = 0;

  p->OrderFall = p->MaxOrder;
  p->RunLength = p->InitRL = -(Int32)((p->MaxOrder < 12) ? p->MaxOrder : 12) - 1;
  p->PrevSuccess = 0;

  p->MinContext = p->MaxContext = (p->HiUnit -= UNIT_SIZE);
  SET_SUFFIX(p->MinContext, 0);
  SET_NS(p->MinContext, 256);
  SET_SF(p->MinContext, 256 + 1);
  p->FoundState = p->LoUnit;
  p->LoUnit += U2B(256 / 2);
  SET_STATS(p->MinContext, p->FoundState);
  for (i = 0; i < 256; i++)
  {
    CPpmd_State s;
    s.Symbol = (Byte)i;
    s.Freq = 1;
    setSuccLocal(&s, 0);
    putS(p, p->FoundState + i * SSIZE, &s);
  }

  for (i = 0; i < 128; i++)
    for (k = 0; k < 8; k++)
    {
      UInt16 *dest = p->BinSumm[i] + k;
      UInt16 val = (UInt16)(PPMD_BIN_SCALE - kInitBinEsc[k] / (i + 2));
      for (m = 0; m < 64; m += 8)
        dest[m] = val;
    }

  for (i = 0; i < 25; i++)
    for (k = 0; k < 16; k++)
    {
      CPpmd_See *s = &p->See[i][k];
      s->Summ = (UInt16)((5 * i + 10) << (s->Shift = PPMD_PERIOD_BITS - 4));
      s->Count = 4;
    }
}

void Ppmd7v_Init(CPpmd7v *p, unsigned maxOrder)
{
  p->MaxOrder = maxOrder;
  RestartModel(p);
  p->DummySee.Shift = PPMD_PERIOD_BITS;
  p->DummySee.Summ = 0;
  p->DummySee.Count = 64;
}

static UInt32 CreateSuccessors(CPpmd7v *p, Bool skip)
{
  CPpmd_State upState;
  UInt32 c = p->MinContext;
  UInt32 upBranch = SUCC(p, p->FoundState);
  UInt32 ps[PPMD7_MAX_ORDER];
  unsigned numPs = 0;
  unsigned fsym = SYM(p->FoundState);

  if (!skip)
    ps[numPs++] = p->FoundState;

  while (SUFFIX(c))
  {
    UInt32 successor, s;
    c = SUFFIX(c);
    if (NS(c) != 1)
    {
      for (s = STATS(c); SYM(s) != fsym; s += SSIZE);
    }
    else
      s = ONE_STATE(c);
    successor = SUCC(p, s);
    if (successor != upBranch)
    {
      c = successor;
      if (numPs == 0)
        return c;
      break;
    }
    if (numPs >= PPMD7_MAX_ORDER)
      return 0; /* corrupt model */
    ps[numPs++] = s;
  }

  upState.Symbol = (Byte)R8(p, upBranch);
  setSuccLocal(&upState, upBranch + 1);

  if (NS(c) == 1)
    upState.Freq = (Byte)FREQ(ONE_STATE(c));
  else
  {
    UInt32 cf, s0, s;
    for (s = STATS(c); SYM(s) != upState.Symbol; s += SSIZE);
    cf = FREQ(s) - 1;
    s0 = SF(c) - NS(c) - cf;
    upState.Freq = (Byte)(1 + ((2 * cf <= s0) ? (5 * cf > s0) : ((2 * cf + 3 * s0 - 1) / (2 * s0))));
  }

  while (numPs != 0)
  {
    UInt32 c1;
    if (p->HiUnit != p->LoUnit)
      c1 = (p->HiUnit -= UNIT_SIZE);
    else if (p->FreeList[0] != 0)
      c1 = RemoveNode(p, 0);
    else
    {
      c1 = AllocUnitsRare(p, 0);
      if (!c1)
        return 0;
    }
    SET_NS(c1, 1);
    putS(p, ONE_STATE(c1), &upState);
    SET_SUFFIX(c1, c);
    SET_SUCC(p, ps[--numPs], c1);
    c = c1;
  }

  return c;
}

static void UpdateModel(CPpmd7v *p)
{
  UInt32 successor, fSuccessor = SUCC(p, p->FoundState);
  UInt32 c;
  unsigned s0, ns;
  unsigned fsym = SYM(p->FoundState);

  if (FREQ(p->FoundState) < MAX_FREQ / 4 && SUFFIX(p->MinContext) != 0)
  {
    c = SUFFIX(p->MinContext);

    if (NS(c) == 1)
    {
      UInt32 s = ONE_STATE(c);
      if (FREQ(s) < 32)
        SET_FREQ(s, FREQ(s) + 1);
    }
    else
    {
      UInt32 s = STATS(c);
      if (SYM(s) != fsym)
      {
        do { s += SSIZE; } while (SYM(s) != fsym);
        if (FREQ(s) >= FREQ(s - SSIZE))
        {
          swapS(p, s, s - SSIZE);
          s -= SSIZE;
        }
      }
      if (FREQ(s) < MAX_FREQ - 9)
      {
        SET_FREQ(s, FREQ(s) + 2);
        SET_SF(c, SF(c) + 2);
      }
    }
  }

  if (p->OrderFall == 0)
  {
    p->MinContext = p->MaxContext = CreateSuccessors(p, True);
    if (p->MinContext == 0)
    {
      RestartModel(p);
      return;
    }
    SET_SUCC(p, p->FoundState, p->MinContext);
    return;
  }

  W8(p, p->Text, fsym);
  p->Text++;
  successor = p->Text;
  if (p->Text >= p->UnitsStart)
  {
    RestartModel(p);
    return;
  }

  if (fSuccessor)
  {
    if (fSuccessor <= successor)
    {
      UInt32 cs = CreateSuccessors(p, False);
      if (cs == 0)
      {
        RestartModel(p);
        return;
      }
      fSuccessor = cs;
    }
    if (--p->OrderFall == 0)
    {
      successor = fSuccessor;
      p->Text -= (p->MaxContext != p->MinContext);
    }
  }
  else
  {
    SET_SUCC(p, p->FoundState, successor);
    fSuccessor = p->MinContext;
  }

  ns = NS(p->MinContext);
  s0 = SF(p->MinContext) - ns - (FREQ(p->FoundState) - 1);

  for (c = p->MaxContext; c != p->MinContext; c = SUFFIX(c))
  {
    unsigned ns1;
    UInt32 cf, sf;
    if ((ns1 = NS(c)) != 1)
    {
      if ((ns1 & 1) == 0)
      {
        unsigned oldNU = ns1 >> 1;
        unsigned i = U2I(oldNU);
        if (i != U2I(oldNU + 1))
        {
          UInt32 ptr = AllocUnits(p, i + 1);
          UInt32 oldPtr;
          if (!ptr)
          {
            RestartModel(p);
            return;
          }
          oldPtr = STATS(c);
          copyUnits(p, ptr, oldPtr, oldNU);
          InsertNode(p, oldPtr, i);
          SET_STATS(c, ptr);
        }
      }
      {
        unsigned sfc = SF(c);
        SET_SF(c, sfc + (2 * ns1 < ns) + 2 * ((4 * ns1 <= ns) & (sfc <= 8 * ns1)));
      }
    }
    else
    {
      CPpmd_State st;
      UInt32 s = AllocUnits(p, 0);
      if (!s)
      {
        RestartModel(p);
        return;
      }
      getS(p, ONE_STATE(c), &st);
      putS(p, s, &st);
      SET_STATS(c, s);
      if (st.Freq < MAX_FREQ / 4 - 1)
        st.Freq = (Byte)(st.Freq << 1);
      else
        st.Freq = MAX_FREQ - 4;
      SET_FREQ(s, st.Freq);
      SET_SF(c, st.Freq + p->InitEsc + (ns > 3));
    }
    cf = 2 * (UInt32)FREQ(p->FoundState) * (SF(c) + 6);
    sf = (UInt32)s0 + SF(c);
    if (cf < 6 * sf)
    {
      cf = 1 + (cf > sf) + (cf >= 4 * sf);
      SET_SF(c, SF(c) + 3);
    }
    else
    {
      cf = 4 + (cf >= 9 * sf) + (cf >= 12 * sf) + (cf >= 15 * sf);
      SET_SF(c, SF(c) + cf);
    }
    {
      UInt32 s = STATS(c) + ns1 * SSIZE;
      CPpmd_State st;
      st.Symbol = (Byte)fsym;
      st.Freq = (Byte)cf;
      setSuccLocal(&st, successor);
      putS(p, s, &st);
      SET_NS(c, ns1 + 1);
    }
  }
  p->MaxContext = p->MinContext = fSuccessor;
}

static void Rescale(CPpmd7v *p)
{
  unsigned i, adder, sumFreq, escFreq, f;
  UInt32 stats = STATS(p->MinContext);
  UInt32 s = p->FoundState;
  {
    CPpmd_State tmp;
    getS(p, s, &tmp);
    for (; s != stats; s -= SSIZE)
      copyS(p, s, s - SSIZE);
    putS(p, s, &tmp);
  }
  escFreq = SF(p->MinContext) - FREQ(s);
  f = (Byte)(FREQ(s) + 4);
  adder = (p->OrderFall != 0);
  f = (Byte)((f + adder) >> 1);
  SET_FREQ(s, f);
  sumFreq = f;

  i = NS(p->MinContext) - 1;
  do
  {
    s += SSIZE;
    escFreq -= FREQ(s);
    f = (Byte)((FREQ(s) + adder) >> 1);
    SET_FREQ(s, f);
    sumFreq += f;
    if (f > FREQ(s - SSIZE))
    {
      UInt32 s1 = s;
      CPpmd_State tmp;
      getS(p, s1, &tmp);
      do
      {
        copyS(p, s1, s1 - SSIZE);
        s1 -= SSIZE;
      }
      while (s1 != stats && tmp.Freq > FREQ(s1 - SSIZE));
      putS(p, s1, &tmp);
    }
  }
  while (--i);

  if (FREQ(s) == 0)
  {
    unsigned numStats = NS(p->MinContext);
    unsigned n0, n1;
    do { i++; s -= SSIZE; } while (FREQ(s) == 0);
    escFreq += i;
    SET_NS(p->MinContext, NS(p->MinContext) - i);
    if (NS(p->MinContext) == 1)
    {
      CPpmd_State tmp;
      getS(p, stats, &tmp);
      do
      {
        tmp.Freq = (Byte)(tmp.Freq - (tmp.Freq >> 1));
        escFreq >>= 1;
      }
      while (escFreq > 1);
      InsertNode(p, stats, U2I(((numStats + 1) >> 1)));
      p->FoundState = ONE_STATE(p->MinContext);
      putS(p, p->FoundState, &tmp);
      return;
    }
    n0 = (numStats + 1) >> 1;
    n1 = (NS(p->MinContext) + 1) >> 1;
    if (n0 != n1)
      SET_STATS(p->MinContext, ShrinkUnits(p, stats, n0, n1));
  }
  SET_SF(p->MinContext, sumFreq + escFreq - (escFreq >> 1));
  p->FoundState = STATS(p->MinContext);
}

static CPpmd_See *MakeEscFreq(CPpmd7v *p, unsigned numMasked, UInt32 *escFreq)
{
  CPpmd_See *see;
  unsigned mns = NS(p->MinContext);
  unsigned nonMasked = mns - numMasked;
  if (mns != 256)
  {
    see = p->See[p->NS2Indx[nonMasked - 1]] +
        (nonMasked < (unsigned)NS(SUFFIX(p->MinContext)) - mns) +
        2 * (SF(p->MinContext) < 11 * mns) +
        4 * (numMasked > nonMasked) +
        p->HiBitsFlag;
    {
      unsigned r = (see->Summ >> see->Shift);
      see->Summ = (UInt16)(see->Summ - r);
      *escFreq = r + (r == 0);
    }
  }
  else
  {
    see = &p->DummySee;
    *escFreq = 1;
  }
  return see;
}

static void NextContext(CPpmd7v *p)
{
  UInt32 c = SUCC(p, p->FoundState);
  if (p->OrderFall == 0 && c > p->Text)
    p->MinContext = p->MaxContext = c;
  else
    UpdateModel(p);
}

static void Update1(CPpmd7v *p)
{
  UInt32 s = p->FoundState;
  SET_FREQ(s, FREQ(s) + 4);
  SET_SF(p->MinContext, SF(p->MinContext) + 4);
  if (FREQ(s) > FREQ(s - SSIZE))
  {
    swapS(p, s, s - SSIZE);
    p->FoundState = s -= SSIZE;
    if (FREQ(s) > MAX_FREQ)
      Rescale(p);
  }
  NextContext(p);
}

static void Update1_0(CPpmd7v *p)
{
  unsigned f;
  p->PrevSuccess = (2 * FREQ(p->FoundState) > SF(p->MinContext));
  p->RunLength += p->PrevSuccess;
  SET_SF(p->MinContext, SF(p->MinContext) + 4);
  f = (Byte)(FREQ(p->FoundState) + 4);
  SET_FREQ(p->FoundState, f);
  if (f > MAX_FREQ)
    Rescale(p);
  NextContext(p);
}

static void UpdateBin(CPpmd7v *p)
{
  unsigned f = FREQ(p->FoundState);
  SET_FREQ(p->FoundState, f + (f < 128 ? 1 : 0));
  p->PrevSuccess = 1;
  p->RunLength++;
  NextContext(p);
}

static void Update2(CPpmd7v *p)
{
  unsigned f;
  SET_SF(p->MinContext, SF(p->MinContext) + 4);
  f = (Byte)(FREQ(p->FoundState) + 4);
  SET_FREQ(p->FoundState, f);
  if (f > MAX_FREQ)
    Rescale(p);
  p->RunLength = p->InitRL;
  UpdateModel(p);
}

#define MASK(sym) ((signed char *)charMask)[sym]

int Ppmd7v_DecodeSymbol(CPpmd7v *p, IPpmd7_RangeDec *rc)
{
  size_t charMask[256 / sizeof(size_t)];
  if (p->io_error)
    return -2;
  if (ppmd7v_yield && (++yield_count & 2047) == 0)
    ppmd7v_yield();
  if (NS(p->MinContext) != 1)
  {
    UInt32 s = STATS(p->MinContext);
    unsigned i;
    UInt32 count, hiCnt;
    if ((count = rc->GetThreshold(rc, SF(p->MinContext))) < (hiCnt = FREQ(s)))
    {
      Byte symbol;
      rc->Decode(rc, 0, FREQ(s));
      p->FoundState = s;
      symbol = (Byte)SYM(s);
      Update1_0(p);
      return symbol;
    }
    p->PrevSuccess = 0;
    i = NS(p->MinContext) - 1;
    do
    {
      s += SSIZE;
      if ((hiCnt += FREQ(s)) > count)
      {
        Byte symbol;
        rc->Decode(rc, hiCnt - FREQ(s), FREQ(s));
        p->FoundState = s;
        symbol = (Byte)SYM(s);
        Update1(p);
        return symbol;
      }
    }
    while (--i);
    if (count >= SF(p->MinContext))
      return -2;
    p->HiBitsFlag = p->HB2Flag[SYM(p->FoundState)];
    rc->Decode(rc, hiCnt, SF(p->MinContext) - hiCnt);
    PPMD_SetAllBitsIn256Bytes(charMask);
    MASK(SYM(s)) = 0;
    i = NS(p->MinContext) - 1;
    do { s -= SSIZE; MASK(SYM(s)) = 0; } while (--i);
  }
  else
  {
    UInt16 *prob;
    {
      UInt32 one = ONE_STATE(p->MinContext);
      unsigned ofreq = FREQ(one), osym = SYM(one);
      unsigned sns = NS(SUFFIX(p->MinContext));
      p->HiBitsFlag = p->HB2Flag[SYM(p->FoundState)];
      prob = &p->BinSumm[ofreq - 1][p->PrevSuccess +
          p->NS2BSIndx[sns - 1] +
          p->HiBitsFlag +
          2 * p->HB2Flag[osym] +
          ((p->RunLength >> 26) & 0x20)];
    }
    if (rc->DecodeBit(rc, *prob) == 0)
    {
      Byte symbol;
      *prob = (UInt16)PPMD_UPDATE_PROB_0(*prob);
      p->FoundState = ONE_STATE(p->MinContext);
      symbol = (Byte)SYM(p->FoundState);
      UpdateBin(p);
      return symbol;
    }
    *prob = (UInt16)PPMD_UPDATE_PROB_1(*prob);
    p->InitEsc = PPMD7_kExpEscape[*prob >> 10];
    PPMD_SetAllBitsIn256Bytes(charMask);
    MASK(SYM(ONE_STATE(p->MinContext))) = 0;
    p->PrevSuccess = 0;
  }
  for (;;)
  {
    UInt32 ps[256], s;
    UInt32 freqSum, count, hiCnt;
    CPpmd_See *see;
    unsigned i, num, numMasked = NS(p->MinContext);
    do
    {
      p->OrderFall++;
      if (!SUFFIX(p->MinContext))
        return -1;
      p->MinContext = SUFFIX(p->MinContext);
    }
    while (NS(p->MinContext) == numMasked);
    hiCnt = 0;
    s = STATS(p->MinContext);
    i = 0;
    num = NS(p->MinContext) - numMasked;
    do
    {
      int k = (int)(MASK(SYM(s)));
      hiCnt += (FREQ(s) & k);
      ps[i] = s;
      s += SSIZE;
      i -= k;
    }
    while (i != num);

    see = MakeEscFreq(p, numMasked, &freqSum);
    freqSum += hiCnt;
    count = rc->GetThreshold(rc, freqSum);

    if (count < hiCnt)
    {
      Byte symbol;
      unsigned k = 0;
      for (hiCnt = 0; (hiCnt += FREQ(ps[k])) <= count; k++);
      s = ps[k];
      rc->Decode(rc, hiCnt - FREQ(s), FREQ(s));
      Ppmd_See_Update(see);
      p->FoundState = s;
      symbol = (Byte)SYM(s);
      Update2(p);
      return symbol;
    }
    if (count >= freqSum)
      return -2;
    rc->Decode(rc, hiCnt, freqSum - hiCnt);
    see->Summ = (UInt16)(see->Summ + freqSum);
    do { MASK(SYM(ps[--i])) = 0; } while (i != 0);
  }
}
