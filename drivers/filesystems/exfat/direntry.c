/*
 * PROJECT:     exFAT Filesystem
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Parsing exFAT directory entry sets; names, hashes and times
 * COPYRIGHT:   Copyright 2026 WinDosDX Team & Contributors
 *              Based on the vfatfs directory entry routines by the ReactOS Team
 */

/*  -------------------------------------------------------  INCLUDES  */

#include "vfat.h"

#define NDEBUG
#include <debug.h>

#define TICKS_PER_15_MINUTES (15LL * 60 * 10000000)

/*
 * exFAT timestamps are DOS date/time pairs (2-second resolution) in local
 * time, refined by a 10 ms increment and qualified by a UTC offset in
 * 15-minute units (bit 7 set when the offset is valid).
 */
VOID
ExfatTimestampToSystemTime(
    ULONG Timestamp,
    UCHAR Increment10ms,
    UCHAR UtcOffset,
    PLARGE_INTEGER SystemTime)
{
    TIME_FIELDS TimeFields;
    LARGE_INTEGER Time;
    CHAR Offset;

    TimeFields.Year = (CSHORT)(1980 + (Timestamp >> 25));
    TimeFields.Month = (CSHORT)((Timestamp >> 21) & 0x0F);
    TimeFields.Day = (CSHORT)((Timestamp >> 16) & 0x1F);
    TimeFields.Hour = (CSHORT)((Timestamp >> 11) & 0x1F);
    TimeFields.Minute = (CSHORT)((Timestamp >> 5) & 0x3F);
    TimeFields.Second = (CSHORT)((Timestamp & 0x1F) * 2);
    TimeFields.Milliseconds = 0;
    TimeFields.Weekday = 0;
    if (Increment10ms < 200)
    {
        TimeFields.Second += Increment10ms / 100;
        TimeFields.Milliseconds = (CSHORT)((Increment10ms % 100) * 10);
    }

    if (!RtlTimeFieldsToTime(&TimeFields, &Time))
    {
        /* Zero or otherwise invalid timestamp */
        SystemTime->QuadPart = 0;
        return;
    }

    if (UtcOffset & 0x80)
    {
        /* Sign-extend the 7-bit offset; UTC = local time - offset. */
        Offset = (CHAR)(UtcOffset << 1) >> 1;
        Time.QuadPart -= Offset * TICKS_PER_15_MINUTES;
    }
    else
    {
        ExLocalTimeToSystemTime(&Time, &Time);
    }
    *SystemTime = Time;
}

VOID
ExfatSystemTimeToTimestamp(
    PLARGE_INTEGER SystemTime,
    PULONG Timestamp,
    PUCHAR Increment10ms,
    PUCHAR UtcOffset)
{
    LARGE_INTEGER LocalTime;
    TIME_FIELDS TimeFields;
    LONGLONG Bias;

    ExSystemTimeToLocalTime(SystemTime, &LocalTime);
    RtlTimeToTimeFields(&LocalTime, &TimeFields);

    if (TimeFields.Year < 1980)
    {
        TimeFields.Year = 1980;
        TimeFields.Month = 1;
        TimeFields.Day = 1;
        TimeFields.Hour = TimeFields.Minute = TimeFields.Second = TimeFields.Milliseconds = 0;
    }
    else if (TimeFields.Year > 1980 + 127)
    {
        TimeFields.Year = 1980 + 127;
        TimeFields.Month = 12;
        TimeFields.Day = 31;
        TimeFields.Hour = 23;
        TimeFields.Minute = TimeFields.Second = 59;
        TimeFields.Milliseconds = 990;
    }

    *Timestamp = ((ULONG)(TimeFields.Year - 1980) << 25) |
                 ((ULONG)TimeFields.Month << 21) |
                 ((ULONG)TimeFields.Day << 16) |
                 ((ULONG)TimeFields.Hour << 11) |
                 ((ULONG)TimeFields.Minute << 5) |
                 ((ULONG)TimeFields.Second / 2);
    *Increment10ms = (UCHAR)((TimeFields.Second % 2) * 100 + TimeFields.Milliseconds / 10);

    Bias = (LocalTime.QuadPart - SystemTime->QuadPart) / TICKS_PER_15_MINUTES;
    if (Bias < -64 || Bias > 63)
        *UtcOffset = 0;
    else
        *UtcOffset = 0x80 | ((UCHAR)Bias & 0x7F);
}

/*
 * The entry set checksum covers every byte of every entry of the set except
 * the checksum field itself (bytes 2 and 3 of the File entry).
 */
USHORT
ExfatEntrySetChecksum(
    PUCHAR Entries,
    ULONG Count)
{
    USHORT Checksum = 0;
    ULONG i;

    for (i = 0; i < Count * EXFAT_ENTRY_SIZE; i++)
    {
        if (i == 2 || i == 3)
            continue;
        Checksum = ((Checksum & 1) ? 0x8000 : 0) + (Checksum >> 1) + Entries[i];
    }
    return Checksum;
}

static
USHORT
ExfatChecksumAdd(
    USHORT Checksum,
    PUCHAR Entry,
    BOOLEAN Primary)
{
    ULONG i;

    for (i = 0; i < EXFAT_ENTRY_SIZE; i++)
    {
        if (Primary && (i == 2 || i == 3))
            continue;
        Checksum = ((Checksum & 1) ? 0x8000 : 0) + (Checksum >> 1) + Entry[i];
    }
    return Checksum;
}

WCHAR
ExfatUpcaseChar(
    PDEVICE_EXTENSION DeviceExt,
    WCHAR Char)
{
    if (DeviceExt->UpcaseTable)
        return DeviceExt->UpcaseTable[Char];
    return RtlUpcaseUnicodeChar(Char);
}

/* The name hash is taken over the up-cased name, byte by byte. */
USHORT
ExfatNameHash(
    PDEVICE_EXTENSION DeviceExt,
    PCUNICODE_STRING Name)
{
    USHORT Hash = 0;
    USHORT i;
    WCHAR Char;

    for (i = 0; i < Name->Length / sizeof(WCHAR); i++)
    {
        Char = ExfatUpcaseChar(DeviceExt, Name->Buffer[i]);
        Hash = ((Hash & 1) ? 0x8000 : 0) + (Hash >> 1) + (UCHAR)(Char & 0xFF);
        Hash = ((Hash & 1) ? 0x8000 : 0) + (Hash >> 1) + (UCHAR)(Char >> 8);
    }
    return Hash;
}

BOOLEAN
ExfatNamesEqual(
    PDEVICE_EXTENSION DeviceExt,
    PCUNICODE_STRING Name1,
    PCUNICODE_STRING Name2)
{
    USHORT i;

    if (Name1->Length != Name2->Length)
        return FALSE;
    for (i = 0; i < Name1->Length / sizeof(WCHAR); i++)
    {
        if (ExfatUpcaseChar(DeviceExt, Name1->Buffer[i]) !=
            ExfatUpcaseChar(DeviceExt, Name2->Buffer[i]))
        {
            return FALSE;
        }
    }
    return TRUE;
}

VOID
ExfatChainFromEntry(
    PDEVICE_EXTENSION DeviceExt,
    PDIR_ENTRY Entry,
    PEXFAT_CHAIN Chain)
{
    ULONG BytesPerCluster = DeviceExt->FatInfo.BytesPerCluster;

    Chain->FirstCluster = Entry->Stream.FirstCluster;
    Chain->NoFatChain = BooleanFlagOn(Entry->Stream.GeneralSecondaryFlags, EXFAT_FLAG_NO_FAT_CHAIN);
    if (Chain->FirstCluster == 0)
        Chain->Count = 0;
    else
        Chain->Count = (ULONG)((Entry->Stream.DataLength + BytesPerCluster - 1) / BytesPerCluster);
}

/*
 * Map the directory page that holds entry Index, when needed, and return a
 * pointer to the entry. Remap is set when the caller moved to a new page.
 */
static
NTSTATUS
ExfatMapDirEntry(
    PVFATFCB DirFcb,
    ULONG Index,
    BOOLEAN Remap,
    PVOID *pContext,
    PVOID *pPage,
    PUCHAR *Entry)
{
    LARGE_INTEGER FileOffset;

    FileOffset.QuadPart = ROUND_DOWN_64((ULONGLONG)Index * EXFAT_ENTRY_SIZE, PAGE_SIZE);
    if (*pContext == NULL || Remap)
    {
        if (*pContext != NULL)
        {
            CcUnpinData(*pContext);
            *pContext = NULL;
        }

        if (FileOffset.QuadPart >= DirFcb->RFCB.FileSize.QuadPart)
            return STATUS_NO_MORE_ENTRIES;

        _SEH2_TRY
        {
            CcMapData(DirFcb->FileObject, &FileOffset, PAGE_SIZE, MAP_WAIT, pContext, pPage);
        }
        _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
        {
            *pContext = NULL;
            _SEH2_YIELD(return STATUS_NO_MORE_ENTRIES);
        }
        _SEH2_END;
    }

    *Entry = (PUCHAR)*pPage + (Index % EXFAT_ENTRIES_PER_PAGE) * EXFAT_ENTRY_SIZE;
    return STATUS_SUCCESS;
}

/*
 * Return the next file or directory at or after DirContext->DirIndex.
 * On success, StartIndex is the File entry, DirIndex the last entry of the
 * set, EntryCount the set size, DirEntry the File and Stream entries and
 * LongNameU the name. The page holding DirIndex stays mapped in *pContext,
 * so the caller can continue with DirIndex + 1.
 */
NTSTATUS
VfatGetNextDirEntry(
    PDEVICE_EXTENSION DeviceExt,
    PVOID *pContext,
    PVOID *pPage,
    IN PVFATFCB pDirFcb,
    PVFAT_DIRENTRY_CONTEXT DirContext,
    BOOLEAN First)
{
    PUCHAR Entry;
    PEXFAT_FILE_ENTRY FileEntry;
    PEXFAT_STREAM_ENTRY StreamEntry;
    PEXFAT_NAME_ENTRY NameEntry;
    ULONG SecondaryCount;
    ULONG NameEntries;
    ULONG NameChars;
    ULONG CopyChars;
    ULONG k;
    USHORT Checksum;
    BOOLEAN Remap;
    BOOLEAN Restart;
    NTSTATUS Status;

    UNREFERENCED_PARAMETER(First);

    DirContext->LongNameU.Length = 0;
    DirContext->LongNameU.Buffer[0] = UNICODE_NULL;

    Status = vfatFCBInitializeCacheFromVolume(DeviceExt, pDirFcb);
    if (!NT_SUCCESS(Status))
        return Status;

    /* The mapped page is the one holding DirIndex, unless DirIndex starts a page. */
    Remap = (*pContext == NULL) || (DirContext->DirIndex % EXFAT_ENTRIES_PER_PAGE) == 0;

    while (TRUE)
    {
        if ((ULONGLONG)DirContext->DirIndex * EXFAT_ENTRY_SIZE >= (ULONGLONG)pDirFcb->RFCB.FileSize.QuadPart)
        {
            if (*pContext)
                CcUnpinData(*pContext);
            *pContext = NULL;
            return STATUS_NO_MORE_ENTRIES;
        }

        Status = ExfatMapDirEntry(pDirFcb, DirContext->DirIndex, Remap, pContext, pPage, &Entry);
        if (!NT_SUCCESS(Status))
            return Status;
        Remap = FALSE;

        if (Entry[0] == EXFAT_TYPE_END)
        {
            CcUnpinData(*pContext);
            *pContext = NULL;
            return STATUS_NO_MORE_ENTRIES;
        }

        if (Entry[0] != EXFAT_TYPE_FILE)
        {
            /* Unused, a system entry (bitmap, up-case, label...), or an orphan */
            DirContext->DirIndex++;
            Remap = (DirContext->DirIndex % EXFAT_ENTRIES_PER_PAGE) == 0;
            continue;
        }

        FileEntry = (PEXFAT_FILE_ENTRY)Entry;
        SecondaryCount = FileEntry->SecondaryCount;
        DirContext->StartIndex = DirContext->DirIndex;
        RtlCopyMemory(&DirContext->DirEntry.File, FileEntry, sizeof(EXFAT_FILE_ENTRY));
        Checksum = ExfatChecksumAdd(0, Entry, TRUE);
        NameEntries = 0;
        NameChars = 0;
        Restart = FALSE;

        for (k = 1; k <= SecondaryCount; k++)
        {
            DirContext->DirIndex++;
            if ((ULONGLONG)DirContext->DirIndex * EXFAT_ENTRY_SIZE >= (ULONGLONG)pDirFcb->RFCB.FileSize.QuadPart)
                break;
            Status = ExfatMapDirEntry(pDirFcb, DirContext->DirIndex,
                                      (DirContext->DirIndex % EXFAT_ENTRIES_PER_PAGE) == 0,
                                      pContext, pPage, &Entry);
            if (!NT_SUCCESS(Status))
                return Status;

            if (k == 1 && Entry[0] != EXFAT_TYPE_STREAM)
                break;
            if (k >= 2 && k <= 1 + NameEntries && Entry[0] != EXFAT_TYPE_NAME)
                break;
            if ((Entry[0] & (EXFAT_TYPE_IN_USE | EXFAT_TYPE_SECONDARY)) !=
                (EXFAT_TYPE_IN_USE | EXFAT_TYPE_SECONDARY))
            {
                /* Not an in-use secondary: the set is cut short. A new
                   primary entry starts over from here. */
                Restart = (Entry[0] & EXFAT_TYPE_SECONDARY) == 0;
                break;
            }

            if (k == 1)
            {
                StreamEntry = (PEXFAT_STREAM_ENTRY)Entry;
                RtlCopyMemory(&DirContext->DirEntry.Stream, StreamEntry, sizeof(EXFAT_STREAM_ENTRY));
                NameEntries = (StreamEntry->NameLength + EXFAT_NAME_CHARS_PER_ENTRY - 1) /
                              EXFAT_NAME_CHARS_PER_ENTRY;
                if (StreamEntry->NameLength == 0 || NameEntries > SecondaryCount - 1 ||
                    StreamEntry->NameLength >= DirContext->LongNameU.MaximumLength / sizeof(WCHAR))
                {
                    break;
                }
            }
            else if (k <= 1 + NameEntries)
            {
                NameEntry = (PEXFAT_NAME_ENTRY)Entry;
                CopyChars = min(EXFAT_NAME_CHARS_PER_ENTRY,
                                (ULONG)DirContext->DirEntry.Stream.NameLength - NameChars);
                RtlCopyMemory(DirContext->LongNameU.Buffer + NameChars, NameEntry->FileName,
                              CopyChars * sizeof(WCHAR));
                NameChars += CopyChars;
            }
            Checksum = ExfatChecksumAdd(Checksum, Entry, FALSE);
        }

        if (k > SecondaryCount && SecondaryCount >= 2 &&
            NameChars == DirContext->DirEntry.Stream.NameLength &&
            Checksum == DirContext->DirEntry.File.SetChecksum)
        {
            DirContext->EntryCount = SecondaryCount + 1;
            DirContext->LongNameU.Buffer[NameChars] = UNICODE_NULL;
            DirContext->LongNameU.Length = (USHORT)(NameChars * sizeof(WCHAR));
            return STATUS_SUCCESS;
        }

        DPRINT1("exFAT: skipping a damaged entry set at index %lu (checksum %04x, expected %04x)\n",
                DirContext->StartIndex, Checksum, DirContext->DirEntry.File.SetChecksum);
        DirContext->LongNameU.Buffer[0] = UNICODE_NULL;
        if (!Restart)
        {
            DirContext->DirIndex++;
            Remap = (DirContext->DirIndex % EXFAT_ENTRIES_PER_PAGE) == 0;
        }
    }
}

BOOLEAN
VfatIsDirectoryEmpty(
    PDEVICE_EXTENSION DeviceExt,
    PVFATFCB Fcb)
{
    VFAT_DIRENTRY_CONTEXT DirContext;
    WCHAR NameBuffer[LONGNAME_MAX_LENGTH];
    PVOID Context = NULL;
    PVOID Page = NULL;
    NTSTATUS Status;

    RtlZeroMemory(&DirContext, sizeof(DirContext));
    DirContext.DeviceExt = DeviceExt;
    DirContext.LongNameU.Buffer = NameBuffer;
    DirContext.LongNameU.MaximumLength = sizeof(NameBuffer);

    Status = VfatGetNextDirEntry(DeviceExt, &Context, &Page, Fcb, &DirContext, TRUE);
    if (Context)
        CcUnpinData(Context);

    /* Only an error proves nothing; a found entry means not empty. */
    return Status == STATUS_NO_MORE_ENTRIES;
}

/* EOF */
