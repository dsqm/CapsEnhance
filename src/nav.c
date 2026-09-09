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
#include <wchar.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "oleacc.lib")

#define SAFE_RELEASE(p) do { if (p) { (p)->Release(); (p)=NULL; } } while(0)

/* 经典 Edit 直接置光标的最少字符步数：小于该值用真实箭头（行为一致），
   等于或大于则用 EM_SETSEL 一步跳(瞬跳)，避免远距离逐格发送方向键拖慢。 */
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
} IAccessibleTextVtbl;
struct IAccessibleText { const IAccessibleTextVtbl* lpVtbl; };

static const IID kIID_IAccessibleText =
    {0x24FD2FFB, 0x3AAD, 0x4a08, {0x83,0x35,0xA3,0xAD,0x89,0xC0,0xFB,0x4B}};


/* 经典 Edit 控件直接置光标（或选区）位置。若可成功则无需发送箭头，瞬跳。 */
static BOOL ClassicSetSel(HWND hwnd, int start, int end, BOOL shift) {
    if (!shift) end = start;
    LRESULT ok = SendMessageTimeoutW(hwnd, EM_SETSEL, (WPARAM)start, (LPARAM)end,
                                  SMTO_ABORTIFHUNG | SMTO_BLOCK, 300, NULL);
    if (ok == 0) return FALSE;
    return TRUE;
}

/* 逐方向键移动：发送 n 个左/右箭头（可带 Shift 位移）。
   每个箭头单独 SendInput 并微小间隔，避免大批量 SendInput 在部分
   程序中只认第一个键；Shift 在整个跳词期间持续按住以维持选区。 */
static void SendArrows(int dir, int n, BOOL shift) {
    WORD vk = (dir == DIR_LEFT) ? VK_LEFT : VK_RIGHT;
    WORD scd = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    INPUT kd, ku, sd, su;
    memset(&kd, 0, sizeof(kd)); kd.type = INPUT_KEYBOARD;
    kd.ki.wVk = vk; kd.ki.wScan = scd;
    ku = kd; ku.ki.dwFlags = KEYEVENTF_KEYUP;
    if (shift) {
        memset(&sd, 0, sizeof(sd)); sd.type = INPUT_KEYBOARD;
        sd.ki.wVk = VK_LSHIFT;
        sd.ki.wScan = (WORD)MapVirtualKeyW(VK_LSHIFT, MAPVK_VK_TO_VSC);
        su = sd; su.ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(1, &sd, sizeof(INPUT));
    }
    for (int i = 0; i < n && i < 64; i++) {
        SendInput(1, &kd, sizeof(INPUT));
        SendInput(1, &ku, sizeof(INPUT));
        Sleep(8);
    }
    if (shift) SendInput(1, &su, sizeof(INPUT));
}

/* 兜底：文字读取失败时，用系统原生 Ctrl+/Shift+ 方向键做词跳 */
static void SendNativeWordJump(int dir, BOOL shift) {
    WORD vk = (dir == DIR_LEFT) ? VK_LEFT : VK_RIGHT;
    INPUT ki[9];
    memset(ki, 0, sizeof(ki));
    int k = 0;
    if (shift) {
        ki[k].type = INPUT_KEYBOARD; ki[k].ki.wVk = VK_LSHIFT;
        ki[k].ki.wScan = (WORD)MapVirtualKeyW(VK_LSHIFT, MAPVK_VK_TO_VSC); k++;
    }
    ki[k].type = INPUT_KEYBOARD; ki[k].ki.wVk = VK_LCONTROL;
    ki[k].ki.wScan = (WORD)MapVirtualKeyW(VK_LCONTROL, MAPVK_VK_TO_VSC); k++;
    ki[k] = ki[k-1]; ki[k].ki.dwFlags = KEYEVENTF_KEYUP; k++;
    ki[k].type = INPUT_KEYBOARD; ki[k].ki.wVk = vk;
    ki[k].ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC); k++;
    ki[k] = ki[k-1]; ki[k].ki.dwFlags = KEYEVENTF_KEYUP; k++;
    /* Ctrl 释放 */
    ki[k].type = INPUT_KEYBOARD; ki[k].ki.wVk = VK_LCONTROL;
    ki[k].ki.wScan = (WORD)MapVirtualKeyW(VK_LCONTROL, MAPVK_VK_TO_VSC);
    ki[k].ki.dwFlags = KEYEVENTF_KEYUP; k++;
    if (shift) {
        ki[k].type = INPUT_KEYBOARD; ki[k].ki.wVk = VK_LSHIFT;
        ki[k].ki.wScan = (WORD)MapVirtualKeyW(VK_LSHIFT, MAPVK_VK_TO_VSC);
        ki[k].ki.dwFlags = KEYEVENTF_KEYUP; k++;
    }
    SendInput(k, ki, sizeof(INPUT));
}

/* 代理对判定：补充平面字符用两个 UTF-16 code unit 表示（高+低代理），
   必须当作"一个字符"整体推进，否则会把它们拆散导致跳词错乱。 */
static BOOL IsHighSurrogateW(WCHAR c) { return c >= 0xD800 && c <= 0xDBFF; }
static BOOL IsLowSurrogateW(WCHAR c)  { return c >= 0xDC00 && c <= 0xDFFF; }

/* 词字符判定（BMP 直接判，编码点 >= 0x10000 的取其整体）：
   CJK 与字母数字视为"词内"，其余（空白+标点）为分隔符。 */
static BOOL IsWordChar(WCHAR c) {
    unsigned cp = (unsigned)c;
    if (c == L'\0') return FALSE;
    if (IsCharAlphaNumericW(c)) return TRUE;
    if (cp >= 0x3400 && cp <= 0x4DBF) return TRUE;   /* 扩展A */
    if (cp >= 0x4E00 && cp <= 0x9FFF) return TRUE;   /* 基本区 */
    if (cp >= 0xF900 && cp <= 0xFAFF) return TRUE;   /* 兼容区 */
    return FALSE;
}

/* 以 s[i] 为"字符起点"（可能是一个代理对的首字）判定是否词内。 */
static BOOL IsWordCharAt(const WCHAR* s, int len, int i) {
    if (i < 0 || i >= len) return FALSE;
    unsigned cp;
    if (i + 1 < len && IsHighSurrogateW(s[i]) && IsLowSurrogateW(s[i+1])) {
        cp = 0x10000u + (((unsigned)(s[i] - 0xD800) << 10) | (unsigned)(s[i+1] - 0xDC00));
    } else if (IsHighSurrogateW(s[i]) || IsLowSurrogateW(s[i])) {
        return FALSE;   /* 孤立的半个代理对：按分隔符处理，避免拆散群组 */
    } else {
        cp = (unsigned)s[i];
    }
    if (cp == 0) return FALSE;
    if (cp > 0xFFFF) {
        /* 补充平面：CJK 扩展 B~H 视为"字内"，其余 Astral 字符按分隔符 */
        return (cp >= 0x20000 && cp <= 0x323AF);
    }
    return IsWordChar((WCHAR)cp);
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
   步数由 target 与 caret 的字符数差得到，与逐箭头移动完全等价。 */
static int WordJumpTarget(const WCHAR* s, int len, int caret, int dir, int* steps) {
    *steps = 0;
    if (!s || len < 1) return caret;
    /* 归一光标：落在代理对中间(异常，不该发生)时，视为整个字符未跨过 */
    if (caret > 0 && caret < len && IsHighSurrogateW(s[caret-1]) && IsLowSurrogateW(s[caret]))
        caret--;
    if (caret < 0) caret = 0;
    if (caret > len) caret = len;

    int i = caret, st = 0;
    if (dir == DIR_LEFT) {
        while (i > 0) {                    /* 跨结尾分隔符 */
            int stt = CharStartBefore(s, len, i);
            if (IsWordCharAt(s, len, stt)) break;
            st++; i = stt;
        }
        while (i > 0) {                    /* 跨整个词 */
            int stt = CharStartBefore(s, len, i);
            if (!IsWordCharAt(s, len, stt)) break;
            st++; i = stt;
        }
    } else {
        /* 右跳：用一个循环同时处理"跨当前词"和"跨分隔符到下一词首"，
           避免拆成两个 for 段时第二段被跳过。 */
        int skw = 1;                     /* 1=还在跨词；0=已转入跨分隔符 */
        for (; i < len; ) {
            int w = IsWordCharAt(s, len, i);
            if (skw) {
                if (!w) skw = 0;         /* 词结束，转入跨分隔符 */
            } else if (w) {
                break;                   /* 遇下一词首，停下 */
            }
            st++;
            i = (i + 1 < len && IsHighSurrogateW(s[i])) ? (i + 2) : (i + 1);
        }
        /* fallthrough to `*steps = st; return i;` */
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

/* 是否经典 Win32 编辑控件（只有它们可靠支持 WM_GETTEXT + EM_GETSEL）。
   现代控件（RichEditD2DPT / Scintilla / Chrome 等）必须走 UIA，
   否则 WM_GETTEXT 返回垃圾长度(常为 1)把跳词带偏。 */
static BOOL IsClassicEdit(HWND hwnd) {
    WCHAR c[96];
    if (!GetClassNameW(hwnd, c, 96)) return FALSE;
    if (_wcsicmp(c, L"Edit") == 0 || _wcsicmp(c, L"RICHEDIT") == 0) return TRUE;
    if (_wcsicmp(c, L"RichEdit20A") == 0 || _wcsicmp(c, L"RichEdit20W") == 0) return TRUE;
    if (_wcsicmp(c, L"RichEdit50W") == 0) return TRUE;
    return FALSE;
}

/* ================= 快速通道：标准 Edit / RichEdit ================= */
/* 通过 WM_GETTEXT + EM_GETSEL 读取文本与光标。仅对经典编辑控件生效。 */
static BOOL FastGetTextCaret(HWND hwnd, WCHAR** text, int* lenOut, int* caretOut) {
    if (!hwnd) return FALSE;
    if (!IsClassicEdit(hwnd)) return FALSE;   /* 非经典编辑：交给 Scintilla/UIA/原生 */

    /* 试探窗口是否支持 WM_GETTEXT（Edit/RichEdit 类） */
    int size = (int)SendMessageTimeoutW(hwnd, WM_GETTEXTLENGTH, 0, 0,
                                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 300, NULL);
    if (size <= 0 || size > 1 << 20) return FALSE;   /* 过长或过短视为不支持 */

    WCHAR* buf = (WCHAR*)malloc((size_t)((size + 2) * sizeof(WCHAR)));
    if (!buf) return FALSE;

    LRESULT got = SendMessageTimeoutW(hwnd, WM_GETTEXT, (WPARAM)(size + 1),
                                      (LPARAM)buf,
                                      SMTO_ABORTIFHUNG | SMTO_BLOCK, 300, NULL);
    if (got <= 0) { free(buf); return FALSE; }

    /* 光标位置：EM_GETSEL */
    DWORD sel = (DWORD)SendMessageTimeout(hwnd, EM_GETSEL, 0, 0,
                                          SMTO_ABORTIFHUNG | SMTO_BLOCK, 300, NULL);
    int caret = (sel != 0xFFFFFFFF) ? (int)LOWORD(sel) : 0;
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
    (void)ok;

    hr = CoCreateInstance(&CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IUIAutomation, (void**)&ua);
    if (FAILED(hr) || !ua) return FALSE;

    /* 锚定候选：优先 UIA 焦点元素（GetFocusedElement 能命中 Chromium/WinUI 的
       真正文本框），取不到 TextPattern 时再回退 ElementFromHandle(hwnd)。
       曾改为 ElementFromHandle 直取顶层窗口，但 Chromium(Chrome_WidgetWin_1)
       顶层窗口本身无 TextPattern，向上也找不到 → 造成 QQ/Edge 跳词回归。 */
    IUIAutomationElement *owner = NULL, *ef = NULL, *eh = NULL;
    if (SUCCEEDED(IUIAutomation_GetFocusedElement(ua, &ef)) && ef) {
        owner = FindDocElement(ua, ef);
        IUIAutomationElement_Release(ef);
    }
    if (!owner && hwnd) {
        if (SUCCEEDED(IUIAutomation_ElementFromHandle(ua, hwnd, &eh)) && eh) {
            owner = FindDocElement(ua, eh);
            IUIAutomationElement_Release(eh);
        }
    }
    if (!owner) goto done;
    el = owner;

    /* owner 上应有 TextPattern；取其 DocumentRange 与 selection 兜底 */
    if (SUCCEEDED(IUIAutomationElement_GetCurrentPatternAs(el, UIA_TextPatternId,
                        &IID_IUIAutomationTextPattern, (void**)&tp)) && tp) {
        IUIAutomationTextPattern_get_DocumentRange(tp, &doc);
    }
    if (!doc) goto done;

    /* 光标位置：优先 TextPattern2 的 caret range */
    {
        IUIAutomationTextPattern2* tp2 = NULL;
        if (SUCCEEDED(IUIAutomationElement_GetCurrentPatternAs(el, UIA_TextPattern2Id,
                            &IID_IUIAutomationTextPattern2, (void**)&tp2)) && tp2) {
            BOOL active = FALSE;
            if (SUCCEEDED(IUIAutomationTextPattern2_GetCaretRange(tp2, &active, &caret))
                && caret) {
                /* ok，拿到 caret range */
            } else {
                caret = NULL;
            }
            IUIAutomationTextPattern2_Release(tp2);
        }
    }
    if (!caret) {
        /* 回退：selection 的第一个 range 作为 caret */
        IUIAutomationTextRangeArray* sel = NULL;
        if (SUCCEEDED(IUIAutomationTextPattern_GetSelection(tp, &sel)) && sel) {
            int len = 0;
            sel->lpVtbl->get_Length(sel, &len);
            if (len > 0) sel->lpVtbl->GetElement(sel, 0, &caret);
            sel->lpVtbl->Release(sel);
        }
    }
    if (!caret) goto done;

    /* 截取 [文档起点, caret 起点) → 长度即光标偏移 */
    if (FAILED(IUIAutomationTextRange_Clone(doc, &tmp)) || !tmp) goto done;
    IUIAutomationTextRange_MoveEndpointByRange(tmp,
                TextPatternRangeEndpoint_End, caret, TextPatternRangeEndpoint_Start);
    if (FAILED(IUIAutomationTextRange_GetText(tmp, 0x7FFFFFFF, &pre)) || !pre)
        goto done;
    {
        int idx = (int)wcslen(pre);
        if (SUCCEEDED(IUIAutomationTextRange_GetText(doc, 0x7FFFFFFF, &all)) && all) {
            int alen = (int)wcslen(all);
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

/* ================= IA2 通道：IAccessible2 (IAccessibleText) =================
   Scintilla(Notepad3/Notepad++ 等)在 Windows 上的无障碍接口是 IAccessible2，
   不暴露 UIA TextPattern —— 这正是读屏器(NVDA)读它们的通道。
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
    HWND focus = GetFocusControl();
    if (focus) got = FastGetTextCaret(focus, &text, &len, &caret);  /* 仅经典 Edit/RichEdit */
    if (got) path = "Edit";
    else {
        got = UiaGetTextCaret(focus, &text, &len, &caret);   /* 现代控件 → 向上找 TextPattern */
        if (got) path = "UIA";
    }
    if (!got) {
        got = MsaaGetTextCaret(focus, &text, &len, &caret);  /* Scintilla 系 → IAccessible2 */
        if (got) path = "IA2";
    }
    if (!got) { SendNativeWordJump(dir, shift); if (g_recording) CapLog("  -> 兜底 native Ctrl+arrow"); return -1; }

    int steps = 0;
    int tgt = WordJumpTarget(text, len, caret, dir, &steps);
    free(text);
    if (g_recording) CapLog("  path=%s len=%d caret=%d -> steps=%d tgt=%d",
                            path, len, caret, steps, tgt);

    if (steps > 0) {
        /* 经典 Edit/RichEdit：距离较大时直接用 EM_SETSEL 一步置到目标(瞬跳)，
           避免逐格发方向键拖慢；小距离仍走箭头以保持完全一致的键行为。
           非经典通道(UIA/IA2)仍发箭头(无法直接设光标)。 */
        BOOL direct = FALSE;
        if (path[0] == 'E' && focus && steps >= DIRECT_JUMP_MIN) {
            direct = ClassicSetSel(focus, caret, tgt, shift);
            if (direct && g_recording) CapLog("  -> 直接 EM_SETSEL [%d,%d]", caret, tgt);
        }
        if (!direct) SendArrows(dir, steps, shift);
    } else {
        SendNativeWordJump(dir, shift);
    }
    return steps;
}

/* ================= 诊断：探测当前焦点控件 =================
   把「前台窗口 / 焦点控件 / 能否读文本 / 光标 / 是否支持 UIA」写日志并弹窗，
   用于排查跳词为何不生效。 */
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

    WCHAR rep[2200] = L"";
    int o = 0;
    o += swprintf_s(rep + o, 2200 - o,
        L"前台窗口: class=[%s] title=[%s]\n焦点控件 hwnd=0x%p\n",
        cls, title, (void*)focus);

    BOOL textOK = FALSE;
    if (focus) {
        WCHAR fcls[96]; GetClassNameW(focus, fcls, 96);
        o += swprintf_s(rep + o, 2200 - o, L"焦点控件类名=[%s]\n", fcls);

        LRESULT len = SendMessageTimeoutW(focus, WM_GETTEXTLENGTH, 0, 0,
                                          SMTO_ABORTIFHUNG | SMTO_BLOCK, 300, NULL);
        o += swprintf_s(rep + o, 2200 - o, L"WM_GETTEXTLENGTH = %lld\n", (LONGLONG)len);
        if (len > 0 && len < 1 << 20) {
            WCHAR* buf = (WCHAR*)malloc(((size_t)len + 1) * sizeof(WCHAR));
            if (buf) {
                LRESULT got = SendMessageTimeoutW(focus, WM_GETTEXT, (WPARAM)(len + 1), (LPARAM)buf,
                                                  SMTO_ABORTIFHUNG | SMTO_BLOCK, 300, NULL);
                if (got > 0) {
                    textOK = TRUE;
                    DWORD sel = (DWORD)SendMessageTimeout(focus, EM_GETSEL, 0, 0,
                                                          SMTO_ABORTIFHUNG | SMTO_BLOCK, 300, NULL);
                    int caret = (sel != 0xFFFFFFFF) ? (int)LOWORD(sel) : -1;
                    o += swprintf_s(rep + o, 2200 - o,
                        L"EM_GETSEL(sel) = 0x%08X  caret=%d\n文本长度=%lld\n",
                        sel, caret, (LONGLONG)got);
                    buf[got < 160 ? got : 160] = 0;
                    o += swprintf_s(rep + o, 2200 - o, L"文本样本(前160) = [%s]\n", buf);
                }
                free(buf);
            }
        }
        if (!textOK)
            o += swprintf_s(rep + o, 2200 - o, L"WM_GETTEXT/WM_GETSEL 不可用（非经典编辑控件）\n");
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
                o += swprintf_s(rep + o, 2200 - o,
                    L"UIA 焦点元素支持 TextPattern = %s\n", txt ? L"YES" : L"NO");
                IUIAutomationElement_Release(el);
            } else {
                o += swprintf_s(rep + o, 2200 - o, L"UIA 取不到焦点元素\n");
            }
            IUIAutomation_Release(ua);
        } else {
            o += swprintf_s(rep + o, 2200 - o, L"UIA 初始化失败\n");
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
                o += swprintf_s(rep + o, 2200 - o,
                    L"IA2 IAccessibleText = YES (caret=%ld)\n", co);
                at->lpVtbl->Release(at);
                if (ok2) textOK = TRUE;
            } else {
                o += swprintf_s(rep + o, 2200 - o, L"IA2 IAccessibleText = NO\n");
            }
            acc->lpVtbl->Release(acc);
        } else {
            o += swprintf_s(rep + o, 2200 - o, L"IA2 取不到 IAccessible\n");
        }
    }

    /* 写日志（UTF-8）并存一份供弹窗 */
    CapLog("%S focus=0x%p class=[%S] textOK=%d", title, (void*)focus, cls, textOK);
    char utf[2400];
    WideCharToMultiByte(CP_UTF8, 0, rep, -1, utf, 2400, NULL, NULL);
    CapLog("%s", utf);

    MessageBoxW(NULL, rep, L"CapsEnhance 焦点探测", MB_OK);
}