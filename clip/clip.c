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
#include <math.h>       /* sqrt：提示圆角羽化的距离场 */
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
   剪贴板历史里最近的 100 条，单击 / 双击 / 回车粘贴（单击可在设置里关掉），
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
#define CLIP_MAX_ITEMS 100        /* 从库里取的条目上限（SQL 的 LIMIT 也由它拼出） */
/* 把数字宏变成字符串字面量：LIMIT 与数组尺寸共用同一个真相，不会漂 */
#define CLIP_STR_(x) #x
#define CLIP_STR(x)  CLIP_STR_(x)
#define CLIP_TEXT_MAX 160

/* ---------- 条目 ---------- */
typedef struct {
    long long id;
    int       lines;              /* 1 或 2 */
    BOOL      hasImage;
    BOOL      hasSwatch;          /* 这条文本本身就是个色值 -> 左侧画色块 */
    COLORREF  swatch;
    int       swatchAlpha;        /* <255：先画棋盘格，再按 alpha 混合 */
    long long ldate;                /* Main.lDate（unix 秒），提示头部的时间行 */
    long long lastpaste;            /* Main.lastPasteDate；0 = 没用过 */
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
static UINT      g_msgShow = 0, g_msgHide = 0, g_msgEsc = 0, g_msgKey = 0;
static UINT      g_msgClosed = 0;
static UINT      g_msgBorn = 0;      /* 出生报信（-> 主程序），见 src/cap_overlay.h */
static BOOL      g_visible = FALSE;
static HDC       g_memDC = NULL;
static HBITMAP   g_bmp = NULL;
static void*     g_bits = NULL;
static int       g_pxW = 0, g_pxH = 0;
static int       g_px = 0, g_py = 0;
static float     g_k = 1.0f;
static float     g_headerH, g_gutterW, g_rowH1, g_rowH2, g_thumbPad;
static float     g_lineH = 20.0f;   /* 一行文字的行高（按字号算，见 FitMetrics） */
static HFONT     g_fontRow = NULL, g_fontSmall = NULL;
static HFONT     g_tipFont = NULL;        /* 提示字体：比列表小一档（Ditto 的提示也不用列表字号） */
static HWND      g_target = NULL;
static HWINEVENTHOOK g_evHook = NULL;
static HHOOK     g_mouseHook = NULL;
static BOOL      g_pasting = FALSE;
static BOOL      g_dragging = FALSE;   /* 正在拖标题栏挪窗口 */
static POINT     g_dragCur;            /* 按下那一刻的光标位置 */
static POINT     g_dragWin;            /* 按下那一刻的窗口位置 */

/* 寿命（2026-09-21 起本进程不再有心跳；完整取舍见 src/child.c 的文件头）：
   **2026-09-26 起本进程没有"到期退出"这回事了** —— 收起后永久驻留，直到主程序退出
   由 Job Object 收走。原来那个 GRACE_MS（待命 10 分钟）被删掉，因为它恰好在最坏的
   时机制造一次冷启动：用户隔十几分钟没用剪贴板，下一次按就要重新付一遍启动成本
   （杀软扫 1.67MB 未签名 exe、装载静态 sqlite3mc、起 GDI+），而这笔钱本来已经付过。
   主程序只是**卡住**时同样没有别的退路 —— 但它是可见的、鼠标能关（这条取舍没变）。
   手动双击起来的那份没有主程序喂它，也据此常驻：**关掉它的唯一办法是右击托盘退出
   主程序**（job 会把它一起收掉），这正是原来那份由 job 保证的行为。 */
#define TID_TIP   6    /* 悬停提示的自动消失（TRACK 模式不会自己收） */
#define TID_TIPSHOW 7  /* 悬停初始延迟：停稳 500ms 才弹（Ditto/系统同款） */
#define TID_TIPFADE 8  /* 提示窗口渐显/渐隐的步进时钟 */
#define TIP_DELAY_MS 500

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

static void FreeThumbs(void);   /* 706 行：DbLoadItems 重载条目后按需作废缩略图 */
/* 解码线程的唤醒事件与换代号：DbLoadItems 判定条目集变了就换代 + SetEvent，
   常驻的解码线程据此重做一轮。定义放这儿，因为 DbLoadItems 自己要用。 */
static HANDLE        g_thumbWake = NULL;   /* 自动重置事件：有新活/退出 */
static volatile LONG g_thumbGen  = 0;      /* 条目重载换代：换一次作废整轮 */

static void DbLoadItems(void) {
    /* 旧条目的 lID 序列：加载完对比 —— 条目集没变就复用缩略图缓存
       （驻留期每次显示都重读文本，图片解码贵，没变就不重解）。 */
    long long oldIds[CLIP_MAX_ITEMS];
    int oldCount = g_count;
    for (int i = 0; i < oldCount; i++) oldIds[i] = g_items[i].id;

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
    /* 提示头部要 lDate / lastPasteDate。lastPasteDate 在很老的库里可能没有，
       prepare 失败就退回不带它的旧查询（lastpaste 记 0，头部显示"-"）。 */
    const char* sql =
        "SELECT m.lID, m.mText, m.QuickPasteText, "
        "       (SELECT group_concat(d.strClipBoardFormat, ',') FROM Data d "
        "        WHERE d.lParentID = m.lID), "
        "       m.lDate, m.lastPasteDate "
        "FROM Main m "
        "WHERE IFNULL(m.bIsGroup, 0) = 0 "
        "ORDER BY COALESCE(m.stickyClipOrder, -2147483647) DESC, "
        "         COALESCE(m.clipOrder, m.lDate) DESC "
        "LIMIT " CLIP_STR(CLIP_MAX_ITEMS);
    const char* sqlOld =
        "SELECT m.lID, m.mText, m.QuickPasteText, "
        "       (SELECT group_concat(d.strClipBoardFormat, ',') FROM Data d "
        "        WHERE d.lParentID = m.lID) "
        "FROM Main m "
        "WHERE IFNULL(m.bIsGroup, 0) = 0 "
        "ORDER BY COALESCE(m.stickyClipOrder, -2147483647) DESC, "
        "         COALESCE(m.clipOrder, m.lDate) DESC "
        "LIMIT " CLIP_STR(CLIP_MAX_ITEMS);
    int hasDate = 1;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        hasDate = 0;
        if (sqlite3_prepare_v2(db, sqlOld, -1, &st, NULL) != SQLITE_OK) {
            sqlite3_close(db);
            g_dbErr = TRUE;
            return;
        }
    }
    while (sqlite3_step(st) == SQLITE_ROW && g_count < CLIP_MAX_ITEMS) {
        ClipItem* it = &g_items[g_count];
        memset(it, 0, sizeof(*it));
        it->id = sqlite3_column_int64(st, 0);
        const WCHAR* mt = (const WCHAR*)sqlite3_column_text16(st, 1);
        const WCHAR* qt = (const WCHAR*)sqlite3_column_text16(st, 2);
        const WCHAR* fm = (const WCHAR*)sqlite3_column_text16(st, 3);
        it->ldate     = hasDate ? sqlite3_column_int64(st, 4) : 0;
        it->lastpaste = hasDate ? sqlite3_column_int64(st, 5) : 0;
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
    BOOL same = (oldCount == g_count);
    if (same)
        for (int i = 0; i < g_count; i++)
            if (g_items[i].id != oldIds[i]) { same = FALSE; break; }
    if (!same) {
        FreeThumbs();
        g_sel = 0;  g_scrollY = 0;
        /* 换代 + 唤醒：叫解码线程按新条目重做一轮（不叫醒就是"再开全灰"） */
        InterlockedIncrement(&g_thumbGen);
        if (g_thumbWake) SetEvent(g_thumbWake);
    }
}

/* ================= 悬停提示（照 Ditto 的 CToolTipEx：自绘窗口） =================
   前两版用系统 tooltip 控件（TTF_TRACK 手动模式），能用，但观感"硬"：
   弹/收是瞬时的，文本切换也是硬切。Ditto 自己的提示（CToolTipEx，参考
   仓库 1665 行）根本不是 tooltip 控件，而是一个自绘 WS_POPUP 窗口 ——
   这才是它"软"的来源。本版照同一架构：
     · 载体：自绘分层窗口（WS_EX_LAYERED），圆角白底 + #CCC 边框，
       UpdateLayeredWindow 逐像素 alpha，边缘 1.5px 羽化；
     · 字体：浮层的行字体（Ditto 的 CToolTipEx 同样用列表字体）；
     · 软感：显示 ~90ms 渐显、收起渐隐（TID_TIPFADE 步进，每步重发一次
       ULW，BLENDFUNCTION.SourceConstantAlpha 当全局乘数，不用改位图）；
     · 文本：TipTextOf 产出的"正文 + 尾部时间行"（含按需回库取全文）；
     · 节奏：停稳 500ms 才弹（TID_TIPSHOW）；已显示时上下移动立即切换；
       超时（TID_TIP，Ditto 的 ToolTipTimeout）渐隐收场。
   系统 tooltip 路线的教训留档：无清单进程 = comctl32 v5，TTM_ADDTOOLW
   传 v6 sizeof 静默失败（须 V1_SIZE）；v5 的 TTF_SUBCLASS 在 NOACTIVATE
   窗口上不触发；TTM_POP 对 TRACK 工具无效。 */
static HWND  g_tip = NULL;
static int   g_tipAlpha = 0;         /* 0..255，渐显渐隐的当前值 */
static int   g_tipAlphaDir = 0;      /* 0 不动；+1 渐显；-1 渐隐 */
static int   g_tipRow = -1;          /* 提示当前指向的行；-1 = 没在显示 */
static int   g_tipW = 0, g_tipH = 0;
static HDC     g_tipDC = NULL;       /* 内容 DIB（含 alpha），ULW 的源 */
static HBITMAP g_tipBmp = NULL;
static void*  g_tipBits = NULL;
static POINT  g_tipPos = { 0, 0 };   /* 当前显示位置（屏幕） */
static WCHAR  g_tipBuf[4096];
typedef struct { const WCHAR* s; int len; } TipLine;

static LRESULT CALLBACK TipWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcW(h, m, w, l);   /* 内容全走 ULW，窗口没有绘制职责 */
}

static int  TipWrapInPlace(const WCHAR* text, HDC dc, int maxW, TipLine* ln, int maxLn);
static void TipTextOf(int idx);
static void MonitorWork(HMONITOR mon, RECT* out);   /* 定义在 winmove 段 */

static void TipEnsureWindow(void) {
    if (g_tip) return;
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = TipWndProc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.lpszClassName = L"CapsEnhanceClipTip";
    RegisterClassW(&wc);
    /* TRANSPARENT：鼠标穿透 —— 提示万一压到光标，悬停选中也不中断 */
    g_tip = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW |
                            WS_EX_TOPMOST | WS_EX_TRANSPARENT,
                            wc.lpszClassName, NULL, WS_POPUP,
                            0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
}

static void TipDestroy(void) {
    if (g_tipDC)  { DeleteDC(g_tipDC);    g_tipDC  = NULL; }
    if (g_tipBmp) { DeleteObject(g_tipBmp); g_tipBmp = NULL; }
    g_tipBits = NULL; g_tipW = g_tipH = 0; g_tipAlpha = 0; g_tipAlphaDir = 0;
    if (g_tip)    { DestroyWindow(g_tip);  g_tip    = NULL; }
    g_tipRow = -1;
}

/* 把内容画进 DIB：白底圆角 + #CCC 边框 + 黑字（浮层行字体），
   alpha 按几何后处理 —— GDI 不写 alpha，且 ULW(AC_SRC_ALPHA) 要求预乘。 */
static BOOL TipBuild(int row) {
    TipTextOf(row);
    if (!g_tipBuf[0]) return FALSE;

    HDC m = CreateCompatibleDC(NULL);
    HGDIOBJ of = SelectObject(m, g_tipFont);
    TEXTMETRICW tmv;
    GetTextMetricsW(m, &tmv);
    int lineH = tmv.tmHeight + tmv.tmExternalLeading;   /* 自然行距 */
    if (lineH < tmv.tmHeight + 2) lineH = tmv.tmHeight + 2;
    int maxW = (int)(500 * g_k + 0.5f);
    TipLine ln[240];
    int n = TipWrapInPlace(g_tipBuf, m, maxW, ln, 240);
    int textW = 0;
    for (int i = 0; i < n; i++) {
        SIZE sz;
        GetTextExtentPoint32W(m, ln[i].s, ln[i].len, &sz);
        if (sz.cx > textW) textW = sz.cx;
    }
    SelectObject(m, of);
    DeleteDC(m);
    if (n <= 0) return FALSE;

    int padX = (int)(10 * g_k + 0.5f), padY = (int)(6 * g_k + 0.5f);
    int newW = textW + (padX + 1) * 2;
    int newH = n * lineH + (padY + 1) * 2;

    if (g_tipDC && (newW != g_tipW || newH != g_tipH)) {
        DeleteDC(g_tipDC);  DeleteObject(g_tipBmp);
        g_tipDC = NULL;     g_tipBmp = NULL;
    }
    g_tipW = newW;  g_tipH = newH;
    if (!g_tipDC) {
        BITMAPINFO bi;
        memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = g_tipW;
        bi.bmiHeader.biHeight      = -g_tipH;      /* 自上而下 */
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        g_tipDC  = CreateCompatibleDC(NULL);
        g_tipBmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &g_tipBits, NULL, 0);
        if (!g_tipBmp || !g_tipBits) {
            if (g_tipDC)  { DeleteDC(g_tipDC);      g_tipDC  = NULL; }
            if (g_tipBmp) { DeleteObject(g_tipBmp); g_tipBmp = NULL; }
            g_tipBits = NULL;
            return FALSE;
        }
        SelectObject(g_tipDC, g_tipBmp);
    }

    HDC dc = g_tipDC;
    RECT rc = { 0, 0, g_tipW, g_tipH };
    HBRUSH bg = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    int rad = (int)(5 * g_k + 0.5f);  if (rad < 2) rad = 2;
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(204, 204, 204));
    HGDIOBJ open_ = SelectObject(dc, pen);
    HGDIOBJ obr  = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, 0, 0, g_tipW, g_tipH, rad * 2, rad * 2);
    SelectObject(dc, open_);
    SelectObject(dc, obr);
    DeleteObject(pen);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0, 0, 0));
    HGDIOBJ ofont = SelectObject(dc, g_tipFont);
    for (int i = 0; i < n; i++)
        TextOutW(dc, padX + 1, padY + 1 + i * lineH, ln[i].s, ln[i].len);
    SelectObject(dc, ofont);

    /* alpha：圆角内 255、边缘 1.5px 羽化；RGB 按 a 预乘（ULW 要求）。
       注意与上面 RoundRect 用同一个 rad，几何必须一致。 */
    BYTE* base = (BYTE*)g_tipBits;
    for (int y = 0; y < g_tipH; y++) {
        BYTE* row8 = base + (size_t)y * (size_t)g_tipW * 4;
        for (int x = 0; x < g_tipW; x++) {
            double dX = (double)((x + 0.5 < g_tipW - 0.5 - x) ? (x + 0.5) : (g_tipW - 0.5 - x));
            double dY = (double)((y + 0.5 < g_tipH - 0.5 - y) ? (y + 0.5) : (g_tipH - 0.5 - y));
            double d  = (dX < dY) ? dX : dY;
            double cx = (x < rad) ? rad : ((x >= g_tipW - rad) ? (g_tipW - 1 - rad) : x);
            double cy = (y < rad) ? rad : ((y >= g_tipH - rad) ? (g_tipH - 1 - rad) : y);
            double dcorn = (double)(x - cx) * (x - cx) + (double)(y - cy) * (y - cy);
            double dc2 = (double)rad - sqrt(dcorn);
            if (dc2 < d) d = dc2;
            int a = (int)((d + 0.5) / 1.5 * 255.0 + 0.5);
            if (a < 0) a = 0;
            if (a > 255) a = 255;
            BYTE* px = row8 + (size_t)x * 4;   /* BGRA */
            px[0] = (BYTE)(px[0] * a / 255);
            px[1] = (BYTE)(px[1] * a / 255);
            px[2] = (BYTE)(px[2] * a / 255);
            px[3] = (BYTE)a;
        }
    }
    return TRUE;
}

/* 定位 + 把当前内容按当前 alpha 发给系统。 */
static void TipApply(POINT scr) {
    if (!g_tip || !g_tipDC) return;
    BLENDFUNCTION bf;
    bf.BlendOp             = AC_SRC_OVER;
    bf.BlendFlags          = 0;
    bf.SourceConstantAlpha = (BYTE)g_tipAlpha;
    bf.AlphaFormat         = AC_SRC_ALPHA;
    POINT src = { 0, 0 };
    SIZE  sz  = { g_tipW, g_tipH };
    HDC sdc = GetDC(NULL);
    UpdateLayeredWindow(g_tip, sdc, &scr, &sz, g_tipDC, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(NULL, sdc);
}

/* 收起：渐隐（TID_TIPFADE 步进到 0 后由 WM_TIMER 藏窗）。 */
static void TipHide(void) {
    KillTimer(g_hwnd, TID_TIP);
    KillTimer(g_hwnd, TID_TIPSHOW);
    if (g_tipRow < 0) return;
    g_tipRow = -1;
    if (!g_tip || !IsWindowVisible(g_tip)) return;
    g_tipAlphaDir = -1;
    SetTimer(g_hwnd, TID_TIPFADE, 15, NULL);
}

/* 显示/切换第 row 行。首次显示从 alpha 0 渐显；已显示时直接换内容
   并跟手移位（不收起、不重计）。位置在光标右下 16/20px，越界翻面。 */
static void TipShowAt(int row, POINT scr) {
    if (DsTipTimeoutMs() == 0) return;
    if (row < 0 || row >= g_count) return;
    if (g_tipRow == row) return;
    if (!TipBuild(row)) return;
    TipEnsureWindow();
    BOOL wasVisible = (g_tipRow >= 0);
    g_tipRow = row;
    g_tipPos.x = scr.x + 16;
    g_tipPos.y = scr.y + 20;
    RECT work;
    MonitorWork(MonitorFromPoint(g_tipPos, MONITOR_DEFAULTTONEAREST), &work);
    if (g_tipPos.x + g_tipW > work.right)  g_tipPos.x = scr.x - g_tipW - 16;
    if (g_tipPos.y + g_tipH > work.bottom) g_tipPos.y = work.bottom - g_tipH;
    if (g_tipPos.x < work.left)   g_tipPos.x = work.left;
    if (g_tipPos.y < work.top)    g_tipPos.y = work.top;
    if (!wasVisible) {
        g_tipAlpha = 0;
        g_tipAlphaDir = +1;
        TipApply(g_tipPos);                       /* 先定位 + 铺内容 */
        ShowWindow(g_tip, SW_SHOWNOACTIVATE);
        SetTimer(g_hwnd, TID_TIPFADE, 15, NULL);  /* 再渐显 */
    } else {
        g_tipAlphaDir = 0;  g_tipAlpha = 255;     /* 切换不闪，直接换 */
        TipApply(g_tipPos);
        SetWindowPos(g_tip, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    SetTimer(g_hwnd, TID_TIP, (UINT)DsTipTimeoutMs(), NULL);
}

/* 逐词换行：CJK 无空格按字断；ASCII 词内不断（超长词硬断）；
   空格断行时空格不进行尾。返回行数。 */
static int TipWrapInPlace(const WCHAR* text, HDC dc, int maxW, TipLine* ln, int maxLn) {
    int n = 0;
    const WCHAR* p = text;
    while (*p && n < maxLn) {
        int w = 0, curlen = 0, outLen = -1, lastSp = -1;
        const WCHAR* q = p;
        while (*q && *q != L'\n') {
            SIZE sz;
            GetTextExtentPoint32W(dc, q, 1, &sz);
            if (w + sz.cx > maxW && q > p) {
                outLen = (lastSp > 0) ? lastSp : curlen;
                break;
            }
            w += sz.cx;  curlen++;
            if (*q == L' ') lastSp = curlen;
            q++;
        }
        if (outLen < 0) outLen = curlen;              /* 自然结束（\n 或串尾） */
        ln[n].s = p;  ln[n].len = outLen;  n++;
        if (outLen < curlen) {                        /* 断在空格：吃掉它 */
            p = p + outLen;
            if (*p == L' ') p++;
        } else if (*q == L'\n') {
            p = q + 1;
        } else {
            p = q;                                    /* 硬断或串尾 */
        }
    }
    return n;
}

/* Ditto 的时间行：COleDateTime::Format() 的中文面貌 —— 不补零的
   年/月/日 时:分:秒。 */
static void FmtDittoTime(long long t, WCHAR* out, int cap) {
    time_t tt = (time_t)t;
    struct tm tmv;
    localtime_s(&tmv, &tt);
    _snwprintf_s(out, (size_t)cap, _TRUNCATE, L"%d/%d/%d %d:%02d:%02d",
                 tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
}

/* 时间行（Ditto 同款 "Added: ... / Last Used: ..."，放提示最下面）。 */
static void TipHeader(const ClipItem* it, WCHAR* out, int cap) {
    out[0] = 0;
    if (it->ldate <= 0) return;          /* 库里没有时间列：不显示头部 */
    WCHAR a[40], b[40];
    FmtDittoTime(it->ldate, a, 40);
    if (it->lastpaste > 0) FmtDittoTime(it->lastpaste, b, 40);
    else                 lstrcpyW(b, L"-");
    /* 两行排版：单行 " | " 连接在 500px 宽下会被系统折到难看的位置，不如主动分行 */
    _snwprintf_s(out, (size_t)cap, _TRUNCATE, L"Added: %s\nLast Used: %s", a, b);
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

    /* 时间行放**最下面**（用户反馈）：正文在上，Added/Last Used 收尾 */
    WCHAR hd[96];
    TipHeader(&g_items[idx], hd, 96);
    if (hd[0] && g_tipBuf[0]) {
        _snwprintf_s(g_tipBuf, 4096, _TRUNCATE,
                     L"%s\n\n%s", g_tipBuf, hd);
    }
}

/* ================= 缩略图 ================= */

static Gdiplus::Bitmap* g_thumb[CLIP_MAX_ITEMS];
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

    /* Ditto 的路子（借鉴其 ImageHelper.cpp 的 prependStream）：DIB 裸数据
       （BITMAPINFOHEADER + 调色板/掩码 + 像素，没有文件头）前面补一个 14 字节
       BITMAPFILEHEADER 拼成完整 BMP，交给 GDI+ 当图片文件解码。
       位深、调色板、BI_BITFIELDS 掩码、V4/V5 扩展头全部由 GDI+ 原生处理。
       原先 CreateDIBSection + FromHBITMAP 的路子有死角：16bpp BITFIELDS 这类
       DIB FromHBITMAP 直接失败 → 解码返回 NULL → 界面上就是一块占位灰框
       （"缩略图显示成灰色"），而 Ditto 同一条数据照常出图。 */
    int clr = 0;
    if (bpp <= 8) clr = h->biClrUsed ? (int)h->biClrUsed : (1 << bpp);
    DWORD offBits = h->biSize + (DWORD)clr * sizeof(RGBQUAD);
    if (comp == BI_BITFIELDS) offBits += 3 * sizeof(DWORD);   /* 3 个掩码在像素前 */
    if (offBits >= (DWORD)len) return NULL;

    BITMAPFILEHEADER bfh;
    memset(&bfh, 0, sizeof(bfh));
    bfh.bfType = 0x4D42;                            /* 'BM' */
    bfh.bfSize = (DWORD)sizeof(bfh) + (DWORD)len;
    bfh.bfOffBits = (DWORD)sizeof(bfh) + offBits;

    HGLOBAL gh = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)sizeof(bfh) + (SIZE_T)len);
    if (!gh) return NULL;
    BYTE* p = (BYTE*)GlobalLock(gh);
    if (!p) { GlobalFree(gh); return NULL; }
    memcpy(p, &bfh, sizeof(bfh));
    memcpy(p + sizeof(bfh), blob, (size_t)len);
    GlobalUnlock(gh);

    IStream* s = NULL;
    if (CreateStreamOnHGlobal(gh, TRUE, &s) != S_OK) { GlobalFree(gh); return NULL; }
    Gdiplus::Bitmap* bm = Gdiplus::Bitmap::FromStream(s);
    s->lpVtbl->Release(s);   /* cap.h 走 CINTERFACE：IStream 没有 C++ 的 Release() */
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
    /* 格式取舍照 Ditto（QPasteWndThread.cpp:349 起的取法）：先精确找 CF_DIB，
       找不到才找 PNG，别的格式（含 CF_DIBV5）一律不用 —— 多格式条目两边解
       同一个源。CASE 把 CF_DIB 排最前，取**首个命中行**（解码失败也不换源，
       Ditto 同样只在"没有 CF_DIB"时才退 PNG）。 */
    if (sqlite3_prepare_v2(db, "SELECT strClipBoardFormat, ooData FROM Data "
                              "WHERE lParentID = ? "
                              "AND strClipBoardFormat IN ('CF_DIB', 'PNG') "
                              "ORDER BY CASE strClipBoardFormat "
                              "WHEN 'CF_DIB' THEN 0 ELSE 1 END, Data.lID",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, g_items[idx].id);
        while (sqlite3_step(st) == SQLITE_ROW) {
            const WCHAR* nm = (const WCHAR*)sqlite3_column_text16(st, 0);
            const void* blob = sqlite3_column_blob(st, 1);
            int len = sqlite3_column_bytes(st, 1);
            if (nm && blob && len > 0) {
                if (wcscmp(nm, L"CF_DIB") == 0)
                    big = BitmapFromDib((const BYTE*)blob, len);
                else if (wcscmp(nm, L"PNG") == 0)
                    big = BitmapFromPng((const BYTE*)blob, len);
                break;                       /* 只取首个命中行，后面不再看 */
            }
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    if (!big) return NULL;

    UINT iw = big->GetWidth(), ih = big->GetHeight();
    if (iw == 0 || ih == 0) { delete big; return NULL; }
    float boxH = g_rowH2 - 2.0f * g_thumbPad;
    /* Ditto 同款（BitmapHelper.cpp:69 的 min）：目标高取 min(行高, 原高)，
       小图保持原大 —— 放大只会糊。 */
    float bh = ((float)ih < boxH) ? (float)ih : boxH;
    float bw = bh * (float)iw / (float)ih;
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

/* 后台解码线程：常驻循环 —— 等唤醒事件，把带图条目的缩略图解码一遍。
   旧版"进程活着只做这一轮"：驻留期间条目集一变（复制了新条目 / 粘贴后
   Ditto 重排序），DbLoadItems 作废全部缓存，却没人再解码 → 第一次开
   正常、再开全是占位灰框。现在 DbLoadItems 换代后 SetEvent 叫醒重做。 */
#define WM_THUMB_READY (WM_APP + 20)
static DWORD WINAPI ThumbThreadProc(LPVOID lp) {
    (void)lp;
    for (;;) {
        WaitForSingleObject(g_thumbWake, INFINITE);
        if (g_thumbQuit) return 0;
        LONG myGen = g_thumbGen;
        for (int i = 0; i < g_count; i++) {
            if (g_thumbQuit) return 0;
            if (g_thumbGen != myGen) break;   /* 条目又重载了：整轮重来 */
            if (!g_items[i].hasImage) continue;
            long long id = g_items[i].id;     /* 记下解的是谁，防中途重载串位 */
            Gdiplus::Bitmap* bm = DecodeThumb(i);
            EnterCriticalSection(&g_thumbLock);
            if (g_thumbGen == myGen && g_items[i].id == id) {
                g_thumb[i] = bm;              /* 失败就是 NULL：占位框逻辑照旧 */
                bm = NULL;                    /* 缓存已接管 */
            }
            LeaveCriticalSection(&g_thumbLock);
            delete bm;                        /* 没接管的（含 NULL）在这里释放 */
            if (g_thumbGen == myGen)
                PostMessageW(g_hwnd, WM_THUMB_READY, (WPARAM)i, 0);
            Sleep(0);
        }
    }
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
    /* ★ 告诉主程序"我关了"：否则 s_want 残留，钩子继续吞回车 —— 宽限期内
       按键进隐藏窗口（再粘一次），销毁后凭空消失（用户实测：回车粘贴后
       第二下回车又粘一次、真回车永远发不出）。 */
    HWND host = FindWindowW(L"CapsEnhanceMain", NULL);
    if (host && g_msgClosed) PostMessageW(host, g_msgClosed, 0, 0);
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
    if (g_tipFont)   DeleteObject(g_tipFont);
    if (g_fontSmall) DeleteObject(g_fontSmall);
    /* 字号是**磅**：按当前显示器 DPI 折成像素（lfHeight 负值 = 按字形高度）。
       用户在设置里选的字体名/字号经主程序用 --font/--size 传进来。 */
    int rowPx = MulDiv(g_fontPt, DpiOfMonitor(mon), 72);
    if (rowPx < 8) rowPx = 8;
    g_fontRow   = MakeFont(g_fontName, rowPx, FW_NORMAL);
    g_fontSmall = MakeFont(g_fontName, rowPx * 3 / 4, FW_NORMAL);
    g_tipFont   = MakeFont(g_fontName, rowPx * 7 / 8, FW_NORMAL);   /* 提示：小一档 */
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
    /* 常驻：不再设定时器自杀（见上面"寿命"那段）。窗口隐掉、数据与缩略图都留着，
       下一次唤出是纯 SHOW —— 这正是当初要它的全部理由，别让它到期返贫。 */
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
    if (!g_visible) {
        /* 驻留期间 Ditto 可能又复制了新内容：每次显示都重读一次（此刻窗口
           还没 SHOW，无闪烁；只读本地库 + LIMIT CLIP_MAX_ITEMS，毫秒级）。
           缩略图在 DbLoadItems 里按 lID 序列对比决定去留，条目集没变就不重解。 */
        DbLoadItems();        PlaceWindow();                 /* ★ 只在这里定位 */
        MoveWindow(g_hwnd, g_px, g_py, g_pxW, g_pxH, FALSE);
        Render();
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        g_visible = TRUE;
    } else {
        DbLoadItems();                 /* 可见时重复触发：同样刷新（置顶模式） */
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
    if (g_msgEsc  && msg == g_msgEsc)  { DoHideClose(); return 0; }
    if (msg == WM_THUMB_READY) { Render(); return 0; }
    if (g_msgKey  && msg == g_msgKey) {
        if (!g_visible) return 0;   /* 已收起、销毁宽限期内：忽略残留按键 */
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
        if (wp == TID_TIP)  { TipHide(); return 0; }   /* Ditto 的 ToolTipTimeout */
        if (wp == TID_TIPFADE) {
            /* 渐显/渐隐步进：每步重发一次 ULW，SourceConstantAlpha 当乘数 */
            g_tipAlpha += g_tipAlphaDir * 48;
            if (g_tipAlpha >= 255) { g_tipAlpha = 255; g_tipAlphaDir = 0; KillTimer(g_hwnd, TID_TIPFADE); }
            if (g_tipAlpha <= 0) {
                g_tipAlpha = 0; g_tipAlphaDir = 0;
                KillTimer(g_hwnd, TID_TIPFADE);
                ShowWindow(g_tip, SW_HIDE);
            }
            if (g_tipAlphaDir || g_tipAlpha) TipApply(g_tipPos);
            return 0;
        }
        if (wp == TID_TIPSHOW) {
            KillTimer(g_hwnd, TID_TIPSHOW);
            /* 延迟到了：按光标**现在**的位置定行与弹出点 —— 等待期间鼠标可能又动过 */
            POINT c;
            if (GetCursorPos(&c) && g_visible) {
                POINT cl = c;
                ScreenToClient(g_hwnd, &cl);
                int r2 = RowAt((float)cl.y);
                if (r2 >= 0) TipShowAt(r2, c);
            }
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
        /* 悬停提示按 Ditto 的节奏分两段：
           · 没显示时：行变了就重计 500ms 延迟 —— 停稳才弹（TID_TIPSHOW），
             划过列表不会连珠炮地弹，NEEDTEXT 的回库查询也只有停稳那一次；
           · 已显示时：上下移动**立即切换**到新行（换文本 + 跟手移位），
             不收起、不重计 —— 和 Ditto 一样"弹一次，之后跟着选区走"。 */
        if (row >= 0) {
            if (g_tipRow != row) {
                if (g_tipRow < 0) {
                    SetTimer(g_hwnd, TID_TIPSHOW, TIP_DELAY_MS, NULL);
                } else {
                    POINT scr = { (short)LOWORD(lp), (short)HIWORD(lp) };
                    ClientToScreen(g_hwnd, &scr);
                    TipShowAt(row, scr);
                }
            }
        } else {
            KillTimer(g_hwnd, TID_TIPSHOW);
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

    /* 自测起点：进程生命周期都从这里算，主程序日志里那两个 ms 就是它。 */
    ULONGLONG t0 = GetTickCount64();
    DWORD dbMs = 0;                  /* 读库那一段的耗时（见下面报信时才带上） */
    g_msgShow = RegisterWindowMessageW(CLIP_MSG_SHOW);
    g_msgHide = RegisterWindowMessageW(CLIP_MSG_HIDE);
    g_msgEsc  = RegisterWindowMessageW(CLIP_MSG_ESC);
    g_msgKey  = RegisterWindowMessageW(CLIP_MSG_KEY);
    g_msgClosed = RegisterWindowMessageW(CLIP_MSG_CLOSED);
    g_msgBorn = RegisterWindowMessageW(CLIP_MSG_BORN);

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
    /* ★ 临界区必须在第一次 DbLoadItems 之前初始化：DbLoadItems 重载条目后
       会按需 FreeThumbs()（进去就 EnterCriticalSection），而启动路径的
       DbLoadItems 就在这一行 —— 初始化放后面就是"未初始化临界区"，
       进程起来即崩（实测踩过：浮层完全不显示）。 */
    InitializeCriticalSection(&g_thumbLock);
    /* 唤醒事件也要在第一次 DbLoadItems 之前建好：那次加载 oldCount=0 必判
       "条目集变了"，会在里面 SetEvent —— 第一轮解码的开工信号就来自它。 */
    g_thumbWake = CreateEventW(NULL, FALSE, FALSE, NULL);
    {   /* 读库单独计时：它是这段里唯一会随历史增长而变慢的一步，值得单独看清 */
        ULONGLONG tDb = GetTickCount64();
        DbLoadItems();
        dbMs = (DWORD)(GetTickCount64() - tDb);
    }
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

    /* ★ 出生报信：**只有我知道自己出世了**，所以由我开口 —— 主程序原来用 40ms
       一次 FindWindow 猜这件事，猜急了（1 秒预算）就把还没出窗的这份杀掉重起，
       那正是"偶发要 2-5 秒"被放大成"要多按几次"的地方（详见 src/cap_overlay.h）。
       带上两段耗时：进程入口 -> 出窗（wParam）与其中读库那段（lParam），主程序
       只在记录模式下抄进日志，用来回答"现在到底快没快"。
       ★ 报信成功就不再自己显示 —— 拉我起来的人会告诉我该不该显示（SHOW/HIDE），
         再来一次 `DoShow` 等于多读一遍库（驻留省下的正是这个）。找不到主窗口
         （手动双击）才退回老样子：自己显示，自己活着。 */
    BOOL toldMain = FALSE;
    if (g_msgBorn) {
        HWND mw = FindWindowW(CLIP_MAIN_CLASS, NULL);
        if (mw) {
            PostMessageW(mw, g_msgBorn, (WPARAM)(DWORD)(GetTickCount64() - t0), (LPARAM)dbMs);
            toldMain = TRUE;
        }
    }

    if (g_thumbWake)    /* 事件没建成就别起线程：等一个 NULL 句柄是忙循环 */
        g_thumbThread = CreateThread(NULL, 0, ThumbThreadProc, NULL, 0, NULL);

    g_target = GetForegroundWindow();
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, NULL, 0);
    g_evHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
                               NULL, WinEventProc, 0, 0,
                               WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    /* 有人拉我起来时（toldMain），显示与否由他说了算 —— 他会发 SHOW/HIDE，样子与
       "驻留中被唤出"完全一致；这条道上不再自己 DoShow，省一遍读库。 */
    if (!toldMain) DoShow();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_thumbThread) {
        InterlockedExchange(&g_thumbQuit, 1);
        if (g_thumbWake) SetEvent(g_thumbWake);   /* 可能正等着活干：叫醒它好退场 */
        WaitForSingleObject(g_thumbThread, 3000);
        CloseHandle(g_thumbThread);
        g_thumbThread = NULL;
    }
    if (g_thumbWake) { CloseHandle(g_thumbWake); g_thumbWake = NULL; }
    if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
    if (g_evHook) UnhookWinEvent(g_evHook);
    if (g_fontRow)   DeleteObject(g_fontRow);
    if (g_tipFont)   DeleteObject(g_tipFont);
    if (g_fontSmall) DeleteObject(g_fontSmall);
    FreeThumbs();
    DeleteCriticalSection(&g_thumbLock);
    FreeFmts();
    if (g_bmp) DeleteObject(g_bmp);
    if (g_memDC) DeleteDC(g_memDC);
    Gdiplus::GdiplusShutdown(token);
    return 0;
}
