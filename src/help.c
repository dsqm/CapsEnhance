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

#include <stdio.h>
#include <string.h>
#include <wchar.h>

/* ================= 按键帮助的"开合"（主程序侧，C + F1） =================
   窗口本身在**独立 exe**（help/help.c → bin/CapsEnhanceHelp.exe）里，这里只做
   两件事：**拉起它** 与 **告诉它当前该显示还是该隐藏**。为什么这么拆、为什么
   需要"送达重试"，见 cap.h 里"按键帮助"那一整段注释。

   状态机很小，但有一处必须写清楚：**"期望状态"与"已送达状态"是两件事。**
   冷启动时窗口还不存在，这时发的 SHOW/HIDE 会直接丢掉 —— 而"按一下就松手"
   恰恰会让两条消息都落在窗口出生之前。所以：
     · s_want      = 用户当前想要的状态（来自和弦：按下=显示，抬起=隐藏）
     · s_delivered = 最后**确认送出**的状态（-1 = 不确定，需要重送）
   只要两者不等就每 40ms 重试一次（定时器），送达即停表。状态是幂等的，
   重复发没有副作用。

   为什么要重试而不是"发一次就算"：本模块**必须**忍受两个进程的启动时序差异。
   反过来，帮助 exe 也有对称的防呆：它先建**隐藏**窗口再等消息，1.5 秒等不到
   就自己退出 —— 两边各兜一半，才不会出现"窗口永久留在屏幕上"。

   第二件事是**喂心跳**（HELP_KA_TIMER）：显示期间每 200ms 一条 KA，帮助 exe
   到点收不到就自己退。SHOW/HIDE 是"事件"，心跳是"状态"—— 只在事件上做判断
   迟早会卡在某个没收到的事件上（消息丢了、对面死了、对面卡住了），而心跳
   把"我还需要你"这件事变成持续可观测的。所以显示期间不需要别的看门狗。 */

static UINT  s_msgShow = 0;    /* 注册消息：与帮助 exe 用同名注册 -> 拿到同一个消息号 */
static UINT  s_msgHide = 0;
static UINT  s_msgKa   = 0;
static BOOL  s_want    = FALSE;  /* 期望：可见？ */
static int   s_delivered = 0;    /* 已送达：-1 未知 / 0 已隐 / 1 已显 */
static BOOL  s_spawned = FALSE;  /* 本轮流是否已经 CreateProcess 过 */
static BOOL  s_kaOn    = FALSE;  /* 心跳定时器是否在跑（只在期望显示期间） */
static int   s_ticks   = 0;

static void EnsureMsgs(void) {
    /* 只注册一次。注册消息（>= 0xC000）跨进程可用，且不会与别的程序撞号 —— 
       这是 Windows 给"两个自有进程之间说私话"准备的正规做法。 */
    if (!s_msgShow) s_msgShow = RegisterWindowMessageW(HELP_MSG_SHOW);
    if (!s_msgHide) s_msgHide = RegisterWindowMessageW(HELP_MSG_HIDE);
    if (!s_msgKa)   s_msgKa   = RegisterWindowMessageW(HELP_MSG_KA);
}

static void StopTick(void) {
    if (g_mainWnd) KillTimer(g_mainWnd, HELP_TIMER);
    s_ticks = 0;
}

/* ---------- 心跳 ----------
   与对账那只看门狗不同：**它跑得越久越说明一切正常**，所以没有"送不到就放弃"
   这种收尾；停表只在"不再需要显示"时发生。 */
static void KaStart(void) {
    if (!g_mainWnd || s_kaOn) return;
    s_kaOn = TRUE;
    SetTimer(g_mainWnd, HELP_KA_TIMER, HELP_KA_MS, NULL);
}

static void KaStop(void) {
    if (!s_kaOn) return;
    s_kaOn = FALSE;
    if (g_mainWnd) KillTimer(g_mainWnd, HELP_KA_TIMER);
}

void HelpKaTick(void) {
    if (!s_kaOn || !s_msgKa) return;
    /* 找不到窗口就什么都不做：可能它还没出生（对账那只看门狗负责拉起），
       也可能它已经退了（那正是心跳想达到的效果，不必重试）。 */
    HWND w = FindWindowW(HELP_CLASS, NULL);
    if (w) PostMessageW(w, s_msgKa, 0, 0);
}

/* 拉起帮助 exe（同目录）。与 LaunchSettings 同一条规矩：**工作目录显式传它
   自己所在目录** —— CreateProcess 默认让子进程继承调用者的当前目录，而本程序
   是以"随机 cwd + 绝对路径"启动的（计划任务 / 文件管理器双击 / 命令行都可能）。 */
static void SpawnHelp(void) {
    WCHAR exe[MAX_PATH] = L"";
    WCHAR dir[MAX_PATH] = L"";
    WCHAR cmd[MAX_PATH + 32];

    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return;
    wcscpy_s(dir, MAX_PATH, exe);
    WCHAR* slash = wcsrchr(dir, L'\\');
    if (!slash) return;
    *slash = 0;
    _snwprintf_s(exe, MAX_PATH, _TRUNCATE, L"%s\\%s", dir, HELP_EXE);
    /* --hold：别自己显示，等主程序告诉你状态（见 help/help.c 的模式说明） */
    _snwprintf_s(cmd, MAX_PATH + 32, _TRUNCATE, L"\"%s\" --hold", exe);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return;
    }
    /* 起不来只能算了：这只影响"C+F1 没反应"，不该打断当前和弦（也会写日志，
       ResetEngine 之后 s_want 会被清掉，不会反复重试）。 */
    if (g_recording.load()) CapLog("  [帮助] 启动 %ls 失败 err=%lu", HELP_EXE, GetLastError());
}

/* 起表并立刻对账一次（不等第一个 40ms 定时器消息）——帮助 exe 已经热着时，
   这一下就把延时压到"一次 PostMessage"。 */
static void Kick(void) {
    if (!g_mainWnd) { s_delivered = s_want ? 1 : 0; return; }
    s_ticks = 0;
    SetTimer(g_mainWnd, HELP_TIMER, HELP_TICK_MS, NULL);
    HelpTick();
}

void HelpShow(void) {
    if (s_want) return;              /* 已经在显示：重复按下不打断（也不重启寿命） */
    EnsureMsgs();
    s_want      = TRUE;
    s_delivered = -1;                /* 强制重送：上一次的"已隐藏"不再代表现状 */
    s_spawned   = FALSE;
    Kick();
    /* 心跳立刻起：帮助 exe 一起身就会先收到心跳（哪怕 SHOW 还在路上），
       这样它那边的"刚显示就超时"从根上不存在。 */
    KaStart();
}

void HelpHide(void) {
    /* 从没显示过、而且已经确认是隐藏态 -> 什么都不用做（松开 Caps 时每轮都会
       调到这里，绝大多数轮次都该在这一行就返回）。 */
    if (!s_want && s_delivered == 0) return;
    s_want      = FALSE;
    s_delivered = -1;
    KaStop();                        /* 先停喂狗：HIDE 万一没送到，800ms 后它自己退 */
    Kick();
}

void HelpTick(void) {
    if (!g_mainWnd) { StopTick(); return; }
    if (s_delivered == (s_want ? 1 : 0)) { StopTick(); return; }

    EnsureMsgs();
    HWND w = FindWindowW(HELP_CLASS, NULL);
    if (w) {
        PostMessageW(w, s_want ? s_msgShow : s_msgHide, 0, 0);
        s_delivered = s_want ? 1 : 0;
        StopTick();
        if (g_recording.load())
            CapLog("  [帮助] %s", s_want ? "已通知显示" : "已通知隐藏");
        return;
    }

    /* 窗口还没出生：显示才需要拉起它；隐藏的话只可能是"按一下就松手"——
       等它出生后下几拍会把 HIDE 送过去（这正是要重试的原因）。 */
    if (s_want && !s_spawned) {
        s_spawned = TRUE;
        SpawnHelp();
    }
    if (++s_ticks > HELP_TRIES_MAX) {
        StopTick();
        if (g_recording.load())
            CapLog("  [帮助] 送达超时（%d 次 x %dms）：%s", HELP_TRIES_MAX,
                   HELP_TICK_MS, s_want ? "没能拉起帮助程序" : "帮助窗口没出生就已松开");
    }
}
