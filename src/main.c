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
/* 日志超过上限时保留最近的一半（老内容丢弃），避免长期开着记录涨到几十 MB。
   上限由配置 [General] LogMaxKB 控制，0 = 不限制。 */
static void log_rotate_if_needed(const WCHAR* path) {
    int maxKB = ConfigGetLogMaxKB();
    if (maxKB <= 0) return;
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &d)) return;
    LARGE_INTEGER sz;
    sz.HighPart = (LONG)d.nFileSizeHigh;
    sz.LowPart  = d.nFileSizeLow;
    if (sz.QuadPart <= (LONGLONG)maxKB * 1024) return;

    LONGLONG keep = (LONGLONG)maxKB * 1024 / 2;
    FILE* f = NULL;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) return;
    if (_fseeki64(f, -keep, SEEK_END) != 0) { fclose(f); return; }
    char* buf = (char*)malloc((size_t)keep + 1);
    if (!buf) { fclose(f); return; }
    size_t got = fread(buf, 1, (size_t)keep, f);
    fclose(f);
    /* 丢掉被切断的半行 */
    size_t off = 0;
    while (off < got && buf[off] != '\n') off++;
    if (off < got) off++;
    if (_wfopen_s(&f, path, L"wb") == 0 && f) {
        fputs("----- 日志已达上限，较旧的内容已丢弃 -----\n", f);
        if (got > off) fwrite(buf + off, 1, got - off, f);
        fclose(f);
    }
    free(buf);
}

void CapLog(const char* fmt, ...) {
    init_logcs();
    EnterCriticalSection(&g_logcs);
    WCHAR wpath[MAX_PATH]; log_path(wpath, MAX_PATH);
    log_rotate_if_needed(wpath);
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

/* 当前仍按着的触发键（按按下顺序）。多个键同时按住时"最后按下的"生效，
   松开它之后退回上一个仍按着的键 —— 与真实键盘的多键行为一致。
   也保证任何一个键的抬起都能被正确处理（此前引擎只认一个动作，
   第二个键的 DOWN/UP 全被忽略，重复停不下来）。 */
#define ACT_STACK_MAX 8
static int g_actStack[ACT_STACK_MAX];
static int g_actStackN = 0;

static void StackClear(void) { g_actStackN = 0; }
static void StackPush(int act) {
    for (int i = 0; i < g_actStackN; i++)        /* 去重（重复 keydown） */
        if (g_actStack[i] == act) return;
    if (g_actStackN < ACT_STACK_MAX) g_actStack[g_actStackN++] = act;
}
static void StackRemove(int act) {
    int w = 0;
    for (int i = 0; i < g_actStackN; i++)
        if (g_actStack[i] != act) g_actStack[w++] = g_actStack[i];
    g_actStackN = w;
}
static int StackTop(void) {
    return g_actStackN ? g_actStack[g_actStackN - 1] : ACT_NONE;
}

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
/* 托盘悬停提示统一入口：按"真实运行权限"追加管理员标注
   （查进程令牌，不看配置值，避免配置与实际不一致时误导） */
static void TrayHint(const WCHAR* state) {
    WCHAR buf[160];
    _snwprintf_s(buf, 160, _TRUNCATE,
                 IsRunElevated() ? L"%s%s（管理员）" : L"%s%s",
                 L"CapsEnhance：", state);
    TrayAddHint(buf);
}
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
        if (!g_state.paused) TrayHint(L"正在映射");
        else TrayHint(L"已暂停（CapsLock 恢复原功能）");
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
    NavSelTrackReset();
    g_pendingAct = ACT_NONE;
    g_activeAct  = ACT_NONE;
    g_actD = FALSE; g_actF = FALSE;
    g_pendD = FALSE; g_pendF = FALSE;
    StackClear();
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
        TrayHint(L"重启失败");
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

/* 单实例：已经有一个实例在跑就直接退出（并请它提示一下）。
   必须在 WaitOldInstanceExit() 之后调用 —— 重启时旧实例持有这把锁，
   等它退出以后新实例才拿得到。 */
#define SINGLE_MUTEX_NAME L"Local\\CapsEnhance_SingleInstance"
static HANDLE g_singleMutex = NULL;
static BOOL EnsureSingleInstance(void) {
    g_singleMutex = CreateMutexW(NULL, FALSE, SINGLE_MUTEX_NAME);
    if (!g_singleMutex) return TRUE;             /* 建不了就不拦，照常运行 */
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND prev = FindWindowW(L"CapsEnhanceMain", NULL);
        if (prev) PostMessageW(prev, WM_ALREADY_RUNNING, 0, 0);
        CloseHandle(g_singleMutex);
        g_singleMutex = NULL;
        return FALSE;
    }
    return TRUE;
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
    case WM_ALREADY_RUNNING:         /* 又启动了一个实例：提示后让它自己退出 */
        TrayHint(L"已在运行中（不允许多个实例）");
        return 0;
    case WM_PROBE:
        /* C+`：切换诊断记录。开始=探测当前焦点并记录；再按=结束记录。
           用 MessageBeep 的两个不同系统事件音（走声卡，无需内置蜂鸣器），
           并同步托盘悬停提示内容。 */
        g_recording = !g_recording;
        if (g_recording) {
            CapLog("===== REC START =====");
            MessageBeep(MB_ICONASTERISK);   /* 系统"星号"音：开始记录（与结束不同） */
            TrayHint(L"正在记录日志（C+\x60 结束）");
            NavProbe();
        } else {
            CapLog("===== REC END =====");
            MessageBeep(MB_ICONHAND);       /* 系统"手形"音：结束记录（低而不同） */
            TrayHint(L"正在映射");
            /* 诊断期间 UIA/MSAA 等 COM 基础设施已加载，结束记录后压回工作集 */
            TrimWorkingSet();
        }
        return 0;
    case WM_TRIG_DOWN: {             /* 触发键按下：进入稳定期待开火 */
        int act = (int)wp;
        StackPush(act);
        if (g_activeAct != ACT_NONE && g_activeAct != act) {
            /* 已在重复时又按下另一个键：立刻切到新键（最后按下优先），
               重新走重复节奏。此前这里直接忽略，导致第二个键完全失灵。 */
            g_actD = g_pendD; g_actF = g_pendF;
            g_activeAct = act;
            g_btnCount = 1;
            DoFireOnce(act);
            g_activeNext = GetTickCount64() + REPEAT_FIRST_DELAY;
        } else if (g_pendingAct != act) {
            /* 稳定期内后按下的键优先 */
            g_pendingAct = act;
            g_pendingTick = GetTickCount64();
        }
        EnsEngine();
        return 0;
    }
    case WM_TRIG_UP: {               /* 触发键抬起 */
        int act = (int)wp;
        StackRemove(act);
        if (g_pendingAct == act) {
            /* 短点：稳定期内即抬起，立即开火一次并补齐（修饰用门闩快照） */
            g_pendingAct = ACT_NONE;
            g_actD = g_pendD; g_actF = g_pendF;
            g_btnCount = 1;
            DoFireOnce(act);
            ReleaseFill(act);
            g_actD = FALSE; g_actF = FALSE;
            /* 还有别的键按着就继续（退回上一个），否则停 */
            int top = StackTop();
            if (top != ACT_NONE) {
                g_pendingAct = top;
                g_pendingTick = GetTickCount64();
                EnsEngine();
            } else StopEngine();
        } else if (g_activeAct == act) {
            /* 长按之后抬起：停止重复，若有增益短板则补齐 */
            g_activeAct = ACT_NONE;
            ReleaseFill(act);
            g_actD = FALSE; g_actF = FALSE;
            int top = StackTop();
            if (top != ACT_NONE) {
                /* 别的键还按着：切回它并继续重复 */
                g_actD = g_pendD; g_actF = g_pendF;
                g_activeAct = top;
                g_btnCount = 1;
                DoFireOnce(top);
                g_activeNext = GetTickCount64() + REPEAT_FIRST_DELAY;
                EnsEngine();
            } else StopEngine();
        }
        /* 其余情况：不是当前动作的抬起，已从栈中移除，不影响当前重复 */
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

    /* --reset-keys：无条件把所有修饰键补一次"抬起"然后退出。
       用于程序已被杀死（状态表随进程消失）留下的卡键，等价于 AHK 社区
       常用的"把所有修饰键 Send Up 一遍"。可自建快捷方式随时调用。 */
    if (wcsstr(GetCommandLineW(), L"--reset-keys")) {
        SendForceResetKeys();
        return 0;
    }

    WaitOldInstanceExit();   /* 重启场景：等旧实例完全退出再初始化 */
    if (!EnsureSingleInstance()) return 0;   /* 已有实例在跑：提示后退出 */

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
        TrayHint(L"钩子安装失败");
        return 1;
    }
    PhysSeedFromLogical();   /* 启动前就按着的键按物理按住处理（防误清） */

    /* 启动不写日志文件；采集系统/环境基线供 C+` 诊断时输出。程序启动本身不发声。 */
    NavBaseInfo();
    ConfigLoad();   /* 读 CapsEnhance.ini（缺失则写注释模板） */

    /* 配置要求以管理员运行且当前不是管理员：申请提权重启一次。
       只试一次——提权起来的实例 IsRunElevated() 为真，不会循环；
       用户取消 UAC 则按普通权限继续运行。 */
    if (ConfigGetRunAsAdmin() && !IsRunElevated()) {
        WCHAR exe[MAX_PATH];
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        HINSTANCE r = ShellExecuteW(NULL, L"runas", exe, NULL, NULL, SW_SHOWNORMAL);
        if ((int)(INT_PTR)r > 32) return 0;    /* 提权实例已拉起，本实例退出 */
    }

    g_menu = CreatePopupMenu();
    AppendMenuW(g_menu, MF_STRING, IDM_PAUSE, L"暂停映射");
    AppendMenuW(g_menu, MF_STRING, IDM_RESTART, L"重启程序");
    AppendMenuW(g_menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(g_menu, MF_STRING, IDM_EXIT, L"退出");

    TrayAdd();
    TrayHint(L"正在映射");

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