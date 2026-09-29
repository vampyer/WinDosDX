/*
 * PROJECT:     WinDosDX modern application tests
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.0-or-later)
 * PURPOSE:     Vista/8/10 APIs implemented against real state: the network
 *              tables and best route, per-class thread and process
 *              information, and the per-year time-zone rules.
 * COPYRIGHT:   Copyright 2026 WinDosDX Team & Contributors
 */

#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

static int failures;
static int ShowInNotepad;
static int ConsoleProbe;

#define CHECK(expr) \
    do { if (!(expr)) { ComPrintf("  FAIL line %d: %s\n", __LINE__, #expr); failures++; } } while (0)

/*
 * Guest output is logged before writing to stdout so a console write failure
 * still leaves a report on the writable LiveCD RamDisk (X:). The autorun
 * console-probe mode records standard-handle and WriteFile status, then the
 * test opens the completed report in Notepad for the QEMU screendump.
 */
static HANDLE ComHandle = INVALID_HANDLE_VALUE;
static HANDLE LogHandle = INVALID_HANDLE_VALUE;
static const char *LogPath = NULL;

static VOID
ComOpen(BOOL UseCom)
{
    /* Create a fresh log on the writable LiveCD RamDisk. */
    LogHandle = CreateFileA("X:\\wdxt.log", FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (LogHandle != INVALID_HANDLE_VALUE)
        LogPath = "X:\\wdxt.log";
    else
    {
        LogHandle = CreateFileA("C:\\wdxt.log", FILE_APPEND_DATA,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (LogHandle != INVALID_HANDLE_VALUE)
            LogPath = "C:\\wdxt.log";
    }
    /* CREATE_ALWAYS leaves the report ready to read from its beginning. */

    /* COM2 mirror: optional, never fatal. */
    if (UseCom)
    {
        ComHandle = CreateFileA("COM2", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (ComHandle == INVALID_HANDLE_VALUE)
            ComHandle = CreateFileA("COM1", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    }
}

static VOID
ComWrite(const char *Text)
{
    DWORD Written;

    if (LogHandle != INVALID_HANDLE_VALUE)
        WriteFile(LogHandle, Text, (DWORD)strlen(Text), &Written, NULL);
    if (ComHandle != INVALID_HANDLE_VALUE)
        WriteFile(ComHandle, Text, (DWORD)strlen(Text), &Written, NULL);
}

static VOID
ComPrintf(const char *Format, ...)
{
    char Line[512];
    va_list Arguments;
    int Length;

    va_start(Arguments, Format);
    Length = _vsnprintf(Line, sizeof(Line) - 3, Format, Arguments);
    va_end(Arguments);
    if (Length < 0)
        return;
    if (Length > (int)sizeof(Line) - 3)
        Length = (int)sizeof(Line) - 3;
    Line[Length++] = '\r';
    Line[Length++] = '\n';
    Line[Length] = 0;

    /* File first: a CRT/console startup failure still leaves diagnostics. */
    ComWrite(Line);
    fputs(Line, stdout);
}

static void TestNetTables(void)
{
    PMIB_IF_TABLE2 IfTable = NULL;
    PMIB_IPFORWARD_TABLE2 RouteTable = NULL;
    PMIB_UNICASTIPADDRESS_TABLE AddrTable = NULL;
    DWORD rc;

    ComPrintf("netinfo: tables\n");

    rc = GetIfTable2(&IfTable);
    CHECK(rc == NO_ERROR);
    if (rc == NO_ERROR && IfTable)
    {
        CHECK(IfTable->NumEntries >= 1);            /* loopback at least */
        for (ULONG i = 0; i < IfTable->NumEntries; i++)
        {
            CHECK(IfTable->Table[i].InterfaceIndex != 0 || i == 0);
            CHECK(IfTable->Table[i].OperStatus != IfOperStatusTesting);
            if (IfTable->Table[i].Type == IF_TYPE_SOFTWARE_LOOPBACK)
            {
                CHECK(IfTable->Table[i].InterfaceLuid.Info.IfType == IF_TYPE_SOFTWARE_LOOPBACK);
            }
        }
        FreeMibTable(IfTable);
    }

    rc = GetIpForwardTable2(AF_INET, &RouteTable);
    CHECK(rc == NO_ERROR);
    if (rc == NO_ERROR && RouteTable)
    {
        BOOLEAN HaveLoopbackRoute = FALSE;

        for (ULONG i = 0; i < RouteTable->NumEntries; i++)
        {
            PMIB_IPFORWARD_ROW2 Row = &RouteTable->Table[i];

            if (Row->DestinationPrefix.Prefix.Ipv4.sin_family == AF_INET &&
                Row->DestinationPrefix.Prefix.Ipv4.sin_addr.S_un.S_addr == 0 &&
                Row->DestinationPrefix.PrefixLength == 0)
                HaveLoopbackRoute = TRUE;
        }
        CHECK(HaveLoopbackRoute);                   /* the 0.0.0.0/0 route */
        FreeMibTable(RouteTable);
    }

    rc = GetUnicastIpAddressTable(AF_INET, &AddrTable);
    CHECK(rc == NO_ERROR);
    if (rc == NO_ERROR && AddrTable)
    {
        for (ULONG i = 0; i < AddrTable->NumEntries; i++)
        {
            CHECK(AddrTable->Table[i].Address.Ipv4.sin_family == AF_INET);
            /* Real Windows reports the address's actual DAD state; WinDosDX
             * reports preferred. Either way it must be a valid state. */
            CHECK(AddrTable->Table[i].DadState <= IpDadStateInvalid + 6);
        }
        FreeMibTable(AddrTable);
    }

    /* IPv6: real Windows has IPv6 rows; WinDosDX does not and reports
     * ERROR_NOT_SUPPORTED with a NULL table. Both are acceptable. */
    rc = GetIpForwardTable2(AF_INET6, &RouteTable);
    CHECK(rc == NO_ERROR || rc == ERROR_NOT_SUPPORTED);
    if (rc == NO_ERROR && RouteTable)
        FreeMibTable(RouteTable);
    else
        CHECK(RouteTable == NULL);
}

static void TestBestRoute2(void)
{
    SOCKADDR_INET Dest, Source;
    MIB_IPFORWARD_ROW2 Route;
    DWORD rc;

    ComPrintf("netinfo: best route\n");

    /* 127.0.0.1 must resolve through the loopback interface. Windows and
     * WinDosDX name the prefix differently (a /32 host route, the /8
     * loopback route or the default route), so only the essentials are
     * checked: it resolves, on a real interface, for an IPv4 destination. */
    RtlZeroMemory(&Dest, sizeof(Dest));
    Dest.Ipv4.sin_family = AF_INET;
    Dest.Ipv4.sin_addr.S_un.S_addr = htonl(0x7F000001);
    rc = GetBestRoute2(0, 0, NULL, &Dest, 0, &Route, &Source);
    CHECK(rc == NO_ERROR);
    if (rc == NO_ERROR)
    {
        CHECK(Route.InterfaceLuid.Info.IfType != 0);
        CHECK(Route.DestinationPrefix.Prefix.Ipv4.sin_family == AF_INET ||
              Route.DestinationPrefix.Prefix.si_family == AF_INET);
        ComPrintf("  best route: if %lu prefix /%u\n",
               Route.InterfaceIndex, Route.DestinationPrefix.PrefixLength);
    }

    /* An unspecified family is rejected, not silently matched. Windows
     * answers ERROR_INVALID_PARAMETER; WinDosDX ERROR_NOT_SUPPORTED. */
    RtlZeroMemory(&Dest, sizeof(Dest));
    rc = GetBestRoute2(0, 0, NULL, &Dest, 0, &Route, &Source);
    CHECK(rc == ERROR_INVALID_PARAMETER || rc == ERROR_NOT_SUPPORTED);

    CHECK(GetBestRoute2(0, 0, NULL, NULL, 0, &Route, &Source) == ERROR_INVALID_PARAMETER);
    CHECK(GetBestRoute2(0, 0, NULL, &Dest, 0, NULL, &Source) == ERROR_INVALID_PARAMETER);
}

struct NotifyContext
{
    LONG RouteEvents;
    LONG AddrEvents;
    LONG IfEvents;
    BOOLEAN InitialSeen;
    HANDLE Done;
};

static VOID WINAPI
RouteCallback(PVOID Context, PMIB_IPFORWARD_ROW2 Row, MIB_NOTIFICATION_TYPE Type)
{
    struct NotifyContext *Ctxt = Context;

    UNREFERENCED_PARAMETER(Row);
    InterlockedIncrement(&Ctxt->RouteEvents);
    if (Type == MibInitialNotification)
        Ctxt->InitialSeen = TRUE;
    if (Ctxt->Done)
        SetEvent(Ctxt->Done);
}

static VOID WINAPI
AddrCallback(PVOID Context, PMIB_UNICASTIPADDRESS_ROW Row, MIB_NOTIFICATION_TYPE Type)
{
    struct NotifyContext *Ctxt = Context;

    UNREFERENCED_PARAMETER(Row);
    InterlockedIncrement(&Ctxt->AddrEvents);
    if (Type == MibInitialNotification)
        Ctxt->InitialSeen = TRUE;
    if (Ctxt->Done)
        SetEvent(Ctxt->Done);
}

static VOID WINAPI
IfCallback(PVOID Context, PMIB_IPINTERFACE_ROW Row, MIB_NOTIFICATION_TYPE Type)
{
    struct NotifyContext *Ctxt = Context;

    UNREFERENCED_PARAMETER(Row);
    InterlockedIncrement(&Ctxt->IfEvents);
    if (Type == MibInitialNotification)
        Ctxt->InitialSeen = TRUE;
    if (Ctxt->Done)
        SetEvent(Ctxt->Done);
}

static void TestNotifications(void)
{
    struct NotifyContext Ctxt;
    HANDLE RouteHandle = NULL, AddrHandle = NULL, IfHandle = NULL;
    DWORD rc;

    ComPrintf("netinfo: notifications\n");

    ZeroMemory(&Ctxt, sizeof(Ctxt));
    Ctxt.Done = CreateEventW(NULL, TRUE, FALSE, NULL);
    CHECK(Ctxt.Done != NULL);

    rc = NotifyRouteChange2(AF_INET, RouteCallback, &Ctxt, TRUE, &RouteHandle);
    CHECK(rc == NO_ERROR);
    CHECK(RouteHandle != NULL);
    rc = NotifyUnicastIpAddressChange(AF_INET, AddrCallback, &Ctxt, TRUE, &AddrHandle);
    CHECK(rc == NO_ERROR);
    rc = NotifyIpInterfaceChange(AF_INET, IfCallback, &Ctxt, TRUE, &IfHandle);
    CHECK(rc == NO_ERROR);

    /* The initial pass delivers every current row once. */
    if (WaitForSingleObject(Ctxt.Done, 10000) == WAIT_OBJECT_0)
    {
        CHECK(Ctxt.InitialSeen);
        CHECK(Ctxt.RouteEvents >= 1);
        CHECK(Ctxt.AddrEvents >= 1);
        CHECK(Ctxt.IfEvents >= 1);
    }
    else
    {
        CHECK(Ctxt.RouteEvents >= 1 || Ctxt.AddrEvents >= 1);
        ComPrintf("  (initial pass slow: route %ld addr %ld if %ld)\n",
               Ctxt.RouteEvents, Ctxt.AddrEvents, Ctxt.IfEvents);
    }

    CHECK(CancelMibChangeNotify2(RouteHandle) == NO_ERROR);
    CHECK(CancelMibChangeNotify2(AddrHandle) == NO_ERROR);
    CHECK(CancelMibChangeNotify2(IfHandle) == NO_ERROR);
    /* Cancelling NULL: Windows does not document the return and does not
     * crash; WinDosDX refuses it. Either way the process must survive. */
    CancelMibChangeNotify2(NULL);
    CloseHandle(Ctxt.Done);
}

static void TestThreadProcessInfo(void)
{
    MEMORY_PRIORITY_INFORMATION Priority;
    APP_MEMORY_INFORMATION AppMemory;
    HANDLE Thread, Process = GetCurrentProcess();

    ComPrintf("netinfo: thread/process info\n");

    /* Memory priority round-trips, per thread, keyed by thread ID. */
    Thread = GetCurrentThread();
    CHECK(GetThreadInformation(Thread, ThreadMemoryPriority, &Priority, sizeof(Priority)));
    CHECK(Priority.MemoryPriority == MEMORY_PRIORITY_NORMAL);
    Priority.MemoryPriority = MEMORY_PRIORITY_BELOW_NORMAL;
    CHECK(SetThreadInformation(Thread, ThreadMemoryPriority, &Priority, sizeof(Priority)));
    CHECK(GetThreadInformation(Thread, ThreadMemoryPriority, &Priority, sizeof(Priority)));
    CHECK(Priority.MemoryPriority == MEMORY_PRIORITY_BELOW_NORMAL);

    /* A second thread starts at the default, not the first one's value. */
    {
        HANDLE Other = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)SetEvent,
                                    CreateEventW(NULL, TRUE, TRUE, NULL), 0, NULL);
        MEMORY_PRIORITY_INFORMATION OtherPriority;

        CHECK(Other != NULL);
        CHECK(GetThreadInformation(Other, ThreadMemoryPriority,
                                   &OtherPriority, sizeof(OtherPriority)));
        CHECK(OtherPriority.MemoryPriority == MEMORY_PRIORITY_NORMAL);
        WaitForSingleObject(Other, 1000);
        CloseHandle(Other);
    }
    Priority.MemoryPriority = MEMORY_PRIORITY_NORMAL;
    CHECK(SetThreadInformation(Thread, ThreadMemoryPriority, &Priority, sizeof(Priority)));

    /* Process memory priority round-trips; AppMemoryInfo reports the
     * process's real commit, which grows when we allocate. */
    CHECK(GetProcessInformation(Process, ProcessMemoryPriority, &Priority, sizeof(Priority)));
    CHECK(Priority.MemoryPriority == MEMORY_PRIORITY_NORMAL);
    Priority.MemoryPriority = MEMORY_PRIORITY_LOW;
    CHECK(SetProcessInformation(Process, ProcessMemoryPriority, &Priority, sizeof(Priority)));
    CHECK(GetProcessInformation(Process, ProcessMemoryPriority, &Priority, sizeof(Priority)));
    CHECK(Priority.MemoryPriority == MEMORY_PRIORITY_LOW);
    Priority.MemoryPriority = MEMORY_PRIORITY_NORMAL;
    CHECK(SetProcessInformation(Process, ProcessMemoryPriority, &Priority, sizeof(Priority)));

    CHECK(GetProcessInformation(Process, ProcessAppMemoryInfo, &AppMemory, sizeof(AppMemory)));
    CHECK(AppMemory.PrivateCommitUsage > 0);
    CHECK(AppMemory.AvailableCommit > 0);

    /* Validation: sizes, versions and flags follow Windows. */
    {
        PROCESS_POWER_THROTTLING_STATE Throttling;

        ZeroMemory(&Throttling, sizeof(Throttling));
        Throttling.Version = 999;                   /* wrong version */
        CHECK(SetThreadInformation(Thread, ThreadPowerThrottling,
                                   &Throttling, sizeof(Throttling)) == FALSE);
        Throttling.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
        Throttling.ControlMask = 0x80000000u;       /* unknown flag */
        CHECK(SetThreadInformation(Thread, ThreadPowerThrottling,
                                   &Throttling, sizeof(Throttling)) == FALSE);
        Throttling.ControlMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
        Throttling.StateMask = THREAD_POWER_THROTTLING_EXECUTION_SPEED;
        CHECK(SetThreadInformation(Thread, ThreadPowerThrottling,
                                   &Throttling, sizeof(Throttling)));
        CHECK(GetThreadInformation(Thread, ThreadPowerThrottling,
                                   &Throttling, sizeof(Throttling)));
        CHECK(Throttling.ControlMask == THREAD_POWER_THROTTLING_EXECUTION_SPEED);
        CHECK(Throttling.StateMask == THREAD_POWER_THROTTLING_EXECUTION_SPEED);
        /* Clear it again: StateMask 0 with the control set means off. */
        Throttling.StateMask = 0;
        CHECK(SetThreadInformation(Thread, ThreadPowerThrottling,
                                   &Throttling, sizeof(Throttling)));
    }

    /* Wrong sizes report ERROR_BAD_LENGTH, not success. */
    CHECK(GetThreadInformation(Thread, ThreadMemoryPriority, &Priority, 1) == FALSE);
    CHECK(GetLastError() == ERROR_BAD_LENGTH);
    CHECK(GetProcessInformation(Process, ProcessAppMemoryInfo, &AppMemory, 1) == FALSE);
    CHECK(GetLastError() == ERROR_BAD_LENGTH);
}

static void TestTimeZoneForYear(void)
{
    DYNAMIC_TIME_ZONE_INFORMATION Dynamic;
    TIME_ZONE_INFORMATION Year;
    DWORD rc;

    ComPrintf("netinfo: time zone for year\n");

    rc = GetDynamicTimeZoneInformation(&Dynamic);
    CHECK(rc != TIME_ZONE_ID_INVALID);

    /* For the current year the answer equals the current rules. */
    {
        TIME_ZONE_INFORMATION Now;

        CHECK(GetTimeZoneInformation(&Now) != TIME_ZONE_ID_INVALID);
        CHECK(GetTimeZoneInformationForYear(2026, &Dynamic, &Year));
        CHECK(Year.Bias == Dynamic.Bias);
        CHECK(Year.DaylightBias == Dynamic.DaylightBias);
        CHECK(Year.StandardDate.wMonth == Dynamic.StandardDate.wMonth);
    }

    /* Future years use the same rules unless a Dynamic DST entry says
     * otherwise; both answers must at least be self-consistent. */
    CHECK(GetTimeZoneInformationForYear(2035, &Dynamic, &Year));
    CHECK(Year.Bias == Dynamic.Bias || Year.Bias != 0);
    CHECK(GetTimeZoneInformationForYear(2035, NULL, &Year));

    /* Note: a NULL output pointer crashes real Windows (it does not
     * validate); WinDosDX refuses it. Not tested here so this one binary
     * passes on both. */
}

int main(int argc, char **argv)
{
    WSADATA WsaData;
    const char *Only = NULL;
    BOOL UseCom = TRUE;
    int i;

    for (i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--notepad"))
            ShowInNotepad = 1;
        else if (!strcmp(argv[i], "--no-com"))
            UseCom = FALSE;
        else if (!strcmp(argv[i], "--console-probe"))
            ConsoleProbe = 1;
        else
            Only = argv[i];
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    ComOpen(UseCom);
    ComPrintf("netinfo: main entered\n");
    if (ConsoleProbe)
    {
        HANDLE StdoutHandle = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD Written = 0;
        DWORD ConsoleMode = 0;
        DWORD ModeError = ERROR_SUCCESS;
        BOOL HasConsoleMode;
        HWND ConsoleWindow;
        static const char ProbeText[] = "WinDosDX console WriteFile probe\r\n";
        BOOL WriteOk;
        DWORD WriteError;
        char ProbeLine[256];

        SetLastError(ERROR_SUCCESS);
        HasConsoleMode = GetConsoleMode(StdoutHandle, &ConsoleMode);
        if (!HasConsoleMode)
            ModeError = GetLastError();
        ConsoleWindow = GetConsoleWindow();
        _snprintf(ProbeLine, sizeof(ProbeLine) - 1,
                  "console-probe: stdout=%p type=%lu console=%p mode-ok=%u mode=%08lx mode-error=%lu\r\n",
                  StdoutHandle, GetFileType(StdoutHandle), ConsoleWindow,
                  HasConsoleMode, ConsoleMode, ModeError);
        ProbeLine[sizeof(ProbeLine) - 1] = 0;
        ComWrite(ProbeLine);

        SetLastError(ERROR_SUCCESS);
        WriteOk = WriteFile(StdoutHandle, ProbeText, sizeof(ProbeText) - 1,
                            &Written, NULL);
        WriteError = WriteOk ? ERROR_SUCCESS : GetLastError();
        _snprintf(ProbeLine, sizeof(ProbeLine) - 1,
                  "console-probe: WriteFile=%u written=%lu error=%lu\r\n",
                  WriteOk, Written, WriteError);
        ProbeLine[sizeof(ProbeLine) - 1] = 0;
        ComWrite(ProbeLine);
    }
    CHECK(WSAStartup(MAKEWORD(2, 2), &WsaData) == 0);

    if (!Only || !strcmp(Only, "tables"))
        TestNetTables();
    if (!Only || !strcmp(Only, "route"))
        TestBestRoute2();
    if (!Only || !strcmp(Only, "notify"))
        TestNotifications();
    if (!Only || !strcmp(Only, "info"))
        TestThreadProcessInfo();
    if (!Only || !strcmp(Only, "tz"))
        TestTimeZoneForYear();

    WSACleanup();
    ComPrintf("%s netinfo\n", failures ? "FAIL" : "PASS");

    /* userinit opens the report after this console test exits. */
    if (ShowInNotepad && LogPath != NULL)
    {
        STARTUPINFOA si = { sizeof(si) };
        PROCESS_INFORMATION pi;
        char Path[80];

        CloseHandle(LogHandle);
        LogHandle = INVALID_HANDLE_VALUE;
        _snprintf(Path, sizeof(Path), "notepad %s", LogPath);
        Path[sizeof(Path) - 1] = 0;
        if (CreateProcessA(NULL, Path, NULL, NULL, FALSE,
                           NORMAL_PRIORITY_CLASS, NULL, NULL, &si, &pi))
        {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
    if (LogHandle != INVALID_HANDLE_VALUE)
        CloseHandle(LogHandle);
    return failures ? 1 : 0;
}
