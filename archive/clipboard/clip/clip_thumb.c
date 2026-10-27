/* ======================================================================
   CapsEnhance —— 缩略图缓存 + 解码线程（原 clip.c 的缩略图段，拆分而出）
   职责：
     · 按 id 的 LRU 缩略图缓存（列表重载不作废，滚动只补新进入视野的）；
     · 后台解码线程：只解**可见范围**里未缓存的条目（Ditto 的预载模型），
       F3 预览的大图解码随时插队；
     · F3 预览大图的解码槽（g_viewWantId/DoneId/Decoded），主线程收割。
   列表数据（g_items/g_count）、窗口（g_hwnd）、行几何与字体都来自 clip.c
   （clip.h 里 extern）。
   ====================================================================== */
#include "cap.h"
#include "clip.h"

#include <stdio.h>
#include <stdlib.h>

#include "sqlite3mc_amalgamation.h"

/* ================= 缩略图缓存（按条目 id，LRU 上限，Ditto 同款） =================
   ★ 缓存按 **id** 而不是行下标：列表重载（复制了新条目 / 粘贴后重排）只是
   条目集合变了，已解好的图照样有效 —— 旧做法"重载即全部作废重来"是"缩略图
   加载慢"的主因（Ditto 开着时每复制一条，下次打开就要重解所有图；Ditto 自己
   是 m_cf_dibCache[id] + 上限 50 条的 LRU，重载只解新增的那条，这里同构）。
   解不出图的条目也进缓存（bm=NULL 的负缓存，Ditto 的 m_cf_NO_dibCache 同款），
   否则每次显示都要为它白跑一趟库。
   缩略图在**后台线程**解码并缩到目标尺寸：磁盘 + 解码 + 缩放全不在 UI 线程；
   渲染只画现成的小图（1:1 贴），没解出来的行**不画占位灰框**（Ditto 同款：
   行里只有文字，"没加载"不该是一个显眼的状态 —— 灰框才是"一直在加载"观感
   的来源），文字预留缩略图宽度，图到了按实际宽度排。 */
#define THUMB_CACHE_MAX 60
typedef struct {
    long long        id;
    Gdiplus::Bitmap* bm;   /* NULL = 该条目解不出图（负缓存） */
    float            w;    /* bm 的宽（渲染时文字让开用；负缓存为 0） */
    unsigned         cnt;  /* LRU 计数（单调递增，越小越旧） */
} ThumbEnt;
static ThumbEnt         g_thumbCache[THUMB_CACHE_MAX];
static int              g_thumbCacheN = 0;
static unsigned         g_thumbCnt = 0;
static HANDLE           g_thumbWake = NULL;   /* 自动重置事件：有新活/退出 */
static volatile LONG    g_thumbVisFirst = 0;  /* 当前可见首行（主线程写，worker 读） */
static volatile LONG    g_thumbVisLast  = 0;  /* 当前可见末行（含） */
static CRITICAL_SECTION g_thumbLock;
static HANDLE           g_thumbThread = NULL;
static volatile LONG    g_thumbQuit = 0;
/* 本帧缩略图实际画了多宽。★ 文字的横向起点必须用它，不能用行高 ——
   宽度是按原图比例算的，横图远比行高宽，拿行高当偏移文字就会压在图上。 */
static float            g_thumbW[CLIP_MAX_ITEMS];

/* 锁内查找：命中返回 TRUE（*bmOut 可为 NULL = 负缓存命中），并刷新 LRU。 */
static BOOL ThumbFind(long long id, Gdiplus::Bitmap** bmOut, float* w) {
    for (int i = 0; i < g_thumbCacheN; i++) {
        if (g_thumbCache[i].id == id) {
            g_thumbCache[i].cnt = ++g_thumbCnt;
            *bmOut = g_thumbCache[i].bm;
            *w = g_thumbCache[i].w;
            return TRUE;
        }
    }
    *bmOut = NULL;
    *w = 0;
    return FALSE;
}

/* 锁内写入：已存在就替换（旧图就地释放）；满了按 LRU 淘汰最旧的。 */
static void ThumbPut(long long id, Gdiplus::Bitmap* bm, float w) {
    int slot = -1, oldest = 0;
    for (int i = 0; i < g_thumbCacheN; i++) {
        if (g_thumbCache[i].id == id) { slot = i; break; }
        if (g_thumbCache[i].cnt < g_thumbCache[oldest].cnt) oldest = i;
    }
    if (slot < 0) {
        if (g_thumbCacheN < THUMB_CACHE_MAX) {
            slot = g_thumbCacheN++;
        } else {
            slot = oldest;                     /* 淘汰最旧的（含负缓存项） */
        }
    }
    delete g_thumbCache[slot].bm;
    g_thumbCache[slot].id = id;
    g_thumbCache[slot].bm = bm;
    g_thumbCache[slot].w = w;
    g_thumbCache[slot].cnt = ++g_thumbCnt;
}

static void FreeThumbs(void) {
    EnterCriticalSection(&g_thumbLock);
    for (int i = 0; i < g_thumbCacheN; i++) {
        delete g_thumbCache[i].bm;
        g_thumbCache[i].bm = NULL;
    }
    g_thumbCacheN = 0;
    LeaveCriticalSection(&g_thumbLock);
}

/* ---------- F3 图片预览的解码请求（与缩略图共用 g_thumbLock / g_thumbWake） ----------
   预览与缩略图的解码都在同一条后台线程上排队：预览一次只要一张图，
   另起线程与事件只会多一条唤醒路径要记（详见 ThumbThreadProc 的预览轮）。
   lID 从 1 开始（Ditto 的主键），0 当"没有请求"用是安全的。 */
static long long        g_viewWantId = 0;      /* 正在请求解码的条目 id（锁内读写） */
static long long        g_viewDoneId = 0;      /* 线程最近解码完成的 id（仅线程读写） */
static Gdiplus::Bitmap* g_viewDecoded = NULL;  /* 线程解好待收割的图（锁内交接，见 ViewHarvest） */
static long long        g_viewDecodedId = 0;   /* 槽里这张图属于哪条（收割时对 g_viewId 防"旧图挂新窗"） */

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

/* 只读连接：一轮解码开一次（原来每张图开/关一次，纯开销）。
   空闲的只读连接不持任何锁，不影响 Ditto 那边的写入。 */
static sqlite3* DbOpen(void) {
    char pathU8[DITTO_DIR_MAX * 4];
    if (!Utf8FromW(g_dbPath, pathU8, (int)sizeof(pathU8))) return NULL;
    sqlite3* db = NULL;
    if (sqlite3_open_v2(pathU8, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }
    sqlite3_busy_timeout(db, 3000);
    return db;
}

/* 解码**原图**（不缩放）：缩略图与 F3 预览大图共用这一个入口。
   格式取舍照 Ditto（QPasteWndThread.cpp:349 起的取法）：先精确找 CF_DIB，
   找不到才找 PNG，别的格式（含 CF_DIBV5）一律不用 —— 多格式条目两边解
   同一个源。CASE 把 CF_DIB 排最前，取**首个命中行**（解码失败也不换源，
   Ditto 同样只在"没有 CF_DIB"时才退 PNG）。db 由调用方开好传入。 */
static Gdiplus::Bitmap* DecodeBig(sqlite3* db, long long id) {
    Gdiplus::Bitmap* big = NULL;
    sqlite3_stmt* st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT strClipBoardFormat, ooData FROM Data "
                              "WHERE lParentID = ? "
                              "AND strClipBoardFormat IN ('CF_DIB', 'PNG') "
                              "ORDER BY CASE strClipBoardFormat "
                              "WHEN 'CF_DIB' THEN 0 ELSE 1 END, Data.lID",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
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
    return big;
}

/* 解码 + **缩到目标尺寸**，返回缩好的小图（调用方负责 delete）。
   缩放在这里一次做完 —— 渲染时就是 1:1 贴，不再每帧做大图高质量缩放
   （HighQualityBicubic 对大图很贵，这正是"图片多滚动卡"的主要开销）。 */
static Gdiplus::Bitmap* DecodeThumb(sqlite3* db, int idx, float* outW) {
    Gdiplus::Bitmap* big = DecodeBig(db, g_items[idx].id);
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
    *outW = bw;
    return out;
}

/* 后台解码线程：常驻循环 —— 等唤醒事件，把带图条目的缩略图解码一遍。
   旧版"进程活着只做这一轮"：驻留期间条目集一变（复制了新条目 / 粘贴后
   Ditto 重排序），DbLoadItems 作废全部缓存，却没人再解码 → 第一次开
   正常、再开全是占位灰框。现在 DbLoadItems 换代后 SetEvent 叫醒重做。 */
static DWORD WINAPI ThumbThreadProc(LPVOID lp) {
    (void)lp;
    /* ---- 只解**可见**范围（Ditto 同款预载模型）----
       主线程随显示/滚动把当前可见行区间写进 g_thumbVisFirst/Last 并叫醒本
       线程；这里把区间内未缓存的图片逐张解出来，解完就睡。冷启动的解码量
       因此是"一屏"而不是"整个历史" —— 历史里大图再多，首屏也只要一两张的
       时间；往下滚，新进入视野的才解（旧版一口气解完所有条目，历史里大图
       一多就是数秒的落后）。预览请求（F3 正盯着的加载）随时插队。 */
    sqlite3* tdb = NULL;
    for (;;) {
        WaitForSingleObject(g_thumbWake, INFINITE);
        if (g_thumbQuit) { if (tdb) sqlite3_close(tdb); return 0; }

        for (;;) {                       /* 把当前可见范围的活干完再睡 */
            if (g_thumbQuit) break;

            /* ---- F3 预览的大图优先 ----
               与缩略图共用线程/事件/锁。解好的图放 g_viewDecoded 槽里等主线程
               收割（ViewHarvest），不直接换 g_viewImg：那张归主线程专有，旧图
               的释放点在主线程消息处理里，线程直接换指针再 delete 旧图，
               ViewPaint 正画着的那张就可能被释放（use-after-free，窗口小但真实）。
               want == doneId 时**补发一次完成消息**：重开同一条目或解码失败的
               条目再按 F3，主程序不知道"这个请求早有结论"，没有这条消息它就
               永远停在"解码中"。消息幂等，解码不重复。 */
            EnterCriticalSection(&g_thumbLock);
            long long want = g_viewWantId;
            LeaveCriticalSection(&g_thumbLock);
            if (want) {
                if (want == g_viewDoneId) {
                    PostMessageW(g_hwnd, WM_VIEW_READY, 0, 0);
                    EnterCriticalSection(&g_thumbLock);
                    if (g_viewWantId == want) g_viewWantId = 0;   /* 请求已有结论，清账 */
                    LeaveCriticalSection(&g_thumbLock);
                } else {
                    Gdiplus::Bitmap* bm = NULL;
                    if (!tdb) tdb = DbOpen();
                    if (tdb) bm = DecodeBig(tdb, want);
                    Gdiplus::Bitmap* stale = NULL;
                    EnterCriticalSection(&g_thumbLock);
                    g_viewDoneId = want;
                    if (g_viewWantId == want) {
                        g_viewWantId = 0;              /* 请求已答复，清账 */
                        stale = g_viewDecoded;         /* 没收割的旧图让出槽 */
                        g_viewDecoded = bm; g_viewDecodedId = want;
                        bm = NULL;
                    }
                    LeaveCriticalSection(&g_thumbLock);
                    delete stale;                      /* 锁外放：收割与这里串行 */
                    delete bm;                         /* 请求已变的这张（含 NULL） */
                    PostMessageW(g_hwnd, WM_VIEW_READY, 0, 0);
                    Sleep(0);
                    continue;                          /* 回去继续扫可见范围 */
                }
            }

            /* ---- 可见范围内找一个未缓存的图片条目 ---- */
            EnterCriticalSection(&g_thumbLock);
            int vf = g_thumbVisFirst, vl = g_thumbVisLast;
            LeaveCriticalSection(&g_thumbLock);
            if (vf < 0) vf = 0;
            int todo = -1;
            for (int i = vf; i <= vl && i < g_count; i++) {
                if (!g_items[i].hasImage) continue;
                Gdiplus::Bitmap* c = NULL;
                float cw2 = 0;
                EnterCriticalSection(&g_thumbLock);
                BOOL have = ThumbFind(g_items[i].id, &c, &cw2);
                LeaveCriticalSection(&g_thumbLock);
                if (!have) { todo = i; break; }
            }
            if (todo < 0) break;                 /* 这一屏全齐了：睡 */

            if (!tdb) tdb = DbOpen();
            float w = 0;
            long long id = g_items[todo].id;     /* 记下解的是谁，防中途重载串位 */
            Gdiplus::Bitmap* bm = tdb ? DecodeThumb(tdb, todo, &w) : NULL;
            EnterCriticalSection(&g_thumbLock);
            if (g_items[todo].id == id) {
                ThumbPut(id, bm, w);             /* 失败就是 NULL：负缓存，不再重试 */
                bm = NULL;                       /* 缓存已接管 */
            }
            LeaveCriticalSection(&g_thumbLock);
            delete bm;                           /* 没接管的（含 NULL）在这里释放 */
            PostMessageW(g_hwnd, WM_THUMB_READY, (WPARAM)todo, 0);
            Sleep(0);
        }
        if (tdb) { sqlite3_close(tdb); tdb = NULL; }
    }
}

/* ---- 对外接口（见 clip.h）---- */

void ThumbsInit(void) {
    InitializeCriticalSection(&g_thumbLock);
    g_thumbWake = CreateEventW(NULL, FALSE, FALSE, NULL);
}

void ThumbsStartThread(void) {
    if (g_thumbWake)    /* 事件没建成就别起线程：等一个 NULL 句柄是忙循环 */
        g_thumbThread = CreateThread(NULL, 0, ThumbThreadProc, NULL, 0, NULL);
}

void ThumbsShutdown(void) {
    if (g_thumbThread) {
        InterlockedExchange(&g_thumbQuit, 1);
        if (g_thumbWake) SetEvent(g_thumbWake);   /* 可能正等着活干：叫醒它好退场 */
        WaitForSingleObject(g_thumbThread, 3000);
        CloseHandle(g_thumbThread);
        g_thumbThread = NULL;
    }
    if (g_thumbWake) { CloseHandle(g_thumbWake); g_thumbWake = NULL; }
    FreeThumbs();
    DeleteCriticalSection(&g_thumbLock);
}

void ThumbsWake(void) {
    if (g_thumbWake) SetEvent(g_thumbWake);
}

void ThumbSetVisRange(int first, int last) {
    BOOL changed = FALSE;
    EnterCriticalSection(&g_thumbLock);
    if (first != g_thumbVisFirst || last != g_thumbVisLast) {
        g_thumbVisFirst = first;
        g_thumbVisLast = last;
        changed = TRUE;
    }
    LeaveCriticalSection(&g_thumbLock);
    if (changed && g_thumbWake) SetEvent(g_thumbWake);
}

Gdiplus::Bitmap* ThumbLookup(long long id, float* w) {
    Gdiplus::Bitmap* bm = NULL;
    EnterCriticalSection(&g_thumbLock);
    ThumbFind(id, &bm, w);
    LeaveCriticalSection(&g_thumbLock);
    return bm;
}

BOOL ThumbHasConclusion(long long id) {
    Gdiplus::Bitmap* bm = NULL;
    float w = 0;
    EnterCriticalSection(&g_thumbLock);
    BOOL have = ThumbFind(id, &bm, &w);
    LeaveCriticalSection(&g_thumbLock);
    return have;
}

void ThumbRequestViewDecode(long long id) {
    EnterCriticalSection(&g_thumbLock);
    g_viewWantId = id;
    LeaveCriticalSection(&g_thumbLock);
    ThumbsWake();
}

/* 主线程收割：把解码线程放进槽的图接走（属于 viewId 的才给）。
   旧图不在这里动 —— 它归主线程专有，由调用方就地 delete。 */
BOOL ThumbViewTake(long long viewId, Gdiplus::Bitmap** out, long long* outId) {
    *out = NULL;
    *outId = 0;
    EnterCriticalSection(&g_thumbLock);
    BOOL got = (g_viewDecoded && g_viewDecodedId == viewId);
    if (got) {
        *out = g_viewDecoded;
        *outId = g_viewDecodedId;
        g_viewDecoded = NULL;
    }
    LeaveCriticalSection(&g_thumbLock);
    return got;
}

BOOL ThumbViewDoneFor(long long viewId) {
    return g_viewDoneId == viewId;   /* 线程做完这条且没有新请求 = 有结论而无图 */
}

void ThumbViewDrop(void) {
    EnterCriticalSection(&g_thumbLock);
    g_viewWantId = 0;                     /* 在途的别再入槽（拦不住已在解的） */
    Gdiplus::Bitmap* drop = g_viewDecoded;
    g_viewDecoded = NULL;
    LeaveCriticalSection(&g_thumbLock);
    delete drop;                          /* 已无人引用（收割与这里串行） */
}
