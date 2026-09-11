/* ============================================================
   Scintilla 通道集成测试：用一个"假 Scintilla 窗口"验证 nav.c 的
   Scintilla 读取/置位链路是真的通的，而不是只在源码上看着对。

   假窗口按 Scintilla 的真实语义实现这几条消息（语义取自 Notepad3 自带的
   Scintilla 源码 Editor.cxx / ScintillaWin.cxx）：
     SCI_GETTEXTLENGTH -> 文档字节数
     SCI_GETTEXT       -> 拷 min(wParam, 字节数) 个字节 + 结尾 0
     SCI_GETCURRENTPOS / SCI_GETANCHOR -> 字节偏移
     SCI_SETSEL(anchor, caret)         -> 选区[anchor,caret]，光标(caret)落在 lParam
     SCI_GETCODEPAGE   -> 65001 (UTF-8)
   文档内容用 UTF-8 存，因此这条链路同时验证"字节 <-> UTF-16"换算 ——
   中文 3 字节/字，换算出错时断言必然不通过。

   指针必须落在对方进程地址空间（本测试里对方进程 == 自己），
   若哪天改回"直传本地指针"，这里会立刻暴露。

   注：逐格箭头走真实 SendInput，到达节奏受机器影响（虚拟机里每键可达
   ~60ms），所以等待用"等到齐/超时"，不用固定时长。
   ============================================================ */
#include "cap.h"
#include "auto.h"
#include <stdio.h>
#include <wchar.h>
#include <stdarg.h>

/* nav.c 单测未链接 main.c，补若干符号 */
volatile BOOL g_recording = FALSE;
HHOOK g_hook = NULL;
void CapLog(const char* fmt, ...) { (void)fmt; }
/* 未链接 hook.c：物理按键状态桩 */
BOOL PhysKeyDown(int vk) { (void)vk; return FALSE; }
/* 未链接 wps.c：WPS 通道桩（真实现要链接 wps.c + autodisp.c + bridge.c，
   本测试只测词边界算法/其它通道，这里让它"读不到"即可）。
   签名从 src/auto.h 抄，改了接口这里要跟着改 —— 编译器会立刻提醒。 */
BOOL WpsGetTextCaret(HWND focus, WCHAR** text, int* lenOut, int* caretOut, int* selEndOut) {
    (void)focus; (void)text; (void)lenOut; (void)caretOut; (void)selEndOut;
    return FALSE;
}
int WpsSetSel(HWND focus, int start, int end) {
    (void)focus; (void)start; (void)end;
    return WPS_SET_NONE;   /* 什么都没做 -> 调用方可安全退回箭头 */
}

#include "nav.c"
#include "send.c"

/* ---------------- 假 Scintilla 窗口 ---------------- */
static char g_doc[4096];
static int  g_docLen = 0;
static int  g_caret  = 0;
static int  g_anchor = 0;
static int  g_kd     = 0;      /* 收到的 WM_KEYDOWN 计数 */

static void SetDocUtf8(const wchar_t* s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, g_doc, (int)sizeof(g_doc), NULL, NULL);
    g_docLen = n - 1;                       /* 不含结尾 0 */
    g_caret = g_anchor = 0;
}
/* UTF-16 下标 -> 文档字节偏移（测试自身用，与被测代码无关） */
static int U16ToBytes(const wchar_t* s, int u16) {
    return WideCharToMultiByte(CP_UTF8, 0, s, u16, NULL, 0, NULL, NULL);
}

static void CaretRight(void) {
    if (g_caret >= g_docLen) return;
    g_caret++;
    while (g_caret < g_docLen && ((unsigned char)g_doc[g_caret] & 0xC0) == 0x80) g_caret++;
}
static void CaretLeft(void) {
    if (g_caret <= 0) return;
    g_caret--;
    while (g_caret > 0 && ((unsigned char)g_doc[g_caret] & 0xC0) == 0x80) g_caret--;
}

static LRESULT CALLBACK SciProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case SCI_GETTEXTLENGTH: return g_docLen;
    case SCI_GETTEXT: {
        char* p = (char*)l;
        int n = (int)w;
        if (!p) return g_docLen;
        if (n > g_docLen) n = g_docLen;
        memcpy(p, g_doc, (size_t)n);
        p[n] = 0;
        return n;
    }
    case SCI_GETCURRENTPOS: return g_caret;
    case SCI_GETANCHOR:     return g_anchor;
    case SCI_GETCODEPAGE:   return 65001;
    case SCI_SETSEL:        g_anchor = (int)w; g_caret = (int)l; return 0;
    case SCI_SCROLLCARET:   return 0;
    case WM_KEYDOWN:
        g_kd++;
        if (w == VK_RIGHT) { CaretRight(); return 0; }
        if (w == VK_LEFT)  { CaretLeft();  return 0; }
        break;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* 把窗口带到前台（失败时退回 AttachThreadInput 老办法） */
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

/* 泵一段时间消息：SendInput 注入的键要经消息队列才到窗口 */
static void Pump(int ms) {
    ULONGLONG end = GetTickCount64() + (ULONGLONG)ms;
    MSG m;
    while (GetTickCount64() < end) {
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(5);
    }
}

/* 泵到新收到 n 个按键或超时，返回实际收到的个数 */
static int PumpFor(int n, int ms) {
    int base = g_kd;
    ULONGLONG end = GetTickCount64() + (ULONGLONG)ms;
    MSG m;
    while (g_kd - base < n && GetTickCount64() < end) {
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(5);
    }
    return g_kd - base;
}

static int fails = 0;
#define EXPECT(cond, name) do { \
    if (cond) printf("  [OK]   %s\n", name); \
    else      { printf("  [FAIL] %s (caret=%d anchor=%d kd=%d fg=%d)\n", name, \
                       g_caret, g_anchor, g_kd, \
                       (int)(GetForegroundWindow() == h)); fails++; } } while (0)

int main(void) {
    WNDCLASSW wc = {0};
    wc.lpfnWndProc   = SciProc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.lpszClassName = L"Scintilla";          /* 必须让 IsScintilla 认出来 */
    if (!RegisterClassW(&wc)) { printf("RegisterClass failed\n"); return 2; }

    HWND h = CreateWindowW(L"Scintilla", L"SciTest", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           40, 40, 420, 220, NULL, NULL, wc.hInstance, NULL);
    if (!h) { printf("CreateWindow failed\n"); return 2; }
    ShowWindow(h, SW_SHOW);
    BringFront(h);
    SetFocus(h);
    Pump(300);

    if (GetForegroundWindow() != h) {
        printf("  [SKIP] 拿不到前台窗口，跳过（需交互桌面）\n");
        return 0;
    }

    /* ---- A. 长中文串：走直接置位（步数 >= 6），验证 UTF-8 字节换算 ----
       这是一个测试句子 = 8 汉字(24字节) + 空格，UTF-16 目标下标 = 9 */
    const wchar_t* t1 = L"这是一个测试句子 end";   /* 8 汉字 + 空格 + end */
    SetDocUtf8(t1);
    Pump(50);
    NavJump(DIR_RIGHT, FALSE);
    EXPECT(g_caret == U16ToBytes(t1, 9),
           "SC: 中文串右跳 -> 字节位 25（8汉字*3+空格，而非 9）");

    /* ---- B. 从该位置左跳回去（步数 9，直接置位） ---- */
    NavJump(DIR_LEFT, FALSE);
    EXPECT(g_caret == 0, "SC: 左跳回文档首 (字节 0)");

    /* ---- C. F(shift) 位移：锚点保持原光标，光标到目标 ---- */
    SetDocUtf8(t1);
    g_caret = g_anchor = 0;
    Pump(50);
    NavJump(DIR_RIGHT, TRUE);
    EXPECT(g_caret == U16ToBytes(t1, 9) && g_anchor == 0,
           "SC: shift 右跳 -> caret=目标, anchor=原位置");

    /* ---- D. 文档末尾：不越界 ---- */
    const wchar_t* t2 = L"ab cd ef";
    SetDocUtf8(t2);
    g_caret = g_anchor = (int)strlen(g_doc);
    Pump(50);
    NavJump(DIR_RIGHT, FALSE);
    Pump(150);
    EXPECT(g_caret == (int)strlen(g_doc), "SC: 末尾右跳不越界");

    /* ---- 排空：上面若残留了注入键，等它们全到齐再测箭头路径 ---- */
    Pump(600);

    /* ---- E. 短距离走逐格箭头（步数 < DIRECT_JUMP_MIN） ---- */
    SetDocUtf8(t2);
    g_caret = g_anchor = 0;
    Pump(50);
    NavJump(DIR_RIGHT, FALSE);
    PumpFor(3, 5000);
    EXPECT(g_caret == 3, "SC: 短距右跳走箭头 -> 字节 3 (cd 首)");
    NavJump(DIR_LEFT, FALSE);
    PumpFor(3, 5000);
    EXPECT(g_caret == 0, "SC: 短距左跳走箭头 -> 回到 0");

    /* ---- F. 原始实验：直接 SendInput 5 对箭头应全部到达 ----
       （验证本机注入通道本身没有丢键，失败时先怀疑环境） */
    g_kd = 0;
    SetDocUtf8(t2);
    g_caret = g_anchor = 0;
    Pump(50);
    {
        for (int i = 0; i < 5; i++) {
            INPUT d, u;
            memset(&d, 0, sizeof(d));
            d.type = INPUT_KEYBOARD;
            d.ki.wVk = VK_RIGHT;
            d.ki.wScan = (WORD)MapVirtualKeyW(VK_RIGHT, MAPVK_VK_TO_VSC);
            d.ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
            u = d; u.ki.dwFlags |= KEYEVENTF_KEYUP;
            SendInput(1, &d, sizeof(INPUT));
            SendInput(1, &u, sizeof(INPUT));
            Sleep(13);
        }
        int got = PumpFor(5, 5000);
        EXPECT(g_kd == 5 && got == 5, "SC: raw 5 对箭头全部到达");
    }

    /* ---- G. 真实 Edit 控件：验证 WM_GETTEXT/EM_GETSEL/EM_SETSEL 链路 ----
       之前 SendMessageTimeout 结果指针传 NULL，长度恒读到 1，词跳完全失效。 */
    {
        HWND e = CreateWindowW(L"EDIT", L"hello world foo bar",
                               WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL,
                               10, 10, 380, 28, h, NULL, wc.hInstance, NULL);
        SendMessageW(e, EM_SETSEL, 0, 0);
        SetFocus(e);
        Pump(150);
        NavJump(DIR_RIGHT, FALSE);
        Pump(150);
        DWORD sel = 0;
        SendMessageW(e, EM_GETSEL, (WPARAM)&sel, 0);
        EXPECT(LOWORD(sel) == 6, "EC: Edit 控件右跳 hello| -> 6");
        NavJump(DIR_LEFT, FALSE);
        Pump(150);
        SendMessageW(e, EM_GETSEL, (WPARAM)&sel, 0);
        EXPECT(LOWORD(sel) == 0, "EC: Edit 控件左跳回到 0");
        DestroyWindow(e);
        SetFocus(h);
    }

    printf(fails ? "\n== %d 项失败 ==\n" : "\n== 全部通过 ==\n", fails);
    DestroyWindow(h);
    return fails;
}
