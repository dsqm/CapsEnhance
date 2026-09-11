#ifndef AUTO_H
#define AUTO_H

/* ================= 通用自动化 / 降权桥 / WPS 通道的对外接口 =================
   为什么不放进 cap.h：这里有 IDispatch / VARIANT 出现，需要 <oaidl.h>；
   而 cap.h 被所有 TU（含钩子路径上的 hook.c/send.c）包含，不该为了几条
   无关的 COM 声明多拉一个 SDK 头。cap.h 只放"整个程序都要知道"的东西。

   三个模块的分工（这是 2026-09-11 从 nav.c 里抽出来的）：
     · autodisp.c —— **通用**：COM 自动化客户端按名字调用对象模型
       （GetIDsOfNames + Invoke 的那套固定套路与坑）。不认识任何具体程序。
     · bridge.c   —— **通用**：管理员进程要访问"只有普通权限才看得见的对象"
       时的降权 IPC（借 shell 令牌拉子进程 + 命名管道 + 帧收发）。
       只做运输，不定义协议内容。
     · wps.c      —— **专用**：WPS 文字的对象模型（ProgID / 进程名 / 属性链）、
       直接置位，以及跑在桥上的那套请求/应答协议。
   于是"再来一个要自动化的程序"只需照 wps.c 写一个新文件，前两层直接复用；
   而 nav.c 只保留一行分派。 */

#include <windows.h>
#include <oaidl.h>

/* ---------- 通用 IDispatch 按名调用（src/autodisp.c） ----------
   自动化（Word/WPS/Excel 这类"对象模型"）全靠两个动作：按名字拿 DISPID，再
   Invoke。这一层把固定套路收住，省得每个调用点重踩同样的坑：
     · Invoke 的参数数组是**逆序**的（rgvarg[0] 是最后一个形参）；
     · 属性写入必须带 DISPID_PROPERTYPUT 作为命名参数，pVarResult 传 NULL；
     · 失败时 EXCEPINFO 里的三个 BSTR 必须放掉 —— 否则每次失败漏一块，
       而"没打开文档"这类失败是**按设计反复发生**的；
     · GetIDsOfNames 要的是可写 LPOLESTR，字面量是 const，得拷一份。 */
BOOL     AdCoUp(void);                        /* 本线程 COM 初始化（幂等） */
DISPID   AdId(IDispatch* d, const WCHAR* name);  /* DISPID_UNKNOWN = 无此名字 */
BOOL     AdGet(IDispatch* d, const WCHAR* name, VARIANT* out);       /* 取属性（无参） */
BOOL     AdGetLong(IDispatch* d, const WCHAR* name, long* out);
BOOL     AdGetText(IDispatch* d, const WCHAR* name, WCHAR** out);    /* BSTR -> malloc 串；调用方 free */
IDispatch* AdGetDisp(IDispatch* d, const WCHAR* name);               /* 子对象（已 AddRef） */
BOOL     AdPutLong(IDispatch* d, const WCHAR* name, long v);         /* 属性写入 */
BOOL     AdCall0(IDispatch* d, const WCHAR* name);                   /* 无参方法 */
BOOL     AdCall2(IDispatch* d, const WCHAR* name, long a1, long a2); /* 两个 long 的方法 */
/* 带两个 long 参数的**属性**（如 Word/WPS 的 Document.Range(Start, End)） */
BOOL     AdGetProp2(IDispatch* d, const WCHAR* name, long a1, long a2, IDispatch** out);

/* ---------- 降权桥（src/bridge.c） ----------
   解决的问题：**管理员进程看不到普通权限进程注册的 COM 运行对象**（ROT 按
   完整性级别隔离，实测管理员调用 GetActiveObject 恒返回 0x800401E3，用
   shell 令牌模拟也无效）。本程序默认以管理员运行，所以这类查询必须交给一个
   "普通权限的自己"去跑。

   本模块只负责运输与生命周期，协议内容由调用方定义：
     · 借 explorer.exe 的令牌 CreateProcessWithTokenW 拉起
       `本 exe --bridge <父pid> <序号>`（普通权限副本，不装钩子/不建窗口）；
     · 命名管道**由子进程创建**（对象带普通权限的完整性标签，高完整性进程
       连它没问题，反向才被拦）——这也绕开了"CreateProcessWithTokenW 不支持
       句柄继承、CreateProcessAsUser 需要未分配的 SeAssignPrimaryTokenPrivilege"
       这两条路；
     · 帧格式 [LONG len][len 字节负载]，len < 0 表示对端处理失败；
     · 按需拉起、常驻复用、空闲自退、失败冷却。 */

/* 子进程侧：收到一帧请求 -> 生成应答。
   resp 由处理函数 malloc，桥发完负责 free；*resp = NULL / *respLen = 0 表示空应答。
   返回 FALSE 表示处理失败（桥发 len=-1，父进程丢弃本次并报告）。 */
typedef BOOL (*BrHandler)(const BYTE* req, DWORD reqLen, BYTE** resp, DWORD* respLen);

BOOL BrIsChild(void);                                    /* 本进程是不是桥子进程 */
BOOL BrParseChildSwitch(DWORD* pid, DWORD* seq);         /* 命令行里有没有 --bridge <pid> <seq> */
int  BrServe(unsigned long pid, unsigned long seq,       /* 子进程主循环（返回退出码） */
             BrHandler handler, void (*onExit)(void));
BOOL BrEnsure(void);                                     /* 父进程：确保子进程与管道就绪 */
BOOL BrSend(const BYTE* buf, DWORD n);                   /* 父进程：发一帧 */
BOOL BrRecv(BYTE** buf, DWORD* n, DWORD timeoutMs);      /* 父进程：收一帧（buf 由调用方 free） */
void BrDrop(void);                                       /* 丢弃连接（含失败冷却） */
void BrShutdown(void);                                   /* 退出前：让子进程立刻收工 */

/* ---------- WPS 文字通道（src/wps.c） ----------
   WPS 文字既不暴露 UIA/IA2 文本、也不响应 WM_GETTEXT，但**完整实现了 Word 的
   对象模型**，所以当作一个 Word 自动化客户端来读它文档引擎里的真数据
   （详见 wps.c 顶部）。 */

/* 读全文 + 光标（返回的 text 由调用方 free）。selEnd 可传 NULL。
   焦点不是 WPS 文字时直接返回 FALSE（判定按**进程名** wps.exe：WPS 的窗口/
   控件类名与真 Word 完全相同，按类名判会把 Word 也卷进来）。 */
BOOL WpsGetTextCaret(HWND focus, WCHAR** text, int* lenOut, int* caretOut, int* selEndOut);

/* 直接置位（免逐格发方向键）：start == end 即"移光标"，否则产生选区。
   与 UIA 的置位一样，返回值要能区分"没生效"和"状态不明"——后者绝不能再补
   箭头（会双重错误）。 */
#define WPS_SET_OK    0   /* 已生效，且回读校验通过 */
#define WPS_SET_NONE  1   /* 没生效、光标未动 -> 可以安全退回逐格箭头 */
#define WPS_SET_UNSURE 2  /* 无法判定 -> 本次不要再补箭头 */
int  WpsSetSel(HWND focus, int start, int end);

int  WpsBridgeServe(unsigned long pid, unsigned long seq);  /* 子进程入口 */
void WpsBridgeShutdown(void);                               /* 父进程退出前：关掉桥 */
int  WpsSelfTest(void);                                     /* --wps-read：读一次并写日志 */

#endif
