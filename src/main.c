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

/* 和弦引擎状态（仅主线程）：开火快照与"本动作已发送次数"。
   实际发送由 act.c 的工作线程执行，快照随 ActJob 一起投递过去。
   快照字段不叫 D/F 而叫 enh/shf：按哪个键由配置决定（见 keymap.c 的模式表）。 */
volatile BOOL g_actEnh = FALSE;
volatile BOOL g_actShf = FALSE;
volatile int  g_btnCount = 0;

/* 诊断记录开关：C+` 切换（主线程读写，hook 线程投递 WM_PROBE） */
volatile BOOL g_recording = FALSE;

static CRITICAL_SECTION g_logcs;
/* 一次性初始化。此前是手写的双检锁：CAS 抢到 0 的线程还没
   InitializeCriticalSection 完，另一个线程就可能已经 EnterCriticalSection
   进去了。当时只有主线程写日志所以没暴露，但 act.c 的工作线程也会写 ——
   必须换成原子的 InitOnce。 */
static INIT_ONCE g_logOnce = INIT_ONCE_STATIC_INIT;
/* 轮转专用锁：与写日志的 g_logcs 分开，见 CapLog 里的说明。 */
static CRITICAL_SECTION g_rotcs;
static volatile LONG    g_rotatePending = 0;
static BOOL CALLBACK LogCsInit(PINIT_ONCE o, PVOID p, PVOID* c) {
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&g_logcs);
    InitializeCriticalSection(&g_rotcs);
    return TRUE;
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
/* 是否已达上限（只做一次 stat，非常便宜，可以放在写日志的路径上）。
   上限由配置 [General] LogMaxKB 控制，0 = 不限制。 */
static BOOL log_over_limit(const WCHAR* path) {
    int maxKB = ConfigGetLogMaxKB();
    if (maxKB <= 0) return FALSE;
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &d)) return FALSE;
    LONGLONG sz = ((LONGLONG)d.nFileSizeHigh << 32) | (LONGLONG)d.nFileSizeLow;
    return sz > (LONGLONG)maxKB * 1024;
}

/* 保留最近的一半（老内容丢弃），避免长期开着记录涨到几十 MB。
   只在"确实超限"的那一次做，平时不碰 —— 它要读回半个文件再整写一遍，
   代价远高于普通写日志，所以**绝不能放在写日志的锁里**（详情见 CapLog）。 */
static void log_rotate(const WCHAR* path) {
    int maxKB = ConfigGetLogMaxKB();
    if (maxKB <= 0) return;
    LONGLONG keep = (LONGLONG)maxKB * 1024 / 2;
    /* 硬上限，双保险：LogMaxKB 已在配置层钳到 65536（keep 最大约 32MB），
       但这里再兜一道，避免以后有人改了配置上限却忘了 32 位构建下
       (size_t)keep 会回绕 —— 回绕后 malloc 拿到小缓冲、fread 按大长度写，
       就是堆越界。 */
    if (keep > 64 * 1024 * 1024) keep = 64 * 1024 * 1024;
    if (keep <= 0) return;

    FILE* f = NULL;
    if (_wfopen_s(&f, path, L"rb") != 0 || !f) return;
    if (_fseeki64(f, -keep, SEEK_END) != 0) { fclose(f); return; }
    size_t want = (size_t)keep;                /* 读的长度 == 分配的长度 */
    char* buf = (char*)malloc(want + 1);
    if (!buf) { fclose(f); return; }
    size_t got = fread(buf, 1, want, f);
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

/* 每条日志都开-写-关，**不要**改成"常驻句柄"：
   Windows 的文件共享规则要求"新打开者声明的共享模式必须覆盖已授权给现有
   句柄的所有访问权"。只要我们把句柄一直开着（哪怕用 _SH_DENYNO 打开），
   只共享读的读者就打不开了 —— 实测 plain fopen("r")（MSVC CRT 默认）返回
   EACCES、_SH_DENYWR 同样失败，PowerShell Get-Content 一类的读者也会失败，
   而日志恰恰是"程序一边跑一边想打开看"的东西。logsmoke 就靠这条覆盖住了。
   逐条开-关在记录模式下只是多条 syscall，且现在日志写入大多发生在执行线程
   （见 act.c），不再压在主线程上 —— 不值得为它换掉可读性。 */
void CapLog(const char* fmt, ...) {
    InitOnceExecuteOnce(&g_logOnce, LogCsInit, NULL, NULL);
    WCHAR wpath[MAX_PATH]; log_path(wpath, MAX_PATH);

    /* 日志轮转**放在写锁之外**：它要读回半个文件再整写一遍，是几十毫秒级的
       I/O。放在锁里会把其它写日志的线程（含 act.c 的工作线程）一起拖住 ——
       而那正是"主线程被阻塞"的同一种味道。
       做法：超限检测（一次 stat）留在常规路径上；真正的轮转由**碰巧拿到
       g_rotcs 的那个线程**做，拿不到就跳过、下次再说 —— 于是没有任何线程
       会为了轮转而等待。轮转与写入可能短暂交错，最坏丢掉一行日志，这对
       诊断日志是可接受的代价。 */
    if (log_over_limit(wpath)) InterlockedExchange(&g_rotatePending, 1);
    if (InterlockedCompareExchange(&g_rotatePending, 0, 0) &&
        TryEnterCriticalSection(&g_rotcs)) {
        if (log_over_limit(wpath)) log_rotate(wpath);   /* 双重检查：可能已被别人转过 */
        InterlockedExchange(&g_rotatePending, 0);
        LeaveCriticalSection(&g_rotcs);
    }

    EnterCriticalSection(&g_logcs);
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

/* 把一次动作投递给执行线程（见 act.c）。
   主线程不再自己发送：逐格方向键 / 跨进程同步 IPC / COM 往返会阻塞数百
   毫秒，而 WH_KEYBOARD_LL 回调正跑在主线程上 —— 期间系统键盘输入得不到
   处理（卡顿），超时还会被系统静默摘钩。
   开火快照（g_actEnh/g_actShf/g_btnCount）在此刻采集并随请求带走。 */
static void PostAct(int act, int fillAct, int count) {
    ActJob j;
    j.act     = act;
    j.fillAct = fillAct;
    j.enh     = g_actEnh;
    j.shf     = g_actShf;
    j.count   = count;
    ActPost(&j);
}

/* 采集本次开火的模式快照：增强（点按补齐到 3 次）与位移（带 Shift）。
   判据是"激活键此刻是否按住"（ModeDown）—— 与鼠标钩子判断滚轮语义用的是
   同一个查询，不会分叉成两种修饰判定。
   "动作先按、修饰后到"的乱序由 CHORD_SETTLE（50ms 稳定期）cover：开火前
   修饰到位即可，开火之后的修饰变化不影响本次动作（快照随 ActJob 带走，
   工作线程不读主线程状态）。 */
static void SnapMods(void) {
    g_actEnh = ModeDown(MODE_ENHANCE);
    g_actShf = ModeDown(MODE_SHIFT);
}

/* 开火一次动作（词跳走文本解析；其余走按键发送，都用开火快照修饰） */
static void DoFireOnce(int act) { PostAct(act, ACT_NONE, 0); }

/* 翻转类动作（开关键：按一次只该切换一次）。属性现在写在键位表里
   （ACTF_SINGLE_SHOT），这里只是给引擎内部一个好读的名字 —— 以前是
   `return act == ACT_CAPSLOCK;`，加一个翻转动作就得回来改这里。 */
static BOOL IsSingleShotAct(int act) { return ActHasFlag(act, ACTF_SINGLE_SHOT); }

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
    if (!g_menu) return;                 /* 菜单创建失败时不要拿 NULL 去用 */
    POINT pt;
    GetCursorPos(&pt);
    SetMenuPauseText();
    SetForegroundWindow(g_mainWnd);
    int cmd = TrackPopupMenu(g_menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                             pt.x, pt.y, 0, g_mainWnd, NULL);
    if (cmd == IDM_PAUSE) {
        g_state.paused = !g_state.paused;
        /* 切换暂停时清掉残留的修饰/引擎状态，避免恢复后粘滞。
           用 ModHoldReset 而不是只清 g_cMod：暂停期间钩子被跳过，
           抬起事件收不到（含真实 Shift），不主动清就会一直粘着。 */
        ModHoldReset();
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

/* 弹一次气泡通知。用于"钩子安装失败""诊断已写入日志"这类必须被看到、
   但不该打断操作的结果 —— 之前是用模态 MessageBox 弹在主线程上，
   会把和弦引擎和键盘响应一起冻住。 */
void TrayBalloon(const WCHAR* s) {
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_INFO;
    wcscpy_s(n.szInfoTitle, 64, APP_NAME);
    wcscpy_s(n.szInfo, 256, s);
    Shell_NotifyIconW(NIM_MODIFY, &n);
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
    /* 丢掉还没开始的"重复开火"尾流，但保留"松手补齐" —— 后者是用户本次
       点按的一部分，不能因为紧接着松开 C 就被清掉。 */
    ActClearKeepFill();
    /* 滚轮加速的待补队列也一起丢：松开 C 之后不该再有加速尾流
       （用户预期"松开 C 就是普通滚轮"）。 */
    ActWheelClear();
    /* 自动滚动一律立即终止（松开 C / 重新按下 C 都该停） */
    ActAutoScrollStop();
    g_pendingAct = ACT_NONE;
    g_activeAct  = ACT_NONE;
    g_actEnh = FALSE; g_actShf = FALSE;
    /* 刻意不动修饰键的按住状态：Alt 还按着时，本次 C 会话内的增强模式
       判定依然成立（ModeDown 是唯一判据，见 cap.h）。以前这里会单独清一份
       "门闩"位图，导致"按住 Alt 再按 C"时点按补齐静默失效。 */
    StackClear();
}

/* 重启程序：清键盘状态 -> 拉起新实例（新实例带 --wait-exit <本进程PID>，
   等本进程退出后才安装钩子）-> 本进程退出。
   清键盘 = 对可能注入过的修饰键补"抬起"，消除目标程序里的卡键；
   钩子 / 和弦引擎 / 修饰门闩随进程重启全部回到干净初态。 */
static void DoRestart(void) {
    ResetEngine();
    ModHoldReset();
    g_btnCount = 0;
    /* 必须先停执行线程并等它收尾：否则它可能正在注入，而 SendClearStuckModifiers
       要读 send.c 里由它维护的 g_injectedDown（清卡键会读错）。 */
    ActStop();
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

/* 命令行开关：按"整词"匹配。此前用 wcsstr 做子串匹配 —— 安装路径或参数里
   恰好含 "--wait-exit" 就会误命中。边界要求两侧是空白或串首尾。
   flag 形如 L"--wait-exit"；args 非空时写出其后紧跟的十进制参数。 */
static BOOL HasSwitch(const WCHAR* flag, DWORD* argOut) {
    const WCHAR* cmd = GetCommandLineW();
    if (!cmd || !flag) return FALSE;
    size_t fl = wcslen(flag);
    for (const WCHAR* p = cmd; (p = wcsstr(p, flag)) != NULL; p += fl) {
        BOOL lb = (p == cmd) || (p[-1] == L' ' || p[-1] == L'\t');
        WCHAR c = p[fl];
        if (!lb || (c != 0 && c != L' ' && c != L'\t')) continue;
        if (argOut) *argOut = c ? (DWORD)wcstoul(p + fl + 1, NULL, 10) : 0;
        return TRUE;
    }
    return FALSE;
}

/* 重启流程：新实例带 --wait-exit <pid>，等旧实例退出（最多 5 秒）后再
   创建窗口/安装钩子，避免两个实例的钩子与托盘图标短暂并存。 */
static void WaitOldInstanceExit(void) {
    DWORD pid = 0;
    if (!HasSwitch(L"--wait-exit", &pid) || !pid) return;
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
           并同步托盘悬停提示内容。探测结果只写日志（原来会弹一个模态
           对话框，既冻住引擎又难以阅读/复制）。 */
        g_recording = !g_recording;
        if (g_recording) {
            CapLog("===== REC START =====");
            /* 配置值入日志：排查"配置没生效"类问题时一目了然 */
            CapLog("配置: MaxArrowSteps=%d OverStepCtrl=%d LogMaxKB=%d RunAsAdmin=%d",
                   ConfigGetMaxArrowSteps(), ConfigGetOverStepCtrl() ? 1 : 0,
                   ConfigGetLogMaxKB(), ConfigGetRunAsAdmin() ? 1 : 0);
            {   /* 实际生效的修饰激活键（编译期常量，见 cap.h）：排查按键映射时一眼可见 */
                WCHAR ek[MODE_KEYS_MAXLEN], sk[MODE_KEYS_MAXLEN];
                char  eb[MODE_KEYS_MAXLEN * 3], sb[MODE_KEYS_MAXLEN * 3];
                ModeKeysText(MODE_ENHANCE, ek, MODE_KEYS_MAXLEN);
                ModeKeysText(MODE_SHIFT,   sk, MODE_KEYS_MAXLEN);
                WideCharToMultiByte(CP_UTF8, 0, ek, -1, eb, (int)sizeof(eb), NULL, NULL);
                WideCharToMultiByte(CP_UTF8, 0, sk, -1, sb, (int)sizeof(sb), NULL, NULL);
                CapLog("按键: 增强=%s 位移=%s（固定值，不读配置） WheelScrollMul=%d "
                       "AutoScrollInterval=%d AutoScrollFastMs=%d",
                       eb, sb, ConfigGetWheelScrollMul(), ConfigGetAutoScrollInterval(),
                       ConfigGetAutoScrollFastMs());
            }
            {   /* 持续滚动的两端方向：AutoScrollFastMs 是"最快"，毫秒数必须
                   **小于** AutoScrollInterval（起步）才有提速空间。写反了不会
                   崩（act.c 里 `if (fast > slow) fast = slow`），但提速阶梯会
                   **静默退化成恒定速度** —— 现象只是"再滚一下没变快"，极难
                   自查，所以在诊断日志里明说。 */
                int slow = ConfigGetAutoScrollInterval();
                int fast = ConfigGetAutoScrollFastMs();
                CapLog("自动滚动: 起步=%dms/格 最快=%dms/格 %s", slow, fast,
                       (fast >= slow) ? "（!! 最快 >= 起步：提速已退化为恒速）" : "");
            }
            MessageBeep(MB_ICONASTERISK);   /* 系统"星号"音：开始记录（与结束不同） */
            TrayHint(L"正在记录日志（C+\x60 结束）");
            /* 探测交给执行线程：NavProbe 会做跨进程 SendMsg + UIA COM 往返，
               是几百毫秒级的阻塞。留在主线程就是同一个根因 —— 期间松开 Caps
               的 keyup 会被系统跳过，g_cMod 卡住，之后按 Tab 冒出大写锁定。 */
            { ActJob j; memset(&j, 0, sizeof(j)); j.act = ACT_PROBE; ActPost(&j); }
            TrayBalloon(L"焦点探测写入 CapsEnhance.log");
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
            SnapMods();
            g_btnCount = 1;
            DoFireOnce(act);
            if (!IsSingleShotAct(act)) {
                /* 开关键只切一次、也不接管重复节奏（原来那个动作继续重复） */
                g_activeAct = act;
                g_activeNext = GetTickCount64() + REPEAT_FIRST_DELAY;
            } else {
                /* 已经切过了：从"仍按着"的栈里摘掉，否则之后松开别的键时
                   "退回上一个"会把它再切一次（两次切换 = 又切回原状态） */
                StackRemove(act);
            }
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
            /* 短点：稳定期内即抬起，立即开火一次并补齐（修饰用门闩快照）。
               开火 + 补齐合成一条请求：中间不会被别的动作插队，也用同一份
               快照（旧代码靠连续两次同步调用达成同样效果）。 */
            g_pendingAct = ACT_NONE;
            SnapMods();
            g_btnCount = 1;
            /* 开关键不做松手补齐（补齐会把一次点按变成 3 次切换，等于又切回去） */
            PostAct(act, IsSingleShotAct(act) ? ACT_NONE : act, g_btnCount);
            g_actEnh = FALSE; g_actShf = FALSE;
            /* 还有别的键按着就继续（退回上一个），否则停 */
            int top = StackTop();
            if (top != ACT_NONE) {
                g_pendingAct = top;
                g_pendingTick = GetTickCount64();
                EnsEngine();
            } else StopEngine();
        } else if (g_activeAct == act) {
            /* 长按之后抬起：停止重复，若有增益短板则补齐（快照要在清空
               g_actEnh/g_actShf 之前采集，故先投递） */
            g_activeAct = ACT_NONE;
            PostAct(ACT_NONE, act, g_btnCount);
            g_actEnh = FALSE; g_actShf = FALSE;
            int top = StackTop();
            if (top != ACT_NONE) {
                /* 别的键还按着：切回它并继续重复 */
                SnapMods();
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
        /* ActInFlight() 门控：同一时刻只让一发动作在飞，等价于原来"主线程
           被发送过程占住、定时器跑不了"的自然限速。没有它，长按词跳键会
           在队列里堆出一串尾流。 */
        if (g_pendingAct != ACT_NONE && now >= g_pendingTick + CHORD_SETTLE
            && ActInFlight() == 0) {
            /* 稳定期到：用门闩快照修饰，开火 */
            int act = g_pendingAct; g_pendingAct = ACT_NONE;
            SnapMods();
            g_btnCount = 1;
            DoFireOnce(act);
            if (IsSingleShotAct(act)) {
                /* 开关键：切一次就结束，不进重复节奏。
                   同时把它从"仍按着"的栈里摘掉 —— 否则之后松开别的键时会
                   "退回上一个"又把它补切一次。 */
                StackRemove(act);
                g_actEnh = FALSE; g_actShf = FALSE;
                int top = StackTop();
                if (top != ACT_NONE) {          /* 还按着别的键：接着它走 */
                    g_pendingAct = top;
                    g_pendingTick = now;
                } else StopEngine();
            } else {
                g_activeAct = act;
                g_activeNext = now + REPEAT_FIRST_DELAY;
            }
        } else if (g_activeAct != ACT_NONE && now >= g_activeNext
                   && ActInFlight() == 0) {
            if (IsSingleShotAct(g_activeAct)) {
                /* 防御：开关键永远不重复（正常路径到不了这里） */
                g_activeAct = ACT_NONE;
                StopEngine();
            } else {
                DoFireOnce(g_activeAct);
                g_btnCount++;
                g_activeNext = now + REPEAT_INTERVAL;
            }
        } else if (g_pendingAct == ACT_NONE && g_activeAct == ACT_NONE
                   && ActInFlight() == 0) {
            StopEngine();
        }
        return 0;
    }
    case WM_WHEEL_ACCEL:
        /* Caps+滚轮：逐格放大。只投递给专用线程，**主线程绝不在这里做注入**。
           原来在这里直接 SendWheelAccel（内含 Sleep 逐格补发），主线程一进
           Sleep 就不在"等消息"状态，系统会把这段时间到达的低级钩子事件**直接
           跳过**：松开 Caps 的 keyup 被漏掉 -> g_cMod 卡在 TRUE -> 松开 C 后
           滚轮还在滚、随后按 Tab 又冒出大写锁定。同一根因也造成倍数高时卡顿。 */
        ActWheelPost((int)wp, (int)lp);
        return 0;
    case WM_WHEEL_ENH:
        /* Caps+Alt/D+滚轮：驱动持续滚动（act.c 的持续滚动通道）。
           这里同样只投递，主线程绝不做注入。 */
        ActAutoScrollWheel((int)wp);
        return 0;
    case WM_WHEEL_HACCEL:
        /* Caps+Shift+滚轮：横向滚动。钩子已经吞掉物理那一格（否则会横纵混滚），
           所以这里要注入**全部** hmul 格。同样只投递、不在主线程注入。 */
        ActWheelPostH((int)wp, (int)lp);
        return 0;
    case WM_MODE_UP:
        /* 某个修饰模式的激活键全部抬起了（钩子投递）：增强模式一失效，
           进行中的持续滚动立刻停 —— 这就是"松开 Alt 立即终止"的快速路径
           （工作线程每拍也会自查一遍，两条路互为兜底）。 */
        if ((int)wp == MODE_ENHANCE) ActAutoScrollStop();
        return 0;
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

/* 形参写成与 SDK 声明一致的 SAL 批注：不加的话 /analyze 会报 C28251
   （"WinMain 的批注不一致"）。这是纯噪音，盖掉它才能让这条步骤的输出只剩
   真正需要看的发现。 */
int WINAPI WinMain(_In_ HINSTANCE hInst, _In_opt_ HINSTANCE hPrev,
                   _In_ LPSTR lpCmd, _In_ int nShow) {
    (void)hInst; (void)hPrev; (void)lpCmd; (void)nShow;
    int rc = 0;
    g_state.paused = FALSE;

    /* --reset-keys：无条件把所有修饰键补一次"抬起"然后退出。
       用于程序已被杀死（状态表随进程消失）留下的卡键，等价于 AHK 社区
       常用的"把所有修饰键 Send Up 一遍"。可自建快捷方式随时调用。 */
    if (HasSwitch(L"--reset-keys", NULL)) {
        SendForceResetKeys();
        return 0;
    }

    WaitOldInstanceExit();   /* 重启场景：等旧实例完全退出再初始化 */
    if (!EnsureSingleInstance()) return 0;   /* 已有实例在跑：提示后退出 */

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"CapsEnhanceMain";
    if (!RegisterClassW(&wc)) return 1;

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

    /* 先建托盘：后面任何一步失败才有地方提示用户。此前钩子安装失败时
       调 TrayHint，但托盘图标还没创建，提示全部落空 —— 用户看不到原因。 */
    TrayAdd();

    /* 装钩子之前的准备**必须全部做完**再装钩子 ——
       钩子一生效，C 按住期间的普通键就会被 hook.c 直接吞掉（return 1）。
       此前顺序是 ActStart -> HookInstall -> ConfigLoad -> KeyMapInit，于是
       "钩子已生效、键位表还是空的"这个窗口期里：所有键既不是触发键也不是
       模式键，Caps+任意键被吞且没有任何动作；滚轮也读到的是静态初值而不是
       用户配置。实测只有数毫秒，但正是"启动瞬间按 Caps 不响应"这类难复现
       问题的标准成因。现在：配置 -> 键位表 -> 执行线程 -> 物理状态种子
       -> 最后才装钩子。 */
    ConfigLoad();   /* 读 CapsEnhance.ini（缺失则写注释模板） */
    KeyMapInit();   /* 装配修饰激活键（编译期常量 Alt+D / F，见 cap.h） */

    /* 执行线程要在装钩子之前起来：装了钩子就随时可能有动作要发。 */
    if (!ActStart())
        CapLog("ActStart 失败：动作退回主线程同步执行（输入响应性下降）");

    /* 物理状态种子也要在装钩子之前：装完之后才"补种"，会漏掉装钩瞬间
       那一小段真实按键。 */
    PhysSeedFromLogical();   /* 启动前就按着的键按物理按住处理（防误清） */

    if (!HookInstall()) {
        TrayHint(L"钩子安装失败（可能被安全软件拦截）");
        TrayBalloon(L"CapsEnhance 启动失败：键盘钩子安装失败，可能被安全软件拦截。");
        rc = 1;
        g_quit = TRUE;
        DestroyWindow(g_mainWnd);      /* 走统一收尾，不留托盘残影 */
    }

    if (!g_quit) {
        /* 启动不写日志文件；采集系统/环境基线供 C+` 诊断时输出。启动不发声。 */
        NavBaseInfo();

        /* 配置要求以管理员运行且当前不是管理员：申请提权重启一次。
           提权实例带 --wait-exit —— 必须等本实例释放单实例互斥量之后再装
           钩子，否则两边抢同一把锁，提权实例会误判"已在运行"而直接退出，
           结果是提权静默失败。用户取消 UAC 则按普通权限继续运行。 */
        if (ConfigGetRunAsAdmin() && !IsRunElevated()) {
            WCHAR exe[MAX_PATH] = L"";
            GetModuleFileNameW(NULL, exe, MAX_PATH);
            WCHAR args[64];
            _snwprintf_s(args, 64, _TRUNCATE, L"--wait-exit %lu",
                         (unsigned long)GetCurrentProcessId());
            HINSTANCE r = ShellExecuteW(NULL, L"runas", exe, args, NULL, SW_SHOWNORMAL);
            if ((int)(INT_PTR)r > 32) {
                /* 提权实例已拉起。这里不能裸 return —— 那会跳过卸载钩子与
                   清卡键收尾，在被让出的窗口里留下卡键和托盘残影。 */
                g_quit = TRUE;
                DestroyWindow(g_mainWnd);
            }
        }
    }

    if (!g_quit) {
        g_menu = CreatePopupMenu();
        if (g_menu) {
            AppendMenuW(g_menu, MF_STRING, IDM_PAUSE, L"暂停映射");
            AppendMenuW(g_menu, MF_STRING, IDM_RESTART, L"重启程序");
            AppendMenuW(g_menu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(g_menu, MF_STRING, IDM_EXIT, L"退出");
        }
        /* 鼠标钩子（滚轮加速）装不上不影响主映射，但要让人看见，别静默降级 */
        TrayHint(HookMouseAvailable() ? L"正在映射" : L"正在映射（滚轮功能不可用）");
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        if (g_quit) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    /* ---- 统一收尾：所有启动失败路径都汇到这里 ---- */
    StopEngine();
    ActStop();                   /* 停执行线程并等它收尾，再清卡键 */
    HookUninstall();
    SendClearStuckModifiers();   /* 退出前清掉可能卡住的注入修饰键 */
    TrayRemove();
    if (g_menu) { DestroyMenu(g_menu); g_menu = NULL; }
    return rc;
}