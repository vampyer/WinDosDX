/*
 * PROJECT:     WinDosDX modern application tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     C99 <fenv.h>: rounding modes, exception flags, environments
 * COPYRIGHT:   Copyright 2026 WinDosDX Team & Contributors
 */

#include <fenv.h>
#include <math.h>
#include <stdio.h>

#pragma fenv_access(on)

static int failures;

#define CHECK(expr) \
    do { if (!(expr)) { printf("  FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while (0)

int main(void)
{
    volatile double zero = 0.0, one = 1.0, three = 3.0, x;
    fenv_t env, held;
    fexcept_t saved;

    printf("fenvtest\n");

    /* Rounding modes change results */
    CHECK(fegetround() == FE_TONEAREST);
    CHECK(fesetround(FE_UPWARD) == 0 && fegetround() == FE_UPWARD);
    CHECK(rint(2.1) == 3.0);
    CHECK(fesetround(FE_DOWNWARD) == 0);
    CHECK(rint(2.9) == 2.0);
    CHECK(fesetround(FE_TOWARDZERO) == 0);
    CHECK(rint(-2.9) == -2.0);
    CHECK(fesetround(FE_TONEAREST) == 0);
    CHECK(rint(2.5) == 2.0);

    /* Exception flags */
    CHECK(feclearexcept(FE_ALL_EXCEPT) == 0);
    CHECK(fetestexcept(FE_ALL_EXCEPT) == 0);
    x = one / zero;
    CHECK(fetestexcept(FE_DIVBYZERO) != 0);
    x = one / three;
    CHECK(fetestexcept(FE_INEXACT) != 0);
    CHECK(fegetexceptflag(&saved, FE_ALL_EXCEPT) == 0);
    CHECK(feclearexcept(FE_DIVBYZERO) == 0);
    CHECK(fetestexcept(FE_DIVBYZERO) == 0 && fetestexcept(FE_INEXACT) != 0);
    CHECK(fesetexceptflag(&saved, FE_ALL_EXCEPT) == 0);
    CHECK(fetestexcept(FE_DIVBYZERO) != 0);

    /* Save / hold / restore the whole environment */
    CHECK(fesetround(FE_UPWARD) == 0);
    CHECK(fegetenv(&env) == 0);
    CHECK(feholdexcept(&held) == 0);
    CHECK(fetestexcept(FE_ALL_EXCEPT) == 0);
    CHECK(fesetround(FE_DOWNWARD) == 0);
    CHECK(fesetenv(&env) == 0);
    CHECK(fegetround() == FE_UPWARD);
    CHECK(fetestexcept(FE_DIVBYZERO) != 0);

    /* FE_DFL_ENV is a constant embedded in this program by <fenv.h> */
    CHECK(fesetenv(FE_DFL_ENV) == 0);
    CHECK(fegetround() == FE_TONEAREST);
    CHECK(fetestexcept(FE_ALL_EXCEPT) == 0);
    (void)x;

    printf("%s fenvtest\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
