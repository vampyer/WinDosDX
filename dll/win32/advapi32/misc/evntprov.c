/*
 * PROJECT:     WinDosDX system libraries
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Event provider API (Vista and later) without a tracing service
 *
 * Programs register as event providers at startup and write events that a
 * tracing session may collect. WinDosDX has no tracing sessions, so this is
 * what Windows does when nobody listens: registration succeeds, no provider
 * is ever enabled, and written events are dropped.
 */

#define _EVNT_SOURCE_
#include <advapi32.h>
#include <evntprov.h>

WINE_DEFAULT_DEBUG_CHANNEL(advapi);

static LONG64 NextRegHandle;

ULONG
WINAPI
EventRegister(
    _In_ LPCGUID ProviderId,
    _In_opt_ PENABLECALLBACK EnableCallback,
    _In_opt_ PVOID CallbackContext,
    _Out_ PREGHANDLE RegHandle)
{
    UNREFERENCED_PARAMETER(EnableCallback);
    UNREFERENCED_PARAMETER(CallbackContext);

    if (!ProviderId || !RegHandle)
        return ERROR_INVALID_PARAMETER;
    *RegHandle = (REGHANDLE)InterlockedIncrement64(&NextRegHandle);
    return ERROR_SUCCESS;
}

ULONG
WINAPI
EventUnregister(_In_ REGHANDLE RegHandle)
{
    UNREFERENCED_PARAMETER(RegHandle);
    return ERROR_SUCCESS;
}

BOOLEAN
WINAPI
EventEnabled(_In_ REGHANDLE RegHandle, _In_ PCEVENT_DESCRIPTOR EventDescriptor)
{
    UNREFERENCED_PARAMETER(RegHandle);
    UNREFERENCED_PARAMETER(EventDescriptor);
    return FALSE;
}

BOOLEAN
WINAPI
EventProviderEnabled(_In_ REGHANDLE RegHandle, _In_ UCHAR Level, _In_ ULONGLONG Keyword)
{
    UNREFERENCED_PARAMETER(RegHandle);
    UNREFERENCED_PARAMETER(Level);
    UNREFERENCED_PARAMETER(Keyword);
    return FALSE;
}

ULONG
WINAPI
EventWrite(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_ ULONG UserDataCount,
    _In_reads_opt_(UserDataCount) PEVENT_DATA_DESCRIPTOR UserData)
{
    UNREFERENCED_PARAMETER(RegHandle);
    UNREFERENCED_PARAMETER(EventDescriptor);
    UNREFERENCED_PARAMETER(UserDataCount);
    UNREFERENCED_PARAMETER(UserData);
    return ERROR_SUCCESS;
}

ULONG
WINAPI
EventWriteTransfer(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_opt_ LPCGUID ActivityId,
    _In_opt_ LPCGUID RelatedActivityId,
    _In_ ULONG UserDataCount,
    _In_reads_opt_(UserDataCount) PEVENT_DATA_DESCRIPTOR UserData)
{
    UNREFERENCED_PARAMETER(ActivityId);
    UNREFERENCED_PARAMETER(RelatedActivityId);
    return EventWrite(RegHandle, EventDescriptor, UserDataCount, UserData);
}

ULONG
WINAPI
EventWriteEx(
    _In_ REGHANDLE RegHandle,
    _In_ PCEVENT_DESCRIPTOR EventDescriptor,
    _In_ ULONG64 Filter,
    _In_ ULONG Flags,
    _In_opt_ LPCGUID ActivityId,
    _In_opt_ LPCGUID RelatedActivityId,
    _In_ ULONG UserDataCount,
    _In_reads_opt_(UserDataCount) PEVENT_DATA_DESCRIPTOR UserData)
{
    UNREFERENCED_PARAMETER(Filter);
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(ActivityId);
    UNREFERENCED_PARAMETER(RelatedActivityId);
    return EventWrite(RegHandle, EventDescriptor, UserDataCount, UserData);
}

ULONG
WINAPI
EventWriteString(
    _In_ REGHANDLE RegHandle,
    _In_ UCHAR Level,
    _In_ ULONGLONG Keyword,
    _In_ PCWSTR String)
{
    UNREFERENCED_PARAMETER(RegHandle);
    UNREFERENCED_PARAMETER(Level);
    UNREFERENCED_PARAMETER(Keyword);
    UNREFERENCED_PARAMETER(String);
    return ERROR_SUCCESS;
}

/* Activity IDs belong to the thread; with no tracing they only need to
 * round-trip, so each thread keeps its current ID in a TLS slot. */
static DWORD ActivityIdTls = TLS_OUT_OF_INDEXES;

static LPGUID
ThreadActivityId(BOOL Create)
{
    LPGUID Id;

    if (ActivityIdTls == TLS_OUT_OF_INDEXES)
    {
        DWORD Slot = TlsAlloc();
        if (Slot == TLS_OUT_OF_INDEXES)
            return NULL;
        if (InterlockedCompareExchange((LONG *)&ActivityIdTls, (LONG)Slot, (LONG)TLS_OUT_OF_INDEXES) !=
            (LONG)TLS_OUT_OF_INDEXES)
        {
            TlsFree(Slot);
        }
    }
    Id = TlsGetValue(ActivityIdTls);
    if (!Id && Create)
    {
        Id = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(GUID));
        if (Id)
            TlsSetValue(ActivityIdTls, Id);
    }
    return Id;
}

ULONG
WINAPI
EventActivityIdControl(_In_ ULONG ControlCode, _Inout_ LPGUID ActivityId)
{
    static LONG Counter;
    LPGUID Current;
    GUID Previous;

    if (!ActivityId)
        return ERROR_INVALID_PARAMETER;

    switch (ControlCode)
    {
        case EVENT_ACTIVITY_CTRL_GET_ID:
            Current = ThreadActivityId(FALSE);
            if (Current)
                *ActivityId = *Current;
            else
                RtlZeroMemory(ActivityId, sizeof(GUID));
            return ERROR_SUCCESS;

        case EVENT_ACTIVITY_CTRL_SET_ID:
            Current = ThreadActivityId(TRUE);
            if (!Current)
                return ERROR_NOT_ENOUGH_MEMORY;
            *Current = *ActivityId;
            return ERROR_SUCCESS;

        case EVENT_ACTIVITY_CTRL_CREATE_ID:
            /* Unique within the process, which is all a lone provider needs. */
            RtlZeroMemory(ActivityId, sizeof(GUID));
            ActivityId->Data1 = GetCurrentProcessId();
            ActivityId->Data2 = (USHORT)GetCurrentThreadId();
            *(LONG *)ActivityId->Data4 = InterlockedIncrement(&Counter);
            return ERROR_SUCCESS;

        case EVENT_ACTIVITY_CTRL_GET_SET_ID:
            Current = ThreadActivityId(TRUE);
            if (!Current)
                return ERROR_NOT_ENOUGH_MEMORY;
            Previous = *Current;
            *Current = *ActivityId;
            *ActivityId = Previous;
            return ERROR_SUCCESS;

        case EVENT_ACTIVITY_CTRL_CREATE_SET_ID:
            Current = ThreadActivityId(TRUE);
            if (!Current)
                return ERROR_NOT_ENOUGH_MEMORY;
            *ActivityId = *Current;
            EventActivityIdControl(EVENT_ACTIVITY_CTRL_CREATE_ID, Current);
            return ERROR_SUCCESS;

        default:
            return ERROR_INVALID_PARAMETER;
    }
}

/* Windows 8: provider traits and similar; nothing to store without tracing. */
ULONG
WINAPI
EventSetInformation(
    _In_ REGHANDLE RegHandle,
    _In_ ULONG InformationClass,
    _In_reads_bytes_opt_(InformationLength) PVOID EventInformation,
    _In_ ULONG InformationLength)
{
    UNREFERENCED_PARAMETER(RegHandle);
    UNREFERENCED_PARAMETER(InformationClass);
    UNREFERENCED_PARAMETER(EventInformation);
    UNREFERENCED_PARAMETER(InformationLength);
    return ERROR_SUCCESS;
}
