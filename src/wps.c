#include "cap.h"
#include "auto.h"
#include "comptr.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

/* ================= WPS 文字通道（COM 自动化） =================
   为什么需要单独一条通道：WPS 文字既不暴露 UIA/IA2 文本（实测焦点元素
   TextPattern=NO、IAccessibleText=NO），也不响应 WM_GETTEXT（焦点控件类名
   _WwG 只回 "DocView"）。但它**完整实现了 Word 的对象模型** —— 连窗口/控件
   类名都照抄 Word（OpusApp / _WwG），Application.Name 自报 "Microsoft Word"，
   所以可以对它做自动化，读的是它文档引擎里的真数据，完全绕开 UI 层：
     Application.ActiveDocument.Range().Text   全文
     Application.Selection.Start / .End        光标 / 选区（同一套字符坐标）
   实测 32 字符的文档 Range.End=32、Selection.Start=12，两套坐标可交叉验证。

   附着用 ROT + GetActiveObject(CLSIDFromProgID("KWPS.Application"))，**不新建
   实例** —— CoCreateInstance 会另起一个没有文档的 WPS 进程（实测其
   ActiveDocument 直接抛异常），读不到用户窗口里那篇文档。

   判定"是不是 WPS"必须按**进程名**（wps.exe）：WPS 的窗口/控件类名与真正的
   Word 一模一样，按类名判会把 Word 也卷进来。

   ★ 管理员运行附不上（COM 的 ROT 按完整性级别隔离，见 bridge.c），所以要经
     降权桥。普通权限运行时本进程直接读，不需要桥。

   直接置位（免逐格发方向键）已实测可用，三条路的实测结果：
     · Selection.SetRange(start, end)      **可用**，且参数会自动有序化
       （实测 SetRange(20,10) 得到 [10,20]）—— 本模块用它；
     · Document.Range(a,b).Select()        **可用**（置成该区间）
     · Selection.Start = n                 **不能用**！它不折叠选区：实测
       原来是 [12,12]，写 Start=5 之后变成 [5,12]（右端留在原地）。
       所以"移光标"必须发 SetRange(t, t)。
   WPS 上 Selection.Start/End 始终有序（Start<=End），与经典 Edit 通道的
   LOWORD/HIWORD 取向一致，因此位移序列的锚点跟踪可以照用那套逻辑。 */

#define WPS_PROGID    L"KWPS.Application"   /* WPS 文字；表格/演示的对象模型不同，不支持 */
#define WPS_PROC      L"wps.exe"
#define WPS_TEXT_MAX  100000                /* 单次读取上限（UTF-16 字符）；超过放弃本通道 */
#define WPS_DOC_MIN   2                     /* 短于此视为占位，弃用（与 UIA/IA2 同理） */

/* 桥协议：请求 {op,a,b}，应答 = WpsResp + 可选文本。
   应答 status 的含义分两种：GET 用 WPS_ST_*；SET 直接回 WPS_SET_* 分类。 */
#define WPS_ST_OK   0
#define WPS_ST_FAIL 100     /* 子进程侧完全没拿到结果（COM 失败，如没打开文档） */
/* 一次请求/应答的等待上限。管道在本机，400ms 极其宽裕；超时说明桥出了问题，
   BrRecv 会连带丢掉连接（下次重新拉起），不会一直挂着。 */
#define WPS_BR_WAIT_MS 400
#define WPS_OP_GET  1
#define WPS_OP_SET  2

typedef struct { LONG op; LONG a; LONG b; } WpsReq;
typedef struct { LONG status; LONG caret; LONG selEnd; LONG len; } WpsResp;

/* 子进程没有"记录模式"这条开关可依赖，而它一旦失败，用户看到的只有父进程那句
   "桥应答异常" —— 原因全丢。所以子进程里**无条件**写日志。 */
static void WpsLog(const char* fmt, ...) {
    if (!g_recording.load() && !BrIsChild()) return;
    char line[512];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, ap);
    va_end(ap);
    CapLog("%s%s", BrIsChild() ? "[桥WPS] " : "  WPS ", line);
}

/* 焦点进程是不是 WPS 文字。**必须按进程名判**（见文件头）。 */
static BOOL WpsFocusIsWps(HWND hwnd) {
    if (!hwnd) return FALSE;
    WCHAR name[64] = {0};
    if (!ConfigGetProcessName(hwnd, name, 64)) return FALSE;
    return _wcsicmp(name, WPS_PROC) == 0;
}

/* 本线程缓存的 WPS Application。缓存失效（WPS 重启 / 关掉文档再开）时由下面的
   包装丢掉重试一次。

   用 `thread_local ComPtr` 持有：线程结束时析构会自动 Release（以前是裸指针，
   那次缓存只能等进程退出才回收）。「线程级」这件事本身是必须的 ——
   `CoInitialize` 与 COM 套间都是线程级的，跨线程共享接口指针要封送。 */
static thread_local ComPtr<IDispatch> t_wpsApp;

/* IID_IDispatch 只在 `#include <initguid.h>` 的 TU 里才有定义（否则是 extern
   声明、要靠 uuid.lib）。自带一份常量：值固定、与链接顺序无关。 */
static const IID kIID_Dispatch =
    {0x00020400, 0x0000, 0x0000, {0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};

static IDispatch* WpsAppGet(void) {
    if (t_wpsApp) return t_wpsApp.get();
    if (!AdCoUp()) return NULL;
    CLSID clsid;
    /* CLSIDFromProgID 收的是 LPCLSID（指针），而下面的 GetActiveObject 收的是
       REFCLSID（C++ 下是引用）—— 同一个变量，两处写法不同，别统一 */
    if (FAILED(CLSIDFromProgID(WPS_PROGID, &clsid))) {
        WpsLog("CLSIDFromProgID(%ls) 失败（WPS 未注册 COM 接口？）", WPS_PROGID);
        return NULL;
    }
    ComPtr<IUnknown> unk;                 /* GetActiveObject 给的引用交给它放 */
    HRESULT hr = GetActiveObject(clsid, NULL, unk.put());
    if (FAILED(hr) || !unk) {
        /* 0x800401E3 = MK_E_UNAVAILABLE：WPS 没在 ROT 里（没开文档 / 没失去过
           焦点），或者**本进程是管理员**（ROT 按完整性级别隔离，永远看不到
           普通权限的 WPS —— 那种情况应该走降权桥，见 bridge.c）。 */
        WpsLog("GetActiveObject 失败 hr=0x%08X%s", (unsigned)hr,
               (hr == (HRESULT)MK_E_UNAVAILABLE) ? " (MK_E_UNAVAILABLE)" : "");
        return NULL;
    }
    ComPtr<IDispatch> app;
    if (SUCCEEDED(unk->lpVtbl->QueryInterface(unk.get(), kIID_Dispatch,
                                              (void**)app.put())) && app)
        t_wpsApp = app;                   /* 拷贝赋值会 AddRef，缓存多持一份 */
    return t_wpsApp.get();
}

/* 读一次：全文 + 光标 + 选区另一端。text 由调用方 free。 */
static BOOL WpsComReadOnce(WCHAR** text, int* lenOut, int* caretOut, int* selEndOut) {
    IDispatch* app = WpsAppGet();
    /* Selection / ActiveDocument / Range 都是我们 AddRef 到手的子对象，
       全部交给 ComPtr —— 以前每个都在 done: 手动 Release，漏一个就是泄漏。 */
    ComPtr<IDispatch> sel, doc, rng;
    BOOL ok = FALSE;
    long caret = -1, selEnd = -1, rs = -1, re = -1;

    if (!app) return FALSE;
    *text = NULL;
    *lenOut = 0;
    *caretOut = 0;
    if (selEndOut) *selEndOut = 0;

    sel = ComPtr<IDispatch>(AdGetDisp(app, L"Selection"));
    if (!sel || !AdGetLong(sel.get(), L"Start", &caret) || caret < 0) {
        WpsLog("Selection.Start 取不到");
        goto done;
    }
    if (!AdGetLong(sel.get(), L"End", &selEnd) || selEnd < caret) selEnd = caret;
    doc = ComPtr<IDispatch>(AdGetDisp(app, L"ActiveDocument"));
    if (!doc) {
        WpsLog("ActiveDocument 取不到（WPS 没打开文档？）");
        goto done;
    }
    /* 整篇文本用 Document.Range()（不带参数 = 全文）。**不能用 Document.Content**：
       WPS 上 Content.Start/End 取不到（实测 -1），而 Range() 与 Selection 是同
       一套字符坐标。 */
    rng = ComPtr<IDispatch>(AdGetDisp(doc.get(), L"Range"));
    if (!rng) {
        WpsLog("Document.Range 取不到");
        goto done;
    }
    AdGetLong(rng.get(), L"Start", &rs);
    AdGetLong(rng.get(), L"End", &re);
    {
        WCHAR* t = NULL;
        if (!AdGetText(rng.get(), L"Text", &t)) {
            WpsLog("Range.Text 取不到");
            goto done;
        }
        int len = (int)wcslen(t);
        if (len < WPS_DOC_MIN || len > WPS_TEXT_MAX) {
            WpsLog("文档长度 %d 不适合本通道（范围 %d..%d）", len, WPS_DOC_MIN, WPS_TEXT_MAX);
            free(t);
            goto done;
        }
        if (caret > len) caret = len;
        if (selEnd > len) selEnd = len;
        *text = t;
        *lenOut = len;
        *caretOut = (int)caret;
        if (selEndOut) *selEndOut = (int)selEnd;
        ok = TRUE;
        if (g_recording.load())
            CapLog("  WPS doc len=%d caret=%d selEnd=%d Range=[%ld,%ld]",
                   len, *caretOut, (int)selEnd, rs, re);
    }

done:
    return ok;          /* 子对象的 Release 全由上面的 ComPtr 负责 */
}

/* 直接置位一次，并把结果分成三态（见 auto.h 的 WPS_SET_*）。
   分类依据是**回读的位置**，不是 Invoke 的 HRESULT —— provider（这里是 WPS）
   完全可能"返回成功但没动"。 */
static int WpsComSetSelOnce(long start, long end) {
    IDispatch* app = WpsAppGet();
    if (!app) return WPS_SET_UNSURE;
    ComPtr<IDispatch> sel(AdGetDisp(app, L"Selection"));
    if (!sel) return WPS_SET_UNSURE;

    long preS = -1, preE = -1;
    BOOL preOK = AdGetLong(sel.get(), L"Start", &preS) &&
                 AdGetLong(sel.get(), L"End", &preE);
    AdCall2(sel.get(), L"SetRange", start, end);  /* 有序化由 WPS 自己做（实测） */
    long postS = -1, postE = -1;
    BOOL postOK = AdGetLong(sel.get(), L"Start", &postS) &&
                  AdGetLong(sel.get(), L"End", &postE);

    /* 回读拿到的 Start/End **必然有序**（WPS 会自行有序化，见上面的调用注释），
       所以只能与有序区间比较：直接比 start/end 的话，"向右扩选"的调用（活动端
       在首个参数、数值大于锚）永远判不出 OK，会静默滑进 WPS_SET_UNSURE，把
       "可安全退回箭头"这条兜底也一起关掉。 */
    long lo = (start < end) ? start : end;
    long hi = (start < end) ? end : start;
    if (postOK && postS == lo && postE == hi) return WPS_SET_OK;
    /* 位置完全没动 -> 安全的"没生效"：调用方可以照原计划退回逐格箭头 */
    if (postOK && preOK && postS == preS && postE == preE) return WPS_SET_NONE;
    /* 其余情况（读不回来 / 动到了别处）一律"状态不明"：绝不能再补箭头，
       否则两个错误叠加。 */
    return WPS_SET_UNSURE;
}

static void WpsAppDrop(void) {
    t_wpsApp.reset();
}

/* 缓存可能失效（WPS 重启、关掉文档又开）→ 失败就丢掉重试一次。
   只在**曾经拿到过**缓存时重试：首次就没有实例的话再试一次没有意义，
   反而让每次按键都多付一遍 COM 往返。 */
static BOOL WpsComRead(WCHAR** text, int* lenOut, int* caretOut, int* selEndOut) {
    BOOL had = (t_wpsApp ? TRUE : FALSE);
    if (WpsComReadOnce(text, lenOut, caretOut, selEndOut)) return TRUE;
    WpsAppDrop();
    if (had) return WpsComReadOnce(text, lenOut, caretOut, selEndOut);
    return FALSE;
}

static int WpsComSetSel(long start, long end) {
    BOOL had = (t_wpsApp ? TRUE : FALSE);
    int r = WpsComSetSelOnce(start, end);
    if (r == WPS_SET_OK) return r;
    WpsAppDrop();
    if (had) return WpsComSetSelOnce(start, end);
    return r;
}

/* ---------- 经降权桥（管理员运行时唯一通路） ---------- */

/* 发一次请求、收一次应答。**只有运输层失败才丢连接** —— 桥报告"COM 读失败"
   （WPS 没打开文档之类）是业务结果，桥本身还好好的，丢掉反而要重新拉进程。 */
static BOOL WpsBrCall(const WpsReq* req, WpsResp* hdr, WCHAR** text, int* textLen) {
    if (text) { *text = NULL; }
    if (textLen) { *textLen = 0; }
    if (!BrEnsure()) return FALSE;
    if (!BrSend((const BYTE*)req, (DWORD)sizeof(*req))) { BrDrop(); return FALSE; }

    BYTE* buf = NULL;
    DWORD n = 0;
    if (!BrRecv(&buf, &n, WPS_BR_WAIT_MS)) { BrDrop(); return FALSE; }
    if (n < sizeof(WpsResp)) { free(buf); BrDrop(); return FALSE; }   /* 协议不符 */
    memcpy(hdr, buf, sizeof(WpsResp));
    if (hdr->status == WPS_ST_OK && text && hdr->len > 0 &&
        hdr->len <= WPS_TEXT_MAX &&
        n >= sizeof(WpsResp) + (DWORD)hdr->len * sizeof(WCHAR)) {
        size_t bytes = (size_t)hdr->len * sizeof(WCHAR);
        WCHAR* t = (WCHAR*)malloc(bytes + sizeof(WCHAR));
        if (t) {
            memcpy(t, buf + sizeof(WpsResp), bytes);
            t[hdr->len] = 0;
            *text = t;
            if (textLen) *textLen = (int)hdr->len;
        }
    }
    free(buf);
    return TRUE;
}

static BOOL WpsBrRead(WCHAR** text, int* lenOut, int* caretOut, int* selEndOut) {
    WpsReq req; memset(&req, 0, sizeof(req)); req.op = WPS_OP_GET;
    WpsResp hdr; memset(&hdr, 0, sizeof(hdr));
    WCHAR* t = NULL; int tl = 0;
    if (!WpsBrCall(&req, &hdr, &t, &tl)) return FALSE;
    if (hdr.status != WPS_ST_OK) {
        WpsLog("桥报告读失败（WPS 没打开文档？）");
        free(t);
        return FALSE;
    }
    *text = t;
    *lenOut = tl;
    *caretOut = (int)hdr.caret;
    if (selEndOut) *selEndOut = (int)hdr.selEnd;
    return TRUE;
}

static int WpsBrSetSel(long start, long end) {
    WpsReq req; memset(&req, 0, sizeof(req));
    req.op = WPS_OP_SET; req.a = start; req.b = end;
    WpsResp hdr; memset(&hdr, 0, sizeof(hdr));
    if (!WpsBrCall(&req, &hdr, NULL, NULL)) return WPS_SET_UNSURE;
    if (hdr.status == WPS_ST_FAIL) return WPS_SET_UNSURE;
    return (int)hdr.status;                     /* 子进程直接回 WPS_SET_* 分类 */
}

/* 按运行权限选通路（不含"是不是 WPS"的判定，自检也用这一条） */
static BOOL WpsReadAny(WCHAR** text, int* lenOut, int* caretOut, int* selEndOut) {
    if (!IsRunElevated()) return WpsComRead(text, lenOut, caretOut, selEndOut);
    return WpsBrRead(text, lenOut, caretOut, selEndOut);
}

/* ---------- 对外的通道接口 ---------- */

BOOL WpsGetTextCaret(HWND focus, WCHAR** text, int* lenOut, int* caretOut, int* selEndOut) {
    if (!text || !lenOut || !caretOut) return FALSE;
    if (!WpsFocusIsWps(focus)) return FALSE;
    return WpsReadAny(text, lenOut, caretOut, selEndOut);
}

int WpsSetSel(HWND focus, int start, int end) {
    /* 不是 WPS 焦点：本模块什么都没做，所以"可以安全退回箭头" */
    if (!WpsFocusIsWps(focus)) return WPS_SET_NONE;
    if (!IsRunElevated()) return WpsComSetSel(start, end);
    return WpsBrSetSel(start, end);
}

/* ---------- 桥的子进程侧 ---------- */

static BOOL WpsBridgeCall(const BYTE* req, DWORD reqLen, BYTE** resp, DWORD* respLen) {
    if (!req || reqLen < sizeof(WpsReq)) return FALSE;
    WpsReq r;
    memcpy(&r, req, sizeof(r));
    WpsResp h;
    memset(&h, 0, sizeof(h));

    if (r.op == WPS_OP_GET) {
        WCHAR* text = NULL;
        int len = 0, caret = 0, selEnd = 0;
        if (!WpsComRead(&text, &len, &caret, &selEnd)) {
            h.status = WPS_ST_FAIL;
        } else {
            h.status = WPS_ST_OK;
            h.caret = caret;
            h.selEnd = selEnd;
            h.len = len;
        }
        DWORD bytes = (DWORD)sizeof(WpsResp) +
                      ((h.status == WPS_ST_OK && len > 0) ? (DWORD)len * sizeof(WCHAR) : 0);
        BYTE* b = (BYTE*)malloc(bytes);
        if (!b) { free(text); return FALSE; }
        memcpy(b, &h, sizeof(WpsResp));
        if (h.status == WPS_ST_OK && len > 0)
            memcpy(b + sizeof(WpsResp), text, (size_t)len * sizeof(WCHAR));
        free(text);
        *resp = b;
        *respLen = bytes;
        return TRUE;
    }
    if (r.op == WPS_OP_SET) {
        h.status = WpsComSetSel(r.a, r.b);      /* WPS_SET_* 直接透传给父进程 */
        long s = -1, e = -1;
        IDispatch* app = WpsAppGet();          /* 借来的（线程缓存），不包 ComPtr */
        if (app) {
            ComPtr<IDispatch> sel(AdGetDisp(app, L"Selection"));
            if (sel) {
                AdGetLong(sel.get(), L"Start", &s);
                AdGetLong(sel.get(), L"End", &e);
            }
        }
        h.caret = s;
        h.selEnd = e;
        BYTE* b = (BYTE*)malloc(sizeof(WpsResp));
        if (!b) return FALSE;
        memcpy(b, &h, sizeof(WpsResp));
        *resp = b;
        *respLen = (DWORD)sizeof(WpsResp);
        return TRUE;
    }
    WpsLog("收到未知请求 op=%ld", (long)r.op);
    return FALSE;
}

static void WpsComTeardown(void) { WpsAppDrop(); }

int WpsBridgeServe(unsigned long pid, unsigned long seq) {
    /* 子进程本就该是普通权限；若还是管理员，说明降权没成功，桥毫无意义 */
    if (IsRunElevated()) {
        WpsLog("子进程仍是管理员权限，放弃（降权没生效）");
        return 3;
    }
    if (!AdCoUp()) {
        WpsLog("COM 初始化失败，放弃");
        return 1;
    }
    return BrServe(pid, seq, WpsBridgeCall, WpsComTeardown);
}

void WpsBridgeShutdown(void) { BrShutdown(); }
