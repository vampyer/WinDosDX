/*
 * PROJECT:        WinDosDX DOS Engine
 * LICENSE:        GNU GPLv2 only as published by the Free Software Foundation
 * PURPOSE:        Settings window and game profiles
 * PROGRAMMERS:    WinDosDX Team
 *
 * Opened from the window's system menu. The settings are saved either in
 * the user's windos.ini (all DOS programs) or, while a program runs, in that
 * program's profile (%APPDATA%\WinDosDX\profiles\<folder> - <file>.ini),
 * which windos_host.c loads on top the next time the program starts.
 * Display settings apply at once; the others at the next start, because
 * DOSBox builds the emulated PC when it starts.
 */

#include "windos_internal.h"
#include "../include/windos_resource.h"

#include <shlobj.h>

typedef struct
{
    const WCHAR *label;
    const char *value;
} WD_Choice;

static const WD_Choice g_memory[] =
{
    { L"1 MB", "1" }, { L"2 MB", "2" }, { L"4 MB", "4" }, { L"8 MB", "8" },
    { L"16 MB", "16" }, { L"32 MB", "32" }, { L"63 MB", "63" },
};

static const WD_Choice g_machines[] =
{
    { L"SVGA (S3 Trio)", "svga_s3" },
    { L"VGA", "vgaonly" },
    { L"EGA", "ega" },
    { L"CGA", "cga" },
    { L"Tandy", "tandy" },
    { L"Hercules (monochrome)", "hercules" },
};

static const WD_Choice g_soundcards[] =
{
    { L"Sound Blaster 16", "sb16" },
    { L"Sound Blaster Pro", "sbpro2" },
    { L"Sound Blaster 2.0", "sb2" },
    { L"None", "none" },
};

static void
WD_FillCombo(HWND dlg, int id, const WD_Choice *choices, int count, const char *current,
             int fallback)
{
    HWND combo = GetDlgItem(dlg, id);
    int i, selected = fallback;

    for (i = 0; i < count; i++)
    {
        SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)choices[i].label);
        if (current && current[0] && !_stricmp(current, choices[i].value))
            selected = i;
    }
    SendMessageW(combo, CB_SETCURSEL, selected, 0);
}

static const char *
WD_ComboValue(HWND dlg, int id, const WD_Choice *choices, int count)
{
    int i = (int)SendDlgItemMessageW(dlg, id, CB_GETCURSEL, 0, 0);
    return (i >= 0 && i < count) ? choices[i].value : choices[0].value;
}

static void
WD_SetDlgTextA(HWND dlg, int id, const char *text)
{
    WCHAR wide[MAX_PATH];
    if (!MultiByteToWideChar(CP_ACP, 0, text ? text : "", -1, wide, MAX_PATH))
        wide[0] = L'\0';
    SetDlgItemTextW(dlg, id, wide);
}

static void
WD_GetDlgTextA(HWND dlg, int id, char *out, int size)
{
    WCHAR wide[MAX_PATH];
    GetDlgItemTextW(dlg, id, wide, MAX_PATH);
    if (!WideCharToMultiByte(CP_ACP, 0, wide, -1, out, size, NULL, NULL))
        out[0] = '\0';
}

static void
WD_BrowseFolder(HWND dlg, int edit_id, const WCHAR *title)
{
    BROWSEINFOW info;
    WCHAR path[MAX_PATH];
    LPITEMIDLIST pidl;

    ZeroMemory(&info, sizeof(info));
    info.hwndOwner = dlg;
    info.lpszTitle = title;
    info.ulFlags = BIF_RETURNONLYFSDIRS;
    pidl = SHBrowseForFolderW(&info);
    if (!pidl)
        return;
    if (SHGetPathFromIDListW(pidl, path))
        SetDlgItemTextW(dlg, edit_id, path);
    CoTaskMemFree(pidl);
}

/* A drive setting; an empty one removes the key, so the drive is unmounted. */
static void
WD_SaveDrive(const char *file, const char *key, const char *path)
{
    WritePrivateProfileStringA("Mounts", key, path[0] ? path : NULL, file);
}

static void
WD_SettingsInit(HWND dlg)
{
    const WD_MachineConfig *cfg = WD_HostGetConfig();
    const char *program = WD_HostProgramName();
    char text[32];
    u32 scale;
    int aspect, smooth, i;
    WCHAR label[96];

    /* CPU */
    CheckRadioButton(dlg, IDC_CPU_AUTO, IDC_CPU_FIXED, cfg->cycles ? IDC_CPU_FIXED : IDC_CPU_AUTO);
    _snprintf(text, sizeof(text), "%u", cfg->cycles ? cfg->cycles : 3000u);
    text[sizeof(text) - 1] = '\0';
    WD_SetDlgTextA(dlg, IDC_CPU_CYCLES, text);
    EnableWindow(GetDlgItem(dlg, IDC_CPU_CYCLES), cfg->cycles != 0);

    _snprintf(text, sizeof(text), "%u", cfg->memsize_mb ? cfg->memsize_mb : 16u);
    text[sizeof(text) - 1] = '\0';
    WD_FillCombo(dlg, IDC_MEMORY, g_memory, ARRAYSIZE(g_memory), text, 4);
    WD_FillCombo(dlg, IDC_MACHINE, g_machines, ARRAYSIZE(g_machines), cfg->machine, 0);
    WD_FillCombo(dlg, IDC_SBTYPE, g_soundcards, ARRAYSIZE(g_soundcards), cfg->sbtype, 0);
    CheckDlgButton(dlg, IDC_MUTE, cfg->mute ? BST_CHECKED : BST_UNCHECKED);

    /* Drives: while a program runs, C: is its own folder. */
    WD_SetDlgTextA(dlg, IDC_DRIVE_C, WD_FSGetMount('C'));
    WD_SetDlgTextA(dlg, IDC_DRIVE_D, WD_FSGetMount('D'));
    if (program)
    {
        EnableWindow(GetDlgItem(dlg, IDC_DRIVE_C), FALSE);
        EnableWindow(GetDlgItem(dlg, IDC_BROWSE_C), FALSE);
        SetDlgItemTextW(dlg, IDC_DRIVE_NOTE,
                        L"C: is the folder of the program you opened.");
    }
    else
    {
        SetDlgItemTextW(dlg, IDC_DRIVE_NOTE,
                        L"When you open a DOS program from Windows, its folder is C:.");
    }

    /* Display, as it is now (the system menu may have changed it). */
    WD_VideoGetOptions(&scale, &aspect, &smooth);
    for (i = 1; i <= 4; i++)
    {
        _snwprintf(label, ARRAYSIZE(label), L"%dx", i);
        SendDlgItemMessageW(dlg, IDC_SCALE, CB_ADDSTRING, 0, (LPARAM)label);
    }
    SendDlgItemMessageW(dlg, IDC_SCALE, CB_SETCURSEL, scale - 1, 0);
    CheckDlgButton(dlg, IDC_ASPECT, aspect ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_SMOOTH, smooth ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dlg, IDC_FULLSCREEN, cfg->fullscreen ? BST_CHECKED : BST_UNCHECKED);

    /* Where to save: a running program gets its own profile by default. */
    if (program)
    {
        WCHAR name[64];
        if (!MultiByteToWideChar(CP_ACP, 0, program, -1, name, ARRAYSIZE(name)))
            name[0] = L'\0';
        _snwprintf(label, ARRAYSIZE(label), L"Only %s", name);
        label[ARRAYSIZE(label) - 1] = L'\0';
        SetDlgItemTextW(dlg, IDC_SAVE_PROGRAM, label);
        CheckRadioButton(dlg, IDC_SAVE_PROGRAM, IDC_SAVE_ALL, IDC_SAVE_PROGRAM);
    }
    else
    {
        SetDlgItemTextW(dlg, IDC_SAVE_PROGRAM, L"Only this program");
        EnableWindow(GetDlgItem(dlg, IDC_SAVE_PROGRAM), FALSE);
        CheckRadioButton(dlg, IDC_SAVE_PROGRAM, IDC_SAVE_ALL, IDC_SAVE_ALL);
    }

    SetDlgItemTextW(dlg, IDC_NOTE,
                    L"Display settings apply now. The emulated PC and the drives change "
                    L"the next time the DOS machine starts.");
}

static void
WD_SettingsSave(HWND dlg)
{
    char file[MAX_PATH];
    char text[MAX_PATH];
    int scale, aspect, smooth;
    const char *profile = WD_HostProfilePath();

    if (profile && IsDlgButtonChecked(dlg, IDC_SAVE_PROGRAM) == BST_CHECKED)
        lstrcpynA(file, profile, sizeof(file));
    else
        WD_DataPath(file, sizeof(file), "windos.ini");

    /* Emulated PC */
    if (IsDlgButtonChecked(dlg, IDC_CPU_FIXED) == BST_CHECKED)
    {
        WD_GetDlgTextA(dlg, IDC_CPU_CYCLES, text, sizeof(text));
        if (atoi(text) <= 0)
            lstrcpyA(text, "3000");
    }
    else
    {
        lstrcpyA(text, "0");
    }
    WritePrivateProfileStringA("Machine", "cycles", text, file);
    WritePrivateProfileStringA("Machine", "memsize",
                               WD_ComboValue(dlg, IDC_MEMORY, g_memory, ARRAYSIZE(g_memory)), file);
    WritePrivateProfileStringA("Machine", "machine",
                               WD_ComboValue(dlg, IDC_MACHINE, g_machines, ARRAYSIZE(g_machines)), file);
    WritePrivateProfileStringA("Machine", "sbtype",
                               WD_ComboValue(dlg, IDC_SBTYPE, g_soundcards, ARRAYSIZE(g_soundcards)), file);
    WritePrivateProfileStringA("Machine", "mute",
                               IsDlgButtonChecked(dlg, IDC_MUTE) == BST_CHECKED ? "1" : "0", file);

    /* Drives */
    if (IsWindowEnabled(GetDlgItem(dlg, IDC_DRIVE_C)))
    {
        WD_GetDlgTextA(dlg, IDC_DRIVE_C, text, sizeof(text));
        WD_SaveDrive(file, "mountC", text);
    }
    WD_GetDlgTextA(dlg, IDC_DRIVE_D, text, sizeof(text));
    WD_SaveDrive(file, "mountD", text);

    /* Display, applied now */
    scale = (int)SendDlgItemMessageW(dlg, IDC_SCALE, CB_GETCURSEL, 0, 0) + 1;
    aspect = IsDlgButtonChecked(dlg, IDC_ASPECT) == BST_CHECKED;
    smooth = IsDlgButtonChecked(dlg, IDC_SMOOTH) == BST_CHECKED;
    _snprintf(text, sizeof(text), "%d", scale);
    text[sizeof(text) - 1] = '\0';
    WritePrivateProfileStringA("Display", "scale", text, file);
    WritePrivateProfileStringA("Display", "aspect", aspect ? "1" : "0", file);
    WritePrivateProfileStringA("Display", "smooth", smooth ? "1" : "0", file);
    WritePrivateProfileStringA("Display", "fullscreen",
                               IsDlgButtonChecked(dlg, IDC_FULLSCREEN) == BST_CHECKED ? "1" : "0",
                               file);
    WD_VideoConfigure((u32)scale, aspect, smooth);
}

static INT_PTR CALLBACK
WD_SettingsProc(HWND dlg, UINT msg, WPARAM wparam, LPARAM lparam)
{
    UNREFERENCED_PARAMETER(lparam);

    switch (msg)
    {
        case WM_INITDIALOG:
            WD_SettingsInit(dlg);
            return TRUE;

        case WM_COMMAND:
            switch (LOWORD(wparam))
            {
                case IDC_CPU_AUTO:
                case IDC_CPU_FIXED:
                    EnableWindow(GetDlgItem(dlg, IDC_CPU_CYCLES),
                                 IsDlgButtonChecked(dlg, IDC_CPU_FIXED) == BST_CHECKED);
                    return TRUE;
                case IDC_BROWSE_C:
                    WD_BrowseFolder(dlg, IDC_DRIVE_C, L"Folder for DOS drive C:");
                    return TRUE;
                case IDC_BROWSE_D:
                    WD_BrowseFolder(dlg, IDC_DRIVE_D, L"Folder for DOS drive D:");
                    return TRUE;
                case IDOK:
                    WD_SettingsSave(dlg);
                    EndDialog(dlg, IDOK);
                    return TRUE;
                case IDCANCEL:
                    EndDialog(dlg, IDCANCEL);
                    return TRUE;
            }
            break;
    }
    return FALSE;
}

void WD_SettingsShow(HWND owner)
{
    /* The DOS program waits while the window is open. */
    WD_SoundPause(1);
    DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_SETTINGS), owner,
                    WD_SettingsProc, 0);
    WD_SoundPause(0);
}
