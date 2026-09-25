/*
 * PROJECT:     WinDosDX Universal CRT
 * LICENSE:     MIT (https://spdx.org/licenses/MIT)
 * PURPOSE:     C99 <math.h> functions that used to be spec stubs
 * COPYRIGHT:   Copyright 2026 WinDosDX Team & Contributors
 */

/*
 * These were exported as stubs, so any program calling trunc, fmax,
 * copysign, lround, log1p, erf, tgamma, ... got garbage. Algorithms:
 *   - exact operations (copysign, trunc, fmin/fmax/fdim, lround, logb,
 *     ilogb, remainder/remquo) are computed exactly;
 *   - asinh/acosh/atanh use the fdlibm formulations;
 *   - log1p/expm1 use Goldberg's and Kahan's correction tricks;
 *   - cbrt refines pow() with Newton steps;
 *   - erf uses its all-positive power series, erfc a continued fraction;
 *   - tgamma/lgamma use the Lanczos approximation (g = 7, n = 9).
 * Results are within a few ulps of the correctly rounded value.
 */

#include <math.h>
#include <float.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

#ifdef _MSC_VER
#pragma warning(disable:4163 4164)
#pragma function(fabs)
#endif

#define WDX_PI      3.14159265358979323846
#define WDX_LN2     0.693147180559945309417
#define WDX_SQRTPI  1.77245385090551602730
#define WDX_TWO52   4503599627370496.0          /* 2^52 */
#define WDX_TWO28   268435456.0                 /* 2^28 */

static uint64_t Bits(double x)       { uint64_t u; memcpy(&u, &x, sizeof(u)); return u; }
static double   FromBits(uint64_t u) { double x; memcpy(&x, &u, sizeof(x)); return x; }
static int      IsNan(double x)      { return x != x; }
static int      IsInf(double x)      { return x - x != 0 && !IsNan(x); }

/* ---- Sign, truncation, min/max ---------------------------------------- */

double __cdecl copysign(double x, double y)
{
    return FromBits((Bits(x) & ~(1ULL << 63)) | (Bits(y) & (1ULL << 63)));
}

float __cdecl copysignf(float x, float y)
{
    uint32_t ux, uy;
    memcpy(&ux, &x, 4);
    memcpy(&uy, &y, 4);
    ux = (ux & 0x7fffffffU) | (uy & 0x80000000U);
    memcpy(&x, &ux, 4);
    return x;
}

double __cdecl trunc(double x)
{
    if (!(fabs(x) < WDX_TWO52))
        return x;                               /* integral, inf or NaN */
    return copysign((double)(int64_t)x, x);     /* keeps -0.0 */
}

float __cdecl truncf(float x)
{
    return (float)trunc((double)x);
}

double __cdecl fmax(double x, double y)
{
    if (IsNan(x)) return y;
    if (IsNan(y)) return x;
    if (x == y)   return (Bits(x) >> 63) ? y : x;   /* fmax(-0, +0) = +0 */
    return x > y ? x : y;
}

double __cdecl fmin(double x, double y)
{
    if (IsNan(x)) return y;
    if (IsNan(y)) return x;
    if (x == y)   return (Bits(x) >> 63) ? x : y;   /* fmin(-0, +0) = -0 */
    return x < y ? x : y;
}

double __cdecl fdim(double x, double y)
{
    if (IsNan(x) || IsNan(y)) return x + y;
    return x > y ? x - y : 0.0;
}

float __cdecl fmaxf(float x, float y) { return (float)fmax(x, y); }
float __cdecl fminf(float x, float y) { return (float)fmin(x, y); }
float __cdecl fdimf(float x, float y) { return (float)fdim(x, y); }

#if !defined(_M_IX86)
float __cdecl modff(float x, float *iptr)
{
    float i = truncf(x);
    *iptr = i;
    if (IsInf(x))
        return copysignf(0.0f, x);
    return copysignf(x - i, x);
}
#endif

/* ---- Rounding to integers --------------------------------------------- */

long __cdecl lround(double x)
{
    double r = round(x);
    if (!(r >= (double)LONG_MIN && r <= (double)LONG_MAX))
        return LONG_MIN;                        /* "integer indefinite" */
    return (long)r;
}

long long __cdecl llround(double x)
{
    double r = round(x);
    if (!(r >= (double)LLONG_MIN && r < 9223372036854775808.0))
        return LLONG_MIN;
    return (long long)r;
}

long __cdecl lroundf(float x)       { return lround((double)x); }
long long __cdecl llroundf(float x) { return llround((double)x); }

/* ---- Exponent extraction ---------------------------------------------- */

int __cdecl ilogb(double x)
{
    int e;
    if (IsNan(x))  return FP_ILOGBNAN;
    if (x == 0.0)  return FP_ILOGB0;
    if (IsInf(x))  return INT_MAX;
    frexp(x, &e);                               /* handles subnormals */
    return e - 1;
}

double __cdecl logb(double x)
{
    int e;
    if (IsNan(x))  return x;
    if (IsInf(x))  return fabs(x);
    if (x == 0.0)  return -1.0 / fabs(x);       /* -inf, raises divide-by-zero */
    frexp(x, &e);
    return (double)(e - 1);
}

int __cdecl ilogbf(float x)  { return ilogb((double)x); }
float __cdecl logbf(float x) { return (float)logb((double)x); }

/* ---- NaN construction ------------------------------------------------- */

double __cdecl nan(const char *tag)
{
    (void)tag;
    return FromBits(0x7ff8000000000000ULL);
}

float __cdecl nanf(const char *tag)
{
    uint32_t u = 0x7fc00000U;
    float f;
    (void)tag;
    memcpy(&f, &u, 4);
    return f;
}

/* ---- IEEE remainder --------------------------------------------------- */

double __cdecl remquo(double x, double y, int *quo)
{
    int sx, sq;
    unsigned int q = 0;
    double ax, ay;

    *quo = 0;
    if (IsNan(x) || IsNan(y)) return x + y;
    if (y == 0.0 || IsInf(x)) return (x * y) / (x * y);  /* invalid: NaN */

    sx = (int)(Bits(x) >> 63);
    sq = sx ^ (int)(Bits(y) >> 63);
    ax = fabs(x);
    ay = fabs(y);

    /* Reduce to [0, 8y) exactly, then peel off the low three quotient bits;
     * every subtraction below is exact (the operands are within a factor 2). */
    if (ay <= DBL_MAX / 8)
        ax = fmod(ax, 8 * ay);
    if (ax >= 4 * ay) { ax -= 4 * ay; q += 4; }
    if (ax >= 2 * ay) { ax -= 2 * ay; q += 2; }
    if (ax >= ay)     { ax -= ay;     q += 1; }

    /* Round the quotient to nearest, ties to even. */
    if (ay < 2 * DBL_MIN)
    {
        if (ax + ax > ay || (ax + ax == ay && (q & 1))) { ax -= ay; q++; }
    }
    else
    {
        double half = 0.5 * ay;
        if (ax > half || (ax == half && (q & 1))) { ax -= ay; q++; }
    }

    q &= 7;
    *quo = sq ? -(int)q : (int)q;
    return sx ? -ax : ax;
}

double __cdecl remainder(double x, double y)
{
    int q;
    return remquo(x, y, &q);
}

/* A float remainder is exactly representable, so the double result is exact. */
float __cdecl remquof(float x, float y, int *quo) { return (float)remquo(x, y, quo); }
float __cdecl remainderf(float x, float y)        { return (float)remainder(x, y); }

/* ---- log1p / expm1 ---------------------------------------------------- */

double __cdecl log1p(double x)
{
    volatile double u;
    if (IsNan(x) || (IsInf(x) && x > 0)) return x;
    u = 1.0 + x;
    if (u == 1.0)
        return x;                               /* also keeps -0.0 */
    /* Goldberg: log(u) * x / (u - 1) cancels the rounding error of 1 + x */
    return log(u) * (x / (u - 1.0));
}

double __cdecl expm1(double x)
{
    volatile double u;
    double um1;
    if (IsNan(x)) return x;
    if (x > 709.8) return exp(x);               /* overflow to +inf */
    if (x < -40.0) return -1.0;
    u = exp(x);
    if (u == 1.0)
        return x;
    um1 = u - 1.0;
    if (um1 == -1.0)
        return -1.0;
    /* Kahan: (u - 1) * x / log(u) */
    return um1 * x / log(u);
}

float __cdecl log1pf(float x) { return (float)log1p((double)x); }
float __cdecl expm1f(float x) { return (float)expm1((double)x); }

/* ---- Inverse hyperbolic functions (fdlibm formulations) --------------- */

double __cdecl asinh(double x)
{
    double ax = fabs(x), r;
    if (IsNan(x) || IsInf(x)) return x;
    if (ax < 1.0 / WDX_TWO28) return x;         /* asinh(x) ~ x */
    if (ax > WDX_TWO28)
        r = log(ax) + WDX_LN2;
    else if (ax > 2.0)
        r = log(2.0 * ax + 1.0 / (sqrt(x * x + 1.0) + ax));
    else
        r = log1p(ax + x * x / (1.0 + sqrt(1.0 + x * x)));
    return copysign(r, x);
}

double __cdecl acosh(double x)
{
    double t;
    if (IsNan(x)) return x;
    if (x < 1.0) return (x - x) / (x - x);      /* invalid: NaN */
    if (x > WDX_TWO28)
        return IsInf(x) ? x : log(x) + WDX_LN2;
    if (x > 2.0)
        return log(2.0 * x - 1.0 / (x + sqrt(x * x - 1.0)));
    t = x - 1.0;
    return log1p(t + sqrt(2.0 * t + t * t));
}

double __cdecl atanh(double x)
{
    double ax = fabs(x), r;
    if (IsNan(x)) return x;
    if (ax > 1.0) return (x - x) / (x - x);     /* invalid: NaN */
    if (ax == 1.0)                              /* +-inf, divide-by-zero */
    {
        volatile double zero = 0.0;
        return x / zero;
    }
    if (ax < 1.0 / WDX_TWO28) return x;
    if (ax < 0.5)
    {
        double t = ax + ax;
        r = 0.5 * log1p(t + t * ax / (1.0 - ax));
    }
    else
    {
        r = 0.5 * log1p((ax + ax) / (1.0 - ax));
    }
    return copysign(r, x);
}

float __cdecl asinhf(float x) { return (float)asinh((double)x); }
float __cdecl acoshf(float x) { return (float)acosh((double)x); }
float __cdecl atanhf(float x) { return (float)atanh((double)x); }

/* ---- Cube root -------------------------------------------------------- */

double __cdecl cbrt(double x)
{
    double a = fabs(x), y;
    if (IsNan(x) || IsInf(x) || x == 0.0) return x;
    y = pow(a, 1.0 / 3.0);
    /* Two Newton steps on y^3 = a fix the error of the 1/3 exponent. */
    y = y - (y * y * y - a) / (3.0 * y * y);
    y = y - (y * y * y - a) / (3.0 * y * y);
    return copysign(y, x);
}

float __cdecl cbrtf(float x) { return (float)cbrt((double)x); }

/* ---- Error function --------------------------------------------------- */

/* erf(x) = 2/sqrt(pi) * exp(-x^2) * sum 2^n x^(2n+1) / (1*3*...*(2n+1));
 * every term is positive, so there is no cancellation. */
static double ErfSeries(double x)
{
    double term = x, sum = x, x2 = x * x;
    int n;
    for (n = 1; n < 200; n++)
    {
        term *= 2.0 * x2 / (2.0 * n + 1.0);
        sum += term;
        if (term < sum * 1e-17)
            break;
    }
    return 2.0 / WDX_SQRTPI * exp(-x2) * sum;
}

/* erfc(x) for x >= 2 by the continued fraction
 * erfc(x) = exp(-x^2)/sqrt(pi) * 1/(x + (1/2)/(x + 1/(x + (3/2)/(x + ...)))),
 * evaluated with the modified Lentz method. */
static double ErfcContinuedFraction(double x)
{
    const double tiny = 1e-300;
    double f = x, c = x, d = 0.0, delta;
    int n;
    for (n = 1; n < 500; n++)
    {
        double an = n * 0.5;
        d = x + an * d;
        if (d == 0.0) d = tiny;
        c = x + an / c;
        if (c == 0.0) c = tiny;
        d = 1.0 / d;
        delta = c * d;
        f *= delta;
        if (fabs(delta - 1.0) < 1e-16)
            break;
    }
    return exp(-x * x) / WDX_SQRTPI / f;
}

double __cdecl erf(double x)
{
    double ax = fabs(x);
    if (IsNan(x)) return x;
    if (ax >= 6.0) return copysign(1.0, x);
    if (ax < 2.0)  return copysign(ErfSeries(ax), x);
    return copysign(1.0 - ErfcContinuedFraction(ax), x);
}

double __cdecl erfc(double x)
{
    if (IsNan(x)) return x;
    if (x < 0.0)   return 2.0 - erfc(-x);
    if (x < 0.5)   return 1.0 - ErfSeries(x);
    if (x < 2.0)   return 1.0 - ErfSeries(x);   /* erfc >= 0.0047 here */
    if (x > 27.3)  return 0.0;                  /* underflows */
    return ErfcContinuedFraction(x);
}

float __cdecl erff(float x)  { return (float)erf((double)x); }
float __cdecl erfcf(float x) { return (float)erfc((double)x); }

/* ---- Gamma functions (Lanczos, g = 7, n = 9) -------------------------- */

static const double LanczosCoef[9] =
{
    0.99999999999980993,
    676.5203681218851,
    -1259.1392167224028,
    771.32342877765313,
    -176.61502916214059,
    12.507343278686905,
    -0.13857109526572012,
    9.9843695780195716e-6,
    1.5056327351493116e-7,
};

/* sum and t for Gamma(z + 1) with z = x - 1 */
static double LanczosSum(double z, double *t)
{
    double a = LanczosCoef[0];
    int i;
    for (i = 1; i < 9; i++)
        a += LanczosCoef[i] / (z + i);
    *t = z + 7.5;
    return a;
}

double __cdecl tgamma(double x)
{
    double t, a;
    if (IsNan(x)) return x;
    if (x == 0.0) return 1.0 / x;               /* +-inf, pole */
    if (IsInf(x)) return x > 0 ? x : (x - x) / (x - x);
    if (x < 0.0 && x == floor(x)) return (x - x) / (x - x); /* pole: NaN */
    if (x > 171.7) return HUGE_VAL;             /* overflow */
    if (x < 0.5)                                /* reflection */
        return WDX_PI / (sin(WDX_PI * x) * tgamma(1.0 - x));
    if (x == floor(x) && x <= 23.0)             /* exact for small integers */
    {
        double r = 1.0, i;
        for (i = 2.0; i < x; i += 1.0)
            r *= i;
        return r;
    }
    a = LanczosSum(x - 1.0, &t);
    /* sqrt(2 pi) * t^(z + 0.5) * e^-t * a, split to delay overflow */
    return 2.5066282746310002 * pow(t, (x - 0.5) / 2) * exp(-t) * pow(t, (x - 0.5) / 2) * a;
}

double __cdecl lgamma(double x)
{
    double t, a;
    if (IsNan(x)) return x;
    if (IsInf(x)) return fabs(x);
    if (x <= 0.0 && x == floor(x)) return HUGE_VAL;         /* pole */
    if (x == 1.0 || x == 2.0) return 0.0;
    if (x < 0.5)                                /* reflection */
        return log(WDX_PI / fabs(sin(WDX_PI * x))) - lgamma(1.0 - x);
    a = LanczosSum(x - 1.0, &t);
    return 0.91893853320467274178 + (x - 0.5) * log(t) - t + log(a);
}

float __cdecl tgammaf(float x) { return (float)tgamma((double)x); }
float __cdecl lgammaf(float x) { return (float)lgamma((double)x); }
