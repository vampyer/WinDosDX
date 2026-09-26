/*
 * PROJECT:     VFAT Filesystem
 * LICENSE:     GPL-2.0-or-later (https://spdx.org/licenses/GPL-2.0-or-later)
 * PURPOSE:     Volume routines
 * COPYRIGHT:   Copyright 1998 Jason Filby <jasonfilby@yahoo.com>
 *              Copyright 2004-2022 Hervé Poussineau <hpoussin@reactos.org>
 */

/* INCLUDES *****************************************************************/

#include "vfat.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

static
NTSTATUS
FsdGetFsVolumeInformation(
    PDEVICE_OBJECT DeviceObject,
    PFILE_FS_VOLUME_INFORMATION FsVolumeInfo,
    PULONG BufferLength)
{
    NTSTATUS Status;
    PDEVICE_EXTENSION DeviceExt;

    DPRINT("FsdGetFsVolumeInformation()\n");
    DPRINT("FsVolumeInfo = %p\n", FsVolumeInfo);
    DPRINT("BufferLength %lu\n", *BufferLength);

    DPRINT("Required length %lu\n", FIELD_OFFSET(FILE_FS_VOLUME_INFORMATION, VolumeLabel) + DeviceObject->Vpb->VolumeLabelLength);
    DPRINT("LabelLength %hu\n", DeviceObject->Vpb->VolumeLabelLength);
    DPRINT("Label %.*S\n", DeviceObject->Vpb->VolumeLabelLength / sizeof(WCHAR), DeviceObject->Vpb->VolumeLabel);

    ASSERT(*BufferLength >= sizeof(FILE_FS_VOLUME_INFORMATION));
    *BufferLength -= FIELD_OFFSET(FILE_FS_VOLUME_INFORMATION, VolumeLabel);

    DeviceExt = DeviceObject->DeviceExtension;

    /* valid entries */
    FsVolumeInfo->VolumeSerialNumber = DeviceObject->Vpb->SerialNumber;
    FsVolumeInfo->VolumeLabelLength = DeviceObject->Vpb->VolumeLabelLength;
    if (*BufferLength < DeviceObject->Vpb->VolumeLabelLength)
    {
        Status =  STATUS_BUFFER_OVERFLOW;
        RtlCopyMemory(FsVolumeInfo->VolumeLabel,
                      DeviceObject->Vpb->VolumeLabel,
                      *BufferLength);
    }
    else
    {
        Status =  STATUS_SUCCESS;
        RtlCopyMemory(FsVolumeInfo->VolumeLabel,
                      DeviceObject->Vpb->VolumeLabel,
                      FsVolumeInfo->VolumeLabelLength);
        *BufferLength -= DeviceObject->Vpb->VolumeLabelLength;
    }

    /* exFAT does not record when a volume was created. */
    UNREFERENCED_PARAMETER(DeviceExt);
    FsVolumeInfo->VolumeCreationTime.QuadPart = 0;

    FsVolumeInfo->SupportsObjects = FALSE;

    DPRINT("Finished FsdGetFsVolumeInformation()\n");
    DPRINT("BufferLength %lu\n", *BufferLength);

    return Status;
}


static
NTSTATUS
FsdGetFsAttributeInformation(
    PDEVICE_EXTENSION DeviceExt,
    PFILE_FS_ATTRIBUTE_INFORMATION FsAttributeInfo,
    PULONG BufferLength)
{
    NTSTATUS Status;
    PCWSTR pName;
    ULONG Length;

    DPRINT("FsdGetFsAttributeInformation()\n");
    DPRINT("FsAttributeInfo = %p\n", FsAttributeInfo);
    DPRINT("BufferLength %lu\n", *BufferLength);

    ASSERT(*BufferLength >= sizeof(FILE_FS_ATTRIBUTE_INFORMATION));
    *BufferLength -= FIELD_OFFSET(FILE_FS_ATTRIBUTE_INFORMATION, FileSystemName);

    UNREFERENCED_PARAMETER(DeviceExt);
    pName = L"exFAT";

    Length = wcslen(pName) * sizeof(WCHAR);
    DPRINT("Required length %lu\n", (FIELD_OFFSET(FILE_FS_ATTRIBUTE_INFORMATION, FileSystemName) + Length));

    if (*BufferLength < Length)
    {
        Status = STATUS_BUFFER_OVERFLOW;
        Length = *BufferLength;
    }
    else
    {
        Status = STATUS_SUCCESS;
    }

    FsAttributeInfo->FileSystemAttributes =
        FILE_CASE_PRESERVED_NAMES | FILE_UNICODE_ON_DISK;
    if (BooleanFlagOn(DeviceExt->Flags, VCB_WRITE_PROTECTED))
        FsAttributeInfo->FileSystemAttributes |= FILE_READ_ONLY_VOLUME;

    FsAttributeInfo->MaximumComponentNameLength = 255;

    FsAttributeInfo->FileSystemNameLength = Length;

    RtlCopyMemory(FsAttributeInfo->FileSystemName, pName, Length);

    DPRINT("Finished FsdGetFsAttributeInformation()\n");

    *BufferLength -= Length;
    DPRINT("BufferLength %lu\n", *BufferLength);

    return Status;
}


static
NTSTATUS
FsdGetFsSizeInformation(
    PDEVICE_OBJECT DeviceObject,
    PFILE_FS_SIZE_INFORMATION FsSizeInfo,
    PULONG BufferLength)
{
    PDEVICE_EXTENSION DeviceExt;
    NTSTATUS Status;

    DPRINT("FsdGetFsSizeInformation()\n");
    DPRINT("FsSizeInfo = %p\n", FsSizeInfo);

    ASSERT(*BufferLength >= sizeof(FILE_FS_SIZE_INFORMATION));

    DeviceExt = DeviceObject->DeviceExtension;
    Status = CountAvailableClusters(DeviceExt, &FsSizeInfo->AvailableAllocationUnits);

    FsSizeInfo->TotalAllocationUnits.QuadPart = DeviceExt->FatInfo.NumberOfClusters;
    FsSizeInfo->SectorsPerAllocationUnit = DeviceExt->FatInfo.SectorsPerCluster;
    FsSizeInfo->BytesPerSector = DeviceExt->FatInfo.BytesPerSector;

    DPRINT("Finished FsdGetFsSizeInformation()\n");
    if (NT_SUCCESS(Status))
        *BufferLength -= sizeof(FILE_FS_SIZE_INFORMATION);

    return Status;
}


static
NTSTATUS
FsdGetFsDeviceInformation(
    PDEVICE_OBJECT DeviceObject,
    PFILE_FS_DEVICE_INFORMATION FsDeviceInfo,
    PULONG BufferLength)
{
    DPRINT("FsdGetFsDeviceInformation()\n");
    DPRINT("FsDeviceInfo = %p\n", FsDeviceInfo);
    DPRINT("BufferLength %lu\n", *BufferLength);
    DPRINT("Required length %lu\n", sizeof(FILE_FS_DEVICE_INFORMATION));

    ASSERT(*BufferLength >= sizeof(FILE_FS_DEVICE_INFORMATION));

    FsDeviceInfo->DeviceType = FILE_DEVICE_DISK;
    FsDeviceInfo->Characteristics = DeviceObject->Characteristics;

    DPRINT("FsdGetFsDeviceInformation() finished.\n");

    *BufferLength -= sizeof(FILE_FS_DEVICE_INFORMATION);
    DPRINT("BufferLength %lu\n", *BufferLength);

    return STATUS_SUCCESS;
}


static
NTSTATUS
FsdGetFsFullSizeInformation(
    PDEVICE_OBJECT DeviceObject,
    PFILE_FS_FULL_SIZE_INFORMATION FsSizeInfo,
    PULONG BufferLength)
{
    PDEVICE_EXTENSION DeviceExt;
    NTSTATUS Status;

    DPRINT("FsdGetFsFullSizeInformation()\n");
    DPRINT("FsSizeInfo = %p\n", FsSizeInfo);

    ASSERT(*BufferLength >= sizeof(FILE_FS_FULL_SIZE_INFORMATION));

    DeviceExt = DeviceObject->DeviceExtension;
    Status = CountAvailableClusters(DeviceExt, &FsSizeInfo->CallerAvailableAllocationUnits);

    FsSizeInfo->TotalAllocationUnits.QuadPart = DeviceExt->FatInfo.NumberOfClusters;
    FsSizeInfo->ActualAvailableAllocationUnits.QuadPart = FsSizeInfo->CallerAvailableAllocationUnits.QuadPart;
    FsSizeInfo->SectorsPerAllocationUnit = DeviceExt->FatInfo.SectorsPerCluster;
    FsSizeInfo->BytesPerSector = DeviceExt->FatInfo.BytesPerSector;

    DPRINT("Finished FsdGetFsFullSizeInformation()\n");
    if (NT_SUCCESS(Status))
        *BufferLength -= sizeof(FILE_FS_FULL_SIZE_INFORMATION);

    return Status;
}


/*
 * The label is the root directory's Volume Label entry (0x83): up to 11
 * UTF-16 characters. It is updated in place, or added when there is none.
 */
static
NTSTATUS
FsdSetFsLabelInformation(
    PDEVICE_OBJECT DeviceObject,
    PFILE_FS_LABEL_INFORMATION FsLabelInfo)
{
    PDEVICE_EXTENSION DeviceExt;
    PVOID Context = NULL;
    ULONG DirIndex;
    ULONG Count;
    PUCHAR Entry = NULL;
    PVFATFCB pRootFcb;
    LARGE_INTEGER FileOffset;
    EXFAT_LABEL_ENTRY Label;
    ULONG LabelLen;
    BOOLEAN LabelFound = FALSE;
    NTSTATUS Status = STATUS_SUCCESS;

    DPRINT("FsdSetFsLabelInformation()\n");

    DeviceExt = (PDEVICE_EXTENSION)DeviceObject->DeviceExtension;
    if (BooleanFlagOn(DeviceExt->Flags, VCB_WRITE_PROTECTED))
        return STATUS_MEDIA_WRITE_PROTECTED;

    LabelLen = FsLabelInfo->VolumeLabelLength / sizeof(WCHAR);
    if (LabelLen > 11 || sizeof(DeviceObject->Vpb->VolumeLabel) < FsLabelInfo->VolumeLabelLength)
        return STATUS_NAME_TOO_LONG;

    RtlZeroMemory(&Label, sizeof(Label));
    Label.EntryType = EXFAT_TYPE_LABEL;
    Label.CharacterCount = (UCHAR)LabelLen;
    RtlCopyMemory(Label.VolumeLabel, FsLabelInfo->VolumeLabel, LabelLen * sizeof(WCHAR));

    pRootFcb = vfatOpenRootFCB(DeviceExt);
    Status = vfatFCBInitializeCacheFromVolume(DeviceExt, pRootFcb);
    if (!NT_SUCCESS(Status))
    {
        vfatReleaseFCB(DeviceExt, pRootFcb);
        return Status;
    }

    /* Look for the existing label entry, up to the end of the directory. */
    Count = (ULONG)(pRootFcb->RFCB.FileSize.QuadPart / EXFAT_ENTRY_SIZE);
    for (DirIndex = 0; DirIndex < Count && NT_SUCCESS(Status); DirIndex++, Entry += EXFAT_ENTRY_SIZE)
    {
        if (Context == NULL || (DirIndex % EXFAT_ENTRIES_PER_PAGE) == 0)
        {
            if (Context)
                CcUnpinData(Context);
            Context = NULL;
            FileOffset.QuadPart = (LONGLONG)DirIndex * EXFAT_ENTRY_SIZE;
            _SEH2_TRY
            {
                CcPinRead(pRootFcb->FileObject, &FileOffset, PAGE_SIZE, PIN_WAIT, &Context, (PVOID*)&Entry);
            }
            _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
            {
                Status = _SEH2_GetExceptionCode();
            }
            _SEH2_END;
            if (!NT_SUCCESS(Status))
                break;
        }

        if (Entry[0] == EXFAT_TYPE_END)
            break;
        if (Entry[0] == EXFAT_TYPE_LABEL)
        {
            RtlCopyMemory(Entry, &Label, sizeof(Label));
            CcSetDirtyPinnedData(Context, NULL);
            LabelFound = TRUE;
            break;
        }
    }
    if (Context)
        CcUnpinData(Context);

    if (NT_SUCCESS(Status) && !LabelFound)
    {
        if (!vfatFindDirSpace(DeviceExt, pRootFcb, 1, &DirIndex))
        {
            Status = STATUS_DISK_FULL;
        }
        else
        {
            FileOffset.QuadPart = (LONGLONG)DirIndex * EXFAT_ENTRY_SIZE;
            _SEH2_TRY
            {
                CcPinRead(pRootFcb->FileObject, &FileOffset, EXFAT_ENTRY_SIZE, PIN_WAIT, &Context, (PVOID*)&Entry);
                RtlCopyMemory(Entry, &Label, sizeof(Label));
                CcSetDirtyPinnedData(Context, NULL);
                CcUnpinData(Context);
            }
            _SEH2_EXCEPT(EXCEPTION_EXECUTE_HANDLER)
            {
                Status = _SEH2_GetExceptionCode();
            }
            _SEH2_END;
        }
    }

    vfatReleaseFCB(DeviceExt, pRootFcb);
    if (!NT_SUCCESS(Status))
    {
        return Status;
    }

    /* Update volume label in memory */
    RtlCopyMemory(DeviceExt->VolumeLabel, FsLabelInfo->VolumeLabel, LabelLen * sizeof(WCHAR));
    DeviceExt->VolumeLabelLength = (USHORT)(LabelLen * sizeof(WCHAR));
    DeviceObject->Vpb->VolumeLabelLength = (USHORT)FsLabelInfo->VolumeLabelLength;
    RtlCopyMemory(DeviceObject->Vpb->VolumeLabel, FsLabelInfo->VolumeLabel, DeviceObject->Vpb->VolumeLabelLength);

    return Status;
}


/*
 * FUNCTION: Retrieve the specified volume information
 */
NTSTATUS
VfatQueryVolumeInformation(
    PVFAT_IRP_CONTEXT IrpContext)
{
    FS_INFORMATION_CLASS FsInformationClass;
    NTSTATUS RC = STATUS_SUCCESS;
    PVOID SystemBuffer;
    ULONG BufferLength;

    /* PRECONDITION */
    ASSERT(IrpContext);

    DPRINT("VfatQueryVolumeInformation(IrpContext %p)\n", IrpContext);

    if (!ExAcquireResourceSharedLite(&((PDEVICE_EXTENSION)IrpContext->DeviceObject->DeviceExtension)->DirResource,
                                     BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT)))
    {
        DPRINT1("DirResource failed!\n");
        return VfatMarkIrpContextForQueue(IrpContext);
    }

    /* INITIALIZATION */
    FsInformationClass = IrpContext->Stack->Parameters.QueryVolume.FsInformationClass;
    BufferLength = IrpContext->Stack->Parameters.QueryVolume.Length;
    SystemBuffer = IrpContext->Irp->AssociatedIrp.SystemBuffer;

    DPRINT("FsInformationClass %d\n", FsInformationClass);
    DPRINT("SystemBuffer %p\n", SystemBuffer);

    RtlZeroMemory(SystemBuffer, BufferLength);

    switch (FsInformationClass)
    {
        case FileFsVolumeInformation:
            RC = FsdGetFsVolumeInformation(IrpContext->DeviceObject,
                                           SystemBuffer,
                                           &BufferLength);
            break;

        case FileFsAttributeInformation:
            RC = FsdGetFsAttributeInformation(IrpContext->DeviceObject->DeviceExtension,
                                              SystemBuffer,
                                              &BufferLength);
            break;

        case FileFsSizeInformation:
            RC = FsdGetFsSizeInformation(IrpContext->DeviceObject,
                                         SystemBuffer,
                                         &BufferLength);
            break;

        case FileFsDeviceInformation:
            RC = FsdGetFsDeviceInformation(IrpContext->DeviceObject,
                                           SystemBuffer,
                                           &BufferLength);
            break;

        case FileFsFullSizeInformation:
            RC = FsdGetFsFullSizeInformation(IrpContext->DeviceObject,
                                             SystemBuffer,
                                             &BufferLength);
            break;

        default:
            RC = STATUS_NOT_SUPPORTED;
    }

    ExReleaseResourceLite(&((PDEVICE_EXTENSION)IrpContext->DeviceObject->DeviceExtension)->DirResource);

    IrpContext->Irp->IoStatus.Information =
        IrpContext->Stack->Parameters.QueryVolume.Length - BufferLength;

    return RC;
}


/*
 * FUNCTION: Set the specified volume information
 */
NTSTATUS
VfatSetVolumeInformation(
    PVFAT_IRP_CONTEXT IrpContext)
{
    FS_INFORMATION_CLASS FsInformationClass;
    NTSTATUS Status = STATUS_SUCCESS;
    PVOID SystemBuffer;
    ULONG BufferLength;
    PIO_STACK_LOCATION Stack = IrpContext->Stack;

    /* PRECONDITION */
    ASSERT(IrpContext);

    DPRINT("VfatSetVolumeInformation(IrpContext %p)\n", IrpContext);

    if (!ExAcquireResourceExclusiveLite(&((PDEVICE_EXTENSION)IrpContext->DeviceObject->DeviceExtension)->DirResource,
                                        BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT)))
    {
        return VfatMarkIrpContextForQueue(IrpContext);
    }

    FsInformationClass = Stack->Parameters.SetVolume.FsInformationClass;
    BufferLength = Stack->Parameters.SetVolume.Length;
    SystemBuffer = IrpContext->Irp->AssociatedIrp.SystemBuffer;

    DPRINT("FsInformationClass %d\n", FsInformationClass);
    DPRINT("BufferLength %u\n", BufferLength);
    DPRINT("SystemBuffer %p\n", SystemBuffer);

    switch (FsInformationClass)
    {
        case FileFsLabelInformation:
            Status = FsdSetFsLabelInformation(IrpContext->DeviceObject,
                                              SystemBuffer);
            break;

        default:
            Status = STATUS_NOT_SUPPORTED;
    }

    ExReleaseResourceLite(&((PDEVICE_EXTENSION)IrpContext->DeviceObject->DeviceExtension)->DirResource);
    IrpContext->Irp->IoStatus.Information = 0;

    return Status;
}

/* EOF */
