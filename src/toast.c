/* ================= 超级复制：结果预览浮层（Caps + C） =================
   "复制到剪贴板的内容"要在屏幕上可见一小会儿，用来判断这次到底复制成功没有：
   字对不对、是不是只取到半截、失败时又是什么原因。

   为什么是本进程内的一条**独立线程**，而不是像帮助 / 窗口信息那样拆成独立 exe：
     · 它是一次性、短寿命、**不接收任何输入**的提示：没有键位图那种常驻位图，
       也没有窗口信息那种编辑状态与事件循环；
     · 为它加第 5 个 exe，就要同时改 build.bat / sources.txt / CI 上传清单 /
       README / 设置程序 —— 换来只是几十 kb 的故障隔离，不划算。
   为什么必须独立线程、不能挂在主线程上：主线程是**钩子宿主**。给它加一个
   带定时器的窗口，等于让"画提示"和"处理系统级键盘输入"抢同一个线程
   （见 docs/DEVELOPMENT.md 那条"会不会阻塞主线程超过 ~300ms"的判据）。
   本线程只在被显示时才画一帧，闲时只有一个隐藏窗口，没有周期性流量。

   ★ 渲染方式：**逐像素 alpha（UpdateLayeredWindow）**，不是整窗 alpha。
   整窗 alpha（SetLayeredWindowAttributes + LWA_ALPHA）实现简单，但有三个
   真实缺点，都在实测里撞到过：
     1. 圆角只能靠 SetWindowRgn 裁 —— 那是 1 bit 的裁剪，四角全是硬锯齿；
     2. 连正文一起被 5% 的底色稀释，小字号看着发虚；
     3. 边框只能沿矩形画，四角的线正好在裁剪区外，"直边有框、转角没框"。
   逐像素之后：圆角与边框由 GDI+ 抗锯齿绘制，正文区域 alpha 保持 255
   （完全不与桌面混色），只有最外圈 1~2 px 做羽化。

   ★ 窗口样式仍是四件套 + 一个额外约束：
     WS_EX_LAYERED     —— 逐像素 alpha 的前提。
     WS_EX_NOACTIVATE  —— 绝不抢焦点（抢焦点会把用户正在输入的目标切走）。
     WS_EX_TOPMOST     —— 盖在别的窗口之上，但不激活它们。
     WS_EX_TOOLWINDOW  —— 不进任务栏、不进 Alt+Tab。
     WS_EX_TRANSPARENT —— 点击穿透：它挡在屏幕上时，下面那一块仍然能点。
   由此带来一个必须接受的限制：**它收不到任何键盘消息**，所以没有 Esc 关闭。
   提前关掉它的方式是再按一次 Caps+C（下一次取词前会先 ToastHideNow）。 */

#include "cap.h"
#include "copy_text.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* GDI+ 的头文件在 /analyze 下会吐一批与本项目无关的诊断（GdiplusFontCollection.h
   的 C6385、GdiplusHeaders.h 的 SAL 注释不一致 C28252/C28253）。它们全部指向 SDK
   头内部，不是我们的代码 —— 围绕 include 关掉，出了这段恢复，我们自己代码的
   /analyze 基线不受影响。 */
#pragma warning(push)
#pragma warning(disable: 28252 28253 6385 6386)
#include <objidl.h>        /* GDI+ 需要 */
#include <gdiplus.h>
#pragma warning(pop)
#pragma comment(lib, "gdiplus.lib")

using namespace Gdiplus;

#define TOAST_CLASS   L"CapsEnhanceToast"
#define WM_TOAST_SHOW (WM_APP + 40)
#define WM_TOAST_HIDE (WM_APP + 41)
#define TID_LIFE      1

/* 交付颜色（浅色面板，与按键帮助浮层同一套观感）。
   边框取 160 而不是 199：外圈半透明时 199 几乎看不见，边框会"有等于没有"。
   分量单独定义一份，是因为 GDI+ 那条路要用 ARGB 拼色 —— 拿 GetXValue 去拆
   一个 RGB() 常量会在 /W4 下报 C4310（宏内部把它先截成 WORD）。 */
#define C_BORDER_R   160
#define C_BORDER_G   160
#define C_BORDER_B   160
#define C_BG_PANEL   RGB(252, 252, 252)
#define C_BG_BORDER  RGB(C_BORDER_R, C_BORDER_G, C_BORDER_B)
#define C_TEXT_HEAD  RGB(108, 108, 108)
#define C_TEXT_META  RGB(154, 154, 154)   /* 标题行右侧的计数：比标题更淡一级 */
#define C_TEXT_BODY  RGB(26, 26, 26)
#define C_TEXT_FAIL  RGB(176, 0, 32)

typedef struct {
    WCHAR* head;
    WCHAR* meta;    /* 标题行右侧的计数（"124 字 · 5 行"）；NULL/空 = 不显示 */
    WCHAR* body;
    BOOL   fail;
} ToastMsg;   /* 跨线程投递：由调用线程 malloc，窗口线程负责 free */

typedef struct {
    ToastMsg msg;        /* 已接管的三条字符串 */
    HFONT    fHead, fBody;
    RECT     headRect;   /* 已排版好的矩形（窗口坐标） */
    RECT     metaRect;
    RECT     bodyRect;
    int      w, h;
    int      radius;     /* 圆角半径（物理像素） */
} ToastState;

static ToastState      s_st;
static HANDLE          s_ready = NULL;
static HWND            s_hwnd = NULL;
static volatile LONG   s_started = 0;

/* 颜色层与蒙版层：两张 32bpp 自顶向下 DIB，尺寸随内容变，复用不重建。 */
static HBITMAP s_colBmp = NULL, s_mskBmp = NULL;
static HDC     s_colDC  = NULL, s_mskDC  = NULL;
static void*   s_colBits = NULL;
static void*   s_mskBits = NULL;
static int     s_bufW = 0, s_bufH = 0;
static ULONG_PTR s_gpToken = 0;

/* ---------- 屏幕度量（与 help.c 同一套：动态取址，老系统上不硬 import） ---------- */

static void MonitorWork(HMONITOR mon, RECT* out) {
    MONITORINFO mi;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (mon && GetMonitorInfoW(mon, &mi)) { *out = mi.rcWork; return; }
    out->left = 0;
    out->top = 0;
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
            if (SUCCEEDED(f(mon, 0 /* MDT_EFFECTIVE_DPI */, &dx, &dy)) && dx >= 96)
                dpi = (int)dx;
        }
        FreeLibrary(m);
    }
    return dpi ? dpi : 96;
}

/* ---------- 正文截断 ----------
   ★ 只截**显示**：剪贴板里永远是全文（见 textgrab.c 的写入顺序 —— 它先写
   剪贴板再弹浮层，两者用的不是同一份文本）。截断规则本身在 copy_text.c，
   与清洗共用同一套换行语义。 */
static WCHAR* TruncateBody(const WCHAR* src, int* outLen) {
    int len = (int)wcslen(src);
    BOOL cut = FALSE;
    int n = CopyPreviewLen(src, len, CfgInt(CFG_COPY_MAX_CHARS),
                           CfgInt(CFG_COPY_MAX_LINES), &cut);

    WCHAR* out = (WCHAR*)malloc(((size_t)n + 2) * sizeof(WCHAR));
    if (!out) { *outLen = 0; return NULL; }
    if (n > 0) memcpy(out, src, (size_t)n * sizeof(WCHAR));
    out[n] = 0;
    if (cut) wcscat_s(out, (size_t)n + 2, L"…");
    *outLen = n + (cut ? 1 : 0);
    return out;
}

/* ---------- 缓冲区 ---------- */

static void FreeBuffers(void) {
    if (s_colDC)  { DeleteDC(s_colDC); s_colDC = NULL; }
    if (s_mskDC)  { DeleteDC(s_mskDC); s_mskDC = NULL; }
    if (s_colBmp) { DeleteObject(s_colBmp); s_colBmp = NULL; }
    if (s_mskBmp) { DeleteObject(s_mskBmp); s_mskBmp = NULL; }
    s_colBits = s_mskBits = NULL;
    s_bufW = s_bufH = 0;
}

static BOOL MakeDib(int w, int h, HBITMAP* outBmp, HDC* outDC, void** outBits) {
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;      /* 负高 = 自顶向下，行序与 GDI 绘制一致 */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* bits = NULL;
    HBITMAP bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp) return FALSE;
    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) { DeleteObject(bmp); return FALSE; }
    SelectObject(dc, bmp);
    *outBmp = bmp;
    *outDC = dc;
    *outBits = bits;
    return TRUE;
}

static BOOL EnsureBuffers(int w, int h) {
    if (w == s_bufW && h == s_bufH && s_colDC && s_mskDC) return TRUE;
    FreeBuffers();
    if (!MakeDib(w, h, &s_colBmp, &s_colDC, &s_colBits)) return FALSE;
    if (!MakeDib(w, h, &s_mskBmp, &s_mskDC, &s_mskBits)) { FreeBuffers(); return FALSE; }
    s_bufW = w;
    s_bufH = h;
    return TRUE;
}

/* 圆角矩形路径：与蒙版、边框共用，保证两者的弧线完全重合。 */
static void AddRoundRect(GraphicsPath* p, REAL x, REAL y, REAL w, REAL h, REAL r) {
    if (r < 0) r = 0;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r <= 0.01f) { p->AddRectangle(RectF(x, y, w, h)); return; }
    REAL d = r * 2;
    p->AddArc(x,         y,         d, d, 180, 90);
    p->AddArc(x + w - d, y,         d, d, 270, 90);
    p->AddArc(x + w - d, y + h - d, d, d, 0,   90);
    p->AddArc(x,         y + h - d, d, d, 90,  90);
    p->CloseFigure();
}

/* ---------- 排版 ---------- */

static void FreeState(void) {
    free(s_st.msg.head);
    free(s_st.msg.meta);
    free(s_st.msg.body);
    if (s_st.fHead) DeleteObject(s_st.fHead);
    if (s_st.fBody) DeleteObject(s_st.fBody);
    memset(&s_st, 0, sizeof(s_st));
}

/* 把一条消息排版成窗口尺寸与两个文本矩形。失败返回 FALSE（窗口保持隐藏）。 */
static BOOL Layout(HWND hwnd, ToastMsg* in) {
    FreeState();
    s_st.msg.head = in->head;  in->head = NULL;
    s_st.msg.meta = in->meta;  in->meta = NULL;
    s_st.msg.body = in->body;  in->body = NULL;
    s_st.msg.fail = in->fail;

    POINT cur;
    if (!GetCursorPos(&cur)) cur.x = cur.y = 0;
    HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);
    int dpi = DpiOfMonitor(mon);
    int pad = MulDiv(14, dpi, 96);          /* 左右内边距 */
    int padTop = MulDiv(9, dpi, 96);        /* 顶部：小字号的行高自带留白，按 14 会显得空 */
    int padBottom = MulDiv(10, dpi, 96);
    int gap = MulDiv(5, dpi, 96);           /* 标题行与正文之间 */
    int metaGapX = MulDiv(14, dpi, 96);     /* 标题与右侧计数之间至少留这么远 */
    int maxW = MulDiv(CfgInt(CFG_COPY_WIDTH_MAX), dpi, 96);
    int minW = MulDiv(280, dpi, 96);
    int radius = MulDiv(CfgInt(CFG_COPY_RADIUS), dpi, 96);
    if (maxW < minW) maxW = minW;
    if (radius < 0)  radius = 0;
    if (radius > 64) radius = 64;

    /* 字体：名字留空就用系统默认 UI 字体（CreateFontIndirectW 用空 face 名会
       拿到一个怪异的回退，所以显式给一个默认）。字号不开放，理由见 config.c。 */
    const WCHAR* face = CfgStr(CFG_COPY_FONT);
    if (!face || !*face) face = L"Microsoft YaHei UI";

    LOGFONTW lf;
    memset(&lf, 0, sizeof(lf));
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = CLEARTYPE_QUALITY;
    lf.lfWeight  = FW_NORMAL;
    lf.lfHeight  = -MulDiv(10, dpi, 72);
    wcscpy_s(lf.lfFaceName, LF_FACESIZE, face);
    s_st.fBody = CreateFontIndirectW(&lf);
    lf.lfHeight = -MulDiv(9, dpi, 72);
    s_st.fHead = CreateFontIndirectW(&lf);

    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) return FALSE;
    int textW = maxW - 2 * pad;

    RECT headRc;
    SetRect(&headRc, 0, 0, textW, 0);
    SelectObject(dc, s_st.fHead ? s_st.fHead : GetStockObject(DEFAULT_GUI_FONT));
    DrawTextW(dc, s_st.msg.head ? s_st.msg.head : L"", -1, &headRc,
              DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    int headH = headRc.bottom - headRc.top;
    int headW = headRc.right - headRc.left;

    /* 正文宽度决策：**先量不折行的自然宽度**，再决定窗口多宽。
       超过上限就一律用满上限并折行 —— 折行宽度必须与绘制宽度是同一个数，
       否则"按 A 宽量、按 B 宽画"会多折出一行，最后一行被裁掉（实测症状：
       必应搜索框那种长占位提示 + 一行内容，尾部少一个字）。 */
    const WCHAR* bodyText = s_st.msg.body ? s_st.msg.body : L"";
    SelectObject(dc, s_st.fBody ? s_st.fBody : GetStockObject(DEFAULT_GUI_FONT));

    RECT one;
    SetRect(&one, 0, 0, 0, 0);
    DrawTextW(dc, bodyText, -1, &one, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    int bodyNaturalW = one.right - one.left;
    int lineH = one.bottom - one.top;

    /* 计数（可选）：与标题同处**顶部一行**，贴右边缘。
       先量出来 —— 窗口宽度必须同时容得下「标题 + 间隔 + 计数」。 */
    int metaH = 0, metaW = 0;
    if (s_st.msg.meta && s_st.msg.meta[0]) {
        SelectObject(dc, s_st.fHead ? s_st.fHead : GetStockObject(DEFAULT_GUI_FONT));
        RECT mr;
        SetRect(&mr, 0, 0, textW, 0);
        DrawTextW(dc, s_st.msg.meta, -1, &mr, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        metaH = mr.bottom - mr.top;
        metaW = mr.right - mr.left;
    }

    int rowW = headW + (metaW > 0 ? metaGapX + metaW : 0);
    int rowH = headH > metaH ? headH : metaH;

    int w, bodyH;
    if (bodyNaturalW <= textW) {
        /* 一行放得下：窗口随内容收窄 */
        int contentW = rowW > bodyNaturalW ? rowW : bodyNaturalW;
        w = contentW + 2 * pad;
        bodyH = lineH;
    } else {
        /* 需要折行：用满上限宽度，按这个宽度量高度 */
        w = maxW;
        RECT wrap;
        SetRect(&wrap, 0, 0, textW, 0);
        DrawTextW(dc, bodyText, -1, &wrap,
                  DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
        bodyH = wrap.bottom - wrap.top;
        /* 标题 + 计数比正文上限还宽（长程序名 + 极短正文）时，宁可让窗口宽一点，
           也不把计数挤出行外 */
        if (rowW + 2 * pad > w) w = rowW + 2 * pad;
    }
    if (w < minW) w = minW;
    if (w > maxW) w = maxW;
    DeleteDC(dc);

    int h = padTop + rowH + gap + bodyH + padBottom;
    int minH = MulDiv(40, dpi, 96);
    if (h < minH) h = minH;

    /* 位置：所在显示器工作区**水平居中**、纵向在 pct% 处（默认 72%）。 */
    RECT work;
    MonitorWork(mon, &work);
    int workW = work.right - work.left;
    int workH = work.bottom - work.top;
    int pct = CfgInt(CFG_COPY_POS_PERCENT);
    if (pct < 50) pct = 50;
    if (pct > 90) pct = 90;

    int x = work.left + (workW - w) / 2;
    int y = work.top + MulDiv(workH, pct, 100);
    int bottomLimit = work.bottom - MulDiv(8, dpi, 96);
    if (y + h > bottomLimit) y = bottomLimit - h;
    if (y < work.top) y = work.top;
    if (x < work.left) x = work.left;

    /* 顶部一行：标题在左、计数贴右。标题的右边界让出计数所占的宽度，
       两者永远不会重叠（标题过长时按 DT_END_ELLIPSIS 截）。 */
    s_st.headRect.left = pad;
    s_st.headRect.top = padTop;
    s_st.headRect.right = (metaW > 0) ? (w - pad - metaW - metaGapX) : (w - pad);
    if (s_st.headRect.right < s_st.headRect.left + MulDiv(24, dpi, 96))
        s_st.headRect.right = s_st.headRect.left + MulDiv(24, dpi, 96);
    s_st.headRect.bottom = padTop + rowH;

    s_st.bodyRect.left = pad;
    s_st.bodyRect.top = padTop + rowH + gap;
    s_st.bodyRect.right = w - pad;
    s_st.bodyRect.bottom = s_st.bodyRect.top + bodyH;

    if (metaW > 0) {
        s_st.metaRect.left = w - pad - metaW;
        s_st.metaRect.top = padTop;
        s_st.metaRect.right = w - pad;
        s_st.metaRect.bottom = padTop + rowH;
    } else {
        SetRect(&s_st.metaRect, 0, 0, 0, 0);
    }

    s_st.w = w;
    s_st.h = h;
    s_st.radius = radius;

    if (!EnsureBuffers(w, h)) return FALSE;

    /* ---- 颜色层：底色 + 抗锯齿圆角边框 + 正文 ---- */
    RECT all;
    SetRect(&all, 0, 0, w, h);
    HBRUSH bg = CreateSolidBrush(C_BG_PANEL);
    FillRect(s_colDC, &all, bg);
    DeleteObject(bg);

    {
        Graphics g(s_colDC);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        GraphicsPath path;
        /* 0.5 偏移让 1px 线正好落在一个像素上（落在整数坐标会被摊成 2px 灰边） */
        AddRoundRect(&path, 0.5f, 0.5f, (REAL)w - 1.0f, (REAL)h - 1.0f, (REAL)radius);
        /* 显式拼 ARGB：分量直接取宏定义，不经 GetXValue —— 那几个宏会先把
           RGB() 常量截成 WORD/BYTE，/W4 下报 C4310。 */
        ARGB borderArgb = 0xFF000000u
                        | ((ARGB)C_BORDER_R << 16)
                        | ((ARGB)C_BORDER_G << 8)
                        |  (ARGB)C_BORDER_B;
        Pen pen(Color(borderArgb), 1.0f);
        g.DrawPath(&pen, &path);
    }

    SetBkMode(s_colDC, TRANSPARENT);
    RECT hr = s_st.headRect;
    SetTextColor(s_colDC, s_st.msg.fail ? C_TEXT_FAIL : C_TEXT_HEAD);
    SelectObject(s_colDC, s_st.fHead ? s_st.fHead : GetStockObject(DEFAULT_GUI_FONT));
    DrawTextW(s_colDC, s_st.msg.head ? s_st.msg.head : L"", -1, &hr,
              DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);

    RECT br = s_st.bodyRect;
    SetTextColor(s_colDC, C_TEXT_BODY);
    SelectObject(s_colDC, s_st.fBody ? s_st.fBody : GetStockObject(DEFAULT_GUI_FONT));
    DrawTextW(s_colDC, s_st.msg.body ? s_st.msg.body : L"", -1, &br,
              DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);

    if (s_st.metaRect.bottom > s_st.metaRect.top) {
        RECT mr = s_st.metaRect;
        SetTextColor(s_colDC, C_TEXT_META);
        SelectObject(s_colDC, s_st.fHead ? s_st.fHead : GetStockObject(DEFAULT_GUI_FONT));
        DrawTextW(s_colDC, s_st.msg.meta ? s_st.msg.meta : L"", -1, &mr,
                  DT_SINGLELINE | DT_RIGHT | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    /* ---- 蒙版层：抗锯齿圆角矩形（黑底白形，R 通道即覆盖度） ---- */
    {
        RECT allm;
        SetRect(&allm, 0, 0, w, h);
        HBRUSH black = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(s_mskDC, &allm, black);
        DeleteObject(black);

        Graphics g(s_mskDC);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetCompositingMode(CompositingModeSourceOver);
        GraphicsPath path;
        AddRoundRect(&path, 0.0f, 0.0f, (REAL)w, (REAL)h, (REAL)radius);
        SolidBrush white(Color(255, 255, 255, 255));
        g.FillPath(&white, &path);
    }

    /* ---- 合层：预乘 alpha，正文区（覆盖度 255）保持完全不透明 ---- */
    int alphaCfg = CfgInt(CFG_COPY_ALPHA);
    if (alphaCfg < 60)  alphaCfg = 60;
    if (alphaCfg > 255) alphaCfg = 255;

    const BYTE* msk = (const BYTE*)s_mskBits;
    BYTE* col = (BYTE*)s_colBits;
    if (msk && col) {
        int n = w * h;
        for (int i = 0; i < n; i++) {
            BYTE* d = col + i * 4;                 /* BGRA（DIB 字节序） */
            BYTE* m = (BYTE*)msk + i * 4;
            int cov = m[2];                        /* R 通道：本次覆盖度 */
            int a = cov * alphaCfg / 255;
            d[0] = (BYTE)(d[0] * a / 255);
            d[1] = (BYTE)(d[1] * a / 255);
            d[2] = (BYTE)(d[2] * a / 255);
            d[3] = (BYTE)a;
        }
    }

    POINT src = { 0, 0 };
    POINT dst = { x, y };
    SIZE  sz  = { w, h };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(hwnd, NULL, &dst, &sz, s_colDC, &src, 0, &bf, ULW_ALPHA);

    SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    return TRUE;
}

/* ---------- 窗口过程 ---------- */

static void HideNow(HWND hwnd) {
    KillTimer(hwnd, TID_LIFE);
    ShowWindow(hwnd, SW_HIDE);
    FreeState();
}

static LRESULT CALLBACK ToastProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TOAST_SHOW: {
        ToastMsg* m = (ToastMsg*)lp;
        if (!m) return 0;
        KillTimer(hwnd, TID_LIFE);
        if (Layout(hwnd, m)) {
            int ms = m->fail ? CfgInt(CFG_COPY_FAIL_STAY_MS) : CfgInt(CFG_COPY_STAY_MS);
            if (ms < 500) ms = 500;
            SetTimer(hwnd, TID_LIFE, (UINT)ms, NULL);
        } else {
            ShowWindow(hwnd, SW_HIDE);
            FreeState();
        }
        free(m->head);
        free(m->meta);
        free(m->body);
        free(m);
        return 0;
    }
    case WM_TOAST_HIDE:
        HideNow(hwnd);
        return 0;
    case WM_TIMER:
        if (wp == TID_LIFE) { HideNow(hwnd); return 0; }
        break;
    case WM_ERASEBKGND:
        return 1;              /* 内容全在分层表面里，不要擦 */
    case WM_PAINT: {
        /* 分层窗口由 UpdateLayeredWindow 交付像素，这里只需把区域验证掉 */
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_NCHITTEST:
        return HTTRANSPARENT;  /* 双保险：WS_EX_TRANSPARENT 之外再明确一次 */
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ---------- 线程 ---------- */

static DWORD WINAPI ToastThread(LPVOID p) {
    (void)p;

    GdiplusStartupInput gpi;
    GdiplusStartup(&s_gpToken, &gpi, NULL);

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ToastProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = TOAST_CLASS;
    RegisterClassExW(&wc);

    s_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
        TOAST_CLASS, L"", WS_POPUP, 0, 0, 10, 10, NULL, NULL, wc.hInstance, NULL);
    if (s_ready) SetEvent(s_ready);
    if (!s_hwnd) return 0;

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    FreeBuffers();
    FreeState();
    if (s_gpToken) GdiplusShutdown(s_gpToken);
    return 0;
}

/* 懒启动：第一次要显示时才建线程与窗口，之后一直复用（闲时只有一个隐藏
   窗口，没有定时器、没有流量）。 */
static BOOL ToastEnsure(void) {
    if (InterlockedCompareExchange(&s_started, 1, 0) == 0) {
        s_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!s_ready || !CreateThread(NULL, 0, ToastThread, NULL, 0, NULL)) {
            InterlockedExchange(&s_started, 0);
            return FALSE;
        }
    }
    if (!s_ready) return FALSE;
    WaitForSingleObject(s_ready, 1500);
    return s_hwnd != NULL;
}

/* ---------- 对外 ---------- */

void ToastShow(BOOL fail, const WCHAR* title, const WCHAR* meta, const WCHAR* body) {
    if (!ToastEnsure()) return;

    ToastMsg* m = (ToastMsg*)calloc(1, sizeof(ToastMsg));
    if (!m) return;
    m->fail = fail ? TRUE : FALSE;

    int shown = 0;
    m->head = _wcsdup(title ? title : L"");
    m->body = TruncateBody(body ? body : L"", &shown);
    /* 计数行可以缺省（失败提示没有它）；拷贝失败就当作没有，不让整条提示消失 */
    if (meta && *meta) m->meta = _wcsdup(meta);
    if (!m->head || !m->body) {
        free(m->head);
        free(m->meta);
        free(m->body);
        free(m);
        return;
    }

    if (!PostMessageW(s_hwnd, WM_TOAST_SHOW, 0, (LPARAM)m)) {
        free(m->head);
        free(m->meta);
        free(m->body);
        free(m);
    }
}

void ToastHideNow(void) {
    if (!s_hwnd) return;
    /* 同步：SendMessage 返回时窗口已经隐藏 —— 采样必须在这一刻之后才开始，
       否则命中测试会先命中我们自己的浮层（见 textgrab.c 的入口注释）。 */
    SendMessageW(s_hwnd, WM_TOAST_HIDE, 0, 0);
}

