/*
 * PROJECT:     WinDosDX Explorer
 * LICENSE:     LGPL-2.1-or-later (https://spdx.org/licenses/LGPL-2.1-or-later)
 * PURPOSE:     Windows 7-style two-pane Start menu
 * COPYRIGHT:   Copyright 2026 WinDosDX Team & Contributors
 */

/*
 * A self-contained Start menu window that implements just enough of
 * IMenuPopup for CTrayWindow to drive it (Popup / OnSelect / GetWindow).
 * It is used instead of the classic shell32 menu band when the shell
 * setting SSF_STARTPANELON is on (see CreateStartMenu in startmnu.cpp).
 *
 * Everything is drawn with plain GDI into a back buffer so the menu looks
 * the same with or without a visual style. There is no DWM, so the Aero
 * "glass" is imitated with gradients.
 *
 * Left pane : pinned programs, frequently used programs, "All Programs"
 *             (an inline, scrollable tree of the Start Menu\Programs folders).
 * Right pane: user picture, personal folders and system links, and a
 *             "Shut down" split button.
 *
 * Settings and usage data live under HKCU\Software\WinDosDX\Explorer\StartPage.
 */

#include "precomp.h"
#include <windowsx.h>
#include <reason.h>
#include <powrprof.h>

/* ------------------------------------------------------------------------ */
/* Settings and usage data                                                  */
/* ------------------------------------------------------------------------ */

static const WCHAR c_szStartPageKey[] = L"Software\\WinDosDX\\Explorer\\StartPage";
static const WCHAR c_szMfuKey[] = L"Software\\WinDosDX\\Explorer\\StartPage\\MFU";

#define SM7_DEFAULT_PROGRAM_COUNT 10
#define SM7_MAX_PROGRAM_COUNT     30

typedef struct _SM7_MFU_RECORD
{
    DWORD dwCount;
    FILETIME ftLastUsed;
} SM7_MFU_RECORD;

static DWORD
SM7_GetDword(LPCWSTR pszValue, DWORD dwDefault)
{
    DWORD dwValue, cbValue = sizeof(dwValue);
    if (SHGetValueW(HKEY_CURRENT_USER, c_szStartPageKey, pszValue, NULL,
                    &dwValue, &cbValue) != ERROR_SUCCESS)
    {
        return dwDefault;
    }
    return dwValue;
}

static VOID
SM7_SetDword(LPCWSTR pszValue, DWORD dwValue)
{
    SHSetValueW(HKEY_CURRENT_USER, c_szStartPageKey, pszValue, REG_DWORD,
                &dwValue, sizeof(dwValue));
}

DWORD
StartMenu7_GetProgramCount()
{
    DWORD dwCount = SM7_GetDword(L"ProgramCount", SM7_DEFAULT_PROGRAM_COUNT);
    return min(dwCount, SM7_MAX_PROGRAM_COUNT);
}

VOID
StartMenu7_SetProgramCount(DWORD dwCount)
{
    SM7_SetDword(L"ProgramCount", min(dwCount, SM7_MAX_PROGRAM_COUNT));
}

BOOL
StartMenu7_GetLargeIcons()
{
    return SM7_GetDword(L"LargeIcons", TRUE) != 0;
}

VOID
StartMenu7_SetLargeIcons(BOOL bLarge)
{
    SM7_SetDword(L"LargeIcons", bLarge ? 1 : 0);
}

VOID
StartMenu7_ClearFrequent()
{
    SHDeleteKeyW(HKEY_CURRENT_USER, c_szMfuKey);
    /* Keep "Seeded" so the default programs do not come back. */
    SM7_SetDword(L"Seeded", 1);
}

static VOID
SM7_RecordLaunch(LPCWSTR pszPath)
{
    SM7_MFU_RECORD rec = { 0 };
    DWORD cb = sizeof(rec);
    if (SHGetValueW(HKEY_CURRENT_USER, c_szMfuKey, pszPath, NULL, &rec, &cb) != ERROR_SUCCESS ||
        cb != sizeof(rec))
    {
        ZeroMemory(&rec, sizeof(rec));
    }
    rec.dwCount++;
    GetSystemTimeAsFileTime(&rec.ftLastUsed);
    SHSetValueW(HKEY_CURRENT_USER, c_szMfuKey, pszPath, REG_BINARY, &rec, sizeof(rec));
}

static VOID
SM7_RemoveFrequent(LPCWSTR pszPath)
{
    SHDeleteValueW(HKEY_CURRENT_USER, c_szMfuKey, pszPath);
}

/* Pinned items: REG_MULTI_SZ "Pinned", in display order. */
static VOID
SM7_LoadPinned(CAtlArray<CStringW>& pinned)
{
    pinned.SetCount(0);

    DWORD cb = 0, dwType;
    if (SHGetValueW(HKEY_CURRENT_USER, c_szStartPageKey, L"Pinned", &dwType, NULL, &cb) != ERROR_SUCCESS ||
        dwType != REG_MULTI_SZ || cb < sizeof(WCHAR))
    {
        return;
    }

    CStringW buffer;
    DWORD cch = cb / sizeof(WCHAR) + 2;
    LPWSTR psz = buffer.GetBuffer(cch);
    ZeroMemory(psz, cch * sizeof(WCHAR));
    if (SHGetValueW(HKEY_CURRENT_USER, c_szStartPageKey, L"Pinned", NULL, psz, &cb) == ERROR_SUCCESS)
    {
        for (LPCWSTR p = psz; *p; p += wcslen(p) + 1)
            pinned.Add(CStringW(p));
    }
    buffer.ReleaseBuffer(0);
}

static VOID
SM7_SavePinned(const CAtlArray<CStringW>& pinned)
{
    size_t cch = 1;
    for (size_t i = 0; i < pinned.GetCount(); ++i)
        cch += pinned[i].GetLength() + 1;

    CStringW buffer;
    LPWSTR psz = buffer.GetBuffer((int)cch);
    LPWSTR p = psz;
    for (size_t i = 0; i < pinned.GetCount(); ++i)
    {
        wcscpy(p, pinned[i]);
        p += pinned[i].GetLength() + 1;
    }
    *p = UNICODE_NULL;
    SHSetValueW(HKEY_CURRENT_USER, c_szStartPageKey, L"Pinned", REG_MULTI_SZ,
                psz, (DWORD)(cch * sizeof(WCHAR)));
    buffer.ReleaseBuffer(0);
}

static BOOL
SM7_IsPinned(const CAtlArray<CStringW>& pinned, LPCWSTR pszPath)
{
    for (size_t i = 0; i < pinned.GetCount(); ++i)
    {
        if (pinned[i].CompareNoCase(pszPath) == 0)
            return TRUE;
    }
    return FALSE;
}

static VOID
SM7_SetPinned(LPCWSTR pszPath, BOOL bPin)
{
    CAtlArray<CStringW> pinned, result;
    SM7_LoadPinned(pinned);
    for (size_t i = 0; i < pinned.GetCount(); ++i)
    {
        if (pinned[i].CompareNoCase(pszPath) != 0)
            result.Add(pinned[i]);
    }
    if (bPin)
        result.Add(CStringW(pszPath));
    SM7_SavePinned(result);
}

/* Find a Start Menu shortcut whose target is pszTarget (shortcut names are
 * localized, targets are not). Searches at most two folder levels deep. */
static BOOL
SM7_FindShortcutInDir(LPCWSTR pszDir, LPCWSTR pszTarget, IShellLinkW *psl,
                      IPersistFile *ppf, int nDepth, CStringW& result)
{
    WCHAR szPattern[MAX_PATH];
    StringCchCopyW(szPattern, _countof(szPattern), pszDir);
    PathAppendW(szPattern, L"*");

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(szPattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE)
        return FALSE;

    BOOL bFound = FALSE;
    do
    {
        if (fd.cFileName[0] == L'.')
            continue;

        WCHAR szPath[MAX_PATH];
        StringCchCopyW(szPath, _countof(szPath), pszDir);
        PathAppendW(szPath, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (nDepth > 0)
                bFound = SM7_FindShortcutInDir(szPath, pszTarget, psl, ppf, nDepth - 1, result);
        }
        else if (!_wcsicmp(PathFindExtensionW(fd.cFileName), L".lnk"))
        {
            WCHAR szLinkTarget[MAX_PATH];
            if (SUCCEEDED(ppf->Load(szPath, STGM_READ)) &&
                SUCCEEDED(psl->GetPath(szLinkTarget, _countof(szLinkTarget), NULL, 0)) &&
                !_wcsicmp(szLinkTarget, pszTarget))
            {
                result = szPath;
                bFound = TRUE;
            }
        }
    } while (!bFound && FindNextFileW(hFind, &fd));

    FindClose(hFind);
    return bFound;
}

static CStringW
SM7_ResolveDefaultProgram(LPCWSTR pszExe)
{
    WCHAR szTarget[MAX_PATH];
    ExpandEnvironmentStringsW(pszExe, szTarget, _countof(szTarget));

    CComPtr<IShellLinkW> psl;
    CComPtr<IPersistFile> ppf;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARG(IShellLinkW, &psl))) &&
        SUCCEEDED(psl->QueryInterface(IID_PPV_ARG(IPersistFile, &ppf))))
    {
        static const int csidls[] = { CSIDL_COMMON_PROGRAMS, CSIDL_PROGRAMS };
        for (size_t i = 0; i < _countof(csidls); ++i)
        {
            WCHAR szDir[MAX_PATH];
            CStringW found;
            if (SUCCEEDED(SHGetFolderPathW(NULL, csidls[i], NULL, 0, szDir)) &&
                SM7_FindShortcutInDir(szDir, szTarget, psl, ppf, 2, found))
            {
                return found;
            }
        }
    }

    return CStringW(szTarget);
}

/* On first use, seed the frequent list like a fresh Windows 7 profile. */
static VOID
SM7_SeedDefaults()
{
    if (SM7_GetDword(L"Seeded", 0))
        return;

    static const LPCWSTR defaults[] =
    {
        L"%SystemRoot%\\system32\\cmd.exe",
        L"%SystemRoot%\\system32\\notepad.exe",
        L"%SystemRoot%\\system32\\mspaint.exe",
        L"%SystemRoot%\\system32\\calc.exe",
    };

    for (size_t i = 0; i < _countof(defaults); ++i)
    {
        CStringW path = SM7_ResolveDefaultProgram(defaults[i]);
        if (!PathFileExistsW(path))
            continue;

        /* Descending counts keep the listed order on first display. */
        SM7_MFU_RECORD rec;
        rec.dwCount = (DWORD)(_countof(defaults) - i);
        GetSystemTimeAsFileTime(&rec.ftLastUsed);
        SHSetValueW(HKEY_CURRENT_USER, c_szMfuKey, path, REG_BINARY, &rec, sizeof(rec));
    }

    SM7_SetDword(L"Seeded", 1);
}

struct SM7_MfuEntry
{
    CStringW path;
    SM7_MFU_RECORD rec;
};

static VOID
SM7_LoadFrequent(CAtlArray<SM7_MfuEntry>& entries)
{
    entries.SetCount(0);

    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, c_szMfuKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return;

    for (DWORD i = 0; ; ++i)
    {
        WCHAR szName[MAX_PATH];
        DWORD cchName = _countof(szName);
        SM7_MFU_RECORD rec;
        DWORD cb = sizeof(rec), dwType;
        LONG lErr = RegEnumValueW(hKey, i, szName, &cchName, NULL, &dwType, (LPBYTE)&rec, &cb);
        if (lErr == ERROR_NO_MORE_ITEMS)
            break;
        if (lErr != ERROR_SUCCESS || dwType != REG_BINARY || cb != sizeof(rec))
            continue;

        SM7_MfuEntry entry;
        entry.path = szName;
        entry.rec = rec;
        entries.Add(entry);
    }
    RegCloseKey(hKey);

    /* Most launched first, then most recent. Lists are short: insertion sort. */
    for (size_t i = 1; i < entries.GetCount(); ++i)
    {
        for (size_t j = i; j > 0; --j)
        {
            const SM7_MfuEntry& a = entries[j - 1];
            const SM7_MfuEntry& b = entries[j];
            BOOL bSwap = (b.rec.dwCount > a.rec.dwCount) ||
                         (b.rec.dwCount == a.rec.dwCount &&
                          CompareFileTime(&b.rec.ftLastUsed, &a.rec.ftLastUsed) > 0);
            if (!bSwap)
                break;
            SM7_MfuEntry tmp = entries[j - 1];
            entries[j - 1] = entries[j];
            entries[j] = tmp;
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Drawing helpers                                                          */
/* ------------------------------------------------------------------------ */

static VOID
SM7_VGradient(HDC hdc, const RECT& rc, COLORREF top, COLORREF bottom)
{
    if (rc.right <= rc.left || rc.bottom <= rc.top)
        return;

    TRIVERTEX v[2];
    v[0].x = rc.left;
    v[0].y = rc.top;
    v[0].Red = (COLOR16)(GetRValue(top) << 8);
    v[0].Green = (COLOR16)(GetGValue(top) << 8);
    v[0].Blue = (COLOR16)(GetBValue(top) << 8);
    v[0].Alpha = 0;
    v[1].x = rc.right;
    v[1].y = rc.bottom;
    v[1].Red = (COLOR16)(GetRValue(bottom) << 8);
    v[1].Green = (COLOR16)(GetGValue(bottom) << 8);
    v[1].Blue = (COLOR16)(GetBValue(bottom) << 8);
    v[1].Alpha = 0;
    GRADIENT_RECT gr = { 0, 1 };
    GradientFill(hdc, v, 2, &gr, 1, GRADIENT_FILL_RECT_V);
}

static VOID
SM7_RoundFrame(HDC hdc, const RECT& rc, COLORREF color, int radius)
{
    HPEN hPen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ hOldPen = SelectObject(hdc, hPen);
    HGDIOBJ hOldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(hdc, hOldBrush);
    SelectObject(hdc, hOldPen);
    DeleteObject(hPen);
}

static VOID
SM7_HLine(HDC hdc, int x1, int x2, int y, COLORREF color)
{
    HPEN hPen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ hOld = SelectObject(hdc, hPen);
    MoveToEx(hdc, x1, y, NULL);
    LineTo(hdc, x2, y);
    SelectObject(hdc, hOld);
    DeleteObject(hPen);
}

static VOID
SM7_Triangle(HDC hdc, int x, int y, int size, BOOL bPointRight, COLORREF color)
{
    POINT pts[3];
    if (bPointRight)
    {
        pts[0].x = x;            pts[0].y = y - size;
        pts[1].x = x + size;     pts[1].y = y;
        pts[2].x = x;            pts[2].y = y + size;
    }
    else
    {
        pts[0].x = x + size;     pts[0].y = y - size;
        pts[1].x = x;            pts[1].y = y;
        pts[2].x = x + size;     pts[2].y = y + size;
    }
    HBRUSH hBrush = CreateSolidBrush(color);
    HPEN hPen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ hOldBrush = SelectObject(hdc, hBrush);
    HGDIOBJ hOldPen = SelectObject(hdc, hPen);
    Polygon(hdc, pts, 3);
    SelectObject(hdc, hOldPen);
    SelectObject(hdc, hOldBrush);
    DeleteObject(hPen);
    DeleteObject(hBrush);
}

/* The WinDosDX mark: a rounded square split diagonally light/dark blue. */
static VOID
SM7_DrawBrandMark(HDC hdc, const RECT& rc)
{
    HRGN hRgn = CreateRoundRectRgn(rc.left, rc.top, rc.right + 1, rc.bottom + 1, 8, 8);
    SelectClipRgn(hdc, hRgn);

    POINT upper[3] = { { rc.left, rc.top }, { rc.right, rc.top }, { rc.left, rc.bottom } };
    POINT lower[3] = { { rc.right, rc.top }, { rc.right, rc.bottom }, { rc.left, rc.bottom } };
    HPEN hNull = (HPEN)GetStockObject(NULL_PEN);
    HGDIOBJ hOldPen = SelectObject(hdc, hNull);

    HBRUSH hLight = CreateSolidBrush(RGB(127, 175, 212));
    HGDIOBJ hOldBrush = SelectObject(hdc, hLight);
    Polygon(hdc, upper, 3);
    HBRUSH hDark = CreateSolidBrush(RGB(57, 107, 154));
    SelectObject(hdc, hDark);
    Polygon(hdc, lower, 3);

    SelectObject(hdc, hOldBrush);
    SelectObject(hdc, hOldPen);
    DeleteObject(hLight);
    DeleteObject(hDark);
    SelectClipRgn(hdc, NULL);
    DeleteObject(hRgn);
}

static VOID
SM7_EnableShutdownPrivilege()
{
    HANDLE hToken;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        return;

    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueW(NULL, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid))
        AdjustTokenPrivileges(hToken, FALSE, &tp, 0, NULL, NULL);
    CloseHandle(hToken);
}

/* ------------------------------------------------------------------------ */
/* The menu window                                                          */
/* ------------------------------------------------------------------------ */

enum SM7_ITEM_KIND
{
    SM7_PROGRAM,        /* pinned or frequent program (left pane) */
    SM7_ALLPROGRAMS,    /* "All Programs" row */
    SM7_BACK,           /* "Back" row in the All Programs view */
    SM7_TREE_FILE,      /* file inside the All Programs tree */
    SM7_TREE_FOLDER,    /* folder inside the All Programs tree */
    SM7_TREE_EMPTY,     /* "(Empty)" placeholder for an empty folder */
    SM7_LINK_FOLDER,    /* right pane: a shell folder (CSIDL) */
    SM7_LINK_COMMAND,   /* right pane: a tray command */
    SM7_SHUTDOWN,       /* Shut down button */
    SM7_SHUTDOWN_MENU,  /* the arrow next to it */
};

struct SM7_Item
{
    SM7_ITEM_KIND kind;
    CStringW name;
    CStringW path;      /* program path, or first folder for tree folders */
    CStringW path2;     /* second (All Users) folder merged into a tree folder */
    int iIcon;
    int nDepth;         /* tree indentation */
    BOOL bExpanded;
    BOOL bPinned;
    BOOL bFrequent;
    UINT uValue;        /* CSIDL or TRAYCMD_* */
    RECT rc;            /* layout rectangle (scrolled for tree rows) */

    SM7_Item() :
        kind(SM7_PROGRAM), iIcon(-1), nDepth(0), bExpanded(FALSE),
        bPinned(FALSE), bFrequent(FALSE), uValue(0)
    {
        SetRectEmpty(&rc);
    }
};

#define SM7_CMD_OPEN          1
#define SM7_CMD_OPENLOCATION  2
#define SM7_CMD_PIN           3
#define SM7_CMD_UNPIN         4
#define SM7_CMD_REMOVE        5
#define SM7_CMD_PROPERTIES    6
#define SM7_CMD_EXPLORE       7

#define SM7_EXIT_SWITCHUSER   100
#define SM7_EXIT_LOGOFF       101
#define SM7_EXIT_LOCK         102
#define SM7_EXIT_RESTART      103
#define SM7_EXIT_SLEEP        104
#define SM7_EXIT_DIALOG       105

class CStartMenu7 :
    public CComCoClass<CStartMenu7>,
    public CComObjectRootEx<CComMultiThreadModelNoCS>,
    public CWindowImpl<CStartMenu7, CWindow,
                       CWinTraits<WS_POPUP | WS_CLIPCHILDREN, WS_EX_TOOLWINDOW | WS_EX_TOPMOST> >,
    public IMenuPopup
{
    CComPtr<ITrayWindow> m_Tray;

    /* Items in both panes; m_nLeftEnd..m_items.GetCount() is the right pane. */
    CAtlArray<SM7_Item> m_items;
    size_t m_nLeftEnd;
    int m_nHot;
    BOOL m_bAllPrograms;
    CAtlArray<SM7_Item> m_tree;   /* flattened, display-ordered tree */
    int m_nScroll;
    int m_nTreeContent;
    RECT m_rcTreeView;
    CAtlArray<int> m_separators;  /* y positions of separators in the left pane */
    CAtlArray<int> m_rightSeparators;

    int m_dpi;
    int m_cx, m_cy;
    RECT m_rcBody, m_rcLeft, m_rcTile, m_rcExclude;
    HFONT m_hFont;
    HFONT m_hFontBold;
    HIMAGELIST m_himlLarge;
    HIMAGELIST m_himlSmall;
    HBITMAP m_hbmUser;
    CStringW m_userName;
    DWORD m_dwSuppressTick;
    BOOL m_bTracking;

    int S(int value) const { return MulDiv(value, m_dpi, 96); }

public:
    DECLARE_WND_CLASS_EX(L"WinDosDX_StartMenu7", CS_DROPSHADOW, COLOR_WINDOW)

    CStartMenu7() :
        m_nLeftEnd(0), m_nHot(-1), m_bAllPrograms(FALSE), m_nScroll(0), m_nTreeContent(0),
        m_dpi(96), m_cx(0), m_cy(0), m_hFont(NULL), m_hFontBold(NULL),
        m_himlLarge(NULL), m_himlSmall(NULL), m_hbmUser(NULL), m_dwSuppressTick(0),
        m_bTracking(FALSE)
    {
        SetRectEmpty(&m_rcBody);
        SetRectEmpty(&m_rcLeft);
        SetRectEmpty(&m_rcTile);
        SetRectEmpty(&m_rcExclude);
        SetRectEmpty(&m_rcTreeView);
    }

    virtual ~CStartMenu7()
    {
        if (m_hWnd)
            DestroyWindow();
        if (m_hFont)
            DeleteObject(m_hFont);
        if (m_hFontBold)
            DeleteObject(m_hFontBold);
        if (m_hbmUser)
            DeleteObject(m_hbmUser);
    }

    HRESULT Initialize(IN ITrayWindow *Tray)
    {
        m_Tray = Tray;

        HDC hdc = ::GetDC(NULL);
        m_dpi = GetDeviceCaps(hdc, LOGPIXELSY);
        ::ReleaseDC(NULL, hdc);

        NONCLIENTMETRICSW ncm = { sizeof(ncm) };
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        LOGFONTW lf = ncm.lfMessageFont;
        lf.lfHeight = -MulDiv(9, m_dpi, 72);
        m_hFont = CreateFontIndirectW(&lf);
        lf.lfWeight = FW_BOLD;
        m_hFontBold = CreateFontIndirectW(&lf);

        SHFILEINFOW sfi;
        /* SHGFI_USEFILEATTRIBUTES: the path need not exist (a live CD has no C:). */
        m_himlLarge = (HIMAGELIST)SHGetFileInfoW(L"file", FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                                                 SHGFI_SYSICONINDEX | SHGFI_LARGEICON |
                                                 SHGFI_USEFILEATTRIBUTES);
        m_himlSmall = (HIMAGELIST)SHGetFileInfoW(L"file", FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi),
                                                 SHGFI_SYSICONINDEX | SHGFI_SMALLICON |
                                                 SHGFI_USEFILEATTRIBUTES);

        WCHAR szUser[256 + 1];
        DWORD cchUser = _countof(szUser);
        if (GetUserNameW(szUser, &cchUser))
            m_userName = szUser;

        LoadUserPicture();

        m_cx = S(420);
        m_cy = S(540);

        Create(NULL);
        if (!m_hWnd)
            return E_FAIL;

        return S_OK;
    }

    /* Optional per-user picture: the first image in the Windows-compatible
     * "User Account Pictures" folder under All Users application data. */
    VOID LoadUserPicture()
    {
        WCHAR szDir[MAX_PATH];
        if (FAILED(SHGetFolderPathW(NULL, CSIDL_COMMON_APPDATA, NULL, 0, szDir)))
            return;
        PathAppendW(szDir, L"Microsoft\\User Account Pictures");

        WCHAR szPath[MAX_PATH];
        StringCchPrintfW(szPath, _countof(szPath), L"%s\\%s.bmp", szDir, m_userName.GetString());
        if (!PathFileExistsW(szPath))
        {
            StringCchPrintfW(szPath, _countof(szPath), L"%s\\Default Pictures\\*.bmp", szDir);
            WIN32_FIND_DATAW fd;
            HANDLE hFind = FindFirstFileW(szPath, &fd);
            if (hFind == INVALID_HANDLE_VALUE)
                return;
            FindClose(hFind);
            StringCchPrintfW(szPath, _countof(szPath), L"%s\\Default Pictures\\%s", szDir, fd.cFileName);
        }

        m_hbmUser = (HBITMAP)LoadImageW(NULL, szPath, IMAGE_BITMAP, 0, 0,
                                        LR_LOADFROMFILE | LR_CREATEDIBSECTION);
    }

    /* -------------------------------------------------------------------- */
    /* Model                                                                */
    /* -------------------------------------------------------------------- */

    static CStringW GetDisplayName(LPCWSTR pszPath)
    {
        SHFILEINFOW sfi;
        if (SHGetFileInfoW(pszPath, 0, &sfi, sizeof(sfi), SHGFI_DISPLAYNAME))
            return CStringW(sfi.szDisplayName);
        return CStringW(PathFindFileNameW(pszPath));
    }

    static int GetIconIndex(LPCWSTR pszPath)
    {
        SHFILEINFOW sfi;
        if (SHGetFileInfoW(pszPath, 0, &sfi, sizeof(sfi), SHGFI_SYSICONINDEX))
            return sfi.iIcon;
        return -1;
    }

    static int GetFolderIconIndex(int csidl)
    {
        CComHeapPtr<ITEMIDLIST> pidl;
        SHFILEINFOW sfi;
        if (SUCCEEDED(SHGetSpecialFolderLocation(NULL, csidl, &pidl)) &&
            SHGetFileInfoW((LPCWSTR)(LPCITEMIDLIST)pidl, 0, &sfi, sizeof(sfi),
                           SHGFI_PIDL | SHGFI_SYSICONINDEX))
        {
            return sfi.iIcon;
        }
        return -1;
    }

    VOID AddRightLink(SM7_ITEM_KIND kind, UINT uNameId, UINT uValue, int iIcon)
    {
        SM7_Item item;
        item.kind = kind;
        item.name.LoadStringW(hExplorerInstance, uNameId);
        item.uValue = uValue;
        item.iIcon = iIcon;
        m_items.Add(item);
    }

    VOID BuildItems()
    {
        m_items.SetCount(0);
        m_separators.SetCount(0);
        m_rightSeparators.SetCount(0);

        if (m_bAllPrograms)
        {
            SM7_Item back;
            back.kind = SM7_BACK;
            back.name.LoadStringW(hExplorerInstance, IDS_SM7_BACK);
            m_items.Add(back);
        }
        else
        {
            SM7_SeedDefaults();

            CAtlArray<CStringW> pinned;
            SM7_LoadPinned(pinned);
            for (size_t i = 0; i < pinned.GetCount(); ++i)
            {
                if (!PathFileExistsW(pinned[i]))
                    continue;
                SM7_Item item;
                item.kind = SM7_PROGRAM;
                item.path = pinned[i];
                item.name = GetDisplayName(pinned[i]);
                item.iIcon = GetIconIndex(pinned[i]);
                item.bPinned = TRUE;
                m_items.Add(item);
            }
            size_t nPinned = m_items.GetCount();

            CAtlArray<SM7_MfuEntry> frequent;
            SM7_LoadFrequent(frequent);
            DWORD nMax = StartMenu7_GetProgramCount();
            DWORD nAdded = 0;
            for (size_t i = 0; i < frequent.GetCount() && nAdded < nMax; ++i)
            {
                const CStringW& path = frequent[i].path;
                if (SM7_IsPinned(pinned, path) || !PathFileExistsW(path))
                    continue;
                SM7_Item item;
                item.kind = SM7_PROGRAM;
                item.path = path;
                item.name = GetDisplayName(path);
                item.iIcon = GetIconIndex(path);
                item.bFrequent = TRUE;
                m_items.Add(item);
                nAdded++;
            }

            /* A separator goes between pinned and frequent items. */
            if (nPinned && nAdded)
                m_separators.Add((int)nPinned);

            SM7_Item all;
            all.kind = SM7_ALLPROGRAMS;
            all.name.LoadStringW(hExplorerInstance, IDS_SM7_ALLPROGRAMS);
            m_items.Add(all);
        }

        m_nLeftEnd = m_items.GetCount();

        /* Right pane */
        SM7_Item user;
        user.kind = SM7_LINK_FOLDER;
        user.name = m_userName;
        user.uValue = CSIDL_PROFILE;
        user.iIcon = GetFolderIconIndex(CSIDL_PROFILE);
        m_items.Add(user);
        AddRightLink(SM7_LINK_FOLDER, IDS_SM7_DOCUMENTS, CSIDL_PERSONAL, GetFolderIconIndex(CSIDL_PERSONAL));
        AddRightLink(SM7_LINK_FOLDER, IDS_SM7_PICTURES, CSIDL_MYPICTURES, GetFolderIconIndex(CSIDL_MYPICTURES));
        AddRightLink(SM7_LINK_FOLDER, IDS_SM7_MUSIC, CSIDL_MYMUSIC, GetFolderIconIndex(CSIDL_MYMUSIC));
        m_rightSeparators.Add((int)(m_items.GetCount() - m_nLeftEnd));
        AddRightLink(SM7_LINK_FOLDER, IDS_SM7_COMPUTER, CSIDL_DRIVES, GetFolderIconIndex(CSIDL_DRIVES));
        if (!SHRestricted(REST_NOSETFOLDERS))
        {
            AddRightLink(SM7_LINK_COMMAND, IDS_SM7_CONTROLPANEL, TRAYCMD_CONTROL_PANEL,
                         GetFolderIconIndex(CSIDL_CONTROLS));
            AddRightLink(SM7_LINK_COMMAND, IDS_SM7_DEVICES, TRAYCMD_PRINTERS_AND_FAXES,
                         GetFolderIconIndex(CSIDL_PRINTERS));
        }
        m_rightSeparators.Add((int)(m_items.GetCount() - m_nLeftEnd));
        AddRightLink(SM7_LINK_COMMAND, IDS_SM7_HELP, TRAYCMD_HELP_AND_SUPPORT, -1);
        if (!SHRestricted(REST_NORUN))
            AddRightLink(SM7_LINK_COMMAND, IDS_SM7_RUN, TRAYCMD_RUN_DIALOG, -1);

        SM7_Item shutdown;
        shutdown.kind = SM7_SHUTDOWN;
        shutdown.name.LoadStringW(hExplorerInstance, IDS_SM7_SHUTDOWN);
        m_items.Add(shutdown);
        SM7_Item arrow;
        arrow.kind = SM7_SHUTDOWN_MENU;
        m_items.Add(arrow);
    }

    /* Enumerate one level of the merged Programs tree (user + All Users). */
    VOID EnumTreeLevel(LPCWSTR pszDir1, LPCWSTR pszDir2, int nDepth, CAtlArray<SM7_Item>& out)
    {
        CAtlArray<SM7_Item> files, folders;
        LPCWSTR dirs[2] = { pszDir1, pszDir2 };

        for (int d = 0; d < 2; ++d)
        {
            if (!dirs[d] || !*dirs[d])
                continue;

            WCHAR szPattern[MAX_PATH];
            StringCchCopyW(szPattern, _countof(szPattern), dirs[d]);
            PathAppendW(szPattern, L"*");

            WIN32_FIND_DATAW fd;
            HANDLE hFind = FindFirstFileW(szPattern, &fd);
            if (hFind == INVALID_HANDLE_VALUE)
                continue;
            do
            {
                if (fd.cFileName[0] == L'.' ||
                    (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))
                {
                    continue;
                }

                WCHAR szPath[MAX_PATH];
                StringCchCopyW(szPath, _countof(szPath), dirs[d]);
                PathAppendW(szPath, fd.cFileName);
                BOOL bFolder = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                CAtlArray<SM7_Item>& list = bFolder ? folders : files;
                CStringW name = GetDisplayName(szPath);

                /* Merge same-named entries from the two roots. */
                BOOL bMerged = FALSE;
                for (size_t i = 0; i < list.GetCount(); ++i)
                {
                    if (list[i].name.CompareNoCase(name) == 0)
                    {
                        if (bFolder)
                            list[i].path2 = szPath;
                        bMerged = TRUE;
                        break;
                    }
                }
                if (bMerged)
                    continue;

                SM7_Item item;
                item.kind = bFolder ? SM7_TREE_FOLDER : SM7_TREE_FILE;
                item.name = name;
                item.path = szPath;
                item.iIcon = GetIconIndex(szPath);
                item.nDepth = nDepth;
                list.Add(item);
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }

        /* Windows 7 lists shortcuts first, then folders, each alphabetically. */
        SortByName(files);
        SortByName(folders);
        for (size_t i = 0; i < files.GetCount(); ++i)
            out.Add(files[i]);
        for (size_t i = 0; i < folders.GetCount(); ++i)
            out.Add(folders[i]);
    }

    static VOID SortByName(CAtlArray<SM7_Item>& list)
    {
        for (size_t i = 1; i < list.GetCount(); ++i)
        {
            for (size_t j = i; j > 0 && list[j].name.CompareNoCase(list[j - 1].name) < 0; --j)
            {
                SM7_Item tmp = list[j - 1];
                list[j - 1] = list[j];
                list[j] = tmp;
            }
        }
    }

    VOID BuildTreeRoot()
    {
        WCHAR szUser[MAX_PATH] = L"", szCommon[MAX_PATH] = L"";
        SHGetFolderPathW(NULL, CSIDL_PROGRAMS, NULL, 0, szUser);
        SHGetFolderPathW(NULL, CSIDL_COMMON_PROGRAMS, NULL, 0, szCommon);
        m_tree.SetCount(0);
        EnumTreeLevel(szUser, szCommon, 0, m_tree);
        m_nScroll = 0;
    }

    VOID ToggleTreeFolder(size_t index)
    {
        CAtlArray<SM7_Item> result;
        SM7_Item& folder = m_tree[index];
        int nDepth = folder.nDepth;

        for (size_t i = 0; i <= index; ++i)
            result.Add(m_tree[i]);
        result[index].bExpanded = !folder.bExpanded;

        size_t next = index + 1;
        if (result[index].bExpanded)
        {
            CAtlArray<SM7_Item> children;
            EnumTreeLevel(folder.path, folder.path2, nDepth + 1, children);
            if (children.GetCount() == 0)
            {
                SM7_Item empty;
                empty.kind = SM7_TREE_EMPTY;
                empty.name.LoadStringW(hExplorerInstance, IDS_SM7_EMPTY);
                empty.nDepth = nDepth + 1;
                children.Add(empty);
            }
            for (size_t i = 0; i < children.GetCount(); ++i)
                result.Add(children[i]);
        }
        else
        {
            /* Collapse: drop everything nested below this folder. */
            while (next < m_tree.GetCount() && m_tree[next].nDepth > nDepth)
                next++;
        }

        for (size_t i = next; i < m_tree.GetCount(); ++i)
            result.Add(m_tree[i]);

        m_tree.SetCount(0);
        for (size_t i = 0; i < result.GetCount(); ++i)
            m_tree.Add(result[i]);
    }

    /* -------------------------------------------------------------------- */
    /* Layout                                                               */
    /* -------------------------------------------------------------------- */

    int RowHeight() const
    {
        return StartMenu7_GetLargeIcons() ? S(38) : S(26);
    }

    int IconSize() const
    {
        return StartMenu7_GetLargeIcons() ? 32 : 16;
    }

    VOID Layout()
    {
        const int overhang = S(26);
        const int margin = S(7);
        const int leftW = S(252);

        m_rcBody.left = 0;
        m_rcBody.top = overhang;
        m_rcBody.right = m_cx;
        m_rcBody.bottom = m_cy;

        m_rcLeft.left = margin;
        m_rcLeft.top = m_rcBody.top + margin;
        m_rcLeft.right = margin + leftW;
        m_rcLeft.bottom = m_cy - margin;

        int rightW = m_cx - m_rcLeft.right - margin;
        int tile = S(60);
        m_rcTile.left = m_rcLeft.right + (rightW - tile) / 2;
        m_rcTile.top = 0;
        m_rcTile.right = m_rcTile.left + tile;
        m_rcTile.bottom = tile;

        /* Left pane */
        int bottomRowH = S(32);
        RECT rcBottom = { m_rcLeft.left + S(3), m_rcLeft.bottom - S(3) - bottomRowH,
                          m_rcLeft.right - S(3), m_rcLeft.bottom - S(3) };

        if (m_bAllPrograms)
        {
            m_items[0].rc = rcBottom;
            m_rcTreeView.left = m_rcLeft.left + S(3);
            m_rcTreeView.top = m_rcLeft.top + S(4);
            m_rcTreeView.right = m_rcLeft.right - S(3);
            m_rcTreeView.bottom = rcBottom.top - S(8);

            int rowH = S(24);
            m_nTreeContent = (int)m_tree.GetCount() * rowH;
            int maxScroll = max(0, m_nTreeContent - (m_rcTreeView.bottom - m_rcTreeView.top));
            m_nScroll = max(0, min(m_nScroll, maxScroll));
            for (size_t i = 0; i < m_tree.GetCount(); ++i)
            {
                RECT& rc = m_tree[i].rc;
                rc.left = m_rcTreeView.left;
                rc.right = m_rcTreeView.right - S(8);
                rc.top = m_rcTreeView.top + (int)i * rowH - m_nScroll;
                rc.bottom = rc.top + rowH;
            }
        }
        else
        {
            int y = m_rcLeft.top + S(4);
            int rowH = RowHeight();
            int limit = rcBottom.top - S(8);
            size_t nextSep = 0;
            for (size_t i = 0; i + 1 < m_nLeftEnd; ++i)
            {
                if (nextSep < m_separators.GetCount() && (size_t)m_separators[nextSep] == i)
                {
                    y += S(9);
                    nextSep++;
                }
                RECT& rc = m_items[i].rc;
                if (y + rowH > limit)
                {
                    SetRectEmpty(&rc);   /* does not fit; hidden */
                    continue;
                }
                rc.left = m_rcLeft.left + S(3);
                rc.right = m_rcLeft.right - S(3);
                rc.top = y;
                rc.bottom = y + rowH;
                y += rowH;
            }
            m_items[m_nLeftEnd - 1].rc = rcBottom;
        }

        /* Right pane */
        int x = m_rcLeft.right + S(6);
        int w = rightW - S(12);
        int y = m_rcTile.bottom + S(10);
        int linkH = S(30);
        size_t nextSep = 0;
        size_t count = m_items.GetCount();
        for (size_t i = m_nLeftEnd; i + 2 < count; ++i)
        {
            if (nextSep < m_rightSeparators.GetCount() &&
                (size_t)m_rightSeparators[nextSep] == i - m_nLeftEnd)
            {
                y += S(9);
                nextSep++;
            }
            RECT& rc = m_items[i].rc;
            rc.left = x;
            rc.right = x + w;
            rc.top = y;
            rc.bottom = y + linkH;
            y += linkH;
        }

        /* Shut down split button */
        RECT& rcShut = m_items[count - 2].rc;
        RECT& rcArrow = m_items[count - 1].rc;
        rcShut.left = x + S(4);
        rcShut.bottom = m_rcLeft.bottom - S(4);
        rcShut.top = rcShut.bottom - S(26);
        rcShut.right = rcShut.left + S(80);
        rcArrow = rcShut;
        rcArrow.left = rcShut.right;
        rcArrow.right = rcArrow.left + S(22);
    }

    /* -------------------------------------------------------------------- */
    /* Painting                                                             */
    /* -------------------------------------------------------------------- */

    VOID DrawGlass(HDC hdc)
    {
        RECT rc = m_rcBody;
        int split = rc.top + (rc.bottom - rc.top) * 2 / 5;
        RECT rcTop = { rc.left, rc.top, rc.right, split };
        RECT rcBottom = { rc.left, split, rc.right, rc.bottom };
        /* Brighter "sheen" band on top; both bands meet at the same colour. */
        SM7_VGradient(hdc, rcTop, RGB(70, 108, 154), RGB(34, 66, 106));
        SM7_VGradient(hdc, rcBottom, RGB(34, 66, 106), RGB(13, 34, 60));

        RECT rcOuter = { rc.left, rc.top, rc.right, rc.bottom };
        SM7_RoundFrame(hdc, rcOuter, RGB(8, 20, 36), S(8));
        RECT rcInner = { rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1 };
        SM7_RoundFrame(hdc, rcInner, RGB(116, 146, 182), S(8));
    }

    VOID DrawLeftHighlight(HDC hdc, const RECT& rc)
    {
        RECT rcFill = { rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1 };
        SM7_VGradient(hdc, rcFill, RGB(232, 242, 254), RGB(196, 222, 252));
        SM7_RoundFrame(hdc, rc, RGB(125, 162, 206), S(4));
    }

    VOID DrawRightHighlight(HDC hdc, const RECT& rc)
    {
        RECT rcFill = { rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1 };
        SM7_VGradient(hdc, rcFill, RGB(88, 122, 164), RGB(52, 86, 128));
        SM7_RoundFrame(hdc, rc, RGB(150, 178, 212), S(4));
    }

    VOID DrawTextIn(HDC hdc, LPCWSTR psz, RECT rc, COLORREF color, HFONT hFont, UINT uExtra = 0)
    {
        HGDIOBJ hOld = SelectObject(hdc, hFont);
        SetTextColor(hdc, color);
        DrawTextW(hdc, psz, -1, &rc,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX | uExtra);
        SelectObject(hdc, hOld);
    }

    VOID DrawProgramRow(HDC hdc, const SM7_Item& item, BOOL bHot, int nIcon, int nIndent)
    {
        if (bHot)
            DrawLeftHighlight(hdc, item.rc);

        HIMAGELIST himl = (nIcon == 32) ? m_himlLarge : m_himlSmall;
        int iconX = item.rc.left + S(5) + nIndent;
        int iconY = item.rc.top + (item.rc.bottom - item.rc.top - nIcon) / 2;
        if (item.iIcon >= 0 && himl)
            ImageList_Draw(himl, item.iIcon, hdc, iconX, iconY, ILD_TRANSPARENT);

        RECT rcText = item.rc;
        rcText.left = iconX + nIcon + S(8);
        rcText.right -= S(6);
        DrawTextIn(hdc, item.name, rcText, RGB(0, 0, 0), m_hFont);
    }

    VOID DrawLeftPane(HDC hdc)
    {
        HBRUSH hWhite = (HBRUSH)GetStockObject(WHITE_BRUSH);
        HRGN hRgn = CreateRoundRectRgn(m_rcLeft.left, m_rcLeft.top,
                                       m_rcLeft.right + 1, m_rcLeft.bottom + 1, S(6), S(6));
        FillRgn(hdc, hRgn, hWhite);
        DeleteObject(hRgn);
        SM7_RoundFrame(hdc, m_rcLeft, RGB(146, 164, 188), S(6));

        if (m_bAllPrograms)
        {
            HRGN hClip = CreateRectRgnIndirect(&m_rcTreeView);
            SelectClipRgn(hdc, hClip);
            for (size_t i = 0; i < m_tree.GetCount(); ++i)
            {
                const SM7_Item& item = m_tree[i];
                if (item.rc.bottom < m_rcTreeView.top || item.rc.top > m_rcTreeView.bottom)
                    continue;
                BOOL bHot = (m_nHot == (int)(m_items.GetCount() + i));
                if (item.kind == SM7_TREE_EMPTY)
                {
                    RECT rcText = item.rc;
                    rcText.left += S(5) + item.nDepth * S(16) + S(24);
                    DrawTextIn(hdc, item.name, rcText, RGB(109, 109, 109), m_hFont);
                    continue;
                }
                DrawProgramRow(hdc, item, bHot, 16, item.nDepth * S(16));
            }
            SelectClipRgn(hdc, NULL);
            DeleteObject(hClip);

            /* Scroll indicator */
            int viewH = m_rcTreeView.bottom - m_rcTreeView.top;
            if (m_nTreeContent > viewH)
            {
                int thumbH = max(S(20), viewH * viewH / m_nTreeContent);
                int thumbY = m_rcTreeView.top +
                             (viewH - thumbH) * m_nScroll / (m_nTreeContent - viewH);
                RECT rcThumb = { m_rcTreeView.right - S(5), thumbY,
                                 m_rcTreeView.right - S(1), thumbY + thumbH };
                HBRUSH hThumb = CreateSolidBrush(RGB(190, 200, 214));
                FillRect(hdc, &rcThumb, hThumb);
                DeleteObject(hThumb);
            }

            const SM7_Item& back = m_items[0];
            SM7_HLine(hdc, m_rcLeft.left + S(8), m_rcLeft.right - S(8), back.rc.top - S(4),
                      RGB(214, 224, 236));
            if (m_nHot == 0)
                DrawLeftHighlight(hdc, back.rc);
            SM7_Triangle(hdc, back.rc.left + S(10), (back.rc.top + back.rc.bottom) / 2, S(4),
                         FALSE, RGB(40, 40, 40));
            RECT rcText = back.rc;
            rcText.left += S(24);
            DrawTextIn(hdc, back.name, rcText, RGB(0, 0, 0), m_hFont);
            return;
        }

        int nIcon = IconSize();
        size_t nextSep = 0;
        for (size_t i = 0; i + 1 < m_nLeftEnd; ++i)
        {
            if (nextSep < m_separators.GetCount() && (size_t)m_separators[nextSep] == i)
            {
                SM7_HLine(hdc, m_rcLeft.left + S(8), m_rcLeft.right - S(8),
                          m_items[i].rc.top - S(5), RGB(214, 224, 236));
                nextSep++;
            }
            const SM7_Item& item = m_items[i];
            if (IsRectEmpty(&item.rc))
                continue;
            DrawProgramRow(hdc, item, m_nHot == (int)i, nIcon, 0);
        }

        const SM7_Item& all = m_items[m_nLeftEnd - 1];
        SM7_HLine(hdc, m_rcLeft.left + S(8), m_rcLeft.right - S(8), all.rc.top - S(4),
                  RGB(214, 224, 236));
        if (m_nHot == (int)(m_nLeftEnd - 1))
            DrawLeftHighlight(hdc, all.rc);
        RECT rcText = all.rc;
        rcText.left += S(12);
        DrawTextIn(hdc, all.name, rcText, RGB(0, 0, 0), m_hFont);
        SIZE size;
        HGDIOBJ hOld = SelectObject(hdc, m_hFont);
        GetTextExtentPoint32W(hdc, all.name, all.name.GetLength(), &size);
        SelectObject(hdc, hOld);
        SM7_Triangle(hdc, rcText.left + size.cx + S(8), (all.rc.top + all.rc.bottom) / 2, S(4),
                     TRUE, RGB(40, 40, 40));
    }

    VOID DrawTile(HDC hdc)
    {
        RECT rc = m_rcTile;
        RECT rcFill = { rc.left + 1, rc.top + 1, rc.right - 1, rc.bottom - 1 };
        SM7_VGradient(hdc, rcFill, RGB(236, 244, 252), RGB(168, 196, 228));
        SM7_RoundFrame(hdc, rc, RGB(30, 56, 90), S(6));

        int pic = S(48);
        RECT rcPic = { rc.left + (rc.right - rc.left - pic) / 2, rc.top + (rc.bottom - rc.top - pic) / 2, 0, 0 };
        rcPic.right = rcPic.left + pic;
        rcPic.bottom = rcPic.top + pic;

        /* Hovering a right-pane link shows that item's icon, like Windows 7. */
        if (m_nHot >= (int)m_nLeftEnd && m_nHot < (int)m_items.GetCount() &&
            m_items[m_nHot].iIcon >= 0 && m_himlLarge)
        {
            FillRect(hdc, &rcPic, (HBRUSH)GetStockObject(WHITE_BRUSH));
            ImageList_Draw(m_himlLarge, m_items[m_nHot].iIcon, hdc,
                           rcPic.left + (pic - 32) / 2, rcPic.top + (pic - 32) / 2, ILD_TRANSPARENT);
        }
        else if (m_hbmUser)
        {
            BITMAP bm;
            GetObject(m_hbmUser, sizeof(bm), &bm);
            HDC hdcMem = CreateCompatibleDC(hdc);
            HGDIOBJ hOld = SelectObject(hdcMem, m_hbmUser);
            SetStretchBltMode(hdc, HALFTONE);
            StretchBlt(hdc, rcPic.left, rcPic.top, pic, pic, hdcMem, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
            SelectObject(hdcMem, hOld);
            DeleteDC(hdcMem);
        }
        else
        {
            SM7_DrawBrandMark(hdc, rcPic);
        }
        SM7_RoundFrame(hdc, rcPic, RGB(98, 124, 156), S(2));
    }

    VOID DrawRightPane(HDC hdc)
    {
        size_t count = m_items.GetCount();
        size_t nextSep = 0;
        for (size_t i = m_nLeftEnd; i + 2 < count; ++i)
        {
            const SM7_Item& item = m_items[i];
            if (nextSep < m_rightSeparators.GetCount() &&
                (size_t)m_rightSeparators[nextSep] == i - m_nLeftEnd)
            {
                int y = item.rc.top - S(5);
                SM7_HLine(hdc, item.rc.left + S(4), item.rc.right - S(4), y, RGB(12, 30, 52));
                SM7_HLine(hdc, item.rc.left + S(4), item.rc.right - S(4), y + 1, RGB(74, 104, 140));
                nextSep++;
            }
            if (m_nHot == (int)i)
                DrawRightHighlight(hdc, item.rc);
            RECT rcText = item.rc;
            rcText.left += S(10);
            rcText.right -= S(6);
            DrawTextIn(hdc, item.name, rcText, RGB(255, 255, 255),
                       (i == m_nLeftEnd) ? m_hFontBold : m_hFont);
        }

        /* Shut down split button */
        const SM7_Item& shut = m_items[count - 2];
        const SM7_Item& arrow = m_items[count - 1];
        RECT rcAll = { shut.rc.left, shut.rc.top, arrow.rc.right, arrow.rc.bottom };
        int mid = (rcAll.top + rcAll.bottom) / 2;
        BOOL bHotShut = (m_nHot == (int)(count - 2));
        BOOL bHotArrow = (m_nHot == (int)(count - 1));

        RECT rcTopHalf = { rcAll.left + 1, rcAll.top + 1, rcAll.right - 1, mid };
        RECT rcBottomHalf = { rcAll.left + 1, mid, rcAll.right - 1, rcAll.bottom - 1 };
        SM7_VGradient(hdc, rcTopHalf, RGB(92, 128, 170), RGB(62, 98, 142));
        SM7_VGradient(hdc, rcBottomHalf, RGB(34, 68, 110), RGB(48, 86, 132));
        if (bHotShut || bHotArrow)
        {
            const RECT& rcHot = bHotShut ? shut.rc : arrow.rc;
            RECT rcHotTop = { rcHot.left + 1, rcHot.top + 1, rcHot.right - 1, mid };
            RECT rcHotBottom = { rcHot.left + 1, mid, rcHot.right - 1, rcHot.bottom - 1 };
            SM7_VGradient(hdc, rcHotTop, RGB(126, 170, 214), RGB(86, 134, 186));
            SM7_VGradient(hdc, rcHotBottom, RGB(52, 104, 164), RGB(76, 136, 196));
        }
        SM7_RoundFrame(hdc, rcAll, RGB(10, 26, 46), S(4));
        RECT rcInner = { rcAll.left + 1, rcAll.top + 1, rcAll.right - 1, rcAll.bottom - 1 };
        SM7_RoundFrame(hdc, rcInner, RGB(132, 162, 198), S(3));
        HPEN hPen = CreatePen(PS_SOLID, 1, RGB(18, 40, 66));
        HGDIOBJ hOldPen = SelectObject(hdc, hPen);
        MoveToEx(hdc, arrow.rc.left, rcAll.top + S(3), NULL);
        LineTo(hdc, arrow.rc.left, rcAll.bottom - S(3));
        SelectObject(hdc, hOldPen);
        DeleteObject(hPen);

        DrawTextIn(hdc, shut.name, shut.rc, RGB(255, 255, 255), m_hFont, DT_CENTER);
        SM7_Triangle(hdc, (arrow.rc.left + arrow.rc.right) / 2 - S(2), mid, S(4), TRUE,
                     RGB(255, 255, 255));
    }

    VOID Paint(HDC hdcTarget)
    {
        HDC hdc = CreateCompatibleDC(hdcTarget);
        HBITMAP hbm = CreateCompatibleBitmap(hdcTarget, m_cx, m_cy);
        HGDIOBJ hOldBmp = SelectObject(hdc, hbm);
        SetBkMode(hdc, TRANSPARENT);

        /* Area above the body (outside the window region) */
        RECT rcAll = { 0, 0, m_cx, m_cy };
        FillRect(hdc, &rcAll, (HBRUSH)GetStockObject(BLACK_BRUSH));

        DrawGlass(hdc);
        DrawLeftPane(hdc);
        DrawRightPane(hdc);
        DrawTile(hdc);

        BitBlt(hdcTarget, 0, 0, m_cx, m_cy, hdc, 0, 0, SRCCOPY);
        SelectObject(hdc, hOldBmp);
        DeleteObject(hbm);
        DeleteDC(hdc);
    }

    /* -------------------------------------------------------------------- */
    /* Hit testing and navigation                                           */
    /* -------------------------------------------------------------------- */

    /* Index space: [0, m_items) are m_items; [m_items, m_items + m_tree) are tree rows. */
    int HitTest(POINT pt)
    {
        if (m_bAllPrograms && PtInRect(&m_rcTreeView, pt))
        {
            for (size_t i = 0; i < m_tree.GetCount(); ++i)
            {
                if (PtInRect(&m_tree[i].rc, pt))
                    return (m_tree[i].kind == SM7_TREE_EMPTY) ? -1 : (int)(m_items.GetCount() + i);
            }
            return -1;
        }
        for (size_t i = 0; i < m_items.GetCount(); ++i)
        {
            if (PtInRect(&m_items[i].rc, pt))
                return (int)i;
        }
        return -1;
    }

    SM7_Item* ItemAt(int index)
    {
        if (index < 0)
            return NULL;
        if ((size_t)index < m_items.GetCount())
            return &m_items[index];
        size_t t = index - m_items.GetCount();
        if (t < m_tree.GetCount())
            return &m_tree[t];
        return NULL;
    }

    BOOL IsSelectable(int index)
    {
        SM7_Item* item = ItemAt(index);
        if (!item || item->kind == SM7_TREE_EMPTY)
            return FALSE;
        if ((size_t)index < m_items.GetCount() && IsRectEmpty(&item->rc))
            return FALSE;
        return TRUE;
    }

    BOOL IsLeftIndex(int index)
    {
        return index >= 0 && ((size_t)index < m_nLeftEnd || (size_t)index >= m_items.GetCount());
    }

    /* Left pane indices in visual order. */
    VOID LeftOrder(CAtlArray<int>& order)
    {
        order.SetCount(0);
        if (m_bAllPrograms)
        {
            for (size_t i = 0; i < m_tree.GetCount(); ++i)
                order.Add((int)(m_items.GetCount() + i));
            order.Add(0);
        }
        else
        {
            for (size_t i = 0; i < m_nLeftEnd; ++i)
                order.Add((int)i);
        }
    }

    VOID RightOrder(CAtlArray<int>& order)
    {
        order.SetCount(0);
        for (size_t i = m_nLeftEnd; i < m_items.GetCount(); ++i)
            order.Add((int)i);
    }

    VOID MoveSelection(int delta)
    {
        CAtlArray<int> order;
        if (m_nHot < 0 || IsLeftIndex(m_nHot))
            LeftOrder(order);
        else
            RightOrder(order);
        if (order.GetCount() == 0)
            return;

        int pos = -1;
        for (size_t i = 0; i < order.GetCount(); ++i)
        {
            if (order[i] == m_nHot)
                pos = (int)i;
        }
        int n = (int)order.GetCount();
        for (int step = 0; step < n; ++step)
        {
            if (pos < 0)
                pos = (delta > 0) ? 0 : n - 1;
            else
                pos = (pos + delta + n) % n;
            if (IsSelectable(order[pos]))
            {
                SetHot(order[pos]);
                return;
            }
        }
    }

    VOID EnsureVisible(int index)
    {
        if (!m_bAllPrograms || (size_t)index < m_items.GetCount())
            return;
        const RECT& rc = m_tree[index - m_items.GetCount()].rc;
        if (rc.top < m_rcTreeView.top)
            m_nScroll -= m_rcTreeView.top - rc.top;
        else if (rc.bottom > m_rcTreeView.bottom)
            m_nScroll += rc.bottom - m_rcTreeView.bottom;
        Layout();
    }

    VOID SetHot(int index)
    {
        if (index == m_nHot)
            return;
        m_nHot = index;
        if (index >= 0)
            EnsureVisible(index);
        Invalidate(FALSE);
    }

    /* -------------------------------------------------------------------- */
    /* Actions                                                              */
    /* -------------------------------------------------------------------- */

    HWND TrayHwnd()
    {
        return m_Tray ? m_Tray->GetHWND() : NULL;
    }

    VOID Hide()
    {
        if (!IsWindowVisible())
            return;
        ShowWindow(SW_HIDE);
        m_bAllPrograms = FALSE;
        m_nHot = -1;
        m_tree.SetCount(0);
        Tray_OnStartMenuDismissed(m_Tray);
    }

    VOID ShellOpen(LPCWSTR pszPath, LPCWSTR pszVerb = NULL, LPCWSTR pszParams = NULL)
    {
        SHELLEXECUTEINFOW sei = { sizeof(sei) };
        sei.fMask = SEE_MASK_FLAG_NO_UI;
        sei.hwnd = TrayHwnd();
        sei.lpVerb = pszVerb;
        sei.lpFile = pszPath;
        sei.lpParameters = pszParams;
        sei.nShow = SW_SHOWNORMAL;
        ShellExecuteExW(&sei);
    }

    VOID OpenFolder(int csidl, LPCWSTR pszVerb)
    {
        CComHeapPtr<ITEMIDLIST> pidl;
        if (FAILED(SHGetSpecialFolderLocation(NULL, csidl, &pidl)))
            return;
        SHELLEXECUTEINFOW sei = { sizeof(sei) };
        sei.fMask = SEE_MASK_IDLIST | SEE_MASK_FLAG_NO_UI;
        sei.hwnd = TrayHwnd();
        sei.lpVerb = pszVerb;
        sei.lpIDList = pidl;
        sei.nShow = SW_SHOWNORMAL;
        ShellExecuteExW(&sei);
    }

    VOID LaunchProgram(const CStringW& path)
    {
        CStringW copy = path;
        Hide();
        SM7_RecordLaunch(copy);
        ShellOpen(copy);
    }

    VOID DoExitCommand(UINT uCmd)
    {
        switch (uCmd)
        {
            case SM7_EXIT_LOGOFF:
                Hide();
                ExitWindowsEx(EWX_LOGOFF, 0);
                break;
            case SM7_EXIT_LOCK:
                Hide();
                LockWorkStation();
                break;
            case SM7_EXIT_RESTART:
                Hide();
                SM7_EnableShutdownPrivilege();
                ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED);
                break;
            case SM7_EXIT_SLEEP:
                Hide();
                SetSuspendState(FALSE, FALSE, FALSE);
                break;
            case SM7_EXIT_DIALOG:
                Hide();
                ::PostMessageW(TrayHwnd(), WM_COMMAND, TRAYCMD_SHUTDOWN_DIALOG, 0);
                break;
        }
    }

    VOID ShowShutdownMenu()
    {
        HMENU hMenu = CreatePopupMenu();
        CStringW text;
        text.LoadStringW(hExplorerInstance, IDS_SM7_SWITCHUSER);
        AppendMenuW(hMenu, MF_STRING | MF_GRAYED, SM7_EXIT_SWITCHUSER, text);
        text.LoadStringW(hExplorerInstance, IDS_SM7_LOGOFF);
        AppendMenuW(hMenu, MF_STRING, SM7_EXIT_LOGOFF, text);
        text.LoadStringW(hExplorerInstance, IDS_SM7_LOCK);
        AppendMenuW(hMenu, MF_STRING, SM7_EXIT_LOCK, text);
        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
        text.LoadStringW(hExplorerInstance, IDS_SM7_RESTART);
        AppendMenuW(hMenu, MF_STRING, SM7_EXIT_RESTART, text);
        text.LoadStringW(hExplorerInstance, IDS_SM7_SLEEP);
        AppendMenuW(hMenu, MF_STRING | (IsPwrSuspendAllowed() ? 0 : MF_GRAYED), SM7_EXIT_SLEEP, text);
        AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
        text.LoadStringW(hExplorerInstance, IDS_SM7_SHUTDOWNDLG);
        AppendMenuW(hMenu, MF_STRING, SM7_EXIT_DIALOG, text);

        const RECT& rc = m_items[m_items.GetCount() - 1].rc;
        POINT pt = { rc.right, rc.top };
        ClientToScreen(&pt);
        UINT uCmd = TrackPopupMenuEx(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_BOTTOMALIGN,
                                     pt.x, pt.y, m_hWnd, NULL);
        DestroyMenu(hMenu);
        if (uCmd)
            DoExitCommand(uCmd);
    }

    VOID Activate(int index)
    {
        SM7_Item* item = ItemAt(index);
        if (!item)
            return;

        switch (item->kind)
        {
            case SM7_PROGRAM:
            case SM7_TREE_FILE:
                LaunchProgram(item->path);
                break;
            case SM7_ALLPROGRAMS:
                ShowAllPrograms(TRUE);
                break;
            case SM7_BACK:
                ShowAllPrograms(FALSE);
                break;
            case SM7_TREE_FOLDER:
                ToggleTreeFolder(index - m_items.GetCount());
                Layout();
                Invalidate(FALSE);
                break;
            case SM7_LINK_FOLDER:
            {
                UINT csidl = item->uValue;
                Hide();
                OpenFolder(csidl, NULL);
                break;
            }
            case SM7_LINK_COMMAND:
            {
                UINT uCmd = item->uValue;
                Hide();
                ::PostMessageW(TrayHwnd(), WM_COMMAND, uCmd, 0);
                break;
            }
            case SM7_SHUTDOWN:
                Hide();
                SM7_EnableShutdownPrivilege();
                ExitWindowsEx(EWX_SHUTDOWN | EWX_POWEROFF,
                              SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED);
                break;
            case SM7_SHUTDOWN_MENU:
                ShowShutdownMenu();
                break;
            default:
                break;
        }
    }

    VOID ShowAllPrograms(BOOL bShow)
    {
        m_bAllPrograms = bShow;
        if (bShow)
            BuildTreeRoot();
        else
            m_tree.SetCount(0);
        BuildItems();
        Layout();
        m_nHot = -1;
        if (bShow && m_tree.GetCount())
            m_nHot = (int)m_items.GetCount();
        Invalidate(FALSE);
    }

    VOID ShowContextMenu(int index, POINT ptScreen)
    {
        SM7_Item* item = ItemAt(index);
        if (!item)
            return;

        BOOL bProgram = (item->kind == SM7_PROGRAM || item->kind == SM7_TREE_FILE);
        BOOL bTreeFolder = (item->kind == SM7_TREE_FOLDER);
        BOOL bLinkFolder = (item->kind == SM7_LINK_FOLDER);
        if (!bProgram && !bTreeFolder && !bLinkFolder)
            return;

        CAtlArray<CStringW> pinned;
        SM7_LoadPinned(pinned);

        HMENU hMenu = CreatePopupMenu();
        CStringW text;
        text.LoadStringW(hExplorerInstance, IDS_SM7_OPEN);
        AppendMenuW(hMenu, MF_STRING, SM7_CMD_OPEN, text);
        SetMenuDefaultItem(hMenu, SM7_CMD_OPEN, FALSE);
        if (bLinkFolder || bTreeFolder)
        {
            text.LoadStringW(hExplorerInstance, IDS_SM7_EXPLORE);
            AppendMenuW(hMenu, MF_STRING, SM7_CMD_EXPLORE, text);
        }
        if (bProgram)
        {
            text.LoadStringW(hExplorerInstance, IDS_SM7_OPENLOCATION);
            AppendMenuW(hMenu, MF_STRING, SM7_CMD_OPENLOCATION, text);
            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
            BOOL bPinned = SM7_IsPinned(pinned, item->path);
            text.LoadStringW(hExplorerInstance, bPinned ? IDS_SM7_UNPIN : IDS_SM7_PIN);
            AppendMenuW(hMenu, MF_STRING, bPinned ? SM7_CMD_UNPIN : SM7_CMD_PIN, text);
            if (item->bFrequent)
            {
                text.LoadStringW(hExplorerInstance, IDS_SM7_REMOVE);
                AppendMenuW(hMenu, MF_STRING, SM7_CMD_REMOVE, text);
            }
        }
        if (bProgram || bTreeFolder)
        {
            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
            text.LoadStringW(hExplorerInstance, IDS_SM7_PROPERTIES);
            AppendMenuW(hMenu, MF_STRING, SM7_CMD_PROPERTIES, text);
        }

        UINT uCmd = TrackPopupMenuEx(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, ptScreen.x, ptScreen.y,
                                     m_hWnd, NULL);
        DestroyMenu(hMenu);

        /* Copy what we need: the item array may be rebuilt below. */
        SM7_Item copy = *item;
        switch (uCmd)
        {
            case SM7_CMD_OPEN:
                Activate(index);
                break;
            case SM7_CMD_EXPLORE:
                Hide();
                if (bLinkFolder)
                    OpenFolder(copy.uValue, L"explore");
                else
                    ShellOpen(copy.path, L"explore");
                break;
            case SM7_CMD_OPENLOCATION:
            {
                Hide();
                CStringW params;
                params.Format(L"/select,\"%s\"", copy.path.GetString());
                ShellOpen(L"explorer.exe", NULL, params);
                break;
            }
            case SM7_CMD_PIN:
            case SM7_CMD_UNPIN:
                SM7_SetPinned(copy.path, uCmd == SM7_CMD_PIN);
                Refresh();
                break;
            case SM7_CMD_REMOVE:
                SM7_RemoveFrequent(copy.path);
                Refresh();
                break;
            case SM7_CMD_PROPERTIES:
                Hide();
                SHObjectProperties(TrayHwnd(), SHOP_FILEPATH, copy.path, NULL);
                break;
        }
    }

    VOID Refresh()
    {
        BuildItems();
        Layout();
        m_nHot = -1;
        Invalidate(FALSE);
    }

    /* -------------------------------------------------------------------- */
    /* Window messages                                                      */
    /* -------------------------------------------------------------------- */

    LRESULT OnEraseBkgnd(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
    {
        return TRUE;
    }

    LRESULT OnPaint(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(&ps);
        Paint(hdc);
        EndPaint(&ps);
        return 0;
    }

    LRESULT OnMouseMove(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
    {
        if (!m_bTracking)
        {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, m_hWnd, 0 };
            m_bTracking = TrackMouseEvent(&tme);
        }
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int index = HitTest(pt);
        if (index != m_nHot)
        {
            m_nHot = index;
            Invalidate(FALSE);
        }
        return 0;
    }

    LRESULT OnMouseLeave(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
    {
        m_bTracking = FALSE;
        if (m_nHot >= 0)
        {
            m_nHot = -1;
            Invalidate(FALSE);
        }
        return 0;
    }

    LRESULT OnLButtonUp(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int index = HitTest(pt);
        if (index >= 0)
            Activate(index);
        return 0;
    }

    LRESULT OnRButtonUp(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
    {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int index = HitTest(pt);
        if (index >= 0)
        {
            ClientToScreen(&pt);
            ShowContextMenu(index, pt);
        }
        return 0;
    }

    LRESULT OnMouseWheel(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
    {
        if (m_bAllPrograms)
        {
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            m_nScroll -= delta * S(24) * 3 / WHEEL_DELTA;
            Layout();
            Invalidate(FALSE);
        }
        return 0;
    }

    LRESULT OnKeyDown(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
    {
        switch (wParam)
        {
            case VK_UP:
                MoveSelection(-1);
                break;
            case VK_DOWN:
                MoveSelection(+1);
                break;
            case VK_HOME:
                m_nHot = -1;
                MoveSelection(+1);
                break;
            case VK_END:
                m_nHot = -1;
                MoveSelection(-1);
                break;
            case VK_PRIOR:
            case VK_NEXT:
                for (int i = 0; i < 8; ++i)
                    MoveSelection(wParam == VK_NEXT ? +1 : -1);
                break;
            case VK_RIGHT:
            {
                SM7_Item* item = ItemAt(m_nHot);
                if (item && item->kind == SM7_TREE_FOLDER && !item->bExpanded)
                    Activate(m_nHot);
                else if (item && item->kind == SM7_ALLPROGRAMS)
                    Activate(m_nHot);
                else if (m_nHot < 0 || IsLeftIndex(m_nHot))
                    SetHot((int)m_nLeftEnd);
                break;
            }
            case VK_LEFT:
            {
                SM7_Item* item = ItemAt(m_nHot);
                if (item && item->kind == SM7_TREE_FOLDER && item->bExpanded)
                    Activate(m_nHot);
                else if (m_nHot >= 0 && !IsLeftIndex(m_nHot))
                {
                    m_nHot = -1;
                    MoveSelection(+1);
                }
                break;
            }
            case VK_RETURN:
                if (m_nHot >= 0)
                    Activate(m_nHot);
                break;
            case VK_ESCAPE:
                if (m_bAllPrograms)
                    ShowAllPrograms(FALSE);
                else
                    Hide();
                break;
            case VK_APPS:
                if (m_nHot >= 0)
                {
                    SM7_Item* item = ItemAt(m_nHot);
                    POINT pt = { item->rc.left + S(20), item->rc.bottom };
                    ClientToScreen(&pt);
                    ShowContextMenu(m_nHot, pt);
                }
                break;
        }
        return 0;
    }

    /* Typing a letter jumps to the next item in the pane starting with it. */
    LRESULT OnChar(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
    {
        WCHAR ch = (WCHAR)wParam;
        if (ch < L' ')
            return 0;

        CAtlArray<int> order;
        if (m_nHot < 0 || IsLeftIndex(m_nHot))
            LeftOrder(order);
        else
            RightOrder(order);

        int start = 0;
        for (size_t i = 0; i < order.GetCount(); ++i)
        {
            if (order[i] == m_nHot)
                start = (int)i + 1;
        }
        int n = (int)order.GetCount();
        for (int k = 0; k < n; ++k)
        {
            int index = order[(start + k) % n];
            SM7_Item* item = ItemAt(index);
            if (item && IsSelectable(index) && !item->name.IsEmpty() &&
                ChrCmpIW(item->name[0], ch) == 0)
            {
                SetHot(index);
                break;
            }
        }
        return 0;
    }

    LRESULT OnActivate(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
    {
        if (LOWORD(wParam) == WA_INACTIVE && IsWindowVisible())
        {
            /* A click on the Start button deactivates us before the button
             * asks the tray to toggle the menu; remember it so that toggle
             * does not immediately reopen the menu. */
            POINT pt;
            GetCursorPos(&pt);
            if (PtInRect(&m_rcExclude, pt) && (GetAsyncKeyState(VK_LBUTTON) & 0x8000))
                m_dwSuppressTick = GetTickCount();
            Hide();
        }
        return 0;
    }

    LRESULT OnMouseActivate(UINT uMsg, WPARAM wParam, LPARAM lParam, BOOL& bHandled)
    {
        return MA_ACTIVATE;
    }

    BEGIN_MSG_MAP(CStartMenu7)
        MESSAGE_HANDLER(WM_ERASEBKGND, OnEraseBkgnd)
        MESSAGE_HANDLER(WM_PAINT, OnPaint)
        MESSAGE_HANDLER(WM_MOUSEMOVE, OnMouseMove)
        MESSAGE_HANDLER(WM_MOUSELEAVE, OnMouseLeave)
        MESSAGE_HANDLER(WM_LBUTTONUP, OnLButtonUp)
        MESSAGE_HANDLER(WM_RBUTTONUP, OnRButtonUp)
        MESSAGE_HANDLER(WM_MOUSEWHEEL, OnMouseWheel)
        MESSAGE_HANDLER(WM_KEYDOWN, OnKeyDown)
        MESSAGE_HANDLER(WM_CHAR, OnChar)
        MESSAGE_HANDLER(WM_ACTIVATE, OnActivate)
        MESSAGE_HANDLER(WM_MOUSEACTIVATE, OnMouseActivate)
    END_MSG_MAP()

    /* -------------------------------------------------------------------- */
    /* IOleWindow / IDeskBar / IMenuPopup                                   */
    /* -------------------------------------------------------------------- */

    STDMETHODIMP
    GetWindow(HWND *phwnd) override
    {
        if (!phwnd)
            return E_POINTER;
        *phwnd = m_hWnd;
        return S_OK;
    }

    STDMETHODIMP
    ContextSensitiveHelp(BOOL fEnterMode) override
    {
        return E_NOTIMPL;
    }

    STDMETHODIMP
    SetClient(IUnknown *punkClient) override
    {
        return E_NOTIMPL;
    }

    STDMETHODIMP
    GetClient(IUnknown **ppunkClient) override
    {
        return E_NOTIMPL;
    }

    STDMETHODIMP
    OnPosRectChangeDB(RECT *prc) override
    {
        return E_NOTIMPL;
    }

    STDMETHODIMP
    Popup(POINTL *ppt, RECTL *prcExclude, MP_POPUPFLAGS dwFlags) override
    {
        if (m_dwSuppressTick && GetTickCount() - m_dwSuppressTick < 500)
        {
            m_dwSuppressTick = 0;
            Tray_OnStartMenuDismissed(m_Tray);
            return S_FALSE;
        }
        m_dwSuppressTick = 0;

        if (prcExclude)
        {
            m_rcExclude.left = prcExclude->left;
            m_rcExclude.top = prcExclude->top;
            m_rcExclude.right = prcExclude->right;
            m_rcExclude.bottom = prcExclude->bottom;
        }

        /* Fit the monitor: shrink the height if the work area is small. */
        POINT ptAnchor = { ppt ? ppt->x : m_rcExclude.left, ppt ? ppt->y : m_rcExclude.top };
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfoW(MonitorFromPoint(ptAnchor, MONITOR_DEFAULTTONEAREST), &mi);
        m_cx = S(420);
        m_cy = min(S(540), (int)(mi.rcWork.bottom - mi.rcWork.top));

        int x = ptAnchor.x, y = ptAnchor.y;
        if (dwFlags & MPPF_TOP)
            y = m_rcExclude.top - m_cy;
        else if (dwFlags & MPPF_BOTTOM)
            y = m_rcExclude.bottom;
        else if (dwFlags & MPPF_RIGHT)
            x = m_rcExclude.right;
        else if (dwFlags & MPPF_LEFT)
            x = m_rcExclude.left - m_cx;

        x = max((int)mi.rcMonitor.left, min(x, (int)mi.rcMonitor.right - m_cx));
        y = max((int)mi.rcMonitor.top, min(y, (int)mi.rcMonitor.bottom - m_cy));

        m_bAllPrograms = FALSE;
        m_tree.SetCount(0);
        m_nHot = -1;
        BuildItems();
        Layout();

        /* Rounded body plus the user tile that sticks out above it. */
        HRGN hRgn = CreateRoundRectRgn(m_rcBody.left, m_rcBody.top, m_rcBody.right + 1,
                                       m_rcBody.bottom + 1, S(8), S(8));
        HRGN hTile = CreateRoundRectRgn(m_rcTile.left, m_rcTile.top, m_rcTile.right + 1,
                                        m_rcTile.bottom + 1, S(6), S(6));
        CombineRgn(hRgn, hRgn, hTile, RGN_OR);
        DeleteObject(hTile);

        SetWindowPos(HWND_TOPMOST, x, y, m_cx, m_cy, SWP_NOACTIVATE);
        SetWindowRgn(hRgn, FALSE);   /* the window now owns hRgn */
        ShowWindow(SW_SHOW);
        ::SetForegroundWindow(m_hWnd);
        SetFocus();
        Invalidate(FALSE);
        return S_OK;
    }

    STDMETHODIMP
    OnSelect(DWORD dwSelectType) override
    {
        if (dwSelectType == MPOS_CANCELLEVEL || dwSelectType == MPOS_FULLCANCEL ||
            dwSelectType == MPOS_EXECUTE)
        {
            Hide();
        }
        return S_OK;
    }

    STDMETHODIMP
    SetSubMenu(IMenuPopup *pmp, BOOL fSet) override
    {
        return E_NOTIMPL;
    }

    DECLARE_NOT_AGGREGATABLE(CStartMenu7)

    DECLARE_PROTECT_FINAL_CONSTRUCT()
    BEGIN_COM_MAP(CStartMenu7)
        COM_INTERFACE_ENTRY_IID(IID_IMenuPopup, IMenuPopup)
        COM_INTERFACE_ENTRY_IID(IID_IDeskBar, IDeskBar)
        COM_INTERFACE_ENTRY_IID(IID_IOleWindow, IOleWindow)
    END_COM_MAP()
};

HRESULT
CStartMenu7_CreateInstance(IN ITrayWindow *Tray, REFIID riid, PVOID *ppv)
{
    return ShellObjectCreatorInit<CStartMenu7>(Tray, riid, ppv);
}

/* ------------------------------------------------------------------------ */
/* "Customize Start Menu" for the Windows 7 menu                            */
/* ------------------------------------------------------------------------ */

static INT_PTR CALLBACK
CustomizeModernProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
        case WM_INITDIALOG:
        {
            HICON hLarge = LoadIconW(hExplorerInstance, MAKEINTRESOURCEW(IDI_STARTMENU));
            SendDlgItemMessageW(hwndDlg, IDC_LARGEICON, STM_SETICON, (WPARAM)hLarge, 0);
            HICON hSmall = (HICON)LoadImageW(hExplorerInstance, MAKEINTRESOURCEW(IDI_STARTMENU),
                                             IMAGE_ICON, 16, 16, 0);
            SendDlgItemMessageW(hwndDlg, IDC_SMALLICON, STM_SETICON, (WPARAM)hSmall, 0);

            CheckRadioButton(hwndDlg, IDC_CHOOSELARGE, IDC_CHOOSESMALL,
                             StartMenu7_GetLargeIcons() ? IDC_CHOOSELARGE : IDC_CHOOSESMALL);
            SendDlgItemMessageW(hwndDlg, IDC_NUMBERUPDOWN, UDM_SETRANGE32, 0, SM7_MAX_PROGRAM_COUNT);
            SendDlgItemMessageW(hwndDlg, IDC_NUMBERUPDOWN, UDM_SETPOS32, 0, StartMenu7_GetProgramCount());

            /* Internet / e-mail entries are not part of the Windows 7 menu. */
            static const UINT unused[] =
            {
                IDC_SHOWINTERNET, IDC_INTERNETDEFAULTAPP, IDC_SHOWEMAIL, IDC_EMAILDEFAULTAPP
            };
            for (size_t i = 0; i < _countof(unused); ++i)
                EnableWindow(GetDlgItem(hwndDlg, unused[i]), FALSE);
            return TRUE;
        }

        case WM_COMMAND:
            if (LOWORD(wParam) == IDC_CLEARLIST)
            {
                StartMenu7_ClearFrequent();
                EnableWindow(GetDlgItem(hwndDlg, IDC_CLEARLIST), FALSE);
            }
            else if (LOWORD(wParam) == IDC_CHOOSELARGE || LOWORD(wParam) == IDC_CHOOSESMALL ||
                     (LOWORD(wParam) == IDC_NUMBEROFPROGRAMS && HIWORD(wParam) == EN_CHANGE))
            {
                PropSheet_Changed(GetParent(hwndDlg), hwndDlg);
            }
            break;

        case WM_NOTIFY:
            if (((LPNMHDR)lParam)->code == PSN_APPLY)
            {
                StartMenu7_SetLargeIcons(IsDlgButtonChecked(hwndDlg, IDC_CHOOSELARGE) == BST_CHECKED);
                BOOL bOk = FALSE;
                LRESULT pos = SendDlgItemMessageW(hwndDlg, IDC_NUMBERUPDOWN, UDM_GETPOS32, 0, (LPARAM)&bOk);
                if (!bOk)
                    StartMenu7_SetProgramCount((DWORD)pos);
                SetWindowLongPtrW(hwndDlg, DWLP_MSGRESULT, PSNRET_NOERROR);
                return TRUE;
            }
            break;
    }
    return FALSE;
}

VOID
ShowCustomizeModern(HINSTANCE hInst, HWND hwndOwner)
{
    PROPSHEETPAGEW psp = { sizeof(psp) };
    psp.dwFlags = PSP_DEFAULT;
    psp.hInstance = hInst;
    psp.pszTemplate = MAKEINTRESOURCEW(IDD_MODERNSTART_GENERAL);
    psp.pfnDlgProc = CustomizeModernProc;

    CStringW caption;
    caption.LoadStringW(hInst, IDS_SM7_CUSTOMIZE);

    PROPSHEETHEADERW psh = { sizeof(psh) };
    psh.dwFlags = PSH_PROPSHEETPAGE | PSH_NOAPPLYNOW;
    psh.hwndParent = hwndOwner;
    psh.hInstance = hInst;
    psh.pszCaption = caption;
    psh.nPages = 1;
    psh.ppsp = &psp;
    PropertySheetW(&psh);
}
