/*
 * PROJECT:        WinDosDX DOS Engine
 * LICENSE:        GNU GPLv2 only as published by the Free Software Foundation
 * PURPOSE:        Filesystem backend (DOS drives -> Win32 host paths)
 * PROGRAMMERS:    WinDosDX Team
 *
 * Maps DOS drive letters to host directories and provides the file I/O the
 * DOSBox localDrive layer calls, so the core never assumes POSIX paths.
 */

#include "windos_internal.h"
#include <stdio.h>
#include <ctype.h>

#define WD_MAX_MOUNTS 26

typedef struct
{
    int  used;
    char drive;             /* 'A'..'Z' */
    char host_path[MAX_PATH];
} WD_Mount;

static WD_Mount g_mounts[WD_MAX_MOUNTS];

/* Uppercase a drive letter and return the mount slot, or NULL. */
static WD_Mount *
WD_FSGetMountByDrive(int drive_letter)
{
    int idx;
    char d = (char)toupper((unsigned char)drive_letter);
    if (d < 'A' || d > 'Z')
        return NULL;
    idx = d - 'A';
    return g_mounts[idx].used ? &g_mounts[idx] : NULL;
}

int WD_FSAddMount(char drive_letter, const char *host_path)
{
    WD_Mount *m;
    char d = (char)toupper((unsigned char)drive_letter);
    if (d < 'A' || d > 'Z' || !host_path)
        return -1;
    m = WD_FSGetMountByDrive(d);
    if (!m)
    {
        m = &g_mounts[d - 'A'];
        m->used = 1;
        m->drive = d;
    }
    lstrcpynA(m->host_path, host_path, MAX_PATH);
    return 0;
}

const char *WD_FSGetMount(char drive_letter)
{
    WD_Mount *m = WD_FSGetMountByDrive(drive_letter);
    return m ? m->host_path : NULL;
}

/*
 * Convert a DOS path ("C:\GAMES\DOOM") into a host path using the mount
 * table. Returns 0 on success, -1 if the drive is unmounted or overflow.
 */
static int
WD_FSResolve(const char *dos_path, char *out, size_t out_max)
{
    char drive;
    const char *rest;
    const char *mount;
    size_t n;

    if (!dos_path || !out || out_max == 0)
        return -1;
    out[0] = '\0';

    if (dos_path[0] && dos_path[1] == ':')
    {
        drive = (char)toupper((unsigned char)dos_path[0]);
        rest = dos_path + 2;
    }
    else
    {
        /* Relative path: interpret against drive C by convention. */
        drive = 'C';
        rest = dos_path;
    }

    mount = WD_FSGetMount(drive);
    if (!mount)
        return -1;

    n = strlen(mount);
    if (n + 1 >= out_max)
        return -1;
    strcpy(out, mount);

    if (*rest == '\\' || *rest == '/')
        rest++;
    while (*rest)
    {
        if (n + 1 >= out_max)
            return -1;
        out[n++] = *rest++;
    }
    out[n] = '\0';
    return 0;
}

void WD_FSNormalizePath(char *inout_path, size_t max_len)
{
    size_t i, n;
    if (!inout_path)
        return;

    n = strlen(inout_path);
    if (n >= max_len)
        n = max_len - 1;
    inout_path[n] = '\0';

    for (i = 0; i < n; i++)
    {
        char c = inout_path[i];
        if (c == '/')
            inout_path[i] = '\\';
        else
            inout_path[i] = (char)toupper((unsigned char)c);
    }
}

void *WD_FSOpen(const char *path, const char *mode)
{
    char host[MAX_PATH * 2];
    if (WD_FSResolve(path, host, sizeof(host)) != 0)
        return NULL;
    return (void *)fopen(host, mode);
}

int WD_FSClose(void *fp)
{
    return fclose((FILE *)fp);
}

long WD_FSSize(void *fp)
{
    FILE *f = (FILE *)fp;
    long cur, size;
    if (!f)
        return -1;
    cur = ftell(f);
    if (fseek(f, 0, SEEK_END) != 0)
        return -1;
    size = ftell(f);
    fseek(f, cur, SEEK_SET);
    return size;
}

long WD_FSSeek(void *fp, long offset, int whence)
{
    return fseek((FILE *)fp, offset, whence);
}

size_t WD_FSRead(void *fp, void *buf, size_t size)
{
    return fread(buf, 1, size, (FILE *)fp);
}

size_t WD_FSWrite(void *fp, const void *buf, size_t size)
{
    return fwrite(buf, 1, size, (FILE *)fp);
}

int WD_FSFlush(void *fp)
{
    return fflush((FILE *)fp);
}

int WD_FSReadDir(const char *path, char *out_names, int max_entries,
                 size_t name_max, int want_dirs)
{
    char host[MAX_PATH * 2];
    char pattern[MAX_PATH * 2];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    int count = 0;

    if (WD_FSResolve(path, host, sizeof(host)) != 0)
        return -1;
    lstrcpynA(pattern, host, sizeof(pattern));
    lstrcatA(pattern, "\\*");

    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return -1;

    do
    {
        BOOL is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? TRUE : FALSE;
        if (is_dir == (want_dirs ? TRUE : FALSE) && count < max_entries)
        {
            size_t copy = name_max;
            if (copy > sizeof(fd.cFileName))
                copy = sizeof(fd.cFileName);
            lstrcpynA(out_names + (size_t)count * name_max, fd.cFileName, (int)copy);
            count++;
        }
    } while (FindNextFileA(h, &fd) && count < max_entries);

    FindClose(h);
    return count;
}
