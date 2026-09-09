#include "cap.h"

#include <shellapi.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

HWND g_mainWnd = NULL;
static HMENU g_menu;
static NOTIFYICONDATAW g_nid;
static BOOL  g_quit = FALSE;

static CRITICAL_SECTION g_logcs;
static void init_logcs(void) { static int once=0; if(!once){once=1; InitializeCriticalSection(&g_logcs);} }

/* ============ 日志（追加写 CapsEnhance.log，进程目录） ============ */
static void log_path(char* out, size_t cap) {
    WCHAR wexe[MAX_PATH]; DWORD n = GetModuleFileNameW(NULL, wexe, MAX_PATH);
    /* 找到最后一个 '\'，截成目录 */
    WCHAR* slash = NULL;
    for (DWORD i = 0; i < n; i++) if (wexe[i] == L'\\') slash = &wexe[i];
    if (slash) *slash = 0;
    WideCharToMultiByte(CP_ACP, 0, wexe, -1, out, (int)cap, 0, 0);
    strncat_s(out, cap, "\\CapsEnhance.log", _TRUNCATE);
}
void CapLog(const char* fmt, ...) {
    init_logcs();
    EnterCriticalSection(&g_logcs);
    char path[MAX_PATH]; log_path(path, MAX_PATH);
    FILE* f = NULL;
    if (fopen_s(&f, path, "a") == 0 && f) {
        SYSTEMTIME st; GetLocalTime(&st);
        fprintf(f, "%02d:%02d:%02d.%03d  ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    LeaveCriticalSection(&g_logcs);
}

/* ============ 和弦引擎（仅主线程访问） ============ */
static UINT_PTR      g_timer = 0;
static int           g_pendingAct = ACT_NONE;   /* 待开火（和弦稳定期） */
static ULONGLONG     g_pendingTick = 0;
static int           g_activeAct  = ACT_NONE;   /* 已开火（正在重复） */
static ULONGLONG     g_activeNext = 0;

static void StopEngine(void) {
    if (g_timer) { KillTimer(g_mainWnd, g_timer); g_timer = 0; }
}
static void EnsEngine(void) {
    if (!g_timer) g_timer = SetTimer(g_mainWnd, 2, 5, NULL);
}

/* 开火一次动作（词跳走文本解析；其余走按键发送，用开火快照修饰） */
static void DoFireOnce(int act) {
    if (act == ACT_WORD_L)      NavJump(DIR_LEFT, g_actF);
    else if (act == ACT_WORD_R) NavJump(DIR_RIGHT, g_actF);
    else                        SendActOnce(act);
}

/* ============ 托盘 ============ */
static const WCHAR* APP_NAME = L"CapsEnhance";
#define IDM_PAUSE  1
#define IDM_EXIT   2

static void SetMenuPauseText(void);
void TrayAddHint(const WCHAR* s);
static void TrayAdd(void) {
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = g_mainWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TOGGLE_TRAY;
    g_nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wcscpy_s(g_nid.szTip, 128, APP_NAME);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}
static void TrayRemove(void) {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}
static void TrayShowMenu(void) {
    POINT pt;
    GetCursorPos(&pt);
    SetMenuPauseText();
    SetForegroundWindow(g_mainWnd);
    int cmd = TrackPopupMenu(g_menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                             pt.x, pt.y, 0, g_mainWnd, NULL);
    if (cmd == IDM_PAUSE) {
        g_state.paused = !g_state.paused;
        if (!g_state.paused) TrayAddHint(L"CapsEnhance：正在映射（C 为增强键）");
        else TrayAddHint(L"CapsEnhance：已暂停（CapsLock 恢复原功能）");
    } else if (cmd == IDM_EXIT) {
        g_quit = TRUE;
        PostMessageW(g_mainWnd, WM_CLOSE, 0, 0);
    }
}
void TrayAddHint(const WCHAR* s) {
    wcscpy_s(g_nid.szTip, 128, s);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}
static void SetMenuPauseText(void) {
    MENUITEMINFOW m;
    memset(&m, 0, sizeof(m));
    m.cbSize = sizeof(m);
    m.fMask = MIIM_STRING;
    m.dwTypeData = (LPWSTR)(g_state.paused ? L"恢复映射" : L"暂停映射");
    SetMenuItemInfoW(g_menu, IDM_PAUSE, FALSE, &m);
}

/* ============ 窗口过程 ============ */
static void ResetEngine(void) {
    StopEngine();
    g_pendingAct = ACT_NONE;
    g_activeAct  = ACT_NONE;
    g_actD = FALSE; g_actF = FALSE;
    g_pendD = FALSE; g_pendF = FALSE;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    static UINT s_taskbarCreated = 0;
    if (!s_taskbarCreated)
        s_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (msg == s_taskbarCreated) { TrayAdd(); return 0; }

    switch (msg) {
    /* ---- 和弦引擎事件 ---- */
    case WM_CAPS_DOWN:              /* 重新按 C：清掉任何陈旧状态 */
        ResetEngine();
        return 0;
    case WM_CAPS_UP:                 /* C 抬起：停止一切 */
        ResetEngine();
        return 0;
    case WM_PROBE:
        NavProbe();
        return 0;
    case WM_TRIG_DOWN: {             /* 触发电按下：进入稳定期待开火 */
        int act = (int)wp;
        if (g_activeAct != ACT_NONE || g_pendingAct != ACT_NONE)
            break;                    /* 已占用，忽略 */
        g_pendingAct = act;
        g_pendingTick = GetTickCount64();
        EnsEngine();
        return 0;
    }
    case WM_TRIG_UP: {               /* 触发电抬起 */
        int act = (int)wp;
        if (g_pendingAct == act) {
            /* 短点：稳定期内即抬起，立即开火一次并补齐（修饰用门闩快照） */
            g_pendingAct = ACT_NONE; g_activeAct = ACT_NONE;
            g_actD = g_pendD; g_actF = g_pendF;
            g_btnCount = 1;
            DoFireOnce(act);
            ReleaseFill(act);
            g_actD = FALSE; g_actF = FALSE;
            StopEngine();
        } else if (g_activeAct == act) {
            /* 长按之后抬起：停止重复，若有增益短板则补齐 */
            g_activeAct = ACT_NONE;
            ReleaseFill(act);
            g_actD = FALSE; g_actF = FALSE;
            StopEngine();
        }
        return 0;
    }
    case WM_TIMER: {
        if (wp == 3) {            /* 启动自检：延时一次后探测当前焦点 */
            KillTimer(g_mainWnd, 3);
            NavProbe();
            return 0;
        }
        if (wp != 2) break;
        if (!g_cMod) { ResetEngine(); return 0; }
        ULONGLONG now = GetTickCount64();
        if (g_pendingAct != ACT_NONE && now >= g_pendingTick + CHORD_SETTLE) {
            /* 稳定期到：用门闩快照修饰，开火，进入重复节奏 */
            g_activeAct = g_pendingAct; g_pendingAct = ACT_NONE;
            g_actD = g_pendD; g_actF = g_pendF;
            g_btnCount = 1;
            DoFireOnce(g_activeAct);
            g_activeNext = now + REPEAT_FIRST_DELAY;
        } else if (g_activeAct != ACT_NONE && now >= g_activeNext) {
            DoFireOnce(g_activeAct);
            g_btnCount++;
            g_activeNext = now + REPEAT_INTERVAL;
        } else if (g_pendingAct == ACT_NONE && g_activeAct == ACT_NONE) {
            StopEngine();
        }
        return 0;
    }
    /* ---- 托盘 ---- */
    case WM_TOGGLE_TRAY:
        if (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP)
            TrayShowMenu();
        return 0;
    case WM_CLOSE:
        g_quit = TRUE;
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    (void)hInst; (void)hPrev; (void)lpCmd; (void)nShow;
    g_state.paused = FALSE;

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"CapsEnhanceMain";
    RegisterClassW(&wc);

    g_mainWnd = CreateWindowW(L"CapsEnhanceMain", APP_NAME, 0, 0, 0, 0, 0,
                              HWND_MESSAGE, NULL, hInst, NULL);
    if (!g_mainWnd) return 1;

    if (!HookInstall()) {
        TrayAddHint(L"CapsEnhance：钩子安装失败");
        return 1;
    }

    CapLog("==== CapsEnhance 启动 (build probe-self-test) ====");
    SetTimer(g_mainWnd, 3, 1500, NULL);   /* 1.5s 后自动探测当前焦点 */

    g_menu = CreatePopupMenu();
    AppendMenuW(g_menu, MF_STRING, IDM_PAUSE, L"暂停映射");
    AppendMenuW(g_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(g_menu, MF_STRING, IDM_EXIT, L"退出");

    TrayAdd();
    TrayAddHint(L"CapsEnhance：正在映射（C 为增强键）");

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (g_quit) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    StopEngine();
    HookUninstall();
    TrayRemove();
    DestroyMenu(g_menu);
    return 0;
}