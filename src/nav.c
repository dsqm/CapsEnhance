#include "cap.h"

#define COBJMACROS   /* 启用 UIA 的 C 风格 *_宏接口 */
#include <initguid.h>  /* 在本 TU 内为 UIA 的 CLSID/IID 提供存储定义 */

#include <oleauto.h>
#include <oleacc.h>
#include <uiautomationcore.h>
#include <uiautomationclient.h>
#include <ole2.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <wchar.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "oleacc.lib")

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


/* ---- 跨进程发消息：取窗口过程的"真实返回值" ----
   坑：SendMessageTimeout 自己的返回值只表示"有没有发成功(非0)"，窗口过程的
   返回值写进最后一个参数 lpdwResult。之前所有调用都传 NULL，于是拿到的恒为 1
   —— 表现为 WM_GETTEXTLENGTH 恒等于 1、读到的文本恒被截成一个字符，
   词边界算法只能在"长度 1 的文本"上算，自然跳不动。 */
static BOOL SendMsg(HWND hwnd, UINT msg, WPARAM w, LPARAM l, LONGLONG* out) {
    DWORD_PTR r = 0;
    if (!SendMessageTimeoutW(hwnd, msg, w, l,
                             SMTO_ABORTIFHUNG | SMTO_BLOCK, 300, &r))
        return FALSE;
    if (out) *out = (LONGLONG)(LONG_PTR)r;
    return TRUE;
}

/* 经典 Edit 控件直接置光标（或选区）位置。若可成功则无需发送箭头，瞬跳。
   无 Shift：光标瞬移到 target；有 Shift：从当前光标(anchor)选到 target。 */
static BOOL ClassicSetSel(HWND hwnd, int caret, int target, BOOL shift) {
    if (shift)
        return SendMsg(hwnd, EM_SETSEL, (WPARAM)caret, (LPARAM)target, NULL);
    return SendMsg(hwnd, EM_SETSEL, (WPARAM)target, (LPARAM)target, NULL);
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
static BOOL EditVerifySel(HWND hwnd, int caret, int target, BOOL shift) {
    LONGLONG sel = 0;
    if (!SendMsg(hwnd, EM_GETSEL, 0, 0, &sel)) return FALSE;
    DWORD v = (DWORD)sel;
    int start = (int)LOWORD(v), end = (int)HIWORD(v);
    return (end == target) && (shift ? (start == caret) : (start == target));
}

/* 读回光标位置（EM_GETSEL 的 LOWORD）；读不到返回 -1 */
static LONGLONG EditGetCaret(HWND hwnd) {
    LONGLONG sel = 0;
    if (!SendMsg(hwnd, EM_GETSEL, 0, 0, &sel)) return -1;
    return (LONGLONG)LOWORD((DWORD)sel);
}

/* 直接置光标：SCI_SETSEL(caret, anchor)。置完立即回读校验，
   位置不符就返回 FALSE，让上层退回逐格箭头。 */
static BOOL SciSetSel(HWND hwnd, LONGLONG caretB, LONGLONG anchorB) {
    LONGLONG now = 0;
    SendMsg(hwnd, SCI_SETSEL, (WPARAM)caretB, (LPARAM)anchorB, NULL);
    SendMsg(hwnd, SCI_GETCURRENTPOS, 0, 0, &now);
    SendMsg(hwnd, SCI_SCROLLCARET, 0, 0, NULL);
    return now == caretB;
}

/* 读 Scintilla 全文 + 光标（UTF-16 域），并给出字节域位置供瞬跳 */
static BOOL SciGetTextCaret(HWND hwnd, WCHAR** text, int* lenOut, int* caretOut,
                            SciPos* pos) {
    if (!hwnd || !IsScintilla(hwnd)) return FALSE;

    LONGLONG n = 0;
    if (!SendMsg(hwnd, SCI_GETTEXTLENGTH, 0, 0, &n)) return FALSE;
    if (n <= 0 || n > SCI_MAX_BYTES) return FALSE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return FALSE;
    /* 只要"在对方进程开内存 + 读回来"两项权限；权限不够就整条通道放弃 */
    HANDLE hp = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ, FALSE, pid);
    if (!hp) return FALSE;

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
   必须当作"一个字符"整体推进，否则会把它们拆散导致跳词错乱。 */
static BOOL IsHighSurrogateW(WCHAR c) { return c >= 0xD800 && c <= 0xDBFF; }
static BOOL IsLowSurrogateW(WCHAR c)  { return c >= 0xDC00 && c <= 0xDFFF; }

/* 字符类别：决定跳词边界。连续同类字符串视为"一个词"，
   跳词就停在这些词(与词/分隔符)的边界处。
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

/* 词跳核心：返回跳转后光标应处的 code-unit 位置，同时写出"需发送的箭头次数"。
   左右箭头在编辑框内按"字符"（code point）移动，因此步数(箭头数)按字符单位计数，
   代理对整体只计 1 步；目标 code-unit 位置供经典 Edit 直接置光标(瞬跳)。
   步数由 target 与 caret 的字符数差得到，与逐箭头移动完全等价。

   行边界规则：词跳默认不跨行（\r/\n 是分隔符，但分隔符串只在行内跨）。
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

/* 非 static 仅供词跳单元测试（test_nav.c）直接调用。 */
int ComputeSteps(const WCHAR* s, int len, int caret, int dir) {
    int steps = 0;
    WordJumpTarget(s, len, caret, dir, &steps);
    return steps;
}

/* 取当前前台窗口的焦点控件 */
static HWND GetFocusControl(void) {
    HWND fg = GetForegroundWindow();
    if (!fg) return NULL;
    DWORD tid = GetWindowThreadProcessId(fg, NULL);
    GUITHREADINFO gi;
    memset(&gi, 0, sizeof(gi));
    gi.cbSize = sizeof(gi);
    if (tid && GetGUIThreadInfo(tid, &gi) && gi.hwndFocus)
        return gi.hwndFocus;
    return NULL;
}

/* 是否"经典 Edit 协议"控件（可靠支持 WM_GETTEXT + EM_GETSEL）。
   白名单外不放行：Chromium 系窗口对 WM_GETTEXT 回的是标题、EM_GETSEL 是
   垃圾值，混进来会把跳词带偏。EmEditor 的 EmEditorView 实测完美支持这套
   协议（文本/光标都是 UTF-16 下标），只是类名不在经典名单里。 */
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
static BOOL FastGetTextCaret(HWND hwnd, WCHAR** text, int* lenOut, int* caretOut) {
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
       多出的 '\0' 会被当成一个分隔符参与跳词，算出越界目标（tgt=文档长+1），
       必须剥掉。 */
    while (got > 0 && buf[got - 1] == L'\0') got--;
    if (got <= 0) { free(buf); return FALSE; }

    /* 光标位置：EM_GETSEL */
    LONGLONG sel = 0;
    if (!SendMsg(hwnd, EM_GETSEL, 0, 0, &sel)) { free(buf); return FALSE; }
    int caret = ((DWORD)sel != 0xFFFFFFFF) ? (int)LOWORD((DWORD)sel) : 0;
    if (caret < 0 || caret > (int)got) caret = (int)got;

    *text = buf;
    *lenOut = (int)got;
    *caretOut = caret;
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
                            &IID_IUIAutomationTextPattern, (void**)&t)) && t) {
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
    for (int i = 0; i < n; i++)
        if (chain[i] && chain[i] != result) IUIAutomationElement_Release(chain[i]);
    return result;
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
                    &IID_IUIAutomationTextPattern, (void**)&t)) && t) {
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
    if (g_recording)
        CapLog("  UIA %-26s hr=0x%08lX %s", what, (unsigned long)hr,
               SUCCEEDED(hr) ? "OK" : "FAIL");
}

static BOOL UiaGetTextCaret(HWND hwnd, WCHAR** text, int* lenOut, int* caretOut) {
    HRESULT hr;
    BSTR all = NULL, pre = NULL;
    IUIAutomation         *ua  = NULL;
    IUIAutomationElement  *el  = NULL;
    IUIAutomationTextPattern *tp = NULL;
    IUIAutomationTextRange *doc = NULL, *caret = NULL, *tmp = NULL;
    BOOL ok = FALSE;

    hr = CoInitialize(NULL);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return FALSE;

    hr = CoCreateInstance(&CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IUIAutomation, (void**)&ua);
    UiaLog("CoCreateInstance", hr);
    if (FAILED(hr) || !ua) { CoUninitialize(); return FALSE; }

    /* 锚定候选：优先 UIA 焦点元素（GetFocusedElement 能命中 Chromium/WinUI 的
       真正文本框），取不到 TextPattern 时再回退 ElementFromHandle(hwnd)。
       曾改为 ElementFromHandle 直取顶层窗口，但 Chromium(Chrome_WidgetWin_1)
       顶层窗口本身无 TextPattern，向上也找不到 → 造成 QQ/Edge 跳词回归。
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
                    &IID_IUIAutomationTextPattern, (void**)&tp);
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
                            &IID_IUIAutomationTextPattern2, (void**)&tp2)) && tp2) {
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
                if (g_recording)
                    CapLog("  UIA doc text len=%d -> a11y 占位。VSCode 请在 settings.json "
                           "设 \"editor.accessibilitySupport\": \"on\" 后即可用词跳引擎", alen);
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
    if (ua)   IUIAutomation_Release(ua);
    CoUninitialize();
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
    IUIAutomationTextRange *doc = NULL, *caretR = NULL, *r = NULL;
    BOOL ok = FALSE;
    int delta = target - caret;
    if (delta == 0) return TRUE;                       /* 无需移动 */

    hr = CoInitialize(NULL);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return FALSE;
    hr = CoCreateInstance(&CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IUIAutomation, (void**)&ua);
    if (FAILED(hr) || !ua) { CoUninitialize(); return FALSE; }

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
                    &IID_IUIAutomationTextPattern, (void**)&tp);
    if (FAILED(hr) || !tp) goto done;
    hr = IUIAutomationTextPattern_get_DocumentRange(tp, &doc);
    if (FAILED(hr) || !doc) goto done;

    {
        IUIAutomationTextPattern2* tp2 = NULL;
        if (SUCCEEDED(IUIAutomationElement_GetCurrentPatternAs(el, UIA_TextPattern2Id,
                            &IID_IUIAutomationTextPattern2, (void**)&tp2)) && tp2) {
            BOOL active = FALSE;
            if (FAILED(IUIAutomationTextPattern2_GetCaretRange(tp2, &active, &caretR)) || !caretR)
                caretR = NULL;
            IUIAutomationTextPattern2_Release(tp2);
        }
    }
    if (!caretR) {
        IUIAutomationTextRangeArray* sel = NULL;
        if (SUCCEEDED(IUIAutomationTextPattern_GetSelection(tp, &sel)) && sel) {
            int n = 0;
            sel->lpVtbl->get_Length(sel, &n);
            if (n > 0) sel->lpVtbl->GetElement(sel, 0, &caretR);
            sel->lpVtbl->Release(sel);
        }
    }
    if (!caretR) goto done;

    if (FAILED(IUIAutomationTextRange_Clone(doc, &r)) || !r) goto done;
    /* 折叠 r 到 caret（两端都对齐 caretR 的 Start，与读取通道同源） */
    if (FAILED(IUIAutomationTextRange_MoveEndpointByRange(r,
                TextPatternRangeEndpoint_End, caretR, TextPatternRangeEndpoint_Start)) ||
        FAILED(IUIAutomationTextRange_MoveEndpointByRange(r,
                TextPatternRangeEndpoint_Start, caretR, TextPatternRangeEndpoint_Start)))
        goto done;

    /* 展开 delta 个字符到目标端；实际移动数不符（provider 不支持/越界）则放弃 */
    {
        long moved = 0;
        int ep = (delta < 0) ? TextPatternRangeEndpoint_Start
                             : TextPatternRangeEndpoint_End;
        hr = IUIAutomationTextRange_MoveEndpointByUnit(r, ep, TextUnit_Character,
                                                       (long)delta, &moved);
        if (FAILED(hr) || moved != (long)(delta < 0 ? -delta : delta)) goto done;
    }
    if (!shift) {
        /* 无 Shift：折叠到目标端点，Select 后光标落在目标 */
        hr = (delta < 0)
           ? IUIAutomationTextRange_MoveEndpointByRange(r, TextPatternRangeEndpoint_End,
                                                        r, TextPatternRangeEndpoint_Start)
           : IUIAutomationTextRange_MoveEndpointByRange(r, TextPatternRangeEndpoint_Start,
                                                        r, TextPatternRangeEndpoint_End);
        if (FAILED(hr)) goto done;
    }
    /* 带 Shift：保留 [caret,target] 展开区间，Select 产生选区 */
    hr = IUIAutomationTextRange_Select(r);
    ok = SUCCEEDED(hr);
    if (g_recording)
        CapLog("  UIA Select hr=0x%08lX delta=%d shift=%d",
               (unsigned long)hr, delta, shift ? 1 : 0);

done:
    if (r)      IUIAutomationTextRange_Release(r);
    if (caretR) IUIAutomationTextRange_Release(caretR);
    if (doc)    IUIAutomationTextRange_Release(doc);
    if (tp)     IUIAutomationTextPattern_Release(tp);
    if (el)     IUIAutomationElement_Release(el);
    if (ua)     IUIAutomation_Release(ua);
    CoUninitialize();
    return ok;
}

/* ---- IA2 直接置位：setCaretOffset / setSelection ----
   官方 IAccessible2 文档：setCaretOffset "Sets the position of the caret"，
   不支持时返回 E_FAIL（此时退回箭头）。 */
static BOOL MsaaSetSel(HWND hwnd, int caret, int target, BOOL shift) {
    if (!hwnd) return FALSE;
    IAccessible* acc = NULL;
    HRESULT hr = AccessibleObjectFromWindow(hwnd, OBJID_CLIENT,
                                            &IID_IAccessible, (void**)&acc);
    if (FAILED(hr) || !acc) return FALSE;
    IAccessibleText* at = NULL;
    hr = acc->lpVtbl->QueryInterface(acc, &kIID_IAccessibleText, (void**)&at);
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
    HRESULT hr = AccessibleObjectFromWindow(hwnd, OBJID_CLIENT, &IID_IAccessible, (void**)&acc);
    if (FAILED(hr) || !acc) return FALSE;

    IAccessibleText* at = NULL;
    hr = acc->lpVtbl->QueryInterface(acc, &kIID_IAccessibleText, (void**)&at);
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
        if (g_recording) CapLog("  IA2 text len=%d -> 占位文本，弃用走原生", len);
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

int NavJump(int dir, BOOL shift) {
    WCHAR* text = NULL;
    int len = 0, caret = 0;
    if (g_recording) CapLog("NavJump dir=%d shift=%d", dir, shift);

    BOOL got = FALSE;
    const char* path = "native";
    SciPos sp;
    memset(&sp, 0, sizeof(sp));
    HWND focus = GetFocusControl();
    /* 配置名单内的程序（如 VSCode）：跳过全部文本探测，直接原生 Ctrl+方向 */
    if (focus && ConfigIsNativeWordJump(focus)) {
        SendNativeWordJump(dir, shift);
        if (g_recording)
            CapLog("  -> 进程命中原生词跳名单，直接 Ctrl+arrow");
        return -1;
    }
    if (focus) got = FastGetTextCaret(focus, &text, &len, &caret);  /* 仅经典 Edit/RichEdit */
    if (got) path = "Edit";
    if (!got) {                                                     /* Scintilla 系 */
        got = SciGetTextCaret(focus, &text, &len, &caret, &sp);
        if (got) path = "Sci";
    }
    if (!got) {
        got = UiaGetTextCaret(focus, &text, &len, &caret);   /* 现代控件 → UIA TextPattern */
        if (got) path = "UIA";
    }
    if (!got) {
        got = MsaaGetTextCaret(focus, &text, &len, &caret);  /* 兜底无障碍 → IAccessible2 */
        if (got) path = "IA2";
    }
    if (!got) { SendNativeWordJump(dir, shift); if (g_recording) CapLog("  -> 兜底 native Ctrl+arrow"); return -1; }

    int steps = 0;
    int tgt = WordJumpTarget(text, len, caret, dir, &steps);
    if (g_recording) CapLog("  path=%s len=%d caret=%d -> steps=%d tgt=%d",
                            path, len, caret, steps, tgt);

    if (steps <= 0) {
        /* 已到边界，无可跳转；此时读文本已成功，只是边界无可跳。
           不再触发 native 兜底，避免多跳一次。 */
        free(text);
        return 0;
    }
    /* 能直接设光标的通道（经典 Edit / Scintilla）：距离较大时一步置到目标(瞬跳)，
       避免逐格发方向键拖慢；小距离仍走箭头以保持完全一致的键行为。
       UIA/IA2 通道无法直接设光标，一律发箭头。 */
    BOOL direct = FALSE;
    BOOL skipArrows = FALSE;   /* 置位已发生但回读不符：不能再发箭头叠加错误 */
    if (path[0] == 'E' && focus && steps >= DIRECT_JUMP_MIN) {
        direct = ClassicSetSel(focus, caret, tgt, shift);
        if (direct) direct = EditVerifySel(focus, caret, tgt, shift);
        if (direct && g_recording) CapLog("  -> 直接 EM_SETSEL [%d,%d] 已校验", caret, tgt);
    } else if (path[0] == 'S' && focus && steps >= DIRECT_JUMP_MIN) {
        LONGLONG tb = Utf16ToDocBytes(text, tgt, sp.cp);
        if (tb >= 0) {
            LONGLONG anchorB = shift ? sp.anchorBytes : tb;
            direct = SciSetSel(focus, tb, anchorB);
            if (direct && g_recording)
                CapLog("  -> 直接 SCI_SETSEL caret=%lld anchor=%lld", tb, anchorB);
        }
    } else if ((path[0] == 'U' || path[0] == 'I') && focus && steps >= DIRECT_JUMP_MIN) {
        /* UIA/IA2 同样有"直接置位"能力（TextRange.Select / setCaretOffset）。
           试一次并回读校验：HRESULT 失败 -> 退回箭头；置位发生但回读不符 ->
           不能再叠加箭头（会双重错误），本次到此为止。 */
        BOOL set = (path[0] == 'U') ? UiaSetSel(focus, caret, tgt, shift)
                                    : MsaaSetSel(focus, caret, tgt, shift);
        if (set) {
            WCHAR* t2 = NULL; int l2 = 0, c2 = 0;
            BOOL ok2 = (path[0] == 'U')
                     ? UiaGetTextCaret(focus, &t2, &l2, &c2)
                     : MsaaGetTextCaret(focus, &t2, &l2, &c2);
            if (ok2 && t2 && l2 == len && wcscmp(t2, text) == 0 && c2 == tgt) {
                direct = TRUE;
                if (g_recording) CapLog("  -> UIA/IA2 直接置位已校验 (caret=%d)", tgt);
            } else {
                skipArrows = TRUE;
                if (g_recording)
                    CapLog("  -> UIA/IA2 置位后回读不符(caret=%d 期望 %d)，放弃补箭头",
                           ok2 ? c2 : -1, tgt);
            }
            free(t2);
        }
    }
    if (!direct && !skipArrows) SendArrows(dir, steps, shift);
    /* 自校验补正：经典 Edit 通道能读回光标。实测 EmEditor 会在直接置位后
       吞掉第一支箭头（虚拟机里也可能丢键），导致逐格跳少走一格——
       读回实际位置，按差值补发（只补一轮、差值不超过原步数，防打转）。
       带 Shift 的选区移动不做补正（anchor 校验复杂，收益低）。 */
    if (!direct && !shift && path[0] == 'E' && focus) {
        LONGLONG now = EditGetCaret(focus);
        if (now >= 0 && now != (LONGLONG)tgt) {
            LONGLONG diff = (LONGLONG)tgt - now;
            if (diff > 0 && diff <= steps)
                SendArrows(DIR_RIGHT, (int)diff, FALSE);
            else if (diff < 0 && -diff <= steps)
                SendArrows(DIR_LEFT, (int)-diff, FALSE);
            if (g_recording)
                CapLog("  -> 箭头补正 %lld (caret=%lld -> %d)", diff, now, tgt);
        }
    }
    /* 自校验补正（UIA / IA2）：部分自定义编辑器（Electron 里带渲染装饰的
       编辑器，如实时预览下把语法/装饰渲染成整块 widget 的编辑器）的无障碍
       文本含"光标不可停靠"的字符——按文本算出的步数发完箭头会整块跳过而
       超调（日志表现：短跳准、长跳偏）。跳完重读一次：
       仅当读回的仍是同一页文本（长度与内容都一致，坐标可比）才按差值补一轮；
       差值超过原步数说明坐标系统性不可靠，放弃补正（不做任何自动切换，
       只提示）—— 这类程序请把进程名写进配置文件的 [NativeWordJump] 名单。 */
    if (!direct && !skipArrows && !shift && (path[0] == 'U' || path[0] == 'I') && focus) {
        WCHAR* t2 = NULL; int l2 = 0, c2 = 0;
        BOOL ok2 = (path[0] == 'U') ? UiaGetTextCaret(focus, &t2, &l2, &c2)
                                    : MsaaGetTextCaret(focus, &t2, &l2, &c2);
        if (ok2 && t2 && l2 == len && wcscmp(t2, text) == 0 && c2 != tgt) {
            int diff = tgt - c2;
            if (diff > 0 && diff <= steps)        SendArrows(DIR_RIGHT,  diff, FALSE);
            else if (diff < 0 && diff >= -steps)  SendArrows(DIR_LEFT,  -diff, FALSE);
            if (diff > steps || diff < -steps) {
                /* 偏差比整个步长还大：坐标系统性不可靠，补也补不准，
                   放弃本次补正（只提示，不做任何自动切换）。 */
                if (g_recording)
                    CapLog("  -> 文本坐标与实际光标不符(caret=%d 期望 %d)，差 %d 超出步数 %d，"
                           "放弃补正。建议把该程序加入 [NativeWordJump] 名单",
                           c2, tgt, diff, steps);
            } else if (g_recording) {
                CapLog("  -> 箭头补正 %d (caret=%d -> %d)", diff, c2, tgt);
            }
        }
        free(t2);
    }
    free(text);
    return steps;
}

/* ================= 诊断：探测当前焦点控件 =================
   把「前台窗口 / 焦点控件 / 能否读文本 / 光标 / 是否支持 UIA」写日志并弹窗，
   用于排查跳词为何不生效。 */
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
        PGETV pGetV = (PGETV)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
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
        BOOL uiaOK = FALSE;
        HRESULT hr = CoInitialize(NULL);
        if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) {
            IUIAutomation* ua = NULL;
            if (SUCCEEDED(CoCreateInstance(&CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER,
                                           &IID_IUIAutomation, (void**)&ua)) && ua) {
                uiaOK = TRUE;
                IUIAutomation_Release(ua);
            }
            CoUninitialize();
        }
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
    HRESULT hr = CoInitialize(NULL);
    if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) {
        IUIAutomation* ua = NULL;
        if (SUCCEEDED(CoCreateInstance(&CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER,
                                       &IID_IUIAutomation, (void**)&ua)) && ua) {
            IUIAutomationElement* el = NULL;
            if (SUCCEEDED(IUIAutomation_GetFocusedElement(ua, &el)) && el) {
                IUIAutomationTextPattern* tp = NULL;
                BOOL txt = SUCCEEDED(IUIAutomationElement_GetCurrentPatternAs(el, UIA_TextPatternId,
                              &IID_IUIAutomationTextPattern, (void**)&tp)) && tp;
                if (tp) IUIAutomationTextPattern_Release(tp);
                RepAppend(rep, 2800, &o,
                    L"UIA 焦点元素支持 TextPattern = %s\n", txt ? L"YES" : L"NO");
                IUIAutomationElement_Release(el);
            } else {
                RepAppend(rep, 2800, &o, L"UIA 取不到焦点元素\n");
            }
            IUIAutomation_Release(ua);
        } else {
            RepAppend(rep, 2800, &o, L"UIA 初始化失败\n");
        }
        CoUninitialize();
    }

    /* IA2 (IAccessibleText) 是否可用 —— Scintilla 系编辑器的通道 */
    if (focus) {
        IAccessible* acc = NULL;
        if (SUCCEEDED(AccessibleObjectFromWindow(focus, OBJID_CLIENT, &IID_IAccessible, (void**)&acc)) && acc) {
            IAccessibleText* at = NULL;
            if (SUCCEEDED(acc->lpVtbl->QueryInterface(acc, &kIID_IAccessibleText, (void**)&at)) && at) {
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

    /* 写日志（UTF-8）并存一份供弹窗 */
    CapLog("%S focus=0x%p class=[%S] textOK=%d", title, (void*)focus, cls, textOK);
    char utf[3000];
    WideCharToMultiByte(CP_UTF8, 0, rep, -1, utf, 3000, NULL, NULL);
    CapLog("%s", utf);

    MessageBoxW(NULL, rep, L"CapsEnhance 焦点探测", MB_OK);
}