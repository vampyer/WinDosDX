# WinDosDX — Product & Architecture Plan

WinDosDX is a ReactOS derivative focused on being a modern Windows desktop
with a separate DOSBox-compatible application for DOS programs and games. The
DOS machine is **not** part of the OS boot path and is not linked into
`userinit.exe`:

* A standalone DOSBox-compatible application that runs 16-bit real-mode and
  32-bit DPMI DOS executables when launched by the user.
* **WOW64** so 32-bit Windows binaries run on the 64-bit kernel.
* A **Windows 7-style desktop** (start menu, taskbar, visual themes).
* A **hardware driver set** targeting the reference PC (see PCI map below).
* Full **rebranding** from ReactOS to WinDosDX.

This document is the source of truth for how the pieces fit together and the
order in which they are built.

---

## 1. Target hardware (reference PC)

From `pci.txt` — an AMD Renoir/Cezanne-class laptop:

| Device | PCI ID | Driver status |
|---|---|---|
| NVMe SSD | `1987:5013` | **new `stornvme` miniport (this change)** |
| AHCI SATA ×2 | `1022:7901` | existing `storahci` |
| xHCI USB ×2 | `1022:1639`, `1022:15df` | existing `xhci` |
| SMBus | `1022:790b` | generic |
| ISA bridge / FCH | `1022:790e` | generic |
| AMD iGPU (Vega) | `1002:1638` | VGA fallback only (no 3D) |
| HDA audio | `1002:1637`, `1022:15e3` | existing `usbaudio`/`sysaudio` |
| MediaTek MT7921 WiFi | `14c3:0608` | missing |

**The NVMe driver is the critical path** — without it the machine shows no
boot disk at all. It is implemented first (see §5).

---

## 2. DOS machine (DOSBox-derived)

### Approach
DOSBox is GPL-2; ReactOS/WinDosDX is GPL-2 — license compatible. The DOSBox
machine core remains vendored for a future standalone DOSBox application. A
Win32 backend and the `windos.exe` development host are optional test
adapters, not the OS product boundary:

* **Engine** — the DOSBox `dosbox` / `dos` / `ints` / `bios` / `cpu` core,
  compiled as a static library (`windos_core`). Provides the 8086/80386 CPU
  core, the DOS kernel, INT 10h/13h/21h, EMS/XMS, and the FAT/drive layer.
  The emulated environment reports **MS-DOS 6.22 compatibility** through
  `INT 21h / AH=30h` and `VER`. This is a compatibility target, not a claim
  that every DOS 6.22 kernel, utility, or device implementation is present.
* **Backend** — a Win32 backend replaces SDL:
  * video → WinDosDX GDI / DirectDraw surface
  * input/keyboard → Win32 keyboard messages
  * timer → Win32 timer / worker thread
  * sound → Win32 waveOut / DirectSound
  * filesystem → WinDosDX paths (`C:\GAMES\...`) mapped through a VFS shim so
    DOS `C:` maps to a host directory.
* **Standalone application integration** — a future DOSBox application will own
  the lifecycle and expose the emulated machine to DOS programs, games, and
  utilities. The OS boot path only launches the normal Windows shell.

### Deliverables
1. `windos_core` static library (DOSBox machine core). — **optional, disabled by default**
2. Standalone DOSBox application packaging and Explorer launch path. — **planned**
3. Optional Win32 development backend (video/input/timer/sound/fs). — **test-only**
4. `windos.exe` development host. — **optional, disabled by default**

The OS-side library lives under `base/applications/windos/` for now, but the
DOS machine itself belongs to the WinDosDX OS. The public contract is
`windos_core/include/windos_platform.h`; the Win32 implementation is a
 development adapter. The DOSBox machine source is in
`windos_core/src/dosbox/` and is wired through `WD_Core*` in
`windos_host.c`.

The normal OS startup path does not link `userinit.exe` to `windos_core` and
starts the configured Windows shell directly. The DOS core is excluded from
the default build and is not launched during logon. The existing `WINDEX`
command remains in the optional core sources for compatibility, but it has no
OS callback until a standalone application supplies one.

---

## 3. WOW64

WOW64 is the most expensive item. It requires the Windows model:

1. **32-bit `ntdll`** (`dll/wow64/ntdll`) exposing the full 32-bit syscall
   surface.
2. **Wow64Transition** — entering 32-bit code: set up a 32-bit context, switch
   to the 32-bit code segment, and marshal arguments.
3. **Syscall translation** — a 64-bit kernel entry (`KiSystemService64` →
   `Nt*` stubs) that converts 32-bit `NTSTATUS`/pointer arguments to 64-bit
   and copies structures across the WoW64 boundary.
4. **32-bit CSRSS / win32k interop** so GUI apps work, not just console.
5. **32-bit-on-64-bit process/thread structures** (TEB32/PEB32, etc.).

WinDosDX base has no `dll/wow64` today; this is greenfield and the single
longest task. It is started after the storage and branding work so the machine
is at least bootable and usable while WoW64 is built.

---

## 4. Windows 7-style shell + visual themes

The base shell (`base/shell/explorer`) is an NT5 Explorer fork. Making it look
and behave like Windows 7 means:

* **Start menu** — the round-cornered Win7 two-column start menu: user
  picture + name, pinned/recent programs, all-programs list, right-side
  Documents/Pictures/Music, shutdown/lock. Rebuilt as a new `startmenu` window
  class in the explorer shell.
* **Taskbar** — Win7 taskbar: larger Start orb, Quick Launch, combined
  task buttons, notification area, peek/Aero details.
* **Aero / visual themes** — a theme engine that consumes the same
  `.theme`-style data Win7 used (visual styles `vsstyles.msstyles`,
  `themeui.dll`-style drawing) so the Start menu, taskbar, and Explorer
  chrome adopt the active theme (Aero Glass, translucent borders, color
  scheme). This is a rendering layer over the existing controls.

This is a large but self-contained UI project in the explorer shell; it does
not require kernel changes.

---

## 5. Drivers (build order)

1. **NVMe (`stornvme`)** — **done.** A storport miniport that maps the
   controller BAR0, resets/initializes the controller, creates the admin + one
   I/O queue pair, identifies namespace 1, and translates SCSI SRBs (INQUIRY,
   READ CAPACITY, TEST UNIT READY, READ/WRITE, FLUSH) into NVMe
   Read/Write/Flush commands with PRP lists, completing synchronously
   (polled). Exposes namespace 1 as a single LUN so the boot disk appears.
   Packaged with a PnP INF and CD staging so it installs and loads.
   *Follow-ups:* admin-queue identify of all namespaces, multiple LUNs,
   interrupt/DPC completion (replace polling), MSI-X, SMART/Log Page.
2. **WiFi MT7921 (`14c3:0608`)** — a WLAN miniport (NDIS 6) on top of the
   MediaTek MT76 family. Large; deferred.
3. **AMD iGPU (Vega, `1002:1638`)** — a display-only framebuffer driver first
   (basic modes), 3D/Accel later. Large; deferred.
4. Existing AHCI (`storahci`), xHCI (`xhci`), HDA already cover the rest.

---

## 6. Branding (ReactOS → WinDosDX)

A sweeping rename across the tree, best done in one focused pass:

* Module/binary renames where the product name is user-visible
  (`explorer.exe`, setup branding, version resource strings).
* `dll/branding` already carries the WinDosDX banner (see commit
  `d102ae772db`); extend it to every user-facing surface.
* Version strings, registry `ProductName`, installer/setup text, EULA
  placeholder, README/docs.

Note: renaming binaries has broad build-integration consequences, so the
branding pass is scoped to user-visible strings and the shell/setup branding
first, with a full module-rename as an optional later step.

---

## 7. Build order / roadmap

```
1. stornvme driver          ✔ done — unlocks boot on the reference PC
2. Branding sweep            — in progress (visible surfaces done)
3. DOSBox machine (OS component) — core integrated; OS loader next
4. Win7 shell + themes       — start menu, taskbar, theme engine
   (userinit starts the internal DOS session)
5. WOW64                     — 32-bit ntdll, transition, syscall translation
6. WiFi / iGPU drivers       — remaining hardware
```

Steps 2–4 are largely independent and can proceed in any order. Step 5 (WOW64)
is the long pole and starts as soon as the machine boots reliably (step 1).

---

## 8. Current status

- `drivers/storage/port/stornvme/` — new NVMe storport miniport, **complete**:
  compiles clean at `/W3 /WX`, links to `stornvme.sys`, ships with a storport
  PnP `stornvme.inf` (UTF-16LE) and CD packaging (`add_cd_file` +
  `add_driver_inf`). Exposes NVMe namespace 1 as a single LUN.
- Branding: first visible surfaces rebranded to WinDosDX (winver About box,
  `rosbrand` resource description). The product name itself is registered by
  the setup engine from INF data — a full rename sweep is still pending.
- DOS engine: `base/applications/windos/` contains the optional DOSBox-derived
  `windos_core` machine. It reports MS-DOS 6.22 compatibility, but it is not
  linked into `userinit.exe` and is excluded from the default OS build. The next
  step is packaging it as a standalone application that the user can launch
  from Explorer.
- Win7 shell and WoW64 work not started yet.
