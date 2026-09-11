/* wspset：实测 WPS 能不能"直接置光标/置选区"（免逐格发方向键）。
   必须**以普通权限**运行（管理员进程附不上 WPS 的 ROT）：
     bin\lowrun.exe bin\wspset.exe bin\wspset.medium.out
   纯 ASCII 输出到 stdout + 指定文件。

   依次试三条路，每条都回读 Selection.Start/End/Text/Type 校验是否真的生效：
     A. Selection.Start = n            （PROPERTYPUT，Word 对象模型里这是"移光标"）
     B. Selection.SetRange(start,end)  （方法；Word 文档语义为有序区间）
     C. Document.Range(a,b).Select()   （方法；让选区等于该 Range）
   另外测：参数倒序时是否有序化；置成非折叠区间后 Selection.Type 是多少。 */
/* Standalone probe: it does not include cap.h, so the C-interface bridge
   (see src/cap.h) has to be repeated here. CINTERFACE keeps the SDK's C
   vtable layout so the `p->lpVtbl->Method(p, ...)` calls below compile as
   C++ too. REFIID/REFCLSID are references in C++ -> pass IID_X, not &IID_X. */
#define CINTERFACE
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ole2.h>
#include <oleauto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

static const IID kNull = { 0, 0, 0, { 0, 0, 0, 0, 0, 0, 0, 0 } };
static FILE* g_out = NULL;

static void P(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stdout, fmt, ap); va_end(ap);
    if (g_out) { va_start(ap, fmt); vfprintf(g_out, fmt, ap); va_end(ap); }
    fflush(stdout);
    if (g_out) fflush(g_out);
}

static void EiClear(EXCEPINFO* ei) {
    if (ei->bstrDescription) { SysFreeString(ei->bstrDescription); ei->bstrDescription = NULL; }
    if (ei->bstrSource)      { SysFreeString(ei->bstrSource);      ei->bstrSource = NULL; }
    if (ei->bstrHelpFile)    { SysFreeString(ei->bstrHelpFile);    ei->bstrHelpFile = NULL; }
}
static void EiShow(EXCEPINFO* ei) {
    if (ei->bstrDescription) {
        char u[400];
        WideCharToMultiByte(CP_UTF8, 0, ei->bstrDescription, -1, u, sizeof(u), NULL, NULL);
        P("        EXCEPINFO: [%s] scode=0x%08X\n", u, (unsigned)ei->scode);
    }
}

static DISPID Id(IDispatch* d, const wchar_t* n) {
    WCHAR buf[64]; wcscpy_s(buf, 64, n);
    LPOLESTR p = buf;
    DISPID id = DISPID_UNKNOWN;
    if (FAILED(d->lpVtbl->GetIDsOfNames(d, kNull, &p, 1, LOCALE_USER_DEFAULT, &id)))
        return DISPID_UNKNOWN;
    return id;
}

static BOOL Get(IDispatch* d, const wchar_t* n, VARIANT* out) {
    VariantInit(out);
    DISPID id = Id(d, n);
    if (id == DISPID_UNKNOWN) return FALSE;
    DISPPARAMS dp; memset(&dp, 0, sizeof(dp));
    EXCEPINFO ei; memset(&ei, 0, sizeof(ei)); UINT ec = 0;
    HRESULT hr = d->lpVtbl->Invoke(d, id, kNull, LOCALE_USER_DEFAULT,
                                   DISPATCH_PROPERTYGET, &dp, out, &ei, &ec);
    EiClear(&ei);
    if (FAILED(hr)) { if (out->vt != VT_EMPTY) VariantClear(out); VariantInit(out); return FALSE; }
    return TRUE;
}
static IDispatch* GetD(IDispatch* d, const wchar_t* n) {
    VARIANT v;
    if (!Get(d, n, &v)) return NULL;
    IDispatch* r = NULL;
    if (v.vt == VT_DISPATCH && v.pdispVal) { r = v.pdispVal; r->lpVtbl->AddRef(r); }
    VariantClear(&v);
    return r;
}
static long GetL(IDispatch* d, const wchar_t* n, long dflt) {
    VARIANT v;
    if (!Get(d, n, &v)) return dflt;
    long r = dflt;
    if (v.vt == VT_I4) r = (long)v.lVal;
    else if (v.vt == VT_I2) r = (long)v.iVal;
    VariantClear(&v);
    return r;
}

/* 属性写入：Selection.Start = n */
static HRESULT PutLong(IDispatch* d, const wchar_t* n, long val) {
    DISPID id = Id(d, n);
    if (id == DISPID_UNKNOWN) return DISP_E_UNKNOWNNAME;
    VARIANT v; VariantInit(&v); v.vt = VT_I4; v.lVal = val;
    DISPID putid = DISPID_PROPERTYPUT;
    DISPPARAMS dp; memset(&dp, 0, sizeof(dp));
    dp.rgvarg = &v; dp.cArgs = 1;
    dp.rgdispidNamedArgs = &putid; dp.cNamedArgs = 1;
    EXCEPINFO ei; memset(&ei, 0, sizeof(ei)); UINT ec = 0;
    HRESULT hr = d->lpVtbl->Invoke(d, id, kNull, LOCALE_USER_DEFAULT,
                                   DISPATCH_PROPERTYPUT, &dp, NULL, &ei, &ec);
    EiShow(&ei); EiClear(&ei);
    return hr;
}

/* 方法调用（2 个 long 参数，按对象模型的形参顺序给出） */
static HRESULT Call2Long(IDispatch* d, const wchar_t* n, long a1, long a2, VARIANT* out) {
    DISPID id = Id(d, n);
    if (id == DISPID_UNKNOWN) return DISP_E_UNKNOWNNAME;
    VARIANTARG args[2]; memset(args, 0, sizeof(args));
    /* IDispatch 的参数数组是**逆序**：rgvarg[0] = 最后一个形参 */
    args[0].vt = VT_I4; args[0].lVal = a2;
    args[1].vt = VT_I4; args[1].lVal = a1;
    DISPPARAMS dp; memset(&dp, 0, sizeof(dp));
    dp.rgvarg = args; dp.cArgs = 2;
    EXCEPINFO ei; memset(&ei, 0, sizeof(ei)); UINT ec = 0;
    if (out) VariantInit(out);
    HRESULT hr = d->lpVtbl->Invoke(d, id, kNull, LOCALE_USER_DEFAULT,
                                   DISPATCH_METHOD, &dp, out, &ei, &ec);
    EiShow(&ei); EiClear(&ei);
    return hr;
}

/* 无参方法 */
static HRESULT Call0(IDispatch* d, const wchar_t* n, VARIANT* out) {
    DISPID id = Id(d, n);
    if (id == DISPID_UNKNOWN) return DISP_E_UNKNOWNNAME;
    DISPPARAMS dp; memset(&dp, 0, sizeof(dp));
    EXCEPINFO ei; memset(&ei, 0, sizeof(ei)); UINT ec = 0;
    if (out) VariantInit(out);
    HRESULT hr = d->lpVtbl->Invoke(d, id, kNull, LOCALE_USER_DEFAULT,
                                   DISPATCH_METHOD, &dp, out, &ei, &ec);
    EiShow(&ei); EiClear(&ei);
    return hr;
}

/* Range(a,b)：Range 是"参数缺省则取全文"的属性，带参调用按 PROPERTYGET 走 */
static IDispatch* DocRange(IDispatch* doc, long a, long b) {
    DISPID id = Id(doc, L"Range");
    if (id == DISPID_UNKNOWN) return NULL;
    VARIANTARG args[2]; memset(args, 0, sizeof(args));
    args[0].vt = VT_I4; args[0].lVal = b;   /* 逆序 */
    args[1].vt = VT_I4; args[1].lVal = a;
    DISPPARAMS dp; memset(&dp, 0, sizeof(dp));
    dp.rgvarg = args; dp.cArgs = 2;
    EXCEPINFO ei; memset(&ei, 0, sizeof(ei)); UINT ec = 0;
    VARIANT v; VariantInit(&v);
    HRESULT hr = doc->lpVtbl->Invoke(doc, id, kNull, LOCALE_USER_DEFAULT,
                                     DISPATCH_PROPERTYGET, &dp, &v, &ei, &ec);
    EiShow(&ei); EiClear(&ei);
    if (FAILED(hr)) { VariantClear(&v); return NULL; }
    IDispatch* r = NULL;
    if (v.vt == VT_DISPATCH && v.pdispVal) { r = v.pdispVal; r->lpVtbl->AddRef(r); }
    VariantClear(&v);
    return r;
}

static void State(IDispatch* app, const char* tag) {
    IDispatch* sel = GetD(app, L"Selection");
    if (!sel) { P("    %s: 取不到 Selection\n", tag); return; }
    long s = GetL(sel, L"Start", -1), e = GetL(sel, L"End", -1), ty = GetL(sel, L"Type", -1);
    VARIANT tv;
    char text[128] = "";
    if (Get(sel, L"Text", &tv)) {
        if (tv.vt == VT_BSTR && tv.bstrVal)
            WideCharToMultiByte(CP_UTF8, 0, tv.bstrVal, -1, text, sizeof(text), NULL, NULL);
        VariantClear(&tv);
    }
    P("    %s: Start=%ld End=%ld Type=%ld Text=[%s]\n", tag, s, e, ty, text);
    sel->lpVtbl->Release(sel);
}

int main(int argc, char** argv) {
    fopen_s(&g_out, (argc > 1) ? argv[1] : "wspset.out", "wb");

    {
        HANDLE t = NULL; char il[16] = "?";
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) {
            DWORD need = 0; GetTokenInformation(t, TokenIntegrityLevel, NULL, 0, &need);
            BYTE* b = (BYTE*)malloc(need ? need : 256);
            if (b && GetTokenInformation(t, TokenIntegrityLevel, b, need, &need))
                sprintf_s(il, sizeof(il), "0x%04lX", (unsigned long)*GetSidSubAuthority(
                    ((TOKEN_MANDATORY_LABEL*)b)->Label.Sid,
                    *GetSidSubAuthorityCount(((TOKEN_MANDATORY_LABEL*)b)->Label.Sid) - 1));
            free(b); CloseHandle(t);
        }
        P("self IL=%s (0x2000=Medium 才能附着 WPS)\n", il);
    }

    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    P("CoInitializeEx hr=0x%08X\n", (unsigned)hr);

    CLSID clsid;
    hr = CLSIDFromProgID(L"KWPS.Application", &clsid);
    if (FAILED(hr)) { P("CLSIDFromProgID FAIL 0x%08X\n", (unsigned)hr); return 1; }
    IUnknown* unk = NULL;
    hr = GetActiveObject(clsid, NULL, &unk);   /* REFCLSID：C++ 下是引用，不收 & */
    P("GetActiveObject hr=0x%08X\n", (unsigned)hr);
    if (FAILED(hr) || !unk) { P("附不上（要用普通权限跑）\n"); return 2; }
    IDispatch* app = NULL;
    if (FAILED(unk->lpVtbl->QueryInterface(unk, IID_IDispatch, (void**)&app)) || !app) {
        P("QI(IDispatch) FAIL\n");
        unk->lpVtbl->Release(unk);
        return 3;
    }
    unk->lpVtbl->Release(unk);

    IDispatch* doc = GetD(app, L"ActiveDocument");
    if (!doc) { P("ActiveDocument 取不到\n"); return 4; }
    P("文档长度（Range.End）=%ld\n", GetL(doc, L"Range.End", -1));

    P("\n== 初始状态 ==\n");            State(app, "init");

    P("\n== A. Selection.Start = 5 (PROPERTYPUT) ==\n");
    {
        IDispatch* sel = GetD(app, L"Selection");
        long before = sel ? GetL(sel, L"Start", -1) : -1;
        hr = sel ? PutLong(sel, L"Start", 5) : E_FAIL;
        P("    PutLong(Selection.Start, 5) hr=0x%08X (before=%ld)\n", (unsigned)hr, before);
        if (sel) sel->lpVtbl->Release(sel);
        State(app, "after ");
    }

    P("\n== B. Selection.SetRange(10, 20) ==\n");
    { IDispatch* sel = GetD(app, L"Selection");
      VARIANT r; hr = Call2Long(sel, L"SetRange", 10, 20, &r);
      if (SUCCEEDED(hr)) VariantClear(&r);
      P("    SetRange(10,20) hr=0x%08X\n", (unsigned)hr);
      if (sel) sel->lpVtbl->Release(sel);
      State(app, "after ");
    }

    P("\n== B2. SetRange(20, 10)（倒序，看是否有序化）==\n");
    { IDispatch* sel = GetD(app, L"Selection");
      VARIANT r; hr = Call2Long(sel, L"SetRange", 20, 10, &r);
      if (SUCCEEDED(hr)) VariantClear(&r);
      P("    SetRange(20,10) hr=0x%08X\n", (unsigned)hr);
      if (sel) sel->lpVtbl->Release(sel);
      State(app, "after ");
    }

    P("\n== C. Document.Range(2,8).Select() ==\n");
    { IDispatch* rng = DocRange(doc, 2, 8);
      if (!rng) P("    Range(2,8) 取不到\n");
      else {
        VARIANT r; hr = Call0(rng, L"Select", &r);
        if (SUCCEEDED(hr)) VariantClear(&r);
        P("    Select() hr=0x%08X\n", (unsigned)hr);
        rng->lpVtbl->Release(rng);
      }
      State(app, "after ");
    }

    P("\n== D. Selection 上直接写 Range 的两端？(Selection.Start/End 一起置) ==\n");
    { IDispatch* sel = GetD(app, L"Selection");
      if (sel) {
        HRESULT h1 = PutLong(sel, L"End", 25);
        P("    PutLong(Selection.End, 25) hr=0x%08X\n", (unsigned)h1);
        sel->lpVtbl->Release(sel);
      }
      State(app, "after ");
    }

    P("\n== E. Range(a,b).Select() 后再读 Selection（折叠到一点？）==\n");
    { IDispatch* rng = DocRange(doc, 17, 17);
      if (rng) {
        VARIANT r; hr = Call0(rng, L"Select", &r);
        if (SUCCEEDED(hr)) VariantClear(&r);
        P("    Range(17,17).Select() hr=0x%08X\n", (unsigned)hr);
        rng->lpVtbl->Release(rng);
      }
      State(app, "after ");
    }

    doc->lpVtbl->Release(doc);
    app->lpVtbl->Release(app);
    CoUninitialize();
    P("\n== done ==\n");
    if (g_out) fclose(g_out);
    return 0;
}
