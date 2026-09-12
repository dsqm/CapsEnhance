/* ============================================================
   CapsEnhance 设置程序（CapsEnhanceSettings.exe）

   一个**表单式**的 CapsEnhance.ini 编辑器：布尔项是勾选框、数值项是输入框、
   三份进程名名单各是一个可增删的列表。保存 = 写回 ini + 让本体重启
   （`CapsEnhance.exe --restart`，本体重读配置才算生效）。

   为什么不是"直接编辑文本"：配置项的值域（LogMaxKB 0..65536、
   AutoScrollInterval 100..60000 …）与布尔语义（只认 0/1）在**配置文件里
   只是注释**，把原文摊给用户编辑等于让每一行都可能写出一个"解析得出来但
   不合理"的值。表单把这些约束变成控件本身（勾选框 / 数字框），写回的
   永远是钳制过的值。

   为什么是独立 exe 而不是给本体加个窗口：本体的主线程是 WH_KEYBOARD_LL
   的宿主 —— 钩子回调跑在装它的线程上（见 src/act.c 文件头）。设置窗口要跑
   消息循环、持有控件、还要写文件，任何一处阻塞都会直接表现为"键盘卡住 /
   钩子被系统摘掉"。这是两类进程，不该共用一个主线程。

   值域与键名**不在这里重复定义**：读用 config.c 的 ConfigGet*，写用
   ConfigSet*（钳制与解析同源），写回走 ConfigSaveFile —— 它按行做增量更新，
   用户的注释、未知段、BOM 与行尾风格都保留。

   高 DPI：进程声明 Per-Monitor V2（拿不到就退回系统级 DPI 感知），字体按
   当前 DPI 取系统消息字体，所有像素尺寸经 S() 缩放；拖到不同缩放的显示器
   上时按 WM_DPICHANGED 重建字体并重排（见 MakeFont / Layout）。
   ============================================================ */
#define WIN32_LEAN_AND_MEAN
#include "cap.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <shellapi.h>      /* ShellExecuteW（CreateProcess 失败时的兜底） */

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

#define APP_TITLE   L"CapsEnhance 设置"
#define WND_CLASS   L"CapsEnhanceSettingsMain"
#define INI_NAME    L"CapsEnhance.ini"
#define MAIN_EXE    L"CapsEnhance.exe"

/* ---- 控件 ID ---- */
#define IDC_STATUS  105
#define IDC_SAVE    102
#define IDC_RELOAD  103
#define IDC_EXIT    104

#define IDC_RUN_ADMIN  200
#define IDC_LOG_KB     201
#define IDC_STEPS      202
#define IDC_OVER_CTRL  203
#define IDC_WMUL       204
#define IDC_INTERVAL   205
#define IDC_FASTMS     206
#define IDC_HMUL       207
#define IDC_SCI        208

/* 三份名单各一组：列表框 / 输入框 / 添加 / 删除 / 标题 */
#define IDC_LIST0  300
#define IDC_NAME0  301
#define IDC_ADD0   302
#define IDC_DEL0   303
#define IDC_LAB0   304
#define LIST_STRIDE 10

/* ---------- 字段表：控件的"是什么"，不是"长什么样" ----------
   界面的每个输入框都从这里取标签、类型与范围；加一个配置项 = 加一行 +
   Load / Save 里各加一个分支（键名与值域仍只在 config.c 定义一次）。 */
typedef struct {
    int          id;
    const WCHAR* label;
    int          isBool;     /* 1 = 勾选框，0 = 整数输入框 */
    int          min, max;
} Field;

static const Field kFields[] = {
    { IDC_RUN_ADMIN, L"以管理员身份运行（RunAsAdmin，会弹 UAC）", 1, 0, 0 },
    { IDC_LOG_KB,    L"日志大小上限 LogMaxKB（KB，0 = 不限制）",   0, 0, 65536 },
    { IDC_STEPS,     L"按词跳转逐格上限 MaxArrowSteps（0 = 不限制）", 0, 0, 256 },
    { IDC_OVER_CTRL, L"超过逐格上限时改发 Ctrl+方向（否则只发一格）", 1, 0, 0 },
    { IDC_WMUL,      L"C+滚轮 放大倍数 WheelScrollMul（0/1 = 关闭）", 0, 0, 16 },
    { IDC_INTERVAL,  L"持续滚动初始速度 AutoScrollInterval（毫秒/格，越大越慢）", 0, 100, 60000 },
    { IDC_FASTMS,    L"持续滚动最快速度 AutoScrollFastMs（毫秒/格，越小越快）", 0, 20, 60000 },
    { IDC_HMUL,      L"C+Shift+滚轮 横向倍数 WheelHScrollMul（0 = 关闭）", 0, 0, 16 },
    { IDC_SCI,       L"允许读取 Scintilla 程序内存（关掉则退回逐格）", 1, 0, 0 },
};
#define FIELD_N ((int)(sizeof(kFields) / sizeof(kFields[0])))

typedef struct {
    const WCHAR* label;
    int          list;      /* CFG_LIST_* */
} ListGroup;
static const ListGroup kLists[] = {
    { L"原生按词跳转 [NativeWordJump]：Caps+N/M 不再探测文本，直接发 Ctrl+←/→",
      CFG_LIST_NATIVE },
    { L"自动屏蔽 [AutoDisable]：这些程序在前台时，本程序完全不介入（CapsLock 恢复正常）",
      CFG_LIST_AUTODISABLE },
    { L"持续滚动只发整格 [NoSmoothScroll]：不累积小数刻度（滚了不动）的程序",
      CFG_LIST_NOSMOOTH },
};
#define LIST_N ((int)(sizeof(kLists) / sizeof(kLists[0])))

static HWND   g_hwnd    = NULL;
static HWND   g_status  = NULL;
static WCHAR  g_dir[MAX_PATH]  = L"";    /* exe 所在目录（结尾带 '\'） */
static WCHAR  g_path[MAX_PATH] = L"";    /* CapsEnhance.ini 完整路径 */
static BOOL   g_dirty   = FALSE;
static BOOL   g_loading = FALSE;         /* 装载期屏蔽 EN_CHANGE，否则一打开就"未保存" */
static int    g_dpi     = 96;            /* 当前窗口 DPI（96 = 100%） */
static HFONT  g_font    = NULL;

/* 像素按 DPI 缩放：所有尺寸常量都必须过一遍它，否则高分屏上控件挤成一团
   （96 DPI 下量出来的坐标在 200% 屏上只有一半大）。 */
static int S(int v) { return MulDiv(v, g_dpi, 96); }

/* ---------------- 小工具 ---------------- */

static void SetStatus(const WCHAR* fmt, ...) {
    WCHAR buf[512];
    va_list ap; va_start(ap, fmt);
    _vsnwprintf_s(buf, 512, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (g_status) SetWindowTextW(g_status, buf);
}

static void UpdateTitle(void) {
    WCHAR t[256];
    _snwprintf_s(t, 256, _TRUNCATE, L"%s%s", APP_TITLE, g_dirty ? L" *" : L"");
    SetWindowTextW(g_hwnd, t);
}

static void MarkDirty(void) {
    if (g_loading) return;
    if (!g_dirty) { g_dirty = TRUE; UpdateTitle(); }
}

/* 输入校验：**只认十进制数字**（与配置解析器同一套严格度）。
   不合法的直接拦下并说清范围 —— 静默写个 0 进去才是对用户最坏的结果。 */
static BOOL ReadIntField(HWND edit, const Field* f, int* out) {
    WCHAR buf[64] = L"";
    GetWindowTextW(edit, buf, 64);
    const WCHAR* p = buf;
    while (*p == L' ' || *p == L'\t') p++;
    if (!*p) { *out = f->min; return FALSE; }
    long long v = 0;
    for (const WCHAR* q = p; *q; q++) {
        if (*q < L'0' || *q > L'9') { *out = f->min; return FALSE; }
        v = v * 10 + (*q - L'0');
        if (v > 100000000LL) { *out = f->min; return FALSE; }
    }
    *out = (int)v;
    return TRUE;
}

static void SetIntField(HWND edit, int v) {
    WCHAR buf[32];
    _snwprintf_s(buf, 32, _TRUNCATE, L"%d", v);
    SetWindowTextW(edit, buf);
}

static void SetCheck(HWND btn, BOOL on) {
    SendMessageW(btn, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}
static BOOL GetCheck(HWND btn) {
    return SendMessageW(btn, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

/* ---------------- 字体 / DPI ---------------- */

typedef BOOL  (WINAPI* PFN_SetProcessDpiAwarenessContext)(HANDLE);
typedef BOOL  (WINAPI* PFN_SetProcessDPIAware)(void);
typedef UINT  (WINAPI* PFN_GetDpiForWindow)(HWND);
typedef BOOL  (WINAPI* PFN_SystemParametersInfoForDpi)(UINT, UINT, PVOID, UINT, UINT);

/* 声明 DPI 感知：**必须早于创建任何窗口**。不声明的话系统会按 96 DPI 把
   整个窗口**位图拉伸**到高 DPI 上 —— 文字发虚就是这个原因，而不是"字体太小"。
   Per-Monitor V2 拿不到（Win10 1607 之前）就退回进程级感知，再不行就听天由命。 */
static void InitDpiAwareness(void) {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) {
        PFN_SetProcessDpiAwarenessContext p =
            (PFN_SetProcessDpiAwarenessContext)GetProcAddress(u, "SetProcessDpiAwarenessContext");
        /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4 */
        if (p && p((HANDLE)(INT_PTR)-4)) return;
        PFN_SetProcessDPIAware q =
            (PFN_SetProcessDPIAware)GetProcAddress(u, "SetProcessDPIAware");
        if (q) q();
    }
}

static UINT QueryDpi(void) {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u && g_hwnd) {
        PFN_GetDpiForWindow p = (PFN_GetDpiForWindow)GetProcAddress(u, "GetDpiForWindow");
        if (p) { UINT d = p(g_hwnd); if (d >= 96) return d; }
    }
    HDC hdc = GetDC(NULL);
    UINT d = 96;
    if (hdc) { d = (UINT)GetDeviceCaps(hdc, LOGPIXELSY); ReleaseDC(NULL, hdc); }
    return d ? d : 96;
}

/* 按当前 DPI 建字体：优先用系统为这个 DPI 算好的消息字体（NONCLIENTMETRICS
   里的 lfMessageFont 已经缩放过），拿不到就按 DPI 自己算一个字号。 */
static void MakeFont(void) {
    HFONT nf = NULL;
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) {
        PFN_SystemParametersInfoForDpi p =
            (PFN_SystemParametersInfoForDpi)GetProcAddress(u, "SystemParametersInfoForDpi");
        if (p) {
            NONCLIENTMETRICSW ncm;
            memset(&ncm, 0, sizeof(ncm));
            ncm.cbSize = sizeof(ncm);
            if (p(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, (UINT)g_dpi))
                nf = CreateFontIndirectW(&ncm.lfMessageFont);
        }
    }
    if (!nf)
        nf = CreateFontW(-MulDiv(12, g_dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (!nf) nf = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    if (g_font && g_font != nf) DeleteObject(g_font);
    g_font = nf;
}

static BOOL CALLBACK FontApplyCb(HWND h, LPARAM lp) {
    SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    (void)lp;
    return TRUE;
}
static void ApplyFont(void) { EnumChildWindows(g_hwnd, FontApplyCb, 0); }

/* ---------------- 载入 / 保存 ---------------- */

static void FillList(HWND lb, int list) {
    SendMessageW(lb, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < ConfigListCount(list); i++)
        SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)ConfigListAt(list, i));
}

static void LoadConfig(void) {
    /* 文件不存在时让 config.c 写一份带注释的模板（与本体首次启动一致），
       再来读 —— 否则界面是空的，用户不知道该填什么。 */
    if (GetFileAttributesW(g_path) == INVALID_FILE_ATTRIBUTES)
        ConfigLoad();
    ConfigLoadFrom(g_path);

    g_loading = TRUE;
    for (int i = 0; i < FIELD_N; i++) {
        HWND c = GetDlgItem(g_hwnd, kFields[i].id);
        if (!c) continue;
        if (kFields[i].isBool) {
            switch (kFields[i].id) {
            case IDC_RUN_ADMIN: SetCheck(c, ConfigGetRunAsAdmin()); break;
            case IDC_OVER_CTRL: SetCheck(c, ConfigGetOverStepCtrl()); break;
            case IDC_SCI:       SetCheck(c, ConfigGetAllowScintillaRead()); break;
            default: break;
            }
        } else {
            switch (kFields[i].id) {
            case IDC_LOG_KB:   SetIntField(c, ConfigGetLogMaxKB()); break;
            case IDC_STEPS:    SetIntField(c, ConfigGetMaxArrowSteps()); break;
            case IDC_WMUL:     SetIntField(c, ConfigGetWheelScrollMul()); break;
            case IDC_INTERVAL: SetIntField(c, ConfigGetAutoScrollInterval()); break;
            case IDC_FASTMS:   SetIntField(c, ConfigGetAutoScrollFastMs()); break;
            case IDC_HMUL:     SetIntField(c, ConfigGetWheelHScrollMul()); break;
            default: break;
            }
        }
    }
    for (int i = 0; i < LIST_N; i++) {
        FillList(GetDlgItem(g_hwnd, IDC_LIST0 + i * LIST_STRIDE), kLists[i].list);
        SetWindowTextW(GetDlgItem(g_hwnd, IDC_NAME0 + i * LIST_STRIDE), L"");
    }
    g_loading = FALSE;
    g_dirty = FALSE;
    UpdateTitle();
    SetStatus(L"%s（Ctrl+S 保存并重启 CapsEnhance）", g_path);
}

/* 保存 = 界面 -> 配置层 -> 增量写回 ini -> 让本体重启。
   为什么不整份重写：ini 里有大量注释，用户也可能留下我们不认识的段/键，
   整份重写会把它们冲掉。ConfigSaveFile 只改"我们认识的那几行"。 */
static BOOL SaveConfig(void) {
    for (int i = 0; i < FIELD_N; i++) {
        if (kFields[i].isBool) continue;
        HWND c = GetDlgItem(g_hwnd, kFields[i].id);
        if (!c) continue;
        int v = 0;
        if (!ReadIntField(c, &kFields[i], &v)) {
            WCHAR msg[320];
            _snwprintf_s(msg, 320, _TRUNCATE,
                         L"「%s」只能填数字（%d..%d）。", kFields[i].label,
                         kFields[i].min, kFields[i].max);
            MessageBoxW(g_hwnd, msg, APP_TITLE, MB_ICONEXCLAMATION | MB_OK);
            SetFocus(c);
            return FALSE;
        }
    }

    for (int i = 0; i < FIELD_N; i++) {
        HWND c = GetDlgItem(g_hwnd, kFields[i].id);
        if (!c) continue;
        int v = 0;
        if (kFields[i].isBool) {
            switch (kFields[i].id) {
            case IDC_RUN_ADMIN: ConfigSetRunAsAdmin(GetCheck(c)); break;
            case IDC_OVER_CTRL: ConfigSetOverStepCtrl(GetCheck(c)); break;
            case IDC_SCI:       ConfigSetAllowScintillaRead(GetCheck(c)); break;
            default: break;
            }
        } else {
            ReadIntField(c, &kFields[i], &v);      /* 校验已过，这里必然成功 */
            switch (kFields[i].id) {
            case IDC_LOG_KB:   ConfigSetLogMaxKB(v); break;
            case IDC_STEPS:    ConfigSetMaxArrowSteps(v); break;
            case IDC_WMUL:     ConfigSetWheelScrollMul(v); break;
            case IDC_INTERVAL: ConfigSetAutoScrollInterval(v); break;
            case IDC_FASTMS:   ConfigSetAutoScrollFastMs(v); break;
            case IDC_HMUL:     ConfigSetWheelHScrollMul(v); break;
            default: break;
            }
        }
    }

    /* 名单：以列表框为准整份替换（顺序即界面顺序）。
       条数上限由 ConfigSetList 在配置层守住。 */
    for (int i = 0; i < LIST_N; i++) {
        HWND lb = GetDlgItem(g_hwnd, IDC_LIST0 + i * LIST_STRIDE);
        int n = (int)SendMessageW(lb, LB_GETCOUNT, 0, 0);
        static WCHAR   names[CFG_MAX_ENTRIES][CFG_NAME_MAX];
        static const WCHAR* ptr[CFG_MAX_ENTRIES];
        if (n > CFG_MAX_ENTRIES) n = CFG_MAX_ENTRIES;
        for (int j = 0; j < n; j++) {
            SendMessageW(lb, LB_GETTEXT, (WPARAM)j, (LPARAM)names[j]);
            ptr[j] = names[j];
        }
        ConfigSetList(kLists[i].list, n ? ptr : NULL, n);
    }

    if (!ConfigSaveFile(g_path)) {
        SetStatus(L"保存失败：写不进 %s（文件被占用或目录只读？）", g_path);
        MessageBoxW(g_hwnd, L"保存失败：无法写入配置文件。\n"
                            L"可能文件正被其它程序占用，或所在目录只读。",
                    APP_TITLE, MB_ICONERROR | MB_OK);
        return FALSE;
    }
    g_dirty = FALSE;
    UpdateTitle();
    SetStatus(L"已保存 %s，正在重启 CapsEnhance…", g_path);
    return TRUE;
}

/* 让本体重启：带 --restart 启动一次 —— 若本体在跑，它只是转告在跑的那个实例
   重启后自己退出；若没在跑，这次启动就是"配好之后直接开跑"。 */
static void RestartMain(void) {
    WCHAR path[MAX_PATH];
    _snwprintf_s(path, MAX_PATH, _TRUNCATE, L"%s%s", g_dir, MAIN_EXE);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        SetStatus(L"已保存，但同目录下没有 %s —— 本体不会重启", MAIN_EXE);
        return;
    }
    WCHAR cmd[MAX_PATH + 32];
    _snwprintf_s(cmd, MAX_PATH + 32, _TRUNCATE, L"\"%s\" --restart", path);

    STARTUPINFOW si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (CreateProcessW(path, cmd, NULL, NULL, FALSE, 0, NULL, g_dir, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        SetStatus(L"已保存并请求重启 CapsEnhance");
        return;
    }
    if ((int)(INT_PTR)ShellExecuteW(NULL, L"open", path, L"--restart", g_dir,
                                    SW_SHOWNORMAL) > 32) {
        SetStatus(L"已保存并请求重启 CapsEnhance");
        return;
    }
    SetStatus(L"已保存，但重启 CapsEnhance 失败（请从托盘手动重启）");
}

static void DoSave(void) {
    if (SaveConfig()) RestartMain();
}

static void AskSaveAndClose(void) {
    if (!g_dirty) { DestroyWindow(g_hwnd); return; }
    int r = MessageBoxW(g_hwnd, L"配置已修改，是否保存并重启 CapsEnhance？",
                        APP_TITLE, MB_ICONQUESTION | MB_YESNOCANCEL | MB_DEFBUTTON1);
    if (r == IDCANCEL) return;
    if (r == IDYES) DoSave();
    DestroyWindow(g_hwnd);
}

/* ---------------- 界面 ---------------- */

/* 行高是固定的，位置按"从上往下排"算 —— 比手填坐标不容易错位。
   名单组的列表框吃掉剩余空间，窗口变高时先受益的是它。 */
static void Layout(int w, int h) {
    int x = S(12), y = S(10);
    int rowBool = S(24), rowInt = S(26), gap = S(4);
    int editW = S(92);

    for (int i = 0; i < FIELD_N; i++) {
        int id = kFields[i].id;
        HWND c = GetDlgItem(g_hwnd, id);
        if (!c) continue;
        if (kFields[i].isBool) {
            MoveWindow(c, x, y, w - x * 2, rowBool - S(2), TRUE);
            y += rowBool;
        } else {
            /* 数字框放**左边**、标签放右边：标签是长句（含键名与说明），
               把输入框放在标签之后的话，窗口一窄它就被挤出客户区 ——
               用户看到"有说明、没地方填"。输入框宽固定，标签吃剩下的空间。 */
            HWND lab = GetDlgItem(g_hwnd, id + 500);
            MoveWindow(c, x, y, editW, S(22), TRUE);
            if (lab)
                MoveWindow(lab, x + editW + S(10), y + S(4),
                           w - x * 2 - editW - S(10), S(18), TRUE);
            y += rowInt;
        }
        y += gap;
    }

    y += S(6);
    int bottom = h - S(34) - S(24);                 /* 底部留给状态栏 + 按钮 */
    int listH = (bottom - y) / LIST_N - (S(20) + S(28) + S(8));
    if (listH < S(42)) listH = S(42);

    for (int i = 0; i < LIST_N; i++) {
        int base = IDC_LIST0 + i * LIST_STRIDE;
        MoveWindow(GetDlgItem(g_hwnd, base + 4), x, y, w - x * 2, S(18), TRUE);   /* 标题 */
        y += S(20);
        MoveWindow(GetDlgItem(g_hwnd, base), x, y, w - x * 2, listH, TRUE);
        y += listH + S(4);
        MoveWindow(GetDlgItem(g_hwnd, base + 1), x, y, S(220), S(24), TRUE);      /* 名字输入 */
        MoveWindow(GetDlgItem(g_hwnd, base + 2), x + S(228), y, S(70), S(24), TRUE);
        MoveWindow(GetDlgItem(g_hwnd, base + 3), x + S(304), y, S(70), S(24), TRUE);
        y += S(28) + S(8);
    }

    int sy = h - S(34) - S(22);
    if (g_status) MoveWindow(g_status, x, sy, w - x * 2 - S(300), S(18), TRUE);
    int by = h - S(34), bw = S(96);
    MoveWindow(GetDlgItem(g_hwnd, IDC_SAVE),   w - x - bw * 3 - S(16), by, bw, S(26), TRUE);
    MoveWindow(GetDlgItem(g_hwnd, IDC_RELOAD), w - x - bw * 2 - S(8),  by, bw, S(26), TRUE);
    MoveWindow(GetDlgItem(g_hwnd, IDC_EXIT),   w - x - bw,             by, bw, S(26), TRUE);
}

/* 输入框的子类：只为接 Ctrl+S（控件有焦点时主窗口收不到按键） */
static WNDPROC g_editOldProc = NULL;
static LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN && (w == 'S' || w == VK_RETURN) &&
        (GetKeyState(VK_CONTROL) & 0x8000)) {
        DoSave();
        return 0;
    }
    return CallWindowProcW(g_editOldProc, h, m, w, l);
}

static void AddName(int i) {
    int base = IDC_LIST0 + i * LIST_STRIDE;
    HWND ed = GetDlgItem(g_hwnd, base + 1);
    HWND lb = GetDlgItem(g_hwnd, base);
    WCHAR raw[CFG_NAME_MAX] = L"";
    GetWindowTextW(ed, raw, CFG_NAME_MAX);

    /* 去首尾空白：界面里粘进来的名字常带空格，而名单匹配是**完全匹配** */
    size_t l = wcslen(raw);
    while (l > 0 && (raw[l - 1] == L' ' || raw[l - 1] == L'\t')) raw[--l] = 0;
    const WCHAR* lead = raw;
    while (*lead == L' ' || *lead == L'\t') lead++;

    WCHAR buf[CFG_NAME_MAX];
    _snwprintf_s(buf, CFG_NAME_MAX, _TRUNCATE, L"%s", lead);
    /* 去掉结尾的 .exe：名单里存的是"不带后缀"的形态（匹配时两边都去），
       界面显示的样子就该是**真正存进去的样子** —— 否则用户看到
       SmokeApp.exe、文件里却写着 SmokeApp，下次打开又变了个样。 */
    l = wcslen(buf);
    if (l >= 4 && _wcsicmp(buf + l - 4, L".exe") == 0) buf[l - 4] = 0;
    if (!buf[0]) return;

    /* 去重：同样的名字出现两次没有意义（完全匹配，重复项纯噪音） */
    if (SendMessageW(lb, LB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)buf) != LB_ERR) {
        SetWindowTextW(ed, L"");
        return;
    }
    SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)buf);
    SetWindowTextW(ed, L"");
    SetFocus(ed);
    MarkDirty();
}

static void DelName(int i) {
    int base = IDC_LIST0 + i * LIST_STRIDE;
    HWND lb = GetDlgItem(g_hwnd, base);
    int sel = (int)SendMessageW(lb, LB_GETCURSEL, 0, 0);
    if (sel == LB_ERR) return;
    SendMessageW(lb, LB_DELETESTRING, (WPARAM)sel, 0);
    int n = (int)SendMessageW(lb, LB_GETCOUNT, 0, 0);
    SendMessageW(lb, LB_SETCURSEL, (WPARAM)((sel < n) ? sel : n - 1), 0);
    MarkDirty();
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == IDC_SAVE)   { DoSave(); return 0; }
        if (id == IDC_EXIT)   { AskSaveAndClose(); return 0; }
        if (id == IDC_RELOAD) {
            if (g_dirty && MessageBoxW(h, L"放弃未保存的改动并重新加载？", APP_TITLE,
                                       MB_ICONQUESTION | MB_YESNO) != IDYES) return 0;
            LoadConfig();
            return 0;
        }
        if (id >= IDC_LIST0 && id < IDC_LIST0 + LIST_N * LIST_STRIDE) {
            int i = (id - IDC_LIST0) / LIST_STRIDE;
            int sub = (id - IDC_LIST0) % LIST_STRIDE;
            if (sub == 2) { AddName(i); return 0; }
            if (sub == 3) { DelName(i); return 0; }
            if (sub == 0 && HIWORD(w) == LBN_DBLCLK) { DelName(i); return 0; }
            return 0;
        }
        /* 勾选框 / 输入框：任何改动都算"有未保存的改动" */
        if (HIWORD(w) == BN_CLICKED || HIWORD(w) == EN_CHANGE) MarkDirty();
        return 0;
    }
    case WM_DPICHANGED:
        /* 拖到另一块不同缩放的显示器上：DPI 变了 -> 字体与所有尺寸都要重算，
           并按系统给的建议矩形调整窗口（否则窗口大小不变、内容挤在一起）。 */
        g_dpi = (int)HIWORD(w);
        if (g_dpi < 96) g_dpi = 96;
        MakeFont();
        ApplyFont();
        if (l) {
            RECT* r = (RECT*)l;
            SetWindowPos(h, NULL, r->left, r->top,
                         r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    case WM_SIZE:
        Layout(LOWORD(l), HIWORD(l));
        return 0;
    case WM_CLOSE:
        AskSaveAndClose();
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(h, m, w, l);
    }
}

static HWND MakeBtn(HINSTANCE hi, const WCHAR* text, int id) {
    return CreateWindowExW(0, L"BUTTON", text,
                           WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                           0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)id, hi, NULL);
}
static HWND MakeLabel(HINSTANCE hi, const WCHAR* text, int id) {
    return CreateWindowExW(0, L"STATIC", text,
                           WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
                           0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)id, hi, NULL);
}

/* 形参写成与 SDK 声明一致的 SAL 批注：不加的话 /analyze 会报 C28251
   （"WinMain 的批注不一致"）。纯噪音，但要保持 lint 零告警就得加。 */
int WINAPI WinMain(_In_ HINSTANCE hInst, _In_opt_ HINSTANCE hPrev,
                   _In_ LPSTR cmd, _In_ int show) {
    (void)hPrev; (void)cmd; (void)show;

    /* 单实例：已经开着就把那个窗口叫到前面 —— 两个设置窗口同时改同一个 ini
       只会互相覆盖（后保存的赢），用户却看不出为什么改动丢了。 */
    {
        HWND prev = FindWindowW(WND_CLASS, NULL);
        if (prev) {
            if (IsIconic(prev)) ShowWindow(prev, SW_RESTORE);
            SetForegroundWindow(prev);
            return 0;
        }
    }

    InitDpiAwareness();      /* 必须在任何窗口创建之前 */

    /* 配置文件的路径 = **本 exe 同目录**，不是当前工作目录：
       用户可能从任意地方（快捷方式 / 资源管理器）启动它。 */
    {
        WCHAR exe[MAX_PATH] = L"";
        if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return 1;
        WCHAR* slash = wcsrchr(exe, L'\\');
        if (slash) *(slash + 1) = 0;
        _snwprintf_s(g_dir, MAX_PATH, _TRUNCATE, L"%s", exe);
        _snwprintf_s(g_path, MAX_PATH, _TRUNCATE, L"%s%s", g_dir, INI_NAME);
    }

    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    /* IDC_ARROW 是 MAKEINTRESOURCE(...)（窄字符串常量），这里调的是 W 版，显式转一步 */
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)(ULONG_PTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = WND_CLASS;
    if (!RegisterClassW(&wc)) return 1;

    /* DPI 要在**建窗之前**知道：窗口尺寸本身就得按它算（否则 200% 屏上
       窗口只有一半大）。QueryDpi 在没有 hwnd 时退回查屏幕 DC。 */
    g_dpi = (int)QueryDpi();
    if (g_dpi < 96) g_dpi = 96;

    /* 初始大小按 DPI 缩放，并**钳到工作区以内**：高 DPI 笔记本（150% + 768 高）
       上按 96 DPI 算出来的 780 像素窗口会超出屏幕底部，底部的按钮与状态栏
       就永远看不见了。 */
    int winW = S(880), winH = S(780);
    {
        RECT wa;
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
            int aw = wa.right - wa.left - S(40), ah = wa.bottom - wa.top - S(60);
            if (aw > S(480) && winW > aw) winW = aw;
            if (ah > S(360) && winH > ah) winH = ah;
        }
    }

    g_hwnd = CreateWindowExW(0, WND_CLASS, APP_TITLE,
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, winW, winH,
                             NULL, NULL, hInst, NULL);
    if (!g_hwnd) return 1;

    /* 窗口已存在：再问一次 Per-Monitor 的精确值（这块屏可能是 144 而主屏是 96） */
    g_dpi = (int)QueryDpi();
    if (g_dpi < 96) g_dpi = 96;
    MakeFont();

    for (int i = 0; i < FIELD_N; i++) {
        int id = kFields[i].id;
        if (kFields[i].isBool) {
            CreateWindowExW(0, L"BUTTON", kFields[i].label,
                            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                            0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)id, hInst, NULL);
        } else {
            MakeLabel(hInst, kFields[i].label, id + 500);
            HWND e = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                     WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
                                     0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)id, hInst, NULL);
            /* 数字框：限 8 位（最大值 60000 也远用不完），防止粘进一大段文本 */
            SendMessageW(e, EM_LIMITTEXT, 8, 0);
            /* 每个输入框都要能吃 Ctrl+S（焦点多半就在某个输入框里） */
            g_editOldProc = (WNDPROC)SetWindowLongPtrW(e, GWLP_WNDPROC, (LONG_PTR)EditProc);
        }
    }

    for (int i = 0; i < LIST_N; i++) {
        int base = IDC_LIST0 + i * LIST_STRIDE;
        MakeLabel(hInst, kLists[i].label, base + 4);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                        WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP |
                        LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                        0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)base, hInst, NULL);
        HWND ed = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                  WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
                                  0, 0, 10, 10, g_hwnd,
                                  (HMENU)(INT_PTR)(base + 1), hInst, NULL);
        SendMessageW(ed, EM_LIMITTEXT, CFG_NAME_MAX - 1, 0);
        MakeBtn(hInst, L"添加", base + 2);
        MakeBtn(hInst, L"删除", base + 3);
    }

    g_status = MakeLabel(hInst, L"", IDC_STATUS);
    MakeBtn(hInst, L"保存并重启", IDC_SAVE);
    MakeBtn(hInst, L"重新加载", IDC_RELOAD);
    MakeBtn(hInst, L"退出", IDC_EXIT);

    ApplyFont();          /* 一次给所有子控件设字体（含刚建出来的） */
    LoadConfig();
    {
        RECT rc; GetClientRect(g_hwnd, &rc);
        Layout(rc.right, rc.bottom);
    }
    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        /* Tab 在控件之间移动焦点：IsDialogMessage 让输入框也吃 Tab，
           否则设置界面里没法用键盘走完一整张表单。 */
        if (!IsDialogMessageW(g_hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return 0;
}
