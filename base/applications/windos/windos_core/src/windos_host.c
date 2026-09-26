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
    cfg->scale = 2;
    cfg->aspect = 1;
    cfg->smooth = 0;
}


/* The system-wide defaults: WinDosDX.ini, or windos.ini, next to the exe. */
static void
WD_SystemConfigPath(char *out, size_t max)
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
 * Save one of the user's settings in %APPDATA%\WinDosDX\windos.ini, which is
 * read after the system-wide file (see WD_MachineInit).
 */
void WD_ConfigSave(const char *section, const char *key, const char *value)
{
    char ini[MAX_PATH];
    WD_DataPath(ini, sizeof(ini), "windos.ini");
    WritePrivateProfileStringA(section, key, value, ini);
}

void WD_ConfigSaveInt(const char *section, const char *key, int value)
{
    char text[16];
    _snprintf(text, sizeof(text), "%d", value);
    text[sizeof(text) - 1] = '\0';
    WD_ConfigSave(section, key, text);
}

static int WD_ConfigParseFile(const char *host_path, WD_MachineConfig *out_cfg);

/*
 * Minimal key=value INI reader; [section] lines are ignored. Recognised keys:
 *   title, width, height, cycles, memory, fullscreen, mute
 *   scale (1-4), aspect (0/1), smooth (0/1)
 *   memsize (MB), machine (graphics card), sbtype (sound card)
 *   mountC, mountD, ... (DOS drive -> host path)
 */
int WD_MachineLoadConfig(const char *host_path, WD_MachineConfig *out_cfg)
{
    WD_DefaultConfig(out_cfg);
    return WD_ConfigParseFile(host_path, out_cfg);
}

/* Applies the keys in host_path on top of out_cfg. */
static int WD_ConfigParseFile(const char *host_path, WD_MachineConfig *out_cfg)
{
    FILE *f;
    char line[512];

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
        else if (!_stricmp(key, "scale"))      out_cfg->scale = (u32)atoi(val);
        else if (!_stricmp(key, "aspect"))     out_cfg->aspect = atoi(val);
        else if (!_stricmp(key, "smooth"))     out_cfg->smooth = atoi(val);
        else if (!_stricmp(key, "memsize"))    out_cfg->memsize_mb = (u32)atoi(val);
        else if (!_stricmp(key, "machine"))    lstrcpynA(out_cfg->machine, val, sizeof(out_cfg->machine));
        else if (!_stricmp(key, "sbtype"))     lstrcpynA(out_cfg->sbtype, val, sizeof(out_cfg->sbtype));
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
static char g_program_dir[MAX_PATH];
static char g_program_name[64];
static char g_profile_path[MAX_PATH];
static const char *g_default_drive;
static WD_DosDesktopLauncher g_desktop_launcher;

static void WD_Trace(const char *message)
{
    char path[MAX_PATH];
    HANDLE file;
    DWORD written;
    char line[512];

    WD_DataPath(path, sizeof(path), "windos.log");
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
        /* System-wide defaults first, then the user's own settings. */
        char ini[MAX_PATH];
        WD_DefaultConfig(&g_cfg);
        WD_SystemConfigPath(ini, sizeof(ini));
        WD_ConfigParseFile(ini, &g_cfg);
        WD_DataPath(ini, sizeof(ini), "windos.ini");
        WD_ConfigParseFile(ini, &g_cfg);
        /* A program's own profile goes on top (game profiles). */
        if (g_profile_path[0])
        {
            char note[MAX_PATH + 32];
            int loaded = WD_ConfigParseFile(g_profile_path, &g_cfg) == 0;
            _snprintf(note, sizeof(note), "profile %s: %s (machine=%s memsize=%u)",
                      loaded ? "loaded" : "not found", g_profile_path,
                      g_cfg.machine, g_cfg.memsize_mb);
            note[sizeof(note) - 1] = '\0';
            WD_Trace(note);
        }
    }
    cfg = &g_cfg;
    /* A program shows its name; the DOS prompt the configured title. */
    if (g_program_name[0])
    {
        static char program_title[96];
        _snprintf(program_title, sizeof(program_title), "%s - DOS 6.22", g_program_name);
        program_title[sizeof(program_title) - 1] = '\0';
        cfg->title = program_title;
    }
    else if (!cfg->title || !strcmp(cfg->title, "WinDosDX"))
    {
        cfg->title = "WinDosDX DOS 6.22";
    }

    /* WinDosDX always presents a usable DOS C: drive.  An explicit
     * mountC= entry in windos.ini wins; otherwise the current WinDosDX
     * working directory is exposed as C:. */
    if (g_program_dir[0])
        WD_FSAddMount('C', g_program_dir);
    else if (!WD_FSGetMount('C') && g_default_drive)
    {
        CreateDirectoryA(g_default_drive, NULL);
        WD_FSAddMount('C', g_default_drive);
    }
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
    WD_VideoConfigure(cfg->scale, cfg->aspect, cfg->smooth);
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
/*
 * Profiles are named after the program's folder and file, which tells apart
 * the many INSTALL.EXE and SETUP.EXE: "DOOM - DOOM.EXE.ini".
 */
void WD_DosSetProgram(const char *host_dir, const char *name)
{
    const char *folder;
    char file[128];
    char *c;

    lstrcpynA(g_program_dir, host_dir ? host_dir : "", sizeof(g_program_dir));
    lstrcpynA(g_program_name, name ? name : "", sizeof(g_program_name));
    g_profile_path[0] = '\0';
    if (!g_program_dir[0] || !g_program_name[0])
        return;

    folder = strrchr(g_program_dir, '\\');
    folder = (folder && folder[1]) ? folder + 1 : g_program_dir;
    _snprintf(file, sizeof(file), "%s - %s.ini", folder, g_program_name);
    file[sizeof(file) - 1] = '\0';
    for (c = file; *c; c++)
    {
        if (strchr("\\/:*?\"<>|", *c))
            *c = '_';
    }

    WD_DataPath(g_profile_path, sizeof(g_profile_path), "profiles");
    CreateDirectoryA(g_profile_path, NULL);
    lstrcatA(g_profile_path, "\\");
    lstrcatA(g_profile_path, file);
}

const WD_MachineConfig *WD_HostGetConfig(void)
{
    return &g_cfg;
}

const char *WD_HostProgramName(void)
{
    return g_program_name[0] ? g_program_name : NULL;
}

const char *WD_HostProfilePath(void)
{
    return g_profile_path[0] ? g_profile_path : NULL;
}

void WD_DosSetDefaultDrive(const char *host_dir)
{
    g_default_drive = host_dir;
}

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
