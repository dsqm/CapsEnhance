/* ============================================================
   和弦引擎回归测试：直接调用底层钩子进程，注入按键序列，
   用另一个捕获钩子统计软件真实注入的方向键/Shift。
   覆盖：模式修饰（增强=Alt/D、位移=F）在"按键乱序到达"下仍正确、
   长按重复/松手补齐、原生按词跳转按键序列，
   以及"Caps+滚轮 = 快速滚动（倍数）"、"Caps+Alt/D+滚轮 = 持续滚动"
   （都用注入计数钩子观测），"Caps+Shift+滚轮 = 横向滚动"
   （真实 Shift 决定轴向，且钩子吞掉物理格）。
   注意：引擎只问"某个模式是否生效"，从不判断按的是哪个键 ——
   键位表在 keymap.c，激活键是编译期常量（场景S验证"配置里写了也忽略"）。
   ============================================================ */
#define WinMain __CapsEnhanceRealWinMain_Unused
#include "cap.h"
#include "keymap_table.h"   /* 与帮助程序共用同一份键位表：这里顺带守着"文案不缺" */
/* ---- 托盘菜单桩（生产版是 main.c 里那个 static TrayShowMenu）----
   真实现要 CreatePopupMenu + TrackPopupMenu，会**阻塞地弹出一个真菜单**等用户点：
   本测试既不该弹窗，也不该被它挂住消息循环，所以桩成"记账 + 立刻返回"。
   场景AJ 靠它钉住"点气泡不许弹菜单、点图标必须弹菜单"这两条相反的接线。

   做法：include main.c 之前 `#define CAPS_NO_TRAY_MENU`，main.c 里那个定义被门控
   整段跳过，于是 `WM_TOGGLE_TRAY` 里的 `TrayShowMenu()` 调用**指的就是下面这个同名
   static 函数**（同一个 TU，先声明先见，定义在 include 之后）。
   不去 macro 改名的原因：所有源文件都按 /TP 当 C++ 编，而 C++ 里 static 函数
   同名重定义是 C2084"已有主体"，改名/覆盖那条路走不通。门控只围定义，不动调用点，
   也就没有"调用点被宏改成一个名字、桩又得叫那个名字"的连锁。 */
#define CAPS_NO_TRAY_MENU
static void TrayShowMenu(void);   /* 先声明：main.c 里的调用点见到的就是它 */
static BOOL g_menuOpen = FALSE;

#include "main.c"
#undef WinMain
static void TrayShowMenu(void) { g_menuOpen = !g_menuOpen; }
/* ---- 输入法切换桩（src/ime.c 不进本测试）----
   真实现会向**别的进程**的 IME 窗口发 3 条消息 —— 那既不是本测试要验的东西，
   也会让"单击 Caps"这类用例依赖机器上装了什么输入法。所以这里桩成"记账"：
   只记收到了哪些请求，于是 hook.c 里的**单击判定**可以在没有真实输入法的前提下
   被测到（它才是这里唯一的新逻辑）。 */
static int g_imeReq[IME_KEY_COUNT];
void ImeRequest(int key) {
    if (key >= 0 && key < IME_KEY_COUNT) g_imeReq[key]++;
}
void ImeLaneStart(void) { }
void ImeLaneStop(void)  { }
static void ResetImeRec(void) {
    for (int i = 0; i < IME_KEY_COUNT; i++) g_imeReq[i] = 0;
}
#include "hook.c"
#include "send.c"      /* hook.c 的 SendActOnce 现在走统一发送模块 */
#include "act.c"       /* 动作实际由执行线程完成：本测试要能起/停/等它 */
#include <stdio.h>

/* 按词跳转桩：本测试只验证模式修饰/补齐/自动滚动，不测文本解析。
   g_slowJumpMs != 0 时模拟"真按词跳转要几百毫秒"（同步 IPC + COM 往返），
   供场景AC构造"跳词占住执行线程时又按键"的确定性时序。
   直接 Sleep（不泵消息）：它跑在工作线程上，要模拟的正是"这条请求占住
   执行线程"，主线程在别的线程上照常泵消息、投递请求。 */
static LONGLONG g_slowJumpMs = 0;
int NavJump(int dir, BOOL shift) {
    (void)dir; (void)shift;
    if (g_slowJumpMs > 0) Sleep((DWORD)g_slowJumpMs);
    return 0;
}
void NavProbe(void) { /* 诊断探测：本测试桩掉 */ }
void NavBaseInfo(void) { /* 启动基线采集：本测试桩掉 */ }
void NavBaseAppendLine(const WCHAR* t) { (void)t; }   /* main.c 往基线追加 DPI 自检行 */
void NavSelTrackReset(void) { /* 选区锚跟踪：本测试桩掉 */ }
long long NavFocusCaret(void) { return -1; }   /* 光标回读只给诊断日志用，本测试没有真编辑器 */
/* WPS 通道桩（真实现要链接 wps.c/bridge.c，本测试不链）：
   WinMain 里那三个入口 —— --bridge 子进程模式（BrParseChildSwitch）、
   退出时关桥（WpsBridgeShutdown）、以及 wps.c 的桥服务函数 —— 桩掉即可 */
int  WpsBridgeServe(unsigned long pid, unsigned long seq) { (void)pid; (void)seq; return 0; }
BOOL BrParseChildSwitch(DWORD* pid, DWORD* seq) { (void)pid; (void)seq; return FALSE; }
void WpsBridgeShutdown(void) { }

/* ---- 按键帮助桩（src/help.c 不进本测试）----
   真实现会 CreateProcess 拉起 CapsEnhanceHelp.exe、还要跨进程发消息 —— 
   本测试既不该弹窗，也不该在别人机器上找那个 exe。桩成"记账"：
   只记开/关次数与顺序，用来钉"帮助窗口的寿命 = 这个和弦"这条语义
   （按下开、抬起关、松开 Caps 也关）。 */
static int g_helpShow = 0, g_helpHide = 0;
void HelpShow(void) { g_helpShow++; }
void HelpHide(void) { g_helpHide++; }
void HelpTick(void) { }
void HelpKaTick(void) { }
static void ResetHelpRec(void) { g_helpShow = 0; g_helpHide = 0; }

/* 剪贴板浮层（C+Alt+V）：与帮助同一类"只是拉起另一个 exe 的窗口"，同样桩掉 ——
   真去 CreateProcess 会让这条单元测试依赖 bin 里有没有那个 exe。
   这里**故意**把 ClipIsOpen 做成跟着 ClipShow/ClipHide 走：钩子据它决定
   "要不要替浮层吞 Esc"，所以它必须反映真实的开合，而不是恒真/恒假。 */
static int  g_clipShow = 0, g_clipHide = 0, g_clipEsc = 0, g_clipKey = 0;
static BOOL g_clipOpen = FALSE;
void ClipShow(void) { g_clipShow++; g_clipOpen = TRUE; }
void ClipHide(void) { g_clipHide++; g_clipOpen = FALSE; }
void ClipTick(void) { }
void ClipKaTick(void) { }
void ClipEsc(void) { g_clipEsc++; ClipHide(); }
void ClipKey(int vk, unsigned flags) { (void)vk; (void)flags; g_clipKey++; }
void ClipCallDitto(void) { g_clipHide++; g_clipOpen = FALSE; }   /* 转本体先关浮层 */
BOOL ClipIsOpen(void) { return g_clipOpen; }
static void ResetClipRec(void) {
    g_clipShow = 0; g_clipHide = 0; g_clipEsc = 0; g_clipKey = 0; g_clipOpen = FALSE;
}

/* ---- 捕获钩子：记录注入的完整事件序列（按下/抬起），并可统计次数 ---- */
typedef struct { int vk; int n; } Rec;
static Rec g_rec[256]; static int g_recN = 0;
/* 事件序列：验证发送顺序用（如 Ctrl 必须先于方向键按下、后于方向键抬起） */
typedef struct { int vk; int up; } Ev;
static Ev g_seq[512]; static int g_seqN = 0;
static void RecAdd(int vk, int up) {
    if (!up) {
        for (int i = 0; i < g_recN; i++)
            if (g_rec[i].vk == vk) { g_rec[i].n++; return; }
        if (g_recN < 256) { g_rec[g_recN].vk = vk; g_rec[g_recN].n = 1; g_recN++; }
    }
    if (g_seqN < 512) { g_seq[g_seqN].vk = vk; g_seq[g_seqN].up = up; g_seqN++; }
}
LRESULT CALLBACK CapProc(int n, WPARAM w, LPARAM l) {
    if (n == HC_ACTION) {
        KBDLLHOOKSTRUCT *p = (KBDLLHOOKSTRUCT*)l;
        if (p->flags & LLKHF_INJECTED) {
            BOOL down = (w == WM_KEYDOWN || w == WM_SYSKEYDOWN);
            BOOL upEv = (w == WM_KEYUP   || w == WM_SYSKEYUP);
            if (down || upEv) RecAdd((int)p->vkCode, !down);
        }
    }
    return CallNextHookEx(NULL, n, w, l);
}
static int CountVk(int vk) {
    for (int i = 0; i < g_recN; i++) if (g_rec[i].vk == vk) return g_rec[i].n;
    return 0;
}
/* 事件序列里某个 (vk, up) 的索引；找不到返回 -1 */
static int SeqIdx(int vk, int up) {
    for (int i = 0; i < g_seqN; i++)
        if (g_seq[i].vk == vk && g_seq[i].up == up) return i;
    return -1;
}
static void ResetRec(void) { g_recN = 0; g_seqN = 0; }

/* ---- 滚轮注入计数钩子（场景Q/T 用）----
   统计"我们注入出去的滚轮事件"格数与方向和，纵轴与横轴各一套。这**不是**生产
   那个 LowLevelMouseProc（它做决策），所以不存在"注入事件被自己再放大"的风险。 */
static int g_wheelInj = 0;     /* 注入的**纵轴**事件条数 */
static int g_wheelSum = 0;     /* 注入的纵轴 delta 之和（方向也在这里） */
static int g_whInj = 0;        /* 注入的**横轴**事件条数 */
static int g_whSum = 0;        /* 注入的横轴 delta 之和（正=右，负=左） */
static void ResetWheelRec(void) { g_wheelInj = 0; g_wheelSum = 0; g_whInj = 0; g_whSum = 0; }
LRESULT CALLBACK WheelCapProc(int n, WPARAM w, LPARAM l) {
    if (n == HC_ACTION && (w == WM_MOUSEWHEEL || w == WM_MOUSEHWHEEL)) {
        const MSLLHOOKSTRUCT* m = (const MSLLHOOKSTRUCT*)l;
        if (m && (m->flags & LLMHF_INJECTED)) {
            int d = (int)(short)HIWORD(m->mouseData);
            if (w == WM_MOUSEWHEEL) { g_wheelInj++; g_wheelSum += d; }
            else                    { g_whInj++;    g_whSum    += d; }
        }
    }
    return CallNextHookEx(NULL, n, w, l);
}

/* ---- 仿真工具 ---- */
static void Key(int vk, BOOL down) {
    KBDLLHOOKSTRUCT ks; memset(&ks, 0, sizeof(ks));
    ks.vkCode = (DWORD)vk; ks.flags = 0;
    LowLevelKeyboardProc(HC_ACTION,
        down ? WM_KEYDOWN : WM_KEYUP, (LPARAM)&ks);
}
/* 系统键路径：真实 Alt 的按下/抬起是 WM_SYSKEYDOWN / WM_SYSKEYUP */
static void KeyAlt(int vk, BOOL down) {
    KBDLLHOOKSTRUCT ks; memset(&ks, 0, sizeof(ks));
    ks.vkCode = (DWORD)vk; ks.flags = 0;
    LowLevelKeyboardProc(HC_ACTION,
        down ? WM_SYSKEYDOWN : WM_SYSKEYUP, (LPARAM)&ks);
}

/* ---- 场景 X/Z 用：把钩子过程当**函数**调用，并观察它的返回值 ----
   返回值 1 = 吞掉（不外泄给系统），0 = 放行。这是验证"吞/放必须成对"的
   唯一可靠手段（Alt 卡键的根因就是 down 与 up 被区别对待）。

   同时维护一份"系统看到的状态"：本测试是直接调钩子过程的，系统对伪造事件
   一无所知，而生产代码要靠"系统是否也认为该键按着"来区分真按与残留。
   所以这里按钩子的返回值模拟系统：**放行才等于系统看到了**。
   （hook.c 的 SysKeyHeld 通过函数指针替换成 TestSysHeld。） */
static BYTE t_sys[256];
static BOOL TestSysHeld(int vk) { return vk >= 0 && vk < 256 && t_sys[vk]; }
static LRESULT Sim(int vk, BOOL down) {
    KBDLLHOOKSTRUCT ks; memset(&ks, 0, sizeof(ks));
    ks.vkCode = (DWORD)vk; ks.flags = 0;
    LRESULT r = LowLevelKeyboardProc(HC_ACTION,
        down ? WM_KEYDOWN : WM_KEYUP, (LPARAM)&ks);
    if (r == 0 && vk >= 0 && vk < 256) t_sys[vk] = (BYTE)(down ? 1 : 0);
    return r;
}

/* ---- 取走钩子投递的滚轮请求 ----
   这里**不**去数"目标窗口收到了几条 WM_MOUSEWHEEL"：滚轮消息的落点取决于
   焦点窗口 / 光标下方的窗口（还受系统"悬停滚动非活动窗口"设置影响），让测试
   依赖系统把消息派发到哪个窗口是不稳的 —— 实测 10 次里有 1 次因为光标被挪开
   而完全收不到。
   改成验证**钩子的决策**：它该不该投递请求、投递的参数对不对，以及**有没有
   吞掉物理那一格**（返回 1）。这部分完全在本进程内可观测，稳定。 */
static BOOL TakeWheelAccel(int* delta, int* mul) {
    MSG m;
    if (PeekMessageW(&m, g_mainWnd, WM_WHEEL_ACCEL, WM_WHEEL_ACCEL, PM_REMOVE)) {
        if (delta) *delta = (int)m.wParam;
        if (mul)   *mul   = (int)m.lParam;
        return TRUE;
    }
    return FALSE;
}
/* 取走钩子投递的"持续滚动"请求。 */
static BOOL TakeWheelEnh(int* delta) {
    MSG m;
    if (PeekMessageW(&m, g_mainWnd, WM_WHEEL_ENH, WM_WHEEL_ENH, PM_REMOVE)) {
        if (delta) *delta = (int)m.wParam;
        return TRUE;
    }
    return FALSE;
}
/* 取走钩子投递的**横向**滚动请求（Caps+Shift+滚轮）。 */
static BOOL TakeWheelHAccel(int* delta, int* mul) {
    MSG m;
    if (PeekMessageW(&m, g_mainWnd, WM_WHEEL_HACCEL, WM_WHEEL_HACCEL, PM_REMOVE)) {
        if (delta) *delta = (int)m.wParam;
        if (mul)   *mul   = (int)m.lParam;
        return TRUE;
    }
    return FALSE;
}
/* 模拟硬件滚轮：直接喂给生产用的鼠标钩子过程（flags=0 表示非注入），
   返回钩子的判定（1 = 这一格被吞掉，事件不再往下传）。
   注意只模拟"钩子的判断"，不会真的产生那原始一格。 */
static LRESULT Wheel(int delta, DWORD flags) {
    MSLLHOOKSTRUCT ms; memset(&ms, 0, sizeof(ms));
    ms.mouseData = (DWORD)(USHORT)(SHORT)delta << 16;   /* 高字是滚轮增量 */
    ms.flags = flags;
    return LowLevelMouseProc(HC_ACTION, WM_MOUSEWHEEL, (LPARAM)&ms);
}
/* 横轴滚轮（WM_MOUSEHWHEEL）：高字同样是增量。
   为什么单独一个入口：触控板的双指滑动会**两个轴都报**，而横轴事件过去不在
   控制杆分支里（它会一路落到"新输入 -> 打断"）—— 是"按住 Caps+Alt 滚一下就停"
   的怀疑来源之一。 */
static LRESULT WheelH(int delta, DWORD flags) {
    MSLLHOOKSTRUCT ms; memset(&ms, 0, sizeof(ms));
    ms.mouseData = (DWORD)(USHORT)(SHORT)delta << 16;
    ms.flags = flags;
    return LowLevelMouseProc(HC_ACTION, WM_MOUSEHWHEEL, (LPARAM)&ms);
}
/* 日志文件的当前大小（不存在返回 -1）。
   用途：验证"日志只在 C+` 记录模式下生成"这条契约 —— 判定用**文件大小有没有
   变化**，比读内容简单也更准（读内容还要处理编码与轮转）。 */
static LONGLONG LogSize(void) {
    WCHAR p[MAX_PATH];
    log_path(p, MAX_PATH);                 /* main.c 里的静态函数，本 TU 可见 */
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(p, GetFileExInfoStandard, &d)) return -1;
    return ((LONGLONG)d.nFileSizeHigh << 32) | (LONGLONG)d.nFileSizeLow;
}
/* 模拟鼠标的其它动作（按键按下/抬起、移动、横滚轮）。
   持续滚动的"按任意键（含鼠标）终止"规则要靠它来验。 */
static LRESULT MouseEv(UINT msg, DWORD flags) {
    MSLLHOOKSTRUCT ms; memset(&ms, 0, sizeof(ms));
    ms.flags = flags;
    return LowLevelMouseProc(HC_ACTION, msg, (LPARAM)&ms);
}
/* 拖拽相关用例用：带**坐标**的鼠标事件（拖拽按 pt 找窗口，必须能指定位置）。
   flags=0 表示非注入（生产钩子对注入事件一律放行）。 */
static LRESULT MousePt(UINT msg, POINT pt) {
    MSLLHOOKSTRUCT ms; memset(&ms, 0, sizeof(ms));
    ms.pt = pt;
    ms.flags = 0;
    return LowLevelMouseProc(HC_ACTION, msg, (LPARAM)&ms);
}
/* 排干所有待处理消息（含残留 TIMER/TRIG） */
static void Sink(void) {
    MSG m;
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE))
        DispatchMessageW(&m);
}

/* 一边等一边泵：**用 MsgWaitForMultipleObjects，不要用 Sleep**。
   Sleep 期间本线程不在"等消息"状态，系统对低级钩子的处理是**直接跳过**
   （不是延后补发）—— 于是"另一个线程注入、本线程钩子捕获"的事件会时有时无
   地被漏记，断言就飘（实测 12 次里飘 1 次，表现为 LSHIFT 一次都没记到）。
   MsgWait 是真正的等消息状态，钩子能在第一时间被调用。 */
static void SinkWait(int ms) {
    MSG m;
    MsgWaitForMultipleObjects(0, NULL, FALSE, (DWORD)ms, QS_ALLINPUT);
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE))
        DispatchMessageW(&m);
}
/* 只泵消息，不等待执行线程：时序敏感的场景（"按下后马上抬手"）靠它 ——
   一旦在这里等执行线程，异步发送的完成时刻就被改了，测的东西也变了。
   需要等发送做完时，在断言前单独调 Drain()。
   用 SinkWait 而不是 Sleep：要一直在"等消息"状态，钩子才不会漏记注入事件。 */
static void Pump(int ms) {
    ULONGLONG end = GetTickCount64() + ms;
    while (GetTickCount64() < end)
        SinkWait(1);
    Sink();   /* 收尾：把压着的 TIMER 也处理掉 */
}

/* 等执行线程把当前场景的动作做完，并把注入事件"看全"。 */
static void Drain(void) {
    for (int i = 0; i < 3000 && ActInFlight() != 0; i++)
        SinkWait(1);
    /* 再等一段：注入事件到达本线程的钩子有延迟（本机实测可达 ~60ms） */
    ULONGLONG end = GetTickCount64() + 200;
    while (GetTickCount64() < end)
        SinkWait(1);
}

/* 把一条托盘回调送进主窗口（场景AJ）。
   必须走 **PostMessage**：真的 WM_TOGGLE_TRAY 是 explorer 收到通知区的鼠标事件后
   Post 过来的（见 test/traymemtest.c），Post 与 Send 都进同一个 WndProc，
   但 Send **同步跑完** —— 想在"消息到达/被处理"这个观察点上做断言，
   同步 Send 会让它恰好在断言之前消化掉，于是测出来的永远是"什么都没发生"。 */
static void TrayClick(WPARAM lp) {
    PostMessageW(g_mainWnd, WM_TOGGLE_TRAY, 1, lp);
}

/* 场景收尾：松开 C、泵完消息，再等执行线程把注入做完 */
#define END_SCENE() do { Key(CLI_CAPS, FALSE); Pump(50); Drain(); } while (0)

/* 场景间复位：清引擎状态与残留消息，保证场景相互独立 */
static void Reset(void) {
    /* ModHoldReset 一次清掉全部"按住状态"：C / 真实 Shift / 模式位图 /
       探测闩锁 / 吞放记账 / Caps 会话标记。场景之间必须彼此独立，
       否则上一场景残留的记账会让下一场景的吞放断言飘。 */
    ModHoldReset();
    g_actEnh = FALSE; g_actShf = FALSE; g_btnCount = 0;
    ActAutoScrollStop();     /* 上一场景可能还在自动滚动：先停，避免尾流串到下一场景 */
    memset((void*)g_phys, 0, sizeof(g_phys));   /* 物理按键表也归零 */
    memset(t_sys, 0, sizeof(t_sys));            /* 模拟的"系统状态"也归零 */
    PostMessageW(g_mainWnd, WM_CAPS_DOWN, 0, 0);  /* → ResetEngine：清 active + 停表 + 丢尾流 */
    Sink();
    Drain();   /* 等上一场景的尾巴收干净，否则残留注入会被算进下一场景 */
    ResetRec();
}

static int fails = 0;

/* 写一份临时 ini 供 ConfigLoadFrom 用（场景O：超限改道的契约验证）。
   ConfigLoadFrom 会先把所有项复位成缺省再应用文件内容，所以每写必"干净"。 */
static void WriteChordIni(const char* body) {
    FILE* f = NULL;
    if (fopen_s(&f, "bin/chordtest.ini", "wb") == 0 && f) {
        fputs(body, f);
        fclose(f);
    }
}
#define CHECK(cond, name) do { \
    if (cond) printf("  [OK]   %s\n", name); \
    else      { printf("  [FAIL] %s   (captured: ", name); \
               for(int __i=0;__i<g_recN;__i++) printf("%d:%d ",g_rec[__i].vk,g_rec[__i].n);\
               printf(")\n"); fails++; } } while (0)

/* 场景AE 用：找一块**不是** `self` 的显示器，输出它工作区的中心点。
   找不到（本项目最常见的单显示器机器）时 `*found = FALSE`，调用方跳过那条断言。
   多显示器下"挪到另一块屏"才成立，单显示器上硬测就是假红。
   上下文用**文件作用域的命名结构体**：/TP 按 C++ 编译，两处各写一个匿名
   struct 是两个不同的类型，转换直接报错。 */
typedef struct { HMONITOR self; POINT* out; BOOL* found; } MonOtherCtx;
static BOOL CALLBACK MonitorOtherProc(HMONITOR hm, HDC dc, LPRECT rc, LPARAM p) {
    (void)dc; (void)rc;
    MonOtherCtx* st = (MonOtherCtx*)p;
    if (st->found && *st->found) return FALSE;      /* 已经找到了 */
    if (hm == st->self) return TRUE;
    MONITORINFO minfo;                               /* 别叫 mi：文件里有同名全局/局部 */
    memset(&minfo, 0, sizeof(minfo));
    minfo.cbSize = sizeof(minfo);
    if (!GetMonitorInfoW(hm, &minfo)) return TRUE;
    st->out->x = minfo.rcWork.left + (minfo.rcWork.right - minfo.rcWork.left) / 2;
    st->out->y = minfo.rcWork.top + (minfo.rcWork.bottom - minfo.rcWork.top) / 2;
    *st->found = TRUE;
    return FALSE;
}
static void EnumMonitorsForOther(HMONITOR self, POINT* out, BOOL* found) {
    out->x = out->y = 0;
    *found = FALSE;
    MonOtherCtx st;
    st.self = self; st.out = out; st.found = found;
    EnumDisplayMonitors(NULL, NULL, MonitorOtherProc, (LPARAM)&st);
}

/* 场景AE 用：回读到的矩形是否等于"按既定规则算出的落点"。
   **期望值调用产品自己的纯函数**（`WinComputePlacement`）而不是硬写"左上角 == 光标"：
   光标贴着工作区下沿时整窗放不下，产品会按规则钳进来（实测：工作区底 706、窗口高 260、
   光标 y=498 → 正确落点 446），那种情况硬断言必然假红。
   也不拿"光标在不在窗口里"当判据 —— 窗口自己缩小后那个仍然成立，测不出区别。 */
static BOOL PlacementMatches(const RECT* work, POINT cursor, RECT got) {
    POINT exp;
    WinComputePlacement(work, cursor,
                        (int)(got.right - got.left), (int)(got.bottom - got.top), &exp);
    return got.left == exp.x && got.top == exp.y;
}

/* 场景AE 用：模拟两类"不老实"的程序（真机上都会遇到）。
   ① g_shrinkOnMove > 0：挪过去之后**自己改尺寸** —— 实测 CLaunch 跨不同缩放比例的
      屏就是这样（保持物理尺寸不变，逻辑尺寸 578x437 → 495x374）。落点改成"左上角
      对齐光标"之后，这**不该**影响落点，这正是改规则的理由。
   ② g_jumpOnMove > 0：挪过去之后**又把自己挪走**（带吸附/跟随鼠标行为的启动器、
      面板类工具）。这种只能靠"回读锚点 + 再发一次"。
   两个计数器各自只触发一次，免得无限递归。 */
static int g_shrinkOnMove = 0;
static int g_jumpOnMove = 0;
/* 窗口收到过多少次"位置变化"通知。C+Q 那条"最大化窗口同屏不还原不挪"的断言
   靠它：只看矩形的话，"还原成小窗口、挪一下、再最大化铺回去"结束时的矩形与
   刚开始时**完全一样** —— 正是用户说的"挪跟没挪一样，只会抖一下"。
   数通知次数才分得清"真没动"与"动了一圈又回到原样"。 */
static int g_posChanged = 0;
static LRESULT CALLBACK MoveProbeProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_WINDOWPOSCHANGED) {
        g_posChanged++;
        if (g_shrinkOnMove > 0) {
            g_shrinkOnMove--;
            SetWindowPos(h, NULL, 0, 0, 360, 260,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        } else if (g_jumpOnMove > 0) {
            RECT r;
            g_jumpOnMove--;
            if (GetWindowRect(h, &r))
                SetWindowPos(h, NULL, (int)r.left + 150, (int)r.top + 120, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    return DefWindowProcW(h, m, w, l);
}

int main(void) {
    g_state.paused = FALSE;
    SetWindowsHookExW(WH_KEYBOARD_LL, CapProc, GetModuleHandleW(NULL), 0);
    /* 滚轮注入计数（场景Q/T）：只计数，不做任何决策 */
    SetWindowsHookExW(WH_MOUSE_LL, WheelCapProc, GetModuleHandleW(NULL), 0);

    /* 生产路径里动作由 act.c 的执行线程发送，测试必须起同一个线程，
       否则测的是"同步兜底"分支，覆盖不到真实时序。 */
    ActStart();

    /* hook.c 靠"系统是否也认为该键按着"来区分真按与残留；本测试直接调钩子
       过程，系统并不知道伪造事件，所以换成测试自己的状态表。 */
    g_sysKeyHeld = TestSysHeld;

    /* 键位表必须在引擎工作前装配好。生产路径在 WinMain 里调 KeyMapInit；
       本测试不走 WinMain（main.c 的 WinMain 被改名弃用），所以要显式调 ——
       漏掉它模式表就是空的，Alt/D/F 一个都不生效（所有修饰场景会全红）。 */
    KeyMapInit();

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"CapsEnhanceTest";
    RegisterClassW(&wc);
    g_mainWnd = CreateWindowW(L"CapsEnhanceTest", L"t",
                              WS_POPUP | WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1, 1,
                              NULL, NULL, GetModuleHandleW(NULL), NULL);

    /* ---- 场景A：严格要求顺序 —— J 先按、D 后到 = 无增强的一次左键 ----
       这里原先测的是"乱序和弦"：按下触发键后等 50ms 稳定期再开火，好让迟到的
       Alt/D 也能生效。现已改成**严格要求顺序**（修饰键必须先按住，
       再按触发键）—— 稳定期已删除，按下即开火，反向顺序就是一次普通动作，
       不再补正。这个场景就此反过来：D 迟到**不该**改变本次的发送。 */
    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    Key('J', TRUE);  Pump(2);     /* 此刻 D 还没按住：本次就是无增强的左键 */
    Key('D', TRUE);  Pump(2);     /* D 迟到：不补正 */
    Key('J', FALSE); Pump(2);
    Key('D', FALSE); Pump(2);
    END_SCENE();
    CHECK(CountVk(VK_LEFT) == 1, "A: 严格要求顺序 C+J+D -> 只发 1 次左键（不补正）");

    /* ---- 场景B：F 使 Shift 生效（严格序 C F J） ---- */
    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    Key('F', TRUE); Pump(2);     /* 先按住选区修饰 */
    Key('J', TRUE);  Pump(2);
    Key('J', FALSE); Pump(2);    /* 短点：Shift 生效 */
    Key('F', FALSE); Pump(2);
    END_SCENE();
    CHECK(CountVk(VK_LEFT) == 1 && CountVk(VK_LSHIFT) >= 1,
          "B: C+F+J = Shift+左键 (1次)");

    /* ---- 场景C：F 后到（J 先于 F）-> 本次不带 Shift，不补正 ---- */
    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    Key('J', TRUE);  Pump(2);
    Key('F', TRUE);  Pump(2);    /* F 迟到：不补正 */
    Key('J', FALSE); Pump(2);
    Key('F', FALSE); Pump(2);
    END_SCENE();
    CHECK(CountVk(VK_LEFT) == 1 && CountVk(VK_LSHIFT) == 0,
          "C: 先 J 后 F -> 裸左键 1 次（不补 Shift）");

    /* ---- 场景D：C+J 之后 D/F 才到 -> 仍是 1 次裸左键 ---- */
    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    Key('J', TRUE);  Pump(2);
    Key('D', TRUE);  Pump(2);
    Key('F', TRUE);  Pump(2);
    Key('J', FALSE); Pump(2);    /* D/F 门闩均生效 -> 不再补齐、也不加 Shift */
    Key('D', FALSE); Pump(2);
    Key('F', FALSE); Pump(2);
    END_SCENE();
    CHECK(CountVk(VK_LEFT) == 1 && CountVk(VK_LSHIFT) == 0,
          "D: 先 J、后 D/F -> 1 次裸左键（序严格要求）");

    /* ---- 场景AB：按下触发键立即开火，不再等 50ms 稳定期 ----
       旧实现：按下后只置 pending，等 CHORD_SETTLE(50ms) 由 WM_TIMER 开火，
       实测"按下 -> 开始执行"57~71ms（"不跟手"就是这个）。
       现在按下即开火。
       断言用主线程状态（g_activeAct/g_btnCount）而不是注入计数：注入事件
       到达本进程的捕获钩子有延迟（实测可达 ~60ms），拿它测"立即"会飘。 */
    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    Key('J', TRUE);
    Sink();                        /* 只把 WM_TRIG_DOWN 派发掉，不做任何等待 */
    CHECK(g_activeAct == ACT_LEFT && g_btnCount == 1,
          "AB: 按下触发键立即开火（无稳定期等待）");
    Key('J', FALSE); Pump(2);
    END_SCENE();

    /* ---- 场景AC：跳词在飞时按下的键不能被丢掉 ----
       一次点按现在是两条请求（按下开火 + 抬起补齐），两者的 act/fillAct 互补。
       ResetEngine 的 ActClearKeepFill 曾经按"fillAct != NONE"判"这条属于本次
       点按"，于是**开火那条**（fillAct 为空）被当成重复尾流丢掉 —— 用户按了键
       却没有输出。改成显式 keep 标志后，开火与补齐都保留。
       这里用慢 NavJump 桩把跳词拉长，让按键请求确实排在跳词后面。 */
    Reset();
    g_slowJumpMs = 400;
    Key(CLI_CAPS, TRUE); Pump(2);
    Key('N', TRUE); Pump(2);        /* 跳词：立即开火，占住执行线程 400ms */
    Pump(60);                       /* 等它确实在跑 */
    Key('J', TRUE); Pump(2);        /* 跳词期间点按：请求排在跳词后面 */
    Key('J', FALSE); Pump(2);
    Key(CLI_CAPS, FALSE);           /* 松 C：ResetEngine（清重复尾流） */
    Pump(600); Drain();
    g_slowJumpMs = 0;
    CHECK(CountVk(VK_LEFT) >= 1, "AC: 跳词期间点按的方向键不被复位清掉");

    /* ---- 场景E：长按 C+J（无 D）连续重复，松手后不再输出 ----
       发送是异步的：读计数前先 Drain，比较点都取在"松手之后"，
       这样断言的是"松手之后不再产生新的输出"，而不是跟异步时序赛跑。 */
    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    Key('J', TRUE); Pump(300);   /* 按下即开火 + 首次150ms重复 + 若干 */
    Drain();
    int duringHold = CountVk(VK_LEFT);
    Key('J', FALSE); Pump(2);    /* 抬手：不应再补(无D) */
    END_SCENE();
    int afterUp = CountVk(VK_LEFT);
    Pump(400); Drain();
    CHECK(duringHold >= 2, "E: 长按 C+J 连续重复");
    CHECK(afterUp - duringHold <= 1, "E: 松手后最多只剩已派发的那一发");
    CHECK(CountVk(VK_LEFT) == afterUp, "E: 松手后不再继续输出左键");

    /* ---- 场景F：单点 C+J 无 D = 1 左键（不补齐） ---- */
    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    Key('J', TRUE);  Pump(2);
    Key('J', FALSE); Pump(2);    /* 按下即开火、抬起只补齐：无增强 -> 1 次 */
    END_SCENE();
    CHECK(CountVk(VK_LEFT) == 1, "F: 单点 C+J = 1 左键");

    /* ---- 场景G：按住 C 时 F 单独按住（不触发任何 trigger）= 0 注入 ---- */
    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    Key('F', TRUE);  Pump(150);
    Key('F', FALSE); Pump(2);
    END_SCENE();
    CHECK(CountVk(VK_LEFT) == 0 && CountVk(VK_LSHIFT) == 0,
          "G: C+F 按住无 trigger = 不注入任何键");

    /* ---- 场景H：按住 C+F 依次点 J、K，两次都应 Shift 位移 ---- */
    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    Key('F', TRUE);  Pump(2);    /* F 一直按住 */
    Key('J', TRUE);  Pump(2);
    Key('J', FALSE); Pump(2);    /* Shift+左 */
    Key('K', TRUE);  Pump(2);
    Key('K', FALSE); Pump(2);    /* Shift+下 */
    Key('F', FALSE); Pump(2);
    END_SCENE();
    CHECK(CountVk(VK_LEFT) == 1 && CountVk(VK_DOWN) == 1 && CountVk(VK_LSHIFT) >= 2,
          "H: C+F 按住点 J/K 均带 Shift");

    /* ---- 场景I：J、K 同时按住（曾导致"一直输出左键"）----
       旧实现钩子只记一个按住的键：J 按住时再按 K 会把 J 顶掉，
       J 的抬起事件再也发不出去，引擎收不到 UP 就永远重复 LEFT。 */
    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    Key('J', TRUE);  Pump(200);      /* 长按 J：重复左键 */
    int leftAtK = CountVk(VK_LEFT);
    Key('K', TRUE);  Pump(200);      /* J 未抬起时按 K：应切到下键 */
    int downAtK = CountVk(VK_DOWN);
    Key('K', FALSE); Pump(120);      /* 松开 K：退回仍按着的 J，继续左键 */
    int leftAfterKUp = CountVk(VK_LEFT);
    Key('J', FALSE); Pump(2);        /* 松开 J：必须停止 */
    Pump(50); Drain();
    int leftFinal = CountVk(VK_LEFT);
    int downFinal = CountVk(VK_DOWN);
    Pump(400); Drain();              /* 再等 400ms：不应再有任何注入 */
    END_SCENE();
    CHECK(downAtK >= 1, "I: J 按住时再按 K -> 切到下键");
    CHECK(leftAfterKUp > leftAtK, "I: 松开 K 后退回仍按着的 J（继续左键）");
    CHECK(CountVk(VK_LEFT) == leftFinal && CountVk(VK_DOWN) == downFinal,
          "I: 全部抬起后重复停止（不再无限输出左键）");

    /* ---- 场景J：左 Alt 等同 D（增益），走系统键消息路径 ---- */
    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    KeyAlt(VK_LMENU, TRUE);  Pump(2);   /* WM_SYSKEYDOWN：门闩 D */
    Key('J', TRUE);  Pump(2);
    Key('J', FALSE); Pump(2);           /* 短点：增益补齐到 3 */
    KeyAlt(VK_LMENU, FALSE); Pump(2);
    END_SCENE();
    CHECK(CountVk(VK_LEFT) == 3, "J: 左Alt 等同 D（Caps+Alt+J = 3 次左键）");
    CHECK(CountVk(VK_LMENU) == 0, "J: Alt 本身不外泄（被屏蔽）");

    /* ---- 场景K：长按 J 中途松开 Caps -> 重复必须立刻停止 ---- */
    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    Key('J', TRUE);  Pump(200);          /* 长按重复左键 */
    Key(CLI_CAPS, FALSE); Pump(2);       /* J 还按着就松开 C：映射失效 */
    Key('J', FALSE); Pump(2);
    Pump(50); Drain();
    int nKStopped = CountVk(VK_LEFT);
    Pump(400); Drain();                  /* 再等 400ms：不应再有注入 */
    CHECK(CountVk(VK_LEFT) == nKStopped, "K: 松开 Caps 后重复立刻停止");

    /* ---- 场景L：清卡键（SendClearStuckModifiers 的判定）----
       卡键 = OS 逻辑按下 && 物理未按。注入的键同样会置位逻辑状态，
       但不会更新物理表 —— 所以注入 down 之后调用清理，应补发 up。 */
    Reset();
    SendEvent(VK_LCONTROL, FALSE); Pump(30);     /* 注入 Ctrl down（逻辑按下） */
    CHECK(g_phys[VK_LCONTROL] == 0, "L: 注入的键不进物理表");
    SendClearStuckModifiers(); Pump(30); Drain();
    CHECK(SeqIdx(VK_LCONTROL, 1) >= 0, "L: 卡住的 Ctrl 被补发抬起");
    SendEvent(VK_LCONTROL, TRUE); Pump(10);      /* 收尾：确保系统状态干净 */
    Pump(30);

    /* 物理按住的键不能被清掉（用户真的按着 Ctrl 时清理应放过它）。
       模拟：注入 down 后手工把"注入过"标记清零、物理表置位，
       相当于"用户物理按住且与我们无关"的状态。 */
    SendEvent(VK_LCONTROL, FALSE); Pump(30);
    /* 直接清 send.c 的注入标记：g_injectedDown 在同一编译单元 */
    for (int i = 0; i < 8; i++)
        if (kMods[i] == VK_LCONTROL) g_injectedDown[i] = FALSE;
    g_phys[VK_LCONTROL] = 1;                     /* 物理按住 */
    ResetRec();
    SendClearStuckModifiers(); Pump(30); Drain();
    CHECK(SeqIdx(VK_LCONTROL, 1) < 0, "L: 物理按住的 Ctrl 不被清理");
    g_phys[VK_LCONTROL] = 0;
    SendEvent(VK_LCONTROL, TRUE); Pump(10);      /* 收尾 */
    Pump(30);

    /* ---- 场景M：原生按词跳转的按键序列（Ctrl 提前抬起 bug 的回归）----
       Ctrl 必须先于方向键按下、后于方向键抬起，否则目标收到的
       只是单格方向键。 */
    Reset();
    SendNativeWordJump(DIR_LEFT, FALSE);
    Drain();   /* 注入事件要泵消息才会被本线程的捕获钩子看到 */
    int cd  = SeqIdx(VK_LCONTROL, 0);
    int ld  = SeqIdx(VK_LEFT, 0);
    int lu  = SeqIdx(VK_LEFT, 1);
    int cu  = SeqIdx(VK_LCONTROL, 1);
    CHECK(cd >= 0 && ld >= 0 && lu >= 0 && cu >= 0,
          "M: Ctrl+方向 四段序列齐全");
    CHECK(cd < ld && ld < lu && lu < cu,
          "M: 顺序正确 Ctrl↓ -> ←↓ -> ←↑ -> Ctrl↑");
    CHECK(CountVk(VK_LCONTROL) == 1, "M: Ctrl 只按一次");

    /* ---- 场景N：按住 C 时鼠标滚轮的决策（三路分流 + 吞格只发生在横向） ----
       Caps + 滚轮            = **快速滚动**（WheelScrollMul 倍）-> WM_WHEEL_ACCEL
       Caps + Alt/D + 滚轮     = **持续滚动**                   -> WM_WHEEL_ENH
       Caps + Shift + 滚轮     = **横向滚动**                    -> WM_WHEEL_HACCEL
       **吞不吞物理格**：横向吞（物理格是纵向事件，放行就横纵混滚）；
       持续滚动档也吞 —— 那时滚轮是"控制杆"，同向滚的语义只有"提速"，
       不该顺带真滚一格（双重效果）；启动时吞掉不会没反馈，因为通道一激活
       就立刻补发第一格。放大档不吞（它是"加强"：系统一格 + 我们补几格）。
       注：不把生产的 LowLevelMouseProc 真正挂进钩子链 —— 万一哪天
       LLMHF_INJECTED 守卫被删掉，真挂链上会让注入事件自我放大成风暴。
       直接调用它喂"硬件事件"/"注入事件"两条路径，都能确定性地覆盖到。 */
    Reset();
    int d = 0, mul = 0;

    Key(CLI_CAPS, FALSE); Pump(2);       /* 先确保未按 C 的基线 */
    CHECK(Wheel(120, 0) != 1 && !TakeWheelAccel(NULL, NULL),
          "N: 未按 C -> 不吞、不投递");

    Key(CLI_CAPS, TRUE); Pump(2);
    CHECK(Wheel(120, 0) != 1, "N: 只按 C（快速滚动）-> 物理那一格照常放行");
    CHECK(TakeWheelAccel(&d, &mul) && d == 120 && mul == 3,
          "N: 只按 C -> 投递放大请求（倍数 = WheelScrollMul 缺省 3）");
    CHECK(!TakeWheelAccel(NULL, NULL), "N: 一次滚轮只投递一次请求");

    Wheel(-120, 0);                      /* 方向要带对 */
    CHECK(TakeWheelAccel(&d, &mul) && d == -120 && mul == 3,
          "N: 按住 C 下滚 -> 投递 delta=-120（方向原样带过去）");

    /* 加上增强修饰键 -> 持续滚动（另一条消息） */
    Key('D', TRUE); Pump(2);
    CHECK(Wheel(120, 0) == 1, "N: C+D+滚轮（持续滚动）-> 物理那一格被完全拦截");
    CHECK(TakeWheelEnh(&d) && d == 120, "N: C+D+滚轮 -> 投递持续滚动请求");
    CHECK(!TakeWheelAccel(NULL, NULL), "N: 持续滚动时不走放大通道（互斥）");
    Key('D', FALSE); Pump(2);

    Wheel(120, LLMHF_INJECTED);          /* 我们注入出去的滚轮回到钩子 */
    CHECK(!TakeWheelAccel(NULL, NULL) && !TakeWheelEnh(NULL),
          "N: 注入的滚轮不再投递（防无限放大）");

    g_state.paused = TRUE;               /* 暂停映射：也不接管 */
    CHECK(Wheel(120, 0) != 1 && !TakeWheelAccel(NULL, NULL), "N: 暂停时不吞也不投递");
    g_state.paused = FALSE;

    /* WheelScrollMul < 2 -> 放大关闭：不投递放大请求（物理格本来就没吞） */
    WriteChordIni("[General]\r\nWheelScrollMul = 1\r\n");
    ConfigLoadFrom(L"bin/chordtest.ini");
    CHECK(Wheel(120, 0) != 1 && !TakeWheelAccel(NULL, NULL),
          "N: WheelScrollMul=1 -> 不放大（完全按系统原本行为）");
    ConfigLoadFrom(L"bin/__absent__.ini");   /* 回缺省（2） */

    Key(CLI_CAPS, FALSE); Pump(2);

    /* ---- 场景P：Caps+Tab 是"单发"翻转 —— 按一次只切一次 ----
       ACT_CAPSLOCK 原来被当作可重复、可补齐的普通动作：长按期间每
       REPEAT_INTERVAL(35ms) 切一次，切换次数取决于按住时长（偶数次 = 没切，
       CapsLock 看上去"没反应"），现象就是"caps+tab 有时要按两下才能取消锁定"。
       现在：只切一次，不进重复节奏、也不做松手补齐。
       注意 VK_CAPITAL 是真实开关键，注入会切换**本机**大小写状态 —— 测完恢复。 */
    int capsWasOn = (GetKeyState(VK_CAPITAL) & 1) ? 1 : 0;

    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    Key(VK_TAB, TRUE); Pump(250);        /* 远长于首次重复延迟 + 多个重复周期 */
    Key(VK_TAB, FALSE); Pump(2);
    END_SCENE();
    CHECK(CountVk(VK_CAPITAL) == 1, "P: 长按 C+Tab 只切换一次（不重复）");

    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    Key('D', TRUE); Pump(2);             /* 增益门闩：补齐规则对开关键不适用 */
    Key(VK_TAB, TRUE); Pump(2);
    Key(VK_TAB, FALSE); Pump(2);         /* 短点 */
    Key('D', FALSE); Pump(2);
    END_SCENE();
    CHECK(CountVk(VK_CAPITAL) == 1, "P: 带增益短点 C+D+Tab 也只切一次（不补齐）");

    /* 争议路径：另一个键正在重复时按下 Tab（开关键），只切一次；
       随后松开那个键（引擎的"退回上一个仍按着的键"）不能把它再切一次
       —— 切两次等于又切回去，用户看到的还是"没反应"。 */
    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    Key('J', TRUE); Pump(200);           /* J 进入长按重复 */
    Key(VK_TAB, TRUE); Pump(2);          /* 切换分支：应立即切一次 */
    Drain();
    int capsAfterTab = CountVk(VK_CAPITAL);
    Key('J', FALSE); Pump(2);            /* 松开 J：不应"退回"到 Tab 再切 */
    Drain();
    int capsAfterJUp = CountVk(VK_CAPITAL);
    Key(VK_TAB, FALSE); Pump(2);
    END_SCENE();
    CHECK(capsAfterTab == 1 && capsAfterJUp == 1 && CountVk(VK_CAPITAL) == 1,
          "P: J 重复中按 Tab 只切一次（松手不会退回补切）");

    /* 恢复本机 CapsLock 状态（测试不改用户的键盘状态） */
    if (((GetKeyState(VK_CAPITAL) & 1) ? 1 : 0) != capsWasOn) {
        SendKeyOnce(VK_CAPITAL, FALSE);
        Pump(30); Drain();
    }

    /* ---- 场景Q：滚轮注入跑到专用线程上（不在主线程 Sleep） ----
       注入里带 Sleep（逐格 2ms），主线程一旦不在"等消息"状态，系统对低级钩子的
       事件是**直接跳过**（不是延后补发）——松开 Caps 的 keyup 被漏掉就卡住
       g_cMod，表现是"松开 C 后还在滚 + 之后按 Tab 冒出大写锁定"。
       这里验证经专用线程发出去之后，格数与方向都正确。两轴语义不同：
         · 纵向放大：只补 (mul-1) 格（物理那格由系统照常送达）；
         · 横向滚动：注入**全部** n 格（物理那格已被钩子吞掉）。
       倍数换算单独断言 SendWheelExtraCount（真注入条数不好稳定观测）。
       左右各来一次（净滚动量为 0），避免测试真的把光标下的窗口滚走。 */
    Reset();
    Pump(150); Sink();                   /* 先把残留的请求冲干净 */

    CHECK(SendWheelExtraCount(0) == 0 && SendWheelExtraCount(1) == 0,
          "Q: 倍数 0/1 -> 不补格（等同关闭）");
    CHECK(SendWheelExtraCount(2) == 1 && SendWheelExtraCount(3) == 2,
          "Q: 倍数 -> 补 N-1 格");
    CHECK(SendWheelExtraCount(999) == WHEEL_EXTRA_MAX,
          "Q: 倍数超上限 -> 补格数被钳住");

    ResetWheelRec();
    ActWheelPost(120, 2);                /* 纵向倍数 2 -> 补 1 格 */
    Pump(100); Drain();
    CHECK(g_wheelInj == 1 && g_wheelSum != 0, "Q: 纵向倍数 2 -> 补 1 格");
    CHECK(g_whInj == 0, "Q: 纵向不会误发到横轴");

    ResetWheelRec();
    ActWheelPost(-120, 3);               /* 方向原样带过去 */
    Pump(100); Drain();
    CHECK(g_wheelInj == 2 && g_wheelSum < 0, "Q: 倍数 3 -> 补 2 格、方向为负");

    ResetWheelRec();
    ActWheelPost(120, 1);                /* 倍数 1 = 不放大 -> 一格都不补 */
    Pump(100); Drain();
    CHECK(g_wheelInj == 0, "Q: 倍数 1 -> 一格都不补（物理格交给系统）");

    ResetWheelRec();
    ActWheelPostH(120, 1);               /* 横向倍数 1 -> 注入 1 格 */
    Pump(100); Drain();
    CHECK(g_whInj == 1 && g_whSum != 0, "Q: 横向倍数 1 -> 注入 1 格");

    ResetWheelRec();
    ActWheelPostH(120, 3);               /* 横向倍数 3 -> 注入 3 格 */
    Pump(100); Drain();
    CHECK(g_whInj == 3, "Q: 横向倍数 3 -> 注入 3 格");
    CHECK(g_wheelInj == 0, "Q: 横向不会误发到纵轴");

    ResetWheelRec();
    ActWheelPostH(-120, 2);              /* 方向原样带过去 */
    Pump(100); Drain();
    CHECK(g_whInj == 2, "Q: 横向反向同样注入 2 格");

    ResetWheelRec();
    ActWheelPostH(120, 0);               /* 无效参数 */
    ActWheelPostH(0, 3);
    Pump(100); Drain();
    CHECK(g_whInj == 0, "Q: 倍数 0 或 delta=0 -> 一格都不注入");

    /* ---- 场景O：超限"改道"的落点校验契约 ----
       背景（QQ 实测）：MaxArrowSteps 超限后 SendArrows 改发原生 Ctrl+方向，
       但改道的落点由**目标程序自己的词切分**决定 —— QQ/Chromium 按 ICU
       词典切、1~2 字一步，我们按"连续汉字段"整段算（一次 10 字），两者不一致
       时改道会把光标丢在整段中间（日志实例：期望 82，改道后停在 92，再按一次
       停在 90）。修法：SendArrows 把"本次是改道"返回给调用方，nav.c 对改道这
       一次也回读落点并按差额补足；补正自己走 SendArrowsStrict，绝不再改道。
       真实落点要有真编辑器才能验；这里验的是发送层契约（可确定性断言）：
       哪种情形返回哪个模式、注入的键序列对不对。 */
    WriteChordIni("[General]\r\nMaxArrowSteps = 3\r\nOverStepMode = 1\r\n");
    ConfigLoadFrom(L"bin/chordtest.ini");

    ResetRec();
    int mode = SendArrows(DIR_LEFT, 5, FALSE);     /* 5 >= 3：超限 */
    Pump(30);
    CHECK(mode == ARROW_SEND_HANDOFF, "O: 超限+OverStepMode=1 -> 返回\"已改道\"");
    CHECK(CountVk(VK_LCONTROL) == 1 && CountVk(VK_LEFT) == 1 && CountVk(VK_RIGHT) == 0,
          "O: 改道发的是 Ctrl+方向（一支，不逐格）");
    CHECK(SeqIdx(VK_LCONTROL, 0) < SeqIdx(VK_LEFT, 0)
          && SeqIdx(VK_LEFT, 1) < SeqIdx(VK_LCONTROL, 1),
          "O: Ctrl 先按后抬（夹住方向键）");

    ResetRec();
    mode = SendArrows(DIR_LEFT, 2, FALSE);         /* 2 < 3：不超限 */
    Pump(30);
    CHECK(mode == ARROW_SEND_STEPS && CountVk(VK_LEFT) == 2 && CountVk(VK_LCONTROL) == 0,
          "O: 未超限 -> 逐格 2 步、不带 Ctrl");

    ResetRec();
    SendArrowsStrict(DIR_LEFT, 5, FALSE);          /* 补正：即使超限也必须逐格 */
    Pump(30);
    CHECK(CountVk(VK_LEFT) == 5 && CountVk(VK_LCONTROL) == 0,
          "O: 补正路径不因超限再改道（逐格 5 步、无 Ctrl）");

    WriteChordIni("[General]\r\nMaxArrowSteps = 3\r\nOverStepMode = 0\r\n");
    ConfigLoadFrom(L"bin/chordtest.ini");
    ResetRec();
    mode = SendArrows(DIR_LEFT, 5, FALSE);
    Pump(30);
    CHECK(mode == ARROW_SEND_ONE && CountVk(VK_LEFT) == 1 && CountVk(VK_LCONTROL) == 0,
          "O: OverStepMode=0 -> 只发一格，返回\"不补正\"");

    /* 复位成缺省：本场景是最后一个，但别给以后新加的场景留坑
       （SendArrows 的行为受 MaxArrowSteps/OverStepMode 影响） */
    ConfigLoadFrom(L"bin/__absent__.ini");         /* 文件不存在 -> 全部回缺省 */

    /* ---- 场景R：Caps+增强修饰+滚轮 = 持续滚动 ----
       "滚一下就一直滚、继续同向滚提速、反向滚/松开修饰/松开 C 立即停"，
       速度两端 = AutoScrollInterval（起步）/ AutoScrollFastMs（上限），
       **各自独立配置**（不再跟倍数项 WheelScrollMul 共用任何值）。
       状态机（active/dir/gear）由主线程在锁内维护，可确定性断言；
       "真的在注入"由滚轮计数钩子观测（方向要与 dir 一致）。
       计数钩子只数**我们注入的**滚轮（LLMHF_INJECTED），与场景Q/T 共用，
       所以每段观测前先 ResetWheelRec。 */
    Reset();
    int base = ConfigGetAutoScrollInterval();     /* 最慢档 = 1500 */
    int fast = ConfigGetAutoScrollFastMs();        /* 最快档 = 800（独立配置项） */
    CHECK(fast == 800, "R: 最快档 = AutoScrollFastMs 缺省 800ms/格");
    CHECK(ConfigGetWheelScrollMul() == 3,
          "R: 倍数项与水平无关（WheelScrollMul 仍是 3，两边各自独立）");
    CHECK(ActAutoScrollTick() == 0, "R: 未启动时速度为 0");
    CHECK(!ActAutoScrollActive(), "R: 初始未在连续滚动");

    Key(CLI_CAPS, TRUE); Pump(2);
    KeyAlt(VK_LMENU, TRUE); Pump(2);              /* 默认增强键：左 Alt */
    CHECK(ModeDown(MODE_ENHANCE), "R: 按住 Alt -> 增强模式生效");

    CHECK(base == 1500, "R: 初始速度缺省 1500ms/格（约 2 格/秒）");
    CHECK(fast < base, "R: 最快档的毫秒数必须比初始档小（= 更快，阶梯方向正确）");
    CHECK(!ActAutoScrollNoSmooth(), "R: 平滑注入缺省开启（未命中 NoSmoothScroll 名单）");

    ActAutoScrollWheel(120);               /* 第一滚：增强档、最慢档、向上 */
    CHECK(ActAutoScrollActive() &&
          ActAutoScrollDir() == 1 && ActAutoScrollGear() == 1,
          "R: 从最慢档启动（档 1 / 向上）");
    CHECK(ActAutoScrollTick() == base, "R: 第 1 档速度 = AutoScrollInterval");

    /* 平滑注入：把一格拆成小数刻度、发得更密，但**速度不变**。
       最慢档 1500ms/格 -> delta = round(120*40/1500) = 3（仍是小数刻度）。 */
    CHECK(ActAutoScrollDelta() == 3,
          "R: 最慢档拆成小数刻度（delta=round(120*40/1500)=3，仍是一格的零头）");
    CHECK(ActAutoScrollBeat() < ActAutoScrollTick(),
          "R: 平滑模式节拍更短（注入更密）");
    {
        /* 速度等价：delta/beat 应等于 120/每格时长。delta 是整数刻度，必然
           有四舍五入，容差取半个节拍（|d - x| <= 0.5  =>  |d*tick - 120*beat| <= tick/2）。 */
        int lhs = ActAutoScrollDelta() * ActAutoScrollTick();
        int rhs = WHEEL_STD_DELTA * ActAutoScrollBeat();
        int tol = ActAutoScrollTick() / 2 + 1;
        CHECK(lhs >= rhs - tol && lhs <= rhs + tol,
              "R: 平滑前后速度等价（与整格模式一致，仅整数刻度舍入误差）");
    }

    /* 逐档提速：中间档不止两档，速度单调变快（每格时长单调变短）。
       注意从"最慢"到"最快"是变快，所以节拍（ms/格）应当**变短**。 */
    int t1 = ActAutoScrollTick();
    ActAutoScrollWheel(120);
    int t2 = ActAutoScrollTick();
    int g2 = ActAutoScrollGear();
    CHECK(g2 == 2 && t2 != t1, "R: 同向再滚 -> 进第 2 档（速度变化）");
    ActAutoScrollWheel(120);
    int t3 = ActAutoScrollTick();
    CHECK(ActAutoScrollGear() == 3 && t3 != t2, "R: 再滚 -> 第 3 档（中间档不止两档）");
    CHECK(t3 < t1, "R: 档位越高节拍越短（越快）");

    /* 到顶：速度恰好等于 WheelScrollMs，**不会超过**（配置上限） */
    for (int i = 0; i < 10; i++) ActAutoScrollWheel(120);
    int tTop = ActAutoScrollTick(), gTop = ActAutoScrollGear();
    ActAutoScrollWheel(120);
    CHECK(ActAutoScrollGear() == gTop && ActAutoScrollTick() == tTop,
          "R: 到顶档后不再提速（档位饱和）");
    CHECK(tTop == fast, "R: 顶档速度 == AutoScrollFastMs = 800ms/格");
    CHECK(tTop <= base, "R: 最快档不会比初始档还慢（阶梯方向正确）");
    CHECK(ActAutoScrollDelta() > 0 && ActAutoScrollDelta() < 120,
          "R: 顶档仍是小数刻度（平滑提速靠加 delta，不是加格数）");

    /* 命中 [NoSmoothScroll] -> 只发整格：delta 恒为 120、节拍就是速度档。
       这是"某些程序不累积小数刻度（滚了不动）"的兜底，判据是**名单**而不是
       全局开关，所以 ActAutoScrollSetNoSmooth 就是它的可观测入口。 */
    ActAutoScrollSetNoSmooth(TRUE);
    CHECK(ActAutoScrollNoSmooth(), "R: 名单命中 -> 整格模式生效");
    CHECK(ActAutoScrollDelta() == WHEEL_STD_DELTA,
          "R: 整格模式每拍发整格 120（兼容不累积小数刻度的程序）");
    CHECK(ActAutoScrollBeat() == ActAutoScrollTick(),
          "R: 整格模式节拍 = 速度档");
    ActAutoScrollSetNoSmooth(FALSE);
    CHECK(!ActAutoScrollNoSmooth() && ActAutoScrollDelta() < WHEEL_STD_DELTA,
          "R: 关掉后平滑重新开启（delta 回到不足一格）");

    ResetWheelRec();
    Pump(300);                                    /* 顶档 1000ms/格，300ms 内至少 1 拍 */
    CHECK(g_wheelInj >= 1 && g_wheelSum > 0, "R: 连续滚动确实在持续注入上滚");
    CHECK(ActAutoScrollDelta() > 0 && ActAutoScrollDelta() < 120,
          "R: 顶档也是小数刻度（平滑提速靠加 delta，不是加格数）");
    CHECK(ActAutoScrollBeat() < ActAutoScrollTick(),
          "R: 平滑节拍仍短于每格时长（细水长流而非一次一大格）");

    ActAutoScrollWheel(-120);              /* 反向：立即终止 + 清零速度 */
    CHECK(!ActAutoScrollActive() && ActAutoScrollDir() == 0 && ActAutoScrollGear() == 0,
          "R: 反向滚动 -> 立即终止并清零档位");

    /* 反向打断后再滚：必须从**最慢档**重来，不残留打断前的速度 */
    ActAutoScrollWheel(120);
    CHECK(ActAutoScrollActive() && ActAutoScrollGear() == 1 && ActAutoScrollTick() == base,
          "R: 反向打断后重置速度 -> 再滚从第 1 档（最慢）重来");
    ActAutoScrollWheel(-120);              /* 收起，免得影响后面的用例 */

    ResetWheelRec();
    ActAutoScrollWheel(-120);              /* 下滚启动（方向 -1） */
    CHECK(ActAutoScrollActive() && ActAutoScrollDir() == -1, "R: 下滚启动（方向 -1）");
    CHECK(ActAutoScrollDelta() < 0, "R: 下滚时 delta 为负（带方向）");
    Pump(300);
    CHECK(g_wheelSum < 0, "R: 下滚方向正确（与上方上滚大致抵消）");

    /* ---- 终止规则（2026-09-11 改版）：**松手不再打断** ----
       设计意图是"滚一下就能松手读，想停的时候按一下键"，于是：
         松开 Alt/D、松开 Caps -> 继续滚；
         按任意键 / 鼠标按键按下 / 不带和弦的滚轮 -> 终止；
         Caps+Alt（增强修饰）+滚轮 -> 那是控制杆（调速），**不**终止；
         超过上限 -> 自行终止（避免"人走开了还在滚"）。 */
    KeyAlt(VK_LMENU, FALSE); Pump(2); Sink();
    CHECK(ActAutoScrollActive(), "R: 松开 Alt -> 继续滚动（不再终止）");

    /* 但"按任意键"包括我们自己的激活键：重新按下 Alt 就是一次真实按键 -> 终止 */
    KeyAlt(VK_LMENU, TRUE); Pump(2); Sink();
    CHECK(!ActAutoScrollActive(), "R: 重新按下 Alt（也算任意键）-> 终止");

    /* 再启动一次，验证"松开 Caps"同样不终止 */
    ActAutoScrollWheel(120);
    CHECK(ActAutoScrollActive(), "R: 重新启动成功");
    Key(CLI_CAPS, FALSE); Pump(2); Sink();
    CHECK(ActAutoScrollActive(), "R: 松开 Caps -> 继续滚动（不再终止）");

    /* 按任意键（普通字母）-> 终止 */
    Key('A', TRUE); Pump(2); Sink();
    CHECK(!ActAutoScrollActive(), "R: 按任意键 -> 终止连续滚动");
    Key('A', FALSE); Pump(2);

    /* 鼠标按键按下 -> 终止 */
    Key(CLI_CAPS, TRUE); Pump(2); KeyAlt(VK_LMENU, TRUE); Pump(2);
    ActAutoScrollWheel(120);
    Sink();
    CHECK(ActAutoScrollActive(), "R: 鼠标场景已重新启动");
    /* "鼠标按下"的样本点取**任务栏**：它在默认拖动黑名单里（不接管），所以这条
       用例只验"鼠标按下打断持续滚动"本身 —— 不会顺带启动一次拖拽，也不受
       桌面上恰好压着哪个窗口的影响（(0,0) 处有什么是随环境变的）。 */
    HWND trayWnd = FindWindowW(L"Shell_TrayWnd", NULL);
    RECT trayRc = { 0, 0, 0, 0 };
    if (trayWnd) GetWindowRect(trayWnd, &trayRc);
    POINT tbPt = { (trayRc.left + trayRc.right) / 2, (trayRc.top + trayRc.bottom) / 2 };
    MousePt(WM_LBUTTONDOWN, tbPt);
    CHECK(!ActAutoScrollActive(), "R: 鼠标按键按下 -> 终止连续滚动");
    MousePt(WM_LBUTTONUP, tbPt);

    /* 鼠标移动 -> **不**终止（移动不是"按键"，否则手一抖就停） */
    ActAutoScrollWheel(120);
    CHECK(ActAutoScrollActive(), "R: 移动场景已启动");
    MouseEv(WM_MOUSEMOVE, 0);
    CHECK(ActAutoScrollActive(), "R: 鼠标移动 -> 不终止");

    /* 带着和弦（Caps+Alt）的滚轮 = 控制杆，只调速、不终止 */
    CHECK(Wheel(120, 0) == 1, "R: 控制杆那一格仍被吞掉（滚轮是控制杆）");
    Sink();                                    /* 让 WM_WHEEL_ENH 走完（同向 -> 进档） */
    CHECK(ActAutoScrollActive(), "R: Caps+Alt+滚轮（控制杆）-> 不终止（只调速）");

    /* 不带和弦的普通滚轮 -> 终止（"没按住时直接打断"） */
    Key(CLI_CAPS, FALSE); Pump(2); KeyAlt(VK_LMENU, FALSE); Pump(2); Sink();
    CHECK(ActAutoScrollActive(), "R: 松手后仍在滚");
    CHECK(Wheel(120, 0) == 0, "R: 不带和弦的滚轮 -> 放行（只剩持续滚动还在跑）");
    CHECK(!ActAutoScrollActive(), "R: 不带和弦的滚轮 -> 终止连续滚动");

    /* ---- 第二版（2026-09-11）补充："滚一下就停"的两个来源 ----
       ① **横向轴**：触控板双指滑动会同时报纵/横两个轴，倾斜滚轮也报横轴。
          它们过去不在控制杆分支里，一路落到"新输入 -> 打断"，于是刚启动的
          持续滚动被同一只手当场掐掉；
       ② **系统自动重复**：被按住的键（含修饰键）会持续发 keydown，把它当成
          "新输入"就等于"滚一格就被自己按住的那个键打断"。
       下面两组都覆盖：和弦按着 -> 整只手势都不打断；真的新按下 -> 打断。 */
    ActAutoScrollWheel(120);
    CHECK(ActAutoScrollActive(), "R: 无和弦状态下重新启动");
    CHECK(WheelH(120, 0) == 0, "R: 不带和弦的横轴滚轮 -> 放行");
    CHECK(!ActAutoScrollActive(), "R: 不带和弦的横轴滚轮 -> 打断（横向也是新输入）");

    Key(CLI_CAPS, TRUE); Pump(2); KeyAlt(VK_LMENU, TRUE); Pump(2); Sink();
    ActAutoScrollWheel(120);
    CHECK(ActAutoScrollActive(), "R: 和弦按上并启动");
    CHECK(WheelH(30, 0) == 1, "R: 和弦按着时的横轴滚轮也被吞掉（同一个手势）");
    CHECK(ActAutoScrollActive(), "R: 和弦按着时的横轴滚轮 -> 不打断");
    CHECK(Wheel(0, 0) == 1, "R: delta=0 的滚轮（占位帧）被吞掉");
    CHECK(ActAutoScrollActive(),
          "R: delta=0 的滚轮 -> 不打断（不带输入信息，不该把滚动停掉）");

    /* 先按住一个普通键（**新按下**算新输入 -> 中断），再启动；之后同一个键的
       keydown 都是系统自动重复（没有抬起），必须**不**打断。 */
    Key('Q', TRUE); Pump(2); Sink();
    CHECK(!ActAutoScrollActive(), "R: 新按下一个键 -> 打断（原语义不变）");
    ActAutoScrollWheel(120);
    CHECK(ActAutoScrollActive(), "R: 键还按着时也能启动持续滚动");
    Key('Q', TRUE); Pump(2); Sink();              /* 自动重复：中间没有抬起 */
    CHECK(ActAutoScrollActive(), "R: 系统自动重复 -> 不打断（重复不是新输入）");
    Key('Q', FALSE); Pump(2); Sink();
    Key('Q', TRUE); Pump(2); Sink();              /* 抬起后再按下 = 真的新输入 */
    CHECK(!ActAutoScrollActive(), "R: 抬起后再按下 -> 打断");
    Key('Q', FALSE); Pump(2); Sink();

    /* ---- 日志契约：诊断日志**只在 C+` 记录模式下生成** ----
       持续滚动的终止原因也走这条约定（它是查"滚一下就停"的唯一现场，但一样
       不该在没开记录时写文件）。判据用文件大小：不开记录 -> 一个字节都不动。
       正向（开了记录确实写得进）由 logsmoke 与其它记录模式用例覆盖，
       这里只钉死"没开就不许写"。 */
    {
        LONGLONG before = LogSize();
        g_recording.store(false);
        ActAutoScrollWheel(120);
        ActAutoScrollInterrupt("测试_未开记录", 0);
        CHECK(LogSize() == before,
              "R: 未开记录 -> 终止不写日志（日志只在记录模式生成）");
    }

    /* 超时 -> 自行终止。这里把上限缩短到 250ms，免得真等 10 分钟；
       注意要**先设上限再启动**，因为超时时刻是在启动/调速时算出来的。 */
    ActAutoScrollTestSetMax(250);
    Key(CLI_CAPS, TRUE); Pump(2); KeyAlt(VK_LMENU, TRUE); Pump(2); Sink();
    ActAutoScrollWheel(120);
    CHECK(ActAutoScrollActive() && ActAutoScrollRemainMs() > 0,
          "R: 超时场景已启动（剩余时间 > 0）");
    Pump(700);                                 /* 泵消息：让工作线程自己走到超时 */
    CHECK(!ActAutoScrollActive(), "R: 超过上限 -> 自动终止（不会永远滚）");
    CHECK(ActAutoScrollRemainMs() == 0, "R: 终止后剩余超时为 0");
    ActAutoScrollTestSetMax(0);                /* 回缺省 10 分钟 */
    Key(CLI_CAPS, FALSE); Pump(2); KeyAlt(VK_LMENU, FALSE); Pump(2); Sink();

    /* 暂停映射 -> 必须停掉持续滚动。
       暂停期间钩子整段被跳过（连"按任意键打断"都收不到），所以这是一条
       **只能靠显式调用**兜住的生命周期路径：漏了它就是"往别的程序里一直滚、
       谁也停不下来"。这里直接调菜单那条路径（ApplyPauseState）。 */
    ActAutoScrollTestSetMax(0);
    ActAutoScrollWheel(120);
    CHECK(ActAutoScrollActive(), "R: 暂停前已启动持续滚动");
    ApplyPauseState();                             /* 托盘"暂停映射" */
    CHECK(!ActAutoScrollActive(), "R: 暂停映射 -> 终止连续滚动");
    ApplyPauseState();                             /* 恢复映射 */
    CHECK(!g_state.paused && !ActAutoScrollActive(), "R: 恢复映射后仍不在滚动");

    /* 两端配成相等 -> 阶梯退化为恒定速度（而不是出错或反向提速） */
    WriteChordIni("[General]\r\nAutoScrollInterval = 1000\r\n"
                  "AutoScrollFastMs = 1000\r\n");
    ConfigLoadFrom(L"bin/chordtest.ini");
    ActAutoScrollWheel(120);
    CHECK(ActAutoScrollTick() == 1000, "R: 两端配成相等 -> 速度恒定 1000ms/格");

    /* 端到端：钩子路由与吞格（横向 / 持续滚动都吞，只有放大档放行） */
    ConfigLoadFrom(L"bin/__absent__.ini");        /* 回缺省 */
    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    KeyAlt(VK_LMENU, TRUE); Pump(2);
    Sink();                                       /* 冲掉可能残留的消息 */
    ResetWheelRec();
    CHECK(Wheel(120, 0) == 1, "R: 持续滚动档 -> 物理格被完全拦截（滚轮是控制杆）");
    Pump(300);                                    /* 泵消息：驱动状态机并注入 */
    CHECK(ActAutoScrollActive() && ActAutoScrollDir() == 1 && g_wheelInj >= 1,
          "R: 经钩子路由 -> 启动持续滚动");
    END_SCENE();
    ConfigLoadFrom(L"bin/__absent__.ini");

    /* ---- 场景T：Caps+Shift+滚轮 = 横向滚动 ----
       只需真实 Shift（与 keymap 的"选区模式 F"无关）；速度 WheelHScrollMul 默认 1。
       轴向优先级高于纵向两路：Shift 排最前。
       方向照抄系统约定：滚轮**向上 = 向左**（浏览器 / 资源管理器 / 表格都是这个
       方向，Firefox 官方快捷键表就写着 Shift+上滚 = 向左）。 */
    Reset();
    CHECK(ConfigGetWheelHScrollMul() == 1, "T: WheelHScrollMul 缺省为 1（一滚一格）");

    Key(CLI_CAPS, TRUE); Pump(2);
    Key(VK_LSHIFT, TRUE); Pump(2);                /* 真实左 Shift */
    Sink();

    int hd = 0, hm = 0;
    LRESULT consumed = Wheel(120, 0);
    CHECK(consumed == 1, "T: Caps+Shift+滚轮 -> 钩子吞掉物理这一格（防横纵混滚）");
    CHECK(TakeWheelHAccel(&hd, &hm), "T: 投递横向滚动请求");
    CHECK(hd == 120 && hm == 1, "T: 请求带原始方向与倍数 1");
    CHECK(!TakeWheelAccel(NULL, NULL), "T: 不走纵向放大通道（轴向互斥）");
    CHECK(!TakeWheelEnh(NULL), "T: 也不启动持续滚动（按的是 Shift）");
    CHECK(!ActAutoScrollActive(), "T: 确实没有启动持续滚动");

    /* 方向：滚轮向上(delta>0) = 向左(负) —— 与系统 Shift+滚轮一致 */
    ResetWheelRec();
    ActWheelPostH(120, 1);
    Pump(100); Drain();
    CHECK(g_whSum < 0, "T: 滚轮向上 -> 向左（负 delta，同浏览器 Shift+上滚）");
    ResetWheelRec();
    ActWheelPostH(-120, 1);
    Pump(100); Drain();
    CHECK(g_whSum > 0, "T: 滚轮向下 -> 向右（正 delta）");

    /* 倍数可配：3 -> 钩子仍然只投递一条请求（由工作线程注入 3 格） */
    WriteChordIni("[General]\r\nWheelHScrollMul = 3\r\n");
    ConfigLoadFrom(L"bin/chordtest.ini");
    Sink();
    int hd3 = 0, hm3 = 0;
    CHECK(Wheel(120, 0) == 1 && TakeWheelHAccel(&hd3, &hm3) && hm3 == 3,
          "T: WheelHScrollMul=3 -> 请求倍数 3");
    ResetWheelRec();
    ActWheelPostH(120, 3);
    Pump(100); Drain();
    CHECK(g_whInj == 3 && g_whSum < 0, "T: 倍数 3 -> 注入 3 格、方向仍向左");
    ConfigLoadFrom(L"bin/__absent__.ini");        /* 回缺省（1） */

    /* 关闭（0）：不吞这一格，退回纵向语义 */
    WriteChordIni("[General]\r\nWheelHScrollMul = 0\r\n");
    ConfigLoadFrom(L"bin/chordtest.ini");
    Key(VK_LSHIFT, FALSE); Pump(2); Sink();
    Key(VK_LSHIFT, TRUE);  Pump(2); Sink();
    ResetWheelRec();
    CHECK(Wheel(120, 0) != 1, "T: WheelHScrollMul=0 -> 改由纵向接管（放大档，不吞物理格）");
    CHECK(!TakeWheelHAccel(NULL, NULL), "T: 关闭时不再投递横向请求");
    CHECK(TakeWheelAccel(NULL, NULL), "T: 关闭横向后退回纵向放大");
    ConfigLoadFrom(L"bin/__absent__.ini");

    /* 松开 Shift 就回到纵向；Caps+Shift 之外不受影响 */
    Key(VK_LSHIFT, FALSE); Pump(2); Sink();
    CHECK(Wheel(120, 0) != 1, "T: 松开 Shift 后退回纵向放大（不吞物理格）");
    int dS = 0, mS = 0;
    CHECK(TakeWheelAccel(&dS, &mS) && mS == 3, "T: 松开 Shift -> 纵向放大（缺省 3 倍）");
    CHECK(!TakeWheelHAccel(NULL, NULL), "T: 松开 Shift 后不再横向");

    /* 真 Shift 的按住状态必须被正确清掉（否则会粘住） */
    CHECK(!g_shiftMod, "T: 松开 Shift 后 g_shiftMod 归 FALSE");
    ModHoldReset();                               /* 暂停/重启路径 */
    CHECK(!g_shiftMod && !g_cMod, "T: ModHoldReset 清掉按住状态");

    Key(CLI_CAPS, TRUE); Pump(2);                 /* 用 Ctrl 也一样不该横向 */
    Key(VK_LCONTROL, TRUE); Pump(2); Sink();
    CHECK(Wheel(120, 0) != 1 && TakeWheelAccel(NULL, NULL),
          "T: Caps+Ctrl+滚轮 仍是纵向（只有 Shift 决定轴向）");
    CHECK(!TakeWheelHAccel(NULL, NULL), "T: Caps+Ctrl+滚轮 不横向");
    Key(VK_LCONTROL, FALSE); Pump(2);
    END_SCENE();

    /* ---- 场景S：EnhanceKeys / ShiftKeys 已**不再读配置** ----
       "配置项中，去掉 EnhanceKeys = Alt, D 与 ShiftKeys = F，没必要读取配置"
       —— 激活键改成编译期常量（cap.h 的 MODE_*_KEYS_DEFAULT）：配置文件里
       即使写了这两项也一律忽略（解析器不认这两个键名，KeyMapInit 也不读）。
       这么做之所以安全，是因为"模式"这层抽象还在 —— 引擎只问"增强模式生效
       吗"，从不判断按的是哪个键；以后真要改/删激活键，只改常量一处即可。 */
    WriteChordIni("[General]\r\nEnhanceKeys = X\r\nShiftKeys = Y\r\n");
    ConfigLoadFrom(L"bin/chordtest.ini");
    KeyMapInit();
    CHECK(ModeFindByVk(VK_LMENU) == MODE_ENHANCE && ModeFindByVk('D') == MODE_ENHANCE,
          "S: 配置里的 EnhanceKeys 被忽略 —— 增强键仍是固定的 Alt + D");
    CHECK(ModeFindByVk('F') == MODE_SHIFT, "S: 选区键仍是固定的 F");
    CHECK(ModeFindByVk('X') == -1 && ModeFindByVk('Y') == -1,
          "S: 配置里写的 X / Y 不会变成激活键");

    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    KeyAlt(VK_LMENU, TRUE); Pump(2);              /* 固定的增强键：Alt */
    Key('J', TRUE);  Pump(2);
    Key('J', FALSE); Pump(2);                     /* 短点：增强模式 -> 补齐到 3 次 */
    KeyAlt(VK_LMENU, FALSE); Pump(2);
    END_SCENE();
    CHECK(CountVk(VK_LEFT) == 3, "S: 增强键仍是 Alt+D：C+Alt+J 补齐到 3 次左键");

    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);
    Key('D', TRUE); Pump(2);                      /* D 同样是固定的增强键 */
    Key('J', TRUE);  Pump(2);
    Key('J', FALSE); Pump(2);
    Key('D', FALSE); Pump(2);
    END_SCENE();
    CHECK(CountVk(VK_LEFT) == 3, "S: C+D+J 同样补齐到 3 次左键（D 仍是增强键）");

    /* 复位默认键位（别给以后新增的场景留坑） */
    ConfigLoadFrom(L"bin/__absent__.ini");
    KeyMapInit();

    /* ---- 场景AD：[AutoDisable] 名单（自动屏蔽） ----
       名单里的程序在**前台**时，Caps 开头的和弦整段不介入（hook.c 的
       AutoDisableHit 走"反着按"那条已存在的不介入路径，吞/放天然对称，
       不会造出"down 放行、up 被吞"的卡键）。
       这里测的是那条**查询**本身：用一个本进程拥有的窗口代入"前台窗口"，
       不依赖真实焦点 —— CI 的非交互会话里 SetForegroundWindow 不一定成功。 */
    {
        HWND me = CreateWindowExW(0, L"STATIC", L"adprobe", 0,
                                  0, 0, 1, 1, NULL, NULL, NULL, NULL);
        CHECK(me != NULL, "AD: 拿到本进程的窗口（代入前台窗口）");
        if (me) {
            WriteChordIni("[AutoDisable]\r\ntest_chord.exe\r\n");
            ConfigLoadFrom(L"bin/chordtest.ini");
            CHECK(ConfigIsAutoDisabled(me),
                  "AD: 该窗口的进程在名单里 -> 命中（它到前台时整段不介入）");
            CHECK(!ConfigIsAutoDisabled(NULL), "AD: 没有窗口 -> 不命中（NULL 防御）");
            CHECK(!ConfigIsNativeWordJump(me),
                  "AD: AutoDisable 名单不会连带命中 NativeWordJump（各名单各自独立）");
            WriteChordIni("[AutoDisable]\r\notherapp.exe\r\n");
            ConfigLoadFrom(L"bin/chordtest.ini");
            CHECK(!ConfigIsAutoDisabled(me), "AD: 名单里没有本进程 -> 不命中");
            DestroyWindow(me);
        }
        ConfigLoadFrom(L"bin/__absent__.ini");   /* 复位：别影响后面的场景 */
    }

    /* ---- 场景AG：单键切换输入法的"单击"判定（执行在 src/ime.c） ----
       判定在 hook.c，干活在 ime.c 的**专用线程**上 —— 本测试桩掉了后者（真实现
       要向别的进程的 IME 窗口发 3 条消息，既不该在单测里做，也会让用例依赖机器
       上装了什么输入法）。这里验的是"什么算单击"：
         · Caps 是和弦首键 -> 松手时本次会话没触发过任何动作即为单击（无时长上限）；
         · Shift 是**真修饰键**（系统照常用它）-> 必须"期间没按别的键"**且**
           按得够短（IME_TAP_MS）才算单击；
         · Caps 按住期间的 Shift 属于和弦（Caps+Shift+滚轮要横向），不算单击。 */
    ConfigLoadFrom(L"bin/__absent__.ini");
    CHECK(ConfigGetImeMode(IME_KEY_CAPS) == IME_MODE_CN &&
          ConfigGetImeMode(IME_KEY_LSHIFT) == IME_MODE_EN &&
          ConfigGetImeMode(IME_KEY_RSHIFT) == IME_MODE_CN,
          "AG: 缺省 Caps=中文 / 左Shift=英文 / 右Shift=中文");

    Reset(); ResetImeRec();
    Key(CLI_CAPS, TRUE); Pump(2);
    Key(CLI_CAPS, FALSE); Pump(2);
    CHECK(g_imeReq[IME_KEY_CAPS] == 1, "AG: 单击 Caps -> 投递一次切换请求");

    Reset(); ResetImeRec();
    Key(CLI_CAPS, TRUE); Pump(2);
    Key('J', TRUE);  Pump(2);
    Key('J', FALSE); Pump(2);
    Key(CLI_CAPS, FALSE); Pump(2);
    CHECK(g_imeReq[IME_KEY_CAPS] == 0, "AG: Caps+J 是和弦，松手不切输入法");
    Drain();

    Reset(); ResetImeRec();
    Key(VK_LSHIFT, TRUE);  Pump(2);
    Key(VK_LSHIFT, FALSE); Pump(2);
    CHECK(g_imeReq[IME_KEY_LSHIFT] == 1, "AG: 单击左 Shift -> 投递一次");

    Reset(); ResetImeRec();
    Key(VK_RSHIFT, TRUE);  Pump(2);
    Key(VK_RSHIFT, FALSE); Pump(2);
    CHECK(g_imeReq[IME_KEY_RSHIFT] == 1, "AG: 单击右 Shift -> 投递一次");

    Reset(); ResetImeRec();
    Key(VK_LSHIFT, TRUE); Pump(2);
    Key('A', TRUE);  Pump(2);
    Key('A', FALSE); Pump(2);
    Key(VK_LSHIFT, FALSE); Pump(2);
    CHECK(g_imeReq[IME_KEY_LSHIFT] == 0,
          "AG: Shift+A 不是单击 Shift（期间按了别的键）");

    Reset(); ResetImeRec();
    Key(VK_LSHIFT, TRUE); Pump(IME_TAP_MS + 80);   /* 按住超过"单击"时长上限 */
    Key(VK_LSHIFT, FALSE); Pump(2);
    CHECK(g_imeReq[IME_KEY_LSHIFT] == 0,
          "AG: Shift 按住超过 IME_TAP_MS -> 当修饰键用，不切");

    Reset(); ResetImeRec();
    Key(CLI_CAPS, TRUE); Pump(2);
    Key(VK_LSHIFT, TRUE);  Pump(2);
    Key(VK_LSHIFT, FALSE); Pump(2);
    Key(CLI_CAPS, FALSE); Pump(2);
    CHECK(g_imeReq[IME_KEY_LSHIFT] == 0,
          "AG: Caps 按住期间的 Shift 属于和弦，不算单击");
    CHECK(g_imeReq[IME_KEY_CAPS] == 1,
          "AG: 但 Caps 这一按没触发任何动作 -> 松手仍算单击 Caps");
    Drain();

    Reset(); ResetImeRec();
    Key(VK_LSHIFT, TRUE);  Pump(2);
    Key(VK_RSHIFT, TRUE);  Pump(2);        /* 两个 Shift 同时按 = 组合 */
    Key(VK_RSHIFT, FALSE); Pump(2);
    Key(VK_LSHIFT, FALSE); Pump(2);
    CHECK(g_imeReq[IME_KEY_LSHIFT] == 0 && g_imeReq[IME_KEY_RSHIFT] == 0,
          "AG: 两个 Shift 同时按 -> 不判定");

    /* 配置里关掉 -> 一次都不投递（钩子路径上连配置查询都不做） */
    WriteChordIni("[General]\r\nImeCapsMode = 0\r\nImeLShiftMode = 0\r\nImeRShiftMode = 0\r\n");
    ConfigLoadFrom(L"bin/chordtest.ini");
    Reset(); ResetImeRec();
    Key(CLI_CAPS, TRUE);   Pump(2);
    Key(CLI_CAPS, FALSE);  Pump(2);
    Key(VK_LSHIFT, TRUE);  Pump(2);
    Key(VK_LSHIFT, FALSE); Pump(2);
    CHECK(g_imeReq[IME_KEY_CAPS] == 0 && g_imeReq[IME_KEY_LSHIFT] == 0,
          "AG: 三个键都设成 0（不处理）-> 不投递任何请求");
    ConfigLoadFrom(L"bin/__absent__.ini");   /* 复位：别影响后面的场景 */

    /* ---- 场景W：**先按修饰键、后按 C**（P1-1 回归） ----
       实测缺陷：按住 Alt 不放、再按 C、然后点按 J，**只发 1 次左键**（本该
       补齐到 3 次）。根因是当时并行维护了两份修饰状态：一份"此刻按住"
       （downMask），一份"本次和弦内按下过"的门闩（pendMask）。两者本来就
       同步置位、同步清除（恒等），偏偏引擎复位（按 C）会**单独清掉门闩**
       —— 而那一刻 Alt 已经按着、之后不再产生 keydown，于是本次 C 会话内
       增强模式永远不成立。
       现在只保留"此刻按住"一份，按 C 不再动它（见 keymap.c 的 ModeKeyEvent
       与 main.c 的 SnapMods）。顺带把滚轮与点按的修饰判定统一到同一个查询 ——
       否则会出现"滚轮算增强、点按不算"的分裂。
       下面两段是同一件事的两个顺序，必须都绿：前者是刚修的缺陷，后者是
       一直正常的老路径，防止修前面时把后面改坏。 */
    Reset();
    KeyAlt(VK_LMENU, TRUE); Pump(2);      /* 先按修饰键 */
    Key(CLI_CAPS, TRUE);    Pump(2);      /* 后按 C：关键就是这个顺序 */
    Key('J', TRUE);  Pump(2);
    Key('J', FALSE); Pump(2);             /* 短点 -> 应补齐到 3 次 */
    KeyAlt(VK_LMENU, FALSE); Pump(2);
    END_SCENE();
    CHECK(CountVk(VK_LEFT) == 3,
          "W: 先按 Alt 再按 C，点按 J 仍补齐 3 次（P1-1：修饰状态只有一份）");

    Reset();
    Key(CLI_CAPS, TRUE); Pump(2);         /* 老路径：先 C 后 Alt */
    KeyAlt(VK_LMENU, TRUE); Pump(2);
    Key('J', TRUE);  Pump(2);
    Key('J', FALSE); Pump(2);
    KeyAlt(VK_LMENU, FALSE); Pump(2);
    END_SCENE();
    CHECK(CountVk(VK_LEFT) == 3,
          "W: 先按 C 再按 Alt 同样补齐 3 次（老路径未回归）");

    /* ---- 场景V：横向滚动的**注入方向**（与系统约定一致） ----
       "横向滚动的方向反了" —— 修好之后：滚轮**向上** = 向左（负 delta），
       向下 = 向右（正 delta）。这不是随便定的：`Shift + 滚轮` 在浏览器 /
       资源管理器 / 表格里就是这个方向（Firefox 官方快捷键表明写
       `Shift + 上滚 = 向左滚动`），照抄系统约定用户才不用重建肌肉记忆。
       这里直接查注入出去的横轴刻度符号（计数钩子只数 LLMHF_INJECTED）。 */
    Reset();
    Pump(150); Sink();                            /* 冲掉残留请求 */
    ResetWheelRec();
    ActWheelPostH(120, 1);                        /* 物理上滚 */
    Pump(100); Drain();
    CHECK(g_whInj == 1 && g_whSum < 0,
          "V: 滚轮向上 -> 注入向左的横轴刻度（负 delta）");
    ResetWheelRec();
    ActWheelPostH(-120, 1);                       /* 物理下滚 */
    Pump(100); Drain();
    CHECK(g_whInj == 1 && g_whSum > 0,
          "V: 滚轮向下 -> 注入向右的横轴刻度（正 delta）");
    CHECK(g_wheelInj == 0, "V: 横向不会误发到纵轴");


    /* ---- 场景X：反着按 —— Caps 不是本次和弦的第一个键 ----
       实测缺陷：**先按 Alt、再按 Caps，然后先松开 Alt** ——
         · Alt down 时 Caps 还没按（g_cMod 为假）→ 事件被放行，系统看到了 Alt 按下；
         · 之后 Caps 按住期间松开 Alt → 那次 keyup 被"C 按住期间修饰键不外泄"
           吞掉，而往后没有任何人补发 Alt 的抬起 → **目标程序里 Alt 永久按下**。
         现象就是"Alt+caps+J 松手会卡住 Alt"。

       现在的规则：**只拦截 Caps 开头的和弦**。Caps 按下时若
       已有映射相关的键按住，整条会话都不介入 —— 所有键按系统原生行为走，
       天然对称，也就不会有一吞一放的错位。

       断言用**钩子返回值**（1=吞、0=放行）：这是"吞放是否配对"的唯一可靠
       观测手段 —— 只要 Alt 的 down 与 up 都返回 0（都放行），系统侧就不会
       残留"Alt 按着"的状态。 */
    Reset();
    CHECK(Sim(VK_LMENU, TRUE) == 0, "X: 先按 Alt -> 放行（系统看到 Alt down）");
    CHECK(Sim(CLI_CAPS, TRUE) == 0,
          "X: 反着按 Caps 不介入 -> 放行（CapsLock 保持原功能）");
    CHECK(Sim('J', TRUE) == 0 && Sim('J', FALSE) == 0,
          "X: 不介入期间触发键原样放行（J 不变成 Caps+J）");
    CHECK(Sim(VK_LMENU, FALSE) == 0,
          "X: **Alt 抬起也放行** —— 与它的 down 配对（卡键的根因就在这里）");
    CHECK(Sim(CLI_CAPS, FALSE) == 0, "X: Caps 抬起同样放行（与按下对称）");
    Pump(60); Drain();
    CHECK(CountVk(VK_LEFT) == 0, "X: 反着按不触发任何映射动作");
    CHECK(!g_cMod && !ModeDown(MODE_ENHANCE),
          "X: 反着按不留任何内部状态（增强模式未生效）");

    /* 反着按的另一种：先按触发键再按 Caps。同样不介入 —— 否则那个触发键的
       up 会被吞（它的 down 已经被系统看到），又是一个孤立事件。 */
    Reset();
    CHECK(Sim('J', TRUE) == 0, "X: 先按 J -> 放行");
    CHECK(Sim(CLI_CAPS, TRUE) == 0, "X: 再按 Caps 仍不介入");
    CHECK(Sim('J', FALSE) == 0, "X: J 抬起不被吞（与它的 down 配对）");
    CHECK(Sim(CLI_CAPS, FALSE) == 0, "X: Caps 抬起放行");
    Pump(60); Drain();
    CHECK(CountVk(VK_LEFT) == 0, "X: 先按触发键的组合同样不产生映射动作");

    /* ---- 场景Y：正着按（Caps 第一个键）—— 吞放必须成对 ----
       "只拦截 Caps 开头"就意味着 Caps 之后按下的修饰键**要被拦**（不能外泄），
       但拦的时候 down/up 必须一致：只吞 down 会让系统认为它一直按着，
       只吞 up 则会漏一个孤立抬起给目标程序。 */
    Reset();
    CHECK(Sim(CLI_CAPS, TRUE) == 1, "Y: Caps 是第一个键 -> 介入（吞）");
    CHECK(Sim(VK_LMENU, TRUE) == 1, "Y: 会话内按 Alt -> 吞（不外泄）");
    CHECK(Sim('J', TRUE) == 1 && Sim('J', FALSE) == 1, "Y: 触发键 J 也被吞");
    /* 注意这里**不给 Pump 间隔**：让三条消息（Caps down / J down / J up）
       与 Alt 抬起挤在同一批里处理，正是"点按 J 后立刻松 Alt"的极端情形。
       修饰快照必须来自钩子的事件时刻，主线程现读会读到 Alt 已松。 */
    CHECK(Sim(VK_LMENU, FALSE) == 1, "Y: Alt 抬起照样吞（与按下配对）");
    CHECK(Sim(CLI_CAPS, FALSE) == 1, "Y: Caps 抬起被吞");
    Pump(60); Drain();
    CHECK(CountVk(VK_LEFT) == 3,
          "Y: Caps+Alt+点按 J 仍补齐 3 次（修对称性没有改坏功能）");

    /* ---- 场景Z：先松 Caps、再松修饰键 —— up 也要沿用 down 的决定 ----
       进入会话后 g_cMod 在 Caps 抬起那一刻就变假了，此时修饰键的 up 若按
       "g_cMod 为假就放行"处理，就会漏一个孤立抬起给目标程序（down 被吞、
       up 放行）。正确做法是记账：**down 被吞过的，up 也必须吞**。 */
    Reset();
    CHECK(Sim(CLI_CAPS, TRUE) == 1, "Z: Caps 先按 -> 介入");
    CHECK(Sim(VK_LCONTROL, TRUE) == 1, "Z: 会话内按 Ctrl -> 吞");
    CHECK(Sim(CLI_CAPS, FALSE) == 1, "Z: 先松 Caps（会话结束）");
    CHECK(Sim(VK_LCONTROL, FALSE) == 1,
          "Z: **Caps 已松开，Ctrl 抬起仍须吞**（沿用 down 的决定，不留半截）");
    Pump(60); Drain();
    CHECK(!ModeDown(MODE_ENHANCE), "Z: 会话结束后无残留修饰状态");

    /* ---- 场景AA：残留自愈 ----
       物理表说"某键按着"、系统并不认为按着 —— 那是我们漏收过抬起事件
       （例如锁屏时松开）。这种残留若不清，往后每次 Caps 都被判成"反着按"
       而跳过，表现为 Caps 突然完全失效。所以判据里要顺手修正它。 */
    Reset();
    memset((void*)g_phys, 0, sizeof(g_phys));
    g_phys[VK_LMENU] = 1;          /* 伪造：物理表说 Alt 按着 */
    t_sys[VK_LMENU] = 0;           /* 但系统并不认为按着 = 残留 */
    CHECK(Sim(CLI_CAPS, TRUE) == 1,
          "AA: 物理表残留不会永久挡住 Caps（判据交叉校验后自愈）");
    CHECK(g_phys[VK_LMENU] == 0, "AA: 残留的物理位已被修正");
    Sim(CLI_CAPS, FALSE);
    Pump(60); Drain();

    /* ---- 场景AE：把焦点窗口挪到光标处（C+Q）----
       这个动作不发任何键，风险全在"几何"上，所以分两段测：
         ① 纯函数 WinComputePlacement：多显示器（含副屏在左的**负坐标**）、
            任务栏挤掉的高度、窗口比工作区还大 —— 不需要真窗口就能全覆盖；
         ② 真窗口端到端：窗口扔到屏幕外 → 拉回后**左上角对准光标**且整窗可见
            （"窗口被拖出屏幕外、拽不回来"那个用途），外加两类"不老实"的程序：
            自己改尺寸的（不影响落点）、又把自己挪走的（回读锚点后重试拉回）。
       窗口用**自己注册的类 + 自己的窗口过程**：借 main.c 的 WndProc 会在
       WM_DESTROY 里 PostQuitMessage，把测试的消息循环打掉。 */
    {
        RECT work; POINT cur, dst;

        /* 先钉住"接线"：键位表把 Q 接到这个动作上，且属性是"单发、不吃选区、
           不发键"。这些是数据，最容易在改键位时被顺手改坏。 */
        CHECK(ActOfVk('Q') == ACT_MOVEWIN, "AE: 触发键 Q -> 挪窗口动作");
        CHECK(ActHasFlag(ACT_MOVEWIN, ACTF_MOVEWIN) &&
              ActHasFlag(ACT_MOVEWIN, ACTF_SINGLE_SHOT) &&
              ActHasFlag(ACT_MOVEWIN, ACTF_NO_SHIFT),
              "AE: 属性 = 由 winmove 执行 + 单发 + 不吃选区修饰");
        CHECK(ActOutVk(ACT_MOVEWIN) == 0, "AE: 该动作不发任何键");

        work.left = 0; work.top = 0; work.right = 1920; work.bottom = 1080;
        cur.x = 1000; cur.y = 200;
        WinComputePlacement(&work, cur, 800, 600, &dst);
        CHECK(dst.x == 1000 && dst.y == 200, "AE: 单屏 -> 左上角对齐光标");

        cur.y = 500;                                  /* 再往下就放不下了 */
        WinComputePlacement(&work, cur, 800, 600, &dst);
        CHECK(dst.x == 1000 && dst.y == 480,
              "AE: 下边放不下 -> 只往上钳到贴住工作区底");

        cur.x = 10; cur.y = 10;                       /* 光标贴左上角 */
        WinComputePlacement(&work, cur, 800, 600, &dst);
        CHECK(dst.x == 10 && dst.y == 10, "AE: 光标贴左上角 -> 就放在那儿");

        cur.x = 1910; cur.y = 1070;                   /* 光标贴右下角 */
        WinComputePlacement(&work, cur, 800, 600, &dst);
        CHECK(dst.x == 1120 && dst.y == 480, "AE: 光标贴右下角 -> 整窗仍在工作区内");

        cur.x = 960; cur.y = 540;                     /* 窗口比工作区还大 */
        WinComputePlacement(&work, cur, 2000, 1200, &dst);
        CHECK(dst.x == 0 && dst.y == 0,
              "AE: 窗口大于工作区 -> 左上角对齐（不推出屏幕）");

        /* 副屏在右：光标在第二块屏上，窗口必须落到那一块里（本功能的主用途） */
        work.left = 1920; work.right = 3840;
        cur.x = 2500; cur.y = 400;
        WinComputePlacement(&work, cur, 800, 600, &dst);
        CHECK(dst.x == 2500 && dst.y == 400 &&
              dst.x >= work.left && dst.x + 800 <= work.right,
              "AE: 副屏在右 -> 落在光标那一块屏内");

        /* 副屏在左：Windows 里左侧/上方的副屏坐标是**负数**，别当成越界值 */
        work.left = -1920; work.right = 0;
        cur.x = -1000; cur.y = 300;
        WinComputePlacement(&work, cur, 640, 480, &dst);
        CHECK(dst.x == -1000 && dst.y == 300 &&
              dst.x >= work.left && dst.x + 640 <= work.right,
              "AE: 副屏在左（负坐标）-> 同样落在该屏内");

        /* 任务栏占掉底部：工作区比显示器矮，窗口不许压到任务栏上 */
        work.left = 0; work.right = 1920; work.bottom = 1040;
        cur.x = 960; cur.y = 1030;
        WinComputePlacement(&work, cur, 800, 600, &dst);
        CHECK(dst.x == 960 && dst.y == 440 && dst.y + 600 == work.bottom,
              "AE: 有任务栏 -> 下沿贴工作区，不压任务栏");

        /* ---- 真窗口：扔到屏幕外 -> 拉回光标处 ---- */
        POINT saveCur;
        GetCursorPos(&saveCur);
        MONITORINFO mi;
        memset(&mi, 0, sizeof(mi));
        mi.cbSize = sizeof(mi);
        HMONITOR hMon = MonitorFromPoint(saveCur, MONITOR_DEFAULTTONEAREST);
        POINT tgt = saveCur;
        if (hMon && GetMonitorInfoW(hMon, &mi)) {      /* 光标放在工作区正中 */
            tgt.x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left) / 2;
            tgt.y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top) / 2;
        }
        WNDCLASSW wc2 = {0};
        wc2.lpfnWndProc = MoveProbeProc;
        wc2.hInstance = GetModuleHandleW(NULL);
        wc2.lpszClassName = L"CapsEnhanceWinMove";
        RegisterClassW(&wc2);
        HWND w = CreateWindowW(L"CapsEnhanceWinMove", L"winmove",
                               WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               -4000, -4000, 400, 300,
                               NULL, NULL, GetModuleHandleW(NULL), NULL);
        CHECK(w != NULL, "AE: 测试窗口创建成功");
        if (w) {
            RECT wr;
            GetWindowRect(w, &wr);
            CHECK(wr.right <= mi.rcWork.left,
                  "AE: 前置条件 —— 窗口确实在屏幕外（工作区左侧之外）");
            SetCursorPos(tgt.x, tgt.y);
            Pump(30);
            GetCursorPos(&tgt);          /* 以**实际**光标为准：别的进程可能挪过它 */
            POINT c0 = tgt, c1;
            CHECK(WinMoveWindowToCursor(w) != FALSE, "AE: 屏幕外的窗口 -> 移动成功");
            Pump(30);
            GetCursorPos(&c1);           /* 光标若在测试期间被移动，对准断言就不成立 */
            BOOL curStable = (c0.x == c1.x && c0.y == c1.y);
            tgt = c1;
            GetWindowRect(w, &wr);
            printf("       (拉回后: 光标=%d,%d rect=%d,%d,%d,%d)%s\n",
                   (int)tgt.x, (int)tgt.y, (int)wr.left, (int)wr.top,
                   (int)wr.right, (int)wr.bottom, curStable ? "" : "  （光标被移动，跳过对准断言）");
            CHECK(wr.left >= mi.rcWork.left && wr.top >= mi.rcWork.top &&
                  wr.right <= mi.rcWork.right && wr.bottom <= mi.rcWork.bottom,
                  "AE: 拉回后整窗可见（完全落在工作区内）");
            CHECK(!curStable || PlacementMatches(&mi.rcWork, tgt, wr),
                  "AE: 拉回后落点 = 既定规则（左上角对齐光标，放不下就钳进工作区）");
            /* 同样要看 curStable：否则只要有别的进程（用户自己）动了一下鼠标，
               tgt 就已经不是我们刚刚对准的那个点了 —— 这条断言会把用户的鼠标动作
               当成自己的失败（CI 上没人碰鼠标，本机调试时却经常碰）。 */
            CHECK(!curStable || (tgt.x >= wr.left && tgt.x < wr.right &&
                  tgt.y >= wr.top && tgt.y < wr.bottom),
                  "AE: 光标仍在窗口内（伸手就能抓到）");

            /* 最大化：位置由系统按显示器算、给它的 (x,y) 会被忽略，所以挪到
               **另一块显示器**必须先还原再挪 —— 但挪完要还回去，不能顺手取消
               用户的最大化状态。
               ★ 这一条必须让光标落在窗口**所在显示器之外**：光标与本窗口同屏时
               C+Q 会直接拦下不做还原（见下一条断言），走不到这条路。 */
            ShowWindow(w, SW_MAXIMIZE);
            Pump(30);
            CHECK(IsZoomed(w) != FALSE, "AE: 前置条件 —— 窗口已最大化");
            {
                /* 找一块**别的**显示器上的落点；找不到（单显示器）就跳过这一条，
                   不假红 —— 单显示器机器上"挪到别的屏"本来就不成立。 */
                POINT other;
                BOOL haveOther = FALSE;
                HMONITOR hSelfMon = MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST);
                EnumMonitorsForOther(hSelfMon, &other, &haveOther);
                if (haveOther) {
                    SetCursorPos(other.x, other.y);
                    Pump(30);
                    WinMoveWindowToCursor(w);
                    Pump(80);
                    CHECK(IsZoomed(w) != FALSE, "AE: 最大化的窗口挪完仍是最大化");
                    RECT wr2;
                    GetWindowRect(w, &wr2);
                    CHECK(MonitorFromRect(&wr2, MONITOR_DEFAULTTONEAREST) ==
                          MonitorFromPoint(other, MONITOR_DEFAULTTONEAREST),
                          "AE: 最大化窗口挪到了光标所在的另一块显示器");
                } else {
                    printf("       (只有一块显示器：跳过\"最大化窗口挪到另一屏\")\n");
                }
            }

            /* ★★ 最大化 + **光标就在它自己那块屏**上：不许有任何还原/最大化动作。
               这是 bug 现场：「最大化窗口的 C+Q 也会抖动」—— 原流程是
               SW_RESTORE → SetWindowPos → SW_MAXIMIZE，落点其实一模一样，
               用户只看到小窗口闪一帧。
               判据不能只看矩形：动了一圈再铺回去，矩形**完全一样**。
               所以数 `WM_WINDOWPOSCHANGED`（`g_posChanged`）：要求一次都没有。
               （这条在单显示器机器上同样成立 —— 光标与窗口必然同屏。） */
            ShowWindow(w, SW_MAXIMIZE);
            Pump(40);
            CHECK(IsZoomed(w) != FALSE, "AE: 前置条件 —— 窗口已最大化（同屏用例）");
            {
                RECT before, after;
                GetWindowRect(w, &before);
                POINT onSelf = { before.left + 80, before.top + 60 };
                SetCursorPos(onSelf.x, onSelf.y);
                Pump(30);
                GetCursorPos(&onSelf);          /* 以实际光标为准 */
                g_posChanged = 0;
                WinMoveWindowToCursor(w);
                Pump(120);
                GetWindowRect(w, &after);
                CHECK(EqualRect(&before, &after) != FALSE,
                      "AE: 最大化窗口同屏 -> 矩形一点没变");
                CHECK(g_posChanged == 0,
                      "AE: 最大化窗口同屏 -> 连一次位置变化都没发生（不还原、不再最大化）");
                CHECK(IsZoomed(w) != FALSE,
                      "AE: 最大化窗口同屏 -> 最大化状态当然也还在");
            }

            /* ★ 全屏窗口：**不许动**（bug 现场：「在本窗口已经全屏时按 C+Q 不再响应，
               挪跟没挪一样，只会抖动一下」）。
               全屏窗口在系统眼里就是"最大化铺满"，照原流程走是
               SW_RESTORE → SetWindowPos → SW_MAXIMIZE 又铺回原位，
               用户只看到一次还原/最大化重绘 = "抖动一下"。
               判据复用 drag.c 那条（WinIsFullscreen：窗口矩形 == rcMonitor）——
               这里把窗口**真的**铺满整块显示器（用 rcMonitor，盖住任务栏那一侧），
               然后要求：谓词认它、WinMoveWindowToCursor 返回 FALSE、且矩形**一点没变**。
               最后一条是关键：只断言返回值的话，"动了但动了等于没动"照样能过。 */
            ShowWindow(w, SW_RESTORE);
            Pump(30);
            SetWindowPos(w, NULL, mi.rcMonitor.left, mi.rcMonitor.top,
                         (int)(mi.rcMonitor.right - mi.rcMonitor.left),
                         (int)(mi.rcMonitor.bottom - mi.rcMonitor.top),
                         SWP_NOZORDER | SWP_NOACTIVATE);
            Pump(30);
            {
                RECT fr, fr2;
                GetWindowRect(w, &fr);
                CHECK(WinIsFullscreen(w) != FALSE,
                      "AE: 前置条件 —— 窗口已铺满整块显示器（rcMonitor）");
                SetCursorPos(tgt.x, tgt.y);
                Pump(20);
                BOOL moved = WinMoveWindowToCursor(w);
                Pump(60);
                GetWindowRect(w, &fr2);
                CHECK(moved == FALSE, "AE: 全屏窗口 -> 不做移动（返回失败）");
                CHECK(EqualRect(&fr, &fr2) != FALSE,
                      "AE: 全屏窗口矩形**一点没变**（没有还原/最大化那一抖）");
            }

            /* 反向对照：**工作区大小**（rcWork，差一个任务栏）不算全屏 ——
               那是普通最大化，必须照旧能挪。少了这一条，把谓词写成">= 工作区"
               之类的宽松版本也能全绿。
               注意这里只验"谓词不误杀"，不去验移动结果：Windows 会把"和 rcMonitor
               等宽等高、只是位置不同"的窗口吸附成最大化，几何结果不稳定，
               断言它就成了测系统行为而不是测我们的代码。 */
            ShowWindow(w, SW_RESTORE);
            Pump(30);
            SetWindowPos(w, NULL, mi.rcWork.left, mi.rcWork.top,
                         (int)(mi.rcWork.right - mi.rcWork.left),
                         (int)(mi.rcWork.bottom - mi.rcWork.top),
                         SWP_NOZORDER | SWP_NOACTIVATE);
            Pump(30);
            if (!EqualRect(&mi.rcWork, &mi.rcMonitor)) {
                /* 本机有任务栏：工作区比整屏矮 → 必须是"不是全屏" */
                CHECK(WinIsFullscreen(w) == FALSE,
                      "AE: 铺满**工作区**（比全屏矮一个任务栏）不算全屏");
            } else {
                /* 无任务栏的机器：rcWork == rcMonitor，这个窗口本来就是全屏 */
                CHECK(WinIsFullscreen(w) != FALSE,
                      "AE: 无任务栏（rcWork == rcMonitor）-> 铺满即为全屏");
            }

            /* ① 挪过去之后**自己改尺寸**（实测 CLaunch 跨不同缩放比例的屏就是这样：
               保持物理尺寸不变，逻辑尺寸 578x437 → 495x374）。落点改成"左上角对齐
               光标"之后它**不该**影响落点 —— 这正是换规则要证明的事。 */
            ShowWindow(w, SW_RESTORE);
            Pump(30);
            SetWindowPos(w, NULL, -4000, -4000, 400, 300,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            Pump(30);
            SetCursorPos(tgt.x, tgt.y);
            Pump(30);
            GetCursorPos(&tgt);          /* 同上：用实际光标位置做判据 */
            c0 = tgt;
            g_shrinkOnMove = 1;                 /* 下一次移动后自己缩成 360x260 */
            CHECK(WinMoveWindowToCursor(w) != FALSE,
                  "AE: 会自己改尺寸的程序 -> 调用成功");
            Pump(60);
            GetCursorPos(&c1);
            curStable = (c0.x == c1.x && c0.y == c1.y);
            tgt = c1;
            GetWindowRect(w, &wr);
            printf("       (改尺寸后: 光标=%d,%d rect=%d,%d,%d,%d)%s\n",
                   (int)tgt.x, (int)tgt.y, (int)wr.left, (int)wr.top,
                   (int)wr.right, (int)wr.bottom, curStable ? "" : "  （光标被移动，跳过对准断言）");
            CHECK((int)(wr.right - wr.left) == 360 && (int)(wr.bottom - wr.top) == 260,
                  "AE: 前置条件 —— 窗口确实自己缩成了 360x260");
            CHECK(!curStable || PlacementMatches(&mi.rcWork, tgt, wr),
                  "AE: 对方改尺寸也不影响落点（按新尺寸算出的落点一致）");

            /* ② 挪过去之后**又把自己挪走**（带吸附/跟随行为的启动器、面板）。
               这种只能靠"回读锚点 + 再发一次"；一次调用要能收敛（≤3 次）。 */
            SetWindowPos(w, NULL, -4000, -4000, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            Pump(30);
            SetCursorPos(tgt.x, tgt.y);
            Pump(30);
            GetCursorPos(&tgt);
            g_jumpOnMove = 1;                   /* 下一次移动后自己跳 +150,+120 */
            CHECK(WinMoveWindowToCursor(w) != FALSE,
                  "AE: 会把自己挪走的程序 -> 调用成功");
            Pump(60);
            GetWindowRect(w, &wr);
            printf("       (自挪后: 光标=%d,%d rect=%d,%d,%d,%d)\n",
                   (int)tgt.x, (int)tgt.y, (int)wr.left, (int)wr.top,
                   (int)wr.right, (int)wr.bottom);
            CHECK(PlacementMatches(&mi.rcWork, tgt, wr),
                  "AE: 对方自己挪走后被重试拉回（回读锚点生效）");

            DestroyWindow(w);
        }
        SetCursorPos(saveCur.x, saveCur.y);            /* 还原用户的光标位置 */
    }

    /* ---- 场景AF：Caps + 左键拖动挪窗口 ----
       直接喂鼠标事件给生产的鼠标钩子过程（flags=0 = 非注入）：
         LBUTTONDOWN(Caps 按着) -> 接管并吞掉；MOUSEMOVE -> 放行 + 拖拽通道
         按"抓取基准 + 位移"挪窗口；LBUTTONUP -> 吞掉（与按下配对）并收手。
       测试窗口必须是 **topmost**：WindowFromPoint 返回最上层的窗口，
       控制台或别的窗口压在上面就测不准。 */
    {
        WNDCLASSW wc3 = {0};
        wc3.lpfnWndProc = DefWindowProcW;
        wc3.hInstance = GetModuleHandleW(NULL);
        wc3.lpszClassName = L"CapsEnhanceDragProbe";
        RegisterClassW(&wc3);
        HWND w = CreateWindowExW(WS_EX_TOPMOST, L"CapsEnhanceDragProbe", L"drag",
                                 WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                 400, 300, 320, 220,
                                 NULL, NULL, GetModuleHandleW(NULL), NULL);
        CHECK(w != NULL, "AF: 测试窗口创建成功");
        if (w) {
            SetWindowPos(w, NULL, 400, 300, 320, 220, SWP_NOZORDER | SWP_NOACTIVATE);
            Pump(30);
            RECT r0;
            GetWindowRect(w, &r0);
            POINT in = { r0.left + 120, r0.top + 80 };   /* 窗口内的一点 */

            /* ① 内置默认黑名单：**两份段都不写也要生效**（取自 AltSnap 的默认值）。
               窗口名单：任务栏（Shell_TrayWnd）是 AltSnap Windows 列表里的类名，
               用它当"必中"的样本最稳 —— 它一定存在。 */
            WriteChordIni("[General]\r\n");
            ConfigLoadFrom(L"bin/chordtest.ini");
            HWND tray = FindWindowW(L"Shell_TrayWnd", NULL);
            CHECK(tray != NULL && ConfigIsDragBlacklisted(tray),
                  "AF: 窗口名单内置默认命中任务栏（取自 AltSnap 的 Shell_TrayWnd）");
            CHECK(!ConfigIsDragBlacklisted(w), "AF: 普通窗口不在默认黑名单里");

            /* ② 两份名单**各自独立**：
               · 只把本窗口的类名写进 [DragWindowBlacklist] -> 命中（窗口名单生效）；
               · 换成只写进程名，且进程名不是本测试进程 -> 不命中（证明是**两份**，
                 不是"任一份非空就全算命中"）。 */
            WriteChordIni("[DragWindowBlacklist]\r\nCapsEnhanceDragProbe\r\n");
            ConfigLoadFrom(L"bin/chordtest.ini");
            CHECK(ConfigIsDragBlacklisted(w),
                  "AF: [DragWindowBlacklist] 写窗口类 -> 命中");
            /* 窗口名单被显式写成空的（段还在）-> 该类名不再命中，
               而进程名单仍是内置默认 -> 探针进程也不在进程名单里 */
            WriteChordIni("[DragWindowBlacklist]\r\n[DragProcessBlacklist]\r\n");
            ConfigLoadFrom(L"bin/chordtest.ini");
            CHECK(!ConfigIsDragBlacklisted(w),
                  "AF: 两份都写成空段 -> 不再命中（段存在即以文件为准）");
            CHECK(tray != NULL && !ConfigIsDragBlacklisted(tray),
                  "AF: 窗口名单被清空 -> 任务栏也不再命中（各自独立回落）");

            /* ③ 进程名单：用本测试进程的 exe 名当样本（一定存在、也一定不在默认名单里） */
            {
                WCHAR me[MAX_PATH] = L"";
                GetModuleFileNameW(NULL, me, MAX_PATH);
                WCHAR* sl = wcsrchr(me, L'\\');
                const WCHAR* myExe = sl ? sl + 1 : me;
                WCHAR ini[512];
                _snwprintf_s(ini, 512, _TRUNCATE,
                             L"[DragWindowBlacklist]\r\n[DragProcessBlacklist]\r\n%s\r\n",
                             myExe);
                char u8[1024];
                WideCharToMultiByte(CP_UTF8, 0, ini, -1, u8, 1024, NULL, NULL);
                WriteChordIni(u8);
                ConfigLoadFrom(L"bin/chordtest.ini");
                CHECK(ConfigIsDragBlacklisted(w),
                      "AF: [DragProcessBlacklist] 写进程名 -> 命中");
            }

            /* ④ 不认识的段名一律忽略（不再为旧段名留兼容入口 —— 本程序只有作者
               一个人用，配置随版本改即可）。用"旧段名 + 本该命中的条目"当样本：
               命中 = 兼容还在，不命中 = 真的被忽略了。 */
            WriteChordIni("[DragBlacklist]\r\nCapsEnhanceDragProbe\r\n");
            ConfigLoadFrom(L"bin/chordtest.ini");
            CHECK(!ConfigIsDragBlacklisted(w),
                  "AF: 未知段名被忽略（旧 [DragBlacklist] 不再认）");

            /* ⑤ 类名条目：命中后左键**原样透传**（不接管、不进入拖拽） */
            WriteChordIni("[DragWindowBlacklist]\r\nCapsEnhanceDragProbe\r\n");
            ConfigLoadFrom(L"bin/chordtest.ini");
            Key(CLI_CAPS, TRUE); Pump(2);
            CHECK(MousePt(WM_LBUTTONDOWN, in) == 0,
                  "AF: 黑名单（类名条目）-> 左键透传，不接管");
            CHECK(!DragActive(), "AF: 黑名单 -> 不进入拖拽");
            Key(CLI_CAPS, FALSE); Pump(2);

            /* ⑥ 正常拖拽：down 吞掉 + 窗口跟随位移 + up 吞掉并收手 */
            WriteChordIni("[General]\r\n");      /* 清掉黑名单（回内置默认） */
            ConfigLoadFrom(L"bin/chordtest.ini");
            Key(CLI_CAPS, TRUE); Pump(2);
            POINT p1 = in;
            CHECK(MousePt(WM_LBUTTONDOWN, p1) == 1, "AF: 接管 -> 吞掉左键按下");
            CHECK(DragActive(), "AF: 拖拽已开始");

            POINT p2 = { p1.x + 100, p1.y + 80 };
            MousePt(WM_MOUSEMOVE, p2);
            RECT want = { r0.left + 100, r0.top + 80,
                          r0.left + 100 + (r0.right - r0.left),
                          r0.top + 80 + (r0.bottom - r0.top) };
            BOOL moved = FALSE;
            RECT r;
            for (int i = 0; i < 60 && !moved; i++) {
                Pump(10);
                GetWindowRect(w, &r);
                moved = (r.left == want.left && r.top == want.top);
            }
            CHECK(moved, "AF: 窗口跟随光标位移（+100,+80）");

            CHECK(MousePt(WM_LBUTTONUP, p2) == 1, "AF: 吞掉左键抬起（与按下配对）");


            Pump(60);
            CHECK(!DragActive(), "AF: 抬起后拖拽结束");
            GetWindowRect(w, &r);
            CHECK(r.left == want.left && r.top == want.top,
                  "AF: 结束后停在最后一次落点上");

            Pump(60);
            Key(CLI_CAPS, FALSE); Pump(2);

            /* ⑦ **拖出阈值**（AltSnap 同款 DragThreshold + DragOutThresholdPx）：
               最大化窗口按下即还原会把窗口缩小，所以点一下（手没动）**必须什么都不做**。
               阈值内的小位移也不动；拖过阈值才开始还原 + 跟随。 */
            ShowWindow(w, SW_MAXIMIZE);
            Pump(60);
            CHECK(IsZoomed(w) != FALSE, "AF: 前置条件 —— 窗口已最大化");
            {
                RECT maxRc;
                GetWindowRect(w, &maxRc);
                POINT inMax = { maxRc.left + 60, maxRc.top + 40 };   /* 最大化窗口内一点 */

                /* ⑦-1 按下 + 直接抬起（点一下，手没动）-> 仍最大化 */
                Key(CLI_CAPS, TRUE); Pump(2);
                CHECK(MousePt(WM_LBUTTONDOWN, inMax) == 1,
                      "AF: 最大化窗口上按下 -> 仍接管（吞掉点击）");
                CHECK(MousePt(WM_LBUTTONUP, inMax) == 1,
                      "AF: 抬起也被吞（与按下配对）");
                Pump(80);
                CHECK(IsZoomed(w) != FALSE,
                      "AF: Caps+左键**点一下**不会把最大化窗口缩小");
                Key(CLI_CAPS, FALSE); Pump(2);

                /* ⑦-2 阈值内的小位移（+8px）-> 仍最大化、仍未还原 */
                Key(CLI_CAPS, TRUE); Pump(2);
                MousePt(WM_LBUTTONDOWN, inMax);
                /* 变量名别叫 small：rpcndr.h 里 small 是个类型(unsigned char)，
                   写 `POINT small` 会被当成“类型 + 类型”解析失败。 */
                POINT smallPt = { inMax.x + 8, inMax.y + 4 };
                MousePt(WM_MOUSEMOVE, smallPt);
                Pump(120);
                CHECK(IsZoomed(w) != FALSE,
                      "AF: 阈值内的小位移不还原（不缩窗口）");
                MousePt(WM_LBUTTONUP, smallPt);
                Pump(60);
                Key(CLI_CAPS, FALSE); Pump(2);

                /* ⑦-3 拖过阈值（+60px）-> 还原并跟随光标 */
                Key(CLI_CAPS, TRUE); Pump(2);
                MousePt(WM_LBUTTONDOWN, inMax);
                POINT big = { inMax.x + 60, inMax.y + 40 };
                MousePt(WM_MOUSEMOVE, big);
                BOOL restored = FALSE;
                RECT rr;
                for (int i = 0; i < 80 && !restored; i++) {
                    Pump(10);
                    restored = (IsZoomed(w) == FALSE);
                }
                CHECK(restored, "AF: 越过拖出阈值 -> 还原成普通窗口");
                GetWindowRect(w, &rr);
                CHECK(rr.left != maxRc.left || rr.top != maxRc.top,
                      "AF: 还原后位置随光标（不再占满整屏）");
                MousePt(WM_LBUTTONUP, big);
                Pump(60);
                Key(CLI_CAPS, FALSE); Pump(2);
            }

            /* ⑧ 全屏窗口：**不接管**（与 C+Q 共用 WinIsFullscreen 那条判据）。
               把窗口真的铺满整块显示器，然后在它上面按 Caps+左键 ——
               必须原样透传（返回 0）、不进入拖拽。
               这条与场景 AE 的全屏用例是一对：同一个谓词，两个调用方都得拦住，
               否则会出现"拖动不让动、C+Q 却去动"这种自相矛盾的行为。 */
            {
                MONITORINFO fmi;
                memset(&fmi, 0, sizeof(fmi));
                fmi.cbSize = sizeof(fmi);
                HMONITOR fhm = MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST);
                if (fhm && GetMonitorInfoW(fhm, &fmi)) {
                    ShowWindow(w, SW_RESTORE);
                    Pump(30);
                    SetWindowPos(w, NULL, fmi.rcMonitor.left, fmi.rcMonitor.top,
                                 (int)(fmi.rcMonitor.right - fmi.rcMonitor.left),
                                 (int)(fmi.rcMonitor.bottom - fmi.rcMonitor.top),
                                 SWP_NOZORDER | SWP_NOACTIVATE);
                    Pump(40);
                    CHECK(WinIsFullscreen(w) != FALSE,
                          "AF: 前置条件 —— 窗口已铺满整块显示器");
                    RECT fr0, fr1;
                    GetWindowRect(w, &fr0);
                    POINT inFs = { fr0.left + 40, fr0.top + 40 };
                    Key(CLI_CAPS, TRUE); Pump(2);
                    CHECK(MousePt(WM_LBUTTONDOWN, inFs) == 0,
                          "AF: 全屏窗口 -> 左键原样透传（不接管）");
                    CHECK(!DragActive(), "AF: 全屏窗口 -> 不进入拖拽");
                    /* 鼠标位移也不该让它动：透传之后走的是窗口自己的命中测试 */
                    POINT fs2 = { inFs.x + 120, inFs.y + 90 };
                    MousePt(WM_MOUSEMOVE, fs2);
                    Pump(120);
                    GetWindowRect(w, &fr1);
                    CHECK(EqualRect(&fr0, &fr1) != FALSE,
                          "AF: 全屏窗口 -> 拖不动（矩形一点没变）");
                    MousePt(WM_LBUTTONUP, fs2);
                    Pump(60);
                    Key(CLI_CAPS, FALSE); Pump(2);
                }
            }

            Key(CLI_CAPS, FALSE); Pump(2);
            DestroyWindow(w);
        }
    }

    /* ---- 场景AH：打开焦点程序所在文件夹（C+\）----
       shell 那个调用本身**测不了**：它会真的弹出文件管理器窗口、还要等 explorer
       响应。所以只钉能离线断言的三件事：
         ① 接线：`\` 接到这个动作，属性 = 单发 + 不吃选区 + 不发键（同 Q/W 那类表断言）；
         ② 取路径：给一个真实窗口，解出来的就是**本进程 exe 的全路径**（UWP 那条
            分支要有真 UWP 程序在前台才走得到，只能手工验）；
         ③ 失败路径：拿不到目标时必须**报原因**返回，而不是崩溃或假装成功。
       另：这里顺带守住"ConfigGetProcessName = 全路径的最后一段"—— 它刚被改成
       复用一个取全路径的 helper，名单匹配全靠这个名字（多出个目录就会静默失配）。 */
    {
        CHECK(ActOfVk(VK_F3) == ACT_OPENFOLDER,
              "AH: 触发键 F3 -> 打开所在文件夹动作");
        CHECK(ActOfVk(VK_OEM_5) == ACT_NONE,
              "AH: 旧的 `\\` 键已不再映射（改到 F3 之后不留旧绑定）");
        CHECK(ActHasFlag(ACT_OPENFOLDER, ACTF_OPENFOLDER) &&
              ActHasFlag(ACT_OPENFOLDER, ACTF_SINGLE_SHOT) &&
              ActHasFlag(ACT_OPENFOLDER, ACTF_NO_SHIFT),
              "AH: 属性 = 由 folder 执行 + 单发 + 不吃选区修饰");
        CHECK(ActOutVk(ACT_OPENFOLDER) == 0, "AH: 该动作不发任何键");

        WCHAR want[PROC_PATH_MAX];
        DWORD wn = GetModuleFileNameW(NULL, want, PROC_PATH_MAX);
        CHECK(wn > 0 && wn < PROC_PATH_MAX, "AH: 前置条件 —— 取到本进程 exe 路径");

        WNDCLASSW wcF = {0};
        wcF.lpfnWndProc = DefWindowProcW;
        wcF.hInstance = GetModuleHandleW(NULL);
        wcF.lpszClassName = L"CapsEnhanceFolderProbe";
        RegisterClassW(&wcF);
        HWND w = CreateWindowW(L"CapsEnhanceFolderProbe", L"folder",
                               WS_OVERLAPPEDWINDOW,
                               0, 0, 200, 120,
                               NULL, NULL, GetModuleHandleW(NULL), NULL);
        CHECK(w != NULL, "AH: 测试窗口创建成功");
        if (w) {
            WCHAR got[PROC_PATH_MAX];
            got[0] = 0;
            /* err 传 NULL：顺带守住"可以不关心原因"这条调用契约 */
            CHECK(FolderProcessPathOf(w, got, PROC_PATH_MAX, NULL) != FALSE,
                  "AH: 普通窗口 -> 取到进程全路径");
            CHECK(wcsstr(got, L":\\") != NULL, "AH: 解出来是**全路径**而不是文件名");
            CHECK(_wcsicmp(got, want) == 0,
                  "AH: 解出来的就是本进程的 exe 全路径");

            WCHAR name[CFG_NAME_MAX];
            name[0] = 0;
            CHECK(ConfigGetProcessName(w, name, CFG_NAME_MAX) != FALSE,
                  "AH: 取进程名（不含目录）成功");
            const WCHAR* slash = wcsrchr(want, L'\\');
            CHECK(slash && _wcsicmp(name, slash + 1) == 0,
                  "AH: 进程名 = 全路径的最后一段");
            CHECK(wcschr(name, L'\\') == NULL, "AH: 进程名里不许出现目录分隔符");
            CHECK(ConfigGetProcessPath(w, got, PROC_PATH_MAX) != FALSE &&
                  _wcsicmp(got, want) == 0,
                  "AH: 按窗口取全路径与按 PID 取走的是同一条路（去重后仍一致）");

            DestroyWindow(w);
        }

        /* 失败路径：没有窗口就没有路径 —— 必须是"带原因地失败" */
        {
            WCHAR got[PROC_PATH_MAX];
            got[0] = L'x';                       /* 先弄脏：失败时必须被清空 */
            int err = 0;
            CHECK(FolderProcessPathOf(NULL, got, PROC_PATH_MAX, &err) == FALSE,
                  "AH: 无目标窗口 -> 取路径失败（不崩溃、不假装成功）");
            CHECK(err == FOLDER_ERR_NOPATH, "AH: 失败原因 = 拿不到进程路径");
            CHECK(got[0] == 0, "AH: 失败时输出被清空（不留上一次的残留）");
        }
    }

    /* ---- 场景AI：按键帮助（C+F1，窗口在独立 exe 里）----
       帮助窗口是另一个进程、而且"只能按住才显示"，它的像素测不了。能离线钉住的
       是两件容易在以后改坏的事：
         ① 接线与寿命语义：F1 接到帮助动作；按下请求显示、抬起请求隐藏、
            **松开 Caps 也请求隐藏**（窗口的寿命 = 这个和弦，不是"点一下常亮"）；
         ② **键位表与帮助图不会各说各话**：帮助 exe 直接 include 同一张
            keymap_table.h，这里遍历它断言"每条动作都有图上文案" —— 
            以后加动作忘了写 label，帮助图上就会出现一张空白键帽，这条会红。 */
    {
        CHECK(ActOfVk(VK_F1) == ACT_HELP, "AI: 触发键 F1 -> 按键帮助（帮助窗口）");
        CHECK(ActHasFlag(ACT_HELP, ACTF_HELP) &&
              ActHasFlag(ACT_HELP, ACTF_SINGLE_SHOT) &&
              ActHasFlag(ACT_HELP, ACTF_NO_SHIFT),
              "AI: 属性 = 由 help 处理 + 单发 + 不吃选区修饰");
        CHECK(ActOutVk(ACT_HELP) == 0, "AI: 该动作不发任何键（只是个窗口）");

        int noLabel = 0;
        for (int i = 0; i < ACT_N; i++)
            if (!kActions[i].label || !kActions[i].label[0]) noLabel++;
        CHECK(noLabel == 0, "AI: 键位表每条动作都有帮助图文案（不会出现空键帽）");

        ResetHelpRec();
        Sim(CLI_CAPS, TRUE);  Pump(20);
        Sim(VK_F1, TRUE);     Pump(20);
        CHECK(g_helpShow >= 1, "AI: 按下 C+F1 -> 请求显示帮助");
        Sim(VK_F1, FALSE);    Pump(20);
        CHECK(g_helpHide >= 1, "AI: 抬起 F1 -> 请求隐藏帮助");

        int hideBefore = g_helpHide;
        Sim(VK_F1, TRUE);     Pump(20);     /* 再按一次，F1 按着不放 */
        Sim(CLI_CAPS, FALSE); Pump(20);     /* 直接松开 Caps */
        CHECK(g_helpHide > hideBefore,
              "AI: 松开 Caps（F1 仍按着）-> 同样请求隐藏（寿命=和弦）");
        Sim(VK_F1, FALSE);    Pump(20);
        Drain();
    }

    /* ---- 场景AI2：剪贴板浮层（C+V）----
       窗口本体在另一个 exe 里，测不了；能离线钉住的是三件"改坏了很难发现"的事：
         ① C+V 真的接到 ClipShow（不需要任何修饰键 —— 与帮助 F1 同类）。
         ② 寿命**不等于**和弦：松开 Caps 之后浮层仍然开着（否则来不及挑条目），
            且这期间 Esc 被吞掉并转发（它自己收不到键盘）；关掉之后 Esc 必须
            恢复放行 —— 永久吃掉用户的 Esc 是最糟的一类 bug。 */
    {
        ResetClipRec();
        Sim(CLI_CAPS, TRUE);  Pump(20);
        Sim('V', TRUE);       Pump(20);
        Sim('V', FALSE);      Pump(20);
        CHECK(g_clipShow >= 1, "AI2: C+V -> 请求显示剪贴板");
        Sim(CLI_CAPS, FALSE); Pump(20);
        Drain();

        CHECK(g_clipOpen, "AI2: 松开 Caps 之后剪贴板仍然开着（寿命 ≠ 和弦）");
        LRESULT r1 = Sim(VK_ESCAPE, TRUE);
        CHECK(r1 == 1, "AI2: 浮层开着时 Esc 被吞掉（代收转发，不外泄给系统）");
        Sim(VK_ESCAPE, FALSE); Pump(20);
        CHECK(g_clipEsc >= 1, "AI2: Esc 被转给了剪贴板浮层");
        CHECK(!g_clipOpen, "AI2: Esc 之后浮层已关");
        Drain();

        LRESULT r2 = Sim(VK_ESCAPE, TRUE);
        CHECK(r2 == 0, "AI2: 浮层关掉后 Esc 恢复放行（不会永久吃键）");
        Sim(VK_ESCAPE, FALSE); Pump(20);
        Drain();
    }

    /* ---- 场景AJ：记录日志结束的通知**可点击**（点了打开并选中那份日志）----
       这条链路的末端（真的弹出文件管理器）与打开文件夹那条一样测不了，能离线钉住的
       是几件容易在以后改坏的事：
         ① **接线**：托盘回调认出 NIN_BALLOONUSERCLICK 并去开那个路径，**不**顺手弹
            托盘菜单（"点通知却跳出菜单"是最容易犯的错）；反过来，点图标必须**照旧**
            弹菜单 —— 只钉前一半的话，把菜单整个关掉也能蒙过。
         ② **一次点击只生效一次**：同一条气泡重复点不该重复开，更不该在路径被下一次
            记录覆盖之后还留着旧目标。
         ③ 路径 -> shell 操作对象的规则（`FolderResolveTarget`）：文件要**选中它**；
            文件不在（记录期间日志刚被轮转过就可能不在）就得退回**目录**；目录不能带
            尾部分隔符（`C:\dir\` 这种形式 SHParseDisplayName 解析不出 PIDL）。
         ④ 发出气泡的调用点：日志结束那条必须**把 g_logPath 交出去**（它才是可点的
            那条），别的提示类气泡不许带目标。 */
    {
        WCHAR tmpDir[MAX_PATH], logF[MAX_PATH], gone[MAX_PATH];
        tmpDir[0] = logF[0] = gone[0] = 0;
        GetTempPathW(MAX_PATH, tmpDir);
        {   /* 去掉 GetTempPath 一定会带的尾部分隔符：留着的话"退回目录"那条会解出
               `...\Temp\`，把尾斜杠判定和 `C:\dir\` 那条混在一起，说不清测的是哪个。 */
            size_t n = wcslen(tmpDir);
            while (n > 0 && (tmpDir[n - 1] == L'\\' || tmpDir[n - 1] == L'/'))
                tmpDir[--n] = 0;
        }
        _snwprintf_s(logF, MAX_PATH, _TRUNCATE, L"%s\\CapsEnhanceClickProbe.log", tmpDir);
        _snwprintf_s(gone, MAX_PATH, _TRUNCATE,
                     L"%s\\CapsEnhanceClickProbe__absent__.log", tmpDir);
        DeleteFileW(gone);                       /* 前置：它必须真的不存在 */
        FILE* fp = NULL;
        if (_wfopen_s(&fp, logF, L"wb") == 0 && fp) { fputs("x", fp); fclose(fp); }
        CHECK(GetFileAttributesW(logF) != INVALID_FILE_ATTRIBUTES,
              "AJ: 前置条件 —— 造出一个真实的日志文件");

        /* ③ 路径解析规则（纯函数，不碰 shell、不碰线程） */
        {
            WCHAR out[PROC_PATH_MAX]; BOOL dirOnly = FALSE;
            CHECK(FolderResolveTarget(logF, out, PROC_PATH_MAX, &dirOnly) != FALSE &&
                  !dirOnly && _wcsicmp(out, logF) == 0,
                  "AJ: 文件在 -> 原样返回（要选中的就是它）");

            CHECK(FolderResolveTarget(gone, out, PROC_PATH_MAX, &dirOnly) != FALSE &&
                  dirOnly && _wcsicmp(out, tmpDir) == 0,
                  "AJ: 文件不在 -> 退回它所在的目录（至少把文件夹打开）");
            CHECK(out[0] && out[wcslen(out) - 1] != L'\\',
                  "AJ: 退回来的目录不带尾部分隔符（tmpDir 本就去过尾斜杠）");

            CHECK(FolderResolveTarget(tmpDir, out, PROC_PATH_MAX, &dirOnly) != FALSE &&
                  dirOnly && out[wcslen(out) - 1] != L'\\',
                  "AJ: 给的就是目录 -> 仍在（去掉尾部分隔符）");
            CHECK(FolderResolveTarget(NULL, out, PROC_PATH_MAX, &dirOnly) == FALSE,
                  "AJ: 空路径 -> 失败（不崩、不假装成功）");
            CHECK(FolderResolveTarget(L"", out, PROC_PATH_MAX, &dirOnly) == FALSE,
                  "AJ: 空字符串 -> 失败");
            CHECK(FolderResolveTarget(L"no_dir_here.log", out, PROC_PATH_MAX, &dirOnly) == FALSE,
                  "AJ: 只有文件名、没有目录 -> 失败（没有「所在文件夹」可开）");
        }

        /* ④ 接线：真的走一遍"开始记录 -> 结束记录"，看发出的那条气泡记了什么 */
        {
            CHECK(g_balloonPath[0] == 0, "AJ: 前置条件 —— 初始没有可点击的目标");

            Key(CLI_CAPS, TRUE); Pump(5);
            Sim(VK_OEM_3, TRUE); Pump(30);          /* C+` 开始记录 */
            Sim(VK_OEM_3, FALSE); Pump(10);
            CHECK(g_recording.load(), "AJ: 前置条件 —— 已进入记录模式");
            CHECK(g_balloonPath[0] == 0,
                  "AJ: 开始记录那条气泡不可点（它是隐私提示，没有「所在位置」可开）");

            Sim(VK_OEM_3, TRUE); Pump(30);          /* C+` 结束记录 */
            Sim(VK_OEM_3, FALSE); Pump(10);
            Key(CLI_CAPS, FALSE); Pump(10);
            CHECK(!g_recording.load(), "AJ: 前置条件 —— 已退出记录模式");
            CHECK(g_balloonPath[0] != 0 && _wcsicmp(g_balloonPath, g_logPath) == 0,
                  "AJ: 结束记录的气泡记下了日志文件路径（这条才是可点的）");
        }

        /* ①② 点击：认出 NIN_BALLOONUSERCLICK，且不弹托盘菜单、只生效一次 */
        {
            BOOL wasOpen = g_menuOpen;
            TrayClick(NIN_BALLOONUSERCLICK);
            Pump(30);
            CHECK(g_balloonPath[0] == 0,
                  "AJ: 点击后目标被清掉（一次点击只生效一次）");
            CHECK(g_menuOpen == wasOpen,
                  "AJ: 点气泡 -> 不弹托盘菜单（点通知跳出菜单是错的行为）");

            TrayClick(NIN_BALLOONUSERCLICK);        /* 同一条气泡再点一次 */
            Pump(30);
            CHECK(g_menuOpen == wasOpen, "AJ: 同一条气泡重复点 -> 不重复动作");
        }

        /* ① 点图标必须**仍然**弹菜单（与上一条是相反方向） */
        {
            BOOL wasOpen = g_menuOpen;
            TrayClick(WM_RBUTTONUP);
            BOOL fired = FALSE;
            for (int i = 0; i < 40 && !fired; i++) {   /* 最多等 400ms */
                Pump(10);
                fired = (g_menuOpen != wasOpen);
            }
            CHECK(fired, "AJ: 点图标（右键抬起）-> 仍然弹托盘菜单");
        }

        DeleteFileW(logF);
    }

    printf(fails ? "\n== %d 项失败 ==\n" : "\n== 全部通过 ==\n", fails);
    return fails;
}