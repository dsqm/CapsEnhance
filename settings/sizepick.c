/* ============================================================
   CapsEnhance 设置程序（CapsEnhanceSettings.exe）—— 尺寸选取窗

   三对"窗口大小"（快速粘贴 / F3 预览的最大与最小）只给两个数字看不出实际
   多大：光看 220×300 没人知道是不是自己想要的。每行右侧的「调整…」弹出一个
   **照真实浮层画法画的示例窗口**（列表窗画示例条目、预览窗画占位图，配色与
   clip/clip.h 同一套），拖着它定尺寸，点中央「完成」把值填回那一对输入框
   —— **不写 ini**，ini 仍由主窗口底部的保存按钮写。

   为什么单独成一个编译单元：它有自己的窗口类、自己的状态、两套自绘画法，
   六百行塞在表单主文件里只会把主文件的主题稀释掉；单出来之后"这里存着一份
   与 clip.c 平行的第二画法"这件事也看得见 —— 改浮层样式时两边要一起看。
   与主窗口之间只通过 settings_int.h 里那几样共享状态/小工具打交道（那也是
   同一个 exe 两个编译单元之间的内部头，不对其它 exe 暴露）。

   行为要点（完整推导在下面各段的注释里）：
     · 开窗尺寸 = 值 × 所在屏 DPI/96，值是唯一事实来源，像素只是它的显示量；
     · 拖到缩放不同的屏：值不动，物理大小按新 DPI 重算（WM_DPICHANGED）；
     · 尺寸受两道钳制：配置项自己的值域 + 所在显示器工作区；
     · 每次开窗**现读**那两个输入框（含未保存的改动），读不出来才回落到 ini。
   ============================================================ */
#include "settings_int.h"

#include <stdio.h>         /* _snwprintf_s */
#include <wchar.h>
/* ---------------- 尺寸选取窗（示例窗口 + 拖着定尺寸） ----------------
   「宽 × 高」两个数字看不出窗口实际多大 —— 光看 220×300 没人知道是不是自己
   想要的。三对尺寸各配一颗「调整…」：弹出一个**照真实浮层画法画的示例窗口**
   （列表窗画示例条目、预览窗画占位图，配色与 clip/clip.h 同一套），拖它就能
   定尺寸：
     · 中部按住拖 = 挪窗；四边 / 四角按住拖 = 改大小（光标跟着变 ↔ ↕）；
     · 中央实时显示**将写进去的那个数**（96 DPI 像素，与输入框、ini 同一个值）；
     · 点「完成」把这对值填回对应的两个输入框 —— **不写 ini**，ini 仍由底部的
       保存按钮写，和表单里其它项一个规矩，也还反悔得了；Esc = 放弃。
   开窗尺寸 = 值 × 所在屏 DPI/96（与真实浮层同一套语义），所以拖到另一块缩放
   不同的屏上时**物理大小会变、值不变**（WM_DPICHANGED 里按新 DPI 重算）。
   尺寸受两道钳制：该配置项自己的值域，与所在显示器的工作区。

   为什么在设置进程里重画一份而不是拉起真的 Clip 进程：真窗口要开 Ditto 的库
   才有内容，而设置进程只链 config.c —— 为"调个尺寸"付一份 SQLite + 解码的
   代价不值当。代价是这里的画法与 clip 那边的画法是两份：改浮层样式时两边
   要一起看。 */

#define SPICK_CLASS L"CapsEnhanceSettingsSizePicker"

/* 命中区：窗口是 WS_POPUP（与真实浮层一样没有系统边框），命中只能自己算 */
enum { SP_NONE = 0, SP_BTN_CANCEL, SP_BTN_OK, SP_MOVE,
       SP_L, SP_R, SP_T, SP_B, SP_TL, SP_TR, SP_BL, SP_BR };

/* 面板上两颗按钮的下标（画 / 命中 / 悬停用同一套编号） */
enum { SPB_CANCEL = 0, SPB_OK = 1, SPB_N = 2 };
static const WCHAR* const kSpBtnText[SPB_N] = { L"取消", L"完成" };

/* 与 clip/clip.h 同一套配色（照 Ditto 默认主题）。两份：改一边记得改另一边。 */
#define SPC_BG       RGB(255, 255, 255)
#define SPC_BORDER   RGB(204, 204, 204)
#define SPC_TEXT     RGB(0, 0, 0)
#define SPC_ROW_ALT  RGB(243, 243, 243)
#define SPC_SEL_BG   RGB(204, 204, 204)
#define SPC_TITLE    RGB(110, 110, 110)
#define SPC_INDEX    RGB(110, 110, 110)
#define SPC_GUTTER   RGB(240, 240, 240)
#define SPC_THUMB    RGB(224, 224, 224)
#define SPC_ACCENT   RGB(0, 120, 215)
#define SPC_ACCENT_H RGB(26, 134, 217)
#define SPC_ACCENT_P RGB(0, 90, 158)

/* ---- 尺寸对 <-> 「调整…」按钮 ----
   三对尺寸里的两对（F3 预览的最大 / 最小）是**同一个窗口**，所以弹窗样式
   只有两套：列表窗（快速粘贴）与预览窗（F3）。kind 决定画哪种示例内容。
   这张表归弹窗所有（主文件只通过 SizePickerBtnFor 问一句"配没配按钮"）。 */
enum { SPK_LIST = 0, SPK_VIEW = 1 };
typedef struct {
    int firstId;    /* 该对的行首（宽）输入框 id —— 值从它和它的行次读、也写回它们 */
    int btnId;      /* 这一行右侧那颗「调整…」 */
    int kind;       /* SPK_LIST / SPK_VIEW */
} SizePickerDef;
static const SizePickerDef kSizePickers[] = {
    { IDC_CLIP_W,   IDC_CLIP_SIZE_BTN, SPK_LIST },
    { IDC_CLIP_VMW, IDC_VIEWMAX_BTN,   SPK_VIEW },
    { IDC_CLIP_VNW, IDC_VIEWMIN_BTN,   SPK_VIEW },
};
#define SPICK_N ((int)(sizeof(kSizePickers) / sizeof(kSizePickers[0])))

/* 这一对配没配「调整…」按钮（配了返回按钮 id，否则 0）—— 主文件 Layout 用 */
int SizePickerBtnFor(int firstId) {
    for (int i = 0; i < SPICK_N; i++)
        if (kSizePickers[i].firstId == firstId) return kSizePickers[i].btnId;
    return 0;
}

/* 列表窗的示例条目：行数/行高/两行文本/缩略图都照真实列表来，只有文字是假的 */
static const struct {
    const WCHAR* t0;
    const WCHAR* t1;
    BOOL two;      /* 两行文本（行高 = 行距 × 2 + 8） */
    BOOL img;      /* 图片条目：行高按缩略图给，左侧画缩略图占位 */
} kSampleRows[] = {
    { L"示例条目：拖四边或四角改大小", NULL, FALSE, FALSE },
    { L"示例条目：两行文本（第一行）", L"示例条目：两行文本（第二行）", TRUE, FALSE },
    { L"示例条目：图片（缩略图是占位）", NULL, FALSE, TRUE },
    { L"示例条目：Caps+V 打开的列表", NULL, FALSE, FALSE },
    { L"示例条目：选中行是深灰底", NULL, FALSE, FALSE },
    { L"示例条目：奇偶行底色交替", NULL, FALSE, FALSE },
    { L"示例条目：序号只给前十条", NULL, FALSE, FALSE },
    { L"示例条目：拖中部可以挪窗", NULL, FALSE, FALSE },
    { L"示例条目：点中央「完成」写回", NULL, FALSE, FALSE },
    { L"示例条目：Esc 放弃这次改动", NULL, FALSE, FALSE },
};
#define SAMPLE_N ((int)(sizeof(kSampleRows) / sizeof(kSampleRows[0])))

static HWND  g_spick     = NULL;   /* 弹窗句柄（NULL = 没开着） */
static int   g_spickKind = SPK_LIST;
static int   g_spickDpi  = 96;     /* 弹窗自己的 DPI（拖到别的屏会变） */
static int   g_spickW    = 0;      /* 当前宽（96 DPI 像素 —— 要写回的那个值） */
static int   g_spickH    = 0;      /* 当前高（同上） */
static int   g_spickLoW = 0, g_spickHiW = 0, g_spickLoH = 0, g_spickHiH = 0;
static int   g_spickIdW = 0, g_spickIdH = 0;    /* 写回哪两个输入框 */
static int   g_spickMode = SP_NONE;             /* 正在拖哪条边（SP_NONE = 没拖） */
static RECT  g_spickRect0;                      /* 按下那一刻的窗口矩形（屏幕） */
static POINT g_spickPt0;                        /* 按下那一刻的光标（屏幕） */
static RECT  g_spickWork;                       /* 所在显示器工作区（尺寸上限） */
/* 面板上两颗按钮的悬停 / 按下态：存**下标**（SPB_*），-1 = 没有。
   用下标而不是两个 BOOL：两颗按钮互斥，按下态永远只有一颗能亮。 */
static int   g_spickBtnHot = -1, g_spickBtnDown = -1;
static HFONT g_spickRow = NULL, g_spickSmall = NULL;   /* 示例条目的字体 */
static int   g_spickRowPx = 12;                 /* 行字号（物理像素）—— 行高按它算 */
static const WCHAR* g_spickLabel = L"";         /* 这一对的名字（面板上写明在调哪一对） */

/* 弹窗内的尺寸一律按**弹窗自己**的 DPI 缩放（可能与主窗口不是同一块屏） */
static int SpS(int v) { return MulDiv(v, g_spickDpi, 96); }

static void SpDraw(HDC dc, int w, int hgt);   /* 定义在下面（SpPaint 的双缓冲要用） */

/* 读一对输入框当前的文本（**含没保存的改动**）：读不出来（空 / 非数字）就
   回落到配置里的值，两边都钳到值域 —— 弹窗显示的是"将要生效的那个数"。 */
static void SpReadPair(const Field* fw, const Field* fh, int* w, int* h) {
    int v = 0;
    if (ReadIntField(GetDlgItem(g_hwnd, fw->id), fw, &v)) {
        if (v < fw->min) v = fw->min;
        if (v > fw->max) v = fw->max;
    } else {
        v = CfgInt(fw->cfg);
    }
    *w = v;
    if (ReadIntField(GetDlgItem(g_hwnd, fh->id), fh, &v)) {
        if (v < fh->min) v = fh->min;
        if (v > fh->max) v = fh->max;
    } else {
        v = CfgInt(fh->cfg);
    }
    *h = v;
}

/* 两道钳制：配置项值域 + 所在显示器工作区（窗口总不能比屏幕还大） */
static void SpClampValue(int* w, int* h) {
    if (*w < g_spickLoW) *w = g_spickLoW;
    if (*w > g_spickHiW) *w = g_spickHiW;
    if (*h < g_spickLoH) *h = g_spickLoH;
    if (*h > g_spickHiH) *h = g_spickHiH;
    float  k    = (float)g_spickDpi / 96.0f;
    int    maxW = (int)((float)(g_spickWork.right  - g_spickWork.left) / k);
    int    maxH = (int)((float)(g_spickWork.bottom - g_spickWork.top)  / k);
    if (*w > maxW) *w = maxW;
    if (*h > maxH) *h = maxH;
    if (*w < 16) *w = 16;      /* 保底：再小就拖没了 */
    if (*h < 16) *h = 16;
}

/* 值 -> 物理像素（与真实浮层同一条式子：值 × DPI/96） */
static int SpPx(int v) { return (int)((float)v * (float)g_spickDpi / 96.0f + 0.5f); }

/* 示例条目的字体：照 clip.c 的 FitMetrics —— 字号是**磅**，按 DPI 折成像素，
   行字体与小号（序号）同源。 */
static void SpMakeFonts(void) {
    const WCHAR* face = CfgStr(CFG_CLIP_FONT);
    if (!face[0]) face = L"Microsoft YaHei UI";
    int rowPx = MulDiv(CfgInt(CFG_CLIP_FONT_SIZE), g_spickDpi, 72);
    if (rowPx < 8) rowPx = 8;
    g_spickRowPx = rowPx;
    if (g_spickRow)   DeleteObject(g_spickRow);
    if (g_spickSmall) DeleteObject(g_spickSmall);
    g_spickRow = CreateFontW(-rowPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             DEFAULT_QUALITY, VARIABLE_PITCH | FF_DONTCARE, face);
    g_spickSmall = CreateFontW(-(rowPx * 3 / 4), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               DEFAULT_QUALITY, VARIABLE_PITCH | FF_DONTCARE, face);
}

static void SpText(HDC dc, HFONT f, COLORREF c, const WCHAR* s,
                   int x, int y, int w, int h, UINT align) {
    if (!s || !*s || !f || w <= 0 || h <= 0) return;
    RECT rc;
    rc.left = x; rc.top = y; rc.right = x + w; rc.bottom = y + h;
    HGDIOBJ of = SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s, -1, &rc,
              DT_SINGLELINE | DT_NOPREFIX | DT_VCENTER | align | DT_END_ELLIPSIS);
    SelectObject(dc, of);
}

/* 面板上两颗按钮的矩形（客户区坐标）：0 = 取消，1 = 完成。
   画与点用同一份，画得到的就点得到。
   ★ 面板与按钮都要**先按窗口大小收一遍**：这个窗是给用户拖小的（F3 预览的
   最小尺寸可以小到 40×80），面板按 250×74 硬画的话，按钮会伸到窗口外 ——
   看着就是"按钮超出面板/看不见被裁掉"。收的时候只压按钮与面板，不动其它画法。 */
static void SpPanelRect(int w, int hgt, RECT* out) {
    int pw = SpS(210), ph = SpS(74);
    if (pw > w - SpS(8))   pw = w - SpS(8);
    if (ph > hgt - SpS(6)) ph = hgt - SpS(6);
    if (pw < 0) pw = 0;
    if (ph < 0) ph = 0;
    out->left   = (w - pw) / 2;
    out->top    = (hgt - ph) / 2;
    out->right  = out->left + pw;
    out->bottom = out->top + ph;
}

static void SpBtnRect(int idx, RECT* out) {
    RECT rc;
    GetClientRect(g_spick, &rc);
    RECT pr;
    SpPanelRect(rc.right, rc.bottom, &pr);
    int pw = pr.right - pr.left, ph = pr.bottom - pr.top;
    /* 按钮只装两个字（取消 / 完成），按字面宽度给就够 —— 给到 96 宽是照
       系统按钮的惯例拍脑袋定的，看着就是两块空荡荡的色块。 */
    int bw = SpS(64),  bh = SpS(24), bg2 = SpS(10);
    /* 面板内横向放得下两颗就照原尺寸，放不下就均分剩下的宽度 */
    int availW = pw - SpS(16);
    int needW  = bw * SPB_N + bg2 * (SPB_N - 1);
    if (availW < needW) {
        bw = (availW - bg2 * (SPB_N - 1)) / SPB_N;
        if (bw < 0) bw = 0;
    }
    /* 纵向：上面固定留给"这一对的名字 + 尺寸数字"，剩下的才归按钮；
       窗口被拖小时先压按钮，压到没地方就不画（见 SpDraw 里的跳过）。 */
    int reserveTop = SpS(44);
    int availH = ph - reserveTop - SpS(6);
    if (bh > availH) bh = availH;
    if (bh < 0) bh = 0;
    int x0 = pr.left + (pw - (bw * SPB_N + bg2 * (SPB_N - 1))) / 2;
    out->left   = x0 + idx * (bw + bg2);
    out->top    = pr.top + reserveTop;
    out->right  = out->left + bw;
    out->bottom = out->top + bh;
}

/* 点在面板的哪颗按钮上（-1 = 没在按钮上） */
static int SpHitBtn(POINT pt) {
    for (int i = 0; i < SPB_N; i++) {
        RECT r;
        SpBtnRect(i, &r);
        if (PtInRect(&r, pt)) return i;
    }
    return -1;
}

/* 棋盘格（预览窗的占位图用：alpha 的通用表示法，与 clip.c 同一画法）。
   ★ 走**图案画刷**一次 FillRect，不逐格画：占位图比窗口大一圈，逐格画是
     几千次 FillRect —— 拖动时每帧重画一遍，这几千次就是"拖动发涩/抖"的
     直接来源（一帧画不完，窗口跟不上光标）。画刷按格子尺寸缓存，只在
     DPI 变了（格子变大）时重做。 */
static HBITMAP g_spickChkBmp  = NULL;
static int     g_spickChkCell = 0;

static HBRUSH SpCheckerBrush(int cell) {
    if (cell < 2) cell = 2;
    if (!g_spickChkBmp || g_spickChkCell != cell) {
        if (g_spickChkBmp) { DeleteObject(g_spickChkBmp); g_spickChkBmp = NULL; }
        HDC dc = CreateCompatibleDC(NULL);
        if (dc) {
            /* 2×2 个格子的一小块，交给画刷自己平铺（画刷以 DC 原点对齐） */
            g_spickChkBmp = CreateCompatibleBitmap(dc, cell * 2, cell * 2);
            if (g_spickChkBmp) {
                HGDIOBJ ob = SelectObject(dc, g_spickChkBmp);
                RECT all = { 0, 0, cell * 2, cell * 2 };
                HBRUSH w = CreateSolidBrush(RGB(255, 255, 255));
                FillRect(dc, &all, w);
                DeleteObject(w);
                HBRUSH g = CreateSolidBrush(RGB(200, 200, 200));
                RECT a = { 0, 0, cell, cell };
                RECT b = { cell, cell, cell * 2, cell * 2 };
                FillRect(dc, &a, g);
                FillRect(dc, &b, g);
                DeleteObject(g);
                SelectObject(dc, ob);
                g_spickChkCell = cell;
            }
            DeleteDC(dc);
        }
    }
    return g_spickChkBmp ? CreatePatternBrush(g_spickChkBmp) : NULL;
}

static void SpFill(HDC dc, int x, int y, int w, int h, COLORREF c) {
    if (w <= 0 || h <= 0) return;
    RECT r;
    r.left = x; r.top = y; r.right = x + w; r.bottom = y + h;
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

/* 圆角填充（自绘滚动条的拇指用）：GDI 没有现成的圆角填充，RoundRect 配空画笔 */
static void SpRoundFill(HDC dc, int x, int y, int w, int h, int rad, COLORREF c) {
    if (w <= 0 || h <= 0) return;
    HBRUSH b  = CreateSolidBrush(c);
    HGDIOBJ ob = SelectObject(dc, b);
    HGDIOBJ op = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, x, y, x + w, y + h, rad * 2, rad * 2);
    SelectObject(dc, op);
    SelectObject(dc, ob);
    DeleteObject(b);
}

/* 全部内容画进**离屏位图**再一次性 BitBlt 出去。
   为什么：拖动时每个 WM_MOUSEMOVE 都要重画一遍（窗口尺寸在变），直接画在
   窗口 DC 上会把"擦—画—画—画"的中间态逐条送到屏幕上 —— 看到的就是闪/抖。
   双缓冲之后每帧只有一次 BitBlt，中间态全在内存里。 */
static void SpPaint(HWND h, HDC dc) {
    RECT rc;
    GetClientRect(h, &rc);
    int w = rc.right, hgt = rc.bottom;
    if (w <= 0 || hgt <= 0) return;
    HDC     mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, hgt);
    if (!mem || !bmp) {
        if (bmp) DeleteObject(bmp);
        if (mem) DeleteDC(mem);
        SpDraw(dc, w, hgt);
        return;
    }
    HGDIOBJ old = SelectObject(mem, bmp);
    SpDraw(mem, w, hgt);
    BitBlt(dc, 0, 0, w, hgt, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static void SpDraw(HDC dc, int w, int hgt) {
    float k = (float)g_spickDpi / 96.0f;
    int  hdr = (int)(34.0f * k);
    if (hdr < 12) hdr = 12;

    /* ---- 内容区：列表窗 = 白底 + 示例行；预览窗 = 浅灰底 + 占位图 ---- */
    if (g_spickKind == SPK_LIST) {
        SpFill(dc, 0, 0, w, hgt, SPC_BG);
        float gutter = 13.5f * k, rowH1 = 34.0f * k, rowH2 = 76.0f * k, pad = 3.0f * k;
        float lineH = (float)(int)((float)g_spickRowPx * 1.4f);
        if (lineH < 14.0f) lineH = 14.0f;
        float y = (float)hdr;
        for (int i = 0; i < SAMPLE_N && y < (float)hgt; i++) {
            float rh = rowH1;
            if (kSampleRows[i].img)      rh = rowH2;
            else if (kSampleRows[i].two) rh = lineH * 2.0f + 8.0f * k;
            int y1 = (int)(y + rh + 0.5f);
            if (y1 > hgt) y1 = hgt;                 /* 半截行：画到窗口底为止 */
            SpFill(dc, 0, (int)y, w, y1 - (int)y,
                   i == 2 ? SPC_SEL_BG : ((i & 1) ? SPC_ROW_ALT : SPC_BG));

            WCHAR num[8] = L"";
            if (i < 9)       _snwprintf_s(num, 8, _TRUNCATE, L"%d", i + 1);
            else if (i == 9) wcscpy_s(num, 8, L"0");
            SpText(dc, g_spickSmall, SPC_INDEX, num,
                   0, (int)y, (int)(gutter + 6.0f * k), (int)rh, DT_CENTER);

            float bx0 = (i < 10) ? gutter : 0.0f;
            float tx  = bx0 + 6.0f * k;
            int   tw  = w - (int)tx - (int)(8.0f * k);
            if (kSampleRows[i].img) {
                /* 缩略图占位：占满行高（与真实浮层同一尺寸规则） */
                float box = rh - 2.0f * pad;
                float bxx = bx0 + 4.0f * k;
                SpFill(dc, (int)bxx, (int)(y + (rh - box) * 0.5f),
                       (int)box, (int)box, SPC_THUMB);
                SpFill(dc, (int)bxx, (int)(y + (rh - box) * 0.5f),
                       (int)box, 1, SPC_BORDER);
                tx += box + 8.0f * k;
                tw  = w - (int)tx - (int)(8.0f * k);
            }
            if (kSampleRows[i].two) {
                float y0 = y + (rh - lineH * 2.0f) * 0.5f;
                SpText(dc, g_spickRow, SPC_TEXT, kSampleRows[i].t0,
                       (int)tx, (int)y0, tw, (int)lineH, DT_LEFT);
                SpText(dc, g_spickRow, SPC_TEXT, kSampleRows[i].t1,
                       (int)tx, (int)(y0 + lineH), tw, (int)lineH, DT_LEFT);
            } else {
                SpText(dc, g_spickRow, SPC_TEXT, kSampleRows[i].t0,
                       (int)tx, (int)y, tw, (int)rh, DT_LEFT);
            }
            y += rh;
        }
    } else {
        SpFill(dc, 0, hdr, w, hgt - hdr, SPC_GUTTER);
        int barW = (int)(10.0f * k), barH = (int)(10.0f * k);
        /* 占位图：比窗口大一圈（因此两条自绘滚动条都有得滚），顶/左锚定 */
        int imgW = (int)((float)w * 1.6f);
        int imgH = (int)((float)(hgt - hdr) * 1.6f);
        int visW = w - barW, visH = hgt - hdr - barH;
        if (visW > 0 && visH > 0) {
            int saved = SaveDC(dc);
            IntersectClipRect(dc, 0, hdr, visW, hgt - barH);
            HBRUSH chk = SpCheckerBrush((int)(12.0f * k));
            if (chk) {
                RECT ir = { 0, hdr, imgW, hdr + imgH };
                FillRect(dc, &ir, chk);
                DeleteObject(chk);
            }
            RestoreDC(dc, saved);
        }
        /* 自绘滚动条：无轨道无箭头，只有圆角拇指（与 clip_view.c 同款） */
        if (barW > 0 && barH > 0) {
            COLORREF th = RGB(0x8C, 0x8C, 0x8C);
            int rad = (int)(3.0f * k);
            int trackH = visH - 4;
            if (trackH > 0 && imgH > 0) {
                int th1 = (int)((float)trackH * visH / (float)imgH + 0.5f);
                int minT = (int)(18.0f * k);
                if (th1 < minT) th1 = minT;
                if (th1 > trackH) th1 = trackH;
                SpRoundFill(dc, w - barW + 2, hdr + 2, barW - 4, th1, rad, th);
            }
            int trackW = visW - 4;
            if (trackW > 0 && imgW > 0) {
                int tw1 = (int)((float)trackW * visW / (float)imgW + 0.5f);
                int minT = (int)(18.0f * k);
                if (tw1 < minT) tw1 = minT;
                if (tw1 > trackW) tw1 = trackW;
                SpRoundFill(dc, 2, hgt - barH + 2, tw1, barH - 4, rad, th);
            }
            /* 边框：图溢出窗时才画（真实浮层同规则），左右两条从标题栏下缘起 */
            SpFill(dc, 0, 0, w, 1, SPC_BORDER);
            SpFill(dc, 0, hgt - barH - 1, w - barW, 1, SPC_BORDER);
            SpFill(dc, 0, hdr, 1, hgt - barH - hdr, SPC_BORDER);
            SpFill(dc, w - barW - 1, hdr, 1, hgt - barH - hdr, SPC_BORDER);
        }
    }

    /* ---- 标题栏（压在内容之上，与真实浮层同画法）---- */
    SpFill(dc, 0, 0, w, hdr, SPC_BG);
    SpFill(dc, 0, hdr - 1, w, 1, SPC_BORDER);
    SpText(dc, g_spickRow, SPC_TITLE,
           g_spickKind == SPK_LIST ? L"CapsEnhance 剪贴板" : L"图片 1920×1080 100%",
           (int)(12.0f * k), 0, w - (int)(120.0f * k), hdr, DT_LEFT);
    {
        int s = (int)(26.0f * k);
        SpText(dc, g_spickRow, SPC_TITLE, L"✕",
               w - s - (int)(6.0f * k), (hdr - s) / 2, s, s, DT_CENTER);
    }

    /* ---- 外框：1px，压住可能溢出的行底色 / 图 ---- */
    SpFill(dc, 0, 0, w, 1, SPC_BORDER);
    SpFill(dc, 0, hgt - 1, w, 1, SPC_BORDER);
    SpFill(dc, 0, 0, 1, hgt, SPC_BORDER);
    SpFill(dc, w - 1, 0, 1, hgt, SPC_BORDER);

    /* ---- 中央面板：这一对的名字 + 当前尺寸 + 「取消」/「完成」 ---- */
    {
        /* 面板上写明**在调哪一对**：三颗按钮两套样式，最大/最小那两颗开出来的
           窗长得一模一样，不写清楚用户不知道自己改的是哪个。 */
        RECT pr;
        SpPanelRect(w, hgt, &pr);
        int pw = pr.right - pr.left, ph = pr.bottom - pr.top;
        if (pw > 0 && ph > 0) {
            int rad = SpS(10);
            if (rad > pw / 2) rad = pw / 2;
            if (rad > ph / 2) rad = ph / 2;
            HBRUSH wb = CreateSolidBrush(SPC_BG);
            HPEN   bp = CreatePen(PS_SOLID, 1, RGB(190, 190, 190));
            HGDIOBJ ob = SelectObject(dc, wb);
            HGDIOBJ op = SelectObject(dc, bp);
            RoundRect(dc, pr.left, pr.top, pr.right - 1, pr.bottom - 1,
                      rad * 2, rad * 2);
            SelectObject(dc, op);
            SelectObject(dc, ob);
            DeleteObject(bp);
            DeleteObject(wb);

            SpText(dc, g_font, SPC_TITLE, g_spickLabel,
                   pr.left + SpS(8), pr.top + SpS(6), pw - SpS(16), SpS(16), DT_CENTER);
            WCHAR num[48];
            _snwprintf_s(num, 48, _TRUNCATE, L"%d × %d", g_spickW, g_spickH);
            SpText(dc, g_fontBold, SPC_TEXT, num,
                   pr.left, pr.top + SpS(22), pw, SpS(20), DT_CENTER);   /* 22..42，下面 44 起是按钮 */
        }

        /* 「取消」是次级按钮（白底描边）、「完成」是主按钮（强调蓝）：
           一颗"真的改"一颗"不改"，颜色上就该分得出主次。 */
        for (int i = 0; i < SPB_N; i++) {
            RECT br;
            SpBtnRect(i, &br);
            if (br.right - br.left <= 0 || br.bottom - br.top <= 0) continue;  /* 窗太小：画不下就不画 */
            BOOL hot  = (g_spickBtnHot == i);
            BOOL down = (g_spickBtnDown == i);
            COLORREF fill, edge, text;
            if (i == SPB_OK) {
                fill = down ? SPC_ACCENT_P : (hot ? SPC_ACCENT_H : SPC_ACCENT);
                edge = fill;
                text = RGB(255, 255, 255);
            } else {
                fill = down ? RGB(204, 228, 247) : (hot ? RGB(229, 241, 251)
                                                        : RGB(252, 252, 252));
                edge = down ? RGB(0, 84, 153) : (hot ? SPC_ACCENT : RGB(173, 173, 173));
                text = RGB(0, 0, 0);
            }
            HPEN ep = CreatePen(PS_SOLID, 1, edge);
            HBRUSH fb = CreateSolidBrush(fill);
            HGDIOBJ ob2 = SelectObject(dc, fb);
            HGDIOBJ op2 = SelectObject(dc, ep);
            int r2 = SpS(8);
            if (r2 > (br.right - br.left) / 2) r2 = (br.right - br.left) / 2;
            if (r2 > (br.bottom - br.top) / 2) r2 = (br.bottom - br.top) / 2;
            RoundRect(dc, br.left, br.top, br.right - 1, br.bottom - 1, r2 * 2, r2 * 2);
            SelectObject(dc, op2);
            SelectObject(dc, ob2);
            DeleteObject(fb);
            DeleteObject(ep);
            SpText(dc, g_font, text, kSpBtnText[i],
                   br.left, br.top, br.right - br.left, br.bottom - br.top, DT_CENTER);
        }
    }
}

/* 命中：按钮优先（它压在所有热区之上），其次四角，再四边，其余算"中部" */
static int SpHit(POINT pt) {
    int bi = SpHitBtn(pt);
    if (bi == SPB_OK)     return SP_BTN_OK;
    if (bi == SPB_CANCEL) return SP_BTN_CANCEL;
    RECT rc;
    GetClientRect(g_spick, &rc);
    /* 角的热区比边宽（边 6、角 14）：贴着角起拖时好命中，也不会把"边"的中段
       整个吃掉 —— 判定的顺序就是角在前、边在后。 */
    int e = SpS(6), c = SpS(14);
    BOOL l = pt.x < e, r = pt.x >= rc.right - e;
    BOOL t = pt.y < e, b = pt.y >= rc.bottom - e;
    if (pt.x < c && pt.y < c)                             return SP_TL;
    if (pt.x >= rc.right - c && pt.y < c)                 return SP_TR;
    if (pt.x < c && pt.y >= rc.bottom - c)                return SP_BL;
    if (pt.x >= rc.right - c && pt.y >= rc.bottom - c)    return SP_BR;
    if (l) return SP_L;
    if (r) return SP_R;
    if (t) return SP_T;
    if (b) return SP_B;
    return SP_MOVE;
}

/* 当前光标位置的命中 —— WM_SETCURSOR 只给消息不给坐标，得自己问一次 */
static int SpCurHit(void) {
    POINT pt;
    if (!GetCursorPos(&pt) || !ScreenToClient(g_spick, &pt)) return SP_MOVE;
    return SpHit(pt);
}

/* 拖动一步：按模式改一条（或两条）边，像素 -> 值 -> 钳制 -> 值 -> 像素。
   ★ 值是唯一事实来源：像素只是它按 DPI 折出来的显示量 —— 先钳值再回算像素，
     被钳住的那条边才不会"看着还能拖、松手就弹回"。 */
static void SpApplyDrag(int dx, int dy) {
    RECT r0 = g_spickRect0;
    int left = r0.left, top = r0.top, right = r0.right, bottom = r0.bottom;
    int m = g_spickMode;
    if (m == SP_MOVE) {
        int nw = r0.right - r0.left, nh = r0.bottom - r0.top;
        left = r0.left + dx; top = r0.top + dy;
        if (left < g_spickWork.left) left = g_spickWork.left;
        if (top  < g_spickWork.top)  top  = g_spickWork.top;
        if (left + nw > g_spickWork.right)  left = g_spickWork.right  - nw;
        if (top  + nh > g_spickWork.bottom) top  = g_spickWork.bottom - nh;
        RECT now;
        if (GetWindowRect(g_spick, &now) && now.left == left && now.top == top)
            return;                       /* 没动就别再动（拖动去抖，同上） */
        SetWindowPos(g_spick, NULL, left, top, 0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return;
    }
    if (m == SP_L || m == SP_TL || m == SP_BL) left = r0.left + dx;
    if (m == SP_R || m == SP_TR || m == SP_BR) right = r0.right + dx;
    if (m == SP_T || m == SP_TL || m == SP_TR) top = r0.top + dy;
    if (m == SP_B || m == SP_BL || m == SP_BR) bottom = r0.bottom + dy;

    float k  = (float)g_spickDpi / 96.0f;
    int   vw = (int)((float)(right - left)  / k + 0.5f);
    int   vh = (int)((float)(bottom - top)  / k + 0.5f);
    SpClampValue(&vw, &vh);
    int nw = SpPx(vw), nh = SpPx(vh);
    /* 没被拖的那条边钉死：拖左边时右边不动，反之亦然 */
    left = (m == SP_L || m == SP_TL || m == SP_BL) ? r0.right - nw : r0.left;
    top  = (m == SP_T || m == SP_TL || m == SP_TR) ? r0.bottom - nh : r0.top;
    if (left < g_spickWork.left) left = g_spickWork.left;
    if (top  < g_spickWork.top)  top  = g_spickWork.top;
    /* ★ 与当前矩形一致就**什么都不做**：WM_MOUSEMOVE 比屏幕刷新密得多，
       同一个矩形反复 SetWindowPos + 重画，就是"拖动时抖"的一半来源
       （另一半是逐帧重画的开销，见 SpPaint 的双缓冲）。 */
    {
        RECT now;
        if (GetWindowRect(g_spick, &now) &&
            now.left == left && now.top == top &&
            now.right - now.left == nw && now.bottom - now.top == nh)
            return;
    }
    g_spickW = vw;
    g_spickH = vh;
    /* SWP_NOCOPYBITS：改大小时别让系统把旧内容搬到新位置 —— 拖左/上边时
       那块旧内容会先跟着窗口滑一下再被重画盖掉，看着就是"抖一下"。
       我们本来就是整块重画，不需要它搬。 */
    SetWindowPos(g_spick, NULL, left, top, nw, nh,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
    /* 立刻画掉：InvalidateRect 只排队，等下一轮 WM_PAINT 才画的话，窗口
       位置已经变了、画出来的还是上一帧的排版 —— 这也是抖。 */
    InvalidateRect(g_spick, NULL, FALSE);
    UpdateWindow(g_spick);
}

/* 收场：save = 把值填回那一对输入框（ini 不写）。
   主窗口的消息循环也要能叫它：弹窗没有关闭按钮，Esc 万一没落在弹窗身上
   （主窗口此时是禁用的，焦点有可能回到主窗口）就成了"关不掉"，所以循环里
   另有一条 Esc 兜底路径（见 SizePickerIsOpen 的调用处）。 */
void SizePickerClose(BOOL save) {
    if (!g_spick) return;
    if (save) {
        SetIntField(GetDlgItem(g_hwnd, g_spickIdW), g_spickW);
        SetIntField(GetDlgItem(g_hwnd, g_spickIdH), g_spickH);
        UpdateDirty();     /* 按基线重新判定：填回来的值一样就不算改动 */
        SetStatus(L"窗口大小已填回 %d × %d（96 DPI 像素）—— 还要点底部「保存」才写进 ini",
                  g_spickW, g_spickH);
    } else {
        /* ★ 取消 = 一个字都不往输入框里写。输入框里是什么，关掉弹窗后还是
           什么（弹窗从不改 ini，只用"完成"往界面里填一次值）。 */
        SetStatus(L"已取消：窗口大小没变");
    }
    if (g_hwnd) EnableWindow(g_hwnd, TRUE);     /* 先还主窗口，再拆弹窗（焦点有着落） */
    if (g_spickRow)   { DeleteObject(g_spickRow);   g_spickRow = NULL; }
    if (g_spickSmall) { DeleteObject(g_spickSmall); g_spickSmall = NULL; }
    if (g_spickChkBmp) { DeleteObject(g_spickChkBmp); g_spickChkBmp = NULL; g_spickChkCell = 0; }
    DestroyWindow(g_spick);
    g_spick = NULL;
    g_spickMode = SP_NONE;
    if (g_hwnd) { SetForegroundWindow(g_hwnd); BringWindowToTop(g_hwnd); }
}

static LRESULT CALLBACK SpWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND:
        return 1;                       /* 整块自画，先擦一遍只会闪 */
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (dc) SpPaint(h, dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_RBUTTONUP:
        SizePickerClose(FALSE);               /* 右键 = 放弃（与"选窗添加"同一套取消手势） */
        return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE)               { SizePickerClose(FALSE); return 0; }
        if (w == VK_RETURN || w == VK_SPACE) { SizePickerClose(TRUE);  return 0; }
        break;
    case WM_LBUTTONDOWN: {
        POINT pt;
        pt.x = (LONG)(short)LOWORD(l);
        pt.y = (LONG)(short)HIWORD(l);
        int hit = SpHit(pt);
        if (hit == SP_BTN_OK || hit == SP_BTN_CANCEL) {
            g_spickBtnDown = (hit == SP_BTN_OK) ? SPB_OK : SPB_CANCEL;
            InvalidateRect(h, NULL, FALSE);
            UpdateWindow(h);
            return 0;
        }
        g_spickMode = hit;
        GetWindowRect(h, &g_spickRect0);
        GetCursorPos(&g_spickPt0);
        SetCapture(h);
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT pt;
        pt.x = (LONG)(short)LOWORD(l);
        pt.y = (LONG)(short)HIWORD(l);
        if (g_spickMode != SP_NONE) {
            POINT cur;
            if (GetCursorPos(&cur))
                SpApplyDrag(cur.x - g_spickPt0.x, cur.y - g_spickPt0.y);
            return 0;
        }
        int hot = SpHitBtn(pt);              /* -1 = 没在按钮上 */
        if (hot != g_spickBtnHot) {
            g_spickBtnHot = hot;
            InvalidateRect(h, NULL, FALSE);
            UpdateWindow(h);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_spickBtnDown >= 0) {          /* 按下时落在某颗按钮上 */
            POINT pt;
            pt.x = (LONG)(short)LOWORD(l);
            pt.y = (LONG)(short)HIWORD(l);
            int hit = SpHitBtn(pt);         /* 抬起时还得在同一颗上才算 */
            int want = (g_spickBtnDown == SPB_OK) ? SPB_OK : SPB_CANCEL;
            g_spickBtnDown = -1;
            if (hit == want) { SizePickerClose(want == SPB_OK); return 0; }
            InvalidateRect(h, NULL, FALSE);
            UpdateWindow(h);
            return 0;
        }
        if (g_spickMode != SP_NONE) {
            g_spickMode = SP_NONE;
            ReleaseCapture();
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        g_spickMode = SP_NONE;
        g_spickBtnDown = -1;
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            /* ★ 拖动中**不再重新命中**：拖右边时光标几乎贴在窗口边上，
               每帧重算会在"边"与"中部"之间来回跳，光标闪成一片。
               拖动期间的光标只由"按下那一刻定下的模式"决定。 */
            int hit = (g_spickMode != SP_NONE) ? g_spickMode : SpCurHit();
            const WCHAR* cur = IDC_ARROW;
            if (hit == SP_BTN_OK || hit == SP_BTN_CANCEL) cur = IDC_HAND;
            else if (hit == SP_L || hit == SP_R)      cur = IDC_SIZEWE;
            else if (hit == SP_T || hit == SP_B)      cur = IDC_SIZENS;
            else if (hit == SP_TL || hit == SP_BR)    cur = IDC_SIZENWSE;
            else if (hit == SP_TR || hit == SP_BL)    cur = IDC_SIZENESW;
            SetCursor(LoadCursorW(NULL, (LPCWSTR)(ULONG_PTR)cur));
            return TRUE;
        }
        break;
    case WM_DPICHANGED: {
        /* 拖到缩放不同的另一块屏：值不动，物理大小按新 DPI 重算（与真实
           浮层同一套语义 —— 存的是 96 DPI 的数，换个屏就该换个物理大小）。 */
        int dpi = (int)HIWORD(w);
        if (dpi < 96) dpi = 96;
        g_spickDpi = dpi;
        SpMakeFonts();
        {
            HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi;
            memset(&mi, 0, sizeof(mi));
            mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(mon, &mi)) g_spickWork = mi.rcWork;
        }
        SpClampValue(&g_spickW, &g_spickH);
        RECT r;
        GetWindowRect(h, &r);
        int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
        SetWindowPos(h, NULL, cx - SpPx(g_spickW) / 2, cy - SpPx(g_spickH) / 2,
                     SpPx(g_spickW), SpPx(g_spickH), SWP_NOZORDER | SWP_NOACTIVATE);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_CLOSE:
        SizePickerClose(FALSE);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}

/* 弹一颗「调整…」对应的示例窗。每次都**现读**那两个输入框（含未保存改动）。 */
void OpenSizePicker(int btnId) {
    int idx = -1;
    for (int i = 0; i < SPICK_N; i++)
        if (kSizePickers[i].btnId == btnId) idx = i;
    if (idx < 0 || g_spick) return;                 /* 已开着就再来一次没意义 */
    const Field* fw = FieldById(kSizePickers[idx].firstId);
    int idH = PairSecondId(kSizePickers[idx].firstId);
    const Field* fh = idH ? FieldById(idH) : NULL;
    if (!fw || !fh) return;

    /* 工作区 = 设置窗口所在显示器（弹窗就摆在它中间） */
    HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) {
        if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &g_spickWork, 0))
            GetClientRect(GetDesktopWindow(), &g_spickWork);
    } else {
        g_spickWork = mi.rcWork;
    }

    g_spickKind = kSizePickers[idx].kind;
    g_spickDpi  = g_dpi;
    g_spickIdW  = fw->id;
    g_spickIdH  = fh->id;
    g_spickLabel = fw->label;
    g_spickLoW  = fw->min; g_spickHiW = fw->max;
    g_spickLoH  = fh->min; g_spickHiH = fh->max;
    SpReadPair(fw, fh, &g_spickW, &g_spickH);
    SpClampValue(&g_spickW, &g_spickH);
    SpMakeFonts();
    g_spickBtnHot = g_spickBtnDown = -1;

    int pxW = SpPx(g_spickW), pxH = SpPx(g_spickH);
    int ww = g_spickWork.right - g_spickWork.left;
    int wh = g_spickWork.bottom - g_spickWork.top;
    int left = g_spickWork.left + (ww - pxW) / 2;
    int top  = g_spickWork.top  + (wh - pxH) / 2;
    if (left < g_spickWork.left) left = g_spickWork.left;
    if (top  < g_spickWork.top)  top  = g_spickWork.top;

    g_spick = CreateWindowExW(WS_EX_TOOLWINDOW, SPICK_CLASS, L"调整窗口大小",
                              WS_POPUP | WS_CLIPCHILDREN,
                              left, top, pxW, pxH,
                              g_hwnd, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_spick) { if (g_hwnd) EnableWindow(g_hwnd, TRUE); return; }
    ShowWindow(g_spick, SW_SHOWNORMAL);
    EnableWindow(g_hwnd, FALSE);       /* 模态：主窗口先歇着，改动只走弹窗 */
    SetFocus(g_spick);                 /* Esc / 回车要落在弹窗上 */
    SetStatus(L"拖动示例窗口定尺寸：中部拖 = 挪窗，四边四角拖 = 改大小；"
              L"「完成」把值填回设置界面的输入框，「取消」/ Esc 一个字都不改");
}

/* 注册弹窗的窗口类（WinMain 在建主窗之前调一次）。
   类归这里注册而不是主文件：事件过程、类名、底色与光标都是弹窗自己的事，
   摊到主文件里只会让"主窗口长什么样"和"弹窗长什么样"混在一起。
   底色给白：整块内容自己画（WM_ERASEBKGND 直接返回 1），只有圆角外的缝隙
   和首次显示前的那一瞬会用到类底色，白比按钮灰更不显眼。 */
BOOL SizePickerRegister(HINSTANCE hi) {
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = SpWndProc;
    wc.hInstance     = hi;
    wc.hCursor       = LoadCursorW(NULL, (LPCWSTR)(ULONG_PTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.lpszClassName = SPICK_CLASS;
    return RegisterClassW(&wc) != 0;
}

/* 弹窗开着吗（主窗口的消息循环要用它拦 Esc） */
BOOL SizePickerIsOpen(void) { return g_spick != NULL; }

/* 三颗「调整…」随页显隐：表在这里，主文件不该为了开关可见性再抄一份 id。
   判据照 ShowCtrl 那套——按 WS_VISIBLE **风格位**比，状态没变就不发消息。 */
void SizePickerShowButtons(BOOL show) {
    for (int i = 0; i < SPICK_N; i++) {
        HWND b = GetDlgItem(g_hwnd, kSizePickers[i].btnId);
        if (!b) continue;
        BOOL vis = (GetWindowLongW(b, GWL_STYLE) & WS_VISIBLE) != 0;
        if (vis != show) ShowWindow(b, show ? SW_SHOW : SW_HIDE);
    }
}
