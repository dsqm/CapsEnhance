#include "cap.h"
#include "auto.h"

#define COBJMACROS   /* 启用 UIA 的 C 风格 *_宏接口 */
#include <initguid.h>  /* 在本 TU 内为 UIA 的 CLSID/IID 提供存储定义 */

#include <oleauto.h>
#include <oleacc.h>
#include <uiautomationcore.h>
#include <uiautomationclient.h>
#include <ole2.h>
#include <tlhelp32.h>   /* 降权桥：枚举进程找 explorer.exe */
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "oleacc.lib")
#pragma comment(lib, "advapi32.lib")   /* OpenProcessToken / DuplicateTokenEx */

/* 能直接设光标的通道（经典 Edit / Scintilla）瞬跳的最少步数：小于该值用
   真实箭头（行为一致），等于或大于则一步置到目标(瞬跳)，避免远距离逐格
   发送方向键拖慢。UIA/IA2 通道无法直接设光标，不受此值影响。 */
#define DIRECT_JUMP_MIN 6

/* ============ IAccessibleText（IAccessible2）手工声明 ============
   SDK 不带 IA2 头文件，这里按官方 AccessibleText.idl 手工声明 vtable。
   IID: {24FD2FFB-3AAD-4a08-8335-A3AD89C0FB4B}
   方法槽位（IUnknown 后依次）：
     addSelection, attributes, caretOffset, characterExtents, nSelections,
     offsetAtPoint, selection, text, textAtOffset, ...
   仅声明用到的槽位，其余以 void* 占位保持布局。 */
typedef struct IAccessibleText IAccessibleText;
typedef struct IAccessibleTextVtbl {
    /* IUnknown */
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(IAccessibleText*, REFIID, void**);
    ULONG   (STDMETHODCALLTYPE *AddRef)(IAccessibleText*);
    ULONG   (STDMETHODCALLTYPE *Release)(IAccessibleText*);
    void* slot3_addSelection;        /* addSelection(start,end) */
    void* slot4_attributes;         /* attributes(offset,...) */
    HRESULT (STDMETHODCALLTYPE *caretOffset)(IAccessibleText*, long*);
    void* slot6_characterExtents;
    void* slot7_nSelections;
    void* slot8_offsetAtPoint;
    void* slot9_selection;
    HRESULT (STDMETHODCALLTYPE *text)(IAccessibleText*, long start, long end, BSTR*);
    void* slot11_textBeforeOffset;   /* [propget] textBeforeOffset(...) */
    void* slot12_textAfterOffset;    /* [propget] textAfterOffset(...) */
    void* slot13_textAtOffset;       /* [propget] textAtOffset(...) */
    void* slot14_removeSelection;    /* removeSelection(index) */
    /* 槽 15/16：直接置位。官方 AccessibleText.idl：
       HRESULT setCaretOffset([in] long offset);
       HRESULT setSelection([in] long selectionIndex, [in] long startOffset, [in] long endOffset);
       setCaretOffset 不支持时实现返回 E_FAIL。 */
    HRESULT (STDMETHODCALLTYPE *setCaretOffset)(IAccessibleText*, long offset);
    HRESULT (STDMETHODCALLTYPE *setSelection)(IAccessibleText*, long index, long start, long end);
} IAccessibleTextVtbl;
struct IAccessibleText { const IAccessibleTextVtbl* lpVtbl; };

static const IID kIID_IAccessibleText =
    {0x24FD2FFB, 0x3AAD, 0x4a08, {0x83,0x35,0xA3,0xAD,0x89,0xC0,0xFB,0x4B}};


/* ---- 一次按词跳转的同步 IPC 预算 ----
   NavJump 会串行发多条跨进程消息（Edit 通道 3 条、Sci 通道最多 6 条），每条
   各自 300ms 超时。目标窗口卡住时这些超时是**相加**的：最坏能叠到 1.8s，
   用户看到的是"按一下键整个输入粘住"。
   现在整个 NavJump 共用一个预算：花完就没了，后续 IPC 直接失败 —— 于是
   NavJump 走"读不到文本"的正常分支，退回原生 Ctrl+方向兜底，而不是卡在那里。
   预算是**线程局部**的：NavJump 通常跑在 act.c 的工作线程上，但同步兜底
   路径（ActPost 在 !s_init 时）也可能在主线程，两条路不能互相干扰。 */
#define NAV_IPC_TIMEOUT_MS  300   /* 单条上限 */
#define NAV_IPC_BUDGET_MS   600   /* 一次 NavJump 的总预算 */
static __declspec(thread) ULONGLONG t_ipcDeadline = 0;

static void NavIpcBudgetBegin(void) {
    t_ipcDeadline = GetTickCount64() + NAV_IPC_BUDGET_MS;
}
static void NavIpcBudgetEnd(void) { t_ipcDeadline = 0; }
static DWORD NavIpcTimeout(void) {
    if (!t_ipcDeadline) return NAV_IPC_TIMEOUT_MS;   /* 未开预算（NavProbe 等） */
    ULONGLONG now = GetTickCount64();
    if (now >= t_ipcDeadline) return 0;              /* 预算耗尽：IPC 直接放弃 */
    ULONGLONG left = t_ipcDeadline - now;
    return (left > NAV_IPC_TIMEOUT_MS) ? NAV_IPC_TIMEOUT_MS : (DWORD)left;
}

/* ---- 跨进程发消息：取窗口过程的"真实返回值" ----
   坑：SendMessageTimeout 自己的返回值只表示"有没有发成功(非0)"，窗口过程的
   返回值写进最后一个参数 lpdwResult。之前所有调用都传 NULL，于是拿到的恒为 1
   —— 表现为 WM_GETTEXTLENGTH 恒等于 1、读到的文本恒被截成一个字符，
   词边界算法只能在"长度 1 的文本"上算，自然跳不动。 */
static BOOL SendMsg(HWND hwnd, UINT msg, WPARAM w, LPARAM l, LONGLONG* out) {
    DWORD ms = NavIpcTimeout();
    if (ms == 0) return FALSE;      /* 本次按词跳转的预算已用完 */
    DWORD_PTR r = 0;
    if (!SendMessageTimeoutW(hwnd, msg, w, l,
                             SMTO_ABORTIFHUNG | SMTO_BLOCK, ms, &r))
        return FALSE;
    if (out) *out = (LONGLONG)(LONG_PTR)r;
    return TRUE;
}

/* 经典 Edit 控件直接置光标（或选区）位置。若可成功则无需发送箭头，瞬跳。
   无 Shift：光标瞬移到 target；有 Shift：从当前光标(anchor)选到 target。 */
/* 经典 Edit 直接置选区：EM_SETSEL(wantStart, wantEnd)，参数有序。
   注意 EM_SETSEL 没有 anchor 概念（anchor 落在 start），Shift 位移时
   由调用方按"向左锚右端 / 向右锚左端"给出有序期望（见 NavJump）。 */
static BOOL ClassicSetSel(HWND hwnd, int wantStart, int wantEnd) {
    return SendMsg(hwnd, EM_SETSEL, (WPARAM)wantStart, (LPARAM)wantEnd, NULL);
}

/* ================= Scintilla 通道（Notepad3 / Notepad++ / SciTE ...） =================
   Scintilla 不暴露 UIA/IA2 文本，但对窗口消息很配合：
     SCI_GETTEXTLENGTH(2183)                 -> 文档字节数
     SCI_GETTEXT(2182)                       -> 把文档字节写进缓冲区
     SCI_GETCURRENTPOS(2008)/GETANCHOR(2009) -> 光标/锚点（文档字节偏移）
     SCI_SETSEL(2160)                        -> (caret, anchor) 直接置光标
     SCI_SCROLLCARET(2169)                   -> 滚动到光标
   两个必须注意的点：
   · SCI_GETTEXT 的缓冲区指针属于对方进程地址空间，跨进程直传本地指针会读崩
     对方（v1 的"SCI 直读"就栽在这）—— 必须先在目标进程 VirtualAllocEx。
   · 文档位置是"字节"，而 WM_GETTEXT 给出的是 UTF-16 文本、EM_GETSEL 给出的是
     字节位置，混用会在中文（UTF-8 下 3 字节/字）处错位。所以整条通道统一在
     字节域读出原始文本，再一次性转成 UTF-16 交给词边界算法。 */
#define SCI_GETCURRENTPOS 2008
#define SCI_GETANCHOR     2009
#define SCI_GETCODEPAGE   2137
#define SCI_SETSEL        2160
#define SCI_SCROLLCARET   2169
#define SCI_GETTEXT       2182
#define SCI_GETTEXTLENGTH 2183

/* 超过这个体积就不读全文，交给原生 Ctrl+方向兜底（不给巨型文件开大内存） */
#define SCI_MAX_BYTES     (2 * 1024 * 1024)

/* 光标/锚点的文档字节位置（供 SCI_SETSEL 瞬跳用） */
typedef struct {
    UINT     cp;           /* 文档代码页：65001=UTF-8，0 视为 CP_ACP */
    LONGLONG caretBytes;
    LONGLONG anchorBytes;
} SciPos;

static BOOL IsScintilla(HWND hwnd) {
    WCHAR c[96];
    if (!GetClassNameW(hwnd, c, 96)) return FALSE;
    return wcsstr(c, L"Scintilla") != NULL;
}

/* UTF-16 偏移 -> 文档字节偏移；编码不可逆（往返不一致）时返回 -1，
   上层收到 -1 就放弃瞬跳、改用逐格箭头（宁可慢也不能跳错位置）。 */
static LONGLONG Utf16ToDocBytes(const WCHAR* s, int u16len, UINT cp) {
    if (u16len < 0) return -1;
    if (u16len == 0) return 0;              /* 文档首：0 个前缀字符 = 字节 0 */
    int nb = WideCharToMultiByte(cp, 0, s, u16len, NULL, 0, NULL, NULL);
    if (nb <= 0) return -1;
    char* tmp = (char*)malloc((size_t)nb + 2);
    if (!tmp) return -1;
    WideCharToMultiByte(cp, 0, s, u16len, tmp, nb, NULL, NULL);
    tmp[nb] = 0;
    int back = MultiByteToWideChar(cp, 0, tmp, nb, NULL, 0);
    free(tmp);
    return (back == u16len) ? (LONGLONG)nb : -1;
}

/* EM_SETSEL 之后回读校验：有些"类 Edit"控件只实现半套协议
   （能回 EM_GETSEL 不一定能收 EM_SETSEL），置位失败就退回逐格箭头，
   宁可慢也不能光标原地不动。EM_GETSEL 返回值按 LOWORD/HIWORD 拆
   （跨进程不能传指针参数，文本超过 64K 时该返回值会截断，此为已知限制）。 */
/* EM_SETSEL 置位校验：读回的选区是有序 (start<=end)，与期望端点逐一比对 */
static BOOL EditVerifySel(HWND hwnd, int wantStart, int wantEnd) {
    LONGLONG sel = 0;
    if (!SendMsg(hwnd, EM_GETSEL, 0, 0, &sel)) return FALSE;
    DWORD v = (DWORD)sel;
    return (int)LOWORD(v) == wantStart && (int)HIWORD(v) == wantEnd;
}

/* 读回光标位置（EM_GETSEL 的 LOWORD）；读不到返回 -1 */
static LONGLONG EditGetCaret(HWND hwnd) {
    LONGLONG sel = 0;
    if (!SendMsg(hwnd, EM_GETSEL, 0, 0, &sel)) return -1;
    return (LONGLONG)LOWORD((DWORD)sel);
}

/* 直接置光标：SCI_SETSEL(anchor, caret)。Scintilla 语义：wParam=选区起点
   (anchor)，lParam=终点，活动光标落在 lParam —— 带选区位移(F)时 anchor
   固定在原光标、caret 移到目标，正好匹配。置完立即回读校验，位置不符就
   返回 FALSE，让上层退回逐格箭头。此前 wParam/lParam 写反：无选区时两点
   重合看不出来，Shift 位移时光标根本不动（选区还被反向设置）。 */
static BOOL SciSetSel(HWND hwnd, LONGLONG caretB, LONGLONG anchorB) {
    LONGLONG now = 0;
    SendMsg(hwnd, SCI_SETSEL, (WPARAM)anchorB, (LPARAM)caretB, NULL);
    SendMsg(hwnd, SCI_GETCURRENTPOS, 0, 0, &now);
    SendMsg(hwnd, SCI_SCROLLCARET, 0, 0, NULL);
    return now == caretB;
}

/* 读 Scintilla 全文 + 光标（UTF-16 域），并给出字节域位置供瞬跳 */
static BOOL SciGetTextCaret(HWND hwnd, WCHAR** text, int* lenOut, int* caretOut,
                            SciPos* pos) {
    if (!hwnd || !IsScintilla(hwnd)) return FALSE;

    /* 本通道要**在对方进程里开一块内存、再读回来**：Scintilla 的 SCI_GETTEXT
       只接受"对方进程地址空间里的缓冲区"，没有别的办法绕过，所以必须申请
       PROCESS_VM_OPERATION | PROCESS_VM_READ。这是对目标进程的实质性权限，
       默认开启但**可由用户关掉**：`[General] AllowScintillaRead = 0` 会让整条
       Scintilla 通道直接放弃，退回 UIA / IA2 / 原生 Ctrl+方向（功能降级，
       但不再触碰任何进程内存）。句柄即用即关，不缓存。 */
    if (!ConfigGetAllowScintillaRead()) {
        if (g_recording.load())
            CapLog("  Sci 通道已被配置关闭（AllowScintillaRead=0），跳过");
        return FALSE;
    }

    LONGLONG n = 0;
    if (!SendMsg(hwnd, SCI_GETTEXTLENGTH, 0, 0, &n)) return FALSE;
    if (n <= 0 || n > SCI_MAX_BYTES) return FALSE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return FALSE;
    /* 只要"在对方进程开内存 + 读回来"两项权限；权限不够就整条通道放弃。
       记录模式下写明对谁申请了什么 —— 排查"权限不足导致按词跳转降级"时一眼可见。 */
    HANDLE hp = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ, FALSE, pid);
    if (!hp) {
        if (g_recording.load())
            CapLog("  Sci 通道：OpenProcess(VM_OPERATION|VM_READ) 被拒 pid=%lu err=%lu",
                   (unsigned long)pid, (unsigned long)GetLastError());
        return FALSE;
    }

    BOOL   ok  = FALSE;
    char*  buf = (char*)malloc((size_t)n + 2);
    void*  rem = VirtualAllocEx(hp, NULL, (SIZE_T)n + 2,
                                MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf || !rem) goto done;

    LONGLONG got = 0;
    if (!SendMsg(hwnd, SCI_GETTEXT, (WPARAM)(n + 1), (LPARAM)rem, &got)) goto done;
    if (got <= 0 || got > n + 1) goto done;

    SIZE_T read = 0;
    if (!ReadProcessMemory(hp, rem, buf, (SIZE_T)got, &read) || read < (SIZE_T)got)
        goto done;
    buf[got] = 0;

    LONGLONG cpRaw = 0;
    if (!SendMsg(hwnd, SCI_GETCODEPAGE, 0, 0, &cpRaw)) goto done;
    int cp = (int)cpRaw;
    UINT ucp = (cp == 65001) ? CP_UTF8 : (cp == 0 ? CP_ACP : (UINT)cp);

    int nw = MultiByteToWideChar(ucp, 0, buf, (int)got, NULL, 0);
    if (nw <= 0) goto done;
    WCHAR* w = (WCHAR*)malloc(((size_t)nw + 1) * sizeof(WCHAR));
    if (!w) goto done;
    if (MultiByteToWideChar(ucp, 0, buf, (int)got, w, nw) != nw) { free(w); goto done; }
    w[nw] = 0;

    LONGLONG cb = 0, ab = 0;
    SendMsg(hwnd, SCI_GETCURRENTPOS, 0, 0, &cb);
    if (cb < 0) cb = 0;
    if (cb > got) cb = got;
    SendMsg(hwnd, SCI_GETANCHOR, 0, 0, &ab);
    if (ab < 0) ab = cb;

    int caretU16 = MultiByteToWideChar(ucp, 0, buf, (int)cb, NULL, 0);
    if (caretU16 < 0) caretU16 = 0;
    if (caretU16 > nw) caretU16 = nw;

    *text = w; *lenOut = nw; *caretOut = caretU16;
    pos->cp = ucp; pos->caretBytes = cb; pos->anchorBytes = ab;
    ok = TRUE;

done:
    if (rem) VirtualFreeEx(hp, rem, 0, MEM_RELEASE);
    if (buf) free(buf);
    CloseHandle(hp);
    return ok;
}

/* 代理对判定：补充平面字符用两个 UTF-16 code unit 表示（高+低代理），
   必须当作"一个字符"整体推进，否则会把它们拆散导致按词跳转错乱。 */
static BOOL IsHighSurrogateW(WCHAR c) { return c >= 0xD800 && c <= 0xDBFF; }
static BOOL IsLowSurrogateW(WCHAR c)  { return c >= 0xDC00 && c <= 0xDFFF; }

/* 字符类别：决定按词跳转边界。连续同类字符串视为"一个词"，
   按词跳转就停在这些词(与词/分隔符)的边界处。
   英文与数字同组（21M 是一个词）；谚文（韩文）自成一组；
   采用"反向"判定：先把 字母数字/谚文/符号 归类，
   其余兜底一律算 CH_OTHER（汉字、其他语言字符、生僻字、emoji 等）。 */
typedef enum { CH_OTHER, CH_HANGUL, CH_ALPHA, CH_SEP } CharClass;

static CharClass ClassOf(WCHAR c) {
    unsigned cp = (unsigned)c;
    /* 字母+数字（含全角 Ａ-Ｚ ａ-ｚ ０-９）同属一组 */
    if ((cp >= 0x30 && cp <= 0x39) || (cp >= 0x41 && cp <= 0x5A) ||
        (cp >= 0x61 && cp <= 0x7A))
        return CH_ALPHA;
    if ((cp >= 0xFF10 && cp <= 0xFF19) || (cp >= 0xFF21 && cp <= 0xFF3A) ||
        (cp >= 0xFF41 && cp <= 0xFF5A))
        return CH_ALPHA;
    /* 谚文（韩文）自成一组，完全按指定范围匹配：
       Hangul Jamo / 兼容 Jamo / Jamo Extended-A / 音节 */
    if ((cp >= 0x1100 && cp <= 0x11FF) ||
        (cp >= 0x3130 && cp <= 0x318F) ||
        (cp >= 0xA960 && cp <= 0xA97F) ||
        (cp >= 0xAC00 && cp <= 0xD7FF))
        return CH_HANGUL;
    /* 分隔符：空白 + 标点 */
    if (c == L'\0') return CH_SEP;
    if (iswspace(c)) return CH_SEP;
    if (iswpunct(c)) return CH_SEP;
    if (cp >= 0x2000 && cp <= 0x206F) return CH_SEP;  /* 通用标点 */
    if (cp >= 0x3000 && cp <= 0x303F) return CH_SEP;  /* CJK 标点(含全角空格) */
    if (cp >= 0xFF00 && cp <= 0xFFEF) return CH_SEP;  /* 其余全角符号标点 */
    /* 兜底：纯反向判定——非英文/数字/符号，一律归"其他"(含汉字与其他语言字符) */
    return CH_OTHER;
}

/* 以 s[i] 为"字符起点"（可能是一个代理对的首字）分类。 */
static CharClass ClassOfAt(const WCHAR* s, int len, int i) {
    if (i < 0 || i >= len) return CH_SEP;
    if (i + 1 < len && IsHighSurrogateW(s[i]) && IsLowSurrogateW(s[i+1]))
        return CH_OTHER;                               /* 补充平面字符：生僻字/emoji 等 → 其他 */
    if (IsHighSurrogateW(s[i]) || IsLowSurrogateW(s[i]))
        return CH_SEP;                                 /* 孤立半代理按分隔 */
    return ClassOf(s[i]);
}

/* 由 code-unit 位置 i 反推其左侧那个"字符"的起点（代理对整体归为一句）。 */
static int CharStartBefore(const WCHAR* s, int len, int i) {
    (void)len;
    if (i <= 0) return 0;
    /* 前一 code unit 是 low 代理，则它属于 high 起头的那个字符 */
    if (IsLowSurrogateW(s[i-1]) && i >= 2 && IsHighSurrogateW(s[i-2]))
        return i - 2;
    return i - 1;
}

/* 按词跳转核心：返回跳转后光标应处的 code-unit 位置，同时写出"需发送的箭头次数"。
   左右箭头在编辑框内按"字符"（code point）移动，因此步数(箭头数)按字符单位计数，
   代理对整体只计 1 步；目标 code-unit 位置供经典 Edit 直接置光标(瞬跳)。
   步数由 target 与 caret 的字符数差得到，与逐箭头移动完全等价。

   行边界规则：按词跳转默认不跨行（\r/\n 是分隔符，但分隔符串只在行内跨）。
   只有出发点已经在行尾时，右跳才跨过换行落到下一行行首词；
   只有出发点已经在行首时，左跳才跨过换行落到上一行行尾词。 */
static int WordJumpTarget(const WCHAR* s, int len, int caret, int dir, int* steps) {
    *steps = 0;
    if (!s || len < 1) return caret;
    /* 归一光标：落在代理对中间(异常，不该发生)时，视为整个字符未跨过 */
    if (caret > 0 && caret < len && IsHighSurrogateW(s[caret-1]) && IsLowSurrogateW(s[caret]))
        caret--;
    if (caret < 0) caret = 0;
    if (caret > len) caret = len;

    int i = caret, st = 0;

    if (dir == DIR_RIGHT) {
        int atEOL = (i >= len || s[i] == L'\n' || s[i] == L'\r');
        /* 跨当前同类型词串（\n 是分隔符，词串自然停在行尾） */
        CharClass cur = ClassOfAt(s, len, i);
        if (cur != CH_SEP) {
            while (i < len) {
                if (ClassOfAt(s, len, i) != cur) break;
                st++;
                i = (i + 1 < len && IsHighSurrogateW(s[i])) ? (i + 2) : (i + 1);
            }
        }
        /* 跨本行右侧分隔符（空格/制表等，不含换行） */
        while (i < len && ClassOfAt(s, len, i) == CH_SEP &&
               s[i] != L'\n' && s[i] != L'\r') {
            st++;
            i = (i + 1 < len && IsHighSurrogateW(s[i])) ? (i + 2) : (i + 1);
        }
        /* 出发点就在行尾：跨过换行与下一行行首缩进，停在下一行词首 */
        if (atEOL) {
            while (i < len && ClassOfAt(s, len, i) == CH_SEP) {
                st++;
                i = (i + 1 < len && IsHighSurrogateW(s[i])) ? (i + 2) : (i + 1);
            }
        }
    } else {
        int atBOL = (i <= 0 || s[i-1] == L'\n' || s[i-1] == L'\r');
        /* 跨本行左侧分隔符（空格/制表等，不含换行） */
        while (i > 0) {
            int st0 = CharStartBefore(s, len, i);
            if (ClassOfAt(s, len, st0) != CH_SEP) break;
            if (s[st0] == L'\n' || s[st0] == L'\r') break;
            st++; i = st0;
        }
        if (atBOL && i > 0) {
            /* 出发点就在行首：跨过换行与上一行行尾缩进，停在上一行词尾 */
            while (i > 0) {
                int st0 = CharStartBefore(s, len, i);
                if (ClassOfAt(s, len, st0) != CH_SEP) break;
                st++; i = st0;
            }
        } else if (i > 0) {
            /* 跨到左侧同类型词串首 */
            CharClass cur = ClassOfAt(s, len, CharStartBefore(s, len, i));
            while (i > 0) {
                int st0 = CharStartBefore(s, len, i);
                if (ClassOfAt(s, len, st0) != cur) break;
                st++; i = st0;
            }
        }
    }
    *steps = st;
    return i;
}

/* 非 static 仅供按词跳转单元测试（test/test_nav.c）直接调用。
   声明在 cap.h 里（不要在每个测试文件里各自 extern —— 签名一改就会漏改一处，
   而漏改的后果是链接期才发现，或者更糟：隐式声明按 int 返回值调用）。 */
int ComputeSteps(const WCHAR* s, int len, int caret, int dir) {
    int steps = 0;
    WordJumpTarget(s, len, caret, dir, &steps);
    return steps;
}

/* 取当前前台窗口的焦点控件。
   注意 hwndFocus **可能取不到**：GetGUIThreadInfo 只报告"属于该线程输入队列"
   的焦点窗口，而多线程 UI 的程序（**实测 WPS**：前台是 OpusApp，但焦点落在
   另一个线程持有的 _WwG 编辑控件上）会返回 NULL。这时退回**前台窗口本身**：
   各通道都自带类型判定（经典 Edit 白名单 / Scintilla 类名 / WPS 按进程名），
   拿错了不会误读；而 WPS 通道判的就是进程名，有前台窗口就够用。
   退回前要问"会不会把别的程序带偏"：UIA 通道拿到顶层窗口后走的是
   ElementFromHandle + 子树检索，本来就是它支持的用法。 */
static HWND GetFocusControl(void) {
    HWND fg = GetForegroundWindow();
    if (!fg) return NULL;
    DWORD tid = GetWindowThreadProcessId(fg, NULL);
    GUITHREADINFO gi;
    memset(&gi, 0, sizeof(gi));
    gi.cbSize = sizeof(gi);
    if (tid && GetGUIThreadInfo(tid, &gi) && gi.hwndFocus)
        return gi.hwndFocus;
    return fg;
}

/* 该控件能否用 EM_SETSEL **外部置位**。
   能读文本不代表能写：EmEditor 的 EmEditorView 接受 EM_SETSEL（光标确实会动，
   EM_GETSEL 读回也对），但它是**外部**改的选区，它的内部状态没跟着走 ——
   紧接着的第一次注入按键会被它吞掉（用户看到的就是"跳词后按 J 要按两下才
   有反应"）。日志实证：
     · 走过 EM_SETSEL 的那次跳词 -> 之后 `[注入] ←(J) 光标 51 -> 51`（无效），
       再按一次才 51 -> 50
     · 走改道（发原生 Ctrl+arrow，没碰 EM_SETSEL）的那次 -> 之后 `69 -> 68`
       一次就动
   所以对它**只读不写**：跳转改走逐格箭头 / 改道（发原生 Ctrl+方向），
   落点照样精确（有回读补正），只是不再"一步置位"。 */
static BOOL AllowDirectSet(HWND hwnd) {
    WCHAR c[96];
    if (!GetClassNameW(hwnd, c, 96)) return FALSE;
    if (_wcsicmp(c, L"EmEditorView") == 0) return FALSE;   /* EmEditor：只读不写 */
    return TRUE;
}

/* 是否"经典 Edit 协议"控件（可靠支持 WM_GETTEXT + EM_GETSEL）。
   白名单外不放行：Chromium 系窗口对 WM_GETTEXT 回的是标题、EM_GETSEL 是
   垃圾值，混进来会把按词跳转带偏。

   EmEditor 的 EmEditorView 支持这套协议，但**位置的单位随文档编码变**：
   UTF-16 文档给 UTF-16 下标（所以当初实测"完美支持"），**UTF-8 文档给的是
   字节偏移**（实测 73 字符的中文文档 EM_GETSEL 回 181）。所以放它进来的前提
   是 FastGetTextCaret 的单位一致性自检 —— 检测到不一致就放弃整条通道，
   绝不能用它的值算 steps 或写 EM_SETSEL（见那里的说明）。 */
static BOOL IsClassicEdit(HWND hwnd) {
    WCHAR c[96];
    if (!GetClassNameW(hwnd, c, 96)) return FALSE;
    if (_wcsicmp(c, L"Edit") == 0 || _wcsicmp(c, L"RICHEDIT") == 0) return TRUE;
    if (_wcsicmp(c, L"RichEdit20A") == 0 || _wcsicmp(c, L"RichEdit20W") == 0) return TRUE;
    if (_wcsicmp(c, L"RichEdit50W") == 0) return TRUE;
    if (_wcsicmp(c, L"EmEditorView") == 0) return TRUE;   /* EmEditor */
    return FALSE;
}

/* ================= 快速通道：经典 Edit / RichEdit / EmEditorView ================= */
/* 通过 WM_GETTEXT + EM_GETSEL 读取文本与光标。仅对白名单内的控件生效。 */
static BOOL FastGetTextCaret(HWND hwnd, WCHAR** text, int* lenOut, int* caretOut,
                             int* selEndOut) {
    if (!hwnd) return FALSE;
    if (!IsClassicEdit(hwnd)) return FALSE;   /* 非经典编辑：交给 Scintilla/UIA/原生 */

    /* 试探窗口是否支持 WM_GETTEXT（白名单控件） */
    LONGLONG size = 0;
    if (!SendMsg(hwnd, WM_GETTEXTLENGTH, 0, 0, &size)) return FALSE;
    /* 过短视为不支持；超过 64K 也不走本通道——EM_GETSEL 的打包返回值按
       LOWORD/HIWORD 拆，长文档读回的光标会回绕成错位值（宁可走原生兜底，
       也不能算出错误的目标位置）。 */
    if (size <= 0 || size > 64000) return FALSE;

    WCHAR* buf = (WCHAR*)malloc((size_t)((size + 2) * sizeof(WCHAR)));
    if (!buf) return FALSE;

    LONGLONG got = 0;
    if (!SendMsg(hwnd, WM_GETTEXT, (WPARAM)(size + 1), (LPARAM)buf, &got) || got <= 0) {
        free(buf);
        return FALSE;
    }
    /* EmEditor 等控件的 WM_GETTEXT 返回计数把结尾 NUL 也算进去（got=长度+1）。
       多出的 '\0' 会被当成一个分隔符参与按词跳转，算出越界目标（tgt=文档长+1），
       必须剥掉。 */
    while (got > 0 && buf[got - 1] == L'\0') got--;
    if (got <= 0) { free(buf); return FALSE; }

    /* 光标位置：EM_GETSEL。返回值打包 (选区start, 选区end)——选区有序
       (start<=end)，Shift 位移时"活动端"按方向取其一（见 NavJump）。

       ★ 单位一致性自检（EmEditor 实测踩坑）：本通道假定 EM_GETSEL 与
       WM_GETTEXT **同单位**（UTF-16 单元）。但有的"类 Edit"控件对外给 UTF-16
       文本，EM_GETSEL/EM_SETSEL 却按**文档编码的字节**计 —— EmEditor 的
       EmEditorView 就是：73 个字符的 UTF-8 中文文档，EM_GETSEL 回 181
       （一个汉字 3 字节），而 WM_GETTEXT 回 73 个字符。
       以前这里把越界值**钳到文本末尾**，把"单位不一致"这个明确信号抹成了
       "看起来正常"：从假光标位置算出 steps、再把错位的字节值写回 EM_SETSEL，
       落点会停在多字节字符中间并把该控件的内部状态改坏 —— 之后连**真实**
       方向键都失灵（用户上报："跳词后自己按方向键都不行，只有 EmEditor"）。
       判据只有一条：EM_GETSEL 的值超出文本长度就说明单位不同 ⇒ 整条通道放弃，
       交给 UIA / IA2 / 原生 Ctrl+方向（原生是相对移动，与单位无关，安全）。
       标准 Edit 控件恒有 值 <= 文本长度，不会误伤（也正因如此，钳制在这里
       本来就永远不该触发 —— 它只在单位不一致时才生效）。 */
    LONGLONG sel = 0;
    if (!SendMsg(hwnd, EM_GETSEL, 0, 0, &sel)) { free(buf); return FALSE; }
    DWORD packed = (DWORD)sel;
    if (packed != 0xFFFFFFFF &&
        ((LONGLONG)LOWORD(packed) > got || (LONGLONG)HIWORD(packed) > got)) {
        if (g_recording.load())
            CapLog("  Edit 通道：EM_GETSEL=%d/%d 超出文本长度 %lld，"
                   "该控件按字节计位置（非 UTF-16），放弃本通道",
                   (int)LOWORD(packed), (int)HIWORD(packed), got);
        free(buf);
        return FALSE;
    }
    int caret = (packed != 0xFFFFFFFF) ? (int)LOWORD(packed) : 0;
    int selEnd = (packed != 0xFFFFFFFF) ? (int)HIWORD(packed) : caret;
    if (selEnd < caret) selEnd = caret;          /* 有序化 */

    *text = buf;
    *lenOut = (int)got;
    *caretOut = caret;
    if (selEndOut) *selEndOut = selEnd;
    return TRUE;
}

/* ================= UIA 回退通道 ================= */
/* 以 C 宏接口访问 UIA COM（uiautomationclient.h 在纯 C 下提供 *_宏） */
/* 从 start 出发沿祖先链向上定位拥有 DocumentRange 的元素。
   命中的元素结果会 AddRef 一份交给调用者持有，沿途其余元素全部 Release。 */
static IUIAutomationElement* FindDocElement(IUIAutomation* ua, IUIAutomationElement* start) {
    IUIAutomationElement* chain[32];
    int n = 0;
    IUIAutomationElement* cur = start;
    IUIAutomationTreeWalker* wk = NULL;
    IUIAutomation_get_RawViewWalker(ua, &wk);
    IUIAutomationElement* result = NULL;

    while (cur && n < 32) {
        chain[n++] = cur;
        IUIAutomationTextPattern* t = NULL;
        if (SUCCEEDED(IUIAutomationElement_GetCurrentPatternAs(cur, UIA_TextPatternId,
                            IID_IUIAutomationTextPattern, (void**)&t)) && t) {
            IUIAutomationTextRange* d = NULL;
            if (SUCCEEDED(IUIAutomationTextPattern_get_DocumentRange(t, &d)) && d) {
                IUIAutomationTextRange_Release(d);
                result = cur;
            }
            IUIAutomationTextPattern_Release(t);
            if (result) break;
        }
        IUIAutomationElement* parent = NULL;
        if (!wk || FAILED(IUIAutomationTreeWalker_GetParentElement(wk, cur, &parent)))
            break;
        cur = parent;
    }
    if (wk) IUIAutomationTreeWalker_Release(wk);

    if (result) IUIAutomationElement_AddRef(result);   /* 抄一份给调用者 */
    /* 只释放本函数**自己拿到**的引用（chain[1..n-1]，来自 GetParentElement）。
       chain[0] 是调用方传进来的 start —— 它不属于本函数，调用方随后还会
       Release 一次，这里再释放就是**双重释放**。
       实测后果（2026-09-11）：WPS 不提供 UIA 文本，每次按词跳转都走"找不到
       doc element"的失败路径 → 每次都对调用方的 ef/eh 多释放一次 →
       refcount 归零、对象被销毁 → 调用方的 Release 读已释放内存的 vtable
       指针 → c0000005（WER 定位在 UiaGetTextCaret 的 Release 调用点）。
       同一处还导致 FindDocInSubtree(ua, eh) 拿着已释放的 eh 继续用。 */
    for (int i = 1; i < n; i++)
        if (chain[i] && chain[i] != result) IUIAutomationElement_Release(chain[i]);
    return result;
}

/* ================= COM / UIA 资源缓存 =================
   原来每个 UIA 函数都成对 CoInitialize/CoUninitialize，并且每次
   CoCreateInstance(CLSID_CUIAutomation)；而一次按词跳转最多走两遍（读取 +
   置位后回读校验）。CUIAutomation 的创建要拉起 UIA 服务与缓存层，开销在
   数十毫秒量级 —— 全落在大键路径上，还叠加了主线程阻塞。

   缓存必须是"每线程一份"，不能做成进程级单例：
     · CoInitialize 是线程级状态，执行线程（act.c）要有自己的；
     · CUIAutomation 注册为 Apartment 线程模型，接口指针跨套间裸传是错的
       （需要封送），所以不能两个线程共享同一个实例。
   结束时不 CoUninitialize：调用者只有主线程与执行线程，都在进程退出时才
   结束，交给进程退出回收。 */
static __declspec(thread) BOOL           t_coReady = FALSE;
static __declspec(thread) IUIAutomation* t_uia     = NULL;

static BOOL CoUp(void) {
    if (t_coReady) return TRUE;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    /* RPC_E_CHANGED_MODE：本线程已按别的模式初始化过 COM，照样能用；
       S_FALSE：本线程重复初始化。两者都不该再反初始化。 */
    if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) t_coReady = TRUE;
    return t_coReady;
}

/* 取本线程的 IUIAutomation（首次创建后常驻）。返回的是缓存引用，
   调用方**不要** Release。 */
static IUIAutomation* UiaGet(void) {
    if (t_uia) return t_uia;
    if (!CoUp()) return NULL;
    IUIAutomation* ua = NULL;
    if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER,
                                   IID_IUIAutomation, (void**)&ua)) && ua)
        t_uia = ua;
    return t_uia;
}

/* 在子树里找第一个"带 DocumentRange"的元素。
   Chromium 系（QQ / Edge / VSCode）的 UIA 焦点有时落在 Pane 上：Pane 自身没有
   DocumentRange，沿祖先链向上也找不到（顶层 Chrome_WidgetWin_1 同样没有）——
   必须往下找真正的文本框。用 UIA 自己的条件检索（FindAll +
   IsTextPatternAvailable），避免在客户端逐个节点遍历。 */
#define UIA_SCAN_LIMIT 24
static IUIAutomationElement* FindDocInSubtree(IUIAutomation* ua,
                                              IUIAutomationElement* root) {
    IUIAutomationCondition* cond = NULL;
    IUIAutomationElementArray* arr = NULL;
    IUIAutomationElement* result = NULL;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_BOOL;
    v.boolVal = VARIANT_TRUE;
    if (FAILED(IUIAutomation_CreatePropertyCondition(ua,
            UIA_IsTextPatternAvailablePropertyId, v, &cond)) || !cond) {
        VariantClear(&v);
        return NULL;
    }
    VariantClear(&v);

    if (SUCCEEDED(IUIAutomationElement_FindAll(root, TreeScope_Subtree, cond, &arr))
        && arr) {
        int n = 0;
        arr->lpVtbl->get_Length(arr, &n);
        if (n > UIA_SCAN_LIMIT) n = UIA_SCAN_LIMIT;
        for (int i = 0; i < n; i++) {
            IUIAutomationElement* e = NULL;
            if (FAILED(arr->lpVtbl->GetElement(arr, i, &e)) || !e) continue;
            IUIAutomationTextPattern* t = NULL;
            if (SUCCEEDED(IUIAutomationElement_GetCurrentPatternAs(e, UIA_TextPatternId,
                    IID_IUIAutomationTextPattern, (void**)&t)) && t) {
                IUIAutomationTextRange* d = NULL;
                if (SUCCEEDED(IUIAutomationTextPattern_get_DocumentRange(t, &d)) && d) {
                    IUIAutomationTextRange_Release(d);
                    IUIAutomationTextPattern_Release(t);
                    result = e;              /* 命中：引用交给调用者 */
                    break;
                }
                IUIAutomationTextPattern_Release(t);
            }
            IUIAutomationElement_Release(e);
        }
        arr->lpVtbl->Release(arr);
    }
    if (cond) IUIAutomationCondition_Release(cond);
    return result;
}

/* UIA 每一步的 HRESULT 只在 C+` 记录期间写日志，便于定位"为什么读不到文本" */
static void UiaLog(const char* what, HRESULT hr) {
    if (g_recording.load())
        CapLog("  UIA %-26s hr=0x%08lX %s", what, (unsigned long)hr,
               SUCCEEDED(hr) ? "OK" : "FAIL");
}

/* 诊断：把 WCHAR 文本片段转成 UTF-8 写日志，控制字符转义可见。
   用于核对"UIA 读到的文本"与实际显示是否一致（WPF 等实现可能夹带
   不可见字符，词边界在其上计算会错位）。 */
static void LogTextSample(const WCHAR* s, int start, int count, const char* tag) {
    if (!g_recording.load() || !s || count <= 0) return;
    char utf8[1200];
    size_t o = 0;
    for (int i = 0; i < count; i++) {
        WCHAR c = s[start + i];
        char one[8];
        int n = 0;
        if (c == L'\r')      { one[0]='\\'; one[1]='r'; n=2; }
        else if (c == L'\n') { one[0]='\\'; one[1]='n'; n=2; }
        else if (c == L'\t') { one[0]='\\'; one[1]='t'; n=2; }
        else {
            WCHAR w[2] = { c, 0 };
            n = WideCharToMultiByte(CP_UTF8, 0, w, 1, one, sizeof(one), NULL, NULL);
            if (n <= 0) { one[0]='?'; n=1; }
        }
        if (o + n >= sizeof(utf8) - 1) break;
        memcpy(utf8 + o, one, (size_t)n);
        o += (size_t)n;
    }
    utf8[o] = 0;
    CapLog("  UIA 文本%s=[%s] (start=%d count=%d)", tag, utf8, start, count);
}

static BOOL UiaGetTextCaret(HWND hwnd, WCHAR** text, int* lenOut, int* caretOut) {
    HRESULT hr;
    BSTR all = NULL, pre = NULL;
    IUIAutomation         *ua  = NULL;
    IUIAutomationElement  *el  = NULL;
    IUIAutomationTextPattern *tp = NULL;
    IUIAutomationTextRange *doc = NULL, *caret = NULL, *tmp = NULL;
    BOOL ok = FALSE;

    ua = UiaGet();                       /* 本线程缓存，不 Release */
    UiaLog("UiaGet", ua ? S_OK : E_FAIL);
    if (!ua) return FALSE;

    /* 锚定候选：优先 UIA 焦点元素（GetFocusedElement 能命中 Chromium/WinUI 的
       真正文本框），取不到 TextPattern 时再回退 ElementFromHandle(hwnd)。
       曾改为 ElementFromHandle 直取顶层窗口，但 Chromium(Chrome_WidgetWin_1)
       顶层窗口本身无 TextPattern，向上也找不到 → 造成 QQ/Edge 按词跳转回归。
       现在补第三级：句柄元素沿祖先链找不到时，往子树里找真正的文本框。 */
    IUIAutomationElement *owner = NULL, *ef = NULL, *eh = NULL;
    hr = IUIAutomation_GetFocusedElement(ua, &ef);
    if (SUCCEEDED(hr) && ef) {
        owner = FindDocElement(ua, ef);
        UiaLog("focus/ancestors", owner ? S_OK : E_FAIL);
        IUIAutomationElement_Release(ef);
    } else {
        UiaLog("GetFocusedElement", hr);
    }
    if (!owner && hwnd) {
        if (SUCCEEDED(IUIAutomation_ElementFromHandle(ua, hwnd, &eh)) && eh) {
            owner = FindDocElement(ua, eh);
            UiaLog("handle/ancestors", owner ? S_OK : E_FAIL);
            if (!owner) {
                owner = FindDocInSubtree(ua, eh);
                UiaLog("handle/subtree", owner ? S_OK : E_FAIL);
            }
            IUIAutomationElement_Release(eh);
        }
    }
    if (!owner) { UiaLog("no doc element", E_FAIL); goto done; }
    el = owner;

    /* owner 上应有 TextPattern；取其 DocumentRange 与 selection 兜底 */
    hr = IUIAutomationElement_GetCurrentPatternAs(el, UIA_TextPatternId,
                    IID_IUIAutomationTextPattern, (void**)&tp);
    UiaLog("TextPattern", hr);
    if (SUCCEEDED(hr) && tp) {
        hr = IUIAutomationTextPattern_get_DocumentRange(tp, &doc);
        UiaLog("DocumentRange", hr);
    }
    if (!doc) goto done;

    /* 光标位置：优先 TextPattern2 的 caret range */
    {
        IUIAutomationTextPattern2* tp2 = NULL;
        if (SUCCEEDED(IUIAutomationElement_GetCurrentPatternAs(el, UIA_TextPattern2Id,
                            IID_IUIAutomationTextPattern2, (void**)&tp2)) && tp2) {
            BOOL active = FALSE;
            hr = IUIAutomationTextPattern2_GetCaretRange(tp2, &active, &caret);
            if (FAILED(hr) || !caret) caret = NULL;
            UiaLog("GetCaretRange", caret ? hr : E_FAIL);
            IUIAutomationTextPattern2_Release(tp2);
        }
    }
    if (!caret) {
        /* 回退：selection 的第一个 range 作为 caret（Chromium 系常走这里） */
        IUIAutomationTextRangeArray* sel = NULL;
        hr = IUIAutomationTextPattern_GetSelection(tp, &sel);
        if (SUCCEEDED(hr) && sel) {
            int len = 0;
            sel->lpVtbl->get_Length(sel, &len);
            if (len > 0) sel->lpVtbl->GetElement(sel, 0, &caret);
            sel->lpVtbl->Release(sel);
        }
        UiaLog("GetSelection", caret ? hr : E_FAIL);
    }
    if (!caret) goto done;

    /* 截取 [文档起点, caret 起点) → 长度即光标偏移 */
    if (FAILED(IUIAutomationTextRange_Clone(doc, &tmp)) || !tmp) goto done;
    hr = IUIAutomationTextRange_MoveEndpointByRange(tmp,
                TextPatternRangeEndpoint_End, caret, TextPatternRangeEndpoint_Start);
    UiaLog("MoveEndpointByRange", hr);
    if (FAILED(IUIAutomationTextRange_GetText(tmp, -1, &pre)) || !pre) goto done;
    {
        int idx = (int)wcslen(pre);
        if (SUCCEEDED(IUIAutomationTextRange_GetText(doc, -1, &all)) && all) {
            int alen = (int)wcslen(all);
            /* VSCode 等 Electron 应用：DOM 焦点落在 a11y 节点（role=textbox 的
               div）上。VSCode 源码 screenReaderContentSimple.ts 的
               updateScreenReaderContent()：只有 isScreenReaderOptimized() 时
               才把真实文本写进节点并同步 DOM selection；否则主动清空——
               所以我们读到的是 1 字符占位。弃用本通道交给原生兜底，
               并在日志里给出开启方法。 */
            if (alen < 2) {
                if (g_recording.load())
                    CapLog("  UIA doc text len=%d -> a11y 占位。VSCode 请在 settings.json "
                           "设 \"editor.accessibilitySupport\": \"on\" 后即可用按词跳转引擎", alen);
                goto done;
            }
            if (idx > alen) idx = alen;
            if (idx < 0) idx = 0;
            WCHAR* copy = (WCHAR*)malloc((size_t)(alen + 1) * sizeof(WCHAR));
            if (copy) {
                memcpy(copy, all, (size_t)alen * sizeof(WCHAR));
                copy[alen] = 0;
                *text = copy;
                *lenOut = alen;
                *caretOut = idx;
                ok = TRUE;
                /* 头/尾样本入日志：核对 UIA 文本与显示是否一致 */
                LogTextSample(copy, 0, alen < 24 ? alen : 24, "头");
                LogTextSample(copy, alen > 48 ? alen - 48 : 0,
                              alen > 48 ? 48 : alen, "尾");
            }
        }
    }

done:
    if (all) SysFreeString(all);
    if (pre) SysFreeString(pre);
    if (tmp)  IUIAutomationTextRange_Release(tmp);
    if (caret) IUIAutomationTextRange_Release(caret);
    if (doc)  IUIAutomationTextRange_Release(doc);
    if (tp)   IUIAutomationTextPattern_Release(tp);
    if (el)   IUIAutomationElement_Release(el);
    /* ua 由 UiaGet 的线程缓存持有，这里不 Release、也不 CoUninitialize */
    return ok;
}

/* ---- UIA 直接置位：TextRange.Select() ----
   官方文档（learn.microsoft.com, IUIAutomationTextRange::Select）：
   "If the Select method is called on a text range object that represents a
   degenerate (empty) text range, the text insertion point moves to the
   starting endpoint of the text range." —— 空 range 即"移动光标"。
   做法：锚定流程与读取通道同源（焦点元素 -> 祖先链 -> 句柄/子树），
   克隆 DocumentRange 折叠到 caret，再按 delta 展开到目标端，折叠后
   Select() 落位；带 Shift 时保留展开区间 Select（产生选区）。
   返回 TRUE 仅表示 Select 的 HRESULT 成功，调用方仍需回读校验
   （provider 可能"成功"但没动光标）。 */
static BOOL UiaSetSel(HWND hwnd, int caret, int target, BOOL shift) {
    HRESULT hr;
    IUIAutomation *ua = NULL;
    IUIAutomationElement *el = NULL, *ef = NULL, *eh = NULL, *owner = NULL;
    IUIAutomationTextPattern *tp = NULL;
    IUIAutomationTextRange *doc = NULL, *r = NULL;
    BOOL ok = FALSE;
    int delta = target - caret;
    const char* why = "";                              /* 失败点（诊断日志用） */
    if (delta == 0) return TRUE;                       /* 无需移动 */

    ua = UiaGet();                                     /* 本线程缓存，不 Release */
    if (!ua) return FALSE;
    if (SUCCEEDED(IUIAutomation_GetFocusedElement(ua, &ef)) && ef) {
        owner = FindDocElement(ua, ef);
        IUIAutomationElement_Release(ef);
    }
    if (!owner && hwnd) {
        if (SUCCEEDED(IUIAutomation_ElementFromHandle(ua, hwnd, &eh)) && eh) {
            owner = FindDocElement(ua, eh);
            if (!owner) owner = FindDocInSubtree(ua, eh);
            IUIAutomationElement_Release(eh);
        }
    }
    if (!owner) goto done;
    el = owner;

    hr = IUIAutomationElement_GetCurrentPatternAs(el, UIA_TextPatternId,
                    IID_IUIAutomationTextPattern, (void**)&tp);
    if (FAILED(hr) || !tp) goto done;
    hr = IUIAutomationTextPattern_get_DocumentRange(tp, &doc);
    if (FAILED(hr) || !doc) goto done;

    if (FAILED(IUIAutomationTextRange_Clone(doc, &r)) || !r) goto done;
    /* 构造 [A,B]（A=min(target,caret)，B=max）。实测 Chromium 系（QQ/Edge）
       的 UIA TextRange 对这些操作的态度：
         可用：End 端点的**正向** MoveEndpointByUnit(+n)、端点**绝对定位**
              （MoveEndpointByRange，无方向语义）、Select、ScrollIntoView
         不可用：Start 端点的 MoveEndpointByUnit（任何方向 moved=0）、
              负向 Move —— 即"只支持移动 End"
       因此构造顺序全部依赖"End 正向移动 + 绝对定位到已知参照"：
         1) End 绝对定位到文档头          r=[0,0]
         2) End 正向 +A                   r=[0,A]
         3) Start 绝对定位到 r.End        r=[A,A]
         4) End 正向 +(B-A)               r=[A,B]
       Select 作用在折叠 range 上即移动光标（官方语义），带 Shift 时保留
       展开区间产生选区。 */
    {
        int A = (target < caret) ? target : caret;
        int B = (target < caret) ? caret : target;
        /* count / moved 都按头文件声明用 int（原来写 long，/W4 报 C4057；
           x64 下 long 也是 32 位所以没出事，但类型对不上就是在掩盖真问题） */
        int moved = 0;
        hr = IUIAutomationTextRange_MoveEndpointByRange(r, TextPatternRangeEndpoint_End,
                doc, TextPatternRangeEndpoint_Start);
        if (FAILED(hr)) { why = "pin End to doc head"; goto done; }
        if (A > 0) {
            hr = IUIAutomationTextRange_MoveEndpointByUnit(r, TextPatternRangeEndpoint_End,
                    TextUnit_Character, A, &moved);
            if (FAILED(hr) || moved != A) { why = "move End to A"; goto done; }
        }
        hr = IUIAutomationTextRange_MoveEndpointByRange(r, TextPatternRangeEndpoint_Start,
                r, TextPatternRangeEndpoint_End);
        if (FAILED(hr)) { why = "fold Start to End"; goto done; }
        if (B > A) {
            hr = IUIAutomationTextRange_MoveEndpointByUnit(r, TextPatternRangeEndpoint_End,
                    TextUnit_Character, B - A, &moved);
            if (FAILED(hr) || moved != (B - A)) { why = "move End to B"; goto done; }
        }
    }
    if (!shift) {
        /* 无 Shift：折叠到目标端（绝对定位）。向左时 tgt 在 A=Start、
           向右时 tgt 在 B=End —— 折叠错端会让 Select 落在原地（N 失效） */
        hr = (delta < 0)
           ? IUIAutomationTextRange_MoveEndpointByRange(r, TextPatternRangeEndpoint_End,
                                                        r, TextPatternRangeEndpoint_Start)
           : IUIAutomationTextRange_MoveEndpointByRange(r, TextPatternRangeEndpoint_Start,
                                                        r, TextPatternRangeEndpoint_End);
        if (FAILED(hr)) { why = "collapse to target"; goto done; }
    }
    /* 带 Shift：保留 [A,B] 展开区间，Select 产生选区 */
    hr = IUIAutomationTextRange_Select(r);
    ok = SUCCEEDED(hr);
    if (ok) {
        /* 规范不保证 Select 滚动视图：补一次 ScrollIntoView 让光标可见
           （不支持者返回错误码，忽略即可） */
        IUIAutomationTextRange_ScrollIntoView(r, FALSE);
    }
    if (g_recording.load())
        CapLog("  UIA Select hr=0x%08lX delta=%d shift=%d",
               (unsigned long)hr, delta, shift ? 1 : 0);

done:
    if (!ok && g_recording.load() && why[0])
        CapLog("  UIA SetSel 失败于 %s (delta=%d)", why, delta);
    if (r)      IUIAutomationTextRange_Release(r);
    if (doc)    IUIAutomationTextRange_Release(doc);
    if (tp)     IUIAutomationTextPattern_Release(tp);
    if (el)     IUIAutomationElement_Release(el);
    /* ua 由 UiaGet 的线程缓存持有，这里不 Release、也不 CoUninitialize */
    return ok;
}

/* ---- IA2 直接置位：setCaretOffset / setSelection ----
   官方 IAccessible2 文档：setCaretOffset "Sets the position of the caret"，
   不支持时返回 E_FAIL（此时退回箭头）。 */
static BOOL MsaaSetSel(HWND hwnd, int caret, int target, BOOL shift) {
    if (!hwnd) return FALSE;
    /* OBJID_CLIENT 在 winuser.h 里是 ((LONG)0xFFFFFFFC)，而该 API 的形参是
       DWORD —— 不加转换会有一条 C4245（有符号/无符号不匹配）。语义上完全
       无害（就是那个固定值），但显式转一下能让 /W4 输出保持干净：以后新增的
       警告不会被这三条"已知无害"的噪音淹掉。 */
    IAccessible* acc = NULL;
    HRESULT hr = AccessibleObjectFromWindow(hwnd, (DWORD)OBJID_CLIENT,
                                            IID_IAccessible, (void**)&acc);
    if (FAILED(hr) || !acc) return FALSE;
    IAccessibleText* at = NULL;
    hr = acc->lpVtbl->QueryInterface(acc, kIID_IAccessibleText, (void**)&at);
    acc->lpVtbl->Release(acc);
    if (FAILED(hr) || !at) return FALSE;
    hr = shift ? at->lpVtbl->setSelection(at, 0, caret, target)
               : at->lpVtbl->setCaretOffset(at, target);
    at->lpVtbl->Release(at);
    return SUCCEEDED(hr);
}

/* ================= IA2 通道：IAccessible2 (IAccessibleText) =================
   兜底无障碍通道，面向"既不是经典 Edit、也不暴露 UIA/SCI 文本"的控件。
   （Scintilla 系现在优先走更快的 SCI 直读通道，不再依赖这里。）
   COM 调用全程系统封送，跨进程/跨位宽安全。 */
static BOOL MsaaGetTextCaret(HWND hwnd, WCHAR** text, int* lenOut, int* caretOut) {
    if (!hwnd) return FALSE;
    IAccessible* acc = NULL;
    HRESULT hr = AccessibleObjectFromWindow(hwnd, (DWORD)OBJID_CLIENT, IID_IAccessible, (void**)&acc);
    if (FAILED(hr) || !acc) return FALSE;

    IAccessibleText* at = NULL;
    hr = acc->lpVtbl->QueryInterface(acc, kIID_IAccessibleText, (void**)&at);
    acc->lpVtbl->Release(acc);
    if (FAILED(hr) || !at) return FALSE;

    long caret = -1;
    hr = at->lpVtbl->caretOffset(at, &caret);
    if (FAILED(hr) || caret < 0) { at->lpVtbl->Release(at); return FALSE; }

    BSTR b = NULL;
    hr = at->lpVtbl->text(at, 0, -1, &b);   /* -1 = IA2_TEXT_OFFSET_LENGTH(文本末尾) */
    at->lpVtbl->Release(at);
    if (FAILED(hr) || !b) return FALSE;

    int len = (int)SysStringLen(b);
    /* 与 UIA 通道同理：短于 2 字符视为占位文本（非真实编辑器内容），弃用 */
    if (len < 2) {
        SysFreeString(b);
        if (g_recording.load()) CapLog("  IA2 text len=%d -> 占位文本，弃用走原生", len);
        return FALSE;
    }
    WCHAR* copy = (WCHAR*)malloc(((size_t)len + 1) * sizeof(WCHAR));
    if (copy) { memcpy(copy, b, (size_t)len * sizeof(WCHAR)); copy[len] = 0; }
    SysFreeString(b);
    if (!copy) return FALSE;
    if (caret > len) caret = len;

    *text = copy; *lenOut = len; *caretOut = (int)caret;
    return TRUE;
}

/* Shift 位移序列的锚点跟踪（经典 Edit 通道）：
   EM_GETSEL 只给有序两端、没有 anchor 概念。序列第一次位移时把"当时
   的活动端"定为锚并固定，其后活动端 = 另一端 —— 这样 F+N 后再 F+M，
   活动端会沿原路缩回锚点（符合系统 Shift 位移的直觉）。
   序列结束（无 Shift 的跳转、文本长度变化、引擎复位）即清除。 */
static int  g_selAnchor = -1;         /* -1 = 未在位移序列中 */
static int  g_selAnchorLen = -1;
static BOOL g_selAnchorIsStart = FALSE;

/* 这几份状态（以及下面的 g_noSetProc / g_noSetCount）的**归属必须说清**：
   NavJump 跑在 act.c 的**工作线程**上（ActPost 的同步兜底路径还可能落在
   主线程），而 NavSelTrackReset() 由主线程的和弦引擎调用 —— 两边都会读写。
   用一把临界区包住（只在读写这几个变量的那几行持有，中间不做任何 I/O），
   是这里最省事也最不容易搞错的做法。 */
static CRITICAL_SECTION g_navCs;
static INIT_ONCE g_navOnce = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK NavCsInit(PINIT_ONCE o, PVOID p, PVOID* c) {
    (void)o; (void)p; (void)c;
    InitializeCriticalSection(&g_navCs);
    return TRUE;
}
#define NAV_LOCK()   do { \
        InitOnceExecuteOnce(&g_navOnce, NavCsInit, NULL, NULL); \
        EnterCriticalSection(&g_navCs); \
    } while (0)
#define NAV_UNLOCK() LeaveCriticalSection(&g_navCs)

void NavSelTrackReset(void) {
    NAV_LOCK();
    g_selAnchor = -1; g_selAnchorLen = -1;
    NAV_UNLOCK();
}

/* "该程序连续两次直接置位无效"的记账：命中后本次会话内不再尝试置位。
   WPF 一类实现的 TextRange.Select() 会返回成功却不生效，还会让显示光标闪一下 ——
   每次试一次就每次都闪，所以要记账退出（重启程序恢复）。
   与上面几份状态同属"两个线程都会碰"的变量，读写都在 NAV_LOCK 里。 */
static WCHAR g_noSetProc[64] = {0};
static int   g_noSetCount = 0;

/* ================= WPS 通道：见 src/wps.c（此处只留用法说明） =================
   WPS 文字既没有 UIA/IA2 文本、也不响应 WM_GETTEXT，但它完整实现了 Word 的
   对象模型，所以走 COM 自动化读它文档引擎里的数据；以管理员运行时还要经
   "降权桥"（COM 的 ROT 按完整性级别隔离，管理员进程附不上普通权限的 WPS）。

   这一块**不该住在 nav.c 里**：它跟"按词跳转"没关系，只是又一个文本通道；
   而且里面有两层对别的场景同样有用的能力（通用 IDispatch 按名调用、通用降权
   IPC）。所以拆成三个模块，nav.c 只留下面两处调用：
     · Ad* 通用 IDispatch 助手   -> src/autodisp.c + src/auto.h
     · Br* 通用降权桥            -> src/bridge.c   + src/auto.h
     · Wps* WPS 对象模型与协议   -> src/wps.c      + src/auto.h
   以后要支持"另一个只有 COM 才读得到的程序"，照 wps.c 再写一个文件即可，
   前两层直接复用。 */

/* 文本通道标识。以前这里是 `const char* path` 配"首字母判据"
   （'E' 看 Edit、'W' 看 WPS…）—— 加一个通道得先确认首字母不和已有的撞车，
   读代码的人还得回头翻字符串表才知道 'I' 是 IA2。改成强类型枚举后，比较处
   一眼可读，通道名也不再和业务字符串搅在一起。 */
enum class Chan { Native, Edit, Sci, UIA, IA2, WPS };

/* 日志用的通道名（诊断输出里仍是人读的短名）。 */
static const char* ChanName(Chan c) {
    switch (c) {
    case Chan::Edit: return "Edit";
    case Chan::Sci:  return "Sci";
    case Chan::UIA:  return "UIA";
    case Chan::IA2:  return "IA2";
    case Chan::WPS:  return "WPS";
    case Chan::Native:
    default:         return "native";
    }
}

static int NavJumpInner(int dir, BOOL shift);

/* 对外的入口只做一件事：给本次跳转开一个同步 IPC 预算（见 NavIpcBudgetBegin），
   把"目标程序卡住时超时会叠加"的最坏情况从 ~1.8s 压到 NAV_IPC_BUDGET_MS。 */
int NavJump(int dir, BOOL shift) {
    NavIpcBudgetBegin();
    int r = NavJumpInner(dir, shift);
    NavIpcBudgetEnd();
    return r;
}

/* 非 static 仅供"注入后回读"的诊断日志用（act.c 记录模式下调）。
   读当前焦点控件的光标：经典 Edit 用 EM_GETSEL；Scintilla 用字节位
   （不换算成 UTF-16，诊断只看"动没动"，量纲一致即可）。读不到返回 -1。
   为什么需要它：日志能证明"我们发了方向键"，但证明不了"目标程序认了" ——
   用户看到"按了没反应"时，只有这一读才能把"没发出去"和"发了但目标程序
   没动"分开。故意不做任何缓存，也不改任何状态。 */
long long NavFocusCaret(void) {
    HWND focus = GetFocusControl();
    if (!focus) return -1;
    if (IsClassicEdit(focus)) {
        LONGLONG sel = 0;
        if (!SendMsg(focus, EM_GETSEL, 0, 0, &sel)) return -1;
        return (LONGLONG)LOWORD((DWORD)sel);
    }
    if (IsScintilla(focus)) {
        LONGLONG pos = -1;
        if (!SendMsg(focus, SCI_GETCURRENTPOS, 0, 0, &pos)) return -1;
        return pos;
    }
    return -1;
}

static int NavJumpInner(int dir, BOOL shift) {
    WCHAR* text = NULL;
    int len = 0, caret = 0;
    if (g_recording.load()) CapLog("NavJump dir=%d shift=%d", dir, shift);

    if (!shift) { NAV_LOCK(); g_selAnchor = -1; g_selAnchorLen = -1; NAV_UNLOCK(); }

    BOOL got = FALSE;
    Chan path = Chan::Native;
    SciPos sp;
    memset(&sp, 0, sizeof(sp));
    HWND focus = GetFocusControl();
    /* 配置名单内的程序（如 VSCode）：跳过全部文本探测，直接原生 Ctrl+方向 */
    if (focus && ConfigIsNativeWordJump(focus)) {
        SendNativeWordJump(dir, shift);
        if (g_recording.load())
            CapLog("  -> 进程命中原生按词跳转名单，直接 Ctrl+arrow");
        return -1;
    }
    int selEnd = caret;          /* 选区另一端（Edit 的 HIWORD / WPS 的 Selection.End），无选区=caret */
    if (focus) got = FastGetTextCaret(focus, &text, &len, &caret, &selEnd);  /* 仅经典 Edit/RichEdit */
    if (got) path = Chan::Edit;
    if (!got) {                                                     /* Scintilla 系 */
        got = SciGetTextCaret(focus, &text, &len, &caret, &sp);
        if (got) path = Chan::Sci;
    }
    if (!got) {
        got = UiaGetTextCaret(focus, &text, &len, &caret);   /* 现代控件 → UIA TextPattern */
        if (got) path = Chan::UIA;
    }
    if (!got) {
        got = MsaaGetTextCaret(focus, &text, &len, &caret);  /* 兜底无障碍 → IAccessible2 */
        if (got) path = Chan::IA2;
    }
    if (!got) {
        /* WPS：既无 UIA/IA2 文本也不响应 WM_GETTEXT，只能走 COM 自动化（见上节说明） */
        got = WpsGetTextCaret(focus, &text, &len, &caret, &selEnd);
        if (got) path = Chan::WPS;
    }
    if (!got) { SendNativeWordJump(dir, shift); if (g_recording.load()) CapLog("  -> 兜底 native Ctrl+arrow"); return -1; }

    /* Shift 位移的活动端：序列锚未建立时，向左扩活动端=选区 start、向右扩
       活动端=选区 end；锚已建立则活动端固定在锚的另一侧。
       适用条件 = 该通道读回的是一对**有序**的两端：Edit 的
       (LOWORD,HIGHWORD) 与 WPS 的 (Selection.Start,Selection.End) 都是。
       Sci 通道例外：SCI_GETCURRENTPOS 本身就是活动端，caret 直接可用。 */
    int activeCaret = caret;
    if (got && shift && (path == Chan::Edit || path == Chan::WPS)) {
        NAV_LOCK();
        if (g_selAnchor < 0 || g_selAnchorLen != len) {
            g_selAnchorIsStart = (dir == DIR_RIGHT);
            g_selAnchor = g_selAnchorIsStart ? caret : selEnd;
            g_selAnchorLen = len;
        }
        activeCaret = g_selAnchorIsStart ? selEnd : caret;
        NAV_UNLOCK();
    } else if (!shift) {
        NAV_LOCK();
        g_selAnchor = -1;
        g_selAnchorLen = -1;
        NAV_UNLOCK();
    }

    int steps = 0;
    int tgt = WordJumpTarget(text, len, activeCaret, dir, &steps);
    if (g_recording.load()) CapLog("  path=%s len=%d caret=%d -> steps=%d tgt=%d",
                            ChanName(path), len, activeCaret, steps, tgt);

    if (steps <= 0) {
        /* 已到边界，无可跳转；此时读文本已成功，只是边界无可跳。
           不再触发 native 兜底，避免多跳一次。 */
        free(text);
        return 0;
    }
    /* 能直接设光标的通道：距离较大时一步置到目标(瞬跳)，避免逐格发方向键
       拖慢；小距离仍走箭头以保持完全一致的键行为。 */
    BOOL direct = FALSE;
    BOOL skipArrows = FALSE;   /* 置位已发生但回读不符：不能再发箭头叠加错误 */
    WCHAR procName[64] = {0};
    if (focus) ConfigGetProcessName(focus, procName, 64);
    BOOL skipSetTry;
    NAV_LOCK();
    skipSetTry = (g_noSetProc[0] && procName[0] && !_wcsicmp(g_noSetProc, procName));
    NAV_UNLOCK();
    /* 期望区间（有序）：锚已建立时活动端=tgt、锚端固定；无 Shift 收缩为一点。
       Edit / WPS 两个"吃有序区间"的通道共用这一份计算（Sci 走字节域，自己算）。 */
    int ws, we;
    if (!shift) {
        ws = we = tgt;
    } else {
        NAV_LOCK();
        int anchor = g_selAnchor;      /* 在锁内取快照：别把锁带进下面的 IPC */
        NAV_UNLOCK();
        if (anchor >= 0) {
            ws = (tgt < anchor) ? tgt : anchor;
            we = (tgt < anchor) ? anchor : tgt;
        } else {
            ws = (dir == DIR_LEFT) ? tgt : caret;
            we = (dir == DIR_LEFT) ? selEnd : tgt;
        }
    }
    if (path == Chan::Edit && focus && steps >= DIRECT_JUMP_MIN && !AllowDirectSet(focus)
        && g_recording.load())
        CapLog("  -> 该控件禁用外部置位（只读不写：外部改选区会让它吞掉一次按键）"
               "，改走箭头/改道");
    if (path == Chan::Edit && focus && steps >= DIRECT_JUMP_MIN && AllowDirectSet(focus)) {
        direct = ClassicSetSel(focus, ws, we);
        if (direct) direct = EditVerifySel(focus, ws, we);
        if (direct) {
            /* EM_SETSEL 不会滚动视图：光标可能被移到窗口外看不到
               （EmEditor 实测）。EM_SCROLLCARET 把光标滚到可见，
               已在视图内时无操作；不实现该消息的控件会忽略它。 */
            LONGLONG dummy;
            SendMsg(focus, 0x00B7 /* EM_SCROLLCARET */, 0, 0, &dummy);
        }
        if (direct && g_recording.load()) CapLog("  -> 直接 EM_SETSEL [%d,%d] 已校验", ws, we);
    } else if (path == Chan::Sci && focus && steps >= DIRECT_JUMP_MIN) {
        LONGLONG tb = Utf16ToDocBytes(text, tgt, sp.cp);
        if (tb >= 0) {
            LONGLONG anchorB = shift ? sp.anchorBytes : tb;
            direct = SciSetSel(focus, tb, anchorB);
            if (direct && g_recording.load())
                CapLog("  -> 直接 SCI_SETSEL caret=%lld anchor=%lld", tb, anchorB);
        }
    } else if (path == Chan::WPS && focus) {
        /* WPS：Selection.SetRange(start, end) 一步到位（实测可用，且参数会自动
           有序化）。start == end 就是移光标 —— **不能**用 `Selection.Start = n`
           那招：它不折叠选区（实测原本 [12,12]，写 Start=5 之后变成 [5,12]）。

           什么时候必须用置位、不能用箭头：实测 WPS 的"活动端"概念和我们的模型
           **不一致** —— SetRange(a,b) 之后 WPS 认为活动端是 **a**，于是它那里的
           Shift+方向会去动另一端（实测 [12,31] 按 Shift+右 得到 [13,31]，动了
           起点而不是终点）；带选区时普通方向键往哪一端折叠也由它决定。所以：
             · 带 Shift（要精确控制选区两端）-> 必须置位；
             · 已有选区（caret != selEnd）-> 必须置位，否则箭头会先按 WPS 的
               规矩折叠选区，落点就与我们的计算差了整整一段；
             · 小距离且光标是折叠的（无选区）-> 箭头，行为与真实按键一致。
           返回值是三态而不是 bool：provider"报成功但没动"与"动到了别处"必须分开
           处理 —— 后者若再补箭头就是两个错误叠加。 */
        BOOL wpsDirect = (steps >= DIRECT_JUMP_MIN) || shift || (caret != selEnd);
        if (wpsDirect) {
            int r = WpsSetSel(focus, ws, we);
            if (r == WPS_SET_OK) {
                direct = TRUE;
                if (g_recording.load())
                    CapLog("  -> 直接 Selection.SetRange [%d,%d] 已校验", ws, we);
            } else if (r == WPS_SET_UNSURE) {
                skipArrows = TRUE;
                if (g_recording.load())
                    CapLog("  -> WPS 置位后回读不符（状态不明），放弃补箭头");
            } else if (g_recording.load()) {
                CapLog("  -> WPS 直接置位未生效（光标未动），退回逐格箭头");
            }
        } else if (g_recording.load()) {
            CapLog("  -> WPS 小距离且无选区：走逐格箭头");
        }
    } else if ((path == Chan::UIA || path == Chan::IA2) && focus && !shift
               && !skipSetTry && steps >= DIRECT_JUMP_MIN) {
        /* UIA/IA2 直接置位（TextRange.Select / setCaretOffset）仅限无 Shift：
           带选区时读回的 caret 是选区起点而非活动端，目标计算本身就不成立，
           置位会产生错误选区 —— Shift 位移走箭头（anchor 语义天然正确）。
           无 Shift 时试一次并回读校验：HRESULT 失败 -> 退回箭头；置位发生
           但回读不符 -> 不能再叠加箭头（会双重错误），本次到此为止。 */
        BOOL set = (path == Chan::UIA) ? UiaSetSel(focus, caret, tgt, FALSE)
                                    : MsaaSetSel(focus, caret, tgt, FALSE);
        if (set) {
            WCHAR* t2 = NULL; int l2 = 0, c2 = 0;
            BOOL ok2 = (path == Chan::UIA)
                     ? UiaGetTextCaret(focus, &t2, &l2, &c2)
                     : MsaaGetTextCaret(focus, &t2, &l2, &c2);
            if (ok2 && t2 && l2 == len && wcscmp(t2, text) == 0 && c2 == tgt) {
                direct = TRUE;
                NAV_LOCK();
                g_noSetProc[0] = 0; g_noSetCount = 0;
                NAV_UNLOCK();
                if (g_recording.load()) CapLog("  -> UIA/IA2 直接置位已校验 (caret=%d)", tgt);
            } else if (ok2 && t2 && l2 == len && wcscmp(t2, text) == 0 && c2 == caret) {
                /* Select 返回成功但光标未动（provider 静默忽略）：
                   光标仍在原位，从原 caret 算的 steps 依旧有效，安全退回箭头 */
                if (g_recording.load())
                    CapLog("  -> UIA Select 未生效(光标未动 c2=%d)，退回箭头。"
                           "嫌慢可把该程序加入 [NativeWordJump]", c2);
                /* 连续两次无效 -> 本次会话内不再尝试（WPF 上它会让显示光标
                   闪一下却不生效，每次试一次就每次都闪） */
                BOOL justDisabled = FALSE;
                NAV_LOCK();
                if (++g_noSetCount >= 2 && procName[0]) {
                    wcscpy_s(g_noSetProc, 64, procName);
                    justDisabled = TRUE;     /* 只在"刚跨过阈值"那一次记日志 */
                }
                NAV_UNLOCK();
                if (justDisabled && g_recording.load())
                    CapLog("  -> 该程序连续 2 次置位无效，本次会话内不再尝试"
                           "（消除多余闪烁，重启程序恢复）");
            } else {
                skipArrows = TRUE;
                if (g_recording.load())
                    CapLog("  -> UIA/IA2 置位后回读不符(caret=%d 期望 %d)，放弃补箭头",
                           ok2 ? c2 : -1, tgt);
            }
            free(t2);
        }
    }
    /* 超限与否由 SendArrows 在发送处判定（达到阈值即改道）。它的返回值说明
       本次走了哪条路，"改道"那一次**必须回读校验落点**：改道的前提是"目标
       程序自己的词切分 == 我们算的跳法"，而中文里两者经常不一致 —— 实测
       QQ(Chromium) 的 Ctrl+方向按 ICU 词典切、1~2 字一步，我们按"连续汉字
       段"整段算，于是期望 tgt=82 却停在 92、再按一次停在 90：光标落在整段
       中间，用户看到的就是"按词跳转落到段中间、左右不对称"。
       只有 OverStepMode=0 的"只发一格"不补正 —— 那是用户明确要的"跳一格
       即停"，补正一叠加就成了"跳过去又跳回来"。 */
    int sendMode = ARROW_SEND_STEPS;
    if (!direct && !skipArrows) sendMode = SendArrows(dir, steps, shift);
    BOOL handedOff = (sendMode == ARROW_SEND_HANDOFF);
    BOOL oneStep   = (sendMode == ARROW_SEND_ONE);
    /* 自校验补正：经典 Edit 通道能读回光标。实测 EmEditor 会在直接置位后
       吞掉第一支箭头（虚拟机里也可能丢键），导致逐格跳少走一格——
       读回实际位置，按差值补发（只补一轮、差值不超过原步数，防打转）。
       带 Shift 的选区移动不做补正（anchor 校验复杂，收益低）。
       补正一律走 SendArrowsStrict：步数是"还差多少"算出来的，再判一次超限
       就又改道一次、又落不到目标，而改道之后没人再校验。
       Scintilla 通道暂不做补正（SCI_GETCURRENTPOS 是字节域，回读后要反向
       换算成 UTF-16 才能和 tgt 比，现无该方向的换算函数）；记此备查。 */
    if (!direct && !oneStep && !shift && path == Chan::Edit && focus) {
        LONGLONG now = EditGetCaret(focus);
        if (now >= 0 && now == (LONGLONG)tgt) {
            if (handedOff && g_recording.load())
                CapLog("  -> 改道落点与目标一致 (caret=%lld)", now);
        } else if (now >= 0) {
            LONGLONG diff = (LONGLONG)tgt - now;
            if (diff > 0 && diff <= steps)
                SendArrowsStrict(DIR_RIGHT, (int)diff, FALSE);
            else if (diff < 0 && -diff <= steps)
                SendArrowsStrict(DIR_LEFT, (int)-diff, FALSE);
            if (g_recording.load())
                CapLog("  -> 箭头补正 %lld (caret=%lld -> %d)%s", diff, now, tgt,
                       handedOff ? " [改道落点不符，已补足]" : "");
        }
    }
    /* 自校验补正（UIA / IA2 / WPS）：部分自定义编辑器（Electron 里带渲染装饰的
       编辑器，如实时预览下把语法/装饰渲染成整块 widget 的编辑器）的无障碍
       文本含"光标不可停靠"的字符——按文本算出的步数发完箭头会整块跳过而
       超调（日志表现：短跳准、长跳偏）。跳完重读一次：
       仅当读回的仍是同一页文本（长度与内容都一致，坐标可比）才按差值补一轮；
       差值超过原步数说明坐标系统性不可靠，放弃补正（不做任何自动切换，
       只提示）—— 这类程序请把进程名写进配置文件的 [NativeWordJump] 名单。

       WPS 也走这一套，但**只在"改道"那一次**：我们发的是单字符箭头，落点由
       我们算出的字符位决定、本身是确定的；只有超过 MaxArrowSteps 改发原生
       Ctrl+方向时才依赖 WPS 自己的词切分 —— 那正是必须回读的情境。别的通道
       每次回读是因为它们的坐标本身不可靠。 */
    BOOL isUA  = (path == Chan::UIA || path == Chan::IA2);
    BOOL isWps = (path == Chan::WPS);
    if (!direct && !skipArrows && !oneStep && !shift && focus
        && (isUA || (isWps && handedOff))) {
        WCHAR* t2 = NULL; int l2 = 0, c2 = 0;
        BOOL ok2;
        if (isWps)                     ok2 = WpsGetTextCaret(focus, &t2, &l2, &c2, NULL);
        else if (path == Chan::UIA)    ok2 = UiaGetTextCaret(focus, &t2, &l2, &c2);
        else                           ok2 = MsaaGetTextCaret(focus, &t2, &l2, &c2);
        /* 只在"读回的仍是同一页文本"（长度与内容都一致）时才比坐标 */
        BOOL comparable = (ok2 && t2 && l2 == len && wcscmp(t2, text) == 0);
        if (comparable && c2 == tgt) {
            /* 改道的落点跟目标一致：说明这个程序的词切分和我们算的一致，
               不必再动它（只记一行日志，便于确认改道是否可信） */
            if (handedOff && g_recording.load())
                CapLog("  -> 改道落点与目标一致 (caret=%d)", c2);
        } else if (comparable) {
            int diff = tgt - c2;
            if (diff > 0 && diff <= steps)        SendArrowsStrict(DIR_RIGHT,  diff, FALSE);
            else if (diff < 0 && diff >= -steps)  SendArrowsStrict(DIR_LEFT,  -diff, FALSE);
            if (diff > steps || diff < -steps) {
                /* 偏差比整个步长还大：坐标系统性不可靠，补也补不准，
                   放弃本次补正（只提示，不做任何自动切换）。 */
                if (g_recording.load())
                    CapLog("  -> 文本坐标与实际光标不符(caret=%d 期望 %d)，差 %d 超出步数 %d，"
                           "放弃补正。建议把该程序加入 [NativeWordJump] 名单%s",
                           c2, tgt, diff, steps,
                           handedOff ? "（或把 MaxArrowSteps 调大）" : "");
            } else if (g_recording.load()) {
                CapLog("  -> 箭头补正 %d (caret=%d -> %d)%s", diff, c2, tgt,
                       handedOff ? " [改道落点不符，已补足]" : "");
            }
        }
        free(t2);
    }
    free(text);
    return steps;
}

/* ================= 诊断：探测当前焦点控件 =================
   把「前台窗口 / 焦点控件 / 能否读文本 / 光标 / 是否支持 UIA」写日志并弹窗，
   用于排查按词跳转为何不生效。 */
/* 安全地向 rep 追加格式化文本；超容量即截断并不再追加，避免越界。 */
static void RepAppend(WCHAR* rep, int cap, int* o, const WCHAR* fmt, ...) {
    if (!rep || !o || *o < 0 || *o >= cap || cap <= 0) return;
    va_list ap; va_start(ap, fmt);
    int n = _vsnwprintf_s(rep + *o, (size_t)(cap - *o), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) *o = cap;          /* 超过容量/编码错误：停写 */
    else *o += n;
}

/* ---- 启动时采集的系统/环境基线信息 ----
   启动即采集并保存在内存（不写日志文件），C+` 诊断时随探测一并输出，
   便于遇到无法使用的窗口时判断问题根源（位宽/权限/UIA 等）。 */
static WCHAR g_baseInfo[1800] = L"";
static volatile LONG g_baseReady = 0;

/* 提权判定统一用 config.c 的 IsRunElevated()（查进程令牌） */

void NavBaseInfo(void) {
    WCHAR b[1700] = L"";
    int o = 0;

    RepAppend(b, 1700, &o, L"进程位宽 = %d-bit\n", (int)(8 * sizeof(void*)));
    RepAppend(b, 1700, &o, L"钩子状态 = %s\n", g_hook ? L"已安装" : L"未安装");

    /* OS 版本：RtlGetVersion（GetVersionEx 受兼容层遮蔽，Win10+ 不可靠） */
    {
        typedef struct { ULONG cb; DWORD major, minor, build, plt; WCHAR csd[128]; } MYROV;
        typedef LONG (WINAPI *PGETV)(void*);
        /* ntdll 一定已加载，但 GetModuleHandleW 的返回值理论上可以是 NULL
           （/analyze 的 C6387 就是提这一条）；先取句柄再判空，
           免得把 NULL 喂给 GetProcAddress。诊断代码不该有任何崩溃可能。 */
        HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
        PGETV pGetV = hNtdll ? (PGETV)GetProcAddress(hNtdll, "RtlGetVersion") : NULL;
        if (pGetV) {
            MYROV ro; memset(&ro, 0, sizeof(ro)); ro.cb = sizeof(ro);
            if (pGetV(&ro) == 0)
                RepAppend(b, 1700, &o, L"系统版本 = %u.%u (build %u)\n",
                          (unsigned)ro.major, (unsigned)ro.minor,
                          (unsigned)(ro.build & 0xFFFF));
        }
    }
    RepAppend(b, 1700, &o, L"管理员特权 = %s\n", IsRunElevated() ? L"是" : L"否");

    /* 键盘布局主语言 / 用户 locale */
    RepAppend(b, 1700, &o, L"键盘布局(主语言) = 0x%04X  用户locale = 0x%04X\n",
              (unsigned)LOWORD(GetKeyboardLayout(0)), (unsigned)GetUserDefaultLCID());

    /* UIA 自动化是否可用 */
    {
        /* 顺带让本线程的 UIA 实例在启动时就建好，后续按词跳转直接用缓存 */
        BOOL uiaOK = (UiaGet() != NULL);
        RepAppend(b, 1700, &o, L"UIA 自动化可用 = %s\n", uiaOK ? L"是" : L"否");
    }

    /* 屏幕信息 */
    HDC dc = GetDC(NULL);
    RepAppend(b, 1700, &o, L"屏幕 = %d x %d  每英寸点数 = %d\n",
              GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
              dc ? GetDeviceCaps(dc, LOGPIXELSX) : 0);
    if (dc) ReleaseDC(NULL, dc);

    memcpy(g_baseInfo, b, ((size_t)o + 1) * sizeof(WCHAR));
    g_baseReady = 1;
}

void NavProbe(void) {
    HWND fg = GetForegroundWindow();
    WCHAR cls[96] = L"?"; WCHAR title[128] = L"?";
    HWND focus = NULL;
    if (fg) {
        DWORD tid = GetWindowThreadProcessId(fg, NULL);
        GUITHREADINFO gi; memset(&gi, 0, sizeof(gi)); gi.cbSize = sizeof(gi);
        if (tid && GetGUIThreadInfo(tid, &gi) && gi.hwndFocus) focus = gi.hwndFocus;
        GetClassNameW(fg, cls, 96);
        GetWindowTextW(fg, title, 128);
    }

    WCHAR rep[2800] = L"";
    int o = 0;
    if (g_baseReady)
        RepAppend(rep, 2800, &o, L"[启动基线]\n%ls\n----------\n", g_baseInfo);
    RepAppend(rep, 2800, &o,
        L"前台窗口: class=[%s] title=[%s]\n焦点控件 hwnd=0x%p\n",
        cls, title, (void*)focus);

    BOOL textOK = FALSE;
    if (focus) {
        WCHAR fcls[96]; GetClassNameW(focus, fcls, 96);
        RepAppend(rep, 2800, &o, L"焦点控件类名=[%s]\n", fcls);

        LONGLONG len = 0;
        SendMsg(focus, WM_GETTEXTLENGTH, 0, 0, &len);
        RepAppend(rep, 2800, &o, L"WM_GETTEXTLENGTH = %lld\n", len);
        if (len > 0 && len < 1 << 20) {
            /* 多留 2 个 slot：EmEditor 类控件 WM_GETTEXT 的返回计数含结尾 NUL
               （got 可能 = len+1），下面的 buf[got]=0 才不会越界写 */
            WCHAR* buf = (WCHAR*)malloc(((size_t)len + 2) * sizeof(WCHAR));
            if (buf) {
                LONGLONG got = 0;
                if (SendMsg(focus, WM_GETTEXT, (WPARAM)(len + 1), (LPARAM)buf, &got)
                    && got > 0) {
                    textOK = TRUE;
                    LONGLONG sel = 0;
                    SendMsg(focus, EM_GETSEL, 0, 0, &sel);
                    int caret = ((DWORD)sel != 0xFFFFFFFF) ? (int)LOWORD((DWORD)sel) : -1;
                    RepAppend(rep, 2800, &o,
                        L"EM_GETSEL(sel) = 0x%08llX  caret=%d\n文本长度=%lld\n",
                        (unsigned long long)(DWORD)sel, caret, got);
                    buf[got < 160 ? got : 160] = 0;
                    RepAppend(rep, 2800, &o, L"文本样本(前160) = [%s]\n", buf);
                }
                free(buf);
            }
        }
        if (!textOK)
            RepAppend(rep, 2800, &o, L"WM_GETTEXT/WM_GETSEL 不可用（非经典编辑控件）\n");
    }

    /* UIA 是否支持文本模式 */
    {
        IUIAutomation* ua = UiaGet();       /* 本线程缓存，不 Release */
        if (ua) {
            IUIAutomationElement* el = NULL;
            if (SUCCEEDED(IUIAutomation_GetFocusedElement(ua, &el)) && el) {
                IUIAutomationTextPattern* tp = NULL;
                BOOL txt = SUCCEEDED(IUIAutomationElement_GetCurrentPatternAs(el, UIA_TextPatternId,
                              IID_IUIAutomationTextPattern, (void**)&tp)) && tp;
                if (tp) IUIAutomationTextPattern_Release(tp);
                RepAppend(rep, 2800, &o,
                    L"UIA 焦点元素支持 TextPattern = %s\n", txt ? L"YES" : L"NO");
                IUIAutomationElement_Release(el);
            } else {
                RepAppend(rep, 2800, &o, L"UIA 取不到焦点元素\n");
            }
        } else {
            RepAppend(rep, 2800, &o, L"UIA 初始化失败\n");
        }
    }

    /* IA2 (IAccessibleText) 是否可用 —— Scintilla 系编辑器的通道 */
    if (focus) {
        IAccessible* acc = NULL;
        if (SUCCEEDED(AccessibleObjectFromWindow(focus, (DWORD)OBJID_CLIENT, IID_IAccessible, (void**)&acc)) && acc) {
            IAccessibleText* at = NULL;
            if (SUCCEEDED(acc->lpVtbl->QueryInterface(acc, kIID_IAccessibleText, (void**)&at)) && at) {
                long co = -1;
                BOOL ok2 = SUCCEEDED(at->lpVtbl->caretOffset(at, &co)) && co >= 0;
                RepAppend(rep, 2800, &o,
                    L"IA2 IAccessibleText = YES (caret=%ld)\n", co);
                at->lpVtbl->Release(at);
                if (ok2) textOK = TRUE;
            } else {
                RepAppend(rep, 2800, &o, L"IA2 IAccessibleText = NO\n");
            }
            acc->lpVtbl->Release(acc);
        } else {
            RepAppend(rep, 2800, &o, L"IA2 取不到 IAccessible\n");
        }
    }

    /* 只写日志。原来这里弹一个模态 MessageBox，三个问题：把和弦引擎和键盘
       响应一起冻住（探测期间再按 Caps+J 会异常）、2800 字符的文本在对话框里
       既读不完也复制不了、而它的内容本就已经全文写入日志了。
       UTF-8 缓冲区按最坏情况给（BMP 最多 3 字节/字符）：原来固定 3000 字节
       且不检查返回值 —— 内容含中文时转换会失败，utf 保持未初始化就被当
       字符串打印，等于把栈上的残留内容写进日志。 */
    CapLog("%S focus=0x%p class=[%S] textOK=%d", title, (void*)focus, cls, textOK);
    char utf[2800 * 3 + 1];
    int un = WideCharToMultiByte(CP_UTF8, 0, rep, -1, utf, (int)sizeof(utf), NULL, NULL);
    if (un > 0) CapLog("%s", utf);
    else        CapLog("(探测结果转 UTF-8 失败, err=%lu)", (unsigned long)GetLastError());
}