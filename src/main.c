#include "cap.h"

#include <shellapi.h>
#include <commctrl.h>
#include <imm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

HWND g_mainWnd = NULL;
static HMENU g_menu;
static NOTIFYICONDATAW g_nid;
static BOOL  g_quit = FALSE;

/* 诊断记录开关：C+` 切换（主线程读写，hook 线程投递 WM_PROBE） */
volatile BOOL g_recording = FALSE;

static CRITICAL_SECTION g_logcs;
/* 惰性初始化。用 InterlockedCompareExchange 防双线程竞态；
   当前 CapLog 只在主线程调用，这里按可跨线程的标准写。 */
static void init_logcs(void) {
    static volatile LONG once = 0;
    if (InterlockedCompareExchange(&once, 1, 0) == 0)
        InitializeCriticalSection(&g_logcs);
}

/* ============ 日志（追加写 CapsEnhance.log，exe 同目录） ============ */
static void log_path(WCHAR* out, size_t cap) {
    WCHAR wexe[MAX_PATH]; DWORD n = GetModuleFileNameW(NULL, wexe, MAX_PATH);
    /* 找到最后一个 '\'，截成目录 */
    WCHAR* slash = NULL;
    for (DWORD i = 0; i < n; i++) if (wexe[i] == L'\\') slash = &wexe[i];
    if (slash) *slash = 0;
    /* 全程用宽字符拼路径：经 ANSI 转换会在安装路径含非 ACP 字符时写错文件 */
    _snwprintf_s(out, cap, _TRUNCATE, L"%s\\CapsEnhance.log", wexe);
}
void CapLog(const char* fmt, ...) {
    init_logcs();
    EnterCriticalSection(&g_logcs);
    WCHAR wpath[MAX_PATH]; log_path(wpath, MAX_PATH);
    FILE* f = NULL;
    if (_wfopen_s(&f, wpath, L"a") == 0 && f) {
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
#define IDM_PAUSE   1
#define IDM_EXIT    2
#define IDM_RESTART 3

/* 把进程工作集压回待机内存：UIA/TSF 等 DLL 一旦加载无法卸载，只能压页。
   用于"诊断结束"和"托盘菜单关闭"这类低频节点，让内存数值回落；
   页面被换出后再次使用时会自动换回，仅一次软缺页开销。 */
static void TrimWorkingSet(void) {
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

static void SetMenuPauseText(void);
static void ResetEngine(void);   /* 前向声明（定义在下方，TrayShowMenu 先调用） */
static void DoRestart(void);     /* 前向声明（定义在下方，TrayShowMenu 先调用） */
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
        /* 切换暂停时清掉残留的修饰/引擎状态，避免恢复后粘滞 */
        g_cMod = FALSE; g_dMod = FALSE; g_fMod = FALSE;
        g_btnCount = 0;
        ResetEngine();
        if (!g_state.paused) TrayAddHint(L"CapsEnhance：正在映射");
        else TrayAddHint(L"CapsEnhance：已暂停（CapsLock 恢复原功能）");
    } else if (cmd == IDM_RESTART) {
        DoRestart();
    } else if (cmd == IDM_EXIT) {
        g_quit = TRUE;
        PostMessageW(g_mainWnd, WM_CLOSE, 0, 0);
    }
    TrimWorkingSet();   /* 菜单交互会拉起 TSF/DWM 等模块，关完菜单压回 */
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

/* 重启程序：清键盘状态 -> 拉起新实例（新实例带 --wait-exit <本进程PID>，
   等本进程退出后才安装钩子）-> 本进程退出。
   清键盘 = 对可能注入过的修饰键补"抬起"，消除目标程序里的卡键；
   钩子 / 和弦引擎 / 修饰门闩随进程重启全部回到干净初态。 */
static void DoRestart(void) {
    ResetEngine();
    g_cMod = FALSE; g_dMod = FALSE; g_fMod = FALSE;
    g_pendD = FALSE; g_pendF = FALSE; g_btnCount = 0;
    SendClearStuckModifiers();

    WCHAR exe[MAX_PATH] = L"";
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    WCHAR cmd[MAX_PATH + 64];
    _snwprintf_s(cmd, MAX_PATH + 64, _TRUNCATE,
                 L"\"%s\" --wait-exit %lu", exe,
                 (unsigned long)GetCurrentProcessId());

    STARTUPINFOW si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        TrayAddHint(L"CapsEnhance：重启失败");
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    g_quit = TRUE;
    PostMessageW(g_mainWnd, WM_CLOSE, 0, 0);
}

/* 重启流程：新实例带 --wait-exit <pid>，等旧实例退出（最多 5 秒）后再
   创建窗口/安装钩子，避免两个实例的钩子与托盘图标短暂并存。 */
static void WaitOldInstanceExit(void) {
    const WCHAR* flag = wcsstr(GetCommandLineW(), L"--wait-exit");
    if (!flag) return;
    DWORD pid = (DWORD)wcstoul(flag + 11, NULL, 10);
    if (!pid) return;
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return;                      /* 旧实例已退出 */
    WaitForSingleObject(h, 5000);
    CloseHandle(h);
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
        /* C+`：切换诊断记录。开始=探测当前焦点并记录；再按=结束记录。
           用 MessageBeep 的两个不同系统事件音（走声卡，无需内置蜂鸣器），
           并同步托盘悬停提示内容。 */
        g_recording = !g_recording;
        if (g_recording) {
            CapLog("===== REC START =====");
            MessageBeep(MB_ICONASTERISK);   /* 系统"星号"音：开始记录（与结束不同） */
            TrayAddHint(L"CapsEnhance：正在记录日志（C+\x60 结束）");
            NavProbe();
        } else {
            CapLog("===== REC END =====");
            MessageBeep(MB_ICONHAND);       /* 系统"手形"音：结束记录（低而不同） */
            TrayAddHint(L"CapsEnhance：正在映射");
            /* 诊断期间 UIA/MSAA 等 COM 基础设施已加载，结束记录后压回工作集 */
            TrimWorkingSet();
        }
        return 0;
    case WM_TRIG_DOWN: {             /* 触发键按下：进入稳定期待开火 */
        int act = (int)wp;
        if (g_activeAct != ACT_NONE || g_pendingAct != ACT_NONE)
            break;                    /* 已占用，忽略 */
        g_pendingAct = act;
        g_pendingTick = GetTickCount64();
        EnsEngine();
        return 0;
    }
    case WM_TRIG_UP: {               /* 触发键抬起 */
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
    case WM_IME_SETCONTEXT:
        /* 不给 IME 关联 UI：见 ImmAssociateContext 注释 */
        return 0;
    case WM_CLOSE:
        /* 必须销毁窗口 -> WM_DESTROY -> PostQuitMessage；
           只设 g_quit 的话循环还堵在 GetMessage 上，要等下一条消息才退出 */
        DestroyWindow(hwnd);
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

    WaitOldInstanceExit();   /* 重启场景：等旧实例完全退出再初始化 */

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"CapsEnhanceMain";
    RegisterClassW(&wc);

    g_mainWnd = CreateWindowW(L"CapsEnhanceMain", APP_NAME,
                              WS_POPUP | WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1, 1,
                              NULL, NULL, hInst, NULL);
    if (!g_mainWnd) return 1;

    /* 本窗口只承载托盘菜单，永不接收文字输入。禁掉 IME/TSF：
       SetForegroundWindow 让它获得键盘焦点时，系统会为它初始化输入法
       上下文并加载 textinputframework/CoreUIComponents 等（实测 +4MB
       上下）。禁用后这些模块不再加载。 */
    ImmAssociateContext(g_mainWnd, NULL);

    if (!HookInstall()) {
        TrayAddHint(L"CapsEnhance：钩子安装失败");
        return 1;
    }

    /* 启动不写日志文件；采集系统/环境基线供 C+` 诊断时输出。程序启动本身不发声。 */
    NavBaseInfo();

    g_menu = CreatePopupMenu();
    AppendMenuW(g_menu, MF_STRING, IDM_PAUSE, L"暂停映射");
    AppendMenuW(g_menu, MF_STRING, IDM_RESTART, L"重启程序");
    AppendMenuW(g_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(g_menu, MF_STRING, IDM_EXIT, L"退出");

    TrayAdd();
    TrayAddHint(L"CapsEnhance：正在映射");

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (g_quit) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    StopEngine();
    HookUninstall();
    SendClearStuckModifiers();   /* 退出前清掉可能卡住的注入修饰键 */
    TrayRemove();
    DestroyMenu(g_menu);
    return 0;
}