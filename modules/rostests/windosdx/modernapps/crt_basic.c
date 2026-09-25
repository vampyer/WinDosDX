/*
 * PROJECT:     WinDosDX modern application tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Universal CRT basics. Built twice by build.cmd: /MD (dynamic
 *              UCRT through api-ms-win-crt-*.dll) and /MT (static CRT).
 * COPYRIGHT:   Copyright 2026 WinDosDX Team & Contributors
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <locale.h>
#include <math.h>

static int failures;

#define CHECK(expr) \
    do { if (!(expr)) { printf("  FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while (0)

static int compare_ints(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

int main(void)
{
    char path[512];
    char buffer[64];
    int values[] = { 5, 3, 9, 1, 7 };
    FILE *fp;
    time_t now;
    const char *temp;

    printf("%s\n", TEST_NAME);

    /* Heap and strings */
    char *copy = _strdup("WinDosDX");
    CHECK(copy && strcmp(copy, "WinDosDX") == 0);
    free(copy);
    CHECK(snprintf(buffer, sizeof(buffer), "%d-%s-%.2f", 42, "x", 3.14159) == 9);
    CHECK(strcmp(buffer, "42-x-3.14") == 0);

    /* Conversions and math */
    CHECK(strtol("7fff", NULL, 16) == 0x7fff);
    CHECK(fabs(strtod("2.5e3", NULL) - 2500.0) < 1e-9);
    CHECK(fabs(sqrt(2.0) * sqrt(2.0) - 2.0) < 1e-12);

    /* qsort */
    qsort(values, 5, sizeof(int), compare_ints);
    CHECK(values[0] == 1 && values[4] == 9);

    /* Locale and time */
    CHECK(setlocale(LC_ALL, "C") != NULL);
    now = time(NULL);
    CHECK(now > 0);
    CHECK(strftime(buffer, sizeof(buffer), "%Y", localtime(&now)) == 4);

    /* File I/O in %TEMP% */
    temp = getenv("TEMP");
    snprintf(path, sizeof(path), "%s\\wdx_crt_%s.txt", temp ? temp : ".", TEST_NAME);
    fp = fopen(path, "w+");
    CHECK(fp != NULL);
    if (fp)
    {
        fprintf(fp, "line %d\n", 1);
        rewind(fp);
        CHECK(fgets(buffer, sizeof(buffer), fp) && strcmp(buffer, "line 1\n") == 0);
        fclose(fp);
        remove(path);
    }

    printf("%s %s\n", failures ? "FAIL" : "PASS", TEST_NAME);
    return failures ? 1 : 0;
}
