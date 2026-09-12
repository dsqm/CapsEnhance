#include "cap.h"
#include "auto.h"

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

/* 诊断记录开关：C+` 切换（hook 回调只投递 WM_PROBE，实际在这里翻转）。
   **只由主线程写**，但 act.c / send.c / nav.c 的**工作线程**会读它来决定
   要不要写日志 —— 真跨线程，所以是 std::atomic 而非 volatile（理由见 cap.h
   的"atomic / volatile 的分界"）。 */
std::atomic<bool> g_recording{ false };

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

/* ============ 日志（exe 同目录的 log\ 子目录，一次记录一个文件） ============
   为什么要单独一个子目录：记录一开就往 exe 旁边堆好几个 MB 级的日志，
   bin\ 里全是 .obj / .exe，混在一起看不出哪些是日志、也不好整份清理。

   为什么每次开始记录都新建文件：日志是"判断这一次操作"的唯一依据，而它是
   追加写的 —— 新内容永远落在旧内容后面，翻找本次会话的头部要看心情
   （此前 REC START 会被追加到上一次、甚至轮转过一半的旧文件里）。
   一次记录 = 一个 CapsEnhance-<YYYYMMDD>-<HHMMSS>.log，开记录那一刻就是
   文件第一行，不存在"不知道从哪儿读起"的问题。
   还没开始记录时写的零星日志（降权桥子进程等）落在兜底的
   CapsEnhance.log 里，不至于把文件散得完全找不着。 */
#define LOG_SUBDIR       L"log"
#define LOG_FALLBACK     L"CapsEnhance.log"
#define LOG_SESSION_ENV  L"CAPSENHANCE_LOG"   /* 传给降权桥子进程：写哪个文件 */

static WCHAR g_logPath[MAX_PATH] = { 0 };   /* 当前会话日志（绝对路径，空=未开始） */

/* exe 同目录的 log\，并保证它存在；目录建不出来（只读等）就退回 exe 目录 ——
   宁肯少一层目录，也不能让日志功能整个失效。 */
static void log_dir(WCHAR* out, size_t cap) {
    WCHAR wexe[MAX_PATH]; DWORD n = GetModuleFileNameW(NULL, wexe, MAX_PATH);
    /* 找到最后一个 '\'，截成目录 */
    WCHAR* slash = NULL;
    for (DWORD i = 0; i < n; i++) if (wexe[i] == L'\\') slash = &wexe[i];
    if (slash) *slash = 0;
    /* 全程用宽字符拼路径：经 ANSI 转换会在安装路径含非 ACP 字符时写错文件 */
    _snwprintf_s(out, cap, _TRUNCATE, L"%s\\%s", wexe, LOG_SUBDIR);
    if (!CreateDirectoryW(out, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        _snwprintf_s(out, cap, _TRUNCATE, L"%s", wexe);
}

/* 当前该往哪个文件写。g_logPath 由主线程在 LogNewSession 里写、写日志的
   线程在这里读，所以两边都进 g_logcs —— 光看这段代码很容易以为它是"只主线程
   访问"，但 CapLog 在任何线程都可能被调到。 */
static void log_path(WCHAR* out, size_t cap) {
    if (g_logPath[0]) { _snwprintf_s(out, cap, _TRUNCATE, L"%s", g_logPath); return; }
    WCHAR dir[MAX_PATH]; log_dir(dir, MAX_PATH);
    _snwprintf_s(out, cap, _TRUNCATE, L"%s\\%s", dir, LOG_FALLBACK);
}

/* 开始记录时调用一次：新建一个以本地时间命名的会话日志文件。
   同一秒里再开一次会追加序号，不会盖掉刚才那份（松开手就忘的记录是靠
   文件回访的，被覆盖就等于没发生过）。 */
void LogNewSession(void) {
    InitOnceExecuteOnce(&g_logOnce, LogCsInit, NULL, NULL);
    WCHAR dir[MAX_PATH]; log_dir(dir, MAX_PATH);
    SYSTEMTIME st; GetLocalTime(&st);
    WCHAR cand[MAX_PATH];
    for (int seq = 0; seq < 100; seq++) {
        if (seq == 0)
            _snwprintf_s(cand, MAX_PATH, _TRUNCATE,
                         L"%s\\CapsEnhance-%04d%02d%02d-%02d%02d%02d.log",
                         dir, st.wYear, st.wMonth, st.wDay,
                         st.wHour, st.wMinute, st.wSecond);
        else
            _snwprintf_s(cand, MAX_PATH, _TRUNCATE,
                         L"%s\\CapsEnhance-%04d%02d%02d-%02d%02d%02d-%d.log",
                         dir, st.wYear, st.wMonth, st.wDay,
                         st.wHour, st.wMinute, st.wSecond, seq);
        if (GetFileAttributesW(cand) == INVALID_FILE_ATTRIBUTES) break;   /* 没人占用 */
    }
    EnterCriticalSection(&g_logcs);
    wcscpy_s(g_logPath, MAX_PATH, cand);
    LeaveCriticalSection(&g_logcs);
    /* 降权桥是独立进程，读不到主进程的 g_logPath —— 用环境变量告诉它写哪，
       它继承主进程的环境（CreateProcessWithTokenW 传 lpEnvironment=NULL）。 */
    SetEnvironmentVariableW(LOG_SESSION_ENV, cand);
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
    WCHAR wpath[MAX_PATH];
    EnterCriticalSection(&g_logcs);
    log_path(wpath, MAX_PATH);      /* 取路径也在锁内：LogNewSession 会改写目标文件 */
    LeaveCriticalSection(&g_logcs);

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
   开火快照（g_actEnh/g_actShf/g_btnCount）在此刻采集并随请求带走。
   keep = 引擎复位（松开 C）时是否保留这条请求：用户按下的那一发与它的松手
   补齐要留（见 act.c 的 ActClearKeepFill），自动重复的尾流要丢。 */
static void PostAct(int act, int fillAct, int count, BOOL keep) {
    ActJob j;
    j.act     = act;
    j.fillAct = fillAct;
    j.enh     = g_actEnh;
    j.shf     = g_actShf;
    j.count   = count;
    j.keep    = keep;
    ActPost(&j);
}

/* 采集本次开火的模式快照：增强（点按补齐到 3 次）与选区（带 Shift）。
   判据是"激活键此刻是否按住"（ModeDown）—— 与鼠标钩子判断滚轮语义用的是
   同一个查询，不会分叉成两种修饰判定。
   在**按下触发键的那一刻**采集：一旦开火，之后修饰键的按/松都不影响本次动作
   （快照随 ActJob 带走，工作线程不读主线程状态）。
   由此确定的顺序语义：**修饰键必须先按住，再按触发键**。反向顺序（先按 J、
   再按 Alt）就是一次没有增强/选区的普通动作 —— 不补正、也不"过一会儿才生效"。
   这里曾经是"按下触发键后等 50ms（CHORD_SETTLE 稳定期）再开火"，用来兜住
   "动作先到、修饰后到"的乱序；代价是**每一个**触发键都要白等 50ms 才响应
   （实测按下到开火 57~71ms），用户明确要求改成严格要求顺序。 */
static void SnapMods(void) {
    g_actEnh = ModeDown(MODE_ENHANCE);
    g_actShf = ModeDown(MODE_SHIFT);
}

/* 开火一次动作（按词跳转走文本解析；其余走按键发送，都用开火快照修饰）。
   keep：这一发是"用户按下产生的"（TRUE，复位时要保留）还是"自动重复的尾流"
   （FALSE，松手/复位时丢弃）—— 两者都是同一个 act，只能由调用点说明身份。 */
static void DoFireOnce(int act, BOOL keep) { PostAct(act, ACT_NONE, 0, keep); }

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
/* 切换"暂停映射"要做的全部事情（托盘菜单调用；也供单测直接调用）。
   抽成函数是为了能被验证：**暂停必须停掉持续滚动**，否则暂停期间钩子整段被
   跳过（连"按任意键打断"都收不到），那个滚动就成了没人能停的东西。 */
void ApplyPauseState(void);
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
        ApplyPauseState();
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
    /* Windows 通知区对同一个图标"同一时刻只显示一条气泡"，且新的 NIF_INFO
       请求若赶上当前气泡还在显示，会被排队：Win10/11 下气泡被系统转成 toast，
       同来源还会被摞进队列，要等当前那条超时（约 10s）才弹 —— 现象就是
       "开始记录的隐私气泡还挂着，按 C+` 结束的气泡迟迟不出现"。
       MS 规定的立即替换办法（.NET 的 NotifyIcon 内部也是这么做的）：先发一条
       空 szInfo 把当前气泡收掉（取消待显示项），再发新内容，explorer 才会
       立即弹出新的。空 szInfo 那次在"当前没有气泡"时也无害。
       （注意：NIF_REALTIME 是反方向——"不能立即显示就丢弃"，不能用来做这个。）*/
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_INFO;
    wcscpy_s(n.szInfoTitle, 64, APP_NAME);
    n.szInfo[0] = L'\0';                       /* 第一步：收掉当前气泡（取消排队） */
    Shell_NotifyIconW(NIM_MODIFY, &n);
    wcscpy_s(n.szInfo, 256, s);                /* 第二步：新内容 → 立即弹出 */
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

/* 切换暂停 / 恢复。菜单项与单测都走这一条。 */
void ApplyPauseState(void) {
    g_state.paused = !g_state.paused;
    /* 切换暂停时清掉残留的修饰/引擎状态，避免恢复后粘滞。
       用 ModHoldReset 而不是只清 g_cMod：暂停期间钩子被跳过，
       抬起事件收不到（含真实 Shift），不主动清就会一直粘着。 */
    ModHoldReset();
    g_btnCount = 0;
    ResetEngine();
    /* **暂停必须显式停掉持续滚动**：暂停期间钩子整段被跳过（连"按任意键
       打断"都收不到），不停就等于"谁也没法让它停"，只能等 10 分钟超时 ——
       而它是往别的程序里持续注入滚轮，属于最不能留的尾巴。
       这事**不能靠 ResetEngine 代做**：松手不再打断之后，ResetEngine 已经不
       管持续滚动了（见那里的注释）。 */
    if (g_recording.load() && ActAutoScrollActive())
        CapLog("持续滚动：终止（暂停映射）");
    ActAutoScrollStop();
    if (!g_state.paused) TrayHint(L"正在映射");
    else TrayHint(L"已暂停（CapsLock 恢复原功能）");
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
    /* **持续滚动刻意不在这里停**（2026-09-11 改版）：用户要的是"滚一下就能
       松手读，想停的时候按一下键"，所以松开 C / 松开修饰键都不该打断它。
       它的终止条件见 cap.h 的持续滚动一节：反向滚、用户新输入、超时、
       以及暂停/退出/重启（后两者在各自的路径上显式调 ActAutoScrollStop）。 */
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
        g_recording.store(!g_recording.load());
        if (g_recording.load()) {
            /* 先开新文件，再写第一行：本次记录的日志从空文件开始 */
            LogNewSession();
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
                CapLog("按键: 增强=%s 选区=%s（固定值，不读配置） WheelScrollMul=%d "
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
            /* 开始记录的气泡 = **隐私提示**（原来的文案只说了"焦点探测写入日志"，
               力度不够）。记录会把焦点控件的文本片段落盘，属于"用户未必意识到
               的数据外带"，必须在开始那一刻说清；托盘悬停提示只有一行、还要
               悬停才看得到，承担不了这个提醒。 */
            TrayBalloon(L"开始记录日志。注意隐私：记录期间会写入焦点控件的文本片段"
                        L"（前 160 字符）与按词跳转位置，敏感内容上请勿长时间开启。");
        } else {
            CapLog("===== REC END =====");
            MessageBeep(MB_ICONHAND);       /* 系统"手形"音：结束记录（低而不同） */
            TrayHint(L"正在映射");
            /* 结束也给一次明确反馈：开始有气泡，结束若只有一声提示音，很容易
               让人以为"还在记录"，于是继续在敏感内容上操作。 */
            {   /* 结束气泡里带上实际文件名：一次记录一个文件，不给名字等于
                   让人去 log\ 里猜哪份是刚录的。 */
                WCHAR tip[160];
                const WCHAR* slash = wcsrchr(g_logPath, L'\\');
                _snwprintf_s(tip, 160, _TRUNCATE, L"日志记录已结束（写入 log\\%s）",
                             slash ? (slash + 1) : g_logPath);
                TrayBalloon(tip);
            }
            /* 诊断期间 UIA/MSAA 等 COM 基础设施已加载，结束记录后压回工作集 */
            TrimWorkingSet();
        }
        return 0;
    case WM_TRIG_DOWN: {             /* 触发键按下：立即开火 */
        int act = (int)wp;
        StackPush(act);
        if (g_recording.load())
            CapLog("  [和弦] %s 按下 (active=%s inflight=%d enh=%d shf=%d)",
                   ActName(act), ActName(g_activeAct), ActInFlight(),
                   (lp & TRIGF_ENH) ? 1 : 0, (lp & TRIGF_SHF) ? 1 : 0);
        /* 已在重复时按下另一个键、或当前没动作：都走这里 —— 按下即开火，
           不等稳定期（见 SnapMods 的说明）。短点（按下就抬手）同样从这条路
           开火，抬起时只补"还差的次数"（见 WM_TRIG_UP）。 */
        if (g_activeAct != act) {
            /* 开火快照取**钩子随消息带来的那份**（按下事件发生的那一刻），
               不是现在现读 ModeDown —— 钩子回调与本处理是异步的，详见 hook.c
               的 ModeSnapFlags。这也是"修饰键必须先按住、再按触发键"这条顺序
               语义的落地：J 按下那一刻 Alt 有没有按住，就决定这一发带不带增强。 */
            g_actEnh = (lp & TRIGF_ENH) != 0;
            g_actShf = (lp & TRIGF_SHF) != 0;
            g_btnCount = 1;
            DoFireOnce(act, TRUE);
            if (!IsSingleShotAct(act)) {
                /* 开关键只切一次、也不接管重复节奏（原来那个动作继续重复） */
                g_activeAct = act;
                g_activeNext = GetTickCount64() + REPEAT_FIRST_DELAY;
            } else {
                /* 已经切过了：从"仍按着"的栈里摘掉，否则之后松开别的键时
                   "退回上一个"会把它再切一次（两次切换 = 又切回原状态） */
                StackRemove(act);
            }
        }
        EnsEngine();
        return 0;
    }
    case WM_TRIG_UP: {               /* 触发键抬起 */
        int act = (int)wp;
        StackRemove(act);
        if (g_recording.load())
            CapLog("  [和弦] %s 抬起 (active=%s) -> %s", ActName(act),
                   ActName(g_activeAct),
                   (g_activeAct == act) ? "停重复+按增强模式补齐" : "与本动作无关");
        if (g_activeAct == act) {
            /* 抬起 = 停重复 + 按增强模式补齐。
               "点按"与"长按"走的是同一条路：开火都发生在按下那一刻，这里只
               补"还差的次数"（增强模式补到 3 次）。原来点按走的是另一条分支
               （稳定期内抬起才开火+补齐），那条分支随稳定期一起去掉了。
               快照要在清空 g_actEnh/g_actShf 之前用，故先投递。 */
            g_activeAct = ACT_NONE;
            PostAct(ACT_NONE, act, g_btnCount, TRUE);
            g_actEnh = FALSE; g_actShf = FALSE;
            int top = StackTop();
            if (top != ACT_NONE) {
                /* 别的键还按着：切回它并继续重复（同样按下即开火，不等稳定期） */
                SnapMods();
                g_activeAct = top;
                g_btnCount = 1;
                DoFireOnce(top, TRUE);
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
           被发送过程占住、定时器跑不了"的自然限速。没有它，长按按词跳转键会
           在队列里堆出一串尾流。 */
        if (g_activeAct != ACT_NONE && now >= g_activeNext
            && ActInFlight() == 0) {
            if (IsSingleShotAct(g_activeAct)) {
                /* 防御：开关键永远不重复（正常路径到不了这里） */
                g_activeAct = ACT_NONE;
                StopEngine();
            } else {
                DoFireOnce(g_activeAct, FALSE);
                g_btnCount++;
                g_activeNext = now + REPEAT_INTERVAL;
            }
        } else if (g_activeAct == ACT_NONE && ActInFlight() == 0) {
            StopEngine();
        }
        return 0;
    }
    case WM_WHEEL_ACCEL:
        /* Caps+滚轮：快速滚动。只投递给专用线程，**主线程绝不在这里做注入**。
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
        /* 某个修饰模式的激活键全部抬起了（钩子投递）。
           **这里刻意不终止持续滚动**（2026-09-11 改版）：松开 Alt/D 不再打断，
           滚动继续，直到用户按任意键/鼠标，或超过 10 分钟上限（act.c）。
           （此前这里有一句 `if (mode == MODE_ENHANCE) ActAutoScrollStop()`，
           与工作线程每拍自查 g_cMod/ModeDown 一起构成"松手即停"，现均按用户
           要求移除。） */
        (void)wp;
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

    /* --wps-read：读一次 WPS 光标处文本并写日志（自查"WPS 里为什么不生效"）。
       放在单实例检查之前：它不该被已经在跑的实例挡住。 */
    if (HasSwitch(L"--wps-read", NULL))
        return WpsSelfTest();

    /* --bridge <父pid> <序号>：降权桥的子进程模式（见 src/bridge.c）。
       它是本 exe 的"普通权限副本"，只在别的进程里代跑那些"只有普通权限才看得见
       的对象"，不装钩子、不建窗口、不建托盘 —— 必须抢在单实例检查之前返回。
       命令行开关的解析与拼接都归 bridge.c（它自己定义管道名，主程序不必知道）。 */
    {
        DWORD bpid = 0, bseq = 0;
        if (BrParseChildSwitch(&bpid, &bseq))
            return WpsBridgeServe((unsigned long)bpid, (unsigned long)bseq);
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

    /* 启动时清一次"上次留下的卡键"：程序被强杀（或上电异常）时，
       它注入过的修饰键可能还停在"按下"状态，而状态表随进程一起消失了 ——
       受害的是**别的**程序，表现为"Alt/Ctrl 一直按着"。
       判据是"系统认为按着 且 用户物理上没按"（见 send.c），所以刚播种完
       的物理表正好用来保护用户此刻真按着的键，不会误清。
       放在装钩子之前：此时我们注入的抬起不会被自己的钩子当成输入处理。 */
    SendClearStuckModifiers();

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
    WpsBridgeShutdown();         /* 让 WPS 降权桥立刻退出（否则要等它的空闲超时） */
    HookUninstall();
    SendClearStuckModifiers();   /* 退出前清掉可能卡住的注入修饰键 */
    TrayRemove();
    if (g_menu) { DestroyMenu(g_menu); g_menu = NULL; }
    return rc;
}