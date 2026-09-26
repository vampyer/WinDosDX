# chkdsk of a test image. Run from an elevated PowerShell:
#   powershell -ExecutionPolicy Bypass -File chkdsk-image.ps1 -Vhd <path to .vhd> [-ReadWrite] [-FileSystem exFAT]
# Attaches the VHD (read-only unless -ReadWrite), runs chkdsk without /f (no
# repairs), and always detaches. -ReadWrite lets Windows mount the volume the
# normal way, which re-initializes an emptied $LogFile; it changes the VHD, so
# use it only on a disposable copy. Output goes to <image>.chkdsk.txt.
param([Parameter(Mandatory)] [string] $Vhd, [switch] $ReadWrite, [string] $FileSystem = 'NTFS')
$ErrorActionPreference = 'Stop'
$Vhd = (Resolve-Path $Vhd).Path
$Log = "$Vhd.chkdsk.txt"

$Access = if ($ReadWrite) { 'ReadWrite' } else { 'ReadOnly' }
Mount-DiskImage -ImagePath $Vhd -Access $Access | Out-Null
try {
    # Windows reports the disk before its volumes are enumerated; poll.
    $Vol = $null
    for ($i = 0; $i -lt 60 -and -not $Vol; $i++) {
        Start-Sleep -Milliseconds 500
        $Number = (Get-DiskImage -ImagePath $Vhd).Number
        if ($null -eq $Number) { continue }
        $Vol = Get-Partition -DiskNumber $Number -ErrorAction SilentlyContinue |
               Get-Volume -ErrorAction SilentlyContinue |
               Where-Object FileSystem -eq $FileSystem | Select-Object -First 1
    }
    if (-not $Vol) {
        # Record what Windows does see, to tell a rejected volume from a disk
        # left offline (for example by a disk signature collision).
        $Number = (Get-DiskImage -ImagePath $Vhd).Number
        "No $FileSystem volume appeared within 30 seconds. Disk number: $Number" | Out-File $Log -Encoding utf8
        if ($null -ne $Number) {
            Get-Disk -Number $Number | Format-List Number, OperationalStatus, IsOffline, OfflineReason,
                IsReadOnly, PartitionStyle, Signature | Out-File $Log -Append -Encoding utf8
            Get-Partition -DiskNumber $Number -ErrorAction SilentlyContinue |
                Format-List PartitionNumber, Offset, Size, MbrType, Type |
                Out-File $Log -Append -Encoding utf8
            Get-Partition -DiskNumber $Number -ErrorAction SilentlyContinue |
                Get-Volume -ErrorAction SilentlyContinue |
                Format-List FileSystem, FileSystemLabel, HealthStatus, Size |
                Out-File $Log -Append -Encoding utf8
        }
        Get-Content $Log
        throw "No $FileSystem volume appeared on the image within 30 seconds (details in $Log)."
    }
    $Target = if ($Vol.DriveLetter) { "$($Vol.DriveLetter):" } else { $Vol.Path.TrimEnd('\') }

    "Volume: $Target  label=$($Vol.FileSystemLabel)  attached $Access" | Out-File $Log -Encoding utf8
    "--- fsutil dirty query" | Out-File $Log -Append -Encoding utf8
    fsutil dirty query $Target 2>&1 | Out-File $Log -Append -Encoding utf8
    "--- chkdsk (read-only)" | Out-File $Log -Append -Encoding utf8
    chkdsk $Target 2>&1 | Out-File $Log -Append -Encoding utf8
    "chkdsk exit code: $LASTEXITCODE" | Out-File $Log -Append -Encoding utf8
}
finally {
    Dismount-DiskImage -ImagePath $Vhd | Out-Null
}
Get-Content $Log
