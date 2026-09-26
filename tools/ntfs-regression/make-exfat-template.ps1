# Creates a 128 MB disk image with one exFAT partition formatted and filled by
# Windows, for testing the WinDosDX exFAT driver against a real volume.
# Run from an elevated PowerShell. It only touches the new file
# exfat-template.vhd in -OutDir (default: the current folder): the image is attached, partitioned,
# formatted, filled with test files, and detached.
#
# Binary test files hold byte[i] = (i * 31 + seed) mod 256; the guest test
# regenerates the same pattern to check every byte.
param([string] $OutDir = (Get-Location).Path)
$OutDir = (Resolve-Path $OutDir).Path  # diskpart needs an absolute path
$ErrorActionPreference = 'Stop'
$Vhd = Join-Path $OutDir 'exfat-template.vhd'
$Log = Join-Path $OutDir 'make-exfat-template.log'
if (Test-Path $Vhd) { throw "$Vhd already exists; delete it first to make a new one." }

function New-Pattern([int] $Length, [int] $Seed) {
    $Bytes = New-Object byte[] $Length
    for ($i = 0; $i -lt $Length; $i++) { $Bytes[$i] = [byte](($i * 31 + $Seed) -band 0xFF) }
    return ,$Bytes
}

$Script = Join-Path $env:TEMP 'windosdx-exfat-template.txt'
@"
create vdisk file="$Vhd" maximum=128 type=fixed
select vdisk file="$Vhd"
attach vdisk
convert mbr
create partition primary offset=1024
format fs=exfat quick label=EXFATREG unit=4096
assign
"@ | Set-Content -Path $Script -Encoding ascii

try {
    diskpart /s $Script | Tee-Object -FilePath $Log
    if ($LASTEXITCODE -ne 0) { throw "diskpart failed with exit code $LASTEXITCODE (see $Log)" }

    $Disk = Get-DiskImage -ImagePath $Vhd | Get-Disk
    $Part = Get-Partition -DiskNumber $Disk.Number | Where-Object DriveLetter | Select-Object -First 1
    if (-not $Part) { throw "The formatted partition got no drive letter." }
    $Root = "$($Part.DriveLetter):\"
    "Filling $Root" | Tee-Object -FilePath $Log -Append

    Set-Content -Path "$Root\hello.txt" -Value 'Hello from Windows exFAT' -Encoding ascii
    [IO.File]::WriteAllBytes("$Root\pattern-1m.bin", (New-Pattern 1048576 7))

    New-Item -ItemType Directory -Path "$Root\Folder\Sub" | Out-Null
    Set-Content -Path "$Root\Folder\Sub\deep.txt" -Value 'deep' -Encoding ascii

    $LongName = 'Long file name with ' + [char]0x00FC + 'n' + [char]0x00EF + 'c' + [char]0x00F6 +
                'd' + [char]0x00E9 + ' characters ' + [char]0x2713 + '.txt'
    Set-Content -Path (Join-Path $Root $LongName) -Value 'unicode' -Encoding ascii

    New-Item -ItemType Directory -Path "$Root\many" | Out-Null
    for ($i = 0; $i -lt 200; $i++) {
        $Name = 'file-{0:D3}.txt' -f $i
        Set-Content -Path "$Root\many\$Name" -Value ('file {0:D3}' -f $i) -Encoding ascii
    }

    # Two files grown alternately in 32 KB steps, flushed each time, so their
    # clusters interleave and both need FAT chains (fragmented).
    $A = New-Pattern 1048576 11
    $B = New-Pattern 1048576 13
    $Fa = [IO.File]::Open("$Root\frag-a.bin", 'CreateNew', 'Write', 'None')
    $Fb = [IO.File]::Open("$Root\frag-b.bin", 'CreateNew', 'Write', 'None')
    try {
        for ($Off = 0; $Off -lt 1048576; $Off += 32768) {
            $Fa.Write($A, $Off, 32768); $Fa.Flush($true)
            $Fb.Write($B, $Off, 32768); $Fb.Flush($true)
        }
    }
    finally { $Fa.Close(); $Fb.Close() }

    Get-ChildItem -Recurse $Root | Measure-Object -Property Length -Sum |
        ForEach-Object { "Wrote $($_.Count) items, $($_.Sum) bytes" } | Tee-Object -FilePath $Log -Append
}
finally {
    Remove-Item $Script -ErrorAction SilentlyContinue
    # Detaching dismounts the volume cleanly (flushes and clears the dirty flag).
    if (Get-DiskImage -ImagePath $Vhd -ErrorAction SilentlyContinue | Where-Object Attached) {
        Dismount-DiskImage -ImagePath $Vhd | Out-Null
    }
}
"Done: $Vhd"
