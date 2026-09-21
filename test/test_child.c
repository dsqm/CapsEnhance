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

/* test_child.c — 直接验**生产路径**：ChildSpawn 有没有把子进程真的挂进 job。

   为什么需要它：smoke_clip.sh 验的是"job 一旦关闭，成员就被内核收掉"这条 **OS
   行为**；而"ChildSpawn 到底有没有调用 AssignProcessToJobObject"是 src/child.c
   里的代码，它此前没有任何测试碰过。漏掉那一句时，既有的一切验证照样全绿 ——
   表现只是"主程序退出后浮层还留在屏幕上"，而那要等 10 分钟空闲退出才看得出来，
   肉眼根本验不到（本项目在"全绿不覆盖"上已经吃过两次亏：浮层的未初始化临界区、
   bin\ 存在这个隐性前提）。

   做法：**include 真的 src/child.c**（不另写一份实现，本仓库 test_sci.c 也是这么
   做的），用它拉起一个真的子进程 —— 就是本 exe 自己带 `--sleep`（一个什么都不做、
   只是活着的进程），然后：
     · 用 `IsProcessInJob` **指定我们那个 job 句柄**问成员资格。
       ★ 不能传 NULL 问"在不在**任何** job 里"：CI 或某些启动器下父进程本身就在
         某个 job 里，那样问会恒真，等于没测。
     · 结束子进程，看注册在它进程句柄上的等待是否**立刻**把消息送到 ——
       这是"子死父知"那条链路的真身（心跳时代靠 600ms 轮询，现在是内核通知）。 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <wchar.h>

/* src/child.c 的两个外部依赖（定义在 src/main.c 里，本测试不拉那份）。
   注意 g_recording 不能加 static —— cap.h 里是 extern 声明。 */
std::atomic<bool> g_recording{false};
void CapLog(const char* fmt, ...) { (void)fmt; }

#include "cap.h"
#include "child.c"

static int g_fails = 0;

static void Check(BOOL cond, const char* name) {
    printf("  [%s] %s\n", cond ? "OK  " : "FAIL", name);
    if (!cond) g_fails++;
}

/* 消息窗口：只为接 ChildGoneProc 的那一条 PostMessage。
   用 HWND_MESSAGE（消息专用窗口）—— 不占屏幕、不进 Z 序，正是这种"只为收消息"的用途。 */
static const UINT WM_TEST_GONE = WM_APP + 1;

static LRESULT CALLBACK TestProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    (void)w; (void)l;
    if (m == WM_TEST_GONE) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "--sleep") == 0) {
        Sleep(60000);                       /* 子进程模式：活着，等着被收 */
        return 0;
    }

    printf("test_child：子进程生命周期（ChildSpawn / ChildWatchPid 的生产路径）\n");

    WCHAR exe[MAX_PATH] = L"", dir[MAX_PATH] = L"", cmd[MAX_PATH + 32];
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) { printf("  拿不到自身路径\n"); return 1; }
    wcscpy_s(dir, MAX_PATH, exe);
    WCHAR* slash = wcsrchr(dir, L'\\');
    if (slash) *slash = 0;
    _snwprintf_s(cmd, MAX_PATH + 32, _TRUNCATE, L"\"%s\" --sleep", exe);

    HANDLE hp = NULL;
    BOOL spawned = ChildSpawn(exe, cmd, dir, &hp);
    Check(spawned && hp != NULL, "ChildSpawn 起来了子进程");
    if (!spawned || !hp) return 1;

    Check(s_job != NULL, "job 建起来了（CreateJobObject + KILL_ON_JOB_CLOSE）");
    BOOL injob = FALSE;
    if (s_job) IsProcessInJob(hp, s_job, &injob);
    Check(injob, "子进程确在这个 job 里（漏 AssignProcessToJobObject 就 FAIL）");

    /* 消息窗口 + 死亡通知：注册 -> 结束子进程 -> 消息应当**立刻**到 */
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = TestProc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.lpszClassName = L"CapsEnhanceTestChild";
    RegisterClassW(&wc);
    HWND w = CreateWindowExW(0, L"CapsEnhanceTestChild", L"", 0, 0, 0, 0, 0,
                             HWND_MESSAGE, NULL, wc.hInstance, NULL);
    Check(w != NULL, "建起消息窗口（接死亡通知）");

    ChildWatch watch;
    memset(&watch, 0, sizeof(watch));
    DWORD pid = GetProcessId(hp);
    Check(ChildWatchPid(&watch, pid, w, WM_TEST_GONE), "ChildWatchPid 注册成功");
    Check(ChildWatchPid(&watch, pid, w, WM_TEST_GONE), "已经在盯时重复注册直接返回成功");

    TerminateProcess(hp, 0);
    BOOL got = FALSE;
    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < 3000) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_TEST_GONE) { got = TRUE; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (got) break;
        Sleep(20);
    }
    Check(got, "子进程一退出就收到死亡通知（内核事件，不是轮询）");
    printf("       （从结束到收到消息：%lu ms）\n", (unsigned long)(GetTickCount() - t0));

    ChildWatchStop(&watch);
    Check(watch.proc == NULL && watch.wait == NULL && watch.notify == NULL,
          "ChildWatchStop 拆干净并复位结构");

    CloseHandle(hp);
    printf(g_fails ? "\n== %d 项失败 ==\n" : "\n== 全部通过 ==\n", g_fails);
    return g_fails ? 1 : 0;
}
