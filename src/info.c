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
#include "info_shared.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

/* ================= 窗口信息弹窗的"开"（主程序侧，C + F12） =================
   窗口本身在**独立 exe**（info/info.c → bin/CapsEnhanceInfo.exe）里。这里只做
   三件事：拉起它、告诉它看哪个窗口、接住它回存的尺寸。为什么拆出去，见
   info_shared.h 的文件头（一句话：主线程是钩子宿主，不能去枚举别人家的窗口树）。

   与 help.c 的区别（那边的状态机不能照抄）：
     · 帮助窗口的寿命**就是和弦本身**（按下显、抬起隐），所以它要"期望状态 /
       已送达状态"对账；本窗口是**持久**的，按下只表示"看这个窗口"，
       松不松 Caps 与它无关 —— 状态只有一件事：**待送达的目标 HWND**。
     · 帮助窗口每次都要新起一个进程（隐完即退）；本窗口的用户可能连着按几次
       （换窗口看），所以**已存在就复用**，只把新的目标发过去。

   仍然需要那条 40ms 的送达重试：冷启动时窗口还不存在，FindWindow 找不到时
   发的消息会直接丢掉。这里对账的判据很干脆 —— FindWindow 找到 = 窗口的
   消息队列已经存在（CreateWindow 之后就有），此时 PostMessage 必达。 */

#define INFO_TICK_MS   40   /* 对账间隔 */
#define INFO_TRIES_MAX 25   /* 40ms x 25 ≈ 1s 内起不来就放弃（下次按下再试） */

static UINT s_msgOpen = 0;      /* 注册消息：让它打开/刷新到某个目标窗口 */
static UINT s_msgSize = 0;      /* 注册消息：它关窗时把用户拖出来的尺寸还回来 */
static HWND s_target  = NULL;   /* 本轮要看的目标窗口（开火那一刻的前台窗口快照） */
static BOOL s_pending = FALSE;  /* 有未送达的目标 */
static BOOL s_spawned = FALSE;  /* 本轮是否已经 CreateProcess 过 */
static int  s_ticks   = 0;

static void EnsureMsgs(void) {
    /* 注册消息（>= 0xC000）跨进程可用、不会与别的程序的私有消息撞号 ——
       两个自有进程之间说私话的正规通道（与 help.c 同一套做法）。 */
    if (!s_msgOpen) s_msgOpen = RegisterWindowMessageW(INFO_MSG_OPEN);
    if (!s_msgSize) s_msgSize = RegisterWindowMessageW(INFO_MSG_SIZE);
}

static void StopTick(void) {
    if (g_mainWnd) KillTimer(g_mainWnd, INFO_TIMER);
    s_ticks = 0;
}

/* 拉起弹窗（同目录）。与 SpawnHelp / LaunchSettings 同一条规矩：**工作目录
   显式传它自己所在目录** —— 本程序是以"随机 cwd + 绝对路径"启动的（计划任务 /
   文件管理器双击 / 命令行都可能）。

   两个参数都走命令行，而不是"先起进程再发消息"：冷启动时窗口还不存在，
   消息会丢（40ms 重试能兜，但第一次按下就要白等一拍）；而 `--hwnd` 让它在
   建窗后立刻开始采集。`--parent` 是它回存尺寸时要投递的窗口句柄 —— 主程序的
   窗口是 message-only 的，FindWindow 找不到它，只能由我们自己告诉它。 */
/* exe 路径按长路径留容量：MAX_PATH(260) 在深目录里会截断，拼出来的
   CapsEnhanceInfo.exe 路径不存在，F12 就只是静默没反应。 */
#define INFO_PATH_CAP 1024

static void SpawnInfo(void) {
    WCHAR self[INFO_PATH_CAP] = L"";
    WCHAR dir[INFO_PATH_CAP] = L"";
    WCHAR exe[INFO_PATH_CAP] = L"";
    WCHAR cmd[INFO_PATH_CAP + 96];

    if (!GetModuleFileNameW(NULL, self, INFO_PATH_CAP)) return;
    wcscpy_s(dir, INFO_PATH_CAP, self);
    WCHAR* slash = wcsrchr(dir, L'\\');
    if (!slash) return;
    *slash = 0;
    _snwprintf_s(exe, INFO_PATH_CAP, _TRUNCATE, L"%s\\%s", dir, INFO_EXE);
    _snwprintf_s(cmd, INFO_PATH_CAP + 96, _TRUNCATE, L"\"%s\" --parent 0x%llX --hwnd 0x%llX",
                 exe, (unsigned long long)(ULONG_PTR)g_mainWnd,
                 (unsigned long long)(ULONG_PTR)s_target);

    /* 交给 child.c：挂起起 -> 挂进 job（主程序一死，内核收掉它）-> 放行。
       hProcess 不接：弹窗的去留由用户点关闭决定，不需要"进程没了"这条通知。 */
    if (ChildSpawn(exe, cmd, dir, NULL))
        return;
    if (g_recording.load())
        CapLog("  [窗口信息] 启动 %ls 失败 err=%lu", INFO_EXE, GetLastError());
}

/* 起表并立刻对账一次（不等第一个 40ms 定时器消息）：弹窗已经热着时，
   这一下就把延时压到"一次 PostMessage"。 */
void InfoOpen(HWND target) {
    /* 目标无效（窗口在按下与处理之间就没了）：不弹。这与 C+F4 的"取不到路径
       就不动作"不同 —— 那边失败是常态（桌面/受保护进程），这边没对象可看。 */
    if (!target || !IsWindow(target)) {
        if (g_recording.load()) CapLog("  [窗口信息] 目标窗口已失效，忽略");
        return;
    }
    if (!g_mainWnd) return;

    EnsureMsgs();
    s_target  = target;
    s_pending = TRUE;
    s_spawned = FALSE;
    s_ticks   = 0;
    SetTimer(g_mainWnd, INFO_TIMER, INFO_TICK_MS, NULL);
    InfoTick();
}

void InfoTick(void) {
    if (!g_mainWnd) { StopTick(); return; }
    if (!s_pending) { StopTick(); return; }

    EnsureMsgs();
    HWND w = FindWindowW(INFO_CLASS, NULL);
    if (w) {
        PostMessageW(w, s_msgOpen, (WPARAM)s_target, 0);
        /* 已经开着（用户看的是上一个窗口）：抬到前面，否则他看不到刷新后的内容。
           弹窗不再置顶，这一步是「再按一次 F12 能看见它」的关键。 */
        if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
        SetForegroundWindow(w);
        s_pending = FALSE;
        StopTick();
        if (g_recording.load())
            CapLog("  [窗口信息] 已通知 0x%llX", (unsigned long long)(ULONG_PTR)s_target);
        return;
    }

    /* 窗口还没出生：本轮的第一次先拉起它，之后每拍只查 FindWindow。 */
    if (!s_spawned) {
        s_spawned = TRUE;
        SpawnInfo();
    }
    if (++s_ticks > INFO_TRIES_MAX) {
        s_pending = FALSE;
        StopTick();
        if (g_recording.load())
            CapLog("  [窗口信息] 送达超时（%d 次 x %dms）：没能拉起弹窗",
                   INFO_TRIES_MAX, INFO_TICK_MS);
    }
}

/* 弹窗关窗时把用户拖出来的尺寸还回来（INFO_MSG_SIZE）。
   写 ini 放在主程序：config.c 的增量写回已经处理好了"保留注释、保留未知段、
   跟随行尾与 BOM"，弹窗自己再写一份就等于多一套容易写坏用户文件的代码。
   返回 TRUE 表示"这条消息是我的"，WndProc 据此不再往下传。 */
BOOL InfoHandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    EnsureMsgs();
    if (!s_msgSize || msg != s_msgSize) return FALSE;

    int w = (int)(INT_PTR)wp;
    int h = (int)(INT_PTR)lp;
    if (w <= 0 || h <= 0) return TRUE;      /* 最小化/异常值：不记 */

    CfgSetInt(CFG_INFO_W, w);               /* 钳制与解析同源（config.c 的表） */
    CfgSetInt(CFG_INFO_H, h);

    WCHAR path[MAX_PATH];
    if (ConfigFilePath(path, MAX_PATH) && !ConfigSaveFile(path) && g_recording.load())
        CapLog("  [窗口信息] 尺寸写回失败：%ls", path);
    return TRUE;
}
