/* ================= 把"焦点窗口"挪到鼠标光标处（C + Q） =================
   两个真实痛点，一个动作解决：
     · 多显示器：鼠标在 B 屏上按一下，当前窗口就跳过去 —— 不用拖、也不用
       Win+Shift+方向键（后者要求窗口是自己"激活"的，且只在多屏才有效）；
     · 单显示器：窗口被拖到屏幕外、拽不回来（标题栏落在可见区域之外），
       按一下让它在光标处重新出现。

   落点规则（`WinComputePlacement`，纯函数、可单测）：
     **窗口左上角对齐光标**，再把整个窗口钳进"光标所在显示器的工作区"。
     为什么不用"中心对齐"：居中必须先按尺寸换算（`左上角 = 光标 − 尺寸/2`），
     而尺寸是**对方的** —— 它跨屏时可能自己改尺寸（实测 CLaunch：125% 屏上
     578x437，挪到 150% 屏上变 495x374），按旧尺寸算出的中心立刻就偏了，
     表现出来就是"按一下没完全过去、再按一下才对"。左上角对齐**不依赖尺寸**。
     钳制顺序是先"右下"后"左上"：窗口比工作区还大时让左上角对齐，至少保证
     标题栏可见 —— 那种情况钳不出更优解，但也不能把窗口推到屏幕外。

   多显示器：`MonitorFromPoint(..., MONITOR_DEFAULTTONEAREST)`
   （官方文档：pt 是**虚拟屏坐标**；点不在任何显示器内时返回**最近**的那块，
   于是"光标落在两屏之间的缝里""某块屏刚被拔掉、坐标悬空"都能兜住）。
   工作区取 `MONITORINFO.rcWork`（已排除任务栏），所以窗口不会被压到托盘上，
   钳出来的位置一定是"整窗可见"的。

   ★ 移动是"请求 + 回读锚点 + 重试"，最多 3 次。回读的是**左上角**是否落在我们要
   的位置上（右/下两边不判：对方改尺寸是它的自由，左上角对了窗口就是"从这里开始"）。
   左上角对齐本身已经免疫"对方改尺寸"，所以这一步针对的是另一种程序：**收到移动
   请求后又自己挪走**（带吸附/跟随行为的启动器、面板类工具）。那就再发一次，最多
   3 次，最后把实际落点写进日志 —— "发了对方未必认，动完回读一次"是本项目的老规矩
   （见 `SendArrowsStrict` 的补正、`EM_SETSEL` 的回读）。

   ★ 坐标系与 DPI：本进程在 `WinMain` 一开始就声明 **Per-Monitor V2** 感知
   （`WinInitDpiAwareness`），于是 `GetCursorPos` / `MonitorFromPoint` /
   `GetWindowRect` / `SetWindowPos` 全部落在**物理像素**空间 —— 与低级鼠标钩子
   给的 `MSLLHOOKSTRUCT.pt`（官方定义就是 per-monitor-aware screen coordinates）
   是同一个空间。**这一点是必须的**：不声明感知时系统会把整个桌面按 96DPI 虚拟化，
   只有本进程的 API 用虚拟坐标，钩子仍给物理坐标，两边差一个缩放比 ——
   表现为"只能拖有焦点的窗口""窗口比光标多跑一截"。声明之后，混合缩放的多屏也不
   再有虚拟空间那道"缝"。
   将来若给本体声明 Per-Monitor 感知，这段代码不用改：那只是把同一个自洽关系
   挪到物理像素空间。（依据：MS「High DPI Desktop Application Development on
   Windows」——"Windows scales and converts DPI-sensitive information from some
   APIs to the coordinate space of the calling thread"。）

   ★ 调用位置：act.c 的**执行线程**（`RunJob`）。`SetWindowPos` 会向窗口过程
   发 WM_WINDOWPOSCHANGING/CHANGED，对方线程卡住时同步调用能一直等下去 ——
   这条路径绝不能落在主线程（那正是"钩子被系统静默摘除"的根因，见 act.c 顶部）。
   因此这里一律带 `SWP_ASYNCWINDOWPOS`（请求投递过去、本线程不等），代价是
   **回读会抢在请求生效之前** —— 所以回读前先 `Sleep(MOVE_SETTLE_MS)` 让一小会儿。
   顺带解决了"最大化必须等位置生效"的问题：确认回读一致之后才 `SW_MAXIMIZE`。

   本文件只做"窗口几何"，不认识键盘/键位：触发键（`Q`）在 keymap.c 的动作表里，
   分派在 act.c。 */

#include "cap.h"

#include <algorithm>   /* std::min / std::max：钳制的意图比嵌套三目清楚 */
#include <stdlib.h>    /* llabs */
#include <string.h>
#include <wchar.h>

/* 移动最多试几次 / 每次重试之间等多久 / 判定"已就位"的容差（像素）。
   容差留 3px：程序自己可能有 1~2px 的对齐习惯，但差出几十像素就说明它真的
   改了尺寸或位置（那时重试会按新尺寸重算落点）。 */
#define MOVE_TRIES      3
#define MOVE_SETTLE_MS  25
#define MOVE_TOLERANCE  3

/* 桌面壳与任务栏也是"前台窗口"的候选（Win+T、点任务栏都会），但挪它们毫无
   意义，而且会真把任务栏搬走。按类名挡掉（桌面 = Progman/WorkerW）。 */
BOOL WinIsShellClass(HWND hwnd) {
    WCHAR cls[64] = L"";
    if (!GetClassNameW(hwnd, cls, 64)) return FALSE;
    return wcscmp(cls, L"Shell_TrayWnd") == 0
        || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0
        || wcscmp(cls, L"Progman") == 0
        || wcscmp(cls, L"WorkerW") == 0;
}

/* 类名转窄字符，只为日志（%s 比 %ls 稳，也不给分析器留口实）。 */
static void ClassNameToAnsi(HWND hwnd, char* out, int cap) {
    WCHAR w[64] = L"";
    out[0] = 0;
    if (!GetClassNameW(hwnd, w, 64)) return;
    WideCharToMultiByte(CP_ACP, 0, w, -1, out, cap, NULL, NULL);
    out[cap - 1] = 0;
}

/* 两个点是否"基本重合"（容差见 MOVE_TOLERANCE）。只比**锚点**（左上角）：
   右/下两边不参与判定 —— 程序被挪过去之后自己改尺寸是它的自由，那不是我们的
   落点错了（左上角对了，窗口就是"从这里开始"）。 */
static BOOL AnchorAt(const RECT* got, POINT want, int tol) {
    return llabs((long long)got->left - (long long)want.x) <= tol
        && llabs((long long)got->top  - (long long)want.y) <= tol;
}

/* 落点计算（纯函数：不碰窗口、不碰屏幕，所以多显示器几何能被单测覆盖）。
   out = 窗口左上角。

   **规则：左上角对齐光标**（不是"中心对齐光标"）。理由：居中要算
   `左上角 = 光标 − 尺寸/2`，而尺寸是**对方的** —— 有的程序跨屏会自己改尺寸
   （实测 CLaunch 在 125% 屏上 578x437、到 150% 屏上变 495x374），按旧尺寸算出的
   中心立刻就偏了（实测偏 41px），用户看到的是"按一下没完全过去、再按一下才对"。
   左上角对齐**不依赖尺寸**，它怎么改都不影响落点。
   （尺寸仍然要读，但**只为把整窗钳进工作区**：窗口比工作区大时钳不出更优解，
   至少保证左上角可见 —— 钳制需要尺寸，"落点"不需要。） */
void WinComputePlacement(const RECT* work, POINT cursor, int w, int h, POINT* out) {
    LONG x = cursor.x;                  /* 左上角对齐光标 */
    LONG y = cursor.y;
    x = std::min<LONG>(x, work->right - w);   /* 先钳右下：整窗别越过工作区 */
    y = std::min<LONG>(y, work->bottom - h);
    x = std::max<LONG>(x, work->left);        /* 再钳左上：窗口大于工作区时保左上角 */
    y = std::max<LONG>(y, work->top);
    out->x = x;
    out->y = y;
}

/* ================= 坐标空间：声明 DPI 感知 =================
   为什么这件事必须做、以及不做会怎样，见 `cap.h` 里 `WinInitDpiAwareness` 的说明。
   这里只记实现上要注意的：
     · **必须在创建任何窗口之前调用**（消息窗口 / 托盘 / 任何 HWND），否则系统已经
       按旧感知级别把窗口建好了，改感知也来不及；
     · 动态取 `SetProcessDpiAwarenessContext`（Win10 1607+）才能拿到 PMv2；
       拿不到就退回进程级 `SetProcessDPIAware`（Win8.1 之前）；
     · 一旦被清单/外部设过（`SetProcessDpiAwarenessContext` 返回 FALSE 且
       GetLastError == ERROR_ACCESS_DENIED），就不要再尝试 —— 那次调用已经失败。 */
typedef BOOL (WINAPI* PFN_SetProcessDpiAwarenessContext)(HANDLE);
typedef BOOL (WINAPI* PFN_SetProcessDPIAware)(void);
typedef UINT (WINAPI* PFN_GetAwarenessFromCtx)(HANDLE);
typedef HANDLE (WINAPI* PFN_GetThreadCtx)(void);
typedef BOOL (WINAPI* PFN_AreCtxEqual)(HANDLE, HANDLE);

/* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4（不引 shellscalingapi.h，
   那个头在旧 SDK 里可能没有）。 */
#define DPI_CTX_PMV2 ((HANDLE)(INT_PTR)-4)

void WinInitDpiAwareness(void) {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (!u) return;
    PFN_SetProcessDpiAwarenessContext setCtx =
        (PFN_SetProcessDpiAwarenessContext)GetProcAddress(u, "SetProcessDpiAwarenessContext");
    if (setCtx) {
        if (setCtx(DPI_CTX_PMV2)) return;
        /* 已经是别的感知级别（清单设过 / 已被调用过）→ 不再降级覆盖，
           免得把显式设好的级别改坏。 */
        if (GetLastError() == ERROR_ACCESS_DENIED) return;
    }
    PFN_SetProcessDPIAware setOld = (PFN_SetProcessDPIAware)GetProcAddress(u, "SetProcessDPIAware");
    if (setOld) setOld();
}

/* 自检：钩子给的是物理坐标，所以本进程也必须活在物理空间。
   ★ 判据用**显示模式尺寸**（`EnumDisplaySettings` 的 `dmPelsWidth`）：它**不被**
   DPI 虚拟化，永远返回真实像素；而本进程的 `SM_CXSCREEN` 在"不感知"时会被按系统
   DPI 缩小。两者不等 = 虚拟化生效 = 与钩子不在一个空间（本机 1462 vs 2558）。
   ★ **别用 `GetPhysicalCursorPos` 当判据**（这是我第一版的错）：实测它对本进程
   同样做虚拟化 —— 不感知时它与 `GetCursorPos` 一起返回 772,350（真实 1351,613），
   两边一起变，看起来"一致"，恰好把这个 bug 藏过去。
   比不出结果（拿不到 DEVMODE）时不报错：宁可不报，也不要误报。 */
BOOL WinCoordSpaceConsistent(void) {
    DEVMODEW dm;
    memset(&dm, 0, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(NULL, ENUM_CURRENT_SETTINGS, &dm)) return TRUE;
    if (dm.dmPelsWidth == 0) return TRUE;
    int w = GetSystemMetrics(SM_CXSCREEN);
    if (w <= 0) return TRUE;
    return (DWORD)w == dm.dmPelsWidth;
}

/* 当前感知级别名（日志用）。同样动态取，避免旧 SDK 缺符号。

   ★ 实测坑：`GetAwarenessFromDpiAwarenessContext` 对 **PerMonitor 与 PerMonitorV2
   都返回 2**（V2 的差别在行为上——非客户区缩放、子窗口 DPI 通知——不在这个枚举里），
   所以想区分 V2 必须用 `AreDpiAwarenessContextsEqual(ctx, PMv2)`。
   本函数先按上下文精确比，比不出来再退回枚举名。 */
const WCHAR* WinDpiAwarenessName(void) {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (!u) return L"未知";
    PFN_GetThreadCtx getCtx = (PFN_GetThreadCtx)GetProcAddress(u, "GetThreadDpiAwarenessContext");
    if (!getCtx) return L"未知";
    HANDLE ctx = getCtx();

    PFN_AreCtxEqual eq =
        (PFN_AreCtxEqual)GetProcAddress(u, "AreDpiAwarenessContextsEqual");
    if (eq) {
        /* 各上下文常量的原始值：-1 不感知 / -2 系统级 / -3 每显示器 / -4 每显示器V2 */
        if (eq(ctx, (HANDLE)(INT_PTR)-4)) return L"每显示器V2(PerMonitorV2)";
        if (eq(ctx, (HANDLE)(INT_PTR)-3)) return L"每显示器(PerMonitor)";
        if (eq(ctx, (HANDLE)(INT_PTR)-2)) return L"系统级(DPI_AWARE)";
        if (eq(ctx, (HANDLE)(INT_PTR)-1)) return L"不感知(DPI_UNAWARE)";
    }
    PFN_GetAwarenessFromCtx getAw =
        (PFN_GetAwarenessFromCtx)GetProcAddress(u, "GetAwarenessFromDpiAwarenessContext");
    if (getAw) {
        switch (getAw(ctx)) {
        case 0: return L"不感知(DPI_UNAWARE)";
        case 1: return L"系统级(DPI_AWARE)";
        case 2: return L"每显示器(PerMonitor 或 V2，本机 API 分不出)";
        default: break;
        }
    }
    return L"未知";
}

/* 移动**指定**窗口（不自己去找前台窗口：测试要能一次一个地验证它）。
   返回是否成功把移动请求交给了系统。 */
BOOL WinMoveWindowToCursor(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return FALSE;

    /* 最小化/最大化的窗口"挪不动"：
       · 最大化的窗口位置由系统按所在显示器算，(x,y) 直接忽略 —— 不还原就看不出变化；
       · 最小化的窗口 GetWindowRect 不是它的还原位置，照着算会把窗口放到错的地方。
       所以先还原成普通窗口。最大化的那个最后再最大化回去：此时还原位置已经在
       目标显示器上，于是它在新屏上重新铺满（等价于 Win+Shift+方向 的语义，
       而不是"顺手把用户的最大化取消掉"）。 */
    const BOOL wasMax = IsZoomed(hwnd) != FALSE;
    if (IsIconic(hwnd) || wasMax) ShowWindow(hwnd, SW_RESTORE);

    /* 声明区都放在**第一条 goto 之前**并用 C 风格赋值：`goto` 跳过"带初始化
       的局部声明"在 C++ 里是错（C2362），写成声明区就从结构上不会踩到。 */
    BOOL ok = FALSE;
    RECT wr, got;
    POINT cur, dst;
    int w = 0, h = 0, wFirst = 0, hFirst = 0;
    int tries = 0;
    MONITORINFO mi;
    HMONITOR hMon = NULL;
    memset(&mi, 0, sizeof(mi));
    memset(&wr, 0, sizeof(wr));
    memset(&got, 0, sizeof(got));
    memset(&cur, 0, sizeof(cur));
    memset(&dst, 0, sizeof(dst));

    if (!GetCursorPos(&cur)) goto done;
    hMon = MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);
    if (!hMon) goto done;
    mi.cbSize = sizeof(mi);              /* 不先填 cbSize，GetMonitorInfo 直接拒绝 */
    if (!GetMonitorInfoW(hMon, &mi)) goto done;

    for (int i = 0; i < MOVE_TRIES; i++) {
        if (!GetWindowRect(hwnd, &wr)) break;
        w = (int)(wr.right - wr.left);
        h = (int)(wr.bottom - wr.top);
        if (i == 0) { wFirst = w; hFirst = h; }      /* 只为日志：对方改没改尺寸 */
        WinComputePlacement(&mi.rcWork, cur, w, h, &dst);

        /* 坐标是**屏幕坐标**（顶层窗口；SetWindowPos 的文档写作 "client
           coordinates"，那是文档里沿用已久的措辞错误）。不改大小、不动 Z 序、
           不抢激活；ASYNCWINDOWPOS 见文件头 —— 回读前先让一小会儿。 */
        ok = SetWindowPos(hwnd, NULL, (int)dst.x, (int)dst.y, 0, 0,
                          SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                          SWP_ASYNCWINDOWPOS) != FALSE;
        tries = i + 1;
        if (!ok) break;                              /* 请求被拒（UIPI 等）：没什么可重试的 */
        Sleep(MOVE_SETTLE_MS);
        if (!GetWindowRect(hwnd, &got)) break;
        if (AnchorAt(&got, dst, MOVE_TOLERANCE)) break;   /* 锚点已就位 */
    }

    if (g_recording.load()) {
        char cls8[64];
        ClassNameToAnsi(hwnd, cls8, 64);
        CapLog("  [窗口] %s cls=%s 光标(%d,%d) 显示器[%d,%d..%d,%d] 工作区[%d,%d..%d,%d]",
               wasMax ? "最大化→挪屏后仍最大化" : "普通",
               cls8[0] ? cls8 : "?",
               (int)cur.x, (int)cur.y,
               (int)mi.rcMonitor.left, (int)mi.rcMonitor.top,
               (int)mi.rcMonitor.right, (int)mi.rcMonitor.bottom,
               (int)mi.rcWork.left, (int)mi.rcWork.top,
               (int)mi.rcWork.right, (int)mi.rcWork.bottom);
        CapLog("  [窗口]   尺寸%dx%d%s -> 请求左上角(%d,%d) 实际[%d,%d..%d,%d] 尝试%d次%s",
               wFirst, hFirst,
               (w != wFirst || h != hFirst) ? "（对方改过尺寸，落点不受影响）" : "",
               (int)dst.x, (int)dst.y,
               (int)got.left, (int)got.top, (int)got.right, (int)got.bottom,
               tries,
               AnchorAt(&got, dst, MOVE_TOLERANCE) ? "" : "   <<< 锚点未就位（对方又把它挪走了）");
    }

done:
    if (wasMax) ShowWindow(hwnd, SW_MAXIMIZE);   /* 失败也要把最大化状态还回去 */
    return ok;
}

/* 对外入口（C+Q 的动作）：把"焦点程序"的顶层窗口挪到光标处。
   焦点窗口由系统给（`GetForegroundWindow`），所以不需要我们自己维护状态 ——
   本程序从不改变前台窗口，用户按 C+Q 时前台就是他想挪的那个程序。 */
BOOL WinMoveFocusedToCursor(void) {
    HWND fg = GetForegroundWindow();
    if (!fg) return FALSE;

    /* 自己的窗口不算：托盘菜单打开时前台可能是本进程的窗口，而它是个专门
       用来挂托盘菜单的隐藏消息窗口，挪它没有任何意义。 */
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) return FALSE;

    /* 真正能挪的是**顶层**窗口：Chromium 系/UWP 的前台窗口往往是内容子窗口，
       挪子窗口只会把它从父窗口里撕出来。GA_ROOT = 该子窗口所属的顶层窗口。 */
    HWND top = GetAncestor(fg, GA_ROOT);
    if (!top) return FALSE;
    if (WinIsShellClass(top)) return FALSE;

    return WinMoveWindowToCursor(top);
}
