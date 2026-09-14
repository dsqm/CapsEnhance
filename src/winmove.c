/* ================= 把"焦点窗口"挪到鼠标光标处（C + Q） =================
   两个真实痛点，一个动作解决：
     · 多显示器：鼠标在 B 屏上按一下，当前窗口就跳过去 —— 不用拖、也不用
       Win+Shift+方向键（后者要求窗口是自己"激活"的，且只在多屏才有效）；
     · 单显示器：窗口被拖到屏幕外、拽不回来（标题栏落在可见区域之外），
       按一下让它在光标处重新出现。

   落点规则（`WinComputePlacement`，纯函数、可单测）：
     以窗口**中心**对齐光标，再把整个窗口钳进"光标所在显示器的工作区"。
     钳制顺序是先"右下"后"左上"：窗口比工作区还大时让左上角对齐，至少保证
     标题栏可见 —— 那种情况钳不出更优解，但也不能把窗口推到屏幕外。

   多显示器：`MonitorFromPoint(..., MONITOR_DEFAULTTONEAREST)`
   （官方文档：pt 是**虚拟屏坐标**；点不在任何显示器内时返回**最近**的那块，
   于是"光标落在两屏之间的缝里""某块屏刚被拔掉、坐标悬空"都能兜住）。
   工作区取 `MONITORINFO.rcWork`（已排除任务栏），所以窗口不会被压到托盘上，
   钳出来的位置一定是"整窗可见"的。

   ★ 移动是"请求 + 回读校验 + 按新尺寸重算"，最多 3 次 —— 因为**对方程序可能
   自己改尺寸**：实测 CLaunch 在 125% 屏上 578x437、挪到 150% 屏上变成 495x374
   （它保持物理尺寸不变）。按旧尺寸算出的中心一遇到它改尺寸就偏了几十像素，
   用户看到的就是"按一下没完全过去，再按一下才对"。回读它的新尺寸、重算落点，
   把这件事变成一次点击完成 —— "发了对方未必认，动完回读一次"是本项目的老规矩
   （见 `SendArrowsStrict` 的补正、`EM_SETSEL` 的回读）。

   ★ 坐标系与 DPI：本进程**没有声明 DPI 感知**，于是 Windows 把整个桌面按系统
   DPI 统一虚拟化后交给它 —— `GetCursorPos` / `MonitorFromPoint` /
   `GetWindowRect` / `SetWindowPos` 用的是**同一个**（本进程的）虚拟坐标空间，
   读与写自洽。混合缩放的两块屏上，这个空间会有"缝"（实测主屏 0..1463、
   副屏 1707..3414），但读写都在同一空间里，所以仍然落到光标处。
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
static BOOL IsShellClass(HWND hwnd) {
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

/* 两个矩形是否"基本重合"（容差见 MOVE_TOLERANCE）。 */
static BOOL NearRect(const RECT* a, const RECT* b, int tol) {
    return llabs((long long)a->left   - (long long)b->left)   <= tol
        && llabs((long long)a->top    - (long long)b->top)    <= tol
        && llabs((long long)a->right  - (long long)b->right)  <= tol
        && llabs((long long)a->bottom - (long long)b->bottom) <= tol;
}

/* 落点计算（纯函数：不碰窗口、不碰屏幕，所以多显示器几何能被单测覆盖）。
   out = 窗口左上角。 */
void WinComputePlacement(const RECT* work, POINT cursor, int w, int h, POINT* out) {
    LONG x = cursor.x - w / 2;          /* 中心对齐光标 */
    LONG y = cursor.y - h / 2;
    x = std::min<LONG>(x, work->right - w);   /* 先钳右下：整窗别越过工作区 */
    y = std::min<LONG>(y, work->bottom - h);
    x = std::max<LONG>(x, work->left);        /* 再钳左上：窗口大于工作区时保左上角 */
    y = std::max<LONG>(y, work->top);
    out->x = x;
    out->y = y;
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
    RECT wr, want, got;
    POINT cur, dst;
    int w = 0, h = 0, wFirst = 0, hFirst = 0;
    int tries = 0;
    MONITORINFO mi;
    HMONITOR hMon = NULL;
    memset(&mi, 0, sizeof(mi));
    memset(&wr, 0, sizeof(wr));
    memset(&want, 0, sizeof(want));
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
        want.left = dst.x; want.top = dst.y;
        want.right = dst.x + w; want.bottom = dst.y + h;

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
        if (NearRect(&got, &want, MOVE_TOLERANCE)) break;   /* 已就位 */
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
        CapLog("  [窗口]   尺寸%dx%d -> 请求(%d,%d) 实际[%d,%d..%d,%d] 尝试%d次%s%s",
               wFirst, hFirst, (int)dst.x, (int)dst.y,
               (int)got.left, (int)got.top, (int)got.right, (int)got.bottom,
               tries,
               NearRect(&got, &want, MOVE_TOLERANCE) ? "" : "   <<< 未就位",
               (w != wFirst || h != hFirst) ? "   （对方改了尺寸，已按新尺寸重算）" : "");
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
    if (IsShellClass(top)) return FALSE;

    return WinMoveWindowToCursor(top);
}
