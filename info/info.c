/* ======================================================================
   CapsEnhance —— Windows CapsLock 快捷键增强程序
   Copyright (C) 2026  qm

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.
   ------------------------------------------------------------------ */

#include <windows.h>
#include <commctrl.h>   /* SetWindowSubclass / DefSubclassProc / InitCommonControlsEx */
#include <richedit.h>   /* CHARFORMAT2W / MSFTEDIT_CLASS */
#include <winver.h>     /* GetFileVersionInfoW / VerQueryValueW */
#include <sddl.h>       /* ConvertSidToStringSidW：用户 SID 的文本形态 */
#include <dwmapi.h>     /* DwmSetWindowAttribute / DwmGetWindowAttribute：深色标题栏与 cloak */
#include <uxtheme.h>    /* SetWindowTheme：现代滚动条主题段 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../src/info_shared.h"   /* 类名 / 消息名 / 默认值与值域：与主程序同一份 */
#include "../src/info_fields.h"   /* 字段注册表（常用/全部两个视图共用） */

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

/* 深色标题栏属性号。SDK 26100 的 dwmapi.h 已定义它；老 SDK 没有时补一个
   （Win10 1809+ / Win11 都是 20）。取不到该属性的老系统上 DwmSetWindowAttribute
   返回失败码、标题栏保持系统默认，不影响其它部分。 */
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

/* 路径类字段的缓冲长度。**不用 MAX_PATH**：长路径能到 32767，而 260 会截断 ——
   截断的路径拿去显示只是难看，但截断的路径拿去解析就是另一个文件（config.c 里
   有同样的说明，那里用的是 PROC_PATH_MAX=1024）。 */
#define PATH_CAP 1024

/* ================= 窗口信息弹窗（CapsEnhanceInfo.exe） =================
   `C + F12` 打开：把**开火那一刻的前台窗口**以及它所属进程的详情列成一屏文本，
   用一个多行可编辑控件承载（可拖选、可 Ctrl+C、可改，右键有常规编辑菜单）。

   为什么是独立 exe（不放进主程序）：见 src/info_shared.h 的文件头。一句话
   —— 主程序的主线程是低层键盘钩子的宿主，而本功能要 `EnumChildWindows`
   一棵可能上千节点的树、跨进程取标题、`OpenProcess` + 令牌查询；任何一步
   碰上无响应或受保护的进程都可能阻塞到系统超时，钩子会被静默摘掉。

   三个设计约束与它们的落地方式：
     1. **窗口绝对不能被"卡住"的白屏占着**。采集放在自己的工作线程上跑，
        窗口先出来显示「正在收集…」，采完由工作线程把缓冲 `PostMessage` 回
        UI 线程灌进编辑控件。UI 线程一次跨进程调用都不做。
     2. **它是个小文本编辑器，不是一个只读报表**。默认就可编辑（用户要摘一段
        贴到别处、删掉不要的行）。因此刷新只能手动触发，且缓冲被改过
        （`EM_GETMODIFY`）时先问一句 —— 定时刷新会把用户刚敲的字无声抹掉。
     3. **字体由用户定**（设置界面「窗口信息」段 / ini 的 InfoFont + InfoFontSize）。
        留空时按 Cascadia Mono → Consolas → 微软雅黑 逐级回落（等宽优先：
        字段值靠空格对齐，比例字体下对齐会散）。

   两个视图与筛选：**常用**按 InfoCommonFields 排好的一批字段平铺（友好名），
   **全部**是全量分组信息（`[段名]` 行加粗 + 强调色）。两个视图**各自留一份源
   文本**（用户改过的版本就是它），切换时各存各、互不覆盖。
   顶部筛选框做的是**过滤**而不是高亮：只留下包含该词的行（键或值都算，不区分
   大小写），同时把控件设成只读 —— 过滤结果和原文没有逐行对应关系，在其上改字
   说不清改的是哪一行；清空筛选词就把源文本原样放回去。

   位置：**窗口中心对准鼠标光标**，并钳进光标所在显示器的工作区。这条几何
   规则照搬 CLaunch-search 的 `windowPos(光标)` 分支（同为作者的项目）：
   光标点 → `MonitorFromPoint` → `GetMonitorInfo(rcWork)` → 居中 → 双向钳制。
   两个必须照搬的护栏：① 目标屏 DPI 要用 `GetDpiForMonitor` 求，不能用
   `GetDpiForWindow`（窗口还没搬过去）；② 落位必须在 `ShowWindow` 之前，
   且 `WM_DPICHANGED` 的系统建议矩形**不能**照抄（隐藏窗口上照抄会二次缩放，
   多次启动尺寸会漂）。

   与主程序的协议（两个注册消息，见 src/info.c）：
     INFO_MSG_OPEN  主程序 -> 本程序：wParam = 目标 HWND（0 = 沿用上次的）
     INFO_MSG_SIZE  本程序 -> 主程序：wParam/lParam = 宽/高（DIP），关窗时回存
   命令行：`--parent <hwnd> --hwnd <hwnd>`。前者用于回存尺寸（主程序的窗口是
   message-only，FindWindow 找不到），后者让第一帧就能开始采集，不必等消息。
   */ 

#define APP_TITLE   L"CapsEnhance 窗口信息"
#define EDIT_CLASS  MSFTEDIT_CLASS      /* "RICHEDIT50W"（Msftedit.dll） */

#define IDC_EDIT    1001
#define IDC_SEARCH  1002                /* 顶部筛选框 */
#define IDC_VCOMMON 1004                /* 顶部工具条按钮：常用 */
#define IDC_VALL    1005                /* 顶部工具条按钮：全部 */

/* 筛选框的占位提示（cue banner）。放在这里免得依赖头文件是否声明了它。 */
#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER (ECM_FIRST + 1)
#endif

/* 右上菜单（编辑控件不自带上下文菜单，得我们自己弹） */
#define IDM_UNDO      2001
#define IDM_CUT       2002
#define IDM_COPY      2003
#define IDM_PASTE     2004
#define IDM_DELETE    2005
#define IDM_SELECTALL 2006
#define IDM_REFRESH   2007
#define IDM_PICK      2008                /* 瞄准窗口：点一下就采集那个窗口 */

#define WM_APP_COLLECTED (WM_APP + 1)   /* 工作线程 -> UI：wParam 保留，lParam = WCHAR* 缓冲 */

#define TID_ARMED 1                     /* 1.5s 等不到目标就自己退（见 wWinMain） */
#define ARMED_MS  1500
#define TID_POLL  2                     /* 采集超时保护（见 ArmCollectGuard） */
#define TID_SMOOTH 3                    /* 滚轮平滑滚动的一帧（见 ScrollSmoothTick） */

/* 采集上限：Chromium 系的顶层窗口底下能有上千个子窗口，全量列出来是几 MB
   的文本，编辑控件排版与滚动都会明显变沉，而真正要看的通常是前几层。 */
#define MAX_TREE_DEPTH 8
#define MAX_TREE_NODES 400

/* 字段标签的对齐列宽（单位：**半角**；一个汉字算 2）。值从这个列开始，
   因此用等宽字体时值能对齐成一条线。超过这个列宽的标签（没有这么长的，
   但别让它在将来变成"值被挤到看不见"）只留一个空格。 */
#define LABEL_COL 20

/* ---------- 全局状态 ---------- */
static HINSTANCE g_inst;
static HWND   g_hwnd, g_edit;
static HWND   g_parent;          /* 主程序窗口：关窗时把尺寸投给它 */
static HWND   g_target;          /* 要看的窗口（开火那一刻的前台窗口快照） */
static UINT   g_msgOpen, g_msgSize;
static BOOL   g_busy;            /* 采集线程在跑 */
static int    g_dpi = 96;

static int    g_wDip = INFO_W_DEFAULT, g_hDip = INFO_H_DEFAULT;   /* 96 DPI 逻辑像素 */
static WCHAR  g_face[LF_FACESIZE];      /* 实际使用的字体名（回落已解析） */
static int    g_pt = INFO_FONT_SIZE_DEFAULT;

static HFONT  g_editFont;
static int    g_lineH = 0;            /* 编辑区一行的高度（像素）：滚轮折算用 */
static int    g_scrollTargetY = -1;   /* 平滑滚动的目标（像素）；<0 = 没在滚 */

static BOOL   g_dark;            /* 系统应用主题为深色（跟随，见 ApplyTheme） */
static HBRUSH g_bgBrush;         /* 窗口留白区底色刷（随主题重建） */

static HWND   g_search, g_btnCommon, g_btnAll;   /* 筛选框 / 视图按钮 */
static int    g_view;            /* 0 = 常用，1 = 全部；两个视图都可编辑 */
/* 两个视图各存一份**源文本**（用户编辑后的版本就是它）。筛选只是显示层：
   控件里显示过滤结果时，源文本原样留着，清掉筛选词就把它放回去 —— 否则
   一边筛一边想改两笔，改的到底是哪一份就说不清了。 */
static WCHAR* g_viewText[2];
static BOOL   g_filtering;       /* 当前显示的是筛选结果（此时控件只读） */
static BOOL   g_noSync;          /* 刚换过源文本：别拿控件里的旧内容回写 */
static int    g_common[IF_COUNT];/* 常用字段顺序（字段 id，来自 InfoCommonFields） */
static int    g_commonN;
static HBRUSH g_editBrush;       /* 筛选框底色刷（随主题重建） */
static HFONT  g_uiFont;          /* 筛选框 / 按钮 / 状态文字的界面字体 */

/* 动态取址：GetProcAddress 要求 hModule 非空（/analyze C6387）。kernel32 /
   user32 在本进程里必然已加载，把判空收在一处，调用点不必各自解释一遍。 */
static FARPROC ModFn(const WCHAR* mod, const char* name) {
    HMODULE m = GetModuleHandleW(mod);
    return m ? GetProcAddress(m, name) : NULL;
}

/* ================= ini：只读四项，但必须按 UTF-8 解 =================
   用户的 ini 里可能有中文（字体名就是最典型的一项："微软雅黑"）。Windows 的
   GetPrivateProfileStringW 在文件没有 BOM 时按 ANSI 解释，中文会变成乱码，
   所以这里**不用**它 —— 与 config.c 那条"ini 是 UTF-8"的约定保持一致：
   整份读进来、跳 BOM、MultiByteToWideChar(CP_UTF8)，再按行找 [General]。 */
typedef struct {
    WCHAR font[LF_FACESIZE];
    int   pt;
    int   wDip, hDip;
    WCHAR common[512];     /* InfoCommonFields：常用视图的有序字段 key 列表 */
} Ini;

static void TrimW(WCHAR* s) {
    WCHAR* p = s;
    while (*p == L' ' || *p == L'\t' || *p == L'\r' || *p == L'\n') p++;
    if (p != s) memmove(s, p, (wcslen(p) + 1) * sizeof(WCHAR));
    size_t n = wcslen(s);
    while (n && (s[n - 1] == L' ' || s[n - 1] == L'\t' || s[n - 1] == L'\r' || s[n - 1] == L'\n'))
        s[--n] = 0;
}

static int ParseIntW(const WCHAR* s, int def) {
    while (*s == L' ' || *s == L'\t') s++;
    int sign = 1, v = 0, any = 0;
    if (*s == L'-') { sign = -1; s++; }
    while (*s >= L'0' && *s <= L'9') { v = v * 10 + (*s - L'0'); s++; any = 1; if (v > 100000000) return def; }
    return any ? sign * v : def;
}

static int ClampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* 读 ini（只读、失败即用默认值；文件不在或没这一段都算正常）。 */
static void IniLoad(Ini* ini) {
    ini->font[0] = 0;
    ini->pt   = INFO_FONT_SIZE_DEFAULT;
    ini->wDip = INFO_W_DEFAULT;
    ini->hDip = INFO_H_DEFAULT;
    ini->common[0] = 0;

    WCHAR path[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    WCHAR* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    if ((size_t)(slash + 1 - path) + 16 >= MAX_PATH) return;
    wcscpy_s(slash + 1, MAX_PATH - (size_t)(slash + 1 - path), L"CapsEnhance.ini");

    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD sz = GetFileSize(f, NULL);
    if (sz == 0 || sz > (1024 * 1024)) { CloseHandle(f); return; }

    char* raw = (char*)calloc((size_t)sz + 1, 1);
    if (!raw) { CloseHandle(f); return; }
    DWORD got = 0;
    BOOL ok = ReadFile(f, raw, sz, &got, NULL);
    CloseHandle(f);
    if (!ok || !got) { free(raw); return; }

    char* body = raw;
    size_t bodyLen = got;
    if (bodyLen >= 3 && (unsigned char)body[0] == 0xEF &&
        (unsigned char)body[1] == 0xBB && (unsigned char)body[2] == 0xBF) {
        body += 3; bodyLen -= 3;
    }
    int wl = MultiByteToWideChar(CP_UTF8, 0, body, (int)bodyLen, NULL, 0);
    if (wl <= 0) { free(raw); return; }
    WCHAR* wide = (WCHAR*)calloc((size_t)wl + 1, sizeof(WCHAR));
    if (!wide) { free(raw); return; }
    MultiByteToWideChar(CP_UTF8, 0, body, (int)bodyLen, wide, wl);
    free(raw);

    /* 逐行扫；键一律在 [General] 段（见 info_shared.h 里那段"为什么不另开段"） */
    WCHAR cur[64] = L"";
    WCHAR* line = wide;
    while (line && *line) {
        WCHAR* nl = wcschr(line, L'\n');
        if (nl) *nl = 0;
        WCHAR* t = line;
        TrimW(t);
        if (t[0] == L'[') {
            WCHAR* rb = wcschr(t, L']');
            if (rb) *rb = 0;
            _snwprintf_s(cur, 64, _TRUNCATE, L"%s", t + 1);
        } else if (t[0] && t[0] != L';' && t[0] != L'#') {
            WCHAR* eq = wcschr(t, L'=');
            if (eq && _wcsicmp(cur, L"General") == 0) {
                *eq = 0;
                WCHAR* key = t;  TrimW(key);
                WCHAR* val = eq + 1; TrimW(val);
                if (_wcsicmp(key, L"InfoFont") == 0)
                    _snwprintf_s(ini->font, LF_FACESIZE, _TRUNCATE, L"%s", val);
                else if (_wcsicmp(key, L"InfoFontSize") == 0)
                    ini->pt = ClampInt(ParseIntW(val, INFO_FONT_SIZE_DEFAULT),
                                       INFO_FONT_SIZE_MIN, INFO_FONT_SIZE_MAX);
                else if (_wcsicmp(key, L"InfoW") == 0)
                    ini->wDip = ClampInt(ParseIntW(val, INFO_W_DEFAULT), INFO_W_MIN, INFO_W_MAX);
                else if (_wcsicmp(key, L"InfoH") == 0)
                    ini->hDip = ClampInt(ParseIntW(val, INFO_H_DEFAULT), INFO_H_MIN, INFO_H_MAX);
                else if (_wcsicmp(key, L"InfoCommonFields") == 0)
                    _snwprintf_s(ini->common, 512, _TRUNCATE, L"%s", val);
            }
        }
        line = nl ? nl + 1 : NULL;
    }
    free(wide);
}

/* 把逗号分隔的字段 key 解析成 g_common[]（去重、跳过未知 key）。空串 = 默认那 11 项。 */
static void AddCommonKey(const char* key) {
    int id = InfoFieldByKey(key);
    if (id < 0 || g_commonN >= IF_COUNT) return;
    for (int i = 0; i < g_commonN; i++) if (g_common[i] == id) return;
    g_common[g_commonN++] = id;
}

static void ParseCommon(const WCHAR* s) {
    g_commonN = 0;
    if (s && s[0]) {
        WCHAR tok[64];
        int tn = 0;
        for (const WCHAR* p = s; ; p++) {
            if (*p == L',' || *p == 0) {
                tok[tn] = 0;
                if (tn) {
                    char key[64];
                    if (WideCharToMultiByte(CP_ACP, 0, tok, -1, key, 64, NULL, NULL) > 0)
                        AddCommonKey(key);
                }
                tn = 0;
                if (*p == 0) break;
            } else if (tn < 63) {
                tok[tn++] = *p;
            }
        }
    }
    if (!g_commonN) {          /* 空配置或解析失败：用默认那 11 项（ASCII 字面量） */
        const char* d = INFO_COMMON_DEFAULT;
        char key[64];
        int kn = 0;
        for (const char* p = d; ; p++) {
            if (*p == ',' || *p == 0) {
                key[kn] = 0;
                if (kn) AddCommonKey(key);
                kn = 0;
                if (*p == 0) break;
            } else if (kn < 63) {
                key[kn++] = *p;
            }
        }
    }
}

/* ================= 字体：逐级回落 ================= */

static BOOL CALLBACK FindFontCb(const LOGFONTW* lf, const TEXTMETRICW* tm, DWORD type, LPARAM lp) {
    (void)tm; (void)type;
    if (_wcsicmp(lf->lfFaceName, (const WCHAR*)lp) == 0) return FALSE;   /* 找到 -> 停 */
    return TRUE;
}

static BOOL FontExists(const WCHAR* face) {
    if (!face || !face[0]) return FALSE;
    HDC dc = GetDC(NULL);
    if (!dc) return FALSE;
    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfCharSet = DEFAULT_CHARSET;
    _snwprintf_s(lf.lfFaceName, LF_FACESIZE, _TRUNCATE, L"%s", face);
    BOOL found = !EnumFontFamiliesExW(dc, &lf, FindFontCb, (LPARAM)face, 0);
    ReleaseDC(NULL, dc);
    return found;
}

/* 取得"实际能用"的字体名：先看配置，再按等宽优先的顺序回落。
   等宽在前是有理由的：字段靠空格对齐，比例字体下值那一列会参差不齐。 */
static void ResolveFont(const WCHAR* want, WCHAR* out, size_t cap) {
    static const WCHAR* kChain[] = { L"Cascadia Mono", L"Consolas", L"微软雅黑" };
    if (FontExists(want)) { _snwprintf_s(out, cap, _TRUNCATE, L"%s", want); return; }
    for (size_t i = 0; i < sizeof(kChain) / sizeof(kChain[0]); i++) {
        if (FontExists(kChain[i])) { _snwprintf_s(out, cap, _TRUNCATE, L"%s", kChain[i]); return; }
    }
    /* 一个都没有（精简系统）：退回系统消息字体的字面名，至少不是空白。 */
    NONCLIENTMETRICSW ncm;
    memset(&ncm, 0, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0) &&
        ncm.lfMessageFont.lfFaceName[0]) {
        _snwprintf_s(out, cap, _TRUNCATE, L"%s", ncm.lfMessageFont.lfFaceName);
        return;
    }
    _snwprintf_s(out, cap, _TRUNCATE, L"%s", L"Segoe UI");
}

/* 磅 -> 像素高度（CreateFont 的 lfHeight 是逻辑单位）。字号是磅，与 DPI 无关：
   窗口换到别的显示器时只需重建字体，用户设的"12 磅"仍然表示 12 磅。 */
static HFONT MakeFont(const WCHAR* face, int pt, int dpi, BOOL bold) {
    return CreateFontW(-MulDiv(pt, dpi, 72), 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL,
                       FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, face);
}

/* ================= 文本构造：一块会自己长的缓冲 ================= */

typedef struct { WCHAR* p; size_t n, cap; } Buf;

static BOOL BufReserve(Buf* b, size_t need) {
    if (b->cap >= need) return TRUE;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < need) cap *= 2;
    WCHAR* np = (WCHAR*)realloc(b->p, cap * sizeof(WCHAR));
    if (!np) return FALSE;
    b->p = np;
    b->cap = cap;
    return TRUE;
}

static void BufAddf(Buf* b, const WCHAR* fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int need = _vscwprintf(fmt, ap);
    va_end(ap);
    if (need < 0) { va_end(ap2); return; }
    if (!BufReserve(b, b->n + (size_t)need + 1)) { va_end(ap2); return; }
    _vsnwprintf_s(b->p + b->n, b->cap - b->n, _TRUNCATE, fmt, ap2);
    va_end(ap2);
    b->n += (size_t)need;
    b->p[b->n] = 0;
}

/* ================= 字段值暂存（「常用」视图重排用） =================
   采集时每发一条「键: 值」，按字段 id 把**原始值**存一份；采完再按用户配置的
   常用字段顺序另拼一份「常用」文本。只在采集线程里读写（g_busy 保证不并发），
   UI 线程拿的是采完后的成品文本。 */
#define FV_MAX 2048
static BOOL  s_fvHas[IF_COUNT];
static WCHAR s_fvVal[IF_COUNT][FV_MAX];

static void FvStore(int id, const WCHAR* value) {
    if (id < 0 || id >= IF_COUNT) return;
    _snwprintf_s(s_fvVal[id], FV_MAX, _TRUNCATE, L"%s", value ? value : L"");
    s_fvHas[id] = TRUE;
}

/* 一行字段：`标签:` + **制表符** + 值。用制表符而不是按半角补空格，是为了让
   值精确对齐到同一像素列（EM_SETTABSTOPS 定了唯一的制表位）：等宽、比例字体
   都对得齐，也不会被"汉字按 2 个半角估算"的误差坑到。

   ★ 值一律压成**一行**：CR / LF / TAB 与其余控制字符换成可见转义。窗口标题、
     命令行、子控件文本都来自别的进程，完全可能自带换行；原样写进去会把一行
     裂成几行，裂出来的那几行没有键名，整屏就分不清哪部分是键了。宁可让长值
     靠横向滚动看全，也不让一行字段占多行。 */
static void BufFieldRaw(Buf* b, int id, const WCHAR* label, const WCHAR* value) {
    FvStore(id, value);
    BufAddf(b, L"%s:\t", label);
    for (const WCHAR* q = value ? value : L""; *q; q++) {
        if (*q == L'\r')      BufAddf(b, L"\\r");
        else if (*q == L'\n') BufAddf(b, L"\\n");
        else if (*q == L'\t') BufAddf(b, L"\\t");
        else if (*q < 0x20)    BufAddf(b, L"\\x%02X", (unsigned)*q);
        else                   BufAddf(b, L"%c", *q);
    }
    BufAddf(b, L"\n");
}

/* 注册表里的字段（重载：第二参数给 IF_* 编号）：名字一律取自 kInfoFields，
   于是「常用」与「全部」永远同一套说法；值顺带按 id 存一份，供「常用」视图
   重排。Collect 里因此不再出现字面标签 —— 也就没有"字面标签与注册表对不上、
   值静默丢失"这种可能。 */
static void BufField(Buf* b, int id, const WCHAR* value) {
    if (id < 0 || id >= IF_COUNT) return;
    BufFieldRaw(b, id, kInfoFields[id].name, value);
}

/* 不属于注册表的行（UWP 说明、子窗口树节点、合计 / 结果 / 截断）：只打印，不入暂存。 */
static void BufField(Buf* b, const WCHAR* label, const WCHAR* value) {
    BufFieldRaw(b, -1, label, value);
}

static void BufFieldF(Buf* b, int id, const WCHAR* fmt, ...) {
    WCHAR tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(tmp, 1024, _TRUNCATE, fmt, ap);
    va_end(ap);
    BufField(b, id, tmp);
}

static void BufFieldF(Buf* b, const WCHAR* label, const WCHAR* fmt, ...) {
    WCHAR tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(tmp, 1024, _TRUNCATE, fmt, ap);
    va_end(ap);
    BufField(b, label, tmp);
}

/* 明确的"不可用"：写清原因。留空会让人以为字段坏了，写"无"又分不出
   "本来就没有"和"取不到"。第二参数给 IF_* 编号，名字走注册表。 */
static void BufUnavail(Buf* b, int id, const WCHAR* why) {
    WCHAR tmp[256];
    _snwprintf_s(tmp, 256, _TRUNCATE, L"不可用（%s）", why);
    BufField(b, id, tmp);
}

/* 组与组之间空一行。幂等：已经以空行结尾就不再加 —— 某个条件组（如进程资源）
   整组取不到时不会叠出两个空行，段头前也始终只留一个空行。 */
static void BufBlankLine(Buf* b) {
    if (b->n == 0) return;
    if (b->p[b->n - 1] != L'\n') BufAddf(b, L"\n");
    if (b->n < 2 || b->p[b->n - 2] != L'\n') BufAddf(b, L"\n");
}

static void BufSection(Buf* b, const WCHAR* name) {
    if (b->n) BufBlankLine(b);
    BufAddf(b, L"[%s]\n", name);
}

/* ================= 取窗口/进程信息的原语 ================= */

/* 跨进程取窗口文本。`GetWindowTextW` 对**别的进程**的窗口只返回缓存标题，
   对子控件恒返回空串（文档写得很清楚：这是为了不让调用方被无响应的进程拖住）。
   而本功能最需要的恰恰是子控件的标题，所以首选 user32 导出的
   `InternalGetWindowText`（服务端直接取，不往目标进程发消息，因此**不会阻塞**；
   System Informer 全项目都用这一条，本项目的取舍相同），取不到再退回
   `GetWindowTextW`。 */
typedef int (WINAPI* PFN_InternalGetWindowText)(HWND, LPWSTR, int);

static PFN_InternalGetWindowText s_internalGetText;

static void EnsureInternalGetText(void) {
    if (s_internalGetText) return;
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (u) s_internalGetText = (PFN_InternalGetWindowText)(void*)GetProcAddress(u, "InternalGetWindowText");
}

static void GetWinTitle(HWND h, WCHAR* out, int cap) {
    out[0] = 0;
    if (!h || !cap) return;
    EnsureInternalGetText();
    if (s_internalGetText && s_internalGetText(h, out, cap) > 0) { out[cap - 1] = 0; return; }
    out[0] = 0;
    GetWindowTextW(h, out, cap);
    out[cap - 1] = 0;
}

/* UWP：前台窗口属于宿主 `ApplicationFrameHost.exe`，真实程序在它下面的
   `Windows.UI.Core.CoreWindow` 子窗口上（那个窗口属于应用自己的进程）。
   这与 C+F4「打开所在文件夹」用的是同一条判定（见 src/folder.c 的 CorePidOf）：
   不这么换，用户看到的就是"ApplicationFrameHost.exe / C:\Windows\System32"，
   而不是他心里的那个应用。 */
typedef struct { DWORD pid; } CoreFind;

static BOOL CALLBACK CorePidCb(HWND child, LPARAM lp) {
    WCHAR cls[64];
    if (!GetClassNameW(child, cls, 64)) return TRUE;
    if (_wcsicmp(cls, L"Windows.UI.Core.CoreWindow") != 0) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(child, &pid);
    if (!pid) return TRUE;
    ((CoreFind*)lp)->pid = pid;
    return FALSE;                      /* 找到第一个就停 */
}

static void Hex(HWND h, WCHAR* out, size_t cap) {
    _snwprintf_s(out, cap, _TRUNCATE, L"0x%llX", (unsigned long long)(ULONG_PTR)h);
}

/* 一行"某窗口"：句柄 + 类名 + 标题（标题截断，免得一个网页标题把整行撑断）。
   句柄为 0 时写「无」——父窗口/所有者窗口本来就可能是 0，那不是失败。 */
/* 窗口句柄类字段（父 / 所有者 / 根 / 根所有者）：没有就写「无（0）」，
   有就给「句柄  类名  "标题"」。第二参数是 IF_* 编号 —— 名字走注册表。 */
static void BufHandleField(Buf* b, int id, HWND h) {
    if (id < 0 || id >= IF_COUNT) return;
    if (!h) { BufField(b, id, L"无（0）"); return; }
    WCHAR hx[32], cls[128], title[160], line[512];
    Hex(h, hx, 32);
    cls[0] = 0;
    GetClassNameW(h, cls, 128);
    GetWinTitle(h, title, 160);
    if (title[0])
        _snwprintf_s(line, 512, _TRUNCATE, L"%s  %s  \"%s\"%s", hx, cls[0] ? cls : L"(无类名)", title,
                     wcslen(title) >= 159 ? L"…" : L"");
    else
        _snwprintf_s(line, 512, _TRUNCATE, L"%s  %s", hx, cls[0] ? cls : L"(无类名)");
    BufField(b, id, line);
}

/* ---------- 窗口层的"内部字段"：都是单次 user32 / DWM 调用 ---------- */

/* 还原矩形：最大化/最小化时 GetWindowRect 给的是"当前"，这一行给"还原后"。 */
static void NormalRectText(HWND h, WCHAR* out, size_t cap) {
    out[0] = 0;
    WINDOWPLACEMENT wp;
    memset(&wp, 0, sizeof(wp));
    wp.length = sizeof(wp);
    if (!GetWindowPlacement(h, &wp)) return;
    RECT r = wp.rcNormalPosition;
    _snwprintf_s(out, cap, _TRUNCATE, L"(%ld,%ld)-(%ld,%ld)  %ldx%ld",
                 r.left, r.top, r.right, r.bottom, r.right - r.left, r.bottom - r.top);
}

/* DWM cloak：UWP 挂起、虚拟桌面切走、被 DWM 主动隐藏的窗口，IsWindowVisible 仍是
   TRUE —— "看不见但活着"的窗口全靠这一项解释。 */
static void CloakedText(HWND h, WCHAR* out, size_t cap) {
    out[0] = 0;
    DWORD cloak = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloak, sizeof(cloak))))
        _snwprintf_s(out, cap, _TRUNCATE, cloak ? L"是（DWM 隐藏）" : L"否");
}

/* 窗口的 DPI 感知上下文：同一个 DPI 数值下，"PMv2 / System / Unaware" 决定了它
   会不会被系统拉伸（模糊）—— 排查缩放假象第一步。 */
static void DpiAwarenessText(HWND h, WCHAR* out, size_t cap) {
    out[0] = 0;
    typedef DPI_AWARENESS_CONTEXT (WINAPI* PFN_GETCTX)(HWND);
    typedef BOOL (WINAPI* PFN_EQUAL)(DPI_AWARENESS_CONTEXT, DPI_AWARENESS_CONTEXT);
    typedef DPI_AWARENESS (WINAPI* PFN_AWARE)(DPI_AWARENESS_CONTEXT);
    PFN_GETCTX fget = (PFN_GETCTX)(void*)ModFn(L"user32.dll", "GetWindowDpiAwarenessContext");
    if (!fget) { _snwprintf_s(out, cap, _TRUNCATE, L"取不到（Win10 1607 之前）"); return; }
    DPI_AWARENESS_CONTEXT ctx = fget(h);
    PFN_EQUAL feq = (PFN_EQUAL)(void*)ModFn(L"user32.dll", "AreDpiAwarenessContextsEqual");
    if (feq && feq(ctx, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        _snwprintf_s(out, cap, _TRUNCATE, L"每显示器 V2");
        return;
    }
    PFN_AWARE faware = (PFN_AWARE)(void*)ModFn(L"user32.dll", "GetAwarenessFromDpiAwarenessContext");
    if (!faware) return;
    switch (faware(ctx)) {
    case DPI_AWARENESS_UNAWARE:               _snwprintf_s(out, cap, _TRUNCATE, L"未感知（会被系统拉伸）"); break;
    case DPI_AWARENESS_SYSTEM_AWARE:          _snwprintf_s(out, cap, _TRUNCATE, L"系统"); break;
    case DPI_AWARENESS_PER_MONITOR_AWARE:     _snwprintf_s(out, cap, _TRUNCATE, L"每显示器"); break;
    default:                                  _snwprintf_s(out, cap, _TRUNCATE, L"未知"); break;
    }
}

/* 窗口字体：WM_GETFONT 只给 HFONT。GDI 句柄属于目标进程，GetObject 多半失败，
   那就把句柄值写出来（至少能看出"有没有设字体"）。 */
static void FontText(HWND h, WCHAR* out, size_t cap) {
    out[0] = 0;
    HFONT f = (HFONT)SendMessageW(h, WM_GETFONT, 0, 0);
    if (!f) { _snwprintf_s(out, cap, _TRUNCATE, L"未设置（用系统默认）"); return; }
    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    if (GetObjectW(f, sizeof(lf), &lf) == sizeof(lf) && lf.lfFaceName[0])
        _snwprintf_s(out, cap, _TRUNCATE, L"%s  %d 像素%s", lf.lfFaceName,
                     lf.lfHeight < 0 ? -lf.lfHeight : lf.lfHeight,
                     lf.lfWeight >= 700 ? L" 粗体" : L"");
    else
        _snwprintf_s(out, cap, _TRUNCATE, L"0x%llX（句柄属于目标进程，取不到字体名）",
                     (unsigned long long)(ULONG_PTR)f);
}

/* AppUserModelID / 包名：都在 appmodel.h，一次调用，不需要 COM。 */
static void PackageText(DWORD pid, WCHAR* aumid, size_t capA, WCHAR* pkg, size_t capP) {
    if (aumid && capA) aumid[0] = 0;
    if (pkg && capP) pkg[0] = 0;
    typedef LONG (WINAPI* PFN_AUMID)(HANDLE, UINT32*, PWSTR);
    typedef LONG (WINAPI* PFN_PKG)(HANDLE, UINT32*, PWSTR);
    PFN_AUMID fa = (PFN_AUMID)(void*)ModFn(L"kernel32.dll", "GetApplicationUserModelId");
    PFN_PKG fp = (PFN_PKG)(void*)ModFn(L"kernel32.dll", "GetPackageFullName");
    if (!fa && !fp) return;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return;
    WCHAR buf[512];
    if (fa && aumid && capA) {
        UINT32 n = 512;
        if (fa(h, &n, buf) == ERROR_SUCCESS)
            _snwprintf_s(aumid, capA, _TRUNCATE, L"%s", buf);
    }
    if (fp && pkg && capP) {
        UINT32 n = 512;
        if (fp(h, &n, buf) == ERROR_SUCCESS)
            _snwprintf_s(pkg, capP, _TRUNCATE, L"%s", buf);
    }
    CloseHandle(h);
}

/* 样式位 -> 可读符号（枚举够用就行：全列出来反而更难读） */typedef struct { DWORD bit; const WCHAR* name; } StyleBit;
static const StyleBit kStyle[] = {
    { WS_POPUP,        L"WS_POPUP" },
    { WS_CHILD,        L"WS_CHILD" },
    { WS_MINIMIZE,     L"WS_MINIMIZE" },
    { WS_MAXIMIZE,     L"WS_MAXIMIZE" },
    { WS_VISIBLE,      L"WS_VISIBLE" },
    { WS_DISABLED,     L"WS_DISABLED" },
    { WS_CLIPSIBLINGS, L"WS_CLIPSIBLINGS" },
    { WS_CLIPCHILDREN, L"WS_CLIPCHILDREN" },
    { WS_CAPTION,      L"WS_CAPTION" },
    { WS_BORDER,       L"WS_BORDER" },
    { WS_DLGFRAME,     L"WS_DLGFRAME" },
    { WS_VSCROLL,      L"WS_VSCROLL" },
    { WS_HSCROLL,      L"WS_HSCROLL" },
    { WS_SYSMENU,      L"WS_SYSMENU" },
    { WS_THICKFRAME,   L"WS_THICKFRAME" },
    { WS_GROUP,        L"WS_GROUP" },
    { WS_TABSTOP,      L"WS_TABSTOP" },
};
/* 截图保护的两个取值不是所有 SDK 版本都定义（WDA_EXCLUDEFROMCAPTURE 要
   Win10 2004 的 SDK），自己补一份，语义与 winuser.h 一致。 */
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

/* 扩展样式表只列"看到名字就明白现象"的那些位。NOREDIRECTIONBITMAP 一定要有：
   带这一位的窗口（DirectComposition / 部分 UWP）没有重定向表面，PrintWindow
   与 BitBlt 截出来就是全黑 —— 这正是"截图黑"最常见的第二种原因。 */
#ifndef WS_EX_NOREDIRECTIONBITMAP
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#endif
static const StyleBit kExStyle[] = {
    { WS_EX_TOPMOST,       L"WS_EX_TOPMOST" },
    { WS_EX_TOOLWINDOW,    L"WS_EX_TOOLWINDOW" },
    { WS_EX_APPWINDOW,     L"WS_EX_APPWINDOW" },
    { WS_EX_LAYERED,       L"WS_EX_LAYERED" },
    { WS_EX_TRANSPARENT,   L"WS_EX_TRANSPARENT" },
    { WS_EX_NOACTIVATE,    L"WS_EX_NOACTIVATE" },
    { WS_EX_ACCEPTFILES,   L"WS_EX_ACCEPTFILES" },
    { WS_EX_CONTROLPARENT, L"WS_EX_CONTROLPARENT" },
    { WS_EX_WINDOWEDGE,    L"WS_EX_WINDOWEDGE" },
    { WS_EX_CLIENTEDGE,    L"WS_EX_CLIENTEDGE" },
    { WS_EX_STATICEDGE,    L"WS_EX_STATICEDGE" },
    { WS_EX_MDICHILD,      L"WS_EX_MDICHILD" },
    { WS_EX_DLGMODALFRAME, L"WS_EX_DLGMODALFRAME" },
    { WS_EX_NOPARENTNOTIFY,L"WS_EX_NOPARENTNOTIFY" },
    { WS_EX_COMPOSITED,    L"WS_EX_COMPOSITED" },
    { WS_EX_NOREDIRECTIONBITMAP, L"WS_EX_NOREDIRECTIONBITMAP" },
};

static void StyleText(DWORD v, const StyleBit* tab, int n, WCHAR* out, size_t cap) {
    WCHAR* p = out;
    size_t left = cap;
    int wrote = 0;
    for (int i = 0; i < n; i++) {
        if (!(v & tab[i].bit)) continue;
        int k = _snwprintf_s(p, left, _TRUNCATE, L"%s%s", wrote ? L"|" : L"", tab[i].name);
        if (k < 0) break;
        size_t used = wcslen(p);
        p += used; left -= used;
        wrote = 1;
        if (left < 2) break;
    }
    if (!wrote) _snwprintf_s(out, cap, _TRUNCATE, L"(无)");
}

/* 进程映像全路径（文档化通道；本程序通常以管理员运行，能覆盖大多数目标） */
static BOOL ProcPath(DWORD pid, WCHAR* out, size_t cap) {
    out[0] = 0;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return FALSE;
    DWORD n = (DWORD)cap;
    BOOL ok = QueryFullProcessImageNameW(h, 0, out, &n);
    CloseHandle(h);
    if (!ok || !out[0]) { out[0] = 0; return FALSE; }
    return TRUE;
}

/* 命令行：`NtQueryInformationProcess(ProcessCommandLineInformation=60)`。
   这是 WDK 里公开的进程信息类（Win8.1+），**不需要**读目标进程内存，
   因此 32/64 位目标一视同仁 —— 比"读 PEB 拿 CommandLine"省掉一整套位数分支。
   返回的缓冲由调用方 HeapFree。 */
typedef LONG (NTAPI* PFN_NtQueryInformationProcess)(HANDLE, ULONG, PVOID, ULONG, PULONG);

typedef struct {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR  Buffer;
} UStr;

static PFN_NtQueryInformationProcess s_ntQip;

static void EnsureNt(void) {
    if (s_ntQip) return;
    HMODULE n = GetModuleHandleW(L"ntdll.dll");
    if (!n) n = LoadLibraryW(L"ntdll.dll");
    if (n) s_ntQip = (PFN_NtQueryInformationProcess)(void*)GetProcAddress(n, "NtQueryInformationProcess");
}

static BOOL ProcCommandLine(DWORD pid, WCHAR* out, size_t cap) {
    out[0] = 0;
    EnsureNt();
    if (!s_ntQip) return FALSE;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return FALSE;

    ULONG need = 0;
    LONG st = s_ntQip(h, 60, NULL, 0, &need);
    /* STATUS_INFO_LENGTH_MISMATCH(0xC0000004) / STATUS_BUFFER_TOO_SMALL(0xC0000023) */
    if ((st == (LONG)0xC0000004 || st == (LONG)0xC0000023) && need > 0 && need < (1024 * 64)) {
        UStr* us = (UStr*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, need + sizeof(WCHAR) * 2);
        if (us) {
            st = s_ntQip(h, 60, us, need, &need);
            if (st == 0 && us->Buffer && us->Length > 0) {
                us->Buffer[us->Length / sizeof(WCHAR)] = 0;
                _snwprintf_s(out, cap, _TRUNCATE, L"%s", us->Buffer);
            }
            HeapFree(GetProcessHeap(), 0, us);
        }
    }
    CloseHandle(h);
    return out[0] != 0;
}

/* 当前目录：只能读目标进程的 PEB（`ProcessCommandLineInformation` 没有对应
   兄弟类）。偏移量取自 WDK 的 RTL_USER_PROCESS_PARAMETERS 布局（Vista 起稳定）：
     PEB.ProcessParameters    x64 0x20 / x86 0x10
     ...CurrentDirectory       x64 0x38 / x86 0x24
   只对**同位数**目标尝试：32 位目标的 PEB 结构要用 NtWow64* 系列读，收益不抵
   复杂度，直接写"未实现"。读失败/校验不过一律退回"不可用"，绝不猜。 */
static BOOL ProcCwd(DWORD pid, WCHAR* out, size_t cap) {
    out[0] = 0;
    EnsureNt();
    if (!s_ntQip) return FALSE;

    BOOL targetWow64 = FALSE;
    HANDLE hq = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hq) return FALSE;
    BOOL gotWow = IsWow64Process(hq, &targetWow64);
    CloseHandle(hq);
    if (!gotWow) return FALSE;
#ifdef _WIN64
    if (targetWow64) return FALSE;      /* 32 位目标：见上 */
#else
    (void)targetWow64;
#endif

    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) return FALSE;

    /* PROCESS_BASIC_INFORMATION（winternl.h 里也有，但字段名与这里用到的顺序
       一致，自己写一份免得依赖它的 typedef 顺序） */
    struct {
        LONG     ExitStatus;
        PVOID    PebBaseAddress;
        ULONG_PTR AffinityMask;
        LONG     BasePriority;
        ULONG_PTR UniqueProcessId;
        ULONG_PTR InheritedFromUniqueProcessId;
    } pbi;
    memset(&pbi, 0, sizeof(pbi));
    ULONG got = 0;
    LONG st = s_ntQip(h, 0, &pbi, sizeof(pbi), &got);
    if (st != 0 || !pbi.PebBaseAddress) { CloseHandle(h); return FALSE; }

    BYTE* peb = (BYTE*)pbi.PebBaseAddress;
    PVOID params = NULL;
    SIZE_T rd = 0;
#ifdef _WIN64
    const SIZE_T offParams = 0x20, offCurDir = 0x38;
#else
    const SIZE_T offParams = 0x10, offCurDir = 0x24;
#endif
    if (!ReadProcessMemory(h, peb + offParams, &params, sizeof(params), &rd) ||
        rd != sizeof(params) || !params) {
        CloseHandle(h);
        return FALSE;
    }
    UStr us;
    memset(&us, 0, sizeof(us));
    if (!ReadProcessMemory(h, (BYTE*)params + offCurDir, &us, sizeof(us), &rd) ||
        rd != sizeof(us) || !us.Buffer || us.Length == 0 || (us.Length & 1) ||
        us.Length > 0x1000 || us.MaximumLength < us.Length) {
        CloseHandle(h);
        return FALSE;
    }
    WCHAR* tmp = (WCHAR*)calloc((size_t)us.Length / sizeof(WCHAR) + 2, sizeof(WCHAR));
    if (!tmp) { CloseHandle(h); return FALSE; }
    if (ReadProcessMemory(h, us.Buffer, tmp, us.Length, &rd) && rd == us.Length) {
        /* 合理性校验：必须是盘符路径或 UNC。对不上说明偏移量不适用于这个
           系统（结构布局改了），此时宁可写"不可用"也不把垃圾当路径显示。 */
        if ((tmp[0] >= L'A' && tmp[0] <= L'Z') || (tmp[0] >= L'a' && tmp[0] <= L'z') ||
            (tmp[0] == L'\\' && tmp[1] == L'\\'))
            _snwprintf_s(out, cap, _TRUNCATE, L"%s", tmp);
    }
    free(tmp);
    CloseHandle(h);
    return out[0] != 0;
}

/* ---------- 安全上下文：一次 OpenProcess + 一次 OpenProcessToken 取全 ----------
   账户 / 用户 SID / 完整性级别 / 运行权限 / UAC 虚拟化 / 缓解策略 / 保护级别
   都来自同一份数据源（进程句柄 + 令牌）。分七次各开一遍对象既慢，也容易只读到
   一半。受保护进程（PPL / System）在 OpenProcessToken 就会被拒，那是"权限"
   不是"错"，调用方逐项写"不可用"。 */
typedef struct {
    WCHAR account[196];      /* 机器\用户 */
    WCHAR sid[192];          /* S-1-5-21-... */
    WCHAR integrity[128];    /* Low / Medium / High / System */
    WCHAR elevation[128];    /* 管理员（已提升）/ 标准用户（未提升）… */
    WCHAR uiaccess[96];      /* UIAccess 标志（令牌上的位，不随清单声明） */
    WCHAR mandatory[96];     /* 完整性策略：禁写高 / 禁读高 */
    WCHAR restrictions[128]; /* 普通 / 受限令牌 / 曾被过滤 */
    WCHAR virtualized[96];   /* UAC 文件与注册表虚拟化（含"允不允许"） */
    WCHAR linked[96];        /* 拆分令牌的另一半是否存在 */
    WCHAR container[160];    /* AppContainer / 沙箱 / AppSilo / 私有命名空间 */
    WCHAR capabilities[64];  /* 能力数量（只有容器进程才有内容） */
    WCHAR appsid[192];       /* AppContainer 的包 SID */
    WCHAR logonsid[96];      /* 登录会话 SID（S-1-5-5-…） */
    WCHAR protection[128];   /* PPL 类型与签名者 */
    WCHAR mitigation[256];   /* DEP / ASLR / CFG / ACG / 严格句柄 */
    WCHAR privs[1400];       /* 已启用的特权（前 12 个 + 总数） */
} ProcSecurity;

/* SID -> "S-1-5-21-..."。账户名会被改名、也可能同名的本地/域账户打架，
   只有 SID 是稳定标识。 */
static void SidToText(PSID sid, WCHAR* out, size_t cap) {
    out[0] = 0;
    if (!sid) return;
    WCHAR* s = NULL;
    if (ConvertSidToStringSidW(sid, &s) && s) {
        _snwprintf_s(out, cap, _TRUNCATE, L"%s", s);
        LocalFree(s);
    }
}

/* 缓解策略摘要：只列"装没装"最容易解释现象的几项（DEP / ASLR / CFG / ACG /
   句柄严格校验）。十来个策略全列会把这一屏撑长，需要细看时另有专门工具。 */
static void ProcMitigationText(HANDLE hp, WCHAR* out, size_t cap) {
    out[0] = 0;
    if (!hp) return;
    WCHAR s[256] = L"";
    PROCESS_MITIGATION_DEP_POLICY dep;
    memset(&dep, 0, sizeof(dep));
    if (GetProcessMitigationPolicy(hp, ProcessDEPPolicy, &dep, sizeof(dep)) && dep.Enable)
        wcscat_s(s, 256, L"DEP");
    PROCESS_MITIGATION_ASLR_POLICY aslr;
    memset(&aslr, 0, sizeof(aslr));
    if (GetProcessMitigationPolicy(hp, ProcessASLRPolicy, &aslr, sizeof(aslr)) &&
        (aslr.EnableBottomUpRandomization || aslr.EnableForceRelocateImages))
        wcscat_s(s, 256, s[0] ? L" / ASLR" : L"ASLR");
    PROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY cfg;
    memset(&cfg, 0, sizeof(cfg));
    if (GetProcessMitigationPolicy(hp, ProcessControlFlowGuardPolicy, &cfg, sizeof(cfg)) &&
        cfg.EnableControlFlowGuard)
        wcscat_s(s, 256, s[0] ? L" / CFG" : L"CFG");
    PROCESS_MITIGATION_BINARY_SIGNATURE_POLICY sig;
    memset(&sig, 0, sizeof(sig));
    if (GetProcessMitigationPolicy(hp, ProcessSignaturePolicy, &sig, sizeof(sig)) &&
        (sig.MicrosoftSignedOnly || sig.StoreSignedOnly))
        wcscat_s(s, 256, s[0] ? L" / 仅签名映像(ACG)" : L"仅签名映像(ACG)");
    PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY sh;
    memset(&sh, 0, sizeof(sh));
    if (GetProcessMitigationPolicy(hp, ProcessStrictHandleCheckPolicy, &sh, sizeof(sh)) &&
        sh.RaiseExceptionOnInvalidHandleReference)
        wcscat_s(s, 256, s[0] ? L" / 句柄严格校验" : L"句柄严格校验");
    _snwprintf_s(out, cap, _TRUNCATE, L"%s", s[0] ? s : L"无");
}

/* 保护级别（PPL）：level 位布局 = 签名者 << 4 | 审计位 | 类型（0 无 1 Light 2 Full）。
   受保护进程读不到令牌时，这一行是"为什么读不到"的解释。
   ★ 这个策略号与结构体是 Win10 才进 SDK 的，老 SDK 上自己补一份（枚举值固定 8）。 */
#ifndef PROCESS_MITIGATION_PROTECTION_LEVEL_POLICY
typedef struct _PROCESS_MITIGATION_PROTECTION_LEVEL_POLICY_LOCAL {
    union {
        DWORD Flags;
        struct {
            DWORD ProtectionLevel : 8;
            DWORD ReservedFlags : 24;
        } s;
    } u;
} PROCESS_MITIGATION_PROTECTION_LEVEL_POLICY;
#define ProcessProtectionLevelInfo ((PROCESS_MITIGATION_POLICY)8)
#endif

static void ProcProtectionText(HANDLE hp, WCHAR* out, size_t cap) {
    out[0] = 0;
    if (!hp) return;
    PROCESS_MITIGATION_PROTECTION_LEVEL_POLICY pl;
    memset(&pl, 0, sizeof(pl));
    if (!GetProcessMitigationPolicy(hp, ProcessProtectionLevelInfo, &pl, sizeof(pl))) return;
    DWORD lv = pl.u.Flags & 0xFF;
    if (!lv) { _snwprintf_s(out, cap, _TRUNCATE, L"无（未受保护）"); return; }
    DWORD type = lv & 0x7;
    DWORD signer = (lv >> 4) & 0xF;
    const WCHAR* tn = (type == 1) ? L"Light" : (type == 2) ? L"Full" : L"None";
    const WCHAR* sn = L"未知";
    switch (signer) {
    case 1:  sn = L"Authenticode"; break;
    case 2:  sn = L"CodeGen";      break;
    case 3:  sn = L"Antimalware";  break;
    case 4:  sn = L"LSA";          break;
    case 5:  sn = L"Windows";      break;
    case 6:  sn = L"WinTcb";       break;
    case 7:  sn = L"WinSystem";    break;
    case 8:  sn = L"App";          break;
    default: break;
    }
    _snwprintf_s(out, cap, _TRUNCATE, L"PPL %s·%s", sn, tn);
}

/* 已启用的特权：全列会撑很长，这里给总数 + 前 12 个名字（剩下的用"…"）。
   排查"为什么打不开进程/为什么能调试"时，SeDebugPrivilege 在不在这一眼可见。 */
static void PrivilegeText(HANDLE tok, WCHAR* out, size_t cap) {
    out[0] = 0;
    DWORD sz = 0;
    GetTokenInformation(tok, TokenPrivileges, NULL, 0, &sz);
    if (sz < sizeof(TOKEN_PRIVILEGES) || sz > 64 * 1024) return;
    TOKEN_PRIVILEGES* tp = (TOKEN_PRIVILEGES*)calloc(1, sz);
    if (!tp) return;
    if (GetTokenInformation(tok, TokenPrivileges, tp, sz, &sz)) {
        int shown = 0, total = 0;
        WCHAR list[1200] = L"";
        for (DWORD i = 0; i < tp->PrivilegeCount; i++) {
            if (!(tp->Privileges[i].Attributes & SE_PRIVILEGE_ENABLED)) continue;
            total++;
            if (shown >= 12) continue;
            WCHAR nm[128] = L"";
            DWORD nl = 128;
            if (!LookupPrivilegeNameW(NULL, &tp->Privileges[i].Luid, nm, &nl)) continue;
            if (shown) wcscat_s(list, 1200, L"，");
            wcscat_s(list, 1200, nm);
            shown++;
        }
        if (!total) _snwprintf_s(out, cap, _TRUNCATE, L"无（未启用任何特权）");
        else _snwprintf_s(out, cap, _TRUNCATE, L"%d 项：%s%s", total, list,
                          total > shown ? L"，…" : L"");
    }
    free(tp);
}

/* 映像类型：直接从 exe 文件读 PE 头 —— 子系统（GUI / 控制台 / Native）、EXE 还是
   DLL、是否 .NET。版本资源那条路已经在读同一个文件，这里另开一次只读前 4 KB。 */
static void ImageTypeText(const WCHAR* path, WCHAR* out, size_t cap) {
    out[0] = 0;
    if (!path || !path[0]) return;
    HANDLE f = CreateFileW(path, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    BYTE hdr[4096];
    DWORD got = 0;
    BOOL ok = ReadFile(f, hdr, sizeof(hdr), &got, NULL);
    CloseHandle(f);
    if (!ok || got < sizeof(IMAGE_DOS_HEADER) + sizeof(IMAGE_NT_HEADERS32)) return;

    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)hdr;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return;
    if ((size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS32) > got) return;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(hdr + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    WORD subsystem = 0, chars = nt->FileHeader.Characteristics;
    BOOL dotnet = FALSE;
    if (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC) {
        subsystem = nt->OptionalHeader.Subsystem;
        dotnet = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR].VirtualAddress != 0;
    } else if (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        if ((size_t)dos->e_lfanew + sizeof(IMAGE_NT_HEADERS64) > got) return;
        IMAGE_NT_HEADERS64* nt64 = (IMAGE_NT_HEADERS64*)(hdr + dos->e_lfanew);
        subsystem = nt64->OptionalHeader.Subsystem;
        dotnet = nt64->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR].VirtualAddress != 0;
    } else {
        return;
    }
    const WCHAR* sub = L"未知子系统";
    switch (subsystem) {
    case IMAGE_SUBSYSTEM_WINDOWS_GUI:     sub = L"GUI";      break;
    case IMAGE_SUBSYSTEM_WINDOWS_CUI:     sub = L"控制台";   break;
    case IMAGE_SUBSYSTEM_NATIVE:          sub = L"Native";   break;
    case IMAGE_SUBSYSTEM_EFI_APPLICATION: sub = L"EFI";      break;
    default: break;
    }
    _snwprintf_s(out, cap, _TRUNCATE, L"%s 子系统，%s%s", sub,
                 (chars & IMAGE_FILE_DLL) ? L"DLL" : L"EXE",
                 dotnet ? L"，.NET" : L"");
}

/* FILETIME（100ns）-> 本地时间 "YYYY-MM-DD hh:mm:ss" */
static void FileTimeToLocalText(ULONGLONG ft, WCHAR* out, size_t cap) {
    out[0] = 0;
    if (!ft) return;
    FILETIME f, lf;
    SYSTEMTIME st;
    f.dwLowDateTime = (DWORD)(ft & 0xFFFFFFFFu);
    f.dwHighDateTime = (DWORD)(ft >> 32);
    if (FileTimeToLocalFileTime(&f, &lf) && FileTimeToSystemTime(&lf, &st))
        _snwprintf_s(out, cap, _TRUNCATE, L"%04d-%02d-%02d %02d:%02d:%02d",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

/* 父进程的创建时间：光看父 PID 分不清"同一个父进程"和"父 PID 被复用"，
   (PID, 创建时间) 这一对才是判定父进程的口径。用 NtQuerySystemInformation 一次
   拿整张进程表 —— 比逐个 OpenProcess + GetProcessTimes 便宜得多。
   ★ 结构体自己写前缀：winternl.h 里的 SYSTEM_PROCESS_INFORMATION 视 SDK 版本
   省掉了 CreateTime，而这里只要"前 13 个字段的偏移对得上"就够。 */
typedef struct {
    ULONG         NextEntryOffset;
    ULONG         NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG         HardFaultCount;
    ULONG         NumberOfThreadsHighWatermark;
    ULONGLONG     CycleTime;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    struct { USHORT Length, MaximumLength; PWSTR Buffer; } ImageName;   /* UNICODE_STRING 的布局 */
    LONG          BasePriority;
    HANDLE        UniqueProcessId;
} SysProcPrefix;

static BOOL ProcessCreateTime(DWORD pid, ULONGLONG* out) {
    *out = 0;
    typedef NTSTATUS (NTAPI* PFN_QSI)(ULONG, PVOID, ULONG, PULONG);
    PFN_QSI q = (PFN_QSI)(void*)ModFn(L"ntdll.dll", "NtQuerySystemInformation");
    if (!q) return FALSE;
    ULONG size = 1u << 20;
    BYTE* buf = NULL;
    for (int tries = 0; tries < 6; tries++) {
        buf = (BYTE*)malloc(size);
        if (!buf) return FALSE;
        ULONG need = 0;
        NTSTATUS st = q(5 /*SystemProcessInformation*/, buf, size, &need);
        if (st >= 0) break;
        free(buf);
        buf = NULL;
        if ((ULONG)st != 0xC0000004UL /*STATUS_INFO_LENGTH_MISMATCH*/) return FALSE;
        size = need ? need + 65536 : size * 2;
    }
    if (!buf) return FALSE;
    BOOL found = FALSE;
    BYTE* p = buf;
    for (;;) {
        SysProcPrefix* spi = (SysProcPrefix*)p;
        if ((DWORD)(ULONG_PTR)spi->UniqueProcessId == pid) {
            *out = (ULONGLONG)spi->CreateTime.QuadPart;
            found = TRUE;
            break;
        }
        if (!spi->NextEntryOffset) break;
        p += spi->NextEntryOffset;
    }
    free(buf);
    return found;
}

static void ProcSecurityGet(DWORD pid, ProcSecurity* s) {
    memset(s, 0, sizeof(*s));
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) {
        /* System 进程（PID 4）连打开都不行 —— 单独说清楚 */
        if (pid == 4) _snwprintf_s(s->integrity, 128, _TRUNCATE, L"System（受保护，不可查）");
        return;
    }
    HANDLE tok = NULL;
    if (OpenProcessToken(h, TOKEN_QUERY, &tok)) {
        DWORD sz = 0;
        /* 账户 + SID */
        GetTokenInformation(tok, TokenUser, NULL, 0, &sz);
        if (sz > 0 && sz < 4096) {
            TOKEN_USER* tu = (TOKEN_USER*)calloc(1, sz);
            if (tu && GetTokenInformation(tok, TokenUser, tu, sz, &sz) && tu->User.Sid) {
                WCHAR name[128], domain[128];
                DWORD cn = 128, cd = 128;
                SID_NAME_USE use;
                if (LookupAccountSidW(NULL, tu->User.Sid, name, &cn, domain, &cd, &use)) {
                    if (domain[0])
                        _snwprintf_s(s->account, 196, _TRUNCATE, L"%s\\%s", domain, name);
                    else
                        _snwprintf_s(s->account, 196, _TRUNCATE, L"%s", name);
                }
                SidToText(tu->User.Sid, s->sid, 192);
            }
            free(tu);
        }
        /* 提权：TokenIsElevated 只说"提没提"，配合 TokenElevationType 才说得清
           "管理员组成员但只有受限令牌"和"就是标准用户"的差别。 */
        BOOL elevated = FALSE;
        GetTokenInformation(tok, TokenElevation, NULL, 0, &sz);
        if (sz == sizeof(TOKEN_ELEVATION)) {
            TOKEN_ELEVATION te;
            memset(&te, 0, sizeof(te));
            if (GetTokenInformation(tok, TokenElevation, &te, sizeof(te), &sz))
                elevated = te.TokenIsElevated ? TRUE : FALSE;
        }
        TOKEN_ELEVATION_TYPE et = TokenElevationTypeDefault;
        BOOL hasType = FALSE;
        sz = sizeof(et);
        if (GetTokenInformation(tok, TokenElevationType, &et, sizeof(et), &sz))
            hasType = TRUE;
        /* 完整性级别（顺便给提权文案定口径：SYSTEM 不是"管理员"） */
        DWORD rid = 0;
        GetTokenInformation(tok, TokenIntegrityLevel, NULL, 0, &sz);
        if (sz > 0 && sz < 4096) {
            TOKEN_MANDATORY_LABEL* tml = (TOKEN_MANDATORY_LABEL*)calloc(1, sz);
            if (tml) {
                if (GetTokenInformation(tok, TokenIntegrityLevel, tml, sz, &sz) &&
                    tml->Label.Sid) {
                    DWORD cnt = *GetSidSubAuthorityCount(tml->Label.Sid);
                    rid = (cnt > 0) ? *GetSidSubAuthority(tml->Label.Sid, cnt - 1) : 0;
                    const WCHAR* name = NULL;
                    switch (rid) {
                    case SECURITY_MANDATORY_UNTRUSTED_RID:  name = L"Untrusted"; break;
                    case SECURITY_MANDATORY_LOW_RID:        name = L"Low";       break;
                    case SECURITY_MANDATORY_MEDIUM_RID:     name = L"Medium";    break;
                    case SECURITY_MANDATORY_HIGH_RID:       name = L"High";      break;
                    case SECURITY_MANDATORY_SYSTEM_RID:     name = L"System";    break;
                    default:
                        if (rid >= SECURITY_MANDATORY_PROTECTED_PROCESS_RID) name = L"Protected";
                        break;
                    }
                    /* 非标准档（既不是五档标准 RID 也不是 Protected，例如某些
                       被 AppInfo 抬过完整性的进程）：给出数值，别只写"未知"。 */
                    if (name)
                        _snwprintf_s(s->integrity, 128, _TRUNCATE, L"%s", name);
                    else
                        _snwprintf_s(s->integrity, 128, _TRUNCATE, L"非标准（RID 0x%X）", rid);
                }
                free(tml);
            }
        }
        if (rid >= SECURITY_MANDATORY_SYSTEM_RID)
            _snwprintf_s(s->elevation, 128, _TRUNCATE, L"SYSTEM（系统账户，高于管理员）");
        else if (elevated)
            _snwprintf_s(s->elevation, 128, _TRUNCATE,
                         (hasType && et != TokenElevationTypeFull)
                             ? L"管理员（已提升，UAC 未拆分令牌）"
                             : L"管理员（已提升）");
        else if (hasType && et == TokenElevationTypeLimited)
            _snwprintf_s(s->elevation, 128, _TRUNCATE, L"标准用户（管理员组，未提升）");
        else
            _snwprintf_s(s->elevation, 128, _TRUNCATE, L"标准用户（未提升）");
        /* UIAccess：令牌上的一位，和清单里的 uiAccess="true" 不是一回事 ——
           声明只是申请，AppInfo 校验签名与安装位置通过后才把这一位写进令牌；
           子进程拿到的是父进程那份令牌，所以这一位会连带传给后代。它**不是**
           提权：UIAccess 进程最常见的形态恰恰是"高完整性 + 未提升"，能力是
           绕过前台锁、绕过 UIPI 去驱动高权限窗口。 */
        DWORD uia = 0;
        sz = sizeof(uia);
        if (GetTokenInformation(tok, TokenUIAccess, &uia, sizeof(uia), &sz))
            _snwprintf_s(s->uiaccess, 96, _TRUNCATE,
                         uia ? L"已启用（可绕前台锁与 UIPI；常见高完整性但未提升）"
                             : L"未启用");
        /* 完整性策略：令牌上的 MIC 义务（NO_WRITE_UP = 不能写更高完整性的
           对象）。它说的不是"目标有多高"，而是"这个进程能往多高读写"。 */
        DWORD mp = 0;
        sz = sizeof(mp);
        if (GetTokenInformation(tok, TokenMandatoryPolicy, &mp, sizeof(mp), &sz)) {
            if (mp & 3) {
                const WCHAR* t = L"";
                switch (mp & 3) {
                case 1:  t = L"禁写高（NO_WRITE_UP）"; break;
                case 2:  t = L"禁读高（NO_READ_UP）"; break;
                default: t = L"禁写高 + 禁读高"; break;
                }
                _snwprintf_s(s->mandatory, 96, _TRUNCATE, L"%s", t);
            } else {
                _snwprintf_s(s->mandatory, 96, _TRUNCATE, L"无（读写都能上探）");
            }
        }
        /* 令牌限制：IsRestricted = 现在是受限令牌（服务 / 沙箱那一套），
           HasRestrictions = 历史上被过滤过（UAC 拆分令牌就是这种）。 */
        {
            DWORD hr = 0, ir = 0;
            sz = sizeof(hr);
            BOOL okH = GetTokenInformation(tok, TokenHasRestrictions, &hr, sizeof(hr), &sz);
            sz = sizeof(ir);
            BOOL okI = GetTokenInformation(tok, TokenIsRestricted, &ir, sizeof(ir), &sz);
            if (okI && ir)
                _snwprintf_s(s->restrictions, 128, _TRUNCATE, L"受限令牌（SeRestrictedSids 生效）");
            else if (okH && hr)
                _snwprintf_s(s->restrictions, 128, _TRUNCATE, L"普通（曾被过滤：UAC 拆分令牌）");
            else if (okH || okI)
                _snwprintf_s(s->restrictions, 128, _TRUNCATE, L"普通（未经过滤）");
        }
        /* UAC 虚拟化：Allowed 与 Enabled 是两个位 —— 64 位进程常见"允许但未启用" */
        {
            DWORD va = 0, ve = 0;
            sz = sizeof(va);
            BOOL okA = GetTokenInformation(tok, TokenVirtualizationAllowed, &va, sizeof(va), &sz);
            sz = sizeof(ve);
            BOOL okE = GetTokenInformation(tok, TokenVirtualizationEnabled, &ve, sizeof(ve), &sz);
            if (okA && !va)
                _snwprintf_s(s->virtualized, 96, _TRUNCATE, L"不支持（令牌不允许）");
            else if (okE)
                _snwprintf_s(s->virtualized, 96, _TRUNCATE,
                             ve ? L"已启用（32 位写入会被重定向）" : L"允许但未启用");
        }
        /* 关联令牌：拆分令牌机制下"高权那一半"是否存在。查询失败但错误码是
           NO_SUCH_LOGON_SESSION 就是"没有另一份"，与"读不到"要分开。 */
        {
            TOKEN_LINKED_TOKEN lt;
            memset(&lt, 0, sizeof(lt));
            sz = sizeof(lt);
            if (GetTokenInformation(tok, TokenLinkedToken, &lt, sizeof(lt), &sz))
                _snwprintf_s(s->linked, 96, _TRUNCATE, L"有（拆分令牌的高权那一半）");
            else if (GetLastError() == ERROR_NO_SUCH_LOGON_SESSION)
                _snwprintf_s(s->linked, 96, _TRUNCATE, L"无（单一令牌）");
        }
        /* 容器：AppContainer / 沙箱 / AppSilo / 私有命名空间 —— 四个位都读得到 */
        {
            DWORD ac = 0, sb = 0, silo = 0, pns = 0;
            sz = sizeof(ac);
            BOOL okc = GetTokenInformation(tok, TokenIsAppContainer, &ac, sizeof(ac), &sz);
            sz = sizeof(sb);
            BOOL oks = GetTokenInformation(tok, TokenIsSandboxed, &sb, sizeof(sb), &sz);
            sz = sizeof(silo);
            BOOL okSilo = GetTokenInformation(tok, TokenIsAppSilo, &silo, sizeof(silo), &sz);
            sz = sizeof(pns);
            BOOL okP = GetTokenInformation(tok, TokenPrivateNameSpace, &pns, sizeof(pns), &sz);
            if (okc && !ac && (!okSilo || !silo) && (!okP || !pns))
                _snwprintf_s(s->container, 160, _TRUNCATE, L"无（普通 Win32 进程）");
            else if (okc || okSilo || okP || oks) {
                WCHAR t[160] = L"";
                if (ac)  wcscat_s(t, 160, L"AppContainer");
                if (sb)  wcscat_s(t, 160, t[0] ? L" + 沙箱" : L"沙箱");
                if (silo) wcscat_s(t, 160, t[0] ? L" + AppSilo" : L"AppSilo");
                if (pns) wcscat_s(t, 160, t[0] ? L" + 私有命名空间" : L"私有命名空间");
                _snwprintf_s(s->container, 160, _TRUNCATE, L"%s", t);
            }
        }
        /* 能力清单：容器里"能访问什么"由它决定（Internet 客户端、runFullTrust…）。
           名字要逐个 LookupAccountSid，成本高，这里只给数量。 */
        {
            sz = 0;
            if (GetTokenInformation(tok, TokenCapabilities, NULL, 0, &sz) ||
                GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
                TOKEN_GROUPS* g = sz ? (TOKEN_GROUPS*)calloc(1, sz) : NULL;
                if (g && GetTokenInformation(tok, TokenCapabilities, g, sz, &sz))
                    _snwprintf_s(s->capabilities, 64, _TRUNCATE, L"%lu 项", g->GroupCount);
                else
                    _snwprintf_s(s->capabilities, 64, _TRUNCATE, L"0 项");
                free(g);
            }
        }
        /* 包 SID：容器进程的稳定身份（包名会改名，SID 不会） */
        {
            sz = 0;
            if (GetTokenInformation(tok, TokenAppContainerSid, NULL, 0, &sz) ||
                GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
                TOKEN_APPCONTAINER_INFORMATION* ai =
                    sz ? (TOKEN_APPCONTAINER_INFORMATION*)calloc(1, sz) : NULL;
                if (ai && GetTokenInformation(tok, TokenAppContainerSid, ai, sz, &sz) &&
                    ai->TokenAppContainer) {
                    LPWSTR sid = NULL;
                    if (ConvertSidToStringSidW(ai->TokenAppContainer, &sid) && sid) {
                        _snwprintf_s(s->appsid, 192, _TRUNCATE, L"%s", sid);
                        LocalFree(sid);
                    }
                } else {
                    _snwprintf_s(s->appsid, 192, _TRUNCATE, L"无（非容器进程）");
                }
                free(ai);
            }
        }
        /* 登录会话 SID：同一个 SID 的窗口才互相看得见（跨会话窗口互相不可见） */
        {
            sz = 0;
            if (GetTokenInformation(tok, TokenLogonSid, NULL, 0, &sz) ||
                GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
                TOKEN_GROUPS* g = sz ? (TOKEN_GROUPS*)calloc(1, sz) : NULL;
                if (g && GetTokenInformation(tok, TokenLogonSid, g, sz, &sz) &&
                    g->GroupCount > 0 && g->Groups[0].Sid) {
                    LPWSTR sid = NULL;
                    if (ConvertSidToStringSidW(g->Groups[0].Sid, &sid) && sid) {
                        _snwprintf_s(s->logonsid, 96, _TRUNCATE, L"%s", sid);
                        LocalFree(sid);
                    }
                }
                free(g);
            }
        }
        PrivilegeText(tok, s->privs, 1400);
        CloseHandle(tok);
    }
    ProcMitigationText(h, s->mitigation, 256);
    ProcProtectionText(h, s->protection, 128);
    CloseHandle(h);
}

/* ---------- 进程标志：关键进程 / 在 Job 里 ----------
   两项都要进程句柄，但都不是令牌数据，所以与安全上下文分开：开一次句柄问两遍，
   比各开一次省一半。优先级类放在资源段，用那里已经开好的句柄查。
   打不开进程时逐项写"不可用"——整段消失会让人以为字段没实现。 */
static void ProcFlagsEmit(Buf* b, DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) {
        BufUnavail(b, IF_CRITICAL, L"进程无权限打开");
        BufUnavail(b, IF_INJOB, L"进程无权限打开");
        return;
    }
    EnsureNt();
    WCHAR t[96];
    t[0] = 0;
    if (s_ntQip) {
        /* ProcessBreakOnTermination：置位 = 结束它会直接蓝屏（关键进程） */
        ULONG v = 0, ret = 0;
        if (s_ntQip(h, 29, &v, sizeof(v), &ret) == 0)
            _snwprintf_s(t, 96, _TRUNCATE, v ? L"是（结束它会触发蓝屏）" : L"否");
    }
    if (t[0]) BufField(b, IF_CRITICAL, t);
    else      BufUnavail(b, IF_CRITICAL, L"该信息类不可用");

    BOOL injob = FALSE;
    if (IsProcessInJob(h, NULL, &injob))
        BufField(b, IF_INJOB, injob ? L"是（受 Job 的资源 / UI 限制）" : L"否");
    else
        BufUnavail(b, IF_INJOB, L"IsProcessInJob 失败");
    CloseHandle(h);
}

/* 优先级类：数值一起给，便于比对。REALTIME 还额外要
   SeIncreaseBasePriorityPrivilege，光看类别名看不出来，所以保留十六进制。 */
static void ProcPriorityText(HANDLE h, WCHAR* out, size_t cap) {
    out[0] = 0;
    if (!h) return;
    DWORD pc = GetPriorityClass(h);
    if (!pc) return;
    const WCHAR* n = NULL;
    switch (pc) {
    case IDLE_PRIORITY_CLASS:          n = L"低（Idle）"; break;
    case BELOW_NORMAL_PRIORITY_CLASS:  n = L"低于普通";   break;
    case NORMAL_PRIORITY_CLASS:        n = L"普通";       break;
    case ABOVE_NORMAL_PRIORITY_CLASS:  n = L"高于普通";   break;
    case HIGH_PRIORITY_CLASS:          n = L"高";         break;
    case REALTIME_PRIORITY_CLASS:      n = L"实时";       break;
    default: break;
    }
    if (n) _snwprintf_s(out, cap, _TRUNCATE, L"%s（0x%lX）", n, (unsigned long)pc);
    else   _snwprintf_s(out, cap, _TRUNCATE, L"0x%lX", (unsigned long)pc);
}

/* PE 架构：IsWow64Process2 能区分 ARM64 与 x64；老系统退回 IsWow64Process。 */
static void ProcArch(DWORD pid, WCHAR* out, size_t cap) {
    _snwprintf_s(out, cap, _TRUNCATE, L"未知");
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) { _snwprintf_s(out, cap, _TRUNCATE, L"不可查（受保护）"); return; }

    typedef BOOL (WINAPI* PFN_Wow2)(HANDLE, USHORT*, USHORT*);
    PFN_Wow2 wow2 = (PFN_Wow2)(void*)ModFn(L"kernel32.dll", "IsWow64Process2");
    if (wow2) {
        USHORT proc = 0, host = 0;
        if (wow2(h, &proc, &host)) {
            USHORT mach = (proc == IMAGE_FILE_MACHINE_UNKNOWN) ? host : proc;
            const WCHAR* name = L"未知";
            switch (mach) {
            case IMAGE_FILE_MACHINE_I386:   name = L"x86 (32 位)";  break;
            case IMAGE_FILE_MACHINE_AMD64:  name = L"x64";          break;
            case IMAGE_FILE_MACHINE_ARM64:  name = L"ARM64";        break;
            case IMAGE_FILE_MACHINE_ARM:    name = L"ARM (32 位)";  break;
            default: break;
            }
            _snwprintf_s(out, cap, _TRUNCATE, L"%s", name);
            CloseHandle(h);
            return;
        }
    }
    BOOL wow = FALSE;
    if (IsWow64Process(h, &wow))
        _snwprintf_s(out, cap, _TRUNCATE, L"%s", wow ? L"x86 (32 位)" : L"与系统同位数");
    CloseHandle(h);
}

/* 版本资源：文件版本 / 产品名 / 公司名 */
static void ProcVersion(DWORD pid, const WCHAR* path, WCHAR* ver, size_t vcap,
                        WCHAR* prod, size_t pcap, WCHAR* comp, size_t ccap) {
    ver[0] = prod[0] = comp[0] = 0;
    (void)pid;
    if (!path || !path[0]) return;
    DWORD dummy = 0;
    DWORD sz = GetFileVersionInfoSizeW(path, &dummy);
    if (!sz || sz > (1024 * 1024)) return;
    BYTE* buf = (BYTE*)calloc(1, sz);
    if (!buf) return;
    if (GetFileVersionInfoW(path, 0, sz, buf)) {
        VS_FIXEDFILEINFO* fi = NULL;
        UINT len = 0;
        if (VerQueryValueW(buf, L"\\", (LPVOID*)&fi, &len) && fi && len >= sizeof(*fi)) {
            _snwprintf_s(ver, vcap, _TRUNCATE, L"%u.%u.%u.%u",
                         HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS),
                         HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
        }
        /* 字符串表要按 Translation 里的语言码去取，写死 0409 在中文软件上取空 */
        struct { WORD lang; WORD cp; }* tr = NULL;
        if (VerQueryValueW(buf, L"\\VarFileInfo\\Translation", (LPVOID*)&tr, &len) &&
            tr && len >= sizeof(*tr)) {
            WCHAR key[128];
            _snwprintf_s(key, 128, _TRUNCATE, L"\\StringFileInfo\\%04X%04X\\ProductName",
                         tr->lang, tr->cp);
            WCHAR* s = NULL;
            if (VerQueryValueW(buf, key, (LPVOID*)&s, &len) && s)
                _snwprintf_s(prod, pcap, _TRUNCATE, L"%s", s);
            _snwprintf_s(key, 128, _TRUNCATE, L"\\StringFileInfo\\%04X%04X\\CompanyName",
                         tr->lang, tr->cp);
            s = NULL;
            if (VerQueryValueW(buf, key, (LPVOID*)&s, &len) && s)
                _snwprintf_s(comp, ccap, _TRUNCATE, L"%s", s);
        }
    }
    free(buf);
}

static void ProcStartTime(DWORD pid, WCHAR* out, size_t cap) {
    out[0] = 0;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return;
    FILETIME c, e, k, u;
    if (GetProcessTimes(h, &c, &e, &k, &u)) {
        FILETIME loc;
        SYSTEMTIME st;
        if (FileTimeToLocalFileTime(&c, &loc) && FileTimeToSystemTime(&loc, &st)) {
            _snwprintf_s(out, cap, _TRUNCATE, L"%04u-%02u-%02u %02u:%02u:%02u",
                         st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        }
    }
    CloseHandle(h);
}

/* 父进程：InheritedFromUniqueProcessId（同一个 PBI 调用，代价几乎为零） */
static void ProcParent(DWORD pid, DWORD* outPid) {
    *outPid = 0;
    EnsureNt();
    if (!s_ntQip) return;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return;
    struct {
        LONG ExitStatus; PVOID PebBaseAddress; ULONG_PTR AffinityMask;
        LONG BasePriority; ULONG_PTR UniqueProcessId; ULONG_PTR InheritedFromUniqueProcessId;
    } pbi;
    memset(&pbi, 0, sizeof(pbi));
    ULONG got = 0;
    if (s_ntQip(h, 0, &pbi, sizeof(pbi), &got) == 0)
        *outPid = (DWORD)pbi.InheritedFromUniqueProcessId;
    CloseHandle(h);
}

/* ================= 采集：整屏文本 ================= */

typedef struct TreeCtx {
    Buf*  b;
    int   listed;
    BOOL  hitCap;
} TreeCtx;

static void WalkChildren(TreeCtx* c, HWND parent, int depth) {
    if (depth > MAX_TREE_DEPTH) return;
    for (HWND ch = GetWindow(parent, GW_CHILD); ch; ch = GetWindow(ch, GW_HWNDNEXT)) {
        if (c->listed >= MAX_TREE_NODES) { c->hitCap = TRUE; return; }
        WCHAR cls[128], title[160], hx[32], rect[96];
        RECT r;
        cls[0] = title[0] = 0;
        GetClassNameW(ch, cls, 128);
        GetWinTitle(ch, title, 160);
        Hex(ch, hx, 32);
        if (GetWindowRect(ch, &r))
            _snwprintf_s(rect, 96, _TRUNCATE, L"(%ld,%ld)-(%ld,%ld) %ldx%ld",
                         r.left, r.top, r.right, r.bottom, r.right - r.left, r.bottom - r.top);
        else
            _snwprintf_s(rect, 96, _TRUNCATE, L"(取矩形失败)");

        /* 每条都是「#N: 值」：树的层级放进值里用缩进表示，键列始终从第 0 列
           开始。这样整屏没有一条"无键的值行"，也不会因为缩进把值挤到看不见。 */
        WCHAR indent[2 * MAX_TREE_DEPTH + 1];
        int ni = 0;
        for (int i = 0; i < depth && ni < 2 * MAX_TREE_DEPTH; i++) {
            indent[ni++] = L' ';
            indent[ni++] = L' ';
        }
        indent[ni] = 0;

        WCHAR tit[220];
        if (title[0])
            _snwprintf_s(tit, 220, _TRUNCATE, L"  \"%s%s\"",
                         title, wcslen(title) >= 159 ? L"…" : L"");
        else
            tit[0] = 0;

        WCHAR key[32];
        _snwprintf_s(key, 32, _TRUNCATE, L"#%d", c->listed + 1);
        BufFieldF(c->b, key, L"%s└─ %s  %s%s  %s%s",
                  indent, hx, cls[0] ? cls : L"(无类名)", tit,
                  rect, IsWindowVisible(ch) ? L"" : L"  隐藏");
        c->listed++;
        WalkChildren(c, ch, depth + 1);
        if (c->hitCap) return;
    }
}

static BOOL CALLBACK CountChildCb(HWND ch, LPARAM lp) {
    (void)ch;
    (*(int*)lp)++;
    return TRUE;
}

/* 一次采集的成品：全部视图文本 + 常用视图文本（都由采集线程拼好，UI 线程直接
   换上去即可，切换视图不必重新采集）。两个指针都在堆上，调用方负责 free。 */
typedef struct { WCHAR* all; WCHAR* common; } Collected;

/* 常用视图文本：按用户配置的顺序，从 s_fv（本次采集暂存的字段值）里取。 */
static WCHAR* BuildCommonText(void) {
    Buf c;
    memset(&c, 0, sizeof(c));
    if (!BufReserve(&c, 1024)) return NULL;
    c.p[0] = 0;
    for (int i = 0; i < g_commonN; i++) {
        int id = g_common[i];
        if (id < 0 || id >= IF_COUNT || !s_fvHas[id]) continue;
        BufField(&c, id, s_fvVal[id]);
    }
    if (!c.n) BufAddf(&c, L"（常用视图为空：请在设置里挑选常用字段）\n");
    return c.p;
}

/* 采集一整屏文本（全部 + 常用两份）。返回堆上的 Collected*（调用方 free），
   失败返回 NULL。全程只碰**目标**，不碰我们自己的窗口 —— 所以可以在工作线程上跑。 */
static Collected* Collect(HWND target) {
    Collected* col = (Collected*)calloc(1, sizeof(Collected));
    if (!col) return NULL;
    memset(s_fvHas, 0, sizeof(s_fvHas));

    Buf b;
    memset(&b, 0, sizeof(b));
    if (!BufReserve(&b, 8192)) { free(col); return NULL; }
    b.p[0] = 0;

    if (!IsWindow(target)) {
        BufAddf(&b, L"目标窗口已销毁（0x%llX）\n",
                (unsigned long long)(ULONG_PTR)target);
        col->all = b.p;
        col->common = BuildCommonText();
        return col;
    }

    DWORD pid = 0;
    DWORD tid = GetWindowThreadProcessId(target, &pid);
    DWORD hostPid = pid;
    BOOL  uwpUnresolved = FALSE;

    /* UWP：窗口可能属于 ApplicationFrameHost.exe，而用户要看的是窗口里那个
       应用自己。判定与 C+F4 一致（src/folder.c 的 FolderProcessPathOf）：先按
       窗口的 PID 取映像路径，命中宿主名再往下找第一个 CoreWindow 的 PID。
       找不到就保留宿主 PID —— 信息窗不像打开文件夹那样可以「宁可不动作」，
       宿主本身在这里也是有效数据。 */
    {
        WCHAR probe[PATH_CAP];
        if (ProcPath(pid, probe, PATH_CAP)) {
            const WCHAR* base = probe;
            for (const WCHAR* q = probe; *q; q++) if (*q == L'\\') base = q + 1;
            if (_wcsicmp(base, L"ApplicationFrameHost.exe") == 0) {
                CoreFind cf;
                memset(&cf, 0, sizeof(cf));
                HWND top = GetAncestor(target, GA_ROOT);
                EnumChildWindows(top ? top : target, CorePidCb, (LPARAM)&cf);
                if (cf.pid && cf.pid != pid) pid = cf.pid;
                else uwpUnresolved = TRUE;
            }
        }
    }

    /* ---- 窗口层 ----
       排序按"先看什么"分四组：身份（标题/类名/句柄/状态）→ 位置尺寸 → 窗口关系
       → 原始样式位。最技术、最少看的关系句柄与样式位压到最后，一眼先看到这是
       哪个窗口、在不在前台、多大；组与组之间空一行便于扫读。 */
    BufSection(&b, L"窗口");
    WCHAR title[512], cls[128], hx[32];
    WCHAR aumid[512], pkgname[512];
    aumid[0] = 0;
    pkgname[0] = 0;
    GetWinTitle(target, title, 512);
    cls[0] = 0;
    GetClassNameW(target, cls, 128);
    Hex(target, hx, 32);

    /* 身份 */
    BufField(&b, IF_TITLE, title[0] ? title : L"(无标题)");
    BufFieldF(&b, IF_TITLELEN, L"%d 个字符%s", (int)wcslen(title),
              wcslen(title) >= 511 ? L"（已截断）" : L"");
    BufField(&b, IF_CLASS, cls[0] ? cls : L"(无)");
    /* Unicode 窗口与否决定 SendMessage 要不要走 A/W 转换（跨进程发自定义消息
       踩到乱码时，第一眼看的就是它）。 */
    BufField(&b, IF_UNICODE, IsWindowUnicode(target) ? L"是" : L"否（ANSI 窗口）");
    BufFieldF(&b, IF_HANDLE, L"%s（%llu）", hx, (unsigned long long)(ULONG_PTR)target);
    /* 句柄的**唯一性序号**：USER 句柄被回收后会复用同一个数值，高 15 位这个
       计数器能把"同一个窗口"和"复用了句柄的新窗口"分开。 */
    BufFieldF(&b, IF_WNDUID, L"0x%04X", (unsigned)((((ULONG_PTR)target) >> 16) & 0x7FFF));
    int ctlId = GetDlgCtrlID(target);
    BufFieldF(&b, IF_WNDID, L"%d（0x%X）%s", ctlId, ctlId, ctlId ? L"" : L"  （0 = 未设 ID）");
    /* 这里**不列"窗口过程地址"**：`GetWindowLongPtrW(hwnd, GWLP_WNDPROC)` 对别的
       进程的窗口自 Vista 起恒返回 0（系统不再把对方的代码地址暴露给外部进程）。
       留一行永远是 0x0 的字段只会让人以为取数坏了；要判断"是哪个窗口类"，
       类名（上一行）才是可用的那个信息。 */
    WCHAR st[256];
    _snwprintf_s(st, 256, _TRUNCATE, L"%s%s%s%s",
                 IsWindowVisible(target) ? L"可见" : L"隐藏",
                 IsWindowEnabled(target) ? L"" : L" / 已禁用",
                 IsIconic(target) ? L" / 最小化" : (IsZoomed(target) ? L" / 最大化" : L""),
                 GetForegroundWindow() == target ? L" / 前台" : L"");
    BufField(&b, IF_STATE, st);
    {
        WCHAR cloak[64];
        CloakedText(target, cloak, 64);
        if (cloak[0]) BufField(&b, IF_CLOAKED, cloak);
    }
    /* 截图保护（显示亲和性）：这一位决定"截图 / 录屏里能不能看见它"。
       它是**可读的**（GetWindowDisplayAffinity），不需要特殊权限；黑屏截图的
       第一大原因就是 WDA_EXCLUDEFROMCAPTURE / WDA_MONITOR。 */
    {
        DWORD aff = 0;
        if (GetWindowDisplayAffinity(target, &aff)) {
            if (aff == WDA_NONE)
                BufField(&b, IF_AFFINITY, L"无（可被截图 / 录屏）");
            else if (aff == WDA_MONITOR)
                BufField(&b, IF_AFFINITY, L"仅显示器可见（截图里是黑块）");
            else if (aff == WDA_EXCLUDEFROMCAPTURE)
                BufField(&b, IF_AFFINITY, L"已排除（截图 / 录屏里不出现该窗口）");
            else
                BufFieldF(&b, IF_AFFINITY, L"0x%lX（未知取值）", (unsigned long)aff);
        } else {
            BufUnavail(&b, IF_AFFINITY, L"GetWindowDisplayAffinity 失败");
        }
    }

    /* 位置与尺寸 */
    BufBlankLine(&b);
    RECT wr, cr;
    if (GetWindowRect(target, &wr))
        BufFieldF(&b, IF_RECT, L"(%ld,%ld)-(%ld,%ld)  %ldx%ld",
                  wr.left, wr.top, wr.right, wr.bottom, wr.right - wr.left, wr.bottom - wr.top);
    else
        BufUnavail(&b, IF_RECT, L"GetWindowRect 失败");
    {
        /* 最大化/最小化时上面那行是"当前"，这行是"还原后"——贴边/记忆位置的判断
           都发生在还原矩形上。 */
        WCHAR nr[128];
        NormalRectText(target, nr, 128);
        if (!nr[0]) BufUnavail(&b, IF_NORMALRECT, L"GetWindowPlacement 失败");
        else if (wr.right > wr.left && wr.bottom > wr.top &&
                 !IsIconic(target) && !IsZoomed(target))
            BufFieldF(&b, IF_NORMALRECT, L"%s  （同窗口位置）", nr);
        else
            BufField(&b, IF_NORMALRECT, nr);
    }
    if (GetClientRect(target, &cr)) {
        POINT tl = { cr.left, cr.top }, br = { cr.right, cr.bottom };
        ClientToScreen(target, &tl);
        ClientToScreen(target, &br);
        BufFieldF(&b, IF_CLIENT, L"(%ld,%ld)-(%ld,%ld)  %ldx%ld",
                  tl.x, tl.y, br.x, br.y, cr.right - cr.left, cr.bottom - cr.top);
    } else {
        BufUnavail(&b, IF_CLIENT, L"GetClientRect 失败");
    }
    HMONITOR mon = MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST);
    if (mon) {
        /* 要设备名（形如 \.\DISPLAY1）就必须用 EX 版：MONITORINFO 没有 szDevice 字段 */
        MONITORINFOEXW mi;
        memset(&mi, 0, sizeof(mi));
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi))
            BufFieldF(&b, IF_MONITOR, L"%s  工作区 (%ld,%ld)-(%ld,%ld)",
                      mi.szDevice, mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom);
    }
    typedef UINT (WINAPI* PFN_GetDpiForWindow)(HWND);
    PFN_GetDpiForWindow gdfw = (PFN_GetDpiForWindow)(void*)ModFn(L"user32.dll", "GetDpiForWindow");
    UINT wdpi = gdfw ? gdfw(target) : 0;
    if (!wdpi) wdpi = 96;
    BufFieldF(&b, IF_DPI, L"%u（%d%%）", wdpi, MulDiv((int)wdpi, 100, 96));
    {
        WCHAR aware[96];
        DpiAwarenessText(target, aware, 96);
        if (aware[0]) BufField(&b, IF_DPICTX, aware);
    }

    /* 窗口关系 + 线程 */
    BufBlankLine(&b);
    BufHandleField(&b, IF_PARENT, GetParent(target));
    BufHandleField(&b, IF_OWNER, GetWindow(target, GW_OWNER));
    BufHandleField(&b, IF_ROOT, GetAncestor(target, GA_ROOT));
    BufHandleField(&b, IF_ROOTOWNER, GetAncestor(target, GA_ROOTOWNER));
    BufField(&b, IF_TOPLEVEL,
             GetAncestor(target, GA_ROOT) == target ? L"是" : L"否（嵌在别的窗口里）");
    BufFieldF(&b, IF_TID, L"0x%lX（%lu）%s", tid, tid,
              IsHungAppWindow(target) ? L"  ** 无响应 **" : L"");

    /* 内部字段（调试向）：一次 user32 调用就能拿到，排查"这窗口是谁建的、
       有没有设字体、是不是挂在某个 IME/菜单上"时省一趟 Spy++。 */
    BufBlankLine(&b);
    {
        HMENU mn = GetMenu(target);
        if (mn) BufFieldF(&b, IF_MENU, L"0x%llX", (unsigned long long)(ULONG_PTR)mn);
        else    BufField(&b, IF_MENU, L"无");
        HINSTANCE hi = (HINSTANCE)GetWindowLongPtrW(target, GWLP_HINSTANCE);
        if (hi) BufFieldF(&b, IF_HINSTANCE, L"0x%llX", (unsigned long long)(ULONG_PTR)hi);
        ULONG_PTR wndproc = (ULONG_PTR)GetClassLongPtrW(target, GCLP_WNDPROC);
        if (wndproc) BufFieldF(&b, IF_WNDPROC, L"0x%llX（目标进程内的地址，仅供比对）",
                               (unsigned long long)wndproc);
        else         BufField(&b, IF_WNDPROC, L"取不到（跨进程不暴露）");
        ULONG_PTR ud = (ULONG_PTR)GetWindowLongPtrW(target, GWLP_USERDATA);
        if (ud) BufFieldF(&b, IF_USERDATA, L"0x%llX", (unsigned long long)ud);
        else    BufField(&b, IF_USERDATA, L"空（0）");
        DWORD extra = (DWORD)GetClassLongPtrW(target, GCL_CBWNDEXTRA);
        BufFieldF(&b, IF_WNDEXTRA, L"%lu 字节", (unsigned long)extra);
        ATOM atom = (ATOM)GetClassLongPtrW(target, GCW_ATOM);
        if (atom) BufFieldF(&b, IF_CLASSATOM, L"0x%04X（系统注册类）", (unsigned)atom);
        else      BufField(&b, IF_CLASSATOM, L"0（动态注册，无原子）");
        BufFieldF(&b, IF_CLASSSTYLE, L"0x%08lX", (unsigned long)GetClassLongPtrW(target, GCL_STYLE));
        WCHAR fnt[256];
        FontText(target, fnt, 256);
        BufField(&b, IF_WNDFONT, fnt[0] ? fnt : L"取不到");
        /* AppUserModelID 是**进程**级的（任务栏按它分组），一次取好留给下面
           「包名（UWP）」那行复用，不重复开进程句柄。 */
        PackageText(pid, aumid, 512, pkgname, 512);
        BufField(&b, IF_APPID, aumid[0] ? aumid : L"无（未声明）");
        HWND ime = NULL;
        typedef HWND (WINAPI* PFN_IMM)(HWND);
        PFN_IMM fi = (PFN_IMM)(void*)ModFn(L"imm32.dll", "ImmGetDefaultIMEWnd");
        if (fi) ime = fi(target);
        if (ime) BufFieldF(&b, IF_IMEWND, L"0x%llX", (unsigned long long)(ULONG_PTR)ime);
        else     BufField(&b, IF_IMEWND, L"无（尚未创建）");
    }

    /* 原始样式位（最技术，放最后）。数值与符号表各占**带键名的一行**：符号若
       另起一行只缩进到值列，那一行没有键名，扫一眼分不出是续值还是新字段；
       值仍是单行，超宽靠横向滚动看全。 */
    BufBlankLine(&b);
    DWORD style = (DWORD)GetWindowLongPtrW(target, GWL_STYLE);
    DWORD exStyle = (DWORD)GetWindowLongPtrW(target, GWL_EXSTYLE);
    WCHAR sym[512];
    BufFieldF(&b, IF_STYLE, L"0x%08lX", style);
    StyleText(style, kStyle, (int)(sizeof(kStyle) / sizeof(kStyle[0])), sym, 512);
    BufField(&b, IF_STYLESYM, sym);
    BufFieldF(&b, IF_EXSTYLE, L"0x%08lX", exStyle);
    StyleText(exStyle, kExStyle, (int)(sizeof(kExStyle) / sizeof(kExStyle[0])), sym, 512);
    BufField(&b, IF_EXSTYLESYM, sym);

    int total = 0;
    EnumChildWindows(target, CountChildCb, (LPARAM)&total);

    /* ---- 进程层 ----
       同样按"先看什么"分四组：身份（进程名/PID/路径/版本）→ 运行（架构/命令行/
       目录/启动/父进程）→ 安全上下文（账户/完整性/运行权限/会话）→ 资源（句柄/内存）。 */
    BufSection(&b, L"进程");
    if (hostPid != pid)
        BufFieldF(&b, IF_HOSTNOTE, L"ApplicationFrameHost（PID %lu）；以下为窗口内 UWP 应用本体", hostPid);
    else if (uwpUnresolved)
        BufField(&b, L"提示", L"这是 UWP 宿主窗口，其下没有 CoreWindow 子窗口可换算；以下为宿主进程本身");
    WCHAR path[PATH_CAP];
    if (!ProcPath(pid, path, PATH_CAP)) path[0] = 0;
    const WCHAR* base = path;
    for (const WCHAR* q = path; *q; q++) if (*q == L'\\') base = q + 1;
    WCHAR tmp[PATH_CAP];

    /* 身份 */
    BufField(&b, IF_PROCNAME, path[0] ? base : L"(取不到)");
    BufFieldF(&b, IF_PID, L"%lu（0x%lX）", pid, pid);
    if (path[0]) BufField(&b, IF_IMAGE, path);
    else BufUnavail(&b, IF_IMAGE, L"进程无权限打开或已退出");
    WCHAR ver[64], prod[128], comp[128];
    ProcVersion(pid, path, ver, 64, prod, 128, comp, 128);
    if (ver[0])  BufField(&b, IF_VER, ver);
    if (prod[0]) BufField(&b, IF_PRODUCT, prod);
    if (comp[0]) BufField(&b, IF_COMPANY, comp);
    {
        WCHAR it[128];
        ImageTypeText(path, it, 128);
        if (it[0]) BufField(&b, IF_IMAGETYPE, it);
        else       BufUnavail(&b, IF_IMAGETYPE, L"读不到映像文件头");
    }

    /* 运行 */
    BufBlankLine(&b);
    ProcArch(pid, tmp, 128);
    BufField(&b, IF_ARCH, tmp);
    if (ProcCommandLine(pid, tmp, PATH_CAP)) BufField(&b, IF_CMDLINE, tmp);
    else BufUnavail(&b, IF_CMDLINE, L"该系统不支持该信息类，或进程无权限");
    if (ProcCwd(pid, tmp, PATH_CAP)) BufField(&b, IF_CWD, tmp);
    else BufUnavail(&b, IF_CWD, L"32 位目标或读取被拒");
    ProcStartTime(pid, tmp, 64);
    if (tmp[0]) BufField(&b, IF_START, tmp);
    else BufUnavail(&b, IF_START, L"进程无权限打开");
    DWORD ppid = 0;
    ProcParent(pid, &ppid);
    if (ppid) {
        WCHAR ppath[PATH_CAP];
        ppath[0] = 0;
        ProcPath(ppid, ppath, PATH_CAP);
        const WCHAR* pb = ppath;
        for (const WCHAR* q = ppath; *q; q++) if (*q == L'\\') pb = q + 1;
        BufFieldF(&b, IF_PPROC, L"%s（PID %lu）", ppath[0] ? pb : L"(取不到进程名)", ppid);
        /* 父进程的创建时间：只有 (PID, 创建时间) 这一对才能排除"父 PID 被复用"
           —— 父进程早就退了、PID 被新进程接手时，只给 PID 会认错爹。 */
        ULONGLONG pct = 0;
        WCHAR pt[64];
        if (ProcessCreateTime(ppid, &pct) && pct) {
            FileTimeToLocalText(pct, pt, 64);
            BufFieldF(&b, IF_PPROCTIME, L"%s%s", pt,
                      ppid == 4 ? L"（System，启动最早）" : L"");
        } else {
            BufUnavail(&b, IF_PPROCTIME, L"父进程已退出（PID 可能被复用）");
        }
    } else {
        BufUnavail(&b, IF_PPROC, L"进程无权限打开");
    }
    ProcFlagsEmit(&b, pid);

    /* 安全上下文（同源于一次 OpenProcess + 一次 OpenProcessToken）。
       ★ 这个结构体带 1.4 KB 的特权清单，放栈上会把采集线程的栈用量顶过
       /analyze 的 16 KB 线（实测 20 KB），所以放堆上。 */
    BufBlankLine(&b);
    {
        ProcSecurity* sec = (ProcSecurity*)calloc(1, sizeof(ProcSecurity));
        if (sec) {
            ProcSecurityGet(pid, sec);
            if (sec->account[0]) BufField(&b, IF_ACCOUNT, sec->account);
            else                 BufUnavail(&b, IF_ACCOUNT, L"令牌不可读");
            if (sec->sid[0])     BufField(&b, IF_USERSID, sec->sid);
            else                 BufUnavail(&b, IF_USERSID, L"令牌不可读");
            if (sec->integrity[0]) BufField(&b, IF_INTEGRITY, sec->integrity);
            else                   BufUnavail(&b, IF_INTEGRITY, L"令牌不可读");
            if (sec->elevation[0]) BufField(&b, IF_ELEVATION, sec->elevation);
            else                   BufUnavail(&b, IF_ELEVATION, L"令牌不可读");
            if (sec->uiaccess[0]) BufField(&b, IF_UIAACCESS, sec->uiaccess);
            else                  BufUnavail(&b, IF_UIAACCESS, L"令牌不可读");
            if (sec->mandatory[0]) BufField(&b, IF_MANDATORY, sec->mandatory);
            else                   BufUnavail(&b, IF_MANDATORY, L"令牌不可读");
            if (sec->restrictions[0]) BufField(&b, IF_RESTRICTIONS, sec->restrictions);
            else                      BufUnavail(&b, IF_RESTRICTIONS, L"令牌不可读");
            if (sec->virtualized[0]) BufField(&b, IF_VIRTUALIZED, sec->virtualized);
            else                     BufUnavail(&b, IF_VIRTUALIZED, L"令牌不可读");
            if (sec->linked[0]) BufField(&b, IF_LINKED, sec->linked);
            else                BufUnavail(&b, IF_LINKED, L"令牌不可读");
            if (sec->container[0]) BufField(&b, IF_CONTAINER, sec->container);
            else                   BufUnavail(&b, IF_CONTAINER, L"令牌不可读");
            if (sec->capabilities[0]) BufField(&b, IF_CAPABILITIES, sec->capabilities);
            else                      BufUnavail(&b, IF_CAPABILITIES, L"令牌不可读");
            if (sec->appsid[0]) BufField(&b, IF_APPSID, sec->appsid);
            else                BufUnavail(&b, IF_APPSID, L"令牌不可读");
            if (sec->logonsid[0]) BufField(&b, IF_LOGONSID, sec->logonsid);
            else                  BufUnavail(&b, IF_LOGONSID, L"令牌不可读");
            if (sec->protection[0]) BufField(&b, IF_PROTECTION, sec->protection);
            else                    BufUnavail(&b, IF_PROTECTION, L"进程句柄不可读");
            if (sec->mitigation[0]) BufField(&b, IF_MITIGATION, sec->mitigation);
            else                    BufUnavail(&b, IF_MITIGATION, L"进程句柄不可读");
            if (sec->privs[0])      BufField(&b, IF_PRIVS, sec->privs);
            else                    BufUnavail(&b, IF_PRIVS, L"令牌不可读");
            free(sec);
        }
        /* 包名放在安全上下文末尾：只有 UWP / MSIX 进程才有，普通 Win32 是空的 */
        if (pkgname[0])        BufField(&b, IF_PKGNAME, pkgname);
        else                   BufField(&b, IF_PKGNAME, L"无（非打包进程）");
    }
    DWORD sess = 0;
    if (ProcessIdToSessionId(pid, &sess)) BufFieldF(&b, IF_SESSION, L"%lu", sess);

    /* 资源（可能整组取不到：受保护进程 OpenProcess 就被拒。整组为空时
       BufBlankLine 的幂等性保证不会在下一段段头前叠出两个空行）。 */
    BufBlankLine(&b);
    HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hp) {
        DWORD handles = 0;
        if (GetProcessHandleCount(hp, &handles)) BufFieldF(&b, IF_HANDLES, L"%lu", handles);
        typedef BOOL (WINAPI* PFN_Mem)(HANDLE, PVOID, DWORD);
        PFN_Mem gm = (PFN_Mem)(void*)ModFn(L"kernel32.dll", "K32GetProcessMemoryInfo");
        if (gm) {
            /* PROCESS_MEMORY_COUNTERS（psapi.h 里的结构；只用前几个字段，自己写一份
               免得为了一次查询去 include psapi.h 并链接 psapi.lib） */
            struct {
                DWORD cb; DWORD PageFaultCount; SIZE_T PeakWorkingSetSize;
                SIZE_T WorkingSetSize; SIZE_T QuotaPeakPagedPoolUsage;
                SIZE_T QuotaPagedPoolUsage; SIZE_T QuotaPeakNonPagedPoolUsage;
                SIZE_T QuotaNonPagedPoolUsage; SIZE_T PagefileUsage; SIZE_T PeakPagefileUsage;
            } pmc;
            memset(&pmc, 0, sizeof(pmc));
            pmc.cb = sizeof(pmc);
            if (gm(hp, &pmc, sizeof(pmc)))
                BufFieldF(&b, IF_WORKSET, L"%.1f MB", (double)pmc.WorkingSetSize / (1024.0 * 1024.0));
        }
        {   /* 优先级类：顺手用这里已经开好的句柄问，不必再开一次 */
            WCHAR pr[64];
            ProcPriorityText(hp, pr, 64);
            if (pr[0]) BufField(&b, IF_PRIORITY, pr);
            else       BufUnavail(&b, IF_PRIORITY, L"GetPriorityClass 失败");
        }
        CloseHandle(hp);
    }

    /* ---- 子窗口树 ----
       合计放在段头之后、树之前：先给结论再给细节。合计用一次额外的
       EnumChildWindows 只数不取文本（不读标题、不取矩形，代价很低），
       因为递归遍历会在达到上限时提前收工，自己数不出真实总数。 */
    BufSection(&b, L"子窗口");
    BufFieldF(&b, L"合计", L"%d 个（以下列出至多 %d 个，深度上限 %d 层）",
              total, MAX_TREE_NODES, MAX_TREE_DEPTH);
    BufAddf(&b, L"\n");
    TreeCtx tc;
    memset(&tc, 0, sizeof(tc));
    tc.b = &b;
    if (target) WalkChildren(&tc, target, 0);
    if (!tc.listed) BufField(&b, L"结果", L"（无子窗口）");
    else if (tc.hitCap || tc.listed < total)
        BufFieldF(&b, L"截断", L"是（实际 %d 个，这里只列出 %d 个）", total, tc.listed);

    col->all = b.p;
    col->common = BuildCommonText();
    return col;
}

/* ================= 工作线程 ================= */

typedef struct { HWND notify; HWND target; } Job;

static DWORD WINAPI CollectThread(LPVOID p) {
    Job* j = (Job*)p;
    Collected* col = Collect(j->target);
    HWND n = j->notify;
    free(j);
    if (!col) {
        col = (Collected*)calloc(1, sizeof(Collected));
        if (col) {
            static const WCHAR kMsg[] = L"（采集失败：内存不足）\n";
            size_t cn = wcslen(kMsg) + 1;
            col->all    = (WCHAR*)calloc(cn, sizeof(WCHAR));
            col->common = (WCHAR*)calloc(cn, sizeof(WCHAR));
            if (col->all)    wcscpy_s(col->all, cn, kMsg);
            if (col->common) wcscpy_s(col->common, cn, kMsg);
        }
    }
    if (col && !PostMessageW(n, WM_APP_COLLECTED, 0, (LPARAM)col)) {
        free(col->all); free(col->common); free(col);
    }
    return 0;
}

/* ================= UI ================= */

static void SetEditText(const WCHAR* s) {
    SetWindowTextW(g_edit, s ? s : L"");
    SendMessageW(g_edit, EM_SETSEL, 0, 0);
    SendMessageW(g_edit, EM_SCROLLCARET, 0, 0);
    SendMessageW(g_edit, EM_SETMODIFY, FALSE, 0);
}

static BOOL ConfirmDiscard(const WCHAR* what) {
    if (!SendMessageW(g_edit, EM_GETMODIFY, 0, 0)) return TRUE;
    WCHAR msg[512];
    _snwprintf_s(msg, 512, _TRUNCATE,
                 L"%s将丢弃你在文本框里的修改。\n\n继续吗？", what);
    return MessageBoxW(g_hwnd, msg, APP_TITLE, MB_OKCANCEL | MB_ICONWARNING) == IDOK;
}

static void StartCollect(void) {
    if (g_busy) return;
    if (!g_target || !IsWindow(g_target)) {
        WCHAR m[128];
        _snwprintf_s(m, 128, _TRUNCATE, L"目标窗口已销毁（0x%llX）\n",
                     (unsigned long long)(ULONG_PTR)g_target);
        SetEditText(m);
        return;
    }
    Job* j = (Job*)calloc(1, sizeof(Job));
    if (!j) return;
    j->notify = g_hwnd;
    j->target = g_target;
    g_busy = TRUE;
    SetEditText(L"正在收集…\n");
    HANDLE t = CreateThread(NULL, 0, CollectThread, j, 0, NULL);
    if (!t) {
        g_busy = FALSE;
        free(j);
        SetEditText(L"（无法创建采集线程）\n");
        return;
    }
    CloseHandle(t);
}

static void Refresh(void) {
    if (g_busy) return;
    if (!ConfirmDiscard(L"刷新")) return;
    StartCollect();
}

/* ---------- 主题：跟随系统深 / 浅色 ----------
   系统"应用主题"读 HKCU 的 Personalize\AppsUseLightTheme（0 = 深色）。RegGetValueW
   一步读完整个值（不必先 OpenKey 再 QueryValue），既是更短的路径，也避开了
   /analyze 对 RegOpenKeyExW 第三参数的一条 C6553 误报。键取不到就按浅色（系统
   默认）；深浅色只影响观感、不影响任何采集逻辑。 */
static BOOL SystemUsesDark(void) {
    DWORD val = 1, sz = sizeof(val);
    LONG r = RegGetValueW(HKEY_CURRENT_USER,
                          L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                          L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &val, &sz);
    return r == ERROR_SUCCESS && val == 0;
}

/* 编辑区与窗口留白区的配色。深色取编辑器常用的那套（底 #1E1E1E、字 #D4D4D4、
   留白区 #252526 略浅一档把编辑区"托"出来）；浅色取 Win11 浅表面（底白、字近黑、
   留白区 #F3F3F3）。两套都是高对比、久看不累，且与系统标题栏的深浅一致。 */
static COLORREF EditBg(void) { return g_dark ? RGB(30, 30, 30)    : RGB(255, 255, 255); }
static COLORREF EditFg(void) { return g_dark ? RGB(212, 212, 212) : RGB(27, 27, 27); }
static COLORREF WinBg(void)  { return g_dark ? RGB(37, 37, 38)    : RGB(243, 243, 243); }

/* 把当前主题铺到窗口与编辑控件上。建窗后调一次；系统主题变了（WM_SETTINGCHANGE
   的 ImmersiveColorSet）再调一次。 */
static void ApplyTheme(void) {
    g_dark = SystemUsesDark();

    /* 标题栏：深色系统下让 DWM 画深色标题栏，浅色系统传 FALSE 回到浅色。 */
    BOOL darkAttr = g_dark;
    DwmSetWindowAttribute(g_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkAttr, sizeof(darkAttr));

    /* 滚动条：comctl32 v6 清单（info.manifest）已启用视觉样式，这里再指定资源
       管理器那套现代细滚动条的主题段（深色用 DarkMode_Explorer）。SetWindowTheme
       是文档化 API；主题段名万一取不到，控件退回默认主题滚动条，仍是现代样式而
       不是经典灰条。 */
    SetWindowTheme(g_edit, g_dark ? L"DarkMode_Explorer" : L"Explorer", NULL);

    /* 编辑区底色 + 字色。字色同时写进**默认**字符格式（SCF_DEFAULT），之后
       SetWindowText 灌进来的新文本自动带这个颜色，不必每次采集后重刷。 */
    SendMessageW(g_edit, EM_SETBKGNDCOLOR, 0, (LPARAM)EditBg());
    CHARFORMAT2W cf;
    memset(&cf, 0, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_COLOR;
    cf.crTextColor = EditFg();
    SendMessageW(g_edit, EM_SETCHARFORMAT, SCF_DEFAULT, (LPARAM)&cf);
    SendMessageW(g_edit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);

    /* 窗口留白区（编辑控件四周那一圈）的底色刷，WM_ERASEBKGND 用它填。 */
    if (g_bgBrush) DeleteObject(g_bgBrush);
    g_bgBrush = CreateSolidBrush(WinBg());
    if (g_editBrush) DeleteObject(g_editBrush);
    g_editBrush = CreateSolidBrush(EditBg());
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, TRUE);
}

/* ---------- 字体 ---------- */

static void ApplyEditFont(void) {
    if (g_editFont) DeleteObject(g_editFont);
    g_editFont = MakeFont(g_face, g_pt, g_dpi, FALSE);
    if (!g_editFont) return;
    SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_editFont, TRUE);

    /* RichEdit 的字符格式要走 CHARFORMAT2：yHeight 的单位是 twip（1/20 磅），
       与 DPI 无关 —— 换显示器时只需重发一次，字号仍是用户设的磅值。 */
    CHARFORMAT2W cf;
    memset(&cf, 0, sizeof(cf));
    cf.cbSize = sizeof(cf);
    cf.dwMask = CFM_FACE | CFM_SIZE | CFM_CHARSET;
    cf.yHeight = (LONG)(g_pt * 20);
    cf.bCharSet = DEFAULT_CHARSET;
    _snwprintf_s(cf.szFaceName, LF_FACESIZE, _TRUNCATE, L"%s", g_face);
    SendMessageW(g_edit, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);

    /* 制表位：值列固定在 LABEL_COL 个半角处。EM_SETTABSTOPS 的单位是"对话框
       模板单位"（1/4 平均字符宽），所以 LABEL_COL 个半角 = LABEL_COL*4 个 DU。
       换字体/字号后要重发一次（DU 随字体走，列位置才会跟着缩放）。 */
    LONG tab = LABEL_COL * 4;
    SendMessageW(g_edit, EM_SETTABSTOPS, 1, (LPARAM)&tab);

    /* 一行的高度（像素）：滚轮一格滚几行、横向一格滚几"行宽"都按它折算。 */
    HDC dc = GetDC(g_edit);
    if (dc) {
        HGDIOBJ old = SelectObject(dc, g_editFont);
        TEXTMETRICW tm;
        if (GetTextMetricsW(dc, &tm)) g_lineH = tm.tmHeight + tm.tmExternalLeading;
        SelectObject(dc, old);
        ReleaseDC(g_edit, dc);
    }
    if (g_lineH < 8) g_lineH = MulDiv(16, g_dpi, 96);
}

/* 筛选框 / 按钮 / 状态文字用的界面字体（不用编辑器那套等宽字体）。 */
static void ApplyUiFont(void) {
    if (g_uiFont) DeleteObject(g_uiFont);
    g_uiFont = MakeFont(L"Microsoft YaHei UI", 9, g_dpi, FALSE);
    if (!g_uiFont) return;
    HWND ctls[3] = { g_search, g_btnCommon, g_btnAll };
    for (int i = 0; i < 3; i++)
        if (ctls[i]) SendMessageW(ctls[i], WM_SETFONT, (WPARAM)g_uiFont, TRUE);
}

/* ---------- 段头样式 / 筛选 / 视图切换 ---------- */

static int g_matches;            /* 最近一次筛选命中的行数（写进标题） */

/* 给 `[段名]` 行加粗 + 强调色，让分段一眼可辨。逐行用**控件的行索引**取行文本：
   早先的写法是把 WM_GETTEXT 的整篇按 '\r' 切分、再拿字符偏移去选段 —— 那偏移
   是错的：GetWindowText 把段落分隔符扩成 CRLF（两个字符），而编辑控件内部的
   cp 里一个段落只占一个，于是第一行之后每个段头都被推后几十个字符，选中的
   其实是别的行，只有最前面那个段头真染上了色。EM_LINEINDEX / EM_LINELENGTH
   用的就是控件自己的坐标，不存在这个换算。
   改格式会把缓冲标脏，所以前后保存/恢复 EM_GETMODIFY —— 用户没编辑过就仍是
   "未修改"，否则右键刷新会平白弹确认。 */
static void StyleHeaders(void) {
    if (!g_edit) return;
    BOOL wasDirty = (BOOL)SendMessageW(g_edit, EM_GETMODIFY, 0, 0);
    LONG lines = (LONG)SendMessageW(g_edit, EM_GETLINECOUNT, 0, 0);
    if (lines > 0 && lines < 200000) {
        COLORREF accent = g_dark ? RGB(86, 156, 214) : RGB(0, 95, 184);
        CHARRANGE sel;
        SendMessageW(g_edit, EM_EXGETSEL, 0, (LPARAM)&sel);
        WCHAR buf[512];
        for (LONG l = 0; l < lines; l++) {
            LONG idx = (LONG)SendMessageW(g_edit, EM_LINEINDEX, (WPARAM)l, 0);
            if (idx < 0) continue;
            LONG len = (LONG)SendMessageW(g_edit, EM_LINELENGTH, (WPARAM)idx, 0);
            /* 段头是 "[" + 段名 + "]"，短行；超长的行不必去取文本 */
            if (len < 2 || len >= (LONG)(sizeof(buf) / sizeof(buf[0]) - 1)) continue;
            buf[0] = (WCHAR)(sizeof(buf) / sizeof(buf[0]) - 1);   /* EM_GETLINE 要求首字给容量 */
            LONG got = (LONG)SendMessageW(g_edit, EM_GETLINE, (WPARAM)l, (LPARAM)buf);
            if (got <= 0) continue;
            if (got > (LONG)(sizeof(buf) / sizeof(buf[0]) - 1)) got = (LONG)(sizeof(buf) / sizeof(buf[0]) - 1);
            while (got > 0 && (buf[got - 1] == L'\r' || buf[got - 1] == L'\n')) got--;
            if (got < 2 || buf[0] != L'[' || buf[got - 1] != L']') continue;
            CHARRANGE cr;
            cr.cpMin = idx;
            cr.cpMax = idx + got;
            SendMessageW(g_edit, EM_EXSETSEL, 0, (LPARAM)&cr);
            CHARFORMAT2W cf;
            memset(&cf, 0, sizeof(cf));
            cf.cbSize = sizeof(cf);
            cf.dwMask = CFM_BOLD | CFM_COLOR;
            cf.dwEffects = CFE_BOLD;
            cf.crTextColor = accent;
            SendMessageW(g_edit, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
        }
        SendMessageW(g_edit, EM_EXSETSEL, 0, (LPARAM)&sel);   /* 光标/选区还原 */
    }
    SendMessageW(g_edit, EM_SETMODIFY, wasDirty, 0);
}

/* 筛选期间在标题里带一句命中行数：没有状态条之后，"到底有没有命中"总得有个
   反馈（否则筛出个空窗口看不出是没匹配还是控件坏了）。不筛选时恢复纯标题。 */
static void UpdateTitle(void) {
    if (!g_hwnd) return;
    if (!g_filtering) { SetWindowTextW(g_hwnd, APP_TITLE); return; }
    WCHAR t[128];
    _snwprintf_s(t, 128, _TRUNCATE, L"%s — 筛选命中 %d 行", APP_TITLE, g_matches);
    SetWindowTextW(g_hwnd, t);
}

/* 不区分大小写的子串查找。整屏文本最多几十 KB，逐位置比就够，不必上算法。 */
static BOOL ContainsNoCase(const WCHAR* hay, int hayLen, const WCHAR* needle, int ndLen) {
    if (ndLen <= 0) return TRUE;
    for (int i = 0; i + ndLen <= hayLen; i++)
        if (_wcsnicmp(hay + i, needle, (size_t)ndLen) == 0) return TRUE;
    return FALSE;
}

/* 筛选：只保留**包含**搜索词的整行（键或值命中都算，不区分大小写）。
   返回堆上的结果（调用方 free）；词为空返回 NULL（= 不过滤）。空行不留 ——
   筛选的意义是"把相关的那几行摆出来"，留一堆空隙反而更难扫。 */
static WCHAR* FilterText(const WCHAR* src, const WCHAR* needle, int* outLines) {
    *outLines = 0;
    if (!src || !needle || !needle[0]) return NULL;
    int ndLen = (int)wcslen(needle);
    size_t cap = wcslen(src) + 2;                 /* 只会变短，这个容量够 */
    WCHAR* out = (WCHAR*)calloc(cap, sizeof(WCHAR));
    if (!out) return NULL;
    size_t n = 0;
    const WCHAR* p = src;
    while (*p) {
        const WCHAR* e = p;
        while (*e && *e != L'\r' && *e != L'\n') e++;
        int len = (int)(e - p);
        if (ContainsNoCase(p, len, needle, ndLen)) {
            memcpy(out + n, p, (size_t)len * sizeof(WCHAR));
            n += (size_t)len;
            out[n++] = L'\n';
            (*outLines)++;
        }
        while (*e == L'\r' || *e == L'\n') e++;
        p = e;
    }
    return out;
}

/* 取编辑区全文（堆）。失败返回 NULL。 */
static WCHAR* EditTextDup(void) {
    if (!g_edit) return NULL;
    int n = GetWindowTextLengthW(g_edit);
    if (n < 0) return NULL;
    WCHAR* t = (WCHAR*)calloc((size_t)n + 1, sizeof(WCHAR));
    if (!t) return NULL;
    GetWindowTextW(g_edit, t, n + 1);
    return t;
}

/* 把编辑区的改动存回当前视图的源文本。三种情况跳过：
   · 筛选期间 —— 控件里是过滤结果，存回去会把原文冲掉；
   · 采集进行中 —— 控件里是"正在收集…"那行占位文字；
   · g_noSync —— 刚换过源文本，控件里的还是上一份内容。 */
static void ViewSyncText(void) {
    if (!g_edit || g_filtering || g_busy || g_noSync) return;
    WCHAR* t = EditTextDup();
    if (!t) return;
    free(g_viewText[g_view]);
    g_viewText[g_view] = t;
}

/* 批量改文本/格式时关掉重绘：换视图（常用 ↔ 全部）要整篇替换 + 逐行刷段头，
   中间态会闪一下；抑制期间只在最后统一重画。用计数支持嵌套调用（ApplyView
   里套 RefreshEditText）。 */
static int g_batch;

static void BeginBatch(void) {
    if (!g_edit) return;
    if (g_batch++ == 0) SendMessageW(g_edit, WM_SETREDRAW, FALSE, 0);
}

static void EndBatch(void) {
    if (!g_edit) return;
    if (g_batch > 0 && --g_batch == 0) {
        SendMessageW(g_edit, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(g_edit, NULL, FALSE);
    }
}

/* 按当前搜索词重排编辑区：没词 → 放回源文本、可编辑；有词 → 只显示命中的行、
   只读（过滤结果和原文没有逐行对应关系，在上面改字改的是"哪一行"说不清）。
   视图切换、搜索词变化、采集到新数据，三条路都汇到这里。 */
static void RefreshEditText(void) {
    if (!g_edit) return;
    WCHAR q[256] = L"";
    if (g_search) GetWindowTextW(g_search, q, 256);
    BOOL wasDirty = (BOOL)SendMessageW(g_edit, EM_GETMODIFY, 0, 0);
    const WCHAR* src = g_viewText[g_view] ? g_viewText[g_view] : L"";
    BeginBatch();
    if (!q[0]) {
        if (g_filtering) {                       /* 退出筛选：源文本原样放回 */
            g_filtering = FALSE;
            SetEditText(src);
        }
        SendMessageW(g_edit, EM_SETREADONLY, FALSE, 0);
        g_matches = 0;
    } else {
        if (!g_filtering) ViewSyncText();        /* 进筛选前先把编辑存回源 */
        g_filtering = TRUE;
        int hits = 0;
        WCHAR* f = FilterText(src, q, &hits);
        SetEditText(f ? f : L"");
        free(f);
        SendMessageW(g_edit, EM_SETREADONLY, TRUE, 0);
        g_matches = hits;
    }
    StyleHeaders();
    SendMessageW(g_edit, EM_SETMODIFY, wasDirty, 0);
    EndBatch();
    UpdateTitle();
}

/* 切视图。两个视图都可编辑，各自留着自己那份源文本：离开前把编辑存回去，
   回来还是原样。筛选态下切视图 = 用同一个词去筛另一个视图。 */
static void ApplyView(int v) {
    if (!g_edit) return;
    if (v != 0 && v != 1) v = 0;
    ViewSyncText();                     /* 离开前把（非筛选态的）编辑存回它自己那份 */
    g_view = v;
    if (g_btnCommon) SendMessageW(g_btnCommon, BM_SETCHECK, v == 0 ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_btnAll)    SendMessageW(g_btnAll,    BM_SETCHECK, v == 1 ? BST_CHECKED : BST_UNCHECKED, 0);
    /* 控件里现在是**另一个视图**的内容：整篇换掉，并且不许 RefreshEditText
       把它当成"本视图的编辑"回写（g_noSync 只在这一段生效）。整段关重绘：
       换文本 + 逐行刷段头 + 重算滚动范围，中途重画会闪。 */
    BOOL savedNoSync = g_noSync;
    g_noSync = TRUE;
    g_filtering = FALSE;
    g_scrollTargetY = -1;               /* 内容整篇换了，旧的滚动目标作废 */
    BeginBatch();
    SetEditText(g_viewText[v] ? g_viewText[v] : L"");
    RefreshEditText();                  /* 有筛选词就按它重新筛这个视图 */
    EndBatch();
    g_noSync = savedNoSync;
    SendMessageW(g_edit, EM_SETMODIFY, FALSE, 0);
}

/* ---------- 布局 ---------- */

/* 文字与编辑框**内侧**的留白（仅横向；纵向由控件自身行距负责）。外层不再留
   pad —— 编辑区贴客户区右/下边，滚动条才紧贴边缘，不会在右侧/下方空出一圈。 */
static void ApplyTextMargins(void) {
    if (!g_edit) return;
    int left  = MulDiv(10, g_dpi, 96);
    int right = MulDiv(6,  g_dpi, 96);
    if (left  < 4) left  = 4;
    if (right < 2) right = 2;
    SendMessageW(g_edit, EM_SETMARGINS,
                 EC_LEFTMARGIN | EC_RIGHTMARGIN,
                 MAKELPARAM(left, right));
}

/* 两层：顶部工具条（常用/全部 + 筛选框）→ 编辑区（**贴边**，一直铺到客户区
   底边）。文字离边的距离由 ApplyTextMargins 负责，外层不再加 pad。 */
static void Layout(void) {
    if (!g_hwnd) return;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int cw = rc.right, ch = rc.bottom;
    int pad = MulDiv(8, g_dpi, 96);
    int toolH = MulDiv(34, g_dpi, 96);
    int bh = MulDiv(22, g_dpi, 96);

    /* 顶部一栏：左边两个视图按钮，右边筛选框吃掉剩下的宽度。按钮在左、输入框
       在右 —— 先选视图再筛，视线也是从左往右。 */
    int bw = MulDiv(56, g_dpi, 96);
    int bg = MulDiv(6, g_dpi, 96);
    int sy = (toolH - bh) / 2;
    if (g_btnCommon) MoveWindow(g_btnCommon, pad, sy, bw, bh, TRUE);
    if (g_btnAll)    MoveWindow(g_btnAll, pad + bw + bg, sy, bw, bh, TRUE);
    if (g_search) {
        /* 右边界对齐**竖向滚动条的左边**（不是客户区右边界）：编辑区下面那一条
           17px 的滚动条是常驻的，筛选框若按 pad 收边就会压到滚动条上方、右边缘
           落在空白区中间，看着别扭。对齐之后工具栏的右边与正文那一列是同一条竖线。 */
        int sbW = GetSystemMetrics(SM_CXVSCROLL);
        if (sbW <= 0) sbW = MulDiv(17, g_dpi, 96);
        int sx = pad + (bw + bg) * 2;
        int sw = cw - sbW - sx;
        int minW = MulDiv(90, g_dpi, 96);
        if (sw < minW) sw = minW;
        MoveWindow(g_search, sx, sy, sw, bh, TRUE);
    }
    /* 中间编辑区：左右下都贴边（没有状态条了，一直铺到客户区底边） */
    int editTop = toolH;
    int editH = ch - toolH;
    if (editH < 10) editH = 10;
    if (g_edit) MoveWindow(g_edit, 0, editTop, cw, editH, TRUE);
    ApplyTextMargins();
}

/* ---------- 定位：光标居中 + 工作区钳制（CLaunch-search 的做法） ---------- */

static void PlaceNearCursor(void) {
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    RECT work = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    MONITORINFO mi;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi)) work = mi.rcWork;

    /* 目标屏 DPI 只能用 GetDpiForMonitor：窗口还没搬过去，GetDpiForWindow 给的
       是它**当前**所在屏的 DPI（多屏不同缩放时会算错尺寸）。shcore 是 Win8.1+
       才有的，取不到就退回系统 DPI。 */
    UINT dpiX = (UINT)g_dpi, dpiY = (UINT)g_dpi;
    typedef HRESULT (WINAPI* PFN_GetDpiForMonitor)(HMONITOR, int, UINT*, UINT*);
    static PFN_GetDpiForMonitor gdfm;
    if (!gdfm) {
        HMODULE sc = LoadLibraryW(L"shcore.dll");
        if (sc) gdfm = (PFN_GetDpiForMonitor)(void*)GetProcAddress(sc, "GetDpiForMonitor");
    }
    if (gdfm && mon) {
        UINT dx = 96, dy = 96;
        if (SUCCEEDED(gdfm(mon, 0 /* MDT_EFFECTIVE_DPI */, &dx, &dy)) && dx >= 48 && dx <= 480) {
            dpiX = dx; dpiY = dy;
        }
    }

    int w = MulDiv(g_wDip, (int)dpiX, 96);
    int h = MulDiv(g_hDip, (int)dpiY, 96);
    /* 先按工作区收一遍尺寸：配置里写 2000 或 175% 缩放下 780 DIP 会高于屏幕，
       光钳位置是没用的（窗口比工作区还大时怎么摆都会有一半在屏外）。 */
    int wMax = (int)(work.right - work.left);
    int hMax = (int)(work.bottom - work.top);
    if (wMax > 0 && w > wMax) w = wMax;
    if (hMax > 0 && h > hMax) h = hMax;
    int x = pt.x - w / 2;
    int y = pt.y - h / 2;
    /* 先夹左上（负坐标显示器照样成立：rcWork 本身就是负的），再从右下回退，
       最后再夹一次左上 —— 窗口比工作区还大时贴左上角，至少标题栏可见。 */
    if (x < work.left) x = work.left;
    if (y < work.top)  y = work.top;
    if (x + w > work.right)  x = work.right - w;
    if (y + h > work.bottom) y = work.bottom - h;
    if (x < work.left) x = work.left;
    if (y < work.top)  y = work.top;

    /* 必须在 ShowWindow 之前落位，且不激活：隐藏窗口上的 MoveWindow 会触发
       WM_DPICHANGED，若照抄系统建议矩形就会二次缩放（CLaunch-search 那边为此
       专门加了护栏）。我们的 WM_DPICHANGED 只更新字体，不动矩形。 */
    SetWindowPos(g_hwnd, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

/* ---------- 上下文菜单（编辑控件不自带；也是「刷新」唯一的入口） ---------- */

static void ShowContextMenu(int sx, int sy) {
    HMENU m = CreatePopupMenu();
    if (!m) return;
    CHARRANGE cr;
    memset(&cr, 0, sizeof(cr));
    SendMessageW(g_edit, EM_EXGETSEL, 0, (LPARAM)&cr);
    BOOL hasSel = (cr.cpMin != cr.cpMax);
    BOOL canUndo = (BOOL)SendMessageW(g_edit, EM_CANUNDO, 0, 0);
    BOOL canPaste = (BOOL)SendMessageW(g_edit, EM_CANPASTE, 0, 0);

    AppendMenuW(m, MF_STRING | (canUndo ? 0 : MF_GRAYED), IDM_UNDO, L"撤销\tCtrl+Z");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (hasSel ? 0 : MF_GRAYED), IDM_CUT,   L"剪切\tCtrl+X");
    AppendMenuW(m, MF_STRING | (hasSel ? 0 : MF_GRAYED), IDM_COPY,  L"复制\tCtrl+C");
    AppendMenuW(m, MF_STRING | (canPaste ? 0 : MF_GRAYED), IDM_PASTE, L"粘贴\tCtrl+V");
    AppendMenuW(m, MF_STRING | (hasSel ? 0 : MF_GRAYED), IDM_DELETE, L"删除\tDel");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_SELECTALL, L"全选\tCtrl+A");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, IDM_PICK, L"瞄准窗口（点击取样）");
    AppendMenuW(m, MF_STRING, IDM_REFRESH, L"刷新");

    if (sx == -1 && sy == -1) {          /* 键盘唤出（Shift+F10 / 菜单键） */
        RECT rc;
        GetWindowRect(g_edit, &rc);
        sx = rc.left + 20;
        sy = rc.top + 20;
    }
    TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN,
                   sx, sy, 0, g_hwnd, NULL);
    DestroyMenu(m);
}

/* ---------- 滚轮：平滑纵向 / Shift 横向 ----------
   RichEdit 自带的滚轮是"一次跳三行"，240Hz 屏上看得出一格一格地顿。这里自己接：
   累计一个**像素级**目标位置，再用帧定时器逐帧靠近（每次走剩余距离的一半、最少
   1 像素），停下就 KillTimer。EM_GETSCROLLPOS / EM_SETSCROLLPOS 用的就是像素，
   而系统那条路只有"行"这一档粒度。
   ★ 只处理能吃下的情况（拿得到滚动位置）；拿不到就交回默认处理，别把滚轮吞掉。 */

/* 走一帧：把滚动位置往目标挪。挪不动（已在顶/底）就收手，别让定时器空转。 */
static void ScrollSmoothTick(void) {
    if (g_scrollTargetY < 0 || !g_edit) { KillTimer(g_hwnd, TID_SMOOTH); return; }
    POINT sp;
    if (!SendMessageW(g_edit, EM_GETSCROLLPOS, 0, (LPARAM)&sp)) {
        g_scrollTargetY = -1;
        KillTimer(g_hwnd, TID_SMOOTH);
        return;
    }
    int dy = g_scrollTargetY - sp.y;
    if (dy == 0) { g_scrollTargetY = -1; KillTimer(g_hwnd, TID_SMOOTH); return; }
    int step = dy / 2;
    if (step == 0) step = (dy > 0) ? 1 : -1;
    POINT to = sp;
    to.y += step;
    if (to.y < 0) to.y = 0;
    SendMessageW(g_edit, EM_SETSCROLLPOS, 0, (LPARAM)&to);
    POINT now;
    if (SendMessageW(g_edit, EM_GETSCROLLPOS, 0, (LPARAM)&now) && now.y == sp.y) {
        g_scrollTargetY = -1;                 /* 位置没变 = 撞到内容边界 */
        KillTimer(g_hwnd, TID_SMOOTH);
    }
}

/* 滚轮：Shift = 横向，否则纵向平滑。返回 TRUE 表示已处理。 */
static BOOL EditWheel(HWND h, WPARAM wp) {
    int delta = GET_WHEEL_DELTA_WPARAM(wp);
    if (!delta) return TRUE;
    POINT sp;
    if (!SendMessageW(h, EM_GETSCROLLPOS, 0, (LPARAM)&sp)) return FALSE;
    int lineH = g_lineH > 0 ? g_lineH : MulDiv(16, g_dpi, 96);
    int perNotch = lineH * 3;                 /* 一格滚三行，跟系统默认的观感一致 */

    if (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) {
        /* 横向：一格滚"三行高"那么宽（等宽字体下约等于十几个字符） */
        sp.x -= MulDiv(delta, perNotch, WHEEL_DELTA);
        if (sp.x < 0) sp.x = 0;
        SendMessageW(h, EM_SETSCROLLPOS, 0, (LPARAM)&sp);
        return TRUE;
    }

    int base = (g_scrollTargetY >= 0) ? g_scrollTargetY : sp.y;
    int target = base - MulDiv(delta, perNotch, WHEEL_DELTA);
    if (target < 0) target = 0;
    g_scrollTargetY = target;
    SetTimer(g_hwnd, TID_SMOOTH, 10, NULL);
    ScrollSmoothTick();                       /* 先走一帧，别等到第一次定时器 */
    return TRUE;
}

/* ---------- 编辑控件的子类化：Esc 关窗 / Tab 切视图 ----------
   RichEdit 会把键盘消息都吃掉，主窗口收不到 WM_KEYDOWN；又不想为此上
   对话框（本窗口不是 dialog，没有 IsDialogMessage 那条捷径）。
   刷新不绑快捷键：它唯一的入口是右键菜单，见 ShowContextMenu。 */
static LRESULT CALLBACK EditSub(HWND h, UINT m, WPARAM wp, LPARAM lp,
                                UINT_PTR id, DWORD_PTR ref) {
    (void)id; (void)ref;
    if (m == WM_KEYDOWN && wp == VK_ESCAPE) { DestroyWindow(g_hwnd); return 0; }
    /* Tab / Shift+Tab：在两个视图之间切。只认 WM_KEYDOWN —— 同一次按键还会被
       消息循环翻译成一条 WM_CHAR('\t')，那条**只吞不做**：两条都切的话一次 Tab
       会切两次、等于没切（实测过）。 */
    if (m == WM_KEYDOWN && wp == VK_TAB) {
        ApplyView(1 - g_view);
        return 0;
    }
    if (m == WM_CHAR && wp == L'\t') return 0;   /* 别把制表符插进正文 */
    if (m == WM_MOUSEWHEEL) {
        if (EditWheel(h, wp)) return 0;       /* 吃不下就交回默认 */
    }
    return DefSubclassProc(h, m, wp, lp);
}

/* 筛选框：Esc 先清空筛选（有词时），空时关窗。 */
static LRESULT CALLBACK SearchSub(HWND h, UINT m, WPARAM wp, LPARAM lp,
                                  UINT_PTR id, DWORD_PTR ref) {
    (void)id; (void)ref;
    if (m == WM_KEYDOWN && wp == VK_ESCAPE) {
        WCHAR q[8] = L"";
        GetWindowTextW(h, q, 8);
        if (q[0]) { SetWindowTextW(h, L""); RefreshEditText(); }
        else DestroyWindow(g_hwnd);
        return 0;
    }
    return DefSubclassProc(h, m, wp, lp);
}

/* ---------- 瞄准窗口（右键菜单 → 瞄准窗口）与"双击启动看什么" ----------
   Spy++ / WinSpy / Window Detective 这类工具的标准交互：鼠标变十字，点哪个窗口就
   看哪个。做法是 SetCapture 把鼠标消息全收到本窗口，按下左键时用 WindowFromPoint
   取根窗口 —— 目标可以是任意程序的窗口，不需要主程序在场。 */
static BOOL g_picking;

static void PickBegin(void) {
    if (g_picking) return;
    g_picking = TRUE;
    SetFocus(g_hwnd);                     /* 让 Esc 落到本窗口，而不是被编辑控件吃掉 */
    SetCapture(g_hwnd);
    SetCursor(LoadCursorW(NULL, IDC_CROSS));
}

static void PickEnd(BOOL take) {
    if (!g_picking) return;
    g_picking = FALSE;
    ReleaseCapture();
    if (take) {
        POINT pt;
        if (GetCursorPos(&pt)) {
            HWND h = WindowFromPoint(pt);
            if (h) h = GetAncestor(h, GA_ROOT);
            if (h && h != g_hwnd) { g_target = h; StartCollect(); }
        }
    }
    if (g_edit) SetFocus(g_edit);
}

/* 双击启动（没有主程序牵线）时挑一个目标：先看"现在的"前台窗口，再看指针下的
   窗口，最后退回桌面。双击那一刻前台通常是资源管理器、指针也停在它上面 ——
   两条都指向同一个东西，出来的是用户想看的资料，而不是一个空窗口。 */
static HWND PickLaunchTarget(void) {
    HWND fg = GetForegroundWindow();
    if (fg && fg != g_hwnd) return fg;
    POINT pt;
    if (GetCursorPos(&pt)) {
        HWND h = WindowFromPoint(pt);
        if (h) return GetAncestor(h, GA_ROOT);
    }
    return GetDesktopWindow();
}

/* ---------- 窗口过程 ---------- */

static void SaveSizeToParent(void) {
    if (!g_parent || IsIconic(g_hwnd) || !IsWindow(g_parent)) return;
    RECT rc;
    if (!GetWindowRect(g_hwnd, &rc)) return;
    int dpi = g_dpi > 0 ? g_dpi : 96;
    typedef UINT (WINAPI* PFN_GetDpiForWindow)(HWND);
    PFN_GetDpiForWindow gdfw = (PFN_GetDpiForWindow)(void*)ModFn(L"user32.dll", "GetDpiForWindow");
    if (gdfw) { UINT d = gdfw(g_hwnd); if (d) dpi = (int)d; }
    /* 存 DIP：换显示器（不同缩放）时窗口观感不变，与其它配置项一致。 */
    int wDip = MulDiv((int)(rc.right - rc.left), 96, dpi);
    int hDip = MulDiv((int)(rc.bottom - rc.top), 96, dpi);
    if (!g_msgSize) g_msgSize = RegisterWindowMessageW(INFO_MSG_SIZE);
    if (g_msgSize) PostMessageW(g_parent, g_msgSize, (WPARAM)wDip, (LPARAM)hDip);
}

static LRESULT CALLBACK InfoProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_msgOpen && msg == g_msgOpen) {
        HWND t = (HWND)wp;
        if (t) g_target = t;
        KillTimer(hwnd, TID_ARMED);     /* 主程序来消息了：不需要那条兜底 */
        /* 唤出 / 换目标是"给我看这个窗口"，直接采集，**不**弹"丢弃修改"确认。
           那条确认只属于右键菜单里用户主动点的"刷新"（见 Refresh）—— 把它套在
           唤出上，会让每次按 F12 都先问一句，且首帧字体/主题格式化已把缓冲标脏，
           第一次唤出就会平白弹框。 */
        StartCollect();
        return 0;
    }
    switch (msg) {
    case WM_ERASEBKGND: {
        /* 窗口留白区用主题底色填（编辑控件自己用 EM_SETBKGNDCOLOR 画底）。
           g_bgBrush 在 ApplyTheme 里建；建窗早期还没建好时退回系统面色。 */
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, g_bgBrush ? g_bgBrush : (HBRUSH)(COLOR_BTNFACE + 1));
        return 1;
    }

    case WM_CTLCOLOREDIT: {
        /* 筛选框（RichEdit 自己用 EM_SETBKGNDCOLOR 画底，不走这里）。 */
        HDC dc = (HDC)wp;
        SetTextColor(dc, EditFg());
        SetBkColor(dc, EditBg());
        return (LRESULT)(g_editBrush ? g_editBrush : (HBRUSH)(COLOR_WINDOW + 1));
    }

    case WM_SIZE:
        Layout();
        return 0;

    /* 瞄准窗口期间的鼠标：捕获状态下这些消息全落在本窗口，左键取样、右键/Esc 取消 */
    case WM_LBUTTONDOWN:
        if (g_picking) { PickEnd(TRUE); return 0; }
        break;
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        if (g_picking) { PickEnd(FALSE); return 0; }
        break;
    case WM_SETCURSOR:
        if (g_picking) { SetCursor(LoadCursorW(NULL, IDC_CROSS)); return TRUE; }
        break;
    case WM_KEYDOWN:
        if (g_picking && wp == VK_ESCAPE) { PickEnd(FALSE); return 0; }
        break;
    case WM_CAPTURECHANGED:
        /* 捕获被别人抢走（弹了别的窗口等）：退出瞄准态，别卡在十字光标上 */
        if (g_picking) g_picking = FALSE;
        break;

    case WM_TIMER:
        if (wp == TID_SMOOTH) { ScrollSmoothTick(); return 0; }
        if (wp == TID_ARMED) {
            /* 1.5s 没等到主程序的 OPEN：用命令行带来的目标自己开工；
               连目标都没有（用户双击）就退出 —— 不留一个空白窗口在屏上。 */
            KillTimer(hwnd, TID_ARMED);
            if (g_target && IsWindow(g_target)) StartCollect();
            else if (!g_busy) DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_VCOMMON: if (HIWORD(wp) == BN_CLICKED) ApplyView(0); return 0;
        case IDC_VALL:    if (HIWORD(wp) == BN_CLICKED) ApplyView(1); return 0;
        case IDC_SEARCH:  if (HIWORD(wp) == EN_CHANGE)  RefreshEditText(); return 0;
        /* 没有按钮行：关闭交给标题栏的 X，刷新只由右键菜单的 IDM_REFRESH 触发。 */
        case IDM_UNDO:    SendMessageW(g_edit, EM_UNDO, 0, 0); return 0;
        case IDM_CUT:     SendMessageW(g_edit, WM_CUT, 0, 0); return 0;
        case IDM_COPY:    SendMessageW(g_edit, WM_COPY, 0, 0); return 0;
        case IDM_PASTE:   SendMessageW(g_edit, WM_PASTE, 0, 0); return 0;
        case IDM_DELETE:  SendMessageW(g_edit, WM_CLEAR, 0, 0); return 0;
        case IDM_SELECTALL: SendMessageW(g_edit, EM_SETSEL, 0, -1); return 0;
        case IDM_REFRESH: Refresh(); return 0;
        case IDM_PICK:    PickBegin(); return 0;
        default: break;
        }
        break;

    case WM_CONTEXTMENU:
        ShowContextMenu((int)(short)LOWORD(lp), (int)(short)HIWORD(lp));
        return 0;

    case WM_APP_COLLECTED: {
        Collected* col = (Collected*)lp;
        g_busy = FALSE;
        if (col) {
            free(g_viewText[0]); g_viewText[0] = col->common;
            free(g_viewText[1]); g_viewText[1] = col->all;
            free(col);
            /* 源文本换了：控件里还是上一份（或"正在收集…"），不许回写 */
            g_noSync = TRUE;
            g_filtering = FALSE;
            ApplyView(g_view);         /* 按当前视图与筛选词渲染新数据 */
            g_noSync = FALSE;
        }
        return 0;
    }

    case WM_DPICHANGED:
        /* 只更新字体与布局：**不照抄**系统给的建议矩形（原因见 PlaceNearCursor
           末尾那段）。窗口尺寸保持用户拖出来的值。 */
        g_dpi = HIWORD(wp) ? (int)HIWORD(wp) : 96;
        ApplyEditFont();
        ApplyUiFont();
        Layout();
        return 0;

    case WM_SETTINGCHANGE:
        /* 系统深 / 浅色切换时 lParam 是 "ImmersiveColorSet"：重铺主题。其它
           设置变更原样交给 DefWindowProc。 */
        if (lp && _wcsicmp((const WCHAR*)lp, L"ImmersiveColorSet") == 0) {
            ApplyTheme();
            ApplyView(g_view);       /* 重铺后按新配色重刷段头 */
        }
        break;

    case WM_CLOSE:
        SaveSizeToParent();
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_editFont) DeleteObject(g_editFont);
        if (g_uiFont) DeleteObject(g_uiFont);
        if (g_bgBrush) DeleteObject(g_bgBrush);
        if (g_editBrush) DeleteObject(g_editBrush);
        free(g_viewText[0]);
        free(g_viewText[1]);
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ================= 入口 ================= */

static void DpiAware(void) {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (!u) return;
    typedef BOOL (WINAPI* PFN_SetCtx)(HANDLE);
    PFN_SetCtx p = (PFN_SetCtx)(void*)GetProcAddress(u, "SetProcessDpiAwarenessContext");
    if (p) { p((HANDLE)(INT_PTR)-4); return; }    /* -4 = PER_MONITOR_AWARE_V2 */
    typedef BOOL (WINAPI* PFN_SetAware)(void);
    PFN_SetAware q = (PFN_SetAware)(void*)GetProcAddress(u, "SetProcessDPIAware");
    if (q) q();
}

/* 从命令行取 `--key 0xVALUE`。健壮性要求不高（只有我们自己的主程序会这么调），
   但也不能假设顺序 —— 两个参数都按名字找。 */
static ULONG_PTR ArgHWND(const WCHAR* cmd, const WCHAR* key) {
    const WCHAR* p = cmd ? wcsstr(cmd, key) : NULL;
    if (!p) return 0;
    p += wcslen(key);
    while (*p == L' ' || *p == L'\t') p++;
    return (ULONG_PTR)wcstoull(p, NULL, 0);
}

int APIENTRY wWinMain(_In_ HINSTANCE hInst, _In_opt_ HINSTANCE hPrev,
                      _In_ LPWSTR cmdLine, _In_ int show) {
    (void)hPrev; (void)show;
    g_inst = hInst;

    /* 已经有一个在跑（上一轮还没退完）：把目标转给它，自己退。
       多实例会让"再按一次 F12"变成两个窗口，而用户要的是"看同一个"。 */
    if (FindWindowW(INFO_CLASS, NULL)) {
        g_msgOpen = RegisterWindowMessageW(INFO_MSG_OPEN);
        ULONG_PTR t = ArgHWND(cmdLine, L"--hwnd");
        HWND w = FindWindowW(INFO_CLASS, NULL);
        if (w && g_msgOpen) PostMessageW(w, g_msgOpen, (WPARAM)t, 0);
        return 0;
    }

    DpiAware();

    /* 启用视觉样式：comctl32 v6 由清单（info.manifest）声明，这里按 MS 的指引
       初始化通用控件库，滚动条等才会走当前系统的现代主题而非经典灰条。 */
    {
        INITCOMMONCONTROLSEX icc;
        memset(&icc, 0, sizeof(icc));
        icc.dwSize = sizeof(icc);
        icc.dwICC = ICC_STANDARD_CLASSES;
        InitCommonControlsEx(&icc);
    }

    Ini ini;
    IniLoad(&ini);
    ResolveFont(ini.font, g_face, LF_FACESIZE);
    g_pt = ini.pt;
    g_wDip = ini.wDip;
    g_hDip = ini.hDip;
    ParseCommon(ini.common);     /* 常用视图的字段顺序（空 = 默认那 11 项） */

    g_parent = (HWND)ArgHWND(cmdLine, L"--parent");
    g_target = (HWND)ArgHWND(cmdLine, L"--hwnd");
    /* 两个参数都没有 = 用户直接双击（不是主程序拉起的那一份）：此时没有"等主程序
       送目标"这回事，自己挑一个目标立刻出内容（见 PickLaunchTarget）。 */
    BOOL standalone = (!g_parent && !g_target);
    HWND launchTarget = NULL;
    if (standalone) launchTarget = PickLaunchTarget();   /* 必须在 ShowWindow 之前取：那时我们还是后台 */
    g_msgOpen = RegisterWindowMessageW(INFO_MSG_OPEN);
    g_msgSize = RegisterWindowMessageW(INFO_MSG_SIZE);

    /* Msftedit.dll：RICHEDIT50W 类由它提供。系统自带，加载失败就没有编辑器，
       直接退出比给一个残窗更好（正常 Windows 上不会失败）。 */
    if (!LoadLibraryW(L"Msftedit.dll")) return 1;

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = InfoProc;
    wc.hInstance = hInst;
    wc.lpszClassName = INFO_CLASS;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    if (!RegisterClassExW(&wc)) return 1;

    g_dpi = 96;
    {
        HDC dc = GetDC(NULL);
        if (dc) { int d = GetDeviceCaps(dc, LOGPIXELSX); if (d > 0) g_dpi = d; ReleaseDC(NULL, dc); }
    }

    int w = MulDiv(g_wDip, g_dpi, 96);
    int h = MulDiv(g_hDip, g_dpi, 96);
    /* 不置顶：这是个要长期摆在一边的编辑器，盖住别人家窗口反而是干扰。
       再按一次 C+F12 时由主程序 SetForegroundWindow 把它抬回来（src/info.c）。
       不用 TOOLWINDOW：它需要进任务栏（用户可能顺手点回去看别的窗口）。 */
    g_hwnd = CreateWindowExW(0, INFO_CLASS, APP_TITLE,
                             WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, w, h,
                             NULL, NULL, hInst, NULL);
    if (!g_hwnd) return 1;

    /* 编辑控件：**不自动换行** —— 字段是「标签: 值」的竖排长行，软折行会把
       值的列位置冲掉，也没法用横向滚动看全一条路径。WS_HSCROLL +
       ES_AUTOHSCROLL 给横向滚动条；竖直滚动条常驻，内容一定比窗口长。
       ES_WANTRETURN 让回车能打进去（可编辑的语义）。
       ★ ES_DISABLENOSCROLL：内容没超出一屏时也**留着**滚动条（灰色不可拖）。
       否则「常用」那屏用不着竖向滚动条、「全部」那屏用得上，两条滚动条一隐一现
       就把编辑区宽高改了，切视图时整篇文字跟着重排、看着像抖了一下。
       扩展样式给 0：**不**用 WS_EX_CLIENTEDGE 那圈 Win95 式凹陷边框，编辑区做成
       平底面板，靠与窗口留白区的一档色差托出边界（现代观感，见 ApplyTheme）。 */
    g_edit = CreateWindowExW(0, EDIT_CLASS, L"正在收集…\n",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                             WS_VSCROLL | WS_HSCROLL |
                             ES_MULTILINE | ES_AUTOHSCROLL | ES_WANTRETURN |
                             ES_NOHIDESEL | ES_AUTOVSCROLL | ES_DISABLENOSCROLL,
                             0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)IDC_EDIT, hInst, NULL);
    if (!g_edit) { DestroyWindow(g_hwnd); return 1; }

    SetWindowSubclass(g_edit, EditSub, 1, 0);
    ApplyEditFont();
    SendMessageW(g_edit, EM_SETUNDOLIMIT, 100, 0);
    /* 关掉软折行。EM_SETTARGETDEVICE 的 lParam 是排版行宽（twip）：
         (0,0)      = 按控件宽度折行，长行被拆成好几个显示行；
         (0,1)      = 不折行，横向滚动范围按文本实际宽度算；
         (0,500000) = 同样不折行，但横向范围会被撑到 5 万多像素 —— 手柄小到
                      几乎拖不动，看一条长路径全靠碰运气。
       取 1：超宽的长行交给横向滚动，值的列位置也不会被软折行冲掉。 */
    SendMessageW(g_edit, EM_SETTARGETDEVICE, 0, (LPARAM)1);
    /* 可读可改：两个视图都可编辑，只读只出现在筛选态（见 RefreshEditText）。 */

    /* 顶部筛选框（单行 EDIT；cue banner 给占位提示；WM_CTLCOLOREDIT 上底色）。
       边框给 0：靠底色与工具条区分，避免 Win95 式凹陷边框。 */
    g_search = CreateWindowExW(0, L"EDIT", L"",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                               0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)IDC_SEARCH, hInst, NULL);
    if (g_search) {
        SetWindowSubclass(g_search, SearchSub, 2, 0);
        SendMessageW(g_search, EM_SETCUEBANNER, TRUE, (LPARAM)L"筛选");
    }
    /* 顶部右侧：常用 / 全部 两个切换按钮（toggle 外观）。 */
    g_btnCommon = CreateWindowExW(0, L"BUTTON", L"常用",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                  BS_AUTOCHECKBOX | BS_PUSHLIKE,
                                  0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)IDC_VCOMMON, hInst, NULL);
    g_btnAll = CreateWindowExW(0, L"BUTTON", L"全部",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                               BS_AUTOCHECKBOX | BS_PUSHLIKE,
                               0, 0, 10, 10, g_hwnd, (HMENU)(INT_PTR)IDC_VALL, hInst, NULL);

    ApplyUiFont();
    /* 默认「常用」视图（可编辑）；文本等采集回来由 ApplyView 填。 */
    g_view = 0;
    if (g_btnCommon) SendMessageW(g_btnCommon, BM_SETCHECK, BST_CHECKED, 0);
    SendMessageW(g_edit, EM_SETREADONLY, FALSE, 0);

    ApplyTheme();          /* 跟随系统深 / 浅色：标题栏、滚动条、编辑区配色 */
    Layout();
    UpdateTitle();
    PlaceNearCursor();
    ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);
    SetFocus(g_edit);

    /* 主程序会在起窗后几十毫秒内把 OPEN 送来（它在重试到 FindWindow 成功为止）。
       送不到就用命令行带来的目标兜底；两者都没有就自己退。
       双击启动的那一份不等 —— 目标已经挑好，立刻采集，第一帧就是资料。 */
    if (standalone) {
        if (launchTarget) g_target = launchTarget;
        StartCollect();
    } else {
        SetTimer(g_hwnd, TID_ARMED, ARMED_MS, NULL);
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
