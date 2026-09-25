# windos_core — optional WinDosDX DOSBox machine

`windos_core` is a standalone WinDosDX component built from the DOSBox 0.74-3
machine core. It is **not** linked into `userinit.exe`, `explorer.exe`, or any
other OS boot path. The default product build leaves this directory and its
library target disabled.

The source remains here for the planned DOSBox application/add-on. When
packaged, the application will own the machine lifecycle and platform backend;
the operating system will only launch that application like any other program.

## Build policy

The parent `base/applications/windos/CMakeLists.txt` exposes two opt-in
options:

- `WINDOSDX_BUILD_DOS_CORE=ON` — build the `windos_core` static library.
- `WINDOSDX_BUILD_DOS_HOST=ON` — additionally build the current `windos.exe`
  development host. This requires the core option.

Neither option is enabled for a normal OS build. The current host is a
development/test executable, not an OS startup component.

## Layout

```
windos_core/
├── include/
│   ├── windos_platform.h   # platform/backend contract (C, no C++ types)
│   └── windos_dos.h        # machine lifecycle API
├── src/
│   ├── windos_internal.h   # shared internals
│   ├── windos_common.c     # global state, quit primitive
│   ├── windos_video.c      # GDI window + 32-bit DIB blit
│   ├── windos_input.c      # Win32 message -> portable events
│   ├── windos_timer.c      # monotonic clock + callbacks
│   ├── windos_sound.c      # waveOut sink
│   ├── windos_fs.c         # DOS drive -> host path mounts
│   ├── windos_host.c       # machine lifecycle + frame loop
│   └── dosbox/             # vendored DOSBox machine source
```

## Machine lifecycle

The optional host uses `include/windos_dos.h`:

```c
if (WD_DosStart(&config, "DOOM") == 0)
{
    if (WD_DosGetState() == WD_DOS_STATE_RUNNING)
        WD_DosRun();
    WD_DosShutdown();
}
```

`WD_DosStart` installs an optional initial command in the emulated
`AUTOEXEC.BAT`. The core then emulates the CPU, DOS kernel, BIOS interrupts,
DOS filesystem, and machine loop inside the application process.

The `WINDEX` command and its OS callback are retained for compatibility with
the existing core sources, but `userinit` no longer registers or invokes them.
A future DOSBox application may provide its own desktop handoff independently.

## Backend boundary

The current platform contract is `include/windos_platform.h`:

| Subsystem | Entry points | Notes |
|---|---|---|
| Video | `WD_VideoInit`, `WD_VideoPresent`, `WD_VideoResize`, `WD_VideoSetFullscreen` | 32-bit XRGB framebuffer presented through GDI. |
| Input | `WD_InputPump` | Produces key, mouse, and wheel events with DOS scancodes. |
| Timer | `WD_TimerNowUs/Ms`, `WD_TimerAddCallback` | Monotonic host timing. |
| Sound | `WD_SoundInit`, `WD_SoundFill` | 16-bit stereo mixed through `waveOut`. |
| Filesystem | `WD_FSAddMount`, `WD_FSOpen/Read/Write/...` | DOS drives map to host directories. |

The DOSBox frontend, mapper, platform layer, debugger, and external helper
libraries are excluded from the core target. WinDosDX replacements are kept in
`src/dosbox/`, including `windos_dosbox.cpp`, `windos_mapper.cpp`,
`windos_serial.cpp`, `windos_cdrom.cpp`, and `windos_stubs.cpp`.

The machine reports **MS-DOS 6.22** through `INT 21h / AH=30h` and `VER`. This
is a compatibility target inherited from the DOSBox-derived machine, not a
claim that every MS-DOS 6.22 component is implemented.

## Shutdown and ownership

The application that owns the core can use `WD_DosGetState()`,
`WD_DosIsRunning()`, `WD_DosRequestShutdown()`, and `WD_DosShutdown()` to
manage the machine. These calls operate only on the standalone application
process; they are not an operating-system boot or logon interface.

The license is compatible: DOSBox is GPL-2, and WinDosDX is GPL-2.
