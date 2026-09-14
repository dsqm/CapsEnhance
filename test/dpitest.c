/* ================= 坐标空间（DPI 感知）回归测试 =================
   守的是一个**真实事故**：低级鼠标钩子给的 `MSLLHOOKSTRUCT.pt` 永远是**物理像素**
   （官方定义就是 per-monitor-aware screen coordinates），而**不声明 DPI 感知**的
   进程里 `GetCursorPos` / `WindowFromPoint` / `GetWindowRect` / `SetWindowPos` 用的
   是系统按 96DPI **虚拟化**过的坐标。两者差一个缩放比，于是 Caps+左键拖动会
   "指错窗口"（看起来只能拖有焦点的窗口）而且"位移比光标多跑一截"。
   本机（真实 2558x1319 @175%）实测：钩子 (1351,613) vs 不感知进程的 (772,350)。

   这个测试**必须在没有任何窗口的进程里跑**：`SetProcessDpiAwarenessContext` 的
   前提就是"建窗口之前"，所以它独立成一个 exe，不做别的。

   判据（两条，都对实机缩放敏感）：
     A) 声明之后，本进程坐标空间必须与钩子一致（GetCursorPos == GetPhysicalCursorPos）；
     B) 感知级别不能是"不感知"——否则 A 必然在缩放显示器上失败。
   在 100% 缩放的机器上 A 恒真（无害），在 125%/150%/175% 的机器上它就是那根保险丝。 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "cap.h"

/* 这个测试**独立成 exe**（不能在 test_chord 里跑：那会儿已经有窗口了，而
   SetProcessDpiAwarenessContext 的前提就是"建窗口之前"）。于是它要自己补上
   winmove.c 用到的两个 main.c 符号 —— 只是**链接桩**，不参与任何判定。
   若将来 winmove.c 又依赖了别的东西，这里会给出明确的 LNK2019，按需再补。 */
std::atomic<bool> g_recording{false};
void CapLog(const char* fmt, ...) { (void)fmt; }

static int fails = 0;
#define CHECK(cond, name) do { \
    if (cond) printf("  [OK]   %s\n", name); \
    else      { printf("  [FAIL] %s\n", name); fails++; } } while (0)

typedef BOOL (WINAPI* PFN_SetProcessDpiAwarenessContext)(HANDLE);
typedef UINT (WINAPI* PFN_GetAwarenessFromCtx)(HANDLE);
typedef HANDLE (WINAPI* PFN_GetThreadCtx)(void);

/* 控制台 printf 打印宽字符要受 locale 影响（"C" 区域下非 ASCII 会打印成空），
   所以这里自己转成 UTF-8 再打 —— 输出可读，判定仍用宽字符比较。 */
static void PrintW(const WCHAR* w) {
    char u8[256] = "";
    WideCharToMultiByte(CP_UTF8, 0, w, -1, u8, (int)sizeof(u8), NULL, NULL);
    fputs(u8, stdout);
}

/* 原始感知数值：0=不感知 1=系统级 2=每显示器 3=每显示器V2（诊断用） */
static int RawAwareness(void) {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (!u) return -1;
    PFN_GetThreadCtx g = (PFN_GetThreadCtx)GetProcAddress(u, "GetThreadDpiAwarenessContext");
    PFN_GetAwarenessFromCtx a =
        (PFN_GetAwarenessFromCtx)GetProcAddress(u, "GetAwarenessFromDpiAwarenessContext");
    if (!g || !a) return -1;
    return (int)a(g());
}

int main(void) {
    /* 声明之前的状态（**只报告，不断言**）：本机 175% 下应为"不一致"，
       100% 缩放的机器上本来就是一致的 —— 断言它会让测试与机器绑定。 */
    BOOL before = WinCoordSpaceConsistent();
    printf("  声明前：坐标空间 %s；感知级别 = ",
           before ? "一致（本机未开缩放）" : "**不一致（虚拟化生效，正是那个 bug 的土壤）**");
    PrintW(WinDpiAwarenessName());
    printf(" (原始值=%d)\n", RawAwareness());

    WinInitDpiAwareness();

    printf("  声明后：坐标空间 %s；感知级别 = ",
           WinCoordSpaceConsistent() ? "一致" : "**仍不一致**");
    PrintW(WinDpiAwarenessName());
    printf(" (原始值=%d)\n", RawAwareness());

    CHECK(WinCoordSpaceConsistent(),
          "声明之后本进程与钩子在同一坐标空间（拖动不会指错窗口/位移不会多跑）");
    CHECK(wcscmp(WinDpiAwarenessName(), L"不感知(DPI_UNAWARE)") != 0,
          "声明之后不再是 DPI 不感知");
    if (before == FALSE)
        CHECK(WinCoordSpaceConsistent(), "缩放显示器上确实由\"不一致\"变成了\"一致\"");

    /* 有 API 就该是 PMv2。注意：**不能用 GetAwarenessFromDpiAwarenessContext 判**
       ——它对 PerMonitor 与 PerMonitorV2 都返回 2（实测），必须看上下文名
       （WinDpiAwarenessName 内部用 AreDpiAwarenessContextsEqual）。
       没有该 API 说明是很老的系统，那时退回进程级感知也算合格 → 只在 API 存在时断言。 */
    {
        HMODULE u = GetModuleHandleW(L"user32.dll");
        BOOL hasApi = u && GetProcAddress(u, "SetProcessDpiAwarenessContext") != NULL;
        if (hasApi)
            CHECK(wcscmp(WinDpiAwarenessName(), L"每显示器V2(PerMonitorV2)") == 0,
                  "有 SetProcessDpiAwarenessContext 时应拿到 PerMonitorV2");
        else
            printf("  [--]   本机没有 SetProcessDpiAwarenessContext（很老的系统），跳过 PMv2 断言\n");
    }

    printf(fails ? "\n== %d 项失败 ==\n" : "\n== 全部通过 ==\n", fails);
    return fails;
}
