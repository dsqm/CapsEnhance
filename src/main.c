#include "cap.h"
#include "auto.h"

#include <shellapi.h>
#include <commctrl.h>
#include <imm.h>
#include <mmsystem.h>   /* PlaySoundW：记录日志的起止提示音走系统事件别名 */
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
    int maxKB = CfgInt(CFG_LOG_MAX_KB);
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
    int maxKB = CfgInt(CFG_LOG_MAX_KB);
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
    /* 需要"目标窗口"的动作（打开所在文件夹 / 窗口置顶）在这里一并快照 ——
       与 enh/shf 同一个时机（按下触发键的这一发）、同一个理由：执行线程是
       异步的，等它跑到时前台窗口可能已经换了（见 cap.h 的 ActJob.hwnd）。
       postTick 不在这里填：投递时刻由 act.c 的 ActPost 写。 */
    j.hwnd    = (ActHasFlag(act, ACTF_OPENFOLDER) || ActHasFlag(act, ACTF_PIN))
                    ? GetForegroundWindow() : NULL;
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
   （实测按下到开火 57~71ms），故改为严格要求顺序。 */
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

/* 程序图标：exe 资源里的 app.ico（16/24/32 三帧 PNG，见 res/app.rc，资源号 1）。
   托盘显示的是"小图标"：**请求 32（最大帧）或 SM_CXSMICON 中较大者**。
   为什么不直接用 SM_CXSMICON：本进程已声明 Per-Monitor V2（见 WinInitDpiAwareness），
   SM_CXSMICON 是**真实**值（本机 175% 下 = 28），但不同机器可能是 16/24/32/40…；
   而本 exe 只有 16/24/32 三帧。取两者的较大值 = "有 32 就交给 explorer 缩小
   （缩小几乎无损），要的比 32 还大时也尽量给足"。
   参数不能叫 small：rpcndr.h 里 `small` 是个类型（unsigned char）。
   LR_SHARED：系统缓存图标句柄，不需要（也不得）DestroyIcon。 */
static HICON LoadAppIcon(BOOL wantSmall) {
    int want = GetSystemMetrics(SM_CXSMICON);
    int cx = wantSmall ? (want > 32 ? want : 32) : 32;
    return (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1),
                             IMAGE_ICON, cx, cx, LR_SHARED);
}
#define IDM_PAUSE    1
#define IDM_SETTINGS 2
#define IDM_EXIT     3
#define IDM_RESTART  4
#define IDM_FOLDER   5   /* 程序文件夹：打开本 exe 所在目录（见 TrayShowMenu 的分支） */
#define IDM_UNPINALL 6   /* 取消全部置顶：Caps+T 钉住的窗口一次性全放（见 pin.c） */
/* 设置程序：**独立 exe**，与本 exe 同目录。
   为什么不把设置界面做进本体：本体是托盘常驻程序，它的主线程是键盘钩子的
   宿主 —— 任何模态窗口/重活都会把钩子拖住（见 act.c 文件头）。设置界面要跑
   消息循环、持有编辑控件、还要写文件，属于"另一类进程"。 */
#define SETTINGS_EXE L"CapsEnhanceSettings.exe"

/* 把进程工作集压回待机内存：UIA/TSF 等 DLL 一旦加载无法卸载，只能压页。
   用于"诊断结束"和"托盘菜单关闭"这类低频节点，让内存数值回落；
   页面被换出后再次使用时会自动换回，仅一次软缺页开销。 */
static void TrimWorkingSet(void) {
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

/* 记录日志的起止提示音（C+`）。两声必须**明显不同**，且都不像"出事了"：
   原先结束用的是 MessageBeep(MB_ICONHAND)，那正是系统给**严重错误/UAC 提权**
   配的音 —— 记录本来就常开常关，每次结束都响一声"出错了"，用户会去查一个
   并不存在的问题；而且它和"未按管理员运行时弹窗"的提示音同源，两个语义完全
   不同的信号共用一个声音，等于两个都没说清。
   改用 PlaySound 直接点名系统事件别名：声音方案里可被用户单独改，也能在
   "无声"方案下安静下来（MessageBeep 在"无声"方案下同样是静音，行为一致）。
   取 SystemAsterisk（开始）/ SystemExclamation（结束）：前者是"注意"，后者只是
   "提醒"，都远在错误音之外，且人耳容易分辨。
   别名缺失（注册表被精简）时退回 MessageBeep：宁可音色退化成原来那样，也不要
   完全没反馈 —— 结束记录时"有没有响"本身就是用户判断状态的主要依据。 */
void RecBeep(BOOL start) {
    const WCHAR* alias = start ? L"SystemAsterisk" : L"SystemExclamation";
    if (PlaySoundW(alias, NULL, SND_ALIAS | SND_ASYNC | SND_NODEFAULT)) return;
    MessageBeep(start ? MB_ICONASTERISK : MB_ICONEXCLAMATION);
}

#ifndef CAPS_NO_TRAY_MENU
static void SetMenuPauseText(void);
#endif
static void ResetEngine(void);   /* 前向声明（定义在下方，TrayShowMenu 先调用） */
static void DoRestart(void);     /* 前向声明（定义在下方，TrayShowMenu 先调用） */
/* 切换"暂停映射"要做的全部事情（托盘菜单调用；也供单测直接调用）。
   抽成函数是为了能被验证：**暂停必须停掉持续滚动**，否则暂停期间钩子整段被
   跳过（连"按任意键打断"都收不到），那个滚动就成了没人能停的东西。 */
void ApplyPauseState(void);
void TrayAddHint(const WCHAR* s);
/* 托盘气泡。`clickPath` != NULL 时气泡**可点击**：用户点了它就在资源管理器里
   打开并选中那个路径（复用 C+F3 的 shell 链路，见 folder.c 的 FolderRevealPath）。
   `clickPath` 指向的内部缓冲区会被拷走，所以传栈上的临时变量也行。 */
void TrayBalloon(const WCHAR* s, const WCHAR* clickPath);
static void TrayBalloonClearClick(void);   /* 定义在下方：TrayRebuild 要作废旧目标 */
/* 记录日志的起止提示音（开始 / 结束），定义在下方。抽成函数是为了让"两声必须
   不同"这条约束有个单一落点，不再散在 WM_PROBE 的两个分支里各写一遍。 */
void RecBeep(BOOL start);
/* 托盘悬停提示统一入口：按"真实运行权限"追加管理员标注
   （查进程令牌，不看配置值，避免配置与实际不一致时误导） */
static void TrayHint(const WCHAR* state) {
    WCHAR buf[160];
    _snwprintf_s(buf, 160, _TRUNCATE,
                 IsRunElevated() ? L"%s%s（管理员）" : L"%s%s",
                 L"CapsEnhance：", state);
    TrayAddHint(buf);
}

/* 按**当前全部状态**算出该显示的提示文案并写上去。
   为什么需要它：悬停提示的内容由好几个状态共同决定（暂停 / 记录中 /
   [AutoDisable] 命中 / 滚轮钩子可用性），而这些状态各自只在自己**变化时**
   主动改提示。一旦发生"图标被重建"的事件 —— 最典型的是 **explorer 重启**
   （收到 TaskbarCreated，系统把通知区图标全清掉、要程序自己重新加）——
   TrayAdd() 只会把提示写回静态的 APP_NAME，于是"（管理员）""正在映射"
   这些信息全部丢失，且**再没有任何状态变化去补它**：用户看到的就是
   "时间一长，悬停提示里管理员那行字没了"（实为 explorer 悄悄重启过）。
   所以图标（重）建之后一律走这个函数，把完整文案重建出来。
   优先级：暂停 > 记录中 > 自动屏蔽 > 滚轮不可用 > 正常映射。
   （暂停最优先：那是用户显式切的，不该被其它状态顶掉 —— 与 WM_AUTODISABLE
     的既有约定一致；记录中是临时的诊断态，也要能看出来。） */
static void TrayCurrentHint(void) {
    if (g_state.paused)
        TrayHint(L"已暂停（CapsLock 恢复原功能）");
    else if (g_recording.load())
        TrayHint(L"正在记录日志（C+\x60 结束）");
    else if (HookAutoDisabled())
        TrayHint(L"已自动屏蔽（前台程序在名单内）");
    else
        TrayHint(HookMouseAvailable() ? L"正在映射" : L"正在映射（滚轮功能不可用）");
}

/* explorer 重建任务栏时广播的消息号。
   ★ 必须在建窗**之前**注册：它要进 UIPI 白名单（见 WinMain 里的
   ChangeWindowMessageFilterEx），而白名单要在建窗后立刻设。 */
static UINT s_taskbarCreated = 0;

static BOOL TrayAdd(void) {
    memset(&g_nid, 0, sizeof(g_nid));
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = g_mainWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TOGGLE_TRAY;
    g_nid.hIcon = LoadAppIcon(FALSE);       /* 通用系统图标早已让人认不出本程序 */
    if (!g_nid.hIcon) {
        /* 图标句柄为 NULL 时 NIM_ADD **照样返回成功**，但通知区里是一块
           点不到、也看不出来的空白 —— 与"图标没加上"对使用者是同一个结果，
           却是我们的重试查不出来的那种（返回值是 TRUE）。所以这里补兜底：
           用系统图标保证至少可见，并记一条日志说明来源。 */
        g_nid.hIcon = LoadIconW(NULL, IDI_APPLICATION);
        CapLog("托盘：加载自带图标失败，已退回系统图标（通知区仍可用）");
    }
    wcscpy_s(g_nid.szTip, 128, APP_NAME);
    return Shell_NotifyIconW(NIM_ADD, &g_nid) ? TRUE : FALSE;
}
static void TrayRemove(void) {
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
}

/* ---------- 图标"没加上"时的定时重试 ----------
   ★★ 为什么必须有：**计划任务的管理员自启**会在用户登录的瞬间把本程序拉起来，
   那时 explorer 的任务栏（通知区）常常还没建好 —— Shell_NotifyIcon(NIM_ADD)
   直接失败。而"任务栏建好了"这个信号，系统只通过一条广播 **TaskbarCreated**
   发出来；本进程若是管理员（高完整性级别），explorer（中完整性）发来的广播会被
   **UIPI 拦掉**（低 -> 高 默认禁止），于是再也没人通知我们重建 —— 现象就是
   "开机后托盘图标时有时无，重启程序就好"。
   两件事都得做，缺一个都会漏：
     · 把 TaskbarCreated 放进 UIPI 白名单（覆盖"explorer 后来重启"）；
     · 即便没收到广播也自己定时重试（覆盖"我们起得太早"，那条广播**早于**
       我们存在，本来就收不到）。
   用 SetTimer 而不是 Sleep/轮询：主线程一旦离开"等消息"状态，低级钩子会被
   系统静默摘除（见 hook.c 顶部）—— 这条规矩在这里同样不能破。 */
#define TRAY_RETRY_TIMER  3
#define TRAY_RETRY_MS     1500
#define TRAY_RETRY_MAX    120     /* ≈3 分钟：够等 explorer 起来；再久就该看日志 */
static int  s_trayRetryN = 0;
static BOOL s_trayRetrying = FALSE;

static void TrayStartRetry(void) {
    if (s_trayRetrying) return;
    s_trayRetrying = TRUE;
    s_trayRetryN = 0;
    SetTimer(g_mainWnd, TRAY_RETRY_TIMER, TRAY_RETRY_MS, NULL);
}
static void TrayStopRetry(void) {
    if (!s_trayRetrying) return;
    s_trayRetrying = FALSE;
    KillTimer(g_mainWnd, TRAY_RETRY_TIMER);
}
static void TrayRetryTick(void) {
    s_trayRetryN++;
    if (TrayAdd()) {
        TrayCurrentHint();      /* 与 TaskbarCreated 那条同理：把完整提示重建出来 */
        TrayStopRetry();
        CapLog("托盘：图标在第 %d 次重试后添加成功（首次添加时任务栏未就绪）",
               s_trayRetryN);
        return;
    }
    if (s_trayRetryN >= TRAY_RETRY_MAX) {
        TrayStopRetry();
        CapLog("托盘：图标连续 %d 次添加失败（约 %d 秒），停止重试 —— 任务栏一直不可用",
               s_trayRetryN, TRAY_RETRY_MAX * TRAY_RETRY_MS / 1000);
    }
}
/* 图标（重）建：成功就顺带重建完整提示并停掉可能正在跑的重试 */
static void TrayRebuild(void) {
    if (TrayAdd()) {
        TrayCurrentHint();
        TrayStopRetry();
        /* 新图标 = 新身份：旧气泡连同它的点击目标一起作废（旧 explorer 的
           通知不该再被点到，那条气泡此时也已经从屏幕上消失了）。 */
        TrayBalloonClearClick();
    }
}
/* ★ 测试替换点（test/test_chord.c 场景AJ 要用一个"只记账、不弹窗"的托盘菜单桩）：
   从 ExeDir 一直到下面 TrayShowMenu 的结尾，整段被 `CAPS_NO_TRAY_MENU` 门控。
   这一段里每个函数的唯一调用者都在段内（ExeDir ← LaunchSettings ← TrayShowMenu），
   所以只要有一个不进测试 TU 就会被 /W4 报 C4505"已被删除的未引用静态函数" ——
   门控必须覆盖整条链，不能只围最外面那个。
   ★ 门控只围**定义**，不围 WM_TOGGLE_TRAY 里的调用点：调用点照旧写
   `TrayShowMenu`，单测自己提供一个同名 static 函数接上，生产构建里门控不生效。 */
#ifndef CAPS_NO_TRAY_MENU

/* exe 所在目录（结尾带 '\\'） */
static void ExeDir(WCHAR* out, size_t cap) {
    WCHAR exe[MAX_PATH] = L"";
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) { out[0] = 0; return; }
    WCHAR* slash = wcsrchr(exe, L'\\');
    if (slash) *(slash + 1) = 0;
    _snwprintf_s(out, cap, _TRUNCATE, L"%s", exe);
}

/* 拉起设置程序。找不到就明说（托盘气泡）：静默失败会让人以为"点了没反应"。
   工作目录设成 exe 目录：设置程序据此定位 CapsEnhance.ini，双击时的工作
   目录是不确定的（快捷方式/资源管理器都可能给不一样的）。
   失败时那条气泡带上设置程序的路径 —— 可点击，点了直接去那个目录看它到底在不在。 */
static void LaunchSettings(void) {
    WCHAR dir[MAX_PATH]; ExeDir(dir, MAX_PATH);
    WCHAR path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s%s", dir, SETTINGS_EXE);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        TrayBalloon(L"找不到设置程序：CapsEnhanceSettings.exe 应与 CapsEnhance.exe 在同一目录。"
                    L"点击打开本程序所在目录。", dir);
        return;
    }
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    /* CreateProcess 失败（策略/沙箱）再退 ShellExecute：后者走 shell 的启动
       路径，覆盖率更高。两者都不行才提示 —— 不要一上来就只靠 ShellExecute，
       它对"同目录 exe"也会受当前目录影响。 */
    if (CreateProcessW(path, path, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        /* 授权新进程"可以把自己拉到前台"：本程序自己是后台（托盘）进程，
           按系统规则子进程的 SetForegroundWindow 会被拒 —— 表现就是"点了设置，
           窗口弹在别的程序后面，找不着；再点一次（走单实例分支）才跑到前面来"。
           AllowSetForegroundWindow 是这条链路上的官方授权口。 */
        AllowSetForegroundWindow(pi.dwProcessId);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return;
    }
    if ((int)(INT_PTR)ShellExecuteW(NULL, L"open", path, NULL, dir, SW_SHOWNORMAL) > 32)
        return;
    TrayBalloon(L"设置程序启动失败。", NULL);
}

/* 托盘菜单（右键图标）。
   ★ 测试替换点（test/test_chord.c 场景AJ 要用一个"只记账、不弹窗"的桩）：
   整个定义被 `CAPS_NO_TRAY_MENU` 门控。单测 include 之前定义它，本函数就不进
   测试 TU，`WM_TOGGLE_TRAY` 里的调用由测试自己提供的同名函数满足。
   门控只围定义，不围调用点 —— 调用点照旧写 `TrayShowMenu`，单测提供的同名桩
   自然接上，生产构建里门控不生效，一切照旧。
   ★ 门控范围是**从 LaunchSettings 到本函数结束的整条菜单链**：SetMenuPauseText /
   LaunchSettings 的唯一调用者就是下面的 TrayShowMenu，单独门控菜单会留下 C4505
   "已被删除的未引用静态函数"。 */
static void TrayShowMenu(void) {
    if (!g_menu) return;                 /* 菜单创建失败时不要拿 NULL 去用 */
    POINT pt;
    GetCursorPos(&pt);
    SetMenuPauseText();
    /* 「取消全部置顶」只在真有窗口钉着时可点：菜单是常驻的一份，
       enable 状态每次弹出前重设（PinCount 读的是置顶线程维护的镜像）。 */
    EnableMenuItem(g_menu, IDM_UNPINALL,
                   MF_BYCOMMAND | (PinCount() > 0 ? MF_ENABLED : MF_GRAYED));
    SetForegroundWindow(g_mainWnd);
    int cmd = TrackPopupMenu(g_menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                             pt.x, pt.y, 0, g_mainWnd, NULL);
    if (cmd == IDM_PAUSE) {
        ApplyPauseState();
    } else if (cmd == IDM_UNPINALL) {
        PinUnpinAll();
    } else if (cmd == IDM_SETTINGS) {
        LaunchSettings();
    } else if (cmd == IDM_RESTART) {
        DoRestart();
    } else if (cmd == IDM_FOLDER) {
        /* 「程序文件夹」：复用日志定位与 C+F3 的同一条 shell 链路
           （FolderRevealPath：一次性线程 + SHOpenFolderAndSelectItems，失败
           自动绕回主线程弹气泡）。传**exe 文件**而不是目录 —— 它的语义是
           "打开父文件夹并选中这一项"，正合"打开文件所在的位置"；传目录反而
           会去开它的上级（见 folder.c 里 FolderRevealPidl 的说明）。 */
        WCHAR exe[MAX_PATH];
        if (GetModuleFileNameW(NULL, exe, MAX_PATH)) FolderRevealPath(exe);
    } else if (cmd == IDM_EXIT) {
        g_quit = TRUE;
        PostMessageW(g_mainWnd, WM_CLOSE, 0, 0);
    }
    TrimWorkingSet();   /* 菜单交互会拉起 TSF/DWM 等模块，关完菜单压回 */
}
#endif /* CAPS_NO_TRAY_MENU */

void TrayAddHint(const WCHAR* s) {
    wcscpy_s(g_nid.szTip, 128, s);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

/* 上一条**可点击**气泡要打开的路径（空 = 最近这条气泡不可点）。
   为什么要存下来、而不是点击时现算：气泡是**异步**的 —— 用户可能几秒后才点，
   那时 g_logPath 已经被下一次记录覆盖了（或者已经被清空）。
   也为什么是"最近这条"而不是"每条各自一份"：通知区对同一个图标同一时刻只显示
   一条气泡，能点到的永远是最后那条，多存没有意义。
   只在主线程读写（发送与点击都在 WndProc 里），不需要锁。 */
static WCHAR g_balloonPath[MAX_PATH] = { 0 };

/* explorer 重启（TaskbarCreated）会把通知区的图标全清掉，**当前那条气泡也随之
   消失**；更根本的是"新 explorer 不认识旧通知" —— 再让用户去点就是点了没反应。
   所以重建图标时一并作废：宁可不动作，也不要"点了有反应但点错东西"。 */
static void TrayBalloonClearClick(void) {
    g_balloonPath[0] = 0;
}

/* 弹一次气泡通知。用于"钩子安装失败""诊断已写入日志"这类必须被看到、
   但不该打断操作的结果 —— 之前是用模态 MessageBox 弹在主线程上，
   会把和弦引擎和键盘响应一起冻住。
   `clickPath` 非空 = 这条气泡可点击（点了打开并选中它，见 TrayBalloonClicked）。 */
void TrayBalloon(const WCHAR* s, const WCHAR* clickPath) {
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
    /* 气泡**不自动消失**：本程序的气泡都是"结果通知" —— 钩子装上没有、日志写到
       哪份文件了。看到 = 确认，没看到就该一直挂着。默认那 10 秒对"记录结束"
       这条尤其不够用：用户往往过一会儿才回来处理日志，那时气泡早就没了，
       "点击打开所在位置"也无从点起。
       0 = 永不超时（**不是** `uTimeout`：那个字段只在 Win2000 下叫这名，此后
       这个联合体成员改叫 `uTimeoutOrVersion` 且由系统填版本号，写它没有意义）。 */
    n.uTimeout = 0;
    wcscpy_s(n.szInfoTitle, 64, APP_NAME);
    n.szInfo[0] = L'\0';                       /* 第一步：收掉当前气泡（取消排队） */
    Shell_NotifyIconW(NIM_MODIFY, &n);
    wcscpy_s(n.szInfo, 256, s);                /* 第二步：新内容 → 立即弹出 */
    Shell_NotifyIconW(NIM_MODIFY, &n);

    /* 先记目标、再复位（两条分支都要走到，见上面那张状态表） */
    TrayBalloonClearClick();
    if (clickPath && clickPath[0])
        wcscpy_s(g_balloonPath, MAX_PATH, clickPath);
}

/* 气泡被点击（WndProc 从 uCallbackMessage 收到 NIN_BALLOONUSERCLICK）。
   为什么点"气泡"与点"图标"必须分开：图标那一套是打开托盘菜单，
   气泡被点（尤其在 Win10/11 转成 toast 之后）离图标很远，把两件事接在一起会
   让用户"明明点的是通知，怎么弹出了菜单"。 */
static void TrayBalloonClicked(void) {
    if (!g_balloonPath[0]) return;      /* 最近这条不可点 / 已经被作废 */
    WCHAR p[MAX_PATH];
    wcscpy_s(p, MAX_PATH, g_balloonPath);
    TrayBalloonClearClick();            /* 一次点击只做一次（同一条气泡重复点不重开） */
    {   /* 路径进日志：排查"点了没反应"时能分清是没收到点击还是打开失败 */
        char p8[MAX_PATH];
        p8[0] = 0;
        WideCharToMultiByte(CP_ACP, 0, p, -1, p8, (int)sizeof(p8), NULL, NULL);
        CapLog("托盘：通知被点击，打开 %s", p8);
    }
    FolderRevealPath(p);
}
/* 定义在 TrayShowMenu 之后，所以不在上面那一段门控里 —— 它的唯一调用者仍是
   TrayShowMenu，菜单被替换掉时它在本 TU 里同样没有引用者，必须一起门控。 */
#ifndef CAPS_NO_TRAY_MENU
static void SetMenuPauseText(void) {
    MENUITEMINFOW m;
    memset(&m, 0, sizeof(m));
    m.cbSize = sizeof(m);
    m.fMask = MIIM_STRING;
    m.dwTypeData = (LPWSTR)(g_state.paused ? L"恢复映射" : L"暂停映射");
    SetMenuItemInfoW(g_menu, IDM_PAUSE, FALSE, &m);
}
#endif /* CAPS_NO_TRAY_MENU（整条托盘菜单链在此收口） */

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
    /* 帮助窗口跟着和弦一起收：松开 C、暂停、重启都会走到这里，一处覆盖全 ——
       否则"按着 C+F1 直接松 C"会让窗口一直留着（子进程那边虽有 3 分钟上限，
       但那是异常兜底；正常路径必须在这里就收）。 */
    HelpHide();
    NavSelTrackReset();
    /* 丢掉还没开始的"重复开火"尾流，但保留"松手补齐" —— 后者是用户本次
       点按的一部分，不能因为紧接着松开 C 就被清掉。 */
    ActClearKeepFill();
    /* 滚轮加速的待补队列：**只丢陈旧项**，刚滚的那几格留着补完。
       整队清空（原来的写法）会把每次甩轮的尾部一起吞掉 —— 那正是
       "有时候不加速、松开重按才行"的成因；而松手后 120ms 之前的请求
       视觉上已经过期，丢掉才不会变成尾流。 */
    ActWheelDropOlder(ACT_WHEEL_STALE_MS);
    /* **持续滚动刻意不在这里停**（2026-09-11 改版）：设计意图是"滚一下就能
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
    g_btnCount = 0;
    /* 必须先停执行线程并等它收尾：否则它可能正在注入，而 SendClearStuckModifiers
       要读 send.c 的注入记账（清卡键会读错）。 */
    ActStop();
    /* 清卡键必须排在 ModHoldReset **之前**：ModHoldReset 里的 PhysSeedFromLogical
       会重播种 g_phys，排在后面判据就被播种改写了。此刻钩子还装着，g_phys
       是钩子亲眼维护的物理状态 —— 正是判据最准的时刻。
       protectSeededHolds=FALSE（激进）：重启是用户显式的"重置键盘"动作，
       只保护"钩子亲眼见到的按住"，**播种态的卡键无论谁造成的都一并清掉**
       —— 包括别的程序崩溃留下的（用户用 KeyCleaner 类工具修的那种）。
       顺序约束的完整说明见 send.c。 */
    SendClearStuckModifiers(FALSE);
    ModHoldReset();

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
        /* 拉起新实例失败 -> 必须把执行线程**放回去**：上面已经 ActStop()
           （s_dead = 1），直接返回就卡在"钩子还装着、投递全丢"的半死状态 ——
           钩子照旧吞键、动作一个也不执行，用户看到的是"键盘坏了"。
           ActStart 再失败就退回同步兜底（更慢，但不至于功能全无）。 */
        BOOL back = ActStart();
        TrayBalloon(back ? L"重启失败（映射已恢复到重启前）"
                         : L"重启失败，且执行线程起不来：请退出后重新运行", NULL);
        TrayCurrentHint();
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

/* ---- 会话事件（锁屏 / 切换用户 / 注销 / 断开）----
   WM_WTSSESSION_CHANGE 与下面几个事件码都来自 wtsapi32.h。自带一份常量是为了
   把它做成**可选**能力（见 RegisterSessionNotify）。 */
#define WM_WTSSESSION_CHANGE    0x02B1
#define WTS_CONSOLE_DISCONNECT  0x2
#define WTS_REMOTE_DISCONNECT   0x4
#define WTS_SESSION_LOGOFF      0x6
#define WTS_SESSION_LOCK        0x7

/* 注册会话通知；拿不到就返回 FALSE，退化成"靠下一次 Caps down 自愈"（旧行为）。
   为什么动态取函数、不加 wtsapi32.lib：这条通知只对**钩子宿主**有意义，而导入表
   里每一项都是"启动时要加载的 DLL"。取不到也不影响任何功能。
   注册成功后不 FreeLibrary：通知有效期内 DLL 必须留着（进程退出时系统自己收）。 */
static BOOL RegisterSessionNotify(HWND hwnd) {
    static HMODULE s_wts = NULL;
    if (!s_wts) s_wts = LoadLibraryW(L"wtsapi32.dll");
    if (!s_wts) return FALSE;
    typedef BOOL (WINAPI *PFN_WTSREG)(HWND, DWORD);
    PFN_WTSREG reg = (PFN_WTSREG)(void*)GetProcAddress(s_wts, "WTSRegisterSessionNotification");
    if (!reg) return FALSE;
    return reg(hwnd, 0);            /* NOTIFY_FOR_THIS_SESSION = 0：只关心本会话 */
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    /* explorer 重启后系统清空了通知区（图标连同悬停提示一起没了），必须重新
       ADD，**并把完整提示一起重建**（只 ADD 的话 szTip 是裸 APP_NAME，
       "（管理员）""正在映射"这些全丢，而且再没有状态变化去补它 —— 用户看到的
       就是"时间一长，悬停提示里管理员那行字不见了"，实为 explorer 悄悄重启过）。
       此时钩子早已装好，TrayCurrentHint 能读到真实状态，重建出的文案与当时一致。
       ★ 这条广播要能被收到，前提是 WinMain 里把它放进了 UIPI 白名单 ——
       管理员运行时 explorer 的广播本来是会被 UIPI 拦掉的（见 TrayStartRetry）。 */
    if (msg == s_taskbarCreated && s_taskbarCreated) { TrayRebuild(); return 0; }
    if (ClipIsBornMsg(msg))   { ClipBorn((DWORD)wp, (DWORD)lp); return 0; }  /* 浮层窗口已出生，握手完成 */
    if (ClipIsClosedMsg(msg)) { ClipClosed(); return 0; }   /* 浮层自行关闭（如粘贴后收起） */
    if (ClipViewIsStateMsg(msg)) { ClipViewState((int)wp); return 0; } /* 预览窗开/关报信 */
    if (msg == WM_CLIP_GONE) { ClipGone(); return 0; }      /* 浮层进程没了（内核告知，见 src/child.c） */

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
    case WM_RESTART_REQUEST:
        /* 外部（--restart 命令行 / 设置程序保存后）请求重启。
           走的是托盘"重启程序"同一条路：它会先停执行线程、清卡键，
           再拉起带 --wait-exit 的新实例 —— 配置因此被完整重读。 */
        DoRestart();
        return 0;
    case WM_AUTODISABLE:
        /* 钩子判定"前台程序命中 [AutoDisable]"的状态变了。
           只在**未暂停**时改提示：暂停是用户显式切的，不该被自动屏蔽的
           状态变化悄悄顶掉（否则托盘会显示"正在映射"而实际是暂停中）。 */
        if (!g_state.paused)
            TrayHint(wp ? L"已自动屏蔽（前台程序在名单内）" : L"正在映射");
        return 0;
    case WM_PROBE:
        /* C+`：切换诊断记录。开始=探测当前焦点并记录；再按=结束记录。
           起止各响一声不同的系统事件音（见 RecBeep），并同步托盘悬停提示内容。
           探测结果只写日志（原来会弹一个模态对话框，既冻住引擎又难以阅读/复制）。 */
        g_recording.store(!g_recording.load());
        if (g_recording.load()) {
            /* 先开新文件，再写第一行：本次记录的日志从空文件开始 */
            LogNewSession();
            CapLog("===== REC START =====");
            /* 配置值入日志：排查"配置没生效"类问题时一目了然 */
            CapLog("配置: MaxArrowSteps=%d OverStepCtrl=%d LogMaxKB=%d RunAsAdmin=%d",
                   CfgInt(CFG_MAX_ARROW_STEPS), CfgBool(CFG_OVER_STEP_CTRL) ? 1 : 0,
                   CfgInt(CFG_LOG_MAX_KB), CfgBool(CFG_RUN_AS_ADMIN) ? 1 : 0);
            {   /* 实际生效的修饰激活键（编译期常量，见 cap.h）：排查按键映射时一眼可见 */
                WCHAR ek[MODE_KEYS_MAXLEN], sk[MODE_KEYS_MAXLEN];
                char  eb[MODE_KEYS_MAXLEN * 3], sb[MODE_KEYS_MAXLEN * 3];
                ModeKeysText(MODE_ENHANCE, ek, MODE_KEYS_MAXLEN);
                ModeKeysText(MODE_SHIFT,   sk, MODE_KEYS_MAXLEN);
                WideCharToMultiByte(CP_UTF8, 0, ek, -1, eb, (int)sizeof(eb), NULL, NULL);
                WideCharToMultiByte(CP_UTF8, 0, sk, -1, sb, (int)sizeof(sb), NULL, NULL);
                CapLog("按键: 增强=%s 选区=%s（固定值，不读配置） WheelScrollMul=%d "
                       "AutoScrollInterval=%d AutoScrollFastMs=%d",
                       eb, sb, CfgInt(CFG_WHEEL_SCROLL_MUL), CfgInt(CFG_AUTOSCROLL_INTERVAL),
                       CfgInt(CFG_AUTOSCROLL_FAST_MS));
            }
            {   /* 持续滚动的两端方向：AutoScrollFastMs 是"最快"，毫秒数必须
                   **小于** AutoScrollInterval（起步）才有提速空间。写反了不会
                   崩（act.c 里 `if (fast > slow) fast = slow`），但提速阶梯会
                   **静默退化成恒定速度** —— 现象只是"再滚一下没变快"，极难
                   自查，所以在诊断日志里明说。 */
                int slow = CfgInt(CFG_AUTOSCROLL_INTERVAL);
                int fast = CfgInt(CFG_AUTOSCROLL_FAST_MS);
                CapLog("自动滚动: 起步=%dms/格 最快=%dms/格 %s", slow, fast,
                       (fast >= slow) ? "（!! 最快 >= 起步：提速已退化为恒速）" : "");
            }
            RecBeep(TRUE);                  /* 开始记录：短促的"注意"音 */
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
                        L"（前 160 字符）与按词跳转位置，敏感内容上请勿长时间开启。", NULL);
        } else {
            CapLog("===== REC END =====");
            RecBeep(FALSE);                 /* 结束记录：另一声，与"出错/UAC"无关 */
            /* 悬停提示由"暂停 / 记录中 / 自动屏蔽 / 滚轮可用"共同决定，统一入口是
               TrayCurrentHint（见那里的优先级说明）。这里直接写"正在映射"，
               在"暂停中结束记录"时会把提示改错 —— 而托盘提示是用户判断当前
               状态的唯一依据。 */
            TrayCurrentHint();
            /* 结束也给一次明确反馈：开始有气泡，结束若只有一声提示音，很容易
               让人以为"还在记录"，于是继续在敏感内容上操作。 */
            {   /* 结束气泡里带上实际文件名：一次记录一个文件，不给名字等于
                   让人去 log\ 里猜哪份是刚录的。
                   顺带把它做成**可点击**的：点了就在资源管理器里打开并选中这份
                   日志（复用 C+F3 那条 shell 链路）。记录一旦结束，日志文件所在
                   的目录就未必还找得到 —— 这是"刚录完那份"唯一的一键入口。
                   注意传的是 g_logPath 而不是拼出来的显示文本：气泡里那行是
                   "log\<文件名>"，是给人看的，不是能去解析的路径。 */
                WCHAR tip[160];
                const WCHAR* slash = wcsrchr(g_logPath, L'\\');
                _snwprintf_s(tip, 160, _TRUNCATE, L"日志记录已结束（写入 log\\%s）"
                             L"—— 点击打开所在位置",
                             slash ? (slash + 1) : g_logPath);
                TrayBalloon(tip, g_logPath);
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
            BOOL  single = IsSingleShotAct(act);
            BOOL  oldEnh = g_actEnh, oldShf = g_actShf;
            int   oldCnt = g_btnCount;
            g_actEnh = (lp & TRIGF_ENH) != 0;
            g_actShf = (lp & TRIGF_SHF) != 0;
            g_btnCount = 1;
            DoFireOnce(act, TRUE);
            /* 帮助窗口（C+F1）：**不走执行线程**，因为它不阻塞也不发键 ——
               它只是"拉起/唤起另一个进程的窗口"，在主线程上做才是对的
               （要按和弦的寿命开合，见 help.c）。 */
            if (ActHasFlag(act, ACTF_HELP)) HelpShow();
            /* 剪贴板（Caps+V）：同样不走执行线程 —— 它只是"拉起/唤起另一个
               进程的窗口"。toggle 语义在 ClipShow 里（再按一次 = 关掉）。 */
            if (ActHasFlag(act, ACTF_CLIPLIST)) ClipShow();
            if (!single) {
                /* 开关键只切一次、也不接管重复节奏（原来那个动作继续重复） */
                g_activeAct = act;
                g_activeNext = GetTickCount64() + REPEAT_FIRST_DELAY;
            } else {
                /* 单发键不接管节奏，所以**它自己的快照也必须只用于它自己这一发**：
                   不放回去的话，仍在重复的那个动作从这里开始每一发（以及它的松手
                   补齐）都改用这一发的 enh/shf 与 count=1 —— 长按 C+F+J 扩展选区的
                   中途按一下 V，选区当场就不再扩展了。 */
                g_actEnh = oldEnh; g_actShf = oldShf; g_btnCount = oldCnt;
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
        /* 帮助窗口的寿命就是这个和弦：F1 一抬就隐（松开 Caps 那条走 ResetEngine，
           两条路都收，先到的那条生效）。放在最前面：单发动作根本不进下面的
           "停重复 + 补齐"分支（g_activeAct 从来不会指向它）。 */
        if (ActHasFlag(act, ACTF_HELP)) HelpHide();
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
    case WM_FOLDER_OPEN: {           /* 打开所在文件夹失败（folder.c 的一次性线程发来） */
        /* 为什么绕回来：气泡要动主线程的托盘状态（g_nid），工作线程不能直接弹。
           成功不发这条消息 —— 成功时文件管理器自己弹出来了，那本身就是反馈。 */
        switch ((int)wp) {
        case FOLDER_ERR_SHELLWIN:
            TrayBalloon(L"打不开所在文件夹：前台是桌面或任务栏，它没有「所在文件夹」。", NULL);
            break;
        case FOLDER_ERR_SHELL:
            TrayBalloon(L"打不开所在文件夹：系统拒绝了这次打开请求"
                        L"（路径解析失败或被拒）。", NULL);
            break;
        default:   /* FOLDER_ERR_NOPATH 及任何未知码：都按"拿不到路径"说 */
            TrayBalloon(L"打不开所在文件夹：拿不到该程序的路径（前台没有焦点窗口、"
                        L"受保护进程、已退出，或 UWP 应用的安装目录解析不到）。", NULL);
            break;
        }
        return 0;
    }
    case WM_PIN_ELEVATED:            /* 置顶失败：目标窗口权限比本程序高（pin.c 发来） */
        /* 与 WM_FOLDER_OPEN 同一条理由绕回主线程：气泡动的是托盘的 g_nid。
           这件事必须让人看见 —— 否则就是"按了 Caps+T 什么都没发生"，
           而真正的原因在另一个进程的完整性级别上，用户无从推断。 */
        TrayBalloon(L"无法置顶：目标窗口以管理员身份运行。"
                    L"需要置顶这类窗口，请在设置里打开「以管理员运行」（RunAsAdmin）。", NULL);
        return 0;
    case WM_CLIP_ESC:                /* 钩子代收的 Esc（浮层自己收不到键盘，见 clip.c） */
        ClipEsc();
        return 0;
    case WM_CLIP_KEY:                /* 钩子代收的导航键（回车 / ↑↓ / Alt+数字 …） */
        ClipKey((int)wp, (unsigned)lp);
        return 0;
    case WM_CLIP_DITTO:              /* 浮层开着时单击 Caps：关浮层、拉起 Ditto 本体 */
        ClipCallDitto();
        return 0;
    case WM_CLIP_VIEW:               /* 裸 F3（toggle 预览）/ 无关键（预览退场），见 src/clip.c */
        if (wp) ClipViewToggle();
        else    ClipViewHide();
        return 0;
    case WM_TIMER: {
        if (wp == TRAY_RETRY_TIMER) { TrayRetryTick(); return 0; }
        if (wp == HELP_TIMER) { HelpTick(); return 0; }
        if (wp == CLIP_TIMER) { ClipTick(); return 0; }
        if (wp == CLIP_PREWARM_TIMER) {
            KillTimer(g_mainWnd, CLIP_PREWARM_TIMER);   /* 一次性：预热完就拆 */
            ClipPrewarm();
            return 0;
        }
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
           与工作线程每拍自查 g_cMod/ModeDown 一起构成"松手即停"，现均已移除。） */
        (void)wp;
        return 0;
    /* ---- 托盘 ---- */
    case WM_TOGGLE_TRAY: {
        /* LOWORD 取低 16 位：通知码都是 0x04xx 量级（NIN_BALLOONUSERCLICK =
           WM_USER + 5），高位留白 —— 明确写 LOWORD 之后，"传进来的就是个干净
           通知码"这条契约落在代码里，而不是靠调用方的自觉。
           （图标自身的 WM_LBUTTONUP/WM_RBUTTONUP 比 0x0405 小，LOWORD 之后不变。） */
        UINT evt = LOWORD(lp);

        /* 气泡被点击：只做它的那件事，不再顺手弹托盘菜单 —— 图标（= 菜单）与气泡
           （= 通知）是屏幕上隔着一段距离的两个东西，把两者接在一起会让用户
           "点个通知，怎么跳出一个菜单"。 */
        if (evt == NIN_BALLOONUSERCLICK) {
            TrayBalloonClicked();
            return 0;
        }
        if (lp == WM_RBUTTONUP || lp == WM_LBUTTONUP)
            TrayShowMenu();
        return 0;
    }
    case WM_WTSSESSION_CHANGE:
        /* 锁屏 / 切换用户 / 注销 / 断开会话之后，按键进的是**安全桌面**或另一个
           会话，我们的低级钩子收不到 —— 期间松开的 Caps（或一直按着的映射键）
           其 keyup 就永久丢了，g_cMod 卡在"按住"上：回到桌面后第一件事是按
           j/k/l 打字，而它们全被当方向键吞掉，用户看到的是"键盘坏了"。
           （Caps 的 down 分支也能自愈，但那要等用户再按一次 Caps —— 而他要先
           意识到键盘"坏了"。）所以拿到通知就按"状态未知"整份复位。
           RegisterSessionNotify 取不到通知时退回旧行为，功能不受影响。 */
        if (wp == WTS_SESSION_LOCK || wp == WTS_SESSION_LOGOFF ||
            wp == WTS_CONSOLE_DISCONNECT || wp == WTS_REMOTE_DISCONNECT) {
            if (g_recording.load())
                CapLog("会话事件 %u：按住状态已复位（期间 keyup 已丢失）", (unsigned)wp);
            ModHoldReset();        /* 钩子侧：按住状态 + 吞键配对表 */
            g_btnCount = 0;
            ResetEngine();
            /* 与暂停同一条理由：钩子收不到"按任意键打断"了，持续滚动必须显式停 */
            if (g_recording.load() && ActAutoScrollActive())
                CapLog("持续滚动：终止（会话锁定/断开）");
            ActAutoScrollStop();
            TrayCurrentHint();
        }
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

    /* ★ 第一件事：声明 DPI 感知（Per-Monitor V2）。
       必须在**创建任何窗口之前** —— 托盘消息窗口就在下面几步里建。
       不声明的后果见 cap.h 的 `WinInitDpiAwareness`：低级鼠标钩子给物理坐标，
       而本进程的窗口 API 会用虚拟坐标，两者差一个缩放比 → Caps+左键拖动会
       指错窗口、位移也会比光标多跑一截。AltSnap 同样在清单里声明了这一条。 */
    WinInitDpiAwareness();

    /* --bridge <父pid> <序号>：降权桥的子进程模式（见 src/bridge.c）。
       它是本 exe 的"普通权限副本"，只在别的进程里代跑那些"只有普通权限才看得见
       的对象"，不装钩子、不建窗口、不建托盘 —— 必须抢在单实例检查之前返回。
       命令行开关的解析与拼接都归 bridge.c（它自己定义管道名，主程序不必知道）。 */
    {
        DWORD bpid = 0, bseq = 0;
        if (BrParseChildSwitch(&bpid, &bseq))
            return WpsBridgeServe((unsigned long)bpid, (unsigned long)bseq);
    }

    /* --restart：请求**已经在跑的那个实例**重启（设置程序保存配置后用它）。
       为什么不直接改配置热加载：配置影响键位表、钩子的滚轮语义、日志上限等
       多处状态，"全部重读一遍"就是重启，与其做一半不如把这条路径走实。
       没找到运行中的实例 -> **照常启动**（不 return）：设置程序在程序没跑时
       保存配置，用户按"保存"期待的也是"配置生效"。 */
    if (HasSwitch(L"--restart", NULL)) {
        HWND prev = FindWindowW(L"CapsEnhanceMain", NULL);
        if (prev) {
            PostMessageW(prev, WM_RESTART_REQUEST, 0, 0);
            return 0;
        }
    }

    WaitOldInstanceExit();   /* 重启场景：等旧实例完全退出再初始化 */
    if (!EnsureSingleInstance()) return 0;   /* 已有实例在跑：提示后退出 */

    /* WNDCLASSEX（不是 WNDCLASS）：小图标 hIconSm 只在 EX 版里有 */
    /* ★ TaskbarCreated 必须在建窗前注册：它要进下面的 UIPI 白名单。 */
    s_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadAppIcon(FALSE);        /* 任务切换器 / 资源管理器用大图标 */
    wc.hIconSm = LoadAppIcon(TRUE);       /* 标题栏与托盘用小图标 */
    wc.lpszClassName = L"CapsEnhanceMain";
    if (!RegisterClassExW(&wc)) return 1;

    g_mainWnd = CreateWindowW(L"CapsEnhanceMain", APP_NAME,
                              WS_POPUP | WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1, 1,
                              NULL, NULL, hInst, NULL);
    if (!g_mainWnd) return 1;

    /* UIPI 放行：允许**完整性级别更低**的进程向本窗口发这几条消息。
       Windows 默认拦掉"低 -> 高"的 PostMessage（UIPI），而管理员运行是本程序
       的 RunAsAdmin（开启时），设置程序往往是普通权限 —— 不放行的话
       "保存并重启"在管理员模式下会**静默失效**（消息被丢弃，无任何报错）。
       同理覆盖 WM_ALREADY_RUNNING（重复启动时的提示也来自另一个实例）。 */
#if WINVER >= 0x0600
    ChangeWindowMessageFilterEx(g_mainWnd, WM_RESTART_REQUEST, MSGFLT_ALLOW, NULL);
    ChangeWindowMessageFilterEx(g_mainWnd, WM_ALREADY_RUNNING, MSGFLT_ALLOW, NULL);
    /* ★★ TaskbarCreated 也必须放行，而且它是**最要紧**的一条：
       本程序以管理员运行（RunAsAdmin / 计划任务 /rl HIGHEST）时是高完整性级别，
       而 explorer 是中完整性 —— 它那条"任务栏重建好了"的广播是**低 -> 高**，
       默认被 UIPI 丢弃。放不掉它，启动那一刻若赶在任务栏建好之前
       （计划任务自启的典型情形），图标就永远补不回来（详见 TrayStartRetry）。
       注册消息（>= 0xC000）同样可以进白名单，与普通消息没有区别。 */
    if (s_taskbarCreated)
        ChangeWindowMessageFilterEx(g_mainWnd, s_taskbarCreated, MSGFLT_ALLOW, NULL);
#endif

    /* 本窗口只承载托盘菜单，永不接收文字输入。禁掉 IME/TSF：
       SetForegroundWindow 让它获得键盘焦点时，系统会为它初始化输入法
       上下文并加载 textinputframework/CoreUIComponents 等（实测 +4MB
       上下）。禁用后这些模块不再加载。 */
    ImmAssociateContext(g_mainWnd, NULL);

    /* 会话通知：锁屏 / 切用户回来时把"按住状态"复位（见 WndProc 的
       WM_WTSSESSION_CHANGE）。失败不影响功能，只是少一条自愈路径。 */
    if (!RegisterSessionNotify(g_mainWnd))
        CapLog("会话通知注册失败：锁屏后若残留按住状态，按一下 Caps 即自愈");

    /* 先建托盘：后面任何一步失败才有地方提示用户。此前钩子安装失败时
       调 TrayHint，但托盘图标还没创建，提示全部落空 —— 用户看不到原因。
       ★ 加了要**看返回值**：自启时任务栏常常还没就绪，NIM_ADD 会直接失败，
       失败不重试就再也没人补这个图标（见 TrayStartRetry 的说明）。 */
    if (!TrayAdd()) {
        CapLog("托盘：首次添加图标失败（任务栏尚未就绪），开始定时重试");
        TrayStartRetry();
    }

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

    /* 置顶线程（C+T 用，见 pin.c）：必须在执行线程之前起。
       这里建好之后 s_tool 就不再变化，动作线程直接 PostMessage 过去即可 ——
       顺序放在 ActStart 之前，是为了让"主线程建窗"与"工作线程读句柄"之间
       有一条真实的 happens-before（CreateThread 本身就是那道栅栏）。
       起不来不算致命：Caps+T 会静默无反应，其余功能照旧。 */
    PinInit();

    /* 执行线程要在装钩子之前起来：装了钩子就随时可能有动作要发。 */
    if (!ActStart()) {
        /* 退回同步兜底（见 act.c 的 ActPost）：动作改在**承载钩子的线程**上执行，
           目标程序卡住时能把输入冻住近一秒（跨进程 COM 的限时预算见 nav.c）。
           功能还在、代价明显，所以除了日志还要让用户看见并自己去重启。 */
        CapLog("ActStart 失败：动作退回主线程同步执行（输入响应性下降）");
        TrayBalloon(L"执行线程创建失败：动作改在主线程执行，遇到卡住的程序会明显卡顿，"
                    L"建议重启程序。", NULL);
    }

    /* 物理状态种子也要在装钩子之前：装完之后才"补种"，会漏掉装钩瞬间
       那一小段真实按键。 */
    PhysSeedFromLogical();   /* 启动前就按着的键按物理按住处理（防误清） */

    /* 启动时清一次"上次留下的卡键"：程序被强杀（或上电异常）时，它注入过的
       键可能还停在"按下"状态，而注入记账随进程一起消失了 —— 受害的是**别的**
       程序，表现为"Alt/Ctrl 一直按着"或"方向键自己跑"。
       两类残留的清法不同（见 send.c）：注入过的键靠记账补抬（记账没了），
       所以这里靠**全 VK 扫描**兜底 —— 判据"系统认为按着 且 用户物理上没按"，
       与卡键是谁造成的无关（别的程序崩溃留下的同样清）。
       protectSeededHolds=TRUE（保守）：刚播种的键（启动前就按着的）分不清
       "用户正按着"还是"早已卡住"，一律当按住保护 —— 代价是上次崩溃残留的
       修饰键在启动这一遍漏清，**再重启一次即可**（重启路径走激进清理）。
       放在装钩子之前：此时我们注入的抬起不会被自己的钩子当成输入处理。 */
    SendClearStuckModifiers(TRUE);

    if (!HookInstall()) {
        TrayHint(L"钩子安装失败（可能被安全软件拦截）");
        TrayBalloon(L"CapsEnhance 启动失败：键盘钩子安装失败，可能被安全软件拦截。", NULL);
        rc = 1;
        g_quit = TRUE;
        DestroyWindow(g_mainWnd);      /* 走统一收尾，不留托盘残影 */
    }

    if (!g_quit) {
        /* 启动不写日志文件；采集系统/环境基线供 C+` 诊断时输出。启动不发声。 */
        NavBaseInfo();
        /* 追加坐标空间自检（**这一行是拖动/挪窗口错位类问题的第一现场**）：
           低级鼠标钩子的 MSLLHOOKSTRUCT.pt 永远是物理像素，所以本进程也必须活在
           物理空间 —— 只有 WinInitDpiAwareness() 生效时两边才一致。判定与实现
           都在 winmove.c（本文件只是把它报出来），详见 cap.h 的说明。 */
        {
            WCHAR dpiLine[256];
            _snwprintf_s(dpiLine, sizeof(dpiLine) / sizeof(dpiLine[0]), _TRUNCATE,
                         L"DPI 感知 = %ls  坐标空间 = %ls\n",
                         WinDpiAwarenessName(),
                         WinCoordSpaceConsistent()
                             ? L"物理(与钩子一致)"
                             : L"**虚拟化(与钩子不一致，拖动会错位!)**");
            NavBaseAppendLine(dpiLine);
        }

        /* 配置要求以管理员运行且当前不是管理员：申请提权重启一次。
           提权实例带 --wait-exit —— 必须等本实例释放单实例互斥量之后再装
           钩子，否则两边抢同一把锁，提权实例会误判"已在运行"而直接退出，
           结果是提权静默失败。用户取消 UAC 则按普通权限继续运行。 */
        if (CfgBool(CFG_RUN_AS_ADMIN) && !IsRunElevated()) {
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
            AppendMenuW(g_menu, MF_STRING, IDM_FOLDER, L"程序文件夹");
            AppendMenuW(g_menu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(g_menu, MF_STRING, IDM_PAUSE, L"暂停映射");
            AppendMenuW(g_menu, MF_STRING, IDM_UNPINALL, L"取消全部置顶");
            AppendMenuW(g_menu, MF_STRING, IDM_SETTINGS, L"设置...");
            AppendMenuW(g_menu, MF_STRING, IDM_RESTART, L"重启程序");
            AppendMenuW(g_menu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(g_menu, MF_STRING, IDM_EXIT, L"退出");
        }
        /* 钩子都装完了，按当前全部状态刷新一次提示（滚轮钩子装不上要让人看见，
           别静默降级；[AutoDisable] 与记录态也一并纳入）。 */
        TrayCurrentHint();

        /* 浮层预热：启动 3 秒后把剪贴板浮层进程先拉起来（隐藏驻留，见
           src/clip.c 的 ClipPrewarm）—— 第一次 Caps+V 从此不用付冷启动。
           用定时器错开而不是现在就拉：启动这几秒里钩子、托盘、杀软都在忙，
           别挤在同一瞬间。 */
        SetTimer(g_mainWnd, CLIP_PREWARM_TIMER, 3000, NULL);
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
    PinShutdown();               /* 解除全部置顶（窗口回到原来的层级）后停置顶线程 */
    WpsBridgeShutdown();         /* 让 WPS 降权桥立刻退出（否则要等它的空闲超时） */
    HookUninstall();
    SendClearStuckModifiers(FALSE);   /* 退出前清卡键：注入记账补抬 + 全 VK 扫描（激进，见 send.c） */
    TrayRemove();
    if (g_menu) { DestroyMenu(g_menu); g_menu = NULL; }
    return rc;
}