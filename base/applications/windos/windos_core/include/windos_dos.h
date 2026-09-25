/*
 * PROJECT:        WinDosDX OS
 * PURPOSE:        Optional DOSBox application lifecycle API
 * PROGRAMMERS:    WinDosDX Team
 *
 * This is the lifecycle boundary for the optional WinDosDX DOSBox
 * application. It is not consumed by userinit or the OS boot path.
 */

#ifndef _WINDOS_DOS_H_
#define _WINDOS_DOS_H_

#include "windos_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    WD_DOS_STATE_STOPPED = 0,
    WD_DOS_STATE_STARTING,
    WD_DOS_STATE_RUNNING,
    WD_DOS_STATE_STOPPING,
    WD_DOS_STATE_FAILED
} WD_DosState;

/* Optional OS callback used by the internal WINDEX DOS command. */
typedef int (*WD_DosDesktopLauncher)(void);

/*
 * Start the internal DOS machine.  initial_command is an optional DOS shell
 * command line (for example, "DOOM", "C:\\UTILS\\PKZIP.EXE -A").  It is
 * installed in the emulated AUTOEXEC.BAT before the machine starts, so the
 * OS loader can launch DOS programs and games without creating a host
 * process.  The pointer is borrowed and must remain valid until this call
 * returns.
 */
int WD_DosStart(const WD_MachineConfig *config, const char *initial_command);

/* Register the OS-owned launcher used by the internal WINDEX command. */
void WD_DosSetDesktopLauncher(WD_DosDesktopLauncher launcher);

/* Run until the DOS machine exits or the OS requests shutdown. */
int WD_DosRun(void);

/* Return the current lifecycle state of the internal DOS machine. */
WD_DosState WD_DosGetState(void);

/* Non-zero while the internal DOS machine is initialized and executing. */
int WD_DosIsRunning(void);

/* Ask the OS loader's DOS machine to stop at the next safe point. */
void WD_DosRequestShutdown(void);

/* Stop and release the internal DOS machine. */
void WD_DosShutdown(void);

/* Invoke the registered OS desktop launcher, if one is available. */
int WD_DosLaunchDesktop(void);

#ifdef __cplusplus
}
#endif

#endif /* _WINDOS_DOS_H_ */
