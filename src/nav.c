#include "cap.h"

#define COBJMACROS   /* 启用 UIA 的 C 风格 *_宏接口 */
#include <initguid.h>  /* 在本 TU 内为 UIA 的 CLSID/IID 提供存储定义 */

#include <oleauto.h>
#include <uiautomationcore.h>
#include <uiautomationclient.h>
#include <ole2.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

#define SAFE_RELEASE(p) do { if (p) { (p)->Release(); (p)=NULL; } } while(0)

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

/* 词字符判定：CJK 与字母数字视为"词内"，其余（空白+标点）为分隔符 */
static BOOL IsWordChar(WCHAR c) {
    unsigned cp = (unsigned)c;
    if (c == L'\0') return FALSE;
    if (IsCharAlphaNumericW(c)) return TRUE;
    if (cp >= 0x3400 && cp <= 0x4DBF) return TRUE;   /* 扩展A */
    if (cp >= 0x4E00 && cp <= 0x9FFF) return TRUE;   /* 基本区 */
    if (cp >= 0xF900 && cp <= 0xFAFF) return TRUE;   /* 兼容区 */
    if (cp >= 0x20000 && cp <= 0x2FA1F) return TRUE; /* 扩展B+ */
    return FALSE;
}

/* 在 s 中以 caret 为起点，dir 方向跳一个词的移动格数/字符数。
   返回"应发送的箭头次数"；若已到边界返回 0。
   非 static 仅供词跳单元测试（test_nav.c）直接调用。 */
int ComputeSteps(const WCHAR* s, int len, int caret, int dir) {
    int i = caret;
    if (dir == DIR_LEFT) {
        /* 跳到左边界：跨过分隔符后跨过词，落在"分隔符右侧"（即前一词之后） */
        if (i <= 0) return 0;
        while (i > 0 && !IsWordChar(s[i-1])) i--;   /* 跨结尾分隔符 */
        while (i > 0 &&  IsWordChar(s[i-1])) i--;   /* 跨整个词 */
        return (caret > i) ? (caret - i) : 0;
    } else {
        if (i >= len) return 0;
        while (i < len &&  IsWordChar(s[i])) i++;   /* 跨当前词 */
        while (i < len && !IsWordChar(s[i])) i++;   /* 跨分隔符到下一词首 */
        return (i > caret) ? (i - caret) : 0;
    }
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

/* 是否真正的经典编辑控件（Edit/RichEdit），只有它们才可靠支持
   WM_GETTEXT + EM_GETSEL。Scintilla、Chrome/Edge 等自定义控件返回的是
   无效长度(如 1)，必须走 UIA / 原生通道，否则跳词会被假数据带偏。 */
static BOOL IsClassicEdit(HWND hwnd) {
    WCHAR cls[96];
    if (!GetClassNameW(hwnd, cls, 96)) return FALSE;
    if (_wcsicmp(cls, L"Edit") == 0) return TRUE;
    if (cls[0] == L'R' || cls[0] == L'r')
        if (_wcsnicmp(cls, L"RichEdit", 8) == 0 || _wcsicmp(cls, L"RICHEDIT") == 0)
            return TRUE;
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
static BOOL UiaGetTextCaret(WCHAR** text, int* lenOut, int* caretOut) {
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
    if (FAILED(hr) || !ua) goto done;

    if (FAILED(IUIAutomation_GetFocusedElement(ua, &el)) || !el) goto done;

    /* 某些控件(如 Scintilla)的 TextPattern 在焦点元素或某个祖先上。
       从焦点元素向上逐层找可用的 DocumentRange；el 最终指向拥有者。 */
    {
        IUIAutomationTreeWalker* wk = NULL;
        IUIAutomation_get_RawViewWalker(ua, &wk);
        for (int depth = 0; el && depth < 24; depth++) {
            IUIAutomationTextPattern* t = NULL;
            if (SUCCEEDED(IUIAutomationElement_GetCurrentPatternAs(el, UIA_TextPatternId,
                                &IID_IUIAutomationTextPattern, (void**)&t)) && t) {
                IUIAutomationTextRange* d = NULL;
                if (SUCCEEDED(IUIAutomationTextPattern_get_DocumentRange(t, &d)) && d) {
                    doc = d;                /* 找到文档，el 即拥有者 */
                    tp = t;                 /* 保留供 selection 兜底 */
                    break;
                }
                IUIAutomationTextPattern_Release(t); t = NULL;
            }
            IUIAutomationElement* parent = NULL;
            if (wk && SUCCEEDED(IUIAutomationTreeWalker_GetParentElement(wk, el, &parent))) {
                IUIAutomationElement_Release(el);
                el = parent;
            } else
                break;      /* 已到根 */
        }
        if (wk) IUIAutomationTreeWalker_Release(wk);
        if (!doc) goto done;
    }

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

/* ================= 对外接口 ================= */
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
        got = UiaGetTextCaret(&text, &len, &caret);   /* 含 Scintilla 等 → 向上找 TextPattern */
        if (got) path = "UIA";
    }
    if (!got) { SendNativeWordJump(dir, shift); if (g_recording) CapLog("  -> 兜底 native Ctrl+arrow"); return -1; }

    int steps = ComputeSteps(text, len, caret, dir);
    free(text);
    if (g_recording) CapLog("  path=%s len=%d caret=%d -> steps=%d", path, len, caret, steps);

    if (steps > 0) SendArrows(dir, steps, shift);
    else SendNativeWordJump(dir, shift);
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

    /* 写日志（UTF-8）并存一份供弹窗 */
    CapLog("%S focus=0x%p class=[%S] textOK=%d", title, (void*)focus, cls, textOK);
    char utf[2400];
    WideCharToMultiByte(CP_UTF8, 0, rep, -1, utf, 2400, NULL, NULL);
    CapLog("%s", utf);

    MessageBoxW(NULL, rep, L"CapsEnhance 焦点探测", MB_OK);
}