/* main.c - 网络收音机 Win32 GUI 主程序
 * 只用基础 Win32 控件：LISTBOX / STATIC / BUTTON / msctls_trackbar32 / msctls_progress32
 * 布局：左侧电台列表，右侧播放控制区，底部全宽信息条（超长文字跑马灯）
 * 固定窗口大小 420 x 410 客户端区
 */

#define _WIN32_WINNT   0x0600
#define WINVER         0x0600
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "radio.h"
#include "resource.h"

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#endif

#define APP_NAME   L"网络收音机"
#define APP_VER    L"1.0"
#define CLIENT_W   420
#define CLIENT_H   410
#define MAX_STATIONS 200
#define TARGET_PBM_MAX 100     /* 进度条 0..100 范围 */

/* --------- 电台数据 --------- */
typedef struct { WCHAR name[128]; char url[1024]; } Station;
static Station  g_stations[MAX_STATIONS];
static int      g_stationCount = 0;
static int      g_mutedVol = -1;    /* -1 表示未静音；>=0 表示静音时保存的音量 */

/* --------- 控件 --------- */
static HWND hMain    = NULL;
static HWND hList    = NULL;
static HWND hLblName = NULL;
static HWND hLblTitle= NULL;
static HWND hLblFmt  = NULL;
static HWND hLblUrl  = NULL;
static HWND hLblStat = NULL;
static HWND hProg    = NULL;
static HWND hTrack   = NULL;
static HWND hVolText = NULL;
static HWND hBtnPrev = NULL;
static HWND hBtnPlay = NULL;
static HWND hBtnPause= NULL;
static HWND hBtnStop = NULL;
static HWND hBtnNext = NULL;
static HFONT hFont    = NULL;
static HFONT hFontBold= NULL;
static WNDPROC g_oldListProc = NULL;
static WNDPROC g_oldMarqueeProc = NULL;
static WNDPROC g_oldLabelProc = NULL;   /* 动态标签统一自绘，整区刷新不残留 */
/* 跑马灯状态：每个控件独立偏移与周期，支持多行同时滚动 */
typedef struct { HWND h; int off; int period; } Marquee;
static Marquee g_mq[4];
static int     g_mqCount = 0;
static Marquee* mqAdd(HWND h) {
    if (g_mqCount >= 4) return NULL;
    g_mq[g_mqCount].h = h; g_mq[g_mqCount].off = 0; g_mq[g_mqCount].period = 0;
    return &g_mq[g_mqCount++];
}
static Marquee* mqGet(HWND h) {
    for (int i = 0; i < g_mqCount; i++) if (g_mq[i].h == h) return &g_mq[i];
    return NULL;
}

/* --------- 前置声明 --------- */
static void  LoadStations(HWND hwnd);
static void  PlayCurrentSelection(HWND hwnd);
static void  StepChannel(HWND hwnd, int delta);
static void  TogglePause(void);
static void  RefreshUiFromInfo(void);
static void  AppendFmt(WCHAR* dst, size_t cap, const WCHAR* prefix, const WCHAR* val);

/* --------- ListBox 子类化：Enter 播放 --------- */
static LRESULT CALLBACK ListSubclass(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN && w == VK_RETURN) {
        PlayCurrentSelection(hMain);
        return 0;
    }
    if (m == WM_GETDLGCODE) return DLGC_WANTALLKEYS | CallWindowProcW(g_oldListProc, h, m, w, l);
    return CallWindowProcW(g_oldListProc, h, m, w, l);
}

/* --------- URL 静态控件 子类化：水平无缝滚动 --------- */
static LRESULT CALLBACK MarqueeSubclass(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        Marquee* mq = mqGet(h);
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
        if (cw <= 0 || ch <= 0 || !mq) { EndPaint(h, &ps); return 0; }

        /* 双缓冲避免闪烁 */
        HDC     mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, cw, ch);
        HBITMAP old = (HBITMAP)SelectObject(mem, bmp);

        /* 背景与 STATIC 默认一致：父窗口 COLOR_BTNFACE */
        FillRect(mem, &rc, (HBRUSH)(COLOR_BTNFACE + 1));
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, GetSysColor(COLOR_WINDOWTEXT));
        HFONT of = (HFONT)SelectObject(mem, hFont);

        WCHAR text[512]; GetWindowTextW(h, text, 512);
        int tlen = lstrlenW(text);
        SIZE sz = {0, 0};
        if (tlen > 0) GetTextExtentPoint32W(mem, text, tlen, &sz);
        int y = (ch - sz.cy) / 2;

        if (sz.cx <= cw) {
            /* 能容纳，左侧对齐画一份 */
            if (tlen > 0) ExtTextOutW(mem, 0, y, 0, NULL, text, tlen, NULL);
            mq->period = 0;
        } else {
            /* 无缝滚动：两份拷贝，中间 40 像素间隙 */
            const int GAP = 40;
            int period = sz.cx + GAP;
            mq->period = period;
            int off = mq->off % period;
            for (int k = -1; k <= 1; k++) {
                int x = -off + k * period;
                if (x + sz.cx > 0 && x < cw) {
                    ExtTextOutW(mem, x, y, 0, NULL, text, tlen, NULL);
                }
            }
        }

        BitBlt(hdc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        SelectObject(mem, of);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_ERASEBKGND) return 1;   /* 自己画了背景，跳过默认擦除 */
    return CallWindowProcW(g_oldMarqueeProc, h, m, w, l);
}

/* --------- 动态标签子类化：双缓冲整区重绘，不闪烁、不残留 --------- */
static LRESULT CALLBACK LabelSubclass(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
        HDC     mem = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, cw > 0 ? cw : 1, ch > 0 ? ch : 1);
        HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
        FillRect(mem, &rc, GetSysColorBrush(COLOR_BTNFACE));
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, GetSysColor(COLOR_WINDOWTEXT));
        HFONT of = (HFONT)SelectObject(mem, hFont);
        WCHAR text[600];
        GetWindowTextW(h, text, 600);
        int tlen = lstrlenW(text);
        if (tlen > 0)
            ExtTextOutW(mem, rc.left, rc.top + 2, 0, NULL, text, tlen, NULL);
        if (cw > 0 && ch > 0) BitBlt(hdc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        SelectObject(mem, of);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_ERASEBKGND) return 1;
    return CallWindowProcW(g_oldLabelProc, h, m, w, l);
}

/* --------- 工具 --------- */
/* 设置动态标签文字：内容未变则不重绘，避免播放时文字不停闪动 */
static void SetLabelIfChanged(HWND hc, const WCHAR* text) {
    WCHAR prev[600];
    GetWindowTextW(hc, prev, 600);
    if (lstrcmpW(prev, text) == 0) return;
    SetWindowTextW(hc, text);
    InvalidateRect(hc, NULL, FALSE);
}

static void AppendFmt(WCHAR* dst, size_t cap, const WCHAR* prefix, const WCHAR* val) {
    if (!cap) return;
    int n = lstrlenW(prefix);
    if (n < 0) n = 0;
    if ((size_t)n >= cap) n = (int)cap - 1;
    lstrcpynW(dst, prefix, n + 1);
    if (val && *val) {
        int left = (int)cap - n;
        lstrcpynW(dst + n, val, left);
    } else {
        int left = (int)cap - n;
        lstrcpynW(dst + n, L"-", left);
    }
}

static HWND MkStatic(HWND parent, const WCHAR* text, int x, int y, int w, int h, int id, HFONT f, DWORD style) {
    HWND hc = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | style,
                              x, y, w, h, parent, (HMENU)(INT_PTR)id,
                              (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE), NULL);
    SendMessageW(hc, WM_SETFONT, (WPARAM)f, TRUE);
    return hc;
}
static HWND MkButton(HWND parent, const WCHAR* text, int x, int y, int w, int h, int id) {
    HWND hc = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                              x, y, w, h, parent, (HMENU)(INT_PTR)id,
                              (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE), NULL);
    SendMessageW(hc, WM_SETFONT, (WPARAM)hFont, TRUE);
    return hc;
}

/* --------- 创建控件 --------- */
static void CreateChildren(HWND hwnd, HINSTANCE hInst) {
    /* 电台列表（左上，占满左侧） */
    hList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | LBS_NOTIFY | LBS_HASSTRINGS,
        10, 10, 190, 250, hwnd, (HMENU)IDC_STATION_LIST, hInst, NULL);
    SendMessageW(hList, WM_SETFONT, (WPARAM)hFont, TRUE);
    g_oldListProc = (WNDPROC)SetWindowLongPtrW(hList, GWLP_WNDPROC, (LONG_PTR)ListSubclass);

    /* 右侧播放控制区：按钮分行排列 + 音量 */
    MkStatic(hwnd, L"正在播放", 210, 10, 200, 20, IDC_HDR_PLAYING, hFontBold, SS_CENTER);
    hBtnPrev  = MkButton(hwnd, L"⏮ 上一个", 210, 42, 99, 26, IDC_BTN_PREV);
    hBtnNext  = MkButton(hwnd, L"下一个 ⏭", 311, 42, 99, 26, IDC_BTN_NEXT);
    hBtnPlay  = MkButton(hwnd, L"▶ 播放",    210, 74, 99, 26, IDC_BTN_PLAY);
    hBtnPause = MkButton(hwnd, L"⏸ 暂停",   311, 74, 99, 26, IDC_BTN_PAUSE);
    hBtnStop  = MkButton(hwnd, L"⏹ 停止",   210, 106, 200, 26, IDC_BTN_STOP);

    MkStatic (hwnd, L"音量:", 210, 142, 40, 20, IDC_PRE_VOL, hFont, SS_SIMPLE);
    hTrack = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
        252, 140, 108, 22, hwnd, (HMENU)IDC_TRACK_VOLUME, hInst, NULL);
    SendMessageW(hTrack, WM_SETFONT, (WPARAM)hFont, TRUE);
    SendMessageW(hTrack, TBM_SETRANGE, TRUE, MAKELONG(0, 100));
    SendMessageW(hTrack, TBM_SETPOS, TRUE, 70);
    hVolText = MkStatic(hwnd, L"70%", 362, 142, 48, 20, IDC_LBL_VOL, hFont, SS_SIMPLE);
    SetWindowLongPtrW(hVolText, GWLP_WNDPROC, (LONG_PTR)LabelSubclass);

    /* 底部全宽信息条：频道/节目/格式/地址/状态，超长文字自动跑马灯 */
    hLblName  = MkStatic(hwnd, L"-", 10, 270, 400, 22, IDC_LBL_NAME,   hFont, SS_SIMPLE);
    hLblTitle = MkStatic(hwnd, L"-", 10, 294, 400, 22, IDC_LBL_TITLE,  hFont, SS_SIMPLE);
    hLblFmt   = MkStatic(hwnd, L"-", 10, 318, 400, 22, IDC_LBL_FORMAT, hFont, SS_SIMPLE);
    hLblUrl   = MkStatic(hwnd, L"-", 10, 342, 400, 22, IDC_LBL_URL,    hFont, SS_SIMPLE);
    hLblStat  = MkStatic(hwnd, L"状态 : 空闲", 10, 368, 400, 20, IDC_LBL_STATUS, hFont, SS_SIMPLE);
    /* 动态标签统一自绘：整区刷新，短文字覆盖长文字不残留 */
    g_oldLabelProc = (WNDPROC)SetWindowLongPtrW(hLblName, GWLP_WNDPROC, (LONG_PTR)LabelSubclass);
    SetWindowLongPtrW(hLblFmt,  GWLP_WNDPROC, (LONG_PTR)LabelSubclass);
    SetWindowLongPtrW(hLblStat, GWLP_WNDPROC, (LONG_PTR)LabelSubclass);
    /* 节目与地址超长时无缝跑马灯 */
    g_oldMarqueeProc = (WNDPROC)SetWindowLongPtrW(hLblUrl, GWLP_WNDPROC, (LONG_PTR)MarqueeSubclass);
    SetWindowLongPtrW(hLblTitle, GWLP_WNDPROC, (LONG_PTR)MarqueeSubclass);
    mqAdd(hLblTitle);
    mqAdd(hLblUrl);

    /* 缓冲进度 */
    MkStatic (hwnd, L"缓冲:", 10, 392, 40, 18, IDC_PRE_BUF, hFont, SS_SIMPLE);
    hProg = CreateWindowExW(0, PROGRESS_CLASSW, L"",
        WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
        55, 392, 355, 18, hwnd, (HMENU)IDC_PROGRESS_BUF, hInst, NULL);
    SendMessageW(hProg, PBM_SETRANGE32, 0, TARGET_PBM_MAX);
    SendMessageW(hProg, PBM_SETPOS, 0, 0);
}

/* --------- 主菜单 --------- */
static HMENU BuildMenu(void) {
    HMENU bar = CreateMenu();
    HMENU radio = CreatePopupMenu();
    AppendMenuW(radio, MF_STRING, IDM_FILE_RELOAD, L"重新加载列表(&R)\tF5");
    AppendMenuW(radio, MF_SEPARATOR, 0, NULL);
    AppendMenuW(radio, MF_STRING, IDM_FILE_EXIT,   L"退出(&X)\tAlt+F4");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)radio, L"电台(&M)");

    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, IDM_HELP_ABOUT, L"关于(&A)…");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"帮助(&H)");
    return bar;
}

/* --------- 加速键 --------- */
static HACCEL BuildAccel(void) {
    static ACCEL a[] = {
        { FVIRTKEY, VK_SPACE,        IDA_SPACE },
        { FVIRTKEY, 'S',             IDA_S },
        { FVIRTKEY, 'M',             IDA_M },
        { FVIRTKEY | FCONTROL, VK_LEFT,  IDA_PREV },
        { FVIRTKEY | FCONTROL, VK_RIGHT, IDA_NEXT },
        { FVIRTKEY | FCONTROL, VK_UP,    IDA_VOLUP },
        { FVIRTKEY | FCONTROL, VK_DOWN,  IDA_VOLDN },
        { FVIRTKEY, VK_F5,           IDA_F5 },
    };
    return CreateAcceleratorTableW(a, sizeof(a) / sizeof(a[0]));
}

/* --------- 加载 stations.txt --------- */
static void LoadStations(HWND hwnd) {
    WCHAR path[MAX_PATH];
    GetModuleFileNameW(NULL, path, MAX_PATH);
    WCHAR* slash = wcsrchr(path, L'\\');
    if (slash) wcscpy(slash + 1, L"stations.txt");
    else       wcscpy(path, L"stations.txt");

    HANDLE hFile = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    g_stationCount = 0;
    SendMessageW(hList, LB_RESETCONTENT, 0, 0);

    if (hFile == INVALID_HANDLE_VALUE) {
        MessageBoxW(hwnd, L"未找到 stations.txt。\n请把它放在 exe 同目录下。",
                    APP_NAME, MB_ICONWARNING);
        return;
    }
    DWORD size = GetFileSize(hFile, NULL);
    if (size == INVALID_FILE_SIZE || size > 256 * 1024) size = 256 * 1024;
    char* buf = (char*)malloc(size + 4);
    DWORD got = 0;
    ReadFile(hFile, buf, size, &got, NULL);
    CloseHandle(hFile);
    buf[got] = 0;

    char* p = buf;
    if (got >= 3 && (unsigned char)p[0] == 0xEF &&
                   (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;

    char* save = NULL;
    (void)save;
    char* line = strtok(p, "\r\n");
    while (line && g_stationCount < MAX_STATIONS) {
        while (*line == ' ' || *line == '\t') line++;
        if (*line && *line != '#') {
            char* eq = strchr(line, '=');
            if (eq) {
                *eq = 0;
                char* name = line;
                char* url  = eq + 1;
                int nlen = (int)strlen(name);
                while (nlen > 0 && (name[nlen - 1] == ' ' || name[nlen - 1] == '\t')) name[--nlen] = 0;
                while (*url == ' ' || *url == '\t') url++;
                int ulen = (int)strlen(url);
                while (ulen > 0 && (url[ulen - 1] == ' ' || url[ulen - 1] == '\t')) url[--ulen] = 0;
                if (name[0] && url[0]) {
                    Station* s = &g_stations[g_stationCount];
                    s->name[0] = 0; s->url[0] = 0;
                    MultiByteToWideChar(CP_UTF8, 0, name, -1, s->name, 127);
                    strncpy(s->url, url, sizeof(s->url) - 1);
                    s->url[sizeof(s->url) - 1] = 0;
                    SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)s->name);
                    g_stationCount++;
                }
            }
        }
        line = strtok(NULL, "\r\n");
    }
    free(buf);
    if (g_stationCount > 0) SendMessageW(hList, LB_SETCURSEL, 0, 0);

    WCHAR msg[64];
    _snwprintf(msg, 63, L"已加载 %d 个电台", g_stationCount);
    SetLabelIfChanged(hLblStat, msg);
}

/* --------- 播放/停止/切台 --------- */
static void PlayCurrentSelection(HWND hwnd) {
    (void)hwnd;
    int idx = (int)SendMessageW(hList, LB_GETCURSEL, 0, 0);
    if (idx < 0 || idx >= g_stationCount) return;
    /* 显示"频道"行 */
    SetLabelIfChanged(hLblName, g_stations[idx].name);
    radio_play(g_stations[idx].url, g_stations[idx].name);
    /* 同步预缓存下一台，切台时即时开播 */
    if (g_stationCount > 1) radio_prewarm(g_stations[(idx + 1) % g_stationCount].url);
}

static void StepChannel(HWND hwnd, int delta) {
    if (g_stationCount == 0) return;
    int idx = (int)SendMessageW(hList, LB_GETCURSEL, 0, 0);
    if (idx < 0) idx = 0;
    else {
        idx += delta;
        if (idx < 0) idx = 0;
        if (idx >= g_stationCount) idx = g_stationCount - 1;
    }
    SendMessageW(hList, LB_SETCURSEL, idx, 0);
    SendMessageW(hList, LB_SETHORIZONTALEXTENT, 0, 0);
    PlayCurrentSelection(hwnd);
}

static void TogglePause(void) {
    if (!radio_is_active()) { PlayCurrentSelection(hMain); return; }
    radio_set_paused(!radio_is_paused());
}

static void OnVolumeChanged(void) {
    int v = (int)SendMessageW(hTrack, TBM_GETPOS, 0, 0);
    radio_set_volume(v);
    WCHAR t[16];
    _snwprintf(t, 15, L"%d%%", v); t[15] = 0;
    SetLabelIfChanged(hVolText, t);
    g_mutedVol = -1;
}

static void ToggleMute(void) {
    if (g_mutedVol < 0) {
        g_mutedVol = (int)SendMessageW(hTrack, TBM_GETPOS, 0, 0);
        SendMessageW(hTrack, TBM_SETPOS, TRUE, 0);
        radio_set_volume(0);
        SetLabelIfChanged(hVolText, L"静音");
    } else {
        SendMessageW(hTrack, TBM_SETPOS, TRUE, g_mutedVol);
        radio_set_volume(g_mutedVol);
        WCHAR t[16]; _snwprintf(t, 15, L"%d%%", g_mutedVol); t[15] = 0;
        SetLabelIfChanged(hVolText, t);
        g_mutedVol = -1;
    }
}

/* --------- 从引擎拉状态刷新 UI --------- */
static void RefreshUiFromInfo(void) {
    RADIO_INFO info; radio_get_info(&info);
    WCHAR buf[512];

    AppendFmt(buf, 512, L"状态 : ", info.status);
    SetLabelIfChanged(hLblStat, buf);

    /* 节目行（超长跑马灯）：内容变化才重设并从头滚 */
    {
        const WCHAR* t = info.icyTitle[0] ? info.icyTitle : L"-";
        WCHAR prev[512] = {0};
        GetWindowTextW(hLblTitle, prev, 512);
        if (lstrcmpW(prev, t) != 0) {
            SetWindowTextW(hLblTitle, t);
            Marquee* mq = mqGet(hLblTitle); if (mq) mq->off = 0;
            InvalidateRect(hLblTitle, NULL, FALSE);
        }
    }

    SetLabelIfChanged(hLblFmt, info.fmtInfo[0] ? info.fmtInfo : L"-");

    {
        const WCHAR* urlText = info.url[0] ? info.url : L"";
        WCHAR prev[512] = {0};
        GetWindowTextW(hLblUrl, prev, 512);
        if (lstrcmpW(prev, urlText) != 0) {
            SetWindowTextW(hLblUrl, urlText);
            Marquee* mq = mqGet(hLblUrl); if (mq) mq->off = 0;
            InvalidateRect(hLblUrl, NULL, FALSE);
        }
    }

    int pos = 0;
    if (info.bufMax > 0) {
        pos = (int)((long long)info.bufCur * 100 / info.bufMax);
        if (pos > 100) pos = 100;
        if (pos < 0)   pos = 0;
    }
    SendMessageW(hProg, PBM_SETPOS, pos, 0);

    /* 按钮启用状态 */
    BOOL act = radio_is_active();
    EnableWindow(hBtnPlay,  !act || info.state == RS_PAUSED ? TRUE : FALSE);
    EnableWindow(hBtnPause, act && info.state != RS_PAUSED);
    EnableWindow(hBtnStop,  act);
}

/* --------- About --------- */
static void ShowAbout(HWND hwnd) {
    MessageBoxW(hwnd,
        APP_NAME L" v" APP_VER L"\n\n"
        L"基于 Win32 平台 API 的极简网络收音机。\n"
        L"HTTP: WinInet   解码: minimp3 + FAAD2   输出: waveOut\n\n"
        L"· 双击列表项 或 按 Enter 播放\n"
        L"· 空格 播放/暂停     S 停止     M 静音\n"
        L"· Ctrl+←/→ 切台      Ctrl+↑/↓ 音量\n"
        L"· F5 重新加载 stations.txt\n",
        L"关于 " APP_NAME, MB_OK | MB_ICONINFORMATION);
}

/* --------- 窗口过程 --------- */
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        HINSTANCE hi = (HINSTANCE)cs->hInstance;
        /* 字体 */
        NONCLIENTMETRICSW ncm; ncm.cbSize = sizeof(ncm);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        hFont = CreateFontIndirectW(&ncm.lfMessageFont);
        LOGFONTW lf = ncm.lfMessageFont; lf.lfWeight = FW_BOLD;
        hFontBold = CreateFontIndirectW(&lf);
        CreateChildren(hwnd, hi);
        SetMenu(hwnd, BuildMenu());
        LoadStations(hwnd);
        SetTimer(hwnd, IDT_UI, 250, NULL);
        SetTimer(hwnd, IDT_MARQUEE, 50, NULL);   /* 约 20 像素/秒 的左移速度 */
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, OPAQUE);
        SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
    }
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (code == LBN_DBLCLK && id == IDC_STATION_LIST) {
            PlayCurrentSelection(hwnd); return 0;
        }
        if (code == LBN_SELCHANGE && id == IDC_STATION_LIST) {
            /* 选中即预缓存：用户翻到哪台就提前连哪台 */
            int idx = (int)SendMessageW(hList, LB_GETCURSEL, 0, 0);
            if (idx >= 0 && idx < g_stationCount)
                radio_prewarm(g_stations[idx].url);
            return 0;
        }
        switch (id) {
        case IDC_BTN_PLAY:
            if (radio_is_active() && radio_is_paused()) radio_set_paused(FALSE);
            else PlayCurrentSelection(hwnd);
            return 0;
        case IDC_BTN_PAUSE:
            if (radio_is_active() && !radio_is_paused()) radio_set_paused(TRUE);
            return 0;
        case IDC_BTN_STOP:  radio_stop(); return 0;
        case IDC_BTN_PREV:  StepChannel(hwnd, -1); return 0;
        case IDC_BTN_NEXT:  StepChannel(hwnd, +1); return 0;
        case IDC_STATION_LIST:
            if (code == LBN_SELCHANGE) {
                int i = (int)SendMessageW(hList, LB_GETCURSEL, 0, 0);
                if (i >= 0 && i < g_stationCount)
                    SetLabelIfChanged(hLblName, g_stations[i].name);
            }
            return 0;
        case IDM_FILE_RELOAD: case IDA_F5:
            LoadStations(hwnd); return 0;
        case IDM_FILE_EXIT:
            DestroyWindow(hwnd); return 0;
        case IDM_HELP_ABOUT:
            ShowAbout(hwnd); return 0;
        case IDA_SPACE:  TogglePause(); return 0;
        case IDA_S:      radio_stop(); return 0;
        case IDA_M:      ToggleMute(); return 0;
        case IDA_PREV:   StepChannel(hwnd, -1); return 0;
        case IDA_NEXT:   StepChannel(hwnd, +1); return 0;
        case IDA_VOLUP: case IDA_VOLDN: {
            int v = (int)SendMessageW(hTrack, TBM_GETPOS, 0, 0) + (id == IDA_VOLUP ? 5 : -5);
            if (v < 0)   v = 0;
            if (v > 100) v = 100;
            SendMessageW(hTrack, TBM_SETPOS, TRUE, v);
            OnVolumeChanged();
            return 0;
        }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    case WM_HSCROLL: {
        if ((HWND)lp == hTrack) OnVolumeChanged();
        return 0;
    }
    case WM_TIMER:
        if (wp == IDT_UI) RefreshUiFromInfo();
        else if (wp == IDT_MARQUEE) {
            /* 只在需要滚动时重画对应控件 */
            for (int i = 0; i < g_mqCount; i++) {
                if (g_mq[i].period > 0) {
                    g_mq[i].off++;
                    InvalidateRect(g_mq[i].h, NULL, FALSE);
                }
            }
        }
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        RECT rc = { 0, 0, CLIENT_W, CLIENT_H };
        AdjustWindowRect(&rc, style, TRUE);   /* TRUE = 有菜单 */
        mmi->ptMinTrackSize.x = rc.right - rc.left;
        mmi->ptMinTrackSize.y = rc.bottom - rc.top;
        mmi->ptMaxTrackSize   = mmi->ptMinTrackSize;
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        KillTimer(hwnd, IDT_UI);
        KillTimer(hwnd, IDT_MARQUEE);
        radio_stop();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* --------- 入口 --------- */
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmd, int nShow) {
    (void)hPrev; (void)cmd;
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_BAR_CLASSES | ICC_PROGRESS_CLASS | ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);
    radio_startup();

    WNDCLASSEXW wc; memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"NetRadio.MainWnd";
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) { radio_cleanup(); return 1; }

    /* 客户端 420x410；有菜单和边框，先算窗口大小 */
    DWORD style   = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT  rc      = { 0, 0, CLIENT_W, CLIENT_H };
    AdjustWindowRect(&rc, style, TRUE);
    int winW = rc.right - rc.left;
    int winH = rc.bottom - rc.top;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - winW) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - winH) / 2;

    hMain = CreateWindowExW(0, wc.lpszClassName, APP_NAME L" v" APP_VER,
                            style, sx, sy, winW, winH,
                            NULL, NULL, hInst, NULL);
    if (!hMain) { radio_cleanup(); return 1; }

    ShowWindow(hMain, nShow);
    UpdateWindow(hMain);

    HACCEL hAccel = BuildAccel();
    MSG m;
    while (GetMessageW(&m, NULL, 0, 0)) {
        if (hAccel && TranslateAcceleratorW(hMain, hAccel, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (hAccel) DestroyAcceleratorTable(hAccel);
    if (hFont)  DeleteObject(hFont);
    if (hFontBold) DeleteObject(hFontBold);
    radio_cleanup();
    return (int)m.wParam;
}
