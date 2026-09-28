/* ======================================================================
   CapsEnhance —— clip 浮层各模块的共享声明（内部头，不对其它 exe 暴露）
   拆分格局：
     clip.c        程序入口 / 列表窗口 / 粘贴 / 数据库列表查询 / Ditto 设置
     clip_thumb.c  缩略图缓存 + 解码线程（含 F3 预览大图的解码槽）
     clip_view.c   F3 图片预览窗口（自绘滚动条 / 放大镜光标 / 点击缩放）
   共享状态在 clip.c 定义、这里 extern；各模块自己的状态留在自己的 .c。
   ====================================================================== */
#pragma once

#pragma warning(push)
#pragma warning(disable: 6385 28252 28253)
#include <gdiplus.h>
#pragma warning(pop)

/* ---- 条目与常量（定义在 clip.c）---- */
#define CLIP_TEXT_MAX 160
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

#define CLIP_MAX_ITEMS 100        /* 从库里取的条目上限（SQL 的 LIMIT 也由它拼出） */
#define ROWS_VISIBLE 10           /* 基准可见行数：窗口高度按它算 */

/* ---- 预览窗配色（照抄 Ditto 默认主题；clip.c 与 clip_view.c 共用）---- */
#define C_BG        0xFFFFFFFF   /* ListBoxOddRowsBG */
#define C_BORDER    0xFFCCCCCC   /* Border */
#define C_TEXT      0xFF000000   /* ListBoxOddRowsText */
#define C_TEXT_DIM  0xFF8C8C8C   /* 空态提示 */
#define C_ROW_ALT   0xFFF3F3F3   /* ListBoxEvenRowsBG */
#define C_SEL_BG    0xFFCCCCCC   /* ListBoxSelectedBG */
#define C_HEAD      0xFFFFFFFF   /* 标题栏底 */
#define C_TITLE     0xFF6E6E6E   /* 标题文字 */
#define C_INDEX     0xFF6E6E6E   /* 序号 */
#define C_THUMB_BG  0xFFF0F0F0   /* MainWindowBG */

#define VIEW_BAR_W   10          /* 预览自绘滚动条的条宽（96 DPI 像素，用时 × g_k） */

/* 浮层内部消息（clip_thumb.c -> 列表窗口 clip.c） */
#define WM_THUMB_READY (WM_APP + 20)   /* 一张缩略图就绪（wParam=行下标） */
#define WM_VIEW_READY  (WM_APP + 21)   /* 预览大图解码完成 */

/* ---- 列表窗口与绘制共享状态（定义在 clip.c）---- */
extern ClipItem     g_items[CLIP_MAX_ITEMS];
extern int          g_count;
extern int          g_sel;
extern float        g_k;                 /* DPI 缩放比 */
extern float        g_headerH, g_gutterW, g_rowH1, g_rowH2, g_thumbPad, g_lineH;
extern int          g_pxW, g_pxH;        /* 列表窗口尺寸（物理像素） */
extern int          g_px, g_py;          /* 列表窗口位置 */
extern HFONT        g_fontRow, g_fontSmall, g_tipFont;
extern WCHAR        g_dbPath[];          /* <DittoDir>\Ditto.db（解析后的真值） */
extern WCHAR        g_dirPath[];         /* Ditto 文件夹 */
extern BOOL         g_dittoOk;           /* <dirPath>\Ditto.exe 存在 */
extern int          g_barW, g_barH;      /* 预览自绘滚动条的条宽/高（FitMetrics 里定） */
extern int          g_viewMaxW, g_viewMaxH, g_viewMinW, g_viewMinH;
extern HWND         g_hwnd;              /* 列表窗口（解码线程的报信目标） */

/* ---- clip.c 提供给 view/thumb 的函数 ---- */
float RowHeight(int idx);
float RowTop(int idx);
int   ClipLoadFormats(long long id);     /* 取一条的全部格式进 g_fmts，返回条数 */
BOOL  PutOnClipboard(BOOL plain);        /* g_fmts 写回系统剪贴板（挂 Ditto 屏蔽标记） */
void  FreeFmts(void);
void  DittoAction(void);                 /* 转 Ditto 本体（预览/列表的 Ditto 按钮） */
void  MonitorWork(HMONITOR mon, RECT* out);  /* 工作区矩形（预览拖动/摆窗用） */
void  ThumbUpdateVisRange(void);         /* 可见行区间重算（渲染/条带里调用，clip.c 实现） */

/* ---- clip_view.c：F3 图片预览窗口 ---- */
void  ViewToggle(void);                  /* F3：开/关预览（当前条目非图片则不动作） */
void  ViewHide(void);                    /* 收起预览（保留解码结果） */
void  ViewFollowSel(void);               /* 选中行变了：预览跟随/收起 */
void  ViewRevalidate(void);              /* 列表重载后校验预览指向 */
void  ViewOnDecodeReady(void);           /* WM_VIEW_READY：收割新图并按内容摆窗 */
void  ViewDestroy(void);                 /* 进程退出收尾 */
void  ViewInitMessages(void);            /* 注册预览的注册消息（wWinMain 早期调用） */
BOOL  ViewIsOpen(void);                  /* 预览会话进行中？（列表键盘/鼠标分支用） */
HWND  ViewWnd(void);                     /* 预览窗口（点外关闭判定的第二矩形） */
BOOL  ViewIsViewMsg(UINT msg);           /* 这条消息是不是 F3 预览的 toggle/退场 */

/* ---- clip_thumb.c：缩略图缓存 + 解码线程 ---- */
void  ThumbsInit(void);                  /* 临界区 + 唤醒事件（首次 DbLoadItems 之前） */
void  ThumbsStartThread(void);           /* 起解码线程（g_hwnd 就绪后） */
void  ThumbsShutdown(void);              /* 退出：停线程、清缓存 */
void  ThumbsWake(void);                  /* 有新活（列表重载 / 预览请求 / 可见范围变了） */
void  ThumbSetVisRange(int first, int last);  /* 当前可见行区间（变化才唤醒） */
Gdiplus::Bitmap* ThumbLookup(long long id, float* w);  /* 缓存查找（命中刷新 LRU） */
BOOL  ThumbHasConclusion(long long id);  /* 缓存里有结论（图或负缓存）？ */
void  ThumbRequestViewDecode(long long id);   /* F3 预览：请求解码某条（幂等） */
BOOL  ThumbViewTake(long long viewId, Gdiplus::Bitmap** out, long long* outId);
void  ThumbViewDrop(void);               /* 预览销毁：在途请求归零、槽图退役 */
BOOL  ThumbViewDoneFor(long long viewId);/* 该条的预览解码已有结论？（标题"解码失败"用） */
BOOL  Utf8FromW(const WCHAR* w, char* out, int cap);  /* 路径转 UTF-8（clip.c 实现） */

/* ---- clip.c：列表绘制的两个小工具（预览标题/按钮共用）---- */
COLORREF CR(unsigned argb);              /* 0xAARRGGBB -> COLORREF */
void  GdiText(HDC dc, const WCHAR* s, HFONT f, COLORREF c, float x, float y,
              float w, float h, UINT align, BOOL ellipsis);
