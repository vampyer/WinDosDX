/*
 * PROJECT:     WinDosDX IP Helper API
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Vista network tables, best route and change notifications
 *
 * The ReactOS network stack reports no change events (the older
 * NotifyAddrChange/NotifyRouteChange are stubs as well), so these
 * notifications are delivered by a monitor thread that polls the IPv4
 * tables once a second, diffs consecutive snapshots and dispatches the
 * difference to the registered callbacks as Windows would: rows that
 * appeared are MibAddInstance, rows that disappeared are
 * MibDeleteInstance, changed interface parameters are
 * MibParameterNotification, and a registration that asks for one gets a
 * snapshot pass labelled MibInitialNotification.
 *
 * GetBestRoute2 performs the same longest-prefix match as GetBestRoute
 * (iphlpapi_main.c) and fills the Windows 8 row and source address.
 */

#include "iphlpapi_private.h"

#include <netioapi.h>

WINE_DEFAULT_DEBUG_CHANNEL(iphlpapi);

/* Table getters from iphlpapi_main.c (same DLL); the private header does
 * not declare the address one. */
DWORD WINAPI AllocateAndGetIpForwardTableFromStack(PMIB_IPFORWARDTABLE *ppIpForwardTable,
                                                   BOOL bOrder, HANDLE heap, DWORD flags);
DWORD WINAPI AllocateAndGetIpAddrTableFromStack(PMIB_IPADDRTABLE *ppIpAddrTable,
                                                BOOL bOrder, HANDLE heap, DWORD flags);

/* Vista-era NTDDI-gated constants the precompiled header hides. */
#define WDX_NDIS_MEDIUM_LOOPBACK ((NDIS_MEDIUM)17)

/* A critical section guards the registry; callbacks run outside it. */
static CRITICAL_SECTION WdxNotifyLock;
static BOOLEAN WdxNotifyLockInit = FALSE;

static VOID
WdxLockInit(VOID)
{
    if (!WdxNotifyLockInit)
    {
        InitializeCriticalSection(&WdxNotifyLock);
        WdxNotifyLockInit = TRUE;
    }
}

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/* ReactOS interfaces are identified by index; the LUID is synthesized
 * deterministically from it (the way Windows keeps LUIDs stable while the
 * machine is up), so GetIfEntry2 can find a row by LUID alone. */
static NET_LUID
WdxInterfaceLuid(NET_IFINDEX Index, ULONG IfType)
{
    NET_LUID Luid;

    Luid.Value = 0;
    Luid.Info.NetLuidIndex = Index;
    Luid.Info.IfType = IfType;
    return Luid;
}

static ULONG
WdxMaskLength(DWORD Mask)
{
    ULONG Length = 0;

    while (Mask & 0x80000000)
    {
        Length++;
        Mask <<= 1;
    }
    return Length;
}

static VOID
WdxSetInet4(SOCKADDR_INET *Address, DWORD Ipv4)
{
    RtlZeroMemory(Address, sizeof(*Address));
    Address->Ipv4.sin_family = AF_INET;
    Address->Ipv4.sin_port = 0;
    Address->Ipv4.sin_addr.S_un.S_addr = Ipv4;
}

#define WDX_INFINITE_LIFETIME 0xFFFFFFFF

/* XP's INTERNAL_IF_OPER_STATUS values mapped onto the Vista IF_OPER_STATUS
 * that the Windows Vista+ rows carry. */
static IF_OPER_STATUS
WdxMapOperStatus(DWORD XpStatus)
{
    switch (XpStatus)
    {
        case IF_OPER_STATUS_OPERATIONAL:
            return IfOperStatusUp;
        case IF_OPER_STATUS_CONNECTED:
            return IfOperStatusDormant;
        case IF_OPER_STATUS_CONNECTING:
            return IfOperStatusTesting;
        case IF_OPER_STATUS_UNREACHABLE:
            return IfOperStatusLowerLayerDown;
        case IF_OPER_STATUS_DISCONNECTED:
        default:
            return IfOperStatusDown;
    }
}

/* ------------------------------------------------------------------ */
/* Table getters                                                       */
/* ------------------------------------------------------------------ */

/* One row per interface, in index order like Windows. */
static VOID
WdxBuildIfRow2(PMIB_IF_ROW2 Row2, PMIB_IFROW Row)
{
    BOOLEAN Loopback = (Row->dwType == IF_TYPE_SOFTWARE_LOOPBACK);

    RtlZeroMemory(Row2, sizeof(*Row2));
    Row2->InterfaceLuid = WdxInterfaceLuid(Row->dwIndex,
                                           Loopback ? IF_TYPE_SOFTWARE_LOOPBACK
                                                    : IF_TYPE_ETHERNET_CSMACD);
    Row2->InterfaceIndex = Row->dwIndex;
    _snwprintf(Row2->Alias, ARRAYSIZE(Row2->Alias),
               Loopback ? L"loopback_%lu" : L"eth_%lu", Row->dwIndex);
    Row2->Alias[ARRAYSIZE(Row2->Alias) - 1] = 0;
    if (Row->wszName[0])
        lstrcpynW(Row2->Description, Row->wszName, ARRAYSIZE(Row2->Description));
    Row2->PhysicalAddressLength = Row->dwPhysAddrLen;
    if (Row->dwPhysAddrLen)
        RtlCopyMemory(Row2->PhysicalAddress, Row->bPhysAddr, Row->dwPhysAddrLen);
    Row2->Mtu = Row->dwMtu;
    Row2->Type = Row->dwType;
    Row2->TunnelType = TUNNEL_TYPE_NONE;
    Row2->MediaType = Loopback ? WDX_NDIS_MEDIUM_LOOPBACK : NdisMedium802_3;
    Row2->PhysicalMediumType = NdisPhysicalMediumUnspecified;
    Row2->AccessType = Loopback ? NET_IF_ACCESS_LOOPBACK : NET_IF_ACCESS_BROADCAST;
    Row2->DirectionType = NET_IF_DIRECTION_SENDRECEIVE;
    Row2->InterfaceAndOperStatusFlags.NotMediaConnected =
        (Row->dwOperStatus != IF_OPER_STATUS_OPERATIONAL);
    Row2->OperStatus = WdxMapOperStatus(Row->dwOperStatus);
    Row2->AdminStatus = (Row->dwAdminStatus == MIB_IF_ADMIN_STATUS_UP)
                        ? NET_IF_ADMIN_STATUS_UP : NET_IF_ADMIN_STATUS_DOWN;
    Row2->MediaConnectState =
        (Row->dwOperStatus == IF_OPER_STATUS_OPERATIONAL)
        ? MediaConnectStateConnected : MediaConnectStateDisconnected;
    Row2->ConnectionType = NET_IF_CONNECTION_DEDICATED;
    Row2->TransmitLinkSpeed = Row->dwSpeed;
    Row2->ReceiveLinkSpeed = Row->dwSpeed;
    Row2->InOctets = Row->dwInOctets;
    Row2->InUcastPkts = Row->dwInUcastPkts;
    Row2->InNUcastPkts = Row->dwInNUcastPkts;
    Row2->InDiscards = Row->dwInDiscards;
    Row2->InErrors = Row->dwInErrors;
    Row2->InUnknownProtos = Row->dwInUnknownProtos;
    Row2->OutOctets = Row->dwOutOctets;
    Row2->OutUcastPkts = Row->dwOutUcastPkts;
    Row2->OutNUcastPkts = Row->dwOutNUcastPkts;
    Row2->OutDiscards = Row->dwOutDiscards;
    Row2->OutErrors = Row->dwOutErrors;
}

NETIOAPI_API
GetIfEntry2(IN OUT PMIB_IF_ROW2 Row)
{
    MIB_IFROW Row1;
    NET_IFINDEX Index;

    if (!Row)
        return ERROR_INVALID_PARAMETER;
    Index = Row->InterfaceIndex;
    RtlZeroMemory(&Row1, sizeof(Row1));
    Row1.dwIndex = Index;
    if (GetIfEntry(&Row1) != NO_ERROR)
        return ERROR_NOT_FOUND;
    WdxBuildIfRow2(Row, &Row1);
    return NO_ERROR;
}

NETIOAPI_API
GetIfTable2(OUT PMIB_IF_TABLE2 *Table)
{
    PMIB_IFTABLE Table1;
    PMIB_IF_TABLE2 Table2;
    ULONG Size = 0, Size2;
    DWORD Error, i;

    if (!Table)
        return ERROR_INVALID_PARAMETER;
    *Table = NULL;

    Error = GetIfTable(NULL, &Size, FALSE);
    if (Error != ERROR_INSUFFICIENT_BUFFER || !Size)
        return ERROR_NOT_FOUND;
    Table1 = HeapAlloc(GetProcessHeap(), 0, Size);
    if (!Table1)
        return ERROR_NOT_ENOUGH_MEMORY;
    Error = GetIfTable(Table1, &Size, FALSE);
    if (Error != NO_ERROR)
    {
        HeapFree(GetProcessHeap(), 0, Table1);
        return Error;
    }

    Size2 = FIELD_OFFSET(MIB_IF_TABLE2, Table) +
            Table1->dwNumEntries * sizeof(MIB_IF_ROW2);
    Table2 = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size2);
    if (!Table2)
    {
        HeapFree(GetProcessHeap(), 0, Table1);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    Table2->NumEntries = Table1->dwNumEntries;
    for (i = 0; i < (ULONG)Table1->dwNumEntries; i++)
        WdxBuildIfRow2(&Table2->Table[i], &Table1->table[i]);
    HeapFree(GetProcessHeap(), 0, Table1);
    *Table = Table2;
    return NO_ERROR;
}

/* The route table as Windows 8 rows, from the working IPv4 getter. */
static VOID
WdxBuildForwardRow2(PMIB_IPFORWARD_ROW2 Row2, PMIB_IPFORWARDROW Row)
{
    BOOLEAN Loopback;

    RtlZeroMemory(Row2, sizeof(*Row2));
    Loopback = (Row->dwForwardDest == 0) && (Row->dwForwardMask == 0) &&
               (Row->dwForwardNextHop == 0);
    Row2->InterfaceLuid = WdxInterfaceLuid(Row->dwForwardIfIndex,
                                           IF_TYPE_ETHERNET_CSMACD);
    Row2->InterfaceIndex = Row->dwForwardIfIndex;
    WdxSetInet4(&Row2->DestinationPrefix.Prefix, Row->dwForwardDest);
    Row2->DestinationPrefix.PrefixLength = (UINT8)WdxMaskLength(Row->dwForwardMask);
    WdxSetInet4(&Row2->NextHop, Row->dwForwardNextHop);
    Row2->SitePrefixLength = 0;
    Row2->ValidLifetime = WDX_INFINITE_LIFETIME;
    Row2->PreferredLifetime = WDX_INFINITE_LIFETIME;
    Row2->Metric = Row->dwForwardMetric1;
    Row2->Protocol = (NL_ROUTE_PROTOCOL)Row->dwForwardProto;
    Row2->Loopback = Loopback;
    Row2->AutoconfigureAddress = FALSE;
    Row2->Publish = FALSE;
    Row2->Immortal = FALSE;
    Row2->Origin = (Row->dwForwardProto == 3) ? NlroDHCP : NlroManual;
}

NETIOAPI_API
GetIpForwardTable2(ADDRESS_FAMILY Family, PMIB_IPFORWARD_TABLE2 *Table)
{
    PMIB_IPFORWARDTABLE Table1;
    PMIB_IPFORWARD_TABLE2 Table2;
    ULONG Size2;
    DWORD Error, i;

    if (!Table)
        return ERROR_INVALID_PARAMETER;
    /* NULL on every failure, as Windows does */
    *Table = NULL;
    if (Family != AF_INET && Family != AF_UNSPEC)
        return ERROR_NOT_SUPPORTED;         /* IPv6 rows are not reported yet */

    Error = AllocateAndGetIpForwardTableFromStack(&Table1, FALSE, GetProcessHeap(), 0);
    if (Error != NO_ERROR || !Table1)
        return ERROR_NOT_FOUND;

    Size2 = FIELD_OFFSET(MIB_IPFORWARD_TABLE2, Table) +
            Table1->dwNumEntries * sizeof(MIB_IPFORWARD_ROW2);
    Table2 = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size2);
    if (!Table2)
    {
        HeapFree(GetProcessHeap(), 0, Table1);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    Table2->NumEntries = Table1->dwNumEntries;
    for (i = 0; i < (ULONG)Table1->dwNumEntries; i++)
        WdxBuildForwardRow2(&Table2->Table[i], &Table1->table[i]);
    HeapFree(GetProcessHeap(), 0, Table1);
    *Table = Table2;
    return NO_ERROR;
}

static VOID
WdxBuildUnicastRow2(PMIB_UNICASTIPADDRESS_ROW Row2, PMIB_IPADDRROW Row)
{
    RtlZeroMemory(Row2, sizeof(*Row2));
    WdxSetInet4(&Row2->Address, Row->dwAddr);
    Row2->InterfaceLuid = WdxInterfaceLuid(Row->dwIndex, IF_TYPE_ETHERNET_CSMACD);
    Row2->InterfaceIndex = Row->dwIndex;
    Row2->PrefixOrigin = IpPrefixOriginOther;
    Row2->SuffixOrigin = IpSuffixOriginOther;
    Row2->ValidLifetime = WDX_INFINITE_LIFETIME;
    Row2->PreferredLifetime = WDX_INFINITE_LIFETIME;
    Row2->OnLinkPrefixLength = (UINT8)WdxMaskLength(Row->dwMask);
    Row2->SkipAsSource = FALSE;
    Row2->DadState = IpDadStatePreferred;
    Row2->ScopeId.Value = 0;
}

NETIOAPI_API
GetUnicastIpAddressTable(ADDRESS_FAMILY Family, PMIB_UNICASTIPADDRESS_TABLE *Table)
{
    PMIB_IPADDRTABLE Table1;
    PMIB_UNICASTIPADDRESS_TABLE Table2;
    ULONG Size = 0, Size2;
    DWORD Error, i;

    if (!Table)
        return ERROR_INVALID_PARAMETER;
    /* NULL on every failure, as Windows does */
    *Table = NULL;
    if (Family != AF_INET && Family != AF_UNSPEC)
        return ERROR_NOT_SUPPORTED;

    Error = GetIpAddrTable(NULL, &Size, FALSE);
    if (Error != ERROR_INSUFFICIENT_BUFFER || !Size)
        return ERROR_NOT_FOUND;
    Table1 = HeapAlloc(GetProcessHeap(), 0, Size);
    if (!Table1)
        return ERROR_NOT_ENOUGH_MEMORY;
    Error = GetIpAddrTable(Table1, &Size, FALSE);
    if (Error != NO_ERROR)
    {
        HeapFree(GetProcessHeap(), 0, Table1);
        return Error;
    }

    Size2 = FIELD_OFFSET(MIB_UNICASTIPADDRESS_TABLE, Table) +
            Table1->dwNumEntries * sizeof(MIB_UNICASTIPADDRESS_ROW);
    Table2 = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size2);
    if (!Table2)
    {
        HeapFree(GetProcessHeap(), 0, Table1);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    Table2->NumEntries = Table1->dwNumEntries;
    for (i = 0; i < (ULONG)Table1->dwNumEntries; i++)
        WdxBuildUnicastRow2(&Table2->Table[i], &Table1->table[i]);
    HeapFree(GetProcessHeap(), 0, Table1);
    *Table = Table2;
    return NO_ERROR;
}

/* One row per (interface, family); the parameters are the stack defaults. */
static VOID
WdxBuildIpInterfaceRow(PMIB_IPINTERFACE_ROW Row, PMIB_IFROW IfRow, ADDRESS_FAMILY Family)
{
    BOOLEAN Loopback = (IfRow->dwType == IF_TYPE_SOFTWARE_LOOPBACK);

    RtlZeroMemory(Row, sizeof(*Row));
    Row->Family = Family;
    Row->InterfaceLuid = WdxInterfaceLuid(IfRow->dwIndex,
                                          Loopback ? IF_TYPE_SOFTWARE_LOOPBACK
                                                   : IF_TYPE_ETHERNET_CSMACD);
    Row->InterfaceIndex = IfRow->dwIndex;
    Row->MaxReassemblySize = 65535;
    Row->InterfaceIdentifier = 0;
    Row->MinRouterAdvertisementInterval = 200;
    Row->MaxRouterAdvertisementInterval = 600;
    Row->AdvertisingEnabled = FALSE;
    Row->ForwardingEnabled = FALSE;
    Row->WeakHostSend = FALSE;
    Row->WeakHostReceive = FALSE;
    Row->UseAutomaticMetric = TRUE;
    Row->UseNeighborUnreachabilityDetection = TRUE;
    Row->ManagedAddressConfigurationSupported = TRUE;
    Row->OtherStatefulConfigurationSupported = TRUE;
    Row->AdvertiseDefaultRoute = FALSE;
    Row->RouterDiscoveryBehavior = Loopback ? RouterDiscoveryDisabled
                                            : RouterDiscoveryDhcp;
    Row->DadTransmits = 3;
    Row->BaseReachableTime = 30000;
    Row->RetransmitTime = 1000;
    Row->PathMtuDiscoveryTimeout = 600000;
    Row->LinkLocalAddressBehavior = LinkLocalDelayed;
    Row->LinkLocalAddressTimeout = 3000;
    Row->SitePrefixLength = Loopback ? 0 : 64;
    Row->Metric = 1;
    Row->NlMtu = IfRow->dwMtu;
    Row->Connected = WdxMapOperStatus(IfRow->dwOperStatus) == IfOperStatusUp;
    Row->SupportsWakeUpPatterns = FALSE;
    Row->SupportsNeighborDiscovery = FALSE;
    Row->SupportsRouterDiscovery = FALSE;
    Row->ReachableTime = 0;
    Row->DisableDefaultRoutes = FALSE;
}

NETIOAPI_API
GetIpInterfaceEntry(IN OUT PMIB_IPINTERFACE_ROW Row)
{
    MIB_IFROW IfRow;
    ADDRESS_FAMILY Family;

    if (!Row)
        return ERROR_INVALID_PARAMETER;
    Family = Row->Family;
    if (Family != AF_INET && Family != AF_INET6)
        return ERROR_INVALID_PARAMETER;
    if (Family == AF_INET6)
        return ERROR_NOT_SUPPORTED;         /* no IPv6 interface data yet */

    RtlZeroMemory(&IfRow, sizeof(IfRow));
    IfRow.dwIndex = Row->InterfaceIndex;
    if (GetIfEntry(&IfRow) != NO_ERROR)
        return ERROR_NOT_FOUND;
    WdxBuildIpInterfaceRow(Row, &IfRow, AF_INET);
    return NO_ERROR;
}

NETIOAPI_API
GetIpInterfaceTable(ADDRESS_FAMILY Family, PMIB_IPINTERFACE_TABLE *Table)
{
    PMIB_IFTABLE IfTable;
    PMIB_IPINTERFACE_TABLE Table2;
    ULONG Size = 0, Size2;
    DWORD Error, i;

    if (!Table)
        return ERROR_INVALID_PARAMETER;
    /* NULL on every failure, as Windows does */
    *Table = NULL;
    if (Family != AF_INET && Family != AF_UNSPEC)
        return ERROR_NOT_SUPPORTED;

    Error = GetIfTable(NULL, &Size, FALSE);
    if (Error != ERROR_INSUFFICIENT_BUFFER || !Size)
        return ERROR_NOT_FOUND;
    IfTable = HeapAlloc(GetProcessHeap(), 0, Size);
    if (!IfTable)
        return ERROR_NOT_ENOUGH_MEMORY;
    Error = GetIfTable(IfTable, &Size, FALSE);
    if (Error != NO_ERROR)
    {
        HeapFree(GetProcessHeap(), 0, IfTable);
        return Error;
    }

    Size2 = FIELD_OFFSET(MIB_IPINTERFACE_TABLE, Table) +
            IfTable->dwNumEntries * sizeof(MIB_IPINTERFACE_ROW);
    Table2 = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, Size2);
    if (!Table2)
    {
        HeapFree(GetProcessHeap(), 0, IfTable);
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    Table2->NumEntries = IfTable->dwNumEntries;
    for (i = 0; i < (ULONG)IfTable->dwNumEntries; i++)
        WdxBuildIpInterfaceRow(&Table2->Table[i], &IfTable->table[i], AF_INET);
    HeapFree(GetProcessHeap(), 0, IfTable);
    *Table = Table2;
    return NO_ERROR;
}

/* ------------------------------------------------------------------ */
/* Best route                                                          */
/* ------------------------------------------------------------------ */

/*
 * Longest-prefix match over the IPv4 route table, as GetBestRoute does,
 * reported in the Windows 8 form. The source address is the interface
 * address of the winning route's interface; the sort options are ignored
 * because the match itself already picks the Windows answer.
 */
NETIOAPI_API
GetBestRoute2(NET_LUID InterfaceLuid, NET_IFINDEX InterfaceIndex,
              CONST SOCKADDR_INET *SourceAddress, CONST SOCKADDR_INET *DestinationAddress,
              ULONG AddressSortOptions, PMIB_IPFORWARD_ROW2 BestRoute,
              SOCKADDR_INET *BestSourceAddress)
{
    PMIB_IPFORWARDTABLE Table;
    PMIB_IPADDRTABLE AddrTable;
    DWORD Dest, Error, ndx, minMaskSize, matchedNdx;
    ULONG i;

    UNREFERENCED_PARAMETER(InterfaceLuid);
    UNREFERENCED_PARAMETER(AddressSortOptions);
    UNREFERENCED_PARAMETER(InterfaceIndex);

    if (!DestinationAddress || !BestRoute)
        return ERROR_INVALID_PARAMETER;
    if (DestinationAddress->si_family != AF_INET)
        return ERROR_NOT_SUPPORTED;         /* IPv6 routes are not reported yet */
    Dest = DestinationAddress->Ipv4.sin_addr.S_un.S_addr;
    UNREFERENCED_PARAMETER(SourceAddress);

    Error = AllocateAndGetIpForwardTableFromStack(&Table, FALSE, GetProcessHeap(), 0);
    if (Error != NO_ERROR || !Table)
        return ERROR_NOT_FOUND;

    matchedNdx = 0;
    minMaskSize = 255;
    for (ndx = 0; ndx < Table->dwNumEntries; ndx++)
    {
        if ((Dest & Table->table[ndx].dwForwardMask) ==
            (Table->table[ndx].dwForwardDest & Table->table[ndx].dwForwardMask))
        {
            DWORD hostMaskSize;

            if (!_BitScanForward(&hostMaskSize, ntohl(Table->table[ndx].dwForwardMask)))
                hostMaskSize = 32;
            if (hostMaskSize < minMaskSize)
            {
                minMaskSize = hostMaskSize;
                matchedNdx = ndx;
            }
        }
    }
    if (minMaskSize == 255)
    {
        HeapFree(GetProcessHeap(), 0, Table);
        return ERROR_NOT_FOUND;             /* no route to host */
    }
    WdxBuildForwardRow2(BestRoute, &Table->table[matchedNdx]);
    HeapFree(GetProcessHeap(), 0, Table);

    /* The source address: the winning interface's unicast address. */
    if (BestSourceAddress)
    {
        RtlZeroMemory(BestSourceAddress, sizeof(*BestSourceAddress));
        Error = AllocateAndGetIpAddrTableFromStack(&AddrTable, FALSE, GetProcessHeap(), 0);
        if (Error == NO_ERROR && AddrTable)
        {
            for (i = 0; i < AddrTable->dwNumEntries; i++)
            {
                if (AddrTable->table[i].dwIndex == BestRoute->InterfaceIndex &&
                    AddrTable->table[i].dwAddr)
                {
                    WdxSetInet4(BestSourceAddress, AddrTable->table[i].dwAddr);
                    break;
                }
            }
            HeapFree(GetProcessHeap(), 0, AddrTable);
        }
    }
    return NO_ERROR;
}

/* ------------------------------------------------------------------ */
/* Change notifications                                                */
/* ------------------------------------------------------------------ */

typedef enum _WDX_WATCH_TYPE
{
    WdxWatchIpInterface,
    WdxWatchUnicastAddress,
    WdxWatchRoute,
} WDX_WATCH_TYPE;

typedef struct _WDX_NOTIFICATION
{
    LIST_ENTRY Entry;
    LONG RefCount;
    WDX_WATCH_TYPE Type;
    ADDRESS_FAMILY Family;
    PVOID Callback;
    PVOID Context;
} WDX_NOTIFICATION, *PWDX_NOTIFICATION;

/* What one poll saw; kept as Windows rows so the diff can hand the caller
 * the row without re-querying the stack. */
typedef struct _WDX_ROUTE_SNAP
{
    ULONG Count;
    MIB_IPFORWARD_ROW2 Rows[1];
} WDX_ROUTE_SNAP, *PWDX_ROUTE_SNAP;

typedef struct _WDX_ADDR_SNAP
{
    ULONG Count;
    MIB_UNICASTIPADDRESS_ROW Rows[1];
} WDX_ADDR_SNAP, *PWDX_ADDR_SNAP;

typedef struct _WDX_IF_SNAP
{
    ULONG Count;
    NET_IFINDEX Index[1];
    IF_OPER_STATUS Oper[1];
    DWORD Speed[1];
} WDX_IF_SNAP, *PWDX_IF_SNAP;

static LIST_ENTRY WdxNotifyList;
static BOOLEAN WdxNotifyListReady = FALSE;
static HANDLE WdxMonitorThread;
static volatile BOOLEAN WdxMonitorQuit;
static HANDLE WdxMonitorKick;

static VOID
WdxLock(VOID)
{
    WdxLockInit();
    EnterCriticalSection(&WdxNotifyLock);
    if (!WdxNotifyListReady)
    {
        InitializeListHead(&WdxNotifyList);
        WdxNotifyListReady = TRUE;
    }
}

static VOID
WdxUnlock(VOID)
{
    LeaveCriticalSection(&WdxNotifyLock);
}

static VOID
WdxReference(PWDX_NOTIFICATION Notification)
{
    InterlockedIncrement(&Notification->RefCount);
}

static VOID
WdxDereference(PWDX_NOTIFICATION Notification)
{
    if (InterlockedDecrement(&Notification->RefCount) == 0)
        HeapFree(GetProcessHeap(), 0, Notification);
}

static BOOLEAN
WdxFamilyMatches(ADDRESS_FAMILY Registered, ADDRESS_FAMILY RowFamily)
{
    return (Registered == AF_UNSPEC) || (Registered == RowFamily);
}

/* --- snapshots ------------------------------------------------------ */

static VOID
WdxTakeRouteSnapshot(PWDX_ROUTE_SNAP *Snapshot)
{
    PMIB_IPFORWARD_TABLE2 Table = NULL;
    PWDX_ROUTE_SNAP Snap;
    ULONG Size, i;

    *Snapshot = NULL;
    if (GetIpForwardTable2(AF_INET, &Table) != NO_ERROR || !Table)
        return;
    Size = FIELD_OFFSET(WDX_ROUTE_SNAP, Rows) + Table->NumEntries * sizeof(MIB_IPFORWARD_ROW2);
    Snap = HeapAlloc(GetProcessHeap(), 0, Size);
    if (!Snap)
    {
        FreeMibTable(Table);
        return;
    }
    Snap->Count = Table->NumEntries;
    for (i = 0; i < Table->NumEntries; i++)
        Snap->Rows[i] = Table->Table[i];
    FreeMibTable(Table);
    *Snapshot = Snap;
}

static VOID
WdxTakeAddrSnapshot(PWDX_ADDR_SNAP *Snapshot)
{
    PMIB_UNICASTIPADDRESS_TABLE Table = NULL;
    PWDX_ADDR_SNAP Snap;
    ULONG Size, i;

    *Snapshot = NULL;
    if (GetUnicastIpAddressTable(AF_INET, &Table) != NO_ERROR || !Table)
        return;
    Size = FIELD_OFFSET(WDX_ADDR_SNAP, Rows) + Table->NumEntries * sizeof(MIB_UNICASTIPADDRESS_ROW);
    Snap = HeapAlloc(GetProcessHeap(), 0, Size);
    if (!Snap)
    {
        FreeMibTable(Table);
        return;
    }
    Snap->Count = Table->NumEntries;
    for (i = 0; i < Table->NumEntries; i++)
        Snap->Rows[i] = Table->Table[i];
    FreeMibTable(Table);
    *Snapshot = Snap;
}

static VOID
WdxTakeIfSnapshot(PWDX_IF_SNAP *Snapshot)
{
    PMIB_IF_TABLE2 Table = NULL;
    PWDX_IF_SNAP Snap;
    ULONG Size, i;

    *Snapshot = NULL;
    if (GetIfTable2(&Table) != NO_ERROR || !Table)
        return;
    Size = FIELD_OFFSET(WDX_IF_SNAP, Index) + Table->NumEntries *
           (sizeof(NET_IFINDEX) + sizeof(IF_OPER_STATUS) + sizeof(DWORD));
    Snap = HeapAlloc(GetProcessHeap(), 0, Size);
    if (!Snap)
    {
        FreeMibTable(Table);
        return;
    }
    Snap->Count = Table->NumEntries;
    for (i = 0; i < Table->NumEntries; i++)
    {
        Snap->Index[i] = Table->Table[i].InterfaceIndex;
        Snap->Oper[i] = Table->Table[i].OperStatus;
        Snap->Speed[i] = (DWORD)Table->Table[i].TransmitLinkSpeed;
    }
    FreeMibTable(Table);
    *Snapshot = Snap;
}

/* Row identity: routes are (interface, destination, nexthop), addresses are
 * (interface, address). Parameters are ignored for identity; the interface
 * pass reports those changes. */
static BOOLEAN
WdxRouteEqual(PMIB_IPFORWARD_ROW2 A, PMIB_IPFORWARD_ROW2 B)
{
    return A->InterfaceIndex == B->InterfaceIndex &&
           A->DestinationPrefix.Prefix.Ipv4.sin_addr.S_un.S_addr ==
               B->DestinationPrefix.Prefix.Ipv4.sin_addr.S_un.S_addr &&
           A->DestinationPrefix.PrefixLength == B->DestinationPrefix.PrefixLength &&
           A->NextHop.Ipv4.sin_addr.S_un.S_addr == B->NextHop.Ipv4.sin_addr.S_un.S_addr;
}

static BOOLEAN
WdxAddrEqual(PMIB_UNICASTIPADDRESS_ROW A, PMIB_UNICASTIPADDRESS_ROW B)
{
    return A->InterfaceIndex == B->InterfaceIndex &&
           A->Address.Ipv4.sin_addr.S_un.S_addr == B->Address.Ipv4.sin_addr.S_un.S_addr;
}

static BOOLEAN
WdxFindRoute(PWDX_ROUTE_SNAP Snap, PMIB_IPFORWARD_ROW2 Row)
{
    ULONG i;

    for (i = 0; i < Snap->Count; i++)
        if (WdxRouteEqual(&Snap->Rows[i], Row))
            return TRUE;
    return FALSE;
}

static BOOLEAN
WdxFindAddr(PWDX_ADDR_SNAP Snap, PMIB_UNICASTIPADDRESS_ROW Row)
{
    ULONG i;

    for (i = 0; i < Snap->Count; i++)
        if (WdxAddrEqual(&Snap->Rows[i], Row))
            return TRUE;
    return FALSE;
}

#define WDX_MAX_NOTIFY 32

/* The monitor: polls, diffs and dispatches, one second at a time. */
static DWORD WINAPI
WdxMonitor(PVOID Parameter)
{
    PWDX_ROUTE_SNAP OldRoutes = NULL, NewRoutes = NULL;
    PWDX_ADDR_SNAP OldAddrs = NULL, NewAddrs = NULL;
    PWDX_IF_SNAP OldIfs = NULL, NewIfs = NULL;
    PWDX_NOTIFICATION Batch[WDX_MAX_NOTIFY];
    ULONG i;
    UNREFERENCED_PARAMETER(Parameter);

    WdxTakeRouteSnapshot(&OldRoutes);
    WdxTakeAddrSnapshot(&OldAddrs);
    WdxTakeIfSnapshot(&OldIfs);

    while (!WdxMonitorQuit)
    {
        ULONG Count = 0, n;

        WaitForSingleObject(WdxMonitorKick, 1000);
        if (WdxMonitorQuit)
            break;

        WdxTakeRouteSnapshot(&NewRoutes);
        WdxTakeAddrSnapshot(&NewAddrs);
        WdxTakeIfSnapshot(&NewIfs);

        /* Reference the registry once, then dispatch without the lock:
         * callbacks may register or cancel freely, including entries of
         * this batch. */
        WdxLock();
        if (WdxNotifyListReady)
        {
            PLIST_ENTRY Entry;
            for (Entry = WdxNotifyList.Flink;
                 Entry != &WdxNotifyList && Count < WDX_MAX_NOTIFY;
                 Entry = Entry->Flink)
            {
                PWDX_NOTIFICATION Notification = CONTAINING_RECORD(Entry, WDX_NOTIFICATION, Entry);

                WdxReference(Notification);
                Batch[Count++] = Notification;
            }
        }
        WdxUnlock();

        for (n = 0; n < Count; n++)
        {
            PWDX_NOTIFICATION Notification = Batch[n];

            switch (Notification->Type)
            {
                case WdxWatchRoute:
                    if (NewRoutes && OldRoutes)
                    {
                        for (i = 0; i < NewRoutes->Count; i++)
                            if (!WdxFindRoute(OldRoutes, &NewRoutes->Rows[i]) &&
                                WdxFamilyMatches(Notification->Family, AF_INET))
                            {
                                ((PIPFORWARD_CHANGE_CALLBACK)Notification->Callback)(
                                    Notification->Context, &NewRoutes->Rows[i], MibAddInstance);
                            }
                        for (i = 0; i < OldRoutes->Count; i++)
                            if (!WdxFindRoute(NewRoutes, &OldRoutes->Rows[i]) &&
                                WdxFamilyMatches(Notification->Family, AF_INET))
                            {
                                ((PIPFORWARD_CHANGE_CALLBACK)Notification->Callback)(
                                    Notification->Context, &OldRoutes->Rows[i], MibDeleteInstance);
                            }
                    }
                    break;

                case WdxWatchUnicastAddress:
                    if (NewAddrs && OldAddrs)
                    {
                        for (i = 0; i < NewAddrs->Count; i++)
                            if (!WdxFindAddr(OldAddrs, &NewAddrs->Rows[i]) &&
                                WdxFamilyMatches(Notification->Family, AF_INET))
                            {
                                ((PUNICAST_IPADDRESS_CHANGE_CALLBACK)Notification->Callback)(
                                    Notification->Context, &NewAddrs->Rows[i], MibAddInstance);
                            }
                        for (i = 0; i < OldAddrs->Count; i++)
                            if (!WdxFindAddr(NewAddrs, &OldAddrs->Rows[i]) &&
                                WdxFamilyMatches(Notification->Family, AF_INET))
                            {
                                ((PUNICAST_IPADDRESS_CHANGE_CALLBACK)Notification->Callback)(
                                    Notification->Context, &OldAddrs->Rows[i], MibDeleteInstance);
                            }
                    }
                    break;

                case WdxWatchIpInterface:
                    if (NewIfs && OldIfs)
                    {
                        MIB_IPINTERFACE_ROW Row;
                        MIB_IFROW IfRow;
                        ULONG j;

                        for (i = 0; i < NewIfs->Count; i++)
                        {
                            BOOLEAN Known = FALSE;

                            for (j = 0; j < OldIfs->Count; j++)
                            {
                                if (OldIfs->Index[j] == NewIfs->Index[i])
                                {
                                    Known = TRUE;
                                    if ((OldIfs->Oper[j] != NewIfs->Oper[i] ||
                                         OldIfs->Speed[j] != NewIfs->Speed[i]) &&
                                        WdxFamilyMatches(Notification->Family, AF_INET))
                                    {
                                        RtlZeroMemory(&IfRow, sizeof(IfRow));
                                        IfRow.dwIndex = NewIfs->Index[i];
                                        if (GetIfEntry(&IfRow) == NO_ERROR)
                                        {
                                            WdxBuildIpInterfaceRow(&Row, &IfRow, AF_INET);
                                            ((PIPINTERFACE_CHANGE_CALLBACK)Notification->Callback)(
                                                Notification->Context, &Row, MibParameterNotification);
                                        }
                                    }
                                }
                            }
                            if (!Known && WdxFamilyMatches(Notification->Family, AF_INET))
                            {
                                RtlZeroMemory(&IfRow, sizeof(IfRow));
                                IfRow.dwIndex = NewIfs->Index[i];
                                if (GetIfEntry(&IfRow) == NO_ERROR)
                                {
                                    WdxBuildIpInterfaceRow(&Row, &IfRow, AF_INET);
                                    ((PIPINTERFACE_CHANGE_CALLBACK)Notification->Callback)(
                                        Notification->Context, &Row, MibAddInstance);
                                }
                            }
                        }
                    }
                    break;
            }

            WdxDereference(Notification);
        }

        /* The next baseline is this poll, keeping only what arrived. */
        if (OldRoutes)
            HeapFree(GetProcessHeap(), 0, OldRoutes);
        if (OldAddrs)
            HeapFree(GetProcessHeap(), 0, OldAddrs);
        if (OldIfs)
            HeapFree(GetProcessHeap(), 0, OldIfs);
        OldRoutes = NewRoutes;
        OldAddrs = NewAddrs;
        OldIfs = NewIfs;
        NewRoutes = NULL;
        NewAddrs = NULL;
        NewIfs = NULL;
    }

    if (OldRoutes)
        HeapFree(GetProcessHeap(), 0, OldRoutes);
    if (OldAddrs)
        HeapFree(GetProcessHeap(), 0, OldAddrs);
    if (OldIfs)
        HeapFree(GetProcessHeap(), 0, OldIfs);
    if (NewRoutes)
        HeapFree(GetProcessHeap(), 0, NewRoutes);
    if (NewAddrs)
        HeapFree(GetProcessHeap(), 0, NewAddrs);
    if (NewIfs)
        HeapFree(GetProcessHeap(), 0, NewIfs);
    return 0;
}

static VOID
WdxStartMonitor(VOID)
{
    if (WdxMonitorThread)
        return;
    WdxMonitorQuit = FALSE;
    WdxMonitorKick = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!WdxMonitorKick)
        return;
    WdxMonitorThread = CreateThread(NULL, 0, WdxMonitor, NULL, 0, NULL);
    if (!WdxMonitorThread)
    {
        CloseHandle(WdxMonitorKick);
        WdxMonitorKick = NULL;
    }
}

static VOID
WdxStopMonitor(VOID)
{
    HANDLE Thread;

    if (!WdxMonitorThread)
        return;
    Thread = WdxMonitorThread;
    WdxMonitorQuit = TRUE;
    SetEvent(WdxMonitorKick);
    WaitForSingleObject(Thread, 5000);
    CloseHandle(Thread);
    CloseHandle(WdxMonitorKick);
    WdxMonitorThread = NULL;
    WdxMonitorKick = NULL;
}

static DWORD
WdxRegister(WDX_WATCH_TYPE Type, ADDRESS_FAMILY Family, PVOID Callback,
            PVOID Context, BOOLEAN Initial, HANDLE *NotificationHandle)
{
    PWDX_NOTIFICATION Notification;

    if (!Callback || !NotificationHandle)
        return ERROR_INVALID_PARAMETER;
    if (Family != AF_INET && Family != AF_INET6 && Family != AF_UNSPEC)
        return ERROR_INVALID_PARAMETER;

    Notification = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*Notification));
    if (!Notification)
        return ERROR_NOT_ENOUGH_MEMORY;
    Notification->RefCount = 2;             /* caller + list */
    Notification->Type = Type;
    Notification->Family = Family;
    Notification->Callback = Callback;
    Notification->Context = Context;

    WdxLock();
    InsertTailList(&WdxNotifyList, &Notification->Entry);
    WdxStartMonitor();
    WdxUnlock();

    if (Initial)
    {
        /* Snapshot pass: every current row reported as an initial
         * notification, without waiting for a change. */
        switch (Type)
        {
            case WdxWatchRoute:
            {
                PMIB_IPFORWARD_TABLE2 Table = NULL;
                ULONG i;

                if (GetIpForwardTable2(AF_INET, &Table) == NO_ERROR && Table)
                {
                    for (i = 0; i < Table->NumEntries; i++)
                        ((PIPFORWARD_CHANGE_CALLBACK)Callback)(Context, &Table->Table[i],
                                                               MibInitialNotification);
                    FreeMibTable(Table);
                }
                break;
            }
            case WdxWatchUnicastAddress:
            {
                PMIB_UNICASTIPADDRESS_TABLE Table = NULL;
                ULONG i;

                if (GetUnicastIpAddressTable(AF_INET, &Table) == NO_ERROR && Table)
                {
                    for (i = 0; i < Table->NumEntries; i++)
                        ((PUNICAST_IPADDRESS_CHANGE_CALLBACK)Callback)(Context, &Table->Table[i],
                                                                       MibInitialNotification);
                    FreeMibTable(Table);
                }
                break;
            }
            case WdxWatchIpInterface:
            {
                PMIB_IPINTERFACE_TABLE Table = NULL;
                ULONG i;

                if (GetIpInterfaceTable(AF_INET, &Table) == NO_ERROR && Table)
                {
                    for (i = 0; i < Table->NumEntries; i++)
                        ((PIPINTERFACE_CHANGE_CALLBACK)Callback)(Context, &Table->Table[i],
                                                                 MibInitialNotification);
                    FreeMibTable(Table);
                }
                break;
            }
        }
    }

    *NotificationHandle = Notification;
    return NO_ERROR;
}

static DWORD
WdxCancel(HANDLE NotificationHandle)
{
    PWDX_NOTIFICATION Notification = (PWDX_NOTIFICATION)NotificationHandle;
    PLIST_ENTRY Entry;
    BOOLEAN Found = FALSE;

    if (!Notification)
        return ERROR_INVALID_PARAMETER;

    WdxLock();
    for (Entry = WdxNotifyList.Flink; Entry != &WdxNotifyList; Entry = Entry->Flink)
    {
        if (Entry == &Notification->Entry)
        {
            RemoveEntryList(Entry);
            Found = TRUE;
            break;
        }
    }
    WdxUnlock();

    if (!Found)
        return ERROR_INVALID_PARAMETER;
    WdxDereference(Notification);           /* list reference */
    return NO_ERROR;
}

DWORD WINAPI
NotifyIpInterfaceChange(ADDRESS_FAMILY Family, PIPINTERFACE_CHANGE_CALLBACK Callback,
                        PVOID Context, BOOLEAN InitialNotification, HANDLE *NotificationHandle)
{
    TRACE("NotifyIpInterfaceChange(%u, %p, %p, %u, %p)\n",
          Family, Callback, Context, InitialNotification, NotificationHandle);
    return WdxRegister(WdxWatchIpInterface, Family, Callback, Context,
                       InitialNotification, NotificationHandle);
}

DWORD WINAPI
NotifyUnicastIpAddressChange(ADDRESS_FAMILY Family, PUNICAST_IPADDRESS_CHANGE_CALLBACK Callback,
                             PVOID Context, BOOLEAN InitialNotification, HANDLE *NotificationHandle)
{
    TRACE("NotifyUnicastIpAddressChange(%u, %p, %p, %u, %p)\n",
          Family, Callback, Context, InitialNotification, NotificationHandle);
    return WdxRegister(WdxWatchUnicastAddress, Family, Callback, Context,
                       InitialNotification, NotificationHandle);
}

DWORD WINAPI
NotifyRouteChange2(ADDRESS_FAMILY Family, PIPFORWARD_CHANGE_CALLBACK Callback,
                   PVOID Context, BOOLEAN InitialNotification, HANDLE *NotificationHandle)
{
    TRACE("NotifyRouteChange2(%u, %p, %p, %u, %p)\n",
          Family, Callback, Context, InitialNotification, NotificationHandle);
    return WdxRegister(WdxWatchRoute, Family, Callback, Context,
                       InitialNotification, NotificationHandle);
}

DWORD WINAPI
CancelMibChangeNotify2(HANDLE NotificationHandle)
{
    TRACE("CancelMibChangeNotify2(%p)\n", NotificationHandle);
    return WdxCancel(NotificationHandle);
}

/* Tables from the Vista functions are single heap blocks. */
VOID WINAPI
FreeMibTable(PVOID Memory)
{
    HeapFree(GetProcessHeap(), 0, Memory);
}
