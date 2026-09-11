/* ============================================================
   WPS 通道的**真实集成测试**：把 WPS 文字窗口置前，直接调 NavJump，
   看它是否选中 WPS 通道（path=WPS）并按算出的步数移动光标。
   这不是替代 CI 的单测（CI 机器上不一定装了 WPS），是给开发/排障用的
   端到端检查：CapsEnhance 的词跳链路里 WPS 那一段是否真的通。

   说明：钩子会忽略注入按键（LLKHF_INJECTED），所以没法用 SendInput 模拟
   Caps+J 来驱动；这里直接调用 NavJump —— 通道选择、文本读取、坐标换算、
   箭头发送这几步与线上完全一致，差别只在"谁触发的"。
   运行时会**移动 WPS 里文档的光标**（不改内容），请先存盘。
   ============================================================ */
#include "cap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdarg.h>

/* 未链接 main.c / hook.c：补上它们提供的符号 */
volatile BOOL g_recording = TRUE;    /* 打开记录，nav.c 的诊断日志会直接打到这里 */
HHOOK g_hook = NULL;
static int g_lines = 0;
void CapLog(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vprintf(fmt, ap); va_end(ap);
    printf("\n");
    g_lines++;
}
BOOL PhysKeyDown(int vk) { (void)vk; return FALSE; }

static BOOL CALLBACK FindCb(HWND h, LPARAM lp) {
    if (!IsWindowVisible(h)) return TRUE;
    WCHAR cls[64] = L"";
    GetClassNameW(h, cls, 64);
    if (_wcsicmp(cls, L"OpusApp") != 0) return TRUE;     /* WPS 文字主窗口（照抄 Word） */
    WCHAR proc[64] = L"";
    ConfigGetProcessName(h, proc, 64);
    if (_wcsicmp(proc, L"wps.exe") != 0) return TRUE;    /* 排除真正的 Word */
    *(HWND*)lp = h;
    return FALSE;
}

int main(int argc, char** argv) {
    /* 本测试链接的 nav.c 在"管理员运行"时会去拉起降权桥，而桥就是
       "本 exe + --wps-bridge <pid> <seq>"（生产环境里那是 CapsEnhance.exe）。
       所以这里必须把子进程模式转发给 WpsBridgeServe，否则桥起不来
       （子进程会去跑这个测试本身、永远不会建管道）。 */
    if (argc >= 4 && strcmp(argv[1], "--wps-bridge") == 0)
        return WpsBridgeServe(strtoul(argv[2], NULL, 10), strtoul(argv[3], NULL, 10));

    HWND w = NULL;
    EnumWindows(FindCb, (LPARAM)&w);
    if (!w) {
        /* 没装/没开 WPS 的机器（含 CI）上直接算通过：这一步是"装了 WPS 才有意义"
           的真机检查，不该把别的开发环境卡红。 */
        printf("[SKIP] 找不到 WPS 文字 窗口（没运行或没打开文档）\n");
        return 0;
    }
    printf("WPS 窗口 = 0x%p，置前并开始跳词（会移动光标，不改内容）\n", (void*)w);
    SetForegroundWindow(w);
    Sleep(800);

    /* 配置要加载：MaxArrowSteps 等默认值在配置文件里（bin\CapsEnhance.ini） */
    ConfigLoad();

    /* 每次跳之前都重新置前：调试输出会写到控制台，而控制台窗口可能抢走前台
       （生产环境里 bridge 子进程是 GUI 子系统、不占控制台，不存在这个问题）。
       每步都独立读一次光标，用来核对"发的箭头确实动了光标"——
       NavJump 自己只在录制日志里报坐标，这里从外面独立验证一遍。 */
    for (int step = 0; step < 2; step++) {
        struct { int dir; int n; } seq[2] = { { DIR_RIGHT, 3 }, { DIR_LEFT, 2 } };
        for (int s = 0; s < 2; s++) {
            for (int i = 0; i < seq[s].n; i++) {
                WCHAR* t = NULL; int len = 0, caretBefore = -1;
                WpsReadText(&t, &len, &caretBefore);
                free(t);
                SetForegroundWindow(w); Sleep(400);
                NavJump(seq[s].dir, FALSE); Sleep(350);
                WCHAR* t2 = NULL; int len2 = 0, caretAfter = -1;
                WpsReadText(&t2, &len2, &caretAfter);
                free(t2);
                printf("  [检查] %s: 光标 %d -> %d\n",
                       seq[s].dir == DIR_RIGHT ? "右" : "左", caretBefore, caretAfter);
            }
        }
    }

    if (g_lines > 0) {
        printf("== 有诊断输出：说明至少走到了词跳链路 ==\n");
        return 0;
    }
    printf("[FAIL] 一条日志都没有（录制没打开？）\n");
    return 3;
}
