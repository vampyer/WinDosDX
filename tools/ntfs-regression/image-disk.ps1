# Copies a whole physical disk into a raw image file, for testing a WinDosDX
# file system driver on a copy of a real disk instead of the disk itself.
# Run from an elevated PowerShell:
#   powershell -ExecutionPolicy Bypass -File image-disk.ps1 -DriveLetter E -Out <file.raw>
# The disk is only read. The script refuses anything but a removable or USB
# disk that is neither the boot nor the system disk. All-zero blocks are
# skipped in a sparse output file, so an empty disk takes little space.
param(
    [Parameter(Mandatory)] [char] $DriveLetter,
    [Parameter(Mandatory)] [string] $Out
)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
public static class WinDosDXImage {
    public static bool IsZero(byte[] b, int n) {
        for (int i = 0; i < n; i++) if (b[i] != 0) return false;
        return true;
    }
}
'@

$Partition = Get-Partition -DriveLetter $DriveLetter
$Disk = Get-Disk -Number $Partition.DiskNumber
if ($Disk.IsBoot -or $Disk.IsSystem) { throw "Disk $($Disk.Number) is the boot or system disk; refusing." }
if ($Disk.BusType -ne 'USB' -and (Get-Volume -DriveLetter $DriveLetter).DriveType -ne 'Removable') {
    throw "Disk $($Disk.Number) ($($Disk.FriendlyName)) is not a USB or removable disk; refusing."
}
if (Test-Path $Out) { throw "$Out already exists; delete it first." }
New-Item -ItemType Directory -Force (Split-Path -Parent ([IO.Path]::GetFullPath($Out))) | Out-Null
$Out = [IO.Path]::GetFullPath($Out)

"Imaging disk $($Disk.Number): $($Disk.FriendlyName), $($Disk.Size) bytes, $($Disk.BusType) -> $Out"

$Source = [IO.File]::Open("\\.\PhysicalDrive$($Disk.Number)", 'Open', 'Read', 'ReadWrite')
try {
    [IO.File]::Create($Out).Dispose()
    fsutil sparse setflag $Out | Out-Null
    $Target = [IO.File]::Open($Out, 'Open', 'Write', 'None')
    try {
        $Block = New-Object byte[] (4MB)
        $Total = [long]$Disk.Size
        $Done = [long]0
        $Kept = [long]0
        $Bad = [long]0
        $BadRun = 0
        $BadRanges = New-Object System.Collections.ArrayList
        while ($Done -lt $Total) {
            $Want = [int][Math]::Min([long]$Block.Length, $Total - $Done)
            # A block that fails twice is left as zeros and recorded; many
            # failing in a row means the rest of the disk cannot be read.
            $ReadOk = $false
            for ($Try = 0; $Try -lt 2 -and -not $ReadOk; $Try++) {
                try {
                    $Source.Position = $Done
                    $Got = 0
                    while ($Got -lt $Want) {
                        $n = $Source.Read($Block, $Got, $Want - $Got)
                        if ($n -le 0) { throw "short read" }
                        $Got += $n
                    }
                    $ReadOk = $true
                }
                catch { Start-Sleep -Milliseconds 500 }
            }
            if (-not $ReadOk) {
                $Bad += $Want
                $BadRun++
                if ($BadRanges.Count -gt 0 -and $BadRanges[-1][1] -eq $Done) { $BadRanges[-1][1] = $Done + $Want }
                else { $BadRanges.Add(@($Done, ($Done + $Want))) | Out-Null }
                if ($BadRun -ge 16) {
                    "Stopped: 16 blocks in a row (64 MB) from offset $($Done - 15 * $Block.Length) could not be read."
                    break
                }
                $Done += $Want
                continue
            }
            $BadRun = 0
            if (-not [WinDosDXImage]::IsZero($Block, $Want)) {
                $Target.Position = $Done
                $Target.Write($Block, 0, $Want)
                $Kept += $Want
            }
            $Done += $Want
            if (($Done / $Block.Length) % 256 -eq 0) {
                Write-Progress -Activity "Imaging disk $($Disk.Number)" -PercentComplete ([int](100 * $Done / $Total))
            }
        }
        $Target.SetLength($Total)
    }
    finally { $Target.Dispose() }
}
finally { $Source.Dispose() }

"Done: read up to offset $Done of $Total bytes; $Kept bytes of data kept (the rest is sparse)."
if ($Bad -gt 0) {
    "UNREADABLE: $Bad bytes could not be read (left as zeros in the image):"
    foreach ($r in $BadRanges) { "  offset $($r[0]) to $($r[1])  ($([Math]::Round($r[0] / 1GB, 2)) GB to $([Math]::Round($r[1] / 1GB, 2)) GB)" }
} else {
    "Every byte of the disk was read without errors."
}
