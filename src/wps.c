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
   LOWORD/HIWORD 取向一致，因此位移序列的锚点跟踪可以照用那套逻辑。

   ================= WPS 演示通道（wpp.exe，KWPP.Application） =================
   演示与文字同病：光标对标准接口全不可见（无 Win32 光标、无 MSAA caret、
   UIA 无 TextPattern、走 TSF 不走 IMM），词跳转只能走对象模型。模型抄
   PowerPoint：Selection 挂在 **Window** 上（不是 Application），且只有
   Selection.Type == 3（ppSelectionText，文本编辑态）才有 TextRange —— 非编辑
   态拿的是形状，词跳转无从谈起，直接放弃本通道。

   实测坐标语义（都验证过）：
     · Selection.TextRange.Start 是 **1 基**"第 N 个字符之前"（PowerPoint 官方
       是 0 基，WPS 不一样！）；Length 就是选区长。0 基 caret = Start - 1。
     · 全文 = TextRange.Parent(=TextFrame).TextRange.Text，与 Start 同一坐标
       系；段落分隔符 \r 计 1 字符，与 Text 串对齐。
     · Text 与集合 Item 的 GetIDsOfNames **查不到**（它们映射到 DISPID_VALUE，
       WPS 的名字解析不认）—— autodisp.c 的 AdId 对这两个名字回退 DISPID 0。
     · 置位 = Parent.TextRange.Characters(lo+1, hi-lo).Select()：lo/hi 是 0 基
       有序两端，折叠时长度 0 合法，lo+1 可到 len+1（折叠到末尾）。**没有**
       SetRange/Collapse/End，Start 属性只读，这是唯一的置位原语。
     · 置位后回读校验：Start-1 == lo && Length == hi-lo。
     · ★ 与文字通道的差异：Select() 后 WPS 内部的"活动端"恒为**选区左端**，
       文字通道那种"SetRange 首参即活动端"的手法没法复刻 —— 向右扩选（活动
       端在右）后用户手按 Shift+方向 会动到左端。影响仅此一处，下次跳转即
       重置；不想为它引入真实按键序列。

   表格（et.exe）实测**不可适配**：单元格编辑态下焦点是网格窗口 EXCEL6 自己
   （无子编辑控件，WM_GETTEXT/EM_* 全空），编辑栏 EtFmlEditBar 只是个框架
   窗口（真编辑器是 Qt 内绘、无 HWND），UIA 无 TextPattern（Value 恒空）、
   MSAA 无子对象无 IAccessibleText，KET.Application 的 Excel 模型也够不到
   格内编辑缓冲。词跳转在这些程序里走原生 Ctrl+方向兜底（nav.c 现有逻辑）。 */

#define WPS_PROGID    L"KWPS.Application"   /* WPS 文字 */
#define WPS_PROC      L"wps.exe"
#define WPP_PROGID    L"KWPP.Application"   /* WPS 演示 */
#define WPP_PROC      L"wpp.exe"
#define WPS_SEL_TEXT  3                     /* ppSelectionText：文本编辑态 */
#define WPS_TEXT_MAX  100000                /* 单次读取上限（UTF-16 字符）；超过放弃本通道 */
#define WPS_DOC_MIN   2                     /* 短于此视为占位，弃用（与 UIA/IA2 同理） */

/* 按进程名分派的子通道 */
enum { WPSA_TEXT = 0, WPSA_SHOW = 1, WPSA_COUNT };

/* 桥协议：请求 {op,a,b}，应答 = WpsResp + 可选文本。
   应答 status 的含义分两种：GET 用 WPS_ST_*；SET 直接回 WPS_SET_* 分类。 */
#define WPS_ST_OK   0
#define WPS_ST_FAIL 100     /* 子进程侧完全没拿到结果（COM 失败，如没打开文档） */
/* 一次请求/应答的等待上限。管道在本机，400ms 极其宽裕；超时说明桥出了问题，
   BrRecv 会连带丢掉连接（下次重新拉起），不会一直挂着。 */
#define WPS_BR_WAIT_MS 400
#define WPS_OP_GET  1
#define WPS_OP_SET  2

typedef struct { LONG op; LONG app; LONG a; LONG b; } WpsReq;   /* app = WPSA_* */
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

/* 焦点进程属于哪条 WPS 子通道。**必须按进程名判**（见文件头）：
   WPS 的窗口/控件类名与真正 Office 一模一样，按类名判会把 Word/PPT 卷进来。 */
static int WpsFocusKindOf(HWND hwnd) {
    if (!hwnd) return -1;
    WCHAR name[64] = {0};
    if (!ConfigGetProcessName(hwnd, name, 64)) return -1;
    if (_wcsicmp(name, WPS_PROC) == 0) return WPSA_TEXT;
    if (_wcsicmp(name, WPP_PROC) == 0) return WPSA_SHOW;
    return -1;
}

/* 本线程缓存的 WPS Application（文字/演示各一份）。缓存失效（WPS 重启 /
   关掉文档再开）时由下面的包装丢掉重试一次。

   用 `thread_local ComPtr` 持有：线程结束时析构会自动 Release（以前是裸指针，
   那次缓存只能等进程退出才回收）。「线程级」这件事本身是必须的 ——
   `CoInitialize` 与 COM 套间都是线程级的，跨线程共享接口指针要封送。 */
static thread_local ComPtr<IDispatch> t_wpsApp[WPSA_COUNT];

/* IID_IDispatch 只在 `#include <initguid.h>` 的 TU 里才有定义（否则是 extern
   声明、要靠 uuid.lib）。自带一份常量：值固定、与链接顺序无关。 */
static const IID kIID_Dispatch =
    {0x00020400, 0x0000, 0x0000, {0xC0,0x00,0x00,0x00,0x00,0x00,0x00,0x46}};

static IDispatch* WpsAppGet(int kind) {
    if (t_wpsApp[kind]) return t_wpsApp[kind].get();
    if (!AdCoUp()) return NULL;
    const WCHAR* progid = (kind == WPSA_SHOW) ? WPP_PROGID : WPS_PROGID;
    CLSID clsid;
    /* CLSIDFromProgID 收的是 LPCLSID（指针），而下面的 GetActiveObject 收的是
       REFCLSID（C++ 下是引用）—— 同一个变量，两处写法不同，别统一 */
    if (FAILED(CLSIDFromProgID(progid, &clsid))) {
        WpsLog("CLSIDFromProgID(%ls) 失败（WPS 未注册 COM 接口？）", progid);
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
        t_wpsApp[kind] = app;             /* 拷贝赋值会 AddRef，缓存多持一份 */
    return t_wpsApp[kind].get();
}

/* ---------- 文字：读一次（全文 + 光标 + 选区另一端）。text 由调用方 free。 ---------- */
static BOOL WpsTextReadOnce(WCHAR** text, int* lenOut, int* caretOut, int* selEndOut) {
    IDispatch* app = WpsAppGet(WPSA_TEXT);
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

/* ---------- 演示：读一次。坐标系与置位规则见文件头。 ---------- */
static BOOL WpsShowReadOnce(WCHAR** text, int* lenOut, int* caretOut, int* selEndOut) {
    IDispatch* app = WpsAppGet(WPSA_SHOW);
    ComPtr<IDispatch> win, sel, tr, par, all;
    BOOL ok = FALSE;
    long st = -1, ln = -1;

    if (!app) return FALSE;
    *text = NULL;
    *lenOut = 0;
    *caretOut = 0;
    if (selEndOut) *selEndOut = 0;

    win = ComPtr<IDispatch>(AdGetDisp(app, L"ActiveWindow"));
    if (!win) {
        WpsLog("ActiveWindow 取不到（演示没开文稿？）");
        goto done;
    }
    /* 演示的 Selection 挂在 Window 上（文字挂在 Application 上） */
    sel = ComPtr<IDispatch>(AdGetDisp(win.get(), L"Selection"));
    if (!sel) {
        WpsLog("Selection 取不到");
        goto done;
    }
    {
        long ty = -1;
        /* 只有文本编辑态（Type=3）才有 TextRange；选形状/未进编辑直接放弃，
           让上层走原生兜底 —— 这是常态而非错误，不打日志刷屏 */
        if (!AdGetLong(sel.get(), L"Type", &ty) || ty != WPS_SEL_TEXT) goto done;
    }
    tr = ComPtr<IDispatch>(AdGetDisp(sel.get(), L"TextRange"));
    if (!tr || !AdGetLong(tr.get(), L"Start", &st) || st < 1) {
        WpsLog("TextRange.Start 取不到");
        goto done;
    }
    if (!AdGetLong(tr.get(), L"Length", &ln) || ln < 0) {
        WpsLog("TextRange.Length 取不到");
        goto done;
    }
    par = ComPtr<IDispatch>(AdGetDisp(tr.get(), L"Parent"));        /* = TextFrame */
    if (!par) {
        WpsLog("TextRange.Parent 取不到");
        goto done;
    }
    /* 全文用 TextFrame 自己的 TextRange —— Selection.TextRange 只是选区，
       词算法需要整个形状的文本（同一坐标系） */
    all = ComPtr<IDispatch>(AdGetDisp(par.get(), L"TextRange"));
    if (!all) {
        WpsLog("TextFrame.TextRange 取不到");
        goto done;
    }
    {
        WCHAR* t = NULL;
        /* "Text" 的 GetIDsOfNames 查不到（DISPID_VALUE），AdId 里已回退 0 */
        if (!AdGetText(all.get(), L"Text", &t)) {
            WpsLog("TextFrame.TextRange.Text 取不到");
            goto done;
        }
        int len = (int)wcslen(t);
        if (len < WPS_DOC_MIN || len > WPS_TEXT_MAX) {
            WpsLog("文本框长度 %d 不适合本通道（范围 %d..%d）", len, WPS_DOC_MIN, WPS_TEXT_MAX);
            free(t);
            goto done;
        }
        int caret = (int)st - 1;            /* 实测 Start 是 1 基（见文件头） */
        int selEnd = caret + (int)ln;
        if (caret > len) caret = len;
        if (caret < 0) caret = 0;
        if (selEnd > len) selEnd = len;
        if (selEnd < caret) selEnd = caret;
        *text = t;
        *lenOut = len;
        *caretOut = caret;
        if (selEndOut) *selEndOut = selEnd;
        ok = TRUE;
        if (g_recording.load())
            CapLog("  WPP shape len=%d caret=%d selEnd=%d Start=%ld Length=%ld",
                   len, caret, selEnd, st, ln);
    }

done:
    return ok;
}

/* ---------- 文字：直接置位一次，并把结果分成三态（见 auto.h 的 WPS_SET_*）。
   分类依据是**回读的位置**，不是 Invoke 的 HRESULT —— provider（这里是 WPS）
   完全可能"返回成功但没动"。 ---------- */
static int WpsTextSetSelOnce(long start, long end) {
    IDispatch* app = WpsAppGet(WPSA_TEXT);
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

/* ---------- 演示：置位一次（Characters(lo+1, hi-lo).Select()，见文件头） ---------- */
static int WpsShowSetSelOnce(long a1, long a2) {
    IDispatch* app = WpsAppGet(WPSA_SHOW);
    if (!app) return WPS_SET_UNSURE;
    ComPtr<IDispatch> win(AdGetDisp(app, L"ActiveWindow"));
    if (!win) return WPS_SET_UNSURE;
    ComPtr<IDispatch> sel(AdGetDisp(win.get(), L"Selection"));
    if (!sel) return WPS_SET_UNSURE;
    {
        long ty = -1;
        /* 不在文本编辑态：置位无处可落，但也**不能**当"没动"补箭头 —— 补出的
           方向键会去移动被选中的形状。按"状态不明"处理。 */
        if (!AdGetLong(sel.get(), L"Type", &ty) || ty != WPS_SEL_TEXT)
            return WPS_SET_UNSURE;
    }
    ComPtr<IDispatch> tr(AdGetDisp(sel.get(), L"TextRange"));
    long preS = -1, preL = -1;
    BOOL preOK = tr && AdGetLong(tr.get(), L"Start", &preS) && AdGetLong(tr.get(), L"Length", &preL);

    long lo = (a1 < a2) ? a1 : a2;
    long hi = (a1 < a2) ? a2 : a1;
    /* 取全长：Selection.TextRange 只是选区，Characters 得在 TextFrame 的
       全长 TextRange 上取（lo/hi 是它坐标系里的 0 基偏移） */
    ComPtr<IDispatch> par(tr ? AdGetDisp(tr.get(), L"Parent") : NULL);
    ComPtr<IDispatch> all(par ? AdGetDisp(par.get(), L"TextRange") : NULL);
    if (!all) return WPS_SET_UNSURE;
    long alen = -1;
    if (!AdGetLong(all.get(), L"Length", &alen) || alen < 0) return WPS_SET_UNSURE;
    if (lo < 0 || hi > alen || hi < lo) return WPS_SET_UNSURE;   /* 读到的坐标已过期 */

    /* 折叠也走 Characters(k,0)：0 长度实测合法，k 可到 len+1（折叠到末尾） */
    ComPtr<IDispatch> sub;
    if (!AdCallDisp2(all.get(), L"Characters", lo + 1, hi - lo, sub.put()) || !sub)
        return WPS_SET_UNSURE;
    if (!AdCall0(sub.get(), L"Select")) return WPS_SET_UNSURE;

    /* 回读校验（0 基换算见文件头） */
    ComPtr<IDispatch> tr2(AdGetDisp(sel.get(), L"TextRange"));
    long postS = -1, postL = -1;
    BOOL postOK = tr2 && AdGetLong(tr2.get(), L"Start", &postS) &&
                  AdGetLong(tr2.get(), L"Length", &postL);
    if (postOK && postS - 1 == lo && postL == hi - lo) return WPS_SET_OK;
    if (postOK && preOK && postS == preS && postL == preL) return WPS_SET_NONE;
    return WPS_SET_UNSURE;
}

static void WpsAppDrop(int kind) {
    t_wpsApp[kind].reset();
}

/* 缓存可能失效（WPS 重启、关掉文档又开）→ 失败就丢掉重试一次。
   只在**曾经拿到过**缓存时重试：首次就没有实例的话再试一次没有意义，
   反而让每次按键都多付一遍 COM 往返。 */
static BOOL WpsComRead(int kind, WCHAR** text, int* lenOut, int* caretOut, int* selEndOut) {
    BOOL had = (t_wpsApp[kind] ? TRUE : FALSE);
    BOOL ok = (kind == WPSA_SHOW)
                  ? WpsShowReadOnce(text, lenOut, caretOut, selEndOut)
                  : WpsTextReadOnce(text, lenOut, caretOut, selEndOut);
    if (ok) return TRUE;
    WpsAppDrop(kind);
    if (!had) return FALSE;
    return (kind == WPSA_SHOW)
               ? WpsShowReadOnce(text, lenOut, caretOut, selEndOut)
               : WpsTextReadOnce(text, lenOut, caretOut, selEndOut);
}

static int WpsComSetSel(int kind, long start, long end) {
    BOOL had = (t_wpsApp[kind] ? TRUE : FALSE);
    int r = (kind == WPSA_SHOW) ? WpsShowSetSelOnce(start, end)
                                : WpsTextSetSelOnce(start, end);
    if (r == WPS_SET_OK) return r;
    WpsAppDrop(kind);
    if (!had) return r;
    return (kind == WPSA_SHOW) ? WpsShowSetSelOnce(start, end)
                               : WpsTextSetSelOnce(start, end);
}

/* ---------- 经降权桥（管理员运行时唯一通路） ----------

   桥协议在 {op,a,b} 基础上带 app（WPSA_*）：文字/演示是两个不同的对象模型，
   子进程得知道按哪套读/写。协议只在本程序的父子进程之间使用，同版本同发。 */
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

static BOOL WpsBrRead(int kind, WCHAR** text, int* lenOut, int* caretOut, int* selEndOut) {
    WpsReq req; memset(&req, 0, sizeof(req));
    req.op = WPS_OP_GET; req.app = kind;
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

static int WpsBrSetSel(int kind, long start, long end) {
    WpsReq req; memset(&req, 0, sizeof(req));
    req.op = WPS_OP_SET; req.app = kind; req.a = start; req.b = end;
    WpsResp hdr; memset(&hdr, 0, sizeof(hdr));
    if (!WpsBrCall(&req, &hdr, NULL, NULL)) return WPS_SET_UNSURE;
    if (hdr.status == WPS_ST_FAIL) return WPS_SET_UNSURE;
    return (int)hdr.status;                     /* 子进程直接回 WPS_SET_* 分类 */
}

/* 按运行权限选通路（不含"是不是 WPS"的判定，自检也用这一条） */
static BOOL WpsReadAny(int kind, WCHAR** text, int* lenOut, int* caretOut, int* selEndOut) {
    if (!IsRunElevated()) return WpsComRead(kind, text, lenOut, caretOut, selEndOut);
    return WpsBrRead(kind, text, lenOut, caretOut, selEndOut);
}

/* ---------- 对外的通道接口 ---------- */

BOOL WpsGetTextCaret(HWND focus, WCHAR** text, int* lenOut, int* caretOut, int* selEndOut) {
    if (!text || !lenOut || !caretOut) return FALSE;
    int kind = WpsFocusKindOf(focus);
    if (kind < 0) return FALSE;
    return WpsReadAny(kind, text, lenOut, caretOut, selEndOut);
}

int WpsSetSel(HWND focus, int start, int end) {
    int kind = WpsFocusKindOf(focus);
    /* 不是 WPS 焦点：本模块什么都没做，所以"可以安全退回箭头" */
    if (kind < 0) return WPS_SET_NONE;
    if (!IsRunElevated()) return WpsComSetSel(kind, start, end);
    return WpsBrSetSel(kind, start, end);
}

/* ---------- 桥的子进程侧 ---------- */

static BOOL WpsBridgeCall(const BYTE* req, DWORD reqLen, BYTE** resp, DWORD* respLen) {
    if (!req || reqLen < sizeof(WpsReq)) return FALSE;
    WpsReq r;
    memcpy(&r, req, sizeof(r));
    if (r.app != WPSA_TEXT && r.app != WPSA_SHOW) {
        WpsLog("收到未知 app=%ld", (long)r.app);
        return FALSE;
    }
    WpsResp h;
    memset(&h, 0, sizeof(h));

    if (r.op == WPS_OP_GET) {
        WCHAR* text = NULL;
        int len = 0, caret = 0, selEnd = 0;
        if (!WpsComRead((int)r.app, &text, &len, &caret, &selEnd)) {
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
        h.status = WpsComSetSel((int)r.app, r.a, r.b);   /* WPS_SET_* 直接透传给父进程 */
        long s = -1, e = -1;
        IDispatch* app = WpsAppGet((int)r.app);   /* 借来的（线程缓存），不包 ComPtr */
        if (app) {
            if (r.app == WPSA_SHOW) {
                ComPtr<IDispatch> win(AdGetDisp(app, L"ActiveWindow"));
                ComPtr<IDispatch> sel(win ? AdGetDisp(win.get(), L"Selection") : NULL);
                if (sel) {
                    long ln = -1;
                    AdGetLong(sel.get(), L"Start", &s);
                    if (AdGetLong(sel.get(), L"Length", &ln) && s >= 0) e = s + ln;
                }
            } else {
                ComPtr<IDispatch> sel(AdGetDisp(app, L"Selection"));
                if (sel) {
                    AdGetLong(sel.get(), L"Start", &s);
                    AdGetLong(sel.get(), L"End", &e);
                }
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

static void WpsComTeardown(void) {
    WpsAppDrop(WPSA_TEXT);
    WpsAppDrop(WPSA_SHOW);
}

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
