# Creates a 256 MB disk image with one NTFS partition formatted by Windows,
# for testing the WinDosDX NTFS driver against a real volume.
# Run from an elevated PowerShell. It only touches the new file ntfs-template.vhd
# in -OutDir (default: the current folder): the image is attached, partitioned, formatted and detached.
param([string] $OutDir = (Get-Location).Path)
$OutDir = (Resolve-Path $OutDir).Path  # diskpart needs an absolute path
$ErrorActionPreference = 'Stop'
$Vhd = Join-Path $OutDir 'ntfs-template.vhd'
$Log = Join-Path $OutDir 'make-template.log'
if (Test-Path $Vhd) { throw "$Vhd already exists; delete it first to make a new one." }

$Script = Join-Path $env:TEMP 'windosdx-ntfs-template.txt'
@"
create vdisk file="$Vhd" maximum=256 type=fixed
select vdisk file="$Vhd"
attach vdisk
convert mbr
create partition primary offset=1024
format fs=ntfs quick label=NTFSREG unit=4096
detach vdisk
"@ | Set-Content -Path $Script -Encoding ascii

try {
    diskpart /s $Script | Tee-Object -FilePath $Log
    if ($LASTEXITCODE -ne 0) { throw "diskpart failed with exit code $LASTEXITCODE (see $Log)" }
}
finally {
    Remove-Item $Script -ErrorAction SilentlyContinue
    # Never leave the image attached, even if a step failed.
    if (Get-DiskImage -ImagePath $Vhd -ErrorAction SilentlyContinue | Where-Object Attached) {
        Dismount-DiskImage -ImagePath $Vhd | Out-Null
    }
}
"Done: $Vhd"
