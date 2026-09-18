/* ============================================================
   CapsEnhance 设置程序（CapsEnhanceSettings.exe）

   一个**表单式**的 CapsEnhance.ini 编辑器：布尔项是勾选框、数值项是输入框、
   五份名单各是一个可增删的列表。列表那一行还有"选取窗口"：点它再点别的窗口，
   取到的东西**先填进左侧输入框**（不直接入名单），确认后点"添加"才加进去 ——
   这样能先看清取到了什么、也能顺手改一改。
   取什么由名单决定：进程名单取进程名，窗口类名单取**类名**（原样，无前缀）。
   保存 = 写回 ini + 让本体重启
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

   滚动：内容（数值项 + 五份名单）比窗口高时出竖向滚动条，滚轮和滚动条
   都能滚；底部按钮与状态栏是"窗框"，不参与滚动（见 SetScroll / Layout）。
   滚轮若停在**某个列表框上**，滚的是那个列表框而不是整个表单
   （见 WndProc 的 WM_MOUSEWHEEL / ScrollListBy）。
   ============================================================ */
#define WIN32_LEAN_AND_MEAN
#include "cap.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <shellapi.h>      /* ShellExecuteW（CreateProcess 失败时的兜底） */
#include <commdlg.h>       /* ChooseFontW：系统标准字体对话框 */

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comdlg32.lib")

#define APP_TITLE   L"CapsEnhance 设置"
#define WND_CLASS   L"CapsEnhanceSettingsMain"
#define FOOTER_CLASS L"CapsEnhanceSettingsFooter"
#define INI_NAME    L"CapsEnhance.ini"
#define MAIN_EXE    L"CapsEnhance.exe"

/* ---- 控件 ID ---- */
#define IDC_SAVEONLY 101   /* 只保存不重启 */
#define IDC_STATUS  105
#define IDC_SAVE    102    /* 保存并重启 */
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
#define IDC_IME_CAPS   209
#define IDC_IME_LSHF   210
#define IDC_IME_RSHF   211
#define IDC_DITTO_DB   212
#define IDC_CLIP_PICK  213   /* 按钮：显示当前字体 + 字号，点它弹系统字体对话框 */
#define IDC_CLIP_CLICK 214   /* 勾选框：单击条目直接上屏 */

/* 五份名单各一组：列表框 / 输入框 / 添加 / 删除 / 标题 / 选取窗口 */
#define IDC_LIST0  300
#define IDC_NAME0  301
#define IDC_ADD0   302
#define IDC_DEL0   303
#define IDC_LAB0   304
#define LIST_STRIDE 10

/* ---------- 字段表：控件的"是什么"，不是"长什么样" ----------
   界面的每个输入框都从这里取标签、类型与范围；加一个配置项 = 加一行 +
   Load / Save 里各加一个分支（键名与值域仍只在 config.c 定义一次）。
   标签只写中文说明 —— ini 里的键名属于配置文件的内部拼法，界面不重复；
   数值单位（KB / 毫秒 …）单独一列，显示在输入框右侧。 */
typedef struct {
    int          id;
    const WCHAR* label;
    const WCHAR* unit;      /* 数值单位，显示在输入框右侧；勾选框与文本框为 NULL */
    int          isBool;    /* 1 = 勾选框，0 = 输入框 */
    int          isText;    /* 1 = **文本**输入框（路径这类值）；与 isBool 互斥。
                               之所以要单独一位：文本行没有 min/max，也不该走
                               "只能填数字"那条校验 —— 用 isBool 的三态去编码
                               就会在校验分支里冒出 `if (isBool == 2)` 这种暗号。 */
    int          isBtn;     /* 1 = 按钮（label 就是按钮上的字）。
                               按钮不承载配置值，点击行为在 WM_COMMAND 里按 id 分支。 */
    int          min, max;
} Field;

static const Field kFields[] = {
    { IDC_RUN_ADMIN, L"以管理员身份运行（会弹 UAC）",                   NULL,      1, 0, 0, 0, 0 },
    { IDC_LOG_KB,    L"日志大小上限（0 = 不限制）",                    L"KB",     0, 0, 0, 0, 65536 },
    { IDC_STEPS,     L"按词跳转逐格上限（0 = 不限制，上限 256）",       L"格",     0, 0, 0, 0, 256 },
    { IDC_OVER_CTRL, L"超过逐格上限时改发 Ctrl+方向（取消勾选只发一次方向键）", NULL,      1, 0, 0, 0, 0 },
    { IDC_WMUL,      L"Caps+滚轮 放大倍数（0/1 = 关闭）",              L"倍",     0, 0, 0, 0, 16 },
    { IDC_INTERVAL,  L"持续滚动初始速度（越大越慢）",                  L"毫秒/格", 0, 0, 0, 100, 60000 },
    { IDC_FASTMS,    L"持续滚动最快速度（越小越快）",                  L"毫秒/格", 0, 0, 0, 20, 60000 },
    { IDC_HMUL,      L"Caps+Shift+滚轮 横向倍数（0 = 关闭）",          L"倍",     0, 0, 0, 0, 16 },
    { IDC_SCI,       L"允许读取 Scintilla 程序内存（关掉则退回逐格）", NULL,      1, 0, 0, 0, 0 },
    /* 单键切换输入法：0=不处理 1=切中文 2=切英文（值域与语义只在 config.c 定义） */
    { IDC_IME_CAPS,  L"单击 Caps 时切输入法（0=不处理 1=中文 2=英文）",  NULL,     0, 0, 0, 0, 2 },
    { IDC_IME_LSHF,  L"单击左 Shift 时切输入法（同上；英文键盘下都不处理）", NULL,   0, 0, 0, 0, 2 },
    { IDC_IME_RSHF,  L"单击右 Shift 时切输入法（同上；英文键盘下都不处理）", NULL,   0, 0, 0, 0, 2 },
    /* Ditto 剪贴板数据库路径（`C+V` 读它）：**留空 = 不启用**。相对路径的
       基准是本程序 exe 所在目录 —— 这一句写在标签里而不是让用户去猜。 */
    { IDC_DITTO_DB,  L"Ditto 文件夹路径（含 Ditto.db 与 Ditto.exe；留空 = 不启用 Caps+V；相对路径相对本程序目录）",
                     NULL, 0, 1, 0, 0, 0 },
    /* 快速粘贴的字体只有一个入口：按钮上**显示当前字体 + 字号**，点它弹系统
       字体对话框（对话框里本来就能同时选字体和字号，再给个独立字号框是重复）。 */
    { IDC_CLIP_PICK, L"快速粘贴字体",                                   NULL, 0, 0, 1, 0, 0 },
    { IDC_CLIP_CLICK, L"单击条目直接上屏（取消勾选则单击只选中、双击才粘）", NULL, 1, 0, 0, 0, 0 },
};
#define FIELD_N ((int)(sizeof(kFields) / sizeof(kFields[0])))

/* 选窗时该取什么：进程名单取进程名（`xxx.exe`）；**窗口类名单要取类名**
   —— 把进程名填进"窗口类"名单里永远匹配不上。取到的类名**原样**填进输入框
   （不带任何前缀：本项目的窗口名单只按类名判定，没有"标题"这一维）。 */
#define PICK_PROC  0
#define PICK_CLASS 1

typedef struct {
    const WCHAR* label;
    int          list;      /* CFG_LIST_* */
    int          pickKind;  /* PICK_PROC / PICK_CLASS */
} ListGroup;
static const ListGroup kLists[] = {
    { L"原生按词跳转 [NativeWordJump]：Caps+N/O 不再探测文本，直接发 Ctrl+←/→",
      CFG_LIST_NATIVE,     PICK_PROC },
    { L"自动屏蔽 [AutoDisable]：这些程序在前台时，本程序完全不介入（CapsLock 恢复正常）",
      CFG_LIST_AUTODISABLE, PICK_PROC },
    { L"持续滚动只发整格 [NoSmoothScroll]：不累积小数刻度（滚了不动）的程序",
      CFG_LIST_NOSMOOTH,   PICK_PROC },
    { L"拖动窗口黑名单 [DragWindowBlacklist]：这些窗口类不许 Caps+左键拖动",
      CFG_LIST_DRAGWIN,    PICK_CLASS },
    { L"拖动进程黑名单 [DragProcessBlacklist]：这些进程的窗口一律不许拖动",
      CFG_LIST_DRAGPROC,   PICK_PROC },
};
#define LIST_N ((int)(sizeof(kLists) / sizeof(kLists[0])))

static HWND   g_hwnd    = NULL;
static HWND   g_footer  = NULL;         /* 底部"窗框"：状态栏 + 按钮的实底托盘 */
static HWND   g_status  = NULL;
static WCHAR  g_dir[MAX_PATH]  = L"";    /* exe 所在目录（结尾带 '\'） */
static WCHAR  g_path[MAX_PATH] = L"";    /* CapsEnhance.ini 完整路径 */
static BOOL   g_dirty   = FALSE;
static BOOL   g_loading = FALSE;         /* 装载期屏蔽 EN_CHANGE，否则一打开就"未保存" */
static int    g_dpi     = 96;            /* 当前窗口 DPI（96 = 100%） */
static HFONT  g_font    = NULL;
static int    g_viewH    = 0;            /* 内容可视区高度（底部按钮/状态栏不算） */
static int    g_contentH = 0;            /* 内容总高；比可视区高就出滚动条 */
static int    g_laidScroll = -1;         /* 子控件当前是按哪个滚动位置排的 */

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

static void UpdateClipFontBtn(void);   /* 定义在字体对话框那一节（用到 g_dpi） */

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
        } else if (kFields[i].isText) {
            switch (kFields[i].id) {
            case IDC_DITTO_DB:  SetWindowTextW(c, ConfigGetDittoDir()); break;

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
            case IDC_IME_CAPS: SetIntField(c, ConfigGetImeMode(IME_KEY_CAPS)); break;
            case IDC_IME_LSHF: SetIntField(c, ConfigGetImeMode(IME_KEY_LSHIFT)); break;
            case IDC_IME_RSHF: SetIntField(c, ConfigGetImeMode(IME_KEY_RSHIFT)); break;
            case IDC_CLIP_CLICK: SetCheck(c, ConfigGetClipClickPaste()); break;

            default: break;
            }
        }
    }
    for (int i = 0; i < LIST_N; i++) {
        FillList(GetDlgItem(g_hwnd, IDC_LIST0 + i * LIST_STRIDE), kLists[i].list);
        SetWindowTextW(GetDlgItem(g_hwnd, IDC_NAME0 + i * LIST_STRIDE), L"");
    }
    UpdateClipFontBtn();   /* 按钮文字就是当前值，加载后刷一次 */
    g_loading = FALSE;
    g_dirty = FALSE;
    UpdateTitle();
    SetStatus(L"");
}

/* 保存 = 界面 -> 配置层 -> 增量写回 ini -> 让本体重启。
   为什么不整份重写：ini 里有大量注释，用户也可能留下我们不认识的段/键，
   整份重写会把它们冲掉。ConfigSaveFile 只改"我们认识的那几行"。 */
static BOOL SaveConfig(void) {
    for (int i = 0; i < FIELD_N; i++) {
        if (kFields[i].isBool || kFields[i].isText || kFields[i].isBtn) continue;
        /* 文本框与按钮没有"值域"可校验 */
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
        } else if (kFields[i].isText) {
            WCHAR t[DITTO_DIR_MAX];
            GetWindowTextW(c, t, DITTO_DIR_MAX);
            switch (kFields[i].id) {
            case IDC_DITTO_DB:  ConfigSetDittoDir(t); break;
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
            case IDC_IME_CAPS: ConfigSetImeMode(IME_KEY_CAPS, v); break;
            case IDC_IME_LSHF: ConfigSetImeMode(IME_KEY_LSHIFT, v); break;
            case IDC_IME_RSHF:  ConfigSetImeMode(IME_KEY_RSHIFT, v); break;
            case IDC_CLIP_CLICK: ConfigSetClipClickPaste(GetCheck(c)); break;
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
    /* 文案对"只保存"和"保存并重启"都成立：真重启的话 RestartMain 会马上
       把状态栏覆写成"已保存并请求重启"。 */
    SetStatus(L"已保存 %s（重启 CapsEnhance 后生效）", g_path);
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

/* ---------------- 滚动 ----------------
   名单高度固定，内容总高因此可算：窗口装得下一屏铺完，装不下出竖向滚动条。
   **滚动条位置是唯一事实来源**：子控件落位一律是"虚拟 y − 滚动条位置"，
   每次滚动都按它全量重排。为什么不用 ScrollWindowEx 做增量平移：一是它
   平移子控件时会顺带把它们的 z 序提到顶（溢出内容会盖住底部"窗框"），
   二是滚动条可能被外部直接拨动（读屏 / UIA 走 Scroll 模式，不经过我们的
   WM_VSCROLL），增量平移会让子控件位置和滚动位置永久脱节 —— 每次全量
   重排（MoveWindow 不改 z 序）这两个坑都没有。 */

static int MaxScroll(void) {
    return g_contentH > g_viewH ? g_contentH - g_viewH : 0;
}

static int ScrollPos(void) {
    SCROLLINFO si;
    memset(&si, 0, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask  = SIF_POS;
    GetScrollInfo(g_hwnd, SB_VERT, &si);
    return si.nPos;
}

static void UpdateScrollBar(int pos) {
    SCROLLINFO si;
    memset(&si, 0, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask  = SIF_RANGE | SIF_PAGE | SIF_POS;
    si.nMin   = 0;
    si.nMax   = g_contentH > 0 ? g_contentH - 1 : 0;
    si.nPage  = (UINT)(g_viewH > 0 ? g_viewH : 1);
    si.nPos   = pos;
    SetScrollInfo(g_hwnd, SB_VERT, &si, TRUE);
}

/* 行高是固定的，位置按"从上往下排"算 —— 比手填坐标不容易错位。
   每次都全量落位（虚拟 y − 滚动位置），见"滚动"一节的总注释。 */
static void Layout(int w, int h) {
    int x = S(12), y = S(10);
    int rowBool = S(24), rowInt = S(26), gap = S(4);
    int editW = S(92);

    /* 内容可视区 = 客户区高度减掉底部固定区（状态栏 + 按钮 + 空隙）。
       窗口被拖得很矮时内容区保底 S(120)，此时底部窗框会被挤得薄于 S(60)、
       按钮会被裁掉一截 —— 本窗口没有最小尺寸保护（见下方 footer 注释）。 */
    g_viewH = h - S(60);
    if (g_viewH < S(120)) g_viewH = S(120);

    /* 滚动量以滚动条为准；窗口变矮后原位置可能越界，先钳位写回 */
    int off = ScrollPos();
    if (off > MaxScroll()) {
        off = MaxScroll();
        SCROLLINFO si;
        memset(&si, 0, sizeof(si));
        si.cbSize = sizeof(si);
        si.fMask  = SIF_POS;
        si.nPos   = off;
        SetScrollInfo(g_hwnd, SB_VERT, &si, TRUE);
    }

    for (int i = 0; i < FIELD_N; i++) {
        int id = kFields[i].id;
        HWND c = GetDlgItem(g_hwnd, id);
        if (!c) continue;
        if (kFields[i].isBtn) {
            MoveWindow(c, x, y - off, w - x * 2, S(24), TRUE);
            y += rowBool;
        } else if (kFields[i].isBool) {
            MoveWindow(c, x, y - off, w - x * 2, rowBool - S(2), TRUE);
            y += rowBool;
        } else if (kFields[i].isText) {
            /* 文本行：**标签独占一行、输入框占满下一行**。
               照数字行那样"框在左、标签吃剩下的"是行不通的 —— 路径要看得全，
               框必须撑满；而标签是一整句，塞不进一行的剩余宽度。 */
            HWND lab = GetDlgItem(g_hwnd, id + 500);
            if (lab) MoveWindow(lab, x, y - off, w - x * 2, S(18), TRUE);
            y += S(20);
            MoveWindow(c, x, y - off, w - x * 2, S(22), TRUE);
            y += S(24);
        } else {
            /* 数字框放**左边**、单位紧跟在框右侧、标签吃剩下的空间：
               标签是长句，把输入框放在标签之后的话，窗口一窄它就被挤出
               客户区 —— 用户看到"有说明、没地方填"。输入框与单位宽固定。 */
            HWND lab  = GetDlgItem(g_hwnd, id + 500);
            HWND unit = GetDlgItem(g_hwnd, id + 600);
            int ux = x + editW + S(4), unitW = S(52);
            MoveWindow(c, x, y - off, editW, S(22), TRUE);
            if (unit)
                MoveWindow(unit, ux, y + S(4) - off, unitW, S(18), TRUE);
            if (lab)
                MoveWindow(lab, ux + unitW + S(8), y + S(4) - off,
                           w - (ux + unitW + S(8)) - x, S(18), TRUE);
            y += rowInt;
        }
        y += gap;
    }

    /* 名单高度固定（约五行，再多名单自己有滚动条），三组顺序往下排 */
    y += S(6);
    int listH = S(84);
    /* 名单列表框**定宽**，不跟窗口拉伸：进程名撑死几十个字符，宽列表框
       右边全是用不上的白（328 ≈ 底部四个按钮并排 376 再砍 13%）；真有超长名字时
       列表框自带横向滚动条兜底 */
    int listW = w - x * 2;
    if (listW > S(328)) listW = S(328);
    for (int i = 0; i < LIST_N; i++) {
        int base = IDC_LIST0 + i * LIST_STRIDE;
        MoveWindow(GetDlgItem(g_hwnd, base + 4), x, y - off, w - x * 2, S(18), TRUE);  /* 标题 */
        y += S(20);
        MoveWindow(GetDlgItem(g_hwnd, base), x, y - off, listW, listH, TRUE);
        y += listH + S(4);
        MoveWindow(GetDlgItem(g_hwnd, base + 1), x, y - off, S(220), S(24), TRUE);     /* 名字输入 */
        MoveWindow(GetDlgItem(g_hwnd, base + 2), x + S(228), y - off, S(70), S(24), TRUE);
        MoveWindow(GetDlgItem(g_hwnd, base + 3), x + S(304), y - off, S(70), S(24), TRUE);
        MoveWindow(GetDlgItem(g_hwnd, base + 5), x + S(380), y - off, S(88), S(24), TRUE);  /* 选取窗口 */
        y += S(28) + S(8);
    }
    g_contentH = y + S(6);

    /* 底部固定区：一块实底"窗框"从可视内容区一直盖到客户区底，状态栏与
       按钮是它的子控件 —— 滚动中溢出的内容滑到它底下就被挡住，不会从
       按钮周围的缝隙里露出来。窗口高度正常时窗框高恒为 S(60)，内部坐标
       因此是常量；**矮窗口下会被内容区下限挤薄**（见 Layout 开头）。 */
    if (g_footer) {
        MoveWindow(g_footer, 0, g_viewH, w, h - g_viewH, TRUE);
        /* 窗框的职责就是压住表单内容：每次布局都把 z 序重新钉到顶。
           （子窗口创建/移动后谁在上谁在下并不可靠，实测本机上"最后建的
           反而在底下"，所以这里不赌，显式钉。） */
        SetWindowPos(g_footer, HWND_TOP, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    /* 状态栏独占一行（按钮在它下面那行），给足整个客户区宽：
       窄窗口下提示文案也能完整显示，不用给按钮预留右侧空间 */
    if (g_status) MoveWindow(g_status, x, S(4), w - x * 2, S(18), TRUE);
    int bw = S(88), bg = S(8), fby = S(26);
    /* 右对齐排列，左->右：重新加载、保存、保存并重启、退出 */
    MoveWindow(GetDlgItem(g_footer, IDC_RELOAD),   w - x - bw * 4 - bg * 3, fby, bw, S(26), TRUE);
    MoveWindow(GetDlgItem(g_footer, IDC_SAVEONLY), w - x - bw * 3 - bg * 2, fby, bw, S(26), TRUE);
    MoveWindow(GetDlgItem(g_footer, IDC_SAVE),     w - x - bw * 2 - bg,     fby, bw, S(26), TRUE);
    MoveWindow(GetDlgItem(g_footer, IDC_EXIT),     w - x - bw,              fby, bw, S(26), TRUE);

    UpdateScrollBar(off);
    g_laidScroll = off;
}

/* 滚到指定位置（钳位）并按新位置全量重排。滚动条与子控件位置的同步
   只有这一条路径，WM_VSCROLL / 滚轮都汇聚到这里。 */
static void SetScroll(int pos) {
    if (pos < 0) pos = 0;
    if (pos > MaxScroll()) pos = MaxScroll();
    SCROLLINFO si;
    memset(&si, 0, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask  = SIF_POS;
    si.nPos   = pos;
    SetScrollInfo(g_hwnd, SB_VERT, &si, TRUE);
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    Layout(rc.right, rc.bottom);
}

/* ---------------- 按钮的自绘 ----------------
   默认的 Win32 按钮在 Win10/11 上是一块浅灰浮凸，一排按钮摆在一起既看不出
   主次、也看不出鼠标指没指着。这里在既有子类里把按钮的 WM_PAINT 整个接管：
     · "保存并重启"是主操作 -> 强调蓝填充，悬停变亮、按下变深（Fluent 主按钮）；
     · 其余按钮 -> 白底描边，悬停浅蓝底 + 蓝边（Fluent 次级按钮）；
     · 圆角、焦点虚线框、禁用态、空格键按下（BM_SETSTATE）一并画出。
   状态（悬停/按下/主次）存**窗口属性**：GWLP_USERDATA 已被原过程占用。
   点击行为不动 —— 按下/抬起仍交给按钮默认过程，BN_CLICKED 照常发给父窗口，
   所以跨进程 BM_CLICK、Tab 焦点、键盘触发全都和原来一样。 */

#define BTNPROP_PRIMARY L"capPrimary"
#define BTNPROP_HOVER   L"capHover"
#define BTNPROP_PRESSED L"capPressed"

static BOOL BtnFlag(HWND h, const WCHAR* prop) {
    return (BOOL)(INT_PTR)GetPropW(h, prop);
}
static void SetBtnFlag(HWND h, const WCHAR* prop, BOOL v) {
    if (BtnFlag(h, prop) != v) {
        SetPropW(h, prop, (HANDLE)(INT_PTR)(v ? 1 : 0));
        InvalidateRect(h, NULL, TRUE);
        UpdateWindow(h);          /* 悬停/按下要"即时"反馈，等下次 PAINT 太钝 */
    }
}

static void TrackBtnHover(HWND h) {
    TRACKMOUSEEVENT t;
    memset(&t, 0, sizeof(t));
    t.cbSize = sizeof(t);
    t.dwFlags = TME_LEAVE;
    t.hwndTrack = h;
    TrackMouseEvent(&t);
}

static void PaintButton(HWND h, HDC dc) {
    RECT rc;
    GetClientRect(h, &rc);
    BOOL primary = BtnFlag(h, BTNPROP_PRIMARY);
    BOOL hover   = BtnFlag(h, BTNPROP_HOVER);
    BOOL pressed = BtnFlag(h, BTNPROP_PRESSED);
    BOOL enabled = IsWindowEnabled(h);

    /* 先铺父窗口的底色：圆角露出的四个角才像是"透明"的 */
    {
        HBRUSH bg = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
    }

    COLORREF fill, border, text;
    if (!enabled) {
        fill = RGB(246, 246, 246); border = RGB(189, 189, 189); text = RGB(160, 160, 160);
    } else if (primary) {
        fill   = pressed ? RGB(0, 90, 158)   : hover ? RGB(26, 134, 217) : RGB(0, 120, 215);
        border = fill;
        text   = RGB(255, 255, 255);
    } else {
        fill   = pressed ? RGB(204, 228, 247) : hover ? RGB(229, 241, 251) : RGB(252, 252, 252);
        border = pressed ? RGB(0, 84, 153)    : hover ? RGB(0, 120, 215)   : RGB(173, 173, 173);
        text   = RGB(0, 0, 0);
    }

    int rad = S(4);
    HPEN pen = CreatePen(PS_SOLID, 1, border);   /* 画笔没有 W/A 之分 */
    HBRUSH br = CreateSolidBrush(fill);
    HGDIOBJ open = SelectObject(dc, pen);
    HGDIOBJ obr = SelectObject(dc, br);
    /* 右/下各收 1px：RoundRect 的边画在包围矩形**边上**，不收会被客户区裁掉 */
    RoundRect(dc, rc.left, rc.top, rc.right - 1, rc.bottom - 1, rad * 2, rad * 2);
    SelectObject(dc, open);
    SelectObject(dc, obr);
    DeleteObject(pen);
    DeleteObject(br);

    WCHAR txt[96];
    GetWindowTextW(h, txt, 96);
    HFONT f = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
    if (!f) f = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HGDIOBJ of = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, text);
    RECT tr = rc;
    if (pressed && !primary) OffsetRect(&tr, 0, 1);   /* 次级按钮按下时文字下沉一点 */
    DrawTextW(dc, txt, -1, &tr,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, of);

    if (GetFocus() == h) {                 /* 键盘焦点：虚线框（对话框惯例） */
        RECT frc = rc;
        InflateRect(&frc, -S(3), -S(3));
        DrawFocusRect(dc, &frc);
    }
}

/* 只自绘**普通下压按钮**。类名判断远远不够 —— 勾选框（BS_AUTOCHECKBOX）、
   单选钮、分组框全是 "Button" 类，按类名放行会把它们也画成下压按钮
   （实测：勾选框整个变成一长条按钮，复选框本身消失了）。
   只认 BS_PUSHBUTTON / BS_DEFPUSHBUTTON 两种式样。 */
static BOOL BtnIsPush(HWND h) {
    WCHAR cls[16];
    if (!(GetClassNameW(h, cls, 16) > 0 && _wcsicmp(cls, L"Button") == 0))
        return FALSE;
    DWORD st = (DWORD)GetWindowLongPtrW(h, GWL_STYLE) & BS_TYPEMASK;
    return st == BS_PUSHBUTTON || st == BS_DEFPUSHBUTTON;
}

/* 表单子控件的子类，干三件事：
   1. 接 Ctrl+S / Ctrl+回车 —— 焦点多半在某个子控件上，主窗口收不到按键；
   2. 把滚轮转给主窗口 —— 滚轮默认落在**焦点**控件头上，不转的话页面永远
      滚不动（列表框也转：名单很短，滚轮滚页面比滚列表更符合直觉）；
   3. 按钮全部自绘（见上"按钮的自绘"）。
   原过程存 GWLP_USERDATA：EDIT/BUTTON/LISTBOX 各有各的默认过程，
   不能共用一个全局变量。 */
static LRESULT CALLBACK ChildProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN && (w == 'S' || w == VK_RETURN) &&
        (GetKeyState(VK_CONTROL) & 0x8000)) {
        DoSave();
        return 0;
    }
    if (m == WM_MOUSEWHEEL) {
        SendMessageW(g_hwnd, m, w, l);
        return 0;
    }
    if (BtnIsPush(h)) {
        switch (m) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            if (dc) PaintButton(h, dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;                    /* 整块区域自画，先擦一遍只会闪 */
        case WM_MOUSEMOVE:
            if (IsWindowEnabled(h) && !BtnFlag(h, BTNPROP_HOVER)) {
                SetBtnFlag(h, BTNPROP_HOVER, TRUE);
                TrackBtnHover(h);        /* 只在第一次进入时注册 LEAVE */
            }
            break;
        case WM_MOUSELEAVE:
            SetBtnFlag(h, BTNPROP_HOVER, FALSE);
            break;
        case WM_LBUTTONDOWN:
            SetBtnFlag(h, BTNPROP_PRESSED, TRUE);
            break;
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            SetBtnFlag(h, BTNPROP_PRESSED, FALSE);
            break;
        case BM_SETSTATE:                /* 键盘空格触发：默认过程不发消息只置状态 */
            SetBtnFlag(h, BTNPROP_PRESSED, w != 0);
            break;
        case WM_ENABLE:                  /* 必须落到默认过程：lParam 是新状态 */
        case WM_SETFONT:                 /* wParam 是新字体，WM_GETFONT 要取得到 */
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            InvalidateRect(h, NULL, TRUE);
            break;
        default:
            break;
        }
    }
    return CallWindowProcW((WNDPROC)GetWindowLongPtrW(h, GWLP_USERDATA), h, m, w, l);
}
/* 这个窗口是不是某个名单列表框（滚轮判定用） */
static int ListIndexFromHwnd(HWND h) {
    if (!h) return -1;
    for (int i = 0; i < LIST_N; i++)
        if (h == GetDlgItem(g_hwnd, IDC_LIST0 + i * LIST_STRIDE)) return i;
    return -1;
}

/* 用滚轮滚列表框。
   为什么自己发 WM_VSCROLL 而不是把 WM_MOUSEWHEEL 原样转给列表框：所有子控件都
   被子类过程 ChildProc 接管，而它会把滚轮**转回主窗口**（那是以前"滚轮只滚整个
   表单"的做法）—— 原样转发就会转成死循环。
   步长跟随系统设置（SPI_GETWHEELSCROLLLINES）；用户把"每次滚多少行"设成 0
   表示不滚动，那就真的不滚。 */
static void ScrollListBy(HWND lb, int delta) {
    int notches = delta / WHEEL_DELTA;
    if (!notches) return;
    DWORD lines = 3;
    if (!SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0)) lines = 3;
    const BOOL page = (lines == WHEEL_PAGESCROLL);
    if (!page && lines == 0) return;                 /* 系统设置：不使用滚轮滚动 */
    int n = (int)(page ? 1 : lines) * (notches < 0 ? -notches : notches);
    const WPARAM cmd = (WPARAM)(page ? (notches > 0 ? SB_PAGEUP : SB_PAGEDOWN)
                                     : (notches > 0 ? SB_LINEUP : SB_LINEDOWN));
    for (int k = 0; k < n; k++) SendMessageW(lb, WM_VSCROLL, cmd, 0);
}

static void HookChild(HWND h) {
    if (h) SetWindowLongPtrW(h, GWLP_USERDATA,
                             SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)ChildProc));
}

/* 把一条进程名加进第 i 组名单：去首尾空白、去 .exe 后缀、去重。
   返回是否真的加了（重名/空名不算）。名单里存的是"不带后缀"的形态
   （匹配时两边都去），界面显示的样子就该是**真正存进去的样子**。 */
static BOOL AddListEntry(int i, const WCHAR* raw) {
    int base = IDC_LIST0 + i * LIST_STRIDE;
    HWND lb = GetDlgItem(g_hwnd, base);
    WCHAR trim[CFG_NAME_MAX];
    _snwprintf_s(trim, CFG_NAME_MAX, _TRUNCATE, L"%s", raw);

    /* 去首尾空白：界面里粘进来的名字常带空格，而名单匹配是**完全匹配** */
    size_t l = wcslen(trim);
    while (l > 0 && (trim[l - 1] == L' ' || trim[l - 1] == L'\t')) trim[--l] = 0;
    const WCHAR* lead = trim;
    while (*lead == L' ' || *lead == L'\t') lead++;

    WCHAR buf[CFG_NAME_MAX];
    _snwprintf_s(buf, CFG_NAME_MAX, _TRUNCATE, L"%s", lead);
    l = wcslen(buf);
    if (l >= 4 && _wcsicmp(buf + l - 4, L".exe") == 0) buf[l - 4] = 0;
    if (!buf[0]) return FALSE;

    /* 去重：同样的名字出现两次没有意义（完全匹配，重复项纯噪音） */
    if (SendMessageW(lb, LB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)buf) != LB_ERR)
        return FALSE;
    SendMessageW(lb, LB_ADDSTRING, 0, (LPARAM)buf);
    return TRUE;
}

static void AddName(int i) {
    int base = IDC_LIST0 + i * LIST_STRIDE;
    HWND ed = GetDlgItem(g_hwnd, base + 1);
    WCHAR raw[CFG_NAME_MAX] = L"";
    GetWindowTextW(ed, raw, CFG_NAME_MAX);
    if (AddListEntry(i, raw)) MarkDirty();
    SetWindowTextW(ed, L"");
    SetFocus(ed);
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

/* ---------------- 选窗添加 ----------------
   Spy++ / AutoIt "Finder Tool" 的标准做法（微软 KB Q143045）：点"选窗添加"
   后 SetCapture 把鼠标输入全收归本窗口 —— 目标窗口收不到这次点击、也不会被
   切到前台；移动时 WindowFromPoint + GetAncestor(GA_ROOT) 找光标下的**顶层**
   窗口（名单匹配的是进程，子控件没有自己的进程），用 PatBlt(DSTINVERT) 在
   屏幕上给它描一圈反色边（反色再画一遍 = 擦除，不需要保存位图）；单击就把
   该窗口的进程名（OpenProcess + QueryFullProcessImageNameW）加进名单。
   Esc 或右键取消。 */

static int  g_pickList = -1;      /* 正在为哪份名单选窗（-1 = 没在选窗） */
static HWND g_pickHwnd = NULL;    /* 当前高亮的顶层窗口 */

#ifndef PROCESS_QUERY_LIMITED_INFORMATION
#define PROCESS_QUERY_LIMITED_INFORMATION 0x1000
#endif
typedef BOOL (WINAPI* PFN_QueryFullProcessImageNameW)(HANDLE, DWORD, LPWSTR, PDWORD);

/* 选中窗口的进程名（纯文件名，不含路径）。失败多半是权限不够。 */
static BOOL PickProcName(HWND hwnd, WCHAR* out, int cch) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return FALSE;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return FALSE;
    WCHAR path[1024];
    DWORD sz = 1024;
    BOOL ok = FALSE;
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    PFN_QueryFullProcessImageNameW q =
        k32 ? (PFN_QueryFullProcessImageNameW)GetProcAddress(k32,
                                                       "QueryFullProcessImageNameW")
            : NULL;
    if (q && q(h, 0, path, &sz)) {
        const WCHAR* f = wcsrchr(path, L'\\');
        _snwprintf_s(out, cch, _TRUNCATE, L"%s", f ? f + 1 : path);
        ok = TRUE;
    }
    CloseHandle(h);
    return ok;
}

/* 在屏幕上把窗口描一圈反色边；对同一个窗口调两次正好抵消。 */
static void PickFrame(HWND hwnd) {
    RECT r;
    if (!hwnd || !GetWindowRect(hwnd, &r)) return;
    HDC dc = GetDC(NULL);
    if (!dc) return;
    int th = S(3), ww = r.right - r.left, wh = r.bottom - r.top;
    PatBlt(dc, r.left, r.top, ww, th, DSTINVERT);
    PatBlt(dc, r.left, r.bottom - th, ww, th, DSTINVERT);
    PatBlt(dc, r.left, r.top, th, wh, DSTINVERT);
    PatBlt(dc, r.right - th, r.top, th, wh, DSTINVERT);
    ReleaseDC(NULL, dc);
}

static HWND PickHit(void) {
    POINT pt;
    if (!GetCursorPos(&pt)) return NULL;
    HWND hit = WindowFromPoint(pt);
    return hit ? GetAncestor(hit, GA_ROOT) : NULL;
}

/* 跟随光标更新高亮：目标变了就"旧框再画一遍（=擦）+ 新框画一遍"。
   设置窗口自己不算数 —— 把自己加进名单没有意义，悬停它时不描框。 */
static void PickTrack(void) {
    HWND root = PickHit();
    if (root == g_hwnd) root = NULL;
    if (root == g_pickHwnd) return;
    PickFrame(g_pickHwnd);
    g_pickHwnd = root;
    PickFrame(g_pickHwnd);
}

/* 退出选窗态：擦掉高亮框、还鼠标、释放捕获。 */
static void PickEnd(void) {
    if (g_pickList < 0) return;
    PickFrame(g_pickHwnd);
    g_pickHwnd = NULL;
    g_pickList = -1;
    ReleaseCapture();
    SetCursor(LoadCursorW(NULL, (LPCWSTR)(ULONG_PTR)IDC_ARROW));
    SetStatus(L"");
}

static void PickBegin(int i) {
    PickEnd();                       /* 上一次没结束就又点了一次：先收尾 */
    g_pickList = i;
    SetCapture(g_hwnd);
    SetCursor(LoadCursorW(NULL, (LPCWSTR)(ULONG_PTR)IDC_CROSS));
    SetStatus(kLists[i].pickKind == PICK_CLASS
                  ? L"选窗中：点哪个窗口就取它的窗口类填进输入框（Esc/右键取消）"
                  : L"选窗中：点哪个窗口就取它的进程名填进输入框（Esc/右键取消）");
    PickTrack();
}

/* 单击落定：取光标下窗口的进程名加进名单。必须在**抬起**时收尾 ——
   若在按下时释放捕获，配对的抬起会漏给目标窗口，等于替用户在那里面点了一下。 */
static void PickFinish(void) {
    int i = g_pickList;
    if (i < 0) return;
    HWND root = PickHit();
    if (root == g_hwnd) {
        /* 点到设置窗口自己：不算数，也**不退出选窗**，提示用户去点别的窗口 */
        SetStatus(L"这是设置窗口自己，不算 —— 请点击其它程序的窗口（Esc 取消）");
        return;
    }
    PickEnd();
    if (!root) { SetStatus(L"选窗失败：这里没有可取的窗口"); return; }

    /* 取什么由名单决定：进程名单取进程名，窗口类名单取类名（原样，无前缀）。 */
    WCHAR name[CFG_NAME_MAX] = L"";
    if (kLists[i].pickKind == PICK_CLASS) {
        WCHAR cls[128] = L"";
        if (!GetClassNameW(root, cls, 128) || !cls[0]) {
            SetStatus(L"选窗失败：拿不到该窗口的类名");
            return;
        }
        _snwprintf_s(name, CFG_NAME_MAX, _TRUNCATE, L"%s", cls);
    } else if (!PickProcName(root, name, CFG_NAME_MAX)) {
        SetStatus(L"选窗失败：拿不到该窗口的进程名（权限不足？）");
        return;
    }

    /* ★ 只**填进输入框**，不直接加进名单 —— 由用户确认后点"添加"。
       这样能先看清取到的是什么、也能顺手改一改（比如只留进程名的主体）。 */
    {
        HWND ed = GetDlgItem(g_hwnd, IDC_LIST0 + i * LIST_STRIDE + 1);
        SetWindowTextW(ed, name);
        SetFocus(ed);
        SendMessageW(ed, EM_SETSEL, 0, -1);     /* 全选：直接打字就能覆盖 */
        SetStatus(L"已取到 %s —— 确认后点「添加」才会加进名单", name);
    }
}

/* 底部"窗框"：一块不透明的实底子窗口，状态栏与按钮放在它上面。
   为什么要它：表单内容滚动时，滑出可视区的部分不会被自动裁掉，会从
   状态栏/按钮周围的缝隙里露出来；窗框在 z 序上压住它们，就挡住了。
   带 WS_EX_CONTROLPARENT，Tab 焦点能进到它里面的按钮；
   子控件的通知（按钮点击）原样转给主窗口，处理逻辑不分家。 */
static LRESULT CALLBACK FooterProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_COMMAND) {
        SendMessageW(g_hwnd, m, w, l);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* ---- 快速粘贴字体：按钮上显示"字体名 字号" ----
   学 CLaunch-search（settings_general.go 的 fontDesc）的做法：一个入口，
   按钮文字就是当前值 —— 用户不需要在两个框里对"现在选的是啥"。
   字体名留空 = 用浮层的内置默认，显示时把默认名带出来，不然按钮是空的。 */
static void UpdateClipFontBtn(void) {
    HWND b = GetDlgItem(g_hwnd, IDC_CLIP_PICK);
    if (!b) return;
    const WCHAR* name = ConfigGetClipFont();
    WCHAR txt[CLIP_FONT_MAX + 16];
    _snwprintf_s(txt, (size_t)(CLIP_FONT_MAX + 16), _TRUNCATE, L"%s　%d 磅",
                 (name[0] ? name : L"Microsoft YaHei UI"), ConfigGetClipFontSize());
    SetWindowTextW(b, txt);
}

/* ---------- 系统字体对话框（ChooseFont） ----------
   调用方式取自本机的 CLaunch-search 项目（settings_font.go，同作者），那里踩过
   两个坑，这里一并躲开：
     ① 字体名留空时直接弹，家族预选会是空白 —— 先兜底成实际渲染用的默认字体。
     ② 对话框按「字面名 + 字重」**整体**匹配；确认后写回的 lfFaceName 可能是带
        字重后缀的 GDI 字面名（如选 MiSans 半细体 → 写回 "MiSans Medium"，字重
        380）。第二次打开时恒传 FW_NORMAL(400) 会因字重对不上而家族预选空白。
        所以用 EnumFontFamiliesEx 取该名字的**真实字重**再喂回去。 */

struct FontFaceItem {
    WCHAR name[LF_FACESIZE];
    int   weight;
};
static FontFaceItem g_fontFaces[64];
static int          g_fontFaceN = 0;

static int CALLBACK EnumFaceProc(const LOGFONTW* lf, const TEXTMETRICW* tm,
                                 DWORD type, LPARAM lp) {
    (void)tm; (void)type; (void)lp;
    if (g_fontFaceN < (int)(sizeof(g_fontFaces) / sizeof(g_fontFaces[0]))) {
        wcsncpy_s(g_fontFaces[g_fontFaceN].name, LF_FACESIZE, lf->lfFaceName, _TRUNCATE);
        g_fontFaces[g_fontFaceN].weight = lf->lfWeight;
        g_fontFaceN++;
    }
    return 1;   /* 继续枚举 */
}

/* 取字体名的真实字重；枚举为空 = 字体未安装，返回 0（调用方兜底 FW_NORMAL）。
   家族名与粗体字面同名的场景下，取字重最接近 400 的那个（即常规体）。 */
static int FontFaceWeight(const WCHAR* name) {
    if (!name || !name[0]) return 0;
    LOGFONTW flt;
    memset(&flt, 0, sizeof(flt));
    wcsncpy_s(flt.lfFaceName, LF_FACESIZE, name, _TRUNCATE);
    flt.lfCharSet = DEFAULT_CHARSET;
    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) return 0;
    g_fontFaceN = 0;
    EnumFontFamiliesExW(dc, &flt, EnumFaceProc, 0, 0);
    DeleteDC(dc);
    if (g_fontFaceN <= 0) return 0;
    int best = 0, bestDist = 1 << 30;
    for (int i = 0; i < g_fontFaceN; i++) {
        if (_wcsicmp(g_fontFaces[i].name, name) != 0) continue;
        int dd = g_fontFaces[i].weight - 400;
        if (dd < 0) dd = -dd;
        if (dd < bestDist) { best = g_fontFaces[i].weight; bestDist = dd; }
    }
    return best ? best : g_fontFaces[0].weight;
}

/* 弹字体对话框。name / sizePt 既当初始值也当出参；用户确认返回 TRUE。 */
static BOOL PickFont(HWND owner, WCHAR* name, int cap, int* sizePt) {
    if (!name[0]) wcscpy_s(name, (size_t)cap, L"Microsoft YaHei UI");
    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    wcsncpy_s(lf.lfFaceName, LF_FACESIZE, name, _TRUNCATE);
    lf.lfHeight = -MulDiv(*sizePt, (g_dpi > 0 ? g_dpi : 96), 72);
    lf.lfWeight = FontFaceWeight(name);
    if (!lf.lfWeight) lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;

    CHOOSEFONTW cf;
    memset(&cf, 0, sizeof(cf));
    cf.lStructSize = sizeof(cf);
    cf.hwndOwner   = owner;
    cf.lpLogFont   = &lf;
    cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_FORCEFONTEXIST | CF_NOSCRIPTSEL;
    cf.nSizeMin = 6;
    cf.nSizeMax = CLIP_FONT_SIZE_MAX;
    if (!ChooseFontW(&cf)) return FALSE;

    wcsncpy_s(name, (size_t)cap, lf.lfFaceName, _TRUNCATE);
    int pt = cf.iPointSize / 10;
    if (pt <= 0) pt = *sizePt;
    *sizePt = pt;
    return TRUE;
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_COMMAND: {
        int id = LOWORD(w);
        if (id == IDC_SAVEONLY) {
            /* 只保存不重启：改完几个数值想攒着一起生效，或者本体压根没在跑
               （下次启动自然读到新配置）时用这个。 */
            SaveConfig();
            return 0;
        }
        if (id == IDC_SAVE)   { DoSave(); return 0; }
        if (id == IDC_CLIP_PICK) {
            WCHAR name[CLIP_FONT_MAX] = L"";
            int size = ConfigGetClipFontSize();
            const WCHAR* cur = ConfigGetClipFont();
            if (cur[0]) wcscpy_s(name, CLIP_FONT_MAX, cur);
            if (PickFont(h, name, CLIP_FONT_MAX, &size)) {
                /* 确认即写入配置层（内存态）：ConfigSaveFile 写回时用的就是它，
                   所以不需要"隐藏字段"来替按钮保管值。 */
                ConfigSetClipFont(name);
                ConfigSetClipFontSize(size);
                UpdateClipFontBtn();
                MarkDirty();
            }
            return 0;
        }
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
            if (sub == 5) { PickBegin(i); return 0; }
            if (sub == 0 && HIWORD(w) == LBN_DBLCLK) { DelName(i); return 0; }
            return 0;
        }
        /* 勾选框 / 输入框：任何改动都算"有未保存的改动" */
        if (HIWORD(w) == BN_CLICKED || HIWORD(w) == EN_CHANGE) MarkDirty();
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_pickList >= 0) {          /* 捕获生效期间所有移动都归我们 */
            PickTrack();
            SetCursor(LoadCursorW(NULL, (LPCWSTR)(ULONG_PTR)IDC_CROSS));
            return 0;
        }
        break;
    case WM_LBUTTONDOWN:
        if (g_pickList >= 0) return 0;  /* 按下不做事，等抬起（见 PickFinish） */
        break;
    case WM_LBUTTONUP:
        if (g_pickList >= 0) { PickFinish(); return 0; }
        break;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
        if (g_pickList >= 0) { PickEnd(); return 0; }   /* 右键/中键 = 取消 */
        break;
    case WM_CAPTURECHANGED:
        /* 捕获被别人抢走（正常流程不会走到）：也得收掉选窗态，别让高亮框留在屏上。
           自己 PickEnd 里的 ReleaseCapture 也会发这条，那时 g_pickList 已复位，自然跳过。 */
        if (g_pickList >= 0 && (HWND)l != g_hwnd) {
            PickFrame(g_pickHwnd);
            g_pickHwnd = NULL;
            g_pickList = -1;
            SetStatus(L"选窗已取消");
        }
        return 0;
    case WM_PAINT:
        /* 滚动条可能被外部（读屏 / UIA 的 Scroll 模式）直接拨动而不经过
           WM_VSCROLL：画之前对一下账，位置和上次落位不一致就整体重排（自愈） */
        if (g_hwnd && ScrollPos() != g_laidScroll) {
            RECT rc;
            GetClientRect(g_hwnd, &rc);
            Layout(rc.right, rc.bottom);
        }
        ValidateRect(h, NULL);
        return DefWindowProcW(h, m, w, l);
    case WM_VSCROLL: {
        int cmd = LOWORD(w), pos = ScrollPos();
        switch (cmd) {
        case SB_LINEUP:          pos -= S(40); break;
        case SB_LINEDOWN:        pos += S(40); break;
        case SB_PAGEUP:          pos -= g_viewH - S(40); break;
        case SB_PAGEDOWN:        pos += g_viewH - S(40); break;
        case SB_THUMBPOSITION:
        case SB_THUMBTRACK:      pos = (int)HIWORD(w); break;
        default:                 return 0;    /* SB_ENDSCROLL 等 */
        }
        SetScroll(pos);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        /* 子控件（见 ChildProc）把滚轮转到这里。规则：
             ① 选窗态下滚轮无意义 -> 吞掉；
             ② 光标停在某个**名单列表框**上 -> 滚那个列表框（以前滚轮只滚整个表单，
                列表框只能拖滚动条）；
             ③ 否则滚整个表单。 */
        if (g_pickList >= 0) return 0;
        POINT pt;
        pt.x = (LONG)(short)LOWORD(l);
        pt.y = (LONG)(short)HIWORD(l);
        HWND over = WindowFromPoint(pt);
        if (ListIndexFromHwnd(over) >= 0) {
            ScrollListBy(over, (int)(short)HIWORD(w));
            return 0;
        }
        SetScroll(ScrollPos() - ((short)HIWORD(w)) / WHEEL_DELTA * S(48));
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
    /* 选窗态下"不归我们管"的消息（g_pickList < 0）从上面的 break 落到这里 */
    return DefWindowProcW(h, m, w, l);
}

static HWND MakeBtn(HINSTANCE hi, HWND parent, const WCHAR* text, int id) {
    HWND b = CreateWindowExW(0, L"BUTTON", text,
                             WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_PUSHBUTTON | WS_TABSTOP,
                             0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, hi, NULL);
    /* "保存并重启"是主操作 -> 自绘时上强调蓝；要在第一次绘制前设好 */
    if (b && id == IDC_SAVE)
        SetPropW(b, BTNPROP_PRIMARY, (HANDLE)(INT_PTR)1);
    HookChild(b);        /* 按钮也转发滚轮 / 接 Ctrl+S（焦点常停在按钮上） */
    return b;
}
static HWND MakeLabel(HINSTANCE hi, HWND parent, const WCHAR* text, int id) {
    return CreateWindowExW(0, L"STATIC", text,
                           WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | SS_LEFTNOWORDWRAP,
                           0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, hi, NULL);
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

    /* 底部"窗框"类：实底 + 无边框，见 FooterProc 处的说明 */
    WNDCLASSW fc = wc;
    fc.lpfnWndProc = FooterProc;
    fc.lpszClassName = FOOTER_CLASS;
    if (!RegisterClassW(&fc)) return 1;

    /* DPI 要在**建窗之前**知道：窗口尺寸本身就得按它算（否则 200% 屏上
       窗口只有一半大）。QueryDpi 在没有 hwnd 时退回查屏幕 DC。 */
    g_dpi = (int)QueryDpi();
    if (g_dpi < 96) g_dpi = 96;

    /* 初始大小按 DPI 缩放，并**钳到工作区以内**：高 DPI 笔记本（150% + 768 高）
       上按 96 DPI 算出来的 780 像素窗口会超出屏幕底部，底部的按钮与状态栏
       就永远看不见了。 */
    /* 宽度贴着内容走：最宽的是名单区标题句（约 490）和"输入框 + 单位 +
       说明标签"一行（约 500），再留滚动条与边框的余量 —— 再宽的话
       名单框右侧就是一大块用不上的空白 */
    int winW = S(560), winH = S(780);
    int winX = CW_USEDEFAULT, winY = CW_USEDEFAULT;
    {
        RECT wa;
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
            int aw = wa.right - wa.left - S(40), ah = wa.bottom - wa.top - S(60);
            if (aw > S(480) && winW > aw) winW = aw;
            if (ah > S(360) && winH > ah) winH = ah;
            /* **工作区**居中（不含任务栏）：CW_USEDEFAULT 是级联摆放，
               窗口会落在屏幕左上角 —— 这么大一张表单贴着边放很难受。
               钳位防的是"窗口比工作区还大"时算出负坐标。 */
            winX = wa.left + ((wa.right - wa.left) - winW) / 2;
            winY = wa.top + ((wa.bottom - wa.top) - winH) / 2;
            if (winX < wa.left) winX = wa.left;
            if (winY < wa.top) winY = wa.top;
        }
    }

    g_hwnd = CreateWindowExW(0, WND_CLASS, APP_TITLE,
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_VSCROLL,
                             winX, winY, winW, winH,
                             NULL, NULL, hInst, NULL);
    if (!g_hwnd) return 1;

    /* 窗口已存在：再问一次 Per-Monitor 的精确值（这块屏可能是 144 而主屏是 96） */
    g_dpi = (int)QueryDpi();
    if (g_dpi < 96) g_dpi = 96;
    MakeFont();

    for (int i = 0; i < FIELD_N; i++) {
        int id = kFields[i].id;
        if (kFields[i].isBtn) {
            /* 按钮自己就是一行：它不承载配置值，点击行为在 WM_COMMAND 里按 id 分支 */
            HWND b = CreateWindowExW(0, L"BUTTON", kFields[i].label,
                                     WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP,
                                     0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)id, hInst, NULL);
            HookChild(b);
        } else if (kFields[i].isBool) {
            HWND b = CreateWindowExW(0, L"BUTTON", kFields[i].label,
                                     WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | BS_AUTOCHECKBOX | WS_TABSTOP,
                                     0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)id, hInst, NULL);
            HookChild(b);
        } else {
            MakeLabel(hInst, g_hwnd, kFields[i].label, id + 500);
            if (kFields[i].unit)
                MakeLabel(hInst, g_hwnd, kFields[i].unit, id + 600);  /* 输入框右侧的单位 */
            HWND e = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                     WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | ES_AUTOHSCROLL | WS_TABSTOP,
                                     0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)id, hInst, NULL);
            /* 数字框：限 8 位（最大值 60000 也远用不完），防止粘进一大段文本。
               文本框按配置项自己的上限来限，不是 8 —— 路径比数字长得多。 */
            SendMessageW(e, EM_LIMITTEXT,
                         kFields[i].isText ? (DITTO_DIR_MAX - 1) : 8, 0);
            HookChild(e);
        }
    }

    for (int i = 0; i < LIST_N; i++) {
        int base = IDC_LIST0 + i * LIST_STRIDE;
        MakeLabel(hInst, g_hwnd, kLists[i].label, base + 4);
        HookChild(CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                  WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS |
                                  WS_VSCROLL | WS_HSCROLL | WS_TABSTOP |
                                  LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                                  0, 0, 10, 10, g_hwnd,
                                  (HMENU)(INT_PTR)base, hInst, NULL));
        HWND ed = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                  WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | ES_AUTOHSCROLL | WS_TABSTOP,
                                  0, 0, 10, 10, g_hwnd,
                                  (HMENU)(INT_PTR)(base + 1), hInst, NULL);
        SendMessageW(ed, EM_LIMITTEXT, CFG_NAME_MAX - 1, 0);
        HookChild(ed);
        MakeBtn(hInst, g_hwnd, L"添加", base + 2);
        MakeBtn(hInst, g_hwnd, L"删除", base + 3);
        MakeBtn(hInst, g_hwnd, L"选取窗口", base + 5);   /* 只填输入框，还得点"添加" */
    }

    /* 底部"窗框"最后建：z 序压在表单内容之上，滚动时挡住溢出的内容 */
    g_footer = CreateWindowExW(WS_EX_CONTROLPARENT, FOOTER_CLASS, L"",
                               WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                               0, 0, 10, 10, g_hwnd, NULL, hInst, NULL);
    g_status = MakeLabel(hInst, g_footer, L"", IDC_STATUS);
    MakeBtn(hInst, g_footer, L"重新加载", IDC_RELOAD);
    MakeBtn(hInst, g_footer, L"保存", IDC_SAVEONLY);
    MakeBtn(hInst, g_footer, L"保存并重启", IDC_SAVE);
    MakeBtn(hInst, g_footer, L"退出", IDC_EXIT);

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
        /* 选窗态下 Esc 取消：焦点多半在某个按钮/输入框上，主窗口的 WndProc
           收不到这个按键，所以在循环里拦（这是唯一能稳稳看到所有按键的地方） */
        if (g_pickList >= 0 && msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            PickEnd();
            SetStatus(L"选窗已取消");
            continue;
        }
        /* Tab 在控件之间移动焦点：IsDialogMessage 让输入框也吃 Tab，
           否则设置界面里没法用键盘走完一整张表单。 */
        if (!IsDialogMessageW(g_hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return 0;
}
