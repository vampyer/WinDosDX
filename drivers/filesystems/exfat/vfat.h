#ifndef _VFATFS_PCH_
#define _VFATFS_PCH_

#include <ntifs.h>
#include <ntdddisk.h>
#include <dos.h>
#include <pseh/pseh2.h>
#include <section_attribs.h>
#ifdef KDBG
#include <ndk/kdfuncs.h>
#include <reactos/kdros.h>
#endif


#define USE_ROS_CC_AND_FS
#define ENABLE_SWAPOUT

/* FIXME: because volume is not cached, we have to perform direct IOs
 * The day this is fixed, just comment out that line, and check
 * it still works (and delete old code ;-))
 */
#define VOLUME_IS_NOT_CACHED_WORK_AROUND_IT


#define ROUND_DOWN(n, align) \
    (((ULONG)n) & ~((align) - 1l))

#define ROUND_UP(n, align) \
    ROUND_DOWN(((ULONG)n) + (align) - 1, (align))

#define ROUND_DOWN_64(n, align) \
    (((ULONGLONG)n) & ~((align) - 1LL))

#define ROUND_UP_64(n, align) \
    ROUND_DOWN_64(((ULONGLONG)n) + (align) - 1LL, (align))

/*
 * exFAT on-disk structures (Microsoft exFAT specification, revision 1.00).
 * Every directory entry is 32 bytes; a file is an entry set made of a File
 * entry, a Stream Extension entry and one File Name entry per 15 characters.
 */
#include <pshpack1.h>
typedef struct _EXFAT_BOOT_SECTOR
{
    UCHAR JumpBoot[3];                  /* 0x00 */
    UCHAR FileSystemName[8];            /* 0x03 "EXFAT   " */
    UCHAR MustBeZero[53];               /* 0x0B */
    ULONGLONG PartitionOffset;          /* 0x40 */
    ULONGLONG VolumeLength;             /* 0x48, in sectors */
    ULONG FatOffset;                    /* 0x50, in sectors */
    ULONG FatLength;                    /* 0x54, in sectors */
    ULONG ClusterHeapOffset;            /* 0x58, in sectors */
    ULONG ClusterCount;                 /* 0x5C */
    ULONG FirstClusterOfRootDirectory;  /* 0x60 */
    ULONG VolumeSerialNumber;           /* 0x64 */
    USHORT FileSystemRevision;          /* 0x68 */
    USHORT VolumeFlags;                 /* 0x6A */
    UCHAR BytesPerSectorShift;          /* 0x6C */
    UCHAR SectorsPerClusterShift;       /* 0x6D */
    UCHAR NumberOfFats;                 /* 0x6E */
    UCHAR DriveSelect;                  /* 0x6F */
    UCHAR PercentInUse;                 /* 0x70 */
    UCHAR Reserved[7];                  /* 0x71 */
    UCHAR BootCode[390];                /* 0x78 */
    USHORT BootSignature;               /* 0x1FE */
} EXFAT_BOOT_SECTOR, *PEXFAT_BOOT_SECTOR;

typedef struct _EXFAT_GENERIC_ENTRY
{
    UCHAR EntryType;
    UCHAR Custom[19];
    ULONG FirstCluster;
    ULONGLONG DataLength;
} EXFAT_GENERIC_ENTRY, *PEXFAT_GENERIC_ENTRY;

typedef struct _EXFAT_FILE_ENTRY
{
    UCHAR EntryType;                    /* 0x85 */
    UCHAR SecondaryCount;
    USHORT SetChecksum;
    USHORT FileAttributes;
    USHORT Reserved1;
    ULONG CreateTimestamp;
    ULONG LastModifiedTimestamp;
    ULONG LastAccessedTimestamp;
    UCHAR Create10msIncrement;
    UCHAR LastModified10msIncrement;
    UCHAR CreateUtcOffset;
    UCHAR LastModifiedUtcOffset;
    UCHAR LastAccessedUtcOffset;
    UCHAR Reserved2[7];
} EXFAT_FILE_ENTRY, *PEXFAT_FILE_ENTRY;

typedef struct _EXFAT_STREAM_ENTRY
{
    UCHAR EntryType;                    /* 0xC0 */
    UCHAR GeneralSecondaryFlags;
    UCHAR Reserved1;
    UCHAR NameLength;
    USHORT NameHash;
    USHORT Reserved2;
    ULONGLONG ValidDataLength;
    ULONG Reserved3;
    ULONG FirstCluster;
    ULONGLONG DataLength;
} EXFAT_STREAM_ENTRY, *PEXFAT_STREAM_ENTRY;

typedef struct _EXFAT_NAME_ENTRY
{
    UCHAR EntryType;                    /* 0xC1 */
    UCHAR GeneralSecondaryFlags;
    WCHAR FileName[15];
} EXFAT_NAME_ENTRY, *PEXFAT_NAME_ENTRY;

typedef struct _EXFAT_BITMAP_ENTRY
{
    UCHAR EntryType;                    /* 0x81 */
    UCHAR BitmapFlags;
    UCHAR Reserved[18];
    ULONG FirstCluster;
    ULONGLONG DataLength;
} EXFAT_BITMAP_ENTRY, *PEXFAT_BITMAP_ENTRY;

typedef struct _EXFAT_UPCASE_ENTRY
{
    UCHAR EntryType;                    /* 0x82 */
    UCHAR Reserved1[3];
    ULONG TableChecksum;
    UCHAR Reserved2[12];
    ULONG FirstCluster;
    ULONGLONG DataLength;
} EXFAT_UPCASE_ENTRY, *PEXFAT_UPCASE_ENTRY;

typedef struct _EXFAT_LABEL_ENTRY
{
    UCHAR EntryType;                    /* 0x83 */
    UCHAR CharacterCount;
    WCHAR VolumeLabel[11];
    UCHAR Reserved[8];
} EXFAT_LABEL_ENTRY, *PEXFAT_LABEL_ENTRY;
#include <poppack.h>

C_ASSERT(sizeof(EXFAT_BOOT_SECTOR) == 512);
C_ASSERT(sizeof(EXFAT_GENERIC_ENTRY) == 32);
C_ASSERT(sizeof(EXFAT_FILE_ENTRY) == 32);
C_ASSERT(sizeof(EXFAT_STREAM_ENTRY) == 32);
C_ASSERT(sizeof(EXFAT_NAME_ENTRY) == 32);

#define EXFAT_ENTRY_SIZE                32
#define EXFAT_ENTRIES_PER_PAGE          (PAGE_SIZE / EXFAT_ENTRY_SIZE)

/* EntryType values and bits */
#define EXFAT_TYPE_END                  0x00
#define EXFAT_TYPE_IN_USE               0x80
#define EXFAT_TYPE_SECONDARY            0x40
#define EXFAT_TYPE_BENIGN               0x20
#define EXFAT_TYPE_BITMAP               0x81
#define EXFAT_TYPE_UPCASE               0x82
#define EXFAT_TYPE_LABEL                0x83
#define EXFAT_TYPE_FILE                 0x85
#define EXFAT_TYPE_STREAM               0xC0
#define EXFAT_TYPE_NAME                 0xC1

/* GeneralSecondaryFlags */
#define EXFAT_FLAG_ALLOCATION_POSSIBLE  0x01
#define EXFAT_FLAG_NO_FAT_CHAIN         0x02

/* VolumeFlags */
#define EXFAT_VOLUME_ACTIVE_FAT         0x0001
#define EXFAT_VOLUME_DIRTY              0x0002
#define EXFAT_VOLUME_MEDIA_FAILURE      0x0004

/* FAT entry values */
#define EXFAT_CLUSTER_FREE              0x00000000
#define EXFAT_CLUSTER_BAD               0xFFFFFFF7
#define EXFAT_CLUSTER_EOF               0xFFFFFFFF
#define EXFAT_FIRST_DATA_CLUSTER        2

#define EXFAT_NAME_CHARS_PER_ENTRY      15
#define EXFAT_MAX_NAME_LENGTH           255
#define LONGNAME_MAX_LENGTH             256     /* name buffer size, with room for a NUL */
/* File + Stream + at most 17 name entries: the largest set this driver builds. */
#define EXFAT_MAX_BUILT_SET_ENTRIES     19
/* A set may carry vendor secondary entries up to SecondaryCount 255. */
#define EXFAT_MAX_SET_ENTRIES           256

/*
 * In-memory copy of the two entries of a file's set that describe it: the
 * File entry and the Stream Extension. The name is kept in the FCB.
 */
typedef struct _DIR_ENTRY
{
    EXFAT_FILE_ENTRY File;
    EXFAT_STREAM_ENTRY Stream;
} DIR_ENTRY, *PDIR_ENTRY;

/*
 * A cluster chain. A "no FAT chain" allocation is Count contiguous clusters
 * whose FAT entries are undefined; otherwise the FAT links the clusters.
 */
typedef struct _EXFAT_CHAIN
{
    ULONG FirstCluster;
    ULONG Count;            /* clusters allocated; bounds a NoFatChain walk */
    BOOLEAN NoFatChain;
} EXFAT_CHAIN, *PEXFAT_CHAIN;

#define VCB_VOLUME_LOCKED       0x0001
#define VCB_DISMOUNT_PENDING    0x0002
#define VCB_WRITE_PROTECTED     0x0004 /* Writes refused (not enabled, or unsupported layout) */
#define VCB_IS_SYS_OR_HAS_PAGE  0x0008
#define VCB_IS_DIRTY            0x4000 /* Volume is dirty */
#define VCB_CLEAR_DIRTY         0x8000 /* Clean dirty flag at shutdown */
/* VCB condition state */
#define VCB_GOOD                0x0010 /* If not set, the VCB is improper for usage */

typedef struct
{
    ULONG VolumeID;
    ULONG FATStart;             /* first sector of the active FAT */
    ULONG FATCount;
    ULONG FATSectors;
    ULONG dataStart;            /* first sector of the cluster heap */
    ULONG RootCluster;
    ULONG SectorsPerCluster;
    ULONG BytesPerSector;
    ULONG BytesPerCluster;
    ULONG NumberOfClusters;
    ULONGLONG Sectors;          /* volume length */
    USHORT VolumeFlags;         /* as read at mount */
    USHORT Revision;
    UCHAR SectorShift;
    UCHAR ClusterShift;
    BOOLEAN FixedMedia;
} FATINFO, *PFATINFO;

struct _VFATFCB;
struct _VFAT_DIRENTRY_CONTEXT;
struct _VFAT_MOVE_CONTEXT;
struct _VFAT_CLOSE_CONTEXT;

typedef struct _HASHENTRY
{
    ULONG Hash;
    struct _VFATFCB* self;
    struct _HASHENTRY* next;
}
HASHENTRY;

typedef struct DEVICE_EXTENSION *PDEVICE_EXTENSION;

#define STATISTICS_SIZE_NO_PAD (sizeof(FILESYSTEM_STATISTICS) + sizeof(FAT_STATISTICS))
typedef struct _STATISTICS {
    FILESYSTEM_STATISTICS Base;
    FAT_STATISTICS Fat;
    UCHAR Pad[((STATISTICS_SIZE_NO_PAD + 0x3f) & ~0x3f) - STATISTICS_SIZE_NO_PAD];
} STATISTICS, *PSTATISTICS;

typedef struct DEVICE_EXTENSION
{
    ERESOURCE DirResource;
    ERESOURCE FatResource;      /* guards the FAT and the allocation bitmap */

    KSPIN_LOCK FcbListLock;
    LIST_ENTRY FcbListHead;
    ULONG HashTableSize;
    struct _HASHENTRY **FcbHashTable;

    PDEVICE_OBJECT VolumeDevice;
    PDEVICE_OBJECT StorageDevice;
    PFILE_OBJECT FATFileObject;
    FATINFO FatInfo;
    ULONG LastAvailableCluster;
    ULONG AvailableClusters;
    BOOLEAN AvailableClustersValid;
    ULONG Flags;
    struct _VFATFCB *VolumeFcb;
    struct _VFATFCB *RootFcb;
    PSTATISTICS Statistics;

    /* Overflow request queue */
    KSPIN_LOCK OverflowQueueSpinLock;
    LIST_ENTRY OverflowQueue;
    ULONG OverflowQueueCount;
    ULONG PostedRequestCount;

    /*
     * Allocation bitmap: one bit per cluster of the heap, bit 0 = cluster 2.
     * It is held in memory; changed sectors are marked in BitmapDirty and
     * written back as whole, aligned sectors by ExfatFlushBitmap.
     */
    EXFAT_CHAIN BitmapChain;
    PULONG BitmapClusters;      /* the bitmap's clusters, in order */
    ULONG BitmapBytes;          /* on-disk length, ceil(clusters / 8) */
    RTL_BITMAP Bitmap;
    RTL_BITMAP BitmapDirty;     /* one bit per sector of the bitmap */

    /* $UpCase expanded to one entry per UTF-16 code unit */
    PUSHORT UpcaseTable;

    WCHAR VolumeLabel[11];
    USHORT VolumeLabelLength;   /* bytes */

    LIST_ENTRY VolumeListEntry;

    /* Notifications */
    LIST_ENTRY NotifyList;
    PNOTIFY_SYNC NotifySync;

    /* Incremented on IRP_MJ_CREATE, decremented on IRP_MJ_CLOSE */
    ULONG OpenHandleCount;

    /* VPBs for dismount */
    PVPB IoVPB;
    PVPB SpareVPB;
} DEVICE_EXTENSION, VCB, *PVCB;

/* dirwr.c / direntry.c: the directory operations */

BOOLEAN
VfatIsDirectoryEmpty(
    PDEVICE_EXTENSION DeviceExt,
    struct _VFATFCB* Fcb);

NTSTATUS
VfatAddEntry(
    PDEVICE_EXTENSION DeviceExt,
    PUNICODE_STRING NameU,
    struct _VFATFCB** Fcb,
    struct _VFATFCB* ParentFcb,
    ULONG RequestedOptions,
    USHORT ReqAttr,
    struct _VFAT_MOVE_CONTEXT* MoveContext);

NTSTATUS
VfatDelEntry(
    PDEVICE_EXTENSION DeviceExt,
    struct _VFATFCB* Fcb,
    struct _VFAT_MOVE_CONTEXT* MoveContext);

NTSTATUS
VfatGetNextDirEntry(
    PDEVICE_EXTENSION DeviceExt,
    PVOID *pContext,
    PVOID *pPage,
    struct _VFATFCB* pDirFcb,
    struct _VFAT_DIRENTRY_CONTEXT* DirContext,
    BOOLEAN First);

#define VFAT_BREAK_ON_CORRUPTION 1
/* Set from the service key's EnableWriteSupport value; off by default. */
#define VFAT_ENABLE_WRITE_SUPPORT 2

typedef struct
{
    PDRIVER_OBJECT DriverObject;
    PDEVICE_OBJECT DeviceObject;
    ULONG Flags;
    ULONG NumberProcessors;
    ERESOURCE VolumeListLock;
    LIST_ENTRY VolumeListHead;
    NPAGED_LOOKASIDE_LIST FcbLookasideList;
    NPAGED_LOOKASIDE_LIST CcbLookasideList;
    NPAGED_LOOKASIDE_LIST IrpContextLookasideList;
    PAGED_LOOKASIDE_LIST CloseContextLookasideList;
    FAST_IO_DISPATCH FastIoDispatch;
    CACHE_MANAGER_CALLBACKS CacheMgrCallbacks;
    FAST_MUTEX CloseMutex;
    ULONG CloseCount;
    LIST_ENTRY CloseListHead;
    BOOLEAN CloseWorkerRunning;
    PIO_WORKITEM CloseWorkItem;
    BOOLEAN ShutdownStarted;
} VFAT_GLOBAL_DATA, *PVFAT_GLOBAL_DATA;

extern PVFAT_GLOBAL_DATA VfatGlobalData;

#define FCB_CACHE_INITIALIZED   0x0001
#define FCB_DELETE_PENDING      0x0002
#define FCB_IS_FAT              0x0004
#define FCB_IS_PAGE_FILE        0x0008
#define FCB_IS_VOLUME           0x0010
#define FCB_IS_DIRTY            0x0020
#define FCB_DELAYED_CLOSE       0x0040
/* Deleted on disk and out of the name table: a new file may take the name
   while this FCB waits for its last reference. */
#define FCB_UNLINKED            0x0200
#ifdef KDBG
#define FCB_CLEANED_UP          0x0080
#define FCB_CLOSED              0x0100
#endif

#define NODE_TYPE_FCB ((CSHORT)0x0502)

typedef struct _VFATFCB
{
    /* FCB header required by ROS/NT */
    FSRTL_COMMON_FCB_HEADER RFCB;
    SECTION_OBJECT_POINTERS SectionObjectPointers;
    ERESOURCE MainResource;
    ERESOURCE PagingIoResource;
    /* end FCB header required by ROS/NT */

    /* File and Stream Extension entries of this file's entry set */
    DIR_ENTRY entry;

    /* Pointer to the attributes in entry (the low byte holds every
       attribute exFAT defines) */
    PUCHAR Attributes;

    /*
     * The clusters of the file. This is authoritative while the FCB exists;
     * VfatUpdateEntry copies it back into entry.Stream.
     */
    EXFAT_CHAIN Chain;

    /* file name, points into PathNameBuffer */
    UNICODE_STRING LongNameU;

    /* directory name, points into PathNameBuffer */
    UNICODE_STRING DirNameU;

    /* path + file name */
    UNICODE_STRING PathNameU;

    /* buffer for PathNameU */
    PWCHAR PathNameBuffer;

    /* */
    LONG RefCount;

    /* List of FCB's for this volume */
    LIST_ENTRY FcbListEntry;

    /* List of FCB's for the parent */
    LIST_ENTRY ParentListEntry;

    /* pointer to the parent fcb */
    struct _VFATFCB *parentFcb;

    /* List for the children */
    LIST_ENTRY ParentListHead;

    /* Flags for the fcb */
    ULONG Flags;

    /* pointer to the file object which has initialized the fcb */
    PFILE_OBJECT FileObject;

    /* Index (in 32-byte entries) of the File entry in the parent directory */
    ULONG startIndex;

    /* Number of entries in the set: 1 + SecondaryCount */
    ULONG EntryCount;

    /* Share access for the file object */
    SHARE_ACCESS FCBShareAccess;

    /* Incremented on IRP_MJ_CREATE, decremented on IRP_MJ_CLEANUP */
    ULONG OpenHandleCount;

    /* Entry into the hash table for the path + name */
    HASHENTRY Hash;

    /* List of byte-range locks for this file */
    FILE_LOCK FileLock;

    /*
     * Optimization: caching of last read/write cluster+offset pair. Can't
     * be in VFATCCB because it must be reset everytime the allocated clusters
     * change.
     */
    FAST_MUTEX LastMutex;
    ULONG LastCluster;
    ULONGLONG LastOffset;

    struct _VFAT_CLOSE_CONTEXT * CloseContext;
} VFATFCB, *PVFATFCB;

#define CCB_DELETE_ON_CLOSE     0x0001

typedef struct _VFATCCB
{
    LARGE_INTEGER  CurrentByteOffset;
    ULONG Flags;
    /* for DirectoryControl */
    ULONG Entry;
    /* for DirectoryControl: exFAT stores no "." and "..", so they are
       returned first, and this counts how many were */
    ULONG DotsReturned;
    /* for DirectoryControl */
    UNICODE_STRING SearchPattern;
} VFATCCB, *PVFATCCB;

#define TAG_CCB  'CtaF'
#define TAG_FCB  'FtaF'
#define TAG_IRP  'ItaF'
#define TAG_CLOSE 'xtaF'
#define TAG_STATS 'VtaF'
#define TAG_BUFFER 'OtaF'
#define TAG_VPB 'vtaF'
#define TAG_NAME 'ntaF'
#define TAG_SEARCH 'LtaF'
#define TAG_DIRENT 'DtaF'

typedef struct __DOSTIME
{
    USHORT Second:5;
    USHORT Minute:6;
    USHORT Hour:5;
}
DOSTIME, *PDOSTIME;

typedef struct __DOSDATE
{
    USHORT Day:5;
    USHORT Month:4;
    USHORT Year:7;
}
DOSDATE, *PDOSDATE;

#define IRPCONTEXT_CANWAIT          0x0001
#define IRPCONTEXT_COMPLETE         0x0002
#define IRPCONTEXT_QUEUE            0x0004
#define IRPCONTEXT_PENDINGRETURNED  0x0008
#define IRPCONTEXT_DEFERRED_WRITE   0x0010

typedef struct
{
    PIRP Irp;
    PDEVICE_OBJECT DeviceObject;
    PDEVICE_EXTENSION DeviceExt;
    ULONG Flags;
    WORK_QUEUE_ITEM WorkQueueItem;
    PIO_STACK_LOCATION Stack;
    UCHAR MajorFunction;
    UCHAR MinorFunction;
    PFILE_OBJECT FileObject;
    ULONG RefCount;
    KEVENT Event;
    CCHAR PriorityBoost;
} VFAT_IRP_CONTEXT, *PVFAT_IRP_CONTEXT;

typedef struct _VFAT_DIRENTRY_CONTEXT
{
    ULONG StartIndex;           /* index of the File entry */
    ULONG DirIndex;             /* cursor; on return, the set's last entry */
    ULONG EntryCount;           /* entries in the set */
    DIR_ENTRY DirEntry;
    UNICODE_STRING LongNameU;   /* caller supplies a 256-character buffer */
    PDEVICE_EXTENSION DeviceExt;
} VFAT_DIRENTRY_CONTEXT, *PVFAT_DIRENTRY_CONTEXT;

/* A rename or move keeps the file's attributes, times and clusters. */
typedef struct _VFAT_MOVE_CONTEXT
{
    DIR_ENTRY Entry;
    EXFAT_CHAIN Chain;
    BOOLEAN InPlace;
} VFAT_MOVE_CONTEXT, *PVFAT_MOVE_CONTEXT;

typedef struct _VFAT_CLOSE_CONTEXT
{
    PDEVICE_EXTENSION Vcb;
    PVFATFCB Fcb;
    LIST_ENTRY CloseListEntry;
} VFAT_CLOSE_CONTEXT, *PVFAT_CLOSE_CONTEXT;

FORCEINLINE
NTSTATUS
VfatMarkIrpContextForQueue(PVFAT_IRP_CONTEXT IrpContext)
{
    PULONG Flags = &IrpContext->Flags;

    *Flags &= ~IRPCONTEXT_COMPLETE;
    *Flags |= IRPCONTEXT_QUEUE;

    return STATUS_PENDING;
}

FORCEINLINE
BOOLEAN
vfatFCBIsDirectory(PVFATFCB FCB)
{
    return BooleanFlagOn(*FCB->Attributes, FILE_ATTRIBUTE_DIRECTORY);
}

FORCEINLINE
BOOLEAN
vfatFCBIsReadOnly(PVFATFCB FCB)
{
    return BooleanFlagOn(*FCB->Attributes, FILE_ATTRIBUTE_READONLY);
}

FORCEINLINE
VOID
vfatReportChange(
    IN PDEVICE_EXTENSION DeviceExt,
    IN PVFATFCB Fcb,
    IN ULONG FilterMatch,
    IN ULONG Action)
{
    FsRtlNotifyFullReportChange(DeviceExt->NotifySync,
                                &(DeviceExt->NotifyList),
                                (PSTRING)&Fcb->PathNameU,
                                Fcb->PathNameU.Length - Fcb->LongNameU.Length,
                                NULL, NULL, FilterMatch, Action, NULL);
}

#define vfatAddToStat(Vcb, Stat, Inc)                                                                         \
{                                                                                                             \
    PSTATISTICS Stats = &(Vcb)->Statistics[KeGetCurrentProcessorNumber() % VfatGlobalData->NumberProcessors]; \
    Stats->Stat += Inc;                                                                                       \
}

/* blockdev.c */

NTSTATUS
VfatReadDisk(
    IN PDEVICE_OBJECT pDeviceObject,
    IN PLARGE_INTEGER ReadOffset,
    IN ULONG ReadLength,
    IN PUCHAR Buffer,
    IN BOOLEAN Override);

NTSTATUS
VfatReadDiskPartial(
    IN PVFAT_IRP_CONTEXT IrpContext,
    IN PLARGE_INTEGER ReadOffset,
    IN ULONG ReadLength,
    IN ULONG BufferOffset,
    IN BOOLEAN Wait);

NTSTATUS
VfatWriteDisk(
    IN PDEVICE_OBJECT pDeviceObject,
    IN PLARGE_INTEGER WriteOffset,
    IN ULONG WriteLength,
    IN OUT PUCHAR Buffer,
    IN BOOLEAN Override);

NTSTATUS
VfatWriteDiskPartial(
    IN PVFAT_IRP_CONTEXT IrpContext,
    IN PLARGE_INTEGER WriteOffset,
    IN ULONG WriteLength,
    IN ULONG BufferOffset,
    IN BOOLEAN Wait);

NTSTATUS
VfatBlockDeviceIoControl(
    IN PDEVICE_OBJECT DeviceObject,
    IN ULONG CtlCode,
    IN PVOID InputBuffer,
    IN ULONG InputBufferSize,
    IN OUT PVOID OutputBuffer,
    IN OUT PULONG pOutputBufferSize,
    IN BOOLEAN Override);

/* cleanup.c */

NTSTATUS
VfatCleanup(
    PVFAT_IRP_CONTEXT IrpContext);

/* close.c */

NTSTATUS
VfatClose(
    PVFAT_IRP_CONTEXT IrpContext);

NTSTATUS
VfatCloseFile(
    PDEVICE_EXTENSION DeviceExt,
    PFILE_OBJECT FileObject);

/* create.c */

NTSTATUS
VfatCreate(
    PVFAT_IRP_CONTEXT IrpContext);

NTSTATUS
FindFile(
    PDEVICE_EXTENSION DeviceExt,
    PVFATFCB Parent,
    PUNICODE_STRING FileToFindU,
    PVFAT_DIRENTRY_CONTEXT DirContext,
    BOOLEAN First);

/* dir.c */

NTSTATUS
VfatDirectoryControl(
    PVFAT_IRP_CONTEXT IrpContext);

/* direntry.c */

VOID
ExfatTimestampToSystemTime(
    ULONG Timestamp,
    UCHAR Increment10ms,
    UCHAR UtcOffset,
    PLARGE_INTEGER SystemTime);

VOID
ExfatSystemTimeToTimestamp(
    PLARGE_INTEGER SystemTime,
    PULONG Timestamp,
    PUCHAR Increment10ms,
    PUCHAR UtcOffset);

USHORT
ExfatEntrySetChecksum(
    PUCHAR Entries,
    ULONG Count);

WCHAR
ExfatUpcaseChar(
    PDEVICE_EXTENSION DeviceExt,
    WCHAR Char);

USHORT
ExfatNameHash(
    PDEVICE_EXTENSION DeviceExt,
    PCUNICODE_STRING Name);

BOOLEAN
ExfatNamesEqual(
    PDEVICE_EXTENSION DeviceExt,
    PCUNICODE_STRING Name1,
    PCUNICODE_STRING Name2);

VOID
ExfatChainFromEntry(
    PDEVICE_EXTENSION DeviceExt,
    PDIR_ENTRY Entry,
    PEXFAT_CHAIN Chain);

/* dirwr.c */

NTSTATUS
vfatFCBInitializeCacheFromVolume(
    PVCB vcb,
    PVFATFCB fcb);

NTSTATUS
VfatUpdateEntry(
    IN PDEVICE_EXTENSION DeviceExt,
    PVFATFCB pFcb);

VOID
ExfatSyncEntryFromFcb(
    PVFATFCB pFcb);

NTSTATUS
ExfatFlushFcbSet(
    PVFATFCB pFcb);

BOOLEAN
vfatFindDirSpace(
    PDEVICE_EXTENSION DeviceExt,
    PVFATFCB pDirFcb,
    ULONG nbSlots,
    PULONG start);

NTSTATUS
vfatRenameEntry(
    IN PDEVICE_EXTENSION DeviceExt,
    IN PVFATFCB pFcb,
    IN PUNICODE_STRING FileName,
    IN BOOLEAN CaseChangeOnly);

NTSTATUS
VfatMoveEntry(
    IN PDEVICE_EXTENSION DeviceExt,
    IN PVFATFCB pFcb,
    IN PUNICODE_STRING FileName,
    IN PVFATFCB ParentFcb);

/* ea.h */

NTSTATUS
VfatSetExtendedAttributes(
    PFILE_OBJECT FileObject,
    PVOID Ea,
    ULONG EaLength);

/* fastio.c */

CODE_SEG("INIT")
VOID
VfatInitFastIoRoutines(
    PFAST_IO_DISPATCH FastIoDispatch);

BOOLEAN
NTAPI
VfatAcquireForLazyWrite(
    IN PVOID Context,
    IN BOOLEAN Wait);

BOOLEAN
NTAPI
VfatAcquireForReadAhead(
    IN PVOID Context,
    IN BOOLEAN Wait);

VOID
NTAPI
VfatReleaseFromLazyWrite(
    IN PVOID Context);

/* fat.c */

NTSTATUS
ExfatReadFatEntry(
    PDEVICE_EXTENSION DeviceExt,
    ULONG Cluster,
    PULONG Value);

NTSTATUS
ExfatWriteFatEntry(
    PDEVICE_EXTENSION DeviceExt,
    ULONG Cluster,
    ULONG Value);

NTSTATUS
ExfatLoadBitmap(
    PDEVICE_EXTENSION DeviceExt);

NTSTATUS
ExfatFlushBitmap(
    PDEVICE_EXTENSION DeviceExt);

NTSTATUS
ExfatFlushBitmapEx(
    PDEVICE_EXTENSION DeviceExt,
    BOOLEAN Wait);

NTSTATUS
ExfatFlushAllocation(
    PDEVICE_EXTENSION DeviceExt);

NTSTATUS
ExfatFlushAllocationEx(
    PDEVICE_EXTENSION DeviceExt,
    BOOLEAN Wait);

VOID
ExfatFreeBitmap(
    PDEVICE_EXTENSION DeviceExt);

NTSTATUS
ExfatAllocateCluster(
    PDEVICE_EXTENSION DeviceExt,
    ULONG Hint,
    PULONG Cluster);

NTSTATUS
ExfatFreeClusters(
    PDEVICE_EXTENSION DeviceExt,
    PEXFAT_CHAIN Chain,
    ULONG KeepClusters);

NTSTATUS
ExfatConvertToFatChain(
    PDEVICE_EXTENSION DeviceExt,
    PEXFAT_CHAIN Chain);

NTSTATUS
OffsetToCluster(
    PDEVICE_EXTENSION DeviceExt,
    PEXFAT_CHAIN Chain,
    ULONG StartCluster,
    ULONGLONG Offset,
    PULONG Cluster,
    BOOLEAN Extend);

NTSTATUS
NextCluster(
    PDEVICE_EXTENSION DeviceExt,
    PEXFAT_CHAIN Chain,
    PULONG CurrentCluster,
    BOOLEAN Extend);

ULONGLONG
ClusterToSector(
    PDEVICE_EXTENSION DeviceExt,
    ULONG Cluster);

NTSTATUS
CountAvailableClusters(
    PDEVICE_EXTENSION DeviceExt,
    PLARGE_INTEGER Clusters);

NTSTATUS
GetDirtyStatus(
    PDEVICE_EXTENSION DeviceExt,
    PBOOLEAN DirtyStatus);

NTSTATUS
SetDirtyStatus(
    PDEVICE_EXTENSION DeviceExt,
    BOOLEAN DirtyStatus);

/* fcb.c */

PVFATFCB
vfatNewFCB(
    PDEVICE_EXTENSION pVCB,
    PUNICODE_STRING pFileNameU);

NTSTATUS
vfatSetFCBNewDirName(
    PDEVICE_EXTENSION pVCB,
    PVFATFCB Fcb,
    PVFATFCB ParentFcb);

NTSTATUS
vfatUpdateFCB(
    PDEVICE_EXTENSION pVCB,
    PVFATFCB Fcb,
    PVFAT_DIRENTRY_CONTEXT DirContext,
    PVFATFCB ParentFcb);

VOID
vfatDestroyFCB(
    PVFATFCB pFCB);

VOID
vfatDestroyCCB(
    PVFATCCB pCcb);

VOID
#ifndef KDBG
vfatGrabFCB(
#else
_vfatGrabFCB(
#endif
    PDEVICE_EXTENSION pVCB,
    PVFATFCB pFCB
#ifdef KDBG
    ,
    PCSTR File,
    ULONG Line,
    PCSTR Func
#endif
    );

VOID
#ifndef KDBG
vfatReleaseFCB(
#else
_vfatReleaseFCB(
#endif
    PDEVICE_EXTENSION pVCB,
    PVFATFCB pFCB
#ifdef KDBG
    ,
    PCSTR File,
    ULONG Line,
    PCSTR Func
#endif
    );

#ifdef KDBG
#define vfatGrabFCB(v, f) _vfatGrabFCB(v, f, __FILE__, __LINE__, __FUNCTION__)
#define vfatReleaseFCB(v, f) _vfatReleaseFCB(v, f, __FILE__, __LINE__, __FUNCTION__)
#endif

PVFATFCB
vfatGrabFCBFromTable(
    PDEVICE_EXTENSION pDeviceExt,
    PUNICODE_STRING pFileNameU);

VOID
vfatUnlinkFCB(
    PDEVICE_EXTENSION pVCB,
    PVFATFCB pFCB);

PVFATFCB
vfatMakeRootFCB(
    PDEVICE_EXTENSION pVCB);

PVFATFCB
vfatOpenRootFCB(
    PDEVICE_EXTENSION pVCB);

BOOLEAN
vfatFCBIsDirectory(
    PVFATFCB FCB);

BOOLEAN
vfatFCBIsRoot(
    PVFATFCB FCB);

NTSTATUS
vfatAttachFCBToFileObject(
    PDEVICE_EXTENSION vcb,
    PVFATFCB fcb,
    PFILE_OBJECT fileObject);

NTSTATUS
vfatDirFindFile(
    PDEVICE_EXTENSION pVCB,
    PVFATFCB parentFCB,
    PUNICODE_STRING FileToFindU,
    PVFATFCB *fileFCB);

NTSTATUS
vfatGetFCBForFile(
    PDEVICE_EXTENSION pVCB,
    PVFATFCB *pParentFCB,
    PVFATFCB *pFCB,
    PUNICODE_STRING pFileNameU);

NTSTATUS
vfatMakeFCBFromDirEntry(
    PVCB vcb,
    PVFATFCB directoryFCB,
    PVFAT_DIRENTRY_CONTEXT DirContext,
    PVFATFCB *fileFCB);

/* finfo.c */

NTSTATUS
VfatGetStandardInformation(
    PVFATFCB FCB,
    PFILE_STANDARD_INFORMATION StandardInfo,
    PULONG BufferLength);

NTSTATUS
VfatGetBasicInformation(
    PFILE_OBJECT FileObject,
    PVFATFCB FCB,
    PDEVICE_EXTENSION DeviceExt,
    PFILE_BASIC_INFORMATION BasicInfo,
    PULONG BufferLength);

NTSTATUS
VfatQueryInformation(
    PVFAT_IRP_CONTEXT IrpContext);

NTSTATUS
VfatSetInformation(
    PVFAT_IRP_CONTEXT IrpContext);

NTSTATUS
VfatSetAllocationSizeInformation(
    PFILE_OBJECT FileObject,
    PVFATFCB Fcb,
    PDEVICE_EXTENSION DeviceExt,
    PLARGE_INTEGER AllocationSize);

/* flush.c */

NTSTATUS
VfatFlush(
    PVFAT_IRP_CONTEXT IrpContext);

NTSTATUS
VfatFlushVolume(
    PDEVICE_EXTENSION DeviceExt,
    PVFATFCB VolumeFcb);

/* fsctl.c */

NTSTATUS
VfatFileSystemControl(
    PVFAT_IRP_CONTEXT IrpContext);

/* iface.c */

CODE_SEG("INIT")
NTSTATUS
NTAPI
DriverEntry(
    PDRIVER_OBJECT DriverObject,
    PUNICODE_STRING RegistryPath);

#ifdef KDBG
/* kdbg.c */
KDBG_CLI_ROUTINE vfatKdbgHandler;
#endif

/* misc.c */

DRIVER_DISPATCH
VfatBuildRequest;

NTSTATUS
NTAPI
VfatBuildRequest(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp);

PVOID
VfatGetUserBuffer(
    IN PIRP Irp,
    IN BOOLEAN Paging);

NTSTATUS
VfatLockUserBuffer(
    IN PIRP Irp,
    IN ULONG Length,
    IN LOCK_OPERATION Operation);

BOOLEAN
VfatCheckForDismount(
    IN PDEVICE_EXTENSION DeviceExt,
    IN BOOLEAN Create);

VOID
vfatReportChange(
    IN PDEVICE_EXTENSION DeviceExt,
    IN PVFATFCB Fcb,
    IN ULONG FilterMatch,
    IN ULONG Action);

VOID
NTAPI
VfatHandleDeferredWrite(
    IN PVOID IrpContext,
    IN PVOID Unused);

/* pnp.c */

NTSTATUS
VfatPnp(
    PVFAT_IRP_CONTEXT IrpContext);

/* rw.c */

NTSTATUS
VfatRead(
    PVFAT_IRP_CONTEXT IrpContext);

NTSTATUS
VfatWrite(
    PVFAT_IRP_CONTEXT *pIrpContext);

/* shutdown.c */

DRIVER_DISPATCH
VfatShutdown;

NTSTATUS
NTAPI
VfatShutdown(
    PDEVICE_OBJECT DeviceObject,
    PIRP Irp);

/* string.c */

VOID
vfatSplitPathName(
    PUNICODE_STRING PathNameU,
    PUNICODE_STRING DirNameU,
    PUNICODE_STRING FileNameU);

BOOLEAN
vfatIsLongIllegal(
    WCHAR c);

BOOLEAN
IsDotOrDotDot(
    PCUNICODE_STRING Name);

/* volume.c */

NTSTATUS
VfatQueryVolumeInformation(
    PVFAT_IRP_CONTEXT IrpContext);

NTSTATUS
VfatSetVolumeInformation(
    PVFAT_IRP_CONTEXT IrpContext);

#endif /* _VFATFS_PCH_ */
