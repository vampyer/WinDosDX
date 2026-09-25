/* Copyright (C) 1995 DJ Delorie, see COPYING.DJ for details */

#include <precomp.h>
#include <float.h>

#define X87_CW_IM   (1<<0)      /* Invalid operation mask */
#define X87_CW_DM   (1<<1)      /* Denormal operand mask */
#define X87_CW_ZM   (1<<2)      /* Zero divide mask */
#define X87_CW_OM   (1<<3)      /* Overflow mask */
#define X87_CW_UM   (1<<4)      /* Underflow mask */
#define X87_CW_PM   (1<<5)      /* Precision mask */

#define X87_CW_PC_MASK     (3<<8)   /* precision control mask */
#define X87_CW_PC24        (0<<8)   /* 24 bit precision */
#define X87_CW_PC53        (2<<8)   /* 53 bit precision */
#define X87_CW_PC64        (3<<8)   /* 64 bit precision */

#define X87_CW_RC_MASK     (3<<10)  /* rounding control mask */
#define X87_CW_RC_NEAREST  (0<<10)  /* round to nearest */
#define X87_CW_RC_DOWN     (1<<10)  /* round down */
#define X87_CW_RC_UP       (2<<10)  /* round up */
#define X87_CW_RC_ZERO     (3<<10)  /* round toward zero (chop) */

#define X87_CW_IC          (1<<12)  /* infinity control flag */

#ifdef _M_AMD64
unsigned int __getfpcw87(void);
void __setfpcw87(unsigned int);
#endif

#ifdef _M_IX86
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <cpuid.h>
#endif

/* SSE2 present? Modern x86 compilers do double math in SSE registers, so
 * the control settings must be applied to MXCSR as well as the x87 unit. */
static int Sse2Supported(void)
{
    static int cached = -1;
    if (cached < 0)
    {
#ifdef _MSC_VER
        int regs[4];
        __cpuid(regs, 1);
        cached = (regs[3] >> 26) & 1;
#else
        unsigned int a, b, c, d;
        cached = __get_cpuid(1, &a, &b, &c, &d) ? (int)((d >> 26) & 1) : 0;
#endif
    }
    return cached;
}

/* Apply the abstract exception masks and rounding mode to MXCSR, keeping
 * the sticky exception flags and the DAZ/FZ denormal bits. */
static void SetSseControl(unsigned int flags)
{
    unsigned int mxcsr;

#if defined(__GNUC__)
    __asm__ __volatile__( "stmxcsr %0" : "=m" (mxcsr) : );
#else
    __asm stmxcsr [mxcsr];
#endif

    mxcsr &= ~(0x1f80 | 0x6000); /* exception masks, rounding control */
    if (flags & _EM_INVALID)    mxcsr |= 0x0080;
    if (flags & _EM_DENORMAL)   mxcsr |= 0x0100;
    if (flags & _EM_ZERODIVIDE) mxcsr |= 0x0200;
    if (flags & _EM_OVERFLOW)   mxcsr |= 0x0400;
    if (flags & _EM_UNDERFLOW)  mxcsr |= 0x0800;
    if (flags & _EM_INEXACT)    mxcsr |= 0x1000;
    switch (flags & (_RC_UP | _RC_DOWN))
    {
    case _RC_UP|_RC_DOWN: mxcsr |= 0x6000; break;
    case _RC_UP:          mxcsr |= 0x4000; break;
    case _RC_DOWN:        mxcsr |= 0x2000; break;
    }

#if defined(__GNUC__)
    __asm__ __volatile__( "ldmxcsr %0" : : "m" (mxcsr) );
#else
    __asm ldmxcsr [mxcsr];
#endif
}
#endif /* _M_IX86 */

/*
 * @implemented
 */

unsigned int CDECL _controlfp(unsigned int newval, unsigned int mask)
{
  return _control87( newval, mask & ~_EM_DENORMAL );
}

/*********************************************************************
 *		_control87 (MSVCRT.@)
 */
unsigned int CDECL _control87(unsigned int newval, unsigned int mask)
{
  unsigned short fpword = 0;
  unsigned int flags = 0;

  TRACE("(%08x, %08x): Called\n", newval, mask);

  /* Get fp control word */
#ifdef _M_AMD64
  fpword = __getfpcw87();
#elif defined(__GNUC__)
  __asm__ __volatile__( "fstcw %0" : "=m" (fpword) : );
#else
  __asm fstcw [fpword];
#endif

  TRACE("Control word before : %08x\n", fpword);

  /* Convert into mask constants */
  if (fpword & 0x1)  flags |= _EM_INVALID;
  if (fpword & 0x2)  flags |= _EM_DENORMAL;
  if (fpword & 0x4)  flags |= _EM_ZERODIVIDE;
  if (fpword & 0x8)  flags |= _EM_OVERFLOW;
  if (fpword & 0x10) flags |= _EM_UNDERFLOW;
  if (fpword & 0x20) flags |= _EM_INEXACT;
  switch(fpword & 0xC00) {
  case 0xC00: flags |= _RC_UP|_RC_DOWN; break;
  case 0x800: flags |= _RC_UP; break;
  case 0x400: flags |= _RC_DOWN; break;
  }
  switch(fpword & 0x300) {
  case 0x0:   flags |= _PC_24; break;
  case 0x200: flags |= _PC_53; break;
  case 0x300: flags |= _PC_64; break;
  }
  if (fpword & 0x1000) flags |= _IC_AFFINE;

  /* Mask with parameters */
  flags = (flags & ~mask) | (newval & mask);

  /* Convert (masked) value back to fp word */
  fpword = 0;
  if (flags & _EM_INVALID)    fpword |= 0x1;
  if (flags & _EM_DENORMAL)   fpword |= 0x2;
  if (flags & _EM_ZERODIVIDE) fpword |= 0x4;
  if (flags & _EM_OVERFLOW)   fpword |= 0x8;
  if (flags & _EM_UNDERFLOW)  fpword |= 0x10;
  if (flags & _EM_INEXACT)    fpword |= 0x20;
  switch(flags & (_RC_UP | _RC_DOWN)) {
  case _RC_UP|_RC_DOWN: fpword |= 0xC00; break;
  case _RC_UP:          fpword |= 0x800; break;
  case _RC_DOWN:        fpword |= 0x400; break;
  }
  switch (flags & (_PC_24 | _PC_53)) {
  case _PC_64: fpword |= 0x300; break;
  case _PC_53: fpword |= 0x200; break;
  case _PC_24: fpword |= 0x0; break;
  }
  if (flags & _IC_AFFINE) fpword |= 0x1000;

  TRACE("Control word after  : %08x\n", fpword);

  /* Put fp control word */
#ifdef _M_AMD64
  __setfpcw87(fpword);
#elif defined(__GNUC__)
  __asm__ __volatile__( "fldcw %0" : : "m" (fpword) );
#else
  __asm fldcw [fpword];
#endif

#ifdef _M_IX86
  if (Sse2Supported())
      SetSseControl(flags);
#endif

  return flags;
}
