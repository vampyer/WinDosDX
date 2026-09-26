/*
 *  ReactOS kernel
 *  Copyright (C) 2016 ReactOS Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * COPYRIGHT:        See COPYING in the top level directory
 * PROJECT:          ReactOS kernel
 * FILE:             drivers/filesystem/ntfs/cleanup.c
 * PURPOSE:          NTFS filesystem driver
 * PROGRAMMER:       Pierre Schweitzer (pierre@reactos.org)
 * UPDATE HISTORY:
 */

/* INCLUDES *****************************************************************/

#include "ntfs.h"

#define NDEBUG
#include <debug.h>

/* FUNCTIONS ****************************************************************/

/*
 * FUNCTION: Drops the handle's volume-wide count. Cached file objects can
 * outlive their last handle by a long time, so this happens at cleanup
 * rather than at close; otherwise FSCTL_LOCK_VOLUME sees stale handles.
 */
static
VOID
NtfsReleaseVcbHandle(PDEVICE_EXTENSION DeviceExt,
                     PFILE_OBJECT FileObject)
{
    PNTFS_CCB Ccb = (PNTFS_CCB)(FileObject->FsContext2);

    if (Ccb == NULL || !Ccb->VcbHandleCounted)
        return;

    Ccb->VcbHandleCounted = FALSE;
    ASSERT(DeviceExt->OpenHandleCount > 0);
    if (DeviceExt->OpenHandleCount > 0)
    {
        DeviceExt->OpenHandleCount--;
    }

    if (DeviceExt->VolumeLockOwner == FileObject)
    {
        DeviceExt->VolumeLockOwner = NULL;
        DeviceExt->Flags &= ~VCB_VOLUME_LOCKED;
    }
}

/*
 * FUNCTION: Deletes a file whose last handle went away with a delete pending.
 * The caller holds DirResource and the FCB's MainResource.
 */
static
VOID
NtfsDeletePendingFile(PDEVICE_EXTENSION DeviceExt,
                      PNTFS_FCB Fcb)
{
    NTSTATUS Status;

    /* Cached data must never reach the clusters once they are freed. */
    if (!CcPurgeCacheSection(&Fcb->SectionObjectPointers, NULL, 0, FALSE))
    {
        CcFlushCache(&Fcb->SectionObjectPointers, NULL, 0, NULL);
        if (!CcPurgeCacheSection(&Fcb->SectionObjectPointers, NULL, 0, FALSE))
        {
            DPRINT1("Can't delete %S: its cached data is still in use\n", Fcb->PathName);
            return;
        }
    }

    Status = NtfsDeleteFileRecord(DeviceExt, Fcb->MFTIndex);
    if (!NT_SUCCESS(Status))
    {
        DPRINT1("Deleting %S failed (0x%08lx)\n", Fcb->PathName, Status);
        return;
    }

    Fcb->Flags &= ~FCB_DELETE_PENDING;
    Fcb->Flags |= FCB_IS_DELETED;
    Fcb->RFCB.FileSize.QuadPart = 0;
    Fcb->RFCB.ValidDataLength.QuadPart = 0;
    Fcb->RFCB.AllocationSize.QuadPart = 0;
    NtfsRemoveFCBFromTable(DeviceExt, Fcb);
}

/*
 * FUNCTION: Cleans up a file
 */
NTSTATUS
NtfsCleanupFile(PDEVICE_EXTENSION DeviceExt,
                PFILE_OBJECT FileObject,
                BOOLEAN CanWait)
{
    PNTFS_FCB Fcb;
    PNTFS_CCB Ccb;

    DPRINT("NtfsCleanupFile(DeviceExt %p, FileObject %p, CanWait %u)\n",
           DeviceExt,
           FileObject,
           CanWait);

    Fcb = (PNTFS_FCB)(FileObject->FsContext);
    if (!Fcb)
        return STATUS_SUCCESS;
    if (FileObject->Flags & FO_CLEANUP_COMPLETE)
        return STATUS_SUCCESS;

    if (Fcb->Flags & FCB_IS_VOLUME)
    {
        ASSERT(Fcb->OpenHandleCount > 0);
        if (Fcb->OpenHandleCount > 0)
        {
            Fcb->OpenHandleCount--;
        }

        if (Fcb->OpenHandleCount != 0)
        {
            // Remove share access when handled
        }
        NtfsReleaseVcbHandle(DeviceExt, FileObject);
        FileObject->Flags |= FO_CLEANUP_COMPLETE;
    }
    else
    {
        if (!ExAcquireResourceExclusiveLite(&Fcb->MainResource, CanWait))
        {
            return STATUS_PENDING;
        }

        ASSERT(Fcb->OpenHandleCount > 0);
        if (Fcb->OpenHandleCount > 0)
        {
            Fcb->OpenHandleCount--;
        }

        Ccb = (PNTFS_CCB)(FileObject->FsContext2);
        if (Ccb && Ccb->DeleteOnClose)
        {
            Fcb->Flags |= FCB_DELETE_PENDING;
        }

        CcUninitializeCacheMap(FileObject, &Fcb->RFCB.FileSize, NULL);

        if (Fcb->OpenHandleCount != 0)
        {
            // Remove share access when handled
        }
        else if (Fcb->Flags & FCB_DELETE_PENDING)
        {
            NtfsDeletePendingFile(DeviceExt, Fcb);
        }

        NtfsReleaseVcbHandle(DeviceExt, FileObject);
        FileObject->Flags |= FO_CLEANUP_COMPLETE;

        ExReleaseResourceLite(&Fcb->MainResource);
    }

    return STATUS_SUCCESS;
}

NTSTATUS
NtfsCleanup(PNTFS_IRP_CONTEXT IrpContext)
{
    PDEVICE_EXTENSION DeviceExtension;
    PFILE_OBJECT FileObject;
    NTSTATUS Status;
    PDEVICE_OBJECT DeviceObject;

    DPRINT("NtfsCleanup() called\n");

    DeviceObject = IrpContext->DeviceObject;
    if (DeviceObject == NtfsGlobalData->DeviceObject)
    {
        DPRINT("Cleaning up file system\n");
        IrpContext->Irp->IoStatus.Information = 0;
        return STATUS_SUCCESS;
    }

    FileObject = IrpContext->FileObject;
    DeviceExtension = DeviceObject->DeviceExtension;

    if (!ExAcquireResourceExclusiveLite(&DeviceExtension->DirResource,
                                        BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT)))
    {
        return NtfsMarkIrpContextForQueue(IrpContext);
    }

    Status = NtfsCleanupFile(DeviceExtension, FileObject, BooleanFlagOn(IrpContext->Flags, IRPCONTEXT_CANWAIT));

    ExReleaseResourceLite(&DeviceExtension->DirResource);

    if (Status == STATUS_PENDING)
    {
        return NtfsMarkIrpContextForQueue(IrpContext);
    }

    IrpContext->Irp->IoStatus.Information = 0;
    return Status;
}
