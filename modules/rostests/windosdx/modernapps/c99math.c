/*
 * PROJECT:     WinDosDX modern application tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     C99 <math.h> functions (exact results and reference values)
 * COPYRIGHT:   Copyright 2026 WinDosDX Team & Contributors
 */

#include <math.h>
#include <stdio.h>

static int failures;

#define CHECK(expr) \
    do { if (!(expr)) { printf("  FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while (0)

static int Near(double got, double want)
{
    double tol = 1e-13 * (fabs(want) > 1e-300 ? fabs(want) : 1e-300);
    if (fabs(got - want) <= tol)
        return 1;
    printf("  got %.17g want %.17g\n", got, want);
    return 0;
}

int main(void)
{
    volatile double m27 = -2.7, big = 1e300;
    int q;

    printf("c99math\n");

    /* exact */
    CHECK(copysign(3.0, -0.0) == -3.0);
    CHECK(copysignf(2.0f, -1.0f) == -2.0f);
    CHECK(trunc(m27) == -2.0 && trunc(2.7) == 2.0 && truncf(-1.5f) == -1.0f);
    CHECK(fmax(1.0, NAN) == 1.0 && fmin(-1.0, 2.0) == -1.0 && fdim(5.0, 3.0) == 2.0 && fdim(3.0, 5.0) == 0.0);
    CHECK(lround(2.5) == 3 && lround(-2.5) == -3 && llround(1e15 + 0.5) == 1000000000000001LL);
    CHECK(ilogb(8.0) == 3 && logb(0.25) == -2.0);
    CHECK(isnan(nan("")) && isnan(nanf("")));
    CHECK(remquo(10.0, 3.0, &q) == 1.0 && (q & 7) == 3);
    CHECK(remainder(7.0, 2.0) == -1.0);           /* 7/2 = 3.5 rounds to 4 */
    CHECK(remainder(big, 3.0) == remainder(big, 3.0));

    /* reference values */
    CHECK(Near(log1p(1e-10), 9.9999999995000000000e-11));
    CHECK(Near(expm1(1e-10), 1.00000000005e-10));
    CHECK(Near(cbrt(27.0), 3.0) && Near(cbrt(-8.0), -2.0));
    CHECK(Near(asinh(1.0), 0.88137358701954302523));
    CHECK(Near(acosh(2.0), 1.3169578969248167086));
    CHECK(Near(atanh(0.5), 0.54930614433405484570));
    CHECK(Near(erf(0.5), 0.52049987781304653768));
    CHECK(Near(erf(2.5), 0.99959304798255504107));
    CHECK(Near(erfc(3.0), 2.2090496998585441373e-05));
    CHECK(Near(tgamma(5.0), 24.0) && Near(tgamma(0.5), 1.7724538509055160273));
    CHECK(Near(tgamma(-1.5), 2.3632718012073547031));
    CHECK(Near(lgamma(10.0), 12.801827480081469611));

    printf("%s c99math\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
