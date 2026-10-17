/* ============================================================
   wheelprobe.c — 滚轮落点计数探针（一次性诊断工具，不进主线）

   为什么要它：排查"按住 Caps 滚轮**有时候不加速**"时，CapsEnhance 自己的
   记录模式只能看到**前半段** —— hook.c 那句"滚轮 纵 delta=… caps=…"说明
   "系统把这一格送进了钩子"，act.c 的"滚轮补发/注入"说明"我们决定补几格"。
   它看不到**终点**：补发的格子有没有真的派发到目标窗口、有没有被系统合并、
   有没有在目标侧丢掉。而"不加速"恰恰是终点侧的现象。

   本工具同时给出三个数：
     ① 自己挂一个**只读** WH_MOUSE_LL，按 LLMHF_INJECTED 分流，分别数
        "物理格"与"注入格"。它与 CapsEnhance 的钩子在同一条钩子链上 ——
        所以即使 CapsEnhance 的钩子被系统跳过（超时/不在等消息状态），
        这里仍然能记到。这是"钩子到底漏没漏事件"的独立旁证。
     ② 本窗口收到的 WM_MOUSEWHEEL 条数与 delta 合计 —— 合并之后的真相。

   三种结果怎么读（配合 CapsEnhance 的记录日志）：
     · 日志里有 N 格、探针的物理格也是 N，但"本窗口 delta 合计"只有 N*120
       -> 补发没生效（SendInput 被拒 / 队列丢包）—— 问题在我们这边；
     · 日志里只有 M < N 格，探针的物理格是 N
       -> 有事件没进 CapsEnhance 的钩子（低级钩子被系统跳过）；
     · "本窗口 delta 合计" = N*WheelScrollMul*120（条数 1 或 N 都算正常）
       -> 补发全部到达，链路没问题 —— 该去看目标程序自己的滚动实现。

   用法：
     wheelprobe.exe                    手动：窗口出现并把光标挪到中心，滚轮随便滚
     wheelprobe.exe --slow 80          模拟"目标程序泵消息很慢"（复现合并行为）
     wheelprobe.exe --auto-close 3000  3 秒后自己关掉
     wheelprobe.exe --out x.txt        改写到指定文件
   Esc 或关窗即退出。

   ★ 汇总**默认就会自动写文件**，不用去抄窗口上的字：默认落到本 exe 同目录的
     `wheelprobe.out.txt`，每 300ms 刷新一次（有变化才写），窗口标题里也显示该路径。
     窗口上的文字只是给人瞄一眼的，**以文件内容为准**（截图/OCR 会丢字符）。窗口标题里也实时显示汇总。

   ★ 本文件是**诊断工具**，只读地计数、不做任何决策、不注入任何输入。
   ★ 钩子侧那两个数是**尽力而为**：本线程一旦不在等消息状态（重绘、写标题、
     --slow 的 Sleep），系统就会跳过它的钩子回调，那一格就漏记了。所以
     **以"本窗口"那两个数为准**，钩子侧只用来对账"物理格是不是比
     CapsEnhance 日志里的多"（漏记只会让钩子侧偏少，不会凭空变多）。
   ============================================================ */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>

#define PROBE_CLASS L"CapsEnhanceWheelProbe"

/* ---------- 钩子侧计数（物理 / 注入分开） ---------- */
static int g_hkPhysV = 0, g_hkPhysVSum = 0;
static int g_hkInjV = 0,  g_hkInjVSum = 0;
static int g_hkPhysH = 0, g_hkPhysHSum = 0;
static int g_hkInjH = 0,  g_hkInjHSum = 0;

/* ---------- 窗口侧计数（合并之后） ---------- */
static int g_msgV = 0, g_msgVSum = 0;
static int g_msgH = 0, g_msgHSum = 0;

/* 最近若干条纵轴消息：delta + 与上一条的间隔（毫秒）。
   "被合并"表现为间隔极小、delta 变大；"丢格"表现为 delta 合计不够。 */
#define RECENT 16
static int g_recDelta[RECENT];
static ULONGLONG g_recGap[RECENT];
static int g_recN = 0;
static ULONGLONG g_lastMsgTick = 0;

static volatile LONG g_slowMs = 0;              /* 模拟慢泵（毫秒） */
static DWORD g_autoCloseMs = 0;                 /* --auto-close：到点自己关 */
static BOOL  g_hookOk = FALSE;                  /* 计数钩子是否装上（汇总要如实写） */
static WCHAR g_outPath[MAX_PATH] = L"";
static HWND  g_hwnd = NULL;
static HHOOK g_hook = NULL;

/* 只读计数钩子：不做任何决策、不吞任何事件（一律 CallNextHookEx）。
   CapsEnhance 的钩子被跳过时这里仍能记到 —— 这正是拿它当旁证的理由。 */
static LRESULT CALLBACK CountProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION &&
        (wParam == WM_MOUSEWHEEL || wParam == WM_MOUSEHWHEEL)) {
        const MSLLHOOKSTRUCT* m = (const MSLLHOOKSTRUCT*)lParam;
        if (m) {
            int d = (int)(short)HIWORD(m->mouseData);
            BOOL inj = (m->flags & LLMHF_INJECTED) != 0;
            if (wParam == WM_MOUSEWHEEL) {
                if (inj) { g_hkInjV++;  g_hkInjVSum  += d; }
                else     { g_hkPhysV++; g_hkPhysVSum += d; }
            } else {
                if (inj) { g_hkInjH++;  g_hkInjHSum  += d; }
                else     { g_hkPhysH++; g_hkPhysHSum += d; }
            }
        }
    }
    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

/* 安全追加：_snprintf_s 截断时返回 -1，直接累加会把游标推成负数。 */
static char* Ap(char* p, char* end, const char* fmt, ...) {
    if (p >= end) return end;
    va_list ap;
    va_start(ap, fmt);
    int w = _vsnprintf_s(p, (size_t)(end - p), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (w < 0) return end;
    return p + w;
}

static void BuildSummary(char* buf, int cap) {
    char* end = buf + cap;
    char* p = buf;
    if (cap <= 0) return;
    *buf = 0;
    p = Ap(p, end, "钩子=%s\r\n", g_hookOk ? "已安装" : "未安装（只有窗口侧的数）");
    p = Ap(p, end, "物理滚轮(钩子) 纵条数=%d 纵delta合计=%d 横条数=%d 横delta合计=%d\r\n",
           g_hkPhysV, g_hkPhysVSum, g_hkPhysH, g_hkPhysHSum);
    p = Ap(p, end, "注入滚轮(钩子) 纵条数=%d 纵delta合计=%d 横条数=%d 横delta合计=%d\r\n",
           g_hkInjV, g_hkInjVSum, g_hkInjH, g_hkInjHSum);
    p = Ap(p, end, "本窗口 纵条数=%d 纵delta合计=%d\r\n", g_msgV, g_msgVSum);
    p = Ap(p, end, "本窗口 横条数=%d 横delta合计=%d\r\n", g_msgH, g_msgHSum);
    p = Ap(p, end, "最近(纵) %d 条:", g_recN);
    for (int i = 0; i < g_recN; i++)
        p = Ap(p, end, " %d@%llu", g_recDelta[i], (unsigned long long)g_recGap[i]);
    p = Ap(p, end, "\r\n");
    (void)p;
}

static void WriteOut(BOOL toStdout) {
    char buf[1024];
    BuildSummary(buf, (int)sizeof(buf));
    if (toStdout) {
        fputs(buf, stdout);
        fflush(stdout);
    }
    if (g_outPath[0]) {
        /* 每次写完就关：外部编辑器/脚本随时能读，也不会被我们占着句柄 */
        FILE* f = NULL;
        if (_wfopen_s(&f, g_outPath, L"wb") == 0 && f) {
            fputs(buf, f);
            fclose(f);
        }
    }
}

/* 有变化才写盘：300ms 一次的定时器调用它，于是"滚完直接去看文件"就是最新的。
   不用每来一条消息就写 —— 那样每次滚轮都带一次文件 I/O，反而干扰被测的时序。 */
static int  g_written = -1;
static int  DirtyKey(void) {
    return g_msgV * 1000003 + g_msgH * 10007 + g_hkPhysV * 101 + g_hkInjV * 7
           + g_msgVSum + g_msgHSum + g_hkPhysVSum + g_hkInjVSum + g_recN;
}
static void WriteIfDirty(void) {
    int k = DirtyKey();
    if (k == g_written) return;
    g_written = k;
    WriteOut(FALSE);
}

/* 窗口标题实时显示汇总 + 落盘文件名（手动用时不用去翻文件，也知道文件在哪）。 */
static void RefreshTitle(void) {
    WCHAR w[1024];
    char  a[900];
    char  name[300] = "(未落盘)";
    if (g_outPath[0]) {
        const WCHAR* slash = wcsrchr(g_outPath, L'\\');
        WideCharToMultiByte(CP_UTF8, 0, slash ? slash + 1 : g_outPath, -1,
                            name, (int)sizeof(name), NULL, NULL);
    }
    _snprintf_s(a, sizeof(a), _TRUNCATE,
        "wheelprobe | 本窗口 纵 %d 条/%d 横 %d 条/%d | 物理纵 %d 注入纵 %d | 慢泵 %ldms | 落盘: %s",
        g_msgV, g_msgVSum, g_msgH, g_msgHSum, g_hkPhysV, g_hkInjV, g_slowMs, name);
    MultiByteToWideChar(CP_UTF8, 0, a, -1, w, 1024);
    SetWindowTextW(g_hwnd, w);
}

static LRESULT CALLBACK ProbeProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        /* 消息里 delta 在 wParam 的高字（与钩子的 mouseData 同一个约定） */
        int d = (int)(short)HIWORD(w);
        ULONGLONG now = GetTickCount64();
        ULONGLONG gap = (g_lastMsgTick && g_recN) ? (now - g_lastMsgTick) : 0;
        g_lastMsgTick = now;
        if (g_recN < RECENT) {
            g_recDelta[g_recN] = d;
            g_recGap[g_recN] = gap;
            g_recN++;
        }
        if (m == WM_MOUSEWHEEL) { g_msgV++; g_msgVSum += d; }
        else                    { g_msgH++; g_msgHSum += d; }
        /* 模拟"目标程序泵消息很慢"：这期间到达的滚轮会被系统合并成一条
           （delta 相加），正好复现 send.c 顶部记下的那种行为。
           ★ 注意：Sleep 期间本线程不在等消息状态，自己的计数钩子也会被跳过，
             所以 --slow 下"钩子侧"的两个数会偏低，看"本窗口"那两个数。 */
        if (g_slowMs > 0) Sleep((DWORD)g_slowMs);
        /* **不在这里刷标题**：SetWindowTextW 是同步的、可能阻塞几十毫秒，
           而本线程一旦不在等消息状态，自己的计数钩子就会被系统跳过 ——
           那会把"注入格"数成 0（实测发生过）。标题改由定时器慢慢刷。 */
        InvalidateRect(h, NULL, TRUE);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        char  a[1024];
        WCHAR wa[1024];
        BuildSummary(a, (int)sizeof(a));
        MultiByteToWideChar(CP_UTF8, 0, a, -1, wa, 1024);
        RECT r;
        GetClientRect(h, &r);
        r.left += 12; r.top += 12; r.right -= 12; r.bottom -= 12;
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, wa, -1, &r, DT_LEFT | DT_TOP | DT_WORDBREAK);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) { DestroyWindow(h); return 0; }
        break;
    case WM_TIMER:
        if (w == 2) {                     /* 慢刷：标题 + 落盘（见滚轮分支的说明） */
            RefreshTitle();
            WriteIfDirty();
            return 0;
        }
        DestroyWindow(h);                 /* id 1 = --auto-close */
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = NULL; }
        WriteOut(TRUE);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--out") && i + 1 < argc) {
            MultiByteToWideChar(CP_UTF8, 0, argv[++i], -1, g_outPath, MAX_PATH);
        } else if (!strcmp(argv[i], "--slow") && i + 1 < argc) {
            InterlockedExchange(&g_slowMs, (LONG)atoi(argv[++i]));
        } else if (!strcmp(argv[i], "--auto-close") && i + 1 < argc) {
            g_autoCloseMs = (DWORD)atoi(argv[++i]);
        }
    }

    /* 没给 --out 就默认写到本 exe 同目录的 wheelprobe.out.txt：手工排障时不用抄
       窗口上的字（截图/OCR 会丢字符 —— 实测就丢过），滚完直接看文件。 */
    if (!g_outPath[0]) {
        WCHAR dir[MAX_PATH] = L"";
        DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            WCHAR* slash = wcsrchr(dir, L'\\');
            if (slash) {
                *(slash + 1) = 0;
                _snwprintf_s(g_outPath, MAX_PATH, _TRUNCATE,
                             L"%swheelprobe.out.txt", dir);
            }
        }
    }

    HINSTANCE hi = GetModuleHandleW(NULL);
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = ProbeProc;
    wc.hInstance = hi;
    wc.lpszClassName = PROBE_CLASS;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    if (!RegisterClassW(&wc)) return 2;

    /* 落在主显示器工作区中央：这样"光标移进去就滚"与"悬停滚动"两条路都能命中 */
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int ww = 560, wh = 300;
    int x = (sw - ww) / 2, y = (sh - wh) / 2;
    g_hwnd = CreateWindowExW(WS_EX_TOPMOST, PROBE_CLASS, L"wheelprobe",
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                             x, y, ww, wh, NULL, NULL, hi, NULL);
    if (!g_hwnd) return 3;

    /* 滚轮消息落到"光标下的窗口"（默认开着"悬停滚动"）或"前台窗口"，
       两条都铺上：置顶 + 抢前台 + 把光标挪到窗口中心。 */
    SetForegroundWindow(g_hwnd);
    SetCursorPos(x + ww / 2, y + wh / 2);

    g_hook = SetWindowsHookExW(WH_MOUSE_LL, CountProc, hi, 0);
    g_hookOk = (g_hook != NULL);
    if (!g_hookOk) {
        fputs("[warn] WH_MOUSE_LL 装不上：只能给出窗口侧的数\r\n", stdout);
    }

    if (g_autoCloseMs) SetTimer(g_hwnd, 1, g_autoCloseMs, NULL);
    SetTimer(g_hwnd, 2, 300, NULL);      /* 慢刷：标题 + 落盘（别在滚轮回调里做同步 I/O） */
    RefreshTitle();
    WriteOut(FALSE);                     /* 先把"还没滚"的初始汇总落一份，文件一开就有 */

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
