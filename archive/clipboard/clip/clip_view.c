/* ======================================================================
   CapsEnhance —— F3 图片预览窗口（原 clip.c 的预览段，拆分而出）
   职责：预览窗（自绘滚动条 / 放大镜光标 / 点击切换适应窗口 / 条带滚动），
   尺寸贴内容（ClipViewMaxW/H 上限、ClipViewMinW/H 下限，均本程序配置）。
   列表数据与行几何来自 clip.c；解码走 clip_thumb.c（预览请求 + 收割）。
   ====================================================================== */
#include "cap.h"
#include "clip.h"

#include <math.h>

#pragma warning(push)
#pragma warning(disable: 6385 28252 28253)
#include <gdiplus.h>
#pragma warning(pop)

#include <stdio.h>
#include <wchar.h>

/* 本模块内部的前置声明（文件内的定义顺序保持原样，未重排） */
static BOOL  ViewSurface(void);
static void  ViewBtnRect(int i, Gdiplus::RectF* out);
static int   ViewHitBtn(float x, float y);
static void  ViewHarvest(void);
static void  ViewPaintSoon(void);
static BOOL  ScrollPaintStrip(int dx, int dy);
static void  ViewSyncScroll(int anchor);
static void  ViewScrollBy(int bar, int steps);
static void  ViewAnimStep(void);
static void  ViewPaint(void);
static void  ViewNotifyState(void);
static void  ViewFitOnce(void);
static void  ViewEnsureWindow(void);
static HCURSOR MakeZoomCursor(BOOL plus);

/* ================= F3 图片预览（Ditto 的 View Full Description 同位功能） =================
   Ditto 主列表按 F3 弹的"查看完整说明"窗口（CToolTipEx + CImageViewer），这里
   只取图片那一半：文本条目的全文预览已由悬停提示覆盖（TipTextOf），列表里那
   76px 的缩略图才是真正看不清的东西。非图片条目按 F3 什么都不做 —— 按了没
   反应比弹一个"没有图片"的空壳诚实。

   交互（按用户实测的 Ditto 行为对齐；第一版"滚轮缩放+拖图平移"是猜错的）：
     · 滚轮 = 上下滚动；Ctrl+滚轮 = 缩放（锚=窗口中心）；横向滚轮 = 左右滚动
     · 横竖两条**自绘**滚动条按需出现（图装不下才预留边条画拇指，见
       ViewFitOnce）；没有拖图平移 —— Ditto 没有
     · 单击图片 = 100% ↔ 适应窗口（Ditto 同款，抬起才判定）；标题栏可拖动挪窗、✕ 关闭
   窗口尺寸（Ditto 勾选项"根据内容调整窗口大小"+"缩放图像以适应窗口"）：
     · 解码完成后窗口才显示，尺寸贴着图像内容（上限 = ClipViewMaxW/H 配置，
       仍不超过工作区）—— 没有先弹
       固定默认再收缩的跳变；图像显示即适应（小图不放大，放大只会糊）
     · ↑↓ 跟随换条目：位置不动，尺寸随新内容重摆
   生命周期（Ditto 同款）：
     · 位置锚在选中行左下角往下展开；Esc / 再按 F3 / 点在两窗之外 / 列表收起
       都会收掉。
     · 窗口本体在本 exe 里的第三个窗口（类名独立，FindWindow(CLIP_CLASS) 的
       握手才会继续命中列表窗），共享 g_items / g_sel / g_k / 字体，零 IPC。
   硬约束：
     · 不抢焦点（WS_EX_NOACTIVATE + SW_SHOWNOACTIVATE，与列表同一套），键盘
       全部由主程序钩子代收转发（F3 / 空格），它自己只处理鼠标；
     · ★ 本进程主线程挂着低级鼠标钩子 —— 重绘走帧合并（15ms 定时器），高频
       mousemove/滚动只标记"脏"。每事件全量重画会把主线程打满，钩子回调排队，
       **用户全系统的光标都会卡**（第一版实测踩过）；
     · 大图解码在后台线程（ThumbThreadProc 的预览轮），主线程只画现成位图。
       列表里的 g_thumb 缩略图已经缩到行高尺寸，不能拿来当大图用。 */
static Gdiplus::Bitmap* g_viewImg = NULL;      /* 已解码的原图（本窗专有：只有 ViewHarvest 换指针） */
static long long        g_viewImgId = 0;       /* g_viewImg 属于哪条；0 = 没有。重开同一条目时
                                                  靠它免解码直接显示（F3 关了再开同一条） */
static HWND      g_view = NULL;
static UINT      g_msgView = 0;        /* 主程序 -> 本进程：toggle / 退场 */
static UINT      g_msgViewState = 0;   /* 本进程 -> 主程序：状态报信 */
static BOOL      g_viewOpen = FALSE;   /* 预览会话进行中（窗口可能还在解码） */
static int       g_viewRow = -1;       /* 预览指向的条目；-1 = 没在显示 */
static long long g_viewId = 0;         /* 同上（id 形式，解码请求用它） */
static int       g_viewW = 0, g_viewH = 0;
static int       g_viewX = 0, g_viewY = 0;
static HDC       g_viewDC = NULL;
static HBITMAP   g_viewBmp = NULL;
static void*     g_viewBits = NULL;
static int       g_viewSurfW = 0, g_viewSurfH = 0;   /* 内容 DIB 的现尺寸 */
static float     g_viewScale = 0.0f;   /* 0 = 100% 原大（默认）；>0 = 显式比例；<0 = 适应窗口（双击切换） */
static BOOL      g_viewFitted = FALSE; /* 本条目按内容定过窗口尺寸（换条目重置） */
static BOOL      g_viewWinDrag = FALSE;     /* 正在拖标题栏挪窗 */
static BOOL      g_viewClickArm = FALSE;    /* 按下发生在图片区：抬起时切换适应窗口 */
static POINT     g_viewDragCur;             /* 按下那一刻的光标位置 */
static POINT     g_viewWinPos;              /* 按下那一刻的窗口位置 */
static BOOL      g_viewPaintPending = FALSE;/* 帧合并：有一帧待画 */
#define VIEW_FRAME_MS     15                  /* 帧合并周期（≈66fps 上限） */
#define VIEW_SCROLL_STEP  120                 /* 滚轮一档 / 滚动条一行（图像像素） */
/* ---- 滚动动画与条带重绘 ----
   滚轮不再一档一档地瞬跳（那正是"滚动条跳动"的观感来源）：每档只把**目标位**
   挪 120px，帧合并时钟里每帧向目标趋近 40%，到了就停 —— 滚动条跟着连续滑。
   重绘也不再整帧来：纯滚动只把已画好的像素 ScrollDC 平移、补画露出的那一条
   （g_viewLastSX/SY 记上一帧位置，g_viewFullPaint 标记"内容变了必须整画"）。
   本线程宿着低级鼠标钩子，整帧重画期间全系统的输入都在排队 —— 这两条把
   单帧成本从"全表面填充+图像采样"压到"一次内存平移 + 一条"。 */
static int  g_viewLastSX = 0, g_viewLastSY = 0;   /* 上一帧画到哪个滚动位 */
static BOOL g_viewFullPaint = TRUE;               /* 下一帧必须整画（换图/缩放/首帧） */
static int  g_viewTargetX = 0, g_viewTargetY = 0; /* 滚动动画的目标位 */
static BOOL g_animX = FALSE, g_animY = FALSE;     /* 各轴动画进行中（横竖独立：
   拖动只停自己那轴；若共用一个旗标，拖完横向再滚轮会把纵向动画启动成
   "横竖两轴一起滑"，横向就向过期的旧目标滑回去 —— 实测即"横滚后再滚轮
   回到最左"） */
#define VIEW_ANCHOR_KEEP    0                 /* ViewSyncScroll：现位 clamp 进新范围 */
#define VIEW_ANCHOR_CENTER  1                 /*   滚到内容中心（缩放锚 = 窗口中心） */
#define VIEW_ANCHOR_TOP     2                 /*   回到顶/左（新内容从头看） */
#define VIEW_BTN_COUNT 1                    /* 只剩 ✕（复制按钮已删） */

/* 当前生效缩放。Ditto 勾上"根据内容调整窗口大小 + 缩放图像以适应窗口"后
   的显示态是 **100% 原大、顶对齐、超窗走滚动条**（实测澄清："缩放图像以
   适应窗口"的真实语义是**窗口适应图像**，那件事由 ViewFitOnce 管，图像
   自己不缩）。g_viewScale < 0 是"适应窗口"显式模式（双击图片切换）：
   图像缩进客户区，基于客户区算（滚动条边条已扣除），小图不放大（放大糊）。 */
/* ---- 自绘滚动条（替换系统 SCROLLBAR 控件）----
   系统滚动条带箭头、体量笨重（用户嫌丑），改成细条 + 圆角拇指的现代画法：
   无箭头无轨道，只有半透明灰拇指（常态 alpha 120、悬停 160、拖动 210，
   颜色随状态加深），图不超窗时完全不画、边条也不预留（按需出现）。
   位置：垂直条 = 标题栏以下的一条右缘窄带；水平条 = 底部一整条窄带；
   右下角交叉处留空。滚动位/范围是自己的状态（不再有 SetScrollInfo）。 */
static int  g_vPos = 0, g_vMax = 0, g_vPage = 1;   /* 垂直：现位 / 可滚上限 / 视口高 */
static int  g_hPos = 0, g_hMax = 0, g_hPage = 1;   /* 水平：同上 */
static int  g_vHover = 0, g_hHover = 0;         /* 拇指悬停 */
static BOOL g_vDrag = FALSE, g_hDrag = FALSE;   /* 拇指拖动中 */
static int  g_vDragGrab = 0, g_hDragGrab = 0;   /* 按下点相对拇指顶/左的偏移 */
static BOOL g_barHoverTrack = FALSE;            /* TrackMouseEvent 已挂 */

/* 两条滚动条的轨道矩形（客户区坐标；右下角交叉处各自让位）。 */
static void ViewBarRects(RECT* v, RECT* h) {
    RECT rc;
    if (!g_view || !GetClientRect(g_view, &rc)) return;
    if (v) { v->left = rc.right - g_barW;  v->top = (int)g_headerH;
             v->right = rc.right;          v->bottom = rc.bottom - g_barH; }
    if (h) { h->left = 0;                  h->top = rc.bottom - g_barH;
             h->right = rc.right - g_barW; h->bottom = rc.bottom; }
}

/* 拇指几何（含 2px 内边距）。max=0 时不该被调用。 */
static void ViewThumbRectV(RECT* out) {
    RECT tr;
    ViewBarRects(&tr, NULL);
    int trackH = tr.bottom - tr.top - 4;
    int page = g_vPage > 0 ? g_vPage : 1;
    int th = (int)((float)trackH * page / (page + g_vMax) + 0.5f);
    if (th < (int)(18 * g_k)) th = (int)(18 * g_k);
    if (th > trackH) th = trackH;
    int y = tr.top + 2;
    if (g_vMax > 0) y += (int)((float)(trackH - th) * g_vPos / g_vMax + 0.5f);
    out->left = tr.left + 2;  out->right = tr.right - 2;
    out->top = y;             out->bottom = y + th;
}

static void ViewThumbRectH(RECT* out) {
    RECT tr;
    ViewBarRects(NULL, &tr);
    int trackW = tr.right - tr.left - 4;
    int page = g_hPage > 0 ? g_hPage : 1;
    int tw = (int)((float)trackW * page / (page + g_hMax) + 0.5f);
    if (tw < (int)(18 * g_k)) tw = (int)(18 * g_k);
    if (tw > trackW) tw = trackW;
    int x = tr.left + 2;
    if (g_hMax > 0) x += (int)((float)(trackW - tw) * g_hPos / g_hMax + 0.5f);
    out->top = tr.top + 2;  out->bottom = tr.bottom - 2;
    out->left = x;          out->right = x + tw;
}

/* 圆角矩形填充（拇指用；GDI+ 没有现成的圆角填充）。 */
static void FillRounded(Gdiplus::Graphics* g, float x, float y, float w, float h,
                        float rad, Gdiplus::Brush* br) {
    Gdiplus::GraphicsPath p;
    float d = rad * 2;
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
    g->FillPath(br, &p);
}

/* 画两条滚动条（整帧与条带帧都要调：滚动时拇指也在动）。
   样式：无轨道、无箭头 —— 只有半透明圆角细拇指，悬停/拖动加深；
   图不超窗（max=0）时完全不画（旧版留一条空边条，看着像给内容加了个框）。 */
static void ViewDrawBars(Gdiplus::Graphics* g) {
    BYTE a = g_vDrag || g_hDrag ? 210 : (g_vHover || g_hHover) ? 160 : 120;
    BYTE v = g_vDrag || g_hDrag ? 0x60 : (g_vHover || g_hHover) ? 0x78 : 0x8C;
    Gdiplus::SolidBrush br(Gdiplus::Color(a, v, v, v));
    float rad = 3.0f * g_k;
    if (g_vMax > 0) {
        RECT th; ViewThumbRectV(&th);
        FillRounded(g, (float)th.left, (float)th.top,
                    (float)(th.right - th.left), (float)(th.bottom - th.top),
                    rad, &br);
    }
    if (g_hMax > 0) {
        RECT th; ViewThumbRectH(&th);
        FillRounded(g, (float)th.left, (float)th.top,
                    (float)(th.right - th.left), (float)(th.bottom - th.top),
                    rad, &br);
    }
}

static int ViewScrollPos(int bar) {
    return (bar == SB_VERT) ? g_vPos : g_hPos;
}

/* ---- 放大镜光标（Ditto 同款）----
   系统没有现成的放大镜光标，运行时自己画两枚 32x32 ARGB 光标：
   白底黑圈镜片 + 手柄，镜心 + / -；热点在镜心。点击的语义 = 光标图案：
   适应窗口时镜片是 +（点击放大到 100%），100% 时是 -（点击缩到适配）。 */
static HCURSOR g_curZoomIn = NULL, g_curZoomOut = NULL;

static HCURSOR MakeZoomCursor(BOOL plus) {
    const int S = 32;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = S;
    bi.bmiHeader.biHeight      = -S;             /* 自上而下 */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    BYTE* bits = NULL;
    HBITMAP color = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void**)&bits, NULL, 0);
    if (!color || !bits) return NULL;
    memset(bits, 0, (size_t)S * S * 4);

    {
        Gdiplus::Bitmap bmp(S, S, S * 4, PixelFormat32bppARGB, bits);
        Gdiplus::Graphics g(&bmp);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Pen penBlack(Gdiplus::Color(255, 20, 20, 20), 2.0f);
        Gdiplus::SolidBrush brGlass(Gdiplus::Color(190, 255, 255, 255));
        /* 镜片：白玻璃 + 黑圈 */
        g.FillEllipse(&brGlass, 3.0f, 3.0f, 20.0f, 20.0f);
        g.DrawEllipse(&penBlack, 3.0f, 3.0f, 20.0f, 20.0f);
        /* 手柄：圆头短把 */
        Gdiplus::Pen penHandle(Gdiplus::Color(255, 20, 20, 20), 4.0f);
        penHandle.SetStartCap(Gdiplus::LineCapRound);
        penHandle.SetEndCap(Gdiplus::LineCapRound);
        g.DrawLine(&penHandle, 19.5f, 19.5f, 27.5f, 27.5f);
        /* 镜心 + / - */
        Gdiplus::Pen penSign(Gdiplus::Color(255, 20, 20, 20), 2.5f);
        penSign.SetStartCap(Gdiplus::LineCapRound);
        penSign.SetEndCap(Gdiplus::LineCapRound);
        if (plus) g.DrawLine(&penSign, 13.0f, 8.5f, 13.0f, 17.5f);
        g.DrawLine(&penSign, 8.5f, 13.0f, 17.5f, 13.0f);
    }

    HBITMAP mask = CreateBitmap(S, S, 1, 1, NULL);   /* 全 0 掩码 → 按 alpha 合成 */
    ICONINFO ii;
    memset(&ii, 0, sizeof(ii));
    ii.fIcon = FALSE;                                /* 光标 */
    ii.xHotspot = 13;  ii.yHotspot = 13;             /* 镜心 */
    ii.hbmMask = mask;  ii.hbmColor = color;
    HCURSOR c = (HCURSOR)CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return c;
}

static float ViewEffScale(void) {
    if (g_viewScale > 0.0f) return g_viewScale;
    if (g_viewScale < 0.0f) {
        /* 「缩放图像以适应窗口」的适配比例，Ditto 同式（ImageViewer.cpp
           UpdateBitmapSize）：**可视宽 / 图宽** —— 它把滚动条尺寸加回客户区
           再除图宽，语义是"按可视宽适配"；高度交给滚动条。不设放大上限。 */
        UINT iw = g_viewImg ? g_viewImg->GetWidth() : 0;
        if (!iw || !g_viewImg) return 1.0f;
        RECT rc;
        float cw = 1.0f;
        if (g_view && GetClientRect(g_view, &rc))
            cw = (float)(rc.right - g_barW) - 2.0f;   /* 去掉滚动条占的那条 */
        if (cw < 1.0f) cw = 1.0f;
        float fit = cw / (float)iw;
        if (fit < 0.01f) fit = 0.01f;
        return fit;
    }
    return 1.0f;   /* 默认：100% 原大 */
}

/* 内容 DIB：窗口尺寸变了就重建（照 TipBuild 的做法）。重建后旧像素全部作废，
   下一帧必须整画。 */
static BOOL ViewSurface(void) {
    if (g_viewDC && g_viewW == g_viewSurfW && g_viewH == g_viewSurfH) return TRUE;
    if (g_viewDC)  { DeleteDC(g_viewDC);      g_viewDC  = NULL; }
    if (g_viewBmp) { DeleteObject(g_viewBmp); g_viewBmp = NULL; }
    g_viewBits = NULL;
    HDC screen = GetDC(NULL);
    if (!screen) return FALSE;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = g_viewW;
    bi.bmiHeader.biHeight      = -g_viewH;       /* 自上而下 */
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_viewBmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &g_viewBits, NULL, 0);
    ReleaseDC(NULL, screen);
    if (!g_viewBmp || !g_viewBits) return FALSE;
    g_viewDC = CreateCompatibleDC(NULL);
    if (!g_viewDC) return FALSE;
    SelectObject(g_viewDC, g_viewBmp);
    g_viewSurfW = g_viewW;  g_viewSurfH = g_viewH;
    g_viewFullPaint = TRUE;              /* 新表面内容是垃圾：下一帧整画 */
    return TRUE;
}

static void ViewBtnRect(int i, Gdiplus::RectF* out) {
    /* 贴**客户区**右缘：常驻竖滚动条占窗口右缘一整列（标题栏右侧也在内），
       按窗口宽定位 ✕ 会被滚动条压在底下（实测）。 */
    RECT rc;
    float cw = (float)g_viewW;
    if (g_view && GetClientRect(g_view, &rc) && rc.right > 0) cw = (float)rc.right;
    float s = 26.0f * g_k;
    out->Y = (g_headerH - s) * 0.5f;
    out->Height = s;
    (void)i;                                 /* 只剩 ✕ 一颗：贴最右 */
    out->Width = s;
    out->X = cw - s - 6.0f * g_k;
}

static int ViewHitBtn(float x, float y) {
    for (int i = 0; i < VIEW_BTN_COUNT; i++) {
        Gdiplus::RectF r;
        ViewBtnRect(i, &r);
        if (x >= r.X && x <= r.X + r.Width && y >= r.Y && y <= r.Y + r.Height) return i;
    }
    return -1;
}

/* 主线程收割：把解码线程放进槽的图接进 g_viewImg，旧图就地释放。
   ★ 旧图只能在**这里**释放：主线程所有用图的路径（ViewPaint / 滚轮 / 拖动 /
   标题百分比）都在同一线程的消息处理里，与这里串行 —— 不存在"画着一半被
   线程释放"的竞态（解码线程从不碰 g_viewImg）。槽图不是当前请求的（翻条目
   后才解完）就不接，留给线程下次入槽时清理。 */
static void ViewHarvest(void) {
    Gdiplus::Bitmap* nb = NULL;
    long long nid = 0;
    if (ThumbViewTake(g_viewId, &nb, &nid)) {
        delete g_viewImg;                 /* 旧图就地释放 */
        g_viewImg = nb;
        g_viewImgId = nid;
    }
}

/* ---- 滚动条与帧合并 ----
   平移状态就是 g_vPos/g_hPos 那一组变量（自绘滚动条没有 SetScrollInfo 可
   存）：图不超窗（max=0）时没有边条也没有拇指；超出后滚动接管平移。 */


/* 按"待画一帧"合并高频重绘来源（mousemove / 滚轮 / 滚动条拖动）。本进程
   主线程挂着低级鼠标钩子：每事件全量重画会把主线程打满，钩子回调排队，
   用户全系统的光标都会卡 —— 帧率上限 ~66fps，事件合并只留最新状态。 */
static void ViewPaintSoon(void) {
    if (!g_view || g_viewPaintPending) return;
    g_viewPaintPending = TRUE;
    SetTimer(g_view, 1, VIEW_FRAME_MS, NULL);
}

/* 滚动专属快路径：把已画好的像素整体平移（ScrollDC），只补画露出的那一条。
   整帧重画的代价全花在"本来就对"的像素上，而本线程宿着低级鼠标钩子 ——
   画得越久，全系统的输入排队越久。返回 FALSE = 滚不出来（太怪/失败），
   调用方退回整帧重画。 */
static BOOL ScrollPaintStrip(int dx, int dy) {
    if (!g_viewDC || !g_viewBits) return FALSE;
    RECT rc;
    if (!GetClientRect(g_view, &rc)) return FALSE;
    /* ★ 位移用 memmove 自己搬，不用 ScrollDC：内存 DC 上 ScrollDC 的行为
       不可靠 —— lprcClip=NULL 时"可见区域"不是客户区，位移整个不发生，
       lprcUpdate 却照常报告（实测：只有补画的条带是新内容，上面全是旧帧，
       表现就是"滚动图片破碎"）。DIB 是自上而下的 32bpp：垂直位移 = 整块行
       memmove，水平位移 = 逐行行内 memmove，结果确定。
       ★ 斜向（横竖同时）罕见，退回整帧。 */
    if (dx != 0 && dy != 0) return FALSE;
    /* ★ 搬动区域**不含窗框**（内容边框行/列与滚动条边条都排除在外）：
       边框属于"窗框"，跟着内容搬就是一条随滚动游走、永远洗不掉的细线
       （实测踩过：每滚一档，一条 #CCCCCC 细线在图里向上游 120px）。
       cw2/bot2 = 内容区右缘/下缘（右侧与下侧是自绘滚动条边条）。 */
    int top = (int)g_headerH;
    int cw2 = rc.right - g_barW;
    int bot2 = rc.bottom - g_barH - 1;
    int rowsN = bot2 - top;
    BYTE* base = (BYTE*)g_viewBits;
    int stride = g_viewW * 4;
    RECT up;
    if (dy != 0) {
        if (dy >= rowsN || -dy >= rowsN) return FALSE;          /* 搬不动/搬满：整帧 */
        /* ★ 竖向也**逐行**搬，且每行只搬内容区前 cw2 像素（整行大 memmove
           会把右侧滚动条边条的旧拇指/旧像素一起搬着游走 = "拖影"）。边条
           列留在原地，由下面的边条重刷统一洗净。 */
        if (dy > 0) {
            /* 往下滚：内容上移 dy 行，底部露出 dy 行（内容底边框行原地不动） */
            for (int y = top; y < bot2 - dy; y++)
                memmove(base + (size_t)y * stride,
                        base + (size_t)(y + dy) * stride,
                        (size_t)cw2 * 4);
            up.left = 0;  up.right = cw2;  up.top = bot2 - dy;  up.bottom = bot2;
        } else {
            int n = -dy;   /* 往上滚：内容下移 n 行，顶部露出 n 行（倒序搬，
                              先写低行会覆掉还没读的高行） */
            for (int y = bot2 - 1; y >= top + n; y--)
                memmove(base + (size_t)y * stride,
                        base + (size_t)(y - n) * stride,
                        (size_t)cw2 * 4);
            up.left = 0;  up.right = cw2;  up.top = top;  up.bottom = top + n;
        }
    } else {
        /* ---- 横向：逐行行内 memmove ----
           内容右缘边框列同样不搬（dx>0 时会被搬走、dx<0 时左列会被搬走），
           搬动区域把那一列排除，露出的条带里再补画边框。 */
        if (dx > 0) {
            if (dx >= cw2 - 1) return FALSE;
            for (int y = top; y < bot2; y++) {
                BYTE* row = base + (size_t)y * stride;
                memmove(row, row + (size_t)dx * 4, (size_t)(cw2 - 1 - dx) * 4);
            }
            up.left = cw2 - 1 - dx;  up.right = cw2;  up.top = top;  up.bottom = bot2;
        } else {
            int n = -dx;
            if (n >= cw2 - 1) return FALSE;
            for (int y = top; y < bot2; y++) {
                BYTE* row = base + (size_t)y * stride;
                memmove(row + (size_t)n * 4, row, (size_t)(cw2 - 1 - n) * 4);
            }
            up.left = 0;  up.right = n + 1;  up.top = top;  up.bottom = bot2;
        }
    }

    Gdiplus::Bitmap bmp(g_viewW, g_viewH, g_viewW * 4, PixelFormat32bppPARGB,
                        (BYTE*)g_viewBits);
    if (bmp.GetLastStatus() != Gdiplus::Ok) return FALSE;
    Gdiplus::Graphics g(&bmp);
    Gdiplus::SolidBrush brGut(Gdiplus::Color(C_THUMB_BG));
    Gdiplus::SolidBrush brBorder(Gdiplus::Color(C_BORDER));
    g.FillRectangle(&brGut, Gdiplus::RectF((float)up.left, (float)up.top,
                                           (float)(up.right - up.left),
                                           (float)(up.bottom - up.top)));
    UINT iw = 0, ih = 0;
    if (g_viewImg) { iw = g_viewImg->GetWidth(); ih = g_viewImg->GetHeight(); }
    if (g_viewImg && iw > 0 && ih > 0) {
        float eff = ViewEffScale();
        float dw = (float)iw * eff, dh = (float)ih * eff;
        /* 图像在窗口里的落点（与整帧同一公式）：超窗方向被滚动位推走 */
        float ix0 = -(float)ViewScrollPos(SB_HORZ);
        float iy0 = g_headerH - (float)ViewScrollPos(SB_VERT);
        float ix1 = ix0 + dw, iy1 = iy0 + dh;
        float vx0 = (ix0 > 0.0f) ? ix0 : 0.0f;
        float vy0 = (iy0 > g_headerH) ? iy0 : g_headerH;
        float vx1 = (ix1 < (float)cw2) ? ix1 : (float)cw2;
        float vy1 = (iy1 < bot2) ? iy1 : (float)bot2;
        /* 只补条带：与露出的矩形求交。上下各多盖 1 行：盖住任何半像素
           边缘残留（多盖的是相邻的正确内容或条带自身，无副作用）。 */
        float ovTop = up.top > top ? up.top - 1.0f : (float)up.top;
        float ovBot = up.bottom < bot2 ? up.bottom + 1.0f : (float)up.bottom;
        if (vx0 < (float)up.left)   vx0 = (float)up.left;
        if (vy0 < ovTop)            vy0 = ovTop;
        if (vx1 > (float)up.right)  vx1 = (float)up.right;
        if (vy1 > ovBot)            vy1 = ovBot;
        if (vx0 < vx1 && vy0 < vy1) {
            float sx0 = (vx0 - ix0) / eff, sy0 = (vy0 - iy0) / eff;
            Gdiplus::RectF dst(vx0, vy0, vx1 - vx0, vy1 - vy0);
            g.SetInterpolationMode(Gdiplus::InterpolationModeBilinear);
            g.DrawImage(g_viewImg, dst, sx0, sy0,
                        (vx1 - vx0) / eff, (vy1 - vy0) / eff, Gdiplus::UnitPixel);
        }
    }
    /* 条带盖掉的内容边框补回来（左列 / 内容右缘列；底边框行不搬不用补）。
       与整帧同一守卫：无边条时不补（整帧也没画，补了就是条带帧独有的线）。 */
    if (g_barW > 0 || g_barH > 0) {
        if (up.left == 0)
            g.FillRectangle(&brBorder, Gdiplus::RectF(0.0f, (float)up.top, 1.0f,
                                                      (float)(up.bottom - up.top)));
        if (up.right == cw2)
            g.FillRectangle(&brBorder, Gdiplus::RectF((float)(cw2 - 1), (float)up.top,
                                                      1.0f, (float)(up.bottom - up.top)));
    }
    /* ★ 两条边条整条洗净再画：条带搬移与补画都不碰边条（列/行在搬动区域
       外），上一帧的拇指像素会一直留在上面 —— 不洗就是一串不动的"拖影"，
       横条区域也像冻住不跟滚（实测）。洗成净底后当前拇指是唯一在动的。 */
    if (g_barW > 0)
        g.FillRectangle(&brGut, Gdiplus::RectF((float)cw2, (float)top,
                                               (float)(rc.right - cw2),
                                               (float)(rc.bottom - top)));
    if (g_barH > 0)
        g.FillRectangle(&brGut, Gdiplus::RectF(0.0f, (float)(rc.bottom - g_barH),
                                               (float)cw2, (float)g_barH));
    ViewDrawBars(&g);
    g.Flush(Gdiplus::FlushIntentionSync);
    ThumbUpdateVisRange();           /* 滚动改了可见范围：解码线程按需补图 */
    /* ★ 呈现必须**整帧**：memmove 之后内存 DIB 已经全对，但屏幕（DWM 表面）
       只有被 BitBlt 过的区域才更新 —— 只无效化条带的话，位移过的上面一大片
       永远停在旧帧（实测就是"滚动图片破碎"：底部一条新的、上面全是旧的）。
       贵的部分（GDI+ 填充/采样）已经只做了条带，整帧只是一次 BitBlt 搬运。 */
    InvalidateRect(g_view, NULL, FALSE);
    return TRUE;
}

/* 同步滚动条范围到当前缩放。anchor：CENTER = 滚到内容中心（缩放锚 = 窗口
   中心，视图向四周均匀展开）；TOP = 回到顶/左（新内容从头看，Ditto 默认
   态）；KEEP = 现位 clamp 进新范围（窗口尺寸变化时用）。 */
static void ViewSyncScroll(int anchor) {
    if (!g_view || !g_viewImg) return;
    g_animX = FALSE;  g_animY = FALSE;   /* 范围要变（缩放/换图/适应）：动画目标作废，别跟新范围打架 */
    UINT iw = g_viewImg->GetWidth(), ih = g_viewImg->GetHeight();
    RECT rc;
    if (!iw || !ih || !GetClientRect(g_view, &rc)) return;
    float eff = ViewEffScale();
    /* 视口 = 客户区**去掉滚动条边条**（边条按需预留，见 ViewFitOnce）。
       不减的话可滚范围比真实溢出多出一条边条宽，能滚过图片尽头 —— 尽头
       外露的就是灰底。 */
    int cw = rc.right - g_barW, ch = rc.bottom - (int)g_headerH - g_barH;
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    int hmax = ((float)iw * eff > (float)cw) ? (int)((float)iw * eff - cw + 0.5f) : 0;
    int vmax = ((float)ih * eff > (float)ch) ? (int)((float)ih * eff - ch + 0.5f) : 0;
    SCROLLINFO si;
    memset(&si, 0, sizeof(si));
    si.cbSize = sizeof(si);
    si.fMask = SIF_RANGE | SIF_POS | SIF_PAGE;
    for (int pass = 0; pass < 2; pass++) {
        int bar = pass ? SB_VERT : SB_HORZ;
        int max = pass ? vmax : hmax;
        int page = pass ? ch : cw;
        int pos = (anchor == VIEW_ANCHOR_CENTER) ? (max / 2)
                : (anchor == VIEW_ANCHOR_TOP)    ? 0
                : min(ViewScrollPos(bar), max);
        if (bar == SB_VERT) { g_vMax = max; g_vPage = page; g_vPos = pos; }
        else                { g_hMax = max; g_hPage = page; g_hPos = pos; }
    }
}

/* 滚轮：steps 档（正 = 向下 / 右）。不直接挪滚动位，而是把**目标位**挪过去、
   让帧合并时钟逐帧向它趋近 —— 一档一档瞬跳正是"滚动条跳动"的观感来源，
   连续滑动后滚动条与图像是一起匀滑移动的。 */
static void ViewScrollBy(int bar, int steps) {
    if (!steps || !g_view) return;
    int posmax = (bar == SB_VERT) ? g_vMax : g_hMax;
    if (posmax <= 0) return;                       /* 内容没超窗：没得滚 */
    int *target = (bar == SB_VERT) ? &g_viewTargetY : &g_viewTargetX;
    BOOL *anim  = (bar == SB_VERT) ? &g_animY : &g_animX;
    if (!*anim)   /* 该轴动画没在跑：目标从当前位置起算（上次的目标已过时） */
        *target = ViewScrollPos(bar);
    *target += steps * VIEW_SCROLL_STEP;
    if (*target < 0)      *target = 0;
    if (*target > posmax) *target = posmax;
    *anim = TRUE;
    ViewPaintSoon();
}

/* 帧合并时钟里走一步：向目标位趋近 40%（指数缓动，差 2px 内算到位）。
   没到位就自己再约一帧 —— WM_TIMER 只在队列空时派发，连滚时也不会饿着。 */
static void ViewAnimStep(void) {
    if (!g_view || (!g_animX && !g_animY)) return;
    int px = ViewScrollPos(SB_HORZ), py = ViewScrollPos(SB_VERT);
    int nx = px, ny = py;
    if (g_animX) {
        nx = px + (g_viewTargetX - px) * 2 / 5;
        if (g_viewTargetX - nx < 2 && nx - g_viewTargetX < 2) nx = g_viewTargetX;
        if (nx == px) g_animX = FALSE;
    }
    if (g_animY) {
        ny = py + (g_viewTargetY - py) * 2 / 5;
        if (g_viewTargetY - ny < 2 && ny - g_viewTargetY < 2) ny = g_viewTargetY;
        if (ny == py) g_animY = FALSE;
    }
    if (nx == px && ny == py) return;
    g_hPos = nx;  g_vPos = ny;
    if (g_animX || g_animY)
        ViewPaintSoon();               /* 还没到：下一帧继续 */
}

static void ViewPaint(void) {
    if (!g_view) return;
    if (!ViewSurface()) return;
    ViewHarvest();
    RECT rc;
    if (!GetClientRect(g_view, &rc)) return;

    int sx = ViewScrollPos(SB_HORZ), sy = ViewScrollPos(SB_VERT);
    int dx = sx - g_viewLastSX, dy = sy - g_viewLastSY;

    /* 纯滚动（内容没变）：条带快路径。位置都没动就更没什么可画的。 */
    if (!g_viewFullPaint && dx == 0 && dy == 0) return;
    if (!g_viewFullPaint && (dx || dy)) {
        if (ScrollPaintStrip(dx, dy)) {
            g_viewLastSX = sx;  g_viewLastSY = sy;
            return;
        }
    }

    Gdiplus::Bitmap bmp(g_viewW, g_viewH, g_viewW * 4, PixelFormat32bppPARGB,
                        (BYTE*)g_viewBits);
    if (bmp.GetLastStatus() != Gdiplus::Ok) return;
    Gdiplus::Graphics g(&bmp);
    /* 不再整面 Clear：标题栏与图片区两个填充正好盖满客户区（ClearType 的
       不透明底由这两个填充给足），整面过一遍是纯浪费。 */

    Gdiplus::SolidBrush brHead(Gdiplus::Color(C_HEAD));
    Gdiplus::SolidBrush brGut(Gdiplus::Color(C_THUMB_BG));
    Gdiplus::SolidBrush brBorder(Gdiplus::Color(C_BORDER));

    const float cw = (float)rc.right, chh = (float)rc.bottom - g_headerH;

    /* 标题栏：与列表同一画法（纯白平涂 + 底边线），文字在下面 GDI 那一遍画。
       布局一律按**客户区**走：子滚动条盖住右缘/下缘一条，其余部分全归绘制
       （被盖住的边条底下画什么都看不见）。
       ★ 分隔线与边框一律用**整数行的 FillRectangle**，不用 DrawLine /
         DrawRectangle 的 0.5 对齐画法：那些会触发 GDI+ 抗锯齿，在边界行上
         混出半透明像素 —— 预览的像素要被 memmove 整行搬动（条带重绘），
         半透明行跟着搬就是滚不掉的细线。 */
    g.FillRectangle(&brHead, 0, 0, (int)cw, (int)g_headerH);
    g.FillRectangle(&brBorder, Gdiplus::RectF(0.0f, g_headerH - 1.0f, cw, 1.0f));

    /* 图片区：暗灰底 + 图片（Bilinear：缩放跟手优先，预览不追求打印质量） */
    g.FillRectangle(&brGut, Gdiplus::RectF(0.0f, g_headerH, cw, chh));
    UINT iw = 0, ih = 0;
    if (g_viewImg) { iw = g_viewImg->GetWidth(); ih = g_viewImg->GetHeight(); }
    if (g_viewImg && iw > 0 && ih > 0) {
        float eff = ViewEffScale();
        float dw = (float)iw * eff, dh = (float)ih * eff;
        /* 顶/左锚定（Ditto：100% 显示在顶部）：超窗方向被滚动条推着走，
           不超窗时滚动条恒 0 —— 一条公式覆盖两种情况，没有居中分支。
           ★ 这里刻意不叫 dx/dy/sx/sy：函数开头那四个 int 是**滚动位移**，
             同名会被 C4456 判为遮蔽，读的人也会以为是同一个量。 */
        float imgX = -(float)ViewScrollPos(SB_HORZ);
        float imgY = g_headerH - (float)ViewScrollPos(SB_VERT);
        /* 只采样**可见**部分：反算内容区与图面交集对应的源矩形，每帧的
           采样量只跟窗口大小走，与图像尺寸和缩放比无关（4 倍放大时是
           全图采样的 1/16）。交集反算天然保证源矩形在图内。
           ★ 可见范围裁到**内容区**为止（右/下让出滚动条边条）：图溢进
             边条的话，条带路径不搬不补那两块，溢进去的像素会冻在旧滚动
             位 —— 看起来就是"边条里的图不跟着滚"（实测）。 */
        float cwv = cw - (float)g_barW, rcv = (float)rc.bottom - (float)g_barH;
        float vx0 = (imgX > 0.0f) ? imgX : 0.0f;
        float vy0 = (imgY > g_headerH) ? imgY : g_headerH;
        float vx1 = (imgX + dw < cwv) ? (imgX + dw) : cwv;
        float vy1 = (imgY + dh < rcv) ? (imgY + dh) : rcv;
        if (vx0 < vx1 && vy0 < vy1) {
            float srcX = (vx0 - imgX) / eff, srcY = (vy0 - imgY) / eff;
            float sw = (vx1 - vx0) / eff, sh = (vy1 - vy0) / eff;
            Gdiplus::RectF dst(vx0, vy0, vx1 - vx0, vy1 - vy0);
            g.SetInterpolationMode(Gdiplus::InterpolationModeBilinear);
            g.DrawImage(g_viewImg, dst, srcX, srcY, sw, sh, Gdiplus::UnitPixel);
        }
    }

    /* 边框最后画：压住可能溢出的图片。右/下边框画在**内容区**内缘
       （滚动条边条画在它们外侧的自绘条上）。四条边都是整数 1px 填充。
       ★ 图装得下（无边条）时一条都不画：图贴着窗缘本身就是干净的，
        外框反而是"给内容加框"（实测嫌丑）。
       ★ 左右两条**从标题栏下缘起**：标题栏（✕ 那行）两侧不竖线 —— 线
        贴着 ✕ 很难看（实测），左右框只为圈住图片区。 */
    if (g_barW > 0 || g_barH > 0) {
        g.FillRectangle(&brBorder, Gdiplus::RectF(0.0f, 0.0f, cw, 1.0f));
        g.FillRectangle(&brBorder, Gdiplus::RectF(0.0f, (float)(rc.bottom - g_barH - 1), cw - (float)g_barW, 1.0f));
        g.FillRectangle(&brBorder, Gdiplus::RectF(0.0f, (float)g_headerH, 1.0f,
                                                  (float)(rc.bottom - g_barH) - (float)g_headerH));
        g.FillRectangle(&brBorder, Gdiplus::RectF(cw - (float)g_barW - 1.0f, (float)g_headerH, 1.0f,
                                                  (float)(rc.bottom - g_barH) - (float)g_headerH));
    }
    ViewDrawBars(&g);                /* 自绘滚动条（thumb 跟随滚动位） */
    g.Flush(Gdiplus::FlushIntentionSync);

    /* ---- GDI 文字（ClearType）---- */
    WCHAR title[96];
    if (g_viewImg && iw > 0 && ih > 0) {
        int pct = (int)(ViewEffScale() * 100.0f + 0.5f);
        _snwprintf_s(title, 96, _TRUNCATE, L"图片 %ux%u %d%%", iw, ih, pct);
    } else if (g_viewId && ThumbViewDoneFor(g_viewId)) {
        wcscpy_s(title, 96, L"解码失败");   /* 线程做完了还没有图 = 库里解不出来 */
    } else {
        wcscpy_s(title, 96, L"解码中…");
    }
    GdiText(g_viewDC, title, g_fontRow, CR(C_TITLE),
            12.0f * g_k, 0.0f, cw - 160.0f * g_k, g_headerH,
            DT_LEFT, TRUE);
    Gdiplus::RectF r;
    ViewBtnRect(0, &r);
    GdiText(g_viewDC, L"✕", g_fontRow, CR(0xFF6E6E6E),
            r.X, r.Y, r.Width, r.Height, DT_CENTER, FALSE);

    g_viewLastSX = sx;  g_viewLastSY = sy;
    g_viewFullPaint = FALSE;
    InvalidateRect(g_view, NULL, FALSE);
}

void ViewInitMessages(void) {         /* 必须在首条 F3 消息到来前注册（wWinMain 调） */
    if (!g_msgView) g_msgView = RegisterWindowMessageW(CLIP_MSG_VIEW);
    if (!g_msgViewState) g_msgViewState = RegisterWindowMessageW(CLIP_MSG_VIEWSTATE);
}

BOOL ViewIsOpen(void) { return g_viewOpen; }
HWND ViewWnd(void)   { return g_view; }
BOOL ViewIsViewMsg(UINT msg) { return g_msgView && msg == g_msgView; }

/* 状态报信：主程序在主线程上记一份镜像（s_viewOpen），钩子据此决定
   "无关键要不要叫预览退场"。幂等：重复发只是再确认。 */
static void ViewNotifyState(void) {
    if (!g_msgViewState) return;
    HWND mw = FindWindowW(CLIP_MAIN_CLASS, NULL);
    if (mw) PostMessageW(mw, g_msgViewState, g_viewOpen ? 1 : 0, 0);
}

void ViewHide(void) {
    if (!g_viewOpen && g_viewRow < 0) return;
    g_viewRow = -1;
    g_viewId = 0;
    g_viewWinDrag = FALSE;
    if (g_viewPaintPending) {
        g_viewPaintPending = FALSE;
        if (g_view) KillTimer(g_view, 1);
    }
    /* 解码请求与槽里的图**都留着**：解了一半的让它解完（图进槽等收割），
       解好的留在槽里。为的是"F3 关了再开同一条目"立即出图 —— 收起的只是
       窗口，图已经是现成的。清掉它们（旧做法）省不下什么：在途的那张本来
       就拦不回来；代价却是重开同一张图要再付一次解码，而更糟的是撞上下面的
       线程短路与"解码中"卡死（见 ViewToggle）。最多驻留一张解码图，收场在
       ViewDestroy。 */
    g_viewLastSX = 0;  g_viewLastSY = 0;
    g_viewFullPaint = TRUE;
    g_animX = FALSE;  g_animY = FALSE;
    if (g_view && IsWindowVisible(g_view)) ShowWindow(g_view, SW_HIDE);
    g_viewOpen = FALSE;
    ViewNotifyState();
}

/* 选中项变了：预览可见时跟着换内容 —— 窗口位置不动，尺寸随新内容重摆
   （"根据内容调整窗口大小"的语义延伸到跟随）。缩放回 100% 默认态，解码
   完成后 ViewFitOnce 重摆（顶/左从头看）。新条目不是图片 → 收。 */
void ViewFollowSel(void) {
    if (!g_viewOpen) return;
    if (g_sel < 0 || g_sel >= g_count || !g_items[g_sel].hasImage) { ViewHide(); return; }
    if (g_viewRow == g_sel) return;
    g_viewRow = g_sel;
    g_viewId = g_items[g_sel].id;
    g_viewScale = 0.0f;
    g_viewFitted = FALSE;
    g_viewFullPaint = TRUE;      /* 换条目：内容与标题全变 */
    ViewHarvest();
    if (g_viewImg && g_viewImgId == g_viewId) {
        ViewFitOnce();           /* 这条的图已经解好（刚才看过又翻回来）：直接摆窗 */
        ViewPaint();
        return;
    }
    ThumbRequestViewDecode(g_viewId);
    ViewPaint();                 /* 立即切到"解码中"，图好了 WM_VIEW_READY 再画 */
}

/* 条目集重载（DbLoadItems）后校验预览：g_viewRow 是 g_items 的下标，重载后
   索引意义全变（Ditto 会重排/插入），沿用就是指着错的条目复制错的内容。
   按 id 在新表里找回，找不到了（被清掉）就收。 */
void ViewRevalidate(void) {
    if (!g_viewOpen) return;
    for (int i = 0; i < g_count; i++)
        if (g_items[i].id == g_viewId) { g_viewRow = i; return; }
    ViewHide();
}

static LRESULT CALLBACK ViewWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (g_viewDC && dc)
            BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top,
                   ps.rcPaint.right - ps.rcPaint.left,
                   ps.rcPaint.bottom - ps.rcPaint.top,
                   g_viewDC, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;                       /* 内容全来自双缓冲，别让系统擦出白闪 */
    case WM_TIMER:
        if (wp == 1) {                  /* 帧合并：一个周期只画最新状态 */
            KillTimer(hwnd, 1);
            g_viewPaintPending = FALSE;
            ViewAnimStep();             /* 滚动动画先走一步（没到位它会再约帧） */
            ViewPaint();
        }
        return 0;
    case WM_MOUSEMOVE: {
        /* ---- 拖标题栏挪窗（与列表同套路：NOACTIVATE 收不到 WM_NCLBUTTONDOWN） ---- */
        if (g_viewWinDrag && (wp & MK_LBUTTON)) {
            POINT cur;
            if (GetCursorPos(&cur)) {
                /* 高频重复事件（光标没动系统也派发）提前走：MoveWindow 不便宜 */
                if (cur.x == g_viewDragCur.x && cur.y == g_viewDragCur.y) return 0;
                RECT work;
                MonitorWork(MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST), &work);
                g_viewX = g_viewWinPos.x + cur.x - g_viewDragCur.x;
                g_viewY = g_viewWinPos.y + cur.y - g_viewDragCur.y;
                if (g_viewX < work.left) g_viewX = work.left;
                if (g_viewY < work.top)  g_viewY = work.top;
                if (g_viewX + g_viewW > work.right)  g_viewX = work.right - g_viewW;
                if (g_viewY + g_viewH > work.bottom) g_viewY = work.bottom - g_viewH;
                MoveWindow(hwnd, g_viewX, g_viewY, g_viewW, g_viewH, FALSE);
            }
            return 0;
        }
        /* ---- 自绘滚动条的拖拽与悬停 ---- */
        RECT v, h;
        ViewBarRects(&v, &h);
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        if (g_vDrag) {
            int trackH = v.bottom - v.top - 4;
            RECT th; ViewThumbRectV(&th);
            int thumbH = th.bottom - th.top;
            int y = pt.y - g_vDragGrab;
            if (y < v.top + 2) y = v.top + 2;
            if (y + thumbH > v.bottom - 2) y = v.bottom - 2 - thumbH;
            int npos = (trackH > thumbH)
                     ? (int)((float)(y - v.top - 2) * g_vMax / (trackH - thumbH) + 0.5f) : 0;
            if (npos < 0) npos = 0;
            if (npos > g_vMax) npos = g_vMax;
            if (npos != g_vPos) { g_vPos = npos; g_animY = FALSE; ViewPaintSoon(); }
            return 0;
        }
        if (g_hDrag) {
            int trackW = h.right - h.left - 4;
            RECT th; ViewThumbRectH(&th);
            int thumbW = th.right - th.left;
            int x = pt.x - g_hDragGrab;
            if (x < h.left + 2) x = h.left + 2;
            if (x + thumbW > h.right - 2) x = h.right - 2 - thumbW;
            int npos = (trackW > thumbW)
                     ? (int)((float)(x - h.left - 2) * g_hMax / (trackW - thumbW) + 0.5f) : 0;
            if (npos < 0) npos = 0;
            if (npos > g_hMax) npos = g_hMax;
            if (npos != g_hPos) { g_hPos = npos; g_animX = FALSE; ViewPaintSoon(); }
            return 0;
        }
        {
            RECT th;
            ViewThumbRectV(&th);
            RECT thh;
            ViewThumbRectH(&thh);
            int nhv = (g_vMax > 0 && PtInRect(&v, pt) &&
                       pt.y >= th.top && pt.y <= th.bottom) ? 1 : 0;
            int nhh = (g_hMax > 0 && PtInRect(&h, pt) &&
                       pt.x >= thh.left && pt.x <= thh.right) ? 1 : 0;
            if (nhv != g_vHover || nhh != g_hHover) {
                g_vHover = nhv;  g_hHover = nhh;
                ViewPaintSoon();
            }
            if ((g_vMax > 0 && PtInRect(&v, pt)) || (g_hMax > 0 && PtInRect(&h, pt)) ||
                pt.y >= g_headerH) {
                TRACKMOUSEEVENT tme;
                memset(&tme, 0, sizeof(tme));
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                if (!g_barHoverTrack && TrackMouseEvent(&tme)) g_barHoverTrack = TRUE;
            }
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_barHoverTrack = FALSE;
        if (g_vHover || g_hHover) { g_vHover = 0; g_hHover = 0; ViewPaintSoon(); }
        return 0;
    case WM_LBUTTONDOWN: {
        float x = (float)(short)LOWORD(lp), y = (float)(short)HIWORD(lp);
        int hb = ViewHitBtn(x, y);
        if (hb == 0) { ViewHide(); return 0; }    /* ✕ */
        /* ---- 自绘滚动条：拇指 = 拖拽；轨道 = 翻页 ---- */
        RECT v, h;
        ViewBarRects(&v, &h);
        POINT pt = { (int)x, (int)y };
        if (g_vMax > 0 && PtInRect(&v, pt)) {
            RECT th; ViewThumbRectV(&th);
            if (pt.y >= th.top && pt.y <= th.bottom) {
                g_vDrag = TRUE;
                g_vDragGrab = pt.y - th.top;
            } else {
                int page = v.bottom - v.top;   /* 轨道点击 = 翻页 */
                g_vPos += (pt.y < th.top) ? -page : page;
                if (g_vPos < 0) g_vPos = 0;
                if (g_vPos > g_vMax) g_vPos = g_vMax;
                g_animY = FALSE;
                RECT t2; ViewThumbRectV(&t2);
                g_vDrag = TRUE;
                g_vDragGrab = pt.y - t2.top;
                ViewPaintSoon();
            }
            SetCapture(hwnd);
            return 0;
        }
        if (g_hMax > 0 && PtInRect(&h, pt)) {
            RECT th; ViewThumbRectH(&th);
            if (pt.x >= th.left && pt.x <= th.right) {
                g_hDrag = TRUE;
                g_hDragGrab = pt.x - th.left;
            } else {
                int page = h.right - h.left;
                g_hPos += (pt.x < th.left) ? -page : page;
                if (g_hPos < 0) g_hPos = 0;
                if (g_hPos > g_hMax) g_hPos = g_hMax;
                g_animX = FALSE;
                RECT t2; ViewThumbRectH(&t2);
                g_hDrag = TRUE;
                g_hDragGrab = pt.x - t2.left;
                ViewPaintSoon();
            }
            SetCapture(hwnd);
            return 0;
        }
        if (y < g_headerH) {                      /* 标题栏：拖窗 */
            GetCursorPos(&g_viewDragCur);
            g_viewWinDrag = TRUE;
            g_viewWinPos.x = g_viewX;
            g_viewWinPos.y = g_viewY;
            SetCapture(hwnd);
            g_viewClickArm = FALSE;
            return 0;
        }
        /* 图片区按下：**预备**点击（抬起才切换，Ditto 同款 —— 按着拖出窗外
           松手不算点击）。 */
        g_viewClickArm = TRUE;
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_vDrag || g_hDrag) {
            g_vDrag = FALSE;  g_hDrag = FALSE;
            if (GetCapture() == hwnd) ReleaseCapture();
            return 0;
        }
        if (g_viewWinDrag) {
            g_viewWinDrag = FALSE;
            if (GetCapture() == hwnd) ReleaseCapture();
            return 0;
        }
        if (g_viewClickArm) {
            g_viewClickArm = FALSE;
            /* ★ 单击图片 = 切换「缩放图像以适应窗口」（Ditto 同款：
               ImageViewer.cpp OnLButtonUp —— 切开关、重摆、滚回原点）。
               适配比例也与它同式：**按客户区宽 / 图宽**（它把滚动条尺寸
               加回客户区再除图宽，语义就是"可视宽"），不设放大上限。 */
            if (g_viewScale < 0.0f) g_viewScale = 0.0f;    /* 适应 → 100% */
            else                    g_viewScale = -1.0f;   /* 100%/其他 → 适应 */
            g_viewFullPaint = TRUE;      /* 采样变了：整帧 */
            ViewSyncScroll(VIEW_ANCHOR_TOP);
            ViewPaintSoon();
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        g_viewWinDrag = FALSE;
        g_viewClickArm = FALSE;
        g_vDrag = FALSE;  g_hDrag = FALSE;
        return 0;
    case WM_SETCURSOR: {
        /* ---- 放大镜光标（Ditto 同款）----
           图片区上按当前缩放态给光标：适应窗口 = 放大镜 +（点击放大），
           100% = 放大镜 −（点击缩小到适配）。标题栏/按钮/滚动条 = 箭头。 */
        if (LOWORD(lp) == HTCLIENT && g_view) {
            POINT pt;
            DWORD mp = GetMessagePos();
            pt.x = (short)LOWORD(mp);  pt.y = (short)HIWORD(mp);
            ScreenToClient(hwnd, &pt);
            RECT v, h;
            ViewBarRects(&v, &h);
            BOOL onBar = (g_vMax > 0 && PtInRect(&v, pt)) || (g_hMax > 0 && PtInRect(&h, pt));
            if (pt.y >= g_headerH && !onBar && ViewHitBtn((float)pt.x, (float)pt.y) < 0) {
                if (!g_curZoomIn) { g_curZoomIn = MakeZoomCursor(TRUE); g_curZoomOut = MakeZoomCursor(FALSE); }
                SetCursor((g_viewScale < 0.0f) ? g_curZoomIn : g_curZoomOut);
                return TRUE;
            }
        }
        break;   /* 其余区域交给类光标（箭头） */
    }
    case WM_MOUSEWHEEL: {
        if (!g_viewImg) return 0;
        int d = GET_WHEEL_DELTA_WPARAM(wp);
        if (wp & MK_CONTROL) {
            /* Ctrl+滚轮 = 缩放（Ditto 同款）。锚 = 窗口中心：拖图平移没有
               之后，"中心不动"是最可预期的行为（视图向四周均匀展开） */
            float eff = ViewEffScale();
            float ns = (g_viewScale > 0.0f) ? g_viewScale : eff;
            ns *= powf(1.25f, (float)d / (float)WHEEL_DELTA);
            if (ns < 0.05f) ns = 0.05f;
            if (ns > 8.0f)  ns = 8.0f;
            g_viewScale = ns;
            ViewSyncScroll(VIEW_ANCHOR_CENTER);
            ViewPaintSoon();
        } else {
            /* 滚轮 = 上下滚动（Ditto 同款）：上滚看上方 */
            ViewScrollBy(SB_VERT, -d / WHEEL_DELTA);
        }
        return 0;
    }
    case WM_MOUSEHWHEEL:
        if (g_viewImg) {
            int d = GET_WHEEL_DELTA_WPARAM(wp);
            ViewScrollBy(SB_HORZ, d / WHEEL_DELTA);   /* 右滚看右方 */
        }
        return 0;
    case WM_DESTROY:
        return 0;   /* 收尾统一在主窗口的 WM_DESTROY（那里才 PostQuitMessage） */
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void ViewEnsureWindow(void) {
    if (g_view) return;
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = ViewWndProc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.lpszClassName = L"CapsEnhanceClipView";
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.style = CS_DROPSHADOW;    /* 窗口阴影：浮在内容上时分得清边界 */
    RegisterClassW(&wc);
    /* 与列表同一套样式，但有三处**故意**不同：
       · 没有 WS_EX_TRANSPARENT —— 它要吃鼠标（滚轮/滚动条/按钮），穿透了就
         什么都点不到。"点外面就关"的鼠标钩子已把它的矩形并进 inside 判定
         （见 MouseProc）。
       · 没有 WS_EX_LAYERED —— 列表那 5% 整窗透明（CLIP_ALPHA）用来看文字
         无感，用来看图就是把图片和标题栏一起洗淡（用户实测：看不清）。
         预览要的是像素保真，普通窗口直出。
       · CS_DROPSHADOW —— 系统画的窗口投影，预览贴着内容时能分清边界。 */
    g_view = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                             wc.lpszClassName, NULL,
                             WS_POPUP,
                             0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
}

void ViewToggle(void) {
    if (g_viewOpen) { ViewHide(); return; }
    if (g_sel < 0 || g_sel >= g_count || !g_items[g_sel].hasImage) return;
    ViewEnsureWindow();
    if (!g_view) return;
    g_viewRow = g_sel;
    g_viewId = g_items[g_sel].id;
    g_viewScale = 0.0f;
    g_viewFitted = FALSE;
    /* 窗口不在这里显示："根据内容调整窗口大小"（Ditto 勾选项同款）意味着
       尺寸依赖解码结果 —— WM_VIEW_READY 里按内容摆好再 SHOW。逻辑上预览
       此刻已"开着"（g_viewOpen），无关键退场从这一刻就生效。 */
    g_viewOpen = TRUE;
    ViewNotifyState();
    g_viewFullPaint = TRUE;      /* 新会话第一帧必须整画 */
    /* ★ 先收割再判"图是不是现成的"：解码线程对**已经解过的同一请求**不再
       回应（want == doneId 时直接睡回，也不发完成消息）—— 旧代码在这里
       请求解码就永远等不到 WM_VIEW_READY，而窗口又只在 ViewFitOnce 里
       首显，于是"F3 开 -> F3 关 -> 再 F3"第三下再也没反应（实测）。
       现成图（刚关的那张，或槽里解好还没收割的）直接摆窗显示，顺带把
       重开同一张图的延迟也省了。 */
    ViewHarvest();
    if (g_viewImg && g_viewImgId == g_viewId) {
        ViewFitOnce();
        ViewPaint();
        return;
    }
    ThumbRequestViewDecode(g_viewId);
}

/* 解码完成：按图像内容定窗口尺寸（Ditto"根据内容调整窗口大小"同款），
   上限 = 配置的 ClipViewMaxW/H（设置界面可改；不读 Ditto 配置），且仍被
   工作区钳一道（配得再大，超过屏幕也没有意义）、下限标题栏可读。
   首次调用（窗口尚未显示）负责定位+首显：锚在选中行左下角往下展开，
   底下放不下翻到行上方；之后跟随换条目时位置不动、尺寸随新内容。 */
static void ViewFitOnce(void) {
    if (!g_viewImg || !g_view || g_viewFitted) return;
    UINT iw = g_viewImg->GetWidth(), ih = g_viewImg->GetHeight();
    if (!iw || !ih) return;
    RECT work;
    MonitorWork(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &work);
    int maxW = (int)((float)g_viewMaxW * g_k);
    int maxH = (int)((float)g_viewMaxH * g_k);
    if (maxW > work.right - work.left) maxW = work.right - work.left;
    if (maxH > work.bottom - work.top) maxH = work.bottom - work.top;
    /* 目标**客户区** = 100% 原图 + 四周 1px 边框 + 标题栏（"缩放图像以适应
       窗口"在这里生效：是窗口适应图像，图像自己保持原大）。 */
    int nw0 = (int)iw + 2;
    int nh0 = (int)ih + (int)g_headerH + 2;
    int nw = nw0;
    if (nw > maxW) nw = maxW;
    int nh = nh0;
    if (nh > maxH) nh = maxH;
    /* **最小尺寸**（ClipViewMinW/H）：小图的预览窗也撑到这么大 —— 不然
       "适应窗口"跟 100% 没差别，点一下看不出放大。 */
    int minW = (int)((float)g_viewMinW * g_k);
    int minH = (int)((float)g_viewMinH * g_k);
    if (nw < minW) nw = minW;
    if (nh < minH) nh = minH;
    /* 最小值抬完再被工作区钳一道（最小值配得比屏幕还大时以屏幕为准） */
    if (nw > maxW) nw = maxW;
    if (nh > maxH) nh = maxH;
    if (nw > work.right - work.left) nw = work.right - work.left;
    if (nh > work.bottom - work.top) nh = work.bottom - work.top;
    /* ★ 滚动条**按需出现**：图装不下（窗口被上限/最小值压得比图小）才预留
       边条并画拇指；装得下就一点都不留 —— 旧版无条件预留，图不超窗也挂着
       一条空边 + 内缩边框，看着像给内容加了个框（用户实测）。缩放/切换
       适配后若需要而没预留，拇指以半透明小条覆在图缘，不做二次摆窗。 */
    int barPx = (int)(VIEW_BAR_W * g_k);
    if (barPx < 8) barPx = 8;
    g_barW = (nh < nh0) ? barPx : 0;    /* 高度装不下 → 竖条（占右缘） */
    g_barH = (nw < nw0) ? barPx : 0;    /* 宽度装不下 → 横条（占底部） */
    if (g_barW && nw - g_barW < nw0) g_barH = barPx;   /* 竖条挤出的宽度溢出 */
    if (g_barH && nh - g_barH < nh0) g_barW = barPx;   /* 横条挤出的高度溢出 */
    g_viewFitted = TRUE;
    BOOL firstShow = !IsWindowVisible(g_view);
    g_viewW = nw;  g_viewH = nh;
    if (firstShow) {
        int row = (g_viewRow >= 0) ? g_viewRow : g_sel;
        float rowTop = (float)g_py + RowTop(row);
        float rowBot = rowTop + RowHeight(row);
        g_viewX = g_px;
        g_viewY = (int)rowBot;
        if (g_viewY + g_viewH > work.bottom) g_viewY = (int)(rowTop - (float)g_viewH);
        if (g_viewX + g_viewW > work.right)  g_viewX = work.right - g_viewW;
        if (g_viewX < work.left) g_viewX = work.left;
        if (g_viewY < work.top)  g_viewY = work.top;
        if (g_viewY + g_viewH > work.bottom) g_viewY = work.bottom - g_viewH;
    } else {
        /* 跟随换条目：位置不动，尺寸随新内容，越界收进工作区 */
        if (g_viewX + g_viewW > work.right)  g_viewX = work.right - g_viewW;
        if (g_viewY + g_viewH > work.bottom) g_viewY = work.bottom - g_viewH;
        if (g_viewX < work.left) g_viewX = work.left;
        if (g_viewY < work.top)  g_viewY = work.top;
    }
    MoveWindow(g_view, g_viewX, g_viewY, g_viewW, g_viewH, FALSE);
    ViewSyncScroll(VIEW_ANCHOR_TOP);   /* 新内容从头看（Ditto：100% 显示在顶部） */
    if (firstShow) {
        ShowWindow(g_view, SW_SHOWNOACTIVATE);
        SetWindowPos(g_view, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

/* WM_VIEW_READY（解码线程报信）：预览已收起才解完的什么都不做 —— 图留在槽里，
   等下次打开同一条目时由 ViewToggle 收割；不做这个守卫的话，ViewFitOnce 会拿
   g_viewImg 里**上一条**的旧图把已经关掉的窗口再摆出来。 */
void ViewOnDecodeReady(void) {
    if (!g_viewOpen) return;
    /* 大图解好了：先收割（ViewPaint 里也会做，这里先收割才能按真实图
       尺寸收缩窗口），再按原图调一次尺寸，最后重画。 */
    g_viewFullPaint = TRUE;      /* 新图入座：整帧 */
    ViewHarvest();
    ViewFitOnce();
    ViewPaint();
}

void ViewDestroy(void) {
    /* 退出前把解码线程的账清掉：请求归零、槽里的图就地退役 ——
       它们已无人引用（收割是主线程消息处理，此刻串行）。 */
    ThumbViewDrop();
    delete g_viewImg;                       /* 主线程专有：这里释放它最后一张 */
    g_viewImg = NULL;
    g_viewImgId = 0;
    if (g_viewDC)  { DeleteDC(g_viewDC);      g_viewDC  = NULL; }
    if (g_viewBmp) { DeleteObject(g_viewBmp); g_viewBmp = NULL; }
    g_viewBits = NULL;
    g_viewSurfW = 0;  g_viewSurfH = 0;
    if (g_view) { DestroyWindow(g_view); g_view = NULL; }
    g_viewOpen = FALSE;
    g_viewRow = -1;
    g_viewId = 0;
}
