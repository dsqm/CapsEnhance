/* ======================================================================
   CapsEnhance —— Windows CapsLock 快捷键增强程序
   Copyright (C) 2026  qm

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.

   ------------------------------------------------------------------
   打开文件夹用的两个接口取自微软官方文档：
     SHOpenFolderAndSelectItems
       https://learn.microsoft.com/windows/win32/api/shlobj_core/nf-shlobj_core-shopenfolderandselectitems
     SHParseDisplayName
       https://learn.microsoft.com/windows/win32/api/shlobj_core/nf-shlobj_core-shparsedisplayname
   本项目同样以 GPL-3.0-or-later 授权。
   ------------------------------------------------------------------ */

#include "cap.h"

#include <shlobj.h>   /* SHParseDisplayName / SHOpenFolderAndSelectItems / ILFree */

#pragma comment(lib, "shell32.lib")

/* ================= 打开焦点程序所在文件夹（C + \） =================
   动作本身很简单：把前台那个程序的 exe 所在文件夹打开，并选中这个 exe
   —— 就是资源管理器右键菜单里的「打开文件所在的位置」。

   ★ 为什么用 shell 接口而不是拼 `explorer.exe /select,<路径>`：
   后者把"文件管理器是谁"写死在命令行里。而"打开文件夹并选中一项"是 shell
   定义的**标准动作**，`SHOpenFolderAndSelectItems` 会交给系统当前的默认文件夹
   处理器去做 —— 用户换了第三方文件管理器、或系统行为变化时，这里一行都不用改。
   它还顺带处理了资源管理器自己的那些状态（多标签/多窗口复用、特殊文件夹）。
   代价是它**同步阻塞**（要等 shell 响应），所以不能落在任何不该阻塞的线程上。

   ★ 为什么必须有自己的线程（两条独立的理由，任一条都足够）：
     1. 这条调用会阻塞数百毫秒（拉起或唤醒文件管理器）。跑在承载键盘钩子的
        主线程上就是把钩子冻住 —— 那正是 act.c 存在的理由（见其文件头）。
        也**不**复用 act.c 的动作队列：队列是 FIFO，这条阻塞调用会把排在后面的
        动作（C+J 之类的按键发送）一起堵上，而"打开文件夹"是纯发起的动作，
        没有任何理由占用那条通道。
     2. `CoInitialize` 是**线程级**状态，而 act.c 工作线程的套间可能已被 nav.c
        按 STA 初始化（见 nav.c 的 CoUp）。STA 下做同步阻塞的 shell 调用要求
        该线程泵消息，否则可能死锁。新线程自己按 **MTA** 初始化就没这个问题 ——
        MTA 里阻塞调用是允许的。
   线程是"起一个、干完就退"的一次性线程，句柄立刻关掉（线程对象在线程退出时
   由系统回收），不需要像 DragLane / ImeLane 那样维护生命周期。

   ★ 拿不到路径的三种情况（都算正常失败，不是缺陷）：
     · 前台是桌面/任务栏那类**壳窗口** —— 它没有"所在文件夹"；
     · 更高级别的**受保护进程**（System / PPL）或已经退出的进程：OpenProcess
       被拒；
     · **UWP**：前台窗口属于宿主 ApplicationFrameHost.exe，真实程序在
       WindowsApps 里（见下面 RealPidOf）。 */

/* UWP 的宿主进程名：前台窗口属于它时，窗口的"进程"不是用户心里那个程序。 */
static const WCHAR kFrameHostExe[] = L"ApplicationFrameHost.exe";

/* UWP 应用自己的窗口类：它挂在宿主窗口下，但属于**应用自己的进程**。 */
static const WCHAR kUwpCoreClass[] = L"Windows.UI.Core.CoreWindow";

/* 路径的最后一节（exe 名）。只给本文件用，不改动入参。 */
static const WCHAR* BaseNameOf(const WCHAR* path) {
    const WCHAR* name = path;
    for (const WCHAR* q = path; *q; q++)
        if (*q == L'\\') name = q + 1;
    return name;
}

/* ================= UWP：从宿主窗口找到真实进程 =================
   实测（本机 Win11）：UWP 应用在前台时 GetForegroundWindow 给的是宿主的顶层
   窗口（类名 ApplicationFrameWindow），它的进程是 ApplicationFrameHost.exe，
   路径指向 C:\Windows\System32 —— 照这个路径打开等于开 System32，不是用户要的。
   真实程序在宿主窗口下面的 `Windows.UI.Core.CoreWindow` 子窗口上：那个窗口
   属于应用自己的进程，映像路径就是 WindowsApps 里的安装目录。

   找不到子窗口（应用已挂起、或就是不按这个结构摆放）时**宁可不开**：开
   System32 并选中 ApplicationFrameHost.exe 是确定无用的结果，比"没有反应"
   更容易让人误会程序坏了。 */

static BOOL CALLBACK CorePidProc(HWND child, LPARAM lp) {
    WCHAR cls[64];
    if (!GetClassNameW(child, cls, 64)) return TRUE;
    if (_wcsicmp(cls, kUwpCoreClass) != 0) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(child, &pid);
    if (!pid) return TRUE;
    *(DWORD*)lp = pid;
    return FALSE;                       /* 找到第一个就停 */
}

/* 宿主窗口下第一个 CoreWindow 的进程 PID；0 = 没找到。 */
static DWORD CorePidOf(HWND frame) {
    DWORD pid = 0;
    EnumChildWindows(frame, CorePidProc, (LPARAM)&pid);
    return pid;
}

/* ============ 目标窗口 -> 进程映像全路径（同步、纯本地、可单测） ============
   测试与实现共用：传本进程的窗口即得本 exe 的全路径，不需要真的去开一个文件夹。 */
BOOL FolderProcessPathOf(HWND target, WCHAR* out, size_t cap, int* err) {
    if (err) *err = FOLDER_ERR_NOPATH;
    if (!out || cap == 0) return FALSE;
    out[0] = 0;
    if (!target || !IsWindow(target)) return FALSE;

    /* 能开的只有**顶层**窗口：Chromium 系的前台窗口常常是内容子窗口，
       不过对"取进程"来说子/父同属一个进程，取顶层只是为了让壳窗口判定
       （桌面/任务栏）落在同一个窗口上。 */
    HWND top = GetAncestor(target, GA_ROOT);
    if (!top) top = target;
    if (WinIsShellClass(top)) {
        if (err) *err = FOLDER_ERR_SHELLWIN;
        return FALSE;
    }

    if (!ConfigGetProcessPath(top, out, cap)) return FALSE;

    /* 宿主进程 -> 换成应用自己的进程再看一次 */
    if (_wcsicmp(BaseNameOf(out), kFrameHostExe) == 0) {
        DWORD core = CorePidOf(top);
        if (!core) return FALSE;                       /* 解析不到：当"没有独立文件夹" */
        if (!ConfigGetProcessPathByPid(core, out, cap)) return FALSE;
        if (_wcsicmp(BaseNameOf(out), kFrameHostExe) == 0) return FALSE;  /* 就在宿主里跑 */
    }
    return out[0] != 0;
}

/* 失败原因的可读名（仅日志用，不参与判定）。 */
static const char* ErrText(int err) {
    switch (err) {
    case FOLDER_OK:          return "已交给系统打开";
    case FOLDER_ERR_SHELLWIN: return "前台是桌面/任务栏，没有所在文件夹";
    case FOLDER_ERR_SHELL:   return "shell 接口失败（解析不到路径或被拒）";
    case FOLDER_ERR_NOPATH:  return "拿不到进程路径（受保护进程 / 已退出 / UWP 解析不到）";
    default:                 return "?";
    }
}

/* 真正干活的一次性线程：解析路径 -> 转 PIDL -> 交给 shell。
   结束后把**失败原因**发回主线程（成功不发：弹气泡是打扰，而这条动作成功时
   本来就会弹出文件管理器，本身就是反馈）。

   为什么失败反馈必须绕回主线程：气泡走 Shell_NotifyIconW + 主线程的 g_nid，
   工作线程直接调就是对它的跨线程访问（托盘那一套状态全是主线程独占的）。 */
static DWORD WINAPI FolderThreadProc(LPVOID param) {
    HWND target = (HWND)param;
    WCHAR path[PROC_PATH_MAX];
    path[0] = 0;
    int err = FOLDER_ERR_NOPATH;

    /* MTA 见文件头：本线程只调一次 shell 接口然后退出，没有消息泵，
       所以不能按 STA 初始化。S_FALSE = 本线程已初始化过（正常不会发生，
       但代价为零）；RPC_E_CHANGED_MODE = 已被按别的模式初始化 —— 那时
       COM 已经能用，照常继续，不再自己反初始化。 */
    HRESULT hrCo = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    BOOL ownCo = SUCCEEDED(hrCo) && hrCo != S_FALSE;

    if (SUCCEEDED(hrCo) || hrCo == RPC_E_CHANGED_MODE) {
        if (FolderProcessPathOf(target, path, PROC_PATH_MAX, &err)) {
            /* 路径 -> 绝对 PIDL：交给 shell 命名空间去解析（它会做规范化，
               也认 `::{GUID}` 这类特殊项），不认的路径直接失败。 */
            PIDLIST_ABSOLUTE pidl = NULL;
            if (SHParseDisplayName(path, NULL, &pidl, 0, NULL) == S_OK && pidl) {
                /* cidl = 0 + 单项 PIDL：文档写明这时 shell 会打开它的**父文件夹
                   并选中它**，正是我们要的语义。flags 用 0（OFASI_EDIT 会进
                   重命名状态，OFASI_OPENDESKTOP 会在桌面上选，都不是这个动作的意思）。 */
                if (SHOpenFolderAndSelectItems(pidl, 0, NULL, 0) == S_OK)
                    err = FOLDER_OK;
                else
                    err = FOLDER_ERR_SHELL;
                ILFree(pidl);
            } else {
                err = FOLDER_ERR_SHELL;
            }
        }
    } else {
        /* 连 COM 都起不来：只能算 shell 这条通道失败（枚举里没有更贴切的码） */
        err = FOLDER_ERR_SHELL;
    }

    if (g_recording.load()) {
        char p8[PROC_PATH_MAX];
        p8[0] = 0;
        if (path[0])
            WideCharToMultiByte(CP_ACP, 0, path, -1, p8, (int)sizeof(p8), NULL, NULL);
        CapLog("  [文件夹] %s", ErrText(err));
        if (p8[0]) CapLog("  [文件夹]   目标 %s", p8);
    }

    if (err != FOLDER_OK) PostMessageW(g_mainWnd, WM_FOLDER_OPEN, (WPARAM)err, 0);

    if (ownCo) CoUninitialize();
    return 0;
}

/* 动作入口（act.c 的分派）：丢给一次性线程后立刻返回 —— 这条动作不占动作队列，
   所以 ActInFlight 不会因为它变慢（见文件头第 1 条）。
   线程起不来时退回同步执行：与 act.c 的 ActPost 同一个取舍 —— 宁可损失响应性，
   也不能让功能失效。 */
void FolderOpenRequest(HWND target) {
    HANDLE h = CreateThread(NULL, 0, FolderThreadProc, (LPVOID)target, 0, NULL);
    if (!h) {
        FolderThreadProc((LPVOID)target);
        return;
    }
    CloseHandle(h);   /* 线程对象在线程退出时回收；不需要 join */
}
