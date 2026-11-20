/* ================= 超级复制（Caps + C）：从光标下的控件取文本 =================
   机制照 Textify 1.10.4 的取词部分（GPL-3.0；来源与许可声明见根目录
   THIRD-PARTY-NOTICES.md 第 6 条）。核心思路不是模拟"选中 -> Ctrl+C"，而是
   **按屏幕坐标查无障碍树**，读元素暴露的 Name / Value / Description 三个语义
   字段。它因此与"能不能选中、有没有焦点、是不是只读 / 禁用"全都无关 ——
   只读编辑框、禁用的按钮、网页正文都能取。

   ★ 与上游最大的一处**语义差异**：**只取一个字段，且不爬父链**。
   上游把 Name / Value / Description 拼成一段文本、取不到就往上爬到第一个有
   名字的祖先。在浏览器里这两条都会出错：搜索框的 Name 是占位提示（"在此处
   输入你的搜索…"）、页面空白处会一路爬到 Document / 页面标题。实测症状分别
   是"复制出两行、第一行是提示"和"鼠标下明明没内容却复制出一串奇怪文字"。
   现在按元素类别选字段（输入框只取 Value、容器只认 Value、链接取 Name），
   取不到就如实失败 —— 与参考文档 §7.2 的字段映射表一致。

   为什么是坐标而不是焦点：Caps+C 是键盘触发，但**焦点与"用户正看着的那个
   东西"经常不是一回事**（按住 Caps 时焦点可能还留在上一个窗口，或者压根没有
   焦点控件）。指针才是"我要复制这一行"唯一不会撒谎的指向。
   ★ 由此也决定了：本模块**不复用** nav.c 的读取阶梯。那套读的是"焦点控件
   caret 所在的文档"（Edit / Scintilla / UIA TextPattern / IA2），拿不到
   "坐标下这个按钮的 Name"，两件事的输入不同，硬拼只会互相污染。

   与上游的四处**有意差异**（每一条都是上游真实存在的缺陷）：
     1. 上游把取词放在 UI 线程的 OnInitDialog 里，**没有任何超时**：目标卡死
        就整个程序冻结。这里每次跨进程查询都丢给一条一次性看门线程，最多等
        一个预算（TG_CHANNEL_MS），超时整条作废 —— 与 nav.c 的 ComRun 同一套
        取舍（线程可能永远卡在目标进程里，只能随进程退出回收）。
     2. 上游取不到文本时把占位串 "(no text could be retrieved)" 当文本，且在
        自动复制开启时会把**那句占位串**写进剪贴板。这里取不到就是失败，
        剪贴板一个字节都不动。
     3. 上游不重试。这里对"懒加载控件"（Excel 单元格 / WPF 节点 / 列表项）
        在同一坐标短间隔重查几次 —— 那类控件的文本要指针停留一会儿才进树。
     4. 上游写剪贴板只试一次、失败静默。这里重试 10×20ms，仍失败则明确报错
        并把全文写进日志（文本至少还能从日志里捞回来）。

   本文件只做"取词 + 清洗 + 剪贴板"，**不碰任何窗口**：预览浮层在 toast.c
   的独立线程里（主线程是钩子宿主，绝不能为画图分心；工作线程要做阻塞的
   跨进程查询，也不能兼管窗口消息）。 */

#include "cap.h"
#include "copy_text.h"

/* 本 TU 不用 initguid.h：CLSID_CUIAutomation / IID_IUIAutomation 的定义
   （存储）由 nav.c 那个 TU 提供，这里只要声明。两个 TU 都 include 会重复定义。 */
#include <oleacc.h>
#include <oleauto.h>
#include <uiautomationcore.h>
#include <uiautomationclient.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "oleacc.lib")

/* ---------- 时间预算 ----------
   总预算 600ms 是"按下 Caps+C 到看见浮层"的最坏等待，对应参考文档
   "触发到完成 < 1 秒"那条非功能指标。单通道 300ms：两个通道**各自**最多
   等这么久，剩余预算不足时按剩余算（见 TgRun）。 */
#define TG_TOTAL_BUDGET_MS 600
#define TG_CHANNEL_MS      300
#define TG_RETRY_GAP_MS    200   /* 懒加载重查间隔；也计入总预算 */
#define TG_TRIES_MAX       3

#define TG_DEPTH_MAX       24    /* 父链最多爬几层（防环 / 防病态树） */

/* 通道编号 */
#define TG_CH_NONE 0
#define TG_CH_UIA  1    /* UI Automation：新程序、Win11 任务栏支持更好       */
#define TG_CH_MSAA 2    /* MSAA (IAccessible)：老程序、部分 Delphi/Win32    */

/* 失败原因（浮层上的人话由 TgReason 给出） */
#define TG_ERR_NONE    0
#define TG_ERR_NOELEM  1   /* 光标下没有可访问元素（多半没对准文字）   */
#define TG_ERR_NOTEXT  2   /* 有元素，但它没暴露任何文本               */
#define TG_ERR_NOA11Y  3   /* 命中渲染器兜底壳 = 该程序没开无障碍       */
#define TG_ERR_DENIED  4   /* 目标进程权限更高（UIPI）                 */
#define TG_ERR_TIMEOUT 5   /* 跨进程查询超时（目标可能卡死）           */
#define TG_ERR_CLIP    6   /* 剪贴板被别的程序占着                     */
#define TG_ERR_EMPTY   7   /* 取到了文本，清洗后为空                   */
#define TG_ERR_NOINPUT 8   /* 命中的输入框是空的（只剩占位提示）        */

/* ---------- 采样结果 ---------- */
typedef struct {
    WCHAR* text;    /* malloc 的文本（调用方 free）；NULL = 没取到          */
    int    len;
    RECT   rc;      /* 产出这段文本的那个元素的包围盒（仲裁用）            */
    HWND   hwnd;    /* 元素所属窗口（取来源进程名用）                      */
    int    ch;      /* TG_CH_*                                            */
    BOOL   sawElem; /* 命中到了元素（哪怕没文字）—— 失败分类要靠它          */
    BOOL   shell;   /* 命中的是渲染器兜底壳 / 桌面外壳名（无效名）          */
    BOOL   denied;  /* 见到 E_ACCESSDENIED                                 */
    BOOL   emptyEdit; /* 命中了一个有占位提示、但内容为空的输入框          */
} TgSample;

/* ---------- 动态宽字符串 ----------
   只需要"追加一段 + 用 \r\n 分隔"，不引任何容器：本项目的配置里连
   std::vector<std::string> 都不能用（无 /EHsc，见 DEVELOPMENT.md）。 */
typedef struct {
    WCHAR* p;
    int    len;
    int    cap;
} TgBuf;

static void BufFree(TgBuf* b) {
    free(b->p);
    b->p = NULL;
    b->len = 0;
    b->cap = 0;
}

static BOOL BufReserve(TgBuf* b, int extra) {
    if (b->len + extra + 1 <= b->cap) return TRUE;
    int cap = b->cap ? b->cap : 64;
    while (cap < b->len + extra + 1) cap *= 2;
    WCHAR* np = (WCHAR*)realloc(b->p, (size_t)cap * sizeof(WCHAR));
    if (!np) return FALSE;
    b->p = np;
    b->cap = cap;
    return TRUE;
}

static void BufAppendN(TgBuf* b, const WCHAR* s, int n) {
    if (n <= 0 || !BufReserve(b, n)) return;
    memcpy(b->p + b->len, s, (size_t)n * sizeof(WCHAR));
    b->len += n;
    b->p[b->len] = 0;
}

/* 追加一个"字段块"。多字段之间用换行分隔 —— 与上游一致（它把 Name / Value /
   Description 拼成一段可读文本，而不是二选一）。 */
static void BufAddBlock(TgBuf* b, const WCHAR* s) {
    if (!s || !*s) return;
    if (b->len > 0) BufAppendN(b, L"\n", 1);
    BufAppendN(b, s, (int)wcslen(s));
}

/* ---------- 无效元素名（桌面外壳 / 渲染器兜底壳） ----------
   指针停在空白桌面或任务栏上时，无障碍树会给出这些名字。它们**有名字**，
   于是"值有效"，会把一串垃圾当成功复制走。拉黑之后它们才落到失败分类里。
   （参考文档管这叫 invalidNames，并明确：不拉黑的话，指针停在桌面上会
   "误判为成功"。） */
static const WCHAR* kInvalidNames[] = {
    L"NotificationsWindow",     /* 桌面通知层：最容易撞上的那个          */
    L"Program Manager",         /* 桌面窗口                              */
    L"Desktop Window",          /* 老系统的桌面壳（下面还按前缀匹配一次）  */
    L"Chrome Legacy Window",    /* Chromium 的渲染器兜底壳 = 无障碍没开    */
};

static BOOL TgIsInvalidName(const WCHAR* s) {
    if (!s || !*s) return FALSE;
    for (int i = 0; i < (int)(sizeof(kInvalidNames) / sizeof(kInvalidNames[0])); i++) {
        if (_wcsicmp(s, kInvalidNames[i]) == 0) return TRUE;
    }
    /* 带后缀的桌面壳（"Desktop Window 1" 之类） */
    if (_wcsnicmp(s, L"Desktop Window", 14) == 0) return TRUE;
    return FALSE;
}

/* ---------- BSTR -> 我们自己拥有的串 ----------
   为什么不直接对 BSTR 解引用：MSAA / UIA 的 SAL 标注让 /analyze 认为返回的
   BSTR 缓冲"可能未初始化"（C6001），而真正的守卫是 NULL 检查。先按
   SysStringLen 判长再 memcpy（与 nav.c 的 IA2 通道同一写法），顺手把所有权
   也理清楚：拿到的是一份我们负责 free 的副本。 */
static WCHAR* BstrDup(const BSTR b) {
    if (!b) return NULL;
    int n = (int)SysStringLen(b);
    if (n <= 0) return NULL;
    WCHAR* out = (WCHAR*)malloc(((size_t)n + 1) * sizeof(WCHAR));
    if (!out) return NULL;
    memcpy(out, b, (size_t)n * sizeof(WCHAR));
    out[n] = 0;
    return out;
}

/* ---------- 元素类别：只决定"取哪个字段" ----------
   这是本模块最容易搞错的一处，也是复制浏览器时"多带一句、少一个字"的根因。
   两类错误都来自"把 Name / Value / Description 全拼起来"：
     · 搜索框的 Name 是**占位提示**（"在此处输入你的搜索 — 输入时会显示搜索建议"），
       Value 才是用户输入的内容 —— 拼起来就是"提示 + 内容"两行；
     · 而指针停在页面空白处时，命中的是 Document / Pane 这类**容器**，它的 Name
       往往是页面标题或 aria-label，Value 是整篇正文。拼起来只会得到一串垃圾。
   所以按类别**只取一个字段**（参考文档 §7.2 的字段映射表就是这个意思：
   静态文本取 Name、输入框取 Value、链接取 Name）。 */
#define TGK_OTHER 0
#define TGK_EDIT  1   /* 可编辑文本（单行）：内容在 Value；Name 是占位提示，不能用 */
#define TGK_DOC   2   /* 文档/容器：只认 Value（多行编辑框也是 Document）            */
#define TGK_LINK  3   /* 超链接：可见文字在 Name（Value 常是 URL）                   */

static int TgKindFromRole(LONG role) {
    switch (role) {
    case ROLE_SYSTEM_TEXT:
        return TGK_EDIT;
    case ROLE_SYSTEM_DOCUMENT:
    case ROLE_SYSTEM_WINDOW:
    case ROLE_SYSTEM_PANE:
    case ROLE_SYSTEM_CLIENT:
    case ROLE_SYSTEM_GROUPING:
        return TGK_DOC;
    case ROLE_SYSTEM_LINK:
        return TGK_LINK;
    default:
        return TGK_OTHER;
    }
}

static int TgKindFromControlType(CONTROLTYPEID ct) {
    switch (ct) {
    case UIA_EditControlTypeId:
        return TGK_EDIT;
    case UIA_DocumentControlTypeId:
    case UIA_WindowControlTypeId:
    case UIA_PaneControlTypeId:
    case UIA_GroupControlTypeId:
    case UIA_CustomControlTypeId:
        return TGK_DOC;
    case UIA_HyperlinkControlTypeId:
        return TGK_LINK;
    default:
        return TGK_OTHER;
    }
}

/* 按类别挑出唯一要用的那个字段。返回 NULL = 这个元素没有可复制的内容。

   ★ 容器为什么是"只认 Value"而不是"一律不取"：
     记事本这类**多行编辑框**在 UIA 里就是 `Document`（不是 Edit），正文在
     `Value` 里；一刀切会把它们全砍掉。而浏览器**页面**的 `Document` 恰好没有
     `Value`（正文只暴露在 TextPattern 里，我们不读），它的 `Name` 才是页面标题
     —— 于是"只认 Value"同时满足两件事：真编辑器照常取到正文，网页空白处
     老老实实失败。 */
static const WCHAR* TgPickField(int kind, const WCHAR* name, const WCHAR* value,
                                const WCHAR* desc) {
    switch (kind) {
    case TGK_DOC:
        return (value && *value) ? value : NULL;   /* Name 是页面标题 / aria-label，不取 */
    case TGK_EDIT:
        return (value && *value) ? value : NULL;   /* Name 是占位提示，不取 */
    case TGK_LINK:
        if (name && *name) return name;
        return (value && *value) ? value : NULL;
    default:
        if (name  && *name)  return name;
        if (value && *value) return value;
        if (desc  && *desc)  return desc;
        return NULL;
    }
}

/* ================= MSAA 通道 ================= */

static void MsaaSample(POINT pt, TgSample* out) {
    IAccessible* acc = NULL;
    VARIANT child;
    VariantInit(&child);
    HRESULT hr = AccessibleObjectFromPoint(pt, &acc, &child);
    if (hr == E_ACCESSDENIED) out->denied = TRUE;
    if (FAILED(hr) || !acc) {
        VariantClear(&child);
        return;
    }

    /* Chromium 的已知怪癖：第一次查询常返回上层壳、第二次才是真正的元素。
       上游为此连查两次并留了注释（"Chromium has a bug in which the correct
       element is sometimes returned only after a second query"），照做。 */
    {
        IAccessible* acc2 = NULL;
        VARIANT child2;
        VariantInit(&child2);
        if (SUCCEEDED(AccessibleObjectFromPoint(pt, &acc2, &child2)) && acc2) {
            acc->lpVtbl->Release(acc);
            VariantClear(&child);
            acc = acc2;
            child = child2;
        } else {
            if (acc2) acc2->lpVtbl->Release(acc2);
            VariantClear(&child2);
        }
    }
    out->sawElem = TRUE;

    /* 元素所属窗口：只用来取"来源程序"的名字，与取词无关。 */
    HWND hwnd = NULL;
    if (SUCCEEDED(WindowFromAccessibleObject(acc, &hwnd)) && hwnd)
        out->hwnd = hwnd;

    TgBuf buf;
    memset(&buf, 0, sizeof(buf));

    for (int depth = 0; depth < TG_DEPTH_MAX; depth++) {
        /* 标题栏的 description 是窗口标题的重复，取出来只会得到
           "标题 + 标题"（上游为同一个理由跳过 ROLE_SYSTEM_TITLEBAR）。 */
        VARIANT role;
        VariantInit(&role);
        long rolev = 0;
        if (SUCCEEDED(acc->lpVtbl->get_accRole(acc, child, &role)) && role.vt == VT_I4)
            rolev = role.lVal;
        VariantClear(&role);

        BSTR bsName = NULL, bsValue = NULL, bsDesc = NULL;
        HRESULT hrName = acc->lpVtbl->get_accName(acc, child, &bsName);
        HRESULT hrValue = acc->lpVtbl->get_accValue(acc, child, &bsValue);
        HRESULT hrDesc = S_FALSE;
        if (rolev != ROLE_SYSTEM_TITLEBAR)
            hrDesc = acc->lpVtbl->get_accDescription(acc, child, &bsDesc);

        WCHAR* pName  = SUCCEEDED(hrName)  ? BstrDup(bsName)  : NULL;
        WCHAR* pValue = SUCCEEDED(hrValue) ? BstrDup(bsValue) : NULL;
        WCHAR* pDesc  = SUCCEEDED(hrDesc)  ? BstrDup(bsDesc)  : NULL;

        SysFreeString(bsName);
        SysFreeString(bsValue);
        SysFreeString(bsDesc);

        int kind = TgKindFromRole(rolev);
        /* 无效名判据必须落在**元素名**上：桌面外壳 / 渲染器兜底壳的名字挂在这类
           容器元素上，而容器按设计只取 Value、Name 这一层就被丢掉了。原先拿
           取出的文本去比对黑名单，于是这四个名字永远匹配不上，指针停在桌面时
           被报成"控件没暴露文字"而不是"无障碍没开"。 */
        if (TgIsInvalidName(pName)) {
            out->shell = TRUE;
            free(pName);
            free(pValue);
            free(pDesc);
            break;
        }
        const WCHAR* pick = TgPickField(kind, pName, pValue, pDesc);
        if (pick) BufAddBlock(&buf, pick);
        /* 空输入框单独算一类：它"有元素、有占位提示，但没有内容"，
          报"没有可读取的文字"会让人以为功能坏了。 */
        if (!pick && kind == TGK_EDIT && pName && *pName)
            out->emptyEdit = TRUE;

        free(pName);
        free(pValue);
        free(pDesc);

        if (buf.len > 0) break;

        /* **不往上爬父链**。上游 Textify 会一直爬到"第一个有名字的祖先"，
           但在浏览器里那条路的终点是页面标题 / aria-label —— 指针停在空白处
           也会"成功"复制出一串与光标位置无关的文字（实测踩到）。
           唯一还要走一步的是"命中的是子项"：子项没有自己的元素对象，只能
           改读它所在的容器，这不是爬父链。 */
        if (child.vt == VT_I4 && child.lVal != CHILDID_SELF) {
            child.lVal = CHILDID_SELF;
            continue;
        }
        break;
    }

    if (buf.len > 0) {
        long x = 0, y = 0, w = 0, h = 0;
        if (SUCCEEDED(acc->lpVtbl->accLocation(acc, &x, &y, &w, &h, child)))
            SetRect(&out->rc, x, y, x + w, y + h);
        out->text = buf.p;
        out->len  = buf.len;
        out->ch   = TG_CH_MSAA;
    } else {
        BufFree(&buf);
    }
    acc->lpVtbl->Release(acc);
}

/* ================= UIA 通道 ================= */

/* 容器角色：命中到这些角色基本等于"停在壳上"（浏览器的 document、资源管理器
   的 pane、自动化框架的 host）。再查一次往往就落到真正的内容元素上 ——
   参考文档明确要求这一步，否则网页正文与资源管理器会大量取空。 */
static BOOL TgIsContainer(CONTROLTYPEID ct) {
    return ct == UIA_WindowControlTypeId || ct == UIA_PaneControlTypeId ||
           ct == UIA_DocumentControlTypeId || ct == UIA_EditControlTypeId;
}

static void UiaSample(POINT pt, TgSample* out) {
    IUIAutomation* ua = NULL;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER,
                                IID_IUIAutomation, (void**)&ua)) || !ua)
        return;

    IUIAutomationElement* el = NULL;
    HRESULT hr = IUIAutomation_ElementFromPoint(ua, pt, &el);
    if (hr == E_ACCESSDENIED) out->denied = TRUE;
    if (FAILED(hr) || !el) {
        IUIAutomation_Release(ua);
        return;
    }

    /* 第二次查询（Chromium 怪癖，理由见 MSAA 通道那段） */
    {
        IUIAutomationElement* el2 = NULL;
        if (SUCCEEDED(IUIAutomation_ElementFromPoint(ua, pt, &el2)) && el2) {
            IUIAutomationElement_Release(el);
            el = el2;
        } else if (el2) {
            IUIAutomationElement_Release(el2);
        }
    }

    /* 容器重采 */
    {
        CONTROLTYPEID ct = 0;
        if (SUCCEEDED(IUIAutomationElement_get_CurrentControlType(el, &ct)) && TgIsContainer(ct)) {
            IUIAutomationElement* el3 = NULL;
            if (SUCCEEDED(IUIAutomation_ElementFromPoint(ua, pt, &el3)) && el3) {
                if (el3 != el) {
                    IUIAutomationElement_Release(el);
                    el = el3;
                } else {
                    IUIAutomationElement_Release(el3);
                }
            } else if (el3) {
                IUIAutomationElement_Release(el3);
            }
        }
    }
    out->sawElem = TRUE;

    /* 元素自己的 HWND 经常是空（纯文本节点没有窗口），往上找第一个有窗口的。
       ★ 这一段**只找窗口**，不取任何文字 —— 与"不爬父链取文本"不矛盾。
       上限放到 32 层：浏览器一个带大量 iframe / shadow DOM 的文本节点，到顶层
       窗口的层数可以远超 8 层，够不着就会显示"未知程序"。 */
    HWND hwnd = NULL;
    {
        IUIAutomationElement* cur = el;
        IUIAutomationElement_AddRef(cur);
        for (int i = 0; i < 32 && cur && !hwnd; i++) {
            UIA_HWND uh = 0;
            if (SUCCEEDED(IUIAutomationElement_get_CurrentNativeWindowHandle(cur, &uh)) && uh) {
                hwnd = (HWND)uh;
                break;
            }
            IUIAutomationTreeWalker* tmp = NULL;
            IUIAutomationElement* up = NULL;
            if (SUCCEEDED(IUIAutomation_get_RawViewWalker(ua, &tmp)) && tmp)
                IUIAutomationTreeWalker_GetParentElement(tmp, cur, &up);
            if (tmp) IUIAutomationTreeWalker_Release(tmp);
            IUIAutomationElement_Release(cur);
            cur = up;
        }
        if (cur) IUIAutomationElement_Release(cur);
    }
    out->hwnd = hwnd;

    TgBuf buf;
    memset(&buf, 0, sizeof(buf));

    {
        CONTROLTYPEID ct = 0;
        IUIAutomationElement_get_CurrentControlType(el, &ct);
        int kind = TgKindFromControlType(ct);

        BSTR bsName = NULL;
        HRESULT hrName = IUIAutomationElement_get_CurrentName(el, &bsName);
        WCHAR* pName = SUCCEEDED(hrName) ? BstrDup(bsName) : NULL;
        SysFreeString(bsName);

        /* 只读 Name 与 Value：不碰 TextPattern —— 我们要的是"这个元素的语义
           文本"，不是"它所在文档的正文"（后者正是空白处取到整页的根源）。 */
        VARIANT v;
        VariantInit(&v);
        IUIAutomationElement_GetCurrentPropertyValue(el, UIA_ValueValuePropertyId, &v);
        WCHAR* pValue = (v.vt == VT_BSTR) ? BstrDup(v.bstrVal) : NULL;
        VariantClear(&v);

        /* 无效名判据见 MSAA 通道同一处注释：桌面 / 渲染器外壳的名字在 Name 上，
           而容器元素按设计不取 Name —— 这里必须在选字段之前判。 */
        if (TgIsInvalidName(pName)) {
            out->shell = TRUE;
            free(pName);
            free(pValue);
            pName = NULL;
            pValue = NULL;
        } else {
            const WCHAR* pick = TgPickField(kind, pName, pValue, NULL);
            if (pick) BufAddBlock(&buf, pick);
            if (!pick && kind == TGK_EDIT && pName && *pName)
                out->emptyEdit = TRUE;
        }

        free(pName);
        free(pValue);
    }

    if (buf.len > 0) {
        RECT rc;
        SetRect(&rc, 0, 0, 0, 0);
        if (SUCCEEDED(IUIAutomationElement_get_CurrentBoundingRectangle(el, &rc)))
            out->rc = rc;
        out->text = buf.p;
        out->len  = buf.len;
        out->ch   = TG_CH_UIA;
    } else {
        BufFree(&buf);
    }

    IUIAutomationElement_Release(el);
    IUIAutomation_Release(ua);
}

/* ================= 限时执行器 =================
   一次查询 = 一条一次性线程 + 一个参数块。理由见文件头第 1 条差异，以及
   nav.c 里 ComRun 的完整取舍（为什么不常驻通道线程）。超时则线程与参数块
   一并作废：它可能永远卡在目标进程里，只能随进程退出回收。 */

typedef struct {
    POINT    pt;
    int      which;
    TgSample out;
} TgJob;

static DWORD WINAPI TgWorker(LPVOID p) {
    TgJob* j = (TgJob*)p;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (j->which == TG_CH_UIA) UiaSample(j->pt, &j->out);
    else                       MsaaSample(j->pt, &j->out);
    if (SUCCEEDED(hr)) CoUninitialize();
    return 0;
}

/* TRUE = 在 ms 内跑完了（结果在 *out）；FALSE = 超时或压根起不了线程。 */
static BOOL TgRun(int which, POINT pt, DWORD ms, TgSample* out) {
    if (ms < 50) return FALSE;
    TgJob* j = (TgJob*)calloc(1, sizeof(TgJob));
    if (!j) return FALSE;
    j->pt = pt;
    j->which = which;
    HANDLE th = CreateThread(NULL, 0, TgWorker, j, 0, NULL);
    if (!th) {
        free(j);
        return FALSE;
    }
    DWORD w = WaitForSingleObject(th, ms);
    CloseHandle(th);
    if (w != WAIT_OBJECT_0) return FALSE;   /* 超时：j 有意泄漏，见上 */
    *out = j->out;
    free(j);
    return TRUE;
}

/* ================= 死区 / 仲裁 ================= */

/* msaa 的盒子是否**严格**落在 uia 的盒子里（更内层）。完全相等不算 ——
   那是同一个元素，取 UIA 的（它对文本字段更完整）。上游同判据。 */
static BOOL TgStrictlyInside(const RECT* inner, const RECT* outer) {
    if (inner->left >= inner->right || inner->top >= inner->bottom) return FALSE;
    if (outer->left >= outer->right || outer->top >= outer->bottom) return FALSE;
    BOOL inside = inner->left >= outer->left && inner->top >= outer->top &&
                  inner->right <= outer->right && inner->bottom <= outer->bottom;
    BOOL same = inner->left == outer->left && inner->top == outer->top &&
                inner->right == outer->right && inner->bottom == outer->bottom;
    return inside && !same;
}

/* ================= 剪贴板 =================
   重试 10×20ms：剪贴板是全局单例，另一个程序正在用它时 OpenClipboard 会
   失败，而这在"刚复制过东西"的时序里很常见（参考文档同参数）。
   成功时内存所有权归系统，**不要** GlobalFree。

   ★ 两处顺序上的讲究：
     1. **先分配、后 EmptyClipboard**。反过来的话，分配失败时用户原来的剪贴板
        内容已经被清掉，而浮层报的是"剪贴板被占用" —— 既丢了数据又指错了原因。
     2. 容量按 **2*len+1** 算，不是 len+1：写出去的是 CRLF，每个 \n 会多占一个
        WCHAR。按 len+1 分配时，任何含换行的文本（多行代码、几行日志）都会把
        堆块写越界 —— 越界长度正好等于换行数 + 1。 */
static BOOL TgClipWrite(const WCHAR* text, int len) {
    if (!text || len <= 0) return FALSE;

    /* 先备好要交付的块：CRLF 展开 + 结尾 0，最坏 2*len+1 个 WCHAR */
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, ((size_t)len * 2 + 1) * sizeof(WCHAR));
    if (!h) return FALSE;
    WCHAR* p = (WCHAR*)GlobalLock(h);
    if (!p) { GlobalFree(h); return FALSE; }
    int o = 0;
    for (int i = 0; i < len; i++) {
        if (text[i] == L'\n') p[o++] = L'\r';   /* 剪贴板约定是 CRLF */
        p[o++] = text[i];
    }
    p[o] = 0;
    GlobalUnlock(h);

    int tries = 0;
    while (tries < 10) {
        if (OpenClipboard(NULL)) break;
        Sleep(20);
        tries++;
    }
    if (tries >= 10) { GlobalFree(h); return FALSE; }

    BOOL ok = FALSE;
    if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, h))
        ok = TRUE;                              /* 成功时所有权归系统 */
    if (!ok) GlobalFree(h);
    CloseClipboard();
    return ok;
}

/* ================= 失败原因 =================
   全部是**可操作**的人话，不附通道名与错误码：普通用户看不懂 UIA/MSAA，
   也看不懂 0x80070005，而这两样只对写本程序的人有用（它们进日志）。 */
static const WCHAR* TgReason(int err) {
    switch (err) {
    case TG_ERR_NOELEM:
        return L"光标下没有可识别的控件，把指针对准要复制的文字再按一次";
    case TG_ERR_NOTEXT:
        return L"这个控件没有暴露可读取的文字（自绘界面常见）";
    case TG_ERR_NOA11Y:
        return L"该程序未开启无障碍支持，取不到里面的文字";
    case TG_ERR_DENIED:
        return L"目标程序权限更高，需以管理员身份运行 CapsEnhance 才能读取";
    case TG_ERR_TIMEOUT:
        return L"目标程序没有响应，这次取词已放弃";
    case TG_ERR_CLIP:
        return L"剪贴板被其他程序占用，文本已记入日志";
    case TG_ERR_EMPTY:
        return L"取到的文本是空的";
    case TG_ERR_NOINPUT:
        return L"这个输入框是空的，没有可复制的文字";
    default:
        return L"没有取到文本";
    }
}

/* ================= 入口 ================= */

void TextGrabRun(POINT pt) {
    /* 采样前先把浮层藏掉：它就挂在屏幕下方居中，光标恰好停在它上面时
       （例如复制任务栏或底部工具栏上的字），无障碍命中测试会先命中
       **我们自己的窗口**，取回一堆提示文字。这是"浮层会污染取样"那条坑。 */
    ToastHideNow();

    ULONGLONG deadline = GetTickCount64() + TG_TOTAL_BUDGET_MS;
    TgSample best;
    memset(&best, 0, sizeof(best));
    BOOL timedOut = FALSE;

    for (int attempt = 0; attempt < TG_TRIES_MAX; attempt++) {
        ULONGLONG now = GetTickCount64();
        if (now >= deadline) { timedOut = TRUE; break; }

        DWORD left = (DWORD)(deadline - now);
        DWORD msU = left < TG_CHANNEL_MS ? left : TG_CHANNEL_MS;

        TgSample uia;
        memset(&uia, 0, sizeof(uia));
        BOOL okU = TgRun(TG_CH_UIA, pt, msU, &uia);
        if (!okU) timedOut = TRUE;

        now = GetTickCount64();
        TgSample msaa;
        memset(&msaa, 0, sizeof(msaa));
        BOOL okM = FALSE;
        if (now < deadline) {
            DWORD msM = (DWORD)(deadline - now);
            if (msM > TG_CHANNEL_MS) msM = TG_CHANNEL_MS;
            okM = TgRun(TG_CH_MSAA, pt, msM, &msaa);
            if (!okM) timedOut = TRUE;
        }

        /* 分类标记：只要任一路见到了元素/拒绝，就算"见到了" */
        if (okU && uia.sawElem)  best.sawElem = TRUE;
        if (okM && msaa.sawElem) best.sawElem = TRUE;
        if (okU && uia.denied)   best.denied = TRUE;
        if (okM && msaa.denied)  best.denied = TRUE;
        if (okU && uia.emptyEdit)  best.emptyEdit = TRUE;
        if (okM && msaa.emptyEdit) best.emptyEdit = TRUE;
        /* 无效名（桌面外壳 / 渲染器兜底壳）由各通道在选字段之前判定并置位，
           这里只做汇总 —— 判据必须落在元素名上，见 MsaaSample 的注释。 */
        if (okU && uia.shell)  best.shell = TRUE;
        if (okM && msaa.shell) best.shell = TRUE;

        /* 仲裁：两路都有文本时取包围盒**严格更小**（更内层）的那个 */
        TgSample* pick = NULL;
        if (okU && uia.text)  pick = &uia;
        if (okM && msaa.text) {
            if (!pick) pick = &msaa;
            else if (TgStrictlyInside(&msaa.rc, &uia.rc)) pick = &msaa;
        }

        if (pick) {
            free(best.text);
            best.text = pick->text;
            best.len  = pick->len;
            best.rc   = pick->rc;
            best.hwnd = pick->hwnd;
            best.ch   = pick->ch;
            pick->text = NULL;          /* 所有权转移，别在下面被 free 两次 */
        }

        /* 没被选中的文本要在这里放掉 */
        free(uia.text);
        free(msaa.text);

        if (best.text) break;
        if (GetTickCount64() >= deadline) { timedOut = TRUE; break; }
        /* 懒加载控件的重查间隔；也计入总预算 */
        DWORD gap = (DWORD)(deadline - GetTickCount64());
        Sleep(gap < TG_RETRY_GAP_MS ? gap : TG_RETRY_GAP_MS);
    }

    if (g_recording.load()) {
        CapLog("  [复制] pt=(%ld,%ld) 结果=%s 通道=%d 字符=%d 超时=%d 元素=%d",
               (long)pt.x, (long)pt.y, best.text ? "有文本" : "无文本",
               best.ch, best.len, timedOut ? 1 : 0, best.sawElem ? 1 : 0);
    }

    /* ---- 失败 ---- */
    if (!best.text) {
        int err;
        if (best.denied)              err = TG_ERR_DENIED;
        else if (timedOut && !best.sawElem) err = TG_ERR_TIMEOUT;
        else if (best.shell)          err = TG_ERR_NOA11Y;
        else if (!best.sawElem)       err = TG_ERR_NOELEM;
        else if (best.emptyEdit)      err = TG_ERR_NOINPUT;
        else                          err = TG_ERR_NOTEXT;
        if (g_recording.load()) CapLog("  [复制] 失败：%d", err);
        ToastShow(TRUE, L"复制失败", NULL, TgReason(err));
        return;
    }

    /* ---- 清洗（纯函数在 copy_text.c，便于单测） ---- */
    WCHAR* clean = (WCHAR*)malloc(((size_t)best.len + 1) * sizeof(WCHAR));
    int cleanLen = 0;
    if (clean)
        cleanLen = CopyClean(best.text, best.len, CfgBool(CFG_COPY_STRIP_TAIL),
                             clean, best.len + 1);
    free(best.text);
    best.text = NULL;
    if (!clean || cleanLen == 0) {
        free(clean);
        ToastShow(TRUE, L"复制失败", NULL, TgReason(TG_ERR_EMPTY));
        return;
    }

    /* ---- 写剪贴板 ---- */
    if (!TgClipWrite(clean, cleanLen)) {
        /* 全文进日志（截断到 4000 字，避免一行日志几十 MB）：浮层是纯只读的，
           写不进剪贴板时这里就是用户唯一还能把文本捞回来的地方 —— 所以这条
           **不看记录模式**，否则浮层那句"文本已记入日志"就是假话。 */
        int cap = cleanLen > 4000 ? 4000 : cleanLen;
        CapLog("  [复制] 剪贴板写入失败，全文如下（%d 字%s）：", cleanLen,
               cap < cleanLen ? "，已截断" : "");
        CapLog("%.*ls", cap, clean);
        free(clean);
        ToastShow(TRUE, L"复制失败", NULL, TgReason(TG_ERR_CLIP));
        return;
    }

    /* ---- 成功：组装标题行 ---- */
    WCHAR proc[128];
    if (!best.hwnd) {
        /* 兜底：元素自身与祖先都给不出 HWND（部分纯文本节点如此），就用光标
           下那个顶层窗口 —— 标题行的"来源程序"只要对得上就行，不必是元素本体。 */
        best.hwnd = GetAncestor(WindowFromPoint(pt), GA_ROOT);
    }
    if (!best.hwnd || !ConfigGetProcessName(best.hwnd, proc, 128))
        wcscpy_s(proc, 128, L"未知程序");

    WCHAR head[256];
    WCHAR meta[128];
    meta[0] = 0;
    if (CfgBool(CFG_COPY_SHOW_COUNT)) {
        /* 字数按**码点**数（代理对算一个字），不能用 cleanLen —— 那是 UTF-16
           码元数，𰻝 / emoji 会被算成两个字。
           单行时不写"1 行"：那是冗余信息，只有真多行时行数才有意义。 */
        int chars = CopyCountChars(clean, cleanLen);
        int lines = CopyCountLines(clean, cleanLen);
        if (lines > 1)
            _snwprintf_s(meta, _countof(meta), _TRUNCATE, L"%d 字 · %d 行", chars, lines);
        else
            _snwprintf_s(meta, _countof(meta), _TRUNCATE, L"%d 字", chars);
    }
    _snwprintf_s(head, _countof(head), _TRUNCATE, L"%s", proc);

    if (g_recording.load())
        CapLog("  [复制] 成功 %d 字 %d 行 -> 剪贴板，原文前 60 字：%.*ls",
               CopyCountChars(clean, cleanLen), CopyCountLines(clean, cleanLen),
               60, clean);

    ToastShow(FALSE, head, meta, clean);
    free(clean);
}
