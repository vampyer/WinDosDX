/*
 * PROJECT:     WinDosDX Win32 Base API
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Windows 7 to 10 kernel32 functions that current programs import
 * COPYRIGHT:   Process/thread attribute lists based on Wine's kernelbase
 *              (Copyright 1996-2020 the Wine project authors)
 *
 * Programs built for Windows 10 import these at startup even when they use
 * them only on some paths. Where WinDosDX has the feature, the function does
 * the work; where it does not (power requests, mitigation policies, app
 * packages), the function answers the way Windows does for a desktop program
 * on a machine without that feature.
 */

#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#undef WINVER
#define WINVER 0x0A00

#include "k32_vista.h"

#include <ndk/exfuncs.h>
#include <ndk/extypes.h>
#include <ndk/cmfuncs.h>
#include <ndk/mmtypes.h>
#include <ndk/pstypes.h>
#include <ndk/rtlfuncs.h>
#include <winnls.h>
#include <winreg.h>
#include <winerror.h>

/* "Dynamic DST" value names for the per-year time-zone rules. */
static UNICODE_STRING WdxDynamicDstFirst =
    RTL_CONSTANT_STRING(L"FirstEntry");
static UNICODE_STRING WdxDynamicDstLast =
    RTL_CONSTANT_STRING(L"LastEntry");
static UNICODE_STRING WdxDynamicDstByYear =
    RTL_CONSTANT_STRING(L"ByYear");

/* The registry REG_TZI layout, as the time zones key stores it. */
typedef struct _WDX_REG_TZI
{
    LONG Bias;
    LONG StandardBias;
    LONG DaylightBias;
    struct
    {
        USHORT Year;
        USHORT Month;
        USHORT DayOfWeek;
        USHORT Day;
        USHORT Hour;
        USHORT Minute;
        USHORT Second;
        USHORT Milliseconds;
    } StandardDate;
    struct
    {
        USHORT Year;
        USHORT Month;
        USHORT DayOfWeek;
        USHORT Day;
        USHORT Hour;
        USHORT Minute;
        USHORT Second;
        USHORT Milliseconds;
    } DaylightDate;
} WDX_REG_TZI;

#define NDEBUG
#include <debug.h>

/* Types from Windows 8/10 headers the SDK does not have yet. */
typedef struct _WIN10_MEMORY_RANGE_ENTRY
{
    PVOID VirtualAddress;
    SIZE_T NumberOfBytes;
} WIN10_MEMORY_RANGE_ENTRY, *PWIN10_MEMORY_RANGE_ENTRY;

typedef struct _WIN10_MEMORY_PRIORITY_INFORMATION
{
    ULONG MemoryPriority;
} WIN10_MEMORY_PRIORITY_INFORMATION;

#define WIN10_MEMORY_PRIORITY_NORMAL 5

/* In kernel32 (client/sysinfo.c) but not in the SDK headers. */
BOOL WINAPI GetLogicalProcessorInformation(PSYSTEM_LOGICAL_PROCESSOR_INFORMATION Buffer, PDWORD ReturnedLength);

/* A small lock for the lists below; each is held for a few instructions. */
static VOID
SpinAcquire(LONG volatile *Lock)
{
    while (InterlockedCompareExchange(Lock, 1, 0) != 0)
    {
        while (*Lock)
            YieldProcessor();
    }
}

static VOID
SpinRelease(LONG volatile *Lock)
{
    InterlockedExchange(Lock, 0);
}

/* ---------------------------------------------------------------------- */
/* WaitOnAddress (Windows 8)                                                */
/* ---------------------------------------------------------------------- */

/*
 * Waiters queue per address bucket and sleep on the global keyed event with
 * their own record as the key. A waker unlinks the waiter under the bucket
 * lock, marks it woken and releases the key after dropping the lock. A waiter
 * that times out re-checks under the lock: if a waker already unlinked it,
 * the release is on its way and must be consumed, or the waker would block.
 */

typedef struct _ADDRESS_WAITER
{
    LIST_ENTRY Entry;
    volatile VOID *Address;
    LONG Woken;
} ADDRESS_WAITER, *PADDRESS_WAITER;

typedef struct _ADDRESS_BUCKET
{
    LONG volatile Lock;
    LIST_ENTRY Waiters;
} ADDRESS_BUCKET, *PADDRESS_BUCKET;

#define ADDRESS_BUCKETS 64
static ADDRESS_BUCKET AddressBuckets[ADDRESS_BUCKETS];

static PADDRESS_BUCKET
LockAddressBucket(volatile VOID *Address)
{
    ULONG_PTR Value = (ULONG_PTR)Address;
    PADDRESS_BUCKET Bucket = &AddressBuckets[((Value >> 3) ^ (Value >> 9)) % ADDRESS_BUCKETS];

    SpinAcquire(&Bucket->Lock);
    if (!Bucket->Waiters.Flink)
        InitializeListHead(&Bucket->Waiters);
    return Bucket;
}

static BOOLEAN
AddressChanged(volatile VOID *Address, PVOID CompareAddress, SIZE_T AddressSize)
{
    switch (AddressSize)
    {
        case 1: return *(volatile UCHAR *)Address != *(PUCHAR)CompareAddress;
        case 2: return *(volatile USHORT *)Address != *(PUSHORT)CompareAddress;
        case 4: return *(volatile ULONG *)Address != *(PULONG)CompareAddress;
        default: return *(volatile ULONGLONG *)Address != *(PULONGLONG)CompareAddress;
    }
}

BOOL
WINAPI
WaitOnAddress(
    _In_ volatile VOID *Address,
    _In_ PVOID CompareAddress,
    _In_ SIZE_T AddressSize,
    _In_opt_ DWORD dwMilliseconds)
{
    DECLSPEC_ALIGN(8) ADDRESS_WAITER Waiter;
    PADDRESS_BUCKET Bucket;
    LARGE_INTEGER Timeout;
    NTSTATUS Status;

    if (AddressSize != 1 && AddressSize != 2 && AddressSize != 4 && AddressSize != 8)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    Bucket = LockAddressBucket(Address);
    if (AddressChanged(Address, CompareAddress, AddressSize))
    {
        SpinRelease(&Bucket->Lock);
        return TRUE;
    }
    Waiter.Address = Address;
    Waiter.Woken = FALSE;
    InsertTailList(&Bucket->Waiters, &Waiter.Entry);
    SpinRelease(&Bucket->Lock);

    Timeout.QuadPart = -(LONGLONG)dwMilliseconds * 10000;
    Status = NtWaitForKeyedEvent(NULL, &Waiter, FALSE,
                                 dwMilliseconds == INFINITE ? NULL : &Timeout);
    if (Status == STATUS_TIMEOUT)
    {
        SpinAcquire(&Bucket->Lock);
        if (!Waiter.Woken)
        {
            RemoveEntryList(&Waiter.Entry);
            SpinRelease(&Bucket->Lock);
            SetLastError(ERROR_TIMEOUT);
            return FALSE;
        }
        SpinRelease(&Bucket->Lock);
        /* A waker took us off the list: take its release. */
        NtWaitForKeyedEvent(NULL, &Waiter, FALSE, NULL);
    }
    return TRUE;
}

static VOID
WakeAddress(PVOID Address, BOOLEAN All)
{
    PADDRESS_BUCKET Bucket = LockAddressBucket(Address);
    PLIST_ENTRY Entry, Next;
    PADDRESS_WAITER Woken = NULL, Waiter;

    for (Entry = Bucket->Waiters.Flink; Entry != &Bucket->Waiters; Entry = Next)
    {
        Next = Entry->Flink;
        Waiter = CONTAINING_RECORD(Entry, ADDRESS_WAITER, Entry);
        if (Waiter->Address != Address)
            continue;
        RemoveEntryList(Entry);
        Waiter->Woken = TRUE;
        /* Chain the woken waiters through Entry.Flink. */
        Waiter->Entry.Flink = (PLIST_ENTRY)Woken;
        Woken = Waiter;
        if (!All)
            break;
    }
    SpinRelease(&Bucket->Lock);

    while (Woken)
    {
        /* The waiter's record is gone once it wakes: read the link first. */
        Waiter = Woken;
        Woken = (PADDRESS_WAITER)Waiter->Entry.Flink;
        NtReleaseKeyedEvent(NULL, Waiter, FALSE, NULL);
    }
}

VOID
WINAPI
WakeByAddressSingle(_In_ PVOID Address)
{
    WakeAddress(Address, FALSE);
}

VOID
WINAPI
WakeByAddressAll(_In_ PVOID Address)
{
    WakeAddress(Address, TRUE);
}

/* ---------------------------------------------------------------------- */
/* Errors and time                                                          */
/* ---------------------------------------------------------------------- */

/* Ends the process at once: no handlers, no unwinding (Windows 7). */
VOID
WINAPI
RaiseFailFastException(
    _In_opt_ PEXCEPTION_RECORD pExceptionRecord,
    _In_opt_ PCONTEXT pContextRecord,
    _In_ DWORD dwFlags)
{
    NTSTATUS Code = pExceptionRecord ? pExceptionRecord->ExceptionCode : STATUS_FAIL_FAST_EXCEPTION;

    UNREFERENCED_PARAMETER(pContextRecord);
    UNREFERENCED_PARAMETER(dwFlags);

    DPRINT1("RaiseFailFastException: code 0x%lx, ending the process\n", Code);
    if (NtCurrentPeb()->BeingDebugged)
        DbgBreakPoint();
    NtTerminateProcess(NtCurrentProcess(), Code);
}

/* Interrupt time; WinDosDX does not sleep, so it has no bias (Windows 7). */
BOOL
WINAPI
QueryUnbiasedInterruptTime(_Out_ PULONGLONG UnbiasedTime)
{
    ULARGE_INTEGER Time;

    if (!UnbiasedTime)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    do
    {
        Time.HighPart = SharedUserData->InterruptTime.High1Time;
        Time.LowPart = SharedUserData->InterruptTime.LowPart;
    } while (Time.HighPart != (ULONG)SharedUserData->InterruptTime.High2Time);
    *UnbiasedTime = Time.QuadPart;
    return TRUE;
}

/* ---------------------------------------------------------------------- */
/* Locales (Vista)                                                          */
/* ---------------------------------------------------------------------- */

/*
 * The closest specific locale for a name: "en-GB" stays, a neutral "en"
 * becomes the language's default ("en-US"), NULL means the user's locale.
 */
int
WINAPI
ResolveLocaleName(
    _In_opt_ LPCWSTR lpNameToResolve,
    _Out_writes_opt_(cchLocaleName) LPWSTR lpLocaleName,
    _In_ int cchLocaleName)
{
    WCHAR Name[LOCALE_NAME_MAX_LENGTH];
    LCID Lcid;
    PWSTR Dash;

    if (!lpNameToResolve)
    {
        if (!GetUserDefaultLocaleName(Name, ARRAYSIZE(Name)))
            return 0;
    }
    else
    {
        if (wcslen(lpNameToResolve) >= ARRAYSIZE(Name))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return 0;
        }
        wcscpy(Name, lpNameToResolve);
    }

    /* Drop trailing parts ("zh-Hant-TW" -> "zh-Hant" -> "zh") until one is known. */
    for (;;)
    {
        Lcid = LocaleNameToLCID(Name, LOCALE_ALLOW_NEUTRAL_NAMES);
        if (Lcid)
            break;
        Dash = wcsrchr(Name, L'-');
        if (!Dash)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return 0;
        }
        *Dash = UNICODE_NULL;
    }

    /* A neutral locale resolves to its language's default. */
    if (SUBLANGID(LANGIDFROMLCID(Lcid)) == SUBLANG_NEUTRAL)
        Lcid = ConvertDefaultLocale(MAKELCID(MAKELANGID(PRIMARYLANGID(LANGIDFROMLCID(Lcid)), SUBLANG_DEFAULT), SORT_DEFAULT));

    return LCIDToLocaleName(Lcid, lpLocaleName, lpLocaleName ? cchLocaleName : 0, 0);
}

/* ---------------------------------------------------------------------- */
/* Thread and process information (Windows 8)                               */
/* ---------------------------------------------------------------------- */

/*
 * The Windows 8 information classes are scheduler and memory-manager hints.
 * WinDosDX's kernel answers STATUS_INVALID_INFO_CLASS for them (checked in
 * ntoskrnl/ps/query.c), so the state is kept here, per process and thread
 * ID, the way Windows keys it: values survive handle duplication, and a
 * program that sets and reads back gets what it wrote. Validation follows
 * Windows: bad class, size, version or flags fail before anything is
 * stored. Nothing enforces the hints, exactly as Windows does on hardware
 * that ignores them.
 */

#define WDX_MEMORY_PRIORITY_DEFAULT 5   /* MEMORY_PRIORITY_NORMAL */
#define WDX_INFO_SLOTS 32

typedef struct _WDX_THREAD_INFO
{
    ULONG_PTR Key;                      /* thread ID */
    ULONG MemoryPriority;
    BOOLEAN PowerThrottlingValid;
    ULONG PowerThrottlingControlMask;
    ULONG PowerThrottlingStateMask;
} WDX_THREAD_INFO, *PWDX_THREAD_INFO;

typedef struct _WDX_PROCESS_INFO
{
    ULONG_PTR Key;                      /* process ID */
    ULONG MemoryPriority;
    BOOLEAN MemoryPriorityValid;
    BOOLEAN InPrivate;
    BOOLEAN PowerThrottlingValid;
    ULONG PowerThrottlingControlMask;
    ULONG PowerThrottlingStateMask;
    BOOLEAN MemoryExhaustionValid;
    USHORT MemoryExhaustionVersion;
    ULONG MemoryExhaustionType;
    ULONG_PTR MemoryExhaustionValue;
} WDX_PROCESS_INFO, *PWDX_PROCESS_INFO;

static SRWLOCK WdxInfoLock = SRWLOCK_INIT;
static WDX_THREAD_INFO WdxThreadInfo[WDX_INFO_SLOTS];
static WDX_PROCESS_INFO WdxProcessInfo[WDX_INFO_SLOTS];
static ULONG WdxThreadInfoNext = 1;     /* 0 is the first free slot */
static ULONG WdxProcessInfoNext = 1;

static PWDX_THREAD_INFO
WdxThreadInfoSlot(ULONG_PTR Key, BOOLEAN Create)
{
    ULONG i;
    ULONG Free = 0;
    BOOLEAN HaveFree = FALSE;

    for (i = 0; i < WDX_INFO_SLOTS; i++)
    {
        if (WdxThreadInfo[i].Key == Key)
            return &WdxThreadInfo[i];
        if (!WdxThreadInfo[i].Key && !HaveFree)
        {
            Free = i;
            HaveFree = TRUE;
        }
    }
    if (!Create)
        return NULL;
    if (HaveFree)
        i = Free;
    else
    {
        /* Full: replace round-robin. The hints are advisory; the oldest
         * entry is the least interesting one to lose. */
        i = WdxThreadInfoNext % WDX_INFO_SLOTS;
        WdxThreadInfoNext++;
    }
    RtlZeroMemory(&WdxThreadInfo[i], sizeof(WdxThreadInfo[i]));
    WdxThreadInfo[i].Key = Key;
    WdxThreadInfo[i].MemoryPriority = WDX_MEMORY_PRIORITY_DEFAULT;
    return &WdxThreadInfo[i];
}

static PWDX_PROCESS_INFO
WdxProcessInfoSlot(ULONG_PTR Key, BOOLEAN Create)
{
    ULONG i;
    ULONG Free = 0;
    BOOLEAN HaveFree = FALSE;

    for (i = 0; i < WDX_INFO_SLOTS; i++)
    {
        if (WdxProcessInfo[i].Key == Key)
            return &WdxProcessInfo[i];
        if (!WdxProcessInfo[i].Key && !HaveFree)
        {
            Free = i;
            HaveFree = TRUE;
        }
    }
    if (!Create)
        return NULL;
    if (HaveFree)
        i = Free;
    else
    {
        i = WdxProcessInfoNext % WDX_INFO_SLOTS;
        WdxProcessInfoNext++;
    }
    RtlZeroMemory(&WdxProcessInfo[i], sizeof(WdxProcessInfo[i]));
    WdxProcessInfo[i].Key = Key;
    return &WdxProcessInfo[i];
}

/* Thread information classes our headers lack (see processthreadsapi.h). */
#define WDX_ThreadAbsoluteCpuPriority 1
#define WDX_ThreadDynamicCodePolicy   2

/* Power throttling (Windows 10 RS3): version 1, one control flag. */
#define WDX_POWER_THROTTLING_CURRENT_VERSION 1
#define WDX_POWER_THROTTLING_EXECUTION_SPEED 0x1
#define WDX_PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#define WDX_POWER_THROTTLING_VALID_FLAGS (WDX_POWER_THROTTLING_EXECUTION_SPEED | \
                                          WDX_PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION)

typedef struct _WDX_POWER_THROTTLING_STATE
{
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
} WDX_POWER_THROTTLING_STATE, *PWDX_POWER_THROTTLING_STATE;

typedef struct _WDX_MEMORY_PRIORITY_INFORMATION
{
    ULONG MemoryPriority;
} WDX_MEMORY_PRIORITY_INFORMATION, *PWDX_MEMORY_PRIORITY_INFORMATION;

typedef struct _WDX_APP_MEMORY_INFORMATION
{
    ULONG64 AvailableCommit;
    ULONG64 PrivateCommitUsage;
    ULONG64 PeakPrivateCommitUsage;
    ULONG64 TotalCommitUsage;
} WDX_APP_MEMORY_INFORMATION, *PWDX_APP_MEMORY_INFORMATION;

typedef struct _WDX_PROCESS_MEMORY_EXHAUSTION_INFO
{
    USHORT Version;
    USHORT Reserved;
    ULONG Type;
    ULONG_PTR Value;
} WDX_PROCESS_MEMORY_EXHAUSTION_INFO, *PWDX_PROCESS_MEMORY_EXHAUSTION_INFO;

#define WDX_MEMORY_EXHAUSTION_CURRENT_VERSION 1
#define WDX_PME_TYPE_FAIL_FAST_ON_COMMIT_FAILURE 0

BOOL
WINAPI
SetThreadInformation(
    _In_ HANDLE hThread,
    _In_ THREAD_INFORMATION_CLASS ThreadInformationClass,
    _In_reads_bytes_(ThreadInformationSize) LPVOID ThreadInformation,
    _In_ DWORD ThreadInformationSize)
{
    ULONG_PTR Key;
    PWDX_THREAD_INFO Slot;

    if (!ThreadInformation)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (ThreadInformationClass == ThreadMemoryPriority)
    {
        PWDX_MEMORY_PRIORITY_INFORMATION Priority =
            (PWDX_MEMORY_PRIORITY_INFORMATION)ThreadInformation;

        if (ThreadInformationSize != sizeof(*Priority))
        {
            SetLastError(ERROR_BAD_LENGTH);
            return FALSE;
        }
        if (Priority->MemoryPriority > 7)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        Key = GetThreadId(hThread);
        if (!Key)
            return FALSE;
        AcquireSRWLockExclusive(&WdxInfoLock);
        Slot = WdxThreadInfoSlot(Key, TRUE);
        if (Slot)
            Slot->MemoryPriority = Priority->MemoryPriority;
        ReleaseSRWLockExclusive(&WdxInfoLock);
        if (!Slot)
        {
            SetLastError(ERROR_OUTOFMEMORY);
            return FALSE;
        }
        return TRUE;
    }
    if (ThreadInformationClass == ThreadPowerThrottling)
    {
        PWDX_POWER_THROTTLING_STATE Throttling =
            (PWDX_POWER_THROTTLING_STATE)ThreadInformation;

        if (ThreadInformationSize != sizeof(*Throttling))
        {
            SetLastError(ERROR_BAD_LENGTH);
            return FALSE;
        }
        if (Throttling->Version != WDX_POWER_THROTTLING_CURRENT_VERSION ||
            (Throttling->ControlMask & ~WDX_POWER_THROTTLING_VALID_FLAGS) ||
            (Throttling->StateMask & ~Throttling->ControlMask))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        Key = GetThreadId(hThread);
        if (!Key)
            return FALSE;
        AcquireSRWLockExclusive(&WdxInfoLock);
        Slot = WdxThreadInfoSlot(Key, TRUE);
        if (Slot)
        {
            Slot->PowerThrottlingValid = TRUE;
            Slot->PowerThrottlingControlMask = Throttling->ControlMask;
            Slot->PowerThrottlingStateMask = Throttling->StateMask;
        }
        ReleaseSRWLockExclusive(&WdxInfoLock);
        if (!Slot)
        {
            SetLastError(ERROR_OUTOFMEMORY);
            return FALSE;
        }
        return TRUE;
    }
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
}

BOOL
WINAPI
GetThreadInformation(
    _In_ HANDLE hThread,
    _In_ THREAD_INFORMATION_CLASS ThreadInformationClass,
    _Out_writes_bytes_(ThreadInformationSize) LPVOID ThreadInformation,
    _In_ DWORD ThreadInformationSize)
{
    ULONG_PTR Key;

    if (!ThreadInformation)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (ThreadInformationClass == ThreadMemoryPriority)
    {
        PWDX_MEMORY_PRIORITY_INFORMATION Priority =
            (PWDX_MEMORY_PRIORITY_INFORMATION)ThreadInformation;
        PWDX_THREAD_INFO Slot;

        if (ThreadInformationSize != sizeof(*Priority))
        {
            SetLastError(ERROR_BAD_LENGTH);
            return FALSE;
        }
        Key = GetThreadId(hThread);
        if (!Key)
            return FALSE;
        AcquireSRWLockShared(&WdxInfoLock);
        Slot = WdxThreadInfoSlot(Key, FALSE);
        Priority->MemoryPriority = Slot ? Slot->MemoryPriority
                                        : WDX_MEMORY_PRIORITY_DEFAULT;
        ReleaseSRWLockShared(&WdxInfoLock);
        return TRUE;
    }
    if (ThreadInformationClass == WDX_ThreadAbsoluteCpuPriority)
    {
        /* The absolute CPU priority is the kernel base priority of the
         * thread, which NtQueryInformationThread reports already. */
        THREAD_BASIC_INFORMATION Basic;
        NTSTATUS Status;

        if (ThreadInformationSize != sizeof(ULONG))
        {
            SetLastError(ERROR_BAD_LENGTH);
            return FALSE;
        }
        Status = NtQueryInformationThread(hThread, ThreadBasicInformation,
                                          &Basic, sizeof(Basic), NULL);
        if (!NT_SUCCESS(Status))
        {
            SetLastError(RtlNtStatusToDosError(Status));
            return FALSE;
        }
        *(ULONG *)ThreadInformation = (ULONG)Basic.BasePriority;
        return TRUE;
    }
    if (ThreadInformationClass == ThreadPowerThrottling)
    {
        PWDX_POWER_THROTTLING_STATE Throttling =
            (PWDX_POWER_THROTTLING_STATE)ThreadInformation;
        PWDX_THREAD_INFO Slot;

        if (ThreadInformationSize != sizeof(*Throttling))
        {
            SetLastError(ERROR_BAD_LENGTH);
            return FALSE;
        }
        Key = GetThreadId(hThread);
        if (!Key)
            return FALSE;
        AcquireSRWLockShared(&WdxInfoLock);
        Slot = WdxThreadInfoSlot(Key, FALSE);
        Throttling->Version = WDX_POWER_THROTTLING_CURRENT_VERSION;
        if (Slot && Slot->PowerThrottlingValid)
        {
            Throttling->ControlMask = Slot->PowerThrottlingControlMask;
            Throttling->StateMask = Slot->PowerThrottlingStateMask;
        }
        else
        {
            Throttling->ControlMask = 0;
            Throttling->StateMask = 0;
        }
        ReleaseSRWLockShared(&WdxInfoLock);
        return TRUE;
    }
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
}

BOOL
WINAPI
SetProcessInformation(
    _In_ HANDLE hProcess,
    _In_ PROCESS_INFORMATION_CLASS ProcessInformationClass,
    _In_reads_bytes_(ProcessInformationSize) LPVOID ProcessInformation,
    _In_ DWORD ProcessInformationSize)
{
    ULONG_PTR Key;
    PWDX_PROCESS_INFO Slot;

    if (!ProcessInformation)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    Key = GetProcessId(hProcess);
    if (!Key)
        return FALSE;

    if (ProcessInformationClass == ProcessMemoryPriority)
    {
        PWDX_MEMORY_PRIORITY_INFORMATION Priority =
            (PWDX_MEMORY_PRIORITY_INFORMATION)ProcessInformation;

        if (ProcessInformationSize != sizeof(*Priority))
        {
            SetLastError(ERROR_BAD_LENGTH);
            return FALSE;
        }
        if (Priority->MemoryPriority > 7)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        AcquireSRWLockExclusive(&WdxInfoLock);
        Slot = WdxProcessInfoSlot(Key, TRUE);
        if (Slot)
        {
            Slot->MemoryPriority = Priority->MemoryPriority;
            Slot->MemoryPriorityValid = TRUE;
        }
        ReleaseSRWLockExclusive(&WdxInfoLock);
        if (!Slot)
        {
            SetLastError(ERROR_OUTOFMEMORY);
            return FALSE;
        }
        return TRUE;
    }
    if (ProcessInformationClass == ProcessInPrivateInfo)
    {
        if (ProcessInformationSize != sizeof(BOOLEAN))
        {
            SetLastError(ERROR_BAD_LENGTH);
            return FALSE;
        }
        AcquireSRWLockExclusive(&WdxInfoLock);
        Slot = WdxProcessInfoSlot(Key, TRUE);
        if (Slot)
            Slot->InPrivate = *(BOOLEAN *)ProcessInformation != FALSE;
        ReleaseSRWLockExclusive(&WdxInfoLock);
        if (!Slot)
        {
            SetLastError(ERROR_OUTOFMEMORY);
            return FALSE;
        }
        return TRUE;
    }
    if (ProcessInformationClass == ProcessPowerThrottling)
    {
        PWDX_POWER_THROTTLING_STATE Throttling =
            (PWDX_POWER_THROTTLING_STATE)ProcessInformation;

        if (ProcessInformationSize != sizeof(*Throttling))
        {
            SetLastError(ERROR_BAD_LENGTH);
            return FALSE;
        }
        if (Throttling->Version != WDX_POWER_THROTTLING_CURRENT_VERSION ||
            (Throttling->ControlMask & ~WDX_POWER_THROTTLING_VALID_FLAGS) ||
            (Throttling->StateMask & ~Throttling->ControlMask))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        AcquireSRWLockExclusive(&WdxInfoLock);
        Slot = WdxProcessInfoSlot(Key, TRUE);
        if (Slot)
        {
            Slot->PowerThrottlingValid = TRUE;
            Slot->PowerThrottlingControlMask = Throttling->ControlMask;
            Slot->PowerThrottlingStateMask = Throttling->StateMask;
        }
        ReleaseSRWLockExclusive(&WdxInfoLock);
        if (!Slot)
        {
            SetLastError(ERROR_OUTOFMEMORY);
            return FALSE;
        }
        return TRUE;
    }
    if (ProcessInformationClass == ProcessMemoryExhaustionInfo)
    {
        PWDX_PROCESS_MEMORY_EXHAUSTION_INFO Info =
            (PWDX_PROCESS_MEMORY_EXHAUSTION_INFO)ProcessInformation;

        if (ProcessInformationSize != sizeof(*Info))
        {
            SetLastError(ERROR_BAD_LENGTH);
            return FALSE;
        }
        if (Info->Version != WDX_MEMORY_EXHAUSTION_CURRENT_VERSION ||
            Info->Type != WDX_PME_TYPE_FAIL_FAST_ON_COMMIT_FAILURE ||
            Info->Value > 1)
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return FALSE;
        }
        AcquireSRWLockExclusive(&WdxInfoLock);
        Slot = WdxProcessInfoSlot(Key, TRUE);
        if (Slot)
        {
            Slot->MemoryExhaustionValid = TRUE;
            Slot->MemoryExhaustionVersion = Info->Version;
            Slot->MemoryExhaustionType = Info->Type;
            Slot->MemoryExhaustionValue = Info->Value;
        }
        ReleaseSRWLockExclusive(&WdxInfoLock);
        if (!Slot)
        {
            SetLastError(ERROR_OUTOFMEMORY);
            return FALSE;
        }
        return TRUE;
    }
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
}

BOOL
WINAPI
GetProcessInformation(
    _In_ HANDLE hProcess,
    _In_ PROCESS_INFORMATION_CLASS ProcessInformationClass,
    _Out_writes_bytes_(ProcessInformationSize) LPVOID ProcessInformation,
    _In_ DWORD ProcessInformationSize)
{
    ULONG_PTR Key;
    PWDX_PROCESS_INFO Slot_;

    if (!ProcessInformation)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    Key = GetProcessId(hProcess);
    if (!Key)
        return FALSE;

    if (ProcessInformationClass == ProcessMemoryPriority)
    {
        PWDX_MEMORY_PRIORITY_INFORMATION Priority =
            (PWDX_MEMORY_PRIORITY_INFORMATION)ProcessInformation;

        if (ProcessInformationSize != sizeof(*Priority))
        {
            SetLastError(ERROR_BAD_LENGTH);
            return FALSE;
        }
        AcquireSRWLockShared(&WdxInfoLock);
        Slot_ = WdxProcessInfoSlot(Key, FALSE);
        Priority->MemoryPriority = Slot_ ? Slot_->MemoryPriority
                                         : WDX_MEMORY_PRIORITY_DEFAULT;
        ReleaseSRWLockShared(&WdxInfoLock);
        return TRUE;
    }
    if (ProcessInformationClass == ProcessAppMemoryInfo)
    {
        /* What the process really uses, from the kernel. */
        WDX_APP_MEMORY_INFORMATION *Info = (WDX_APP_MEMORY_INFORMATION *)ProcessInformation;
        VM_COUNTERS_EX Vm;
        SYSTEM_PERFORMANCE_INFORMATION Perf;
        NTSTATUS Status;

        if (ProcessInformationSize != sizeof(*Info))
        {
            SetLastError(ERROR_BAD_LENGTH);
            return FALSE;
        }
        Status = NtQueryInformationProcess(hProcess, ProcessVmCounters,
                                           &Vm, sizeof(Vm), NULL);
        if (!NT_SUCCESS(Status))
        {
            SetLastError(RtlNtStatusToDosError(Status));
            return FALSE;
        }
        Status = NtQuerySystemInformation(SystemPerformanceInformation,
                                          &Perf, sizeof(Perf), NULL);
        if (!NT_SUCCESS(Status))
        {
            SetLastError(RtlNtStatusToDosError(Status));
            return FALSE;
        }
        Info->PrivateCommitUsage = (ULONG64)Vm.PrivateUsage;
        Info->PeakPrivateCommitUsage = (ULONG64)Vm.PeakPagefileUsage;
        Info->TotalCommitUsage = (ULONG64)Vm.PrivateUsage;
        Info->AvailableCommit = Perf.CommitLimit > Perf.CommittedPages
                                ? (ULONG64)(Perf.CommitLimit - Perf.CommittedPages)
                                : 0;
        return TRUE;
    }
    if (ProcessInformationClass == ProcessPowerThrottling)
    {
        PWDX_POWER_THROTTLING_STATE Throttling =
            (PWDX_POWER_THROTTLING_STATE)ProcessInformation;

        if (ProcessInformationSize != sizeof(*Throttling))
        {
            SetLastError(ERROR_BAD_LENGTH);
            return FALSE;
        }
        AcquireSRWLockShared(&WdxInfoLock);
        Slot_ = WdxProcessInfoSlot(Key, FALSE);
        Throttling->Version = WDX_POWER_THROTTLING_CURRENT_VERSION;
        if (Slot_ && Slot_->PowerThrottlingValid)
        {
            Throttling->ControlMask = Slot_->PowerThrottlingControlMask;
            Throttling->StateMask = Slot_->PowerThrottlingStateMask;
        }
        else
        {
            Throttling->ControlMask = 0;
            Throttling->StateMask = 0;
        }
        ReleaseSRWLockShared(&WdxInfoLock);
        return TRUE;
    }
    if (ProcessInformationClass == ProcessMemoryExhaustionInfo)
    {
        PWDX_PROCESS_MEMORY_EXHAUSTION_INFO Info =
            (PWDX_PROCESS_MEMORY_EXHAUSTION_INFO)ProcessInformation;

        if (ProcessInformationSize != sizeof(*Info))
        {
            SetLastError(ERROR_BAD_LENGTH);
            return FALSE;
        }
        AcquireSRWLockShared(&WdxInfoLock);
        Slot_ = WdxProcessInfoSlot(Key, FALSE);
        Info->Version = Slot_ && Slot_->MemoryExhaustionValid
                        ? Slot_->MemoryExhaustionVersion
                        : WDX_MEMORY_EXHAUSTION_CURRENT_VERSION;
        Info->Reserved = 0;
        Info->Type = Slot_ && Slot_->MemoryExhaustionValid
                     ? Slot_->MemoryExhaustionType
                     : WDX_PME_TYPE_FAIL_FAST_ON_COMMIT_FAILURE;
        Info->Value = Slot_ && Slot_->MemoryExhaustionValid
                      ? Slot_->MemoryExhaustionValue : 0;
        ReleaseSRWLockShared(&WdxInfoLock);
        return TRUE;
    }
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
}

/* No mitigation is active; asking for one is accepted and has no effect. */
BOOL
WINAPI
GetProcessMitigationPolicy(
    _In_ HANDLE hProcess,
    _In_ ULONG MitigationPolicy,
    _Out_writes_bytes_(dwLength) PVOID lpBuffer,
    _In_ SIZE_T dwLength)
{
    UNREFERENCED_PARAMETER(hProcess);
    UNREFERENCED_PARAMETER(MitigationPolicy);

    if (!lpBuffer || !dwLength)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    RtlZeroMemory(lpBuffer, dwLength);
    return TRUE;
}

BOOL
WINAPI
SetProcessMitigationPolicy(
    _In_ ULONG MitigationPolicy,
    _In_reads_bytes_(dwLength) PVOID lpBuffer,
    _In_ SIZE_T dwLength)
{
    if (!lpBuffer || !dwLength)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    DPRINT("SetProcessMitigationPolicy(%lu) accepted, not enforced\n", MitigationPolicy);
    return TRUE;
}

/* ---------------------------------------------------------------------- */
/* Power requests (Windows 7)                                               */
/* ---------------------------------------------------------------------- */

/* A request is a plain handle the caller closes; WinDosDX never idles to sleep. */
HANDLE
WINAPI
PowerCreateRequest(_In_ PREASON_CONTEXT Context)
{
    UNREFERENCED_PARAMETER(Context);
    return CreateEventW(NULL, TRUE, FALSE, NULL);
}

BOOL
WINAPI
PowerSetRequest(_In_ HANDLE PowerRequest, _In_ ULONG RequestType)
{
    UNREFERENCED_PARAMETER(RequestType);

    if (!PowerRequest || PowerRequest == INVALID_HANDLE_VALUE)
    {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    return TRUE;
}

BOOL
WINAPI
PowerClearRequest(_In_ HANDLE PowerRequest, _In_ ULONG RequestType)
{
    return PowerSetRequest(PowerRequest, RequestType);
}

/* ---------------------------------------------------------------------- */
/* Memory (Windows 8 / 8.1)                                                 */
/* ---------------------------------------------------------------------- */

BOOL
WINAPI
PrefetchVirtualMemory(
    _In_ HANDLE hProcess,
    _In_ ULONG_PTR NumberOfEntries,
    _In_reads_(NumberOfEntries) PWIN10_MEMORY_RANGE_ENTRY VirtualAddresses,
    _In_ ULONG Flags)
{
    /* Only a hint: the pages come in when touched. */
    UNREFERENCED_PARAMETER(hProcess);
    UNREFERENCED_PARAMETER(NumberOfEntries);
    UNREFERENCED_PARAMETER(VirtualAddresses);
    UNREFERENCED_PARAMETER(Flags);
    return TRUE;
}

DWORD
WINAPI
DiscardVirtualMemory(
    _Inout_updates_(Size) PVOID VirtualAddress,
    _In_ SIZE_T Size)
{
    /* The contents may be thrown away: that is what MEM_RESET means. */
    if (!VirtualAlloc(VirtualAddress, Size, MEM_RESET, PAGE_NOACCESS))
        return GetLastError();
    return ERROR_SUCCESS;
}

/* ---------------------------------------------------------------------- */
/* DLL directories (Windows 7 with KB2533623)                               */
/* ---------------------------------------------------------------------- */

#define SEARCH_FLAGS (LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_APPLICATION_DIR | \
                      LOAD_LIBRARY_SEARCH_USER_DIRS | LOAD_LIBRARY_SEARCH_SYSTEM32 | \
                      LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)

typedef struct _USER_DLL_DIRECTORY
{
    LIST_ENTRY Entry;
    SIZE_T Length;              /* characters, no terminator */
    WCHAR Path[ANYSIZE_ARRAY];
} USER_DLL_DIRECTORY, *PUSER_DLL_DIRECTORY;

static LONG volatile DllDirectoriesLock;
static LIST_ENTRY DllDirectories;
static DWORD DefaultDllDirectoryFlags;

static VOID
LockDllDirectories(VOID)
{
    SpinAcquire(&DllDirectoriesLock);
    if (!DllDirectories.Flink)
        InitializeListHead(&DllDirectories);
}

BOOL
WINAPI
SetDefaultDllDirectories(_In_ DWORD DirectoryFlags)
{
    if (!DirectoryFlags || (DirectoryFlags & ~SEARCH_FLAGS))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    DefaultDllDirectoryFlags = DirectoryFlags;
    return TRUE;
}

DLL_DIRECTORY_COOKIE
WINAPI
AddDllDirectory(_In_ PCWSTR NewDirectory)
{
    PUSER_DLL_DIRECTORY Directory;
    RTL_PATH_TYPE Type;
    SIZE_T Length;

    if (!NewDirectory)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    Type = RtlDetermineDosPathNameType_U(NewDirectory);
    if (Type != RtlPathTypeDriveAbsolute && Type != RtlPathTypeUncAbsolute)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return NULL;
    }
    Length = wcslen(NewDirectory);
    Directory = RtlAllocateHeap(RtlGetProcessHeap(), 0,
                                FIELD_OFFSET(USER_DLL_DIRECTORY, Path[Length + 1]));
    if (!Directory)
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    Directory->Length = Length;
    RtlCopyMemory(Directory->Path, NewDirectory, (Length + 1) * sizeof(WCHAR));

    LockDllDirectories();
    InsertTailList(&DllDirectories, &Directory->Entry);
    SpinRelease(&DllDirectoriesLock);
    return (DLL_DIRECTORY_COOKIE)Directory;
}

BOOL
WINAPI
RemoveDllDirectory(_In_ DLL_DIRECTORY_COOKIE Cookie)
{
    PLIST_ENTRY Entry;

    LockDllDirectories();
    for (Entry = DllDirectories.Flink; Entry != &DllDirectories; Entry = Entry->Flink)
    {
        if (Entry == &((PUSER_DLL_DIRECTORY)Cookie)->Entry)
        {
            RemoveEntryList(Entry);
            SpinRelease(&DllDirectoriesLock);
            RtlFreeHeap(RtlGetProcessHeap(), 0, Cookie);
            return TRUE;
        }
    }
    SpinRelease(&DllDirectoriesLock);
    SetLastError(ERROR_INVALID_PARAMETER);
    return FALSE;
}

/*
 * Called by LoadLibraryExW with its search path: when the load (or the
 * process default) asks for user directories, append the ones added with
 * AddDllDirectory. Returns a new heap string, or the old one unchanged.
 */
PWSTR
WINAPI
BasepAppendUserDllDirectories(_In_ PWSTR SearchPath, _In_ DWORD Flags)
{
    PLIST_ENTRY Entry;
    SIZE_T Length, Extra = 0;
    PWSTR Result, End;

    if (!(Flags & SEARCH_FLAGS))
        Flags = DefaultDllDirectoryFlags;
    if (!(Flags & (LOAD_LIBRARY_SEARCH_USER_DIRS | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)))
        return SearchPath;

    LockDllDirectories();
    for (Entry = DllDirectories.Flink; Entry != &DllDirectories; Entry = Entry->Flink)
        Extra += CONTAINING_RECORD(Entry, USER_DLL_DIRECTORY, Entry)->Length + 1;
    if (!Extra)
    {
        SpinRelease(&DllDirectoriesLock);
        return SearchPath;
    }
    Length = wcslen(SearchPath);
    Result = RtlAllocateHeap(RtlGetProcessHeap(), 0, (Length + Extra + 1) * sizeof(WCHAR));
    if (!Result)
    {
        SpinRelease(&DllDirectoriesLock);
        return SearchPath;
    }
    RtlCopyMemory(Result, SearchPath, Length * sizeof(WCHAR));
    End = Result + Length;
    for (Entry = DllDirectories.Flink; Entry != &DllDirectories; Entry = Entry->Flink)
    {
        PUSER_DLL_DIRECTORY Directory = CONTAINING_RECORD(Entry, USER_DLL_DIRECTORY, Entry);
        *End++ = L';';
        RtlCopyMemory(End, Directory->Path, Directory->Length * sizeof(WCHAR));
        End += Directory->Length;
    }
    *End = UNICODE_NULL;
    SpinRelease(&DllDirectoriesLock);

    RtlFreeHeap(RtlGetProcessHeap(), 0, SearchPath);
    return Result;
}

/* ---------------------------------------------------------------------- */
/* Threads and processors (Windows 7)                                       */
/* ---------------------------------------------------------------------- */

HANDLE
WINAPI
CreateRemoteThreadEx(
    _In_ HANDLE hProcess,
    _In_opt_ LPSECURITY_ATTRIBUTES lpThreadAttributes,
    _In_ SIZE_T dwStackSize,
    _In_ LPTHREAD_START_ROUTINE lpStartAddress,
    _In_opt_ LPVOID lpParameter,
    _In_ DWORD dwCreationFlags,
    _In_opt_ LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList,
    _Out_opt_ LPDWORD lpThreadId)
{
    /* The attributes (group affinity, ideal processor) do not apply to one group. */
    UNREFERENCED_PARAMETER(lpAttributeList);
    return CreateRemoteThread(hProcess, lpThreadAttributes, dwStackSize, lpStartAddress,
                              lpParameter, dwCreationFlags, lpThreadId);
}

WORD
WINAPI
GetMaximumProcessorGroupCount(VOID)
{
    return 1;
}

/*
 * The Windows 7 form of processor information, built from the older one:
 * WinDosDX has one processor group and one NUMA node.
 */
static DWORD
ExInfoSize(LOGICAL_PROCESSOR_RELATIONSHIP Relation)
{
    SIZE_T Header = FIELD_OFFSET(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX, Processor);

    switch (Relation)
    {
        case RelationProcessorCore:
        case RelationProcessorPackage:
            return (DWORD)(Header + sizeof(PROCESSOR_RELATIONSHIP));
        case RelationNumaNode:
            return (DWORD)(Header + sizeof(NUMA_NODE_RELATIONSHIP));
        case RelationCache:
            return (DWORD)(Header + sizeof(CACHE_RELATIONSHIP));
        case RelationGroup:
            return (DWORD)(Header + sizeof(GROUP_RELATIONSHIP));
        default:
            return 0;
    }
}

BOOL
WINAPI
GetLogicalProcessorInformationEx(
    _In_ LOGICAL_PROCESSOR_RELATIONSHIP RelationshipType,
    _Out_writes_bytes_to_opt_(*ReturnedLength, *ReturnedLength) PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX Buffer,
    _Inout_ PDWORD ReturnedLength)
{
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION Info = NULL;
    DWORD InfoLength = 0, Needed, Count, i;
    PUCHAR Out = (PUCHAR)Buffer;
    SYSTEM_INFO SystemInfo;
    BOOL Fits;

    if (!ReturnedLength ||
        (RelationshipType > RelationGroup && RelationshipType != RelationAll))
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    GetLogicalProcessorInformation(NULL, &InfoLength);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !InfoLength)
        return FALSE;
    Info = RtlAllocateHeap(RtlGetProcessHeap(), 0, InfoLength);
    if (!Info)
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }
    if (!GetLogicalProcessorInformation(Info, &InfoLength))
    {
        RtlFreeHeap(RtlGetProcessHeap(), 0, Info);
        return FALSE;
    }
    Count = InfoLength / sizeof(*Info);
    GetSystemInfo(&SystemInfo);

    /* First pass sizes, second pass writes (when it all fits). */
    Needed = 0;
    for (i = 0; i < Count; i++)
    {
        if (RelationshipType == RelationAll || RelationshipType == Info[i].Relationship)
            Needed += ExInfoSize(Info[i].Relationship);
    }
    if (RelationshipType == RelationAll || RelationshipType == RelationGroup)
        Needed += ExInfoSize(RelationGroup);

    Fits = Buffer && *ReturnedLength >= Needed;
    if (Fits)
    {
        RtlZeroMemory(Buffer, Needed);
        for (i = 0; i < Count; i++)
        {
            PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX Ex = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)Out;
            LOGICAL_PROCESSOR_RELATIONSHIP Relation = Info[i].Relationship;
            DWORD Size = ExInfoSize(Relation);

            if (!Size || (RelationshipType != RelationAll && RelationshipType != Relation))
                continue;
            Ex->Relationship = Relation;
            Ex->Size = Size;
            switch (Relation)
            {
                case RelationProcessorCore:
                case RelationProcessorPackage:
                    Ex->Processor.Flags = Relation == RelationProcessorCore ? Info[i].ProcessorCore.Flags : 0;
                    Ex->Processor.GroupCount = 1;
                    Ex->Processor.GroupMask[0].Mask = Info[i].ProcessorMask;
                    break;
                case RelationNumaNode:
                    Ex->NumaNode.NodeNumber = Info[i].NumaNode.NodeNumber;
                    Ex->NumaNode.GroupMask.Mask = Info[i].ProcessorMask;
                    break;
                case RelationCache:
                    Ex->Cache.Level = Info[i].Cache.Level;
                    Ex->Cache.Associativity = Info[i].Cache.Associativity;
                    Ex->Cache.LineSize = Info[i].Cache.LineSize;
                    Ex->Cache.CacheSize = Info[i].Cache.Size;
                    Ex->Cache.Type = Info[i].Cache.Type;
                    Ex->Cache.GroupMask.Mask = Info[i].ProcessorMask;
                    break;
                default:
                    break;
            }
            Out += Size;
        }
        if (RelationshipType == RelationAll || RelationshipType == RelationGroup)
        {
            PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX Ex = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)Out;
            Ex->Relationship = RelationGroup;
            Ex->Size = ExInfoSize(RelationGroup);
            Ex->Group.MaximumGroupCount = 1;
            Ex->Group.ActiveGroupCount = 1;
            Ex->Group.GroupInfo[0].MaximumProcessorCount = (UCHAR)SystemInfo.dwNumberOfProcessors;
            Ex->Group.GroupInfo[0].ActiveProcessorCount = (UCHAR)SystemInfo.dwNumberOfProcessors;
            Ex->Group.GroupInfo[0].ActiveProcessorMask = SystemInfo.dwActiveProcessorMask;
        }
    }
    RtlFreeHeap(RtlGetProcessHeap(), 0, Info);

    *ReturnedLength = Needed;
    if (!Fits)
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return FALSE;
    }
    return TRUE;
}

/* ---------------------------------------------------------------------- */
/* Process/thread attribute lists (Vista), after Wine                       */
/* ---------------------------------------------------------------------- */

/*
 * CreateProcess does not act on the attributes yet (it ignores
 * EXTENDED_STARTUPINFO_PRESENT); the list is still built and checked so
 * programs that prepare one can start their children.
 */
struct proc_thread_attr
{
    DWORD_PTR attr;
    SIZE_T size;
    void *value;
};

struct _PROC_THREAD_ATTRIBUTE_LIST
{
    DWORD mask;  /* bitmask of items in list */
    DWORD size;  /* max number of items in list */
    DWORD count; /* number of items in list */
    DWORD pad;
    DWORD_PTR unk;
    struct proc_thread_attr attrs[1];
};

BOOL
WINAPI
InitializeProcThreadAttributeList(
    _Out_writes_bytes_to_opt_(*lpSize, *lpSize) LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList,
    _In_ DWORD dwAttributeCount,
    _Reserved_ DWORD dwFlags,
    _Inout_ PSIZE_T lpSize)
{
    SIZE_T Needed = FIELD_OFFSET(struct _PROC_THREAD_ATTRIBUTE_LIST, attrs[dwAttributeCount]);
    BOOL Ret = FALSE;

    UNREFERENCED_PARAMETER(dwFlags);

    if (lpAttributeList && *lpSize >= Needed)
    {
        lpAttributeList->mask = 0;
        lpAttributeList->size = dwAttributeCount;
        lpAttributeList->count = 0;
        lpAttributeList->unk = 0;
        Ret = TRUE;
    }
    else
    {
        SetLastError(ERROR_INSUFFICIENT_BUFFER);
    }
    *lpSize = Needed;
    return Ret;
}

static DWORD
ValidateProcThreadAttribute(DWORD_PTR Attribute, SIZE_T Size)
{
    switch (Attribute)
    {
        case PROC_THREAD_ATTRIBUTE_PARENT_PROCESS:
            return Size == sizeof(HANDLE) ? 0 : ERROR_BAD_LENGTH;
        case PROC_THREAD_ATTRIBUTE_HANDLE_LIST:
            return (Size / sizeof(HANDLE)) * sizeof(HANDLE) == Size ? 0 : ERROR_BAD_LENGTH;
        case PROC_THREAD_ATTRIBUTE_IDEAL_PROCESSOR:
            return Size == sizeof(PROCESSOR_NUMBER) ? 0 : ERROR_BAD_LENGTH;
        case PROC_THREAD_ATTRIBUTE_GROUP_AFFINITY:
            return Size == sizeof(GROUP_AFFINITY) ? 0 : ERROR_BAD_LENGTH;
        case ProcThreadAttributeValue(7, FALSE, TRUE, TRUE):    /* MITIGATION_POLICY */
            return (Size == sizeof(DWORD) || Size == sizeof(DWORD64) || Size == 2 * sizeof(DWORD64)) ? 0 : ERROR_BAD_LENGTH;
        case ProcThreadAttributeValue(14, FALSE, TRUE, FALSE):  /* CHILD_PROCESS_POLICY */
            return (Size == sizeof(DWORD) || Size == sizeof(DWORD64)) ? 0 : ERROR_BAD_LENGTH;
        case ProcThreadAttributeValue(13, FALSE, TRUE, FALSE):  /* JOB_LIST */
            return (Size / sizeof(HANDLE)) * sizeof(HANDLE) == Size ? 0 : ERROR_BAD_LENGTH;
        case ProcThreadAttributeValue(22, FALSE, TRUE, FALSE):  /* PSEUDOCONSOLE */
            return Size == sizeof(HANDLE) ? 0 : ERROR_BAD_LENGTH;
        default:
            DPRINT1("UpdateProcThreadAttribute: attribute 0x%Ix not supported\n", Attribute);
            return ERROR_NOT_SUPPORTED;
    }
}

BOOL
WINAPI
UpdateProcThreadAttribute(
    _Inout_ LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList,
    _In_ DWORD dwFlags,
    _In_ DWORD_PTR Attribute,
    _In_reads_bytes_opt_(cbSize) PVOID lpValue,
    _In_ SIZE_T cbSize,
    _Out_writes_bytes_opt_(cbSize) PVOID lpPreviousValue,
    _In_opt_ PSIZE_T lpReturnSize)
{
    struct proc_thread_attr *Entry;
    DWORD Mask, Error;

    UNREFERENCED_PARAMETER(dwFlags);
    UNREFERENCED_PARAMETER(lpPreviousValue);
    UNREFERENCED_PARAMETER(lpReturnSize);

    if (lpAttributeList->count >= lpAttributeList->size)
    {
        SetLastError(ERROR_GEN_FAILURE);
        return FALSE;
    }
    Error = ValidateProcThreadAttribute(Attribute, cbSize);
    if (Error)
    {
        SetLastError(Error);
        return FALSE;
    }
    Mask = 1 << (Attribute & PROC_THREAD_ATTRIBUTE_NUMBER);
    if (lpAttributeList->mask & Mask)
    {
        SetLastError(ERROR_OBJECT_NAME_EXISTS);
        return FALSE;
    }
    lpAttributeList->mask |= Mask;

    Entry = lpAttributeList->attrs + lpAttributeList->count;
    Entry->attr = Attribute;
    Entry->size = cbSize;
    Entry->value = lpValue;
    lpAttributeList->count++;
    return TRUE;
}

VOID
WINAPI
DeleteProcThreadAttributeList(_Inout_ LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList)
{
    UNREFERENCED_PARAMETER(lpAttributeList);
}

/* ---------------------------------------------------------------------- */
/* Error reporting and app packages (Windows 7 / 8)                         */
/* ---------------------------------------------------------------------- */

HRESULT
WINAPI
WerRegisterRuntimeExceptionModule(_In_ PCWSTR pwszOutOfProcessCallbackDll, _In_ PVOID pContext)
{
    UNREFERENCED_PARAMETER(pwszOutOfProcessCallbackDll);
    UNREFERENCED_PARAMETER(pContext);
    return S_OK;
}

HRESULT
WINAPI
WerUnregisterRuntimeExceptionModule(_In_ PCWSTR pwszOutOfProcessCallbackDll, _In_ PVOID pContext)
{
    UNREFERENCED_PARAMETER(pwszOutOfProcessCallbackDll);
    UNREFERENCED_PARAMETER(pContext);
    return S_OK;
}

/* Desktop programs have no package identity: the answer Windows gives them. */
LONG
WINAPI
GetCurrentPackageFullName(_Inout_ UINT32 *packageFullNameLength, _Out_opt_ PWSTR packageFullName)
{
    UNREFERENCED_PARAMETER(packageFullNameLength);
    UNREFERENCED_PARAMETER(packageFullName);
    return APPMODEL_ERROR_NO_PACKAGE;
}

LONG
WINAPI
GetCurrentPackageFamilyName(_Inout_ UINT32 *packageFamilyNameLength, _Out_opt_ PWSTR packageFamilyName)
{
    UNREFERENCED_PARAMETER(packageFamilyNameLength);
    UNREFERENCED_PARAMETER(packageFamilyName);
    return APPMODEL_ERROR_NO_PACKAGE;
}

LONG
WINAPI
GetCurrentPackagePath(_Inout_ UINT32 *pathLength, _Out_opt_ PWSTR path)
{
    UNREFERENCED_PARAMETER(pathLength);
    UNREFERENCED_PARAMETER(path);
    return APPMODEL_ERROR_NO_PACKAGE;
}

LONG
WINAPI
GetCurrentApplicationUserModelId(_Inout_ UINT32 *applicationUserModelIdLength, _Out_opt_ PWSTR applicationUserModelId)
{
    UNREFERENCED_PARAMETER(applicationUserModelIdLength);
    UNREFERENCED_PARAMETER(applicationUserModelId);
    return APPMODEL_ERROR_NO_PACKAGE;
}

LONG
WINAPI
GetPackagePathByFullName(_In_ PCWSTR packageFullName, _Inout_ UINT32 *pathLength, _Out_opt_ PWSTR path)
{
    UNREFERENCED_PARAMETER(packageFullName);
    UNREFERENCED_PARAMETER(pathLength);
    UNREFERENCED_PARAMETER(path);
    return ERROR_NOT_FOUND;
}

/* ---------------------------------------------------------------------- */
/* Windows Error Reporting settings (Vista)                                 */
/* ---------------------------------------------------------------------- */

/* There is no error reporting service; the flags only round-trip. Go
 * programs read and set them at startup. */
static DWORD WerFlags;

HRESULT
WINAPI
WerGetFlags(_In_ HANDLE hProcess, _Out_ PDWORD pdwFlags)
{
    UNREFERENCED_PARAMETER(hProcess);
    if (!pdwFlags)
        return E_INVALIDARG;
    *pdwFlags = WerFlags;
    return S_OK;
}

HRESULT
WINAPI
WerSetFlags(_In_ DWORD dwFlags)
{
    WerFlags = dwFlags;
    return S_OK;
}

HRESULT
WINAPI
WerRegisterFile(_In_ PCWSTR pwzFile, _In_ ULONG regFileType, _In_ DWORD dwFlags)
{
    UNREFERENCED_PARAMETER(pwzFile);
    UNREFERENCED_PARAMETER(regFileType);
    UNREFERENCED_PARAMETER(dwFlags);
    return S_OK;
}

HRESULT
WINAPI
WerUnregisterFile(_In_ PCWSTR pwzFilePath)
{
    UNREFERENCED_PARAMETER(pwzFilePath);
    return S_OK;
}

HRESULT
WINAPI
WerRegisterMemoryBlock(_In_ PVOID pvAddress, _In_ DWORD dwSize)
{
    UNREFERENCED_PARAMETER(pvAddress);
    UNREFERENCED_PARAMETER(dwSize);
    return S_OK;
}

HRESULT
WINAPI
WerUnregisterMemoryBlock(_In_ PVOID pvAddress)
{
    UNREFERENCED_PARAMETER(pvAddress);
    return S_OK;
}

/* ---------------------------------------------------------------------- */
/* CreateFile2 (Windows 8)                                                  */
/* ---------------------------------------------------------------------- */

typedef struct _WIN10_CREATEFILE2_EXTENDED_PARAMETERS
{
    DWORD dwSize;
    DWORD dwFileAttributes;
    DWORD dwFileFlags;
    DWORD dwSecurityQosFlags;
    LPSECURITY_ATTRIBUTES lpSecurityAttributes;
    HANDLE hTemplateFile;
} WIN10_CREATEFILE2_EXTENDED_PARAMETERS;

HANDLE
WINAPI
CreateFile2(
    _In_ LPCWSTR lpFileName,
    _In_ DWORD dwDesiredAccess,
    _In_ DWORD dwShareMode,
    _In_ DWORD dwCreationDisposition,
    _In_opt_ WIN10_CREATEFILE2_EXTENDED_PARAMETERS *pCreateExParams)
{
    LPSECURITY_ATTRIBUTES Security = NULL;
    DWORD FlagsAndAttributes = 0;
    HANDLE Template = NULL;

    if (pCreateExParams)
    {
        if (pCreateExParams->dwSize != sizeof(*pCreateExParams))
        {
            SetLastError(ERROR_INVALID_PARAMETER);
            return INVALID_HANDLE_VALUE;
        }
        FlagsAndAttributes = pCreateExParams->dwFileAttributes | pCreateExParams->dwFileFlags |
                             pCreateExParams->dwSecurityQosFlags;
        Security = pCreateExParams->lpSecurityAttributes;
        Template = pCreateExParams->hTemplateFile;
    }
    return CreateFileW(lpFileName, dwDesiredAccess, dwShareMode, Security, dwCreationDisposition,
                       FlagsAndAttributes, Template);
}

/* ---------------------------------------------------------------------- */
/* Synchronization objects with flags and access masks (Vista)              */
/* ---------------------------------------------------------------------- */

/* The access mask is not narrowed: the handles get full access, which any
 * access the caller asked for is part of. */

#define WIN10_CREATE_EVENT_MANUAL_RESET          0x00000001
#define WIN10_CREATE_EVENT_INITIAL_SET           0x00000002
#define WIN10_CREATE_MUTEX_INITIAL_OWNER         0x00000001
#define WIN10_CREATE_WAITABLE_TIMER_MANUAL_RESET 0x00000001

HANDLE
WINAPI
CreateEventExW(
    _In_opt_ LPSECURITY_ATTRIBUTES lpEventAttributes,
    _In_opt_ LPCWSTR lpName,
    _In_ DWORD dwFlags,
    _In_ DWORD dwDesiredAccess)
{
    UNREFERENCED_PARAMETER(dwDesiredAccess);
    return CreateEventW(lpEventAttributes, !!(dwFlags & WIN10_CREATE_EVENT_MANUAL_RESET),
                        !!(dwFlags & WIN10_CREATE_EVENT_INITIAL_SET), lpName);
}

HANDLE
WINAPI
CreateEventExA(
    _In_opt_ LPSECURITY_ATTRIBUTES lpEventAttributes,
    _In_opt_ LPCSTR lpName,
    _In_ DWORD dwFlags,
    _In_ DWORD dwDesiredAccess)
{
    UNREFERENCED_PARAMETER(dwDesiredAccess);
    return CreateEventA(lpEventAttributes, !!(dwFlags & WIN10_CREATE_EVENT_MANUAL_RESET),
                        !!(dwFlags & WIN10_CREATE_EVENT_INITIAL_SET), lpName);
}

HANDLE
WINAPI
CreateMutexExW(
    _In_opt_ LPSECURITY_ATTRIBUTES lpMutexAttributes,
    _In_opt_ LPCWSTR lpName,
    _In_ DWORD dwFlags,
    _In_ DWORD dwDesiredAccess)
{
    UNREFERENCED_PARAMETER(dwDesiredAccess);
    return CreateMutexW(lpMutexAttributes, !!(dwFlags & WIN10_CREATE_MUTEX_INITIAL_OWNER), lpName);
}

HANDLE
WINAPI
CreateMutexExA(
    _In_opt_ LPSECURITY_ATTRIBUTES lpMutexAttributes,
    _In_opt_ LPCSTR lpName,
    _In_ DWORD dwFlags,
    _In_ DWORD dwDesiredAccess)
{
    UNREFERENCED_PARAMETER(dwDesiredAccess);
    return CreateMutexA(lpMutexAttributes, !!(dwFlags & WIN10_CREATE_MUTEX_INITIAL_OWNER), lpName);
}

HANDLE
WINAPI
CreateWaitableTimerExW(
    _In_opt_ LPSECURITY_ATTRIBUTES lpTimerAttributes,
    _In_opt_ LPCWSTR lpTimerName,
    _In_ DWORD dwFlags,
    _In_ DWORD dwDesiredAccess)
{
    /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION needs no special handling. */
    UNREFERENCED_PARAMETER(dwDesiredAccess);
    return CreateWaitableTimerW(lpTimerAttributes, !!(dwFlags & WIN10_CREATE_WAITABLE_TIMER_MANUAL_RESET),
                                lpTimerName);
}

HANDLE
WINAPI
CreateWaitableTimerExA(
    _In_opt_ LPSECURITY_ATTRIBUTES lpTimerAttributes,
    _In_opt_ LPCSTR lpTimerName,
    _In_ DWORD dwFlags,
    _In_ DWORD dwDesiredAccess)
{
    UNREFERENCED_PARAMETER(dwDesiredAccess);
    return CreateWaitableTimerA(lpTimerAttributes, !!(dwFlags & WIN10_CREATE_WAITABLE_TIMER_MANUAL_RESET),
                                lpTimerName);
}

/* ---------------------------------------------------------------------- */
/* GetQueuedCompletionStatusEx (Vista)                                      */
/* ---------------------------------------------------------------------- */

/*
 * Several completion packets in one call: the first is waited for, the
 * rest are taken only if already queued. An alertable wait runs queued
 * APCs before waiting, which covers how programs use the flag.
 */
BOOL
WINAPI
GetQueuedCompletionStatusEx(
    _In_ HANDLE CompletionPort,
    _Out_writes_to_(ulCount, *ulNumEntriesRemoved) LPOVERLAPPED_ENTRY lpCompletionPortEntries,
    _In_ ULONG ulCount,
    _Out_ PULONG ulNumEntriesRemoved,
    _In_ DWORD dwMilliseconds,
    _In_ BOOL fAlertable)
{
    ULONG Removed = 0;

    if (!lpCompletionPortEntries || !ulCount || !ulNumEntriesRemoved)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (fAlertable && SleepEx(0, TRUE) == WAIT_IO_COMPLETION)
    {
        *ulNumEntriesRemoved = 0;
        SetLastError(WAIT_IO_COMPLETION);
        return FALSE;
    }

    while (Removed < ulCount)
    {
        DWORD Bytes = 0;
        ULONG_PTR Key = 0;
        LPOVERLAPPED Overlapped = NULL;
        BOOL Ok = GetQueuedCompletionStatus(CompletionPort, &Bytes, &Key, &Overlapped,
                                            Removed ? 0 : dwMilliseconds);

        /* No packet: a timeout, or the port was closed. */
        if (!Ok && !Overlapped)
        {
            if (Removed)
                break;
            *ulNumEntriesRemoved = 0;
            return FALSE;       /* GetLastError is WAIT_TIMEOUT or the error */
        }
        /* A packet, successful or for a failed I/O: both are entries. */
        lpCompletionPortEntries[Removed].lpCompletionKey = Key;
        lpCompletionPortEntries[Removed].lpOverlapped = Overlapped;
        lpCompletionPortEntries[Removed].Internal = Overlapped ? Overlapped->Internal : 0;
        lpCompletionPortEntries[Removed].dwNumberOfBytesTransferred = Bytes;
        Removed++;
    }
    *ulNumEntriesRemoved = Removed;
    return TRUE;
}

/* ---------------------------------------------------------------------- */
/* Dynamic time zones (Vista)                                               */
/* ---------------------------------------------------------------------- */

/* WinDosDX keeps one set of rules per zone, so the dynamic information is
 * the classic one plus the zone's key name (its standard name, as for the
 * English Windows zones). */
DWORD
WINAPI
GetDynamicTimeZoneInformation(_Out_ PDYNAMIC_TIME_ZONE_INFORMATION pTimeZoneInformation)
{
    TIME_ZONE_INFORMATION Info;
    DWORD Result;

    if (!pTimeZoneInformation)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return TIME_ZONE_ID_INVALID;
    }
    Result = GetTimeZoneInformation(&Info);
    if (Result == TIME_ZONE_ID_INVALID)
        return Result;

    RtlZeroMemory(pTimeZoneInformation, sizeof(*pTimeZoneInformation));
    pTimeZoneInformation->Bias = Info.Bias;
    RtlCopyMemory(pTimeZoneInformation->StandardName, Info.StandardName, sizeof(Info.StandardName));
    pTimeZoneInformation->StandardDate = Info.StandardDate;
    pTimeZoneInformation->StandardBias = Info.StandardBias;
    RtlCopyMemory(pTimeZoneInformation->DaylightName, Info.DaylightName, sizeof(Info.DaylightName));
    pTimeZoneInformation->DaylightDate = Info.DaylightDate;
    pTimeZoneInformation->DaylightBias = Info.DaylightBias;
    RtlCopyMemory(pTimeZoneInformation->TimeZoneKeyName, Info.StandardName, sizeof(Info.StandardName));
    pTimeZoneInformation->DynamicDaylightTimeDisabled = FALSE;
    return Result;
}

BOOL
WINAPI
SetDynamicTimeZoneInformation(_In_ const DYNAMIC_TIME_ZONE_INFORMATION *lpTimeZoneInformation)
{
    TIME_ZONE_INFORMATION Info;

    if (!lpTimeZoneInformation)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    Info.Bias = lpTimeZoneInformation->Bias;
    RtlCopyMemory(Info.StandardName, lpTimeZoneInformation->StandardName, sizeof(Info.StandardName));
    Info.StandardDate = lpTimeZoneInformation->StandardDate;
    Info.StandardBias = lpTimeZoneInformation->StandardBias;
    RtlCopyMemory(Info.DaylightName, lpTimeZoneInformation->DaylightName, sizeof(Info.DaylightName));
    Info.DaylightDate = lpTimeZoneInformation->DaylightDate;
    Info.DaylightBias = lpTimeZoneInformation->DaylightBias;
    return SetTimeZoneInformation(&Info);
}

/* The same rules every year: those of the given zone, or the current one. */
BOOL
WINAPI
GetTimeZoneInformationForYear(
    _In_ USHORT wYear,
    _In_opt_ PDYNAMIC_TIME_ZONE_INFORMATION pdtzi,
    _Out_ LPTIME_ZONE_INFORMATION ptzi)
{
    TIME_ZONE_INFORMATION *Out = ptzi;
    DYNAMIC_TIME_ZONE_INFORMATION Current;

    if (!ptzi)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }

    /* No zone named: ask for the current one, whose key name may name the
     * zone the Dynamic DST data lives under. */
    if (!pdtzi)
    {
        DWORD Result = GetDynamicTimeZoneInformation(&Current);
        if (Result == TIME_ZONE_ID_INVALID)
            return FALSE;
        pdtzi = &Current;
    }

    /* Base: the rules the caller passed (or the current zone's). */
    Out->Bias = pdtzi->Bias;
    RtlCopyMemory(Out->StandardName, pdtzi->StandardName, sizeof(Out->StandardName));
    Out->StandardDate = pdtzi->StandardDate;
    Out->StandardBias = pdtzi->StandardBias;
    RtlCopyMemory(Out->DaylightName, pdtzi->DaylightName, sizeof(Out->DaylightName));
    Out->DaylightDate = pdtzi->DaylightDate;
    Out->DaylightBias = pdtzi->DaylightBias;

    if (pdtzi->DynamicDaylightTimeDisabled)
    {
        /* Daylight switching turned off: standard time all year. */
        RtlZeroMemory(&Out->DaylightDate, sizeof(Out->DaylightDate));
        return TRUE;
    }

    /* Windows stores per-year rule overrides as a "Dynamic DST" subkey of
     * the zone's key: FirstEntry, LastEntry and ByYear, one REG_TZI blob
     * per year. When present, the named year's rules replace the base. */
    {
        NTSTATUS Status;
        HANDLE KeyHandle;
        WCHAR ZonePath[256];
        UNICODE_STRING ZonePathString;
        OBJECT_ATTRIBUTES ObjectAttributes;
        struct
        {
            KEY_VALUE_PARTIAL_INFORMATION Info;
            ULONG Payload[8];
        } Partial;
        ULONG FirstEntry, LastEntry;
        PKEY_VALUE_PARTIAL_INFORMATION Value;

        _snwprintf(ZonePath, ARRAYSIZE(ZonePath),
                   L"\\Registry\\Machine\\SOFTWARE\\Microsoft\\Windows NT\\"
                   L"CurrentVersion\\Time Zones\\%s\\Dynamic DST",
                   pdtzi->TimeZoneKeyName);
        ZonePath[ARRAYSIZE(ZonePath) - 1] = 0;
        RtlInitUnicodeString(&ZonePathString, ZonePath);
        InitializeObjectAttributes(&ObjectAttributes, &ZonePathString,
                                   OBJ_CASE_INSENSITIVE, NULL, NULL);
        Status = NtOpenKey(&KeyHandle, KEY_QUERY_VALUE, &ObjectAttributes);
        if (!NT_SUCCESS(Status))
            return TRUE;                    /* no overrides: the base rules hold */

        Value = &Partial.Info;

        Status = NtQueryValueKey(KeyHandle, &WdxDynamicDstFirst, KeyValuePartialInformation,
                                 Value, sizeof(Partial), NULL);
        if (!NT_SUCCESS(Status) || Value->Type != REG_DWORD || Value->DataLength != sizeof(ULONG))
            goto Close;
        FirstEntry = *(ULONG *)Value->Data;

        Status = NtQueryValueKey(KeyHandle, &WdxDynamicDstLast, KeyValuePartialInformation,
                                 Value, sizeof(Partial), NULL);
        if (!NT_SUCCESS(Status) || Value->Type != REG_DWORD || Value->DataLength != sizeof(ULONG))
            goto Close;
        LastEntry = *(ULONG *)Value->Data;

        if (wYear < FirstEntry || wYear > LastEntry)
            goto Close;                     /* outside the covered span */

        /* The blob is one 44-byte REG_TZI per year from FirstEntry. */
        {
            ULONG Offset = (wYear - FirstEntry) * 44;
            ULONG Needed = FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION, Data) + Offset + 44;
            PKEY_VALUE_PARTIAL_INFORMATION Blob = RtlAllocateHeap(RtlGetProcessHeap(), 0, Needed);
            WDX_REG_TZI Rules;

            if (!Blob)
                goto Close;
            Status = NtQueryValueKey(KeyHandle, &WdxDynamicDstByYear, KeyValuePartialInformation,
                                     Blob, Needed, NULL);
            if (NT_SUCCESS(Status) &&
                (Blob->Type == REG_BINARY) &&
                Blob->DataLength >= Offset + 44)
            {
                RtlCopyMemory(&Rules, Blob->Data + Offset, sizeof(Rules));
                Out->Bias = Rules.Bias;
                Out->StandardBias = Rules.StandardBias;
                Out->DaylightBias = Rules.DaylightBias;
                Out->StandardDate.wYear = 0;
                Out->StandardDate.wMonth = Rules.StandardDate.Month;
                Out->StandardDate.wDay = Rules.StandardDate.Day;
                Out->StandardDate.wDayOfWeek = Rules.StandardDate.DayOfWeek;
                Out->StandardDate.wHour = Rules.StandardDate.Hour;
                Out->StandardDate.wMinute = Rules.StandardDate.Minute;
                Out->StandardDate.wSecond = Rules.StandardDate.Second;
                Out->StandardDate.wMilliseconds = 0;
                Out->DaylightDate.wYear = 0;
                Out->DaylightDate.wMonth = Rules.DaylightDate.Month;
                Out->DaylightDate.wDay = Rules.DaylightDate.Day;
                Out->DaylightDate.wDayOfWeek = Rules.DaylightDate.DayOfWeek;
                Out->DaylightDate.wHour = Rules.DaylightDate.Hour;
                Out->DaylightDate.wMinute = Rules.DaylightDate.Minute;
                Out->DaylightDate.wSecond = Rules.DaylightDate.Second;
                Out->DaylightDate.wMilliseconds = 0;
            }
            RtlFreeHeap(RtlGetProcessHeap(), 0, Blob);
        }

Close:
        NtClose(KeyHandle);
    }
    return TRUE;
}
