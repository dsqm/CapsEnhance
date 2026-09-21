#ifndef CAP_H
#define CAP_H

/* ---------- C++ 迁移桥：CINTERFACE + COBJMACROS ----------
   本项目现在**按 C++ 编译**（构建脚本用 /TP）。但 COM 代码（nav.c 的 UIA/IA2、
   autodisp.c 的 IDispatch、wps.c 的附着）是按 C 的 `p->lpVtbl->Method(p, ...)`
   习惯写的，而 C++ 下 SDK 默认给的是"带虚函数的 C++ 接口"——`->lpVtbl->` 与
   `IUIAutomationXxx_Release(p)` 这类宏在那边都不存在（约 70 处调用会一起报错）。

   定义 CINTERFACE 让 SDK 继续给出 C 形态的 vtable 结构体，COBJMACROS 提供
   `Xxx_Method(p, ...)` 宏包装 —— 同一份源码在 C 与 C++ 下都能编过。这是"先把
   编译面切到 C++，再逐模块换成 C++ 写法"的过渡层；换完之后本块即可删除。
   （要不要换成 `p->Method(...)` + ComPtr 那套 C++ COM 写法，是独立的一步，
   与"按 C++ 编译"无关 —— 后者只要求能编过，前者才是风格统一。）

   **必须出现在任何 SDK 头之前**，所以放在这里：cap.h 是所有 TU 的第一个包含
   （包括直接 `#include "nav.c"` 的 test_sci.c / memtest.c）。不写成
   `/DCINTERFACE` 编译开关，是为了让"为什么需要它"留在源码里、而不是散在构建脚本。

   注意 REFIID / REFCLSID / REFGUID 在 C++ 下**恒为引用类型**（与 CINTERFACE
   无关，见 guiddef.h），所以传 IID 时直接写 `IID_XXX`，不能写 `&IID_XXX`。 */
#ifdef __cplusplus
#define CINTERFACE
#define COBJMACROS
#endif

#include <windows.h>
#include <atomic>   /* g_recording：真跨线程标志，见下 */

/* ================= 本头文件的构成 =================
   cap.h 只做两件事：把 C++ 迁移桥（必须早于任何 SDK 头，理由见上）与公共
   包含放好，再把下面六个**分区头**按依赖顺序引进来 —— 所有 TU 仍然只写
   `#include "cap.h"`，包含点一处都不改。

     cap_core.h     共享常量（主线程消息 / 动作与模式常量 / atomic 与 volatile
                    的分界）+ 全局状态 + 主窗口句柄 + 诊断日志
     cap_input.h    键位表查询 / 动作执行线程（含滚轮与持续滚动两条通道）/
                    底层钩子 / 发送与补齐
     cap_window.h   窗口几何（winmove / drag / folder）+ 坐标空间自检
     cap_overlay.h  子进程底座（child.c）+ 按键帮助 + 剪贴板浮层
     cap_config.h   配置（CFG_* 编号与通用读写 + 五份名单 + 进程路径查询）
     cap_nav.h      按词跳转 / 单键切输入法 / 自动化与 WPS 的接口去向（auto.h）

   为什么拆：这个头原先 700 多行、被**每一个** TU 包含 —— 想查"剪贴板浮层
   主程序侧有哪些接口"也得整份读一遍。拆开之后按模块读一个分区头就够
   （分区头之间没有互相依赖，只有 cap.h 按上面的顺序包含它们）。
   分区头**不各自 include 任何东西**：它们依赖 cap.h 已经放好的 windows.h /
   atomic 与 CINTERFACE 桥，单独拿去用会缺类型。 */

#include "cap_core.h"
#include "cap_input.h"
#include "cap_window.h"
#include "cap_overlay.h"
#include "cap_config.h"
#include "cap_nav.h"

#endif
