/*
 * PROJECT:     WinDosDX Visual C++ runtime
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     vcruntime140.dll: the parts ucrtbase does not export
 *
 * Programs built with Visual Studio 2015-2022 import vcruntime140.dll (and,
 * for 64-bit C++, vcruntime140_1.dll). Windows gets them from the Visual C++
 * Redistributable; WinDosDX ships these, which forward the runtime functions
 * to ucrtbase (see the .spec files) and implement the few helpers below.
 */

#include <windef.h>
#include <winbase.h>

/* Wrappers the runtime's static code calls instead of kernel32 directly. */

DWORD __cdecl
__vcrt_GetModuleFileNameW(HMODULE Module, LPWSTR FileName, DWORD Size)
{
    return GetModuleFileNameW(Module, FileName, Size);
}

HMODULE __cdecl
__vcrt_GetModuleHandleW(LPCWSTR ModuleName)
{
    return GetModuleHandleW(ModuleName);
}

BOOL __cdecl
__vcrt_InitializeCriticalSectionEx(LPCRITICAL_SECTION CriticalSection, DWORD SpinCount, DWORD Flags)
{
    /* The flags only select debug information. */
    UNREFERENCED_PARAMETER(Flags);
    return InitializeCriticalSectionAndSpinCount(CriticalSection, SpinCount);
}

HMODULE __cdecl
__vcrt_LoadLibraryExW(LPCWSTR FileName, HANDLE File, DWORD Flags)
{
    return LoadLibraryExW(FileName, File, Flags);
}

/* Usage telemetry hooks around main(): nothing is collected. */

void __cdecl
__telemetry_main_invoke_trigger(HINSTANCE Instance)
{
    UNREFERENCED_PARAMETER(Instance);
}

void __cdecl
__telemetry_main_return_trigger(HINSTANCE Instance)
{
    UNREFERENCED_PARAMETER(Instance);
}
