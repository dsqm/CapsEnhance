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
std::atomic<bool> g_recording{ false };
HHOOK g_hook = NULL;
void CapLog(const char* fmt, ...) { (void)fmt; }
/* 未链接 hook.c：物理按键状态桩 */
BOOL PhysKeyDown(int vk) { (void)vk; return FALSE; }
BOOL PhysKeyObservedDown(int vk) { (void)vk; return FALSE; }
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

/* ---- 假 EmEditorView：文本按 UTF-16 给、**位置按字节给** ----
   EmEditor 的 EmEditorView 支持 Edit 协议，但位置的单位随文档编码变：
   UTF-16 文档给 UTF-16 下标，UTF-8 文档给**字节偏移**（实测 73 字符的中文
   文档 EM_GETSEL 回 181）。这个假窗口复现后者，用来钉住"单位一致性自检"
   这条防线：Edit 通道必须整体放弃，而不是把越界值钳一下继续用。 */
static const wchar_t* g_emText = L"马趁香微路远，沙笼月淡烟斜。渡波清彻映妍华。";
static int g_emLen = 0;        /* 字符数（WM_GETTEXTLENGTH / WM_GETTEXT 的单位） */
static int g_emPos = 0;        /* EM_SETSEL 之前的当前光标（各场景自行设定） */
static int g_emSelA = 0, g_emSelB = 0;  /* 当前选区（有序两端），EM_GETSEL 报它 */
static int g_emSetSel = 0;     /* EM_SETSEL 被调用次数 */
static int g_emSetSelW = 0, g_emSetSelL = 0;  /* 最近一次 EM_SETSEL 的两个参数 */
/* 设定"当前光标"（无选区）：EM_GETSEL 的两端都报它。场景里代替直接写 g_emPos。 */
static void SetEmCaret(int p) { g_emPos = p; g_emSelA = p; g_emSelB = p; }
static int g_emKd = 0;         /* 收到的 WM_KEYDOWN 数：退回原生兜底时会 >0 */

static LRESULT CALLBACK EmProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (!g_emLen) g_emLen = (int)wcslen(g_emText);
    switch (m) {
    case WM_GETTEXTLENGTH: return g_emLen;
    case WM_GETTEXT: {
        WCHAR* p = (WCHAR*)l;
        if (!p) return 0;
        int n = (int)w;
        if (n > g_emLen + 1) n = g_emLen + 1;
        if (n > 0) {
            for (int i = 0; i < n - 1 && i < g_emLen; i++) p[i] = g_emText[i];
            p[n - 1] = 0;
        }
        /* EmEditor 风格：把结尾 NUL 也算进计数（nav.c 会剥掉多余的 NUL） */
        return g_emLen + 1;
    }
    case EM_GETSEL:                 /* 打包 (start, end)：真实控件恒按**有序**回报，
                                       所以"活动端在哪一侧"它这边看不出来
                                       （那正是 test/emprobe.c 用 Shift+方向测出来的） */
        return (LRESULT)((DWORD)g_emSelA | ((DWORD)g_emSelB << 16));
    case EM_SETSEL:
        g_emSetSel++;
        g_emSetSelW = (int)w; g_emSetSelL = (int)l;
        /* 真实控件：区间会被有序化，但**活动端留在 lParam**（emprobe H 段实测） */
        g_emSelA = ((int)w < (int)l) ? (int)w : (int)l;
        g_emSelB = ((int)w < (int)l) ? (int)l : (int)w;
        g_emPos  = (int)l;
        return 0;
    case WM_KEYDOWN:
        g_emKd++;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
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

    /* ---- A. 长中文串：走直接置位（步数 9 >= DIRECT_JUMP_MIN），验证 UTF-8 字节换算 ----
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
    const wchar_t* t2 = L"a bcd ef";   /* 词(1字) 空格 词(3字)：E 段两跳都只有 2 步，
                                          < DIRECT_JUMP_MIN(3)，保证走的是逐格箭头 */
    SetDocUtf8(t2);
    g_caret = g_anchor = (int)strlen(g_doc);
    Pump(50);
    NavJump(DIR_RIGHT, FALSE);
    Pump(150);
    EXPECT(g_caret == (int)strlen(g_doc), "SC: 末尾右跳不越界");

    /* ---- 排空：上面若残留了注入键，等它们全到齐再测箭头路径 ---- */
    Pump(600);

    /* ---- E. 短距离走逐格箭头（步数 < DIRECT_JUMP_MIN） ----
       样本必须是 2 步跳：原来 "ab cd ef" 首词 2 字 + 跳到下一词首 = 3 步，
       DIRECT_JUMP_MIN 降到 3 之后那条路被直接置位接管，逐格箭头就没有用例了。 */
    SetDocUtf8(t2);
    g_caret = g_anchor = 0;
    Pump(50);
    NavJump(DIR_RIGHT, FALSE);
    PumpFor(3, 5000);
    EXPECT(g_caret == 2, "SC: 短距右跳走箭头 -> 字节 2 (bcd 首)");
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
       之前 SendMessageTimeout 结果指针传 NULL，长度恒读到 1，按词跳转完全失效。 */
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

    /* ---- H. 假 EmEditorView（UTF-8 文档：位置按字节给）----
       断言两件事：
         · 一次 EM_SETSEL 都不发 —— 说明 Edit 通道**整体放弃**，而不是把越界的
           EM_GETSEL 钳到文本末尾后继续用（那正是把 EmEditor 光标写到多字节
           字符中间、连真实方向键都搞坏的路径）；
         · 退回原生 Ctrl+方向（假窗口收到了方向键），跳转功能不因此消失。 */
    {
        WNDCLASSW ec = {0};
        ec.lpfnWndProc   = EmProc;
        ec.hInstance     = GetModuleHandleW(NULL);
        ec.lpszClassName = L"EmEditorView";
        RegisterClassW(&ec);

        HWND em = CreateWindowW(L"EmEditorView", L"EmTest",
                                WS_CHILD | WS_VISIBLE | WS_BORDER,
                                10, 40, 380, 28, h, NULL, ec.hInstance, NULL);
        if (!em) {
            printf("  [FAIL] 假 EmEditorView 窗口创建失败\n");
            fails++;
        } else {
            SetFocus(em);
            Pump(150);
            g_emLen = (int)wcslen(g_emText);      /* 22 个字符 */

            /* H1. 位置按**字节**给（EmEditor 的 UTF-8 文档）-> 单位不一致 ->
               整条 Edit 通道放弃 */
            SetEmCaret(g_emLen * 3);              /* 66：远大于字符数 */
            g_emSetSel = 0; g_emKd = 0;
            NavJump(DIR_LEFT, FALSE);
            Pump(400);
            EXPECT(g_emSetSel == 0,
                   "EM1: 单位不一致 -> 完全不调用 EM_SETSEL（整条通道放弃）");
            EXPECT(g_emKd > 0, "EM1: 退回原生 Ctrl+方向（功能不丢）");

            /* H2. 位置单位一致（UTF-16 文档）-> 正常走外部置位，但置位后要
               **补一支探针方向键**：EmEditor 外部置位后会吞掉下一次输入（现象：
               "跳词后按 J 要按两下"），而那一次额度可以由我们主动喂掉
               （test/emprobe.c 实测 3/3 稳定），这样既保住瞬跳、用户也不用按两下。 */
            SetEmCaret(21);                       /* 末尾：左跳 steps=7 >= DIRECT_JUMP_MIN */
            g_emSetSel = 0; g_emKd = 0;
            NavJump(DIR_LEFT, FALSE);
            Pump(400);
            EXPECT(g_emSetSel == 1, "EM2: 单位一致 -> 正常外部置位");
            EXPECT(g_emKd >= 1, "EM2: 置位后补发探针方向键（喂掉被吞的那次输入）");

            /* H3/H4. 带 Shift（Caps+F 扩选）时，**活动端必须落在 lParam**。
               EmEditor 实测（test/emprobe.c H 段）：EM_SETSEL(60,73) 之后
               Shift+Left 动的是 73；EM_SETSEL(73,60) 动的是 60 —— 也就是
               "活动端 = lParam"。此前一律传有序区间 (小,大)，于是**向左扩选后
               光标停在右端**（现象："选中的那段光标在结尾，随后按方向键取消选中
               的行为不符合预期"）。这里直接断言传参顺序。 */
            NavSelTrackReset();
            SetEmCaret(21);
            g_emSetSel = 0; g_emSetSelW = -1; g_emSetSelL = -1;
            NavJump(DIR_LEFT, TRUE);              /* 左扩：活动端应落在**小**的一侧 */
            Pump(400);
            EXPECT(g_emSetSel == 1 && g_emSetSelL < g_emSetSelW,
                   "EM3: 带 Shift 左扩 -> 活动端(EM_SETSEL 的 lParam)=左端");

            NavSelTrackReset();
            SetEmCaret(0);
            g_emSetSel = 0; g_emSetSelW = -1; g_emSetSelL = -1;
            NavJump(DIR_RIGHT, TRUE);             /* 右扩：活动端应落在**大**的一侧 */
            Pump(400);
            EXPECT(g_emSetSel == 1 && g_emSetSelL > g_emSetSelW,
                   "EM4: 带 Shift 右扩 -> 活动端(EM_SETSEL 的 lParam)=右端");

            DestroyWindow(em);
            SetFocus(h);
            Pump(100);
        }
    }

    printf(fails ? "\n== %d 项失败 ==\n" : "\n== 全部通过 ==\n", fails);
    DestroyWindow(h);
    return fails;
}
