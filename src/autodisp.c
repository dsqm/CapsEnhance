#include "cap.h"
#include "auto.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ================= 通用 IDispatch 按名调用 =================
   本文件**不认识任何具体程序**：给它一个 IDispatch，它按名字读写属性、调用
   方法。Word / WPS / Excel / 任何自动化对象模型都走同一套。

   四个必须记住的坑（都收在这里，调用点不必再想）：
   1. Invoke 的参数数组**逆序**：rgvarg[0] 是**最后一个**形参。
   2. 属性写入（PROPERTYPUT）必须把 DISPID_PROPERTYPUT 放进
      rgdispidNamedArgs，并且 pVarResult 必须为 NULL。
   3. 失败时 EXCEPINFO 的 bstrSource/bstrDescription/bstrHelpFile 是分配出来
      的 BSTR，必须 SysFreeString —— "没打开文档""对象已失效"这类失败会**反复**
      发生，漏掉就是稳定的内存泄漏。
   4. GetIDsOfNames 的第三个参数是 `LPOLESTR*`（可写），直接传字符串字面量
      在 C 里是丢 const 的违规写法，得拷一份。 */
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

/* IID 只在 `#include <initguid.h>` 的 TU 里才有定义，否则是 extern 声明、要靠
   uuid.lib 提供。本模块一律自带常量：值固定、不依赖链接顺序，也不会和别的 TU
   里 initguid.h 生成的同名定义冲突（不同名）。 */
static const IID kNull = { 0, 0, 0, { 0, 0, 0, 0, 0, 0, 0, 0 } };

/* CoInitializeEx 是**线程级**状态：工作线程（act.c）与桥子进程都要各自初始化 */
static __declspec(thread) BOOL t_coReady = FALSE;

BOOL AdCoUp(void) {
    if (t_coReady) return TRUE;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    /* RPC_E_CHANGED_MODE：本线程已按别的模式初始化过 COM，照样能用；
       S_FALSE：本线程重复初始化。两者都不该再反初始化。 */
    if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) t_coReady = TRUE;
    return t_coReady;
}

/* 释放 EXCEPINFO 里可能被填上的三个 BSTR */
static void EiClear(EXCEPINFO* ei) {
    if (ei->bstrSource)      { SysFreeString(ei->bstrSource);      ei->bstrSource = NULL; }
    if (ei->bstrDescription) { SysFreeString(ei->bstrDescription); ei->bstrDescription = NULL; }
    if (ei->bstrHelpFile)    { SysFreeString(ei->bstrHelpFile);    ei->bstrHelpFile = NULL; }
}

DISPID AdId(IDispatch* d, const WCHAR* name) {
    if (!d || !name) return DISPID_UNKNOWN;
    size_t n = wcslen(name);
    if (n == 0 || n >= 64) return DISPID_UNKNOWN;
    WCHAR buf[64];
    wcscpy_s(buf, 64, name);
    LPOLESTR p = buf;                      /* 见文件头坑 4 */
    DISPID id = DISPID_UNKNOWN;
    if (FAILED(d->lpVtbl->GetIDsOfNames(d, kNull, &p, 1, LOCALE_USER_DEFAULT, &id)))
        return DISPID_UNKNOWN;
    return id;
}

/* 统一收尾：失败时清掉返回值（可能被填了一半）与 EXCEPINFO */
static BOOL EiDone(HRESULT hr, VARIANT* out, EXCEPINFO* ei) {
    EiClear(ei);
    if (FAILED(hr)) {
        if (out) {
            if (out->vt != VT_EMPTY) VariantClear(out);
            VariantInit(out);
        }
        return FALSE;
    }
    return TRUE;
}

BOOL AdGet(IDispatch* d, const WCHAR* name, VARIANT* out) {
    if (!out) return FALSE;
    VariantInit(out);
    DISPID id = AdId(d, name);
    if (id == DISPID_UNKNOWN) return FALSE;
    DISPPARAMS dp; memset(&dp, 0, sizeof(dp));
    EXCEPINFO ei;  memset(&ei, 0, sizeof(ei));
    UINT ec = 0;
    HRESULT hr = d->lpVtbl->Invoke(d, id, kNull, LOCALE_USER_DEFAULT,
                                   DISPATCH_PROPERTYGET, &dp, out, &ei, &ec);
    return EiDone(hr, out, &ei);
}

/* 数值属性：自动化返回的类型可能是 I2/I4/UI4（实测 WPS 用 I4、个别实现用 I2），
   统一转 long。 */
BOOL AdGetLong(IDispatch* d, const WCHAR* name, long* out) {
    VARIANT v;
    if (!AdGet(d, name, &v)) return FALSE;
    BOOL ok = FALSE;
    if      (v.vt == VT_I4)  { *out = (long)v.lVal; ok = TRUE; }
    else if (v.vt == VT_I2)  { *out = (long)v.iVal; ok = TRUE; }
    else if (v.vt == VT_UI4) { *out = (long)v.lVal; ok = TRUE; }
    VariantClear(&v);
    return ok;
}

/* 文本属性：把 BSTR 拷成调用方 free 的普通串。
   注意调用的属性若不存在（或对象不支持），返回 FALSE —— 不区分"空串"与
   "取不到"，由调用方按长度自行判定占位（<2 字符视为占位，与 UIA/IA2 一致）。 */
BOOL AdGetText(IDispatch* d, const WCHAR* name, WCHAR** out) {
    if (!out) return FALSE;
    *out = NULL;
    VARIANT v;
    if (!AdGet(d, name, &v)) return FALSE;
    BOOL ok = FALSE;
    if (v.vt == VT_BSTR && v.bstrVal) {
        int len = (int)SysStringLen(v.bstrVal);
        WCHAR* copy = (WCHAR*)malloc(((size_t)len + 1) * sizeof(WCHAR));
        if (copy) {
            if (len > 0) memcpy(copy, v.bstrVal, (size_t)len * sizeof(WCHAR));
            copy[len] = 0;
            *out = copy;
            ok = TRUE;
        }
    }
    VariantClear(&v);
    return ok;
}

/* 子对象（Range / Selection / ActiveDocument ...）。引用已 AddRef，交给调用者，
   调用者负责 Release —— 而**借来的**指针（别人传进来的、GetParentElement 拿到的）
   绝不能 Release，两件事别混。 */
IDispatch* AdGetDisp(IDispatch* d, const WCHAR* name) {
    VARIANT v;
    if (!AdGet(d, name, &v)) return NULL;
    IDispatch* r = NULL;
    if (v.vt == VT_DISPATCH && v.pdispVal) {
        r = v.pdispVal;
        r->lpVtbl->AddRef(r);
    }
    VariantClear(&v);
    return r;
}

BOOL AdPutLong(IDispatch* d, const WCHAR* name, long val) {
    DISPID id = AdId(d, name);
    if (id == DISPID_UNKNOWN) return FALSE;
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_I4;
    v.lVal = val;
    DISPID putid = DISPID_PROPERTYPUT;      /* 见文件头坑 2 */
    DISPPARAMS dp; memset(&dp, 0, sizeof(dp));
    dp.rgvarg = &v; dp.cArgs = 1;
    dp.rgdispidNamedArgs = &putid; dp.cNamedArgs = 1;
    EXCEPINFO ei; memset(&ei, 0, sizeof(ei));
    UINT ec = 0;
    HRESULT hr = d->lpVtbl->Invoke(d, id, kNull, LOCALE_USER_DEFAULT,
                                   DISPATCH_PROPERTYPUT, &dp, NULL, &ei, &ec);
    return EiDone(hr, NULL, &ei);
}

/* 参数数组打包：形参按 (a1, a2, ...) 给出，这里按逆序放（见文件头坑 1） */
static void Args2(VARIANTARG* av, DISPPARAMS* dp, long a1, long a2) {
    memset(av, 0, sizeof(VARIANTARG) * 2);
    av[0].vt = VT_I4; av[0].lVal = a2;
    av[1].vt = VT_I4; av[1].lVal = a1;
    memset(dp, 0, sizeof(*dp));
    dp->rgvarg = av;
    dp->cArgs = 2;
}

BOOL AdCall2(IDispatch* d, const WCHAR* name, long a1, long a2) {
    DISPID id = AdId(d, name);
    if (id == DISPID_UNKNOWN) return FALSE;
    VARIANTARG av[2];
    DISPPARAMS dp;
    Args2(av, &dp, a1, a2);
    EXCEPINFO ei; memset(&ei, 0, sizeof(ei));
    UINT ec = 0;
    HRESULT hr = d->lpVtbl->Invoke(d, id, kNull, LOCALE_USER_DEFAULT,
                                   DISPATCH_METHOD, &dp, NULL, &ei, &ec);
    return EiDone(hr, NULL, &ei);
}

BOOL AdCall0(IDispatch* d, const WCHAR* name) {
    DISPID id = AdId(d, name);
    if (id == DISPID_UNKNOWN) return FALSE;
    DISPPARAMS dp; memset(&dp, 0, sizeof(dp));
    EXCEPINFO ei;  memset(&ei, 0, sizeof(ei));
    UINT ec = 0;
    HRESULT hr = d->lpVtbl->Invoke(d, id, kNull, LOCALE_USER_DEFAULT,
                                   DISPATCH_METHOD, &dp, NULL, &ei, &ec);
    return EiDone(hr, NULL, &ei);
}

BOOL AdGetProp2(IDispatch* d, const WCHAR* name, long a1, long a2, IDispatch** out) {
    if (out) *out = NULL;
    DISPID id = AdId(d, name);
    if (id == DISPID_UNKNOWN) return FALSE;
    VARIANTARG av[2];
    DISPPARAMS dp;
    Args2(av, &dp, a1, a2);
    VARIANT v;
    VariantInit(&v);
    EXCEPINFO ei; memset(&ei, 0, sizeof(ei));
    UINT ec = 0;
    HRESULT hr = d->lpVtbl->Invoke(d, id, kNull, LOCALE_USER_DEFAULT,
                                   DISPATCH_PROPERTYGET, &dp, &v, &ei, &ec);
    if (!EiDone(hr, &v, &ei)) return FALSE;
    BOOL ok = FALSE;
    if (v.vt == VT_DISPATCH && v.pdispVal && out) {
        *out = v.pdispVal;
        (*out)->lpVtbl->AddRef(*out);
        ok = TRUE;
    }
    VariantClear(&v);
    return ok;
}
