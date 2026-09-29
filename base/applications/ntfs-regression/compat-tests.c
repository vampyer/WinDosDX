/*
 * Program compatibility check: runs real Windows programs from the test disk.
 *
 * C:\COMPAT\LIST.TXT names the programs, one per line:
 *
 *     name|cli|relative\path.exe|arguments|text the output must contain
 *     name|gui|relative\path.exe|arguments|
 *
 * A console program passes when it exits by itself within the time limit
 * with the expected text in its output (stdout and stderr go to
 * C:\COMPAT\OUT\name.txt). A GUI program passes when it opens a visible
 * window; the harness takes a screenshot (DOSREG WAIT) and the program is
 * then closed. Results are COMPATREG lines; a program that cannot load
 * reports the loader's status, e.g. 0xc0000135 (a DLL is missing) or
 * 0xc0000139 (a function is missing).
 */

#define WIN32_LEAN_AND_MEAN
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600    /* GetFinalPathNameByHandleW */
#define WIN32_NO_STATUS

#include <windef.h>
#include <winbase.h>
#include <winuser.h>
#include <winnls.h>
#include <stdio.h>
#include <string.h>

#include "exfat-tests.h"

#define COMPAT_DIR      L"C:\\COMPAT"
#define COMPAT_LIST     COMPAT_DIR L"\\LIST.TXT"
#define CLI_TIMEOUT_MS  (120 * 1000)
#define GUI_TIMEOUT_MS  (90 * 1000)

BOOL
CompatListPresent(void)
{
    return GetFileAttributesW(COMPAT_LIST) != INVALID_FILE_ATTRIBUTES;
}

/* The first line of a file, printable characters only, for the report. */
static void
FirstLine(PCWSTR Path, char *Out, size_t Size)
{
    HANDLE File;
    char Buffer[512];
    DWORD Read = 0, i, n = 0;

    Out[0] = '\0';
    File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (File == INVALID_HANDLE_VALUE)
        return;
    ReadFile(File, Buffer, sizeof(Buffer) - 1, &Read, NULL);
    CloseHandle(File);
    for (i = 0; i < Read && n + 1 < Size; i++)
    {
        char c = Buffer[i];
        if (c == '\n')
        {
            if (n)
                break;
            continue;
        }
        if (c >= 0x20 && c < 0x7F)
            Out[n++] = c;
    }
    Out[n] = '\0';
}

static BOOL
FileContains(PCWSTR Path, const char *Text)
{
    HANDLE File;
    static char Buffer[64 * 1024];
    DWORD Read = 0;

    if (!Text[0])
        return TRUE;
    File = CreateFileW(Path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (File == INVALID_HANDLE_VALUE)
        return FALSE;
    ReadFile(File, Buffer, sizeof(Buffer) - 1, &Read, NULL);
    CloseHandle(File);
    Buffer[Read] = '\0';
    /* The output may contain NULs (UTF-16 text): search byte by byte. */
    {
        size_t Length = strlen(Text), i;
        for (i = 0; i + Length <= Read; i++)
        {
            if (!memcmp(Buffer + i, Text, Length))
                return TRUE;
        }
    }
    return FALSE;
}

typedef struct _FIND_WINDOW
{
    DWORD ProcessId;
    HWND Window;        /* a visible window that is not a dialog box */
    HWND Dialog;        /* a visible dialog box (an error message, often) */
} FIND_WINDOW;

static BOOL CALLBACK
FindProcessWindow(HWND Window, LPARAM Parameter)
{
    FIND_WINDOW *Find = (FIND_WINDOW *)Parameter;
    DWORD ProcessId = 0;
    char Class[64];

    GetWindowThreadProcessId(Window, &ProcessId);
    if (ProcessId != Find->ProcessId || !IsWindowVisible(Window))
        return TRUE;
    GetClassNameA(Window, Class, sizeof(Class));
    if (!strcmp(Class, "#32770"))
    {
        if (!Find->Dialog)
            Find->Dialog = Window;
        return TRUE;
    }
    Find->Window = Window;
    return FALSE;
}

/* The text of a dialog's static controls, for the report. */
typedef struct _DIALOG_TEXT
{
    char Text[240];
} DIALOG_TEXT;

static BOOL CALLBACK
CollectStaticText(HWND Child, LPARAM Parameter)
{
    DIALOG_TEXT *Out = (DIALOG_TEXT *)Parameter;
    char Class[32], Text[160];
    size_t Length = strlen(Out->Text), i;

    GetClassNameA(Child, Class, sizeof(Class));
    if (_stricmp(Class, "Static") || !GetWindowTextA(Child, Text, sizeof(Text)) || !Text[0])
        return TRUE;
    for (i = 0; Text[i]; i++)
    {
        if (Text[i] == '\r' || Text[i] == '\n')
            Text[i] = ' ';
    }
    _snprintf(Out->Text + Length, sizeof(Out->Text) - Length - 1, "%s%s", Length ? " | " : "", Text);
    return TRUE;
}

static BOOL
StartProgram(PCWSTR Name, PCWSTR Exe, PCWSTR Arguments, HANDLE Output, DWORD Flags,
             PROCESS_INFORMATION *Process)
{
    WCHAR CommandLine[1024], Directory[MAX_PATH], *Slash;
    STARTUPINFOW Startup;
    BOOL Ok;

    UNREFERENCED_PARAMETER(Name);

    _snwprintf(CommandLine, ARRAYSIZE(CommandLine) - 1, L"\"%s\\%s\" %s", COMPAT_DIR, Exe, Arguments);
    CommandLine[ARRAYSIZE(CommandLine) - 1] = L'\0';
    _snwprintf(Directory, ARRAYSIZE(Directory) - 1, L"%s\\%s", COMPAT_DIR, Exe);
    Directory[ARRAYSIZE(Directory) - 1] = L'\0';
    Slash = wcsrchr(Directory, L'\\');
    if (Slash)
        *Slash = L'\0';

    ZeroMemory(&Startup, sizeof(Startup));
    Startup.cb = sizeof(Startup);
    if (Output)
    {
        Startup.dwFlags = STARTF_USESTDHANDLES;
        Startup.hStdInput = NULL;
        Startup.hStdOutput = Output;
        Startup.hStdError = Output;
    }
    Ok = CreateProcessW(NULL, CommandLine, NULL, NULL, Output != NULL, Flags, NULL, Directory, &Startup, Process);
    return Ok;
}

/* ---------------------------------------------------------------------- */
/* Diagnosis: run a failing program again under a small debugger           */
/* ---------------------------------------------------------------------- */

#define MAX_MODULES 512

/* Hidden by WIN32_NO_STATUS. */
#ifndef DBG_CONTINUE
#define DBG_CONTINUE              ((DWORD)0x00010002)
#endif
#ifndef DBG_EXCEPTION_NOT_HANDLED
#define DBG_EXCEPTION_NOT_HANDLED ((DWORD)0x80010001)
#endif
#ifndef STATUS_BREAKPOINT
#define STATUS_BREAKPOINT         ((DWORD)0x80000003)
#endif

typedef struct _MODULE_INFO
{
    ULONG_PTR Base;
    char Name[48];
} MODULE_INFO;

static void
RemoteName(HANDLE Process, PVOID ImageName, BOOL Unicode, char *Out, size_t Size)
{
    PVOID Pointer = NULL;
    WCHAR Wide[MAX_PATH];
    char Narrow[MAX_PATH];
    SIZE_T Read = 0;
    const char *Base;

    strcpy(Out, "?");
    if (!ImageName || !ReadProcessMemory(Process, ImageName, &Pointer, sizeof(Pointer), &Read) || !Pointer)
        return;
    if (Unicode)
    {
        ZeroMemory(Wide, sizeof(Wide));
        ReadProcessMemory(Process, Pointer, Wide, sizeof(Wide) - sizeof(WCHAR), &Read);
        WideCharToMultiByte(CP_ACP, 0, Wide, -1, Narrow, sizeof(Narrow), NULL, NULL);
    }
    else
    {
        ZeroMemory(Narrow, sizeof(Narrow));
        ReadProcessMemory(Process, Pointer, Narrow, sizeof(Narrow) - 1, &Read);
    }
    Base = strrchr(Narrow, '\\');
    Base = Base ? Base + 1 : Narrow;
    strncpy(Out, Base, Size - 1);
    Out[Size - 1] = '\0';
}

/* The thread's last error, read from its TEB (for programs that stop
 * themselves with a breakpoint right after a failed call). */
typedef struct _WDX_THREAD_BASIC_INFORMATION
{
    LONG ExitStatus;
    PVOID TebBaseAddress;
    PVOID UniqueProcess;
    PVOID UniqueThread;
    ULONG_PTR AffinityMask;
    LONG Priority;
    LONG BasePriority;
} WDX_THREAD_BASIC_INFORMATION;

static DWORD
RemoteLastError(HANDLE Process, HANDLE Thread)
{
    typedef LONG (NTAPI *QUERY_THREAD)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    QUERY_THREAD Query = (QUERY_THREAD)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
    WDX_THREAD_BASIC_INFORMATION Info;
    DWORD Error = 0;
    SIZE_T Read = 0;

    if (!Query || Query(Thread, 0 /* ThreadBasicInformation */, &Info, sizeof(Info), NULL) < 0)
        return 0xFFFFFFFF;
#ifdef _M_AMD64
    ReadProcessMemory(Process, (PUCHAR)Info.TebBaseAddress + 0x68, &Error, sizeof(Error), &Read);
#else
    ReadProcessMemory(Process, (PUCHAR)Info.TebBaseAddress + 0x34, &Error, sizeof(Error), &Read);
#endif
    return Error;
}

static const MODULE_INFO *
FindModule(const MODULE_INFO *Modules, int Count, ULONG_PTR Address)
{
    const MODULE_INFO *Best = NULL;
    int i;

    for (i = 0; i < Count; i++)
    {
        if (Modules[i].Base <= Address && (!Best || Modules[i].Base > Best->Base))
            Best = &Modules[i];
    }
    return Best;
}

static void
DiagnoseProgram(const char *Name, PCWSTR NameW, PCWSTR Exe, PCWSTR Arguments)
{
    static MODULE_INFO Modules[MAX_MODULES];
    PROCESS_INFORMATION Process;
    DEBUG_EVENT Event;
    int ModuleCount = 0, Exceptions = 0, Strings = 0;
    DWORD Start = GetTickCount();
    BOOL Done = FALSE;
    PCWSTR ExeName = wcsrchr(Exe, L'\\') ? wcsrchr(Exe, L'\\') + 1 : Exe;

    if (!StartProgram(NameW, Exe, Arguments, NULL, DEBUG_ONLY_THIS_PROCESS, &Process))
    {
        Emit("COMPATDBG %s cannot start under the debugger (%lu)", Name, GetLastError());
        return;
    }
    while (!Done && GetTickCount() - Start < 60 * 1000)
    {
        DWORD Continue = DBG_CONTINUE;

        if (!WaitForDebugEvent(&Event, 5000))
            continue;
        switch (Event.dwDebugEventCode)
        {
            case CREATE_PROCESS_DEBUG_EVENT:
                if (ModuleCount < MAX_MODULES)
                {
                    Modules[ModuleCount].Base = (ULONG_PTR)Event.u.CreateProcessInfo.lpBaseOfImage;
                    WideCharToMultiByte(CP_ACP, 0, ExeName, -1, Modules[ModuleCount].Name,
                                        sizeof(Modules[0].Name), NULL, NULL);
                    ModuleCount++;
                }
                if (Event.u.CreateProcessInfo.hFile)
                    CloseHandle(Event.u.CreateProcessInfo.hFile);
                break;

            case LOAD_DLL_DEBUG_EVENT:
                if (ModuleCount < MAX_MODULES)
                {
                    Modules[ModuleCount].Base = (ULONG_PTR)Event.u.LoadDll.lpBaseOfDll;
                    RemoteName(Process.hProcess, Event.u.LoadDll.lpImageName, Event.u.LoadDll.fUnicode,
                               Modules[ModuleCount].Name, sizeof(Modules[0].Name));
                    /* No name from the loader: take it from the file. */
                    if ((!Modules[ModuleCount].Name[0] || !strcmp(Modules[ModuleCount].Name, "?")) &&
                        Event.u.LoadDll.hFile)
                    {
                        WCHAR Path[MAX_PATH];
                        char PathA[MAX_PATH];
                        const char *Base;

                        if (GetFinalPathNameByHandleW(Event.u.LoadDll.hFile, Path, ARRAYSIZE(Path), 0))
                        {
                            WideCharToMultiByte(CP_ACP, 0, Path, -1, PathA, sizeof(PathA), NULL, NULL);
                            Base = strrchr(PathA, '\\');
                            strncpy(Modules[ModuleCount].Name, Base ? Base + 1 : PathA, sizeof(Modules[0].Name) - 1);
                            Modules[ModuleCount].Name[sizeof(Modules[0].Name) - 1] = '\0';
                        }
                    }
                    Emit("COMPATDBG %s dll %s at %Ix", Name, Modules[ModuleCount].Name, Modules[ModuleCount].Base);
                    ModuleCount++;
                }
                if (Event.u.LoadDll.hFile)
                    CloseHandle(Event.u.LoadDll.hFile);
                break;

            case EXCEPTION_DEBUG_EVENT:
            {
                EXCEPTION_RECORD *Record = &Event.u.Exception.ExceptionRecord;
                ULONG_PTR Address = (ULONG_PTR)Record->ExceptionAddress;
                const MODULE_INFO *Module = FindModule(Modules, ModuleCount, Address);

                if (Exceptions++ < 12)
                {
                    Emit("COMPATDBG %s exception 0x%08lx at %s+0x%Ix (%s chance) info %Ix %Ix",
                         Name, Record->ExceptionCode, Module ? Module->Name : "?",
                         Module ? Address - Module->Base : Address,
                         Event.u.Exception.dwFirstChance ? "first" : "second",
                         Record->NumberParameters > 0 ? Record->ExceptionInformation[0] : 0,
                         Record->NumberParameters > 1 ? Record->ExceptionInformation[1] : 0);
                }
                /* A jump to a near-NULL address: the caller is on top of the stack. */
                if (Address < 0x10000 && Exceptions <= 12)
                {
                    HANDLE Thread = OpenThread(THREAD_GET_CONTEXT, FALSE, Event.dwThreadId);
                    CONTEXT Context;
                    ULONG_PTR Stack[6] = { 0 }, Sp;
                    SIZE_T Read = 0;
                    int k;

                    Context.ContextFlags = CONTEXT_CONTROL;
                    if (Thread && GetThreadContext(Thread, &Context))
                    {
#ifdef _M_AMD64
                        Sp = Context.Rsp;
#else
                        Sp = Context.Esp;
#endif
                        ReadProcessMemory(Process.hProcess, (PVOID)Sp, Stack, sizeof(Stack), &Read);
                        Emit("COMPATDBG %s   stack pointer %Ix", Name, Sp);
                        /* The return address is usually on top; show a few more
                         * entries in case the call was a tail call. */
                        for (k = 0; k < 6; k++)
                        {
                            const MODULE_INFO *Caller = FindModule(Modules, ModuleCount, Stack[k]);
                            Emit("COMPATDBG %s   stack[%d] %Ix = %s+0x%Ix", Name, k, Stack[k],
                                 Caller ? Caller->Name : "?", Caller ? Stack[k] - Caller->Base : Stack[k]);
                        }
                    }
                    if (Thread)
                        CloseHandle(Thread);
                }
                /* A program's own breakpoint (after the loader's): report its last
                 * error and the registers that usually hold the failed call's
                 * arguments. */
                if (Record->ExceptionCode == EXCEPTION_BREAKPOINT && Exceptions > 1 && Exceptions <= 12)
                {
                    HANDLE Thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, Event.dwThreadId);
                    CONTEXT Context;

                    Context.ContextFlags = CONTEXT_INTEGER;
                    if (Thread && GetThreadContext(Thread, &Context))
                    {
#ifdef _M_AMD64
                        Emit("COMPATDBG %s   last error %lu; rbx %Ix rsi %Ix rdi %Ix", Name,
                             RemoteLastError(Process.hProcess, Thread), Context.Rbx, Context.Rsi, Context.Rdi);
#else
                        Emit("COMPATDBG %s   last error %lu; ebx %lx esi %lx edi %lx", Name,
                             RemoteLastError(Process.hProcess, Thread), Context.Ebx, Context.Esi, Context.Edi);
#endif
                    }
                    if (Thread)
                        CloseHandle(Thread);
                }
                /* The loader's breakpoint is ours; everything else is the program's. */
                if (Record->ExceptionCode != EXCEPTION_BREAKPOINT || Exceptions > 1)
                    Continue = DBG_EXCEPTION_NOT_HANDLED;
                break;
            }

            case OUTPUT_DEBUG_STRING_EVENT:
                if (Strings++ < 20 && !Event.u.DebugString.fUnicode)
                {
                    char Text[200];
                    SIZE_T Read = 0;
                    WORD Length = Event.u.DebugString.nDebugStringLength;

                    if (Length > sizeof(Text) - 1)
                        Length = sizeof(Text) - 1;
                    ZeroMemory(Text, sizeof(Text));
                    ReadProcessMemory(Process.hProcess, Event.u.DebugString.lpDebugStringData, Text, Length, &Read);
                    Text[strcspn(Text, "\r\n")] = '\0';
                    Emit("COMPATDBG %s debug string: %s", Name, Text);
                }
                break;

            case EXIT_PROCESS_DEBUG_EVENT:
                Emit("COMPATDBG %s exit code 0x%08lx after %lu ms", Name,
                     Event.u.ExitProcess.dwExitCode, GetTickCount() - Start);
                Done = TRUE;
                break;

            default:
                break;
        }
        ContinueDebugEvent(Event.dwProcessId, Event.dwThreadId, Continue);
    }
    if (!Done)
    {
        Emit("COMPATDBG %s still running after 60 s; ending it", Name);
        TerminateProcess(Process.hProcess, 1);
        /* Drain the remaining events so the process can go away. */
        while (WaitForDebugEvent(&Event, 2000))
        {
            if (Event.dwDebugEventCode == LOAD_DLL_DEBUG_EVENT && Event.u.LoadDll.hFile)
                CloseHandle(Event.u.LoadDll.hFile);
            ContinueDebugEvent(Event.dwProcessId, Event.dwThreadId, DBG_CONTINUE);
            if (Event.dwDebugEventCode == EXIT_PROCESS_DEBUG_EVENT)
                break;
        }
    }
    CloseHandle(Process.hThread);
    CloseHandle(Process.hProcess);
}

static void
RunCli(const char *Name, PCWSTR NameW, PCWSTR Exe, PCWSTR Arguments, const char *Expect)
{
    WCHAR OutPath[MAX_PATH];
    SECURITY_ATTRIBUTES Inherit = { sizeof(Inherit), NULL, TRUE };
    PROCESS_INFORMATION Process;
    HANDLE Output;
    DWORD Wait, Exit = 0, Start = GetTickCount();
    char Line[160];

    _snwprintf(OutPath, ARRAYSIZE(OutPath) - 1, L"%s\\OUT\\%s.txt", COMPAT_DIR, NameW);
    OutPath[ARRAYSIZE(OutPath) - 1] = L'\0';
    Output = CreateFileW(OutPath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &Inherit,
                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (Output == INVALID_HANDLE_VALUE)
    {
        Emit("COMPATREG FAIL %s cannot create output file (%lu)", Name, GetLastError());
        return;
    }
    if (!StartProgram(NameW, Exe, Arguments, Output, 0, &Process))
    {
        Emit("COMPATREG FAIL %s CreateProcess error %lu", Name, GetLastError());
        CloseHandle(Output);
        return;
    }
    CloseHandle(Output);

    Wait = WaitForSingleObject(Process.hProcess, CLI_TIMEOUT_MS);
    if (Wait != WAIT_OBJECT_0)
    {
        TerminateProcess(Process.hProcess, 1);
        WaitForSingleObject(Process.hProcess, 5000);
    }
    GetExitCodeProcess(Process.hProcess, &Exit);
    CloseHandle(Process.hThread);
    CloseHandle(Process.hProcess);
    FirstLine(OutPath, Line, sizeof(Line));

    if (Wait != WAIT_OBJECT_0)
        Emit("COMPATREG FAIL %s timeout after %lu s; output: %s", Name, CLI_TIMEOUT_MS / 1000, Line);
    else if (Exit >= 0x80000000)
        Emit("COMPATREG FAIL %s status 0x%08lx after %lu ms; output: %s", Name, Exit, GetTickCount() - Start, Line);
    else if (!FileContains(OutPath, Expect))
        Emit("COMPATREG FAIL %s exit %lu (0x%08lx), expected \"%s\"; output: %s", Name, Exit, Exit, Expect, Line);
    else
    {
        Emit("COMPATREG PASS %s exit %lu in %lu ms; output: %s", Name, Exit, GetTickCount() - Start, Line);
        return;
    }
    DiagnoseProgram(Name, NameW, Exe, Arguments);
}

static void
RunGui(const char *Name, PCWSTR NameW, PCWSTR Exe, PCWSTR Arguments)
{
    PROCESS_INFORMATION Process;
    FIND_WINDOW Find;
    DWORD Start = GetTickCount(), Exit = 0;
    char Title[128];

    if (!StartProgram(NameW, Exe, Arguments, NULL, 0, &Process))
    {
        Emit("COMPATREG FAIL %s CreateProcess error %lu", Name, GetLastError());
        return;
    }
    Find.ProcessId = Process.dwProcessId;
    Find.Window = NULL;
    Find.Dialog = NULL;
    while (GetTickCount() - Start < GUI_TIMEOUT_MS)
    {
        if (WaitForSingleObject(Process.hProcess, 1000) == WAIT_OBJECT_0)
            break;
        Find.Dialog = NULL;
        EnumWindows(FindProcessWindow, (LPARAM)&Find);
        if (Find.Window)
            break;
        /* A dialog alone for a while is the program's answer (often an error). */
        if (Find.Dialog && GetTickCount() - Start > 20 * 1000)
            break;
    }

    if (Find.Window)
    {
        /* Give it a moment to paint, then let the harness take a picture. */
        Sleep(5000);
        GetWindowTextA(Find.Window, Title, sizeof(Title));
        Emit("COMPATREG PASS %s window \"%s\" after %lu ms", Name, Title, GetTickCount() - Start);
        Emit("DOSREG WAIT compat-%s 3", Name);
        Sleep(4000);
        TerminateProcess(Process.hProcess, 0);
    }
    else if (Find.Dialog)
    {
        DIALOG_TEXT Text;

        Text.Text[0] = '\0';
        GetWindowTextA(Find.Dialog, Title, sizeof(Title));
        EnumChildWindows(Find.Dialog, CollectStaticText, (LPARAM)&Text);
        Emit("COMPATREG FAIL %s shows only a dialog \"%s\": %s", Name, Title, Text.Text);
        Emit("DOSREG WAIT compat-%s 3", Name);
        Sleep(4000);
        TerminateProcess(Process.hProcess, 1);
    }
    else if (WaitForSingleObject(Process.hProcess, 0) == WAIT_OBJECT_0)
    {
        GetExitCodeProcess(Process.hProcess, &Exit);
        Emit("COMPATREG FAIL %s exited with 0x%08lx after %lu ms, no window", Name, Exit, GetTickCount() - Start);
        CloseHandle(Process.hThread);
        CloseHandle(Process.hProcess);
        DiagnoseProgram(Name, NameW, Exe, Arguments);
        return;
    }
    else
    {
        Emit("DOSREG WAIT compat-%s 3", Name);
        Sleep(4000);
        Emit("COMPATREG FAIL %s no window after %lu s", Name, GUI_TIMEOUT_MS / 1000);
        TerminateProcess(Process.hProcess, 1);
        WaitForSingleObject(Process.hProcess, 5000);
        CloseHandle(Process.hThread);
        CloseHandle(Process.hProcess);
        DiagnoseProgram(Name, NameW, Exe, Arguments);
        return;
    }
    WaitForSingleObject(Process.hProcess, 5000);
    CloseHandle(Process.hThread);
    CloseHandle(Process.hProcess);
}

/* ---------------------------------------------------------------------- */
/* Memory probe: the reserve/commit patterns of JIT engines (V8)            */
/* ---------------------------------------------------------------------- */

static void
ProbeStep(const char *What, PVOID Result)
{
    Emit("COMPATREG INFO memprobe %-44s %s (error %lu)", What, Result ? "ok" : "FAILED",
         Result ? 0 : GetLastError());
}

static void
MemoryProbe(void)
{
#ifdef _WIN64
    const SIZE_T GB = (SIZE_T)1 << 30;
    PUCHAR Reserve, Aligned, Big;
    SIZE_T Size;

    /* A 4 GB cage aligned to 4 GB: reserve 8 GB, release, reserve inside. */
    Reserve = VirtualAlloc(NULL, 8 * GB, MEM_RESERVE, PAGE_NOACCESS);
    ProbeStep("reserve 8 GB", Reserve);
    if (!Reserve)
        return;
    Aligned = (PUCHAR)(((ULONG_PTR)Reserve + 4 * GB - 1) & ~(4 * GB - 1));
    VirtualFree(Reserve, 0, MEM_RELEASE);
    Reserve = VirtualAlloc(Aligned, 4 * GB, MEM_RESERVE, PAGE_NOACCESS);
    ProbeStep("reserve 4 GB at a 4 GB boundary (hint)", Reserve);
    if (!Reserve)
        return;

    ProbeStep("commit 256 KB read-write at start", VirtualAlloc(Reserve, 256 * 1024, MEM_COMMIT, PAGE_READWRITE));
    ProbeStep("commit 256 KB read-write at +1 GB", VirtualAlloc(Reserve + GB, 256 * 1024, MEM_COMMIT, PAGE_READWRITE));
    ProbeStep("commit 256 KB execute-read-write at +2 GB",
              VirtualAlloc(Reserve + 2 * GB, 256 * 1024, MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    ProbeStep("commit 256 KB execute-read at +2 GB+1 MB",
              VirtualAlloc(Reserve + 2 * GB + (1 << 20), 256 * 1024, MEM_COMMIT, PAGE_EXECUTE_READ));
    ProbeStep("re-commit committed 256 KB read-only",
              VirtualAlloc(Reserve, 256 * 1024, MEM_COMMIT, PAGE_READONLY));
    ProbeStep("commit 512 KB over half-committed range",
              VirtualAlloc(Reserve + GB - 256 * 1024, 512 * 1024, MEM_COMMIT, PAGE_READWRITE));
    ProbeStep("decommit 128 KB", (PVOID)(ULONG_PTR)VirtualFree(Reserve + GB, 128 * 1024, MEM_DECOMMIT));
    ProbeStep("re-commit decommitted 128 KB",
              VirtualAlloc(Reserve + GB, 128 * 1024, MEM_COMMIT, PAGE_READWRITE));
    ProbeStep("commit 64 MB read-write at +3 GB",
              VirtualAlloc(Reserve + 3 * GB, 64 << 20, MEM_COMMIT, PAGE_READWRITE));
    ProbeStep("commit 1 page no-access",
              VirtualAlloc(Reserve + 3 * GB + (128 << 20), 4096, MEM_COMMIT, PAGE_NOACCESS));
    ProbeStep("reset 64 KB (MEM_RESET, read-write)",
              VirtualAlloc(Reserve + 3 * GB, 64 * 1024, MEM_RESET, PAGE_READWRITE));
    ProbeStep("reset 64 KB (MEM_RESET, no-access)",
              VirtualAlloc(Reserve + 3 * GB, 64 * 1024, MEM_RESET, PAGE_NOACCESS));
    ProbeStep("release the 4 GB cage", (PVOID)(ULONG_PTR)VirtualFree(Reserve, 0, MEM_RELEASE));

    /* Bigger reservations (the V8 sandbox asks for up to 1 TB). */
    for (Size = 64 * GB; Size <= 1024 * GB; Size *= 4)
    {
        char What[64];
        _snprintf(What, sizeof(What), "reserve %Iu GB", Size / GB);
        Big = VirtualAlloc(NULL, Size, MEM_RESERVE, PAGE_NOACCESS);
        ProbeStep(What, Big);
        if (Big)
        {
            ProbeStep("  commit 1 MB in it", VirtualAlloc(Big + Size / 2, 1 << 20, MEM_COMMIT, PAGE_READWRITE));
            VirtualFree(Big, 0, MEM_RELEASE);
        }
    }
#endif
}

/* Where programs keep their settings: missing profile folders make many fail. */
static void
ReportProfile(void)
{
    static const WCHAR *Names[] = { L"USERPROFILE", L"APPDATA", L"LOCALAPPDATA", L"TEMP", L"ProgramData" };
    int i;

    for (i = 0; i < ARRAYSIZE(Names); i++)
    {
        WCHAR Value[MAX_PATH], Probe[MAX_PATH];
        char ValueA[MAX_PATH];
        DWORD Attributes;
        BOOL Writable = FALSE;
        HANDLE File;

        if (!GetEnvironmentVariableW(Names[i], Value, ARRAYSIZE(Value)))
        {
            Emit("COMPATREG INFO %S is not set", Names[i]);
            continue;
        }
        Attributes = GetFileAttributesW(Value);
        _snwprintf(Probe, ARRAYSIZE(Probe) - 1, L"%s\\compat-probe.tmp", Value);
        Probe[ARRAYSIZE(Probe) - 1] = L'\0';
        File = CreateFileW(Probe, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL);
        if (File != INVALID_HANDLE_VALUE)
        {
            Writable = TRUE;
            CloseHandle(File);
        }
        WideCharToMultiByte(CP_ACP, 0, Value, -1, ValueA, sizeof(ValueA), NULL, NULL);
        Emit("COMPATREG INFO %S=%s (%s, %s)", Names[i], ValueA,
             Attributes == INVALID_FILE_ATTRIBUTES ? "missing" : "exists",
             Writable ? "writable" : "not writable");
    }
}

VOID
CompatRunTests(void)
{
    FILE *List;
    char Line[1024];
    int Total = 0;

    Emit("COMPATREG BEGIN");
    /* A program whose DLL is missing must fail, not wait on an error box. */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    CreateDirectoryW(COMPAT_DIR L"\\OUT", NULL);
    ReportProfile();
    MemoryProbe();

    List = _wfopen(COMPAT_LIST, L"r");
    if (!List)
    {
        Emit("COMPATREG FAIL cannot open the list");
        Emit("COMPATREG END");
        return;
    }
    while (fgets(Line, sizeof(Line), List))
    {
        char *Field[5] = { 0 }, *p = Line;
        WCHAR NameW[64], ExeW[MAX_PATH], ArgsW[512];
        int i;

        Line[strcspn(Line, "\r\n")] = '\0';
        if (!Line[0] || Line[0] == '#')
            continue;
        for (i = 0; i < 5; i++)
        {
            Field[i] = p;
            p = p ? strchr(p, '|') : NULL;
            if (p)
                *p++ = '\0';
        }
        if (!Field[0] || !Field[1] || !Field[2])
            continue;
        MultiByteToWideChar(CP_ACP, 0, Field[0], -1, NameW, ARRAYSIZE(NameW));
        MultiByteToWideChar(CP_ACP, 0, Field[2], -1, ExeW, ARRAYSIZE(ExeW));
        MultiByteToWideChar(CP_ACP, 0, Field[3] ? Field[3] : "", -1, ArgsW, ARRAYSIZE(ArgsW));

        Emit("COMPATREG INFO start %s", Field[0]);
        if (!strcmp(Field[1], "gui"))
            RunGui(Field[0], NameW, ExeW, ArgsW);
        else
            RunCli(Field[0], NameW, ExeW, ArgsW, Field[4] ? Field[4] : "");
        Total++;
    }
    fclose(List);
    Emit("COMPATREG END %d programs", Total);
}
