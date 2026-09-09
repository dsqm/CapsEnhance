/* memtest.c - UIA 通道内存行为压测。
 *
 * 背景：C+` 开始记录日志后进程内存涨到 ~22MB，结束记录不回落。
 * 怀疑两点：a) 每次跳词的 UIA 调用存在泄漏（会无界增长）；
 *           b) UIA/MSAA 的 COM 基础设施（DLL+缓存）首次使用后常驻（平台特性）。
 * 本工具对同一目标窗口连续调用 UiaGetTextCaret 数百次，每 100 次打印
 * 工作集，区分 a（线性增长）与 b（涨一次后平稳）。
 *
 * 用法：先在 Edge 打开 test/keytest.html（Chromium 走 UIA 通道），
 *       再运行 memtest.exe [次数]。
 */
#include "cap.h"
#include <stdio.h>
#include <psapi.h>
#include <stdlib.h>
#include <string.h>
#pragma comment(lib, "psapi.lib")

volatile BOOL g_recording = FALSE;
HHOOK g_hook = NULL;
void CapLog(const char* fmt, ...) { (void)fmt; }

#include "nav.c"
#include "send.c"

static HWND g_hwnd = NULL;
static BOOL CALLBACK FindCb(HWND h, LPARAM lp) {
    (void)lp;
    char ttl[512] = "";
    if (!IsWindowVisible(h)) return TRUE;
    if (!GetWindowTextA(h, ttl, 512)) return TRUE;
    if (strstr(ttl, "KEYTEST") == NULL) return TRUE;
    g_hwnd = h;
    return FALSE;
}

static BOOL BringFront(HWND h) {
    if (SetForegroundWindow(h)) return TRUE;
    DWORD my = GetCurrentThreadId();
    DWORD fg = GetWindowThreadProcessId(GetForegroundWindow(), NULL);
    BOOL ok = FALSE;
    if (fg && AttachThreadInput(my, fg, TRUE)) {
        ok = SetForegroundWindow(h);
        AttachThreadInput(my, fg, FALSE);
    }
    return ok;
}

static SIZE_T WS(void) {
    PROCESS_MEMORY_COUNTERS pmc;
    memset(&pmc, 0, sizeof(pmc));
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return pmc.WorkingSetSize;
    return 0;
}

int main(int argc, char** argv) {
    int N = (argc > 1) ? atoi(argv[1]) : 600;
    EnumWindows(FindCb, 0);
    if (!g_hwnd) {
        printf("no KEYTEST window. open test/keytest.html in Edge first.\n");
        return 1;
    }
    BringFront(g_hwnd);
    SetFocus(g_hwnd);
    Sleep(300);

    printf("target hwnd=0x%p  N=%d\n", (void*)g_hwnd, N);
    printf("  t=0            ws=%.1f MB\n", WS() / 1048576.0);

    /* 预热：首次 UIA 调用会加载 uiautomationcore 等基础设施 */
    WCHAR* text = NULL;
    int len = 0, caret = 0;
    if (UiaGetTextCaret(g_hwnd, &text, &len, &caret)) free(text);
    printf("  warm-up        ws=%.1f MB  (len=%d caret=%d)\n",
           WS() / 1048576.0, len, caret);

    int okCnt = 0, lastCaret = caret, drift = 0;
    SIZE_T first = WS();
    for (int i = 1; i <= N; i++) {
        text = NULL;
        if (UiaGetTextCaret(g_hwnd, &text, &len, &caret)) {
            okCnt++;
            if (caret != lastCaret) { drift++; lastCaret = caret; }
        }
        if (text) free(text);
        if (i % 100 == 0)
            printf("  n=%-4d ok=%d     ws=%.1f MB  (delta %+.0f KB)\n",
                   i, okCnt, WS() / 1048576.0,
                   (double)(WS() - first) / 1024.0);
    }
    printf("\n结果: ok=%d/%d  caret漂移=%d  工作集增量=%.0f KB\n",
           okCnt, N, drift, (double)(WS() - first) / 1024.0);
    printf(okCnt == N && drift == 0
               ? "若增量在几百 KB 内且不随次数线性增长 => COM 基础设施常驻，非泄漏\n"
               : "若随次数线性增长 => 存在泄漏，需修\n");
    return 0;
}
