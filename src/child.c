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
   ------------------------------------------------------------------ */

#include "cap.h"

#include <string.h>

/* ================= 子进程生命周期（帮助浮层的底座） =================
   主程序会拉起帮助窗口（`CapsEnhanceHelp.exe`）。它的收场原来交给**心跳** ——
   主程序每 200ms 喂一条 KA，对面 800ms 收不到就自杀。那条线同时兼着两件事，
   现在拆成两条 OS 原语：

     · 父死 → 子退：**Job Object**（`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`）。
       主程序建一个只自己持有的 job，把子进程挂进去；句柄随进程结束
       （正常退出 / 崩溃 / 被强杀都一样）由内核关闭，内核随即收掉 job 里的所有
       进程。**零协作**：子进程一行都不用改，甚至不需要知道有这回事。
       心跳做不到这一点 —— 它的看门狗是个 `WM_TIMER`，子进程消息循环一旦卡住
       （绘制 / 等锁），定时器根本不到点，那个孤儿会一直活着。

     · 子死 → 父知：**进程句柄等待**（`ChildWatchPid`）。
       反方向，job 覆盖不到（job 管的是"父死杀子"）。帮助窗口用不上这一半
       （它自己退出，主程序不需要为它恢复按键），保留为子进程底座的能力，
       由 `test/test_child.c` 覆盖。

   ★ 为什么没用"父进程句柄 / 管道 EOF"：那两个方向是对的（子等父死），但都要
     **子进程配合**（把可继承句柄经命令行传过去 + 子侧起一个等待线程），而且子
     进程卡死时那个等待线程同样跑不动。Job Object 不用子进程配合，且由内核执行
     `TerminateProcess` —— 卡死也照杀。

   ★ 为什么 `CreateProcess` 要**挂起**着起：`AssignProcessToJobObject` 必须在子
     进程跑起来之前完成。否则它可能先退出（job 里就没有它了），或先起出自己的
     孙进程（那些就跑出 job 管辖了）。挂起 → 赋值 → 放行，是唯一没有竞态的次序。

   ★ 只挂"该跟主程序一起走"的帮助进程：设置程序（`CapsEnhanceSettings.exe`）
     **不进** job —— 用户期望它活过主程序。
     Windows 会把子进程自动加进**父进程所在**的 job，但我们不把主程序放进任何
     job，所以这里的成员资格完全由本文件显式决定。 */

static HANDLE s_job = NULL;

/* job 只建一次，两个子进程共用。建不出来就返回 NULL —— 调用方照常放行子进程，
   只是这一次没有"父死子退"的保证（见 ChildSpawn 的降级说明）。 */
static HANDLE ChildJob(void) {
    if (s_job) return s_job;
    HANDLE j = CreateJobObjectW(NULL, NULL);
    if (!j) return NULL;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
    memset(&li, 0, sizeof(li));
    /* 只开这一位：最后一个 job 句柄关闭 = 主程序没了 -> 内核收掉 job 里的进程。
       刻意不加内存 / CPU / UI 限制 —— 那不是这里的目的，加了会改变子进程行为，
       而这两个子进程自己该怎么跑是它们的事。 */
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(j, JobObjectExtendedLimitInformation, &li, sizeof(li))) {
        CloseHandle(j);
        return NULL;
    }
    s_job = j;
    return s_job;
}

BOOL ChildSpawn(const WCHAR* exe, WCHAR* cmd, const WCHAR* dir, HANDLE* outProc) {
    if (outProc) *outProc = NULL;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, dir, &si, &pi))
        return FALSE;

    HANDLE job = ChildJob();
    if (!job || !AssignProcessToJobObject(job, pi.hProcess)) {
        /* 降级：子进程照常跑，只是不受 job 管（外层 job 禁止并入等少见情形）。
           后果是主程序退出后它可能多活一会儿 —— 帮助窗口还有自己的 3 分钟上限，
           不会永远留着。 */
        if (g_recording.load())
            CapLog("  [子进程] 未能挂进 job（err=%lu），本次无父死子退保证", GetLastError());
    }
    ResumeThread(pi.hThread);          /* 挂好了才放它跑（见文件头） */
    CloseHandle(pi.hThread);
    if (outProc) *outProc = pi.hProcess;
    else         CloseHandle(pi.hProcess);
    return TRUE;
}

/* ---------- 子进程死亡通知 ----------
   刻意**不做**任何共享状态的更新：回调跑在线程池线程上，而 `s_want` 那类状态
   是主线程在写、钩子在读的。投递一条消息、由主线程改，是唯一不用加锁的形状。 */
static VOID CALLBACK ChildGoneProc(void* ctx, BOOLEAN timedOut) {
    ChildWatch* w = (ChildWatch*)ctx;
    (void)timedOut;                    /* 注册时给的是 INFINITE，永远不会超时 */
    if (w->notify) PostMessageW(w->notify, w->msg, 0, 0);
}

BOOL ChildWatchPid(ChildWatch* w, DWORD pid, HWND notify, UINT msg) {
    if (w->proc) return TRUE;          /* 已经在盯一个了 */
    HANDLE hp = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!hp) {
        /* 连句柄都拿不到（进程已经退干净了，或权限不允许）：按"它已经没了"报一次。
           这是安全的那一侧 —— 多清一次意图，不会漏清（漏清才是吞按键那个 bug）。 */
        if (notify) PostMessageW(notify, msg, 0, 0);
        return FALSE;
    }
    w->notify = notify;
    w->msg    = msg;
    w->proc   = hp;
    /* WT_EXECUTEONLYONCE：只需要在"结束"这一刻响一次。超时给 INFINITE —— 这里要的
       是"进程结束"这个事实，不是"这么久还没结束"的推测（差别就是心跳误伤的根源）。 */
    if (!RegisterWaitForSingleObject(&w->wait, hp, ChildGoneProc, w,
                                     INFINITE, WT_EXECUTEONLYONCE)) {
        CloseHandle(hp);
        w->proc = NULL;
        w->wait = NULL;
        return FALSE;
    }
    return TRUE;
}

void ChildWatchStop(ChildWatch* w) {
    /* ★ 返回值必须看（/analyze 的 C6031 就是冲着"返回值被忽略"来的）：
       失败说明等待**没拆掉**，回调可能仍在飞 —— 那就不能把 notify 清掉，
       否则它醒来时会去 PostMessage 一个已经不属于它的窗口。参数只有等待句柄
       与完成事件，正常调用不会失败，所以这条分支是纯防御；万一走到，就保留
       wait / notify（少回收一个句柄，本函数只在退出路径上跑一次）。 */
    BOOL stopped = TRUE;
    if (w->wait) {
        /* INVALID_HANDLE_VALUE = 等回调跑完再拆。回调只做一次 PostMessage，不会
           卡住；用 NULL（异步）反而可能让它在结构被复位之后才跑。 */
        stopped = UnregisterWaitEx(w->wait, INVALID_HANDLE_VALUE) ? TRUE : FALSE;
        if (stopped) w->wait = NULL;
    }
    if (w->proc) { CloseHandle(w->proc); w->proc = NULL; }
    if (stopped) { w->notify = NULL; w->msg = 0; }
}
