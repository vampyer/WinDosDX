/*
 * PROJECT:     ReactOS NTFS driver
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Directory ($I30) index walking and rewriting
 *
 * Lookups and in-place entry updates walk the index tree from the root.
 * Inserting or removing names rebuilds the directory's index: the entries
 * are collected in order, packed into fresh index blocks (never the blocks
 * the current tree uses), and the new tree is published with one write of
 * the directory's file record. A failure before that write leaves the old
 * index intact.
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* GLOBALS ******************************************************************/

#define NTFS_INDEX_MAX_DEPTH        32
#define NTFS_INDEX_ENTRY_HEADER     FIELD_OFFSET(INDEX_ENTRY_ATTRIBUTE, FileName)
#define NTFS_INDEX_ROOT_HEADER      FIELD_OFFSET(INDEX_ROOT_ATTRIBUTE, Header)
#define NTFS_INDEX_BUFFER_HEADER    FIELD_OFFSET(INDEX_BUFFER, Header)

/* An open directory index: the directory's file record and its $I30 attributes. */
typedef struct _NTFS_INDEX
{
    PDEVICE_EXTENSION Vcb;
    ULONGLONG MftIndex;
    PFILE_RECORD_HEADER Record;
    PNTFS_ATTR_CONTEXT RootCtx;
    ULONG RootOffset;
    PINDEX_ROOT_ATTRIBUTE Root;
    ULONG RootLength;
    PNTFS_ATTR_CONTEXT AllocCtx;
    ULONG BlockSize;
    /* Set once a rewrite has changed anything on disk */
    BOOLEAN DiskTouched;
    /* Index blocks reached by the last walk, as slot numbers */
    PULONG Slots;
    ULONG SlotCount;
    ULONG SlotCapacity;
} NTFS_INDEX, *PNTFS_INDEX;

typedef NTSTATUS
(*PNTFS_INDEX_VISITOR)(PVOID Context,
                       PINDEX_ENTRY_ATTRIBUTE Entry,
                       PBOOLEAN Modified,
                       PBOOLEAN Stop);

typedef struct _NTFS_ENTRY_LIST
{
    PINDEX_ENTRY_ATTRIBUTE *Entries;
    ULONG Count;
    ULONG Capacity;
} NTFS_ENTRY_LIST, *PNTFS_ENTRY_LIST;

/* One key on its way into a node of the rebuilt tree. */
typedef struct _NTFS_LEVEL_ITEM
{
    PINDEX_ENTRY_ATTRIBUTE Entry;
    ULONGLONG ChildVcn;
} NTFS_LEVEL_ITEM, *PNTFS_LEVEL_ITEM;

typedef struct _NTFS_NEW_NODE
{
    ULONG Slot;
    PINDEX_BUFFER Buffer;
} NTFS_NEW_NODE, *PNTFS_NEW_NODE;

typedef struct _NTFS_TREE_BUILDER
{
    PNTFS_INDEX Index;
    PUCHAR OldBitmap;
    ULONG OldBitmapBits;
    ULONG NextSlot;
    PNTFS_NEW_NODE Nodes;
    ULONG NodeCount;
    ULONG NodeCapacity;
    ULONG FirstEntryOffset;     /* header-relative */
    ULONG AllocatedSize;        /* header-relative */
} NTFS_TREE_BUILDER, *PNTFS_TREE_BUILDER;

/* HELPERS ******************************************************************/

static
ULONG
NtfsIndexVcnUnit(PNTFS_INDEX Index)
{
    /* Blocks smaller than a cluster are addressed in sectors (see GetAllocationOffsetFromVCN). */
    if (Index->BlockSize < Index->Vcb->NtfsInfo.BytesPerCluster)
        return Index->Vcb->NtfsInfo.BytesPerSector;
    return Index->Vcb->NtfsInfo.BytesPerCluster;
}

static
ULONGLONG
NtfsIndexSlotToVcn(PNTFS_INDEX Index,
                   ULONG Slot)
{
    return (ULONGLONG)Slot * Index->BlockSize / NtfsIndexVcnUnit(Index);
}

static
NTSTATUS
NtfsIndexVcnToSlot(PNTFS_INDEX Index,
                   ULONGLONG Vcn,
                   PULONG Slot)
{
    ULONGLONG Offset = Vcn * NtfsIndexVcnUnit(Index);

    if (Offset % Index->BlockSize != 0 || Offset / Index->BlockSize > MAXULONG)
        return STATUS_FILE_CORRUPT_ERROR;

    *Slot = (ULONG)(Offset / Index->BlockSize);
    return STATUS_SUCCESS;
}

static
ULONG
NtfsIndexEntryNameLength(PINDEX_ENTRY_ATTRIBUTE Entry)
{
    return FIELD_OFFSET(FILENAME_ATTRIBUTE, Name) + Entry->FileName.NameLength * sizeof(WCHAR);
}

static
VOID
NtfsCloseIndex(PNTFS_INDEX Index)
{
    if (Index->Slots)
        ExFreePoolWithTag(Index->Slots, TAG_NTFS);
    if (Index->Root)
        ExFreePoolWithTag(Index->Root, TAG_NTFS);
    if (Index->AllocCtx)
        ReleaseAttributeContext(Index->AllocCtx);
    if (Index->RootCtx)
        ReleaseAttributeContext(Index->RootCtx);
    if (Index->Record)
        ExFreeToNPagedLookasideList(&Index->Vcb->FileRecLookasideList, Index->Record);
    RtlZeroMemory(Index, sizeof(*Index));
}

/* Reads the root value of the $I30 index out of the (in-memory) directory record. */
static
NTSTATUS
NtfsLoadIndexRoot(PNTFS_INDEX Index)
{
    PNTFS_ATTR_RECORD RootAttribute;
    PINDEX_ROOT_ATTRIBUTE Root;
    ULONG Length;

    RootAttribute = (PNTFS_ATTR_RECORD)((PUCHAR)Index->Record + Index->RootOffset);
    if (RootAttribute->IsNonResident)
        return STATUS_FILE_CORRUPT_ERROR;

    Length = RootAttribute->Resident.ValueLength;
    if (Length < NTFS_INDEX_ROOT_HEADER + sizeof(INDEX_HEADER_ATTRIBUTE) + NTFS_INDEX_ENTRY_HEADER ||
        (ULONGLONG)Index->RootOffset + RootAttribute->Resident.ValueOffset + Length >
            Index->Vcb->NtfsInfo.BytesPerFileRecord)
    {
        return STATUS_FILE_CORRUPT_ERROR;
    }

    Root = ExAllocatePoolWithTag(NonPagedPool, Length, TAG_NTFS);
    if (!Root)
        return STATUS_INSUFFICIENT_RESOURCES;

    RtlCopyMemory(Root,
                  (PUCHAR)RootAttribute + RootAttribute->Resident.ValueOffset,
                  Length);

    if (Root->Header.FirstEntryOffset < sizeof(INDEX_HEADER_ATTRIBUTE) ||
        Root->Header.TotalSizeOfEntries > Length - NTFS_INDEX_ROOT_HEADER ||
        Root->Header.FirstEntryOffset + NTFS_INDEX_ENTRY_HEADER > Root->Header.TotalSizeOfEntries)
    {
        ExFreePoolWithTag(Root, TAG_NTFS);
        return STATUS_FILE_CORRUPT_ERROR;
    }

    if (Index->Root)
        ExFreePoolWithTag(Index->Root, TAG_NTFS);
    Index->Root = Root;
    Index->RootLength = Length;
    return STATUS_SUCCESS;
}

static
NTSTATUS
NtfsOpenIndex(PDEVICE_EXTENSION Vcb,
              ULONGLONG MftIndex,
              PNTFS_INDEX Index)
{
    NTSTATUS Status;
    ULONG BlockSize;

    RtlZeroMemory(Index, sizeof(*Index));
    Index->Vcb = Vcb;
    Index->MftIndex = MftIndex;

    Index->Record = ExAllocateFromNPagedLookasideList(&Vcb->FileRecLookasideList);
    if (!Index->Record)
        return STATUS_INSUFFICIENT_RESOURCES;

    Status = ReadFileRecord(Vcb, MftIndex, Index->Record);
    if (!NT_SUCCESS(Status))
        goto Failure;

    Status = FindAttribute(Vcb, Index->Record, AttributeIndexRoot, L"$I30", 4,
                           &Index->RootCtx, &Index->RootOffset);
    if (!NT_SUCCESS(Status))
        goto Failure;

    Status = NtfsLoadIndexRoot(Index);
    if (!NT_SUCCESS(Status))
        goto Failure;

    BlockSize = Index->Root->SizeOfEntry;
    if (BlockSize < Vcb->NtfsInfo.BytesPerSector ||
        BlockSize > 0x10000 ||
        (BlockSize & (BlockSize - 1)) != 0)
    {
        Status = STATUS_FILE_CORRUPT_ERROR;
        goto Failure;
    }
    Index->BlockSize = BlockSize;

    Status = FindAttribute(Vcb, Index->Record, AttributeIndexAllocation, L"$I30", 4,
                           &Index->AllocCtx, NULL);
    if (!NT_SUCCESS(Status))
    {
        Index->AllocCtx = NULL;
    }
    else if (!Index->AllocCtx->pRecord->IsNonResident)
    {
        Status = STATUS_FILE_CORRUPT_ERROR;
        goto Failure;
    }

    return STATUS_SUCCESS;

Failure:
    NtfsCloseIndex(Index);
    return Status;
}

static
NTSTATUS
NtfsRememberSlot(PNTFS_INDEX Index,
                 ULONG Slot)
{
    ULONG i;

    for (i = 0; i < Index->SlotCount; i++)
    {
        /* A block reached twice means the tree has a cycle. */
        if (Index->Slots[i] == Slot)
            return STATUS_FILE_CORRUPT_ERROR;
    }

    if (Index->SlotCount == Index->SlotCapacity)
    {
        ULONG NewCapacity = max(16, Index->SlotCapacity * 2);
        PULONG NewSlots = ExAllocatePoolWithTag(NonPagedPool, NewCapacity * sizeof(ULONG), TAG_NTFS);

        if (!NewSlots)
            return STATUS_INSUFFICIENT_RESOURCES;
        if (Index->Slots)
        {
            RtlCopyMemory(NewSlots, Index->Slots, Index->SlotCount * sizeof(ULONG));
            ExFreePoolWithTag(Index->Slots, TAG_NTFS);
        }
        Index->Slots = NewSlots;
        Index->SlotCapacity = NewCapacity;
    }

    Index->Slots[Index->SlotCount++] = Slot;
    return STATUS_SUCCESS;
}

static
BOOLEAN
NtfsSlotWasVisited(PNTFS_INDEX Index,
                   ULONG Slot)
{
    ULONG i;

    for (i = 0; i < Index->SlotCount; i++)
    {
        if (Index->Slots[i] == Slot)
            return TRUE;
    }
    return FALSE;
}

/* Reads, fixes up and validates one index block. */
static
NTSTATUS
NtfsReadIndexBlock(PNTFS_INDEX Index,
                   ULONGLONG Vcn,
                   PINDEX_BUFFER *Block,
                   PULONG Slot)
{
    PDEVICE_EXTENSION Vcb = Index->Vcb;
    PINDEX_BUFFER Buffer;
    ULONGLONG Offset;
    NTSTATUS Status;

    if (!Index->AllocCtx)
        return STATUS_FILE_CORRUPT_ERROR;

    Status = NtfsIndexVcnToSlot(Index, Vcn, Slot);
    if (!NT_SUCCESS(Status))
        return Status;

    Offset = (ULONGLONG)*Slot * Index->BlockSize;
    if (Offset + Index->BlockSize > AttributeDataLength(Index->AllocCtx->pRecord))
        return STATUS_FILE_CORRUPT_ERROR;

    Buffer = ExAllocatePoolWithTag(NonPagedPool, Index->BlockSize, TAG_NTFS);
    if (!Buffer)
        return STATUS_INSUFFICIENT_RESOURCES;

    if (ReadAttribute(Vcb, Index->AllocCtx, Offset, (PCHAR)Buffer, Index->BlockSize) != Index->BlockSize)
    {
        ExFreePoolWithTag(Buffer, TAG_NTFS);
        return STATUS_UNEXPECTED_IO_ERROR;
    }

    if (Buffer->Ntfs.Type != NRH_INDX_TYPE ||
        Buffer->Ntfs.UsaCount != Index->BlockSize / Vcb->NtfsInfo.BytesPerSector + 1 ||
        Buffer->Ntfs.UsaOffset + Buffer->Ntfs.UsaCount * sizeof(USHORT) > Index->BlockSize)
    {
        ExFreePoolWithTag(Buffer, TAG_NTFS);
        return STATUS_FILE_CORRUPT_ERROR;
    }

    Status = FixupUpdateSequenceArray(Vcb, &Buffer->Ntfs);
    if (!NT_SUCCESS(Status))
    {
        ExFreePoolWithTag(Buffer, TAG_NTFS);
        return Status;
    }

    if (Buffer->VCN != Vcn ||
        NTFS_INDEX_BUFFER_HEADER + Buffer->Header.AllocatedSize > Index->BlockSize ||
        Buffer->Header.TotalSizeOfEntries > Buffer->Header.AllocatedSize ||
        Buffer->Header.FirstEntryOffset < sizeof(INDEX_HEADER_ATTRIBUTE) ||
        Buffer->Header.FirstEntryOffset + NTFS_INDEX_ENTRY_HEADER > Buffer->Header.TotalSizeOfEntries)
    {
        ExFreePoolWithTag(Buffer, TAG_NTFS);
        return STATUS_FILE_CORRUPT_ERROR;
    }

    *Block = Buffer;
    return STATUS_SUCCESS;
}

/* Writes a block built or modified in memory; the buffer can't be reused afterwards. */
static
NTSTATUS
NtfsWriteIndexBlock(PNTFS_INDEX Index,
                    ULONG Slot,
                    PINDEX_BUFFER Buffer)
{
    ULONG Written;
    NTSTATUS Status;

    Index->DiskTouched = TRUE;
    Status = AddFixupArray(Index->Vcb, &Buffer->Ntfs);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = WriteAttribute(Index->Vcb, Index->AllocCtx,
                            (ULONGLONG)Slot * Index->BlockSize,
                            (PUCHAR)Buffer, Index->BlockSize,
                            &Written, Index->Record);
    if (NT_SUCCESS(Status) && Written != Index->BlockSize)
        Status = STATUS_END_OF_FILE;

    return Status;
}

static
NTSTATUS
NtfsWalkBlock(PNTFS_INDEX Index,
              ULONGLONG Vcn,
              ULONG Depth,
              PNTFS_INDEX_VISITOR Visitor,
              PVOID Context,
              PBOOLEAN Stop);

/* Visits the entries of one node in order, descending into child nodes first. */
static
NTSTATUS
NtfsWalkEntries(PNTFS_INDEX Index,
                PINDEX_HEADER_ATTRIBUTE Header,
                ULONG Depth,
                PNTFS_INDEX_VISITOR Visitor,
                PVOID Context,
                PBOOLEAN Modified,
                PBOOLEAN Stop)
{
    ULONG Offset = Header->FirstEntryOffset;
    ULONG End = Header->TotalSizeOfEntries;
    NTSTATUS Status;

    for (;;)
    {
        PINDEX_ENTRY_ATTRIBUTE Entry;
        ULONG KeyRoom;
        BOOLEAN EntryModified = FALSE;

        if (Offset + NTFS_INDEX_ENTRY_HEADER > End)
            return STATUS_FILE_CORRUPT_ERROR;

        Entry = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)Header + Offset);
        if (Entry->Length < NTFS_INDEX_ENTRY_HEADER ||
            (Entry->Length % 8) != 0 ||
            Entry->Length > End - Offset)
        {
            return STATUS_FILE_CORRUPT_ERROR;
        }

        KeyRoom = Entry->Length - NTFS_INDEX_ENTRY_HEADER;
        if (Entry->Flags & NTFS_INDEX_ENTRY_NODE)
        {
            if (KeyRoom < sizeof(ULONGLONG))
                return STATUS_FILE_CORRUPT_ERROR;
            KeyRoom -= sizeof(ULONGLONG);

            Status = NtfsWalkBlock(Index,
                                   *(PULONGLONG)((PUCHAR)Entry + Entry->Length - sizeof(ULONGLONG)),
                                   Depth + 1,
                                   Visitor,
                                   Context,
                                   Stop);
            if (!NT_SUCCESS(Status) || *Stop)
                return Status;
        }

        if (Entry->Flags & NTFS_INDEX_ENTRY_END)
            return STATUS_SUCCESS;

        if (Entry->KeyLength < FIELD_OFFSET(FILENAME_ATTRIBUTE, Name) ||
            Entry->KeyLength > KeyRoom ||
            NtfsIndexEntryNameLength(Entry) > Entry->KeyLength)
        {
            return STATUS_FILE_CORRUPT_ERROR;
        }

        Status = Visitor(Context, Entry, &EntryModified, Stop);
        if (EntryModified)
            *Modified = TRUE;
        if (!NT_SUCCESS(Status) || *Stop)
            return Status;

        Offset += Entry->Length;
    }
}

static
NTSTATUS
NtfsWalkBlock(PNTFS_INDEX Index,
              ULONGLONG Vcn,
              ULONG Depth,
              PNTFS_INDEX_VISITOR Visitor,
              PVOID Context,
              PBOOLEAN Stop)
{
    PINDEX_BUFFER Block;
    ULONG Slot;
    BOOLEAN Modified = FALSE;
    NTSTATUS Status;

    if (Depth > NTFS_INDEX_MAX_DEPTH)
        return STATUS_FILE_CORRUPT_ERROR;

    Status = NtfsReadIndexBlock(Index, Vcn, &Block, &Slot);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtfsRememberSlot(Index, Slot);
    if (NT_SUCCESS(Status))
    {
        Status = NtfsWalkEntries(Index, &Block->Header, Depth, Visitor, Context, &Modified, Stop);
        if (NT_SUCCESS(Status) && Modified)
            Status = NtfsWriteIndexBlock(Index, Slot, Block);
    }

    ExFreePoolWithTag(Block, TAG_NTFS);
    return Status;
}

/* Visits every entry of the index in collation order. Entries a visitor marks
   as modified are written back (value changes only, never lengths). */
static
NTSTATUS
NtfsWalkIndex(PNTFS_INDEX Index,
              PNTFS_INDEX_VISITOR Visitor,
              PVOID Context)
{
    BOOLEAN Modified = FALSE;
    BOOLEAN Stop = FALSE;
    NTSTATUS Status;

    Index->SlotCount = 0;

    Status = NtfsWalkEntries(Index, &Index->Root->Header, 0, Visitor, Context, &Modified, &Stop);
    if (NT_SUCCESS(Status) && Modified)
    {
        PNTFS_ATTR_RECORD RootAttribute = (PNTFS_ATTR_RECORD)((PUCHAR)Index->Record + Index->RootOffset);

        RtlCopyMemory((PUCHAR)RootAttribute + RootAttribute->Resident.ValueOffset,
                      Index->Root,
                      Index->RootLength);
        Status = UpdateFileRecord(Index->Vcb, Index->MftIndex, Index->Record);
    }

    return Status;
}

/* LOOKUP AND IN-PLACE UPDATES **********************************************/

typedef struct _NTFS_LOOKUP_CONTEXT
{
    PUNICODE_STRING FileName;
    PULONG StartEntry;
    ULONG CurrentEntry;
    BOOLEAN DirSearch;
    BOOLEAN CaseSensitive;
    BOOLEAN Found;
    ULONGLONG MftIndex;
} NTFS_LOOKUP_CONTEXT, *PNTFS_LOOKUP_CONTEXT;

static
NTSTATUS
NtfsLookupVisitor(PVOID Context,
                  PINDEX_ENTRY_ATTRIBUTE Entry,
                  PBOOLEAN Modified,
                  PBOOLEAN Stop)
{
    PNTFS_LOOKUP_CONTEXT Lookup = Context;

    UNREFERENCED_PARAMETER(Modified);

    if ((Entry->Data.Directory.IndexedFile & NTFS_MFT_MASK) >= NTFS_FILE_FIRST_USER_FILE &&
        Lookup->CurrentEntry >= *Lookup->StartEntry &&
        Entry->FileName.NameType != NTFS_FILE_NAME_DOS &&
        CompareFileName(Lookup->FileName, Entry, Lookup->DirSearch, Lookup->CaseSensitive))
    {
        *Lookup->StartEntry = Lookup->CurrentEntry;
        Lookup->MftIndex = Entry->Data.Directory.IndexedFile & NTFS_MFT_MASK;
        Lookup->Found = TRUE;
        *Stop = TRUE;
        return STATUS_SUCCESS;
    }

    Lookup->CurrentEntry++;
    return STATUS_SUCCESS;
}

/**
* Finds the FirstEntry'th (or later) name in directory DirectoryMftIndex that
* matches FileName (a pattern when DirSearch is TRUE). On success, FirstEntry
* receives the position of the match.
*/
NTSTATUS
NtfsIndexLookup(PDEVICE_EXTENSION Vcb,
                ULONGLONG DirectoryMftIndex,
                PUNICODE_STRING FileName,
                PULONG FirstEntry,
                BOOLEAN DirSearch,
                BOOLEAN CaseSensitive,
                PULONGLONG MftIndex)
{
    NTFS_INDEX Index;
    NTFS_LOOKUP_CONTEXT Lookup;
    NTSTATUS Status;

    Status = NtfsOpenIndex(Vcb, DirectoryMftIndex, &Index);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlZeroMemory(&Lookup, sizeof(Lookup));
    Lookup.FileName = FileName;
    Lookup.StartEntry = FirstEntry;
    Lookup.DirSearch = DirSearch;
    Lookup.CaseSensitive = CaseSensitive;

    Status = NtfsWalkIndex(&Index, NtfsLookupVisitor, &Lookup);
    NtfsCloseIndex(&Index);

    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Walking the index of directory %I64u failed (0x%08lx)\n", DirectoryMftIndex, Status);
        return Status;
    }

    if (!Lookup.Found)
        return STATUS_OBJECT_PATH_NOT_FOUND;

    *MftIndex = Lookup.MftIndex;
    return STATUS_SUCCESS;
}

typedef struct _NTFS_SIZE_UPDATE_CONTEXT
{
    PUNICODE_STRING FileName;
    BOOLEAN DirSearch;
    BOOLEAN CaseSensitive;
    BOOLEAN Found;
    ULONGLONG DataSize;
    ULONGLONG AllocatedSize;
} NTFS_SIZE_UPDATE_CONTEXT, *PNTFS_SIZE_UPDATE_CONTEXT;

static
NTSTATUS
NtfsSizeUpdateVisitor(PVOID Context,
                      PINDEX_ENTRY_ATTRIBUTE Entry,
                      PBOOLEAN Modified,
                      PBOOLEAN Stop)
{
    PNTFS_SIZE_UPDATE_CONTEXT Update = Context;

    if ((Entry->Data.Directory.IndexedFile & NTFS_MFT_MASK) >= NTFS_FILE_FIRST_USER_FILE &&
        Entry->FileName.NameType != NTFS_FILE_NAME_DOS &&
        CompareFileName(Update->FileName, Entry, Update->DirSearch, Update->CaseSensitive))
    {
        Entry->FileName.DataSize = Update->DataSize;
        Entry->FileName.AllocatedSize = Update->AllocatedSize;
        Update->Found = TRUE;
        *Modified = TRUE;
        *Stop = TRUE;
    }

    return STATUS_SUCCESS;
}

/**
* Updates the sizes cached in the index entry for FileName. The entry is
* rewritten in place, in the root or in the one index block that holds it.
*/
NTSTATUS
NtfsIndexUpdateSizes(PDEVICE_EXTENSION Vcb,
                     ULONGLONG DirectoryMftIndex,
                     PUNICODE_STRING FileName,
                     BOOLEAN DirSearch,
                     ULONGLONG DataSize,
                     ULONGLONG AllocatedSize,
                     BOOLEAN CaseSensitive)
{
    NTFS_INDEX Index;
    NTFS_SIZE_UPDATE_CONTEXT Update;
    NTSTATUS Status;

    Status = NtfsOpenIndex(Vcb, DirectoryMftIndex, &Index);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlZeroMemory(&Update, sizeof(Update));
    Update.FileName = FileName;
    Update.DirSearch = DirSearch;
    Update.CaseSensitive = CaseSensitive;
    Update.DataSize = DataSize;
    Update.AllocatedSize = AllocatedSize;

    Status = NtfsWalkIndex(&Index, NtfsSizeUpdateVisitor, &Update);
    NtfsCloseIndex(&Index);

    if (NT_SUCCESS(Status) && !Update.Found)
        Status = STATUS_OBJECT_PATH_NOT_FOUND;

    return Status;
}

/* REWRITING ****************************************************************/

static
VOID
NtfsFreeEntryList(PNTFS_ENTRY_LIST List)
{
    ULONG i;

    for (i = 0; i < List->Count; i++)
        ExFreePoolWithTag(List->Entries[i], TAG_NTFS);
    if (List->Entries)
        ExFreePoolWithTag(List->Entries, TAG_NTFS);
    RtlZeroMemory(List, sizeof(*List));
}

static
NTSTATUS
NtfsReserveEntries(PNTFS_ENTRY_LIST List,
                   ULONG Needed)
{
    PINDEX_ENTRY_ATTRIBUTE *NewEntries;
    ULONG NewCapacity;

    if (Needed <= List->Capacity)
        return STATUS_SUCCESS;

    NewCapacity = max(32, max(Needed, List->Capacity * 2));
    NewEntries = ExAllocatePoolWithTag(NonPagedPool, NewCapacity * sizeof(PINDEX_ENTRY_ATTRIBUTE), TAG_NTFS);
    if (!NewEntries)
        return STATUS_INSUFFICIENT_RESOURCES;

    if (List->Entries)
    {
        RtlCopyMemory(NewEntries, List->Entries, List->Count * sizeof(PINDEX_ENTRY_ATTRIBUTE));
        ExFreePoolWithTag(List->Entries, TAG_NTFS);
    }
    List->Entries = NewEntries;
    List->Capacity = NewCapacity;
    return STATUS_SUCCESS;
}

/* Makes a leaf copy of an index entry: no child pointer, no flags. */
static
PINDEX_ENTRY_ATTRIBUTE
NtfsCopyLeafEntry(PINDEX_ENTRY_ATTRIBUTE Source)
{
    ULONG Length = ALIGN_UP_BY(NTFS_INDEX_ENTRY_HEADER + Source->KeyLength, 8);
    PINDEX_ENTRY_ATTRIBUTE Copy = ExAllocatePoolWithTag(NonPagedPool, Length, TAG_NTFS);

    if (!Copy)
        return NULL;

    RtlZeroMemory(Copy, Length);
    RtlCopyMemory(Copy, Source, NTFS_INDEX_ENTRY_HEADER + Source->KeyLength);
    Copy->Length = (USHORT)Length;
    Copy->Flags = 0;
    Copy->Reserved = 0;
    return Copy;
}

static
NTSTATUS
NtfsCollectVisitor(PVOID Context,
                   PINDEX_ENTRY_ATTRIBUTE Entry,
                   PBOOLEAN Modified,
                   PBOOLEAN Stop)
{
    PNTFS_ENTRY_LIST List = Context;
    PINDEX_ENTRY_ATTRIBUTE Copy;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(Modified);
    UNREFERENCED_PARAMETER(Stop);

    Status = NtfsReserveEntries(List, List->Count + 1);
    if (!NT_SUCCESS(Status))
        return Status;

    Copy = NtfsCopyLeafEntry(Entry);
    if (!Copy)
        return STATUS_INSUFFICIENT_RESOURCES;

    List->Entries[List->Count++] = Copy;
    return STATUS_SUCCESS;
}

static
LONG
NtfsCollateEntries(PINDEX_ENTRY_ATTRIBUTE Entry1,
                   PINDEX_ENTRY_ATTRIBUTE Entry2,
                   BOOLEAN CaseSensitive)
{
    UNICODE_STRING Name1, Name2;
    LONG Result;

    Name1.Buffer = Entry1->FileName.Name;
    Name1.Length = Name1.MaximumLength = Entry1->FileName.NameLength * sizeof(WCHAR);
    Name2.Buffer = Entry2->FileName.Name;
    Name2.Length = Name2.MaximumLength = Entry2->FileName.NameLength * sizeof(WCHAR);

    /* $I30 collation: upper-cased names, ties broken case-sensitively. */
    Result = RtlCompareUnicodeString(&Name1, &Name2, TRUE);
    if (Result == 0 && CaseSensitive)
        Result = RtlCompareUnicodeString(&Name1, &Name2, FALSE);
    return Result;
}

static
NTSTATUS
NtfsBuilderAddNode(PNTFS_TREE_BUILDER Builder,
                   PINDEX_BUFFER Buffer,
                   PULONGLONG Vcn)
{
    PNTFS_INDEX Index = Builder->Index;
    ULONG Slot = Builder->NextSlot;

    /* Use a slot that neither the current tree nor its bitmap claims. */
    while (NtfsSlotWasVisited(Index, Slot) ||
           (Slot < Builder->OldBitmapBits && (Builder->OldBitmap[Slot / 8] & (1 << (Slot % 8)))))
    {
        Slot++;
    }
    Builder->NextSlot = Slot + 1;

    if (Builder->NodeCount == Builder->NodeCapacity)
    {
        ULONG NewCapacity = max(16, Builder->NodeCapacity * 2);
        PNTFS_NEW_NODE NewNodes = ExAllocatePoolWithTag(NonPagedPool, NewCapacity * sizeof(NTFS_NEW_NODE), TAG_NTFS);

        if (!NewNodes)
            return STATUS_INSUFFICIENT_RESOURCES;
        if (Builder->Nodes)
        {
            RtlCopyMemory(NewNodes, Builder->Nodes, Builder->NodeCount * sizeof(NTFS_NEW_NODE));
            ExFreePoolWithTag(Builder->Nodes, TAG_NTFS);
        }
        Builder->Nodes = NewNodes;
        Builder->NodeCapacity = NewCapacity;
    }

    *Vcn = NtfsIndexSlotToVcn(Index, Slot);
    Buffer->VCN = *Vcn;
    Builder->Nodes[Builder->NodeCount].Slot = Slot;
    Builder->Nodes[Builder->NodeCount].Buffer = Buffer;
    Builder->NodeCount++;
    return STATUS_SUCCESS;
}

static
PINDEX_BUFFER
NtfsBuilderNewBuffer(PNTFS_TREE_BUILDER Builder,
                     BOOLEAN HasChildren)
{
    PNTFS_INDEX Index = Builder->Index;
    PINDEX_BUFFER Buffer = ExAllocatePoolWithTag(NonPagedPool, Index->BlockSize, TAG_NTFS);

    if (!Buffer)
        return NULL;

    RtlZeroMemory(Buffer, Index->BlockSize);
    Buffer->Ntfs.Type = NRH_INDX_TYPE;
    Buffer->Ntfs.UsaOffset = NTFS_INDEX_BUFFER_HEADER + sizeof(INDEX_HEADER_ATTRIBUTE);
    Buffer->Ntfs.UsaCount = (USHORT)(Index->BlockSize / Index->Vcb->NtfsInfo.BytesPerSector + 1);
    Buffer->Header.FirstEntryOffset = Builder->FirstEntryOffset;
    Buffer->Header.TotalSizeOfEntries = Builder->FirstEntryOffset;
    Buffer->Header.AllocatedSize = Builder->AllocatedSize;
    Buffer->Header.Flags = HasChildren ? INDEX_NODE_LARGE : 0;
    return Buffer;
}

/* Appends an entry (with a child pointer when HasChild) to a node or root being built. */
static
VOID
NtfsAppendNodeEntry(PINDEX_HEADER_ATTRIBUTE Header,
                    PINDEX_ENTRY_ATTRIBUTE Entry,
                    BOOLEAN HasChild,
                    ULONGLONG ChildVcn)
{
    PINDEX_ENTRY_ATTRIBUTE Destination = (PINDEX_ENTRY_ATTRIBUTE)((PUCHAR)Header + Header->TotalSizeOfEntries);

    if (Entry)
    {
        RtlCopyMemory(Destination, Entry, Entry->Length);
    }
    else
    {
        RtlZeroMemory(Destination, NTFS_INDEX_ENTRY_HEADER);
        Destination->Length = NTFS_INDEX_ENTRY_HEADER;
        Destination->Flags = NTFS_INDEX_ENTRY_END;
    }

    if (HasChild)
    {
        *(PULONGLONG)((PUCHAR)Destination + Destination->Length) = ChildVcn;
        Destination->Length += sizeof(ULONGLONG);
        Destination->Flags |= NTFS_INDEX_ENTRY_NODE;
    }

    Header->TotalSizeOfEntries += Destination->Length;
}

/**
* Packs the ordered entries into index blocks, bottom level first, until one
* block holds the top level. Each level's overflowing entry moves up as the
* separator between two nodes.
*/
static
NTSTATUS
NtfsBuildTree(PNTFS_TREE_BUILDER Builder,
              PNTFS_ENTRY_LIST List,
              PULONGLONG TopVcn)
{
    PNTFS_LEVEL_ITEM Items, NextItems = NULL;
    ULONG ItemCount = List->Count;
    ULONGLONG RightChild = 0;
    ULONG Level;
    ULONG i;
    NTSTATUS Status = STATUS_SUCCESS;

    Items = ExAllocatePoolWithTag(NonPagedPool, (ItemCount + 1) * sizeof(NTFS_LEVEL_ITEM), TAG_NTFS);
    if (!Items)
        return STATUS_INSUFFICIENT_RESOURCES;

    for (i = 0; i < ItemCount; i++)
    {
        Items[i].Entry = List->Entries[i];
        Items[i].ChildVcn = 0;
    }

    for (Level = 0; Level < NTFS_INDEX_MAX_DEPTH; Level++)
    {
        BOOLEAN HasChildren = (Level > 0);
        ULONG ChildLength = HasChildren ? sizeof(ULONGLONG) : 0;
        ULONG EndLength = NTFS_INDEX_ENTRY_HEADER + ChildLength;
        ULONG NextCount = 0;
        ULONG NodesAtLevel = 0;
        ULONG KeysInNode = 0;
        ULONGLONG Vcn;
        PINDEX_BUFFER Node;

        NextItems = ExAllocatePoolWithTag(NonPagedPool, (ItemCount + 1) * sizeof(NTFS_LEVEL_ITEM), TAG_NTFS);
        Node = NtfsBuilderNewBuffer(Builder, HasChildren);
        if (!NextItems || !Node)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            if (Node)
                ExFreePoolWithTag(Node, TAG_NTFS);
            break;
        }

        for (i = 0; i < ItemCount; i++)
        {
            ULONG Length = Items[i].Entry->Length + ChildLength;

            if (Node->Header.TotalSizeOfEntries + Length + EndLength <= Node->Header.AllocatedSize)
            {
                NtfsAppendNodeEntry(&Node->Header, Items[i].Entry, HasChildren, Items[i].ChildVcn);
                KeysInNode++;
                continue;
            }

            if (KeysInNode == 0)
            {
                DPRINT1("Index entry of %lu bytes does not fit in a %lu-byte index block\n",
                        Length, Builder->Index->BlockSize);
                Status = STATUS_NOT_IMPLEMENTED;
                break;
            }

            /* Close this node; the entry that didn't fit separates it from the next one. */
            NtfsAppendNodeEntry(&Node->Header, NULL, HasChildren, Items[i].ChildVcn);
            Status = NtfsBuilderAddNode(Builder, Node, &Vcn);
            if (!NT_SUCCESS(Status))
                break;
            NodesAtLevel++;

            NextItems[NextCount].Entry = Items[i].Entry;
            NextItems[NextCount].ChildVcn = Vcn;
            NextCount++;

            KeysInNode = 0;
            Node = NtfsBuilderNewBuffer(Builder, HasChildren);
            if (!Node)
            {
                Status = STATUS_INSUFFICIENT_RESOURCES;
                break;
            }
        }

        if (!NT_SUCCESS(Status))
        {
            /* A node that made it into Builder->Nodes has already been replaced. */
            if (Node)
                ExFreePoolWithTag(Node, TAG_NTFS);
            break;
        }

        NtfsAppendNodeEntry(&Node->Header, NULL, HasChildren, RightChild);
        Status = NtfsBuilderAddNode(Builder, Node, &Vcn);
        if (!NT_SUCCESS(Status))
        {
            ExFreePoolWithTag(Node, TAG_NTFS);
            break;
        }
        NodesAtLevel++;

        ExFreePoolWithTag(Items, TAG_NTFS);
        Items = NextItems;
        NextItems = NULL;
        ItemCount = NextCount;
        RightChild = Vcn;

        if (NodesAtLevel == 1)
        {
            ASSERT(ItemCount == 0);
            *TopVcn = Vcn;
            break;
        }
    }

    if (NT_SUCCESS(Status) && Level == NTFS_INDEX_MAX_DEPTH)
        Status = STATUS_NOT_IMPLEMENTED;

    if (NextItems)
        ExFreePoolWithTag(NextItems, TAG_NTFS);
    ExFreePoolWithTag(Items, TAG_NTFS);
    return Status;
}

/* Replaces the root value in the in-memory record (resizing the attribute). */
static
NTSTATUS
NtfsSetRootValue(PNTFS_INDEX Index,
                 PINDEX_ROOT_ATTRIBUTE NewRoot,
                 ULONG NewLength)
{
    PNTFS_ATTR_RECORD RootAttribute = (PNTFS_ATTR_RECORD)((PUCHAR)Index->Record + Index->RootOffset);
    ULONG NewAttributeLength = ALIGN_UP_BY(RootAttribute->Resident.ValueOffset + NewLength, ATTR_RECORD_ALIGNMENT);
    NTSTATUS Status;

    if (Index->Record->BytesInUse - RootAttribute->Length + NewAttributeLength >
        Index->Vcb->NtfsInfo.BytesPerFileRecord)
    {
        return STATUS_NOT_IMPLEMENTED;
    }

    if (NewLength != RootAttribute->Resident.ValueLength)
    {
        Status = InternalSetResidentAttributeLength(Index->Vcb, Index->RootCtx, Index->Record,
                                                    Index->RootOffset, NewLength);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    RootAttribute = (PNTFS_ATTR_RECORD)((PUCHAR)Index->Record + Index->RootOffset);
    RtlCopyMemory((PUCHAR)RootAttribute + RootAttribute->Resident.ValueOffset, NewRoot, NewLength);
    return NtfsLoadIndexRoot(Index);
}

static
PINDEX_ROOT_ATTRIBUTE
NtfsNewRootValue(PNTFS_INDEX Index,
                 ULONG EntriesLength,
                 BOOLEAN Large)
{
    ULONG Length = NTFS_INDEX_ROOT_HEADER + sizeof(INDEX_HEADER_ATTRIBUTE) + EntriesLength;
    PINDEX_ROOT_ATTRIBUTE Root = ExAllocatePoolWithTag(NonPagedPool, Length, TAG_NTFS);

    if (!Root)
        return NULL;

    RtlZeroMemory(Root, Length);
    Root->AttributeType = Index->Root->AttributeType;
    Root->CollationRule = Index->Root->CollationRule;
    Root->SizeOfEntry = Index->Root->SizeOfEntry;
    Root->ClustersPerIndexRecord = Index->Root->ClustersPerIndexRecord;
    Root->Header.FirstEntryOffset = sizeof(INDEX_HEADER_ATTRIBUTE);
    Root->Header.TotalSizeOfEntries = sizeof(INDEX_HEADER_ATTRIBUTE);
    Root->Header.AllocatedSize = sizeof(INDEX_HEADER_ATTRIBUTE) + EntriesLength;
    Root->Header.Flags = Large ? INDEX_ROOT_LARGE : INDEX_ROOT_SMALL;
    return Root;
}

/* Grows $INDEX_ALLOCATION in the in-memory record without publishing it. */
static
NTSTATUS
NtfsGrowIndexAllocation(PNTFS_INDEX Index,
                        ULONG AllocOffset,
                        ULONGLONG NewDataSize)
{
    PDEVICE_EXTENSION Vcb = Index->Vcb;
    PNTFS_ATTR_CONTEXT AllocCtx = Index->AllocCtx;
    ULONG ClusterSize = Vcb->NtfsInfo.BytesPerCluster;
    ULONGLONG Allocated = AllocCtx->pRecord->NonResident.AllocatedSize;
    ULONGLONG Needed = ROUND_UP(NewDataSize, ClusterSize);
    PNTFS_ATTR_RECORD Destination;

    while (Allocated < Needed)
    {
        LONGLONG LastVbn, LastLcn;
        ULONG FirstCluster, Count;
        ULONG Hint = 0;
        NTSTATUS Status;

        if (Allocated != 0 &&
            FsRtlLookupLastLargeMcbEntry(&AllocCtx->DataRunsMCB, &LastVbn, &LastLcn) &&
            LastLcn >= 0)
        {
            Hint = (ULONG)LastLcn + 1;
        }

        Index->DiskTouched = TRUE;
        Status = NtfsAllocateClusters(Vcb, Hint, (ULONG)((Needed - Allocated) / ClusterSize),
                                      &FirstCluster, &Count);
        if (!NT_SUCCESS(Status))
            return Status;

        Status = AddRunEx(Vcb, AllocCtx, AllocOffset, Index->Record, FirstCluster, Count, FALSE);
        if (!NT_SUCCESS(Status))
            return Status;

        Allocated += (ULONGLONG)Count * ClusterSize;
        Destination = (PNTFS_ATTR_RECORD)((PUCHAR)Index->Record + AllocOffset);
        Destination->NonResident.AllocatedSize = AllocCtx->pRecord->NonResident.AllocatedSize = Allocated;
        /* AddRun() can't lower HighestVCN from the -1 a new attribute starts with. */
        Destination->NonResident.HighestVCN = AllocCtx->pRecord->NonResident.HighestVCN = Allocated / ClusterSize - 1;
    }

    Destination = (PNTFS_ATTR_RECORD)((PUCHAR)Index->Record + AllocOffset);
    if ((ULONGLONG)Destination->NonResident.DataSize < NewDataSize)
    {
        Destination->NonResident.DataSize = AllocCtx->pRecord->NonResident.DataSize = NewDataSize;
        Destination->NonResident.InitializedSize = AllocCtx->pRecord->NonResident.InitializedSize = NewDataSize;
    }

    return STATUS_SUCCESS;
}

/* Publishes the tree built by Builder: blocks, bitmap, then the root in the directory record. */
static
NTSTATUS
NtfsCommitLargeIndex(PNTFS_INDEX Index,
                     PNTFS_TREE_BUILDER Builder,
                     ULONGLONG TopVcn)
{
    PDEVICE_EXTENSION Vcb = Index->Vcb;
    PINDEX_ROOT_ATTRIBUTE NewRoot;
    PNTFS_ATTR_CONTEXT BitmapCtx = NULL;
    ULONG AllocOffset, BitmapOffset;
    ULONG MaxSlot = 0;
    ULONG BitmapLength;
    PUCHAR Bitmap = NULL;
    ULONGLONG DataSize;
    ULONG i;
    NTSTATUS Status;

    for (i = 0; i < Builder->NodeCount; i++)
        MaxSlot = max(MaxSlot, Builder->Nodes[i].Slot);

    /* The root becomes a single end entry pointing at the top block. */
    NewRoot = NtfsNewRootValue(Index, NTFS_INDEX_ENTRY_HEADER + sizeof(ULONGLONG), TRUE);
    if (!NewRoot)
        return STATUS_INSUFFICIENT_RESOURCES;
    NtfsAppendNodeEntry(&NewRoot->Header, NULL, TRUE, TopVcn);

    Status = NtfsSetRootValue(Index, NewRoot,
                              NTFS_INDEX_ROOT_HEADER + NewRoot->Header.TotalSizeOfEntries);
    ExFreePoolWithTag(NewRoot, TAG_NTFS);
    if (!NT_SUCCESS(Status))
        return Status;

    if (!Index->AllocCtx)
    {
        PNTFS_ATTR_RECORD RootAttribute = (PNTFS_ATTR_RECORD)((PUCHAR)Index->Record + Index->RootOffset);
        PNTFS_ATTR_RECORD EndMarker = (PNTFS_ATTR_RECORD)((PUCHAR)RootAttribute + RootAttribute->Length);

        /* Attributes are kept sorted by type; $I30 allocation and bitmap go right after the root. */
        if (EndMarker->Type != AttributeEnd)
        {
            DPRINT1("Directory %I64u has attributes after $INDEX_ROOT; can't add an index allocation\n", Index->MftIndex);
            return STATUS_NOT_IMPLEMENTED;
        }

        Status = AddIndexAllocation(Vcb, Index->Record, EndMarker, L"$I30", 4);
        if (!NT_SUCCESS(Status))
            return Status;

        EndMarker = (PNTFS_ATTR_RECORD)((PUCHAR)EndMarker + EndMarker->Length);
        Status = AddBitmap(Vcb, Index->Record, EndMarker, L"$I30", 4);
        if (!NT_SUCCESS(Status))
            return Status;
    }
    else
    {
        ReleaseAttributeContext(Index->AllocCtx);
        Index->AllocCtx = NULL;
    }

    /* Re-open the allocation from the in-memory record: offsets moved with the root. */
    Status = FindAttribute(Vcb, Index->Record, AttributeIndexAllocation, L"$I30", 4,
                           &Index->AllocCtx, &AllocOffset);
    if (!NT_SUCCESS(Status))
    {
        Index->AllocCtx = NULL;
        return Status;
    }

    DataSize = max((ULONGLONG)(MaxSlot + 1) * Index->BlockSize,
                   AttributeDataLength(Index->AllocCtx->pRecord));
    Status = NtfsGrowIndexAllocation(Index, AllocOffset, DataSize);
    if (!NT_SUCCESS(Status))
        return Status;

    for (i = 0; i < Builder->NodeCount; i++)
    {
        Status = NtfsWriteIndexBlock(Index, Builder->Nodes[i].Slot, Builder->Nodes[i].Buffer);
        if (!NT_SUCCESS(Status))
            return Status;
    }

    /* The bitmap names exactly the blocks of the new tree. */
    Status = FindAttribute(Vcb, Index->Record, AttributeBitmap, L"$I30", 4, &BitmapCtx, &BitmapOffset);
    if (!NT_SUCCESS(Status))
        return STATUS_FILE_CORRUPT_ERROR;

    BitmapLength = (ULONG)ALIGN_UP_BY((DataSize / Index->BlockSize + 7) / 8, 8);
    BitmapLength = max(BitmapLength, (ULONG)AttributeDataLength(BitmapCtx->pRecord));
    Bitmap = ExAllocatePoolWithTag(NonPagedPool, BitmapLength, TAG_NTFS);
    if (!Bitmap)
    {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Cleanup;
    }
    RtlZeroMemory(Bitmap, BitmapLength);
    for (i = 0; i < Builder->NodeCount; i++)
        Bitmap[Builder->Nodes[i].Slot / 8] |= (UCHAR)(1 << (Builder->Nodes[i].Slot % 8));

    if (!BitmapCtx->pRecord->IsNonResident)
    {
        PNTFS_ATTR_RECORD BitmapAttribute = (PNTFS_ATTR_RECORD)((PUCHAR)Index->Record + BitmapOffset);
        ULONG NewAttributeLength = ALIGN_UP_BY(BitmapAttribute->Resident.ValueOffset + BitmapLength,
                                               ATTR_RECORD_ALIGNMENT);

        if (Index->Record->BytesInUse - BitmapAttribute->Length + NewAttributeLength >
            Vcb->NtfsInfo.BytesPerFileRecord)
        {
            DPRINT1("FIXME: $I30 bitmap of directory %I64u would need to become non-resident\n", Index->MftIndex);
            Status = STATUS_NOT_IMPLEMENTED;
            goto Cleanup;
        }

        if (BitmapLength != BitmapAttribute->Resident.ValueLength)
        {
            Status = InternalSetResidentAttributeLength(Vcb, BitmapCtx, Index->Record, BitmapOffset, BitmapLength);
            if (!NT_SUCCESS(Status))
                goto Cleanup;
            BitmapAttribute = (PNTFS_ATTR_RECORD)((PUCHAR)Index->Record + BitmapOffset);
        }

        RtlCopyMemory((PUCHAR)BitmapAttribute + BitmapAttribute->Resident.ValueOffset, Bitmap, BitmapLength);
    }

    /* Commit point: one write of the directory record switches to the new tree. */
    Index->DiskTouched = TRUE;
    Status = UpdateFileRecord(Vcb, Index->MftIndex, Index->Record);
    if (!NT_SUCCESS(Status))
        goto Cleanup;

    if (BitmapCtx->pRecord->IsNonResident)
    {
        ULONG Written;

        if (BitmapLength > AttributeDataLength(BitmapCtx->pRecord))
        {
            LARGE_INTEGER NewSize;

            NewSize.QuadPart = BitmapLength;
            Status = SetNonResidentAttributeDataLength(Vcb, BitmapCtx, BitmapOffset, Index->Record, &NewSize);
            if (NT_SUCCESS(Status))
                Status = UpdateFileRecord(Vcb, Index->MftIndex, Index->Record);
            if (!NT_SUCCESS(Status))
                goto Cleanup;
        }

        Status = WriteAttribute(Vcb, BitmapCtx, 0, Bitmap, BitmapLength, &Written, Index->Record);
        if (NT_SUCCESS(Status) && Written != BitmapLength)
            Status = STATUS_END_OF_FILE;
    }

Cleanup:
    if (Bitmap)
        ExFreePoolWithTag(Bitmap, TAG_NTFS);
    if (BitmapCtx)
        ReleaseAttributeContext(BitmapCtx);
    return Status;
}

/* Reads the $I30 bitmap so the builder can avoid blocks it marks in use. */
static
NTSTATUS
NtfsReadIndexBitmap(PNTFS_INDEX Index,
                    PUCHAR *Bitmap,
                    PULONG Bits)
{
    PNTFS_ATTR_CONTEXT BitmapCtx;
    ULONGLONG Length;
    PUCHAR Data;
    NTSTATUS Status;

    *Bitmap = NULL;
    *Bits = 0;

    if (!Index->AllocCtx)
        return STATUS_SUCCESS;

    Status = FindAttribute(Index->Vcb, Index->Record, AttributeBitmap, L"$I30", 4, &BitmapCtx, NULL);
    if (!NT_SUCCESS(Status))
        return STATUS_FILE_CORRUPT_ERROR;

    Length = AttributeDataLength(BitmapCtx->pRecord);
    if (Length == 0 || Length > MAXULONG / 8)
    {
        ReleaseAttributeContext(BitmapCtx);
        return (Length == 0) ? STATUS_SUCCESS : STATUS_FILE_CORRUPT_ERROR;
    }

    Data = ExAllocatePoolWithTag(NonPagedPool, (ULONG)Length, TAG_NTFS);
    if (!Data)
    {
        ReleaseAttributeContext(BitmapCtx);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (ReadAttribute(Index->Vcb, BitmapCtx, 0, (PCHAR)Data, (ULONG)Length) != Length)
    {
        ExFreePoolWithTag(Data, TAG_NTFS);
        ReleaseAttributeContext(BitmapCtx);
        return STATUS_UNEXPECTED_IO_ERROR;
    }

    ReleaseAttributeContext(BitmapCtx);
    *Bitmap = Data;
    *Bits = (ULONG)Length * 8;
    return STATUS_SUCCESS;
}

/* Writes List as the directory's complete index. The caller has prepared the journal. */
static
NTSTATUS
NtfsRewriteIndex(PNTFS_INDEX Index,
                 PNTFS_ENTRY_LIST List)
{
    PDEVICE_EXTENSION Vcb = Index->Vcb;
    PNTFS_ATTR_RECORD RootAttribute = (PNTFS_ATTR_RECORD)((PUCHAR)Index->Record + Index->RootOffset);
    NTFS_TREE_BUILDER Builder;
    PNTFS_ATTR_CONTEXT ListCtx;
    ULONG EntriesLength = 0;
    ULONG SmallLength;
    ULONGLONG TopVcn = 0;
    ULONG FirstEntry;
    ULONG i;
    NTSTATUS Status;

    if (NT_SUCCESS(FindAttribute(Vcb, Index->Record, AttributeAttributeList, L"", 0, &ListCtx, NULL)))
    {
        ReleaseAttributeContext(ListCtx);
        DPRINT1("FIXME: directory %I64u has an attribute list\n", Index->MftIndex);
        return STATUS_NOT_IMPLEMENTED;
    }

    for (i = 0; i < List->Count; i++)
        EntriesLength += List->Entries[i]->Length;

    /* A directory without an index allocation keeps everything in the root while it fits. */
    SmallLength = NTFS_INDEX_ROOT_HEADER + sizeof(INDEX_HEADER_ATTRIBUTE) + EntriesLength + NTFS_INDEX_ENTRY_HEADER;
    if (!Index->AllocCtx &&
        Index->Record->BytesInUse - RootAttribute->Length +
            ALIGN_UP_BY(RootAttribute->Resident.ValueOffset + SmallLength, ATTR_RECORD_ALIGNMENT) <=
            Vcb->NtfsInfo.BytesPerFileRecord)
    {
        PINDEX_ROOT_ATTRIBUTE NewRoot = NtfsNewRootValue(Index, EntriesLength + NTFS_INDEX_ENTRY_HEADER, FALSE);

        if (!NewRoot)
            return STATUS_INSUFFICIENT_RESOURCES;

        for (i = 0; i < List->Count; i++)
            NtfsAppendNodeEntry(&NewRoot->Header, List->Entries[i], FALSE, 0);
        NtfsAppendNodeEntry(&NewRoot->Header, NULL, FALSE, 0);

        Status = NtfsSetRootValue(Index, NewRoot, SmallLength);
        ExFreePoolWithTag(NewRoot, TAG_NTFS);
        if (NT_SUCCESS(Status))
        {
            Index->DiskTouched = TRUE;
            Status = UpdateFileRecord(Vcb, Index->MftIndex, Index->Record);
        }
        return Status;
    }

    if (Index->BlockSize != Vcb->NtfsInfo.BytesPerIndexRecord)
    {
        DPRINT1("Directory %I64u uses %lu-byte index blocks, volume uses %lu\n",
                Index->MftIndex, Index->BlockSize, Vcb->NtfsInfo.BytesPerIndexRecord);
        return STATUS_NOT_IMPLEMENTED;
    }

    RtlZeroMemory(&Builder, sizeof(Builder));
    Builder.Index = Index;
    FirstEntry = ALIGN_UP_BY(NTFS_INDEX_BUFFER_HEADER + sizeof(INDEX_HEADER_ATTRIBUTE) +
                             (Index->BlockSize / Vcb->NtfsInfo.BytesPerSector + 1) * sizeof(USHORT),
                             8);
    Builder.FirstEntryOffset = FirstEntry - NTFS_INDEX_BUFFER_HEADER;
    Builder.AllocatedSize = Index->BlockSize - NTFS_INDEX_BUFFER_HEADER;

    Status = NtfsReadIndexBitmap(Index, &Builder.OldBitmap, &Builder.OldBitmapBits);
    if (NT_SUCCESS(Status))
        Status = NtfsBuildTree(&Builder, List, &TopVcn);
    if (NT_SUCCESS(Status))
        Status = NtfsCommitLargeIndex(Index, &Builder, TopVcn);

    for (i = 0; i < Builder.NodeCount; i++)
        ExFreePoolWithTag(Builder.Nodes[i].Buffer, TAG_NTFS);
    if (Builder.Nodes)
        ExFreePoolWithTag(Builder.Nodes, TAG_NTFS);
    if (Builder.OldBitmap)
        ExFreePoolWithTag(Builder.OldBitmap, TAG_NTFS);

    return Status;
}

/**
* @name NtfsIndexUpdate
*
* Changes the names listed in a directory's $I30 index: removes every entry
* for RemoveMftIndex (when Remove is TRUE), then adds AddName for the file
* AddFileReference (when AddName isn't NULL). Both happen in one rewrite, so a
* rename within a directory is atomic.
*
* @return
* STATUS_OBJECT_NAME_COLLISION if AddName already exists.
* STATUS_NOT_IMPLEMENTED if the index needs an attribute list, or an entry
* doesn't fit into an index block.
*
* @remarks
* The caller holds DirResource and has called NtfsPrepareForMetadataUpdate().
* On failure after the disk was touched the volume is marked dirty.
*/
NTSTATUS
NtfsIndexUpdate(PDEVICE_EXTENSION Vcb,
                ULONGLONG DirectoryMftIndex,
                BOOLEAN Remove,
                ULONGLONG RemoveMftIndex,
                PFILENAME_ATTRIBUTE AddName,
                ULONGLONG AddFileReference,
                BOOLEAN CaseSensitive,
                PULONG RemovedCount)
{
    NTFS_INDEX Index;
    NTFS_ENTRY_LIST List;
    ULONG Removed = 0;
    ULONG i, j;
    NTSTATUS Status;

    if (RemovedCount)
        *RemovedCount = 0;

    Status = NtfsOpenIndex(Vcb, DirectoryMftIndex, &Index);
    if (!NT_SUCCESS(Status))
        return Status;

    RtlZeroMemory(&List, sizeof(List));
    Status = NtfsWalkIndex(&Index, NtfsCollectVisitor, &List);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Reading the index of directory %I64u failed (0x%08lx)\n", DirectoryMftIndex, Status);
        goto Cleanup;
    }

    if (Remove)
    {
        for (i = 0, j = 0; i < List.Count; i++)
        {
            if ((List.Entries[i]->Data.Directory.IndexedFile & NTFS_MFT_MASK) == RemoveMftIndex)
            {
                ExFreePoolWithTag(List.Entries[i], TAG_NTFS);
                Removed++;
            }
            else
            {
                List.Entries[j++] = List.Entries[i];
            }
        }
        List.Count = j;
    }

    if (AddName)
    {
        ULONG KeyLength = GetFileNameAttributeLength(AddName);
        ULONG EntryLength = ALIGN_UP_BY(NTFS_INDEX_ENTRY_HEADER + KeyLength, 8);
        PINDEX_ENTRY_ATTRIBUTE NewEntry;
        ULONG Position = List.Count;

        NewEntry = ExAllocatePoolWithTag(NonPagedPool, EntryLength, TAG_NTFS);
        if (!NewEntry)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            goto Cleanup;
        }
        RtlZeroMemory(NewEntry, EntryLength);
        NewEntry->Data.Directory.IndexedFile = AddFileReference;
        NewEntry->Length = (USHORT)EntryLength;
        NewEntry->KeyLength = (USHORT)KeyLength;
        RtlCopyMemory(&NewEntry->FileName, AddName, KeyLength);

        for (i = 0; i < List.Count; i++)
        {
            LONG Comparison = NtfsCollateEntries(NewEntry, List.Entries[i], FALSE);

            if (Comparison == 0 &&
                (!CaseSensitive || NtfsCollateEntries(NewEntry, List.Entries[i], TRUE) == 0))
            {
                ExFreePoolWithTag(NewEntry, TAG_NTFS);
                Status = STATUS_OBJECT_NAME_COLLISION;
                goto Cleanup;
            }
            if (NtfsCollateEntries(NewEntry, List.Entries[i], TRUE) < 0)
            {
                Position = i;
                break;
            }
        }

        Status = NtfsReserveEntries(&List, List.Count + 1);
        if (!NT_SUCCESS(Status))
        {
            ExFreePoolWithTag(NewEntry, TAG_NTFS);
            goto Cleanup;
        }
        RtlMoveMemory(&List.Entries[Position + 1], &List.Entries[Position],
                      (List.Count - Position) * sizeof(PINDEX_ENTRY_ATTRIBUTE));
        List.Entries[Position] = NewEntry;
        List.Count++;
    }

    if (Remove && Removed == 0 && !AddName)
    {
        Status = STATUS_OBJECT_NAME_NOT_FOUND;
        goto Cleanup;
    }

    Status = NtfsRewriteIndex(&Index, &List);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Rewriting the index of directory %I64u failed (0x%08lx)\n", DirectoryMftIndex, Status);
        if (Index.DiskTouched)
            NtfsMarkJournalFailure(Vcb, 0x0601);
    }
    else if (RemovedCount)
    {
        *RemovedCount = Removed;
    }

Cleanup:
    NtfsFreeEntryList(&List);
    NtfsCloseIndex(&Index);
    return Status;
}

static
NTSTATUS
NtfsCountVisitor(PVOID Context,
                 PINDEX_ENTRY_ATTRIBUTE Entry,
                 PBOOLEAN Modified,
                 PBOOLEAN Stop)
{
    UNREFERENCED_PARAMETER(Entry);
    UNREFERENCED_PARAMETER(Modified);

    (*(PULONG)Context)++;
    *Stop = TRUE;
    return STATUS_SUCCESS;
}

/* Returns whether a directory's index holds no names. */
NTSTATUS
NtfsIndexIsEmpty(PDEVICE_EXTENSION Vcb,
                 ULONGLONG DirectoryMftIndex,
                 PBOOLEAN Empty)
{
    NTFS_INDEX Index;
    ULONG Count = 0;
    NTSTATUS Status;

    Status = NtfsOpenIndex(Vcb, DirectoryMftIndex, &Index);
    if (!NT_SUCCESS(Status))
        return Status;

    Status = NtfsWalkIndex(&Index, NtfsCountVisitor, &Count);
    NtfsCloseIndex(&Index);

    if (NT_SUCCESS(Status))
        *Empty = (Count == 0);
    return Status;
}

/* EOF */
