/*
 * PROJECT:        WinDosDX DOS Engine
 * LICENSE:        GNU GPLv2 only as published by the Free Software Foundation
 * PURPOSE:        windos.exe host - machine lifecycle + frame loop
 * PROGRAMMERS:    WinDosDX Team
 *
 * The host owns the platform backend and drives the machine lifecycle. The
 * emulated machine itself is supplied by the DOSBox-derived core in
 * windos_core/src/dosbox, which drives its own CPU, event and video loops
 * through the windos_platform contract declared in this directory.
 */

#include "windos_internal.h"
#include "windos_dos.h"

/*
 * Provided by the DOSBox machine core (windos_core/src/dosbox).  The core is
 * C++ but this host is C, so the three entry points keep C linkage; they are
 * declared inside extern "C" in windos_dosbox.cpp.
 */
int  WD_CoreInit(const WD_MachineConfig *config);
void WD_CoreShutdown(void);int WD_CoreRun(void);
void WD_CoreSetInitialCommand(const char *command);

/* ---------------------------------------------------------------------- */
/* Config file (windos.ini, same directory as the exe)                     */
/* ---------------------------------------------------------------------- */

static void
WD_DefaultConfig(WD_MachineConfig *cfg)
{
    ZeroMemory(cfg, sizeof(*cfg));
    cfg->title = "WinDosDX";
    cfg->width = 640;
    cfg->height = 480;
    cfg->cycles = 0;
    cfg->memory_kb = 0;
    cfg->fullscreen = 0;
    cfg->mute = 0;
}

/* Find WinDosDX.ini next to the executable.  Accept windos.ini as a
 * compatibility fallback for existing installations. */
static void
WD_ConfigPath(char *out, size_t max)
{
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
    char *slash;
    if (n == 0 || n >= MAX_PATH)
    {
        lstrcpynA(out, "WinDosDX.ini", (int)max);
        return;
    }
    slash = strrchr(exe, '\\');
    if (slash)
        *(slash + 1) = '\0';
    else
        exe[0] = '\0';
    lstrcpynA(out, exe, (int)max);
    lstrcatA(out, "WinDosDX.ini");
    if (GetFileAttributesA(out) == INVALID_FILE_ATTRIBUTES)
    {
        lstrcpynA(out, exe, (int)max);
        lstrcatA(out, "windos.ini");
    }
}

/*
 * Minimal key=value INI reader. Recognised keys:
 *   title, width, height, cycles, memory, fullscreen, mute
 *   mountC, mountD, ... (DOS drive -> host path)
 */
int WD_MachineLoadConfig(const char *host_path, WD_MachineConfig *out_cfg)
{
    FILE *f;
    char line[512];

    WD_DefaultConfig(out_cfg);
    if (!host_path)
        return -1;

    /* The launcher must not depend on a particular CWD.  A missing sample
     * configuration is normal; the defaults and default C: mount still work. */
    f = fopen(host_path, "r");
    if (!f)
        return -1;

    while (fgets(line, sizeof(line), f))
    {
        char *eq = strchr(line, '=');
        char *key, *val;
        if (line[0] == ';' || line[0] == '#' || !eq)
            continue;
        *eq = '\0';
        key = line;
        val = eq + 1;
        /* trim */
        while (*key == ' ' || *key == '\t') key++;
        while (*val == ' ' || *val == '\t') val++;
        {
            size_t l = strlen(val);
            while (l && (val[l-1] == '\r' || val[l-1] == '\n' || val[l-1] == ' '))
                val[--l] = '\0';
        }

        if (!_stricmp(key, "title"))            out_cfg->title = _strdup(val);
        else if (!_stricmp(key, "width"))      out_cfg->width = (u32)atoi(val);
        else if (!_stricmp(key, "height"))     out_cfg->height = (u32)atoi(val);
        else if (!_stricmp(key, "cycles"))     out_cfg->cycles = (u32)atoi(val);
        else if (!_stricmp(key, "memory"))     out_cfg->memory_kb = (u32)atoi(val);
        else if (!_stricmp(key, "fullscreen")) out_cfg->fullscreen = atoi(val);
        else if (!_stricmp(key, "mute"))       out_cfg->mute = atoi(val);
        else if ((key[0] == 'm' || key[0] == 'M') && key[1] == 'o'
                 && key[2] == 'u' && key[3] == 'n' && key[4] == 't')
        {
            /* mountX=<path> */
            char drive = key[5];
            if (drive && drive != '=')
                WD_FSAddMount(drive, val);
        }
    }
    fclose(f);
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Machine lifecycle                                                        */
/* ---------------------------------------------------------------------- */

/* Audio mixer callback is provided by the core; until then output silence. */
static void
WD_HostSilentFill(void *ctx, s16 *buffer, u32 frames)
{
    (void)ctx;
    ZeroMemory(buffer, frames * 2 * sizeof(s16));
}

static WD_MachineConfig g_cfg;

/* The OS loader may provide one command to execute when the DOS shell starts. */
static const char *g_initial_command;
static WD_DosDesktopLauncher g_desktop_launcher;

static void WD_Trace(const char *message)
{
    char path[MAX_PATH];
    HANDLE file;
    DWORD written;
    char line[512];

    WD_ConfigPath(path, sizeof(path));
    {
        char *ext = strrchr(path, '.');
        if (ext)
            lstrcpynA(ext, ".log", (int)(sizeof(path) - (ext - path)));
        else
            lstrcatA(path, ".log");
    }
    file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                       OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return;
    SetFilePointer(file, 0, NULL, FILE_END);
    _snprintf(line, sizeof(line) - 2, "WinDosDX: %s\r\n", message);
    WriteFile(file, line, (DWORD)strlen(line), &written, NULL);
    CloseHandle(file);
}

int WD_MachineInit(const WD_MachineConfig *config)
{
    WD_MachineConfig *cfg;

    WD_GetGlobal()->quit_requested = 0;

    if (WD_GetGlobal()->state == WD_DOS_STATE_RUNNING ||
        WD_GetGlobal()->state == WD_DOS_STATE_STARTING ||
        WD_GetGlobal()->state == WD_DOS_STATE_STOPPING)
        return -1;

    WD_GetGlobal()->state = WD_DOS_STATE_STARTING;

    if (config)
    {
        g_cfg = *config;
    }
    else
    {
        char ini[MAX_PATH];
        WD_ConfigPath(ini, sizeof(ini));
        WD_MachineLoadConfig(ini, &g_cfg);
    }
    cfg = &g_cfg;
    if (!cfg->title)
        cfg->title = "WinDosDX";

    /* WinDosDX always presents a usable DOS C: drive.  An explicit
     * mountC= entry in windos.ini wins; otherwise the current WinDosDX
     * working directory is exposed as C:. */
    if (!WD_FSGetMount('C'))
    {
        char current_dir[MAX_PATH];
        DWORD n = GetCurrentDirectoryA(MAX_PATH, current_dir);
        if (n == 0 || n >= MAX_PATH)
            lstrcpynA(current_dir, ".", MAX_PATH);
        WD_FSAddMount('C', current_dir);
    }

    if (g_initial_command)
        WD_CoreSetInitialCommand(g_initial_command);

    WD_Trace("machine init begin");
    if (WD_VideoInit(cfg->title, cfg->width, cfg->height, 32) != 0)
    {
        WD_Trace("video init failed");
        WD_GetGlobal()->state = WD_DOS_STATE_FAILED;
        return -1;
    }
    if (WD_InputInit() != 0)
    {
        WD_Trace("input init failed");
        WD_VideoShutdown();
        WD_GetGlobal()->state = WD_DOS_STATE_FAILED;
        return -1;
    }
    if (WD_TimerInit(WD_PIT_HZ) != 0)
    {
        WD_Trace("timer init failed");
        WD_InputShutdown();
        WD_VideoShutdown();
        WD_GetGlobal()->state = WD_DOS_STATE_FAILED;
        return -1;
    }
    WD_Trace("platform backends ready");
    if (cfg->fullscreen)
        WD_VideoSetFullscreen(1);

    /*
     * Bring up the emulated machine.  The core owns the audio device from
     * here on: it opens sound at its own configured rate when the mixer
     * section initialises, so the host must not pre-open it.
     */
    if (WD_CoreInit(cfg) != 0)
    {
        WD_Trace("DOS core init failed");
        WD_TimerShutdown();
        WD_InputShutdown();
        WD_VideoShutdown();
        WD_GetGlobal()->state = WD_DOS_STATE_FAILED;
        return -1;
    }

    WD_GetGlobal()->ready = 1;
    WD_GetGlobal()->state = WD_DOS_STATE_RUNNING;
    WD_Trace("DOS core initialized");
    return 0;
}

void WD_MachineShutdown(void)
{
    if (WD_GetGlobal()->state == WD_DOS_STATE_STOPPED)
        return;

    WD_GetGlobal()->state = WD_DOS_STATE_STOPPING;
    g_initial_command = NULL;
    g_desktop_launcher = NULL;
    WD_CoreShutdown();
    WD_SoundShutdown();
    WD_TimerShutdown();
    WD_InputShutdown();
    WD_VideoShutdown();
    WD_GetGlobal()->ready = 0;
    WD_GetGlobal()->state = WD_DOS_STATE_STOPPED;
}

/* Fired by the timer backend each frame; declared here to avoid a header. */
void WD_TimerDispatch(void);

/*
 * The DOS machine drives itself: WD_CoreRun() enters DOSBox's normal loop,
 * which pumps GFX_Events() (and therefore WD_InputPump) and calls
 * GFX_EndUpdate() (and therefore WD_VideoPresent) as the emulated VGA produces
 * frames.  The host only has to dispatch the periodic timer callbacks that the
 * sound and input backends rely on, which DOSBox's loop does not know about.
 */
int WD_DosStart(const WD_MachineConfig *config, const char *initial_command)
{
    g_initial_command = initial_command;
    return WD_MachineInit(config);
}

void WD_DosSetDesktopLauncher(WD_DosDesktopLauncher launcher)
{
    g_desktop_launcher = launcher;
}

int WD_DosLaunchDesktop(void)
{
    if (g_desktop_launcher)
        return g_desktop_launcher();
    return -1;
}

int WD_DosRun(void)
{
    if (!WD_DosIsRunning())
        return -1;
    return WD_MachineRun();
}

int WD_DosIsRunning(void)
{
    return WD_GetGlobal()->state == WD_DOS_STATE_RUNNING &&
           !WD_GetGlobal()->quit_requested;
}

void WD_DosRequestShutdown(void)
{
    if (WD_DosIsRunning())
        WD_PlatformRequestQuit();
}

void WD_DosShutdown(void)
{
    WD_MachineShutdown();
}

int WD_MachineRun(void)
{
    /* The DOSBox normal loop is self-driving.  Its event hook dispatches
     * the WinDosDX timer callbacks on every pass, so entering it here is
     * what starts the emulated machine rather than an empty host loop. */
    WD_Trace("entering DOS core run loop");
    return WD_CoreRun();
}
