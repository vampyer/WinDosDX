/*
 * PROJECT:     WinDosDX Universal CRT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     C99 <fenv.h> floating-point environment functions
 * COPYRIGHT:   Copyright 2026 WinDosDX Team & Contributors
 */

/*
 * These used to be spec stubs that returned 0 without doing anything, so a
 * program that changed the rounding mode or saved and restored the
 * floating-point environment silently computed with the wrong settings.
 *
 * Compilers targeting x86 (SSE2) and x64 do their floating-point math in
 * SSE registers, so exception flags are read and written through MXCSR;
 * control settings go through _controlfp, which programs both units.
 *
 * fenv_t._Fe_ctl layout (compatible with the FE_DFL_ENV constant that
 * programs embed from <fenv.h>: 0x3f3f103f on x86, 0x3f00003f on x64):
 *   bits 0-5  exception masks (FE_* order, bit 5 = denormal); set = masked
 *   bits 8-9  rounding mode (_RC_NEAR/_RC_DOWN/_RC_UP/_RC_CHOP)
 * fenv_t._Fe_stat holds the raised FE_* exception flags.
 */

#include <float.h>
#include <fenv.h>
#include <xmmintrin.h>

#define FENV_CTL_MASKS  0x3f
#define FENV_CTL_ROUND  0x300

/* MXCSR flag bits for FE_INEXACT, FE_UNDERFLOW, FE_OVERFLOW, FE_DIVBYZERO, FE_INVALID */
static const unsigned int MxcsrFlag[5] = { 0x20, 0x10, 0x08, 0x04, 0x01 };

static unsigned int FeToMxcsrFlags(int excepts)
{
    unsigned int flags = 0;
    int i;
    for (i = 0; i < 5; i++)
    {
        if (excepts & (1 << i))
            flags |= MxcsrFlag[i];
    }
    return flags;
}

static int MxcsrFlagsToFe(unsigned int mxcsr)
{
    int excepts = 0;
    int i;
    for (i = 0; i < 5; i++)
    {
        if (mxcsr & MxcsrFlag[i])
            excepts |= (1 << i);
    }
    return excepts;
}

/* The raised exceptions of both units, as FE_* bits. */
static int CurrentExceptions(void)
{
    /* _SW_* status bits have the same values as FE_* */
    return (MxcsrFlagsToFe(_mm_getcsr()) | (int)(_statusfp() & FE_ALL_EXCEPT)) & FE_ALL_EXCEPT;
}

static void ClearExceptions(int excepts)
{
    _mm_setcsr(_mm_getcsr() & ~FeToMxcsrFlags(excepts));
    /* The x87 status word can only be cleared as a whole. */
    if ((excepts & FE_ALL_EXCEPT) == FE_ALL_EXCEPT)
        _clearfp();
}

static void RaiseFlags(int excepts)
{
    /* Set the flags without trapping, as fesetexceptflag requires. */
    _mm_setcsr(_mm_getcsr() | FeToMxcsrFlags(excepts));
}

int __cdecl fegetround(void)
{
    return (int)(_controlfp(0, 0) & _MCW_RC);
}

int __cdecl fesetround(int round)
{
    if (round & ~_MCW_RC)
        return 1;
    _controlfp((unsigned int)round, _MCW_RC);
    return 0;
}

int __cdecl fegetenv(fenv_t *env)
{
    unsigned int cw = _controlfp(0, 0);
    unsigned long masks = cw & (_EM_INEXACT | _EM_UNDERFLOW | _EM_OVERFLOW | _EM_ZERODIVIDE | _EM_INVALID);
    if (cw & _EM_DENORMAL)
        masks |= 0x20;
    env->_Fe_ctl = masks | (cw & _MCW_RC);
    env->_Fe_stat = (unsigned long)CurrentExceptions();
    return 0;
}

int __cdecl fesetenv(const fenv_t *env)
{
    unsigned int cw = env->_Fe_ctl & (_EM_INEXACT | _EM_UNDERFLOW | _EM_OVERFLOW | _EM_ZERODIVIDE | _EM_INVALID);
    if (env->_Fe_ctl & 0x20)
        cw |= _EM_DENORMAL;
    cw |= env->_Fe_ctl & FENV_CTL_ROUND;

    /* Control first: _controlfp rewrites MXCSR and does not preserve the
     * sticky flags on every architecture. Loading MXCSR with a flag set
     * never traps by itself, so restoring the flags afterwards is safe. */
    _controlfp(cw, _MCW_EM | _MCW_RC);
    ClearExceptions(FE_ALL_EXCEPT);
    RaiseFlags((int)(env->_Fe_stat & FE_ALL_EXCEPT));
    return 0;
}

int __cdecl feholdexcept(fenv_t *env)
{
    fegetenv(env);
    ClearExceptions(FE_ALL_EXCEPT);
    /* Non-stop mode: mask every exception. */
    _controlfp(_MCW_EM, _MCW_EM);
    return 0;
}

int __cdecl feclearexcept(int excepts)
{
    if (excepts & ~FE_ALL_EXCEPT)
        return 1;
    ClearExceptions(excepts);
    return 0;
}

int __cdecl fetestexcept(int excepts)
{
    return CurrentExceptions() & excepts & FE_ALL_EXCEPT;
}

int __cdecl fegetexceptflag(fexcept_t *flags, int excepts)
{
    *flags = (fexcept_t)(CurrentExceptions() & excepts & FE_ALL_EXCEPT);
    return 0;
}

int __cdecl fesetexceptflag(const fexcept_t *flags, int excepts)
{
    if (excepts & ~FE_ALL_EXCEPT)
        return 1;
    ClearExceptions(excepts & ~(int)*flags);
    RaiseFlags((int)*flags & excepts);
    return 0;
}
