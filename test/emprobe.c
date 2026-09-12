/* ============================================================
   emprobe.c — 一次性探测：EmEditor 有没有"更温和"的瞬跳方式

   背景：外部 EM_SETSEL 会让 EmEditor 吞掉紧接着的第一次按键，而且它的位置
   单位随文档编码变（UTF-16 给字符下标、UTF-8 给字节偏移）。所以现在对它
   退回了逐格箭头/改道。这里实测三条替代路，看有没有更好的：

     A. COM 自动化：EmEditor.Application -> ActiveDocument -> Selection
        -> SetActivePoint(eePosType, x, y, bExtend)   ← 官方途径，最正统
     B. EM_POSFROMINDEX：能不能把"第 N 个字符"换成客户区坐标（前置条件）
     C. 后台鼠标消息（PostMessage WM_LBUTTONDOWN/UP）与后台按键
        （PostMessage WM_KEYDOWN VK_LEFT）—— 都不动真实鼠标、不需要前台

   只移动光标、不改文本；测完尽量把光标放回原处。每步都打印结果。
   一次性工具：不进 CI，不进主线（探测结论写进 DEVELOPMENT.md）。
   ============================================================ */
#define WIN32_LEAN_AND_MEAN
#define CINTERFACE          /* 必须早于任何 SDK 头：让 COM 用 C 风格 vtable */
#define COBJMACROS
#include <windows.h>
#include <oaidl.h>
#include <stdio.h>

/* 标准 Edit 消息；SDK 的 winuser.h 只给了一部分，这条自己写 */
#define EM_POSFROMINDEX 0x00D2
#define EM_CHARFROMPOS  0x00D7

/* ---------- 最小 IDispatch 辅助（不依赖 src/autodisp.c，保持工具独立） ---------- */

static DISPID IdOf(IDispatch* d, const WCHAR* name) {
    OLECHAR* n = (OLECHAR*)name;
    DISPID id = DISPID_UNKNOWN;
    if (FAILED(d->lpVtbl->GetIDsOfNames(d, IID_NULL, &n, 1, LOCALE_USER_DEFAULT, &id)))
        return DISPID_UNKNOWN;
    return id;
}

/* args 由调用方按**逆序**填好（rgvarg[0] 是最后一个形参） */
static BOOL Call(IDispatch* d, const WCHAR* name, VARIANT* args, UINT nargs, VARIANT* out) {
    DISPID id = IdOf(d, name);
    if (id == DISPID_UNKNOWN) { printf("      [没有这个成员: %ls]\n", name); return FALSE; }
    DISPPARAMS dp;
    memset(&dp, 0, sizeof(dp));
    dp.rgvarg = args; dp.cArgs = nargs;
    EXCEPINFO ei;
    memset(&ei, 0, sizeof(ei));
    UINT ec = 0;
    HRESULT hr = d->lpVtbl->Invoke(d, id, IID_NULL, LOCALE_USER_DEFAULT,
                                   DISPATCH_METHOD | DISPATCH_PROPERTYGET,
                                   &dp, out, &ei, &ec);
    /* 失败的 EXCEPINFO 里三个 BSTR 必须放掉（"没打开文档"这类失败会反复发生） */
    if (ei.bstrSource)      SysFreeString(ei.bstrSource);
    if (ei.bstrDescription) { printf("      [COM 说: %ls]\n", ei.bstrDescription);
                              SysFreeString(ei.bstrDescription); }
    if (ei.bstrHelpFile)    SysFreeString(ei.bstrHelpFile);
    if (FAILED(hr)) printf("      [Invoke 失败 hr=0x%08lX]\n", (unsigned long)hr);
    return SUCCEEDED(hr);
}

static IDispatch* GetDisp(IDispatch* d, const WCHAR* name) {
    VARIANT v; VariantInit(&v);
    if (!Call(d, name, NULL, 0, &v)) return NULL;
    IDispatch* r = (v.vt == VT_DISPATCH) ? v.pdispVal : NULL;
    if (!r) VariantClear(&v);
    return r;    /* 所有权转移给调用方 */
}

/* 真实注入一支方向键（SendInput：模拟用户真按） */
static void TapVk(WORD vk) {
    INPUT in[2];
    memset(in, 0, sizeof(in));
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = vk;
    in[0].ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    in[0].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
    in[1] = in[0]; in[1].ki.dwFlags |= KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
    Sleep(40);
}

/* 不带 EXTENDEDKEY 的普通键（F8 这类） */
static void TapVkPlain(WORD vk) {
    INPUT in[2];
    memset(in, 0, sizeof(in));
    in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = vk;
    in[0].ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    in[1] = in[0]; in[1].ki.dwFlags |= KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
    Sleep(60);
}

/* 读回 (caret, selEnd)：两者不同 = 当前有选区 */
static void ReadSel(HWND h, int* caret, int* selEnd) {
    LONGLONG s = 0;
    SendMessageTimeoutW(h, EM_GETSEL, 0, 0, SMTO_ABORTIFHUNG, 300, (PDWORD_PTR)&s);
    DWORD v = (DWORD)s;
    *caret = (int)LOWORD(v); *selEnd = (int)HIWORD(v);
}

static int ReadCaret(HWND h) {
    LONGLONG s = 0;
    SendMessageTimeoutW(h, EM_GETSEL, 0, 0, SMTO_ABORTIFHUNG, 300, (PDWORD_PTR)&s);
    return (int)LOWORD((DWORD)s);
}

/* 把编辑区带到前台并给它键盘焦点（跨进程 SetFocus 要先 AttachThreadInput） */
static void FocusEditor(HWND h) {
    HWND root = GetAncestor(h, GA_ROOT);
    SetForegroundWindow(root);
    Sleep(150);
    DWORD fgT = GetWindowThreadProcessId(GetForegroundWindow(), NULL);
    DWORD myT = GetCurrentThreadId();
    if (AttachThreadInput(myT, fgT, TRUE)) {
        SetFocus(h);
        AttachThreadInput(myT, fgT, FALSE);
    }
    Sleep(150);
}

/* ---------- 找 EmEditor 的编辑区窗口（必要时把主窗口带到前台） ---------- */
static HWND FindEditorView(void) {
    WCHAR c[96];
    HWND fg = GetForegroundWindow();
    if (fg) {
        GetClassNameW(fg, c, 96);
        if (wcsstr(c, L"EmEditor")) return fg;
    }
    /* 自己找 EmEditor 主窗口（日志里前台类名就是它），带到前台再找编辑区 */
    HWND main = FindWindowW(L"EmEditorMainFrame3", NULL);
    if (!main) return NULL;
    SetForegroundWindow(main);
    Sleep(400);
    for (HWND w = GetWindow(main, GW_CHILD); w; w = GetWindow(w, GW_HWNDNEXT)) {
        GetClassNameW(w, c, 96);
        if (wcsstr(c, L"EmEditorView")) return w;
        for (HWND g = GetWindow(w, GW_CHILD); g; g = GetWindow(g, GW_HWNDNEXT)) {
            GetClassNameW(g, c, 96);
            if (wcsstr(c, L"EmEditorView")) return g;
        }
    }
    return main;
}

int main(void) {
    printf("=== EmEditor 瞬跳方式探测 ===\n\n");

    HWND h = FindEditorView();
    if (!h) {
        printf("[跳过] 前台窗口不是 EmEditor（请先切到 EmEditor 编辑区再跑）\n");
        return 2;
    }
    WCHAR cls[96]; GetClassNameW(h, cls, 96);
    printf("目标控件: hwnd=0x%p class=[%ls]\n\n", (void*)h, cls);

    /* 记下原光标，测完尽量放回去 */
    LONGLONG sel0 = 0;
    SendMessageTimeoutW(h, EM_GETSEL, 0, 0, SMTO_ABORTIFHUNG, 300, (PDWORD_PTR)&sel0);
    int caret0 = (int)LOWORD((DWORD)sel0);
    printf("起始光标 = %d\n\n", caret0);

    /* ================= A. COM 自动化 ================= */
    printf("---- A. COM: EmEditor.Application -> Selection -> SetActivePoint ----\n");
    HRESULT hrCo = CoInitialize(NULL);
    CLSID clsid;
    HRESULT hr = CLSIDFromProgID(L"EmEditor.Application", &clsid);
    printf("  CLSIDFromProgID hr=0x%08lX\n", (unsigned long)hr);
    IDispatch* app = NULL;
    if (SUCCEEDED(hr)) {
        hr = GetActiveObject(clsid, NULL, (IUnknown**)&app);
        printf("  GetActiveObject  hr=0x%08lX%s\n", (unsigned long)hr,
               (hr == 0x800401E3UL) ? "  <- ROT 权限隔离（本进程与 EmEditor 完整性级别不同）"
                                    : (SUCCEEDED(hr) ? "" : "  <- 没有在运行的实例？"));
    }
    if (app) {
        IDispatch* doc = GetDisp(app, L"ActiveDocument");
        printf("  ActiveDocument   = %s\n", doc ? "OK" : "失败");
        IDispatch* sel = doc ? GetDisp(doc, L"Selection") : NULL;
        printf("  Selection        = %s\n", sel ? "OK" : "失败");

        if (sel) {
            /* 读当前活动点：eePosType 取 1(视图) / 2(逻辑) 都试一遍 */
            for (int t = 1; t <= 2; t++) {
                long x = -1, y = -1;
                VARIANT a[3], v; VariantInit(&v);
                for (int i = 0; i < 3; i++) VariantInit(&a[i]);
                a[2].vt = VT_I4; a[2].lVal = t;                    /* 第 1 个形参 */
                a[1].vt = VT_BYREF | VT_I4; a[1].plVal = &y;       /* 第 2 个形参（out） */
                a[0].vt = VT_BYREF | VT_I4; a[0].plVal = &x;       /* 第 3 个形参（out） */
                BOOL ok = Call(sel, L"GetActivePoint", a, 3, &v);
                printf("  GetActivePoint(type=%d) -> %s x=%ld y=%ld\n",
                       t, ok ? "OK" : "失败", x, y);
                VariantClear(&v);
            }

            /* 写：把光标左移 3（逻辑坐标），成功与否都读回校验 */
            long x0 = 1, y0 = 1;
            {
                VARIANT a[3], v; VariantInit(&v);
                for (int i = 0; i < 3; i++) VariantInit(&a[i]);
                a[2].vt = VT_I4; a[2].lVal = 2;                    /* eePosLogical */
                a[1].vt = VT_BYREF | VT_I4; a[1].plVal = &y0;
                a[0].vt = VT_BYREF | VT_I4; a[0].plVal = &x0;
                Call(sel, L"GetActivePoint", a, 3, &v);
                VariantClear(&v);
            }
            long xw = (x0 > 3) ? x0 - 3 : 1;
            VARIANT a[4], v; VariantInit(&v);
            for (int i = 0; i < 4; i++) VariantInit(&a[i]);
            a[3].vt = VT_I4; a[3].lVal = 2;                        /* eePosType */
            a[2].vt = VT_I4; a[2].lVal = xw;                       /* x */
            a[1].vt = VT_I4; a[1].lVal = y0;                       /* y */
            a[0].vt = VT_I4; a[0].lVal = 0;                        /* bExtend = FALSE */
            BOOL ok = Call(sel, L"SetActivePoint", a, 4, &v);
            VariantClear(&v);
            printf("  SetActivePoint(logical x=%ld y=%ld) -> %s\n", xw, y0, ok ? "OK" : "失败");
            Sleep(150);
            LONGLONG s2 = 0;
            SendMessageTimeoutW(h, EM_GETSEL, 0, 0, SMTO_ABORTIFHUNG, 300, (PDWORD_PTR)&s2);
            printf("  之后 EM_GETSEL caret=%d（原 %d）\n", (int)LOWORD((DWORD)s2), caret0);

            /* 用 COM 读回活动点，确认它的内部状态真的变了 */
            long x2 = -1, y2 = -1;
            VARIANT a2[3], v2; VariantInit(&v2);
            for (int i = 0; i < 3; i++) VariantInit(&a2[i]);
            a2[2].vt = VT_I4; a2[2].lVal = 2;
            a2[1].vt = VT_BYREF | VT_I4; a2[1].plVal = &y2;
            a2[0].vt = VT_BYREF | VT_I4; a2[0].plVal = &x2;
            Call(sel, L"GetActivePoint", a2, 3, &v2);
            printf("  COM 读回活动点: x=%ld y=%ld\n", x2, y2);
            VariantClear(&v2);
            sel->lpVtbl->Release(sel);
        }
        if (doc) doc->lpVtbl->Release(doc);
        app->lpVtbl->Release(app);
    } else {
        printf("  （拿不到 Application 对象，A 这条路本次无法验证）\n");
    }

    /* ================= B. EM_POSFROMINDEX ================= */
    printf("\n---- B. EM_POSFROMINDEX：把字符下标换成客户区坐标 ----\n");
    /* 跨进程不能传 POINT*，lParam=0 时返回打包 (x,y) */
    for (int d = 1; d <= 3; d++) {
        int idx = caret0 - d;
        if (idx < 0) idx = 0;
        DWORD_PTR r = 0;
        DWORD_PTR ok = 0;
        ok = SendMessageTimeoutW(h, EM_POSFROMINDEX, (WPARAM)idx, 0,
                                 SMTO_ABORTIFHUNG, 300, &r);
        printf("  EM_POSFROMINDEX(%d) -> %s packed=0x%08llX x=%d y=%d\n",
               idx, ok ? "OK" : "超时/不支持", (unsigned long long)r,
               (int)(short)LOWORD((DWORD)r), (int)(short)HIWORD((DWORD)r));
    }

    /* ================= C. 后台消息（鼠标 / 键盘） ================= */
    printf("\n---- C. 后台消息：不动真实鼠标、不需要前台 ----\n");
    {
        /* 取一个"看起来安全"的点：直接把 B 的结果用上；拿不到就跳过 */
        DWORD_PTR r = 0;
        SendMessageTimeoutW(h, EM_POSFROMINDEX, (WPARAM)(caret0 > 2 ? caret0 - 2 : 0), 0,
                            SMTO_ABORTIFHUNG, 300, &r);
        int cx = (int)(short)LOWORD((DWORD)r), cy = (int)(short)HIWORD((DWORD)r);
        printf("  目标点客户区坐标 = (%d,%d)\n", cx, cy);

        LONGLONG before = 0;
        SendMessageTimeoutW(h, EM_GETSEL, 0, 0, SMTO_ABORTIFHUNG, 300, (PDWORD_PTR)&before);

        /* C1. 后台鼠标：PostMessage 三连（move -> down -> up） */
        PostMessageW(h, WM_MOUSEMOVE,   0,          MAKELPARAM(cx, cy));
        PostMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(cx, cy));
        PostMessageW(h, WM_LBUTTONUP,   0,          MAKELPARAM(cx, cy));
        Sleep(200);
        LONGLONG a1 = 0;
        SendMessageTimeoutW(h, EM_GETSEL, 0, 0, SMTO_ABORTIFHUNG, 300, (PDWORD_PTR)&a1);
        printf("  C1 PostMessage 点击 -> caret %d -> %d  %s\n",
               (int)LOWORD((DWORD)before), (int)LOWORD((DWORD)a1),
               (LOWORD((DWORD)a1) != LOWORD((DWORD)before)) ? "有反应" : "无反应");

        /* C2. 后台鼠标：SendMessage 版本（同步，有的控件只认这种） */
        before = a1;
        SendMessageTimeoutW(h, WM_MOUSEMOVE,   0,          MAKELPARAM(cx, cy),
                            SMTO_ABORTIFHUNG, 300, NULL);
        SendMessageTimeoutW(h, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(cx, cy),
                            SMTO_ABORTIFHUNG, 300, NULL);
        SendMessageTimeoutW(h, WM_LBUTTONUP,   0,          MAKELPARAM(cx, cy),
                            SMTO_ABORTIFHUNG, 300, NULL);
        Sleep(200);
        LONGLONG a2 = 0;
        SendMessageTimeoutW(h, EM_GETSEL, 0, 0, SMTO_ABORTIFHUNG, 300, (PDWORD_PTR)&a2);
        printf("  C2 SendMessage 点击 -> caret %d -> %d  %s\n",
               (int)LOWORD((DWORD)before), (int)LOWORD((DWORD)a2),
               (LOWORD((DWORD)a2) != LOWORD((DWORD)before)) ? "有反应" : "无反应");

        /* C3. 后台按键：PostMessage WM_KEYDOWN/UP（不动键盘状态机） */
        before = a2;
        PostMessageW(h, WM_KEYDOWN, VK_LEFT, 0x014B0001);
        PostMessageW(h, WM_KEYUP,   VK_LEFT, 0xC14B0001);
        Sleep(200);
        LONGLONG a3 = 0;
        SendMessageTimeoutW(h, EM_GETSEL, 0, 0, SMTO_ABORTIFHUNG, 300, (PDWORD_PTR)&a3);
        printf("  C3 PostMessage 方向键 -> caret %d -> %d  %s\n",
               (int)LOWORD((DWORD)before), (int)LOWORD((DWORD)a3),
               (LOWORD((DWORD)a3) != LOWORD((DWORD)before)) ? "有反应" : "无反应");
    }

    /* ================= D. 消息家族支持度 ================= */
    printf("\n---- D. 其它 EM_* 支持度（拿不到坐标时的备选） ----\n");
    {
        struct { const char* name; UINT msg; } probes[] = {
            { "EM_CHARFROMPOS",  EM_CHARFROMPOS },   /* 坐标 -> 字符 */
            { "EM_LINEFROMCHAR", 0x00C9 },           /* 字符 -> 行号 */
            { "EM_LINEINDEX",    0x00BB },           /* 行号 -> 行首字符 */
            { "EM_LINELENGTH",   0x00C1 },           /* 字符 -> 行长度 */
            { "EM_GETRECT",      0x00B2 },           /* 客户区矩形 */
        };
        for (int i = 0; i < (int)(sizeof(probes)/sizeof(probes[0])); i++) {
            DWORD_PTR r = 0xDEADBEEF;
            DWORD_PTR ok = SendMessageTimeoutW(h, probes[i].msg, (WPARAM)caret0, 0,
                                               SMTO_ABORTIFHUNG, 300, &r);
            printf("  %-16s -> %s 返回 0x%08llX (%lld)\n", probes[i].name,
                   ok ? "OK" : "超时", (unsigned long long)r, (long long)r);
        }
    }

    /* ================= E. 后台按键（先把光标挪开再测，避免 0 处左移无效果） ================= */
    printf("\n---- E. 后台方向键（PostMessage，不动键盘状态机） ----\n");
    {
        /* 用 EM_SETSEL 把光标放到 20（这一步只为构造测试前提；EM_SETSEL 能否
           长期用是另一件事，这里不评价） */
        SendMessageTimeoutW(h, EM_SETSEL, (WPARAM)20, (LPARAM)20, SMTO_ABORTIFHUNG, 300, NULL);
        Sleep(120);
        LONGLONG b = 0;
        SendMessageTimeoutW(h, EM_GETSEL, 0, 0, SMTO_ABORTIFHUNG, 300, (PDWORD_PTR)&b);
        printf("  起点 caret=%d\n", (int)LOWORD((DWORD)b));

        /* E1. 只发 WM_KEYDOWN/UP */
        PostMessageW(h, WM_KEYDOWN, VK_LEFT, 0x014B0001);
        PostMessageW(h, WM_KEYUP,   VK_LEFT, 0xC14B0001);
        Sleep(200);
        LONGLONG c = 0;
        SendMessageTimeoutW(h, EM_GETSEL, 0, 0, SMTO_ABORTIFHUNG, 300, (PDWORD_PTR)&c);
        printf("  E1 只发 WM_KEYDOWN/UP      -> caret %d -> %d  %s\n",
               (int)LOWORD((DWORD)b), (int)LOWORD((DWORD)c),
               (LOWORD((DWORD)c) != LOWORD((DWORD)b)) ? "有反应" : "无反应");

        /* E2. 键盘状态也要对（多数控件自己查 GetKeyState，不看消息） */
        BYTE ks[256]; memset(ks, 0, sizeof(ks));
        SetKeyboardState(ks);
        PostMessageW(h, WM_KEYDOWN, VK_LEFT, 0x014B0001);
        PostMessageW(h, WM_KEYUP,   VK_LEFT, 0xC14B0001);
        Sleep(200);
        LONGLONG e = 0;
        SendMessageTimeoutW(h, EM_GETSEL, 0, 0, SMTO_ABORTIFHUNG, 300, (PDWORD_PTR)&e);
        printf("  E2 （重试一次）             -> caret %d -> %d  %s\n",
               (int)LOWORD((DWORD)c), (int)LOWORD((DWORD)e),
               (LOWORD((DWORD)e) != LOWORD((DWORD)c)) ? "有反应" : "无反应");

        /* E3. 点击的落点是否与坐标相关：点客户区两个不同高度的点，看 caret 变化 */
        LONGLONG g0 = 0;
        SendMessageTimeoutW(h, EM_GETSEL, 0, 0, SMTO_ABORTIFHUNG, 300, (PDWORD_PTR)&g0);
        PostMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(80, 5));
        PostMessageW(h, WM_LBUTTONUP,   0,          MAKELPARAM(80, 5));
        Sleep(200);
        LONGLONG g1 = 0;
        SendMessageTimeoutW(h, EM_GETSEL, 0, 0, SMTO_ABORTIFHUNG, 300, (PDWORD_PTR)&g1);
        PostMessageW(h, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(240, 5));
        PostMessageW(h, WM_LBUTTONUP,   0,          MAKELPARAM(240, 5));
        Sleep(200);
        LONGLONG g2 = 0;
        SendMessageTimeoutW(h, EM_GETSEL, 0, 0, SMTO_ABORTIFHUNG, 300, (PDWORD_PTR)&g2);
        printf("  E3 点击 (80,5) -> caret %d；点击 (240,5) -> caret %d  %s\n",
               (int)LOWORD((DWORD)g1), (int)LOWORD((DWORD)g2),
               (LOWORD((DWORD)g1) != LOWORD((DWORD)g2)) ? "落点随坐标变化（点击定位可行）"
                                                        : "落点与坐标无关");
    }

    /* ================= F. EM_SETSEL + 探针补正 =================
       现象：外部置位后，紧接着的第一次按键会被它吞掉（不是时间窗口——日志里
       隔 1.2s 按下的那次也被吞），所以"要按两下"。
       思路：既然"吞"是可回读的，就在置位后**主动喂一支探针方向键**把这次
       额度用掉，再看光标有没有动：没动=被吞（完成）；动了=没被吞，反向补一格
       回到目标。之后就恢复到"一次就响应"。 */
    printf("\n---- F. EM_SETSEL 置位 + 探针补正 ----\n");
    {
        FocusEditor(h);
        for (int round = 1; round <= 3; round++) {
            int tgt = 50 - round * 5;
            if (tgt < 1) tgt = 1;
            SendMessageTimeoutW(h, EM_SETSEL, (WPARAM)tgt, (LPARAM)tgt,
                                SMTO_ABORTIFHUNG, 300, NULL);
            Sleep(120);
            int c0 = ReadCaret(h);

            TapVk(VK_LEFT);                 /* 探针：把"被吞额度"用掉 */
            int c1 = ReadCaret(h);
            int swallowed = (c1 == c0);
            if (!swallowed) {               /* 没被吞 -> 探针真移动了，补回来 */
                TapVk(VK_RIGHT);
                c1 = ReadCaret(h);
            }

            TapVk(VK_LEFT);                 /* 用户的下一次真实按键：应为 1 次响应 */
            int c2 = ReadCaret(h);

            printf("  第 %d 轮: 置位到 %d -> caret=%d | 探针后=%d (%s) | 再按一次=%d  %s\n",
                   round, tgt, c0, c1, swallowed ? "被吞" : "未被吞，已补回",
                   c2, (c2 == c1 - 1) ? "正常响应 ✓" : "仍不正常 ✗");
            SendMessageTimeoutW(h, EM_SETSEL, (WPARAM)tgt, (LPARAM)tgt,
                                SMTO_ABORTIFHUNG, 300, NULL);   /* 复位以便下一轮 */
            Sleep(60);
        }
    }

    /* ================= G. EmEditor 的"选择模式"假设验证 =================
       现象（用户）：光标在行尾，按两下 N 之后"整行被选中了"，光标却还在结尾，
       之后按 L"取消选中"的行为也不对。
       这与 EmEditor 的**选择模式**（默认 F8，开启后所有光标移动都变成扩展
       选区）完全吻合：活动端留在原处、另一端移到词边界 -> 看起来就是"选中了
       一段、光标还在原地"。
       这里做实验：记下"移动是否变成选区" -> 按一次 F8 -> 再看一遍 -> 再按
       一次 F8 复原。"移动是否变成选区"用 (caret, selEnd) 是否相等判断。 */
    printf("\n---- G. \u9009\u62e9\u6a21\u5f0f\uff08F8\uff09\u5047\u8bbe\u9a8c\u8bc1 ----\n");
    {
        FocusEditor(h);
        /* \u6ce8\u610f\uff1a\u6bcf\u6b21 EM_SETSEL \u5b9a\u4f4d\u540e\uff0c**\u7b2c\u4e00\u652f\u65b9\u5411\u952e\u5fc5\u88ab\u541e**\uff08\u5c31\u662f F \u6bb5\u90a3\u4e2a\u73b0\u8c61\uff09\uff0c
           \u6240\u4ee5\u6bcf\u7ec4\u90fd\u5148\u53d1\u4e00\u652f"\u6d88\u5316\u7528"\u7684\u65b9\u5411\u952e\uff0c\u518d\u53d1\u89c2\u5bdf\u7528\u7684\u90a3\u652f\uff0c
           \u5426\u5219\u6d4b\u5230\u7684\u6c38\u8fdc\u53ea\u662f"\u88ab\u541e"\u3002 */
        #define OBSERVE(tag) \
            do { \
                SendMessageTimeoutW(h, EM_SETSEL, (WPARAM)40, (LPARAM)40, SMTO_ABORTIFHUNG, 300, NULL); \
                Sleep(120); \
                TapVk(VK_LEFT);                 /* \u6d88\u5316\u88ab\u541e\u7684\u90a3\u4e00\u6b21 */ \
                int ca, sa; ReadSel(h, &ca, &sa); \
                TapVk(VK_LEFT);                 /* \u89c2\u5bdf\u7528 */ \
                int cb, sb; ReadSel(h, &cb, &sb); \
                printf("  %s: \u6d88\u5316\u540e (%d,%d) --\u5de6\u79fb--> (%d,%d)  %s\\n", tag, ca, sa, cb, sb, \
                       (cb != sb) ? "\u79fb\u52a8\u53d8\u6210\u4e86\u9009\u533a\uff01" : "\u666e\u901a\u79fb\u52a8\uff08\u6298\u53e0\uff09"); \
            } while (0)

        OBSERVE("G1 \u666e\u901a\u72b6\u6001      ");
        TapVkPlain(VK_F8); Sleep(150);
        OBSERVE("G2 \u6309\u8fc7 F8 \u540e     ");
        TapVkPlain(VK_F8); Sleep(150);
        OBSERVE("G3 \u518d\u6309 F8 \u590d\u539f  ");
        #undef OBSERVE
    }

    /* 尽量把光标放回原处（用 EM_SETSEL 只在"原位"上，不改文本） */
    SendMessageTimeoutW(h, EM_SETSEL, (WPARAM)caret0, (LPARAM)caret0,
                        SMTO_ABORTIFHUNG, 300, NULL);
    if (SUCCEEDED(hrCo)) CoUninitialize();
    printf("\n=== 探测结束（光标已尽量放回 %d）===\n", caret0);
    return 0;
}
