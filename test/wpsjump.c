/* ============================================================
   WPS 通道的**真机集成测试**：把 WPS 文字窗口置前，直接调 NavJump，
   逐步核对"选中的是 WPS 通道"、"光标/选区落在算出的目标上"。

   为什么要直调 NavJump：钩子忽略注入按键（LLKHF_INJECTED），没法用 SendInput
   模拟 Caps+J 驱动端到端。直调之后，通道选择、文本读取、坐标换算、直接置位
   （Selection.SetRange）/逐格箭头这几步与线上完全一致，差别只在"谁触发的"。

   覆盖两条路：
     · 无 Shift —— 期望光标正好落在目标位置上（直接置位生效时就是瞬跳）；
     · 带 Shift —— 期望选区按方向单调扩张/收缩（锚点语义）；
   两者都用"从外面独立回读 Selection"的方式验证，不信 NavJump 的自述。

   机器上没装/没开 WPS 时打印 [SKIP] 并以 0 退出（CI 友好）。
   **会移动 WPS 里文档的光标**（不改内容），先存盘。
   ============================================================ */
#include "cap.h"
#include "auto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdarg.h>

/* 未链接 main.c / hook.c：补上它们提供的符号 */
volatile BOOL g_recording = TRUE;    /* 打开记录，nav.c/wps.c 的诊断日志直接打出来 */
HHOOK g_hook = NULL;
static int g_lines = 0;
void CapLog(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vprintf(fmt, ap); va_end(ap);
    printf("\n");
    g_lines++;
}
BOOL PhysKeyDown(int vk) { (void)vk; return FALSE; }

static int g_fail = 0;

/* 抢前台要"连着线程输入一起抢"：单独 SetForegroundWindow 经常被系统拒绝
   （调用者不是当前前台进程时），AttachThreadInput 之后就能稳定生效 ——
   与 bin\lowrun.exe 那套验证过的做法一致。调试输出会不停打到控制台，
   控制台一被激活就抢走前台，所以每次跳之前都要重新抢一次。 */
static void ForceFg(HWND w) {
    HWND fg = GetForegroundWindow();
    DWORD fgTid = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
    DWORD myTid = GetCurrentThreadId();
    BOOL att = (fgTid && fgTid != myTid) ? AttachThreadInput(fgTid, myTid, TRUE) : FALSE;
    SetForegroundWindow(w);
    BringWindowToTop(w);
    SetFocus(w);
    if (att) AttachThreadInput(fgTid, myTid, FALSE);
}

/* 当前前台窗口 + 焦点控件属于谁 —— 按词跳转为什么"没走 WPS 通道"多半就在这两者。
   focus 这一级照抄 nav.c 的 GetFocusControl（GetGUIThreadInfo 取 hwndFocus），
   因为"前台是 WPS，但焦点控件取不到"会直接让 WPS 通道判定失败。 */
static void FgInfo(char* out, size_t cap) {
    HWND fg = GetForegroundWindow();
    char p[80] = "?", c[80] = "";
    char fp[80] = "?", fc[80] = "";
    HWND fo = NULL;
    DWORD tid = 0;
    if (fg) {
        WCHAR wp[64] = L"?", wc[64] = L"";
        ConfigGetProcessName(fg, wp, 64);
        GetClassNameW(fg, wc, 64);
        WideCharToMultiByte(CP_UTF8, 0, wp, -1, p, sizeof(p), NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, wc, -1, c, sizeof(c), NULL, NULL);
        tid = GetWindowThreadProcessId(fg, NULL);
        GUITHREADINFO gi; memset(&gi, 0, sizeof(gi)); gi.cbSize = sizeof(gi);
        if (tid && GetGUIThreadInfo(tid, &gi)) fo = gi.hwndFocus;
    }
    if (fo) {
        WCHAR wp[64] = L"?", wc[64] = L"";
        ConfigGetProcessName(fo, wp, 64);
        GetClassNameW(fo, wc, 64);
        WideCharToMultiByte(CP_UTF8, 0, wp, -1, fp, sizeof(fp), NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, wc, -1, fc, sizeof(fc), NULL, NULL);
    }
    _snprintf_s(out, cap, _TRUNCATE, "fg=%s/%s fgTid=%lu focus=%p(%s/%s) GetFocus=%p",
                p, c, (unsigned long)tid, (void*)fo, fp, fc, (void*)GetFocus());
}

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

/* 独立回读：光标 + 选区另一端 */
static BOOL Peek(HWND w, int* caret, int* selEnd) {
    WCHAR* t = NULL; int len = 0, c = 0, e = 0;
    if (!WpsGetTextCaret(w, &t, &len, &c, &e)) return FALSE;
    free(t);
    *caret = c;
    *selEnd = e;
    return TRUE;
}

static void Check(const char* what, BOOL ok, int a, int b) {
    printf("  [%s] %s (%d, %d)\n", ok ? "OK" : "FAIL", what, a, b);
    if (!ok) g_fail++;
}

int main(int argc, char** argv) {
    /* 本测试链接的 nav.c/wps.c 在"管理员运行"时会去拉起降权桥，而桥就是
       "本 exe + --bridge <pid> <seq>"（生产环境里那是 CapsEnhance.exe）。
       所以这里必须把子进程模式转发给 WpsBridgeServe，否则桥起不来。 */
    if (argc >= 4 && strcmp(argv[1], "--bridge") == 0)
        return WpsBridgeServe(strtoul(argv[2], NULL, 10), strtoul(argv[3], NULL, 10));

    HWND w = NULL;
    EnumWindows(FindCb, (LPARAM)&w);
    if (!w) {
        /* 没装/没开 WPS 的机器（含 CI）上直接算通过：这一步是"装了 WPS 才有
           意义"的真机检查，不该把别的开发环境卡红。 */
        printf("[SKIP] 找不到 WPS 文字 窗口（没运行或没打开文档）\n");
        return 0;
    }
    printf("WPS 窗口 = 0x%p，置前并开始按词跳转（会移动光标，不改内容）\n", (void*)w);
    ConfigLoad();                       /* MaxArrowSteps 等取 bin\CapsEnhance.ini */
    SetForegroundWindow(w);
    Sleep(800);

    int c0 = -1, e0 = -1;
    if (!Peek(w, &c0, &e0)) { printf("[FAIL] 读不到 WPS 文档（先打开一篇文档）\n"); return 3; }
    printf("初始: caret=%d selEnd=%d\n", c0, e0);

    /* ---- 无 Shift：光标应精确落在目标上 ---- */
    printf("== 无 Shift：右 3 次、左 2 次 ==\n");
    for (int i = 0; i < 5; i++) {
        int dir = (i < 3) ? DIR_RIGHT : DIR_LEFT;
        int before = -1, beforeE = -1;
        Peek(w, &before, &beforeE);
        ForceFg(w); Sleep(400);
        { char fgi[176]; FgInfo(fgi, sizeof(fgi)); printf("  (%s)\n", fgi); }
        NavJump(dir, FALSE);
        Sleep(300);
        int after = -1, afterE = -1;
        if (!Peek(w, &after, &afterE)) { Check("跳后仍能读到光标", FALSE, after, afterE); continue; }
        /* 语义：向右不该左移、向左不该右移；且光标与选区另一端一致（无选区） */
        BOOL ok = (dir == DIR_RIGHT) ? (after >= before) : (after <= before);
        ok = ok && (after == afterE);
        Check(dir == DIR_RIGHT ? "无 Shift 右跳（不左移且无选区）"
                               : "无 Shift 左跳（不右移且无选区）", ok, after, afterE);
    }

    /* ---- 带 Shift：选区按方向单调扩张再收缩（锚点语义） ---- */
    printf("== 带 Shift：右 3 次（扩张）再左 2 次（收缩）==\n");
    int prevS = -1, prevE = -1;
    Peek(w, &prevS, &prevE);
    for (int i = 0; i < 5; i++) {
        BOOL grow = (i < 3);
        int dir = grow ? DIR_RIGHT : DIR_LEFT;
        ForceFg(w); Sleep(400);
        { char fgi[176]; FgInfo(fgi, sizeof(fgi)); printf("  (%s)\n", fgi); }
        NavJump(dir, TRUE);
        Sleep(300);
        int s = -1, e = -1;
        if (!Peek(w, &s, &e)) { Check("位移后仍能读到选区", FALSE, s, e); continue; }
        BOOL ok;
        if (grow) ok = (e > prevE) && (s <= prevS);   /* 活动端在外，另一端不动/外扩 */
        else      ok = (e < prevE);                   /* 收缩：活动端往回走 */
        Check(grow ? "Shift 右扩（末端前进）" : "Shift 左收（末端后退）", ok, s, e);
        prevS = s; prevE = e;
    }

    printf("%s（诊断输出 %d 行）\n", g_fail ? "== 有失败 ==" : "== 全部通过 ==", g_lines);
    return g_fail ? 1 : 0;
}
