/*
 * PROJECT:     WinDosDX Universal CRT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     rint/nearbyint/lrint/llrint families (current rounding mode)
 * COPYRIGHT:   Copyright 2026 WinDosDX Team & Contributors
 */

/*
 * These were spec stubs (rint, nearbyint, llrint) or called __debugbreak()
 * (lrint), which crashed any program using them.
 *
 * Adding and subtracting 2^52 (2^23 for float) forces the FPU to round away
 * the fraction using the *current* rounding mode, which is exactly what
 * rint() must do, without needing SSE4.1's roundsd.
 */

#include <fenv.h>
#include <float.h>
#include <limits.h>
#include <math.h>

#pragma fenv_access(on)

#ifdef _MSC_VER
#pragma function(lrint)
#pragma function(lrintf)
#endif

double __cdecl rint(double x)
{
    static const double two52 = 4503599627370496.0; /* 2^52 */
    volatile double t;

    if (!(fabs(x) < two52))
        return x; /* already integral, infinite or NaN */

    if (x > 0.0)
    {
        t = x + two52;
        t = t - two52;
    }
    else if (x < 0.0)
    {
        t = x - two52;
        t = t + two52;
    }
    else
    {
        return x; /* +0.0 / -0.0 */
    }
    /* Keep the sign of the argument for results that round to zero. */
    return copysign(t, x);
}

float __cdecl rintf(float x)
{
    static const float two23 = 8388608.0f; /* 2^23 */
    volatile float t;

    if (!(fabsf(x) < two23))
        return x;

    if (x > 0.0f)
    {
        t = x + two23;
        t = t - two23;
    }
    else if (x < 0.0f)
    {
        t = x - two23;
        t = t + two23;
    }
    else
    {
        return x;
    }
    return copysignf(t, x);
}

/* nearbyint: like rint, but must not raise FE_INEXACT. */
double __cdecl nearbyint(double x)
{
    fexcept_t saved;
    double r;

    fegetexceptflag(&saved, FE_INEXACT);
    r = rint(x);
    fesetexceptflag(&saved, FE_INEXACT);
    return r;
}

float __cdecl nearbyintf(float x)
{
    fexcept_t saved;
    float r;

    fegetexceptflag(&saved, FE_INEXACT);
    r = rintf(x);
    fesetexceptflag(&saved, FE_INEXACT);
    return r;
}

/* Out-of-range and NaN results: raise FE_INVALID and return the "integer
 * indefinite" value, as the Microsoft CRT does. */
long __cdecl lrint(double x)
{
    double r = rint(x);
    if (!(r >= (double)LONG_MIN && r <= (double)LONG_MAX))
    {
        feraiseexcept(FE_INVALID);
        return LONG_MIN;
    }
    return (long)r;
}

long __cdecl lrintf(float x)
{
    return lrint((double)x);
}

long long __cdecl llrint(double x)
{
    double r = rint(x);
    /* (double)LLONG_MAX rounds up to 2^63, so compare with < */
    if (!(r >= (double)LLONG_MIN && r < 9223372036854775808.0))
    {
        feraiseexcept(FE_INVALID);
        return LLONG_MIN;
    }
    return (long long)r;
}

long long __cdecl llrintf(float x)
{
    return llrint((double)x);
}
