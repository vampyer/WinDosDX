/*
 * PROJECT:     WinDosDX modern application tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Vista/Windows 7 APIs that modern programs import directly,
 *              plus the version a subsystem-6.x program is told it runs on.
 * COPYRIGHT:   Copyright 2026 WinDosDX Team & Contributors
 */

#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <stdio.h>

static int failures;

#define CHECK(expr) \
    do { if (!(expr)) { printf("  FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while (0)

static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;
static LONG g_onceCalls;
static SRWLOCK g_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_cond = CONDITION_VARIABLE_INIT;
static LONG g_counter;
static BOOL g_ready;

static BOOL CALLBACK InitOnceCallback(PINIT_ONCE once, PVOID param, PVOID *context)
{
    InterlockedIncrement(&g_onceCalls);
    return TRUE;
}

static DWORD WINAPI Worker(LPVOID param)
{
    int i;
    InitOnceExecuteOnce(&g_once, InitOnceCallback, NULL, NULL);
    for (i = 0; i < 1000; i++)
    {
        AcquireSRWLockExclusive(&g_lock);
        g_counter++;
        ReleaseSRWLockExclusive(&g_lock);
    }
    AcquireSRWLockExclusive(&g_lock);
    while (!g_ready)
        SleepConditionVariableSRW(&g_cond, &g_lock, INFINITE, 0);
    ReleaseSRWLockExclusive(&g_lock);
    return 0;
}

int main(void)
{
    HANDLE threads[4];
    OSVERSIONINFOEXW osvi = { sizeof(osvi) };
    DWORDLONG mask = 0;
    WCHAR path[MAX_PATH], final[MAX_PATH];
    HANDLE hFile;
    ULONGLONG t1, t2;
    int i;

    printf("win7api\n");

    /* Version: a program linked for NT 6.x must see Windows 7 SP1. */
#pragma warning(suppress: 4996)
    CHECK(GetVersionExW((OSVERSIONINFOW *)&osvi));
    printf("  GetVersionEx: %lu.%lu.%lu SP%u type %u\n", osvi.dwMajorVersion, osvi.dwMinorVersion,
           osvi.dwBuildNumber, osvi.wServicePackMajor, osvi.wProductType);
    CHECK(osvi.dwMajorVersion == 6 && osvi.dwMinorVersion == 1);
    CHECK(osvi.wProductType == VER_NT_WORKSTATION);

    ZeroMemory(&osvi, sizeof(osvi));
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    osvi.dwMajorVersion = 6;
    osvi.dwMinorVersion = 1;
    VER_SET_CONDITION(mask, VER_MAJORVERSION, VER_GREATER_EQUAL);
    VER_SET_CONDITION(mask, VER_MINORVERSION, VER_GREATER_EQUAL);
    CHECK(VerifyVersionInfoW(&osvi, VER_MAJORVERSION | VER_MINORVERSION, mask)); /* IsWindows7OrGreater */

    /* GetTickCount64 */
    t1 = GetTickCount64();
    Sleep(50);
    t2 = GetTickCount64();
    CHECK(t2 >= t1 + 40);

    /* InitOnce, SRW locks, condition variables across threads */
    for (i = 0; i < 4; i++)
        threads[i] = CreateThread(NULL, 0, Worker, NULL, 0, NULL);
    Sleep(200);
    AcquireSRWLockExclusive(&g_lock);
    g_ready = TRUE;
    ReleaseSRWLockExclusive(&g_lock);
    WakeAllConditionVariable(&g_cond);
    CHECK(WaitForMultipleObjects(4, threads, TRUE, 10000) == WAIT_OBJECT_0);
    for (i = 0; i < 4; i++)
        CloseHandle(threads[i]);
    CHECK(g_onceCalls == 1);
    CHECK(g_counter == 4000);

    /* GetFinalPathNameByHandleW */
    GetTempPathW(MAX_PATH, path);
    lstrcatW(path, L"wdx_final.txt");
    hFile = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    CHECK(hFile != INVALID_HANDLE_VALUE);
    if (hFile != INVALID_HANDLE_VALUE)
    {
        DWORD len = GetFinalPathNameByHandleW(hFile, final, MAX_PATH, FILE_NAME_NORMALIZED);
        CHECK(len > 0 && len < MAX_PATH);
        if (len)
            printf("  final path: %ls\n", final);
        CloseHandle(hFile);
    }

    printf("%s win7api\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
