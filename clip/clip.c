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

   ------------------------------------------------------------------
   窗口与文字的画法取自 Ditto 项目
     https://github.com/sabrogden/Ditto
   `QListCtrl.cpp` 用的是**普通（非分层）窗口 + GDI `DrawText` + `DEFAULT_QUALITY`
   字体** —— 也就是 ClearType。本文件第一版照抄了 help/help.c 那套"分层窗口 +
   GDI+ 灰阶抗锯齿"，结果字明显发虚：灰阶抗锯齿只有 256 级灰度，而 ClearType
   用 RGB 子像素，横向有效分辨率是它的三倍。所以改回普通窗口 + 双缓冲 BitBlt +
   GDI 文字，与 Ditto 同源。
   列表外观（左侧序号栏 / 白底隔行 / 选中行左缘蓝杠 / 缩略图占满整行高度）
   同样沿用 Ditto 的快速粘贴窗口。

   Ditto 数据库的读法（只读）沿用它的定义：表结构（`Main` / `Data`）、排序键
   （`stickyClipOrder` / `clipOrder`）、格式名到剪贴板格式号的映射
   （`GetFormatID`）—— 数据库格式是对端的事实，不是我们可以改的设计。
   ------------------------------------------------------------------ */

#include "cap.h"

/* GDI+ 的 SDK 头在 `/analyze` 下会吐它自己的几条告警（见 help/help.c 同一处的
   说明）：压掉这几行 include，pop 之后立刻恢复。 */
#pragma warning(push)
#pragma warning(disable: 6385 28252 28253)
#include <objidl.h>
#include <commctrl.h>    /* 悬停提示：TOOLTIPS_CLASSW / TTM_*（照 Ditto） */
#include <gdiplus.h>
#pragma warning(pop)

#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <stdlib.h>

#include "sqlite3mc_amalgamation.h"

/* 色值识别（照 Ditto 的判据）：src\colorcode.c —— 与浮层同编，不依赖主程序 */
#include "colorcode.h"

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

/* ================= 剪贴板浮层（CapsEnhanceClip.exe） =================
   它是什么：按 `Caps + V` 弹出的一个**不抢焦点**的列表 —— 列出 Ditto
   剪贴板历史里最近的 30 条，单击 / 双击 / 回车粘贴（单击可在设置里关掉），
   Shift 修饰时只粘纯文本。

   四条硬约束与它们的落地方式：
     1. **不抢焦点**。`WS_EX_NOACTIVATE` + `SW_SHOWNOACTIVATE`，全程不激活 ——
        目标程序里的插入符一直待在原地。代价是**它收不到键盘**：Esc / 回车 /
        方向键 / Alt+数字 全部由**主程序**的键盘钩子代收后转发（见 hook.c），
        这里只处理鼠标。
     2. **不碰 Ditto 的写路径**。数据库一律 `SQLITE_OPEN_READONLY` 打开，只有读。
     3. **不污染剪贴板历史**。粘贴必须把条目内容写进系统剪贴板，而 Ditto 正盯着
        剪贴板 —— 所以写入时**额外**挂 Ditto 私有格式 `"Clipboard Viewer Ignore"`：
        Ditto 是**无条件**检查它的，看到就跳过这次变更。
     4. **寿命不是和弦**。松开 Caps 之后窗口必须还在，否则来不及挑条目 —— 所以
        只有明确的关闭途径，没有"松开 Caps 就隐"。

   ★★ 两条"别再犯"的位置规则：
     · 窗口位置只在**显示那一刻**算一次（曾经每次重画都按光标重新定位，鼠标一
       悬停就触发重画，窗口被"吸"过去）。
     · **鼠标移动只改选中，绝不滚动**。曾经悬停会连带 EnsureVisible，于是光标
       往下走窗口就自己滚。滚动只由滚轮和键盘导航产生。

   关闭途径：`Esc`（主程序代收）/ 标题栏 `✕` / **点到窗口外面**（本进程的低级
   鼠标钩子判定 —— 前台窗口不变时 SetWinEventHook 收不到事件，只靠它不够）/
   粘贴后自动关 / 再按一次热键。**置顶（钉住）**后，后两条停用。 */

/* ---------- 主题色（#AARRGGBB）---------- */
/* ---------- 配色：完全照抄 Ditto 默认主题 "(Ditto)" ----------
   色值来自 Ditto 源码 src/Theme.cpp 的 LoadDefaults()（主题名为 Ditto/(Default)
   时走的就是这份内置值），注释里标注对应的 Ditto 字段名：
     ListBoxOddRowsBG/Text        奇数行底/文字
     ListBoxEvenRowsBG            偶数行底（隔行灰）
     ListBoxSelectedBG/Text       选中行底/文字（**灰底黑字**，不是系统蓝）
     listSmallQuickPasteIndexColor 序号
     CaptionLeft/Right/TextColor  标题栏渐变（白→#CCC）与标题文字
     Border                       边框
     MainWindowBG                 主窗口底（占位框用它） */
#define C_BG        0xFFFFFFFF   /* ListBoxOddRowsBG = RGB(255,255,255) */
#define C_BORDER    0xFFCCCCCC   /* Border = RGB(204,204,204) */
#define C_TEXT      0xFF000000   /* ListBoxOddRowsText = RGB(0,0,0) */
#define C_TEXT_DIM  0xFF8C8C8C   /* 空态提示（Ditto 无此元素，沿用） */
#define C_ROW_ALT   0xFFF3F3F3   /* ListBoxEvenRowsBG = RGB(243,243,243) */
#define C_SEL_BG    0xFFCCCCCC   /* ListBoxSelectedBG = RGB(204,204,204) */
#define C_HEAD      0xFFFFFFFF   /* 标题栏：纯白（Ditto 的 Caption 是白→#CCC 渐变，实测观感突兀，弃用渐变只留白底） */
#define C_TITLE     0xFFBFBFBF   /* CaptionTextColor = RGB(191,191,191) */
#define C_INDEX     0xFFB4B4B4   /* listSmallQuickPasteIndexColor = RGB(180,180,180) */
#define C_THUMB_BG  0xFFF0F0F0   /* MainWindowBG = RGB(240,240,240) */

#define APP_TITLE   L"快速粘贴"
/* 整窗 95% 不透明（5% 透明）：Ditto 也支持透明度。用 `LWA_ALPHA` 整窗统一调，
   内容本身仍是不透明的 —— 这一点很关键：ClearType 必须画在不透明的底上，
   逐像素 alpha 的半透明会让它退化成灰阶抗锯齿（第一版字发虚就是这个原因）。 */
#define CLIP_ALPHA  242
#define ROWS_VISIBLE 10           /* 基准可见行数：窗口高度按它算 */
#define CLIP_MAX_ITEMS 30
#define CLIP_TEXT_MAX 160

/* ---------- 条目 ---------- */
typedef struct {
    long long id;
    int       lines;              /* 1 或 2 */
    BOOL      hasImage;
    BOOL      hasSwatch;          /* 这条文本本身就是个色值 -> 左侧画色块 */
    COLORREF  swatch;
    int       swatchAlpha;        /* <255：先画棋盘格，再按 alpha 混合 */
    WCHAR     text[2][CLIP_TEXT_MAX];
} ClipItem;

/* Ditto 的设置（便携版 <DittoDir>\Ditto.Settings）—— 定义放在这里是因为
   读条目时就要按它决定"要不要判色值"；加载函数在下面 DsLoad()。 */
typedef struct {
    BOOL drawColorCode;
    int  tipChars;
    int  tipLines;
    int  tipTimeoutMs;      /* 0 = 不显示；<0 = 默认 5 秒 */
} DittoCfg;
static DittoCfg g_dc = { TRUE, 1000, 30, -1 };

/* 悬停提示自动消失的时间：0 = 关掉提示，其余 = 毫秒（Ditto 的上限是 32000） */
static int DsTipTimeoutMs(void) {
    if (g_dc.tipTimeoutMs == 0) return 0;
    if (g_dc.tipTimeoutMs < 0)  return 5000;
    return (g_dc.tipTimeoutMs > 32000) ? 32000 : g_dc.tipTimeoutMs;
}

static ClipItem g_items[CLIP_MAX_ITEMS];
static int      g_count = 0;
static int      g_sel   = 0;
static int      g_scrollY = 0;    /* 竖直滚动偏移（像素）—— 滚动的唯一真相 */
static BOOL     g_pinned = FALSE;
static WCHAR    g_dirPath[DITTO_DIR_MAX] = L"";  /* Ditto 文件夹（Ditto.db / Ditto.exe 都在里面） */
static WCHAR    g_dbPath[DITTO_DIR_MAX] = L"";   /* = <dirPath>\Ditto.db，打开数据库用 */
static BOOL     g_dittoOk = FALSE;               /* <dirPath>\Ditto.exe 存在（决定"转本体"按钮显隐） */
static WCHAR    g_fontName[CLIP_FONT_MAX] = L"";  /* 空 = 内置 Microsoft YaHei UI */
static int      g_fontPt = 10;                     /* 字号（磅） */
static BOOL     g_clickPaste = TRUE;               /* 单击条目直接上屏 */
static BOOL     g_dbErr = FALSE;

/* ---------- 窗口与绘制 ---------- */
static HWND      g_hwnd = NULL;
static UINT      g_msgShow = 0, g_msgHide = 0, g_msgKa = 0, g_msgEsc = 0, g_msgKey = 0;
static BOOL      g_visible = FALSE;
static ULONGLONG g_lastKa = 0;
static HDC       g_memDC = NULL;
static HBITMAP   g_bmp = NULL;
static void*     g_bits = NULL;
static int       g_pxW = 0, g_pxH = 0;
static int       g_px = 0, g_py = 0;
static float     g_k = 1.0f;
static float     g_headerH, g_gutterW, g_rowH1, g_rowH2, g_thumbPad;
static float     g_lineH = 20.0f;   /* 一行文字的行高（按字号算，见 FitMetrics） */
static HFONT     g_fontRow = NULL, g_fontSmall = NULL;
static HWND      g_target = NULL;
static HWINEVENTHOOK g_evHook = NULL;
static HHOOK     g_mouseHook = NULL;
static BOOL      g_pasting = FALSE;
static BOOL      g_dragging = FALSE;   /* 正在拖标题栏挪窗口 */
static POINT     g_dragCur;            /* 按下那一刻的光标位置 */
static POINT     g_dragWin;            /* 按下那一刻的窗口位置 */

#define TID_GRACE  2
#define TID_LIFE   3
#define TID_KA     5
#define TID_TIP   6    /* 悬停提示的自动消失（TRACK 模式不会自己收） */
#define GRACE_MS   300
#define LIFE_MS    180000
#define KA_POLL_MS 250

static BOOL  Render(void);
static void  DoHideClose(void);
static void  DittoAction(void);
static void  DoPaste(BOOL plain);
static float RowHeight(int idx);
static BOOL  Utf8FromW(const WCHAR* w, char* out, int cap);
static void  MoveSel(int delta);

/* ================= Ditto 数据库（只读） ================= */

static const WCHAR* kSkipFormatNames[] = {
    L"Ditto Ping Format", L"Ditto Delay Saving Data", L"Ditto Remote CF_HDROP",
    L"Ditto File Data", L"Clipboard Viewer Ignore",
    L"ExcludeClipboardContentFromMonitorProcessing", L"CanIncludeInClipboardHistory",
};
static const int kSkipFormatIds[] = {
    CF_METAFILEPICT, CF_PALETTE, CF_ENHMETAFILE, CF_OWNERDISPLAY, CF_DSPMETAFILEPICT
};

static UINT FormatIdFromName(const WCHAR* name) {
    if (!name || !name[0]) return 0;
    struct { const WCHAR* n; UINT id; } std[] = {
        { L"CF_TEXT", CF_TEXT }, { L"CF_BITMAP", CF_BITMAP },
        { L"CF_METAFILEPICT", CF_METAFILEPICT }, { L"CF_SYLK", CF_SYLK },
        { L"CF_DIF", CF_DIF }, { L"CF_TIFF", CF_TIFF },
        { L"CF_OEMTEXT", CF_OEMTEXT }, { L"CF_DIB", CF_DIB },
        { L"CF_PALETTE", CF_PALETTE }, { L"CF_PENDATA", CF_PENDATA },
        { L"CF_RIFF", CF_RIFF }, { L"CF_WAVE", CF_WAVE },
        { L"CF_UNICODETEXT", CF_UNICODETEXT }, { L"CF_ENHMETAFILE", CF_ENHMETAFILE },
        { L"CF_HDROP", CF_HDROP }, { L"CF_LOCALE", CF_LOCALE },
        { L"CF_DIBV5", 17 },
    };
    for (int i = 0; i < (int)(sizeof(std) / sizeof(std[0])); i++)
        if (_wcsicmp(name, std[i].n) == 0) return std[i].id;
    return RegisterClipboardFormatW(name);
}

static void TwoLines(const WCHAR* in, WCHAR out[2][CLIP_TEXT_MAX]) {
    out[0][0] = out[1][0] = 0;
    if (!in) return;
    int row = 0, n = 0;
    for (int i = 0; in[i]; ) {
        WCHAR c = in[i];
        if (c == L'\r') { i++; continue; }
        if (c == L'\n') {
            if (row == 0) { out[0][n] = 0; row = 1; n = 0; i++; continue; }
            break;
        }
        if (c == L'\t') c = L' ';
        if (n >= CLIP_TEXT_MAX - 1) break;
        out[row][n++] = c;
        i++;
    }
    out[row][n] = 0;
    if (row == 1 && !out[1][0]) row = 0;
}

static void LabelFromFormats(const WCHAR* fmts, WCHAR* out, int cap) {
    if (fmts && wcsstr(fmts, L"CF_HDROP"))      { wcscpy_s(out, (size_t)cap, L"[文件]"); return; }
    if (fmts && (wcsstr(fmts, L"CF_DIB") || wcsstr(fmts, L"PNG")))
                                                { wcscpy_s(out, (size_t)cap, L"[图片]"); return; }
    if (fmts && (wcsstr(fmts, L"HTML Format") || wcsstr(fmts, L"Rich Text Format")))
                                                { wcscpy_s(out, (size_t)cap, L"[富文本]"); return; }
    wcscpy_s(out, (size_t)cap, L"[无描述]");
}

static void DbLoadItems(void) {
    g_count = 0;
    g_dbErr = FALSE;
    if (!g_dbPath[0]) { g_dbErr = TRUE; return; }

    char pathU8[DITTO_DIR_MAX * 4];
    if (!Utf8FromW(g_dbPath, pathU8, (int)sizeof(pathU8))) { g_dbErr = TRUE; return; }

    sqlite3* db = NULL;
    if (sqlite3_open_v2(pathU8, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        g_dbErr = TRUE;
        return;
    }
    sqlite3_busy_timeout(db, 3000);

    sqlite3_stmt* st = NULL;
    const char* sql =
        "SELECT m.lID, m.mText, m.QuickPasteText, "
        "       (SELECT group_concat(d.strClipBoardFormat, ',') FROM Data d "
        "        WHERE d.lParentID = m.lID) "
        "FROM Main m "
        "WHERE IFNULL(m.bIsGroup, 0) = 0 "
        "ORDER BY COALESCE(m.stickyClipOrder, -2147483647) DESC, "
        "         COALESCE(m.clipOrder, m.lDate) DESC "
        "LIMIT 30";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        g_dbErr = TRUE;
        return;
    }
    while (sqlite3_step(st) == SQLITE_ROW && g_count < CLIP_MAX_ITEMS) {
        ClipItem* it = &g_items[g_count];
        memset(it, 0, sizeof(*it));
        it->id = sqlite3_column_int64(st, 0);
        const WCHAR* mt = (const WCHAR*)sqlite3_column_text16(st, 1);
        const WCHAR* qt = (const WCHAR*)sqlite3_column_text16(st, 2);
        const WCHAR* fm = (const WCHAR*)sqlite3_column_text16(st, 3);
        const WCHAR* src = (mt && mt[0]) ? mt : ((qt && qt[0]) ? qt : NULL);
        if (src) TwoLines(src, it->text);
        else     LabelFromFormats(fm, it->text[0], CLIP_TEXT_MAX);
        it->lines    = it->text[1][0] ? 2 : 1;
        /* 色块判定用的是**完整文本**（Ditto 也是拿整条文本判的）：
           "#F45F01" 认，"#F45F01 这是主色" 不认 —— 后者不是"一个色值"。 */
        if (g_dc.drawColorCode && src &&
            CcParseColor(src, &it->swatch, &it->swatchAlpha))
            it->hasSwatch = TRUE;
        it->hasImage = (fm && (wcsstr(fm, L"CF_DIB") || wcsstr(fm, L"PNG"))) != NULL;
        g_count++;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
}

/* ================= 悬停提示（照 Ditto：系统 tooltip 控件） =================
   Ditto 的做法是列表控件自带 tooltip：悬停一会儿，弹出这一条的**完整**文本
   （列表里只显示两行摘要，看不全的靠它）。我们照同一套：
     · 载体：TOOLTIPS_CLASSW 子窗口 —— 换行交给系统，不自己画
     · 触发：**手动 TRACK 模式**（TTF_TRACK|TTF_ABSOLUTE），由 WM_MOUSEMOVE
       直接决定弹/收与位置。不用 TTF_SUBCLASS 的自动计时，原因有二：
       (a) 本进程不带清单，激活的是 comctl32 **v5** —— TTM_ADDTOOLW 传
           v6 的 sizeof(TOOLINFOW) 会**静默失败**，必须用 TTTOOLINFOW_V1_SIZE
           （minitip 最小复现实测：72 失败、56 成功）；
       (b) NOACTIVATE 窗口上 v5 的中继计时也不触发（同一实验：注册成功
           也不弹）。手动模式反而更贴合：我们本来就悬停即选中，行变了
           顺手换提示、移出列表立刻收，不需要再赌系统的计时。 */
static HWND  g_tip = NULL;
static WCHAR g_tipBuf[4096];
static int   g_tipRow = -1;          /* 提示当前指向的行；-1 = 没在显示 */

static void TipDestroy(void) {
    if (g_tip) { DestroyWindow(g_tip); g_tip = NULL; }
}

/* 提示窗口只在显示路径创建一次；TRACK 模式的 tool 不占矩形区域，
   窗口大小变化也不用更新它。 */
static void TipUpdate(void) {
    if (DsTipTimeoutMs() == 0) { TipDestroy(); return; }   /* Ditto: 0 = 关掉提示 */
    if (g_tip) return;
    INITCOMMONCONTROLSEX ic;
    ic.dwSize = sizeof(ic);
    ic.dwICC  = ICC_WIN95_CLASSES;
    InitCommonControlsEx(&ic);
    g_tip = CreateWindowExW(0, TOOLTIPS_CLASSW, NULL,
                            WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                            CW_USEDEFAULT, CW_USEDEFAULT,
                            CW_USEDEFAULT, CW_USEDEFAULT,
                            g_hwnd, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_tip) return;
    TOOLINFOW ti;
    memset(&ti, 0, sizeof(ti));
    /* ★ cbSize 必须是 V1_SIZE：comctl32 v5 只认旧结构，传 v6 的 sizeof
       会让 TTM_ADDTOOLW 返回 0 且毫无声息（踩过，见注释块）。 */
    ti.cbSize   = TTTOOLINFOW_V1_SIZE;
    ti.uFlags   = TTF_TRACK | TTF_ABSOLUTE;
    ti.hwnd     = g_hwnd;
    ti.uId      = (UINT_PTR)g_hwnd;
    ti.lpszText = LPSTR_TEXTCALLBACK;      /* 弹出前问我们要文本 */
    SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    /* 与 Ditto 同一个值：最宽 500px，超出的自动折行 */
    SendMessageW(g_tip, TTM_SETMAXTIPWIDTH, 0, 500);
}

/* 收起提示。TTM_TRACKACTIVATE(FALSE) 是 TRACK 模式的收法；
   TTM_POP 对 TRACK 工具无效（也踩过）。 */
static void TipHide(void) {
    KillTimer(g_hwnd, TID_TIP);
    if (g_tipRow < 0) return;
    g_tipRow = -1;
    if (!g_tip) return;
    TOOLINFOW ti;
    memset(&ti, 0, sizeof(ti));
    ti.cbSize = TTTOOLINFOW_V1_SIZE;
    ti.hwnd   = g_hwnd;
    ti.uId    = (UINT_PTR)g_hwnd;
    SendMessageW(g_tip, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti);
}

/* 在光标旁弹出第 row 行的提示。行没变就不动它（TTN_NEEDTEXT 只在
   激活时问一次，反复激活会让长条目每动一下鼠标都回一次库）。 */
static void TipShowAt(int row, POINT scr) {
    if (DsTipTimeoutMs() == 0 || !g_tip) return;
    if (row < 0 || row >= g_count) return;
    if (g_tipRow == row) return;
    g_tipRow = row;
    TOOLINFOW ti;
    memset(&ti, 0, sizeof(ti));
    ti.cbSize   = TTTOOLINFOW_V1_SIZE;
    ti.hwnd     = g_hwnd;
    ti.uId      = (UINT_PTR)g_hwnd;
    ti.lpszText = LPSTR_TEXTCALLBACK;
    SendMessageW(g_tip, TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti);
    /* 光标右下 16/20px —— 系统默认提示也放这个位置，别压住正在看的那行 */
    SendMessageW(g_tip, TTM_TRACKPOSITION, 0,
                 MAKELONG(scr.x + 16, scr.y + 20));
    /* TRACK 模式不会自动消失，AUTOPOP 兜底自己拿定时器实现（同 Ditto 的
       ToolTipTimeout：-1 = 5 秒，0 = 关，上限 32 秒） */
    SetTimer(g_hwnd, TID_TIP, (UINT)DsTipTimeoutMs(), NULL);
}

/* 列表里只有摘要：短条目（没被截断）直接用它，长条目按需回库取全文。 */
static void TipTextOf(int idx) {
    g_tipBuf[0] = 0;
    if (idx < 0 || idx >= g_count) return;

    const WCHAR* full = NULL;
    static WCHAR fetched[4096];
    fetched[0] = 0;
    if (g_items[idx].lines <= 1 &&
        (int)wcslen(g_items[idx].text[0]) < CLIP_TEXT_MAX - 1) {
        full = g_items[idx].text[0];        /* 没被截断：摘要就是全文 */
    } else {
        char pathU8[DITTO_DIR_MAX * 4];
        if (!Utf8FromW(g_dbPath, pathU8, (int)sizeof(pathU8))) return;
        sqlite3* db = NULL;
        if (sqlite3_open_v2(pathU8, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
            if (db) sqlite3_close(db);
            return;
        }
        sqlite3_busy_timeout(db, 3000);
        sqlite3_stmt* st = NULL;
        if (sqlite3_prepare_v2(db, "SELECT mText, QuickPasteText FROM Main WHERE lID = ?",
                               -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, g_items[idx].id);
            if (sqlite3_step(st) == SQLITE_ROW) {
                const WCHAR* mt = (const WCHAR*)sqlite3_column_text16(st, 0);
                const WCHAR* qt = (const WCHAR*)sqlite3_column_text16(st, 1);
                const WCHAR* s  = (mt && mt[0]) ? mt : ((qt && qt[0]) ? qt : NULL);
                if (s) {
                    int n = (int)wcslen(s);
                    int cap = (int)(sizeof(fetched) / sizeof(fetched[0])) - 1;
                    if (n > cap) n = cap;
                    memcpy(fetched, s, (size_t)n * sizeof(WCHAR));
                    fetched[n] = 0;
                    full = fetched;
                }
            }
            sqlite3_finalize(st);
        }
        sqlite3_close(db);
    }
    if (!full || !full[0]) return;

    /* 截断：字符上限 + 行数上限；Tab 换成两个空格（tooltip 不认 \t，Ditto 同） */
    int cap  = (int)(sizeof(g_tipBuf) / sizeof(g_tipBuf[0])) - 1;
    int maxC = g_dc.tipChars;  if (maxC < 1 || maxC > cap) maxC = cap;
    int maxL = g_dc.tipLines;  if (maxL < 1) maxL = 1;
    int o = 0, lines = 1;
    for (int i = 0; full[i]; i++) {
        WCHAR c = full[i];
        if (c == L'\r') continue;
        if (c == L'\n') {
            if (lines >= maxL) break;
            lines++;
            if (o + 1 > cap) break;
            g_tipBuf[o++] = L'\n';
            continue;
        }
        if (c == L'\t') {
            if (o + 2 > maxC) break;
            g_tipBuf[o++] = L' '; g_tipBuf[o++] = L' ';
            continue;
        }
        if (o >= maxC) break;
        g_tipBuf[o++] = c;
    }
    g_tipBuf[o] = 0;
}

/* ================= 缩略图 ================= */

static Gdiplus::Bitmap* g_thumb[CLIP_MAX_ITEMS];
static BOOL             g_thumbTried[CLIP_MAX_ITEMS];
/* 缩略图在**后台线程**解码并缩到目标尺寸：
   第一版在渲染路径里现读库现解码，图片一多每滚一格都要卡一次 —— 磁盘 + 解码 +
   高质量缩放全在 UI 线程上。现在后台一次做完，渲染只画现成的小图（1:1 贴）；
   没画完的行先给占位框，解码完一条就 PostMessage 重画一次。
   这也是 Ditto 的路子：缩略图缓存成小位图（它还额外提供"快速缩略图"选项，
   把 HighQualityBicubic 换成 Bicubic —— 我们在后台做一次，直接用 Bicubic）。 */
static CRITICAL_SECTION g_thumbLock;
static HANDLE           g_thumbThread = NULL;
static volatile LONG    g_thumbQuit = 0;
/* 本帧缩略图实际画了多宽。★ 文字的横向起点必须用它，不能用行高 ——
   宽度是按原图比例算的，横图远比行高宽，拿行高当偏移文字就会压在图上。 */
static float            g_thumbW[CLIP_MAX_ITEMS];

static void FreeThumbs(void) {
    EnterCriticalSection(&g_thumbLock);
    for (int i = 0; i < CLIP_MAX_ITEMS; i++) {
        delete g_thumb[i];
        g_thumb[i] = NULL;
        g_thumbTried[i] = FALSE;
        g_thumbW[i] = 0.0f;
    }
    LeaveCriticalSection(&g_thumbLock);
}

static Gdiplus::Bitmap* BitmapFromDib(const BYTE* blob, int len) {
    if (len < (int)sizeof(BITMAPINFOHEADER)) return NULL;
    const BITMAPINFOHEADER* h = (const BITMAPINFOHEADER*)blob;
    if (h->biWidth <= 0 || h->biHeight == 0) return NULL;
    if (h->biPlanes != 1) return NULL;
    int bpp = h->biBitCount;
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) return NULL;
    DWORD comp = h->biCompression;
    if (comp != BI_RGB && comp != BI_BITFIELDS) return NULL;

    int clrUsed = (int)h->biClrUsed;
    if (clrUsed == 0 && bpp <= 8) clrUsed = 1 << bpp;
    DWORD off = h->biSize + (DWORD)clrUsed * sizeof(RGBQUAD);
    if (comp == BI_BITFIELDS) off += 3 * sizeof(DWORD);
    if (off >= (DWORD)len) return NULL;

    int stride = ((h->biWidth * bpp + 31) / 32) * 4;
    LONG absH = h->biHeight > 0 ? h->biHeight : -h->biHeight;
    DWORD need = (DWORD)stride * (DWORD)absH;
    if (need > (DWORD)(len - (int)off)) return NULL;

    void* bits = NULL;
    HBITMAP hb = CreateDIBSection(NULL, (const BITMAPINFO*)blob, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!hb || !bits) { if (hb) DeleteObject(hb); return NULL; }
    memcpy(bits, blob + off, need);
    Gdiplus::Bitmap* bm = Gdiplus::Bitmap::FromHBITMAP(hb, NULL);
    DeleteObject(hb);
    return bm;
}

static Gdiplus::Bitmap* BitmapFromPng(const BYTE* blob, int len) {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)len);
    if (!h) return NULL;
    void* p = GlobalLock(h);
    if (!p) { GlobalFree(h); return NULL; }
    memcpy(p, blob, (size_t)len);
    GlobalUnlock(h);
    IStream* s = NULL;
    if (CreateStreamOnHGlobal(h, TRUE, &s) != S_OK) { GlobalFree(h); return NULL; }
    Gdiplus::Bitmap* bm = Gdiplus::Bitmap::FromStream(s);
    if (s) s->lpVtbl->Release(s);   /* cap.h 走 CINTERFACE：IStream 没有 C++ 的 Release() */
    return bm;
}

/* 读库 + 解码 + **缩到目标尺寸**，返回缩好的小图（调用方负责 delete）。
   缩放在这里一次做完 —— 渲染时就是 1:1 贴，不再每帧做大图高质量缩放
   （HighQualityBicubic 对大图很贵，这正是"图片多滚动卡"的主要开销）。 */
static Gdiplus::Bitmap* DecodeThumb(int idx) {
    char pathU8[DITTO_DIR_MAX * 4];
    if (!Utf8FromW(g_dbPath, pathU8, (int)sizeof(pathU8))) return NULL;
    sqlite3* db = NULL;
    if (sqlite3_open_v2(pathU8, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }
    sqlite3_busy_timeout(db, 3000);
    Gdiplus::Bitmap* big = NULL;
    sqlite3_stmt* st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT strClipBoardFormat, ooData FROM Data "
                              "WHERE lParentID = ? ORDER BY Data.lID",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, g_items[idx].id);
        while (sqlite3_step(st) == SQLITE_ROW && !big) {
            const WCHAR* nm = (const WCHAR*)sqlite3_column_text16(st, 0);
            const void* blob = sqlite3_column_blob(st, 1);
            int len = sqlite3_column_bytes(st, 1);
            if (!nm || !blob || len <= 0) continue;
            UINT cf = FormatIdFromName(nm);
            if (cf == CF_DIB || cf == 17)
                big = BitmapFromDib((const BYTE*)blob, len);
            else if (wcscmp(nm, L"PNG") == 0)
                big = BitmapFromPng((const BYTE*)blob, len);
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    if (!big) return NULL;

    UINT iw = big->GetWidth(), ih = big->GetHeight();
    if (iw == 0 || ih == 0) { delete big; return NULL; }
    float boxH = g_rowH2 - 2.0f * g_thumbPad;
    float bw = boxH * (float)iw / (float)ih;
    float bh = boxH;
    float maxW = ((float)g_pxW - g_gutterW) * 0.5f;
    if (bw > maxW) { bw = maxW; bh = bw * (float)ih / (float)iw; }
    if (bw < 1.0f || bh < 1.0f) { delete big; return NULL; }

    /* 变量名别叫 small：RPC 头（rpcndr.h）把 small 定义成了 unsigned char */
    Gdiplus::Bitmap* out = new Gdiplus::Bitmap((int)(bw + 0.5f), (int)(bh + 0.5f),
                                               PixelFormat32bppARGB);
    if (out->GetLastStatus() != Gdiplus::Ok) { delete out; delete big; return NULL; }
    Gdiplus::Graphics sg(out);
    sg.SetInterpolationMode(Gdiplus::InterpolationModeBicubic);   /* Ditto 的 fast 档 */
    sg.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    sg.DrawImage(big, Gdiplus::RectF(0.0f, 0.0f, bw, bh),
                 0.0f, 0.0f, (float)iw, (float)ih, Gdiplus::UnitPixel);
    delete big;
    return out;
}

/* 后台解码线程：把所有带图条目的缩略图一次做完（进程活着只做这一轮）。 */
#define WM_THUMB_READY (WM_APP + 20)
static DWORD WINAPI ThumbThreadProc(LPVOID lp) {
    (void)lp;
    for (int i = 0; i < g_count; i++) {
        if (g_thumbQuit) return 0;
        if (!g_items[i].hasImage) continue;
        Gdiplus::Bitmap* bm = DecodeThumb(i);
        EnterCriticalSection(&g_thumbLock);
        g_thumb[i] = bm;                 /* 失败就是 NULL：占位框逻辑照旧 */
        g_thumbTried[i] = TRUE;
        LeaveCriticalSection(&g_thumbLock);
        PostMessageW(g_hwnd, WM_THUMB_READY, (WPARAM)i, 0);
        Sleep(0);
    }
    return 0;
}

/* ================= 粘贴 ================= */

typedef struct { UINT cf; BYTE* data; int len; } ClipFormat;
static ClipFormat g_fmts[32];
static int        g_fmtCount = 0;

static void FreeFmts(void) {
    for (int i = 0; i < g_fmtCount; i++) { free(g_fmts[i].data); g_fmts[i].data = NULL; }
    g_fmtCount = 0;
}

static void DbLoadFormats(long long id) {
    FreeFmts();
    char pathU8[DITTO_DIR_MAX * 4];
    if (!Utf8FromW(g_dbPath, pathU8, (int)sizeof(pathU8))) return;

    sqlite3* db = NULL;
    if (sqlite3_open_v2(pathU8, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return;
    }
    sqlite3_busy_timeout(db, 3000);
    sqlite3_stmt* st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT strClipBoardFormat, ooData FROM Data "
                              "WHERE lParentID = ? ORDER BY Data.lID",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        while (sqlite3_step(st) == SQLITE_ROW && g_fmtCount < 32) {
            const WCHAR* nm = (const WCHAR*)sqlite3_column_text16(st, 0);
            const void* blob = sqlite3_column_blob(st, 1);
            int len = sqlite3_column_bytes(st, 1);
            if (!nm || !blob || len <= 0) continue;
            UINT cf = FormatIdFromName(nm);
            if (!cf) continue;
            BOOL skip = FALSE;
            for (int i = 0; i < (int)(sizeof(kSkipFormatNames) / sizeof(kSkipFormatNames[0])); i++)
                if (_wcsicmp(nm, kSkipFormatNames[i]) == 0) { skip = TRUE; break; }
            for (int i = 0; i < (int)(sizeof(kSkipFormatIds) / sizeof(kSkipFormatIds[0])); i++)
                if (cf == (UINT)kSkipFormatIds[i]) { skip = TRUE; break; }
            if (skip) continue;
            BYTE* buf = (BYTE*)malloc((size_t)len);
            if (!buf) continue;
            memcpy(buf, blob, (size_t)len);
            g_fmts[g_fmtCount].cf = cf;
            g_fmts[g_fmtCount].data = buf;
            g_fmts[g_fmtCount].len = len;
            g_fmtCount++;
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
}

/* 路径转 UTF-8：`sqlite3_open_v2` 只认 UTF-8，而它是唯一带 flags 的打开接口
   （`sqlite3_open16` 没有 flags，只能读写打开；我们要的是只读）。 */
static BOOL Utf8FromW(const WCHAR* w, char* out, int cap) {
    out[0] = 0;
    if (!w || !w[0]) return FALSE;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL);
    return n > 0;
}

#define MASK_VK 0xFF
static void SendEvent(int vk, BOOL up) {
    INPUT in;
    memset(&in, 0, sizeof(in));
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = (WORD)vk;
    in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    if (vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT ||
        vk == VK_PRIOR || vk == VK_NEXT || vk == VK_HOME || vk == VK_END ||
        vk == VK_DELETE || vk == VK_INSERT)
        in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    /* 打标记：主程序的钩子靠它认出"这是浮层在粘贴"（见 cap.h）。 */
    in.ki.dwExtraInfo = CLIP_INJECT_MARKER;
    SendInput(1, &in, sizeof(in));
}

/* 补抬起必须带**扫描码**：目标程序认的是"那个物理键松开了"，只发 VK 有的程序
   不记账 —— 表现就是 Shift 明明松了、别的程序却认为它还按着。 */
static void ReleaseModFull(int vk) {
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    if (vk == VK_LMENU || vk == VK_RMENU || vk == VK_LWIN || vk == VK_RWIN) {
        SendEvent(MASK_VK, FALSE);
        SendEvent(MASK_VK, TRUE);
    }
    INPUT in;
    memset(&in, 0, sizeof(in));
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = (WORD)vk;
    in.ki.dwFlags = KEYEVENTF_KEYUP;
    if (sc) in.ki.wScan = (WORD)sc;
    in.ki.dwExtraInfo = CLIP_INJECT_MARKER;
    SendInput(1, &in, sizeof(in));
}

static void ReleaseHeldMods(void) {
    static const int mods[] = { VK_LMENU, VK_RMENU, VK_LCONTROL, VK_RCONTROL,
                                VK_LSHIFT, VK_RSHIFT, VK_LWIN, VK_RWIN };
    for (int i = 0; i < (int)(sizeof(mods) / sizeof(mods[0])); i++)
        if (GetAsyncKeyState(mods[i]) & 0x8000) ReleaseModFull(mods[i]);
}

/* 目标是不是"Ctrl+V 不管用"的控制台 / 终端系窗口。
   ★ 这不是"多支持一种粘贴方式"，是补一个真实缺口：mintty（Git Bash）、PuTTY、
   Vim 里 Ctrl+V 要么是原样的控制字符、要么是"可视块"，浮层在这些窗口上等于废的。
   Shift+Insert 是它们**共同**认的粘贴键（conhost / Windows Terminal / mintty /
   PuTTY 都支持），所以命中这一类就直接用它，而不是"先试 Ctrl+V 再补救"。
   判据用窗口**类名**：控制台类名是固定的（本机实测 conhost = ConsoleWindowClass），
   而 WSL / cmd / PowerShell 全都跑在同一个类里 —— 比按进程名判更省事也更全。
   名单要加直接往数组里塞，不用改别的。 */
static BOOL TargetIsConsole(void) {
    if (!g_target || !IsWindow(g_target)) return FALSE;
    WCHAR cls[128] = L"";
    if (!GetClassNameW(g_target, cls, 128) || !cls[0]) return FALSE;
    static const WCHAR* kConsoleClasses[] = {
        L"ConsoleWindowClass",                /* conhost：cmd / PowerShell / WSL 默认 */
        L"CASCADIA_HOSTING_WINDOW_CLASS",     /* Windows Terminal */
        L"mintty",                            /* Git Bash / MSYS2 */
        L"PuTTY",                             /* PuTTY */
        L"VirtualConsoleClass",               /* 部分终端模拟器的移植版 */
    };
    for (int i = 0; i < (int)(sizeof(kConsoleClasses) / sizeof(kConsoleClasses[0])); i++)
        if (!_wcsicmp(cls, kConsoleClasses[i])) return TRUE;
    return FALSE;
}

/* 发一次"粘贴"按键。控制台系 -> Shift+Insert；其余 -> Ctrl+V。
   两条路径都打 CLIP_INJECT_MARKER（见 SendEvent）。 */
static void SendPasteKeystroke(void) {
    if (TargetIsConsole()) {
        SendEvent(VK_SHIFT, FALSE);
        SendEvent(VK_INSERT, FALSE);
        Sleep(15);
        SendEvent(VK_INSERT, TRUE);
        SendEvent(VK_SHIFT, TRUE);
        return;
    }
    SendEvent(VK_LCONTROL, FALSE);
    Sleep(15);
    SendEvent('V', FALSE);
    Sleep(15);
    SendEvent('V', TRUE);
    Sleep(15);
    SendEvent(VK_LCONTROL, TRUE);
}

/* 只收起窗口，不启动"优雅退出"计时：粘贴流程要先让开屏幕再去抢前台，
   退出计时留到最后一步（见 DoHideClose）。 */
static void HideIfVisible(void) {
    if (!g_visible) return;
    TipHide();                                       /* 别把提示留在屏幕上 */
    ShowWindow(g_hwnd, SW_HIDE);
    g_visible = FALSE;
}

static BOOL PutOnClipboard(BOOL plain) {
    if (!OpenClipboard(g_hwnd)) return FALSE;
    EmptyClipboard();

    UINT cfIgnore = RegisterClipboardFormatW(L"Clipboard Viewer Ignore");
    BOOL ok = FALSE;

    if (plain) {
        const ClipFormat* src = NULL;
        for (int i = 0; i < g_fmtCount; i++)
            if (g_fmts[i].cf == CF_UNICODETEXT) { src = &g_fmts[i]; break; }
        if (src) {
            HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)src->len);
            if (h) {
                void* p = GlobalLock(h);
                if (p) { memcpy(p, src->data, (size_t)src->len); GlobalUnlock(h);
                         ok = SetClipboardData(CF_UNICODETEXT, h) != NULL; }
                if (!ok) GlobalFree(h);
            }
        } else {
            for (int i = 0; i < g_fmtCount && !ok; i++) {
                if (g_fmts[i].cf != CF_TEXT) continue;
                int wlen = MultiByteToWideChar(CP_ACP, 0, (const char*)g_fmts[i].data,
                                               g_fmts[i].len, NULL, 0);
                if (wlen <= 0) continue;
                HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)((wlen + 1) * sizeof(WCHAR)));
                if (!h) continue;
                WCHAR* p = (WCHAR*)GlobalLock(h);
                if (p) {
                    MultiByteToWideChar(CP_ACP, 0, (const char*)g_fmts[i].data,
                                        g_fmts[i].len, p, wlen);
                    p[wlen] = 0;
                    GlobalUnlock(h);
                    ok = SetClipboardData(CF_UNICODETEXT, h) != NULL;
                }
                if (!ok) GlobalFree(h);
            }
        }
    }
    if (!ok) {
        ok = TRUE;
        for (int i = 0; i < g_fmtCount; i++) {
            HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)g_fmts[i].len);
            if (!h) continue;
            void* p = GlobalLock(h);
            if (!p) { GlobalFree(h); continue; }
            memcpy(p, g_fmts[i].data, (size_t)g_fmts[i].len);
            GlobalUnlock(h);
            if (!SetClipboardData(g_fmts[i].cf, h)) GlobalFree(h);
        }
    }
    if (cfIgnore) {
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, 2);
        if (h) {
            BYTE* p = (BYTE*)GlobalLock(h);
            if (p) { p[0] = 1; p[1] = 0; GlobalUnlock(h);
                     if (!SetClipboardData(cfIgnore, h)) GlobalFree(h); }
            else GlobalFree(h);
        }
    }
    CloseClipboard();
    return ok;
}

static void DoPaste(BOOL plain) {
    if (g_count <= 0 || g_sel < 0 || g_sel >= g_count) return;
    g_pasting = TRUE;
    DbLoadFormats(g_items[g_sel].id);
    if (g_fmtCount <= 0) { g_pasting = FALSE; return; }

    if (!PutOnClipboard(plain)) {
        /* 剪贴板被别的程序占着（OpenClipboard 失败）：此刻剪贴板里还是**上一次**
           的内容，照发 Ctrl+V 就是把不相干的旧内容粘进用户的文档 —— "粘错东西"
           比"什么都没发生"糟糕得多。格式表照旧释放，浮层留着让用户再试一次。 */
        FreeFmts();
        g_pasting = FALSE;
        return;
    }

    /* ★ 先收起浮层，再把前台交给目标（放在"写剪贴板成功"之后：写失败时浮层
       还留着，用户能重试）。留着它挡在目标前面，用户看不到粘贴落到哪儿；抢前台
       时也多一个置顶窗口当对手。 */
    if (!g_pinned) HideIfVisible();

    if (g_target && IsWindow(g_target)) {
        SetForegroundWindow(g_target);
        for (int i = 0; i < 40; i++) {
            if (GetForegroundWindow() == g_target) break;
            Sleep(10);
        }
    }
    ReleaseHeldMods();
    /* 让"释放修饰键"与"写剪贴板"各自落地再发粘贴键：两条 SendInput 挤在同一个
       批次里时，有的程序会把 Ctrl 的按下算在修饰键释放之前，表现就是"粘不上"。 */
    Sleep(30);
    SendPasteKeystroke();
    FreeFmts();
    g_pasting = FALSE;

    if (!g_pinned) DoHideClose();
}

/* ================= 布局 ================= */

static HMONITOR CursorMonitor(void) {
    POINT cur;
    if (!GetCursorPos(&cur)) { cur.x = 0; cur.y = 0; }
    return MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);
}

static void MonitorWork(HMONITOR mon, RECT* out) {
    MONITORINFO mi;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi)) { *out = mi.rcWork; return; }
    out->left = 0; out->top = 0;
    out->right = GetSystemMetrics(SM_CXSCREEN);
    out->bottom = GetSystemMetrics(SM_CYSCREEN);
}

static int DpiOfMonitor(HMONITOR mon) {
    int dpi = 0;
    HMODULE m = LoadLibraryW(L"shcore.dll");
    if (m) {
        typedef HRESULT (WINAPI* PFN_GetDpi)(HMONITOR, int, UINT*, UINT*);
        PFN_GetDpi f = (PFN_GetDpi)(void*)GetProcAddress(m, "GetDpiForMonitor");
        if (f) {
            UINT dx = 0, dy = 0;
            if (SUCCEEDED(f(mon, 0, &dx, &dy)) && dx >= 96) dpi = (int)dx;
        }
        FreeLibrary(m);
    }
    return dpi ? dpi : 96;
}

/* 与 Ditto 同一条路：GDI 字体 + `DEFAULT_QUALITY`（跟随系统的 ClearType 设置）。
   ★ 不用 GDI+ 画字：GDI+ 只能给灰阶抗锯齿（256 级灰度），横向分辨率只有
   ClearType 的三分之一 —— 这就是"字发虚"的全部原因。 */
static HFONT MakeFont(const WCHAR* face, int px, int weight) {
    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfHeight = -px;             /* 负 = 按字形高度取，不是单元格 */
    lf.lfWeight = weight;
    lf.lfQuality = DEFAULT_QUALITY;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfOutPrecision = OUT_DEFAULT_PRECIS;
    lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
    lf.lfPitchAndFamily = VARIABLE_PITCH | FF_DONTCARE;
    /* 字体名来自配置（--font / ini），空则用内置默认。
       ★ 这里曾是写死的 g_fontFace —— 配置的字体"怎么切都没变化"就是它。 */
    wcsncpy_s(lf.lfFaceName, LF_FACESIZE,
              (face && face[0]) ? face : L"Microsoft YaHei UI", _TRUNCATE);
    return CreateFontIndirectW(&lf);
}

static void FitMetrics(void) {
    HMONITOR mon = CursorMonitor();
    g_k = (float)DpiOfMonitor(mon) / 96.0f;
    g_headerH  = 34.0f * g_k;
    g_gutterW  = 26.0f * g_k;
    g_rowH1    = 34.0f * g_k;
    /* 图片行给足高度：缩略图要**占满整行**（Ditto 就是这样），行不够高图就小 */
    g_rowH2    = 76.0f * g_k;
    g_thumbPad = 3.0f * g_k;
    g_pxW = (int)(560.0f * g_k);
    g_pxH = (int)(g_headerH + ROWS_VISIBLE * g_rowH1);

    if (g_fontRow)   DeleteObject(g_fontRow);
    if (g_fontSmall) DeleteObject(g_fontSmall);
    /* 字号是**磅**：按当前显示器 DPI 折成像素（lfHeight 负值 = 按字形高度）。
       用户在设置里选的字体名/字号经主程序用 --font/--size 传进来。 */
    int rowPx = MulDiv(g_fontPt, DpiOfMonitor(mon), 72);
    if (rowPx < 8) rowPx = 8;
    g_fontRow   = MakeFont(g_fontName, rowPx, FW_NORMAL);
    g_fontSmall = MakeFont(g_fontName, rowPx * 3 / 4, FW_NORMAL);
    /* 行高按字号走（约 1.4 倍）：两行文本的行距必须跟字号联动，
       拿"行高 ÷ 2"当行距会让字与字之间隔出一大条空白。 */
    g_lineH = (float)(int)(rowPx * 1.4f);
    if (g_lineH < 14.0f) g_lineH = 14.0f;
}

/* ★ 行高只由**已知于加载时**的量决定（文本行数 / 是否带图），不能看缩略图
   有没有载出来 —— 否则图一加载行就变高，列表会在眼前跳一下。
   两行文本与图片是**两个**高度：文本行贴着行高排（Ditto 的行距），图片行要
   给缩略图留够高度。 */
static float RowHeight(int idx) {
    if (idx < 0 || idx >= g_count) return g_rowH1;
    if (g_items[idx].hasImage) return g_rowH2;
    if (g_items[idx].lines > 1) return g_lineH * 2.0f + 8.0f * g_k;
    return g_rowH1;
}

static float ContentH(void) {
    float h = 0;
    for (int i = 0; i < g_count; i++) h += RowHeight(i);
    return h;
}

static float ViewH(void) { return (float)g_pxH - g_headerH; }

static void ClampScroll(void) {
    float max = ContentH() - ViewH();
    if (max < 0) max = 0;
    if (g_scrollY < 0) g_scrollY = 0;
    if (g_scrollY > (int)max) g_scrollY = (int)max;
}

/* 只在键盘导航时调用：把选中行滚进可见区。鼠标悬停**不**走这里 ——
   曾经悬停也滚，光标往下走窗口就自己跟着跑。 */
static void EnsureVisible(int idx) {
    if (idx < 0 || idx >= g_count) return;
    float top = 0;
    for (int i = 0; i < idx; i++) top += RowHeight(i);
    float bot = top + RowHeight(idx);
    if (top < (float)g_scrollY)                 g_scrollY = (int)top;
    else if (bot > (float)g_scrollY + ViewH())  g_scrollY = (int)(bot - ViewH());
    ClampScroll();
}

static void MoveSel(int delta) {
    if (g_count <= 0) return;
    int n = g_sel + delta;
    if (n < 0) n = 0;
    if (n > g_count - 1) n = g_count - 1;
    g_sel = n;
    EnsureVisible(n);
    Render();
}


#define BTN_COUNT 2   /* 0 = ✕（右上角），1 = Ditto（转本体；仅 g_dittoOk 时存在） */
static void BtnRect(int i, Gdiplus::RectF* out) {
    float s = 26.0f * g_k;
    out->Y = (g_headerH - s) * 0.5f;
    out->Height = s;
    if (i == 0) {                            /* ✕ 贴最右 */
        out->X = (float)g_pxW - s - 6.0f * g_k;
        out->Width = s;
    } else {                                 /* Ditto 在 ✕ 左边，宽一点放得下字 */
        out->Width = 56.0f * g_k;
        out->X = (float)g_pxW - s - 6.0f * g_k - 8.0f * g_k - out->Width;
    }
}

static int HitBtn(float x, float y) {
    for (int i = 0; i < BTN_COUNT; i++) {
        Gdiplus::RectF r;
        BtnRect(i, &r);
        if (x >= r.X && x <= r.X + r.Width && y >= r.Y && y <= r.Y + r.Height) return i;
    }
    return -1;
}

/* 命中测试：y -> 条目下标（绝对）；不在列表区返回 -1。
   从 0 号开始按行高累加（不依赖 g_scrollY 之外的任何缓存状态）。 */
static int RowAt(float y) {
    if (y < g_headerH || y >= (float)g_pxH) return -1;
    float acc = g_headerH - (float)g_scrollY;
    for (int i = 0; i < g_count; i++) {
        float h = RowHeight(i);
        if (y >= acc && y < acc + h) return i;
        acc += h;
        if (acc > (float)g_pxH) break;
    }
    return -1;
}

/* 某一行在屏幕（窗口客户区）上的 y；可见区上方返回负值也没关系，调用方会判。 */
static float RowTop(int idx) {
    float acc = g_headerH - (float)g_scrollY;
    for (int i = 0; i < idx; i++) acc += RowHeight(i);
    return acc;
}

/* ================= 绘制 =================
   一遍 GDI+（底色 / 圆角框 / 缩略图），Flush，再一遍 **GDI** 画全部文字。
   顺序不能反：GDI+ 与 GDI 写的是同一块 DIB，文字必须最后画才不会被底盖掉。 */

static COLORREF CR(unsigned argb) {
    return RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
}

static void GdiText(const WCHAR* s, HFONT f, COLORREF c, float x, float y,
                    float w, float h, UINT align, BOOL ellipsis) {
    if (!s || !*s || !f || !g_memDC || h <= 0 || w <= 0) return;
    RECT rc;
    rc.left   = (int)(x + 0.5f);
    rc.top    = (int)(y + 0.5f);
    rc.right  = (int)(x + w + 0.5f);
    rc.bottom = (int)(y + h + 0.5f);
    HGDIOBJ old = SelectObject(g_memDC, f);
    SetTextColor(g_memDC, c);
    SetBkMode(g_memDC, TRANSPARENT);
    UINT fl = DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER | align;
    if (ellipsis) fl |= DT_END_ELLIPSIS;
    DrawTextW(g_memDC, s, -1, &rc, fl);
    SelectObject(g_memDC, old);
}

/* alpha 的通用表示法（Ditto / Photoshop 一个路子）：先铺棋盘格，再把半透明的
   颜色压上去 —— 否则 #RRGGBBAA 里的 alpha 完全看不出来。 */
static void DrawCheckerboard(Gdiplus::Graphics* g, float x, float y, float w, float h) {
    Gdiplus::SolidBrush b1(Gdiplus::Color(0xFFFFFFFF));
    Gdiplus::SolidBrush b2(Gdiplus::Color(0xFFC8C8C8));
    float cell = (w >= 16.0f) ? 4.0f : 2.0f;
    for (float yy = y, iy = 0.0f; yy < y + h; yy += cell, iy += 1.0f) {
        for (float xx = x, ix = 0.0f; xx < x + w; xx += cell, ix += 1.0f) {
            BOOL odd = (((int)ix + (int)iy) & 1) != 0;
            g->FillRectangle(odd ? &b2 : &b1, Gdiplus::RectF(xx, yy, cell, cell));
        }
    }
}

static BOOL Render(void) {
    if (!g_memDC || !g_bmp) return FALSE;

    Gdiplus::Bitmap bmp(g_pxW, g_pxH, g_pxW * 4, PixelFormat32bppPARGB, (BYTE*)g_bits);
    if (bmp.GetLastStatus() != Gdiplus::Ok) return FALSE;
    Gdiplus::Graphics g(&bmp);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    g.Clear(Gdiplus::Color(0xFFFFFFFF));   /* 不透明白：ClearType 需要不透明背景 */

    Gdiplus::SolidBrush brWhite(Gdiplus::Color(C_BG));
    Gdiplus::SolidBrush brAlt(Gdiplus::Color(C_ROW_ALT));
    Gdiplus::SolidBrush brSel(Gdiplus::Color(C_SEL_BG));
    Gdiplus::SolidBrush brHead(Gdiplus::Color(C_HEAD));
    Gdiplus::SolidBrush brGut(Gdiplus::Color(C_THUMB_BG));
    Gdiplus::Pen penBorder(Gdiplus::Color(C_BORDER), 1.0f);

    float yList = g_headerH;
    float yEnd  = (float)g_pxH;   /* 底栏删了：列表区一直到窗口底 */

    /* ---- 标题栏（文字在下面 GDI 那一遍画）：纯白平涂 ---- */
    g.FillRectangle(&brHead, 0, 0, g_pxW, (int)g_headerH);
    g.DrawLine(&penBorder, 0.0f, g_headerH - 0.5f, (float)g_pxW, g_headerH - 0.5f);

    /* ---- 列表：底色 + 缩略图 ----
       ★ 必须裁剪到列表区：半截行滚进标题栏时，行底色按可见段画是安全的，
       但缩略图是**按整行居中**定位的，不裁剪就会伸进标题栏里（实测踩过）。 */
    g.SetClip(Gdiplus::RectF(0.0f, g_headerH, (float)g_pxW,
                             (float)g_pxH - g_headerH));
    for (int i = 0; i < g_count; i++) {
        float h = RowHeight(i);
        float top = RowTop(i);
        if (top + h <= yList || top >= yEnd) continue;   /* 完全在可见区外 */
        float drawY = top, drawH = h;
        if (drawY < yList) { drawH -= (yList - drawY); drawY = yList; }
        if (drawY + drawH > yEnd) drawH = yEnd - drawY;

        BOOL sel = (i == g_sel);
        BOOL alt = (i % 2) == 1;
        g.FillRectangle(sel ? &brSel : (alt ? &brAlt : &brWhite),
                        Gdiplus::RectF(0.0f, drawY, (float)g_pxW, drawH));
        if (g_items[i].hasImage) {
            float bx = g_gutterW + 4.0f * g_k;
            /* ★ 锁要一直持有到画完：GDI+ 的 Bitmap 不是 COM 对象、没有引用计数，
               不能靠 AddRef 借用。后台线程只在自己那把锁里换指针，
               这里最多等它一次解码的时间，不会卡住消息循环。 */
            EnterCriticalSection(&g_thumbLock);
            Gdiplus::Bitmap* bm = g_thumb[i];
            if (bm) {
                /* 缩略图在后台已经缩到目标尺寸：这里 1:1 贴，不做每帧缩放 */
                float bw = (float)bm->GetWidth(), bh = (float)bm->GetHeight();
                g.DrawImage(bm, bx, top + (h - bh) * 0.5f, bw, bh);
                g_thumbW[i] = bw;
            } else {
                /* 后台还没解码完：占位框（宽度按行高，载入后文字会自己让开） */
                float boxH = h - 2.0f * g_thumbPad;
                g.FillRectangle(&brGut, Gdiplus::RectF(bx, top + g_thumbPad, boxH, boxH));
                g_thumbW[i] = boxH;
            }
            LeaveCriticalSection(&g_thumbLock);
        }
        /* 色值条目：文字左侧一块颜色（照 Ditto）。
           尺寸比 Ditto 略收：Ditto 用满行高，我们一行里可能排两行字，
           满行高的方块太大，所以按缩略图那样上下各留一个 padding。 */
        if (g_items[i].hasSwatch) {
            float box = h - 2.0f * g_thumbPad;
            float bx  = g_gutterW + 6.0f * g_k;
            float by  = top + (h - box) * 0.5f;
            BYTE r = GetRValue(g_items[i].swatch);
            BYTE gg = GetGValue(g_items[i].swatch);
            BYTE bb = GetBValue(g_items[i].swatch);
            if (g_items[i].swatchAlpha < 255) {
                DrawCheckerboard(&g, bx, by, box, box);
                Gdiplus::SolidBrush br(Gdiplus::Color((BYTE)g_items[i].swatchAlpha,
                                                      r, gg, bb));
                g.FillRectangle(&br, Gdiplus::RectF(bx, by, box, box));
            } else {
                Gdiplus::SolidBrush br(Gdiplus::Color(255, r, gg, bb));
                g.FillRectangle(&br, Gdiplus::RectF(bx, by, box, box));
            }
            /* 描边：白底上的白色（#FFFFFF）不描就看不见 */
            g.DrawRectangle(&penBorder, bx, by, box, box);
        }
    }

    g.ResetClip();

    /* 边框最后画：压住可能溢出到边界外的行底色 */
    g.DrawRectangle(&penBorder, 0.5f, 0.5f, (float)g_pxW - 1.0f, (float)g_pxH - 1.0f);

    g.Flush(Gdiplus::FlushIntentionSync);

    /* ---- 第二遍：GDI 文字（ClearType）---- */
    {
        /* 标题：置顶时把状态带出来 —— 用户看不到别处有这个状态 */
        WCHAR title[64];
        wcscpy_s(title, 64, APP_TITLE);
        if (g_pinned) wcscat_s(title, 64, L" 窗口置顶");
        GdiText(title, g_fontRow, CR(C_TITLE),
                12.0f * g_k, 0.0f, (float)g_pxW - 120.0f * g_k, g_headerH,
                DT_LEFT, TRUE);

        Gdiplus::RectF r;
        BtnRect(0, &r);
        GdiText(L"✕", g_fontRow, CR(0xFF6E6E6E),
                r.X, r.Y, r.Width, r.Height, DT_CENTER, FALSE);
        if (g_dittoOk) {                 /* Ditto.exe 在才画：点了没反应的按钮是骗局 */
            Gdiplus::RectF rb;
            BtnRect(1, &rb);
            GdiText(L"Ditto", g_fontSmall, CR(0xFF6E6E6E),
                    rb.X, rb.Y, rb.Width, rb.Height, DT_CENTER, FALSE);
        }
    }

    if (g_count <= 0) {
        const WCHAR* msg = g_dbErr
            ? (g_dbPath[0] ? L"打不开 Ditto 数据库（检查 ini 的 DittoDir）"
                           : L"未配置 Ditto 文件夹：在 ini 的 DittoDir 里写路径")
            : L"（没有读到条目）";
        GdiText(msg, g_fontRow, CR(C_TEXT_DIM),
                g_gutterW + 8.0f * g_k, yList,
                (float)g_pxW - g_gutterW - 16.0f * g_k, g_rowH1 * 2.0f, DT_LEFT, TRUE);
    }
    /* 半截行照画的前提：文字全部裁剪到列表区，不许越进标题栏（标题已画在上面） */
    int textDC = SaveDC(g_memDC);
    IntersectClipRect(g_memDC, 0, (int)yList, g_pxW, (int)yEnd);
    for (int i = 0; i < g_count; i++) {
        float h = RowHeight(i);
        float top = RowTop(i);
        if (top + h <= yList || top >= yEnd) continue;
        WCHAR num[8];
        if (i < 9)       _snwprintf_s(num, 8, _TRUNCATE, L"%d", i + 1);
        else if (i == 9) wcscpy_s(num, 8, L"0");
        else             num[0] = 0;
        GdiText(num, g_fontSmall, CR(C_INDEX),
                0.0f, top, g_gutterW, h, DT_CENTER, FALSE);

        float tx = g_gutterW + 6.0f * g_k;
        if (g_items[i].hasImage)
            tx += g_thumbW[i] + 8.0f * g_k;   /* 让开缩略图**实际**的宽度 */
        if (g_items[i].hasSwatch)
            tx += (h - 2.0f * g_thumbPad) + 6.0f * g_k;   /* 让开色块 */
        if (g_items[i].hasImage && g_items[i].lines <= 1) {
            GdiText(g_items[i].text[0], g_fontRow, CR(C_TEXT),
                    tx, top, (float)g_pxW - tx - 8.0f * g_k, h, DT_LEFT, TRUE);
        } else if (g_items[i].lines > 1) {
            /* 两行**贴着行高**排，整块在行内垂直居中 —— Ditto 的行距就是这样，
               拿"行高 ÷ 2"当行距会让两行之间隔出一大条空白 */
            float y0 = top + (h - g_lineH * 2.0f) * 0.5f;
            GdiText(g_items[i].text[0], g_fontRow, CR(C_TEXT),
                    tx, y0, (float)g_pxW - tx - 8.0f * g_k, g_lineH, DT_LEFT, TRUE);
            GdiText(g_items[i].text[1], g_fontRow, CR(C_TEXT),
                    tx, y0 + g_lineH, (float)g_pxW - tx - 8.0f * g_k, g_lineH,
                    DT_LEFT, TRUE);
        } else {
            GdiText(g_items[i].text[0], g_fontRow, CR(C_TEXT),
                    tx, top, (float)g_pxW - tx - 8.0f * g_k, h, DT_LEFT, TRUE);
        }
    }
    RestoreDC(g_memDC, textDC);

    InvalidateRect(g_hwnd, NULL, FALSE);
    return TRUE;
}

static void MakeSurface(void) {
    if (g_bmp) return;
    HDC screen = GetDC(NULL);
    if (!screen) return;
    g_memDC = CreateCompatibleDC(screen);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = g_pxW;
    bi.bmiHeader.biHeight = -g_pxH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &g_bits, NULL, 0);
    ReleaseDC(NULL, screen);
    if (g_bmp && g_memDC) SelectObject(g_memDC, g_bmp);
}

static void PlaceWindow(void) {
    RECT work;
    MonitorWork(CursorMonitor(), &work);
    POINT cur;
    if (!GetCursorPos(&cur)) { cur.x = work.left; cur.y = work.top; }
    g_px = cur.x + (int)(12.0f * g_k);
    g_py = cur.y + (int)(12.0f * g_k);
    if (g_px + g_pxW > work.right)  g_px = work.right - g_pxW;
    if (g_py + g_pxH > work.bottom) g_py = work.bottom - g_pxH;
    if (g_px < work.left) g_px = work.left;
    if (g_py < work.top)  g_py = work.top;
}

static void DoHideClose(void) {
    HideIfVisible();
    SetTimer(g_hwnd, TID_GRACE, GRACE_MS, NULL);
}

/* 把 Ditto 的快速粘贴窗口拉到前台。
   ★ 为什么需要我们补刀：ShowQPasteWnd 末尾 Ditto 自己会 SetForegroundWindow，
   但前台转移有系统规则（"最近接收输入的进程"才有资格）—— 我们的链路里按键
   被自家钩子收走，长驻 Ditto 进程没收到任何输入，那个调用被系统拒绝，窗口就
   无激活地落在前台窗口**后面**（"被遮挡"，且是否成功取决于偶然满足的豁免
   条件，所以时灵时不灵）。Ditto 启动时调用了
   AllowSetForegroundWindow(ASFW_ANY)（CP_Main.cpp:147）—— 授权任何进程都
   可以把它拉到前台，所以由我们来调就一定成功。 */
static void RaiseDittoWindow(HANDLE hProc) {
    /* -open 子进程要等本体把窗口**显示完**才退出（它同步 SendMessage）：
       等它退出再找窗口，通常一击即中，不用长时间轮询 */
    if (hProc) WaitForSingleObject(hProc, 1500);
    for (int i = 0; i < 20; i++) {
        HWND w = FindWindowW(L"QPasteClass", NULL);
        if (w && IsWindowVisible(w)) {
            SetForegroundWindow(w);
            /* 激活失败也把 z 序顶到最上（双保险，不抢焦点） */
            SetWindowPos(w, HWND_TOP, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            return;
        }
        Sleep(30);
    }
}

/* 转 Ditto 本体（标题栏按钮）。用 Ditto 的官方命令行 `-open`：它向已运行实例
   的保存 HWND 发 WM_OPEN_CLOSE_WINDOW，由本体自己弹出快速粘贴窗口（位置与
   激活检查都是它自己的逻辑）。Ditto 没在跑时该命令是安全无操作 —— 新实例
   发不出消息就自行退出，不会替用户把本体拉起来。
   浮层是 NOACTIVATE，前台一直是目标应用，本体窗口弹出后焦点自然归它管。 */
static void DittoAction(void) {
    if (!g_dittoOk) return;
    WCHAR exe[MAX_PATH];
    _snwprintf_s(exe, MAX_PATH, _TRUNCATE, L"%s\\Ditto.exe", g_dirPath);
    WCHAR cmd[MAX_PATH + 16];
    _snwprintf_s(cmd, (size_t)(MAX_PATH + 16), _TRUNCATE, L"\"%s\" -open", exe);
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, g_dirPath, &si, &pi)) {
        RaiseDittoWindow(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    DoHideClose();                       /* 本体接手，浮层退场 */
}

static void DoShow(void) {
    KillTimer(g_hwnd, TID_GRACE);
    g_lastKa = GetTickCount64();
    SetTimer(g_hwnd, TID_KA, KA_POLL_MS, NULL);
    if (!g_visible) {
        PlaceWindow();                 /* ★ 只在这里定位 */
        MoveWindow(g_hwnd, g_px, g_py, g_pxW, g_pxH, FALSE);
        Render();
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        g_visible = TRUE;
        TipUpdate();          /* 窗口大小定下来之后才能挂 tooltip 的区域 */
    } else {
        Render();
    }
}

/* ---------- "点到窗口外面就关" ----------
   ★ 为什么不能只靠 SetWinEventHook(EVENT_SYSTEM_FOREGROUND)：用户点的是**本来就
   是前台**的那个程序（在记事本里点一下），前台根本没变，事件不会来 —— 于是
   "点外面"判不出来。所以再装一个低级鼠标钩子，只看落点是不是在自己身上。
   只判定、不吞事件（一律 CallNextHookEx）：浮层不该吃掉任何点击。 */
static LRESULT CALLBACK MouseProc(int nCode, WPARAM wp, LPARAM lp) {
    if (nCode == HC_ACTION) {
        MSLLHOOKSTRUCT* s = (MSLLHOOKSTRUCT*)lp;
        if ((wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN ||
             wp == WM_MBUTTONDOWN || wp == WM_NCXBUTTONDOWN) && g_visible) {
            RECT rc;
            GetWindowRect(g_hwnd, &rc);
            BOOL inside = (s->pt.x >= rc.left && s->pt.x < rc.right &&
                           s->pt.y >= rc.top  && s->pt.y < rc.bottom);
            if (!inside && !g_pinned && !g_pasting) DoHideClose();
        }
    }
    return CallNextHookEx(g_mouseHook, nCode, wp, lp);
}

static void CALLBACK WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                  LONG idObject, LONG idChild,
                                  DWORD idEventThread, DWORD dwmsEventTime) {
    (void)hook; (void)idObject; (void)idChild; (void)idEventThread; (void)dwmsEventTime;
    if (event != EVENT_SYSTEM_FOREGROUND) return;
    if (!hwnd || hwnd == g_hwnd) return;
    g_target = hwnd;
    /* 真的切到别的程序了（比"点击"更强的一条）：同样关掉 */
    if (g_visible && !g_pinned && !g_pasting) DoHideClose();
}

static LRESULT CALLBACK ClipProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_msgShow && msg == g_msgShow) { DoShow(); return 0; }
    if (g_msgHide && msg == g_msgHide) { DoHideClose(); return 0; }
    if (g_msgKa   && msg == g_msgKa)   { g_lastKa = GetTickCount64(); return 0; }
    if (g_msgEsc  && msg == g_msgEsc)  { DoHideClose(); return 0; }
    if (msg == WM_THUMB_READY) { Render(); return 0; }
    if (g_msgKey  && msg == g_msgKey) {
        int vk = (int)wp;
        BOOL shift = (lp & 0x1) != 0;
        if (vk == VK_RETURN)      { DoPaste(shift); return 0; }
        if (vk == VK_UP)          { MoveSel(-1); return 0; }
        if (vk == VK_DOWN)        { MoveSel(1); return 0; }
        if (vk == VK_PRIOR)       { MoveSel(-ROWS_VISIBLE); return 0; }
        if (vk == VK_NEXT)        { MoveSel(ROWS_VISIBLE); return 0; }
        if (vk >= '0' && vk <= '9') {
            int i = (vk == '0') ? 9 : (vk - '1');
            if (i < g_count) { g_sel = i; EnsureVisible(i); Render(); DoPaste(shift); }
            return 0;
        }
        return 0;
    }
    switch (msg) {
    case WM_TIMER:
        if (wp == TID_LIFE)  { DoHideClose(); return 0; }
        if (wp == TID_GRACE) { DestroyWindow(hwnd); return 0; }
        if (wp == TID_TIP)  { TipHide(); return 0; }   /* Ditto 的 ToolTipTimeout */
        if (wp == TID_KA) {
            if (g_visible && GetTickCount64() - g_lastKa > HELP_KA_TIMEOUT_MS)
                DestroyWindow(hwnd);
            return 0;
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (g_memDC && dc)
            BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top,
                   ps.rcPaint.right - ps.rcPaint.left,
                   ps.rcPaint.bottom - ps.rcPaint.top,
                   g_memDC, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;                       /* 内容全来自双缓冲，别让系统擦出白闪 */
    case WM_MOUSEMOVE: {
        /* ---- 拖标题栏挪窗口 ----
           置顶／不置顶都允许拖：拖动是"这个窗口在哪"的事，与"要不要自动关"
           无关。窗口是 NOACTIVATE，收不到 WM_NCLBUTTONDOWN，所以拖动只能自己
           用 SetCapture 做 —— 没有系统送的 WM_SYSCOMMAND/SC_MOVE 可白拿。 */
        if (g_dragging && (wp & MK_LBUTTON)) {
            POINT cur;
            if (GetCursorPos(&cur)) {
                RECT work;
                MonitorWork(MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST), &work);
                g_px = g_dragWin.x + cur.x - g_dragCur.x;
                g_py = g_dragWin.y + cur.y - g_dragCur.y;
                if (g_px < work.left) g_px = work.left;
                if (g_py < work.top)  g_py = work.top;
                if (g_px + g_pxW > work.right)  g_px = work.right - g_pxW;
                if (g_py + g_pxH > work.bottom) g_py = work.bottom - g_pxH;
                MoveWindow(g_hwnd, g_px, g_py, g_pxW, g_pxH, FALSE);
            }
            return 0;
        }
        float y = (float)(short)HIWORD(lp);
        int row = RowAt(y);
        /* 只改选中，**不滚动** —— 悬停滚动会让窗口跟着光标跑 */
        if (row >= 0 && row != g_sel) { g_sel = row; Render(); }
        /* 悬停提示跟着走：行变了换提示，移出列表就收。
           ClientToScreen 用消息里的客户坐标，别用 GetCursorPos ——
           两者到达时间有差，会弹到上一次经过的位置。 */
        if (row >= 0) {
            if (g_tipRow != row) {
                POINT scr = { (short)LOWORD(lp), (short)HIWORD(lp) };
                ClientToScreen(g_hwnd, &scr);
                TipShowAt(row, scr);
            }
        } else if (g_tipRow >= 0) {
            TipHide();
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        float x = (float)(short)LOWORD(lp), y = (float)(short)HIWORD(lp);
        int hb = HitBtn(x, y);
        if (hb == 0) { DoHideClose(); return 0; }   /* ✕ */
        if (hb == 1) { DittoAction(); return 0; }   /* Ditto：转本体 */
        if (y >= g_headerH && g_clickPaste) {
            int row = RowAt(y);
            if (row >= 0) {                        /* 单击即粘（Ditto 同款）：挑中就上 */
                g_sel = row;
                /* Shift 与否看消息里的 MK_SHIFT，不查 GetAsyncKeyState：
                   NOACTIVATE 窗口没有键盘焦点，异步采样和消息到达之间有竞态，
                   松 Shift 早一点就会被误判成普通粘贴。 */
                DoPaste((wp & MK_SHIFT) != 0);
            }
            return 0;
        }
        if (y < g_headerH) {
            /* 按下标题栏：进入拖动。双击置顶由 WM_LBUTTONDBLCLK 负责
               （CS_DBLCLKS 下第二次按下送的是 DBLCLK，不会再走这里）。 */
            g_dragging = TRUE;
            SetCapture(hwnd);
            GetCursorPos(&g_dragCur);
            g_dragWin.x = g_px;
            g_dragWin.y = g_py;
            return 0;
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_dragging) {
            g_dragging = FALSE;
            if (GetCapture() == hwnd) ReleaseCapture();
        }
        return 0;
    case WM_CAPTURECHANGED:
        g_dragging = FALSE;
        return 0;
    case WM_LBUTTONDBLCLK: {
        float x = (float)(short)LOWORD(lp), y = (float)(short)HIWORD(lp);
        (void)x;
        if (y < g_headerH) {                /* 双击标题栏 = 置顶开关 */
            g_pinned = !g_pinned;
            Render();
            return 0;
        }
        int row = RowAt(y);
        if (row >= 0) { g_sel = row; DoPaste((wp & MK_SHIFT) != 0); }   /* 同上：MK_SHIFT 权威 */
        return 0;
    }
    case WM_MOUSEWHEEL: {
        if (g_count > 0) {
            int d = GET_WHEEL_DELTA_WPARAM(wp);
            /* 按像素滚：行高不一时按"几行"滚会跳，按像素才跟手。
               一档 = 3 个单行高（系统默认也是三行） */
            g_scrollY -= (d / WHEEL_DELTA) * (int)(g_rowH1 * 3.0f);
            ClampScroll();
            Render();
            TipHide();                    /* 行位全变了，旧提示已经指错行 */
        }
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR* nm = (NMHDR*)lp;
        if (g_tip && nm->hwndFrom == g_tip && nm->code == TTN_NEEDTEXTW) {
            NMTTDISPINFOW* di = (NMTTDISPINFOW*)lp;
            TipTextOf(g_sel);          /* 悬停即选中，所以问的就是当前选中行 */
            di->lpszText = g_tipBuf[0] ? g_tipBuf : NULL;   /* 空 = 不弹 */
            return TRUE;
        }
        break;
    }
    case WM_DESTROY:
        TipDestroy();
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void DpiAware(void) {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (!u) return;
    typedef BOOL (WINAPI* PFN_SetCtx)(HANDLE);
    PFN_SetCtx p = (PFN_SetCtx)(void*)GetProcAddress(u, "SetProcessDpiAwarenessContext");
    if (p) { p((HANDLE)(INT_PTR)-4); return; }
    typedef BOOL (WINAPI* PFN_SetAware)(void);
    PFN_SetAware q = (PFN_SetAware)(void*)GetProcAddress(u, "SetProcessDPIAware");
    if (q) q();
}

/* <DittoDir>\Ditto.db：数据库与 exe 同文件夹，DB 路径永远是派生值，不单独配置。 */
static void BuildDbPath(void) {
    if (!g_dirPath[0]) return;
    _snwprintf_s(g_dbPath, DITTO_DIR_MAX, _TRUNCATE, L"%s\\Ditto.db", g_dirPath);
}

/* ================= Ditto 的设置（便携版：<DittoDir>\Ditto.Settings） =================
   便携版 Ditto 把自己的设置写在这个文件里 —— **UTF-16 LE 带 BOM 的 INI**，
   不是注册表。我们只读关心的四项，读不到就用 Ditto 自己的默认值：
       DrawCopiedColorCode  1     色值条目画色块
       MaxToolTipCharacters 1000  悬停提示的字符上限
       MaxToolTipLines      30    悬停提示的行数上限
       ToolTipTimeout       -1    -1 = 默认 5 秒，0 = 不显示
   ★★ 这些键在用户的 Ditto.Settings 里通常**根本不存在**（Ditto 只在改过时才写），
      所以"读不到"是常态，不是失败 —— 别把它记成错误。
   ★ 不把它们做成 CapsEnhance.ini 的配置项：开关应该只有一处（在 Ditto 里改），
      两边各有一个开关、值还不一致，是最难排查的那类问题（也省掉"八处"改动）。 */
/* 在一段 UTF-16 文本里按行找 key=value */
static BOOL DsFindInt(const WCHAR* txt, size_t nch, const WCHAR* key, int* out) {
    size_t kl = wcslen(key);
    for (size_t i = 0; i < nch; ) {
        size_t j = i;
        while (j < nch && txt[j] != L'\n') j++;
        size_t s = i;
        while (s < j && (txt[s] == L' ' || txt[s] == L'\t' || txt[s] == L'\r')) s++;
        if (s + kl < j && _wcsnicmp(txt + s, key, kl) == 0) {
            size_t e = s + kl;
            while (e < j && (txt[e] == L' ' || txt[e] == L'\t')) e++;
            if (e < j && txt[e] == L'=') { *out = _wtoi(txt + e + 1); return TRUE; }
        }
        i = (j < nch) ? j + 1 : nch;
    }
    return FALSE;
}

static void DsLoad(void) {
    if (!g_dirPath[0]) return;
    WCHAR path[DITTO_DIR_MAX + 32];
    _snwprintf_s(path, (int)(sizeof(path) / sizeof(path[0])), _TRUNCATE,
                 L"%s\\Ditto.Settings", g_dirPath);
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    static BYTE raw[262144];
    DWORD got = 0;
    BOOL rd = ReadFile(f, raw, (DWORD)sizeof(raw) - 2, &got, NULL);
    CloseHandle(f);
    if (!rd || got < 4) return;
    if (!(raw[0] == 0xFF && raw[1] == 0xFE)) return;   /* 只认 UTF-16 LE */
    raw[got] = 0; raw[got + 1] = 0;
    const WCHAR* txt = (const WCHAR*)(raw + 2);
    size_t nch = (size_t)((got - 2) / 2);
    int v = 0;
    if (DsFindInt(txt, nch, L"DrawCopiedColorCode", &v)) g_dc.drawColorCode = (v != 0);
    if (DsFindInt(txt, nch, L"MaxToolTipCharacters", &v) && v > 0)  g_dc.tipChars = v;
    if (DsFindInt(txt, nch, L"MaxToolTipLines", &v) && v > 0)       g_dc.tipLines = v;
    if (DsFindInt(txt, nch, L"ToolTipTimeout", &v))                 g_dc.tipTimeoutMs = v;
}

/* 手动双击（不带 --dir）时兜底读一次 ini：与 help/help.c 读 ImeCapsMode 同一个
   理由 —— 浮层不链主程序任何模块，而路径的权威解析在 config.c。 */
static void IniReadString(const char* buf, size_t len, const char* key,
                          WCHAR* out, int cap) {
    size_t kl = strlen(key);
    for (size_t i = 0; i < len; ) {
        size_t j = i;
        while (j < len && buf[j] != '\n') j++;
        size_t s = i;
        while (s < j && (buf[s] == ' ' || buf[s] == '\t')) s++;
        if (s + kl < j && _strnicmp(buf + s, key, kl) == 0) {
            size_t e = s + kl;
            while (e < j && (buf[e] == ' ' || buf[e] == '\t')) e++;
            if (e < j && buf[e] == '=') {
                e++;
                while (e < j && (buf[e] == ' ' || buf[e] == '\t')) e++;
                size_t vlen = j - e;
                while (vlen > 0 && (buf[e + vlen - 1] == '\r' || buf[e + vlen - 1] == ' ')) vlen--;
                if (vlen > 0) {
                    int n = MultiByteToWideChar(CP_UTF8, 0, buf + e, (int)vlen, out, cap - 1);
                    if (n > 0) { out[n] = 0; return; }
                }
            }
        }
        i = (j < len) ? j + 1 : len;
    }
}

static void LoadDbPathFromIni(void) {
    WCHAR path[MAX_PATH];
    if (!GetModuleFileNameW(NULL, path, MAX_PATH)) return;
    WCHAR* slash = wcsrchr(path, L'\\');
    if (!slash) return;
    size_t off = (size_t)(slash + 1 - path);
    if (off + 16 >= MAX_PATH) return;
    wcscpy_s(slash + 1, MAX_PATH - off, L"CapsEnhance.ini");
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    static char buf[16384];
    DWORD got = 0;
    if (ReadFile(f, buf, sizeof(buf) - 1, &got, NULL) && got) {
        buf[got] = 0;
        IniReadString(buf, got, "DittoDir", g_dirPath, DITTO_DIR_MAX);
    }
    CloseHandle(f);
    BuildDbPath();
}

int APIENTRY wWinMain(_In_ HINSTANCE hInst, _In_opt_ HINSTANCE hPrev,
                      _In_ LPWSTR cmdLine, _In_ int show) {
    (void)hPrev; (void)show;

    g_msgShow = RegisterWindowMessageW(CLIP_MSG_SHOW);
    g_msgHide = RegisterWindowMessageW(CLIP_MSG_HIDE);
    g_msgKa   = RegisterWindowMessageW(CLIP_MSG_KA);
    g_msgEsc  = RegisterWindowMessageW(CLIP_MSG_ESC);
    g_msgKey  = RegisterWindowMessageW(CLIP_MSG_KEY);

    if (cmdLine) {
        const WCHAR* p = wcsstr(cmdLine, L"--dir");
        if (p) {
            p += 5;
            while (*p == L' ') p++;
            if (*p == L'"') {
                p++;
                const WCHAR* e = wcschr(p, L'"');
                size_t n = e ? (size_t)(e - p) : wcslen(p);
                if (n >= DITTO_DIR_MAX) n = DITTO_DIR_MAX - 1;
                wcsncpy_s(g_dirPath, DITTO_DIR_MAX, p, n);
            }
        }
    }
    if (!g_dirPath[0]) LoadDbPathFromIni();
    BuildDbPath();
    DsLoad();                       /* Ditto.Settings：读不到就全用 Ditto 的默认值 */
    /* "转 Ditto 本体"按钮只在 Ditto.exe 真的存在时才画/才响应 —— 靠猜的
       按钮点了没反应是最差的那种 UI。检测一次就够：一次会话里目录不会变。 */
    {
        WCHAR dx[MAX_PATH];
        _snwprintf_s(dx, MAX_PATH, _TRUNCATE, L"%s\\Ditto.exe", g_dirPath);
        g_dittoOk = g_dirPath[0] && GetFileAttributesW(dx) != INVALID_FILE_ATTRIBUTES;
    }

    /* --font "<字体名>" --size <磅>：主程序传进来的（配置的权威解析在 config.c）。
       没传就用手动启动时的 ini 兜底；再没有就用上面的内置默认。 */
    if (cmdLine) {
        const WCHAR* pf = wcsstr(cmdLine, L"--font");
        if (pf) {
            pf += 6;
            while (*pf == L' ') pf++;
            if (*pf == L'"') {
                pf++;
                const WCHAR* e = wcschr(pf, L'"');
                size_t n = e ? (size_t)(e - pf) : wcslen(pf);
                if (n >= CLIP_FONT_MAX) n = CLIP_FONT_MAX - 1;
                wcsncpy_s(g_fontName, CLIP_FONT_MAX, pf, n);
            }
        }
        const WCHAR* ps = wcsstr(cmdLine, L"--size");
        if (ps) {
            int v = _wtoi(ps + 6);
            if (v >= 6 && v <= CLIP_FONT_SIZE_MAX) g_fontPt = v;
        }
        const WCHAR* pc = wcsstr(cmdLine, L"--click");
        if (pc) g_clickPaste = (_wtoi(pc + 7) != 0);
    }
    if (!g_fontName[0]) {
        /* 手动启动（没人传 --font）时兜底读一次 ini 的 ClipFont */
        WCHAR path2[MAX_PATH];
        if (GetModuleFileNameW(NULL, path2, MAX_PATH)) {
            WCHAR* sl2 = wcsrchr(path2, L'\\');
            if (sl2) {
                wcscpy_s(sl2, MAX_PATH - (size_t)(sl2 - path2), L"\\CapsEnhance.ini");
                HANDLE f2 = CreateFileW(path2, GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
                if (f2 != INVALID_HANDLE_VALUE) {
                    static char buf2[16384];
                    DWORD got2 = 0;
                    if (ReadFile(f2, buf2, sizeof(buf2) - 1, &got2, NULL) && got2) {
                        buf2[got2] = 0;
                        IniReadString(buf2, got2, "ClipFont", g_fontName, CLIP_FONT_MAX);
                    }
                    CloseHandle(f2);
                }
            }
        }
    }

    HWND prev = FindWindowW(CLIP_CLASS, NULL);
    if (prev) {
        if (g_msgShow) PostMessageW(prev, g_msgShow, 0, 0);
        return 0;
    }

    DpiAware();
    Gdiplus::GdiplusStartupInput gsi;
    ULONG_PTR token = 0;
    if (Gdiplus::GdiplusStartup(&token, &gsi, NULL) != Gdiplus::Ok) return 1;

    memset(g_thumb, 0, sizeof(g_thumb));
    memset(g_thumbTried, 0, sizeof(g_thumbTried));
    DbLoadItems();
    FitMetrics();
    MakeSurface();

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ClipProc;
    wc.hInstance = hInst;
    wc.lpszClassName = CLIP_CLASS;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.style = CS_DBLCLKS;
    if (!RegisterClassExW(&wc)) { Gdiplus::GdiplusShutdown(token); return 1; }

    /* 普通（非分层）窗口：ClearType 需要不透明的目标，分层窗口只能走灰阶抗锯齿。
       不抢焦点靠 WS_EX_NOACTIVATE，与是否分层无关。
       WS_EX_LAYERED 在这里**只**用来做整窗 5% 透明（LWA_ALPHA），内容照常走
       GDI / WM_PAINT —— 透明是合成期的事，不影响文字怎么画。 */
    g_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                             CLIP_CLASS, APP_TITLE, WS_POPUP,
                             0, 0, g_pxW, g_pxH, NULL, NULL, hInst, NULL);
    if (!g_hwnd) { Gdiplus::GdiplusShutdown(token); return 1; }
    SetLayeredWindowAttributes(g_hwnd, 0, CLIP_ALPHA, LWA_ALPHA);

    InitializeCriticalSection(&g_thumbLock);
    g_thumbThread = CreateThread(NULL, 0, ThumbThreadProc, NULL, 0, NULL);

    g_target = GetForegroundWindow();
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, NULL, 0);
    g_evHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                               NULL, WinEventProc, 0, 0,
                               WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    DoShow();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_thumbThread) {
        InterlockedExchange(&g_thumbQuit, 1);
        WaitForSingleObject(g_thumbThread, 3000);
        CloseHandle(g_thumbThread);
        g_thumbThread = NULL;
    }
    if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
    if (g_evHook) UnhookWinEvent(g_evHook);
    if (g_fontRow)   DeleteObject(g_fontRow);
    if (g_fontSmall) DeleteObject(g_fontSmall);
    FreeThumbs();
    DeleteCriticalSection(&g_thumbLock);
    FreeFmts();
    if (g_bmp) DeleteObject(g_bmp);
    if (g_memDC) DeleteDC(g_memDC);
    Gdiplus::GdiplusShutdown(token);
    return 0;
}
