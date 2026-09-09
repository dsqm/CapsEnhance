/* uiprobe.c - UIA read-back probe with step-by-step HRESULT logging.
 *
 * Why: nav.c's UIA channel returns FALSE for Chromium apps (QQ/Edge/VSCode)
 *      even though NavProbe reports "TextPattern = YES". This tool walks the
 *      exact same call sequence and prints the HRESULT of every step, so we
 *      can see which call breaks.
 *
 * Usage: focus the target editor, then run uiprobe.exe.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define COBJMACROS
#include <initguid.h>
#include <oleauto.h>
#include <oleacc.h>
#include <uiautomationcore.h>
#include <uiautomationclient.h>
#include <ole2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "oleacc.lib")
#pragma comment(lib, "user32.lib")

static void Hr(const char* what, HRESULT h) {
    printf("  %-42s hr=0x%08lX %s\n", what, (unsigned long)h,
           SUCCEEDED(h) ? "OK" : "FAIL");
}

static HWND FocusHwnd(void) {
    HWND fg = GetForegroundWindow();
    if (!fg) return NULL;
    DWORD tid = GetWindowThreadProcessId(fg, NULL);
    GUITHREADINFO gi;
    memset(&gi, 0, sizeof(gi));
    gi.cbSize = sizeof(gi);
    if (tid && GetGUIThreadInfo(tid, &gi) && gi.hwndFocus) return gi.hwndFocus;
    return NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    HWND fg = GetForegroundWindow();
    HWND focus = FocusHwnd();
    char cls[128] = "", fcls[128] = "";
    if (fg) GetClassNameA(fg, cls, 128);
    if (focus) GetClassNameA(focus, fcls, 128);
    printf("foreground=0x%p class=[%s]\nfocus=0x%p class=[%s]\n",
           (void*)fg, cls, (void*)focus, fcls);

    HRESULT hr = CoInitialize(NULL);
    Hr("CoInitialize", hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return 1;

    IUIAutomation* ua = NULL;
    hr = CoCreateInstance(&CLSID_CUIAutomation, NULL, CLSCTX_INPROC_SERVER,
                          &IID_IUIAutomation, (void**)&ua);
    Hr("CoCreateInstance CUIAutomation", hr);
    if (!ua) { CoUninitialize(); return 1; }

    IUIAutomationElement* el = NULL;
    hr = IUIAutomation_GetFocusedElement(ua, &el);
    Hr("GetFocusedElement", hr);
    if (el) {
        BSTR nm = NULL;
        if (SUCCEEDED(IUIAutomationElement_get_CurrentName(el, &nm)) && nm) {
            printf("  focused name=[%ls]\n", nm);
            SysFreeString(nm);
        }
        CONTROLTYPEID ct = 0;
        if (SUCCEEDED(IUIAutomationElement_get_CurrentControlType(el, &ct)))
            printf("  focused controlType=%ld\n", (long)ct);

        IUIAutomationTextPattern* tp = NULL;
        hr = IUIAutomationElement_GetCurrentPatternAs(el, UIA_TextPatternId,
                 &IID_IUIAutomationTextPattern, (void**)&tp);
        Hr("focused -> TextPattern", hr);
        printf("    tp=%p\n", (void*)tp);
        fflush(stdout);
        if (tp) {
            IUIAutomationTextRange* doc = NULL;
            hr = IUIAutomationTextPattern_get_DocumentRange(tp, &doc);
            Hr("  DocumentRange", hr);
            if (doc) {
                BSTR all = NULL;
                hr = IUIAutomationTextRange_GetText(doc, -1, &all);
                Hr("  GetText(doc, -1)", hr);
                if (all) {
                    printf("  doc len=%d sample=[%.80ls]\n",
                           (int)SysStringLen(all), all);
                    SysFreeString(all);
                } else {
                    printf("  doc text BSTR is NULL\n");
                }
                /* caret via TextPattern2 */
                IUIAutomationTextPattern2* tp2 = NULL;
                hr = IUIAutomationElement_GetCurrentPatternAs(el, UIA_TextPattern2Id,
                         &IID_IUIAutomationTextPattern2, (void**)&tp2);
                Hr("  TextPattern2", hr);
                if (tp2) {
                    IUIAutomationTextRange* cr = NULL;
                    BOOL active = FALSE;
                    hr = IUIAutomationTextPattern2_GetCaretRange(tp2, &active, &cr);
                    Hr("  GetCaretRange", hr);
                    printf("    active=%d range=%p\n", (int)active, (void*)cr);
                    if (cr) {
                        IUIAutomationTextRange* tmp = NULL;
                        hr = IUIAutomationTextRange_Clone(doc, &tmp);
                        Hr("    Clone(doc)", hr);
                        if (tmp) {
                            hr = IUIAutomationTextRange_MoveEndpointByRange(tmp,
                                     TextPatternRangeEndpoint_End, cr,
                                     TextPatternRangeEndpoint_Start);
                            Hr("    MoveEndpointByRange", hr);
                            BSTR pre = NULL;
                            hr = IUIAutomationTextRange_GetText(tmp, -1, &pre);
                            Hr("    GetText(prefix)", hr);
                            if (pre) {
                                printf("    caret index=%d\n", (int)SysStringLen(pre));
                                SysFreeString(pre);
                            }
                            IUIAutomationTextRange_Release(tmp);
                        }
                        IUIAutomationTextRange_Release(cr);
                    }
                    IUIAutomationTextPattern2_Release(tp2);
                }
                /* selection fallback */
                IUIAutomationTextRangeArray* sel = NULL;
                hr = IUIAutomationTextPattern_GetSelection(tp, &sel);
                Hr("  GetSelection", hr);
                if (sel) {
                    int n = 0;
                    sel->lpVtbl->get_Length(sel, &n);
                    printf("    selection count=%d\n", n);
                    if (n > 0) {
                        IUIAutomationTextRange* r0 = NULL;
                        sel->lpVtbl->GetElement(sel, 0, &r0);
                        if (r0) {
                            BSTR t = NULL;
                            hr = IUIAutomationTextRange_GetText(r0, -1, &t);
                            Hr("    selection[0].GetText", hr);
                            if (t) { printf("    sel text len=%d\n", (int)SysStringLen(t)); SysFreeString(t); }

                            /* caret index: [docStart, selStart) -- the exact
                               sequence nav.c uses, with both maxLength forms */
                            IUIAutomationTextRange* tmp = NULL;
                            hr = IUIAutomationTextRange_Clone(doc, &tmp);
                            Hr("    Clone(doc)", hr);
                            if (tmp) {
                                hr = IUIAutomationTextRange_MoveEndpointByRange(tmp,
                                         TextPatternRangeEndpoint_End, r0,
                                         TextPatternRangeEndpoint_Start);
                                Hr("    MoveEndpointByRange", hr);
                                BSTR pre = NULL;
                                hr = IUIAutomationTextRange_GetText(tmp, -1, &pre);
                                Hr("    GetText(prefix, -1)", hr);
                                if (pre) { printf("    caret index(-1) = %d\n", (int)SysStringLen(pre)); SysFreeString(pre); }
                                BSTR pre2 = NULL;
                                hr = IUIAutomationTextRange_GetText(tmp, 0x7FFFFFFF, &pre2);
                                Hr("    GetText(prefix, 0x7FFFFFFF)", hr);
                                if (pre2) { printf("    caret index(max) = %d\n", (int)SysStringLen(pre2)); SysFreeString(pre2); }
                                IUIAutomationTextRange_Release(tmp);
                            }
                            /* also check the doc-range maxLength form */
                            BSTR all2 = NULL;
                            hr = IUIAutomationTextRange_GetText(doc, 0x7FFFFFFF, &all2);
                            Hr("    GetText(doc, 0x7FFFFFFF)", hr);
                            if (all2) { printf("    doc len(max) = %d\n", (int)SysStringLen(all2)); SysFreeString(all2); }

                            IUIAutomationTextRange_Release(r0);
                        }
                    }
                    sel->lpVtbl->Release(sel);
                }
                IUIAutomationTextRange_Release(doc);
            }
            IUIAutomationTextPattern_Release(tp);
        }
        IUIAutomationElement_Release(el);
    }

    /* ElementFromHandle path */
    if (focus) {
        IUIAutomationElement* eh = NULL;
        hr = IUIAutomation_ElementFromHandle(ua, focus, &eh);
        Hr("ElementFromHandle(focus)", hr);
        if (eh) {
            IUIAutomationTextPattern* tp = NULL;
            hr = IUIAutomationElement_GetCurrentPatternAs(eh, UIA_TextPatternId,
                     &IID_IUIAutomationTextPattern, (void**)&tp);
            Hr("  handle -> TextPattern", hr);
            if (tp) IUIAutomationTextPattern_Release(tp);
            IUIAutomationElement_Release(eh);
        }
    }

    IUIAutomation_Release(ua);
    CoUninitialize();
    printf("done.\n");
    return 0;
}
